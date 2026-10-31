/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/users.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CMD (SNX_CTR_INITIAL_TLS + 0x80)
static void put(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (8 * i));
    assert(snx_ctr_write_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t get(SnxCtrProcess* p, unsigned word) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, CMD + word * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void request(SnxCtrProcess* p, const uint32_t* words, unsigned n) {
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, CMD, zero, sizeof zero) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < n; ++i) put(p, CMD + i * 4, words[i]);
}
static void send(SnxCtrProcess* p, uint32_t handle, SnxCtrSvcResult state, uint32_t result) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x32) == state);
    assert(p->cpu.state.cpu.r[0] == (state == SNX_CTR_SVC_RETURN ? result : handle));
}
static uint32_t endpoint(SnxCtrProcess* p, uint32_t srv, const char* name) {
    uint32_t words[5] = {0x50100, 0, 0, (uint32_t)strlen(name), 1};
    for (unsigned i = 0; name[i]; ++i) words[1 + i / 4] |= (uint32_t)(unsigned char)name[i] << ((i & 3) * 8);
    request(p, words, 5);
    send(p, srv, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 0) == 0x50042 && !get(p, 1) && get(p, 2) == 0x10);
    return get(p, 3);
}
static void static_output(SnxCtrProcess* p, unsigned size, uint32_t address) {
    put(p, CMD + 256, (size << 14) | 2);
    put(p, CMD + 260, address);
}
static unsigned count_objects(SnxCtrProcess* p) {
    unsigned n = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) n += !!p->objects[i].type;
    return n;
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 4096,
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        assert(snx_ctr_memory_profile(p->memory)->model == (SnxCtrModel)model);
        uint32_t srv;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_SRV, &srv));
        uint32_t reg[] = {0x10002, 0x20, 0xffffffff};
        request(p, reg, 3);
        send(p, srv, SNX_CTR_SVC_RETURN, 0);
        uint32_t apt = endpoint(p, srv, "APT:S"), args[6] = {0x10040, 0};
        request(p, args, 2);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x100c2 && !get(p, 1) && !get(p, 2) && !get(p, 3) && !get(p, 4));
        uint32_t lock = get(p, 5), lock_id = snx_ctr_handle_object(p, lock);
        assert(lock_id == p->apt_lock && p->objects[lock_id].type == CTR_OBJ_MUTEX);
        assert(!snx_ctr_handle_close(p, apt));
        apt = endpoint(p, srv, "APT:S");
        request(p, args, 2);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        uint32_t other_lock = get(p, 5);
        assert(other_lock != lock && snx_ctr_handle_object(p, other_lock) == lock_id);
        assert(!snx_ctr_handle_close(p, other_lock));
        uint32_t* r = p->cpu.state.cpu.r;
        for (unsigned i = 0; i < 2; ++i) {
            r[0] = lock;
            r[2] = r[3] = UINT32_MAX;
            assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && !r[0]);
        }
        assert(p->objects[lock_id].data.mutex.depth == 2);
        for (unsigned i = 0; i < 2; ++i) {
            r[0] = lock;
            assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        }
        r[0] = lock;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e0041f);
        assert(!p->objects[lock_id].data.mutex.owner);
        uint32_t init[] = {0x20080, 0x300, 0};
        request(p, init, 3);
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 1) == SNX_CTR_MEM_OK);
        send(p, apt, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(!p->apt_phase);
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 3) == SNX_CTR_MEM_OK);
        uint32_t duplicates[CTR_HANDLES], n = 0;
        while (n < CTR_HANDLES - 4) assert(!snx_ctr_handle_add(p, snx_ctr_handle_object(p, srv), &duplicates[n++]));
        unsigned before = count_objects(p);
        request(p, init, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd8600413 && !p->apt_phase && count_objects(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, duplicates[i]));
        request(p, init, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x20043 && !get(p, 1) && get(p, 2) == 0x04000000);
        uint32_t notification = get(p, 3), parameter = get(p, 4);
        assert(snx_ctr_handle_object(p, notification) == p->apt_notification);
        assert(snx_ctr_handle_object(p, parameter) == p->apt_parameter);
        assert(p->apt_notification != p->apt_parameter && p->apt_phase == 1);
        request(p, init, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xc8a0cffc);
        uint32_t ready[] = {0x430040, 0x300};
        request(p, ready, 2);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(p->apt_phase == 2);
        uint32_t receive[] = {0xd0080, 0x300, 256};
        static_output(p, 256, 0x301f80);
        unsigned char fill[128], unchanged[128];
        memset(fill, 0x55, sizeof fill);
        assert(snx_ctr_write_ram(p->space, 0x301f80, fill, sizeof fill) == SNX_CTR_MEM_OK);
        request(p, receive, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(p->apt_phase == 2 && get(p, 0) == receive[0]);
        assert(snx_ctr_read_ram(p->space, 0x301f80, unchanged, 128) == SNX_CTR_MEM_OK && !memcmp(fill, unchanged, 128));
        static_output(p, 256, 0x300800);
        uint32_t glance[] = {0xe0080, 0x300, 256};
        request(p, glance, 3); /* a glance answers as the receive does and leaves the parameter queued */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xe0104 && !get(p, 1) && !get(p, 2) && get(p, 3) == 1 && !get(p, 4));
        assert(get(p, 8) == 0x300800 && p->apt_phase == 2);
        static_output(p, 256, 0x300800);
        request(p, receive, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xd0104&& !get(p, 1) && !get(p, 2) && get(p, 3) == 1 && !get(p, 4));
        assert(get(p, 5) == 0x10 && !get(p, 6) && get(p, 7) == 0x400002 && get(p, 8) == 0x300800);
        assert(p->apt_phase == 3);
        uint32_t utility[] = {0x4b00c2, 7, 4, 1, 0x10402, 0x300700};
        put(p, 0x300700, 0x10);
        static_output(p, 1, 0x300710);
        request(p, utility, 6);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(p->apt_phase == 4 && get(p, 0) == 0x4b0082);
        utility[1] = 4;
        utility[2] = 1;
        utility[4] = 0x4402;
        put(p, 0x300700, 0);
        p->shell_closed = 1;
        request(p, utility, 6);
        send(p, apt, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(get(p, 0) == utility[0]);
        p->shell_closed = 0;
        request(p, utility, 6);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x4b0082);
        request(p, ready, 2); /* waiting again, after startup: nothing to record */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(p->apt_phase == 4 && !get(p, 1));
        request(p, receive, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xc8a0cfef && p->apt_phase == 4);
        uint32_t info[] = {0x60040, 0x400};
        request(p, info, 2); /* an applet nobody preloaded: success, nothing set */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x60040 && !get(p, 1));
        uint32_t preload[] = {0x160040, 0x400};
        request(p, preload, 2);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x160040 && !get(p, 1));
        uint32_t registered[] = {0x90040, 0x600};
        request(p, registered, 2); /* not yet asked for */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x90080 && !get(p, 1) && !get(p, 2));
        uint32_t prepare[] = {0x180040, 0x500};
        request(p, prepare, 2); /* preparing one makes it registered too */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x180040 && !get(p, 1));
        registered[1] = 0x500;
        request(p, registered, 2);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x90080 && get(p, 2) == 1);
        request(p, info, 2); /* once preloaded it is registered and loaded */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x601c0 && !get(p, 1) && get(p, 5) == 1 && get(p, 6) == 1);
        /* a library applet's handshake: a Request answers with a capture block, StartLibraryApplet with its exit */
        put(p, 0x300720, 0x2000);
        uint32_t send_parameter[] = {0xc0104, 0x300, 0x500, 2, 0x20, 0, 0, 0x400002, 0x300720};
        request(p, send_parameter, 9);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xc0040 && !get(p, 1));
        static_output(p, 256, 0x300800);
        request(p, glance, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xe0104 && get(p, 2) == 0x500 && get(p, 3) == 3 && !get(p, 4) && get(p, 6));
        static_output(p, 256, 0x300800);
        request(p, receive, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xd0104 && get(p, 3) == 3 && get(p, 6));
        request(p, receive, 3); /* taken */
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xc8a0cfef);
        preload[1] = 0x402;
        request(p, preload, 2);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        uint32_t start[] = {0x1e0084, 0x402, 0x104, 0, 0, 0x100402, 0x300900};
        request(p, start, 7);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x1e0040 && !get(p, 1));
        static_output(p, 256, 0x300800);
        request(p, receive, 3);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xd0104 && get(p, 2) == 0x402 && get(p, 3) == 10 && get(p, 4) == 0x84 && !get(p, 6));
        assert(get(p, 8) == 0x300800 && get(p, 7) == ((256u << 14) | 2));
        unsigned char exit_result[0x84];
        assert(snx_ctr_read_ram(p->space, 0x300800, exit_result, sizeof exit_result) == SNX_CTR_MEM_OK);
        assert(!exit_result[0] && !exit_result[4] && exit_result[8] == 0xff && exit_result[11] == 0xff); /* a Mii, no guest */
        assert(exit_result[0x0c] == 3 && exit_result[0x0c + 0x2e] == 0x40); /* the standard Mii, version 3 */
        uint16_t crc = 0; /* CRC-16/XMODEM of the Mii and the two bytes after it, stored big-endian */
        for (unsigned i = 0x0c; i < 0x6a; ++i) {
            crc ^= (uint16_t)(exit_result[i] << 8);
            for (unsigned bit = 0; bit < 8; ++bit) crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
        }
        assert(exit_result[0x6a] == crc >> 8 && exit_result[0x6b] == (crc & 0xff));
        uint32_t query[] = {0x1020000};
        request(p, query, 1);
        send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 2) == model);
        uint32_t ptm = endpoint(p, srv, "ptm:sysm");
        query[0] = 0x40a0000;
        request(p, query, 1);
        send(p, ptm, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 2) == model);
        uint32_t config[] = {0x8180040, 1};
        request(p, config, 2);
        send(p, ptm, SNX_CTR_SVC_UNSUPPORTED, 0);
        config[1] = 0;
        request(p, config, 2);
        send(p, ptm, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        /* cfg: GetConfig of the sound output mode (libctru's ndsp reads it), stereo, into a writable mapped buffer.
           All three names are served, as Azahar does; another block stops */
        uint32_t cfg = endpoint(p, srv, "cfg:i");
        put(p, 0x300000, 0xffffffffu);
        uint32_t sound[] = {0x10082, 1, 0x70001, 1 << 4 | 0xc, 0x300000};
        request(p, sound, 5);
        send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x10042 && !get(p, 1) && get(p, 2) == (1 << 4 | 0xc) && get(p, 3) == 0x300000);
        unsigned char mode = 0;
        assert(snx_ctr_read_ram(p->space, 0x300000, &mode, 1) == SNX_CTR_MEM_OK && mode == 1);
        sound[1] = 4;
        /* Console names follow the selected ScarletNX profile and obey the
           3DS ten-character field without touching its trailing reserved words. */
        char old_nickname[sizeof g_users[0].nickname];
        memcpy(old_nickname, g_users[0].nickname, sizeof old_nickname);
        strcpy(g_users[0].nickname, "Mario_Test_Long");
        uint32_t username[] = {0x10082, 0x1c, 0xa0000, 0x1c << 4 | 0xc, 0x300000};
        request(p, username, 5);
        send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        unsigned char name_bytes[0x1c];
        assert(snx_ctr_read_ram(p->space, 0x300000, name_bytes, sizeof name_bytes) == SNX_CTR_MEM_OK);
        for (unsigned i = 0; i < 10; ++i)
            assert(name_bytes[i * 2] == (unsigned char)"Mario_Test"[i] && !name_bytes[i * 2 + 1]);
        for (unsigned i = 20; i < sizeof name_bytes; ++i) assert(!name_bytes[i]);
        memcpy(g_users[0].nickname, old_nickname, sizeof old_nickname);
        sound[3] = 4 << 4 | 0xc;
        request(p, sound, 5);
        send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd90103ec); /* InvalidSize */
        sound[1] = 1;
        sound[2] = 0x50005; /* the stereo camera settings: 32 bytes, not 1 */
        sound[3] = 1 << 4 | 0xc;
        request(p, sound, 5);
        send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd90103ec);
        sound[2] = 0x12345678; /* a block no console has */
        request(p, sound, 5);
        send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd90103fa); /* NotFound */
        assert(!snx_ctr_handle_close(p, cfg) && endpoint(p, srv, "cfg:u") && endpoint(p, srv, "cfg:s"));
        /* am:app on a console with no add-on content: one content info for a DLC title, none listed, a title ID that
           is no DLC's refused (Azahar am.cpp) */
        uint32_t am = endpoint(p, srv, "am:app");
        uint32_t dlc_count[] = {0x100100c0, 1, 0x00095000, 0x0004008c};
        request(p, dlc_count, 4);
        send(p, am, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x10010080 && !get(p, 1) && get(p, 2) == 1);
        dlc_count[3] = 0x00040000;
        request(p, dlc_count, 4);
        send(p, am, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xe0e0801f && !get(p, 2));
        uint32_t dlc_list[] = {0x10030142, 0, 1, 0x00095000, 0x0004008c, 0, 0xc, 0x300000};
        request(p, dlc_list, 8);
        send(p, am, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x10030080 && !get(p, 1) && !get(p, 2));
        assert(!snx_ctr_handle_close(p, am));
        uint32_t hid = endpoint(p, srv, "hid:USER"), hid_args[] = {0xa0000};
        n = 0;
        while (n < CTR_HANDLES && !snx_ctr_handle_add(p, snx_ctr_handle_object(p, srv), &duplicates[n])) ++n;
        assert(n > 5);
        for (unsigned i = 0; i < 5; ++i) assert(!snx_ctr_handle_close(p, duplicates[--n]));
        before = count_objects(p);
        request(p, hid_args, 1);
        send(p, hid, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd8600413 && !p->hid_objects[0] && count_objects(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, duplicates[i]));
        request(p, hid_args, 1);
        send(p, hid, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0xa0047 && get(p, 2) == 0x14000000);
        uint32_t shared = get(p, 3);
        for (unsigned i = 0; i < 6; ++i) assert(snx_ctr_handle_object(p, get(p, 3 + i)) == p->hid_objects[i]);
        r[0] = shared;
        r[1] = 0x10000000;
        r[2] = 1;
        r[3] = 0x10000000;
        assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t physical;
        unsigned permissions;
        assert(snx_ctr_query(p->space, 0x10000000, &physical, &permissions) == SNX_CTR_MEM_OK &&
               physical >= p->physical_end && permissions == 1); /* past the application's pool */
        assert(snx_ctr_write_ram(p->space, 0x10000000, fill, 1) == SNX_CTR_MEM_PERMISSION);
        for (unsigned i = 0; i < 6; ++i) {
            assert(!snx_ctr_handle_close(p, get(p, 3 + i)));
            /* the device retains its resources; the shared page mapped above is also kept by its mapping */
            assert(p->objects[p->hid_objects[i]].refs == (i == 0 ? 2u : 1u));
        }
        request(p, hid_args, 1);
        send(p, hid, SNX_CTR_SVC_RETURN, 0);
        for (unsigned i = 0; i < 6; ++i) {
            assert(snx_ctr_handle_object(p, get(p, 3 + i)) == p->hid_objects[i]);
            assert(!snx_ctr_handle_close(p, get(p, 3 + i)));
        }
        uint32_t fs = endpoint(p, srv, "fs:USER");
        reg[0] = 0x8010002;
        request(p, reg, 3);
        send(p, fs, SNX_CTR_SVC_RETURN, 0);
        uint32_t archive[] = {0x80c00c2, 9, 1, 1, 0x4002, 0x300700};
        request(p, archive, 6);
        send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x80c0040 && get(p, 1) == 0xc8804478); /* genuinely no SD backing */
        archive[1] = 3;
        request(p, archive, 6);
        send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x80c0040 && get(p, 1) == 0xc8804464); /* no configured RomFS backing */
        snx_ctr_process_free(p);
    }
    puts("APT startup, real lock/events, grouped handles, checked buffers, PTM/cfg/HID and absent-SD behavior passed");
    return 0;
}
