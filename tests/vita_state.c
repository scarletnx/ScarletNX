/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's save states (src/vita/runtime/state.c), on a process the test makes and drives through the imports the
   runtime answers: a memory block with a pattern, a semaphore, a second thread waiting with a deadline, a file read
   part way, a file written, a folder listed part way, sceGxm with memory mapped, a sound port. The Vita's threads run
   by the host's clock, so two runs are never the same frame for frame: a state is checked by what it puts back. After
   the state is saved and the game changes all of it, a load gives back the memory, the blocks, the objects, the
   threads' registers and every open file at its place (the next read is the bytes after the saved place), and a save
   right after holds the same sections byte for byte (the times within a second); a fresh process takes it the same.
   A state of another game, another version, with a section of another layout, damaged, cut short, or naming a file
   that is gone, is refused before anything changes; a save is refused while a thread runs, while one is inside a
   call of the runtime's into the game, and while a socket is open, each naming what. An AAC decoder that decoded frames
   before the save decodes the frames after it the same once loaded, but for the first, which its transform's overlap
   reaches */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "vita/hle/hle.h"
#include "vita/runtime/process.h"
#include "vita/runtime/state.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define SCE_O_RDONLY 0x0001u
#define SCE_O_WRONLY 0x0002u
#define SCE_O_CREAT  0x0200u
#define SCE_O_TRUNC  0x0400u
#define DATA_BYTES   4096u

/* the import name called by t with args (r0 to r3, then its stack) */
static uint32_t call(VitaProcess* p, VitaThread* t, const char* name, const uint32_t* args, unsigned count) {
    const VitaHle* h = vita_hle_table;
    while (h->name && strcmp(h->name, name)) ++h;
    if (!h->name) {
        fprintf(stderr, "vita_state: no import %s\n", name);
        exit(1);
    }
    for (unsigned i = 0; i < count; ++i)
        if (i < 4)
            t->cpu.r[i] = args[i];
        else
            vita_write32(p, t->cpu.r[13] + (i - 4) * 4, args[i]);
    p->current = t;
    uint32_t r = h->function(p, t);
    p->current = NULL;
    return r;
}
#define CALL(name, ...)                                                                                                \
    call(p, t, name, (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))

/* a guest string at at */
static uint32_t text(VitaProcess* p, uint32_t at, const char* s) {
    memcpy(vita_ptr(p, at), s, strlen(s) + 1);
    return at;
}
/* the byte of the data file at offset */
static uint8_t data_byte(uint32_t offset) { return (uint8_t)(offset * 7 + 3); }

/* AAC LC frames the test writes (FFmpeg here has no AAC encoder): one channel, one long window, band 0 (four
   coefficients) in codebook 11 at the global gain, its two pairs of values of the frame's, so each frame differs and
   what FFmpeg keeps of one (its transform's overlap) reaches the next */
#define AAC_FRAMES 10
#define AAC_GAIN   170
typedef struct {
    uint8_t* p;
    size_t bits;
} Bits;
static void put_bits(Bits* b, uint32_t value, unsigned n) {
    for (unsigned i = n; i-- > 0; ++b->bits)
        if (value >> i & 1) b->p[b->bits / 8] |= (uint8_t)(0x80u >> b->bits % 8);
}
/* a pair of codebook 11's (each 0 to 2), its code (ff_aac_spectral_codes[10][17 * y + z]) and signs */
static void put_pair(Bits* b, unsigned y, unsigned z, bool negative) {
    static const uint16_t code[3][3] = {{0x000, 0x006, 0x019}, {0x005, 0x001, 0x008}, {0x017, 0x007, 0x009}};
    static const uint8_t bits[3][3] = {{4, 5, 6}, {5, 4, 5}, {6, 5, 5}};
    put_bits(b, code[y][z], bits[y][z]);
    if (y) put_bits(b, negative, 1);
    if (z) put_bits(b, 0, 1);
}
static void aac_frame(VitaProcess* p, uint32_t at, unsigned f) {
    uint8_t* out = vita_ptr(p, at);
    memset(out, 0, 64);
    Bits b = {out, 0};
    put_bits(&b, 0, 3);        /* a single channel element */
    put_bits(&b, 0, 4);        /* its tag */
    put_bits(&b, AAC_GAIN, 8); /* global_gain */
    put_bits(&b, 0, 4);        /* ics_info: reserved, ONLY_LONG_SEQUENCE, the sine window */
    put_bits(&b, 1, 6);        /* max_sfb */
    put_bits(&b, 0, 1);        /* no prediction */
    put_bits(&b, 11, 4);       /* a section in codebook 11 */
    put_bits(&b, 1, 5);        /* of one band */
    put_bits(&b, 0, 1);        /* band 0's scalefactor the global gain: the code of a difference of 0 */
    put_bits(&b, 0, 3);        /* no pulse data, TNS or gain control */
    put_pair(&b, 1 + f % 2, f % 3, f & 4);
    put_pair(&b, f % 3, 1 + f / 3 % 2, false);
    put_bits(&b, 7, 3); /* the end */
}
/* frame f decoded by the decoder of ctrl into out (the PCM's bytes there, at most 4096): 0, or 1 having said why */
static int aac_decode(VitaProcess* p, VitaThread* t, uint32_t ctrl, unsigned f, uint8_t* out, uint32_t* got) {
    uint32_t es = ctrl + 0x800, pcm = ctrl + 0x1000;
    aac_frame(p, es, f);
    vita_write32(p, ctrl + 8, es), vita_write32(p, ctrl + 0x14, pcm);
    CHECK(CALL("sceAudiodecDecode", ctrl) == 0);
    *got = vita_read32(p, ctrl + 0x18);
    CHECK(*got && *got <= 4096);
    if (out) memcpy(out, vita_ptr(p, pcm), *got);
    return 0;
}

/* what the test made in a process */
typedef struct {
    uint32_t mem, block, sema, worker, file, written, folder, decoder;
} Made;
/* a process of the game in root/name: 0, or 1 having said why */
static int make(const char* root, const char* name, VitaProcess** out) {
    char error[256] = "", *folder = snx_path_join(root, name);
    CHECK(folder && !snx_mkdirs(folder));
    VitaImage image = {0};
    *out = vita_process_new(&image, folder, error, sizeof error);
    if (!*out) fprintf(stderr, "vita_state: vita_process_new: %s\n", error);
    free(folder);
    return *out ? 0 : 1;
}
/* the game's state the test makes in p, the data file at app0:data.bin */
static int setup(VitaProcess* p, Made* m) {
    VitaThread* t = &p->threads[0];
    t->cpu.r[13] -= 0x100;
    uint32_t mem = m->mem = vita_alloc(p, &p->user, 0x10000, 0x1000);
    CHECK(mem);
    for (uint32_t i = 0; i < 0x1000; ++i) vita_write32(p, mem + 0x8000 + i * 4, i * 0x9E3779B1u);
    m->block = CALL("sceKernelAllocMemBlock", text(p, mem, "state block"), 0x0c20d060u, 0x40000);
    CHECK(!(m->block & 0x80000000u));
    m->sema = CALL("sceKernelCreateSema", text(p, mem, "state sema"), 0, 2, 9);
    CHECK(!(m->sema & 0x80000000u) && CALL("sceKernelSignalSema", m->sema, 3) == 0);
    m->worker = CALL("sceKernelCreateThread", text(p, mem, "worker"), 0x81000000u, 0x40, 0x4000, 0);
    CHECK(!(m->worker & 0x80000000u) && CALL("sceKernelStartThread", m->worker, 0, 0) == 0);
    VitaThread* w = vita_thread(p, m->worker);
    CHECK(w);
    for (unsigned i = 0; i < 13; ++i) w->cpu.r[i] = 0x1000u + i;
    call(p, w, "sceKernelDelayThread", (const uint32_t[]){5000000}, 1); /* waits 5 s, a deadline */
    CHECK(w->state == VITA_THREAD_WAITING && w->wait.deadline);
    m->file = CALL("sceIoOpen", text(p, mem, "app0:data.bin"), SCE_O_RDONLY, 0);
    CHECK(!(m->file & 0x80000000u) && CALL("sceIoRead", m->file, mem + 0x100, 100) == 100);
    m->written = CALL("sceIoOpen", text(p, mem, "ux0:data/out.bin"), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0);
    CHECK(!(m->written & 0x80000000u) && CALL("sceIoWrite", m->written, mem + 0x100, 10) == 10);
    m->folder = CALL("sceIoDopen", text(p, mem, "app0:"));
    CHECK(!(m->folder & 0x80000000u) && CALL("sceIoDread", m->folder, mem + 0x2000) == 1);
    /* sceGxm: its display queue of 2 with no data, and the block's memory mapped */
    vita_write32(p, mem + 0x3000, 0), vita_write32(p, mem + 0x3004, 2), vita_write32(p, mem + 0x3008, 0);
    vita_write32(p, mem + 0x300c, 0), vita_write32(p, mem + 0x3010, 0x40000);
    CHECK(CALL("sceGxmInitialize", mem + 0x3000) == 0);
    uint32_t base = 0;
    CHECK(CALL("sceKernelGetMemBlockBase", m->block, mem + 0x3100) == 0 && (base = vita_read32(p, mem + 0x3100)));
    CHECK(CALL("sceGxmMapMemory", base, 0x40000) == 0);
    CHECK(CALL("sceAudioOutOpenPort", 0, 256, 48000, 1) == 1); /* its ID */
    /* an AAC decoder, one channel at 48 kHz (its SceAudiodecCtrl at decoder, its SceAudiodecInfoAac after it), which
       decoded frames 0 to 2 */
    uint32_t ctrl = m->decoder = mem + 0x4000, info = ctrl + 0x40;
    vita_write32(p, ctrl, 0x28), vita_write32(p, ctrl + 0x24, info);
    vita_write32(p, info, 0x14), vita_write32(p, info + 4, 0), vita_write32(p, info + 8, 1);
    vita_write32(p, info + 12, 48000), vita_write32(p, info + 16, 0);
    CHECK(CALL("sceAudiodecCreateDecoder", ctrl, 0x1005) == 0);
    for (unsigned f = 0; f < 3; ++f) {
        uint32_t got;
        if (aac_decode(p, t, ctrl, f, NULL, &got)) return 1;
    }
    return 0;
}
/* every section of two states but TIME byte for byte, TIME within a second: 0, or 1 naming the first difference */
static int same_state(const char* what, const char* a, const char* b) {
    static const char* const tags[] = {"PROCESS", "LINKS", "MEMORY", "IO", "GXM", "AUDIO", "NGS", "TROPHY", "TIME"};
    SnxStateHeader ha, hb;
    char why[256] = "";
    StateReader *ra = sr_open(a, &ha, why, sizeof why), *rb = ra ? sr_open(b, &hb, why, sizeof why) : NULL;
    int r = !ra || !rb;
    if (r) fprintf(stderr, "vita_state: %s: %s\n", what, why);
    for (size_t t = 0; !r && t < sizeof tags / sizeof *tags; ++t) {
        uint64_t sa = 0, sb = 0;
        if (sr_begin(ra, tags[t], &sa) || sr_begin(rb, tags[t], &sb) || sa != sb || sr_left(ra) != sr_left(rb)) {
            fprintf(stderr, "vita_state: %s: the %s sections differ in their schema or size\n", what, tags[t]);
            r = 1;
            break;
        }
        uint64_t n = sr_left(ra);
        uint8_t *pa = malloc(n ? n : 1), *pb = malloc(n ? n : 1);
        if (!pa || !pb || sr_get(ra, pa, n) || sr_get(rb, pb, n)) r = 1;
        if (!r && !strcmp(tags[t], "TIME")) {
            for (uint64_t i = 4; i + 8 <= n && !r; i += 8) {
                int64_t ta = (int64_t)snx_ld_le64(pa + i), tb = (int64_t)snx_ld_le64(pb + i);
                if ((ta == INT64_MIN) != (tb == INT64_MIN) ||
                    (ta != INT64_MIN && (ta - tb > 1000000000 || tb - ta > 1000000000))) {
                    fprintf(stderr, "vita_state: %s: time %llu is %lld ns, the state's %lld\n", what,
                            (unsigned long long)(i / 8), (long long)tb, (long long)ta);
                    r = 1;
                }
            }
        } else
            for (uint64_t i = 0; !r && i < n; ++i)
                if (pa[i] != pb[i]) {
                    fprintf(stderr, "vita_state: %s: the %s sections differ first at byte %llu (%02X, not %02X)\n",
                            what, tags[t], (unsigned long long)i, pb[i], pa[i]);
                    r = 1;
                }
        free(pa);
        free(pb);
    }
    sr_close(ra);
    sr_close(rb);
    return r;
}
static int copy_changed(const char* from, const char* to, long long offset, uint8_t flip, size_t cut) {
    size_t size = 0;
    uint8_t* bytes = (uint8_t*)snx_read_file(from, &size);
    if (!bytes) return -1;
    if (cut) size = cut;
    if (flip) bytes[offset] ^= flip;
    int r = snx_write_file(to, bytes, size);
    free(bytes);
    return r;
}
/* a load of path refused with each of the texts in its reason, and p as it was: 0, or 1 having said why */
static int refused(VitaProcess* p, const char* dir, const char* path, const char* const* texts) {
    char why[512] = "", *before = snx_path_join(dir, "before.snxstate"), *after = snx_path_join(dir, "after.snxstate");
    CHECK(before && after && !snx_vita_state_save(p, before, NULL, why, sizeof why));
    if (!snx_vita_state_load(p, path, why, sizeof why)) {
        fprintf(stderr, "vita_state: %s loaded\n", path);
        return 1;
    }
    printf("refused %s: %s\n", path, why);
    for (; *texts; ++texts)
        if (!strstr(why, *texts)) {
            fprintf(stderr, "vita_state: the refusal of %s says \"%s\", without \"%s\"\n", path, why, *texts);
            return 1;
        }
    CHECK(!snx_vita_state_save(p, after, NULL, why, sizeof why));
    if (same_state("a refused load", before, after)) return 1;
    remove(before), remove(after);
    free(before), free(after);
    return 0;
}
/* from renamed to, as no handle of the game's holds it open: Windows renames no file that is open, so a test that
   moves a file the game has open would pass only where rename takes one. 0, or -1 having said which */
static int rename_closed(const VitaProcess* p, const char* from, const char* to) {
    for (unsigned i = 0; i < VITA_FILES; ++i) {
        const char *a = p->files[i].path, *b = from;
        if (!p->files[i].host || !a) continue;
        while (*a && (*a == *b || ((*a == '/' || *a == '\\') && (*b == '/' || *b == '\\')))) ++a, ++b;
        if (*a || *b) continue;
        fprintf(stderr, "vita_state: %s is open in the game (its file %u): it can't be renamed on Windows\n", from, i);
        return -1;
    }
    return rename(from, to);
}
/* the open files at their places: the next read of data.bin is from byte 100, the next write of out.bin at byte 10,
   the next entry of the folder its second */
static int files_back(VitaProcess* p, const Made* m, uint32_t mem) {
    VitaThread* t = &p->threads[0];
    CHECK(CALL("sceIoRead", m->file, mem + 0x200, 10) == 10);
    for (uint32_t i = 0; i < 10; ++i) CHECK(*(uint8_t*)vita_ptr(p, mem + 0x200 + i) == data_byte(100 + i));
    CHECK(CALL("sceIoWrite", m->written, mem + 0x200, 5) == 5);
    FILE* f = vita_io_file(p, m->written);
    CHECK(f && fflush(f) == 0);
    char* out = snx_path_join(p->ux0, "data/out.bin");
    size_t size = 0;
    void* bytes = out ? snx_read_file(out, &size) : NULL;
    CHECK(bytes && size == 15);
    free(bytes);
    free(out);
    CHECK(CALL("sceIoDread", m->folder, mem + 0x2000) == 1);
    return 0;
}

/* the decoder made again by the load decodes frames 3 on: the first as many bytes, the ones after it the same */
static int decodes_the_same(VitaProcess* p, const Made* m, uint8_t ref[][4096], const uint32_t* ref_got,
                            uint8_t back[][4096], uint32_t* back_got) {
    VitaThread* t = &p->threads[0];
    for (unsigned f = 3; f < AAC_FRAMES; ++f) {
        if (aac_decode(p, t, m->decoder, f, back[f], &back_got[f])) return 1;
        CHECK(back_got[f] == ref_got[f]);
        if (f > 3 && memcmp(back[f], ref[f], ref_got[f])) {
            fprintf(stderr, "vita_state: AAC frame %u decodes otherwise after the load\n", f);
            return 1;
        }
    }
    return 0;
}

int main(void) {
#ifdef _WIN32
    _putenv_s("SDL_AUDIO_DRIVER", "dummy"); /* the sound port's stream, on no device of the host's */
#else
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
#endif
    char* root = snx_temp_dir(NULL);
    char why[512] = "";
    CHECK(root);
    char *saved = snx_path_join(root, "a.snxstate"), *again = snx_path_join(root, "b.snxstate"),
         *fresh = snx_path_join(root, "c.snxstate"), *changed = snx_path_join(root, "x.snxstate"),
         *app = snx_path_join(root, "game/app"), *data = snx_path_join(root, "game/app/data.bin"),
         *other_app = snx_path_join(root, "other/app"), *ux0_data = snx_path_join(root, "game/ux0/data");
    CHECK(saved && again && fresh && changed && app && data && other_app && ux0_data);
    CHECK(!snx_mkdirs(app) && !snx_mkdirs(other_app) && !snx_mkdirs(ux0_data));
    {
        uint8_t bytes[DATA_BYTES];
        for (uint32_t i = 0; i < DATA_BYTES; ++i) bytes[i] = data_byte(i);
        CHECK(!snx_write_file(data, bytes, sizeof bytes));
        char* second = snx_path_join(app, "second.bin");
        CHECK(second && !snx_write_file(second, bytes, 16));
        free(second);
    }
    VitaProcess* p;
    Made m;
    if (make(root, "game", &p) || setup(p, &m)) return 1;
    VitaThread* t = &p->threads[0];
    uint32_t mem = m.mem;
    CHECK(vita_read32(p, mem + 0x8000 + 4) == 0x9E3779B1u);
    uint32_t r4 = vita_thread(p, m.worker)->cpu.r[4];
    CHECK(!snx_vita_state_save(p, saved, NULL, why, sizeof why));
    /* the frames after the state, as the decoder that went on gives them */
    static uint8_t ref[AAC_FRAMES][4096], back[AAC_FRAMES][4096];
    uint32_t ref_got[AAC_FRAMES] = {0}, back_got[AAC_FRAMES] = {0};
    size_t loud = 0;
    for (unsigned f = 3; f < AAC_FRAMES; ++f) {
        if (aac_decode(p, t, m.decoder, f, ref[f], &ref_got[f])) return 1;
        for (uint32_t i = 0; i < ref_got[f]; ++i) loud += ref[f][i] != 0;
    }
    CHECK(loud > 1000);
    /* everything changed */
    for (uint32_t i = 0; i < 0x1000; ++i) vita_write32(p, mem + 0x8000 + i * 4, 0xDEADBEEFu);
    CHECK(CALL("sceIoRead", m.file, mem + 0x100, 200) == 200);
    CHECK(CALL("sceIoDclose", m.folder) == 0);
    CHECK(CALL("sceKernelSignalSema", m.sema, 1) == 0);
    uint32_t extra = vita_alloc(p, &p->user, 0x20000, 0x1000);
    CHECK(extra);
    uint32_t extra_sema = CALL("sceKernelCreateSema", text(p, mem, "extra"), 0, 0, 1);
    vita_thread(p, m.worker)->cpu.r[4] = 0xBADu;
    /* the state back */
    if (snx_vita_state_load(p, saved, why, sizeof why)) {
        fprintf(stderr, "vita_state: %s\n", why);
        return 1;
    }
    CHECK(vita_read32(p, mem + 0x8000 + 4) == 0x9E3779B1u);
    CHECK(!vita_mapped(p, extra, 0x20000) && !vita_object(p, extra_sema, VITA_OBJ_SEMA));
    CHECK(vita_object(p, m.sema, VITA_OBJ_SEMA)->u.sema.count == 5);
    CHECK(vita_thread(p, m.worker)->cpu.r[4] == r4 && vita_thread(p, m.worker)->state == VITA_THREAD_WAITING);
    CHECK(!snx_vita_state_save(p, again, NULL, why, sizeof why));
    if (same_state("loaded in the same process", saved, again)) return 1;
    if (files_back(p, &m, mem)) return 1;
    if (decodes_the_same(p, &m, ref, ref_got, back, back_got)) return 1;
    /* a fresh process of the game takes it the same */
    VitaProcess* q;
    if (make(root, "game", &q)) return 1;
    if (snx_vita_state_load(q, saved, why, sizeof why)) {
        fprintf(stderr, "vita_state: a fresh process: %s\n", why);
        return 1;
    }
    CHECK(!snx_vita_state_save(q, fresh, NULL, why, sizeof why));
    if (same_state("loaded in a fresh process", saved, fresh)) return 1;
    if (files_back(q, &m, mem)) return 1;
    if (decodes_the_same(q, &m, ref, ref_got, back, back_got)) return 1;
    vita_process_free(q);
    /* refused: another game's, another version's, a section of another layout, a damaged one, one cut short */
    {
        VitaProcess* o;
        if (make(root, "other", &o)) return 1;
        const char* const game[] = {"another game's", "(other)", NULL};
        if (refused(o, root, saved, game)) return 1;
        vita_process_free(o);
        size_t size = 0;
        uint8_t* bytes = (uint8_t*)snx_read_file(saved, &size);
        CHECK(bytes && size > SNX_STATE_HEADER);
        uint64_t table = snx_ld_le64(bytes + 48);
        free(bytes);
        const char* const version[] = {"version 9", "version 1", NULL};
        CHECK(!copy_changed(saved, changed, 12, 1 ^ 9, 0));
        if (refused(p, root, changed, version)) return 1;
        const char* const cut[] = {"not written to its end", NULL};
        CHECK(!copy_changed(saved, changed, 0, 0, (size_t)table + 20));
        if (refused(p, root, changed, cut)) return 1;
        /* the first section is GXM: its schema in the table's first entry */
        const char* const layout[] = {"GXM", "another layout", NULL};
        CHECK(!copy_changed(saved, changed, (long long)table + 8, 0x5A, 0));
        if (refused(p, root, changed, layout)) return 1;
        const char* const damaged[] = {"audio decoders", "damaged", NULL};
        CHECK(!copy_changed(saved, changed, (long long)table - 6, 0x40, 0));
        if (refused(p, root, changed, damaged)) return 1;
    }
    /* a file the state names that is gone: the game's handle closed first, which the state still holds open */
    {
        char* moved = snx_path_join(root, "data.moved");
        CHECK(moved && CALL("sceIoClose", m.file) == 0 && !rename_closed(p, data, moved));
        const char* const gone[] = {"data.bin", "can't be opened again", NULL};
        if (refused(p, root, saved, gone)) return 1;
        CHECK(!rename(moved, data));
        free(moved);
    }
    /* saves refused */
    p->current = t;
    CHECK(snx_vita_state_save(p, changed, NULL, why, sizeof why) && strstr(why, "is running"));
    p->current = NULL;
    t->call.active = true;
    CHECK(snx_vita_state_save(p, changed, NULL, why, sizeof why) && strstr(why, "inside a call"));
    t->call.active = false;
    uint32_t socket = CALL("sceNetSocket", text(p, mem, "s"), 2, 2, 0);
    CHECK(!(socket & 0x80000000u));
    CHECK(snx_vita_state_save(p, changed, NULL, why, sizeof why) && strstr(why, "1 sockets open"));
    CHECK(CALL("sceNetSocketClose", socket) == 0);
    {
        size_t size = 0;
        void* bytes = snx_read_file(saved, &size);
        CHECK(bytes);
        free(bytes);
        printf("a Vita state of the test's process: %zu bytes\n", size);
    }
    vita_process_free(p);
    CHECK(!snx_remove_tree(root));
    free(saved), free(again), free(fresh), free(changed), free(app), free(data), free(other_app), free(ux0_data),
        free(root);
    puts("vita_state: ok");
    return 0;
}
