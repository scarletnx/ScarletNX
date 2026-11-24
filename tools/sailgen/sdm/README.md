<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The SDM's errata

Overrides of the instruction pages `sailgen --sdm` cuts out of the Intel SDM, Volume 2 (325383-093US), where the
manual's Operation section, read as the specification the instruction selection derives from (`../README.md`, "The
x86-64 back end"), is wrong or can't be read as written. Each file is a page of only the sections it replaces, and says
which erratum it is; the pages stay the manual's words otherwise.

The numbers E-*n* are vibe86's (`docs/sdm-errata.md` of the vibe86 project, which compiles the same manual's
pseudocode into microcode and found them first); S-*n* are found here. A selection is only as right as its page, so an
erratum is not just a missing selection: CDQ's read as written was taken for a zero extension into EDX (E-17).

| # | Page | What the text says | What is meant | Handled by |
|---|---|---|---|---|
| E-1 | DIV | the remainder `AH := AX MOD SRC` after `AL := temp` has changed AX | the remainder of the original dividend | `DIV.sdm` |
| E-2 | IDIV | as E-1, and the range check `(temp > 7FH) or (temp < 80H)` on unsigned constants | signed comparisons | `IDIV.sdm` (32- and 64-bit) |
| E-4, E-18 | CMPXCHG | flags 'according to the comparison', none made; a register destination written back | CMP's flags; no write to a register | not read yet (`accumulator` is a word) |
| E-7 | CMOVcc | `CMOVG r64, r/m64`'s modes `V/N.E.` and `N/A` | `Valid`, `N.E.` | `isel.c` takes `V/N.E.` as valid |
| E-9 | IMUL | the product's signedness in a comment ('Signed multiplication') | a signed product | `sdm_code.c` reads the comment |
| E-10 | MOV | the segment registers' loading rules, after `DEST := SRC;`, introduced by a prose sentence | for the Sreg rows alone | `MOV.sdm` |
| E-13 | CMC | `EFLAGS.CF[bit 0] := NOT EFLAGS.CF[bit 0]` | `CF := NOT CF` | `CMC.sdm` |
| E-16 | SETcc | rows like `0F 94 \| SETE r/m8` without `/r` or `/0` | a ModRM byte, for the r/m operand | `isel.c` gives an r/m operand a ModRM byte |
| E-17 | CWD/CDQ/CQO | `DX := SignExtend(AX)` | DX:AX := SignExtend(AX): DX the sign alone | `CWD_CDQ_CQO.sdm` |
| S-0 | (Vol. 2A 3.1) | the notation chapter's example page `CMC—Complement Carry Flag [this is an example]`, before CMC's own | not an instruction's page | `sdm.c` skips a page so titled |
| S-1 | RCL/RCR/ROL/ROR | the four instructions' code in one sequence, told apart by comments | each under its instruction | `RCL_RCR_ROL_ROR.sdm` |
| S-2 | RCL/RCR/ROL/ROR | RCR's and ROR's `tempCF := LSB(SRC)` | `LSB(tempDEST)` | `RCL_RCR_ROL_ROR.sdm` |
| S-3 | RCL/RCR/ROL/ROR | `tempCF * 2SIZE` (a superscript lost) | 2^(SIZE - 1) for ROR, 2^SIZE for RCR | `RCL_RCR_ROL_ROR.sdm` |
| S-4 | RCL/RCR/ROL/ROR | `MSB − 1(tempDEST)`; stray `ELIHW;` lines | the bit below the top; nothing | `RCL_RCR_ROL_ROR.sdm` |
| S-6 | DIV, IDIV | (as E-1 and E-2) | | the overrides keep the pages' words; IDIV's 8- and 16-bit forms are left unread |
| S-7 | PINSRB/PINSRD/PINSRQ, PEXTRB/PEXTRD/PEXTRQ, PINSRW | (pdftotext) rows of neighbouring forms run into one, their cells sharing a line of the table | each form its own row | `PINSRB_PINSRD_PINSRQ.sdm`, `PEXTRB_PEXTRD_PEXTRQ.sdm`, `PINSRW.sdm` (legacy forms only) |
| S-8 | CVTDQ2PS, ROUNDPS | (pdftotext) CVTDQ2PS's table of one mode column read as opcodes alone; ROUNDPS's `/r ib` in the instruction cell | the legacy form's row | `CVTDQ2PS.sdm`, `ROUNDPS.sdm` |
| S-9 | PAVGB | the opcode `66 0F E0, /r`, a comma among the bytes | `66 0F E0 /r` | `isel.c` reads a comma in an opcode as a space |
| S-10 | PMOVSX, PMOVZX | the opcodes in lower case, `66 0f 38 20 /r` | `66 0F 38 20 /r` | `isel.c` reads `0f` as `0F` |
| S-5 | SAL/SAR/SHL/SHR, SARX/SHLX/SHRX | the division's signedness in a comment ('Signed divide, rounding toward negative infinity') | a signed division | `sdm_code.c` reads the comment |
