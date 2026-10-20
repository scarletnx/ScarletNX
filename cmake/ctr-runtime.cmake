# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Embed the project's PIC semantic-helper archive in an offline compiler: the 3DS driver's, or the one SYMBOL and
# HEADER name (the AGB board's). Installed tools need neither a build-tree path nor a separately located SDK archive.
if(NOT DEFINED SYMBOL)
    set(SYMBOL snx_ctr_aot_runtime)
    set(HEADER 3ds/aot/ctr_runtime.h)
endif()
file(READ "${INPUT}" data HEX)
string(REGEX REPLACE "(..)" "0x\\1," data "${data}")
file(WRITE "${OUTPUT}" "/* Generated from snx_arm_rt; do not edit. */\n#include \"${HEADER}\"\nconst unsigned char ${SYMBOL}[] = {${data}};\nconst size_t ${SYMBOL}_size = sizeof ${SYMBOL};\n")
