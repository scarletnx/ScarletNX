/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/services/system.h"
#include "3ds/services/uds.h"
#include "3ds/audio/dsp.h"
#include "common/network/net_room.h"
#include "common/utilities/common.h"
#include "common/network/net_internal.h"
#include "fake_room.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#define BUFFER  0x300000u
#define NET     0x300900u
#define DATA    0x300b00u
/* a request's answer: no transport error, and a result code of 0 */
static void replied(SnxCtrProcess* p, const uint32_t* words, uint32_t result) {
    assert(!result);
    if (ctr_word(p, CTR_COMMAND + 4)) fprintf(stderr, "command %08x -> %08x\n", words[0], ctr_word(p, CTR_COMMAND + 4));
    assert(!ctr_word(p, CTR_COMMAND + 4));
}
static void request(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count) {
    ctr_request(p, words, count);
    uint32_t result = 0;
    int64_t before = snx_net_now_us();
    SnxCtrSvcResult state = snx_ctr_ipc_send(p, handle, &result);
    if (state == SNX_CTR_SVC_BLOCKED) {
        assert(snx_net_now_us() - before < 100000);
        assert((words[0] >> 16) == 0x1e);
        uint32_t caller = p->current_thread;
        assert(snx_ctr_thread(p)->wake_ns > snx_ctr_now_ns());
        uint32_t worker;
        const uint32_t registers[5] = {0x31, 0x100000, 0, SNX_CTR_STACK_TOP - 0x1000, 0xfffffffe};
        assert(!snx_ctr_thread_create(p, registers, &worker));
        assert(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN);
        assert(p->current_thread == snx_ctr_handle_object(p, worker));
        uint32_t worker_tls = p->cpu.state.cpu.tpidruro + 0x80;
        ctr_put(p, worker_tls, 0xfeedface);
        int64_t deadline = snx_net_now_us() + 6000000;
        while (p->objects[caller].data.thread.wake_ns && snx_net_now_us() < deadline) {
            assert(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN);
            snx_net_sleep_us(1000);
        }
        assert(!p->objects[caller].data.thread.wake_ns);
        assert(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN);
        assert(p->current_thread == caller);
        assert(ctr_word(p, worker_tls) == 0xfeedface);
        p->objects[snx_ctr_handle_object(p, worker)].data.thread.dead = 1;
        assert(!snx_ctr_handle_close(p, worker));
        assert(ctr_word(p, CTR_COMMAND) == 0x1e0040);
    } else
        assert(state == SNX_CTR_SVC_RETURN);
    replied(p, words, result);
}
/* NULL: not set */
static void env(const char* key, const char* value) {
#ifdef _WIN32
    assert(!_putenv_s(key, value ? value : ""));
#else
    assert(value ? !setenv(key, value, 1) : !unsetenv(key));
#endif
}
#ifndef _WIN32
/* stderr into a file of this process alone until capture_end; *saved keeps stderr's own. ctest runs this program's two
   modes at once in one folder, where a file found by its name is the other's to truncate and remove: tmpfile's has none */
static FILE* capture_begin(int* saved) {
    FILE* f = tmpfile();
    fflush(stderr);
    assert(f && (*saved = dup(2)) >= 0 && dup2(fileno(f), 2) == 2);
    return f;
}
/* what reached stderr since capture_begin, cut to size - 1 bytes, and their count; stderr back where it was. fd 2 must
   be the capture's file to its end: what closed or replaced it inside took the lines written after */
static size_t capture_end(FILE* f, int saved, char* said, size_t size) {
    struct stat now, mine;
    fflush(stderr);
    int open2 = !fstat(2, &now),
        same = open2 && !fstat(fileno(f), &mine) && now.st_dev == mine.st_dev && now.st_ino == mine.st_ino;
    assert(dup2(saved, 2) == 2);
    close(saved);
    if (!open2)
        fputs("3ds_uds.c: fd 2 is closed at the capture's end: something inside it closed the capture's file\n",
              stderr);
    else if (!same)
        fprintf(stderr,
                "3ds_uds.c: fd 2 is %s of mode %o (inode %llu) at the capture's end, not the capture's file (inode "
                "%llu): something inside it closed or replaced fd 2\n",
                S_ISFIFO(now.st_mode)  ? "a pipe"
                : S_ISREG(now.st_mode) ? "another file"
                                       : "neither file nor pipe",
                (unsigned)now.st_mode, (unsigned long long)now.st_ino, (unsigned long long)mine.st_ino);
    assert(same);
    rewind(f);
    size_t n = fread(said, 1, size - 1, f);
    fclose(f);
    said[n] = 0;
    return n;
}
/* a capture beside another process's, as the two modes make them: each reads its own lines, all of them */
static void captures_apart(void) {
    char said[64];
    int saved;
    FILE* f = capture_begin(&saved);
    fputs("first\n", stderr);
    pid_t other = fork();
    assert(other >= 0);
    if (!other) {
        int its;
        FILE* g = capture_begin(&its);
        fputs("other\n", stderr);
        capture_end(g, its, said, sizeof said);
        if (!strcmp(said, "other\n")) _exit(0);
        fputs(said, stderr); /* into the first capture, which says it */
        _exit(1);
    }
    int status = 0;
    assert(waitpid(other, &status, 0) == other);
    fputs("first again\n", stderr);
    capture_end(f, saved, said, sizeof said);
    if (strcmp(said, "first\nfirst again\n")) fputs(said, stderr); /* what it read instead */
    assert(!strcmp(said, "first\nfirst again\n") && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("UDS: a capture of stderr beside another process's keeps its own lines");
}
static void exit_on_abort(int sig) { _exit(sig); } /* no core, no crash report */
/* a capture whose fd 2 something inside replaced, here with a pipe: its end says so, and stops */
static void capture_lost_fd(void) {
    char said[512];
    int saved;
    FILE* f = capture_begin(&saved);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        int its, ends[2];
        signal(SIGABRT, exit_on_abort);
        FILE* g = capture_begin(&its);
        if (pipe(ends) || dup2(ends[0], 2) != 2) _exit(1);
        capture_end(g, its, said, sizeof said);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    capture_end(f, saved, said, sizeof said);
    if (!strstr(said, "3ds_uds.c: fd 2 is a pipe of mode ")) fputs(said, stderr); /* what it said instead */
    assert(WIFEXITED(status) && WEXITSTATUS(status) == SIGABRT && strstr(said, "3ds_uds.c: fd 2 is a pipe of mode "));
    puts("UDS: a capture whose fd 2 was replaced says so");
}
#endif
/* the init request, which joins the relay: the 3DS runtime says so on stderr, its log, which players attach to public
   reports, so the nickname is never in it, nor in what the members already in say of the join. Read where fd 2 can be
   swapped for a file */
static void init_unnamed(SnxCtrProcess* p, uint32_t handle, const uint32_t* init, const char* nickname) {
#ifndef _WIN32
    char said[4096];
    int saved;
    uint32_t result = 0;
    FILE* f = capture_begin(&saved);
    ctr_request(p, init, 15);
    SnxCtrSvcResult state = snx_ctr_ipc_send(p, handle, &result); /* checked once stderr can say why */
    size_t n = capture_end(f, saved, said, sizeof said);
    fputs(said, stderr); /* where it would have gone */
    if (strlen(said) < n)
        fprintf(stderr, "3ds_uds.c: a NUL at byte %zu of the capture's %zu hides the rest\n", strlen(said), n);
    assert(state == SNX_CTR_SVC_RETURN);
    replied(p, init, result);
    assert(strstr(said, "3ds uds: joined the relay as ") && !strstr(said, nickname));
#else
    (void)nickname;
    request(p, handle, init, 15);
#endif
}
/* nickname: what the join takes, the user's or an old variable's, which the log never shows */
static SnxCtrProcess* process(uint32_t* handle, const char* nickname) {
    uint8_t code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {BUFFER, 1, 4096}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)0);
    assert(p);
    p->title_id = 0x0004000012345600ull;
    snprintf(p->title, sizeof p->title, "UDS relay fixture");
    assert(!snx_ctr_system_open(p, (const uint8_t*)"nwm::UDS", 8, handle));
    uint32_t shared = 0;
    assert(!snx_ctr_object_create(p, CTR_OBJ_SHARED, &shared));
    p->objects[snx_ctr_handle_object(p, shared)].data.shared.size = 4096;
    uint32_t init[15] = {0x1b0302, 4096};
    init[12] = 0x400;
    init[14] = shared;
    init_unnamed(p, *handle, init, nickname);
    assert(ctr_word(p, CTR_COMMAND) == 0x1b0042);
    assert(ctr_word(p, CTR_COMMAND + 12));
    return p;
}
typedef struct {
    SnxCtrProcess* p;
    atomic_int stop;
    char pad[64];
    atomic_int done;
} Pump;
static void pump(void* context) {
    Pump* task = context;
    while (!atomic_load(&task->stop)) {
        snx_ctr_uds_poll(task->p);
        snx_net_sleep_us(1000);
    }
    atomic_store(&task->done, 1);
}
static void bind_channel(SnxCtrProcess* p, uint32_t h) {
    uint32_t bind[] = {0x120100, 1, 8192, 7, 65535};
    request(p, h, bind, 5);
    assert(ctr_word(p, CTR_COMMAND + 12));
}
static void send_data(SnxCtrProcess* p, uint32_t h, unsigned dest, const char* text) {
    unsigned size = (unsigned)strlen(text) + 1;
    assert(snx_ctr_write_ram(p->space, DATA, text, size) == SNX_CTR_MEM_OK);
    uint32_t send[] = {0x170182, 1, dest, 7, (size + 3) / 4, size, 0, ((size + 3) & ~3u) << 14 | 5 << 10 | 2, DATA};
    request(p, h, send, 9);
}
static void receive_data(SnxCtrProcess* p, uint32_t h, unsigned source, const char* text) {
    for (unsigned i = 0; i < 200; i++) {
        snx_ctr_uds_poll(p);
        snx_net_sleep_us(1000);
    }
    ctr_put(p, CTR_COMMAND + 256, 256 << 14 | 2);
    ctr_put(p, CTR_COMMAND + 260, BUFFER);
    uint32_t pull[] = {0x1400c0, 1, 64, 4096};
    request(p, h, pull, 4);
    assert(ctr_word(p, CTR_COMMAND + 8) == strlen(text) + 1 && ctr_word(p, CTR_COMMAND + 12) == source);
    char actual[256];
    assert(snx_ctr_read_ram(p->space, BUFFER, actual, strlen(text) + 1) == SNX_CTR_MEM_OK);
    assert(!strcmp(actual, text));
    request(p, h, pull, 4);
    assert(!ctr_word(p, CTR_COMMAND + 8));
}
/* A room member speaking the public wire format lets us reject, close or ignore
   a join without production-only test hooks or game assets. */
static SnxSocket* wire_socket(SnxRoom* room) {
    SnxSocket* socket = NULL;
    assert(!snx_room_socket_open(room, &socket, SNX_AF_INET, SNX_SOCK_DGRAM, 0));
    SnxSockAddr address = {.family = SNX_AF_INET, .port = (uint16_t)((24873 >> 8) | (24873 << 8))};
    uint32_t enabled = 1;
    assert(!snx_socket_setsockopt(socket, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &enabled, 4));
    assert(!snx_socket_bind(socket, &address));
    return socket;
}
static void wire_send(SnxSocket* socket, const uint8_t ip[4], const uint8_t* wire, size_t size) {
    SnxSockAddr address = {.family = SNX_AF_INET, .port = (uint16_t)((24873 >> 8) | (24873 << 8))};
    memcpy(address.addr, ip, 4);
    assert(snx_socket_send(socket, wire, size, SNX_MSG_DONTWAIT, &address) == (int64_t)size);
}
static void join_failures(void) {
    FakeRoom* relay = fake_room_start();
    char endpoint[64];
    snprintf(endpoint, sizeof endpoint, "127.0.0.1:%u", fake_room_port(relay));
    snprintf(g_settings.eden_room, sizeof g_settings.eden_room, "%s", endpoint);
    SnxRoom* room = NULL;
    int why;
    assert(!snx_room_join(&room, "127.0.0.1", fake_room_port(relay), "WireHost", "secret", "fixture", 0, "", &why));
    SnxSocket* socket = wire_socket(room);
    uint8_t host_ip[4];
    snx_room_fake_ip(room, host_ip);
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        uint32_t handle;
        SnxCtrProcess* p = process(&handle, "Player");
        uint8_t state[952] = {'S', 'N', 'U', 'D', 1, 1, 0xa8, 3, 42};
        state[16] = 2;
        state[17] = 0x53;
        memcpy(state + 18, host_ip, 4);
        state[16 + 29] = 2;
        const uint8_t broadcast[4] = {192, 168, 255, 255};
        wire_send(socket, broadcast, state, sizeof state);
        uint32_t event;
        assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &event));
        uint32_t scan[21] = {0xf0404, 2048};
        scan[4] = scan[5] = UINT32_MAX;
        scan[18] = event;
        scan[19] = 2048 << 4 | 12;
        scan[20] = BUFFER;
        int64_t end = snx_net_now_us() + 2000000;
        do {
            request(p, handle, scan, 21);
            if (ctr_word(p, BUFFER + 8)) break;
            snx_net_sleep_us(1000);
        } while (snx_net_now_us() < end);
        assert(ctr_word(p, BUFFER + 8) == 1);
        assert(snx_ctr_write_ram(p->space, NET, state + 16, 264) == SNX_CTR_MEM_OK);
        assert(snx_ctr_write_ram(p->space, DATA, "wrong-pass", 10) == SNX_CTR_MEM_OK);
        uint32_t join[] = {0x1e0084, 1, 10, 264 << 14 | 1 << 10 | 2, NET, 10 << 14 | 2, DATA};
        if (!scenario) { /* Legacy ConnectNetwork has the same asynchronous reply. */
            ctr_put(p, CTR_COMMAND, 0x90442);
            for (unsigned i = 0; i < 15; ++i) {
                uint32_t word = (uint32_t)state[16 + 4 * i] | (uint32_t)state[17 + 4 * i] << 8 |
                                (uint32_t)state[18 + 4 * i] << 16 | (uint32_t)state[19 + 4 * i] << 24;
                ctr_put(p, CTR_COMMAND + 4 + 4 * i, word);
            }
            ctr_put(p, CTR_COMMAND + 64, 1);
            ctr_put(p, CTR_COMMAND + 68, 10);
            ctr_put(p, CTR_COMMAND + 72, 10 << 14 | 2);
            ctr_put(p, CTR_COMMAND + 76, DATA);
        } else {
            for (unsigned i = 0; i < 7; ++i) ctr_put(p, CTR_COMMAND + 4 * i, join[i]);
        }
        uint32_t result, caller = p->current_thread;
        uint32_t refs = p->objects[caller].refs;
        int64_t start = snx_net_now_us();
        assert(snx_ctr_ipc_send(p, handle, &result) == SNX_CTR_SVC_BLOCKED);
        assert(snx_net_now_us() - start < 100000);
        assert(p->objects[caller].refs == refs + 1);
        uint32_t worker;
        const uint32_t registers[5] = {0x31, 0x100000, 0, SNX_CTR_STACK_TOP - 0x1000, 0xfffffffe};
        assert(!snx_ctr_thread_create(p, registers, &worker));
        uint32_t shared;
        assert(!snx_ctr_object_create(p, CTR_OBJ_SHARED, &shared));
        p->gsp.shared_object = snx_ctr_handle_object(p, shared);
        ++p->objects[p->gsp.shared_object].refs;
        p->dsp.core = snx_ctr_dsp_new(p->memory, NULL, NULL);
        assert(p->dsp.core);
        p->gsp.next_vblank_ns = p->dsp.next_frame_ns = snx_ctr_now_ns();
        assert(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN);
        assert(p->current_thread == snx_ctr_handle_object(p, worker));
        uint32_t worker_tls = p->cpu.state.cpu.tpidruro + 0x80;
        ctr_put(p, worker_tls, 0xfeedface);
        assert(p->gsp.vblank_count && p->dsp.next_frame_ns > snx_ctr_now_ns());
        if (scenario == 1) { /* CLOSE before SYNC: own node ID is still zero. */
            uint8_t incoming[2048];
            SnxSockAddr source;
            end = snx_net_now_us() + 2000000;
            int64_t size;
            do {
                size = snx_socket_recv(socket, incoming, sizeof incoming, SNX_MSG_DONTWAIT, &source, NULL);
                if (size >= 16 && incoming[5] == 2) break;
                snx_net_sleep_us(1000);
            } while (snx_net_now_us() < end);
            assert(size == 88 && incoming[5] == 2);
            state[5] = 6;
            state[6] = state[7] = 0;
            wire_send(socket, source.addr, state, 16);
        } else if (scenario == 2) {
            uint32_t disconnect[] = {0xa0000};
            /* Dispatch from the worker: its reply must not overwrite the caller's. */
            SnxCtrIpc c = {.process = p};
            c.in[0] = disconnect[0];
            snx_ctr_uds_dispatch(&c);
            assert(!c.out[1]);
        } else if (scenario == 3) {
            snx_ctr_uds_close(p);
        } else if (scenario == 4) {
            p->objects[caller].data.thread.dead = 1;
        }
        end = snx_net_now_us() + 6000000;
        while (p->objects[caller].data.thread.wake_ns && snx_net_now_us() < end) {
            snx_ctr_uds_poll(p);
            if (scenario == 4) break;
            assert(snx_ctr_schedule(p) == SNX_CTR_SCHED_RUN);
            snx_net_sleep_us(1000);
        }
        assert(p->objects[caller].refs == refs);
        assert(ctr_word(p, worker_tls) == 0xfeedface);
        if (scenario == 4)
            assert(ctr_word(p, CTR_COMMAND) == join[0]);
        else {
            assert(!p->objects[caller].data.thread.wake_ns);
            assert(ctr_word(p, CTR_COMMAND) == (scenario ? 0x1e0040u : 0x90040u));
            assert(ctr_word(p, CTR_COMMAND + 4) == 0xc8a113f8u);
            if (!scenario) assert(snx_net_now_us() - start >= 4900000);
        }
        snx_ctr_process_free(p);
    }
    snx_socket_unref(socket);
    snx_room_leave(room);
    fake_room_stop(relay);
    puts("UDS: join timeout, early CLOSE, disconnect, shutdown, dead caller, guest scheduling, display and DSP passed");
}
/* the launcher's data folder for the room's password, users/online.ini's room_password (empty: no file) */
static char* data_folder(const char* password) {
    char *root = snx_temp_dir("."), *app = root ? snx_path_join(root, ".scarletnx") : NULL;
    char *users = app ? snx_path_join(app, "users") : NULL, *online = users ? snx_path_join(users, "online.ini") : NULL;
    char line[300];
    int n = snprintf(line, sizeof line, "room_password=%s\n", password);
    assert(online && !snx_mkdirs(users) && (!*password || !snx_write_file(online, line, (size_t)n)));
    snx_ctr_uds_settings(app);
    free(online);
    free(users);
    free(app);
    return root;
}
static char g_said[2048]; /* snx_error's lines */
static void said(const char* line) {
    size_t n = strlen(g_said);
    snprintf(g_said + n, sizeof g_said - n, "%s\n", line);
}
/* The variables an older version read for the room, its password and the nickname (docs/DEPRECATIONS.md): still read
   over the settings, each said once a run with what the run takes */
static void old_variables(void) {
    FakeRoom* relay = fake_room_start();
    char endpoint[64], want[256], *root = data_folder("not-the-room's");
    uint32_t handle;
    snprintf(endpoint, sizeof endpoint, "127.0.0.1:%u", fake_room_port(relay));
    g_settings.eden_room[0] = 0;
    env("SNX_3DS_ROOM", endpoint);
    env("SNX_3DS_ROOM_PASSWORD", "secret");
    env("SNX_3DS_NICKNAME", "OldVariable");
    snx_error_sink(said);
    /* the join needs the room and the password the variables name */
    SnxCtrProcess* p = process(&handle, "OldVariable");
    snprintf(want, sizeof want,
             "3ds uds: SNX_3DS_ROOM=%s is deprecated, replaced by the room of the launcher's Network settings (\"\" "
             "there): this run joins %s (docs/DEPRECATIONS.md)\n",
             endpoint, endpoint);
    assert(strstr(g_said, want));
    assert(strstr(g_said, "3ds uds: SNX_3DS_NICKNAME is deprecated, with nothing in its place: this run joins as its "
                          "value, later ones as the user's nickname and a random suffix"));
    assert(!strstr(g_said, "OldVariable")); /* the nickname, which can name the player, never in the log */
    assert(strstr(g_said, "3ds uds: SNX_3DS_ROOM_PASSWORD is deprecated, replaced by room_password in "
                          "users/online.ini: this run joins with its value, not online.ini's"));
    snx_ctr_process_free(p);
    g_said[0] = 0;
    p = process(&handle, "OldVariable");
    assert(!g_said[0]); /* once a run */
    snx_ctr_process_free(p);
    snx_error_sink(NULL);
    env("SNX_3DS_ROOM", NULL);
    env("SNX_3DS_ROOM_PASSWORD", NULL);
    env("SNX_3DS_NICKNAME", NULL);
    assert(!snx_remove_tree(root));
    free(root);
    fake_room_stop(relay);
    puts("UDS: an older version's room, password and nickname variables, still read and said");
}
int main(int argc, char** argv) {
    const char* public_endpoint = getenv("SNX_TEST_UDS_ROOM");
    int public_room = argc == 2 && !strcmp(argv[1], "--public");
    if (public_room && (!public_endpoint || !*public_endpoint)) {
        fprintf(stderr, "--public needs SNX_TEST_UDS_ROOM=host:port\n");
        return 2;
    }
#ifndef _WIN32
    captures_apart(); /* before any thread: these fork */
    capture_lost_fd();
#endif
    snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "room");
    int delayed = argc == 2 && !strcmp(argv[1], "--delay");
    FakeRoom* room = public_room ? NULL : fake_room_start();
    if (delayed) fake_room_datagram_delay(room, 120);
    char endpoint[64];
    if (room) snprintf(endpoint, sizeof endpoint, "127.0.0.1:%u", fake_room_port(room));
    snprintf(g_settings.eden_room, sizeof g_settings.eden_room, "%s", public_room ? public_endpoint : endpoint);
    char* data = data_folder(public_room ? "" : "secret");
    uint32_t host_h, client_h;
    SnxCtrProcess* host = process(&host_h, "Player");
    uint8_t network[264] = {0};
    network[6] = 11;
    network[16] = 0x12;
    network[17] = 0x34;
    network[18] = 0x56;
    network[19] = 0x78;
    network[20] = 1;
    network[29] = 2;
    assert(snx_ctr_write_ram(host->space, NET, network, sizeof network) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write_ram(host->space, DATA, "game-pass", 10) == SNX_CTR_MEM_OK);
    uint32_t hosting[] = {0x1d0044, 10, 264 << 14 | 1 << 10 | 2, NET, 10 << 14 | 2, DATA};
    request(host, host_h, hosting, 6);
    bind_channel(host, host_h);
    Pump task = {.p = host};
    assert(!snx_net_thread(pump, &task));
    SnxCtrProcess* client = process(&client_h, "Player");
    if (room) {
        char game_name[192];
        uint64_t game_id = UINT64_MAX;
        fake_room_last_game(room, game_name, sizeof game_name, &game_id);
        assert(!game_id && !strcmp(game_name, "UDS relay fixture"));
    }
    if (room) {
        SnxRoom* observer = NULL;
        int why;
        assert(!snx_room_join(&observer, "127.0.0.1", fake_room_port(room), "BeaconObserver", "secret", "fixture", 0,
                              "", &why));
        SnxSocket* socket = wire_socket(observer);
        uint8_t wire[2048];
        SnxSockAddr source;
        int64_t end = snx_net_now_us() + 3000000, size;
        do {
            size = snx_socket_recv(socket, wire, sizeof wire, SNX_MSG_DONTWAIT, &source, NULL);
            if (size == 952 && !memcmp(wire, "SNUD", 4) && wire[5] == 1) break;
            snx_net_sleep_us(1000);
        } while (snx_net_now_us() < end);
        assert(size == 952 && wire[5] == 1);
        const uint8_t zero[32] = {0};
        assert(!memcmp(wire + 16 + 904, zero, sizeof zero));
        /* Knowing the beacon/session must not authenticate a room observer. */
        wire[5] = 2;
        wire[6] = 72;
        wire[7] = 0;
        memset(wire + 16, 0, 72);
        wire_send(socket, source.addr, wire, 88);
        snx_net_sleep_us(200000);
        snx_socket_unref(socket);
        snx_room_leave(observer);
    }

    uint32_t event = 0;
    assert(!snx_ctr_object_create(client, CTR_OBJ_EVENT, &event));
    uint32_t scanning[21] = {0xf0404, 2048};
    scanning[4] = scanning[5] = UINT32_MAX;
    scanning[15] = 0x12345678;
    scanning[16] = 1;
    scanning[18] = event;
    scanning[19] = 2048 << 4 | 12;
    scanning[20] = BUFFER;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int64_t before = snx_net_now_us();
        request(client, client_h, scanning, 21);
        assert(snx_net_now_us() - before < 100000); /* never host-sleep for 300 ms */
        assert(snx_ctr_thread(client)->wake_ns > snx_ctr_now_ns());
        if (ctr_word(client, BUFFER + 8)) break;
        snx_net_sleep_us(20000);
    }
    assert(ctr_word(client, BUFFER + 8) == 1);
    uint8_t beacon[2048];
    unsigned bytes = ctr_word(client, BUFFER + 4);
    assert(bytes <= sizeof beacon);
    assert(snx_ctr_read_ram(client->space, BUFFER, beacon, bytes) == SNX_CTR_MEM_OK);
    memcpy(network, beacon + 20, 6);
    network[6] = beacon[17];
    unsigned at = 12 + 28 + 12;
    while (at + 2 < bytes) {
        unsigned len = beacon[at + 1];
        assert(at + 2 + len <= bytes);
        if (beacon[at] == 221 && len >= 52 && beacon[at + 5] == 21) {
            memcpy(network + 12, beacon + at + 2, 31);
            network[63] = beacon[at + 53];
            memcpy(network + 64, beacon + at + 54, network[63]);
            break;
        }
        at += 2 + len;
    }
    assert(at + 2 < bytes);
    assert(!memcmp(network + 16, "\x12\x34\x56\x78", 4));
    assert(snx_ctr_write_ram(client->space, NET, network, 264) == SNX_CTR_MEM_OK);
    assert(snx_ctr_write_ram(client->space, DATA, "game-pass", 10) == SNX_CTR_MEM_OK);
    bind_channel(client, client_h);
    uint32_t joining[] = {0x1e0084, 1, 10, 264 << 14 | 1 << 10 | 2, NET, 10 << 14 | 2, DATA};
    request(client, client_h, joining, 7);
    atomic_store(&task.stop, 1);
    while (!atomic_load(&task.done)) snx_net_sleep_us(1000);
    /* Network probes must measure the whole peer round trip, including a
       deliberately delayed relay, and never reach the game's data channel. */
    for (unsigned i = 0; i < 7000; ++i) {
        snx_ctr_uds_poll(host);
        snx_ctr_uds_poll(client);
        snx_net_sleep_us(1000);
        if (snx_ctr_uds_rtt_ns(host) && snx_ctr_uds_rtt_ns(client)) break;
    }
    assert(snx_ctr_uds_rtt_ns(host) && snx_ctr_uds_rtt_ns(client));
    if (delayed) {
        assert(snx_ctr_uds_rtt_ns(host) >= 240000000ull);
        assert(snx_ctr_uds_rtt_ns(client) >= 240000000ull);
    }
    printf("UDS measured peer RTT: host %.1fms client %.1fms\n", snx_ctr_uds_rtt_ns(host) / 1000000.0,
           snx_ctr_uds_rtt_ns(client) / 1000000.0);
    uint32_t status[] = {0xb0000};
    request(host, host_h, status, 1);
    assert(ctr_word(host, CTR_COMMAND + 8) == 6);
    request(client, client_h, status, 1);
    assert(ctr_word(client, CTR_COMMAND + 8) == 9);
    uint32_t attributes[] = {0x70080, 2, 1};
    request(host, host_h, attributes, 3);
    send_data(host, host_h, 2, "hello-client");
    receive_data(client, client_h, 1, "hello-client");
    send_data(client, client_h, 1, "hello-host");
    receive_data(host, host_h, 2, "hello-host");
    /* A short compile stall must not discard reliable game inputs. This burst
       exceeds both the former 64-packet cap and the requested 8 KiB binding. */
    for (unsigned i = 0; i < 96; ++i) {
        char payload[128];
        memset(payload, 'x', sizeof payload);
        snprintf(payload, 8, "%06u", i);
        payload[6] = ':';
        payload[7] = 'x';
        payload[127] = 0;
        send_data(host, host_h, 2, payload);
    }
    for (unsigned i = 0; i < 500; ++i) {
        snx_ctr_uds_poll(client);
        snx_ctr_uds_poll(host);
        snx_net_sleep_us(1000);
    }
    ctr_put(client, CTR_COMMAND + 256, 256 << 14 | 2);
    ctr_put(client, CTR_COMMAND + 260, BUFFER);
    uint32_t burst_pull[] = {0x1400c0, 1, 64, 256};
    for (unsigned i = 0; i < 96; ++i) {
        char expected[8], actual[128];
        snprintf(expected, sizeof expected, "%06u:", i);
        request(client, client_h, burst_pull, 4);
        assert(ctr_word(client, CTR_COMMAND + 8) == 128 && ctr_word(client, CTR_COMMAND + 12) == 1);
        assert(snx_ctr_read_ram(client->space, BUFFER, actual, sizeof actual) == SNX_CTR_MEM_OK);
        assert(!memcmp(actual, expected, 7) && actual[127] == 0);
    }
    request(client, client_h, burst_pull, 4);
    assert(!ctr_word(client, CTR_COMMAND + 8));
    SnxCtrIpc bad = {.process = client};
    bad.in[0] = 0x170182;
    bad.in[2] = 1;
    bad.in[3] = 7;
    bad.in[5] = 1479;
    snx_ctr_uds_dispatch(&bad);
    assert(bad.out[1] && !bad.transport_result);
    memset(&bad, 0, sizeof bad);
    bad.process = client;
    bad.in[0] = 0x170000;
    snx_ctr_uds_dispatch(&bad);
    assert(bad.transport_result == SNX_CTR_IPC_BAD_DESCRIPTOR);
    memset(&bad, 0, sizeof bad);
    bad.process = client;
    bad.in[0] = 0x220000;
    snx_ctr_uds_dispatch(&bad);
    assert(bad.state == SNX_CTR_IPC_UNSUPPORTED);
    uint32_t disconnect[] = {0xa0000};
    request(client, client_h, disconnect, 1);
    /* The delayed relay takes at least 120 ms to forward LEAVE. Wait for the
       observed node count, not 100 nominal 1 ms sleeps (which passed only on
       hosts whose sleep overshot enough). Keep failure bounded in wall time. */
    int64_t disconnect_deadline = snx_net_now_us() + 3000000;
    do {
        snx_ctr_uds_poll(host);
        request(host, host_h, status, 1);
        assert(ctr_word(host, CTR_COMMAND + 8) == 6);
        if ((ctr_word(host, CTR_COMMAND + 52) & 255) == 1) break;
        snx_net_sleep_us(1000);
    } while (snx_net_now_us() < disconnect_deadline);
    assert((ctr_word(host, CTR_COMMAND + 52) & 255) == 1);
    /* The scanning caller sleeps while another guest thread remains runnable. */
    uint32_t worker = 0;
    const uint32_t worker_registers[5] = {0x30, 0x100000, 0, SNX_CTR_STACK_TOP - 0x1000, 0xfffffffe};
    assert(!snx_ctr_thread_create(client, worker_registers, &worker));
    uint32_t scanning_thread = client->current_thread;
    request(client, client_h, scanning, 21);
    int64_t schedule_start = snx_net_now_us();
    assert(snx_ctr_schedule(client) == SNX_CTR_SCHED_RUN);
    assert(client->current_thread != scanning_thread);
    assert(client->current_thread == snx_ctr_handle_object(client, worker));
    assert(snx_net_now_us() - schedule_start < 100000);
    snx_ctr_process_free(client);
    snx_ctr_process_free(host);
    if (room) {
        fake_room_stop(room);
        join_failures();
        old_variables();
    }
    assert(!snx_remove_tree(data));
    free(data);
    puts(
        "UDS: real IPC initialization, relay discovery, host/join, bidirectional packets, queue drain and disconnect passed");
    return 0;
}
