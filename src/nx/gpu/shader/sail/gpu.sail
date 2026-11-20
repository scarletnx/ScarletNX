/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* What every GPU ISA's Sail shares (sailgen --gpu, tools/sailgen/README.md's "GPU mode"): the operations of a 32-bit
   word each of them has, which the back end (tools/sailgen/spirv.h) lowers as one instruction each (the truth tables
   of three inputs are lut3.sail's, tools/lut3gen.py's). Sail's own operators (+, -, *, &, |, @, slices, ZeroExtend, the comparisons of UInt and SInt)
   need no val */

/* the bits set */
val PopCount : bits(32) -> bits(32)
/* the bits in the other order */
val BitReverse32 : bits(32) -> bits(32)
/* the highest bit that is 1 (FindUMsb), 0xffffffff where there is none */
val HighestSetBit32 : bits(32) -> bits(32)
/* the highest bit that differs from the sign (FindSMsb), 0xffffffff where there is none */
val HighestSignBit32 : bits(32) -> bits(32)
/* ---- floats: an f32 is bits(32) to the Sail; these are the instructions SPIR-V has for it ---- */

/* the f32 of these bits */
val FConst : int -> bits(32)
/* the value's bits as an integer word (a bitcast of a float), and as a float */
val Bits32 : bits(32) -> bits(32)
val Float32 : bits(32) -> bits(32)
val FAbs : bits(32) -> bits(32)
val FNeg : bits(32) -> bits(32)
val FAdd : (bits(32), bits(32)) -> bits(32)
val FSub : (bits(32), bits(32)) -> bits(32)
val FMul : (bits(32), bits(32)) -> bits(32)
val FDiv : (bits(32), bits(32)) -> bits(32)
/* the number where one is NaN (GLSL's NMin, NMax), and FMin, FMax, which leave that undefined */
val FMinN : (bits(32), bits(32)) -> bits(32)
val FMaxN : (bits(32), bits(32)) -> bits(32)
val FMin : (bits(32), bits(32)) -> bits(32)
val FMax : (bits(32), bits(32)) -> bits(32)
/* to an integral value: 0 to the nearest even, 1 down, 2 up, 3 toward zero */
val FRound : (int, bits(32)) -> bits(32)
val FSin : bits(32) -> bits(32)
val FCos : bits(32) -> bits(32)
val FExp2 : bits(32) -> bits(32)
val FLog2 : bits(32) -> bits(32)
val FInvSqrt : bits(32) -> bits(32)
val FSqrt : bits(32) -> bits(32)
val FTanh : bits(32) -> bits(32)
val FIsNaN : bits(32) -> bool
/* a comparison of NAK's FloatCmpOp codes (spv_emit_fcompare's): 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 num, 8 nan, and
   9 to 14 the same unordered, 0 false, 15 true */
val FCompare : (int, bits(32), bits(32)) -> bool
val FToS32 : bits(32) -> bits(32)
val FToU32 : bits(32) -> bits(32)
val S32ToF : bits(32) -> bits(32)
val U32ToF : bits(32) -> bits(32)
/* of a 64-bit unsigned integer */
val U64ToF : bits(64) -> bits(32)
val FToU64 : bits(32) -> bits(64)
/* a pair of halves as two floats: the low and high halves of a word, or two floats; their components, abs and neg */
val UnpackHalf2 : bits(32) -> half2
val Half2At : (half2, int) -> bits(32)
val Half2 : (bits(32), bits(32)) -> half2
val Half2Abs : half2 -> half2
val Half2Neg : half2 -> half2
/* a field of a word at a run-time place: n bits from at, zero or sign extended */
val BitsU : (bits(32), bits(32), bits(32)) -> bits(32)
val BitsS : (bits(32), bits(32), bits(32)) -> bits(32)
/* x / y of signed words, toward zero */
val QuotS : (bits(32), bits(32)) -> bits(32)
/* a SPIR-V capability the instruction needs */
val Capability : int -> unit

/* ---- a subgroup's lanes ---- */
/* the value of lane, of the invocation's subgroup (OpGroupNonUniformShuffle) */
val Shuffle : (bits(32), bits(32)) -> bits(32)
/* the reduction (a SPIR-V OpGroupNonUniform opcode: IAdd, UMin, BitwiseAnd...) of x over clusters of 32 lanes */
val ClusterReduce32 : (int, bits(32)) -> bits(32)
/* a pair of halves' arithmetic, componentwise in f32: sum, product, GLSL's NMin and NMax, clamped to 0 to 1 */
val Half2Add : (half2, half2) -> half2
val Half2Mul : (half2, half2) -> half2
val Half2MinN : (half2, half2) -> half2
val Half2MaxN : (half2, half2) -> half2
val Half2Clamp01 : half2 -> half2
