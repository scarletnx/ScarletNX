/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* g_hle is flat: a save state writes each of its members as the bytes they are (hle/state.c), so none may hold a
   pointer, a host lock or anything else of the host's. Clang's __builtin_dump_struct walks each member's fields, and
   each struct an array holds (a pool's slots) is walked by its own type: every field whose type is a pointer or a host
   type is reported, as is an array of a struct no walk covered, unless the list of known exceptions below names it.
   A field a load sets again from the host (state.c's fixups) is one of those exceptions; the others are the work
   left. --list prints every member's fields, and --report the exceptions that are no longer needed */
#include "nx/hle/hle.h"
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
    WALK("KThread", h.thread.threads[0]);
    WALK("KEvent", h.kernel.events[0]);
    WALK("HEntry", h.kernel.htab[0]);
    WALK("SvcRec", h.kernel.svc_ring[0]);
    WALK("KWEvent", h.svc.wevents[0]);
    WALK("KShmem", h.svcmem.shmems[0]);
    WALK("KTmem", h.svcmem.tmems[0]);
    WALK("KCodeMem", h.svcmem.codemems[0]);
    WALK("KCodeView", h.svcmem.views[0]);
    WALK("debug.sleeps[]", h.debug.sleeps[0]);
    WALK("SvcObj", h.ipc.svcobjs[0]);
    WALK("KSession", h.ipc.sessions[0]);
    WALK("Domain", h.ipc.domains[0]);
    WALK("ipc.seen[]", h.ipc.seen[0]);
    WALK("SvcReg", h.ipc.reg[0]);
    WALK("IpcRec", h.ipc.ring[0]);
    WALK("AccUser", h.acc.users[0]);
    WALK("StorageData", h.am.storages[0]);
    WALK("AmAccessor", h.am.accessors[0]);
    WALK("AmVolumes", h.am.volumes[0]);
    WALK("AmLock", h.am.lock_objs[0]);
    WALK("LibApplet", h.am.applets[0]);
    WALK("AudOut", h.audio.outs_pool[0]);
    WALK("AudRen", h.audio.rens[0]);
    WALK("AudDevice", h.audio.devices[0]);
    WALK("HwOpus", h.audio.opus[0]);
    WALK("ARRen", h.audren.rens[0]);
    WALK("ARCompressor", h.audren_dsp.compressors[0]);
    WALK("PadMotion", h.hid.pad1.motion[0]);
    WALK("PadState", h.hid.pad[0]);
    WALK("HidMotionState", h.hid.motion[0][0]);
    WALK("HidVibration", h.hid.vibration[0][0]);
    WALK("RoState", h.ldr.states[0]);
    WALK("RoNro", h.ldr.states[0].nro[0]);
    WALK("RoNrr", h.ldr.states[0].nrr[0]);
    WALK("MmRequest", h.misc.mms[0].req[0]);
    WALK("NvEvent", h.nvdrv.ctrl_evt[0].slot[0]);
    WALK("ViSlot", h.vi.layer[0].slot[0]);
    WALK("BsdFd", h.bsd.bsds[0].fd[0]);
    WALK("Mm", h.misc.mms[0]);
    WALK("NfcUser", h.misc.nfc_users[0]);
    WALK("NvMapH", h.nvdrv.nvh[0]);
    WALK("NvEventTable", h.nvdrv.ctrl_evt[0]);
    WALK("NvFile", h.nvdrv.nvf[0]);
    WALK("NvSeen", h.nvdrv.seen[0]);
    WALK("TimeObj", h.time.objs[0]);
    WALK("ViLayer", h.vi.layer[0]);
    WALK("Bsd", h.bsd.bsds[0]);
    WALK("Ldn", h.ldn.ldn[0]);
    WALK("NifmRequest", h.nifm.reqs[0]);
    WALK("SslContext", h.ssl.ctxs[0]);
    WALK("SslConn", h.ssl.conns[0]);
    WALK("ROpen", h.fs.openf[0]);
    WALK("FsData", h.fs.fss[0]);
    WALK("FileData", h.fs.files[0]);
    WALK("DirData", h.fs.dirs[0]);
    /* then g_hle's members */
    WALK("kernel", h.kernel);
    WALK("thread", h.thread);
    WALK("sync", h.sync);
    WALK("debug", h.debug);
    WALK("svc", h.svc);
    WALK("svcmem", h.svcmem);
    WALK("ipc", h.ipc);
    WALK("acc", h.acc);
    WALK("am", h.am);
    WALK("audio", h.audio);
    WALK("audren", h.audren);
    WALK("audren_dsp", h.audren_dsp);
    WALK("hid", h.hid);
    WALK("ldr", h.ldr);
    WALK("misc", h.misc);
    WALK("nvdrv", h.nvdrv);
    WALK("pl", h.pl);
    WALK("set", h.set);
    WALK("sm", h.sm);
    WALK("time", h.time);
    WALK("vi", h.vi);
    WALK("bsd", h.bsd);
    WALK("ldn", h.ldn);
    WALK("nifm", h.nifm);
    WALK("ssl", h.ssl);
    WALK("fs", h.fs);
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
