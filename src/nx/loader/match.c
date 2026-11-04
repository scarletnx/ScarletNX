/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/match.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>

#define STT_FUNC 2

typedef struct {
    SnxU64 prefix, full; /* the masked hashes of the first SNX_MATCH_MIN_WORDS words and of all of them */
    SnxU64 value, size;  /* module offset and bytes */
    SnxU32 symbol, name_len;
} Sig;

static SnxU32 rd32(const unsigned char* p) {
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}

/* FNV-1a, a word a step, of the n words at code with what a link or a load moves zeroed: the offset of a B or a BL (a call's target
   is laid out elsewhere in each module), the immediate of an ADRP, an ADR and an LDR (literal), and the low 12 bits
   that an ADD or a load or store takes from an ADRP's register in the 8 words before it. A conditional branch keeps
   its offset, which stays inside the function, and so does a constant: a CRC's polynomial is part of the function */
static SnxU64 masked_hash(const unsigned char* code, SnxU64 n) {
    SnxU64 h = SNX_FNV_BASIS, i, j;
    for (i = 0; i < n; ++i) {
        SnxU32 w = rd32(code + 4 * i);
        if ((w & 0x7C000000u) == 0x14000000u)
            w &= 0xFC000000u;
        else if ((w & 0x1F000000u) == 0x10000000u)
            w &= 0x9F00001Fu;
        else if ((w & 0x3B000000u) == 0x18000000u)
            w &= 0xFF00001Fu;
        else if ((w & 0xFF800000u) == 0x91000000u || (w & 0x3B000000u) == 0x39000000u)
            for (j = i < 8 ? 0 : i - 8; j < i; ++j) {
                SnxU32 a = rd32(code + 4 * j);
                if ((a & 0x9F000000u) == 0x90000000u && (a & 31) == ((w >> 5) & 31)) {
                    w &= ~(0xFFFu << 10);
                    break;
                }
            }
        h = snx_fnv_step(h, w);
    }
    return h;
}

/* by value, and of a function's aliases the shortest name first, which names it (memcpy before __memcpy_device) */
static int alias_order(const void* a, const void* b) {
    const Sig *x = (const Sig*)a, *y = (const Sig*)b;
    if (x->value != y->value) return x->value < y->value ? -1 : 1;
    if (x->name_len != y->name_len) return x->name_len < y->name_len ? -1 : 1;
    return x->symbol < y->symbol ? -1 : x->symbol > y->symbol;
}
static int sig_order(const void* a, const void* b) {
    const Sig *x = (const Sig*)a, *y = (const Sig*)b;
    if (x->prefix != y->prefix) return x->prefix < y->prefix ? -1 : 1;
    return x->value < y->value ? -1 : x->value > y->value;
}
/* whether an Itanium-mangled name is a constructor's or a destructor's (C1 to C3, D0 to D2 before the parameters):
   their code is the same few stores and calls in every class, whatever the length */
static int structor(const char* name) {
    static const char* const marks[] = {"C1E", "C2E", "C3E", "D0E", "D1E", "D2E"};
    size_t i;
    for (i = 0; !strncmp(name, "_Z", 2) && i < sizeof marks / sizeof *marks; ++i)
        if (strstr(name, marks[i])) return 1;
    return 0;
}

/* the sdk module's exported functions in its text, an alias once, signed when long enough and no structor */
static size_t signatures(const SnxModuleImage* sdk, Sig* sigs, SnxMatchStats* st) {
    SnxU64 text_end = (SnxU64)sdk->text_off + sdk->text_size;
    size_t i, n = 0, kept = 0;
    for (i = 0; i < sdk->symbol_count; ++i) {
        const SnxSymbol* s = &sdk->symbols[i];
        if (s->type == STT_FUNC && s->value >= sdk->text_off && s->size && s->size <= text_end - s->value) {
            sigs[n].symbol = (SnxU32)i;
            sigs[n].name_len = (SnxU32)strlen(s->name);
            sigs[n].value = s->value;
            sigs[n].size = s->size & ~3ull;
            n++;
        }
    }
    qsort(sigs, n, sizeof *sigs, alias_order);
    for (i = 0; i < n; ++i) {
        if (kept && sigs[kept - 1].value == sigs[i].value) continue;
        if (sigs[i].size < 4 * SNX_MATCH_MIN_WORDS || structor(sdk->symbols[sigs[i].symbol].name)) {
            st->skipped++;
            continue;
        }
        sigs[kept] = sigs[i];
        sigs[kept].prefix = masked_hash(sdk->image + sigs[i].value, SNX_MATCH_MIN_WORDS);
        sigs[kept].full = masked_hash(sdk->image + sigs[i].value, sigs[i].size / 4);
        kept++;
    }
    qsort(sigs, kept, sizeof *sigs, sig_order);
    st->signatures = kept;
    return kept;
}

size_t snx_match_program(const SnxProgram* p, SnxMatch** out, SnxMatchStats* st) {
    const SnxModuleImage* sdk = NULL;
    Sig* sigs;
    SnxMatch* found = NULL;
    SnxU32* uses;
    size_t nsig, i, k, n = 0, cap = 0;
    memset(st, 0, sizeof *st);
    *out = NULL;
    for (i = 0; p->npdm.is64 && i < p->module_count; ++i)
        if (!strcmp(p->modules[i].name, "sdk")) sdk = &p->modules[st->sdk = i];
    if (!sdk || !sdk->symbol_count) return 0;
    sigs = (Sig*)snx_alloc(sdk->symbol_count * sizeof *sigs);
    uses = (SnxU32*)snx_alloc(sdk->symbol_count * sizeof *uses);
    nsig = signatures(sdk, sigs, st);
    for (i = 0; nsig && i < p->module_count; ++i) {
        const SnxModuleImage* m = &p->modules[i];
        SnxU64 text_end = (SnxU64)m->text_off + m->text_size, *exported;
        size_t nexp = 0, f, first = n;
        if (m == sdk || !strcmp(m->name, "rtld")) continue;
        exported = (SnxU64*)snx_alloc((m->symbol_count + 1) * sizeof *exported);
        for (k = 0; k < m->symbol_count; ++k)
            if (m->symbols[k].type == STT_FUNC) exported[nexp++] = m->symbols[k].value;
        qsort(exported, nexp, sizeof *exported, snx_compare_u64);
        for (f = 0; f < m->function_count; ++f) { /* the FDEs' starts and the exports' */
            SnxU64 off = m->functions[f], prefix;
            const Sig *hit = NULL, *s;
            size_t lo = 0, hi = nsig;
            int ambiguous = 0;
            if (off < m->text_off || text_end - off < 4 * SNX_MATCH_MIN_WORDS ||
                bsearch(&off, exported, nexp, sizeof *exported, snx_compare_u64))
                continue;
            prefix = masked_hash(m->image + off, SNX_MATCH_MIN_WORDS);
            while (lo < hi) { /* the first signature of this prefix */
                size_t mid = lo + (hi - lo) / 2;
                if (sigs[mid].prefix < prefix)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            for (s = sigs + lo; s < sigs + nsig && s->prefix == prefix; ++s) {
                if (s->size > text_end - off || masked_hash(m->image + off, s->size / 4) != s->full) continue;
                ambiguous |= hit != NULL; /* two sdk functions with this code, or one whose code begins another's */
                hit = s;
            }
            if (!hit) continue;
            if (ambiguous) {
                st->ambiguous++;
                continue;
            }
            if (n == cap) {
                cap = cap ? 2 * cap : 64;
                found = (SnxMatch*)snx_realloc(found, cap * sizeof *found);
            }
            found[n].addr = m->base + off;
            found[n].words = (SnxU32)(hit->size / 4);
            found[n].symbol = hit->symbol;
            n++;
        }
        /* an SDK function is linked into a module once: a signature that matches two of its starts is generic code (a
           deleting destructor, a call wrapper) and names neither */
        memset(uses, 0, sdk->symbol_count * sizeof *uses);
        for (k = first; k < n; ++k) uses[found[k].symbol]++;
        for (k = f = first; k < n; ++k)
            if (uses[found[k].symbol] > 1)
                st->repeated++;
            else
                found[f++] = found[k];
        n = f;
        free(exported);
    }
    free(uses);
    free(sigs);
    *out = found; /* in address order: the modules are, and so are their functions */
    return n;
}
