/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The SGX543's USSE instructions in Sail (sailgen --gpu, tools/sailgen/README.md's "GPU mode"): what each computes,
   from the fields of its 64-bit word that gpu/usse.decode names it by, as Vita3K's translator does (usse.c's
   handlers were its port). This file is the state every family shares; usse_<family>.sail holds a family's
   instructions, with its roots (usse_<family>.roots, which usse.roots includes) and its back end
   (usse_<family>_spirv.h; usse_spirv.h is this file's).

   The back end keeps usse.c's operand model (usse_decode_helpers.cpp's): an instruction's operands are slots
   (DEST, SRC0, SRC1, SRC2 and four more), each a register of a bank, its number, its swizzle, its modifiers and the
   type its components are read in, which the Sail decodes from the fields into them; the values read are slots too
   (A, B, C, R and four more), up to four components of one kind (float, unsigned or signed: an f32, a u32 or an i32),
   which the Sail reads, computes with and writes back component by component. A register read or written is
   usse.c's load and store: its banks, its packed types (two halves, four bytes, three C10s a word) and its
   conversions, and the repeats' offsets (repeat_offset) */

/* bits lo to lo + n - 1 of the instruction (n at most 32), known when the translator runs */
val Field : forall 'n. (int, int('n)) -> bits('n)
/* the opcode's place in its roots line */
val Form : unit -> int
/* the translation of the program failed (an operand it can't read or write, the first reason kept): what is left of
   the instruction is not written */
val Failed : unit -> bool
/* an instruction the translator refuses: why; and the end of one whose translation failed already */
val Refuse : string -> unit
val Stop : unit -> unit
/* the program is a fragment program, and its translation the secondary program's (once per draw, into SA) */
val Fragment : unit -> bool
val Secondary : unit -> bool

/* ---- the operands ---- */
/* operand k decoded from a register field: n, its bank field and its ext bit, doubled (64-bit registers: the number
   twice), and the field's bits (the internal registers are the last of the temporaries' numbers there):
   decode_dest's, decode_src0's (SRC0's banks) and decode_src12's (SRC1's and SRC2's) */
val Dest : (int, int, int, int, int, int) -> unit
val Src0 : (int, int, int, int, int, int) -> unit
val Src12 : (int, int, int, int, int, int) -> unit
/* operand k the internal register (FPINTERNAL) n, with flags, of type */
val Internal : (int, int, int, int) -> unit
/* operand k as operand j is */
val CopyOperand : (int, int) -> unit
/* operand k's type, its modifiers (F_ABS 1, F_NEG 2, F_GPI 4) set and added, its register number */
val SetType : (int, int) -> unit
val SetFlags : (int, int) -> unit
val AddFlags : (int, int) -> unit
val SetNum : (int, int) -> unit
/* operand k's swizzle: four channels (SW_X... SW_U), operand j's, decode_vec34_swizzle's (swizzle, extended, type 0
   scalar, 1 vec3, 2 vec4), and the twelve bits of three a channel (V32NMAD's) */
val SetSwizzle : (int, int, int, int, int) -> unit
val CopySwizzle : (int, int) -> unit
val Vec34Swizzle : (int, int, int, int) -> unit
val Swizzle4 : (int, int) -> unit
/* operand k's bank, register number, type and channel i, as they are now */
val BankOf : int -> int
val NumOf : int -> int
val TypeOf : int -> int
val ChannelOf : (int, int) -> int
/* decode_write_mask: a mask of the instruction's components as the destination's bank holds them (f16: halves) */
val WriteMask : (int, int, int) -> int

/* ---- the repeats: an operand's offset at repeat r (which: 0 SRC0, 1 SRC1, 2 SRC2, 3 DEST; mode: 0 external, 1
   internal, 2 both, 3 SMLSI's increments), and the multipliers of SMLSI's increments ---- */
val Rep : (int, int, int, int) -> int
val Multiplier : (int, int, int, int) -> unit
val ResetMultiplier : unit -> unit
/* SMLSI's and SMBO's increments: operand which's at repeat i */
val SetIncrease : (int, int, int) -> unit

/* ---- the values ---- */
/* value v read from operand k: the components mask enables (in order), offset by shift registers; written to
   operand k's components mask enables (a value of fewer components repeats its last), offset by shift */
val Load : (int, int, int, int) -> unit
val Store : (int, int, int, int) -> unit
/* value v's components and kind (K_FLOAT 0, K_UINT 1, K_INT 2) */
val Count : int -> int
val KindOf : int -> int
/* value v's component i (an f32, a u32 or an i32, as its kind), and x made it */
val At : (int, int) -> bits(32)
val Put : (int, int, bits(32)) -> unit
/* value v made n components of kind, x appended to it, its first n components alone kept, its kind made another (its components left as they are),
   and value w made as v is */
val Reset : (int, int, int) -> unit
val Push : (int, bits(32)) -> unit
val SetCount : (int, int) -> unit
val SetKind : (int, int) -> unit
val CopyValue : (int, int) -> unit
/* value v's components made floats from type's integers, and integers of type from floats, scaled to type's range
   where normal (usse_utilities.cpp: convert_to_float, convert_to_int) */
val ToFloat : (int, int, int) -> unit
val ToInt : (int, int, int) -> unit

/* the slots */
let DEST : int = 0
let SRC0 : int = 1
let SRC1 : int = 2
let SRC2 : int = 3
let A : int = 0
let B : int = 1
let C : int = 2
let R : int = 3

/* the banks (usse_types.h) */
let B_TEMP : int = 0
let B_PA : int = 1
let B_OUT : int = 2
let B_SA : int = 3
let B_INTERNAL : int = 4
let B_SPECIAL : int = 5
let B_GLOBAL : int = 6
let B_CONST : int = 7
let B_IMM : int = 8
/* the types */
let T_I8 : int = 0
let T_I16 : int = 1
let T_I32 : int = 2
let T_C10 : int = 3
let T_F16 : int = 4
let T_F32 : int = 5
let T_U8 : int = 6
let T_U16 : int = 7
let T_U32 : int = 8
let T_O8 : int = 9
let T_UNK : int = 10
/* the channels */
let SW_X : int = 0
let SW_Y : int = 1
let SW_Z : int = 2
let SW_W : int = 3
let SW_0 : int = 4
let SW_1 : int = 5
let SW_2 : int = 6
let SW_H : int = 7
let SW_U : int = 8
/* the modifiers */
let F_ABS : int = 1
let F_NEG : int = 2
let F_GPI : int = 4
/* the kinds */
let K_FLOAT : int = 0
let K_UINT : int = 1
let K_INT : int = 2
/* the repeat modes and the operands' places in them */
let R_EXTERNAL : int = 0
let R_INTERNAL : int = 1
let R_BOTH : int = 2
let R_SLMSI : int = 3
let O_SRC0 : int = 0
let O_SRC1 : int = 1
let O_SRC2 : int = 2
let O_DEST : int = 3

function IsFloatType(t : int) -> bool = t == T_C10 | t == T_F16 | t == T_F32
function IsSignedType(t : int) -> bool = t == T_I8 | t == T_I16 | t == T_I32
function IsUnsignedType(t : int) -> bool = t == T_U8 | t == T_U16 | t == T_U32
/* decode_modifier: 1 negated, 2 absolute, 3 both */
function Modifier(m : int) -> int = if m == 1 then F_NEG else if m == 2 then F_ABS else if m == 3 then F_NEG + F_ABS else 0
function Bit(lo : int) -> int = UInt(Field(lo, 1))
function Popcount4(m : int) -> int = MOD(m, 2) + MOD(DIV(m, 2), 2) + MOD(DIV(m, 4), 2) + MOD(DIV(m, 8), 2)
/* the f32 1.0 and 0.0 */
function One() -> bits(32) = FConst(0x3f800000)
function Zero() -> bits(32) = FConst(0x00000000)

/* a rotate left of a word (bits 16: of its low half) by n, of the u32s (usse.c's rotate_left) */
function RotateLeft(x : bits(32), by : bits(32), bits : int) -> bits(32) = {
    let n = AndT(by, UConst(bits - 1));
    let left = ShlT(x, n);
    let right = ShrT(x, ISubT(UConst(bits), n));
    let r = if IEqT(n, UConst(0)) then x else OrT(left, right);
    if bits == 16 then AndT(r, UConst(65535)) else r
}
