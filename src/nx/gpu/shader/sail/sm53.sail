/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Maxwell's (SM53's) instructions in Sail (sailgen --gpu): what each computes, from the fields of its 64-bit word that
   sm53.decode names it by. This file is the state every family shares, sm53_<family>.sail the instructions of a family
   (spirv_translate.c's emit_int, emit_float...), each with its roots (sm53_<family>.roots, which sm53.roots includes)
   and its back end over the translator's helpers (sm53_<family>_spirv.h; sm53_spirv.h is this file's). A register
   read in a block is spirv_translate.c's value of it there (reg_read), a write keeps the old value where a pure
   instruction's guard is false (reg_write), and the booleans' operations fold the true and false constants out, as
   its cc_and and shader_select do */

/* bits lo to lo + n - 1 of the instruction (n at most 32), known when the translator runs */
val Field : forall 'n. (int, int('n)) -> bits('n)
/* the opcode's place in its roots line: an instruction's forms (_R 0, _C 1, _I 2... as the line lists them) */
val Form : unit -> int
/* the fields the translator decoded: d at 0, a at 8, b at 20 */
val Dst : unit -> int
val RegA : unit -> int
val RegB : unit -> int
/* register r (RZ, 255, reads 0), written (where a guard is false, kept), and copied as the block holds it (a value of
   another type whose bits nothing asked for stays that value: MOV's reg_copy) */
val R_read : int -> bits(32)
val R_write : (int, bits(32)) -> unit
val R_copy : (int, int) -> unit
/* register r as a float, the block's float of it when it has one (reg_read_as), and a float written as its bits
   (reg_write_as) */
val R_readF : int -> bits(32)
val R_writeF : (int, bits(32)) -> unit
/* predicate p (PT, 7, reads true), negated with neg; written (PT is never) */
val PredRead : (int, int) -> bool
val PredWrite : (int, bool) -> unit
/* the condition code's flags: 0 zero, 1 sign, 2 carry, 3 overflow, read and written */
val FlagRead : int -> bool
val FlagWrite : (int, bool) -> unit
/* the right operand of an integer instruction's form: 0 the register at 20, 1 the constant buffer word, 2 the
   immediate of 19 bits and its sign at 56 (shader_integer_right) */
val Right : int -> bits(32)
/* the constant buffer word the instruction names (bank at 34, offset at 20) */
val CbufWord : unit -> bits(32)
/* an instruction the translation refuses: why */
val Unsupported : string -> unit

let PT : int = 7
let RZ : int = 255
