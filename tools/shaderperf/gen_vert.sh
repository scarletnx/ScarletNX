#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Regenerates fullscreen_vert.h from fullscreen.vert (glslc on PATH)
set -e
cd "$(dirname "$0")"
glslc -O --target-env=vulkan1.1 -mfmt=num -o fullscreen_vert.num fullscreen.vert
{
    echo "/* SPDX-License-Identifier: GPL-3.0-or-later"
    echo "   Copyright 2026 the ScarletNX contributors */"
    echo "/* fullscreen.vert compiled by gen_vert.sh: do not edit */"
    echo "#ifndef SNX_SHADERPERF_FULLSCREEN_VERT_H"
    echo "#define SNX_SHADERPERF_FULLSCREEN_VERT_H"
    echo "#include <stdint.h>"
    echo "static const uint32_t k_fullscreen_vert[] = {"
    cat fullscreen_vert.num
    echo "};"
    echo "#endif"
} > fullscreen_vert.h
rm fullscreen_vert.num
