/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hook table (aot/ctr_hook.h) and the hooks on the SDK's IPC stubs.

   An SDK IPC stub is a small leaf function: it reads the TLS pointer (MRC p15, c13, c0, 3), writes its command's
   header and arguments into the command buffer at TLS + 0x80, loads the session's handle into r0, makes
   svcSendSyncRequest (SVC 0x32), and then copies the reply's words from the buffer to its out parameters and returns
   the result. Each is decoded once, when the hooks are found: an instruction it uses outside a small set of ARM ones
   (data processing with an immediate shift, single, halfword, doubleword and multiple loads and stores, forward
   branches, BX LR) leaves the stub to the game's code, and so does a register that holds a TLS address escaping into
   anything but the buffer's addressing. Which registers hold TLS addresses, and at which offsets, is the decode's: the
   hook runs the stub's instructions on the CPU's registers with each access to the buffer going to 64 words of its
   own, and the SVC sending them to the session's service (snx_ctr_ipc_exchange). The game's TLS is never read or
   written, and the kernel's SVC and the scheduling after it are skipped.

   A stub whose handle reaches srv: or a service the runtime has no model of (where a request can block the thread,
   whose reply the TLS buffer then takes later) declines that call before it changed anything but its stack's free
   space and registers it restores, and the game's code makes the request; so does one whose window of text a CRO's
   relocation rewrote (the dispatcher checks).

   The SDK's GX command enqueue (gxlow's: it copies a 0x20-byte command into the GPU right holder's queue in GSP's
   shared memory, counts it in with LDREX/STREX, and makes TriggerCmdReqQueue when the queue was empty) is found by
   its code, its two calls checked to be the GSP handle's getter and a TriggerCmdReqQueue stub. Its hook runs the
   command there and then (services/gsp.c snx_ctr_gsp_run_queued), leaving the queue as the trigger would: the copy,
   the count's atomics and the request are skipped. A queue with commands pending, or one that isn't the right
   holder's, is the game's code's. */
#include "3ds/aot/ctr_hook.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/gsp.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STUB_OPS 64        /* a stub's instructions at most, from its entry to its last return */
#define TLS_BUFFER  0x80   /* the command buffer's offset in the TLS page */
#define TLS_END     0x180  /* its end: the static buffer descriptors past it stay the game's TLS */
#define TLS_STATIC_END 0x200 /* their end: the services read them there (a stub's are saved, set and restored) */

enum {
    OP_DP,     /* data processing: a = opcode, s; operand: imm (with carry) or rm shifted by an immediate */
    OP_MOVW,   /* rd = imm; OP_MOVT: rd's top half = imm */
    OP_MOVT,
    OP_MUL,    /* rd = rm * rs (+ rn when a) */
    OP_CONST,  /* rd = imm: a load from the text's literal pool, read when the stub was decoded */
    OP_TLS,    /* rd = TLS: the MRC that reads TPIDRURO */
    OP_LOAD,   /* single, halfword, signed or doubleword load from the guest (size 1, 2, 4 or 8; s: signed); a = 1 for a
                  VFP register's (rd its S or D register, by the size) */
    OP_STORE,
    OP_BUF_LOAD,  /* the same, from the command buffer at the fixed offset imm (bytes from TLS + 0x80) */
    OP_BUF_STORE,
    OP_LDM,    /* multiple: list in imm, P/U/W in a */
    OP_STM,
    OP_BUF_LDM, /* multiple from or to the buffer at the fixed offset imm2 */
    OP_BUF_STM,
    OP_B,      /* to the op at index target */
    OP_BX_LR,  /* return */
    OP_SVC,    /* svcSendSyncRequest with the buffer */
    OP_NOP
};
/* register shift types */
enum { LSL, LSR, ASR, ROR };
typedef struct {
    uint8_t kind, cond, a, s;
    uint8_t rd, rn, rm, rs; /* rd also the transfer register, rn the base */
    uint8_t shift, amount, imm_carry, has_imm_carry;
    uint8_t pre, up, writeback, reg_offset; /* addressing: P, U, W, and an index register (rm shifted) */
    uint32_t imm, imm2;
    uint16_t buf;     /* a buffer access's offset from the buffer's start */
    uint32_t address; /* the instruction's, for a read of PC (its address + 8) */
    uint16_t target;  /* OP_B's */
} Op;
enum { HOOK_STUB, HOOK_GX_ENQUEUE };
typedef struct SnxCtrHook {
    uint32_t entry;
    uint8_t kind;
    bool static_buffers; /* it writes the TLS's static buffer descriptors before the SVC: a decline puts them back */
    uint32_t header; /* the stub's command header, the first constant stored at the buffer's start, 0 unknown */
    uint64_t calls, declined;
    uint16_t count;
    Op ops[STUB_OPS];
} SnxCtrHook;
struct SnxCtrHooks {
    uint32_t text, words;   /* the static text's address and its size in words */
    uint32_t* bits;         /* a bit per word of the text: a hook's entry */
    SnxCtrHook* hooks;      /* sorted by entry */
    size_t count;
};

/* ---- the table ---- */
const SnxCtrHook* snx_ctr_hook_at(const SnxCtrHooks* h, uint32_t pc) {
    uint32_t word = (pc - h->text) >> 2;
    if (pc < h->text || word >= h->words || !(h->bits[word >> 5] >> (word & 31) & 1)) return NULL;
    size_t low = 0, high = h->count;
    while (low < high) {
        size_t mid = (low + high) / 2;
        if (h->hooks[mid].entry < pc)
            low = mid + 1;
        else
            high = mid;
    }
    return low < h->count && h->hooks[low].entry == pc ? &h->hooks[low] : NULL;
}
void snx_ctr_hooks_free(SnxCtrHooks* h) {
    if (!h) return;
    free(h->bits);
    free(h->hooks);
    free(h);
}

/* ---- decoding a stub ---- */
typedef struct {
    int16_t tls[16]; /* each register's offset from the TLS pointer when it holds one, else NO_TLS */
} Tags;
#define NO_TLS INT16_MIN
static bool tagged(const Tags* t, unsigned r) { return t->tls[r] != NO_TLS; }
/* whether bytes [offset, offset + size) from TLS are inside the command buffer */
static bool in_buffer(int32_t offset, uint32_t size) {
    return offset >= TLS_BUFFER && offset + (int32_t)size <= TLS_END;
}
/* whether they are among the static buffer descriptors, which stay the game's TLS */
static bool in_static(int32_t offset, uint32_t size) {
    return offset >= TLS_END && offset + (int32_t)size <= TLS_STATIC_END;
}
static uint32_t ror32(uint32_t v, unsigned n) { return n ? v >> n | v << (32 - n) : v; }
static bool same_tags(const Tags* a, const Tags* b) { return !memcmp(a, b, sizeof *a); }
typedef enum { FAIL, NEXT, END } Step; /* an instruction the hook can't take, the next one, a path's end */
typedef struct {
    const unsigned char* text;
    uint32_t base, bytes;
    SnxCtrHook* hook;
    Tags tags;
    bool svc_seen, pending[STUB_OPS];
    Tags pending_tags[STUB_OPS];
} Decode;
static uint32_t text_word(const Decode* d, uint32_t address) {
    return snx_ld_le32(d->text + (address - d->base));
}
/* a register read by an instruction other than as a buffer address: a TLS address escaping is refused */
static bool plain(const Decode* d, unsigned r) { return !tagged(&d->tags, r); }
/* r is written with something not a TLS address */
static void untag(Decode* d, unsigned r) { d->tags.tls[r] = NO_TLS; }
static Step data_processing(Decode* d, Op* op, uint32_t w) {
    unsigned opcode = w >> 21 & 15, rn = w >> 16 & 15, rd = w >> 12 & 15;
    bool imm = w >> 25 & 1, s = w >> 20 & 1;
    op->kind = OP_DP;
    op->a = (uint8_t)opcode;
    op->s = s;
    op->rd = (uint8_t)rd;
    op->rn = (uint8_t)rn;
    if (rd == 15) return FAIL;
    if (imm) {
        unsigned rotate = (w >> 8 & 15) * 2;
        op->imm = ror32(w & 255, rotate);
        op->has_imm_carry = rotate != 0;
        op->imm_carry = (uint8_t)(op->imm >> 31);
        op->reg_offset = 0;
    } else {
        if (w & 0x10) return FAIL; /* a register-specified shift */
        op->reg_offset = 1;
        op->rm = w & 15;
        op->shift = w >> 5 & 3;
        op->amount = w >> 7 & 31;
    }
    bool compare = opcode >= 8 && opcode <= 11, single = opcode == 13 || opcode == 15; /* MOV, MVN: no Rn */
    /* a TLS address moves on through MOV and through ADD or SUB of an immediate; anything else may not read one */
    if (opcode == 13 && !imm && !op->amount && op->shift == LSL && tagged(&d->tags, op->rm)) {
        if (op->cond != 14 || s) return FAIL;
        d->tags.tls[rd] = d->tags.tls[op->rm];
        return NEXT;
    }
    if ((opcode == 4 || opcode == 2) && imm && tagged(&d->tags, rn)) {
        int32_t offset = d->tags.tls[rn] + (opcode == 4 ? (int32_t)op->imm : -(int32_t)op->imm);
        if (op->cond != 14 || s || op->imm > 0x1000 || offset < -0x1000 || offset > 0x1000) return FAIL;
        d->tags.tls[rd] = (int16_t)offset;
        return NEXT;
    }
    if ((!single && !plain(d, rn)) || (!imm && !plain(d, op->rm))) return FAIL;
    if (!compare) {
        if (op->cond != 14 && tagged(&d->tags, rd)) return FAIL;
        untag(d, rd);
    }
    return NEXT;
}
/* single and halfword/doubleword transfers: kind and size already set */
static Step transfer(Decode* d, Op* op, uint32_t w, bool load, uint32_t size, bool register_offset) {
    unsigned rn = w >> 16 & 15, rt = w >> 12 & 15;
    op->rd = (uint8_t)rt;
    op->rn = (uint8_t)rn;
    op->pre = w >> 24 & 1;
    op->up = w >> 23 & 1;
    op->writeback = (uint8_t)(!op->pre || (w >> 21 & 1));
    op->reg_offset = register_offset;
    op->imm2 = size;
    if (!op->pre && (w >> 21 & 1)) return FAIL; /* LDRT, STRT */
    if (rt == 15 || (size == 8 && (rt & 1 || rt == 14))) return FAIL;
    if (op->writeback && (rn == 15 || rn == rt || (size == 8 && rn == rt + 1))) return FAIL;
    if (register_offset && (op->rm == 15 || !plain(d, op->rm))) return FAIL;
    if (!load && (!plain(d, rt) || (size == 8 && !plain(d, rt + 1)))) return FAIL;
    if (rn == 15) { /* a literal: the text's, read now (a rewritten window's hooks decline) */
        if (!load || register_offset || size != 4) return FAIL;
        uint32_t at = op->address + 8 + (op->up ? op->imm : -op->imm);
        if (at < d->base || at + 4 > d->base + d->bytes || (at & 3)) return FAIL;
        if (op->cond != 14 && tagged(&d->tags, rt)) return FAIL;
        op->kind = OP_CONST;
        op->imm = text_word(d, at);
        untag(d, rt);
        return NEXT;
    }
    if (tagged(&d->tags, rn)) {
        if (register_offset) return FAIL;
        int32_t offset = d->tags.tls[rn], moved = offset + (op->up ? (int32_t)op->imm : -(int32_t)op->imm);
        int32_t at = op->pre ? moved : offset;
        if ((!in_buffer(at, size) && !in_static(at, size)) || (op->writeback && op->cond != 14)) return FAIL;
        if (in_buffer(at, size)) {
            op->kind = load ? OP_BUF_LOAD : OP_BUF_STORE;
            op->buf = (uint16_t)(at - TLS_BUFFER);
        } else { /* the guest's TLS, where the register points */
            op->kind = load ? OP_LOAD : OP_STORE;
            if (!load && !d->svc_seen) d->hook->static_buffers = true;
        }
        if (op->writeback) d->tags.tls[rn] = (int16_t)moved;
    } else {
        op->kind = load ? OP_LOAD : OP_STORE;
        /* before the SVC, the game's memory is written only on the stack, below the caller's frame: a declined call
           changed nothing the game reads again */
        if (!load && !d->svc_seen && rn != 13) return FAIL;
    }
    if (load) {
        if (op->cond != 14 && (tagged(&d->tags, rt) || (size == 8 && tagged(&d->tags, rt + 1)))) return FAIL;
        untag(d, rt);
        if (size == 8) untag(d, rt + 1);
    }
    return NEXT;
}
static Step multiple(Decode* d, Op* op, uint32_t w) {
    unsigned rn = w >> 16 & 15, list = w & 0xffff;
    bool load = w >> 20 & 1;
    op->rn = (uint8_t)rn;
    op->imm = list;
    op->pre = w >> 24 & 1;
    op->up = w >> 23 & 1;
    op->writeback = w >> 21 & 1;
    if ((w >> 22 & 1) || !list || rn == 15 || (op->writeback && (list >> rn & 1))) return FAIL;
    unsigned count = (unsigned)__builtin_popcount(list);
    if (tagged(&d->tags, rn)) {
        int32_t offset = d->tags.tls[rn], bytes = (int32_t)count * 4;
        int32_t low = op->up ? offset + (op->pre ? 4 : 0) : offset - bytes + (op->pre ? 0 : 4);
        bool buffer = in_buffer(low, (uint32_t)bytes);
        if ((!buffer && !in_static(low, (uint32_t)bytes)) || (list >> 15 & 1) || (op->writeback && op->cond != 14))
            return FAIL;
        if (buffer) {
            op->kind = load ? OP_BUF_LDM : OP_BUF_STM;
            op->buf = (uint16_t)(low - TLS_BUFFER);
        } else {
            op->kind = load ? OP_LDM : OP_STM;
            if (!load && !d->svc_seen) d->hook->static_buffers = true;
        }
        if (op->writeback) d->tags.tls[rn] = (int16_t)(op->up ? offset + bytes : offset - bytes);
    } else {
        op->kind = load ? OP_LDM : OP_STM;
        if (!load && !d->svc_seen && rn != 13) return FAIL;
    }
    for (unsigned r = 0; r < 16; ++r) {
        if (!(list >> r & 1)) continue;
        if (!load && !plain(d, r)) return FAIL;
        if (load) {
            if (op->cond != 14 && tagged(&d->tags, r)) return FAIL;
            untag(d, r);
        }
    }
    if (load && (list >> 15 & 1)) return op->cond == 14 ? END : NEXT;
    return NEXT;
}
static Step decode_one(Decode* d, Op* op, uint32_t w) {
    op->cond = (uint8_t)(w >> 28);
    if (op->cond == 15) return FAIL;
    if ((w & 0x0fffffff) == 0x0320f000) { /* NOP */
        op->kind = OP_NOP;
        return NEXT;
    }
    if ((w & 0x0fffffff) == 0x012fff1e) { /* BX LR */
        if (!plain(d, 14)) return FAIL;
        op->kind = OP_BX_LR;
        return op->cond == 14 ? END : NEXT;
    }
    if ((w & 0x0fff0fff) == 0x0e1d0f70) { /* MRC p15, 0, Rd, c13, c0, 3: TPIDRURO */
        unsigned rd = w >> 12 & 15;
        if (rd == 15 || op->cond != 14) return FAIL;
        op->kind = OP_TLS;
        op->rd = (uint8_t)rd;
        d->tags.tls[rd] = 0;
        return NEXT;
    }
    if ((w & 0x0ff00000) == 0x03000000 || (w & 0x0ff00000) == 0x03400000) { /* MOVW, MOVT */
        unsigned rd = w >> 12 & 15;
        if (rd == 15 || tagged(&d->tags, rd)) return FAIL;
        op->kind = (w & 0x00400000) ? OP_MOVT : OP_MOVW;
        op->rd = (uint8_t)rd;
        op->imm = (w >> 4 & 0xf000) | (w & 0xfff);
        return NEXT;
    }
    if ((w & 0x0f000000) == 0x0f000000) { /* SVC */
        if ((w & 0xffffff) != 0x32 || d->svc_seen || op->cond != 14 || !plain(d, 0)) return FAIL;
        op->kind = OP_SVC;
        d->svc_seen = true;
        untag(d, 0);
        return NEXT;
    }
    if ((w & 0x0f200e00) == 0x0d000a00) { /* VLDR, VSTR: single (coprocessor 10) or double (11), no writeback */
        bool dbl = w >> 8 & 1, load = w >> 20 & 1;
        unsigned rn = w >> 16 & 15, vd = w >> 12 & 15, bit = w >> 22 & 1;
        op->a = 1;
        op->rn = (uint8_t)rn;
        op->rd = (uint8_t)(dbl ? bit << 4 | vd : vd << 1 | bit);
        op->pre = 1;
        op->up = w >> 23 & 1;
        op->imm = (w & 255) * 4;
        op->imm2 = dbl ? 8 : 4;
        /* a decline restores the core registers only */
        if (rn == 15 || (load && !d->svc_seen)) return FAIL;
        if (tagged(&d->tags, rn)) {
            int32_t at = d->tags.tls[rn] + (op->up ? (int32_t)op->imm : -(int32_t)op->imm);
            if (!in_buffer(at, op->imm2)) return FAIL;
            op->kind = load ? OP_BUF_LOAD : OP_BUF_STORE;
            op->buf = (uint16_t)(at - TLS_BUFFER);
            return NEXT;
        }
        if (!load && !d->svc_seen && rn != 13) return FAIL;
        op->kind = load ? OP_LOAD : OP_STORE;
        return NEXT;
    }
    if ((w & 0x0e000000) == 0x0a000000) { /* B, BL */
        if (w & 0x01000000) return FAIL;
        int32_t offset = (int32_t)(w << 8) >> 6;
        uint32_t target = op->address + 8 + (uint32_t)offset;
        uint32_t entry = d->hook->entry;
        if (target <= op->address || ((target - entry) >> 2) >= STUB_OPS) return FAIL; /* forward, inside the stub */
        unsigned at = (target - entry) >> 2;
        op->kind = OP_B;
        op->target = (uint16_t)at;
        if (d->pending[at] && !same_tags(&d->pending_tags[at], &d->tags)) return FAIL;
        d->pending[at] = true;
        d->pending_tags[at] = d->tags;
        return op->cond == 14 ? END : NEXT;
    }
    if ((w & 0x0fc000f0) == 0x00000090) { /* MUL, MLA */
        op->kind = OP_MUL;
        op->rd = w >> 16 & 15;
        op->rn = w >> 12 & 15;
        op->rs = w >> 8 & 15;
        op->rm = w & 15;
        op->a = w >> 21 & 1;
        if ((w >> 20 & 1) || op->rd == 15 || !plain(d, op->rm) || !plain(d, op->rs) || (op->a && !plain(d, op->rn)))
            return FAIL;
        if (op->cond != 14 && tagged(&d->tags, op->rd)) return FAIL;
        untag(d, op->rd);
        return NEXT;
    }
    if ((w & 0x0e000090) == 0x00000090 && (w & 0x60)) { /* halfword, signed and doubleword transfers */
        unsigned sh = w >> 5 & 3;
        bool l = w >> 20 & 1, immediate = w >> 22 & 1;
        op->imm = immediate ? ((w >> 4 & 0xf0) | (w & 15)) : 0;
        op->rm = w & 15;
        op->shift = LSL;
        op->amount = 0;
        if (l) {
            op->s = sh != 1;
            return transfer(d, op, w, true, sh == 2 ? 1 : 2, !immediate);
        }
        if (sh == 1) return transfer(d, op, w, false, 2, !immediate);
        return transfer(d, op, w, sh == 2, 8, !immediate); /* LDRD (2), STRD (3) */
    }
    if ((w & 0x0c000000) == 0x00000000) { /* data processing */
        unsigned opcode = w >> 21 & 15;
        if (opcode >= 8 && opcode <= 11 && !(w >> 20 & 1)) return FAIL; /* MRS, MSR and the like */
        return data_processing(d, op, w);
    }
    if ((w & 0x0c000000) == 0x04000000) { /* LDR, STR, LDRB, STRB */
        bool reg = w >> 25 & 1;
        if (reg && (w & 0x10)) return FAIL; /* the media instructions */
        op->imm = reg ? 0 : (w & 0xfff);
        op->rm = w & 15;
        op->shift = w >> 5 & 3;
        op->amount = w >> 7 & 31;
        op->s = 0;
        return transfer(d, op, w, w >> 20 & 1, (w >> 22 & 1) ? 1 : 4, reg);
    }
    if ((w & 0x0e000000) == 0x08000000) return multiple(d, op, w); /* LDM, STM */
    return FAIL;
}
/* the stub at entry, decoded into hook: false when it is not one the hook can run, with *stop the instruction it
   can't take (the entry for a stub with no SVC or no end) */
static bool decode_stub(const unsigned char* text, uint32_t base, uint32_t bytes, uint32_t entry, SnxCtrHook* hook,
                        uint32_t* stop) {
    Decode d;
    memset(&d, 0, sizeof d);
    d.text = text;
    d.base = base;
    d.bytes = bytes;
    d.hook = hook;
    for (unsigned r = 0; r < 16; ++r) d.tags.tls[r] = NO_TLS;
    memset(hook, 0, sizeof *hook);
    hook->entry = entry;
    bool reachable = true;
    *stop = entry;
    for (unsigned i = 0; i < STUB_OPS; ++i) {
        if (d.pending[i]) {
            if (reachable && !same_tags(&d.pending_tags[i], &d.tags)) return false;
            d.tags = d.pending_tags[i];
            reachable = true;
        }
        if (!reachable) {
            bool later = false;
            for (unsigned j = i; j < STUB_OPS; ++j) later |= d.pending[j];
            if (!later) {
                hook->count = (uint16_t)i;
                return d.svc_seen;
            }
            hook->ops[i].kind = OP_NOP; /* a gap no path reaches: a literal pool */
            hook->ops[i].cond = 14;
            continue;
        }
        uint32_t address = entry + i * 4;
        if (address + 4 > base + bytes) return false;
        Op* op = &hook->ops[i];
        op->address = address;
        uint32_t w = text_word(&d, address);
        Step step = decode_one(&d, op, w);
        if (step == FAIL) {
            *stop = address;
            return false;
        }
        /* the first constant word stored at the buffer's start is the header */
        if (!hook->header && op->kind == OP_BUF_STORE && op->buf == 0 && op->imm2 == 4) {
            for (unsigned j = i; j-- > 0;)
                if ((hook->ops[j].kind == OP_CONST || hook->ops[j].kind == OP_MOVW ||
                     (hook->ops[j].kind == OP_DP && hook->ops[j].a == 13 && !hook->ops[j].reg_offset)) &&
                    hook->ops[j].rd == op->rd) {
                    hook->header = hook->ops[j].imm;
                    break;
                }
        }
        if (step == END) reachable = false;
    }
    return false;
}

/* ---- running a stub ---- */
static bool condition(uint32_t cpsr, unsigned cond) {
    bool n = cpsr >> 31 & 1, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
    switch (cond) {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        case 13: return z || n != v;
        default: return true;
    }
}
static uint32_t reg(const SnxArmCPU* cpu, const Op* op, unsigned r) { return r == 15 ? op->address + 8 : cpu->r[r]; }
/* the shifter's operand and carry out */
static uint32_t shifted(const SnxArmCPU* cpu, const Op* op, unsigned* carry) {
    uint32_t v = reg(cpu, op, op->rm);
    unsigned n = op->amount, c = *carry;
    switch (op->shift) {
        case LSL:
            if (n) {
                c = v >> (32 - n) & 1;
                v <<= n;
            }
            break;
        case LSR:
            if (!n) n = 32;
            c = v >> (n - 1) & 1;
            v = n == 32 ? 0 : v >> n;
            break;
        case ASR:
            if (!n) n = 32;
            c = v >> (n - 1) & 1;
            v = n == 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n);
            break;
        default:
            if (!n) { /* RRX */
                unsigned out = v & 1;
                v = v >> 1 | (uint32_t)c << 31;
                c = out;
            } else {
                v = ror32(v, n);
                c = v >> 31;
            }
            break;
    }
    *carry = c;
    return v;
}
static void set_nz(SnxArmCPU* cpu, uint32_t v) {
    cpu->cpsr = (cpu->cpsr & 0x3fffffffu) | (v & 0x80000000u) | (v ? 0 : 0x40000000u);
}
static void set_cv(SnxArmCPU* cpu, unsigned c, unsigned v) {
    cpu->cpsr = (cpu->cpsr & ~0x30000000u) | (uint32_t)c << 29 | (uint32_t)v << 28;
}
static void run_dp(SnxArmCPU* cpu, const Op* op) {
    unsigned carry = cpu->cpsr >> 29 & 1, c_in = carry;
    uint32_t b = op->reg_offset ? shifted(cpu, op, &carry) : op->imm;
    if (!op->reg_offset && op->has_imm_carry) carry = op->imm_carry;
    uint32_t a = reg(cpu, op, op->rn), r = 0;
    uint64_t wide = 0;
    bool arithmetic = false, write = true;
    switch (op->a) {
        case 0: r = a & b; break;
        case 1: r = a ^ b; break;
        case 2: wide = (uint64_t)a + (uint32_t)~b + 1; arithmetic = true; break;  /* SUB */
        case 3: wide = (uint64_t)b + (uint32_t)~a + 1; arithmetic = true; break;  /* RSB */
        case 4: wide = (uint64_t)a + b; arithmetic = true; break;                 /* ADD */
        case 5: wide = (uint64_t)a + b + c_in; arithmetic = true; break;          /* ADC */
        case 6: wide = (uint64_t)a + (uint32_t)~b + c_in; arithmetic = true; break; /* SBC */
        case 7: wide = (uint64_t)b + (uint32_t)~a + c_in; arithmetic = true; break; /* RSC */
        case 8: r = a & b; write = false; break;                                  /* TST */
        case 9: r = a ^ b; write = false; break;                                  /* TEQ */
        case 10: wide = (uint64_t)a + (uint32_t)~b + 1; arithmetic = true; write = false; break; /* CMP */
        case 11: wide = (uint64_t)a + b; arithmetic = true; write = false; break;                 /* CMN */
        case 12: r = a | b; break;
        case 13: r = b; break;
        case 14: r = a & ~b; break;
        default: r = ~b; break;
    }
    if (arithmetic) {
        r = (uint32_t)wide;
        /* the second operand as added: ~b for the subtractions, a's for the reversed ones */
        uint32_t x = op->a == 3 || op->a == 7 ? b : a;
        uint32_t y = op->a == 2 || op->a == 6 || op->a == 10 ? ~b : op->a == 3 || op->a == 7 ? ~a : b;
        if (op->s) {
            set_nz(cpu, r);
            set_cv(cpu, (unsigned)(wide >> 32) & 1, ((x ^ r) & (y ^ r)) >> 31);
        }
    } else if (op->s) {
        set_nz(cpu, r);
        cpu->cpsr = (cpu->cpsr & ~0x20000000u) | (uint32_t)carry << 29;
    }
    if (write) cpu->r[op->rd] = r;
}
/* the guest's memory, little-endian: false (with the CPU's exit set) on a data abort */
static bool guest_read(SnxCtrProcess* p, SnxArmCPU* cpu, const Op* op, uint32_t address, unsigned char* bytes,
                       size_t size) {
    if (snx_ctr_read(snx_ctr_process_space(p), address, bytes, size) == SNX_CTR_MEM_OK) return true;
    cpu->r[15] = op->address;
    cpu->exit = SNX_ARM_DATA_ABORT;
    cpu->syndrome = address;
    return false;
}
static bool guest_write(SnxCtrProcess* p, SnxArmCPU* cpu, const Op* op, uint32_t address, const unsigned char* bytes,
                        size_t size) {
    if (snx_ctr_write(snx_ctr_process_space(p), address, bytes, size) == SNX_CTR_MEM_OK) return true;
    cpu->r[15] = op->address;
    cpu->exit = SNX_ARM_DATA_ABORT;
    cpu->syndrome = address;
    return false;
}
static uint32_t load_value(const unsigned char* b, uint32_t size, bool sign) {
    if (size == 1) return sign ? (uint32_t)(int32_t)(int8_t)b[0] : b[0];
    if (size == 2) return sign ? (uint32_t)(int32_t)(int16_t)snx_ld_le16(b) : snx_ld_le16(b);
    return snx_ld_le32(b);
}
static void store_value(unsigned char* b, uint32_t size, uint32_t v) {
    if (size == 1)
        b[0] = (unsigned char)v;
    else if (size == 2)
        snx_st_le16(b, (uint16_t)v);
    else
        snx_st_le32(b, v);
}
/* a load's bytes into its register or registers (a pair for LDRD, a VFP register for VLDR) */
static void put_value(SnxArmCPU* cpu, const Op* op, const unsigned char* b) {
    if (op->a) { /* S(2n) is Dn's low half, S(2n + 1) its high half */
        if (op->imm2 == 8)
            cpu->d[op->rd] = snx_ld_le64(b);
        else if (op->rd & 1)
            cpu->d[op->rd >> 1] = (cpu->d[op->rd >> 1] & 0xffffffffu) | (uint64_t)snx_ld_le32(b) << 32;
        else
            cpu->d[op->rd >> 1] = (cpu->d[op->rd >> 1] & ~(uint64_t)0xffffffffu) | snx_ld_le32(b);
        return;
    }
    cpu->r[op->rd] = load_value(b, op->imm2 == 8 ? 4 : op->imm2, op->s);
    if (op->imm2 == 8) cpu->r[op->rd + 1] = snx_ld_le32(b + 4);
}
/* a store's bytes from its register or registers */
static void get_value(const SnxArmCPU* cpu, const Op* op, unsigned char* b) {
    if (op->a) {
        if (op->imm2 == 8)
            snx_st_le64(b, cpu->d[op->rd]);
        else
            snx_st_le32(b, (uint32_t)(cpu->d[op->rd >> 1] >> (op->rd & 1 ? 32 : 0)));
        return;
    }
    store_value(b, op->imm2 == 8 ? 4 : op->imm2, cpu->r[op->rd]);
    if (op->imm2 == 8) snx_st_le32(b + 4, cpu->r[op->rd + 1]);
}
/* a transfer's address, and its base's writeback */
static uint32_t address_of(SnxArmCPU* cpu, const Op* op) {
    unsigned carry = cpu->cpsr >> 29 & 1;
    uint32_t offset = op->reg_offset ? shifted(cpu, op, &carry) : op->imm;
    uint32_t base = reg(cpu, op, op->rn), moved = op->up ? base + offset : base - offset;
    if (op->writeback) cpu->r[op->rn] = moved;
    return op->pre ? moved : base;
}
/* a branch to target: the CPU's state for a return there (interworking by its low bit) */
static void return_to(SnxArmCPU* cpu, uint32_t target) {
    if (target & 1)
        cpu->cpsr |= SNX_ARM_T;
    else
        cpu->cpsr &= ~SNX_ARM_T;
    cpu->r[15] = target & ~1u;
}
static SnxCtrHookResult run_stub(SnxCtrProcess* p, SnxArmCPU* cpu, SnxCtrHook* h) {
    uint32_t saved[16], saved_cpsr = cpu->cpsr;
    memcpy(saved, cpu->r, sizeof saved);
    /* the static buffer descriptors the stub sets before its SVC, as they were, for a decline to put back */
    unsigned char descriptors[TLS_STATIC_END - TLS_END];
    if (h->static_buffers &&
        snx_ctr_read_ram(snx_ctr_process_space(p), cpu->tpidruro + TLS_END, descriptors, sizeof descriptors) !=
            SNX_CTR_MEM_OK)
        return SNX_CTR_HOOK_DECLINE;
    /* the buffer, as little-endian bytes: the stub's byte and halfword accesses see the words' bytes */
    unsigned char buffer[SNX_CTR_IPC_WORDS * 4];
    memset(buffer, 0, sizeof buffer);
    unsigned i = 0;
    while (i < h->count) {
        const Op* op = &h->ops[i++];
        if (op->kind == OP_NOP || !condition(cpu->cpsr, op->cond)) continue;
        switch (op->kind) {
            case OP_DP: run_dp(cpu, op); break;
            case OP_MOVW: cpu->r[op->rd] = op->imm; break;
            case OP_MOVT: cpu->r[op->rd] = (cpu->r[op->rd] & 0xffff) | op->imm << 16; break;
            case OP_MUL: cpu->r[op->rd] = cpu->r[op->rm] * cpu->r[op->rs] + (op->a ? cpu->r[op->rn] : 0); break;
            case OP_CONST: cpu->r[op->rd] = op->imm; break;
            case OP_TLS: cpu->r[op->rd] = cpu->tpidruro; break;
            case OP_LOAD:
            case OP_STORE: {
                uint32_t address = address_of(cpu, op), size = op->imm2;
                unsigned char b[8];
                if (op->kind == OP_LOAD) {
                    if (!guest_read(p, cpu, op, address, b, size)) return SNX_CTR_HOOK_STOP;
                    put_value(cpu, op, b);
                } else {
                    get_value(cpu, op, b);
                    if (!guest_write(p, cpu, op, address, b, size)) return SNX_CTR_HOOK_STOP;
                }
                break;
            }
            case OP_BUF_LOAD:
            case OP_BUF_STORE: {
                unsigned char* b = buffer + op->buf;
                (void)address_of(cpu, op); /* the base's writeback: a TLS address the stub goes on using */
                if (op->kind == OP_BUF_LOAD)
                    put_value(cpu, op, b);
                else
                    get_value(cpu, op, b);
                break;
            }
            case OP_LDM:
            case OP_STM: {
                unsigned count = (unsigned)__builtin_popcount(op->imm);
                uint32_t base = cpu->r[op->rn];
                uint32_t low = op->up ? base + (op->pre ? 4 : 0) : base - count * 4 + (op->pre ? 0 : 4);
                unsigned char b[64];
                if (op->kind == OP_LDM) {
                    if (!guest_read(p, cpu, op, low, b, count * 4)) return SNX_CTR_HOOK_STOP;
                } else {
                    unsigned n = 0;
                    for (unsigned r = 0; r < 16; ++r)
                        if (op->imm >> r & 1) snx_st_le32(b + 4 * n++, reg(cpu, op, r));
                    if (!guest_write(p, cpu, op, low, b, count * 4)) return SNX_CTR_HOOK_STOP;
                }
                if (op->writeback) cpu->r[op->rn] = op->up ? base + count * 4 : base - count * 4;
                if (op->kind == OP_LDM) {
                    unsigned n = 0;
                    for (unsigned r = 0; r < 16; ++r)
                        if (op->imm >> r & 1) cpu->r[r] = snx_ld_le32(b + 4 * n++);
                    if (op->imm >> 15 & 1) {
                        return_to(cpu, cpu->r[15]);
                        return SNX_CTR_HOOK_DONE;
                    }
                }
                break;
            }
            case OP_BUF_LDM:
            case OP_BUF_STM: {
                unsigned count = (unsigned)__builtin_popcount(op->imm), n = 0;
                for (unsigned r = 0; r < 16; ++r) {
                    if (!(op->imm >> r & 1)) continue;
                    unsigned char* b = buffer + op->buf + 4 * n++;
                    if (op->kind == OP_BUF_LDM)
                        cpu->r[r] = snx_ld_le32(b);
                    else
                        snx_st_le32(b, cpu->r[r]);
                }
                if (op->writeback) cpu->r[op->rn] += op->up ? count * 4 : -(count * 4);
                break;
            }
            case OP_B: i = op->target; break;
            case OP_BX_LR: return_to(cpu, cpu->r[14]); return SNX_CTR_HOOK_DONE;
            case OP_SVC: {
                uint32_t service = snx_ctr_ipc_service(p, cpu->r[0]);
                /* srv: and the stand-ins can block the thread, with the reply left for its TLS: the game's code */
                if (service == SNX_CTR_SERVICE_NONE || service == SNX_CTR_SERVICE_SRV ||
                    service == SNX_CTR_SERVICE_SYSTEM) {
                    memcpy(cpu->r, saved, sizeof saved);
                    cpu->cpsr = saved_cpsr;
                    if (h->static_buffers)
                        (void)snx_ctr_write_ram(snx_ctr_process_space(p), cpu->tpidruro + TLS_END, descriptors,
                                                sizeof descriptors);
                    ++h->declined;
                    return SNX_CTR_HOOK_DECLINE;
                }
                uint32_t words[SNX_CTR_IPC_WORDS], result = 0;
                for (unsigned k = 0; k < SNX_CTR_IPC_WORDS; ++k) words[k] = snx_ld_le32(buffer + 4 * k);
                SnxCtrSvcResult state = snx_ctr_ipc_exchange(p, cpu->r[0], words, &result);
                if (state != SNX_CTR_SVC_RETURN) {
                    /* a request the services can't answer stops the run, as at the SVC */
                    cpu->r[15] = op->address;
                    cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
                    cpu->syndrome = op->address;
                    return SNX_CTR_HOOK_STOP;
                }
                cpu->r[0] = result;
                if (!result)
                    for (unsigned k = 0; k < SNX_CTR_IPC_WORDS; ++k) snx_st_le32(buffer + 4 * k, words[k]);
                ++h->calls;
                break;
            }
            default: break;
        }
    }
    /* a stub decoded to a return on every path: a fall off its end is the decoder's mistake */
    cpu->r[15] = h->entry;
    cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
    cpu->syndrome = h->entry;
    return SNX_CTR_HOOK_STOP;
}
/* the enqueue (r0: the queue's object, whose first word is the queue; r1: the command) */
static SnxCtrHookResult run_gx_enqueue(SnxCtrProcess* p, SnxArmCPU* cpu, SnxCtrHook* h) {
    unsigned char word[4], command[0x20];
    SnxCtrAddressSpace* space = snx_ctr_process_space(p);
    if (snx_ctr_read(space, cpu->r[0], word, 4) != SNX_CTR_MEM_OK || !snx_ld_le32(word) ||
        snx_ctr_read(space, cpu->r[1], command, sizeof command) != SNX_CTR_MEM_OK) {
        ++h->declined; /* the game's code answers a missing queue, or faults */
        return SNX_CTR_HOOK_DECLINE;
    }
    switch (snx_ctr_gsp_run_queued(p, snx_ld_le32(word), command)) {
        case SNX_CTR_GX_DONE: break;
        case SNX_CTR_GX_DECLINED: ++h->declined; return SNX_CTR_HOOK_DECLINE;
        default:
            cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
            cpu->syndrome = h->entry;
            return SNX_CTR_HOOK_STOP;
    }
    ++h->calls;
    cpu->r[0] = 0;
    return_to(cpu, cpu->r[14]);
    return SNX_CTR_HOOK_DONE;
}
SnxCtrHookResult snx_ctr_hook_run(SnxCtrProcess* p, SnxArmCPU* cpu, const SnxCtrHook* hook) {
    if (hook->kind == HOOK_GX_ENQUEUE) return run_gx_enqueue(p, cpu, (SnxCtrHook*)hook);
    return run_stub(p, cpu, (SnxCtrHook*)hook);
}

/* ---- finding the stubs ---- */
/* the start of the function holding the SVC at at: the PUSH (STMDB SP!) before it, past the previous return */
static uint32_t function_start(const unsigned char* text, uint32_t base, uint32_t at) {
    for (uint32_t x = at; x > base && at - x < 0x100;) {
        x -= 4;
        uint32_t w = snx_ld_le32(text + (x - base));
        if ((w & 0xffff0000u) == 0xe92d0000u) return x;                     /* PUSH */
        if (w == 0xe12fff1eu || (w & 0xffff8000u) == 0xe8bd8000u) return x + 4; /* BX LR, POP {..., PC} */
    }
    return 0;
}
static int by_entry(const void* a, const void* b) {
    uint32_t x = ((const SnxCtrHook*)a)->entry, y = ((const SnxCtrHook*)b)->entry;
    return x < y ? -1 : x > y;
}
/* The SDK's GX command enqueue, as the 3DS SDK of Mario Kart 7 builds it: its words, with the two BLs' offsets
   (GX_ENQUEUE_GETTER, GX_ENQUEUE_TRIGGER) the only ones that differ from game to game. The function ends at the POP
   before its three literals */
static const uint32_t gx_enqueue[] = {
    0xe92d47f0u, 0xe5902000u, 0xe3520000u, 0x059f00dcu, 0x0a000035u, 0xe5d23001u, 0xe353000fu, 0x259f00d0u,
    0x2a000031u, 0xe1923f9fu, 0xe1a02803u, 0xe59fe0c4u, 0xe203c0ffu, 0xe1a02c22u, 0xe08cc002u, 0xe3a08020u,
    0xe08e4c9eu, 0xe5919018u, 0xe1a0e1aeu, 0xe04ee20eu, 0xe08cc00eu, 0xe590e000u, 0xe088c28cu, 0xe08cc00eu,
    0xe89141f0u, 0xe591101cu, 0xe58c101cu, 0xe58c9018u, 0xe88c41f0u, 0xe3a01000u, 0xee071f9au, 0xe2821001u,
    0xe3c32cffu, 0xe1a01401u, 0xe2011cffu, 0xe1811002u, 0xe5903000u, 0xe1832f91u, 0xe3520000u, 0x0a000009u,
    0xe5901000u, 0xe1911f9fu, 0xe1a03801u, 0xe3c12cffu, 0xe1a01c23u, 0xe2811001u, 0xe1a01401u, 0xe2011cffu,
    0xe1811002u, 0xeafffff1u, 0xe1a00801u, 0xe1a00c20u, 0xe3500001u, 0x1a000003u, 0xeb000000u, 0xe320f000u,
    0xe320f000u, 0xeb000000u, 0xe3a00000u, 0xe8bd87f0u, 0xd8a02bf8u, 0xd8a02a02u, 0x88888889u};
#define GX_ENQUEUE_GETTER  54
#define GX_ENQUEUE_TRIGGER 57
static uint32_t bl_target(uint32_t address, uint32_t w) { return address + 8 + (uint32_t)((int32_t)(w << 8) >> 6); }
/* whether the enqueue's words are at offset, its getter returns a literal (LDR R0, [PC]; BX LR), and its trigger is a
   hooked stub of TriggerCmdReqQueue's header */
static bool is_gx_enqueue(const SnxCtrHooks* h, const unsigned char* text, uint32_t base, uint32_t bytes,
                          uint32_t offset) {
    const unsigned count = sizeof gx_enqueue / sizeof *gx_enqueue;
    if (offset + count * 4 > bytes) return false;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t w = snx_ld_le32(text + offset + 4 * i);
        uint32_t mask = i == GX_ENQUEUE_GETTER || i == GX_ENQUEUE_TRIGGER ? 0xff000000u : 0xffffffffu;
        if ((w & mask) != gx_enqueue[i]) return false;
    }
    uint32_t at = base + offset;
    uint32_t getter = bl_target(at + GX_ENQUEUE_GETTER * 4, snx_ld_le32(text + offset + GX_ENQUEUE_GETTER * 4));
    uint32_t trigger = bl_target(at + GX_ENQUEUE_TRIGGER * 4, snx_ld_le32(text + offset + GX_ENQUEUE_TRIGGER * 4));
    if (getter < base || getter + 8 > base + bytes || snx_ld_le32(text + (getter - base)) != 0xe59f0000u ||
        snx_ld_le32(text + (getter - base) + 4) != 0xe12fff1eu)
        return false;
    const SnxCtrHook* stub = snx_ctr_hook_at(h, trigger);
    return stub && stub->kind == HOOK_STUB && stub->header == 0x000c0000u;
}
SnxCtrHooks* snx_ctr_hooks_find(SnxCtrProcess* p, char* summary, size_t summary_size) {
    if (summary_size) summary[0] = 0;
    uint32_t base = p->segment[0].address, bytes = p->segment[0].bytes & ~3u;
    if (!bytes) return NULL;
    unsigned char* text = malloc(bytes);
    SnxCtrHooks* h = calloc(1, sizeof *h);
    if (!text || !h || snx_ctr_read(snx_ctr_process_space(p), base, text, bytes) != SNX_CTR_MEM_OK) goto none;
    h->text = base;
    h->words = bytes / 4;
    h->bits = calloc((h->words + 31) / 32, sizeof *h->bits);
    if (!h->bits) goto none;
    size_t stubs = 0, capacity = 0;
    for (uint32_t at = 0; at < bytes; at += 4) {
        if (snx_ld_le32(text + at) != 0xef000032u) continue;
        ++stubs;
        uint32_t entry = function_start(text, base, base + at);
        if (!entry || (h->count && h->hooks[h->count - 1].entry == entry)) continue;
        if (h->count == capacity) {
            capacity = capacity ? capacity * 2 : 256;
            SnxCtrHook* grown = realloc(h->hooks, capacity * sizeof *grown);
            if (!grown) goto none;
            h->hooks = grown;
        }
        uint32_t stop;
        if (!decode_stub(text, base, bytes, entry, &h->hooks[h->count], &stop)) {
            if (g_settings.ctr_ipc_trace)
                fprintf(stderr, "3ds hooks: the IPC stub at 0x%08" PRIx32 " is left to the SVC, at 0x%08" PRIx32
                        " (0x%08" PRIx32 ")\n", entry, stop, snx_ld_le32(text + (stop - base)));
            continue;
        }
        uint32_t word = (entry - base) / 4;
        h->bits[word >> 5] |= 1u << (word & 31);
        ++h->count;
    }
    qsort(h->hooks, h->count, sizeof *h->hooks, by_entry);
    /* the enqueues, found while the stubs alone are in the table, then added */
    size_t stubs_hooked = h->count, enqueues = 0;
    uint32_t found[8];
    for (uint32_t at = 0; at + 4 <= bytes && enqueues < sizeof found / sizeof *found; at += 4)
        if (snx_ld_le32(text + at) == gx_enqueue[0] && is_gx_enqueue(h, text, base, bytes, at)) found[enqueues++] = at;
    for (size_t k = 0; k < enqueues; ++k) {
        uint32_t at = found[k];
        if (h->count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            SnxCtrHook* grown = realloc(h->hooks, capacity * sizeof *grown);
            if (!grown) goto none;
            h->hooks = grown;
        }
        SnxCtrHook* hook = &h->hooks[h->count++];
        memset(hook, 0, sizeof *hook);
        hook->entry = base + at;
        hook->kind = HOOK_GX_ENQUEUE;
        h->bits[at / 4 >> 5] |= 1u << (at / 4 & 31);
    }
    if (enqueues) qsort(h->hooks, h->count, sizeof *h->hooks, by_entry);
    if (summary_size)
        snprintf(summary, summary_size, "%zu of the %zu IPC stubs in the code run without the SVC%s", stubs_hooked,
                 stubs, enqueues ? ", and the GX command queue's enqueue runs each command at once" : "");
    free(text);
    if (!h->count) {
        snx_ctr_hooks_free(h);
        return NULL;
    }
    return h;
none:
    free(text);
    snx_ctr_hooks_free(h);
    return NULL;
}
void snx_ctr_hooks_report(const SnxCtrHooks* h, char* line, size_t size) {
    uint64_t calls = 0, declined = 0, commands = 0, queued = 0;
    size_t used = 0;
    for (size_t i = 0; h && i < h->count; ++i) {
        if (h->hooks[i].kind == HOOK_GX_ENQUEUE) {
            commands += h->hooks[i].calls;
            queued += h->hooks[i].declined;
            continue;
        }
        calls += h->hooks[i].calls;
        declined += h->hooks[i].declined;
        used += h->hooks[i].calls != 0;
    }
    int n = snprintf(line, size, "hooks: %" PRIu64 " requests sent by %zu IPC stubs without the SVC, %" PRIu64
                     " left to it", calls, used, declined);
    if (commands + queued && n > 0 && (size_t)n < size)
        snprintf(line + n, size - (size_t)n, "; %" PRIu64 " GX commands run at their enqueue, %" PRIu64 " queued",
                 commands, queued);
}
