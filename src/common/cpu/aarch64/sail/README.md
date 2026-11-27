<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The A64 instructions' Sail

`a64.sail` is the part of [sail-arm](https://github.com/rems-project/sail-arm)'s Armv9.4-A model that the generated
subsets need (`arm-v9.4-a/src/instrs64.sail` and `v8_base.sail` at commit
`5416ef6b43e9527c38dc71fb752259c1d01a47e1`), copied as it is. sail-arm is the Arm architecture's ASL translated to
Sail; it is BSD 3-Clause Clear (copyright Arm Limited and its authors), in `LICENSE` beside this file.

`a64.roots` names what is copied: the decode functions of each subset (`root`), the names the generator knows or
lowers itself, the state no instruction there reaches, and the NX's fixups, which are not followed (`stop`), and the
functions the back end lowers, of which only the `val` is copied (`lower`: the floating-point arithmetic). Every A64
decode clause is also copied alone (`encode __DecodeA64`), for the encoders of the back end that writes A64 itself
(`tools/sailgen` `--encoders`).
`tools/sailgen --extract` copies every definition the roots reach, in sail-arm's order. To update it, fetch the files of
a newer commit and run, from the repository's root:

```sh
sailgen --extract src/common/cpu/aarch64/sail/a64.roots -o src/common/cpu/aarch64/sail/a64.sail \
    SAIL_ARM/arm-v9.4-a/src/instrs64.sail SAIL_ARM/arm-v9.4-a/src/v8_base.sail
```

and change the commit above. A new subset is its decode functions added to `a64.roots` and the same command.

`nx.sail` is ScarletNX's own (GPL-3.0-or-later): the NX's fixups, each with why. The generator reads it after
`a64.sail`, and its definitions take the place of `a64.sail`'s of the same name: the general-purpose registers as
ScarletNX's CPU keeps them (one array, `GPR`), EL0's stack pointer and exception level, no EL2, no SError, no Guarded
Control Stack, no guarded page, no Statistical Profiling, PAuth with its keys disabled, XPAC on 39-bit addresses and
PACGA's fixed mix; for the loads and stores, an access descriptor of EL0's, a pair as one access, little-endian
data, no alignment fault, a CONSTRAINED UNPREDICTABLE encoding run as it is written, and guest memory, the exclusive
monitor and the atomics as ScarletNX makes them (functions the back end lowers); and for the floating point, the
SIMD&FP registers as 128 bits each (`_V`) and FPCR as the CPU keeps them, floating point enabled at EL0, no merging of
a scalar result without FEAT_AFP, AArch64 alone, and FEAT_FP16 as the guest's. Another system's fixups (the 3DS's,
the NDS's) go in a file of their own the same way.

`tools/sailgen --generate` makes each machine's emitters from them (`snx_sailgen_a64_x64` of `cmake/sdm.cmake`,
`snx_sailgen_a64_arm64` and `snx_sailgen_a64_risc` of `cmake/sailgen.cmake`), with the features the decoder's `a64.profile` names: a feature it names nowhere is one no guest has, and the code behind it
folds away (TME, the Branch Record Buffer, BTI). The generated code is derived from `a64.sail` and carries its
license. `tools/sailgen/README.md` describes the generator.
