# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The profile of a64.decode that tools/a64dec reads with it: every name of the .decode files has one line here, and a
# name they lack is an error.
#
#   NAME FLOW [nop] [PREDICATE...] FEATURE...
#
# FLOW: B, BL, BC (B.cond, CBZ, TBZ, CB<cc>), BR, BLR, RET, ERET, SVC, BRK, EXC (HVC, SMC, HLT), else PLAIN.
# nop: no architectural effect at EL0.
# PREDICATE: comparisons of the row's arguments joined by | (arg, arg&MASK, OP == != < <= > >=, a number or another
# argument), or valid=bitmask(arg) for a logical immediate. A row whose predicate fails does not match: the
# encodings the Arm ARM reserves inside a pattern (and QEMU's trans_* refuse).
# FEATURE: the FEAT_* the Arm ARM's encoding index names (v8.0 for the base), as QEMU's translate-a64.c gates it
# with dc_isar_feature(). FEAT_X(cond) is needed only when cond holds, as half precision is for esz == 1.
# A row needing a feature the target lacks does not match.
#
# EL1 marks an encoding that is UNDEF at EL0, where games and every target here run: it needs EL1, which `*` leaves
# out, so the word is UNDEF for every target and the runtime raises an undefined instruction, as the console does.
# ERET, ERETA, HVC, SMC and HLT (halting debug is off) are UNDEFINED at EL0. MSR SPSel, PAN, UAO and ALLINT
# (immediate) are UNDEFINED at EL0. MSR DAIFSet and DAIFClr trap to EL1 because Horizon keeps SCTLR_EL1.UMA 0, and
# the kernel delivers the trap to the process as an exception, which ends it as an UNDEF does. The old decoder ran the MSR immediates as NOPs
# and these as UNDEF: no game that runs on the console executes one, so only the code after one changes, which
# discovery no longer follows, as the console never falls through to it.
# SYS (MRS, MSR register, SYS, SYSL) stays EL0 here: which system registers and operations EL0 can reach depends on
# op0:op1:CRn:CRm:op2 and on the EL1 controls (SCTLR_EL1.UCT, UCI, DZE, CNTKCTL_EL1), more than a decode table
# should hold, so the emitter decides each one and raises UNDEF for the rest.

feature v8.0 FEAT_CRC32 FEAT_AES FEAT_PMULL FEAT_SHA1 FEAT_SHA256
feature FEAT_LSE FEAT_RDM FEAT_LOR FEAT_PAN
feature FEAT_FP16 FEAT_DotProd FEAT_FHM FEAT_SHA512 FEAT_SHA3 FEAT_SM3 FEAT_SM4 FEAT_UAO
feature FEAT_LRCPC FEAT_JSCVT FEAT_FCMA FEAT_PAuth
feature FEAT_LRCPC2 FEAT_DIT FEAT_FlagM FEAT_FlagM2 FEAT_FRINTTS FEAT_SB FEAT_SSBS
feature FEAT_MTE FEAT_MTE2 FEAT_BF16 FEAT_I8MM FEAT_RAS FEAT_XS FEAT_WFxT FEAT_HBC FEAT_MOPS FEAT_NMI
feature FEAT_CSSC FEAT_SME FEAT_GCS FEAT_CHK FEAT_LSE128
feature FEAT_FAMINMAX FEAT_LUT FEAT_FP8 FEAT_FP8FMA FEAT_FP8DOT4 FEAT_FP8DOT2 FEAT_F8F32MM FEAT_F8F16MM
feature FEAT_CMPBR FEAT_FPRCVT
feature EL1

# The Switch's Cortex-A57: Armv8.0 with the CRC32 and Cryptographic Extensions
target switch v8.0 FEAT_CRC32 FEAT_AES FEAT_PMULL FEAT_SHA1 FEAT_SHA256
# Experimental HLE profile: LSE atomics, RCpc loads and PAuth instructions with SCTLR_EL1.EnIA/EnIB/EnDA/EnDB clear.
# This is not a complete CPU profile for newer consoles.
target switch_extended v8.0 FEAT_CRC32 FEAT_AES FEAT_PMULL FEAT_SHA1 FEAT_SHA256 FEAT_LSE FEAT_LRCPC FEAT_PAuth
# every feature the decoder knows
target max *

ADR             PLAIN v8.0
ADRP            PLAIN v8.0
ADD_i           PLAIN v8.0
ADDS_i          PLAIN v8.0
SUB_i           PLAIN v8.0
SUBS_i          PLAIN v8.0
ADDG_i          PLAIN FEAT_MTE
SUBG_i          PLAIN FEAT_MTE
AND_i           PLAIN valid=bitmask(dbm) v8.0
ORR_i           PLAIN valid=bitmask(dbm) v8.0
EOR_i           PLAIN valid=bitmask(dbm) v8.0
ANDS_i          PLAIN valid=bitmask(dbm) v8.0
MOVN            PLAIN v8.0
MOVZ            PLAIN v8.0
MOVK            PLAIN v8.0
SMAX_i          PLAIN FEAT_CSSC
SMIN_i          PLAIN FEAT_CSSC
UMAX_i          PLAIN FEAT_CSSC
UMIN_i          PLAIN FEAT_CSSC
SBFM            PLAIN v8.0
BFM             PLAIN v8.0
UBFM            PLAIN v8.0
EXTR            PLAIN v8.0
B               B     v8.0
BL              BL    v8.0
CBZ             BC    v8.0
TBZ             BC    v8.0
B_cond          BC    v8.0 FEAT_HBC(c==1)
CB_cond         BC    cc!=4 cc!=5 FEAT_CMPBR
CB_cond_imm     BC    cc!=4 cc!=5 FEAT_CMPBR
BR              BR    v8.0
BLR             BLR   v8.0
RET             RET   v8.0
BRAZ            BR    FEAT_PAuth
BLRAZ           BLR   FEAT_PAuth
RETA            RET   FEAT_PAuth
BRA             BR    FEAT_PAuth
BLRA            BLR   FEAT_PAuth
ERET            ERET  v8.0 EL1
ERETA           ERET  FEAT_PAuth EL1
YIELD           PLAIN nop v8.0
WFE             PLAIN nop v8.0
WFI             PLAIN nop v8.0
SEV             PLAIN nop v8.0
SEVL            PLAIN nop v8.0
XPACLRI         PLAIN FEAT_PAuth
PACIA1716       PLAIN FEAT_PAuth
PACIB1716       PLAIN FEAT_PAuth
AUTIA1716       PLAIN FEAT_PAuth
AUTIB1716       PLAIN FEAT_PAuth
ESB             PLAIN FEAT_RAS
GCSB            PLAIN FEAT_GCS
PACIAZ          PLAIN FEAT_PAuth
PACIASP         PLAIN FEAT_PAuth
PACIBZ          PLAIN FEAT_PAuth
PACIBSP         PLAIN FEAT_PAuth
AUTIAZ          PLAIN FEAT_PAuth
AUTIASP         PLAIN FEAT_PAuth
AUTIBZ          PLAIN FEAT_PAuth
AUTIBSP         PLAIN FEAT_PAuth
CHKFEAT         PLAIN FEAT_CHK
NOP             PLAIN nop v8.0
WFET            PLAIN FEAT_WFxT
WFIT            PLAIN FEAT_WFxT
CLREX           PLAIN v8.0
DSB_DMB         PLAIN v8.0
DSB_nXS         PLAIN FEAT_XS
ISB             PLAIN v8.0
SB              PLAIN FEAT_SB
CFINV           PLAIN FEAT_FlagM
XAFLAG          PLAIN FEAT_FlagM2
AXFLAG          PLAIN FEAT_FlagM2
MSR_i_UAO       PLAIN FEAT_UAO EL1
MSR_i_PAN       PLAIN FEAT_PAN EL1
MSR_i_SPSEL     PLAIN v8.0 EL1
MSR_i_SBSS      PLAIN FEAT_SSBS
MSR_i_DIT       PLAIN FEAT_DIT
MSR_i_TCO       PLAIN FEAT_MTE
MSR_i_DAIFSET   PLAIN v8.0 EL1
MSR_i_DAIFCLEAR PLAIN v8.0 EL1
MSR_i_ALLINT    PLAIN FEAT_NMI EL1
MSR_i_SVCR      PLAIN mask!=0 FEAT_SME
SYS             PLAIN v8.0
SVC             SVC   v8.0
HVC             EXC   v8.0 EL1
SMC             EXC   v8.0 EL1
BRK             BRK   v8.0
HLT             EXC   v8.0 EL1
STXR            PLAIN v8.0
LDXR            PLAIN v8.0
STLR            PLAIN v8.0 FEAT_LOR(lasr==0)
LDAR            PLAIN v8.0 FEAT_LOR(lasr==0)
STXP            PLAIN v8.0
LDXP            PLAIN v8.0
CASP            PLAIN rs&1==0 rt&1==0 FEAT_LSE
CAS             PLAIN FEAT_LSE
LD_lit          PLAIN v8.0
LD_lit_v        PLAIN v8.0
STP             PLAIN v8.0
LDP             PLAIN v8.0
STP_v           PLAIN v8.0
LDP_v           PLAIN v8.0
STGP            PLAIN FEAT_MTE
STR_i           PLAIN v8.0
LDR_i           PLAIN v8.0
STR_v_i         PLAIN v8.0
LDR_v_i         PLAIN v8.0
STR             PLAIN opt&2!=0 v8.0
LDR             PLAIN opt&2!=0 v8.0
STR_v           PLAIN opt&2!=0 v8.0
LDR_v           PLAIN opt&2!=0 v8.0
LDADD           PLAIN FEAT_LSE
LDCLR           PLAIN FEAT_LSE
LDEOR           PLAIN FEAT_LSE
LDSET           PLAIN FEAT_LSE
LDSMAX          PLAIN FEAT_LSE
LDSMIN          PLAIN FEAT_LSE
LDUMAX          PLAIN FEAT_LSE
LDUMIN          PLAIN FEAT_LSE
SWP             PLAIN FEAT_LSE
LDAPR           PLAIN FEAT_LRCPC
LDCLRP          PLAIN FEAT_LSE128
LDSETP          PLAIN FEAT_LSE128
SWPP            PLAIN FEAT_LSE128
LDRA            PLAIN FEAT_PAuth
STLR_i          PLAIN FEAT_LRCPC2
LDAPR_i         PLAIN FEAT_LRCPC2
GCSSTR          PLAIN FEAT_GCS
ST_mult         PLAIN p==1|rm==0 sz!=3|q==1|selem==1 v8.0
LD_mult         PLAIN p==1|rm==0 sz!=3|q==1|selem==1 v8.0
ST_single       PLAIN p==1|rm==0 v8.0
LD_single       PLAIN p==1|rm==0 v8.0
LD_single_repl  PLAIN p==1|rm==0 v8.0
STZGM           PLAIN FEAT_MTE2
STG             PLAIN FEAT_MTE
LDG             PLAIN FEAT_MTE
STZG            PLAIN FEAT_MTE
STGM            PLAIN FEAT_MTE2
ST2G            PLAIN FEAT_MTE
LDGM            PLAIN FEAT_MTE2
STZ2G           PLAIN FEAT_MTE
SETP            PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rn!=31 FEAT_MOPS
SETM            PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rn!=31 FEAT_MOPS
SETE            PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rn!=31 FEAT_MOPS
SETGP           PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rn!=31 FEAT_MOPS FEAT_MTE2
SETGM           PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rn!=31 FEAT_MOPS FEAT_MTE2
SETGE           PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rn!=31 FEAT_MOPS FEAT_MTE2
CPYFP           PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rs!=31 rn!=31 FEAT_MOPS
CPYFM           PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rs!=31 rn!=31 FEAT_MOPS
CPYFE           PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rs!=31 rn!=31 FEAT_MOPS
CPYP            PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rs!=31 rn!=31 FEAT_MOPS
CPYM            PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rs!=31 rn!=31 FEAT_MOPS
CPYE            PLAIN rs!=rn rs!=rd rn!=rd rd!=31 rs!=31 rn!=31 FEAT_MOPS
UDIV            PLAIN v8.0
SDIV            PLAIN v8.0
LSLV            PLAIN v8.0
LSRV            PLAIN v8.0
ASRV            PLAIN v8.0
RORV            PLAIN v8.0
CRC32           PLAIN FEAT_CRC32
CRC32C          PLAIN FEAT_CRC32
SUBP            PLAIN FEAT_MTE
SUBPS           PLAIN FEAT_MTE
IRG             PLAIN FEAT_MTE
GMI             PLAIN FEAT_MTE
PACGA           PLAIN FEAT_PAuth
SMAX            PLAIN FEAT_CSSC
SMIN            PLAIN FEAT_CSSC
UMAX            PLAIN FEAT_CSSC
UMIN            PLAIN FEAT_CSSC
RBIT            PLAIN v8.0
REV16           PLAIN v8.0
REV32           PLAIN v8.0
REV64           PLAIN v8.0
CLZ             PLAIN v8.0
CLS             PLAIN v8.0
CTZ             PLAIN FEAT_CSSC
CNT             PLAIN FEAT_CSSC
ABS             PLAIN FEAT_CSSC
PACIA           PLAIN z==0|rn==31 FEAT_PAuth
PACIB           PLAIN z==0|rn==31 FEAT_PAuth
PACDA           PLAIN z==0|rn==31 FEAT_PAuth
PACDB           PLAIN z==0|rn==31 FEAT_PAuth
AUTIA           PLAIN z==0|rn==31 FEAT_PAuth
AUTIB           PLAIN z==0|rn==31 FEAT_PAuth
AUTDA           PLAIN z==0|rn==31 FEAT_PAuth
AUTDB           PLAIN z==0|rn==31 FEAT_PAuth
XPACI           PLAIN FEAT_PAuth
XPACD           PLAIN FEAT_PAuth
AND_r           PLAIN sf==1|sa<32 v8.0
ORR_r           PLAIN sf==1|sa<32 v8.0
EOR_r           PLAIN sf==1|sa<32 v8.0
ANDS_r          PLAIN sf==1|sa<32 v8.0
ADD_r           PLAIN st!=3 sf==1|sa<32 v8.0
SUB_r           PLAIN st!=3 sf==1|sa<32 v8.0
ADDS_r          PLAIN st!=3 sf==1|sa<32 v8.0
SUBS_r          PLAIN st!=3 sf==1|sa<32 v8.0
ADD_ext         PLAIN sa<=4 v8.0
SUB_ext         PLAIN sa<=4 v8.0
ADDS_ext        PLAIN sa<=4 v8.0
SUBS_ext        PLAIN sa<=4 v8.0
ADC             PLAIN v8.0
ADCS            PLAIN v8.0
SBC             PLAIN v8.0
SBCS            PLAIN v8.0
RMIF            PLAIN FEAT_FlagM
SETF8           PLAIN FEAT_FlagM
SETF16          PLAIN FEAT_FlagM
CCMP            PLAIN v8.0
CSEL            PLAIN v8.0
MADD_w          PLAIN v8.0
MSUB_w          PLAIN v8.0
MADD_x          PLAIN v8.0
MSUB_x          PLAIN v8.0
SMADDL          PLAIN v8.0
SMSUBL          PLAIN v8.0
UMADDL          PLAIN v8.0
UMSUBL          PLAIN v8.0
SMULH           PLAIN v8.0
UMULH           PLAIN v8.0
AESE            PLAIN FEAT_AES
AESD            PLAIN FEAT_AES
AESMC           PLAIN FEAT_AES
AESIMC          PLAIN FEAT_AES
SHA1C           PLAIN FEAT_SHA1
SHA1P           PLAIN FEAT_SHA1
SHA1M           PLAIN FEAT_SHA1
SHA1SU0         PLAIN FEAT_SHA1
SHA256H         PLAIN FEAT_SHA256
SHA256H2        PLAIN FEAT_SHA256
SHA256SU1       PLAIN FEAT_SHA256
SHA1H           PLAIN FEAT_SHA1
SHA1SU1         PLAIN FEAT_SHA1
SHA256SU0       PLAIN FEAT_SHA256
SHA512H         PLAIN FEAT_SHA512
SHA512H2        PLAIN FEAT_SHA512
SHA512SU1       PLAIN FEAT_SHA512
RAX1            PLAIN FEAT_SHA3
SM3PARTW1       PLAIN FEAT_SM3
SM3PARTW2       PLAIN FEAT_SM3
SM4EKEY         PLAIN FEAT_SM4
SHA512SU0       PLAIN FEAT_SHA512
SM4E            PLAIN FEAT_SM4
EOR3            PLAIN FEAT_SHA3
BCAX            PLAIN FEAT_SHA3
SM3SS1          PLAIN FEAT_SM3
SM3TT1A         PLAIN FEAT_SM3
SM3TT1B         PLAIN FEAT_SM3
SM3TT2A         PLAIN FEAT_SM3
SM3TT2B         PLAIN FEAT_SM3
XAR             PLAIN FEAT_SHA3
DUP_element_s   PLAIN imm&15!=0 v8.0
DUP_element_v   PLAIN imm&15!=0 imm&7!=0|q==1 v8.0
DUP_general     PLAIN imm&15!=0 imm&7!=0|q==1 v8.0
INS_general     PLAIN imm&15!=0 v8.0
SMOV            PLAIN imm&7!=0 imm&3!=0|q==1 v8.0
UMOV            PLAIN imm&15!=0 imm&7!=0|q==1 imm&7==0|q==0 v8.0
INS_element     PLAIN di&15!=0 v8.0
FADD_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FSUB_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FDIV_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMUL_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FNMUL_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMAX_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMIN_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMAXNM_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMINNM_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMULX_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMEQ_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMGE_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMGT_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FACGE_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FACGT_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FABD_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRECPS_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRSQRTS_s       PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
SQADD_s         PLAIN v8.0
UQADD_s         PLAIN v8.0
SQSUB_s         PLAIN v8.0
UQSUB_s         PLAIN v8.0
SUQADD_s        PLAIN v8.0
USQADD_s        PLAIN v8.0
SSHL_s          PLAIN v8.0
USHL_s          PLAIN v8.0
SRSHL_s         PLAIN v8.0
URSHL_s         PLAIN v8.0
SQSHL_s         PLAIN v8.0
UQSHL_s         PLAIN v8.0
SQRSHL_s        PLAIN v8.0
UQRSHL_s        PLAIN v8.0
ADD_s           PLAIN v8.0
SUB_s           PLAIN v8.0
CMGT_s          PLAIN v8.0
CMHI_s          PLAIN v8.0
CMGE_s          PLAIN v8.0
CMHS_s          PLAIN v8.0
CMTST_s         PLAIN v8.0
CMEQ_s          PLAIN v8.0
SQDMULH_s       PLAIN esz!=0 esz!=3 v8.0
SQRDMULH_s      PLAIN esz!=0 esz!=3 v8.0
SQRDMLAH_s      PLAIN esz!=0 esz!=3 FEAT_RDM
SQRDMLSH_s      PLAIN esz!=0 esz!=3 FEAT_RDM
SQDMULL_si      PLAIN v8.0
SQDMLAL_si      PLAIN v8.0
SQDMLSL_si      PLAIN v8.0
FADDP_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMAXP_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMINP_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMAXNMP_s       PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMINNMP_s       PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
ADDP_s          PLAIN v8.0
FADD_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FSUB_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FDIV_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMUL_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMAX_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMIN_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMAXNM_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMINNM_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMULX_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMLA_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMLS_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMLAL_v         PLAIN FEAT_FHM
FMLSL_v         PLAIN FEAT_FHM
FMLAL2_v        PLAIN FEAT_FHM
FMLSL2_v        PLAIN FEAT_FHM
FCMEQ_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMGE_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMGT_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FACGE_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FACGT_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FABD_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRECPS_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRSQRTS_v       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FADDP_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMAXP_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMINP_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMAXNMP_v       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMINNMP_v       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
ADDP_v          PLAIN q==1|esz!=3 v8.0
SMAXP_v         PLAIN esz!=3 v8.0
SMINP_v         PLAIN esz!=3 v8.0
UMAXP_v         PLAIN esz!=3 v8.0
UMINP_v         PLAIN esz!=3 v8.0
AND_v           PLAIN v8.0
BIC_v           PLAIN v8.0
ORR_v           PLAIN v8.0
ORN_v           PLAIN v8.0
EOR_v           PLAIN v8.0
BSL_v           PLAIN v8.0
BIT_v           PLAIN v8.0
BIF_v           PLAIN v8.0
SQADD_v         PLAIN q==1|esz!=3 v8.0
UQADD_v         PLAIN q==1|esz!=3 v8.0
SQSUB_v         PLAIN q==1|esz!=3 v8.0
UQSUB_v         PLAIN q==1|esz!=3 v8.0
SUQADD_v        PLAIN q==1|esz!=3 v8.0
USQADD_v        PLAIN q==1|esz!=3 v8.0
SSHL_v          PLAIN q==1|esz!=3 v8.0
USHL_v          PLAIN q==1|esz!=3 v8.0
SRSHL_v         PLAIN q==1|esz!=3 v8.0
URSHL_v         PLAIN q==1|esz!=3 v8.0
SQSHL_v         PLAIN q==1|esz!=3 v8.0
UQSHL_v         PLAIN q==1|esz!=3 v8.0
SQRSHL_v        PLAIN q==1|esz!=3 v8.0
UQRSHL_v        PLAIN q==1|esz!=3 v8.0
ADD_v           PLAIN q==1|esz!=3 v8.0
SUB_v           PLAIN q==1|esz!=3 v8.0
CMGT_v          PLAIN q==1|esz!=3 v8.0
CMHI_v          PLAIN q==1|esz!=3 v8.0
CMGE_v          PLAIN q==1|esz!=3 v8.0
CMHS_v          PLAIN q==1|esz!=3 v8.0
CMTST_v         PLAIN q==1|esz!=3 v8.0
CMEQ_v          PLAIN q==1|esz!=3 v8.0
SHADD_v         PLAIN esz!=3 v8.0
UHADD_v         PLAIN esz!=3 v8.0
SHSUB_v         PLAIN esz!=3 v8.0
UHSUB_v         PLAIN esz!=3 v8.0
SRHADD_v        PLAIN esz!=3 v8.0
URHADD_v        PLAIN esz!=3 v8.0
SMAX_v          PLAIN esz!=3 v8.0
UMAX_v          PLAIN esz!=3 v8.0
SMIN_v          PLAIN esz!=3 v8.0
UMIN_v          PLAIN esz!=3 v8.0
SABD_v          PLAIN esz!=3 v8.0
UABD_v          PLAIN esz!=3 v8.0
SABA_v          PLAIN esz!=3 v8.0
UABA_v          PLAIN esz!=3 v8.0
MUL_v           PLAIN esz!=3 v8.0
PMUL_v          PLAIN v8.0
MLA_v           PLAIN esz!=3 v8.0
MLS_v           PLAIN esz!=3 v8.0
SQDMULH_v       PLAIN esz!=0 esz!=3 v8.0
SQRDMULH_v      PLAIN esz!=0 esz!=3 v8.0
SQRDMLAH_v      PLAIN esz!=0 esz!=3 FEAT_RDM
SQRDMLSH_v      PLAIN esz!=0 esz!=3 FEAT_RDM
SDOT_v          PLAIN FEAT_DotProd
UDOT_v          PLAIN FEAT_DotProd
USDOT_v         PLAIN FEAT_I8MM
BFDOT_v         PLAIN FEAT_BF16
BFMLAL_v        PLAIN FEAT_BF16
BFMMLA          PLAIN FEAT_BF16
SMMLA           PLAIN FEAT_I8MM
UMMLA           PLAIN FEAT_I8MM
USMMLA          PLAIN FEAT_I8MM
FCADD_90        PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FCMA
FCADD_270       PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FCMA
FCMLA_v         PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FCMA
SMULL_v         PLAIN esz!=3 v8.0
UMULL_v         PLAIN esz!=3 v8.0
SMLAL_v         PLAIN esz!=3 v8.0
UMLAL_v         PLAIN esz!=3 v8.0
SMLSL_v         PLAIN esz!=3 v8.0
UMLSL_v         PLAIN esz!=3 v8.0
SADDL_v         PLAIN esz!=3 v8.0
UADDL_v         PLAIN esz!=3 v8.0
SSUBL_v         PLAIN esz!=3 v8.0
USUBL_v         PLAIN esz!=3 v8.0
SABAL_v         PLAIN esz!=3 v8.0
UABAL_v         PLAIN esz!=3 v8.0
SABDL_v         PLAIN esz!=3 v8.0
UABDL_v         PLAIN esz!=3 v8.0
SQDMULL_v       PLAIN v8.0
SQDMLAL_v       PLAIN v8.0
SQDMLSL_v       PLAIN v8.0
SADDW           PLAIN esz!=3 v8.0
UADDW           PLAIN esz!=3 v8.0
SSUBW           PLAIN esz!=3 v8.0
USUBW           PLAIN esz!=3 v8.0
ADDHN           PLAIN esz!=3 v8.0
RADDHN          PLAIN esz!=3 v8.0
SUBHN           PLAIN esz!=3 v8.0
RSUBHN          PLAIN esz!=3 v8.0
PMULL_p8        PLAIN v8.0
PMULL_p64       PLAIN FEAT_PMULL
FAMAX           PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FAMINMAX
FAMIN           PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FAMINMAX
FSCALE          PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FP8
FCVTN_bh        PLAIN FEAT_FP8
FCVTN_bs        PLAIN FEAT_FP8
FMLAL_hb_v      PLAIN FEAT_FP8FMA
FMLALL_sb_v     PLAIN FEAT_FP8FMA
FDOT_sb_v       PLAIN FEAT_FP8DOT4
FDOT_hb_v       PLAIN FEAT_FP8DOT2
FMMLA_sb        PLAIN FEAT_F8F32MM
FMMLA_hb        PLAIN FEAT_F8F16MM
FMUL_si         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMLA_si         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMLS_si         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMULX_si        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
SQDMULH_si      PLAIN v8.0
SQRDMULH_si     PLAIN v8.0
SQRDMLAH_si     PLAIN FEAT_RDM
SQRDMLSH_si     PLAIN FEAT_RDM
FMUL_vi         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMLA_vi         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMLS_vi         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMULX_vi        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FMLAL_vi        PLAIN FEAT_FHM
FMLSL_vi        PLAIN FEAT_FHM
FMLAL2_vi       PLAIN FEAT_FHM
FMLSL2_vi       PLAIN FEAT_FHM
MUL_vi          PLAIN v8.0
MLA_vi          PLAIN v8.0
MLS_vi          PLAIN v8.0
SQDMULH_vi      PLAIN v8.0
SQRDMULH_vi     PLAIN v8.0
SQRDMLAH_vi     PLAIN FEAT_RDM
SQRDMLSH_vi     PLAIN FEAT_RDM
SDOT_vi         PLAIN FEAT_DotProd
UDOT_vi         PLAIN FEAT_DotProd
SUDOT_vi        PLAIN FEAT_I8MM
USDOT_vi        PLAIN FEAT_I8MM
BFDOT_vi        PLAIN FEAT_BF16
BFMLAL_vi       PLAIN FEAT_BF16
FCMLA_vi        PLAIN FEAT_FP16(esz==1) FEAT_FCMA
SMULL_vi        PLAIN v8.0
UMULL_vi        PLAIN v8.0
SMLAL_vi        PLAIN v8.0
UMLAL_vi        PLAIN v8.0
SMLSL_vi        PLAIN v8.0
UMLSL_vi        PLAIN v8.0
SQDMULL_vi      PLAIN v8.0
SQDMLAL_vi      PLAIN v8.0
SQDMLSL_vi      PLAIN v8.0
FMLAL_hb_vi     PLAIN FEAT_FP8FMA
FMLALL_sb_vi    PLAIN FEAT_FP8FMA
FDOT_sb_vi      PLAIN FEAT_FP8DOT4
FDOT_hb_vi      PLAIN FEAT_FP8DOT2
FCSEL           PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMADD           PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMSUB           PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FNMADD          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FNMSUB          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
EXT_d           PLAIN v8.0
EXT_q           PLAIN v8.0
TBL_TBX         PLAIN v8.0
LUTI2_1b        PLAIN FEAT_LUT
LUTI2_1h        PLAIN FEAT_LUT
LUTI4_1b        PLAIN FEAT_LUT
LUTI4_2h        PLAIN FEAT_LUT
UZP1            PLAIN q==1|esz!=3 v8.0
UZP2            PLAIN q==1|esz!=3 v8.0
TRN1            PLAIN q==1|esz!=3 v8.0
TRN2            PLAIN q==1|esz!=3 v8.0
ZIP1            PLAIN q==1|esz!=3 v8.0
ZIP2            PLAIN q==1|esz!=3 v8.0
ADDV            PLAIN esz!=3 q==1|esz!=2 v8.0
SADDLV          PLAIN esz!=3 q==1|esz!=2 v8.0
UADDLV          PLAIN esz!=3 q==1|esz!=2 v8.0
SMAXV           PLAIN esz!=3 q==1|esz!=2 v8.0
UMAXV           PLAIN esz!=3 q==1|esz!=2 v8.0
SMINV           PLAIN esz!=3 q==1|esz!=2 v8.0
UMINV           PLAIN esz!=3 q==1|esz!=2 v8.0
FMAXNMV_h       PLAIN FEAT_FP16
FMAXNMV_s       PLAIN v8.0
FMINNMV_h       PLAIN FEAT_FP16
FMINNMV_s       PLAIN v8.0
FMAXV_h         PLAIN FEAT_FP16
FMAXV_s         PLAIN v8.0
FMINV_h         PLAIN FEAT_FP16
FMINV_s         PLAIN v8.0
SCVTF_g         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
UCVTF_g         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTZS_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTZU_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTNS_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTNU_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTPS_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTPU_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTMS_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTMU_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTAS_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTAU_g        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
SCVTF_simd      PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
UCVTF_simd      PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTAS_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTAU_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTMS_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTMU_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTNS_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTNU_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTPS_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTPU_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTZS_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FCVTZU_g_simd   PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FPRCVT
FJCVTZS         PLAIN FEAT_JSCVT
FMOV_ws         PLAIN v8.0
FMOV_sw         PLAIN v8.0
FMOV_xd         PLAIN v8.0
FMOV_dx         PLAIN v8.0
FMOV_xu         PLAIN v8.0
FMOV_ux         PLAIN v8.0
FMOV_xh         PLAIN FEAT_FP16
FMOV_hx         PLAIN FEAT_FP16
FMOV_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FABS_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FNEG_s          PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FSQRT_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTN_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTP_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTM_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTZ_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTA_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTX_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRINTI_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
BFCVT_s         PLAIN FEAT_BF16
FRINT32Z_s      PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FRINTTS
FRINT32X_s      PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FRINTTS
FRINT64Z_s      PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FRINTTS
FRINT64X_s      PLAIN esz!=0 FEAT_FP16(esz==1) FEAT_FRINTTS
FCVT_s_ds       PLAIN v8.0
FCVT_s_hs       PLAIN v8.0
FCVT_s_sd       PLAIN v8.0
FCVT_s_hd       PLAIN v8.0
FCVT_s_sh       PLAIN v8.0
FCVT_s_dh       PLAIN v8.0
FMOVI_s         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMP            PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCCMP           PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FMOVI_v_h       PLAIN FEAT_FP16
Vimm            PLAIN cmode!=15|op==0|q==1 v8.0
SSHR_v          PLAIN v8.0
USHR_v          PLAIN v8.0
SSRA_v          PLAIN v8.0
USRA_v          PLAIN v8.0
SRSHR_v         PLAIN v8.0
URSHR_v         PLAIN v8.0
SRSRA_v         PLAIN v8.0
URSRA_v         PLAIN v8.0
SRI_v           PLAIN v8.0
SHL_v           PLAIN v8.0
SLI_v           PLAIN v8.0
SSHLL_v         PLAIN v8.0
USHLL_v         PLAIN v8.0
SHRN_v          PLAIN v8.0
RSHRN_v         PLAIN v8.0
SQSHL_vi        PLAIN v8.0
UQSHL_vi        PLAIN v8.0
SQSHLU_vi       PLAIN v8.0
SQSHRN_v        PLAIN v8.0
UQSHRN_v        PLAIN v8.0
SQSHRUN_v       PLAIN v8.0
SQRSHRN_v       PLAIN v8.0
UQRSHRN_v       PLAIN v8.0
SQRSHRUN_v      PLAIN v8.0
SSHR_s          PLAIN v8.0
USHR_s          PLAIN v8.0
SSRA_s          PLAIN v8.0
USRA_s          PLAIN v8.0
SRSHR_s         PLAIN v8.0
URSHR_s         PLAIN v8.0
SRSRA_s         PLAIN v8.0
URSRA_s         PLAIN v8.0
SRI_s           PLAIN v8.0
SHL_s           PLAIN v8.0
SLI_s           PLAIN v8.0
SQSHL_si        PLAIN v8.0
UQSHL_si        PLAIN v8.0
SQSHLU_si       PLAIN v8.0
SQSHRN_si       PLAIN v8.0
UQSHRN_si       PLAIN v8.0
SQSHRUN_si      PLAIN v8.0
SQRSHRN_si      PLAIN v8.0
UQRSHRN_si      PLAIN v8.0
SQRSHRUN_si     PLAIN v8.0
SQABS_s         PLAIN v8.0
SQNEG_s         PLAIN v8.0
ABS_s           PLAIN v8.0
NEG_s           PLAIN v8.0
CMGT0_s         PLAIN v8.0
CMGE0_s         PLAIN v8.0
CMEQ0_s         PLAIN v8.0
CMLE0_s         PLAIN v8.0
CMLT0_s         PLAIN v8.0
SQXTUN_s        PLAIN esz!=3 v8.0
SQXTN_s         PLAIN esz!=3 v8.0
UQXTN_s         PLAIN esz!=3 v8.0
FCVTXN_s        PLAIN v8.0
FCMGT0_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMGE0_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMEQ0_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMLE0_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCMLT0_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRECPE_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRECPX_s        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FRSQRTE_s       PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
SCVTF_f         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
UCVTF_f         PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTNS_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTNU_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTPS_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTPU_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTMS_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTMU_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTZS_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTZU_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTAS_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
FCVTAU_f        PLAIN esz!=0 v8.0 FEAT_FP16(esz==1)
SQABS_v         PLAIN q==1|esz!=3 v8.0
SQNEG_v         PLAIN q==1|esz!=3 v8.0
ABS_v           PLAIN q==1|esz!=3 v8.0
NEG_v           PLAIN q==1|esz!=3 v8.0
CLS_v           PLAIN esz!=3 v8.0
CLZ_v           PLAIN esz!=3 v8.0
CNT_v           PLAIN v8.0
NOT_v           PLAIN v8.0
RBIT_v          PLAIN v8.0
CMGT0_v         PLAIN q==1|esz!=3 v8.0
CMGE0_v         PLAIN q==1|esz!=3 v8.0
CMEQ0_v         PLAIN q==1|esz!=3 v8.0
CMLE0_v         PLAIN q==1|esz!=3 v8.0
CMLT0_v         PLAIN q==1|esz!=3 v8.0
REV16_v         PLAIN v8.0
REV32_v         PLAIN v8.0
REV64_v         PLAIN esz!=3 v8.0
SADDLP_v        PLAIN esz!=3 v8.0
UADDLP_v        PLAIN esz!=3 v8.0
SADALP_v        PLAIN esz!=3 v8.0
UADALP_v        PLAIN esz!=3 v8.0
XTN             PLAIN esz!=3 v8.0
SQXTUN_v        PLAIN esz!=3 v8.0
SQXTN_v         PLAIN esz!=3 v8.0
UQXTN_v         PLAIN esz!=3 v8.0
FCVTN_v         PLAIN v8.0
FCVTXN_v        PLAIN v8.0
BFCVTN_v        PLAIN FEAT_BF16
SHLL_v          PLAIN esz!=3 v8.0
FABS_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FNEG_v          PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FSQRT_v         PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTN_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTM_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTP_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTZ_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTA_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTX_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINTI_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRINT32Z_v      PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FRINTTS
FRINT32X_v      PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FRINTTS
FRINT64Z_v      PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FRINTTS
FRINT64X_v      PLAIN esz!=0 q==1|esz!=3 FEAT_FP16(esz==1) FEAT_FRINTTS
SCVTF_vi        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
UCVTF_vi        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTNS_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTNU_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTPS_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTPU_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTMS_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTMU_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTZS_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTZU_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTAS_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTAU_vi       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMGT0_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMGE0_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMEQ0_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMLE0_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCMLT0_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRECPE_v        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FRSQRTE_v       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
URECPE_v        PLAIN v8.0
URSQRTE_v       PLAIN v8.0
FCVTL_v         PLAIN v8.0
F1CVTL          PLAIN FEAT_FP8
F2CVTL          PLAIN FEAT_FP8
BF1CVTL         PLAIN FEAT_FP8
BF2CVTL         PLAIN FEAT_FP8
SCVTF_vf        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
UCVTF_vf        PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTZS_vf       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
FCVTZU_vf       PLAIN esz!=0 q==1|esz!=3 v8.0 FEAT_FP16(esz==1)
