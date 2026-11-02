/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/shader_recompiler/frontend/maxwell/control_flow.cpp, indirect_branch_table_track.cpp,
   translate_program.cpp):
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* Maxwell program structure: length and hash, branch targets and jump tables, the CRS stack resolved statically,
   and VertexA + VertexB merged into one program. */
#include "nx/gpu/shader/translator.h"

/* Instruction words are 64-bit. Words 0-9 hold the 80-byte SPH, word 10 is the first scheduling word, and every
   fourth word after it is another one (IDX_OK). */
#define SH_SCAN_WORDS 16384 /* the length scan never looks further */
#define CRS_DEPTH     16    /* static resolution gives up on a deeper stack */

int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
int g_shader_sm70;

const u64* shader_words(const u8* code, u32 bytes, u64** copy) {
    u32 i, n = bytes / 8;
    *copy = NULL;
    if (!SNX_HOST_BE) return (const u64*)(const void*)code;
    *copy = (u64*)malloc((size_t)n * 8 + 8);
    if (!*copy) return NULL;
    for (i = 0; i < n; i++) (*copy)[i] = snx_ld_le64(code + (size_t)i * 8);
    return *copy;
}

/* each stage binds its own constant buffers in its own descriptor set; compute runs alone and shares the vertex
   stage's number */
int shader_vk_set(int stage) {
    switch (stage) {
        case ST_FS: return 1;
        case ST_TCS: return 2;
        case ST_TES: return 3;
        case ST_GS: return 4;
        default: return 0;
    }
}

static i32 sext(u32 value, int bits) {
    return (i32)(value & ((1u << (bits - 1)) - 1)) - (i32)(value & (1u << (bits - 1)));
}

/* the code word after i */
static u32 next_code(u32 i) {
    i++;
    if (i >= 10 && (i - 10) % 4 == 0) i++;
    return i;
}

/* a relative branch: a signed 24-bit byte offset from the next instruction. A target on a scheduling word means the
   instruction after it, as the hardware fetches bundles */
u32 bra_target(u64 w, u32 idx) {
    i32 t = (i32)idx + 1 + sext(B(w, 20, 24), 24) / 8;
    if (t >= 10 && (t - 10) % 4 == 0) t++;
    return (u32)t;
}

/* predicate PT, not negated, and flow condition T (15): the instruction always takes effect */
int flow_always(u64 w) { return B(w, 16, 4) == 7 && B(w, 0, 5) == 15; }

/* The nearest instruction before from in the code's order that writes reg, within limit instructions (0 for no
   limit), or -1. An undecodable word ends the search. */
static int find_writer(const u64* words, u32 from, u32 reg, u32 limit) {
    u32 j, seen = 0;
    if (reg == 255) return -1;
    for (j = from; j-- > 11;) {
        const Op* op;
        if (!IDX_OK(j)) continue;
        if (limit && seen++ == limit) return -1;
        op = decode(words[j]);
        if (!op) return -1;
        if (shader_writes_reg(words[j], op->id, reg)) return (int)j;
    }
    return -1;
}

/* One path through a region in def_reaches: where it is, whether the value def wrote still holds there, and the
   tokens pushed on the CRS stack since the region began (target << 2 | kind) */
typedef struct {
    u32 pc, valid, depth;
    u32 entry[CRS_DEPTH];
} RegionPath;

#define REGION_PATHS 256 /* the distinct paths def_reaches follows before it gives up */

/* Whether the paths left holding these tokens could come back into (start, use] through them later: a SYNC, BRK
   or CONT outside the region would pop one and land at its target */
static int tokens_return(const RegionPath* p, u32 start, u32 use) {
    u32 k;
    for (k = 0; k < p->depth; k++)
        if (p->entry[k] >> 2 > start && p->entry[k] >> 2 <= use) return 1;
    return 0;
}

/* path p goes on at target: inside the region it's queued unless an equal path was, and leaving it ends the path,
   allowed unless a token it leaves on the stack leads back in. Returns 0 on what def_reaches can't follow. */
static int region_go(RegionPath* paths, u32* npaths, u32* work, u32* nwork, const RegionPath* p, u32 target,
                     u32 start, u32 use) {
    u32 q;
    if (target < start || target > use) return !tokens_return(p, start, use);
    for (q = 0; q < *npaths; q++)
        if (paths[q].pc == target && paths[q].valid == p->valid && paths[q].depth == p->depth &&
            !memcmp(paths[q].entry, p->entry, p->depth * sizeof *p->entry))
            return 1;
    if (*npaths == REGION_PATHS) return 0;
    paths[*npaths] = *p;
    paths[*npaths].pc = target;
    work[(*nwork)++] = (*npaths)++;
    return 1;
}

/* Whether the value def writes to reg is the one use reads on every path that reaches use: control comes into
   (def, use] only through def, and nothing on the way writes reg again. The code between them may branch and
   reconverge: the compiler loads a jump table's entry and then puts an if-else (SSY ... SYNC) before the BRX that
   reads it. The region starts at the earliest push of a token whose target lies in (def, use], so that its SYNCs,
   BRKs and CONTs are seen, and each path through it is followed with the CRS tokens pushed in it, as
   shader_resolve_crs does for the whole program. Other BRXs' tables are assumed not to land in the region: the
   compiler's tables lead to their own switch's cases. */
static int def_reaches(const u64* words, u32 count, u32 def, u32 use, u32 reg) {
    RegionPath* paths;
    u32 start = def, j, npaths = 0, nwork = 0, work[REGION_PATHS];
    int ok = 1, pexit = 0;
    if (def >= use || use >= count) return 0;
    /* one pass down from def: a push below the region's start so far moves it down when its target lies in it */
    for (j = def; j-- > 11;) {
        const Op* op;
        u32 t;
        if (!IDX_OK(j)) continue;
        op = decode(words[j]);
        if (!op || (op->id != O_SSY && op->id != O_PBK && op->id != O_PCNT)) continue;
        t = bra_target(words[j], j);
        if (t > start && t <= use) start = j;
    }
    /* nothing outside the region branches into it, or pushes a token or a return point there */
    for (j = 11; j < count; j++) {
        const Op* op;
        u32 t;
        if (!IDX_OK(j)) continue;
        op = decode(words[j]);
        if (op && op->id == O_PEXIT) pexit = 1;
        if (j >= start && j < use) continue;
        if (!op || (op->id != O_BRA && op->id != O_SSY && op->id != O_PBK && op->id != O_PCNT && op->id != O_CAL &&
                    op->id != O_PRET && op->id != O_PEXIT))
            continue;
        t = bra_target(words[j], j);
        if (t > start && t <= use) return 0;
    }
    paths = (RegionPath*)calloc(REGION_PATHS, sizeof *paths);
    if (!paths) return 0;
    paths[0].pc = start;
    work[nwork++] = npaths++;
    while (ok && nwork) {
        RegionPath p = paths[work[--nwork]];
        u32 i = p.pc;
        for (;;) {
            u64 w;
            const Op* op;
            u32 t, k, kind;
            if (i == use) {
                ok = p.valid && !tokens_return(&p, start, use);
                break;
            }
            w = words[i];
            op = decode(w);
            if (!op) {
                ok = 0;
                break;
            }
            if (i == def)
                p.valid = 1;
            else if (shader_writes_reg(w, op->id, reg))
                p.valid = 0;
            if (B(w, 16, 4) != 15) { /* predicated never: no effect */
                switch (op->id) {
                    case O_SSY:
                    case O_PBK:
                    case O_PCNT:
                    case O_PEXIT:
                        if (p.depth == CRS_DEPTH) {
                            ok = 0;
                            break;
                        }
                        p.entry[p.depth++] = bra_target(w, i) << 2 | (u32)CRS_KIND(op->id);
                        break;
                    case O_SYNC:
                    case O_BRK:
                    case O_CONT: {
                        RegionPath popped = p;
                        kind = (u32)CRS_KIND(op->id);
                        for (k = p.depth; k > 0 && (p.entry[k - 1] & 3) != kind; k--) {
                        }
                        /* with no token of its kind pushed here the pop takes one from before the region, and
                           drops every token pushed since: the path leaves the region */
                        popped.depth = k ? k - 1 : 0;
                        t = k ? p.entry[k - 1] >> 2 : 0;
                        if (k) ok = region_go(paths, &npaths, work, &nwork, &popped, t, start, use);
                        if (flow_always(w)) goto next_path;
                        break;
                    }
                    case O_BRA:
                        ok = region_go(paths, &npaths, work, &nwork, &p, bra_target(w, i), start, use);
                        if (flow_always(w)) goto next_path;
                        break;
                    case O_EXIT: {
                        /* an EXIT goes to the target of the nearest PEXIT token. Whether it keeps the token and
                           the ones above it isn't settled, so both stacks are followed. With none pushed here it
                           ends the invocation, or goes to a PEXIT's from before the region with every token
                           pushed here still on the stack */
                        RegionPath popped = p;
                        for (k = p.depth; k > 0 && (p.entry[k - 1] & 3) != 3; k--) {
                        }
                        popped.depth = k ? k - 1 : 0;
                        if (k)
                            ok = region_go(paths, &npaths, work, &nwork, &p, p.entry[k - 1] >> 2, start, use) &&
                                 region_go(paths, &npaths, work, &nwork, &popped, p.entry[k - 1] >> 2, start, use);
                        else if (pexit)
                            ok = !tokens_return(&p, start, use);
                        if (flow_always(w)) goto next_path;
                        break;
                    }
                    case O_PRET: /* a return point outside the region doesn't lead back */
                        t = bra_target(w, i);
                        if (t > start && t <= use) ok = 0;
                        break;
                    case O_BRX:
                    case O_JMP:
                    case O_CAL:
                    case O_RET: ok = 0; break;
                    default: break;
                }
            }
            if (!ok) break;
            i = next_code(i);
        }
    next_path:;
    }
    free(paths);
    return ok;
}

/* The compiler bounds a switch index before it loads the jump table: IMNMX Ri, Rx, max; SHL Rj, Ri, 2;
   LDC.B32 Rk, c[bank][Rj + offset]. Gives the table's entry count, or 0 when the load isn't that idiom (as Eden's
   TrackIndirectBranchTable finds it). Each value has to reach the next instruction on every path (def_reaches): a
   nearer clamp on one side of a branch would give too few entries, and the translated BRX ends the invocation
   where an index past them lands. */
static int clamped_entries(const u64* words, u32 count, u32 load) {
    u64 w;
    int shl, at = find_writer(words, load, B(words[load], 8, 8), 32);
    if (at < 0) return 0;
    w = words[at];
    if (decode(w)->id != O_SHL_I || B(w, 16, 4) != 7 || B(w, 20, 19) != 2 || B(w, 56, 1) ||
        !def_reaches(words, count, (u32)at, load, B(w, 0, 8)))
        return 0;
    shl = at;
    at = find_writer(words, (u32)shl, B(w, 8, 8), 32);
    if (at < 0) return 0;
    w = words[at];
    if (decode(w)->id != O_IMNMX_I || B(w, 16, 4) != 7 || B(w, 56, 1) ||
        !def_reaches(words, count, (u32)at, (u32)shl, B(w, 0, 8)))
        return 0;
    return (int)B(w, 20, 19) + 1;
}

/* The targets of the BRX at index, read from its jump table through g_shader_cb_read: up to max_targets, or -1 when
   the table can't be found or read. The table address comes from the 32-bit LDC that last wrote the BRX's register,
   whose value the BRX reads on every path (def_reaches). With the clamp idiom every entry it allows has to be a
   valid target; without it the valid entries up to the first invalid one are taken. */
int shader_brx_targets(const u64* words, u32 index, u32 count, u32* targets, int max_targets) {
    u64 brx, load;
    u32 bank;
    i32 offset, imm;
    int at, exact, limit, k, n = 0;
    if (!g_shader_cb_read || index >= count || !IDX_OK(index) || max_targets <= 0) return -1;
    brx = words[index];
    at = find_writer(words, index, B(brx, 8, 8), 0);
    if (at < 0) return -1;
    load = words[at];
    /* entries are word offsets: a load of another size reads them wrong. Mode 0 is the plain c[bank][reg + offset] */
    if (decode(load)->id != O_LDC || B(load, 0, 8) != B(brx, 8, 8) || B(load, 16, 4) != 7 || B(load, 48, 3) != 4 ||
        B(load, 44, 2) != 0)
        return -1;
    if (!def_reaches(words, count, (u32)at, index, B(brx, 8, 8))) return -1;
    exact = clamped_entries(words, count, (u32)at);
    if (exact > max_targets) return -1;
    limit = exact ? exact : max_targets < 64 ? max_targets : 64;
    bank = B(load, 36, 5);
    offset = sext(B(load, 20, 16), 16);
    imm = sext(B(brx, 20, 24), 24);
    for (k = 0; k < limit; k++) {
        long long addr = (long long)offset + 4LL * k, rel;
        u32 value;
        long long t = 0;
        int ok = addr >= 0 && g_shader_cb_read(bank, (u32)addr, &value);
        if (ok) {
            /* a case lies after the BRX, never back in the region def_reaches followed: from there only a branch
               or a token it already refused would lead back */
            rel = (long long)value + imm;
            ok = rel >= 0 && rel % 8 == 0;
            if (ok) {
                t = (long long)index + 1 + rel / 8;
                if ((t - 10) % 4 == 0) t++;
                ok = t >= 11 && t < (long long)count;
            }
        }
        if (!ok) {
            if (exact) return -1;
            break;
        }
        targets[n++] = (u32)t;
    }
    return n ? n : -1;
}

/* The program's length in bytes (out_len) and a 64-bit FNV-1a hash of that many bytes. The scan follows every branch
   target forward and ends at an EXIT that always runs once no target lies past it, at an unconditional backward BRA
   past every target, or at an unconditional RET past every target when the program has calls. A jump table that
   can't be read leaves targets unknown: then only the compiler's own end of the program ends it, an unconditional
   EXIT and right after it a BRA to itself, which the compiler puts once, after all of the program's code (one game's
   program kept its jump table right after that end, which the scan read as instructions). Else an all-zero
   instruction word ends the scan before it, as it does in every case. That word is no Maxwell instruction, so
   control never reaches it, and the compiler fills bundles with NOPs, leaving zeros only as the padding after a
   program. NVN packs programs into one pool, so the window goes on past that padding into the next program, which an
   unknown jump table would otherwise take for its own code. */
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len) {
    const u64* words;
    u64* copy;
    u32 n = max_bytes / 8, i, last = 10, furthest = 0, len, exit_at = 0;
    int resolved = 1, calls = 0;
    u64 h = 1469598103934665603ull;
    if (n > SH_SCAN_WORDS) n = SH_SCAN_WORDS;
    words = shader_words(code, n * 8, &copy);
    if (!words) n = 0; /* no memory for the words (a big-endian host): the length is the header's */
    for (i = 11; i < n; i++) {
        const Op* op;
        u64 w;
        u32 t;
        if (!IDX_OK(i)) continue;
        w = words[i];
        if (!w) break;
        last = i;
        op = decode(w);
        if (!op) continue;
        switch (op->id) {
            case O_CAL:
            case O_PRET: calls = 1; /* fall through */
            case O_BRA:
            case O_PEXIT:
            case O_SSY:
            case O_PBK:
            case O_PCNT:
                t = bra_target(w, i);
                if (t < n && t > furthest) furthest = t;
                if (op->id == O_BRA && resolved && flow_always(w) && t <= i && i >= furthest) goto done;
                /* the compiler's end: a BRA to itself right after an unconditional EXIT, whatever the targets */
                if (op->id == O_BRA && flow_always(w) && t == i && exit_at &&
                    exit_at == (IDX_OK(i - 1) ? i - 1 : i - 2))
                    goto done;
                break;
            case O_BRX: {
                u32 targets[64];
                int k, count = shader_brx_targets(words, i, n, targets, 64);
                if (count < 0) resolved = 0;
                for (k = 0; k < count; k++)
                    if (targets[k] > furthest) furthest = targets[k];
                break;
            }
            case O_EXIT:
                if (flow_always(w) && i >= furthest && resolved) goto done;
                if (flow_always(w)) exit_at = i;
                break;
            case O_RET:
                if (calls && resolved && flow_always(w) && i >= furthest) goto done;
                break;
            default: break;
        }
    }
done:
    free(copy);
    len = (last + 1) * 8;
    if (len > max_bytes) len = max_bytes;
    for (i = 0; i < len; i++) { /* over the guest's bytes, the same hash on any host */
        h ^= code[i];
        h *= 1099511628211ull;
    }
    if (out_len) *out_len = len;
    return h;
}

/* One CRS stack entry: target << 2 | kind (CRS_KIND) */
typedef struct {
    u32 depth; /* CRS_DEPTH + 1 marks a block not reached yet */
    u32 entry[CRS_DEPTH];
} CrsStack;

typedef struct {
    const u64* words;
    u32 count, blocks;
    u8 leader[65536 / 8];
    u32* start;      /* each block's first word, ascending */
    u32* work;       /* blocks to walk */
    CrsStack* stack; /* the stack each block starts with */
    u32 nwork;
} CrsWalk;

static int is_leader(const CrsWalk* c, u32 i) { return i < c->count && (c->leader[i >> 3] >> (i & 7) & 1); }
static void mark_leader(CrsWalk* c, u32 i) {
    if (i < c->count) c->leader[i >> 3] |= (u8)(1u << (i & 7));
}

/* control arrives at target with stack s: the block's first arrival sets its stack, a later one must agree */
static int crs_arrive(CrsWalk* c, u32 target, const CrsStack* s) {
    u32 lo = 0, hi = c->blocks;
    CrsStack* at;
    if (!is_leader(c, target)) return 0;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (c->start[mid] < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    at = &c->stack[lo];
    if (at->depth > CRS_DEPTH) {
        *at = *s;
        c->work[c->nwork++] = lo;
        return 1;
    }
    return at->depth == s->depth && !memcmp(at->entry, s->entry, s->depth * sizeof *s->entry);
}

/* Walks one block from its stack. 0 when the stack can't be known statically. */
static int crs_block(CrsWalk* c, u32 b, u32* pop_targets) {
    CrsStack s = c->stack[b];
    u32 i = c->start[b];
    for (; i < c->count; i = next_code(i)) {
        u64 w = c->words[i];
        const Op* op;
        u32 t, k, kind, targets[64];
        int count, n;
        if (i != c->start[b] && is_leader(c, i)) return crs_arrive(c, i, &s);
        op = decode(w);
        if (!op || B(w, 16, 4) == 15) continue; /* predicated never */
        switch (op->id) {
            case O_SSY:
            case O_PBK:
            case O_PCNT:
                /* a push happens whatever its predicate says */
                if (s.depth == CRS_DEPTH) return 0;
                s.entry[s.depth++] = bra_target(w, i) << 2 | (u32)CRS_KIND(op->id);
                break;
            case O_SYNC:
            case O_BRK:
            case O_CONT: {
                CrsStack popped = s;
                kind = (u32)CRS_KIND(op->id);
                for (k = s.depth; k > 0 && (s.entry[k - 1] & 3) != kind; k--) {
                }
                if (!k) return 0;
                /* the pop takes the nearest entry of its kind and drops the ones pushed after it */
                t = s.entry[k - 1] >> 2;
                if (pop_targets[i] && pop_targets[i] != t) return 0;
                pop_targets[i] = t;
                popped.depth = k - 1;
                if (!crs_arrive(c, t, &popped)) return 0;
                if (flow_always(w)) return 1;
                break;
            }
            case O_BRA:
                if (!crs_arrive(c, bra_target(w, i), &s)) return 0;
                if (flow_always(w)) return 1;
                break;
            case O_BRX:
                count = shader_brx_targets(c->words, i, c->count, targets, 64);
                if (count < 0) return 0;
                for (n = 0; n < count; n++)
                    if (!crs_arrive(c, targets[n], &s)) return 0;
                if (flow_always(w)) return 1;
                break;
            case O_EXIT:
            case O_RET: /* without calls a RET ends the program */
                if (flow_always(w)) return 1;
                break;
            default: break;
        }
    }
    return 1;
}

/* The single CRS stack resolved at translation time: SSY, PBK and PCNT push a token and its target, and SYNC, BRK
   and CONT pop down to the nearest token of their kind (Eden's control_flow.cpp Stack). Walks the control flow from
   the entry with an empty stack and gives each pop its one target in pop_targets. Returns 0 when that can't be done
   statically (calls, PEXIT, JMP, an unreadable jump table, a pop with no token, a block or pop reached with two
   stacks, a deep stack, no memory): the translator then keeps the stack at run time. */
int shader_resolve_crs(const u64* words, u32 count, u32* pop_targets) {
    CrsWalk* c;
    u32 i, b, targets[64];
    int ok = 1, k, n;
    size_t bytes;
    if (count < 12 || count > 65536 || !words || !pop_targets) return 0;
    c = calloc(1, sizeof *c);
    if (!c) return 0;
    c->words = words;
    c->count = count;
    mark_leader(c, 11);
    for (i = 11; i < count && ok; i = next_code(i)) {
        u64 w = words[i];
        const Op* op = decode(w);
        if (!op) continue;
        switch (op->id) {
            case O_CAL:
            case O_PRET:
            case O_PEXIT:
            case O_JMP: ok = 0; break;
            case O_BRA:
            case O_SSY:
            case O_PBK:
            case O_PCNT: {
                u32 t = bra_target(w, i);
                if (t >= count || !IDX_OK(t)) ok = 0;
                mark_leader(c, t);
                break;
            }
            case O_BRX:
                n = shader_brx_targets(words, i, count, targets, 64);
                if (n < 0) ok = 0;
                for (k = 0; k < n; k++) mark_leader(c, targets[k]);
                break;
            default: break;
        }
        if (op->id == O_BRA || op->id == O_BRX || op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT ||
            op->id == O_EXIT || op->id == O_RET)
            mark_leader(c, next_code(i));
    }
    for (i = 0; i < count && ok; i++) c->blocks += is_leader(c, i);
    bytes = (size_t)c->blocks * (2 * sizeof(u32) + sizeof(CrsStack));
    if (ok) c->stack = malloc(bytes);
    if (!c->stack) {
        free(c);
        return 0;
    }
    c->start = (u32*)(c->stack + c->blocks);
    c->work = c->start + c->blocks;
    for (i = 0, b = 0; i < count; i++)
        if (is_leader(c, i)) {
            c->start[b] = i;
            c->stack[b++].depth = CRS_DEPTH + 1;
        }
    memset(pop_targets, 0, count * sizeof *pop_targets);
    {
        CrsStack empty;
        memset(&empty, 0, sizeof empty);
        crs_arrive(c, 11, &empty);
    }
    while (ok && c->nwork) ok = crs_block(c, c->work[--c->nwork], pop_targets);
    free(c->stack);
    free(c);
    return ok;
}

/* NVN splits some vertex programs in two: VertexA (slot 0) runs first and usually writes the position, VertexB
   (slot 1) the varyings. They become one program with B's header: A's code from word 10, then B's from the next
   whole bundle on, where A's EXITs now branch, under the same predicate and flow condition. Branches are relative,
   so both halves keep working where they land. Returns 0 when the result doesn't fit in max_bytes. */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes) {
    u32 len_a, len_b, words_a, start, i;
    if (!a || !b || !out || max_bytes < 96) return 0;
    shader_hash(a, max_bytes, &len_a);
    shader_hash(b, max_bytes, &len_b);
    if (len_a < 88 || len_b < 88) return 0;
    words_a = len_a / 8;
    start = 10 + ((words_a - 10 + 3) & ~3u);
    if ((unsigned long long)start * 8 + (len_b - 80) > max_bytes) return 0;
    memset(out, 0, max_bytes);
    memcpy(out, b, 80);
    /* A's last bundle whole: its length ends at the instruction that ends it, and the bundle's later words, zeros
       here, would read as an unknown instruction (each of a bundle's three is decoded) */
    memcpy(out + 80, a + 80, (size_t)start * 8 - 80);
    memcpy(out + (size_t)start * 8, b + 80, len_b - 80);
    for (i = 11; i < start; i++) { /* the program stays guest bytes: its words little-endian */
        u64 w = snx_ld_le64(out + (size_t)i * 8);
        const Op* op;
        if (!IDX_OK(i)) continue;
        /* a zero after A's end, its padding, would end the merged program there (shader_hash): a NOP instead */
        if (!w) snx_st_le64(out + (size_t)i * 8, w = 0x50B0000000070F00ull);
        op = decode(w);
        if (!op || op->id != O_EXIT) continue;
        /* BRA (opcode 0xE24) to B's first instruction, keeping the EXIT's predicate (16-19) and condition (0-4) */
        snx_st_le64(out + (size_t)i * 8, 0xE24ull << 52 | (u64)((start - i) * 8 & 0xFFFFFF) << 20 | (w & 0xF001Full));
    }
    return 1;
}
