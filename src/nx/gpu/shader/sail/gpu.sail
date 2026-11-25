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

/* a * b + c rounded once (GLSL.std.450's Fma), and the derivatives along the screen's x and y (a fragment's) */
val FFma : (bits(32), bits(32), bits(32)) -> bits(32)
val FDdx : bits(32) -> bits(32)
val FDdy : bits(32) -> bits(32)

/* ---- integers that keep their type: a translator whose words are each an i32 or a u32 (the USSE's operands, as
   their register's type reads them) makes an operation of the operands' own type, no bitcast; the Sail chooses a
   signed or an unsigned comparison ---- */
/* the i32 and the u32 of this value */
val SConst : int -> bits(32)
val UConst : int -> bits(32)
val IAddT : (bits(32), bits(32)) -> bits(32)
val ISubT : (bits(32), bits(32)) -> bits(32)
val IMulT : (bits(32), bits(32)) -> bits(32)
val INegT : bits(32) -> bits(32)
val AndT : (bits(32), bits(32)) -> bits(32)
val OrT : (bits(32), bits(32)) -> bits(32)
val XorT : (bits(32), bits(32)) -> bits(32)
val ShlT : (bits(32), bits(32)) -> bits(32)
val ShrT : (bits(32), bits(32)) -> bits(32)
val AsrT : (bits(32), bits(32)) -> bits(32)
/* GLSL.std.450's SAbs of an i32 */
val SAbsT : bits(32) -> bits(32)
val IEqT : (bits(32), bits(32)) -> bool
val INeT : (bits(32), bits(32)) -> bool
val SLtT : (bits(32), bits(32)) -> bool
val SLeT : (bits(32), bits(32)) -> bool
val SGtT : (bits(32), bits(32)) -> bool
val SGeT : (bits(32), bits(32)) -> bool
val ULtT : (bits(32), bits(32)) -> bool
val ULeT : (bits(32), bits(32)) -> bool
val UGtT : (bits(32), bits(32)) -> bool
val UGeT : (bits(32), bits(32)) -> bool
/* a value as an i32 and as a u32: a bitcast, where it has the other type */
val AsI32 : bits(32) -> bits(32)
val AsU32 : bits(32) -> bits(32)

/* ---- vectors of floats (vec3, vec4) and of booleans (bvec4): what PICA's registers and the USSE's operands are ---- */
/* the vec4 of four floats' bits, constant; of four floats; of one float four times */
val V4Const : (int, int, int, int) -> vec4
val V4Of : (bits(32), bits(32), bits(32), bits(32)) -> vec4
val V4Splat : bits(32) -> vec4
/* component k (0-3) */
val V4At : (vec4, int) -> bits(32)
/* four components of a (0-3) and b (4-7) */
val V4Shuffle : (vec4, vec4, int, int, int, int) -> vec4
/* the first three components; the vec2 and vec3 of two and three floats */
val V3Of4 : vec4 -> vec3
val V2Of : (bits(32), bits(32)) -> vec2
val V3Of : (bits(32), bits(32), bits(32)) -> vec3
val V4FAdd : (vec4, vec4) -> vec4
val V4FMul : (vec4, vec4) -> vec4
val V4FNeg : vec4 -> vec4
val V4FAbs : vec4 -> vec4
val V4Floor : vec4 -> vec4
/* FMin and FMax, which leave a NaN's result undefined */
val V4FMin : (vec4, vec4) -> vec4
val V4FMax : (vec4, vec4) -> vec4
val V4Dot : (vec4, vec4) -> bits(32)
val V3Dot : (vec3, vec3) -> bits(32)
/* FCompare of each component; a's components where c's hold, else b's; a bool four times */
val V4FCompare : (int, vec4, vec4) -> bvec4
val V4Select : (bvec4, vec4, vec4) -> vec4
val B4Splat : bool -> bvec4

/* ---- a sampled image's texels: the vec4 at a coordinate (a vec2 or a vec3), at the implicit level of detail, biased
   from it, at a level of detail, and with the coordinate's gradients (a vec2 or a vec3 each) ---- */
val SampleImplicit : (sampler, vec2) -> vec4
val SampleBias : (sampler, vec2, bits(32)) -> vec4
val SampleLod : (sampler, vec2, bits(32)) -> vec4
val SampleGrad : (sampler, vec2, vec2, vec2) -> vec4
