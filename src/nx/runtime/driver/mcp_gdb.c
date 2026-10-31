/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GDB tools (mcp_int.h): GDB driven through its machine interface (GDB/MI 3), connected to a game's GDB stub
   (nx/runtime/gdbstub.c: gdbserver attached to the runtime), and the guest's state read through it from the
   runtime's own structures by their debug information. A guest thread's KThread is its host thread's tls_thread,
   with the registers in its cpu (a 64-bit game's) or arm (a 32-bit one's), and guest memory is at g_mem. One GDB at a
   time, from gdb_attach to gdb_detach or the server's end. The game resumes in the background (mi-async, the
   -exec-* commands): a console resume would hold GDB's input until the game stops, so no command would reach it
   meanwhile */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/mcp_int.h"
#include "common/utilities/common.h"
#include <yyjson.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static const char unix_only[] = "the GDB tools are UNIX only, as the GDB stub is";
char* mcp_gdb_attach(long port, Text* err) {
    (void)port;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_command(const char* command, long wait, McpNamer name, const void* ctx, Text* err) {
    (void)command;
    (void)wait;
    (void)name;
    (void)ctx;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_threads(McpNamer name, const void* ctx, Text* err) {
    (void)name;
    (void)ctx;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_guest(long thread, int frames, McpNamer name, const void* ctx, Text* err) {
    (void)thread;
    (void)frames;
    (void)name;
    (void)ctx;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_memory(SnxU64 address, long size, Text* err) {
    (void)address;
    (void)size;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_detach(Text* err) {
    mcp_put(err, unix_only);
    return NULL;
}
void mcp_gdb_close(void) {}
int mcp_read_line(Text* pending, Text* line) {
    char chunk[4096];
    (void)pending;
    line->n = 0;
    if (line->p) line->p[0] = 0;
    while (fgets(chunk, sizeof chunk, stdin)) {
        size_t n = strlen(chunk);
        mcp_put_n(line, chunk, n);
        if (n && chunk[n - 1] == '\n') return 1; /* else the rest of a long message */
    }
    return line->n > 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ATTACH_MS    180000     /* connecting: GDB reads the runtime's symbols and its libraries' */
#define COMMAND_MS   30000      /* a command that doesn't resume the game */
#define INTERRUPT_MS 10000      /* the game stopping once interrupted */
#define EXIT_MS      2000       /* GDB ending after -gdb-exit, before it is killed */
#define MAX_WAIT     600        /* the seconds a resume may wait for the game to stop */
#define MAX_LINES    400        /* what a tool shows of GDB's output */
#define MAX_ASYNC    (16 << 10) /* GDB's console text between commands, kept for the next one */
#define MAX_FRAMES   64
#define MAX_MEMORY   4096
#define THREAD_BYTES 8192 /* the most of a KThread read at once */

/* exec's and its users' results: done, refused (an MI error: the session goes on), or GDB gone or not answering (a
   loop of commands stops) */
enum { DONE = 0, REFUSED = -1, LOST = -2 };

/* KThread's members the tools read, at offsets GDB gives from the runtime's debug information */
enum { K_SIZE, K_ID, K_NAME, K_STATE, K_IN_SVC, K_TLS, K_ARG, K_X, K_PC, K_N, K_TPIDRRO, K_R, K_CPSR, K_COUNT };
static const char* const layout[K_COUNT] = {
    "sizeof(KThread)",         "&((KThread*)0)->id",    "&((KThread*)0)->name",        "&((KThread*)0)->state",
    "&((KThread*)0)->in_svc",  "&((KThread*)0)->tls",   "&((KThread*)0)->arg",         "&((KThread*)0)->cpu.x",
    "&((KThread*)0)->cpu.pc",  "&((KThread*)0)->cpu.n", "&((KThread*)0)->cpu.tpidrro", "&((KThread*)0)->arm.r",
    "&((KThread*)0)->arm.cpsr"};

static const char no_gdb[] = "no GDB: gdb_attach connects one";

/* what one command answered: its console output, unescaped, and its result record */
typedef struct {
    Text console;
    yyjson_mut_doc* doc; /* result's */
    yyjson_mut_val* result;
    char cls[16]; /* done, running, connected, error or exit; empty until the record came */
} Reply;

static struct {
    pid_t pid;  /* 0: no GDB */
    pid_t game; /* the game's process, which an interrupt signals */
    int to, from;
    Text pending;    /* what GDB wrote past the last line taken */
    unsigned token;  /* the last command's */
    unsigned stale;  /* a command's that timed out, 0 for none: its result is read and dropped before the next */
    int running;     /* the game runs: resumed and not stopped since */
    int exited;      /* the game is gone */
    Text stop;       /* the last stop, described, until a command reports it */
    Text async;      /* GDB's console text that came while no command ran, for the next one */
    long k[K_COUNT]; /* -1 without the runtime's debug information */
} g_gdb;

/* the guest's memory as the runtime maps it, for one tool call */
typedef struct {
    SnxU64 mem;       /* the host address of guest address 0 (g_mem) */
    SnxU64 base, end; /* what the game may map in (g_layout) */
} Guest;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* a message taken back: a step that may fail without failing the tool */
static void clear(Text* t) {
    t->n = 0;
    if (t->p) t->p[0] = 0;
}

static void reply_free(Reply* r) {
    free(r->console.p);
    yyjson_mut_doc_free(r->doc);
    memset(r, 0, sizeof *r);
}

static void close_gdb(void) {
    if (!g_gdb.pid) return;
    close(g_gdb.to);
    close(g_gdb.from);
    kill(g_gdb.pid, SIGKILL);
    while (waitpid(g_gdb.pid, NULL, 0) < 0 && errno == EINTR) {}
    free(g_gdb.pending.p);
    free(g_gdb.stop.p);
    free(g_gdb.async.p);
    memset(&g_gdb, 0, sizeof g_gdb);
}

/* GDB's next line, without its end, in line: 1, 0 when the deadline passes first (a past one still takes what is
   there already), -1 when GDB is gone */
static int next_line(Text* line, long long deadline) {
    for (;;) {
        char* end = g_gdb.pending.n ? (char*)memchr(g_gdb.pending.p, '\n', g_gdb.pending.n) : NULL;
        struct pollfd p;
        char chunk[4096];
        long long left;
        ssize_t n;
        int r;
        if (end) {
            size_t len = (size_t)(end - g_gdb.pending.p);
            line->n = 0;
            mcp_put_n(line, g_gdb.pending.p, len && end[-1] == '\r' ? len - 1 : len);
            g_gdb.pending.n -= len + 1;
            memmove(g_gdb.pending.p, end + 1, g_gdb.pending.n + 1);
            return 1;
        }
        left = deadline - now_ms();
        if (left < 0) left = 0;
        p.fd = g_gdb.from;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return r < 0 ? -1 : 0;
        n = read(g_gdb.from, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        mcp_put_n(&g_gdb.pending, chunk, (size_t)n);
    }
}

/* ============================================================ GDB/MI's output syntax, into a yyjson document */
/* *s at a c-string's opening quote: the string unescaped (a NUL dropped, as the text is a C string), *s past it */
static char* mi_cstring(const char** s) {
    Text t = {0};
    const char* p = *s + 1;
    mcp_put_n(&t, "", 0);
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'v': c = '\v'; break;
                case 'e': c = 27; break;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0', k;
                        for (k = 0; k < 2 && *p >= '0' && *p <= '7'; k++) v = v * 8 + *p++ - '0';
                        c = (char)v;
                    }
            }
        }
        if (c) mcp_put_n(&t, &c, 1);
    }
    if (*p == '"') p++;
    *s = p;
    return t.p;
}
static yyjson_mut_val* mi_value(yyjson_mut_doc* d, const char** s);
/* name=value results up to close (0: the line's end) into object, *s at close */
static void mi_results(yyjson_mut_doc* d, const char** s, char close, yyjson_mut_val* object) {
    while (**s && **s != close) {
        const char* name = *s;
        char key[64];
        size_t n;
        if (**s == ',') {
            ++*s;
            continue;
        }
        while (**s && **s != '=' && **s != close) ++*s;
        if (**s != '=') return;
        n = (size_t)(*s - name);
        snprintf(key, sizeof key, "%.*s", (int)(n < sizeof key - 1 ? n : sizeof key - 1), name);
        ++*s;
        yyjson_mut_obj_add(object, yyjson_mut_strcpy(d, key), mi_value(d, s));
    }
}
/* a value: a c-string, a tuple (an object) or a list (an array, of the values of a list of results) */
static yyjson_mut_val* mi_value(yyjson_mut_doc* d, const char** s) {
    yyjson_mut_val* v;
    if (**s == '"') {
        char* text = mi_cstring(s);
        v = yyjson_mut_strcpy(d, text);
        free(text);
    } else if (**s == '{') {
        v = yyjson_mut_obj(d);
        ++*s;
        mi_results(d, s, '}', v);
        if (**s == '}') ++*s;
    } else if (**s == '[') {
        v = yyjson_mut_arr(d);
        ++*s;
        while (**s && **s != ']') {
            if (**s == ',') {
                ++*s;
                continue;
            }
            if (**s != '"' && **s != '{' && **s != '[') {
                while (**s && **s != '=' && **s != ']') ++*s;
                if (**s != '=') break;
                ++*s;
            }
            yyjson_mut_arr_append(v, mi_value(d, s));
        }
        if (**s == ']') ++*s;
    } else {
        v = yyjson_mut_null(d);
        if (**s) ++*s;
    }
    return v;
}
static const char* field(yyjson_mut_val* o, const char* name) {
    return yyjson_mut_get_str(yyjson_mut_obj_get(o, name));
}

/* "<func> (<file>:<line>)", or "<func or address> in <library>", of a frame tuple */
static void put_frame(Text* t, yyjson_mut_val* frame) {
    const char *func = field(frame, "func"), *file = field(frame, "file"), *line = field(frame, "line"),
               *addr = field(frame, "addr"), *from = field(frame, "from");
    mcp_put(t, func ? func : addr ? addr : "?");
    if (file && line) {
        mcp_put(t, " (");
        mcp_put(t, file);
        mcp_put(t, ":");
        mcp_put(t, line);
        mcp_put(t, ")");
    } else if (from) {
        mcp_put(t, " in ");
        mcp_put(t, from);
    }
}

/* the game gone: GDB reports it by a *stopped of an exited reason, or, when the stub's connection closes with the
   game (its relay ends on the game's exit before gdbserver's last packet), by =thread-group-exited */
static void gone(const char* code, const char* signal_name) {
    g_gdb.exited = 1;
    g_gdb.running = 0;
    g_gdb.stop.n = 0;
    mcp_put(&g_gdb.stop, "the game exited");
    if (code) {
        mcp_put(&g_gdb.stop, " with code ");
        mcp_put(&g_gdb.stop, code);
    }
    if (signal_name) {
        mcp_put(&g_gdb.stop, " on ");
        mcp_put(&g_gdb.stop, signal_name);
    }
}
/* a *stopped record: the game stopped, and why */
static void stopped(const char* s) {
    yyjson_mut_doc* d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* o = yyjson_mut_obj(d);
    const char *reason, *thread;
    if (*s == ',') s++;
    mi_results(d, &s, 0, o);
    reason = field(o, "reason");
    thread = field(o, "thread-id");
    g_gdb.running = 0;
    g_gdb.stop.n = 0;
    if (reason && !strncmp(reason, "exited", 6))
        gone(field(o, "exit-code"), field(o, "signal-name"));
    else {
        mcp_put(&g_gdb.stop, "stopped");
        if (reason) {
            mcp_put(&g_gdb.stop, " (");
            mcp_put(&g_gdb.stop, reason);
            if (field(o, "signal-name")) {
                mcp_put(&g_gdb.stop, " ");
                mcp_put(&g_gdb.stop, field(o, "signal-name"));
            }
            if (field(o, "bkptno")) {
                mcp_put(&g_gdb.stop, " ");
                mcp_put(&g_gdb.stop, field(o, "bkptno"));
            }
            mcp_put(&g_gdb.stop, ")");
        }
        if (thread) {
            mcp_put(&g_gdb.stop, " in thread ");
            mcp_put(&g_gdb.stop, thread);
        }
        if (yyjson_mut_obj_get(o, "frame")) {
            mcp_put(&g_gdb.stop, " at ");
            put_frame(&g_gdb.stop, yyjson_mut_obj_get(o, "frame"));
        }
    }
    yyjson_mut_doc_free(d);
}

/* One line of GDB's output: the stream records into r's console (into g_gdb.async, capped, when r is NULL: no
   command runs), the running command's result record into r, a stale command's dropped, stops and the game's end
   noted. ^running needs no record of its own: GDB sends it before the resume's *stopped */
static void take(const char* line, Reply* r) {
    unsigned token = 0;
    int tokened = 0;
    while (*line >= '0' && *line <= '9') {
        token = token * 10 + (unsigned)(*line++ - '0');
        tokened = 1;
    }
    if (*line == '~' || *line == '@' || *line == '&') { /* the console, the target, and GDB's log: its warnings */
        const char* s = line + 1;
        if (*s == '"') {
            char* text = mi_cstring(&s);
            Text* to = r ? &r->console : &g_gdb.async;
            if (r || g_gdb.async.n + strlen(text) <= MAX_ASYNC) mcp_put(to, text);
            free(text);
        }
    } else if (*line == '^' && tokened && g_gdb.stale && token == g_gdb.stale)
        g_gdb.stale = 0;
    else if (*line == '^' && tokened && r && token == g_gdb.token) {
        const char* s = line + 1;
        size_t n = strcspn(s, ",");
        snprintf(r->cls, sizeof r->cls, "%.*s", (int)(n < sizeof r->cls - 1 ? n : sizeof r->cls - 1), s);
        s += n;
        if (*s == ',') s++;
        yyjson_mut_doc_free(r->doc);
        r->doc = yyjson_mut_doc_new(NULL);
        r->result = yyjson_mut_obj(r->doc);
        mi_results(r->doc, &s, 0, r->result);
        if (!strcmp(r->cls, "running")) g_gdb.running = 1;
    } else if (!strncmp(line, "*stopped", 8))
        stopped(line + 8);
    else if (!strncmp(line, "*running", 8))
        g_gdb.running = 1;
    else if (!strncmp(line, "=thread-group-exited", 20)) {
        yyjson_mut_doc* d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* o = yyjson_mut_obj(d);
        const char* s = line + 20;
        if (*s == ',') s++;
        mi_results(d, &s, 0, o);
        gone(field(o, "exit-code"), NULL);
        yyjson_mut_doc_free(d);
    }
}

static int write_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* GDB's output that is there now, taken: what came while no command ran (a stop, the game's end, console text) */
static void drain(void) {
    Text line = {0};
    int got;
    while (g_gdb.pid && (got = next_line(&line, 0)) > 0) take(line.p, NULL);
    free(line.p);
    if (g_gdb.pid && got < 0) close_gdb();
}
int mcp_read_line(Text* pending, Text* line) {
    for (;;) {
        char* end = pending->n ? (char*)memchr(pending->p, '\n', pending->n) : NULL;
        struct pollfd p[2];
        char chunk[4096];
        ssize_t n;
        nfds_t count = 1;
        if (end) {
            size_t len = (size_t)(end - pending->p);
            line->n = 0;
            mcp_put_n(line, pending->p, len);
            pending->n -= len + 1;
            memmove(pending->p, end + 1, pending->n + 1);
            return 1;
        }
        p[0].fd = STDIN_FILENO;
        p[0].events = POLLIN;
        p[0].revents = 0;
        if (g_gdb.pid) {
            p[1].fd = g_gdb.from;
            p[1].events = POLLIN;
            p[1].revents = 0;
            count = 2;
        }
        if (poll(p, count, -1) < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (count == 2 && p[1].revents) drain();
        if (!p[0].revents) continue;
        n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { /* the end: a last message without its newline is still one */
            if (!pending->n) return 0;
            line->n = 0;
            mcp_put_n(line, pending->p, pending->n);
            clear(pending);
            return 1;
        }
        mcp_put_n(pending, chunk, (size_t)n);
    }
}

/* An MI command, read to its result record and the prompt after it: DONE with the reply in r (which the caller
   frees), REFUSED with GDB's message in err, or LOST (GDB gone, or not answering by deadline: its answer is then
   dropped when it comes, before the next command's) */
static int exec(const char* command, long long deadline, Reply* r, Text* err) {
    Text line = {0};
    char token[16];
    memset(r, 0, sizeof *r);
    if (!g_gdb.pid) {
        mcp_put(err, no_gdb);
        return LOST;
    }
    drain();
    while (g_gdb.pid && g_gdb.stale) { /* the timed-out command's result, which nothing waits for any more */
        int got = next_line(&line, now_ms() + COMMAND_MS);
        if (got <= 0) {
            free(line.p);
            mcp_put(err, got ? "GDB is gone" : "GDB stopped answering: gdb_detach ends the session");
            if (got < 0) close_gdb();
            return LOST;
        }
        take(line.p, NULL);
    }
    if (!g_gdb.pid) {
        free(line.p);
        mcp_put(err, "GDB is gone");
        return LOST;
    }
    snprintf(token, sizeof token, "%u", ++g_gdb.token);
    if (write_all(g_gdb.to, token, strlen(token)) || write_all(g_gdb.to, command, strlen(command)) ||
        write_all(g_gdb.to, "\n", 1)) {
        free(line.p);
        mcp_put(err, "GDB is gone");
        close_gdb();
        return LOST;
    }
    mcp_put_n(&r->console, "", 0);
    for (;;) {
        int got = next_line(&line, deadline);
        if (got <= 0) {
            free(line.p);
            reply_free(r);
            if (got < 0) {
                mcp_put(err, "GDB is gone");
                close_gdb();
            } else {
                g_gdb.stale = g_gdb.token;
                mcp_put(err, "GDB didn't answer in time");
            }
            return LOST;
        }
        if (!strncmp(line.p, "(gdb)", 5)) {
            if (r->cls[0]) break;
            continue;
        }
        take(line.p, r);
    }
    free(line.p);
    if (!strcmp(r->cls, "error")) {
        const char* msg = field(r->result, "msg");
        mcp_put(err, msg ? msg : "GDB refused the command");
        reply_free(r);
        return REFUSED;
    }
    return DONE;
}
static int run(const char* command, Text* err) {
    Reply r;
    int rc = exec(command, now_ms() + COMMAND_MS, &r, err);
    if (rc == DONE) reply_free(&r);
    return rc;
}

/* GDB's output until the game stops or the deadline passes, its console stream into console: 1 stopped, 0 still
   running, -1 GDB gone */
static int wait_stop(long long deadline, Text* console) {
    Text line = {0};
    Reply r;
    int got = 1;
    memset(&r, 0, sizeof r);
    mcp_put_n(&r.console, "", 0);
    while (g_gdb.running && (got = next_line(&line, deadline)) > 0) take(line.p, &r);
    free(line.p);
    mcp_put(console, r.console.p);
    reply_free(&r);
    if (got < 0) close_gdb();
    return got < 0 ? -1 : !g_gdb.running;
}
/* The game stopped by a SIGINT, which gdbserver takes from it and reports as a stop. Sent to the game itself:
   -exec-interrupt has gdbserver signal the process group the game's pid names, which only exists when the game leads
   its group (the driver and the launcher start it so, a non-interactive shell's job doesn't) */
static int interrupt(Text* console, Text* err) {
    if (kill(g_gdb.game, SIGINT)) {
        mcp_put(err, "the game's process can't be signalled");
        return -1;
    }
    if (wait_stop(now_ms() + INTERRUPT_MS, console) != 1) {
        if (g_gdb.pid) mcp_put(err, "the game didn't stop when interrupted");
        return -1;
    }
    return 0;
}

/* s as an MI c-string */
static void put_quoted(Text* t, const char* s) {
    mcp_put(t, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') mcp_put_n(t, "\\", 1);
        if (*s == '\n')
            mcp_put(t, "\\n");
        else
            mcp_put_n(t, s, 1);
    }
    mcp_put(t, "\"");
}

/* expr's value in thread (0: the selected one; --thread selects no other) as an integer: DONE, or REFUSED (not a
   number, or GDB's error) or LOST, with why in err */
static int eval(long thread, const char* expr, SnxU64* value, Text* err) {
    Text command = {0};
    Reply r;
    const char* v;
    int rc;
    mcp_put(&command, "-data-evaluate-expression ");
    if (thread > 0) {
        mcp_put(&command, "--thread ");
        mcp_put_num(&command, (unsigned long)thread);
        mcp_put(&command, " ");
    }
    put_quoted(&command, expr);
    rc = exec(command.p, now_ms() + COMMAND_MS, &r, err);
    free(command.p);
    if (rc) return rc;
    v = field(r.result, "value");
    rc = v && (isdigit((unsigned char)*v) || *v == '-') ? DONE : REFUSED;
    if (rc == DONE)
        *value = (SnxU64)strtoull(v, NULL, 0);
    else {
        mcp_put(err, expr);
        mcp_put(err, " is not a number: ");
        mcp_put(err, v ? v : "?");
    }
    reply_free(&r);
    return rc;
}

/* size bytes of the runtime's memory at address: DONE, or REFUSED (not all readable) or LOST, with why in err */
static int read_memory(SnxU64 address, size_t size, unsigned char* out, Text* err) {
    char command[96];
    yyjson_mut_val* first;
    const char *contents, *offset;
    Reply r;
    size_t i;
    int rc;
    snprintf(command, sizeof command, "-data-read-memory-bytes 0x%llx %lu", (unsigned long long)address,
             (unsigned long)size);
    if ((rc = exec(command, now_ms() + COMMAND_MS, &r, err)) != DONE) return rc;
    first = yyjson_mut_arr_get_first(yyjson_mut_obj_get(r.result, "memory"));
    contents = field(first, "contents");
    offset = field(first, "offset");
    if (contents && offset && !strtoull(offset, NULL, 0) && strlen(contents) >= 2 * size) {
        for (i = 0; i < size; i++) {
            unsigned v = 0;
            sscanf(contents + 2 * i, "%2x", &v);
            out[i] = (unsigned char)v;
        }
    } else {
        char what[96];
        snprintf(what, sizeof what, "%lu bytes at %llx can't all be read", (unsigned long)size,
                 (unsigned long long)address);
        mcp_put(err, what);
        rc = REFUSED;
    }
    reply_free(&r);
    return rc;
}

/* guest memory, which is little endian whatever the host is */
static SnxU64 get64(const unsigned char* p) {
    SnxU64 v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static SnxU32 get32(const unsigned char* p) {
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}
/* the guest's memory map, for this call: DONE, or REFUSED (not mapped yet) or LOST */
static int guest_map(Guest* g, Text* err) {
    int rc;
    if ((rc = eval(0, "(unsigned long)g_mem", &g->mem, err)) || (rc = eval(0, "g_layout.base", &g->base, err)) ||
        (rc = eval(0, "g_layout.end", &g->end, err)))
        return rc;
    if (!g->mem) {
        mcp_put(err, "guest memory isn't mapped yet");
        return REFUSED;
    }
    return DONE;
}
/* size bytes of guest memory at a, within what the game may map */
static int guest_read(const Guest* g, SnxU64 a, size_t size, unsigned char* out, Text* err) {
    if (a < g->base || a >= g->end || size > g->end - a) {
        char what[96];
        snprintf(what, sizeof what, "%llx is outside the guest's memory", (unsigned long long)a);
        mcp_put(err, what);
        return REFUSED;
    }
    return read_memory(g->mem + a, size, out, err);
}

/* A symbol of the recompiled code, at s (after a byte that can't be in a name): the guest address it names, and its
   length. F_<address> is a 64-bit game's function (upper case digits), A_ and T_<8 digits> a 32-bit game's A32 and
   Thumb blocks, a32_<8 digits>_<0 or 1> a block the runtime's 32-bit JIT compiled (lower case digits) */
static size_t guest_symbol(const char* s, SnxU64* address) {
    size_t n = 0, at;
    const char* digits = "0123456789ABCDEF";
    if (s[0] == 'F' && s[1] == '_')
        at = 2;
    else if ((s[0] == 'A' || s[0] == 'T') && s[1] == '_') {
        at = 2;
        digits = "0123456789abcdef";
    } else if (!strncmp(s, "a32_", 4)) {
        at = 4;
        digits = "0123456789abcdef";
    } else
        return 0;
    while (s[at + n] && strchr(digits, s[at + n])) n++;
    if (!n || n > 16) return 0;
    *address = (SnxU64)strtoull(s + at, NULL, 16);
    if (at == 4) { /* the Thumb flag after the address */
        if (n != 8 || s[at + n] != '_' || (s[at + n + 1] != '0' && s[at + n + 1] != '1')) return 0;
        n += 2;
    }
    if (isalnum((unsigned char)s[at + n]) || s[at + n] == '_') return 0;
    return at + n;
}

/* The answer, MAX_LINES lines at most. Unless raw (a memory dump, whose bytes are the guest's), the recompiled code's
   symbols get their guest names after them, and source paths are cut to src/ on: shorter, and without the folders
   above the checkout (the person's) */
static char* finish(Text* t, McpNamer name, const void* ctx, int raw) {
    Text out = {0};
    const char* p = t->p ? t->p : "";
    int lines = 0;
    size_t i;
    mcp_put_n(&out, "", 0);
    for (i = 0; p[i]; i++) {
        SnxU64 address;
        size_t n;
        if (p[i] == '\n' && ++lines >= MAX_LINES && p[i + 1]) {
            unsigned long more = 0;
            size_t j;
            for (j = i + 1; p[j]; j++) more += p[j] == '\n';
            if (p[j - 1] != '\n') more++; /* the last line, unended */
            mcp_put(&out, "\n... ");
            mcp_put_num(&out, more);
            mcp_put(&out, " more lines\n");
            break;
        }
        if (!raw && p[i] == '/' && (!i || p[i - 1] == ' ' || p[i - 1] == '(' || p[i - 1] == '=')) {
            size_t end = i + strcspn(p + i, " \t\n:),"), k, cut = 0;
            for (k = i + 1; k + 5 <= end; k++)
                if (!strncmp(p + k, "/src/", 5)) cut = k; /* the last: a checkout may itself be under a src/ */
            if (cut) {
                i = cut;
                continue; /* the next byte written is the 's' after this '/' */
            }
        }
        if (!raw && name && (!i || (!isalnum((unsigned char)p[i - 1]) && p[i - 1] != '_')) &&
            (n = guest_symbol(p + i, &address)) != 0) {
            mcp_put_n(&out, p + i, n);
            mcp_put(&out, " [");
            name(&out, ctx, address);
            mcp_put(&out, "]");
            i += n - 1;
            continue;
        }
        mcp_put_n(&out, p + i, 1);
    }
    free(t->p);
    return out.p;
}
/* a guest address, named (with mask's bits off, a Thumb return address's bit 0) */
static void put_guest(Text* t, SnxU64 a, SnxU64 mask, McpNamer name, const void* ctx) {
    mcp_put_hex(t, a);
    if (!name) return;
    mcp_put(t, " [");
    name(t, ctx, a & ~mask);
    mcp_put(t, "]");
}

/* a session with GDB and a stopped game; layout: the KThread layout read at attach too */
static int need_stopped(Text* err, int layout_too) {
    if (g_gdb.pid) drain();
    if (!g_gdb.pid) {
        mcp_put(err, no_gdb);
        return -1;
    }
    if (g_gdb.exited) {
        mcp_put(err, "the game exited: gdb_detach ends the session");
        return -1;
    }
    if (g_gdb.running) {
        mcp_put(err, "the game is running: gdb_command interrupt stops it");
        return -1;
    }
    if (layout_too && g_gdb.k[K_SIZE] <= 0) {
        mcp_put(err, "the runtime has no debug information: GDB can't find its threads' KThread (a build with it, "
                     "the default preset's RelWithDebInfo, not a release built with -g0)");
        return -1;
    }
    return 0;
}

/* ============================================================ the tools */
char* mcp_gdb_attach(long port, Text* err) {
    /* each needed */
    static const char* const setup[] = {
        "-gdb-set mi-async on", "-gdb-set pagination off", "-gdb-set confirm off", "-gdb-set width 0",
        "-gdb-set print thread-events off", "-gdb-set auto-load off",
        /* the game runs on this machine: its libraries are read from here, not through the remote protocol */
        "-gdb-set sysroot /",
        /* the runtime's own: plat_freeze's write faults on memory it moves, the crash report's faults, and a closed
           socket's SIGPIPE, which it handles */
        "-interpreter-exec console \"handle SIGSEGV SIGBUS SIGPIPE nostop noprint pass\""};
    /* what an older GDB lacks (debuginfod: GDB 10, styling: 9) goes without */
    static const char* const optional[] = {"-gdb-set debuginfod enabled off", "-gdb-set style enabled off"};
    int in[2] = {-1, -1}, out[2] = {-1, -1}, rc;
    size_t i;
    char* gdb;
    char command[64];
    Text t = {0}, line = {0};
    Reply r;
    SnxU64 v;
    long long deadline;
    if (g_gdb.pid) {
        mcp_put(err, "already attached: gdb_detach first (the stub takes one connection per run)");
        return NULL;
    }
    if (port <= 0 || port > 65535) {
        mcp_put(err, "port: the stub's, 1234");
        return NULL;
    }
    gdb = snx_find_tool("gdb");
    if (!gdb) {
        mcp_put(err, "gdb is not on PATH");
        return NULL;
    }
    signal(SIGPIPE, SIG_IGN); /* a write to a GDB that died fails instead of ending the server */
    if (pipe(in) || pipe(out)) {
        mcp_put(err, "no pipes for GDB");
        goto failed;
    }
    g_gdb.pid = fork();
    if (g_gdb.pid < 0) {
        g_gdb.pid = 0;
        mcp_put(err, "can't start GDB");
        goto failed;
    }
    if (!g_gdb.pid) {
        signal(SIGPIPE, SIG_DFL); /* the server's ignoring it is its own */
        setpgid(0, 0);            /* a ^C meant for the server stays the server's */
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        execl(gdb, gdb, "--nx", "--quiet", "--interpreter=mi3", (char*)NULL);
        _exit(127);
    }
    free(gdb);
    gdb = NULL;
    close(in[0]);
    close(out[1]);
    g_gdb.to = in[1];
    g_gdb.from = out[0];
    fcntl(g_gdb.to, F_SETFD, FD_CLOEXEC);
    fcntl(g_gdb.from, F_SETFD, FD_CLOEXEC);
    for (i = 0; i < K_COUNT; i++) g_gdb.k[i] = -1;
    deadline = now_ms() + COMMAND_MS;
    do {
        if (next_line(&line, deadline) <= 0) {
            free(line.p);
            mcp_put(err, "GDB didn't start");
            close_gdb();
            return NULL;
        }
    } while (strncmp(line.p, "(gdb)", 5));
    free(line.p);
    for (i = 0; i < sizeof setup / sizeof *setup; i++)
        if (run(setup[i], err)) {
            close_gdb();
            return NULL;
        }
    for (i = 0; i < sizeof optional / sizeof *optional; i++)
        if (run(optional[i], err) == LOST) {
            close_gdb();
            return NULL;
        } else
            clear(err);
    /* the stub binds the loopback's IPv4 address only, which localhost may not name first */
    snprintf(command, sizeof command, "-target-select remote 127.0.0.1:%ld", port);
    if (exec(command, now_ms() + ATTACH_MS, &r, err)) {
        mcp_put(err, " (is the game running with -S or -s? The stub takes one connection per run)");
        close_gdb();
        return NULL;
    }
    reply_free(&r);
    /* the game's process, which interrupts signal: gdbserver's on this machine, the stub listening on the loopback */
    rc = exec("-list-thread-groups", now_ms() + COMMAND_MS, &r, err);
    if (rc == DONE) {
        const char* id = field(yyjson_mut_arr_get_first(yyjson_mut_obj_get(r.result, "groups")), "pid");
        g_gdb.game = id ? (pid_t)strtol(id, NULL, 10) : 0;
        reply_free(&r);
    }
    if (g_gdb.game <= 0) {
        if (!err->n) mcp_put(err, "GDB doesn't say which process the game is: an interrupt couldn't stop it");
        close_gdb();
        return NULL;
    }
    g_gdb.running = 0;
    g_gdb.stop.n = 0;
    mcp_put(&t, "Attached to the game at 127.0.0.1:");
    mcp_put_num(&t, (unsigned long)port);
    mcp_put(&t, ". It is stopped: gdb_command continue resumes it.\n");
    rc = exec("-break-insert -f crash_report", now_ms() + COMMAND_MS, &r, err);
    if (rc == LOST) goto lost;
    if (rc == REFUSED) {
        mcp_put(&t, "No breakpoint on crash_report: ");
        mcp_put(&t, err->p);
        mcp_put(&t, "\n");
        clear(err);
    } else {
        const char* number = field(yyjson_mut_obj_get(r.result, "bkpt"), "number");
        mcp_put(&t, "Breakpoint ");
        mcp_put(&t, number ? number : "?");
        mcp_put(&t, " is on crash_report: a crash stops there, with the faulting frames below the signal handler's. "
                    "SIGSEGV and SIGBUS otherwise pass to the runtime (memory it moves, its crash report).\n");
        reply_free(&r);
    }
    for (i = 0; i < K_COUNT; i++) {
        char expr[96];
        snprintf(expr, sizeof expr, "(long)(%s)", layout[i]);
        rc = eval(0, expr, &v, err);
        if (rc == LOST) goto lost;
        if (rc) {
            mcp_put(&t, "The runtime's threads can't be read (");
            mcp_put(&t, err->p);
            mcp_put(&t, "): gdb_threads and gdb_guest need its debug information.\n");
            clear(err);
            g_gdb.k[K_SIZE] = -1;
            break;
        }
        g_gdb.k[i] = (long)v;
    }
    return finish(&t, NULL, NULL, 0);
lost:
    free(t.p);
    close_gdb();
    return NULL;
failed:
    free(gdb);
    if (in[0] >= 0) close(in[0]);
    if (in[1] >= 0) close(in[1]);
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    return NULL;
}

/* ============================================================ the commands gdb_command runs */
/* What gdb_command runs: these commands only, each a GDB command or one of its aliases, matched as GDB matches a
   command word (letters, digits, '-', '_' and '.') and by an unambiguous prefix of the list's words. GDB is then sent
   the full word, so a prefix never reaches another GDB command. Nothing here runs a shell or a script or writes a
   file, opens a body read from GDB's input (commands, define, if, while), or changes the session (target, file,
   attach, detach, kill, quit: gdb_attach and gdb_detach do that). print, call and the like still evaluate the game's
   own functions, which can do anything the game can */
enum { CONSOLE, RESUME, INTERRUPT };
enum { ANY, NONE, COUNT, WHERE, WHERE_OPT, THREAD, FRAME, SET, MAINT }; /* what may follow the word */
typedef struct {
    const char* word; /* GDB's, an alias's target when alias is set */
    const char* alias;
    int kind, args;
    const char* mi; /* RESUME's command */
} Verb;
static const Verb verbs[] = {
    {"continue", NULL, RESUME, NONE, "-exec-continue"},
    {"continue", "c", RESUME, NONE, "-exec-continue"},
    {"continue", "fg", RESUME, NONE, "-exec-continue"},
    {"next", NULL, RESUME, COUNT, "-exec-next"},
    {"next", "n", RESUME, COUNT, "-exec-next"},
    {"step", NULL, RESUME, COUNT, "-exec-step"},
    {"step", "s", RESUME, COUNT, "-exec-step"},
    {"stepi", NULL, RESUME, COUNT, "-exec-step-instruction"},
    {"stepi", "si", RESUME, COUNT, "-exec-step-instruction"},
    {"nexti", NULL, RESUME, COUNT, "-exec-next-instruction"},
    {"nexti", "ni", RESUME, COUNT, "-exec-next-instruction"},
    {"finish", NULL, RESUME, NONE, "-exec-finish"},
    {"until", NULL, RESUME, WHERE_OPT, "-exec-until"},
    {"until", "u", RESUME, WHERE_OPT, "-exec-until"},
    {"advance", NULL, RESUME, WHERE, "-exec-until"},
    {"jump", NULL, RESUME, WHERE, "-exec-jump"},
    {"interrupt", NULL, INTERRUPT, NONE, NULL},
    {"backtrace", NULL, CONSOLE, ANY, NULL},
    {"backtrace", "bt", CONSOLE, ANY, NULL},
    {"backtrace", "where", CONSOLE, ANY, NULL},
    {"info", NULL, CONSOLE, ANY, NULL},
    {"info", "i", CONSOLE, ANY, NULL},
    {"print", NULL, CONSOLE, ANY, NULL},
    {"print", "p", CONSOLE, ANY, NULL},
    {"print", "inspect", CONSOLE, ANY, NULL},
    {"output", NULL, CONSOLE, ANY, NULL},
    {"call", NULL, CONSOLE, ANY, NULL},
    {"x", NULL, CONSOLE, ANY, NULL},
    {"ptype", NULL, CONSOLE, ANY, NULL},
    {"whatis", NULL, CONSOLE, ANY, NULL},
    {"list", NULL, CONSOLE, ANY, NULL},
    {"list", "l", CONSOLE, ANY, NULL},
    {"disassemble", NULL, CONSOLE, ANY, NULL},
    {"frame", NULL, CONSOLE, FRAME, NULL},
    {"frame", "f", CONSOLE, FRAME, NULL},
    {"up", NULL, CONSOLE, COUNT, NULL},
    {"down", NULL, CONSOLE, COUNT, NULL},
    {"thread", NULL, CONSOLE, THREAD, NULL},
    {"break", NULL, CONSOLE, ANY, NULL},
    {"break", "b", CONSOLE, ANY, NULL},
    {"tbreak", NULL, CONSOLE, ANY, NULL},
    {"hbreak", NULL, CONSOLE, ANY, NULL},
    {"thbreak", NULL, CONSOLE, ANY, NULL},
    {"watch", NULL, CONSOLE, ANY, NULL},
    {"rwatch", NULL, CONSOLE, ANY, NULL},
    {"awatch", NULL, CONSOLE, ANY, NULL},
    {"delete", NULL, CONSOLE, ANY, NULL},
    {"delete", "d", CONSOLE, ANY, NULL},
    {"disable", NULL, CONSOLE, ANY, NULL},
    {"enable", NULL, CONSOLE, ANY, NULL},
    {"condition", NULL, CONSOLE, ANY, NULL},
    {"ignore", NULL, CONSOLE, ANY, NULL},
    {"display", NULL, CONSOLE, ANY, NULL},
    {"undisplay", NULL, CONSOLE, ANY, NULL},
    {"show", NULL, CONSOLE, ANY, NULL},
    {"set", NULL, CONSOLE, SET, NULL},
    {"maintenance", NULL, CONSOLE, MAINT, NULL},
    {"maintenance", "mt", CONSOLE, MAINT, NULL},
};

static int word_char(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.'; }
/* the word at s, n bytes long, as one of the list's: by its alias or its own name exactly, else an unambiguous prefix
   of one name. NULL when none or several */
static const Verb* verb_of(const char* s, size_t n) {
    const Verb* found = NULL;
    size_t i;
    for (i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        const char* w = verbs[i].alias ? verbs[i].alias : verbs[i].word;
        if (strlen(w) == n && !strncmp(w, s, n)) return &verbs[i];
    }
    for (i = 0; i < sizeof verbs / sizeof *verbs; i++)
        if (!verbs[i].alias && strlen(verbs[i].word) > n && !strncmp(verbs[i].word, s, n)) {
            if (found) return NULL;
            found = &verbs[i];
        }
    return found;
}
/* the first word of s and its length, past spaces */
static const char* first_word(const char* s, size_t* n) {
    while (*s == ' ') s++;
    for (*n = 0; word_char(s[*n]); ++*n) {}
    return s;
}
/* whether args (past the verb, spaces skipped) suit kind: 1, or 0 with why in err */
static int args_fit(int kind, const char* args, Text* err) {
    size_t n;
    const char* w = first_word(args, &n);
    switch (kind) {
        case NONE:
            if (!*args) return 1;
            mcp_put(err, "it takes nothing after it here");
            return 0;
        case COUNT:
            if (!*args || strspn(args, "0123456789") == strlen(args)) return 1;
            mcp_put(err, "only a count may follow it");
            return 0;
        case WHERE:
            if (*args) return 1;
            mcp_put(err, "a location must follow it");
            return 0;
        case THREAD:
        case FRAME: /* their apply runs any command, on each thread or frame */
            if (!n || strncmp(w, "apply", n)) return 1;
            mcp_put(err, "its apply form runs any command: give it each thread or frame's command instead");
            return 0;
        case SET: /* a variable, and how values print: the rest changes the session or writes files */
            if ((n == 3 && !strncmp(w, "var", 3)) || (n == 8 && !strncmp(w, "variable", 8)) ||
                (n == 5 && !strncmp(w, "print", 5)) || (n == 17 && !strncmp(w, "scheduler-locking", 17)))
                return 1;
            mcp_put(err, "only set var, set print and set scheduler-locking here");
            return 0;
        case MAINT:
            if (n == 4 && !strncmp(w, "info", 4)) return 1;
            mcp_put(err, "only maintenance info here");
            return 0;
        default: return 1;
    }
}
/* command checked and read: its verb and what follows it (into args, spaces trimmed), or NULL with why in err */
static const Verb* parse_command(const char* command, Text* args, Text* err) {
    const Verb* v;
    const char *s, *rest;
    size_t n, len;
    for (s = command; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) {
            mcp_put(err, "a control character in the command");
            return NULL;
        }
    if (strstr(command, "$_shell")) { /* GDB's convenience function that runs a shell */
        mcp_put(err, "$_shell runs a shell");
        return NULL;
    }
    s = first_word(command, &n);
    v = n ? verb_of(s, n) : NULL;
    if (!v) {
        mcp_put(err, "not a command gdb_command runs (it runs a list of reading, breakpoint and execution commands, "
                     "and no shells, scripts, files, bodies of commands or session changes)");
        return NULL;
    }
    rest = s + n;
    while (*rest == ' ') rest++;
    len = strlen(rest);
    while (len && rest[len - 1] == ' ') len--;
    mcp_put_n(args, rest, len);
    if (!args_fit(v->args, args->p, err)) return NULL;
    return v;
}

char* mcp_gdb_command(const char* command, long wait, McpNamer name, const void* ctx, Text* err) {
    Text t = {0}, mi = {0}, args = {0};
    const Verb* v;
    Reply r;
    size_t kept = 0;
    int rc;
    if (g_gdb.pid) drain();
    if (!g_gdb.pid) {
        mcp_put(err, no_gdb);
        return NULL;
    }
    if (!(v = parse_command(command, &args, err))) goto refused;
    if (g_gdb.exited) {
        mcp_put(err, "the game exited: gdb_detach ends the session");
        goto refused;
    }
    if (wait < 0) wait = 0;
    if (wait > MAX_WAIT) wait = MAX_WAIT;
    mcp_put_n(&t, "", 0);
    if (g_gdb.async.n) { /* what GDB said since the last command (a stop after one left running) */
        mcp_put(&t, g_gdb.async.p);
        if (t.p[t.n - 1] != '\n') mcp_put(&t, "\n");
        kept = t.n;
        clear(&g_gdb.async);
    }
    if (v->kind == INTERRUPT) {
        if (!g_gdb.running) {
            mcp_put(err, "the game is stopped already");
            goto refused;
        }
        if (interrupt(&t, err)) goto refused;
    } else if (g_gdb.running) {
        /* GDB takes commands while the game runs, but they read nothing of it */
        mcp_put(err, "the game is running: gdb_command interrupt stops it");
        goto refused;
    } else {
        if (v->kind == RESUME) {
            mcp_put(&mi, v->mi);
            if (args.n) {
                mcp_put(&mi, " ");
                if (v->args == COUNT)
                    mcp_put(&mi, args.p);
                else
                    put_quoted(&mi, args.p);
            }
        } else {
            Text cli = {0};
            mcp_put(&cli, v->word);
            if (args.n) {
                mcp_put(&cli, " ");
                mcp_put(&cli, args.p);
            }
            mcp_put(&mi, "-interpreter-exec console ");
            put_quoted(&mi, cli.p);
            free(cli.p);
        }
        g_gdb.stop.n = 0;
        rc = exec(mi.p, now_ms() + COMMAND_MS, &r, err);
        if (rc) goto refused;
        mcp_put(&t, r.console.p);
        reply_free(&r);
        if (g_gdb.running && wait > 0) {
            rc = wait_stop(now_ms() + wait * 1000LL, &t);
            if (rc < 0) {
                mcp_put(err, "GDB is gone");
                goto refused;
            }
            if (!rc) {
                mcp_put(&t, "\nStill running after ");
                mcp_put_num(&t, (unsigned long)wait);
                mcp_put(&t, " s: interrupted.\n");
                if (interrupt(&t, err)) { /* what came before goes back with the error */
                    mcp_put(&t, "\nThe interrupt failed: ");
                    mcp_put(&t, err->p);
                    mcp_put(&t, "\n");
                    clear(err);
                }
            }
        }
    }
    if (g_gdb.running)
        mcp_put(&t, "\nThe game is running: gdb_command interrupt stops it.\n");
    else if (g_gdb.stop.n) {
        mcp_put(&t, t.n && t.p[t.n - 1] != '\n' ? "\n" : "");
        mcp_put(&t, g_gdb.stop.p);
        mcp_put(&t, "\n");
        g_gdb.stop.n = 0;
    }
    free(mi.p);
    free(args.p);
    return finish(&t, name, ctx, 0);
refused:
    if (kept && g_gdb.pid) { /* what GDB said before this command waits for the next one, ahead of what came since */
        t.n = kept;
        mcp_put_n(&t, g_gdb.async.p ? g_gdb.async.p : "", g_gdb.async.n);
        free(g_gdb.async.p);
        g_gdb.async = t;
        t.p = NULL;
    }
    free(t.p);
    free(mi.p);
    free(args.p);
    return NULL;
}

/* ============================================================ guest threads */
/* thread's KThread: 1 and its bytes, 0 for a thread of the runtime's own (no tls_thread), or REFUSED or LOST with why
   in err */
static int kthread(long thread, SnxU64* at, unsigned char* k, Text* err) {
    size_t size = (size_t)g_gdb.k[K_SIZE] < THREAD_BYTES ? (size_t)g_gdb.k[K_SIZE] : THREAD_BYTES;
    int rc;
    if ((rc = eval(thread, "(unsigned long)tls_thread", at, err)) != DONE) return rc;
    if (!*at) return 0;
    return (rc = read_memory(*at, size, k, err)) != DONE ? rc : 1;
}
static int is_wide(Text* err) {
    SnxU64 arm;
    int rc = eval(0, "(unsigned long)g_arm_mod", &arm, err);
    return rc ? rc : !arm;
}
/* a member's offset in the bytes read, -1 when past them; the runtime's own memory, in the host's byte order */
static long k_off(int member, size_t index, size_t width) {
    long off = g_gdb.k[member] + (long)(index * width);
    return g_gdb.k[member] >= 0 && off + (long)width <= g_gdb.k[K_SIZE] && off + (long)width <= THREAD_BYTES ? off : -1;
}
static SnxU64 k64(const unsigned char* k, int member, size_t index) {
    long off = k_off(member, index, 8);
    SnxU64 v = 0;
    if (off >= 0) memcpy(&v, k + off, 8);
    return v;
}
static SnxU32 k32(const unsigned char* k, int member, size_t index) {
    long off = k_off(member, index, 4);
    SnxU32 v = 0;
    if (off >= 0) memcpy(&v, k + off, 4);
    return v;
}
/* The thread's name as the SDK keeps it, as hle/kernel/debug.c's sdk_name reads it: its nn::os ThreadType from the
   TLS (0x1F8, or 0x1FC in a 32-bit game), checked against the argument it was started with, and the name pointer at
   0x1A0 or 0x1A8 by the type's version at 0x46 (0xE4 or 0xE8 by 0x26). The KThread's own name (main's) otherwise */
static void put_kname(Text* t, const Guest* g, const unsigned char* k, int wide) {
    unsigned char b[64];
    char name[65];
    Text ignored = {0};
    SnxU64 tls = k64(k, K_TLS, 0), arg = k64(k, K_ARG, 0), type = 0, ptr = 0;
    long off = k_off(K_NAME, 0, 32);
    int found = 0;
    if (g && tls && !guest_read(g, tls + (wide ? 0x1F8 : 0x1FC), wide ? 8 : 4, b, &ignored)) {
        type = wide ? get64(b) : get32(b);
        if (type && (!arg || type == arg) && !guest_read(g, type + (wide ? 0x46 : 0x26), 2, b, &ignored)) {
            unsigned version = (unsigned)b[0] | (unsigned)b[1] << 8;
            SnxU64 at = type + (wide ? (version == 1 ? 0x1A0 : 0x1A8) : (version == 1 ? 0xE4 : 0xE8));
            if (!guest_read(g, at, wide ? 8 : 4, b, &ignored)) ptr = wide ? get64(b) : get32(b);
        }
    }
    if (ptr) {
        size_t i;
        for (i = 0; i < sizeof b; i++) /* a page edge may cut the read: byte by byte past it */
            if (guest_read(g, ptr + i, 1, b + i, &ignored) || !b[i]) break;
        memcpy(name, b, i);
        name[i] = 0;
        found = i > 0;
    }
    free(ignored.p);
    if (!found) {
        memcpy(name, off < 0 ? (const unsigned char*)"" : k + off, off < 0 ? 1 : 32);
        name[32] = 0;
    }
    mcp_put(t, "'");
    mcp_put(t, name);
    mcp_put(t, "'");
}
/* where a guest thread is: in an SVC (its registers in the KThread exact), or running game code */
static void put_where(Text* t, const unsigned char* k) {
    if (k32(k, K_STATE, 0) == 2)
        mcp_put(t, "exited");
    else if (k32(k, K_IN_SVC, 0))
        mcp_put(t, "in an SVC (its registers are exact)");
    else
        mcp_put(t, "running game code (the recompiled code keeps registers on the host between calls into the "
                   "runtime: these may be stale)");
}

/* where a host thread is: its innermost frame with a source file and that frame's caller (the innermost is mostly a
   system call in a library without symbols), else the innermost. DONE, or LOST with why in err */
static int put_host(Text* t, const char* id, yyjson_mut_val* top, Text* err) {
    char command[64];
    yyjson_mut_val *f, *found = NULL;
    size_t i, n;
    Reply r;
    int rc;
    snprintf(command, sizeof command, "-stack-list-frames --thread %s 0 7", id);
    if ((rc = exec(command, now_ms() + COMMAND_MS, &r, err)) != DONE) {
        if (rc == LOST) return LOST;
        clear(err);
        put_frame(t, top);
        return DONE;
    }
    yyjson_mut_arr_foreach(yyjson_mut_obj_get(r.result, "stack"), i, n, f) {
        if (!field(f, "file")) continue;
        if (found) {
            mcp_put(t, " < ");
            put_frame(t, f);
            break;
        }
        found = f;
        put_frame(t, f);
    }
    if (!found) put_frame(t, top);
    reply_free(&r);
    return DONE;
}

char* mcp_gdb_threads(McpNamer name, const void* ctx, Text* err) {
    unsigned char k[THREAD_BYTES];
    Text t = {0};
    Reply r;
    Guest g, *gp = &g;
    yyjson_mut_val* th;
    size_t i, n;
    int wide, guests = 0, hosts = 0, tls_failed = 0;
    if (need_stopped(err, 1)) return NULL;
    if ((wide = is_wide(err)) < 0) return NULL;
    if (guest_map(&g, err) == LOST) return NULL;
    if (err->n) { /* no guest memory yet: no SDK names */
        clear(err);
        gp = NULL;
    }
    if (exec("-thread-info", now_ms() + COMMAND_MS, &r, err)) return NULL;
    mcp_put_n(&t, "", 0);
    yyjson_mut_arr_foreach(yyjson_mut_obj_get(r.result, "threads"), i, n, th) {
        const char *id = field(th, "id"), *target = field(th, "target-id"), *tname = field(th, "name");
        SnxU64 at = 0;
        int got;
        if (!id) continue;
        got = tls_failed ? 0 : kthread(strtol(id, NULL, 10), &at, k, err);
        if (got == LOST) goto lost;
        if (got == REFUSED) {
            /* a thread's TLS that GDB can't read (no libthread_db: a musl build): said once, the rest unread */
            mcp_put(&t, "(GDB can't read the threads' tls_thread: ");
            mcp_put(&t, err->p);
            mcp_put(&t, ". Every thread is listed as the runtime's.)\n");
            clear(err);
            tls_failed = 1;
            got = 0;
        }
        mcp_put(&t, id);
        mcp_put(&t, " ");
        mcp_put(&t, target ? target : "?");
        if (tname) {
            mcp_put(&t, " '");
            mcp_put(&t, tname);
            mcp_put(&t, "'");
        }
        if (got) {
            SnxU64 pc = wide ? k64(k, K_PC, 0) : k32(k, K_R, 15), lr = wide ? k64(k, K_X, 30) : k32(k, K_R, 14);
            guests++;
            mcp_put(&t, ": guest thread ");
            mcp_put_num(&t, (unsigned long)k64(k, K_ID, 0));
            mcp_put(&t, " ");
            put_kname(&t, gp, k, wide);
            mcp_put(&t, ", ");
            put_where(&t, k);
            mcp_put(&t, "\n    pc=");
            put_guest(&t, pc, 0, name, ctx);
            mcp_put(&t, " lr=");
            put_guest(&t, lr, wide ? 0 : 1, name, ctx);
            mcp_put(&t, "\n    host at ");
        } else {
            hosts++;
            mcp_put(&t, ": the runtime's, at ");
        }
        if (put_host(&t, id, yyjson_mut_obj_get(th, "frame"), err) == LOST) goto lost;
        mcp_put(&t, "\n");
    }
    reply_free(&r);
    mcp_put_num(&t, (unsigned long)guests);
    mcp_put(&t, " guest threads, ");
    mcp_put_num(&t, (unsigned long)hosts);
    mcp_put(&t, " of the runtime's. gdb_guest with a thread's GDB number (the first on its line) shows its registers "
                "and guest backtrace.\n");
    return finish(&t, name, ctx, 0);
lost:
    reply_free(&r);
    free(t.p);
    return NULL;
}

char* mcp_gdb_guest(long thread, int frames, McpNamer name, const void* ctx, Text* err) {
    unsigned char k[THREAD_BYTES], record[16];
    Text t = {0};
    Guest g;
    SnxU64 at, fp, pc, lr;
    int wide, got, i, n = 0, rc;
    SnxU64 thumb = 0;
    if (need_stopped(err, 1)) return NULL;
    if ((wide = is_wide(err)) < 0) return NULL;
    if (thread <= 0) {
        SnxU64 current;
        if (eval(0, "$_thread", &current, err)) return NULL;
        thread = (long)current;
    }
    got = kthread(thread, &at, k, err);
    if (got < 0) return NULL;
    if (!got) {
        mcp_put(err, "not a guest thread: one of the runtime's own (gdb_threads lists which are guest threads)");
        return NULL;
    }
    if (guest_map(&g, err)) return NULL;
    mcp_put(&t, "guest thread ");
    mcp_put_num(&t, (unsigned long)k64(k, K_ID, 0));
    mcp_put(&t, " ");
    put_kname(&t, &g, k, wide);
    mcp_put(&t, " (GDB thread ");
    mcp_put_num(&t, (unsigned long)thread);
    mcp_put(&t, "), ");
    put_where(&t, k);
    mcp_put(&t, "\n");
    for (i = 0; i < (wide ? 31 : 16); i++) {
        char reg[48];
        if (wide)
            snprintf(reg, sizeof reg, "x%-2d=%016llx", i, (unsigned long long)k64(k, K_X, (size_t)i));
        else
            snprintf(reg, sizeof reg, "r%-2d=%08x", i, (unsigned)k32(k, K_R, (size_t)i));
        mcp_put(&t, reg);
        mcp_put(&t, i % 4 == 3 ? "\n" : " ");
    }
    if (wide) {
        char reg[96];
        snprintf(reg, sizeof reg, "\nsp=%llx nzcv=%c%c%c%c tpidrro=%llx\n", (unsigned long long)k64(k, K_X, 31),
                 k32(k, K_N, 0) ? 'N' : 'n', k32(k, K_N, 1) ? 'Z' : 'z', k32(k, K_N, 2) ? 'C' : 'c',
                 k32(k, K_N, 3) ? 'V' : 'v', (unsigned long long)k64(k, K_TPIDRRO, 0));
        mcp_put(&t, reg);
        pc = k64(k, K_PC, 0);
        lr = k64(k, K_X, 30);
        fp = k64(k, K_X, 29);
    } else {
        char reg[48];
        SnxU32 cpsr = k32(k, K_CPSR, 0);
        snprintf(reg, sizeof reg, "cpsr=%08x (%s)\n", (unsigned)cpsr, cpsr & 0x20 ? "Thumb" : "A32");
        mcp_put(&t, reg);
        pc = k32(k, K_R, 15);
        lr = k32(k, K_R, 14);
        fp = k32(k, K_R, cpsr & 0x20 ? 7 : 11);
        thumb = 1; /* a return address's bit 0 is the mode it returns to */
    }
    /* the frame records fp points at, the caller's fp and then the return address, as hle/kernel/debug.c walks them */
    mcp_put(&t, "pc ");
    put_guest(&t, pc, 0, name, ctx);
    mcp_put(&t, "\nlr ");
    put_guest(&t, lr, thumb, name, ctx);
    mcp_put(&t, "\nframes from ");
    mcp_put(&t, wide ? "x29" : "the frame pointer");
    mcp_put(&t, ":\n");
    if (frames <= 0 || frames > MAX_FRAMES) frames = MAX_FRAMES;
    while (n < frames && fp && !(fp & (wide ? 7 : 3))) {
        SnxU64 next;
        if ((rc = guest_read(&g, fp, wide ? 16 : 8, record, err)) != DONE) {
            if (rc == LOST) {
                free(t.p);
                return NULL;
            }
            mcp_put(&t, "  (");
            mcp_put(&t, err->p);
            mcp_put(&t, ")\n");
            clear(err);
            break;
        }
        next = wide ? get64(record) : get32(record);
        lr = wide ? get64(record + 8) : get32(record + 4);
        if (!lr) break;
        mcp_put(&t, "  #");
        mcp_put_num(&t, (unsigned long)++n);
        mcp_put(&t, " ");
        put_guest(&t, lr, thumb, name, ctx);
        mcp_put(&t, "\n");
        if (next <= fp) break; /* frames go up the stack */
        fp = next;
    }
    if (!n) mcp_put(&t, "  (none)\n");
    return finish(&t, NULL, NULL, 0);
}

char* mcp_gdb_memory(SnxU64 address, long size, Text* err) {
    unsigned char bytes[MAX_MEMORY];
    Text t = {0};
    Guest g;
    long i, j;
    if (need_stopped(err, 0)) return NULL;
    if (size <= 0) size = 256;
    if (size > MAX_MEMORY) size = MAX_MEMORY;
    if (guest_map(&g, err)) return NULL;
    if (guest_read(&g, address, (size_t)size, bytes, err)) return NULL;
    for (i = 0; i < size; i += 16) {
        char line[96];
        int n = snprintf(line, sizeof line, "%010llx ", (unsigned long long)(address + (SnxU64)i));
        for (j = i; j < i + 16; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, j < size ? " %02x" : "   ", bytes[j < size ? j : 0]);
        n += snprintf(line + n, sizeof line - (size_t)n, "  ");
        for (j = i; j < i + 16 && j < size; j++) line[n++] = bytes[j] >= 0x20 && bytes[j] < 0x7f ? (char)bytes[j] : '.';
        line[n++] = '\n';
        line[n] = 0;
        mcp_put(&t, line);
    }
    return finish(&t, NULL, NULL, 1);
}

char* mcp_gdb_detach(Text* err) {
    Text t = {0};
    if (!g_gdb.pid) {
        mcp_put(err, no_gdb);
        return NULL;
    }
    mcp_gdb_close();
    mcp_put(&t, "Detached: the game runs on without the debugger. The stub takes one connection per run, so attaching "
                "again needs a new run.\n");
    return t.p;
}

void mcp_gdb_close(void) {
    Text ignored = {0}, line = {0};
    long long deadline;
    if (!g_gdb.pid) return;
    drain();
    if (g_gdb.pid && !g_gdb.exited) {
        if (g_gdb.running) interrupt(&ignored, &ignored);
        if (g_gdb.pid) run("-target-detach", &ignored);
    }
    /* GDB's own end, which would detach too, before it is killed */
    if (g_gdb.pid && !write_all(g_gdb.to, "-gdb-exit\n", 10)) {
        deadline = now_ms() + EXIT_MS;
        while (next_line(&line, deadline) > 0) {}
    }
    free(line.p);
    free(ignored.p);
    close_gdb();
}
#endif
