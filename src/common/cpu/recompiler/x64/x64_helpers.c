/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the x86-64 back end's code calls (tools/sailgen/x64_a64.h) rather than writes inline: the exclusive monitor
   (cpu.h's ldx, stx, ldxp, stxp), the LSE atomics, CRC32 and CRC32C, DC ZVA. The LLVM back end builds its own
   helpers into each module as IR (ir_helpers.c); these are C, in the runtime library every module links (rtlib.c
   includes this), and in the x86-64 back end's test directly. Each takes the state first, as its code has it in RBX */
#include "common/cpu/aarch64/cpu.h"
#include <stdatomic.h>
#include <string.h>


u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr);
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr);
void snx_x64_fp_vec(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc);
void snx_x64_crypto(CPU* c, void* out, const void* x, const void* y, const void* z, u32 op);
extern const u32 snx_x64_crc32_table[256];

/* ---- floating point: cpu.h's helpers, which LLVM's slow paths call, with the state ----
   Each takes the guest's FPCR as the code read it (fpcr, bits 0-31), ORs the FPSR flags the operation raised into
   CPU.fpsr and gives the value. Bit 32 of fpcr says the instruction's Sail doesn't run the operation here (gen.c's
   run-time path: FCCMP's compare where the condition fails): then nothing happens, and the value is 0. The rest of
   the operation's parameters are constants of the instruction, packed into desc: width bits 0-7, the rest per
   helper. A rounding is rm + 1 (FPRounding's 0-4), 0 for FPCR's mode */
static inline int snx_x64_rm(u32 desc_rm, u64 fpcr) { return desc_rm ? (int)desc_rm - 1 : (int)((fpcr >> 22) & 3); }
static inline u64 snx_x64_fp_done(CPU* c, SnxFpReply r) {
    c->fpsr |= r.flags;
    return r.value;
}
/* desc: width, op << 8 */
u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_binary(a, b, (int)(desc & 0xFF), (int)(desc >> 8), (u32)fpcr, c->fpsr));
}
/* add + a * b; desc: width */
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_fma(add, a, b, (int)(desc & 0xFF), (u32)fpcr));
}
/* desc: width, op << 8 */
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_unary(a, (int)(desc & 0xFF), (int)(desc >> 8), (u32)fpcr));
}
/* desc: width, minimum << 8, numeric << 9 */
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_minmax(a, b, (int)(desc & 0xFF), (int)(desc >> 8 & 1), (int)(desc >> 9 & 1),
                                                 (u32)fpcr));
}
/* the NZCV nibble; desc: width, signal << 8 */
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_compare(a, b, (int)(desc & 0xFF), (int)(desc >> 8 & 1), (u32)fpcr));
}
/* desc: from, to << 8, rm + 1 << 16 */
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_convert(a, (int)(desc & 0xFF), (int)(desc >> 8 & 0xFF),
                                             snx_x64_rm(desc >> 16 & 7, fpcr), (u32)fpcr));
}
/* FRINT*; desc: width, rm + 1 << 8, exact << 12 */
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_round(a, (int)(desc & 0xFF), snx_x64_rm(desc >> 8 & 7, fpcr),
                                           (int)(desc >> 12 & 1), (u32)fpcr));
}
/* FRINT32*, FRINT64*; desc: width, rm + 1 << 8, intsize << 16 */
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_round_int(a, (int)(desc & 0xFF), snx_x64_rm(desc >> 8 & 7, fpcr),
                                               (int)(desc >> 16 & 0xFF), (u32)fpcr));
}
/* desc: fp width, int width << 8, uns << 16, fbits << 17 (7 bits), rm + 1 << 24 */
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_to_int(a, (int)(desc & 0xFF), (int)(desc >> 8 & 0xFF), (int)(desc >> 16 & 1),
                                            (int)(desc >> 17 & 0x7F), snx_x64_rm(desc >> 24 & 7, fpcr), (u32)fpcr));
}
/* desc: int width, fp width << 8, uns << 16, fbits << 17 (7 bits) */
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_from_int(a, (int)(desc & 0xFF), (int)(desc >> 8 & 0xFF), (int)(desc >> 16 & 1),
                                              (int)(desc >> 17 & 0x7F), (u32)fpcr));
}
/* FJCVTZS: the W result, Z in bit 32 */
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_jcvt(a, (u32)fpcr));
}
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_bfcvt(a, (u32)fpcr));
}

/* a vector's lanes, the cold path of x64_vec.c's: desc's kind (bits 0-3) 0 to 3 are snx_fp_host_lanes' (its desc as it
   takes it); the others lane by lane through the helpers LLVM's lanes call (ir_vec_fp.c's map), of bytes (bits 4-7)
   and lanes (8-11): 4 a compare, cmp (12-13: 0 EQ, 1 GE, 2 GT) of the magnitudes where abs (16), all ones where it
   holds; 5 a conversion to the width of bytes (12-15); 6 to integers of the lanes' width, uns (12), fbits (16-22),
   rm + 1 (24-26, 0 FPCR's); 7 from integers, uns (12), fbits (16-22); 8 FRINT, rm + 1 (12-14), exact (16) */
static inline u64 snx_x64_lane_get(const void* p, unsigned i, unsigned bytes) {
    u64 v = 0;
    memcpy(&v, (const char*)p + i * bytes, bytes);
    return v;
}
static inline void snx_x64_lane_put(void* p, unsigned i, unsigned bytes, u64 v) { memcpy((char*)p + i * bytes, &v, bytes); }
void snx_x64_fp_vec(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc) {
    unsigned kind = desc & 15, bytes = desc >> 4 & 15, lanes = desc >> 8 & 15, i, w = bytes * 8;
    unsigned char r[16];
    if (kind <= 3) {
        c->fpsr = snx_fp_host_lanes(out, x, y, z, desc, c->fpcr, c->fpsr);
        return;
    }
    memset(r, 0, sizeof r);
    for (i = 0; i < lanes; ++i) {
        u64 a = snx_x64_lane_get(x, i, bytes), b = snx_x64_lane_get(y, i, bytes);
        SnxFpReply rep;
        switch (kind) {
        case 4: {
            unsigned cmp = desc >> 12 & 3;
            u64 nz;
            if (desc >> 16 & 1) {
                a &= ~(1ull << (w - 1));
                b &= ~(1ull << (w - 1));
            }
            rep = snx_fp_compare(a, b, (int)w, cmp != 0, c->fpcr);
            nz = rep.value;
            rep.value = (cmp == 0 ? nz == 6 : cmp == 1 ? (nz & 0xB) == 2 : nz == 2) ? ~0ull : 0;
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        }
        case 5: {
            unsigned to = desc >> 12 & 15;
            rep = snx_fp_convert(a, (int)w, (int)to * 8, (int)((c->fpcr >> 22) & 3), c->fpcr);
            snx_x64_lane_put(r, i, to, rep.value);
            break;
        }
        case 6: {
            unsigned rm1 = desc >> 24 & 7;
            rep = snx_fp_to_int(a, (int)w, (int)w, (int)(desc >> 12 & 1), (int)(desc >> 16 & 127),
                                rm1 ? (int)rm1 - 1 : (int)((c->fpcr >> 22) & 3), c->fpcr);
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        }
        case 7:
            rep = snx_fp_from_int(a, (int)w, (int)w, (int)(desc >> 12 & 1), (int)(desc >> 16 & 127), c->fpcr);
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        default: {
            unsigned rm1 = desc >> 12 & 7;
            rep = snx_fp_round(a, (int)w, rm1 ? (int)rm1 - 1 : (int)((c->fpcr >> 22) & 3), (int)(desc >> 16 & 1),
                               c->fpcr);
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        }
        }
        c->fpsr |= rep.flags;
        (void)b;
    }
    memcpy(out, r, 16);
}

/* the cryptographic instructions where the host has none for them: out from Vd's, Vn's and Vm's values (x, y, z); op
   v_sha1's or v_sha256's, or from 0x20 AESE, AESD (x with the key y), AESMC, AESIMC (of x) */
void snx_x64_crypto(CPU* c, void* out, const void* x, const void* y, const void* z, u32 op) {
    CPU s; /* the three registers alone */
    (void)c;
    memcpy(&s.v[0], x, 16);
    memcpy(&s.v[1], y, 16);
    memcpy(&s.v[2], z, 16);
    if (op >= 0x22)
        v_aesmc(&s.v[0], &s.v[0], (int)op - 0x22);
    else if (op >= 0x20)
        v_aes(&s.v[0], &s.v[1], (int)op - 0x20);
    else if (op == 0x12 || (op >= 4 && op <= 6))
        v_sha256(&s, op, 0, 1, 2);
    else
        v_sha1(&s, op, 0, 1, 2);
    memcpy(out, &s.v[0], 16);
}

/* CRC32's (not CRC32C's) reflected table of the bytes: the x86-64 back end's CRC32 a byte at a time (x64_a64.h's
   SGX_M_CRC), RIP-relative from the code */
const u32 snx_x64_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

/* ---- for the back ends without inline code of their own for these (risc_a64.h's) ---- */

/* CRC32 or (desc bit 4) CRC32C of the low (desc & 15) bytes of value into acc, a bit at a time (the reflected
   polynomials, no inversion: the instructions' own) */
u64 snx_x64_crc32(u64 acc, u64 value, u32 desc) {
    u32 crc = (u32)acc, poly = desc & 16 ? 0x82F63B78u : 0xEDB88320u;
    unsigned k, bit;
    for (k = 0; k < (desc & 15); ++k) {
        crc ^= (u8)(value >> (8 * k));
        for (bit = 0; bit < 8; ++bit) crc = crc & 1 ? (crc >> 1) ^ poly : crc >> 1;
    }
    return crc;
}
/* STXP of two doublewords at a (cpu.h's stxp: the reservation's pair compared and swapped by snx_cas128) */
u64 snx_x64_stxp(CPU* c, u64 a, u64 v1, u64 v2) {
    u8* M = c->mem;
    return stxp(c, M, a, 3, v1, v2);
}
/* CASP of doublewords at a: the old pair (low, high), the desired stored where it was the expected. The pair is read
   by snx_cas128 (a compare-and-swap of what was read with itself), so the old value is one atomic snapshot. The low one
   is in the first result register on either byte order, where an __int128's is in the second of a big-endian one */
typedef struct {
    u64 lo, hi;
} SnxX64Pair;
SnxX64Pair snx_x64_casp(CPU* c, u64 a, u64 e0, u64 e1, u64 d0, u64 d1) {
    u8* M = c->mem;
    u64* p = (u64*)MP(a);
    for (;;) {
        u64 r0 = atomic_load_explicit((_Atomic u64*)p, memory_order_relaxed);
        u64 r1 = atomic_load_explicit((_Atomic u64*)(p + 1), memory_order_relaxed);
        u64 o0 = SNX_GUEST64(r0), o1 = SNX_GUEST64(r1);
        int match = o0 == e0 && o1 == e1;
        if (snx_cas128(c, p, o0, o1, match ? d0 : o0, match ? d1 : o1)) /* its values the guest's */
            return (SnxX64Pair){o0, o1};
    }
}
