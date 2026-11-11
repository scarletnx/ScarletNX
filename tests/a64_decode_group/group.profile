# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The profile of group.decode (the format: src/common/cpu/aarch64/decode/a64.profile).

feature v8.0 FEAT_AES

target base v8.0
target max *

# refuses rd 31
FIRST   PLAIN rd!=31 v8.0
# needs FEAT_AES from rd 30 on
SECOND  PLAIN FEAT_AES(rd>=30) v8.0
THIRD   B nop v8.0
ALONE   PLAIN rd!=31 v8.0
EDGE_8   PLAIN v8.0
EDGE_9   PLAIN v8.0
EDGE_U16 PLAIN v8.0
EDGE_S16 PLAIN v8.0
EDGE_U17 PLAIN v8.0
EDGE_S17 PLAIN v8.0
EDGE_K   PLAIN v8.0
EDGE_F   PLAIN v8.0
