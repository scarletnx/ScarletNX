/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The file of shaders and pipelines kept across runs (gpu/vk/pipeline_file.c), without a GPU: modules are numbers.
   Shader and pipeline records come back as written, a pipeline's shaders as the modules the next run made of them;
   a pipeline record waits for its shaders' records and is dropped when one of them couldn't be made again; records
   appended after a reading refer to the shaders read; a record cut short by a crash or changed on disk ends the
   reading, and the file goes on from the last good record; a file of another key starts over; a full file takes no
   more; a PF_SHIPREF counts as a shader record that pipelines name. A file read as its records are used
   (pf_open_mapped) hands each over with where it starts, checks one when it is read (a payload changed on disk doesn't
   check out), drops a record cut short and maps the file as rewritten, leaves one it can't map as it is and never
   appends to it, and shows what was appended once sealed */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/pipeline_file.c"

Settings g_settings;
int plat_cpus(int* ids, int max) {
    int n = max < 8 ? max : 8;
    for (int i = 0; i < n; i++) ids[i] = i;
    return n;
}

#define PATH "pipeline_file_test.vkshaders"
/* the test's file and the temporary one a rewrite makes, for a failing check to leave neither */
static void cleanup(void) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.%u.tmp", PATH, pf_pid());
    remove(PATH);
    remove(tmp);
}
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            cleanup();                                                                                                 \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the last warning */
static char warned[1024];
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    if (lvl) return;
    va_start(ap, fmt);
    vsnprintf(warned, sizeof warned, fmt, ap);
    va_end(ap);
}

/* the platform's file functions the file uses, over a copy in memory */
static int fail_map; /* the next maps fail, as one of a file the system won't map */
int plat_file_map(PFileMap* m, const char* path) {
    FILE* f = fail_map ? NULL : fopen(path, "rb");
    long n = -1;
    u8* d;
    memset(m, 0, sizeof *m);
    if (!f) return -1;
    if (!fseek(f, 0, SEEK_END)) n = ftell(f);
    d = n > 0 && !fseek(f, 0, SEEK_SET) ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
        m->data = d;
        m->size = (u64)n;
        m->heap = 1;
    } else
        free(d);
    fclose(f);
    return n < 0 ? -1 : 0;
}
void plat_file_unmap(PFileMap* m) {
    free((void*)m->data);
    memset(m, 0, sizeof *m);
}
/* rename doesn't replace a file on Windows. fail_replace: the next replaces fail, as on a full disk */
static int fail_replace;
int plat_replace_file(const char* tmp, const char* dst) {
    if (fail_replace) return -1;
    remove(dst);
    return rename(tmp, dst) ? -1 : 0;
}
#define KEYB 96 /* a pipeline key's bytes */

/* what a reading handed back */
static struct {
    int nsh, npipe;
    PfShader sh[8];
    u32 cbr[8][8], spv[8][16];
    u64 mods[8][5];
    u8 key[8][KEYB];
    u64 next_module; /* the module the next shader is made into; 0 fails the first one when fail_first */
    int fail_first;
} got;

static u64 load_shader(void* ctx, const PfShader* s, const u32* cbr, const u32* spv) {
    (void)ctx;
    CHECK(got.nsh < 8 && s->ncbr <= 2 && s->nspv <= 16);
    got.sh[got.nsh] = *s;
    memcpy(got.cbr[got.nsh], cbr, s->ncbr * 16);
    memcpy(got.spv[got.nsh], spv, s->nspv * 4);
    got.nsh++;
    if (got.fail_first && got.nsh == 1) return 0;
    return got.next_module++;
}
static void load_pipe(void* ctx, const u64 modules[5], const void* key, u32 keylen) {
    (void)ctx;
    CHECK(got.npipe < 8 && keylen == KEYB);
    memcpy(got.mods[got.npipe], modules, sizeof got.mods[0]);
    memcpy(got.key[got.npipe], key, KEYB);
    got.npipe++;
}

static struct {
    int n;
    u64 at, check;
} shipref_got;
static u64 load_shipref(void* ctx, u64 at, u64 check) {
    (void)ctx;
    shipref_got.n++;
    shipref_got.at = at;
    shipref_got.check = check;
    return got.next_module++;
}
static void make_shader(int n, PfShader* s, u32* cbr, u32* spv);
static long file_size(void);
/* the bytes of open_file's key that are its guest part (pf_open's guest) */
static size_t guest_bytes;
/* a rebuild's sources handed back: each one's shader number (its code's first byte) and whether a transform feedback
   layout came with it; each recorded into the new file as shader n again, made into module source_module + i */
static PFile* rebuilding;
static struct {
    int n, shader[8], xfb[8], rebuild;
} source_got;
static u64 source_module;
static void add_sourced(PFile* pf, int n, u64 module, int with_xfb);
static u64 load_source(void* ctx, const PfSource* s, const u32* cbr, const u8* code, const ShXfb* xfb) {
    PfShader sh;
    u32 want[8], spv[16];
    (void)ctx;
    make_shader(code[0], &sh, want, spv);
    CHECK(source_got.n < 8 && s->ncode == 64 && s->hash == sh.hash && s->stage == sh.stage && s->ncbr == sh.ncbr &&
          !memcmp(cbr, want, sh.ncbr * 16) && !memcmp(s->mask, sh.mask, sizeof s->mask));
    CHECK(!xfb == !(s->flags & PF_SRC_XFB) && (!xfb || xfb->stride[3] == 4321));
    source_got.shader[source_got.n] = code[0];
    source_got.xfb[source_got.n] = xfb != NULL;
    source_got.n++;
    add_sourced(rebuilding, code[0], source_module + (u64)source_got.n - 1, xfb != NULL);
    return source_module + (u64)source_got.n - 1;
}
static void open_file(PFile* pf, const char* key, u64 first_module, int fail_first, u32 loaded[2]);
static void close_file(PFile* pf);
static void add_shader(PFile* pf, int n, u64 module);
static void add_pipe(PFile* pf, int n, u64 vs, u64 tcs, u64 tes, u64 gs, u64 fs);
static void check_pipe(int i, int n, u64 vs, u64 tcs, u64 tes, u64 gs, u64 fs);
/* the file's byte at at changed, or its last cut bytes cut off, by writing it again */
static void rewrite(long at, long cut) {
    long n = file_size();
    u8* d = n > 0 ? malloc((size_t)n) : NULL;
    FILE* f = d ? fopen(PATH, "rb") : NULL;
    CHECK(f && fread(d, 1, (size_t)n, f) == (size_t)n && at < n && cut < n);
    fclose(f);
    if (at >= 0) d[at] ^= 0x5A;
    f = fopen(PATH, "wb");
    CHECK(f && fwrite(d, 1, (size_t)(n - cut), f) == (size_t)(n - cut));
    fclose(f);
    free(d);
}
/* what pf_open_mapped handed over */
static struct {
    int n;
    u32 type[8];
    u64 at[8];
} each_got;
static void each_rec(void* ctx, u32 type, const u8* payload, u32 len, u64 at) {
    (void)ctx;
    (void)payload;
    (void)len;
    if (each_got.n < 8) {
        each_got.type[each_got.n] = type;
        each_got.at[each_got.n] = at;
    }
    each_got.n++;
}
static void mapped(void) {
    PFile pf = {0};
    PfShader s;
    u32 cbr[8], spv[16], len = 0;
    u64 at[2], check = 0;
    const u8* p;
    remove(PATH);
    pm_init(&pf.lock);
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "mapped key", each_rec, NULL);
    CHECK(!pf.f && !pf.kept && each_got.n == 0 && pf.view.data && pf.size == pf.view.size); /* its header, mapped */
    pf_unseal(&pf);
    CHECK(pf.f && !pf.view.data);
    make_shader(1, &s, cbr, spv);
    at[0] = pf_shader(&pf, 0, &s, cbr, spv, NULL, NULL, NULL);
    pf_scan(&pf, 1, 77);
    make_shader(2, &s, cbr, spv);
    at[1] = pf_shader(&pf, 0, &s, cbr, spv, NULL, NULL, NULL);
    CHECK(at[0] && at[1] > at[0] && pf.nmap == 0); /* no module: nothing for pipelines to name */
    CHECK(!pf_payload(&pf, at[1], &len, &check));  /* unmapped while appended to */
    pf_seal(&pf);
    CHECK(!pf.f);
    p = pf_payload(&pf, at[1], &len, &check);
    CHECK(p && len >= sizeof s && !memcmp(p, &s, sizeof s) && check == pf_get64(p - 8));
    CHECK(!pf_payload(&pf, at[1] + 8, &len, &check)); /* not a record's start */
    plat_file_unmap(&pf.view);
    /* read again: each record with where it starts, in order */
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "mapped key", each_rec, NULL);
    CHECK(!pf.f && each_got.n == 3 && each_got.type[0] == PF_SHADER && each_got.at[0] == at[0] &&
          each_got.type[1] == PF_SCAN && each_got.type[2] == PF_SHADER && each_got.at[2] == at[1]);
    CHECK(pf_payload(&pf, at[0], &len, &check) != NULL);
    plat_file_unmap(&pf.view);
    /* a payload changed on disk: handed over unchecked, but it doesn't check out when read */
    rewrite((long)at[0] + PF_REC + 4, 0);
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "mapped key", each_rec, NULL);
    CHECK(each_got.n == 3 && !pf_payload(&pf, at[0], &len, &check) && pf_payload(&pf, at[1], &len, &check));
    plat_file_unmap(&pf.view);
    rewrite((long)at[0] + PF_REC + 4, 0);
    /* the last record cut short: the file goes on without it, mapped as rewritten */
    rewrite(-1, 8);
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "mapped key", each_rec, NULL);
    CHECK(!pf.f && each_got.n == 2 && pf.size == at[1] && file_size() == (long)at[1] && pf.view.size == at[1]);
    plat_file_unmap(&pf.view);
    /* cut short again, and the rewrite fails: left as it is, said, and never appended to */
    rewrite(-1, 8);
    fail_replace = 1;
    warned[0] = 0;
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "mapped key", each_rec, NULL);
    CHECK(pf.kept && strstr(warned, "cannot write") && file_size() == (long)at[1] - 8);
    {
        char name[64];
        FILE* tmp;
        snprintf(name, sizeof name, "%s.%u.tmp", PATH, pf_pid());
        tmp = fopen(name, "rb");
        CHECK(!tmp); /* removed */
    }
    pf_unseal(&pf);
    CHECK(!pf.f);
    pf_seal(&pf);
    plat_file_unmap(&pf.view);
    fail_replace = 0;
    pf.kept = 0;
    /* there, but it can't be mapped: left as it is, and never appended to */
    fail_map = 1;
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "mapped key", each_rec, NULL);
    CHECK(pf.kept && each_got.n == 0 && !pf.view.data && file_size() == (long)at[1] - 8);
    pf_unseal(&pf);
    CHECK(!pf.f);
    pf_seal(&pf);
    fail_map = 0;
    pf.kept = 0;
    /* another key's: started over */
    memset(&each_got, 0, sizeof each_got);
    pf_open_mapped(&pf, PATH, "another key", each_rec, NULL);
    CHECK(!pf.f && each_got.n == 0 && file_size() == (long)pf.size);
    plat_file_unmap(&pf.view);
    pm_destroy(&pf.lock);
    remove(PATH);
}
/* a PF_SHIPREF: read back with its place and check, and a shader record that a pipeline after it names */
static void shiprefs(void) {
    PFile pf;
    u32 loaded[2] = {0, 0};
    remove(PATH);
    open_file(&pf, "key one", 0x10, 0, loaded);
    add_shader(&pf, 1, 0x7001);
    pf_shipref(&pf, 0x7002, 0x1230, 0xABCDEF);
    CHECK(pf.nsh == 2 && pf.nmap == 2);
    add_pipe(&pf, 1, 0x7002, 0, 0, 0, 0x7001);
    close_file(&pf);
    memset(&shipref_got, 0, sizeof shipref_got);
    open_file(&pf, "key one", 0x20, 0, loaded);
    CHECK(loaded[0] == 2 && loaded[1] == 1 && got.nsh == 1 && pf.nsh == 2);
    CHECK(shipref_got.n == 1 && shipref_got.at == 0x1230 && shipref_got.check == 0xABCDEF);
    check_pipe(0, 1, 0x21, 0, 0, 0, 0x20);
    close_file(&pf);
    remove(PATH);
}

static void open_file(PFile* pf, const char* key, u64 first_module, int fail_first, u32 loaded[2]) {
    PfLoad load;
    load.shader = load_shader;
    load.pipe = load_pipe;
    load.shipref = load_shipref;
    load.source = load_source;
    load.ctx = NULL;
    load.rebuild = 0;
    memset(&got, 0, sizeof got);
    got.next_module = first_module;
    got.fail_first = fail_first;
    memset(pf, 0, sizeof *pf);
    pm_init(&pf->lock);
    pf_open(pf, PATH, key, guest_bytes, &load, loaded);
}
static void close_file(PFile* pf) {
    if (pf->f) fclose(pf->f);
    free(pf->map);
    pm_destroy(&pf->lock);
    memset(pf, 0, sizeof *pf);
}
static long file_size(void) {
    FILE* f = fopen(PATH, "rb");
    long n = -1;
    if (f && !fseek(f, 0, SEEK_END)) n = ftell(f);
    if (f) fclose(f);
    return n;
}

/* shader n of the test: its fields, reads and words from n */
static void make_shader(int n, PfShader* s, u32* cbr, u32* spv) {
    int i;
    memset(s, 0, sizeof *s);
    s->hash = 0x1234567890ABCDEFull * (u64)(n + 1);
    s->qh = 0xFEDCBA0987654321ull ^ (u64)n;
    s->qh_a = n == 1 ? 0x55AA55AA55AA55AAull : 0;
    s->stage = n == 1 ? ST_FS : ST_VS;
    s->clen = 0x100 + 8 * (u32)n;
    s->clen_a = n == 1 ? 0x80 : 0;
    s->ncbr = (u32)n % 3; /* 0, 1 or 2 reads */
    s->nspv = 5 + (u32)n;
    for (i = 0; i < 20; i++) s->mask[i] = (u32)(n * 100 + i);
    s->si.stage = (int)s->stage;
    s->si.ntex = n;
    s->si.tex[0].flags = SNX_TEX_INDEXED;
    s->si.cbuf_max[17] = 0xC0DE0000u + (u32)n;
    snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "shader %d", n);
    for (i = 0; i < (int)s->ncbr * 4; i++) cbr[i] = (u32)(n * 1000 + i);
    for (i = 0; i < (int)s->nspv; i++) spv[i] = i == 0 ? 0x07230203u : (u32)(n * 7 + i);
}
static void make_key(int n, u8* key) {
    int i;
    for (i = 0; i < KEYB; i++) key[i] = (u8)(n * 31 + i);
}
static void check_shader(int i, int n) {
    PfShader s;
    u32 cbr[8], spv[16];
    make_shader(n, &s, cbr, spv);
    CHECK(!memcmp(&got.sh[i], &s, sizeof s));
    CHECK(!memcmp(got.cbr[i], cbr, s.ncbr * 16));
    CHECK(!memcmp(got.spv[i], spv, s.nspv * 4));
}
static void add_shader(PFile* pf, int n, u64 module) {
    PfShader s;
    u32 cbr[8], spv[16];
    make_shader(n, &s, cbr, spv);
    pf_shader(pf, module, &s, cbr, spv, NULL, NULL, NULL);
}
/* shader n with its source: 64 bytes of code starting with n, and a transform feedback layout when with_xfb */
static void add_sourced(PFile* pf, int n, u64 module, int with_xfb) {
    PfShader s;
    PfSource src;
    ShXfb xfb;
    u32 cbr[8], spv[16];
    u8 code[64];
    make_shader(n, &s, cbr, spv);
    memset(&src, 0, sizeof src);
    src.hash = s.hash;
    src.stage = s.stage;
    src.ncbr = s.ncbr;
    src.ncode = sizeof code;
    src.flags = with_xfb ? PF_SRC_XFB : 0;
    memcpy(src.mask, s.mask, sizeof src.mask);
    memset(code, 0xC5, sizeof code);
    code[0] = (u8)n;
    memset(&xfb, 0, sizeof xfb);
    xfb.stride[3] = 4321;
    pf_shader(pf, module, &s, cbr, spv, &src, code, with_xfb ? &xfb : NULL);
}
/* a file of another build or driver whose guest part is the same: rebuilt from its sources, into a file of the new
   key that the next run reads as it is */
static void rebuilds(void) {
    PFile pf;
    u32 loaded[2] = {0, 0};
    remove(PATH);
    guest_bytes = 6;
    open_file(&pf, "guest;build one", 0x10, 0, loaded);
    add_sourced(&pf, 0, 0x8001, 0);
    add_shader(&pf, 1, 0x8002); /* without a source: can't be translated again */
    add_sourced(&pf, 2, 0x8003, 1);
    add_pipe(&pf, 0, 0x8001, 0, 0, 0, 0x8003);
    add_pipe(&pf, 1, 0x8001, 0, 0, 0, 0x8002); /* dropped with the shader */
    add_pipe(&pf, 2, 0x8003, 0, 0, 0, 0);
    close_file(&pf);
    /* the same build: the sources aren't looked at */
    memset(&source_got, 0, sizeof source_got);
    open_file(&pf, "guest;build one", 0x20, 0, loaded);
    CHECK(loaded[0] == 3 && loaded[1] == 3 && source_got.n == 0);
    close_file(&pf);
    /* another build: each source handed back, recorded in the new file, and the pipelines of its shaders */
    memset(&source_got, 0, sizeof source_got);
    source_module = 0x30;
    rebuilding = &pf;
    open_file(&pf, "guest;build two", 0x90, 0, loaded);
    CHECK(source_got.n == 2 && source_got.shader[0] == 0 && !source_got.xfb[0] && source_got.shader[1] == 2 &&
          source_got.xfb[1] && got.nsh == 0 && loaded[0] == 2);
    CHECK(loaded[1] == 2 && got.npipe == 2);
    check_pipe(0, 0, 0x30, 0, 0, 0, 0x31);
    check_pipe(1, 2, 0x31, 0, 0, 0, 0);
    CHECK(pf.nsh == 2); /* the new file's */
    add_pipe(&pf, 0, 0x30, 0, 0, 0, 0x31); /* as pipeline.c records each one again */
    add_pipe(&pf, 2, 0x31, 0, 0, 0, 0);
    close_file(&pf);
    /* the next run of that build reads the rebuilt file as it is */
    memset(&source_got, 0, sizeof source_got);
    open_file(&pf, "guest;build two", 0xA0, 0, loaded);
    CHECK(source_got.n == 0 && loaded[0] == 2 && loaded[1] == 2);
    check_shader(0, 0);
    check_shader(1, 2);
    check_pipe(0, 0, 0xA0, 0, 0, 0, 0xA1);
    close_file(&pf);
    /* another guest part (another device): started over */
    open_file(&pf, "gues2;build two", 0xB0, 0, loaded);
    CHECK(source_got.n == 0 && loaded[0] == 0 && loaded[1] == 0);
    close_file(&pf);
    rebuilding = NULL;
    guest_bytes = 0;
    remove(PATH);
}
static void add_pipe(PFile* pf, int n, u64 vs, u64 tcs, u64 tes, u64 gs, u64 fs) {
    u64 m[5];
    u8 key[KEYB];
    m[0] = vs;
    m[1] = tcs;
    m[2] = tes;
    m[3] = gs;
    m[4] = fs;
    make_key(n, key);
    pf_pipe(pf, m, key, KEYB);
}
static void check_pipe(int i, int n, u64 vs, u64 tcs, u64 tes, u64 gs, u64 fs) {
    u8 key[KEYB];
    make_key(n, key);
    CHECK(!memcmp(got.key[i], key, KEYB));
    CHECK(got.mods[i][0] == vs && got.mods[i][1] == tcs && got.mods[i][2] == tes && got.mods[i][3] == gs &&
          got.mods[i][4] == fs);
}

/* Large warm caches take the parallel validation path. A bad source at any range boundary still cuts the file at
   that record, before any shader or pipeline after it is handed back. Mapping failure takes the same checked path. */
static void large_checks(void) {
    PFile pf;
    u32 loaded[2];
    remove(PATH);
    guest_bytes = 0;
    open_file(&pf, "parallel", 0x10, 0, loaded);
    size_t header = (size_t)pf.size, chunk = 1u << 20;
    u8* payload = calloc(1, chunk);
    CHECK(payload);
    const void* pieces[] = {payload};
    size_t sizes[] = {chunk};
    u64 check;
    for (unsigned i = 0; i < 9; i++) CHECK(!pf_append(&pf, PF_SOURCE, pieces, sizes, 1, &check));
    free(payload);
    add_shader(&pf, 0, 0x101);
    add_shader(&pf, 1, 0x102);
    add_pipe(&pf, 0, 0x101, 0, 0, 0, 0x102);
    close_file(&pf);
    size_t n = (size_t)file_size();
    u8* original = malloc(n);
    FILE* f = fopen(PATH, "rb");
    CHECK(original && f && fread(original, 1, n, f) == n);
    fclose(f);
    g_settings.prepare_shaders = 2;
    for (int fallback = 0; fallback < 2; fallback++) {
        fail_map = fallback;
        open_file(&pf, "parallel", 0x20, 0, loaded);
        CHECK(loaded[0] == 2 && loaded[1] == 1 && pf.size == n);
        check_shader(0, 0);
        check_shader(1, 1);
        check_pipe(0, 0, 0x20, 0, 0, 0, 0x21);
        close_file(&pf);
    }
    fail_map = 0;
    for (unsigned record = 0; record < 9; record += 4) {
        size_t at = header + record * (PF_REC + chunk), byte = at + PF_REC + chunk / 2;
        original[byte] ^= 1;
        f = fopen(PATH, "wb");
        CHECK(f && fwrite(original, 1, n, f) == n);
        fclose(f);
        original[byte] ^= 1;
        open_file(&pf, "parallel", 0x30, 0, loaded);
        CHECK(!loaded[0] && !loaded[1] && pf.size == at && file_size() == (long)at);
        close_file(&pf);
    }
    free(original);
    g_settings.prepare_shaders = 0;
    remove(PATH);
}

int main(void) {
    PFile pf;
    u32 loaded[2];
    long whole, header;
    remove(PATH);

    /* a first run: no file, records written as shaders and pipelines are made */
    open_file(&pf, "key one", 0xA0, 0, loaded);
    CHECK(loaded[0] == 0 && loaded[1] == 0 && got.nsh == 0 && pf.f);
    header = file_size();
    CHECK(header > 16 && header % 8 == 0);
    add_shader(&pf, 0, 0x1111);
    add_shader(&pf, 1, 0x2222);
    add_pipe(&pf, 0, 0x1111, 0, 0, 0, 0x2222);
    add_pipe(&pf, 1, 0x1111, 0, 0, 0, 0x9999); /* a module without a record (a clear's): not kept */
    add_pipe(&pf, 2, 0x2222, 0, 0, 0, 0);
    CHECK(pf.nsh == 2);
    close_file(&pf);

    /* A mapping can be unavailable while ordinary reads and appends still work. */
    fail_map = 1;
    open_file(&pf, "key one", 0xB0, 0, loaded);
    CHECK(loaded[0] == 2 && loaded[1] == 2 && pf.f);
    check_shader(0, 0);
    check_shader(1, 1);
    check_pipe(0, 0, 0xB0, 0, 0, 0, 0xB1);
    check_pipe(1, 2, 0xB1, 0, 0, 0, 0);
    close_file(&pf);
    fail_map = 0;

    /* the next run: everything back in order, the pipelines' shaders as the modules made of them now */
    open_file(&pf, "key one", 0xB0, 0, loaded);
    CHECK(loaded[0] == 2 && loaded[1] == 2 && got.nsh == 2 && got.npipe == 2);
    check_shader(0, 0);
    check_shader(1, 1);
    check_pipe(0, 0, 0xB0, 0, 0, 0, 0xB1);
    check_pipe(1, 2, 0xB1, 0, 0, 0, 0);
    /* what it makes refers to the shaders read and the ones it adds */
    add_shader(&pf, 2, 0x3333);
    add_pipe(&pf, 3, 0xB0, 0xB1, 0x3333, 0, 0xB1);
    close_file(&pf);
    whole = file_size();

    open_file(&pf, "key one", 0xC0, 0, loaded);
    CHECK(loaded[0] == 3 && loaded[1] == 3);
    check_shader(2, 2);
    check_pipe(2, 3, 0xC0, 0xC1, 0xC2, 0, 0xC1);
    CHECK(file_size() == whole && pf.size == (u64)whole);
    close_file(&pf);

    /* a shader that can't be made again: the pipelines with it are dropped */
    open_file(&pf, "key one", 0xD0, 1, loaded);
    CHECK(loaded[0] == 2 && got.nsh == 3 && loaded[1] == 1 && got.npipe == 1);
    check_pipe(0, 2, 0xD0, 0, 0, 0, 0);
    close_file(&pf);

    /* a record cut short (a crash while writing it): the reading stops before it, and the file goes on from there */
    {
        FILE* f = fopen(PATH, "rb");
        u8* all = (u8*)malloc((size_t)whole);
        CHECK(f && all && fread(all, 1, (size_t)whole, f) == (size_t)whole);
        fclose(f);
        f = fopen(PATH, "wb");
        CHECK(f && fwrite(all, 1, (size_t)whole - 12, f) == (size_t)whole - 12);
        fclose(f);
        open_file(&pf, "key one", 0xE0, 0, loaded);
        CHECK(loaded[0] == 3 && loaded[1] == 2); /* the last pipeline is gone */
        CHECK(pf.size < (u64)whole && file_size() == (long)pf.size);
        add_pipe(&pf, 4, 0xE2, 0, 0, 0, 0);
        close_file(&pf);
        open_file(&pf, "key one", 0xF0, 0, loaded);
        CHECK(loaded[0] == 3 && loaded[1] == 3);
        check_pipe(2, 4, 0xF2, 0, 0, 0, 0);
        close_file(&pf);

        /* a changed byte in the second shader's payload: only the first is read, then the file is cut there */
        f = fopen(PATH, "wb");
        all[header + PF_REC + sizeof(PfShader) + 40]++; /* past the first record, inside the second */
        CHECK(f && fwrite(all, 1, (size_t)whole, f) == (size_t)whole);
        fclose(f);
        free(all);
        open_file(&pf, "key one", 0x10, 0, loaded);
        CHECK(got.nsh == 1 && loaded[0] == 1 && loaded[1] == 0);
        CHECK(pf.nsh == 1 && file_size() == (long)pf.size);
        /* the shaders' numbers go on from the one left */
        add_shader(&pf, 1, 0x4444);
        add_pipe(&pf, 5, 0x10, 0, 0, 0, 0x4444);
        close_file(&pf);
        open_file(&pf, "key one", 0x20, 0, loaded);
        CHECK(loaded[0] == 2 && loaded[1] == 1);
        check_shader(1, 1);
        check_pipe(0, 5, 0x20, 0, 0, 0, 0x21);
        close_file(&pf);
    }

    /* another build or device: nothing read, the file started over */
    open_file(&pf, "key two", 0x30, 0, loaded);
    CHECK(loaded[0] == 0 && loaded[1] == 0 && file_size() == header && pf.nsh == 0);
    add_shader(&pf, 0, 0x5555);
    close_file(&pf);
    open_file(&pf, "key one", 0x40, 0, loaded);
    CHECK(loaded[0] == 0 && file_size() == header);
    close_file(&pf);

    /* a full file takes no more, and the run goes on */
    open_file(&pf, "key one", 0x50, 0, loaded);
    pf.size = PF_MAX - 64;
    add_shader(&pf, 3, 0x6666);
    CHECK(!pf.f && pf.nsh == 0);
    add_pipe(&pf, 6, 0x6666, 0, 0, 0, 0);
    close_file(&pf);
    CHECK(file_size() == header);

    remove(PATH);
#ifdef __linux__
    /* a file no write reaches (a full disk): said with its path and the system's reason */
    {
        PfLoad load;
        load.shader = load_shader;
        load.pipe = load_pipe;
        load.shipref = load_shipref;
        load.source = load_source;
        load.ctx = NULL;
        load.rebuild = 0;
        memset(&pf, 0, sizeof pf);
        pm_init(&pf.lock);
        warned[0] = 0;
        pf_open(&pf, "/dev/full", "key one", 0, &load, loaded);
        CHECK(!pf.f && strstr(warned, "cannot write /dev/full (") && strstr(warned, strerror(ENOSPC)));
        close_file(&pf);
    }
#endif
    mapped();
    shiprefs();
    rebuilds();
    large_checks();
    printf("pipeline_file: %u checks passed\n", checks);
    return 0;
}
