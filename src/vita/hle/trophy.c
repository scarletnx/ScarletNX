/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceNpTrophy: the game's trophies, the console's own (no network). A context is a game's trophy set, its
   communication ID's: what it holds comes from the trophy pack the game ships, app0:sce_sys/trophy/<ID>_<NN>/TROPHY.TRP
   (the update's over it), whose TROPCONF.SFM lists each trophy (its id, its grade, platinum, gold, silver or bronze).
   What the player unlocked is kept in the memory card, ux0:user/00/trophy/<ID>_<NN>/unlocked.bin (the game folder's
   ux0/): the 128 trophies' flags, the runtime's own record. Unlocking the last trophy but the platinum unlocks the
   platinum too, which the game is told. Each unlock is logged. Handles (for the console's asynchronous calls, which
   are synchronous here) are numbers. Error codes and the unlock rules as Vita3K (GPL-2.0-or-later,
   modules/SceNpTrophy/SceNpTrophy.cpp) has them */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_NP_TROPHY_ERROR_NOT_INITIALIZED         0x80551601u
#define SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED     0x80551602u
#define SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT        0x80551604u
#define SCE_NP_TROPHY_ERROR_EXCEEDS_MAX             0x80551606u
#define SCE_NP_TROPHY_ERROR_INVALID_HANDLE          0x80551608u
#define SCE_NP_TROPHY_ERROR_INVALID_CONTEXT         0x80551609u
#define SCE_NP_TROPHY_ERROR_INVALID_NPCOMMID        0x8055160au
#define SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID       0x8055160eu
#define SCE_NP_TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED 0x8055160fu
#define SCE_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK  0x80551610u
#define SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_EXISTS  0x80551616u
#define SCE_NP_TROPHY_ERROR_TRP_FILE_NOT_FOUND      0x80551619u
#define SCE_NP_TROPHY_ERROR_INVALID_TRP_FILE_FORMAT 0x8055161au
#define SCE_NP_TROPHY_INVALID_TROPHY_ID             0xffffffffu
#define TROPHIES 128u /* SceNpTrophyFlagArray's */
#define CONTEXTS 4u
#define HANDLES  16u
#define TRP_MAGIC 0xdca24d00u

enum { GRADE_NONE, GRADE_PLATINUM, GRADE_GOLD, GRADE_SILVER, GRADE_BRONZE };
typedef struct {
    bool used;
    char id[16];             /* NPWR01234_00 */
    unsigned count;          /* the trophies the set has */
    uint32_t platinum;       /* its id, SCE_NP_TROPHY_INVALID_TROPHY_ID for none */
    uint8_t grades[TROPHIES]; /* by id, GRADE_NONE for an id the set doesn't have */
    uint32_t unlocked[TROPHIES / 32];
} Context;
struct VitaTrophy {
    bool initialized;
    Context contexts[CONTEXTS]; /* by handle minus 1 */
    bool handles[HANDLES];      /* by handle minus 1 */
};

static struct VitaTrophy* trophy(VitaProcess* p) {
    if (!p->trophy) p->trophy = calloc(1, sizeof *p->trophy);
    return p->trophy;
}
void vita_trophy_free(VitaProcess* p) {
    free(p->trophy);
    p->trophy = NULL;
}
static Context* context_of(VitaProcess* p, uint32_t handle) {
    struct VitaTrophy* s = p->trophy;
    return s && handle - 1 < CONTEXTS && s->contexts[handle - 1].used ? &s->contexts[handle - 1] : NULL;
}

/* ---- the trophy pack ---- */
/* the value of attribute name in the element [at, end): its text up to the closing quote, into out */
static bool attribute(const char* at, const char* end, const char* name, char* out, size_t size) {
    size_t n = strlen(name);
    for (const char* q = at; q + n + 2 < end; ++q)
        if (!memcmp(q, name, n) && q[n] == '=' && q[n + 1] == '"' && (q == at || q[-1] == ' ')) {
            const char* v = q + n + 2;
            size_t length = 0;
            while (v + length < end && v[length] != '"' && length + 1 < size) ++length;
            memcpy(out, v, length);
            out[length] = 0;
            return true;
        }
    return false;
}
/* TROPHY.TRP (big-endian: magic, version, size, entries, entry size, then from 0x40 each entry's name (32 bytes),
   offset and size) read for its TROPCONF.SFM's trophies */
static uint32_t read_pack(const unsigned char* trp, size_t size, Context* c) {
    if (size < 0x40 || snx_ld_be32(trp) != TRP_MAGIC) return SCE_NP_TROPHY_ERROR_INVALID_TRP_FILE_FORMAT;
    uint32_t entries = snx_ld_be32(trp + 16), entry_size = snx_ld_be32(trp + 20);
    if (entry_size < 48 || entries > 4096 || 0x40 + (uint64_t)entries * entry_size > size)
        return SCE_NP_TROPHY_ERROR_INVALID_TRP_FILE_FORMAT;
    for (uint32_t i = 0; i < entries; ++i) {
        const unsigned char* e = trp + 0x40 + (size_t)i * entry_size;
        uint64_t at = snx_ld_be64(e + 32), length = snx_ld_be64(e + 40);
        if (memcmp(e, "TROPCONF.SFM", 13) || at > size || length > size - at) continue;
        const char *x = (const char*)trp + at, *end = x + length;
        c->platinum = SCE_NP_TROPHY_INVALID_TROPHY_ID;
        for (const char* q = x; q + 8 < end; ++q) {
            if (memcmp(q, "<trophy ", 8)) continue;
            const char* close = memchr(q, '>', (size_t)(end - q));
            if (!close) break;
            char id[8], type[4];
            if (attribute(q, close, "id", id, sizeof id) && attribute(q, close, "ttype", type, sizeof type)) {
                unsigned n = (unsigned)strtoul(id, NULL, 10);
                uint8_t grade = type[0] == 'P' ? GRADE_PLATINUM
                                : type[0] == 'G' ? GRADE_GOLD
                                : type[0] == 'S' ? GRADE_SILVER
                                                 : GRADE_BRONZE;
                if (n < TROPHIES && !c->grades[n]) {
                    c->grades[n] = grade, ++c->count;
                    if (grade == GRADE_PLATINUM) c->platinum = n;
                }
            }
            q = close;
        }
        return c->count ? 0 : SCE_NP_TROPHY_ERROR_INVALID_TRP_FILE_FORMAT;
    }
    return SCE_NP_TROPHY_ERROR_INVALID_TRP_FILE_FORMAT;
}
static char* record_path(VitaProcess* p, const Context* c) {
    return snx_format("%s/user/00/trophy/%s/unlocked.bin", p->ux0, c->id);
}
static void record_write(VitaProcess* p, const Context* c) {
    char *path = record_path(p, c), *folder = path ? snx_dirname(path) : NULL;
    unsigned char bytes[TROPHIES / 8];
    for (unsigned i = 0; i < TROPHIES / 8; ++i) bytes[i] = (unsigned char)(c->unlocked[i / 4] >> (i % 4 * 8));
    if (!folder || snx_mkdirs(folder) || snx_write_file(path, bytes, sizeof bytes))
        printf("vita: cannot keep the trophies unlocked in %s\n", path ? path : "the memory card");
    free(folder);
    free(path);
}

/* ---- the calls ---- */
/* sceNpTrophyInit(options) */
uint32_t vita_trophy_init(VitaProcess* p, VitaThread* t, uint32_t options) {
    (void)options;
    (void)t;
    struct VitaTrophy* s = trophy(p);
    if (!s) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (s->initialized) return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;
    s->initialized = true;
    return 0;
}
uint32_t vita_trophy_term(VitaProcess* p, VitaThread* t) {
    (void)t;
    if (!p->trophy || !p->trophy->initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    memset(p->trophy, 0, sizeof *p->trophy);
    return 0;
}
/* sceNpTrophyCreateContext(context*, const SceNpCommunicationId* (9 characters, a NUL, the number, a byte), sign,
   options) */
uint32_t vita_trophy_create_context(VitaProcess* p, VitaThread* t, uint32_t out, uint32_t commid) {
    (void)t;
    struct VitaTrophy* s = p->trophy;
    if (!s || !s->initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!out || !commid) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const unsigned char* id = vita_ptr(p, commid);
    if (id[10] > 99) return SCE_NP_TROPHY_ERROR_INVALID_NPCOMMID;
    char name[16];
    snprintf(name, sizeof name, "%.9s_%02u", (const char*)id, id[10]);
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, '.')) return SCE_NP_TROPHY_ERROR_INVALID_NPCOMMID;
    unsigned free_slot = CONTEXTS;
    for (unsigned i = 0; i < CONTEXTS; ++i) {
        if (s->contexts[i].used && !strcmp(s->contexts[i].id, name)) return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_EXISTS;
        if (!s->contexts[i].used && free_slot == CONTEXTS) free_slot = i;
    }
    if (free_slot == CONTEXTS) return SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
    Context* c = &s->contexts[free_slot];
    *c = (Context){.used = true};
    snprintf(c->id, sizeof c->id, "%s", name);
    /* the pack: the update's, else the game's */
    unsigned char* trp = NULL;
    size_t size = 0;
    for (int i = 0; i < 2 && !trp; ++i) {
        const char* root = i ? p->app : p->patch;
        char* path = root ? snx_format("%s/sce_sys/trophy/%s/TROPHY.TRP", root, name) : NULL;
        trp = path ? snx_read_file(path, &size) : NULL;
        free(path);
    }
    uint32_t error = trp ? read_pack(trp, size, c) : SCE_NP_TROPHY_ERROR_TRP_FILE_NOT_FOUND;
    free(trp);
    if (error) {
        *c = (Context){0};
        return error;
    }
    char* path = record_path(p, c);
    size_t got = 0;
    unsigned char* bytes = path ? snx_read_file(path, &got) : NULL;
    for (unsigned i = 0; bytes && got == TROPHIES / 8 && i < TROPHIES / 8; ++i)
        c->unlocked[i / 4] |= (uint32_t)bytes[i] << (i % 4 * 8);
    free(bytes);
    free(path);
    vita_write32(p, out, free_slot + 1);
    return 0;
}
uint32_t vita_trophy_destroy_context(VitaProcess* p, VitaThread* t, uint32_t context_id) {
    (void)t;
    Context* c = context_of(p, context_id);
    if (!c) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    *c = (Context){0};
    return 0;
}
/* sceNpTrophyCreateHandle(handle*) */
uint32_t vita_trophy_create_handle(VitaProcess* p, VitaThread* t, uint32_t out) {
    (void)t;
    struct VitaTrophy* s = p->trophy;
    if (!s || !s->initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!out) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    for (unsigned i = 0; i < HANDLES; ++i)
        if (!s->handles[i]) {
            s->handles[i] = true;
            vita_write32(p, out, i + 1);
            return 0;
        }
    return SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
}
uint32_t vita_trophy_destroy_handle(VitaProcess* p, VitaThread* t, uint32_t handle) {
    (void)t;
    struct VitaTrophy* s = p->trophy;
    if (!s || handle - 1 >= HANDLES || !s->handles[handle - 1]) return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;
    s->handles[handle - 1] = false;
    return 0;
}
/* sceNpTrophyGetTrophyUnlockState(context, handle, SceNpTrophyFlagArray*, count*) */
uint32_t vita_trophy_get_unlock_state(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t handle, uint32_t flags,
                                      uint32_t count) {
    (void)t, (void)handle;
    if (!p->trophy || !p->trophy->initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!flags || !count) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const Context* c = context_of(p, context);
    if (!c) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    for (unsigned i = 0; i < TROPHIES / 32; ++i) vita_write32(p, flags + i * 4, c->unlocked[i]);
    vita_write32(p, count, c->count);
    return 0;
}
static bool unlocked(const Context* c, uint32_t id) { return c->unlocked[id / 32] >> (id % 32) & 1; }
/* sceNpTrophyUnlockTrophy(context, handle, trophy, platinum*) */
uint32_t vita_trophy_unlock_trophy(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t handle, uint32_t id,
                                   uint32_t out) {
    (void)t, (void)handle;
    if (!p->trophy || !p->trophy->initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (id >= TROPHIES) return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
    Context* c = context_of(p, context);
    if (!c) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (!c->grades[id]) return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
    if (c->grades[id] == GRADE_PLATINUM) return SCE_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK;
    if (unlocked(c, id)) return SCE_NP_TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED;
    c->unlocked[id / 32] |= 1u << (id % 32);
    printf("vita: trophy %03u of %s unlocked\n", (unsigned)id, c->id);
    /* the platinum, once every other trophy is unlocked */
    uint32_t platinum = SCE_NP_TROPHY_INVALID_TROPHY_ID;
    if (c->platinum != SCE_NP_TROPHY_INVALID_TROPHY_ID && !unlocked(c, c->platinum)) {
        bool all = true;
        for (uint32_t i = 0; i < TROPHIES && all; ++i) all = !c->grades[i] || i == c->platinum || unlocked(c, i);
        if (all) {
            c->unlocked[c->platinum / 32] |= 1u << (c->platinum % 32);
            platinum = c->platinum;
            printf("vita: trophy %03u of %s (the platinum) unlocked\n", (unsigned)platinum, c->id);
        }
    }
    if (out) vita_write32(p, out, platinum);
    record_write(p, c);
    return 0;
}

/* ---- save states (vita/runtime/state.c): the trophy contexts and handles as they are ---- */
/* a state's trophies, in an allocation of its own, which the process takes: NULL when the state has none */
typedef struct {
    struct VitaTrophy* trophy;
} TrophyStaged;
uint64_t vita_trophy_state_schema(void) {
    static const struct VitaTrophy s; /* only walked */
    uint64_t h = SNX_FNV_BASIS ^ sizeof s;
    __builtin_dump_struct(&s, snx_state_schema_out, &h);
    __builtin_dump_struct(&s.contexts[0], snx_state_schema_out, &h);
    return h;
}
int vita_trophy_state_save(VitaProcess* p, StateIo* io) {
    uint8_t present = p->trophy != NULL;
    return io_put(io, &present, 1) || (present && io_put(io, p->trophy, sizeof *p->trophy)) ? -1 : 0;
}
void vita_trophy_state_drop(void* staged) {
    TrophyStaged* s = staged;
    if (s) free(s->trophy);
    free(s);
}
void* vita_trophy_state_parse(StateIo* io, char* why, size_t whysize) {
    TrophyStaged* s = calloc(1, sizeof *s);
    uint8_t present = 0;
    if (!s || io_get(io, &present, 1) || (present && !(s->trophy = malloc(sizeof *s->trophy))))
        snprintf(why, whysize, "its trophies' section is damaged, or no memory is left for it (%zu bytes)",
                 sizeof(struct VitaTrophy));
    else if (s->trophy && io_get(io, s->trophy, sizeof *s->trophy))
        snprintf(why, whysize, "its trophies' section is damaged or cut short");
    else
        return s;
    vita_trophy_state_drop(s);
    return NULL;
}
void vita_trophy_state_install(VitaProcess* p, void* staged) {
    TrophyStaged* s = staged;
    vita_trophy_free(p);
    p->trophy = s->trophy;
    free(s);
}
