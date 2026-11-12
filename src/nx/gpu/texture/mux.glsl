/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Entry i of a table of constant words, picked with selects and read from no memory: a bit of i halves the entries
   at each step, so a table of n costs n - 1 selects between constants and registers. For the builds that keep no
   arrays (SNX_NO_ARRAYS), where a table is read once a block: an array a shader indexes by a value is memory mobile
   GPUs are slow with, and a compiler copies a constant one into it first */
uint Mux8(uint i, uint c0, uint c1, uint c2, uint c3, uint c4, uint c5, uint c6, uint c7) {
    const bool b0 = (i & 1u) != 0u, b1 = (i & 2u) != 0u;
    const uint lo = b1 ? (b0 ? c3 : c2) : (b0 ? c1 : c0), hi = b1 ? (b0 ? c7 : c6) : (b0 ? c5 : c4);
    return (i & 4u) != 0u ? hi : lo;
}
uint Mux32(uint i, uint c0, uint c1, uint c2, uint c3, uint c4, uint c5, uint c6, uint c7, uint c8, uint c9, uint c10,
           uint c11, uint c12, uint c13, uint c14, uint c15, uint c16, uint c17, uint c18, uint c19, uint c20, uint c21,
           uint c22, uint c23, uint c24, uint c25, uint c26, uint c27, uint c28, uint c29, uint c30, uint c31) {
    const uint a = Mux8(i, c0, c1, c2, c3, c4, c5, c6, c7), b = Mux8(i, c8, c9, c10, c11, c12, c13, c14, c15);
    const uint c = Mux8(i, c16, c17, c18, c19, c20, c21, c22, c23), d = Mux8(i, c24, c25, c26, c27, c28, c29, c30, c31);
    return (i & 16u) != 0u ? ((i & 8u) != 0u ? d : c) : ((i & 8u) != 0u ? b : a);
}
