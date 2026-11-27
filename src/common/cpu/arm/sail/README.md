<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The AArch32 instructions' Sail

`a32.sail` is the part of [sail-arm](https://github.com/rems-project/sail-arm)'s Armv9.4-A model that the generated
AArch32 subsets need (`arm-v9.4-a/src/instrs32.sail` and `v8_base.sail` at commit
`5416ef6b43e9527c38dc71fb752259c1d01a47e1`), copied as it is. sail-arm is the Arm architecture's ASL translated to
Sail; it is BSD 3-Clause Clear (copyright Arm Limited and its authors), in `LICENSE` beside this file. One Sail serves
every AArch32 guest: the GBA's ARM7TDMI (ARMv4T), the NDS's ARM946E-S (ARMv5TE), the 3DS's ARM11 MPCore (ARMv6K), the
Vita's Cortex-A9 (ARMv7-A) and a 32-bit Switch program's Cortex-A57 (ARMv8-A).

`a32.roots` names what is copied, as `a64.roots` does for A64: the decode functions of each subset (`root`), the
dispatcher each instruction set's clauses make (`decoder sg_decode_t16 __DecodeT16`), and the names the generator knows
itself and `arm.sail` defines, which are not followed (`stop`). To update it, fetch the files of a newer commit and run,
from the repository's root:

```sh
sailgen --extract src/common/cpu/arm/sail/a32.roots -o src/common/cpu/arm/sail/a32.sail \
    SAIL_ARM/arm-v9.4-a/src/instrs32.sail SAIL_ARM/arm-v9.4-a/src/v8_base.sail
```

and change the commit above. Today the subset is Thumb's 16-bit instructions, every `__DecodeT16` clause but HLT's
(halting debug is off) and UDF's: with no clause their halfwords stay UNDEFINED, UDF's whatever an IT block's
condition, as the Thumb decoder has them. A32's data processing (immediate, register, register-shifted register),
B, BL, BLX, BX, MOVW and MOVT come next (`decoder sg_decode_a32`): an A32 word none of their clauses takes stays the
block compiler's emitters written by hand (`mach_arm.inc`, `x64_arm.c`). The rest of A32, and T32, are the next.

The guests differ in two ways, each said once here:

- `a32.profile`, which sailgen reads with `--profile`: which encodings each architecture has (`since VERSION ROOT...`:
  the dispatcher tries those only on a guest of that `ArchVersion()` or later, so on an older one the word is no
  instruction, UNDEFINED), and the features of sail-arm's that some guest has (none of AArch32's yet).
- `arm.sail`, ScarletNX's own fixups (GPL-3.0-or-later), each with why. The generator reads it after `a32.sail`, and
  its definitions take the place of `a32.sail`'s of the same name: the registers as `SnxArmCPU` keeps them (`GPR`,
  user mode, no banking), the instruction set, ITSTATE and the condition as the block compiler decides them when it
  compiles a block, guest memory as the board's (`GuestRead`, `GuestWrite`, and the multiple transfers' whole range
  checked first, `GuestTransfer`), the hints, the exceptions, what an UNPREDICTABLE encoding does on each guest's core
  (`UnpredictableEncoding()`, each rule with its reason) and the guest's architecture (`ArchVersion()`, which a fixup
  that differs from one architecture to the next asks, as the ARMv7 ARM's ASL does: `LoadWritePC`, the ARM7TDMI's
  `STM`).

`tools/sailgen --generate` makes each machine's emitters from them (`snx_sailgen_a32_x64` of `cmake/sdm.cmake`,
`snx_sailgen_a32_arm64` and `snx_sailgen_a32_risc` of `cmake/sailgen.cmake`), with the machine's back end files, into
`a32_<machine>.inc`, which its block compiler includes. The generated code is derived from `a32.sail` and carries its
license. `tools/sailgen/README.md` describes the generator.
