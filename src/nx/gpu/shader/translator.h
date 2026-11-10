/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the parts of the shader translator share: the opcodes and their decoder, the program's layout in words, and
   the control flow helpers of shader.c. */
#ifndef SNX_TRANSLATOR_H
#define SNX_TRANSLATOR_H
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* The Maxwell opcodes: X(NAME, display name) for each. sm53.decode has their encodings, under the same names, and
   decode.c a trans_ function for each. The enum is O_NONE, then O_<NAME> in this order, then O__COUNT: the translator
   tests ranges of it (O_XMAD_R to O_XMAD_I...), so keep each family together. The display names go into the
   translator's "unsupported" messages, which tests match by prefix */
#define SM53_OPS(X)                                                                                                    \
    X(FADD_R, "FADD (reg)")                                                                                            \
    X(FADD_C, "FADD (cbuf)")                                                                                           \
    X(FADD_I, "FADD (imm)")                                                                                            \
    X(FADD32I, "FADD32I")                                                                                              \
    X(FMUL_R, "FMUL (reg)")                                                                                            \
    X(FMUL_C, "FMUL (cbuf)")                                                                                           \
    X(FMUL_I, "FMUL (imm)")                                                                                            \
    X(FMUL32I, "FMUL32I")                                                                                              \
    X(FFMA_R, "FFMA (reg)")                                                                                            \
    X(FFMA_RC, "FFMA (rc)")                                                                                            \
    X(FFMA_CR, "FFMA (cr)")                                                                                            \
    X(FFMA_I, "FFMA (imm)")                                                                                            \
    X(FFMA32I, "FFMA32I")                                                                                              \
    X(FMNMX_R, "FMNMX (reg)")                                                                                          \
    X(FMNMX_C, "FMNMX (cbuf)")                                                                                         \
    X(FMNMX_I, "FMNMX (imm)")                                                                                          \
    X(FSETP_R, "FSETP (reg)")                                                                                          \
    X(FSETP_C, "FSETP (cbuf)")                                                                                         \
    X(FSETP_I, "FSETP (imm)")                                                                                          \
    X(FSET_R, "FSET (reg)")                                                                                            \
    X(FSET_C, "FSET (cbuf)")                                                                                           \
    X(FSET_I, "FSET (imm)")                                                                                            \
    X(FCMP_R, "FCMP (reg)")                                                                                            \
    X(FCMP_RC, "FCMP (rc)")                                                                                            \
    X(FCMP_CR, "FCMP (cr)")                                                                                            \
    X(FCMP_I, "FCMP (imm)")                                                                                            \
    X(MUFU, "MUFU")                                                                                                    \
    X(RRO_R, "RRO (reg)")                                                                                              \
    X(RRO_C, "RRO (cbuf)")                                                                                             \
    X(RRO_I, "RRO (imm)")                                                                                              \
    X(F2F_R, "F2F (reg)")                                                                                              \
    X(F2F_C, "F2F (cbuf)")                                                                                             \
    X(F2F_I, "F2F (imm)")                                                                                              \
    X(F2I_R, "F2I (reg)")                                                                                              \
    X(F2I_C, "F2I (cbuf)")                                                                                             \
    X(F2I_I, "F2I (imm)")                                                                                              \
    X(I2F_R, "I2F (reg)")                                                                                              \
    X(I2F_C, "I2F (cbuf)")                                                                                             \
    X(I2F_I, "I2F (imm)")                                                                                              \
    X(I2I_R, "I2I (reg)")                                                                                              \
    X(I2I_C, "I2I (cbuf)")                                                                                             \
    X(I2I_I, "I2I (imm)")                                                                                              \
    X(MOV_R, "MOV (reg)")                                                                                              \
    X(MOV_C, "MOV (cbuf)")                                                                                             \
    X(MOV_I, "MOV (imm)")                                                                                              \
    X(MOV32I, "MOV32I")                                                                                                \
    X(IADD_R, "IADD (reg)")                                                                                            \
    X(IADD_C, "IADD (cbuf)")                                                                                           \
    X(IADD_I, "IADD (imm)")                                                                                            \
    X(IADD32I, "IADD32I")                                                                                              \
    X(IADD3_R, "IADD3 (reg)")                                                                                          \
    X(IADD3_C, "IADD3 (cbuf)")                                                                                         \
    X(IADD3_I, "IADD3 (imm)")                                                                                          \
    X(ISCADD_R, "ISCADD (reg)")                                                                                        \
    X(ISCADD_C, "ISCADD (cbuf)")                                                                                       \
    X(ISCADD_I, "ISCADD (imm)")                                                                                        \
    X(ISCADD32I, "ISCADD32I")                                                                                          \
    X(IMNMX_R, "IMNMX (reg)")                                                                                          \
    X(IMNMX_C, "IMNMX (cbuf)")                                                                                         \
    X(IMNMX_I, "IMNMX (imm)")                                                                                          \
    X(ISETP_R, "ISETP (reg)")                                                                                          \
    X(ISETP_C, "ISETP (cbuf)")                                                                                         \
    X(ISETP_I, "ISETP (imm)")                                                                                          \
    X(ISET_R, "ISET (reg)")                                                                                            \
    X(ISET_C, "ISET (cbuf)")                                                                                           \
    X(ISET_I, "ISET (imm)")                                                                                            \
    X(ICMP_R, "ICMP (reg)")                                                                                            \
    X(ICMP_RC, "ICMP (rc)")                                                                                            \
    X(ICMP_CR, "ICMP (cr)")                                                                                            \
    X(ICMP_I, "ICMP (imm)")                                                                                            \
    X(LOP_R, "LOP (reg)")                                                                                              \
    X(LOP_C, "LOP (cbuf)")                                                                                             \
    X(LOP_I, "LOP (imm)")                                                                                              \
    X(LOP32I, "LOP32I")                                                                                                \
    X(LOP3_R, "LOP3 (reg)")                                                                                            \
    X(LOP3_C, "LOP3 (cbuf)")                                                                                           \
    X(LOP3_I, "LOP3 (imm)")                                                                                            \
    X(SHL_R, "SHL (reg)")                                                                                              \
    X(SHL_C, "SHL (cbuf)")                                                                                             \
    X(SHL_I, "SHL (imm)")                                                                                              \
    X(SHR_R, "SHR (reg)")                                                                                              \
    X(SHR_C, "SHR (cbuf)")                                                                                             \
    X(SHR_I, "SHR (imm)")                                                                                              \
    X(SHF_LR, "SHF (l reg)")                                                                                           \
    X(SHF_LI, "SHF (l imm)")                                                                                           \
    X(SHF_RR, "SHF (r reg)")                                                                                           \
    X(SHF_RI, "SHF (r imm)")                                                                                           \
    X(BFE_R, "BFE (reg)")                                                                                              \
    X(BFE_C, "BFE (cbuf)")                                                                                             \
    X(BFE_I, "BFE (imm)")                                                                                              \
    X(BFI_R, "BFI (reg)")                                                                                              \
    X(BFI_RC, "BFI (rc)")                                                                                              \
    X(BFI_CR, "BFI (cr)")                                                                                              \
    X(BFI_I, "BFI (imm)")                                                                                              \
    X(SEL_R, "SEL (reg)")                                                                                              \
    X(SEL_C, "SEL (cbuf)")                                                                                             \
    X(SEL_I, "SEL (imm)")                                                                                              \
    X(POPC_R, "POPC (reg)")                                                                                            \
    X(POPC_C, "POPC (cbuf)")                                                                                           \
    X(POPC_I, "POPC (imm)")                                                                                            \
    X(FLO_R, "FLO (reg)")                                                                                              \
    X(FLO_C, "FLO (cbuf)")                                                                                             \
    X(FLO_I, "FLO (imm)")                                                                                              \
    X(PRMT_R, "PRMT (reg)")                                                                                            \
    X(PRMT_RC, "PRMT (rc)")                                                                                            \
    X(PRMT_CR, "PRMT (cr)")                                                                                            \
    X(PRMT_I, "PRMT (imm)")                                                                                            \
    X(XMAD_R, "XMAD (reg)")                                                                                            \
    X(XMAD_RC, "XMAD (rc)")                                                                                            \
    X(XMAD_CR, "XMAD (cr)")                                                                                            \
    X(XMAD_I, "XMAD (imm)")                                                                                            \
    X(IMUL_R, "IMUL (reg)")                                                                                            \
    X(IMUL_C, "IMUL (cbuf)")                                                                                           \
    X(IMUL_I, "IMUL (imm)")                                                                                            \
    X(IMUL32I, "IMUL32I")                                                                                              \
    X(IMAD_R, "IMAD (reg)")                                                                                            \
    X(IMAD_RC, "IMAD (rc)")                                                                                            \
    X(IMAD_CR, "IMAD (cr)")                                                                                            \
    X(IMAD_I, "IMAD (imm)")                                                                                            \
    X(IMAD32I, "IMAD32I")                                                                                              \
    X(LEA_HR, "LEA (hi reg)")                                                                                          \
    X(LEA_HC, "LEA (hi cbuf)")                                                                                         \
    X(LEA_LR, "LEA (lo reg)")                                                                                          \
    X(LEA_LC, "LEA (lo cbuf)")                                                                                         \
    X(LEA_LI, "LEA (lo imm)")                                                                                          \
    X(PSETP, "PSETP")                                                                                                  \
    X(PSET, "PSET")                                                                                                    \
    X(P2R_R, "P2R (reg)")                                                                                              \
    X(P2R_C, "P2R (cbuf)")                                                                                             \
    X(P2R_I, "P2R (imm)")                                                                                              \
    X(R2P_R, "R2P (reg)")                                                                                              \
    X(R2P_C, "R2P (cbuf)")                                                                                             \
    X(R2P_I, "R2P (imm)")                                                                                              \
    X(CSET, "CSET")                                                                                                    \
    X(CSETP, "CSETP")                                                                                                  \
    X(S2R, "S2R")                                                                                                      \
    X(CS2R, "CS2R")                                                                                                    \
    X(IPA, "IPA")                                                                                                      \
    X(ALD, "ALD")                                                                                                      \
    X(AST, "AST")                                                                                                      \
    X(OUT_R, "OUT (reg)")                                                                                              \
    X(OUT_C, "OUT (cbuf)")                                                                                             \
    X(OUT_I, "OUT (imm)")                                                                                              \
    X(LDC, "LDC")                                                                                                      \
    X(LDG, "LDG")                                                                                                      \
    X(STG, "STG")                                                                                                      \
    X(LDS, "LDS")                                                                                                      \
    X(STS, "STS")                                                                                                      \
    X(LDL, "LDL")                                                                                                      \
    X(STL, "STL")                                                                                                      \
    X(LD, "LD")                                                                                                        \
    X(ST, "ST")                                                                                                        \
    X(STP, "STP")                                                                                                      \
    X(ATOM, "ATOM")                                                                                                    \
    X(ATOMS, "ATOMS")                                                                                                  \
    X(RED, "RED")                                                                                                      \
    X(ATOM_CAS, "ATOM.CAS")                                                                                            \
    X(ATOMS_CAS, "ATOMS.CAS")                                                                                          \
    X(TEX, "TEX")                                                                                                      \
    X(TEX_B, "TEX.B")                                                                                                  \
    X(TEXS, "TEXS")                                                                                                    \
    X(TLD, "TLD")                                                                                                      \
    X(TLD_B, "TLD.B")                                                                                                  \
    X(TLDS, "TLDS")                                                                                                    \
    X(TLD4, "TLD4")                                                                                                    \
    X(TLD4_B, "TLD4.B")                                                                                                \
    X(TLD4S, "TLD4S")                                                                                                  \
    X(TXQ, "TXQ")                                                                                                      \
    X(TXQ_B, "TXQ.B")                                                                                                  \
    X(TMML, "TMML")                                                                                                    \
    X(TMML_B, "TMML.B")                                                                                                \
    X(TXD, "TXD")                                                                                                      \
    X(TXD_B, "TXD.B")                                                                                                  \
    X(SULD, "SULD")                                                                                                    \
    X(SUST, "SUST")                                                                                                    \
    X(SURED, "SURED")                                                                                                  \
    X(SUATOM, "SUATOM")                                                                                                \
    X(SUATOM_CAS, "SUATOM.CAS")                                                                                        \
    X(BRA, "BRA")                                                                                                      \
    X(BRX, "BRX")                                                                                                      \
    X(SSY, "SSY")                                                                                                      \
    X(SYNC, "SYNC")                                                                                                    \
    X(PBK, "PBK")                                                                                                      \
    X(BRK, "BRK")                                                                                                      \
    X(PCNT, "PCNT")                                                                                                    \
    X(CONT, "CONT")                                                                                                    \
    X(EXIT, "EXIT")                                                                                                    \
    X(KIL, "KIL")                                                                                                      \
    X(JCAL, "JCAL")                                                                                                    \
    X(JMX, "JMX")                                                                                                      \
    X(JMP, "JMP")                                                                                                      \
    X(CAL, "CAL")                                                                                                      \
    X(RET, "RET")                                                                                                      \
    X(PRET, "PRET")                                                                                                    \
    X(PEXIT, "PEXIT")                                                                                                  \
    X(NOP, "NOP")                                                                                                      \
    X(BAR, "BAR")                                                                                                      \
    X(MEMBAR, "MEMBAR")                                                                                                \
    X(DEPBAR, "DEPBAR")                                                                                                \
    X(VOTE, "VOTE")                                                                                                    \
    X(VOTE_VTG, "VOTE.VTG")                                                                                            \
    X(SHFL, "SHFL")                                                                                                    \
    X(FSWZADD, "FSWZADD")                                                                                              \
    X(FCHK_R, "FCHK (reg)")                                                                                            \
    X(FCHK_C, "FCHK (cbuf)")                                                                                           \
    X(FCHK_I, "FCHK (imm)")                                                                                            \
    X(HADD2_R, "HADD2 (reg)")                                                                                          \
    X(HADD2_C, "HADD2 (cbuf)")                                                                                         \
    X(HADD2_I, "HADD2 (imm)")                                                                                          \
    X(HADD2_32I, "HADD2_32I")                                                                                          \
    X(HMUL2_R, "HMUL2 (reg)")                                                                                          \
    X(HMUL2_C, "HMUL2 (cbuf)")                                                                                         \
    X(HMUL2_I, "HMUL2 (imm)")                                                                                          \
    X(HMUL2_32I, "HMUL2_32I")                                                                                          \
    X(HFMA2_R, "HFMA2 (reg)")                                                                                          \
    X(HFMA2_RC, "HFMA2 (rc)")                                                                                          \
    X(HFMA2_CR, "HFMA2 (cr)")                                                                                          \
    X(HFMA2_I, "HFMA2 (imm)")                                                                                          \
    X(HFMA2_32I, "HFMA2_32I")                                                                                          \
    X(HSETP2_R, "HSETP2 (reg)")                                                                                        \
    X(HSETP2_C, "HSETP2 (cbuf)")                                                                                       \
    X(HSETP2_I, "HSETP2 (imm)")                                                                                        \
    X(HSET2_R, "HSET2 (reg)")                                                                                          \
    X(HSET2_C, "HSET2 (cbuf)")                                                                                         \
    X(HSET2_I, "HSET2 (imm)")                                                                                          \
    X(DADD_R, "DADD (reg)")                                                                                            \
    X(DADD_C, "DADD (cbuf)")                                                                                           \
    X(DADD_I, "DADD (imm)")                                                                                            \
    X(DMUL_R, "DMUL (reg)")                                                                                            \
    X(DMUL_C, "DMUL (cbuf)")                                                                                           \
    X(DMUL_I, "DMUL (imm)")                                                                                            \
    X(DFMA_R, "DFMA (reg)")                                                                                            \
    X(DFMA_RC, "DFMA (rc)")                                                                                            \
    X(DFMA_CR, "DFMA (cr)")                                                                                            \
    X(DFMA_I, "DFMA (imm)")                                                                                            \
    X(DSET_R, "DSET (reg)")                                                                                            \
    X(DSET_C, "DSET (cbuf)")                                                                                           \
    X(DSET_I, "DSET (imm)")                                                                                            \
    X(DSETP_R, "DSETP (reg)")                                                                                          \
    X(DSETP_C, "DSETP (cbuf)")                                                                                         \
    X(DSETP_I, "DSETP (imm)")                                                                                          \
    X(DMNMX_R, "DMNMX (reg)")                                                                                          \
    X(DMNMX_C, "DMNMX (cbuf)")                                                                                         \
    X(DMNMX_I, "DMNMX (imm)")                                                                                          \
    X(VMNMX, "VMNMX")                                                                                                  \
    X(VMAD, "VMAD")                                                                                                    \
    X(VSETP, "VSETP")                                                                                                  \
    X(VADD, "VADD")                                                                                                    \
    X(VABSDIFF, "VABSDIFF")                                                                                            \
    X(VSET, "VSET")                                                                                                    \
    X(VSHL, "VSHL")                                                                                                    \
    X(VSHR, "VSHR")                                                                                                    \
    X(IDE, "IDE")                                                                                                      \
    X(CCTL, "CCTL")                                                                                                    \
    X(CCTLL, "CCTLL")                                                                                                  \
    X(BPT, "BPT")                                                                                                      \
    X(ISBERD, "ISBERD")                                                                                                \
    X(AL2P, "AL2P")                                                                                                    \
    X(PIXLD, "PIXLD")                                                                                                  \
    X(TXA, "TXA")                                                                                                      \
    X(B2R, "B2R")                                                                                                      \
    X(R2B, "R2B")                                                                                                      \
    X(LEPC, "LEPC")                                                                                                    \
    X(SETCRSPTR, "SETCRSPTR")                                                                                          \
    X(GETCRSPTR, "GETCRSPTR")
enum {
    O_NONE = 0,
#define SM53_ENUM(name, show) O_##name,
    SM53_OPS(SM53_ENUM)
#undef SM53_ENUM
    O__COUNT
};

/* n bits of v from bit lo */
#define B(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

/* a decoded opcode and its name for messages */
typedef struct {
    int id;
    const char* name;
} Op;

/* Word i of a program is an instruction: words 0-9 are the 80-byte SPH, and word 10 and every fourth after it
   schedule the three instructions that follow. */
#define IDX_OK(i) ((i) >= 11 && ((i) - 10) % 4 != 0)
/* the token kind a CRS stack instruction pushes or pops: SSY/SYNC 0, PBK/BRK 1, PCNT/CONT 2, PEXIT 3 */
#define CRS_KIND(id)                                                                                                   \
    ((id) == O_SSY || (id) == O_SYNC ? 0 : (id) == O_PBK || (id) == O_BRK ? 1 : (id) == O_PEXIT ? 3 : 2)

/* A program's bytes are guest memory's, so its words are little-endian: the instruction words as host numbers, which
   the decoder and the translator read. code itself on a little-endian host; on a big-endian one a malloc'd copy of
   bytes / 8 words, in *copy for the caller to free (NULL without memory) */
const u64* shader_words(const u8* code, u32 bytes, u64** copy);
/* the opcode of instruction w, or NULL when no encoding matches */
const Op* decode(u64 w);
/* the word a relative branch (BRA, SSY, PBK, PCNT, CAL, PRET, PEXIT) at word idx goes to */
u32 bra_target(u64 w, u32 idx);
/* whether the instruction w (decoded as op) writes general register reg, its whole destination range included
   (forms.c) */
int shader_writes_reg(u64 w, int op, u32 reg);
/* whether a flow instruction always takes effect: predicate PT and flow condition T */
int flow_always(u64 w);
/* each SYNC, BRK and CONT's target when the CRS stack can be followed at translation time: 1, or 0 when the stack
   has to be kept at run time */
int shader_resolve_crs(const u64* words, u32 count, u32* pop_targets);

/* A program's control flow as structured constructs (structure.c), as a list of tokens in the order the translator
   writes them. Scopes are SPIR-V loop constructs: a block scope runs once and ends before the block it was opened
   for, a loop scope repeats from its header. */
enum {
    SHS_BODY,       /* a block's instructions: the words from a up to b */
    SHS_RETURNED,   /* the body before ended the program: an EXIT that always runs */
    SHS_OPEN_BLOCK, /* a: the scope */
    SHS_OPEN_LOOP,  /* a: the scope */
    SHS_CLOSE,      /* a: the scope. Running off its end leaves it */
    SHS_IF,         /* on the condition of the branch that ends the body before: its target first */
    SHS_ELSE,
    SHS_ENDIF,
    SHS_BREAK,    /* a: the innermost scope, left; b: the scope + 1 to store as the pending exit first, 0 for none */
    SHS_CONTINUE, /* a: the innermost scope, a loop, repeated */
    SHS_END,      /* control runs off the program's last instruction */
    SHS_SWITCH,   /* on the word the BRX that ends the body before goes to: a cases */
    SHS_CASE,     /* a: the case's index, b: its word */
    SHS_DEFAULT,  /* a: the cases (another word ends the program, SHS_END) */
    SHS_ENDSWITCH
};
typedef struct {
    u32 kind, a, b;
} ShsToken;
typedef struct {
    u8 loop;
    /* a pending exit may reach its merge: whether one there goes to the parent scope, or to one further out */
    u8 check, arrive, beyond;
    u32 parent; /* the enclosing scope + 1, 0 for none */
} ShsScope;
typedef struct {
    ShsToken* token;
    ShsScope* scope;
    u32 ntokens, nscopes;
    int uses_flag; /* some break stores a pending exit */
} ShStructure;
/* The structure of a program without calls, unknown jump tables or a CRS stack kept at run time, from the translator's
   block leaders (a byte a word) and its pops' targets: NULL when the graph is irreducible or the structured form
   can't take it. One allocation, freed with free */
ShStructure* shader_structure(const u64* insn, u32 n, const u8* leader, const u32* pop_targets);
/* how a block of shader_structure_graph's graph ends: it runs on into the next block, it always branches, it branches
   on a condition (its target first, then the next block), a jump table (only shader_structure's), the program ends
   there (an EXIT that always runs), or control runs off the program's last instruction */
enum { SHT_FALL, SHT_GOTO, SHT_COND, SHT_SWITCH, SHT_EXIT, SHT_END };
/* The structure of a graph that another frontend made (sm86_translate.c): n instructions in nb blocks, block b from
   instruction start[b] (start[0] is 0, then increasing) up to the next block's start, ending as term[b] says, with its
   successor blocks in succ[2 * b] and succ[2 * b + 1]. A SHT_SWITCH block's targets are the blocks sw_targets[sw_first[b]]
   up to sw_targets[sw_first[b + 1]] (64 at most), and SHS_CASE's b their first instruction. sw_first has nb + 1 entries,
   and both are NULL without a switch. The SHS_BODY tokens name instruction indices. NULL when the graph is irreducible
   or the structured form can't take it. One allocation, freed with free */
ShStructure* shader_structure_graph(u32 n, u32 nb, const u32* start, const u8* term, const u32* succ,
                                    const u32* sw_first, const u32* sw_targets);
/* The fixed function's varyings: the front and back colors (0x280-0x2bf), the fog coordinate (0x2e8) and the 10
   texture coordinates (0x300-0x39f). SPIR-V has none of them, so each group (a color, the fog, a coordinate: 15 of
   them) takes generic 17 + its index in every stage. Eden and Ryujinx give each one of the generics a program leaves
   unused, matched to the previous stage's choice; a program here is translated without the other stages, so the
   choice is fixed, the last 15 generics, and a program that uses one of those itself as well is refused. A group's
   index, or -1 for an address of none (forms.c). The SM86 translator moves them the same way */
#define LEGACY_GENERIC 17u
int shader_legacy_group(u32 addr);
/* the generic address a legacy varying's moves to, and group g's first address, for messages */
u32 shader_legacy_address(u32 addr);
u32 shader_legacy_base(u32 g);
#endif
