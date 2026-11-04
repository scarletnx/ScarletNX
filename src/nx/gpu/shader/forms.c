/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The forms of each instruction the translator takes, and what an instruction reads and writes: facts of
   one word, for the analyses and the emitters */
#include "nx/gpu/shader/translate.h"

u32 shader_global_atomic_opcode(u64 w, int reduction) {
    u32 size = reduction ? B(w, 20, 3) : B(w, 49, 3);
    u32 op = reduction ? B(w, 23, 3) : B(w, 52, 4);
    if (op == 10) op = 0; /* SAFEADD */
    if (size > 1 && size != 3) return 0;
    if (size == 3) return op == 8 ? 229 : op == 0 ? 0xffffffffu : 0;
    switch (op) {
        case 0: return 234;              /* IAdd */
        case 1: return size ? 236 : 237; /* SMin / UMin */
        case 2: return size ? 238 : 239; /* SMax / UMax */
        case 3:
        case 4: return 0xffffffffu; /* Inc / Dec use compare exchange. */
        case 5: return 240;         /* And */
        case 6: return 241;         /* Or */
        case 7: return 242;         /* Xor */
        case 8: return 229;         /* Exchange */
        default: return 0;
    }
}

u32 shader_shared_atomic_opcode(u64 w) {
    if (B(w, 28, 2) == 2 && B(w, 52, 4) == 8) return 0xfffffffeu; /* U64 exchange */
    if (B(w, 28, 2) > 1) return 0;
    switch (B(w, 52, 4)) {
        case 0: return 234;                     /* IAdd */
        case 1: return B(w, 28, 2) ? 236 : 237; /* SMin / UMin */
        case 2: return B(w, 28, 2) ? 238 : 239; /* SMax / UMax */
        case 3:
        case 4: return 0xffffffffu; /* Inc / Dec use compare exchange. */
        case 5: return 240;         /* And */
        case 6: return 241;         /* Or */
        case 7: return 242;         /* Xor */
        case 8: return 229;         /* Exchange */
        default: return 0;
    }
}

/* Instructions that only compute registers, predicates and flags from registers, constants and immediates: no memory,
   no texture or attribute, no lanes other than their own, no flow. Run with their guard false they change nothing a
   select of their writes keeps, so a guarded one needs no branch around it */
int shader_pure_op(int op) {
    switch (op) {
        case O_BFE_C:
        case O_BFE_I:
        case O_BFE_R:
        case O_BFI_CR:
        case O_BFI_I:
        case O_BFI_R:
        case O_BFI_RC:
        case O_CS2R:
        case O_CSET:
        case O_CSETP:
        case O_DADD_C:
        case O_DADD_I:
        case O_DADD_R:
        case O_DFMA_CR:
        case O_DFMA_I:
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DMNMX_C:
        case O_DMNMX_I:
        case O_DMNMX_R:
        case O_DMUL_C:
        case O_DMUL_I:
        case O_DMUL_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_DSETP_R:
        case O_DSET_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2F_R:
        case O_F2I_C:
        case O_F2I_I:
        case O_F2I_R:
        case O_FADD32I:
        case O_FADD_C:
        case O_FADD_I:
        case O_FADD_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_FCHK_R:
        case O_FCMP_CR:
        case O_FCMP_I:
        case O_FCMP_R:
        case O_FCMP_RC:
        case O_FFMA32I:
        case O_FFMA_CR:
        case O_FFMA_I:
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FLO_C:
        case O_FLO_I:
        case O_FLO_R:
        case O_FMNMX_C:
        case O_FMNMX_I:
        case O_FMNMX_R:
        case O_FMUL32I:
        case O_FMUL_C:
        case O_FMUL_I:
        case O_FMUL_R:
        case O_FSET_C:
        case O_FSET_I:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_FSETP_R:
        case O_FSET_R:
        case O_HADD2_32I:
        case O_HADD2_C:
        case O_HADD2_I:
        case O_HADD2_R:
        case O_HFMA2_32I:
        case O_HFMA2_CR:
        case O_HFMA2_I:
        case O_HFMA2_R:
        case O_HFMA2_RC:
        case O_HMUL2_32I:
        case O_HMUL2_C:
        case O_HMUL2_I:
        case O_HMUL2_R:
        case O_HSET2_C:
        case O_HSET2_I:
        case O_HSET2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_HSETP2_R:
        case O_I2F_C:
        case O_I2F_I:
        case O_I2F_R:
        case O_I2I_C:
        case O_I2I_I:
        case O_I2I_R:
        case O_IADD32I:
        case O_IADD3_C:
        case O_IADD3_I:
        case O_IADD3_R:
        case O_IADD_C:
        case O_IADD_I:
        case O_IADD_R:
        case O_ICMP_CR:
        case O_ICMP_I:
        case O_ICMP_R:
        case O_ICMP_RC:
        case O_IMAD32I:
        case O_IMAD_CR:
        case O_IMAD_I:
        case O_IMAD_R:
        case O_IMAD_RC:
        case O_IMNMX_C:
        case O_IMNMX_I:
        case O_IMNMX_R:
        case O_IMUL32I:
        case O_IMUL_C:
        case O_IMUL_I:
        case O_IMUL_R:
        case O_ISCADD32I:
        case O_ISCADD_C:
        case O_ISCADD_I:
        case O_ISCADD_R:
        case O_ISET_C:
        case O_ISET_I:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_ISETP_R:
        case O_ISET_R:
        case O_LEA_HC:
        case O_LEA_HR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_LEA_LR:
        case O_LOP32I:
        case O_LOP3_C:
        case O_LOP3_I:
        case O_LOP3_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP_R:
        case O_MOV32I:
        case O_MOV_C:
        case O_MOV_I:
        case O_MOV_R:
        case O_MUFU:
        case O_P2R_C:
        case O_P2R_I:
        case O_P2R_R:
        case O_POPC_C:
        case O_POPC_I:
        case O_POPC_R:
        case O_PRMT_CR:
        case O_PRMT_I:
        case O_PRMT_R:
        case O_PRMT_RC:
        case O_PSET:
        case O_PSETP:
        case O_RRO_C:
        case O_RRO_I:
        case O_RRO_R:
        case O_SEL_C:
        case O_SEL_I:
        case O_SEL_R:
        case O_SHF_LI:
        case O_SHF_LR:
        case O_SHF_RI:
        case O_SHF_RR:
        case O_SHL_C:
        case O_SHL_I:
        case O_SHL_R:
        case O_SHR_C:
        case O_SHR_I:
        case O_SHR_R:
        case O_VABSDIFF:
        case O_VADD:
        case O_VMAD:
        case O_VMNMX:
        case O_VSET:
        case O_VSETP:
        case O_VSHL:
        case O_VSHR:
        case O_XMAD_CR:
        case O_XMAD_I:
        case O_XMAD_R:
        case O_XMAD_RC: return 1;
        default: return 0;
    }
}

/* An FTZ float instruction whose result is flushed as it is written: no scale after the flush (FMUL), no FMZ
   select of an operand. Its destination is d alone */
int shader_ftz_writer(u64 w) {
    int id = decode(w)->id;
    if (id == O_FADD_R || id == O_FADD_C || id == O_FADD_I) return (int)B(w, 44, 1);
    if (id == O_FADD32I) return (int)B(w, 55, 1);
    if (id == O_FMUL_R || id == O_FMUL_C || id == O_FMUL_I) return (B(w, 44, 2) & 1) && !B(w, 41, 3);
    if (id == O_FMUL32I) return (int)(B(w, 53, 2) & 1);
    if (id == O_FFMA_R || id == O_FFMA_RC || id == O_FFMA_CR || id == O_FFMA_I || id == O_FFMA32I)
        return (int)(B(w, 53, 2) & 1);
    return 0;
}
/* whether w may write register r: its destinations taken wide, d to d+3 and the second field texture instructions
   write (bits 28 to 35) to it plus 3 */
int shader_may_write(u64 w, u32 r) {
    u32 d = B(w, 0, 8), e = B(w, 28, 8);
    return (d < 255 && r >= d && r - d < 4) || (e < 255 && r >= e && r - e < 4);
}

static int cc_valid(u32 code) {
    return code <= 14 || code == 15 || (code >= 16 && code <= 23) || code == 30 || code == 31;
}
int shader_flow_has_cc(int op) {
    return op == O_BRA || op == O_BRX || op == O_SYNC || op == O_BRK || op == O_CONT || op == O_EXIT || op == O_RET ||
           op == O_KIL;
}
int shader_helper_side_effect(int op) {
    return op == O_STG || op == O_STL || op == O_STS || op == O_ST || op == O_STP || op == O_RED || op == O_ATOM ||
           op == O_ATOM_CAS || op == O_ATOMS || op == O_ATOMS_CAS || op == O_SUST || op == O_SURED || op == O_SUATOM ||
           op == O_SUATOM_CAS;
}
int shader_gbuf_tag(int stage, u32 index, u32 offset) {
    /* The vertex system buffer also supplies a bare 64-bit output pointer at 0xb0. */
    if (stage == ST_VS && index == 0 && offset == 0xb0) return 0xb1;
    return index == 0 && shader_gbuf_sized(stage, offset) && !(offset & 15) ? (int)offset + 1 : 0;
}
/* A bare 64-bit address in a constant buffer of the program's own (1 to 17: cbuf 0 is NVN's, its descriptors and
   system values), 16-byte aligned as Eden requires of one (SH_GBUF_*). Taken only where neither an NVN descriptor
   (shader_gbuf_tag) nor a register already holding a base explains the access, and not for compute dispatches, which copy
   each buffer whole and would have no size for one. Without it such a graphics program wasn't translated, and its
   draws were skipped: one game's terrain, all of it */
int shader_gbuf_tag_any(int stage, u32 index, u32 offset) {
    return stage != ST_CS && index > 0 && index < 18 && offset < 0x10000 && !(offset & 15)
               ? (int)((index << 16) | offset) + 1
               : 0;
}
int shader_texs_outputs(u64 w, u32 regs[4], u32 comps[4]) {
    u32 da = B(w, 0, 8), db = B(w, 28, 8), sw = B(w, 50, 3);
    int n = 0;
    int i;
    if (db == 255) {
        static const int tab[8][2] = {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {0, 1}, {0, 3}, {1, 3}, {2, 3}};
        for (i = 0; i < 2; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = da == 255 ? 255 : da + (u32)n;
                comps[n++] = (u32)tab[sw][i];
            }
    } else {
        static const int tab[5][4] = {{0, 1, 2, -1}, {0, 1, 3, -1}, {0, 2, 3, -1}, {1, 2, 3, -1}, {0, 1, 2, 3}};
        if (sw > 4) sw = 4;
        for (i = 0; i < 4; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = n < 2 ? da + (u32)n : db + (u32)(n - 2);
                comps[n++] = (u32)tab[sw][i];
            }
    }
    return n;
}
/* The registers TEXS and TLDS write: each component its own register (bit 59), or two halves to a register, the
   first pair in Rd and the second in bits 28-35's register */
void shader_texs_mark_outputs(u64 w, u8 used[255]) {
    u32 regs[4], comps[4], d = B(w, 0, 8);
    int outputs = shader_texs_outputs(w, regs, comps);
    if (B(w, 59, 1)) {
        for (int k = 0; k < outputs; k++)
            if (regs[k] < 255) used[regs[k]] = 1;
        return;
    }
    if (outputs && d < 255) used[d] = 1;
    if (outputs > 2 && B(w, 28, 8) < 255) used[B(w, 28, 8)] = 1;
}

static u32 direct_mask_count(u32 mask) {
    u32 count = 0;
    for (; mask; mask &= mask - 1) count++;
    return count;
}
/* Whether the hardware writes reg when it runs w. Backward scans for a register's
   definition need the whole range: loads, 64-bit results and masked texture results
   write registers after the one in bits 0-7, and stores and predicate setters keep a
   source there instead. Forms the translator rejects are described too, so a scan
   never skips over a write. */
int shader_writes_reg(u64 w, int op, u32 reg) {
    u32 d = B(w, 0, 8), count = 1;
    if (reg == 255) return 0;
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_PSETP:
        case O_CSETP:
        case O_VSETP:
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_R2B:
        case O_STG:
        case O_STS:
        case O_STL:
        case O_ST:
        case O_STP:
        case O_RED:
        case O_AST:
        case O_SUST:
        case O_SURED:
        case O_BRA:
        case O_BRX:
        case O_SSY:
        case O_SYNC:
        case O_PBK:
        case O_BRK:
        case O_PCNT:
        case O_CONT:
        case O_EXIT:
        case O_KIL:
        case O_JMP:
        case O_CAL:
        case O_RET:
        case O_PRET:
        case O_PEXIT:
        case O_NOP:
        case O_BAR:
        case O_MEMBAR:
        case O_DEPBAR:
        case O_VOTE_VTG:
        case O_CCTL:
        case O_CCTLL:
        case O_BPT:
        case O_IDE:
        case O_SETCRSPTR: return 0;
        case O_TLD4S: {
            /* Two destination pairs, the second named in bits 28-35, or one packed register for each. */
            u32 second = B(w, 28, 8);
            if (B(w, 55, 1)) return d == reg || second == reg;
            return (d != 255 && reg - d < 2) || (second != 255 && reg - second < 2);
        }
        case O_TEXS:
        case O_TLDS: {
            u32 regs[4], comps[4];
            int outputs = shader_texs_outputs(w, regs, comps);
            int k;
            if (!B(w, 59, 1)) return outputs && (d == reg || (outputs > 2 && B(w, 28, 8) == reg));
            for (k = 0; k < outputs; k++)
                if (regs[k] == reg) return 1;
            return 0;
        }
        /* A mask of 0 writes nothing, as Eden and Ryujinx read it (NVN never emits one). */
        case O_TEX:
        case O_TEX_B:
        case O_TLD:
        case O_TLD_B:
        case O_TLD4:
        case O_TLD4_B:
        case O_TMML:
        case O_TMML_B:
        case O_TXD:
        case O_TXD_B:
        case O_TXQ:
        case O_TXQ_B: count = direct_mask_count(B(w, 31, 4)); break;
        case O_SULD:
            count = !B(w, 52, 1) ? direct_mask_count(B(w, 20, 4)) : B(w, 20, 3) == 5 ? 2 : B(w, 20, 3) >= 6 ? 4 : 1;
            break;
        case O_LDG:
        case O_LDS:
        case O_LDL: count = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1; break;
        case O_LD: count = B(w, 53, 3) == 5 ? 2 : B(w, 53, 3) >= 6 ? 4 : 1; break;
        case O_LDC: count = B(w, 48, 3) == 5 ? 2 : 1; break;
        case O_ALD: count = B(w, 47, 2) + 1; break;
        case O_ATOM: count = B(w, 49, 3) == 2 || B(w, 49, 3) == 5 ? 2 : 1; break;
        case O_ATOMS: count = B(w, 28, 2) >= 2 ? 2 : 1; break;
        case O_SUATOM: count = B(w, 51, 3) == 2 || B(w, 51, 3) == 3 || B(w, 51, 3) == 7 ? 2 : 1; break;
        case O_ATOM_CAS: count = B(w, 49, 1) ? 2 : 1; break;
        case O_ATOMS_CAS: count = B(w, 52, 4) == 5 ? 2 : 1; break;
        case O_SUATOM_CAS: count = 2; break;
        /* Mesa NAK treats CS2R as a 64-bit sysval load, so it writes both destination words. */
        case O_CS2R: count = 2; break;
        case O_DADD_R:
        case O_DADD_C:
        case O_DADD_I:
        case O_DMUL_R:
        case O_DMUL_C:
        case O_DMUL_I:
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DFMA_CR:
        case O_DFMA_I:
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: count = 2; break;
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I:
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I:
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I: count = B(w, 8, 2) == 3 ? 2 : 1; break;
        default: break;
    }
    return d != 255 && reg >= d && reg - d < count;
}
u32 shader_direct_pred_writes(u64 w, int op) {
    u32 p;
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_PSETP:
        case O_CSETP:
        case O_VSETP: return (1u << B(w, 3, 3) | 1u << B(w, 0, 3)) & 0x7f;
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_VOTE: return 0x7f;
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP3_R:
        case O_LEA_HR:
        case O_LEA_HC:
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_SHFL: p = B(w, 48, 3); break;
        default: p = shader_sparse_predicate(w, op); break;
    }
    return p < 7 ? 1u << p : 0;
}

u32 shader_next_instruction(u32 index, u32 limit) {
    do { index++; } while (index < limit && !IDX_OK(index));
    return index;
}

static int texture_sample_supported(u64 w, int bindless) {
    u32 type = B(w, 28, 3);
    u32 offset = B(w, bindless ? 36 : 54, 1);
    u32 mode = B(w, bindless ? 37 : 55, 3);
    /* Type 5 denotes a 3D array, which Vulkan cannot sample. LC is a packed 4.8 minimum LOD in the high half of
       the layer register (Mesa nak_nir_lower_tex.c, lower_tex_to_nak); this translator does not decode it, so refuse
       bit 58 (TEX.B bit 40). */
    return type != 5 && mode != 4 && mode != 5 && !(type == 4 && B(w, 50, 1)) && (!offset || type <= 4) &&
           !B(w, bindless ? 40 : 58, 1);
}

/* SHFL within a fragment program's 2x2 quad (the immediate mask 0x1c03: segments of 4 lanes, clamped to the last)
   from a lane known when translated (an immediate, or RZ): 1 for IDX, which reads lane index & 3, 2 for BFLY 1-3, the
   neighbour across (*lane its direction, 0 horizontal). The lanes a triangle's edge leaves out of a quad still run
   the program on the console, and SHFL reads them; a subgroup shuffle needn't (on RADV such a helper lane never
   computed the value, and the pixels reading it drew a line along the edge). A quad operation reads them */
int shader_shfl_quad(u64 w, u32* lane) {
    if ((!B(w, 28, 1) && B(w, 20, 8) != 255) || !B(w, 29, 1) || B(w, 34, 13) != 0x1c03) return 0;
    u32 index = B(w, 28, 1) ? B(w, 20, 5) : 0;
    if (lane) *lane = B(w, 30, 2) == 0 ? index & 3 : index - 1;
    if (B(w, 30, 2) == 0) return 1;
    return B(w, 30, 2) == 3 && index >= 1 && index <= 3 ? 2 : 0;
}

/* FSWZADD's lanes (bit L for quad lane L) whose two bits of the swizzle (28-35) are mode: lane L's are bits
   6 - 2L of it, the upper left lane's the top two, as nouveau's QUADOP lays them out (nv50_ir_lowering_gm107.cpp) */
u32 shader_fswzadd_lanes(u64 w, u32 mode) {
    u32 lanes = 0;
    for (u32 lane = 0; lane < 4; lane++)
        if ((B(w, 28, 8) >> (6 - 2 * lane) & 3) == mode) lanes |= 1u << lane;
    return lanes;
}

/* the comparisons that write two predicates (bits 3-5 and 0-2) and read a third (39-41, PSETP two more) */
int shader_predicate_comparison(int op) {
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_CSETP:
        case O_VSETP:
        case O_PSETP: return 1;
        default: return 0;
    }
}

/* whether the instruction writes predicate p: the comparisons' two results, AL2P's (bits 44-46), VOTE's (45-47),
   the carry or zero that SHFL, LOP, LOP3 and LEA give (48-50), FCHK's, and any R2P */
int shader_writes_predicate(u64 w, int op, u32 p) {
    if (shader_predicate_comparison(op)) return B(w, 3, 3) == p || B(w, 0, 3) == p;
    switch (op) {
        case O_AL2P: return B(w, 44, 3) == p;
        case O_VOTE: return B(w, 45, 3) == p;
        case O_SHFL:
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP3_R:
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI: return B(w, 48, 3) == p;
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I: return B(w, 3, 3) == p;
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I: return 1;
        default: return 0;
    }
}

/* whether the instruction reads predicate p, as the translator reads them (pred_read's callers): its guard, but the
   stack instructions' it takes no notice of, a guarded write of p (which keeps p where the guard fails), the
   operand at bits 39-41 of the comparisons, selects and min and max, PSET's and PSETP's other two, and P2R's set */
int shader_reads_predicate(u64 w, int op, u32 p) {
    int guarded = B(w, 16, 3) != 7 || B(w, 19, 1);
    if (B(w, 16, 3) == p && op != O_SSY && op != O_PBK && op != O_PCNT && op != O_PEXIT && op != O_CAL) return 1;
    if (guarded && shader_writes_predicate(w, op, p)) return 1;
    if (op == O_PSET || op == O_PSETP) return B(w, 12, 3) == p || B(w, 29, 3) == p || B(w, 39, 3) == p;
    if (shader_predicate_comparison(op)) return B(w, 39, 3) == p;
    switch (op) {
        case O_CSET:
        case O_VOTE:
        case O_SEL_R:
        case O_SEL_C:
        case O_SEL_I:
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I:
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I:
        case O_DSET_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_HSET2_R:
        case O_HSET2_C:
        case O_HSET2_I:
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I:
        case O_IMNMX_R:
        case O_IMNMX_C:
        case O_IMNMX_I:
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: return B(w, 39, 3) == p;
        case O_P2R_R:
        case O_P2R_C:
        case O_P2R_I: return !B(w, 40, 1);
        default: return 0;
    }
}

int shader_supported(u64 w, int op) {
    switch (op) {
        case O_EXIT: return cc_valid(B(w, 0, 5));
        case O_NOP:
        case O_DEPBAR:
        case O_MEMBAR:
        case O_VOTE_VTG: return 1;
        case O_VOTE: return B(w, 48, 2) < 3;
        case O_FLO_R:
        case O_FLO_C:
        case O_FLO_I: return !B(w, 47, 1);
        case O_POPC_R:
        case O_POPC_C:
        case O_POPC_I: return 1;
        case O_SHFL: return 1;
        case O_FSWZADD: return !B(w, 47, 1);
        case O_VMNMX: return !B(w, 47, 1);
        case O_VMAD:
            return (((w >> 48) & ~3ull) == 0x5f04 || ((w >> 48) & ~3ull) == 0x5f00) && !B(w, 47, 1) && !B(w, 51, 5);
        case O_VSETP: return 1;
        case O_RED: return shader_global_atomic_opcode(w, 1) != 0;
        case O_ATOM: return shader_global_atomic_opcode(w, 0) != 0;
        case O_ATOMS: return shader_shared_atomic_opcode(w) != 0;
        /* the scalar video instructions but their CC (bit 47), whose flags no source describes */
        case O_VADD:
        case O_VABSDIFF:
        case O_VSET:
        case O_VSHL:
        case O_VSHR: return !B(w, 47, 1);
        /* PIXLD's MyIndex (mode 5, the sample) as Eden translates it, and CovMask (1, the coverage), with no address
           and no predicate */
        case O_PIXLD:
            return (B(w, 31, 3) == 5 || B(w, 31, 3) == 1) && B(w, 45, 3) == 7 && B(w, 8, 8) == 255 && !B(w, 20, 8);
        /* PRMT modes 0-6; modes 7-15 are not translated */
        case O_PRMT_R:
        case O_PRMT_RC:
        case O_PRMT_CR:
        case O_PRMT_I: return B(w, 48, 4) <= 6;
        /* compare and swap of 32 bits, the comparison in Rb and the new value in Rb + 1, as Mesa's NAK encodes it:
           global (bit 49 clear, data layout 50-51 packed) and shared (sub-op 4) */
        case O_ATOM_CAS: return !B(w, 49, 1) && !B(w, 50, 2);
        case O_ATOMS_CAS: return B(w, 52, 4) == 4 && !B(w, 28, 2);
        case O_LDL:
        case O_STL: return B(w, 48, 3) <= 7;
        case O_LDS:
        case O_STS: return B(w, 48, 3) <= 7;
        /* BAR.SYNC 0 for the whole workgroup: arrivals, reductions and thread counts are other barriers */
        case O_BAR:
            return (w & 0x0000009B00000000ull) == 0x0000008000000000ull && B(w, 43, 1) && !B(w, 8, 8) && B(w, 44, 1) &&
                   !B(w, 20, 12);
        case O_S2R: {
            u32 sr = B(w, 20, 8);
            /* SR_AFFINITY (0x1c) is not modeled; the clock registers require the shader-clock feature below. */
            return sr == 0 || sr == 0x11 || sr == 0x12 || sr == 0x13 || sr == 0x15 || sr == 0x16 || sr == 0x17 ||
                   sr == 0x1c || sr == 0x1d || sr == 0x1e || sr == 0x1f || (sr >= 0x20 && sr <= 0x23) ||
                   (sr >= 0x25 && sr <= 0x27) || sr == 0x38 || sr == 0x39 || sr == 0x3a || sr == 0x3b || sr == 0x3c ||
                   (sr >= 0x50 && sr <= 0x53);
        }
        /* Attribute addresses use the same byte offsets as ALD/AST. The range predicate is not modeled: a program
           whose AL2P predicate something can read is refused (shader_al2p_predicate_unseen) */
        case O_AL2P: return 1;
        /* ALD and AST: a patch's attributes (bit 31) are its tessellation levels and 30 vectors from 0x20. Which stage
           has which is shader_stage_has's. ALD.O (bit 32) reads a control program's per-vertex outputs. With an index
           register (an AL2P's result), an input vector of the header's map, at the register plus addr */
        case O_ALD: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            if (B(w, 0, 8) + count > 255) return 0;
            if (B(w, 8, 8) != 255) return !B(w, 31, 1) && !B(w, 32, 1);
            if (B(w, 31, 1)) return addr + count * 4 <= 0x200;
            return (addr >= 0x70 && addr + count * 4 <= 0x280) ||
                   ((addr == 0x60 || addr == 0x2f4 || addr == 0x2f8 || addr == 0x2fc) && count == 1) ||
                   (addr == 0x2f0 && count <= 2);
        }
        /* AST also takes the viewport mask (0x3a0): what it holds is checked over the program (to_spirv) */
        case O_AST: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            if (B(w, 8, 8) != 255 || !(B(w, 0, 8) + count <= 255 || (B(w, 0, 8) == 255 && count == 1))) return 0;
            if (B(w, 31, 1)) return addr + count * 4 <= 0x200;
            return ((addr == 0x64 || addr == 0x68 || addr == 0x6c) && count == 1) ||
                   (addr >= 0x70 && addr + count * 4 <= 0x80) || (addr >= 0x80 && addr + count * 4 <= 0x280) ||
                   (addr >= 0x2c0 && addr + count * 4 <= 0x2e0) || (addr == 0x3a0 && count == 1);
        }
        /* OUT emits and ends geometry primitives on the stream its third operand names */
        case O_OUT_R:
        case O_OUT_C:
        case O_OUT_I: return 1;
        /* ISBE handles are represented by vertex indices. Input/output buffers and allocation modes do not
           change that index; ALD selects the interface to read. Skewed addresses remain unsupported. */
        case O_ISBERD: return B(w, 8, 8) != 255 && !B(w, 24, 8) && !B(w, 31, 1);
        case O_KIL: return cc_valid(B(w, 0, 5));
        /* BRA/CAL/PRET bit 5 selects a constant-buffer target; BRX bit 6 enables LMT. The other flow forms below
           also require bit 5 clear in their supported encoding. */
        case O_BRA: return cc_valid(B(w, 0, 5)) && !B(w, 5, 1);
        case O_CAL: return !B(w, 5, 1);
        case O_PRET: return !B(w, 5, 1);
        /* EXIT consumes a PEXIT token and branches to its target. */
        case O_PEXIT: return !B(w, 5, 1);
        case O_RET: return cc_valid(B(w, 0, 5)) && !B(w, 5, 1);
        case O_BRX: return cc_valid(B(w, 0, 5)) && !B(w, 5, 2);
        case O_SSY:
        case O_PBK:
        case O_PCNT: return !B(w, 5, 1);
        case O_SYNC:
        case O_BRK:
        case O_CONT: return cc_valid(B(w, 0, 5)) && !B(w, 5, 1);
        case O_MOV32I: return B(w, 12, 4) == 1 || B(w, 12, 4) == 15;
        case O_MOV_R:
        case O_MOV_C:
        case O_MOV_I: return 1;
        /* Saturating adds and IMNMX's XLO/XHI aren't translated. */
        case O_IADD32I: return !B(w, 54, 1);
        case O_IMUL_R:
        case O_IMUL_C:
        case O_IMUL_I:
        case O_IMAD_R:
        case O_IMAD_RC:
        case O_IMAD_CR:
        case O_IMAD_I: return !B(w, 47, 1) && !(op >= O_IMAD_R && op <= O_IMAD_I && B(w, 50, 1));
        case O_IMUL32I: return !B(w, 52, 1);
        case O_IMAD32I: return !B(w, 52, 1) && !B(w, 50, 1);
        case O_IADD3_R: return 1;
        case O_IADD3_C:
        case O_IADD3_I: return 1;
        case O_IMNMX_R:
        case O_IMNMX_C:
        case O_IMNMX_I: return !B(w, 43, 2);
        case O_IADD_R:
        case O_IADD_C:
        case O_IADD_I: return !B(w, 50, 1);
        case O_LDG:
        case O_STG: return B(w, 48, 3) <= 7;
        case O_TXQ:
        case O_TXQ_B: return B(w, 22, 6) == 1;
        case O_TLD4S: return 1;
        /* TLD4.B bit 40 and TLD bit 54 request a minimum LOD that this translator does not decode. */
        case O_TLD4:
            return (B(w, 28, 3) <= 3 || B(w, 28, 3) >= 6) &&
                   (!B(w, 54, 2) || (B(w, 54, 2) <= 2 && (B(w, 28, 3) >> 1) <= 1));
        case O_TLD4_B:
            return !B(w, 40, 1) && (B(w, 28, 3) <= 3 || B(w, 28, 3) >= 6) &&
                   (!B(w, 36, 2) || (B(w, 36, 2) <= 2 && (B(w, 28, 3) >> 1) <= 1));
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I:
            return B(w, 8, 2) <= 2 && B(w, 10, 2) <= 2 && B(w, 41, 3) <= 3 &&
                   (B(w, 10, 2) != 1 || !(B(w, 41, 3) & 1)) && (B(w, 10, 2) != 2 || !B(w, 41, 3));
        case O_LOP32I: return 1;
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I: return 1;
        case O_LOP3_R:
        case O_LOP3_C:
        case O_LOP3_I: return 1;
        case O_SEL_R:
        case O_SEL_C:
        case O_SEL_I: return 1;
        /* every select but CSFU (3), which no source describes, and 5-7, which aren't selects */
        case O_XMAD_R:
        case O_XMAD_I: return B(w, 50, 3) <= 2 || B(w, 50, 3) == 4;
        case O_XMAD_RC:
        case O_XMAD_CR: return B(w, 50, 2) <= 2;
        /* the plain addressing mode: IS, IL and ISL (bits 44-45) take the bank or address from the register */
        case O_LDC:
            return B(w, 48, 3) <= 5 && B(w, 36, 5) < 18 && (B(w, 48, 3) != 5 || B(w, 0, 8) < 254) && !B(w, 44, 2);
        case O_SHF_LR:
        case O_SHF_LI:
        case O_SHF_RR:
        case O_SHF_RI: return B(w, 37, 2) != 1 && !B(w, 47, 1) && !B(w, 48, 2);
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_LEA_HR:
        case O_LEA_HC: return 1;
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I: return B(w, 45, 2) < 3;
        case O_ICMP_R:
        case O_ICMP_RC:
        case O_ICMP_CR:
        case O_ICMP_I: return 1;
        case O_FCMP_R:
        case O_FCMP_RC:
        case O_FCMP_CR:
        case O_FCMP_I: return 1;
        case O_BFI_R:
        case O_BFI_RC:
        case O_BFI_CR:
        case O_BFI_I: return 1;
        /* F2F/F2I format 0 is invalid. I2F refuses 16-bit selectors 1 and 3 and every selector on 32/64-bit
           sources. Conversion CC (bit 47) is refused, as are undocumented shift, FLO and FSWZADD flag forms. */
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I:
            return !B(w, 47, 1) && B(w, 10, 2) != 0 && B(w, 8, 2) != 0 &&
                   ((op != O_F2F_C && op != O_F2I_C) || B(w, 10, 2) != 3 || !(B(w, 20, 14) & 1));
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I:
            return !B(w, 47, 1) && (B(w, 8, 2) == 1 || B(w, 8, 2) == 2 || B(w, 8, 2) == 3) &&
                   (B(w, 10, 2) != 1 || !(B(w, 41, 2) & 1)) && (B(w, 10, 2) < 2 || !B(w, 41, 2)) &&
                   (op != O_I2F_C || B(w, 10, 2) != 3 || !(B(w, 20, 14) & 1));
        case O_ISCADD_R:
        case O_ISCADD_C:
        case O_ISCADD_I:
        case O_ISCADD32I: return 1;
        case O_BFE_R:
        case O_BFE_C:
        case O_BFE_I: return 1;
        case O_SHL_R:
        case O_SHL_C:
        case O_SHL_I: return !B(w, 43, 1) && !B(w, 47, 1);
        case O_SHR_R:
        case O_SHR_C:
        case O_SHR_I: return !B(w, 43, 2) && !B(w, 47, 1);
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I: return B(w, 45, 2) < 3;
        case O_FADD_R:
        case O_FADD_C:
        case O_FADD_I: return !B(w, 47, 1);
        case O_FADD32I: return !B(w, 52, 1);
        case O_FMUL_R:
        case O_FMUL_C:
        case O_FMUL_I: return !B(w, 47, 1) && B(w, 41, 3) < 7;
        case O_FMUL32I: return B(w, 53, 2) <= 2 && !B(w, 52, 1);
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FFMA_CR:
        case O_FFMA_I: return !B(w, 47, 1);
        case O_FFMA32I: return !B(w, 52, 1);
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I: return !B(w, 47, 1);
        case O_IPA: {
            u32 addr = B(w, 30, 8) * 4;
            /* IPA.IDX: the address is the index register's alone (an AL2P's result), as Eden and Ryujinx read it */
            if (B(w, 38, 1)) return B(w, 8, 8) != 255;
            return ((addr >= 0x80 && addr < 0x280) || (addr >= 0x70 && addr < 0x80) || addr == 0x60 || addr == 0x64 ||
                    addr == 0x2e0 || addr == 0x2e4 || addr == 0x3fc) &&
                   !B(w, 38, 1);
        }
        case O_PSETP: return B(w, 24, 2) < 3 && B(w, 45, 2) < 3;
        case O_PSET: return B(w, 24, 2) < 3 && B(w, 45, 2) < 3;
        case O_CSETP:
        case O_CSET: return cc_valid(B(w, 8, 5)) && B(w, 45, 2) < 3;
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_P2R_R:
        case O_P2R_C:
        case O_P2R_I: return 1;
        /* every function, RCP64H and RSQ64H (6, 7) included */
        case O_MUFU: return B(w, 20, 4) <= 8;
        case O_RRO_R:
        case O_RRO_C:
        case O_RRO_I: return 1;
        /* FCHK.DIVIDE (mode 0) says whether a division needs its slow path: always, which the slow path computes
           exactly. No source documents the condition, and Eden and Ryujinx don't translate it. */
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I: return !B(w, 39, 6);
        /* cache control and the interrupt and breakpoint instructions change nothing a shader computes */
        case O_CCTL:
        case O_CCTLL:
        case O_IDE:
        case O_BPT: return 1;
        /* CS2R reads a 64-bit clock or global timer into a pair. SRZ zeroes the pair. */
        case O_CS2R: return B(w, 20, 8) == 0x50 || B(w, 20, 8) == 0x52 || B(w, 20, 8) == 0xff;
        case O_TEXS: return B(w, 53, 4) <= 13 && (B(w, 28, 8) == 255 || B(w, 50, 3) <= 4);
        /* the encodings Eden lists: 3, 9, 10, 11 and 13 aren't TLDS's */
        case O_TLDS:
            return B(w, 53, 4) <= 12 && B(w, 53, 4) != 3 && B(w, 53, 4) != 9 && B(w, 53, 4) != 10 &&
                   B(w, 53, 4) != 11 && (B(w, 28, 8) == 255 || B(w, 50, 3) <= 4);
        /* every type up to 3D (a fetch can't address a cube), multisample only on 2D and 2D arrays */
        case O_TLD:
        case O_TLD_B: return !B(w, 54, 1) && B(w, 28, 3) <= 4 && (!B(w, 50, 1) || B(w, 28, 3) == 2 || B(w, 28, 3) == 3);
        /* Formatted and raw 32-bit loads from buffer, 2D, array and volume surfaces; buffer loads use storage texel
           buffers. */
        case O_SULD:
            return (B(w, 33, 3) == 1 || B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5) &&
                   (!B(w, 52, 1) || B(w, 20, 4) == 4);
        /* stores and atomics also reach 2D array surfaces (4), the layer in the coordinates' third register; stores
           reach 3D surfaces (5) too, z in that register */
        case O_SUST:
            return !B(w, 52, 1) && (B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5 || B(w, 33, 3) == 1);
        /* 32-bit atomics on buffers, 2D images, arrays and volumes, every operation (bits 29-32): INC and DEC, which
           wrap at the operand, as a compare and exchange loop. Bindless is bit 54 here. */
        case O_SUATOM:
            return (B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5 || B(w, 33, 3) == 1) &&
                   (B(w, 51, 3) <= 1 || B(w, 51, 3) == 6) && B(w, 29, 4) <= 8 && !B(w, 49, 2);
        /* reductions of 32 bits, U32 or S32 (bits 20-22), on 2D, 2D array, 3D and buffer surfaces (a compute program's
           histogram adds into a buffer, another ORs flags into one): every operation (bits 24-26). ADD and the
           bitwise ones are the same for both, MIN and MAX compare S32 signed, INC and DEC wrap */
        case O_SURED:
            return (B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5 || B(w, 33, 3) == 1) && B(w, 20, 3) <= 1 &&
                   !B(w, 23, 1) && !B(w, 49, 2);
        case O_HADD2_R:
        case O_HADD2_C:
        case O_HADD2_I:
        case O_HMUL2_R:
        case O_HMUL2_C:
        case O_HMUL2_I:
        case O_HFMA2_R:
        case O_HFMA2_RC:
        case O_HFMA2_CR:
        case O_HFMA2_I:
        case O_HADD2_32I:
        case O_HMUL2_32I:
        case O_HFMA2_32I: return 1;
        case O_HSET2_R:
        case O_HSET2_C:
        case O_HSET2_I: return 1;
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I: return 1;
        case O_DSET_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I: return 1;
        case O_DADD_R:
        case O_DADD_C:
        case O_DADD_I:
        case O_DMUL_R:
        case O_DMUL_C:
        /* the rounding mode (DADD's and DMUL's 39-40, DFMA's 50-51) is round to nearest, as FADD's and Eden's are:
           SPIR-V sets rounding per conversion, not per instruction */
        case O_DMUL_I: return !B(w, 47, 1);
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DFMA_CR:
        case O_DFMA_I: return !B(w, 47, 1);
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: return !B(w, 47, 1);
        case O_TEX: return texture_sample_supported(w, 0);
        case O_TEX_B: return texture_sample_supported(w, 1);
        /* every type but a 3D array, which doesn't exist */
        case O_TMML:
        case O_TMML_B: return B(w, 28, 3) != 5;
        /* TXD supports 1D and 2D textures and their arrays, without a LOD clamp (bit 50). */
        case O_TXD:
        case O_TXD_B: return B(w, 28, 3) < 4 && !B(w, 50, 1);
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I: return 1;
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I: return 1;
        default: return 0;
    }
}

/* The attribute instructions each stage has. Vertex, tessellation and geometry programs read their inputs and store
   their outputs, the stages after the vertex one per vertex of their input primitive (its position too).
   Tessellation control programs store the patch's attributes, which tessellation evaluation programs read with the
   tessellation coordinate. Only geometry programs emit vertices. */
int shader_stage_has(u64 w, int op, int stage) {
    int between = stage == ST_TCS || stage == ST_TES || stage == ST_GS;
    u32 addr = B(w, 20, 10);
    if (op == O_ALD) {
        if (stage == ST_FS || stage == ST_CS) return 0;
        if (B(w, 8, 8) != 255) return 1; /* indexed: the stage's generic inputs */
        if (B(w, 31, 1)) return stage == ST_TCS || stage == ST_TES;
        if (stage == ST_TCS && B(w, 32, 1) && addr < 0x70) return 0;
        if (addr == 0x2f8 || addr == 0x2fc) return stage == ST_VS;
        if (addr == 0x2f0 || addr == 0x2f4) return stage == ST_TES;
        return addr >= 0x80 || between;
    }
    if (op == O_AST) {
        if (stage == ST_FS || stage == ST_CS || (B(w, 31, 1) && stage != ST_TCS)) return 0;
        if (stage == ST_TCS && !B(w, 31, 1) && (addr == 0x64 || addr == 0x6c || addr >= 0x2c0)) return 0;
        return 1;
    }
    if (op == O_OUT_R || op == O_OUT_C || op == O_OUT_I) return stage == ST_GS;
    if (op == O_ISBERD) return between;
    if (op == O_S2R && ((B(w, 20, 8) >= 0x20 && B(w, 20, 8) <= 0x23) || (B(w, 20, 8) >= 0x25 && B(w, 20, 8) <= 0x27)))
        return stage == ST_CS;
    if (op == O_S2R && B(w, 20, 8) == 0x11) return stage == ST_TCS || stage == ST_GS;
    if (op == O_BAR) return stage == ST_CS || stage == ST_TCS;
    if (op == O_TMML || op == O_TMML_B || op == O_IPA || op == O_KIL || op == O_PIXLD) return stage == ST_FS;
    return 1;
}

/* TEXS's encodings (bits 53-56): 0 1D.LZ, 1 2D, 2 2D.LZ, 3 2D.LL, 4 2D.DC, 5 2D.LL.DC, 6 2D.LZ.DC, 7 ARRAY_2D,
   8 ARRAY_2D.LZ, 9 ARRAY_2D.LZ.DC, 10 3D, 11 3D.LZ, 12 CUBE, 13 CUBE.LL. The registers, as Eden reads them: x in Ra,
   y in Ra + 1 (in Rb for 2D and 2D.LZ, and for an array, whose layer is in Ra and x in Ra + 1), a 3D or cube
   texture's z in Rb, then the level and the depth reference in Rb or Rb + 1. */
int shader_texs_type(u32 encoding) { return encoding >= 12 ? 3 : encoding >= 10 ? 2 : encoding == 0 ? 0 : 1; }
int shader_texs_array(u32 encoding) { return encoding >= 7 && encoding <= 9; }
int shader_texs_shadow(u32 encoding) { return encoding == 4 || encoding == 5 || encoding == 6 || encoding == 9; }
u32 shader_texs_y(u32 encoding, u32 a, u32 b) {
    return encoding == 1 || encoding == 2 || shader_texs_array(encoding) ? b : a + 1;
}
/* whether the encoding takes an explicit level (LZ, LL), and the register of LL's level (255 for LZ's 0) */
int shader_texs_explicit(u32 encoding) {
    return encoding == 0 || encoding == 2 || encoding == 3 || encoding == 5 || encoding == 6 || encoding == 8 ||
           encoding == 9 || encoding == 11 || encoding == 13;
}
u32 shader_texs_level(u32 encoding, u32 b) { return encoding == 3 || encoding == 5 ? b : encoding == 13 ? b + 1 : 255; }
u32 shader_texs_dref(u32 encoding, u32 b) { return encoding == 5 || encoding == 9 ? b + 1 : b; }

int shader_half_arithmetic(int op) {
    return op == O_HADD2_R || op == O_HADD2_C || op == O_HADD2_I || op == O_HADD2_32I || op == O_HMUL2_R ||
           op == O_HMUL2_C || op == O_HMUL2_I || op == O_HMUL2_32I || op == O_HFMA2_R || op == O_HFMA2_RC ||
           op == O_HFMA2_CR || op == O_HFMA2_I || op == O_HFMA2_32I;
}

/* The predicate a texture fetch writes when a texel it read isn't resident (bits 51-53), PT (7) for none */
u32 shader_sparse_predicate(u64 w, int op) {
    return op == O_TEX || op == O_TEX_B || op == O_TLD || op == O_TLD_B || op == O_TLD4 || op == O_TLD4_B ||
                   op == O_TXD || op == O_TXD_B
               ? B(w, 51, 3)
               : 7;
}
int shader_legacy_group(u32 addr) {
    if (addr >= 0x280 && addr < 0x2c0) return (int)((addr - 0x280) >> 4);
    if (addr == 0x2e8) return 4;
    if (addr >= 0x300 && addr < 0x3a0) return 5 + (int)((addr - 0x300) >> 4);
    return -1;
}
/* the generic address a legacy varying's moves to */
u32 shader_legacy_address(u32 addr) {
    int g = shader_legacy_group(addr);
    return 0x80 + (LEGACY_GENERIC + (u32)g) * 16 + (g == 4 ? 0 : (addr & 0xf));
}
/* group g's first address, for messages */
u32 shader_legacy_base(u32 g) { return g < 4 ? 0x280 + g * 16 : g == 4 ? 0x2e8 : 0x300 + (g - 5) * 16; }
