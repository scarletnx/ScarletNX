/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* g_hle is flat: a save state writes each of its members as the bytes they are (hle/state.c), so none may hold a
   pointer, a host lock or anything else of the host's. Clang's __builtin_dump_struct walks each member's fields, and
   each struct an array holds (a pool's slots) is walked by its own type: every field whose type is a pointer or a host
   type is reported, as is an array of a struct no walk covered, unless the list of known exceptions below names it.
   A field a load sets again from the host (state.c's fixups) is one of those exceptions; the others are the work
   left. --list prints every member's fields, and --report the exceptions that are no longer needed */
#include "nx/hle/hle.h"
#include "nx/hle/hle_walk.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_text[1 << 22];
static size_t g_len;
static int out(void* ctx, const char* fmt, ...) {
    va_list ap;
    (void)ctx;
    va_start(ap, fmt);
    if (g_len < sizeof g_text) g_len += (size_t)vsnprintf(g_text + g_len, sizeof g_text - g_len, fmt, ap);
    va_end(ap);
    return 0;
}

/* the types walked, by name: an array of one of them is covered */
static const char* g_walked[256];
static int g_nwalked;

/* each walk's fields, as path type pairs */
typedef struct {
    char path[256], type[128];
} Field;
static Field g_fields[1 << 16];
static int g_nfields;

/* the text of one walk into fields under root: lines "<indent><type> <name> = ..." and "<indent>}" */
static void parse(const char* root) {
    char* names[32];
    char* line = g_text;
    int depth = -1;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        char *eq, *sp, *t;
        int indent = 0;
        if (nl) *nl = 0;
        while (line[indent] == ' ') indent++;
        t = line + indent;
        if (!strcmp(t, "}")) {
            depth--;
        } else if ((eq = strstr(t, " =")) != NULL) {
            int level = indent / 2 - 1, i;
            Field* f = &g_fields[g_nfields++];
            *eq = 0;
            sp = strrchr(t, ' ');
            *sp = 0;
            if (level < 0) level = 0;
            names[level] = sp + 1;
            snprintf(f->path, sizeof f->path, "%s", root);
            for (i = 0; i <= level; i++) {
                size_t n = strlen(f->path);
                snprintf(f->path + n, sizeof f->path - n, ".%s", names[i]);
            }
            snprintf(f->type, sizeof f->type, "%s", t);
            depth = level;
        }
        line = nl ? nl + 1 : NULL;
    }
    (void)depth;
}
#define WALK(root, obj)                                                                                                \
    do {                                                                                                               \
        g_len = 0;                                                                                                     \
        memset(g_text, 0, sizeof g_text);                                                                              \
        __builtin_dump_struct(&(obj), out, NULL);                                                                      \
        g_walked[g_nwalked++] = root;                                                                                  \
        parse(root);                                                                                                   \
    } while (0)

static Hle h;

/* a walk's name as the paths have it: an array of an unnamed struct, walked as its first element (debug_sleeps_slot),
   is the array's path with "[]" (debug.sleeps[]), kept in g_names for the test's whole run */
static char g_names[8][64];
static int g_nnames;
static const char* walk_name(const char* name) {
    size_t n = strlen(name);
    char* s;
    if (n < 5 || strcmp(name + n - 5, "_slot")) return name;
    if (g_nnames == (int)(sizeof g_names / sizeof *g_names) || n > sizeof *g_names) {
        fprintf(stderr, "hle_flat: %s is walk %d of an unnamed struct's array, past the %d names of %d bytes kept\n",
                name, g_nnames + 1, (int)(sizeof g_names / sizeof *g_names), (int)sizeof *g_names);
        exit(1);
    }
    s = g_names[g_nnames++];
    memcpy(s, name, n - 5);
    s[n - 5] = 0;
    s[strcspn(s, "_")] = '.';
    strcat(s, "[]");
    return s;
}

/* the fields known not to be flat yet, or set again by a load: a path, or a prefix ending in '*' */
static const char* const KNOWN[] = {
    "KThread.cpu.mem",
    "KThread.cpu.disp",
    "KThread.cpu.rt",
    "KThread.cpu.thread",
    "KThread.arm.mem",
    "KThread.arm.rt",
    "KThread.arm.io",
    "KShmem.host",
    "KCodeView.snap",
    "SvcObj.name",
    "SvcObj.dispatch",
    "SvcObj.destroy",
    "SvcObj.data",
    "KSession.obj",
    "KSession.domain",
    "Domain.obj",
    "SvcReg.name",
    "IpcRec.name",
    "IpcRec.what",
    "StorageData.d",
    "AmAccessor.storage",
    "LibApplet.in",
    "LibApplet.out",
    "AudOut.next",
    "AudOut.output",
    "HwOpus.lk*",
    "HwOpus.dec",
    "HwOpus.pkt",
    "HwOpus.frame",
    "HwOpus.queue",
    "HwOpus.taps",
    "HwOpus.hist",
    "ARRen.o",
    "ARRen.s",
    "ARRen.out",
    "RoNrr.hashes",
    "BsdFd.s",
    "BsdFd.bcast",
    "NvFile.h1x",
    "Bsd.lk*",
    "Ldn.lk*",
    "Ldn.lan",
    "SslContext.lk*",
    "SslContext.ctx",
    "SslContext.store",
    "SslContext.client_key",
    "SslContext.client_cert",
    "SslContext.client_chain",
    "SslConn.lk*",
    "SslConn.ssl",
    "SslConn.s",
    "SslConn.context",
    "ROpen.node",
    "ROpen.f",
    "FsData.root",
    "FileData.f",
    "FileData.node",
    "DirData.node",
    "debug.syms",
    "am.user_channel",
    "am.nacp",
    "audio.outs",
    "nvdrv.gmmu",
    "nvdrv.gmaps",
    "nvdrv.gruns",
    "nvdrv.alias.bound",
    "nvdrv.alias.first",
    "nvdrv.alias.cover",
    "nvdrv.alias.seen",
    "nvdrv.vafree",
    "bsd.room",
    "bsd.client",
    "ssl.roots",
    "ssl.bio",
    "fs.tree.base",
    "fs.tree.root",
    "fs.tree.lock*",
    "fs.rfiles",
    "fs.rmeta",
    "fs.rraw",
    "fs.rimage",
    "fs.dump.hac",
    "fs.dump.keys",
    "fs.dump.title_keys",
    "fs.dump.opened",
    "fs.dfs",
    "fs.dmap.data",
    "fs.hfs",
    "fs.amap.data",
    "fs.aocs",
    "fs.dlcs",
    "fs.mexefs_names",
    "fs.mexefs_paths",
    "fs.patches",
    NULL,
};
static int known_used[sizeof KNOWN / sizeof *KNOWN];
static int known(const char* path) {
    int i;
    for (i = 0; KNOWN[i]; i++) {
        size_t n = strlen(KNOWN[i]);
        if (KNOWN[i][n - 1] == '*' ? !strncmp(path, KNOWN[i], n - 1) : !strcmp(path, KNOWN[i])) {
            known_used[i] = 1;
            return 1;
        }
    }
    return 0;
}

/* the C types that are no struct: an array of them needs no walk */
static int scalar(const char* t) {
    static const char* const S[] = {
        "u8",       "u16",      "u32",    "u64",     "i8",      "i16",     "i32",  "i64",  "uint8_t",  "uint16_t",
        "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "char", "int",  "unsigned", "short",
        "long",     "float",    "double", "size_t",  "_Bool",   "signed",  "bool", "V128", NULL};
    int i;
    for (i = 0; S[i]; i++)
        if (!strncmp(t, S[i], strlen(S[i])) && (t[strlen(S[i])] == '[' || t[strlen(S[i])] == ' ')) return 1;
    return 0;
}
static int walked(const char* t) {
    size_t n = strcspn(t, "[");
    int i;
    for (i = 0; i < g_nwalked; i++)
        if (strlen(g_walked[i]) == n && !strncmp(g_walked[i], t, n)) return 1;
    return 0;
}
/* why a field's type keeps its struct from being flat, or NULL */
/* an array of an unnamed struct is covered by a walk of its own path with "[]" after it */
static int walked_path(const char* path) {
    int i;
    for (i = 0; i < g_nwalked; i++) {
        size_t n = strlen(g_walked[i]);
        if (n > 2 && !strcmp(g_walked[i] + n - 2, "[]") && strlen(path) == n - 2 && !strncmp(path, g_walked[i], n - 2))
            return 1;
    }
    return 0;
}
static const char* problem(const char* path, const char* t) {
    if (strchr(t, '*')) return "a pointer";
    if (!strncmp(t, "PMutex", 6) || !strncmp(t, "PCond", 5)) return "a host lock";
    if (strchr(t, '[') && !scalar(t) && !walked(t) && !walked_path(path)) return "an array of a struct no walk covers";
    return NULL;
}

int main(int argc, char** argv) {
    int list = argc > 1 && !strcmp(argv[1], "--list"), report = argc > 1 && !strcmp(argv[1], "--report");
    int bad = 0, i;
    /* the pools' slots and the structs arrays hold, walked by their types first */
#define FLAT_WALK(name, obj) WALK(walk_name(#name), obj);
    HLE_WALKS(FLAT_WALK, h)
#undef FLAT_WALK
    for (i = 0; i < g_nfields; i++) {
        const char* why = problem(g_fields[i].path, g_fields[i].type);
        if (list) printf("%s %s%s%s\n", g_fields[i].path, g_fields[i].type, why ? "  <- " : "", why ? why : "");
        if (!why || known(g_fields[i].path)) continue;
        if (!list) printf("%s (%s) is %s\n", g_fields[i].path, g_fields[i].type, why);
        bad++;
    }
    if (report)
        for (i = 0; KNOWN[i]; i++)
            if (!known_used[i]) printf("no longer needed: %s\n", KNOWN[i]);
    printf("%d fields walked, %d not flat\n", g_nfields, bad);
    return bad != 0;
}
