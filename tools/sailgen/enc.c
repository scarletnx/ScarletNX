/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --encoders: an encoder for each decode clause of the Sail, the instruction table a back end that writes
   that architecture's own code takes its encodings from (README.md, "Encoders"). A clause's pattern fixes some of
   the word's bits; its lets slice the others into the decode function's fields (`let Rd = Slice(__opcode, 0, 5)`):
   the encoder is the fixed bits ORed with each field in its place, the fields the pattern fixes whole left out of
   its parameters. Nothing about any instruction is written here: the clauses are the Sail's.

     sailgen --encoders PREFIX -o OUT INPUT... [--roots ROOTS]

   PREFIX names the clauses' scattered function (__DecodeA64: every __DecodeA64_* clause). With --roots, only the
   clauses that call a root of that file (a roots file's root lines); else every one. Each encoder is

     static inline SnxU32 sge_TAG_NAME(fields...)

   TAG the architecture's in PREFIX, lower case (a64), NAME the decode function's, less decode_ and its
   _aarch64_instrs_... (add_addsub_shift: ADD, shifted register); decode functions named alike get the shortest tail
   of their names that tells them apart (ldr_imm_gen_post_idx, ldr_imm_gen_pre_idx). With a table of every encoder's
   family (its scattered function's name past PREFIX: DataProcFPSIMD), fixed bits, mask and fields
   (sge_TAG_table), for the tests to walk and a back end to find a word's clause in */
#include "sailgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N(i) (g_parse.nodes[i])
static const char* name_of(unsigned node) { return sg_str(N(node).name); }

typedef struct {
    unsigned name; /* string offset */
    unsigned lo, width;
} Field;

typedef struct {
    unsigned def, decode;  /* the clause, the decode function's name (string offset) */
    unsigned width;        /* 32, or 16 for a T16 halfword */
    SgU64 mask, value;     /* the fixed bits */
    Field fields[16];
    unsigned field_count;
    char short_name[160];
} Enc;

static void enc_bits(unsigned def, Enc* e) {
    unsigned p = g_parse.defs[def].params, x, i = 0, count = 0;
    if (N(p).kind != N_TUPLE || !N(N(p).a).next) sg_fail(p, "a decode clause's pattern is not (pc, (... as __opcode))");
    p = N(N(p).a).next;
    if (N(p).kind == N_BINOP && !strcmp(name_of(p), "as")) p = N(p).a;
    if (N(p).kind == N_BITS) {
        e->width = N(p).width;
        e->mask = e->width == 32 ? 0xffffffffull : 0xffffull;
        e->value = N(p).value;
        return;
    }
    if (N(p).kind != N_VEC) sg_fail(p, "a decode clause's pattern is not a bit vector");
    for (x = N(p).a; x; x = N(x).next) ++count;
    if (count != 32 && count != 16) sg_fail(p, "a decode pattern of %u bits", count);
    e->width = count;
    e->mask = e->value = 0;
    for (x = N(p).a; x; x = N(x).next, ++i) {
        unsigned bit = count - 1 - i;
        if (N(x).kind == N_WILD) continue;
        if (N(x).kind != N_ID || (strcmp(name_of(x), "bitone") && strcmp(name_of(x), "bitzero")))
            sg_fail(x, "a decode pattern bit that is not bitzero, bitone or _");
        e->mask |= 1ull << bit;
        if (!strcmp(name_of(x), "bitone")) e->value |= 1ull << bit;
    }
}

/* the decode function a clause calls, anywhere in its body (CFINV's under an if of its unpredictable bits) */
static unsigned enc_call(unsigned n) {
    unsigned r;
    if (!n) return 0;
    if (N(n).kind == N_CALL && !strncmp(name_of(n), "decode_", 7)) return N(n).name;
    if ((r = enc_call(N(n).a)) || (r = enc_call(N(n).b)) || (r = enc_call(N(n).c)) || (r = enc_call(N(n).d))) return r;
    return enc_call(N(n).next);
}

/* the clause's lets of slices of the word, and the decode function it calls */
static void enc_fields(unsigned def, Enc* e) {
    unsigned body = g_parse.defs[def].body, item;
    if (N(body).kind != N_BLOCK) sg_fail(body, "a decode clause's body is not a block");
    for (item = N(body).a; item; item = N(item).next) {
        unsigned v;
        if (N(item).kind != N_LET) continue;
        v = N(item).b;
        if (N(v).kind == N_CALL && !strcmp(name_of(v), "Slice")) {
            unsigned word = N(v).a, lo = N(word).next, n = lo ? N(lo).next : 0;
            Field* f;
            if (N(N(item).a).kind != N_ID || N(word).kind != N_ID || strcmp(name_of(word), "__opcode") || !n ||
                N(lo).kind != N_NUM || N(n).kind != N_NUM)
                sg_fail(item, "a decode clause's let that is not a field of the word");
            if (e->field_count == sizeof e->fields / sizeof e->fields[0]) sg_fail(item, "a clause of too many fields");
            f = &e->fields[e->field_count++];
            f->name = N(N(item).a).name;
            f->lo = (unsigned)N(lo).value;
            f->width = (unsigned)N(n).value;
        }
    }
    e->decode = enc_call(N(body).a);
    if (!e->decode) sg_fail(body, "a decode clause that calls no decode function");
}

static int enc_in_roots(unsigned name, const unsigned* roots, unsigned root_count) {
    unsigned i;
    if (!roots) return 1;
    for (i = 0; i < root_count; ++i)
        if (roots[i] == name) return 1;
    return 0;
}

/* a field the pattern fixes whole is no parameter */
static int enc_field_free(const Enc* e, const Field* f) {
    SgU64 m = (f->width >= 64 ? ~0ull : (1ull << f->width) - 1) << f->lo;
    return (e->mask & m) != m;
}

int sg_encoders(const char* prefix, const char* roots_path, const char* const* inputs, int input_count,
                const char* out_path) {
    unsigned d, root_count = 0, root_cap = 0, *roots = NULL, count = 0, cap = 0, k, j;
    size_t plen = strlen(prefix);
    Enc* encs = NULL;
    char tag[32];
    int i;
    /* the encoders' prefix: sge_ and the architecture's tag, __DecodeA64's a64 */
    if (strncmp(prefix, "__Decode", 8) || !prefix[8] || strlen(prefix + 8) >= sizeof tag)
        sg_fail(0, "--encoders takes a decode function's name, __Decode and the architecture (__DecodeA64)");
    for (k = 0; prefix[8 + k]; ++k) tag[k] = (char)(prefix[8 + k] >= 'A' && prefix[8 + k] <= 'Z' ? prefix[8 + k] + 32 : prefix[8 + k]);
    tag[k] = 0;
    for (i = 0; i < input_count; ++i) sg_parse_file(inputs[i], 0);
    if (roots_path) {
        size_t size;
        char *text = sg_read(roots_path, &size), *line;
        for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
            char word[32], name[512];
            if (sscanf(line, "%31s %511s", word, name) != 2 || strcmp(word, "root")) continue;
            if (root_count == root_cap) {
                root_cap = root_cap ? 2 * root_cap : 256;
                roots = realloc(roots, root_cap * sizeof *roots);
                if (!roots) sg_fail(0, "out of memory");
            }
            roots[root_count++] = sg_intern(name, strlen(name));
        }
        free(text);
    }
    for (d = 1; d < g_parse.def_count; ++d) {
        const SgDef* def = &g_parse.defs[d];
        Enc e;
        const char *dn, *cut;
        if (def->kind != D_CLAUSE || strncmp(sg_str(def->name), prefix, plen)) continue;
        memset(&e, 0, sizeof e);
        e.def = d;
        enc_bits(d, &e);
        enc_fields(d, &e);
        if (!enc_in_roots(e.decode, roots, root_count)) continue;
        dn = sg_str(e.decode) + 7;
        cut = strstr(dn, "_aarch64_instrs");
        if (!cut) cut = strstr(dn, "_aarch32_instrs");
        snprintf(e.short_name, sizeof e.short_name, "%.*s", cut ? (int)(cut - dn) : (int)strlen(dn), dn);
        if (count == cap) {
            cap = cap ? 2 * cap : 1024;
            encs = realloc(encs, cap * sizeof *encs);
            if (!encs) sg_fail(0, "out of memory");
        }
        encs[count++] = e;
    }
    /* names two clauses share: each with the shortest tail of its decode function's name (its words split at _)
       that is its own in the group (LDR immediate's post_idx, pre_idx and unsigned_offset) */
    for (k = 0; k < count; ++k) {
        char base[160];
        unsigned group[64], g = 0, m, words;
        if (encs[k].short_name[0] == '!') continue;
        for (j = k; j < count && g < 64; ++j)
            if (!strcmp(encs[j].short_name, encs[k].short_name)) group[g++] = j;
        if (g < 2) continue;
        snprintf(base, sizeof base, "%s", encs[k].short_name);
        for (words = 1; words < 8; ++words) {
            const char* tails[64];
            unsigned a, b, same = 0;
            for (m = 0; m < g; ++m) {
                const char* t = sg_str(encs[group[m]].decode) + strlen(sg_str(encs[group[m]].decode));
                unsigned w = 0;
                while (t > sg_str(encs[group[m]].decode) && w < words)
                    if (*--t == '_') ++w;
                tails[m] = *t == '_' ? t + 1 : t;
            }
            for (a = 0; a < g; ++a)
                for (b = a + 1; b < g; ++b) same |= !strcmp(tails[a], tails[b]);
            if (same) continue;
            for (m = 0; m < g; ++m)
                snprintf(encs[group[m]].short_name, sizeof encs[group[m]].short_name, "%s_%s", base, tails[m]);
            break;
        }
        if (words == 8) sg_fail(0, "the decode functions of %s are named alike", base);
    }
    if (roots)
        for (k = 0; k < root_count; ++k) {
            for (j = 0; j < count && encs[j].decode != roots[k]; ++j) {}
            if (j == count) sg_fail(0, "%s: no %s clause calls the root %s", roots_path, prefix, sg_str(roots[k]));
        }
    sg_out("/* Generated by tools/sailgen (--encoders %s) from", prefix);
    for (k = 0; k < g_parse.file_count; ++k) sg_out(" %s", g_parse.files[k].path);
    sg_out(": do not edit */\n\n");
    for (k = 0; k < count; ++k) {
        const Enc* e = &encs[k];
        int first = 1;
        sg_out("/* %s */\nstatic inline SnxU32 sge_%s_%s(", sg_str(e->decode), tag, e->short_name);
        for (j = 0; j < e->field_count; ++j) {
            if (!enc_field_free(e, &e->fields[j])) continue;
            sg_out("%sSnxU32 %s", first ? "" : ", ", sg_str(e->fields[j].name));
            first = 0;
        }
        if (first) sg_out("void");
        sg_out(") {\n    return 0x%08llXu", e->value);
        for (j = 0; j < e->field_count; ++j) {
            const Field* f = &e->fields[j];
            SgU64 m = ((1ull << f->width) - 1) << f->lo;
            if (!enc_field_free(e, f)) continue;
            sg_out(" | ((%s << %u) & 0x%08llXu)", sg_str(f->name), f->lo, m & ~e->mask);
        }
        sg_out(";\n}\n");
    }
    sg_out("\n/* every encoder's fixed bits and fields, in the clauses' order */\n");
    sg_out("static const struct {\n    const char* name;\n    const char* family;\n    SnxU32 mask, value;\n    unsigned char nfields;\n"
           "    struct {\n        const char* name;\n        unsigned char lo, width;\n    } fields[16];\n} sge_%s_table[] = {\n",
           tag);
    for (k = 0; k < count; ++k) {
        const Enc* e = &encs[k];
        sg_out("    {\"%s\", \"%s\", 0x%08llXu, 0x%08llXu, %u, {", e->short_name,
               sg_str(g_parse.defs[e->def].name) + plen + (sg_str(g_parse.defs[e->def].name)[plen] == '_'), e->mask,
               e->value, e->field_count);
        for (j = 0; j < e->field_count; ++j)
            sg_out("%s{\"%s\", %u, %u}", j ? ", " : "", sg_str(e->fields[j].name), e->fields[j].lo, e->fields[j].width);
        sg_out("}},\n");
    }
    sg_out("};\n#define sge_%s_count %uu\n", tag, count);
    free(encs);
    free(roots);
    return sg_flush(out_path);
}
