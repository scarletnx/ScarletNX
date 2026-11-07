<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The A32 and Thumb decoders' sources

`a32.decode`, `a32-uncond.decode`, `t32.decode` and `t16.decode` are QEMU's files of `target/arm/tcg/`, unmodified,
from QEMU commit `a19f59a72d96056d9a8f48cf6c77f5787491e414`, the commit of `a64.decode`
(`src/common/cpu/aarch64/decode/`). They are LGPL-2.1-or-later (Copyright (c) 2019 Linaro, Ltd), their notice at
the top of each file. To update them, copy the files again from a newer QEMU and change the commit here.

The files say that QEMU's `scripts/decodetree.py` reads them. ScarletNX does not use that Python script:
`tools/decodetree` is a port of it in C. `src/common/cpu/arm/CMakeLists.txt` makes `decode-a32.inc`,
`decode-a32-uncond.inc`, `decode-t32.inc` and `decode-t16.inc` from them into the build folder, with the options
QEMU's build gives, and `decode-a32-args.inc` and `decode-a32-uncond-args.inc` (`--argsets`): the argument sets that
`t32.decode` and `t16.decode` name `!extern`. `a32.c` includes the A32 decoders and `thumb.c` the Thumb ones, and
each defines the `trans_` functions. The generated files are derived from the `.decode` files and carry their
license.
