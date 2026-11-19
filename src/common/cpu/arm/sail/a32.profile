# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The AArch32 guests' profiles, which sailgen --generate reads with a32.sail: one Sail serves every architecture, and what
# each has is said here (the encodings) and in arm.sail's fixups (ArchVersion(), the guest's).
#
#   feature FEAT_X...         the features of sail-arm's that some guest has; one named nowhere no guest has, so
#                             IsFeatureImplemented of it is false when sailgen runs and the code behind it folds away
#   since VERSION ROOT...     the encodings ArchVersion() VERSION added, which the dispatcher tries only on a guest of
#                             that version or later: on an older one the word is no instruction (UNDEFINED)
#
# The guests: the GBA's ARM7TDMI (ARMv4T, 4), the NDS's ARM946E-S (ARMv5TE, 5), the 3DS's ARM11 MPCore (ARMv6K, 6), the
# Vita's Cortex-A9 (ARMv7-A, 7) and a 32-bit Switch program's Cortex-A57 (ARMv8-A, 8). Thumb-2 came with ARMv6T2, which
# no guest here is short of ARMv7: its encodings are since 7.

# Thumb: BKPT and BLX (register) are ARMv5T's, and so is the BLX suffix of the BL halves before Thumb-2 (arm.sail's)
since 5 decode_aarch32_instrs_BKPT_T1enc_A_txt decode_aarch32_instrs_BLX_r_T1enc_A_txt
since 5 decode_aarch32_legacy_BLX_suffix
# CPS, SETEND, the byte reversals and the extends are ARMv6's
since 6 decode_aarch32_instrs_CPS_T1enc_AS_txt decode_aarch32_instrs_SETEND_T1enc_A_txt
since 6 decode_aarch32_instrs_REV_T1enc_A_txt decode_aarch32_instrs_REV16_T1enc_A_txt decode_aarch32_instrs_REVSH_T1enc_A_txt
since 6 decode_aarch32_instrs_SXTB_T1enc_A_txt decode_aarch32_instrs_SXTH_T1enc_A_txt
since 6 decode_aarch32_instrs_UXTB_T1enc_A_txt decode_aarch32_instrs_UXTH_T1enc_A_txt
# CBZ, CBNZ, IT and the hints are Thumb-2's
since 7 decode_aarch32_instrs_CBNZ_T1enc_A_txt decode_aarch32_instrs_IT_T1enc_A_txt
since 7 decode_aarch32_instrs_NOP_T1enc_A_txt decode_aarch32_instrs_YIELD_T1enc_A_txt decode_aarch32_instrs_WFE_T1enc_A_txt
since 7 decode_aarch32_instrs_WFI_T1enc_A_txt decode_aarch32_instrs_SEV_T1enc_A_txt decode_aarch32_unallocated_hint_T1
# SEVL is ARMv8's
since 8 decode_aarch32_instrs_SEVL_T1enc_A_txt
# A32: BLX's forms are ARMv5T's, MOVW and MOVT came with ARMv6T2 (no guest short of ARMv7 here has them). A word the
# Sail takes only from a later version stays the block compiler's on an older guest (sg_emit_a32 hands it back)
since 5 decode_aarch32_instrs_BLX_r_A1enc_A_txt decode_aarch32_instrs_BL_i_A2enc_A_txt
since 7 decode_aarch32_instrs_MOV_i_A2enc_A_txt decode_aarch32_instrs_MOVT_A1enc_A_txt
