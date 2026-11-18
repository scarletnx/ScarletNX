/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NX's fixups over a64.sail, each with why: what Horizon's EL0 makes of the architecture where it chooses, and
   where ScarletNX's CPU keeps the registers. sailgen reads this file after a64.sail (--overlay), and a definition
   here takes the place of a64.sail's of the same name; a top-level let is a constant the generated code folds.
   a64.roots stops the extraction at the names defined here, so a64.sail holds only sail-arm's own text */

// ScarletNX's CPU keeps the general-purpose registers in one array of 32 (cpu.h's CPU.x): X0 to X30, then the stack
// pointer. sail-arm has 31 registers R0 to R30, which get_R and set_R pick by a match on the number; here they are
// GPR's elements, by that number, which the back end loads and stores where the CPU keeps them (sg_GPR_read)
register GPR : vector(32, bits(64))

val get_R : forall 'n, 0 <= 'n <= 30. int('n) -> bits(64)
function get_R n = GPR[n]

val set_R : forall 'n, 0 <= 'n <= 30. (int('n), bits(64)) -> unit
function set_R (n, v) = {
    GPR[n] = v
}

// Guest code runs at EL0 alone, where the stack pointer is always SP_EL0 (PSTATE.SP is 0 there and EL0 can't set
// it), which the CPU keeps after X30, as GPR's last element
val SP_read : unit -> bits(64)
function SP_read () = GPR[31]

val SP_set : bits(64) -> unit
function SP_set value_name = {
    GPR[31] = value_name
}

// The exception level guest code runs at, which sailgen reads PSTATE.EL as (a let named for a register's field holds
// that field fixed)
let PSTATE_EL : bits(2) = EL0

// No EL2 runs under the guest: no hypervisor's controls (HCR_EL2, HCRX_EL2) or virtual SError reach its EL0
val EL2Enabled : unit -> bool
function EL2Enabled () = false

val IsHCRXEL2Enabled : unit -> bool
function IsHCRXEL2Enabled () = false

// No SError is ever pending (the runtime raises none), so ESB defers none into DISR_EL1
val AArch64_ESBOperation : unit -> unit
function AArch64_ESBOperation () = ()

// No Guarded Control Stack: the NX's CPUs have none (FEAT_GCS is Armv9.4's), so a call pushes no record and a return
// checks none
val HaveGCS : unit -> bool
function HaveGCS () = false

// No branch target identification: Horizon maps no guarded page, so no instruction checks the branch type that led to
// it (BTypeNext, which the back end keeps nowhere)
let InGuardedPage : bool = false

// and the CPU keeps no branch type: what a BTI or PACIASP makes of PSTATE.BTYPE (BTypeCompatible) only that check reads
let PSTATE_BTYPE : bits(2) = 0b00

// No Statistical Profiling Extension: no sample is ever in flight
let SPESampleInFlight : bool = false

// Horizon runs EL0 with SCTLR_EL1.EnIA, EnIB, EnDA and EnDB clear: signing and authenticating leave the pointer as it
// is (AddPAC* and Auth* return it unchanged when the key is disabled)
val AddPACIA : (bits(64), bits(64)) -> bits(64)
function AddPACIA (x, y) = x

val AddPACIB : (bits(64), bits(64)) -> bits(64)
function AddPACIB (x, y) = x

val AddPACDA : (bits(64), bits(64)) -> bits(64)
function AddPACDA (x, y) = x

val AddPACDB : (bits(64), bits(64)) -> bits(64)
function AddPACDB (x, y) = x

val AuthIA : (bits(64), bits(64), bool) -> bits(64)
function AuthIA (x, y, is_combined) = x

val AuthIB : (bits(64), bits(64), bool) -> bits(64)
function AuthIB (x, y, is_combined) = x

val AuthDA : (bits(64), bits(64), bool) -> bits(64)
function AuthDA (x, y, is_combined) = x

val AuthDB : (bits(64), bits(64), bool) -> bits(64)
function AuthDB (x, y, is_combined) = x

// XPAC strips whatever the keys' state: the extended HLE profile has 39-bit virtual addresses and no top-byte
// ignore, so bit 55 selects the address range and fills bits 63:39
val Strip : (bits(64), bool) -> bits(64)
function Strip (A, data) = Replicate([A[55]], 25) @ A[38 .. 0]

// PACGA has no SCTLR enable: it always answers a code in bits 63:32 under the generic key, the low half zero. No key
// is modelled, so the code is a fixed mix of Xn and Xm|SP: what a game signs with it (an SDK's, a fiber's context) it
// checks later with the same instruction, which answers the same. The multipliers are 0x9E3779B97F4A7C15 and
// 0xBF58476D1CE4E5B9, written negative so the integer stays in 64 bits; the product's low 64 bits are the same
val AddPACGA : (bits(64), bits(64)) -> bits(64)
function AddPACGA (x, y) = {
    let t1 : bits(64) = EOR(x, (UInt(y) * -7046029254386353131)[63 .. 0]);
    let t2 : bits(64) = EOR(t1, LSR(t1, 29));
    let t3 : bits(64) = (UInt(t2) * -4658895280553007687)[63 .. 0];
    let t4 : bits(64) = EOR(t3, LSR(t3, 32));
    return(t4[63 .. 32] @ Zeros(32))
}
