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

// No Scalable Matrix Extension: the NX's Cortex-A57 has none (FEAT_SME is Armv9.2's), so it is never in streaming mode
// and the SIMD&FP loads and stores take the plain path
val HaveSME : unit -> bool
function HaveSME () = false

val InStreamingMode : unit -> bool
function InStreamingMode () = false

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

// ---- loads and stores ----
// An access descriptor is EL0's (PSTATE_EL above) in the Non-secure state, with no MPAM partition kept: sail-arm's
// asks the security state and MPAM's controls, which no register here keeps
val NewAccDesc : AccessType -> AccessDescriptor
function NewAccDesc acctype = {
    accdesc : AccessDescriptor = undefined;
    accdesc.acctype = acctype;
    accdesc.el = EL0;
    accdesc.ss = SS_NonSecure;
    accdesc.acqsc = false;
    accdesc.acqpc = false;
    accdesc.relsc = false;
    accdesc.limitedordered = false;
    accdesc.exclusive = false;
    accdesc.rcw = false;
    accdesc.rcws = false;
    accdesc.atomicop = false;
    accdesc.nontemporal = false;
    accdesc.read = false;
    accdesc.write = false;
    accdesc.pan = false;
    accdesc.nonfault = false;
    accdesc.firstfault = false;
    accdesc.first = false;
    accdesc.contiguous = false;
    accdesc.streamingsve = false;
    accdesc.ls64 = false;
    accdesc.mops = false;
    accdesc.a32lsmd = false;
    accdesc.tagchecked = false;
    accdesc.tagaccess = false;
    accdesc.transactional = false;
    return(accdesc)
}

// and an unprivileged load or store (LDTR, STTR) is EL0's as any other
val AArch64_IsUnprivAccessPriv : unit -> bool
function AArch64_IsUnprivAccessPriv () = false

// A pair of registers is one access of both (the pairs' path for FEAT_LSE2, which reaches HaveLSE2Ext alone here):
// GuestMemRead and GuestMemWrite make it one 64-bit access for a pair of words and two 64-bit ones for a pair of
// doublewords, the accesses ScarletNX always made. The NX's CPUs have no FEAT_LSE2 (a64.profile), whose 16-byte
// single-copy atomicity that second one is not. LDPSW and STNP, which Arm makes two accesses on any CPU, stay two
val HaveLSE2Ext : unit -> bool
function HaveLSE2Ext () = true

val Mem_read__1 : forall ('ispair : Bool) 'size, 'size * 8 >= 0.
  (bits(64), int('size), AccessDescriptor, bool('ispair)) -> bits('size * 8)
function Mem_read__1 (address, size, accdesc, ispair) = Mem_read(address, size, accdesc)

val Mem_set__1 : forall ('ispair : Bool) 'size, 'size * 8 >= 0.
  (bits(64), int('size), AccessDescriptor, bool('ispair), bits('size * 8)) -> unit
function Mem_set__1 (address, size, accdesc, ispair, value_name) = Mem_set(address, size, accdesc, value_name)

// Horizon runs EL0 little-endian (SCTLR_EL1.E0E clear)
val BigEndian : AccessType -> bool
function BigEndian acctype = false

// A guest access is the host's (the back end's GuestMemRead), at any address: ScarletNX raises no alignment fault, of the
// stack pointer's or an exclusive pair's, and no data abort, so no syndrome is recorded
val CheckSPAlignment : unit -> unit
function CheckSPAlignment () = ()

val IsAligned__1 : forall 'N 'y, 'N >= 0. (bits('N), int('y)) -> bool
function IsAligned__1 (x, y) = true

val AArch64_SetLSInstructionSyndrome : forall 'Rt ('acq_rel : Bool) ('sign_extend : Bool) ('sixty_four : Bool) 'size.
  (int('size), bool('sign_extend), int('Rt), bool('sixty_four), bool('acq_rel)) -> unit
function AArch64_SetLSInstructionSyndrome (size, sign_extend, Rt, sixty_four, acq_rel) = ()

// A prefetch is a hint ScarletNX does not act on
val Prefetch : (bits(64), bits(5)) -> unit
function Prefetch (address, prfop) = ()

// A CONSTRAINED UNPREDICTABLE encoding (a load's write-back register or a pair's two registers the same, a
// store-exclusive's status register one it reads) runs as it is written: its registers are written in the order its
// Sail writes them, so a load's write-back comes after its loads and a store's data is read before its write-back,
// which is one of the UNKNOWN values Arm allows and what ScarletNX's emitters always did
val ConstrainUnpredictable : Unpredictable -> Constraint
function ConstrainUnpredictable which = Constraint_NONE

// An UNKNOWN value is zero, as sailgen starts an undefined local (the LSE atomics' compare value, which only CAS uses)
val __UNKNOWN_bits : forall 'n, 'n >= 0. int('n) -> bits('n)
function __UNKNOWN_bits n = Zeros(n)

// and so is an UNKNOWN integer (LD1R's, LD2's... unused element count of a form that has none)
val __UNKNOWN_integer : unit -> int
function __UNKNOWN_integer () = 0

// An implementation-defined choice the Cortex-A57 has no part in (the streaming mode's tag checks, an EL it doesn't
// run): the plain one
val __IMPDEF_boolean : string -> bool
function __IMPDEF_boolean s = false

// Guest memory is ScarletNX's own: the host's bytes at CPU.mem plus the address, read and written as the host does
// (the back end's). An acquire load and a release store are sequentially consistent host atomics, which order both ways, so
// RCpc's acquire is the stronger one and LOR's limited ordering the full one (cpu.h)
val GuestMemRead : forall 'size. (bits(64), int('size), bool) -> bits('size * 8)
val GuestMemWrite : forall 'size. (bits(64), int('size), bool, bits('size * 8)) -> unit

val Mem_read : forall 'size, 'size * 8 >= 0.
  (bits(64), int('size), AccessDescriptor) -> bits('size * 8)
function Mem_read (address, size, accdesc) =
    if accdesc.exclusive then GuestExclusiveValue(address, size)
    else GuestMemRead(address, size, accdesc.acqsc | accdesc.acqpc)

val Mem_set : forall 'size, 'size * 8 >= 0.
  (bits(64), int('size), AccessDescriptor, bits('size * 8)) -> unit
function Mem_set (address, size, accdesc, value_name) =
    if accdesc.exclusive then GuestStoreExclusive(address, size, value_name)
    else GuestMemWrite(address, size, accdesc.relsc, value_name)

// The exclusive monitor is cpu.h's, value based: the load-exclusive arms it with the value it reads, and a
// store-exclusive is a compare-and-swap from that value, which clears it. So the monitor is set by the load itself
// (GuestLoadExclusive, a pair's too: one access of all its bytes), the exclusive reads that follow take what it read
// (GuestExclusiveValue), whether the monitor passes is the compare-and-swap's answer, and the status is that store's
val GuestLoadExclusive : forall 'size. (bits(64), int('size)) -> unit
val GuestExclusiveValue : forall 'size. (bits(64), int('size)) -> bits('size * 8)
val GuestStoreExclusive : forall 'size. (bits(64), int('size), bits('size * 8)) -> unit
val GuestExclusiveStatus : unit -> bits(1)

val AArch64_SetExclusiveMonitors : forall 'size. (bits(64), int('size)) -> unit
function AArch64_SetExclusiveMonitors (address, size) = GuestLoadExclusive(address, size)

val AArch64_ExclusiveMonitorsPass : forall 'size. (bits(64), int('size)) -> bool
function AArch64_ExclusiveMonitorsPass (address, size) = true

val ExclusiveMonitorsStatus : unit -> bits(1)
function ExclusiveMonitorsStatus () = GuestExclusiveStatus()

// The LSE atomics are sequentially consistent host atomics of the access size, whatever their acquire and release
// bits ask: the old value, and the operation's result stored
val GuestAtomicAdd : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicAnd : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicXor : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicOr : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicSMax : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicSMin : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicUMax : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicUMin : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestAtomicSwap : forall 'size. (bits(64), bits('size)) -> bits('size)
val GuestCompareSwap : forall 'size. (bits(64), bits('size), bits('size)) -> bits('size)

val MemAtomic : forall 'size, 'size >= 0.
  (bits(64), bits('size), bits('size), AccessDescriptor) -> bits('size)
function MemAtomic (address, cmpoperand, operand, accdesc_in) = match accdesc_in.modop {
  MemAtomicOp_ADD => GuestAtomicAdd(address, operand),
  MemAtomicOp_BIC => GuestAtomicAnd(address, not_vec(operand)),
  MemAtomicOp_EOR => GuestAtomicXor(address, operand),
  MemAtomicOp_ORR => GuestAtomicOr(address, operand),
  MemAtomicOp_SMAX => GuestAtomicSMax(address, operand),
  MemAtomicOp_SMIN => GuestAtomicSMin(address, operand),
  MemAtomicOp_UMAX => GuestAtomicUMax(address, operand),
  MemAtomicOp_UMIN => GuestAtomicUMin(address, operand),
  MemAtomicOp_SWP => GuestAtomicSwap(address, operand),
  MemAtomicOp_CAS => GuestCompareSwap(address, cmpoperand, operand)
}

// ScarletNX's CPU keeps the 32 SIMD&FP registers as 128 bits each (cpu.h's CPU.v), and the NX has no SVE: sail-arm keeps
// them in _Z, SVE's registers of up to 2048 bits, which a write zeroes above the vector length. Here they are _V's
// elements, which the back end loads only as far as an instruction reads (sg__V_read_bits: a scalar's low lane)
register _V : vector(32, bits(128))

val V_read : forall 'n 'width, 'width in {8, 16, 32, 64, 128}. (int('n), int('width)) -> bits('width)
function V_read (n, width) = _V[n][width - 1 .. 0]

val V_set : forall 'n 'width, 'width in {8, 16, 32, 64, 128}. (int('n), int('width), bits('width)) -> unit
function V_set (n, width, value_name) = {
    _V[n] = ZeroExtend(value_name, 128)
}

// FPCR as the CPU keeps it (cpu.h's CPU.fpcr): the NX implements none of its trap enables, which read as 0 (sail-arm's
// __get_FPCR asks each as IMPDEF), so the register is all FPCR_read gives
register FPCR : bits(64)

val FPCR_read : unit -> bits(64)
function FPCR_read () = FPCR

// Horizon runs EL0 with CPACR_EL1.FPEN 0b11: floating point and AdvSIMD never trap
val CheckFPEnabled64 : unit -> unit
function CheckFPEnabled64 () = ()

val CheckFPAdvSIMDEnabled64 : unit -> unit
function CheckFPAdvSIMDEnabled64 () = ()

// No FEAT_AFP (Armv8.7's, which a64.profile names nowhere, so HaveAltFP is false): FPCR.NEP is RES0, and a scalar
// result never merges into the destination's other elements
val IsMerging : bits(64) -> bool
function IsMerging fpcr = false

// and the AArch64 check the crypto and the BF16 instructions make, which asks the profiling of a trapped one
val AArch64_CheckFPAdvSIMDEnabled : unit -> unit
function AArch64_CheckFPAdvSIMDEnabled () = ()

// FPSR as the CPU keeps it (cpu.h's CPU.fpsr): the back end sets its cumulative flags, QC among them
register FPSR : bits(64)

// The comparisons' answers are those of FPCompare's flags, which raises the exceptions theirs do (a signalling NaN's
// Invalid Operation in an equality, any NaN's in the others): equal 0110, less 1000, greater 0010, unordered 0011
val FPCompareEQ : forall 'N, 'N in {16, 32, 64}. (bits('N), bits('N), FPCRType) -> bool
function FPCompareEQ (op1, op2, fpcr) = FPCompare(op1, op2, false, fpcr)[2 .. 2] == 0b1

val FPCompareGE : forall 'N, 'N in {16, 32, 64}. (bits('N), bits('N), FPCRType) -> bool
function FPCompareGE (op1, op2, fpcr) = FPCompare(op1, op2, true, fpcr)[1 .. 0] == 0b10

val FPCompareGT : forall 'N, 'N in {16, 32, 64}. (bits('N), bits('N), FPCRType) -> bool
function FPCompareGT (op1, op2, fpcr) = FPCompare(op1, op2, true, fpcr) == 0b0010

// Without FEAT_AFP the alternate floating point's argument is false, and a minimum or a maximum is the plain one
val FPMax__1 : forall 'N ('altfp : Bool), 'N in {16, 32, 64}. (bits('N), bits('N), FPCRType, bool('altfp)) -> bits('N)
function FPMax__1 (op1, op2, fpcr, altfp) = FPMax(op1, op2, fpcr)

val FPMin__1 : forall 'N ('altfp : Bool), 'N in {16, 32, 64}. (bits('N), bits('N), FPCRType, bool('altfp)) -> bits('N)
function FPMin__1 (op1, op2, fpcr, altfp) = FPMin(op1, op2, fpcr)

// A rounded shift right by one more than the rounding's place, the same number: (v + 2^(n - 1)) >> n, a floor, is
// ((v >> (n - 1)) + 1) >> 1, whose sum needs one bit more than v however far the shift goes
val RShr : forall ('round : Bool) 'shift 'value_name. (int('value_name), int('shift), bool('round)) -> int
function RShr (value_name, shift, round) = {
    if round then {
        return(((value_name >> (shift - 1)) + 1) >> 1)
    } else {
        return((value_name >> shift))
    }
}

// The A64 instructions run in AArch64 alone: a 32-bit game's code goes through the A32 recompiler
val UsingAArch32 : unit -> bool
function UsingAArch32 () = false

// sail-arm declares HaveFP16Ext and leaves its answer to its runtime: the guest's FEAT_FP16
val HaveFP16Ext : unit -> bool
function HaveFP16Ext () = IsFeatureImplemented(FEAT_FP16)

// A bfloat16 is the single of its bits over 16 zeros, so BFMLAL's product is added as the singles' is
val BFMulAddH : forall 'N, 'N == 32. (bits('N), bits(div('N, 2)), bits(div('N, 2)), FPCRType) -> bits('N)
function BFMulAddH (addend, op1, op2, fpcr) = FPMulAdd(addend, op1 @ Zeros(16), op2 @ Zeros(16), fpcr)

// EL0's allocation tags are off (SCTLR_EL1.ATA0 clear, as a process without PR_MTE's): the tag IRG, ADDG and SUBG give an
// address is 0, whatever GCR_EL1 excludes
val execute_aarch64_instrs_integer_tags_mcaddtag : forall ('ADD : Bool) 'd 'n, (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (bool('ADD), int('d), int('n), bits(64), bits(4)) -> unit
function execute_aarch64_instrs_integer_tags_mcaddtag (ADD, d, n, offset, tag_offset) = {
    let operand1 : bits(64) = if n == 31 then SP_read() else X_read(n, 64);
    let sum : bits(64) = if ADD then operand1 + offset else operand1 - offset;
    let result : bits(64) = (sum[63 .. 60] @ 0x0) @ sum[55 .. 0];
    if d == 31 then {
        SP_set() = result
    } else {
        X_set(d, 64) = result
    }
}

val execute_aarch64_instrs_integer_tags_mcsubtag : forall ('ADD : Bool) 'd 'n, (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (bool('ADD), int('d), int('n), bits(64), bits(4)) -> unit
function execute_aarch64_instrs_integer_tags_mcsubtag (ADD, d, n, offset, tag_offset) = {
    let operand1 : bits(64) = if n == 31 then SP_read() else X_read(n, 64);
    let sum : bits(64) = if ADD then operand1 + offset else operand1 - offset;
    let result : bits(64) = (sum[63 .. 60] @ 0x0) @ sum[55 .. 0];
    if d == 31 then {
        SP_set() = result
    } else {
        X_set(d, 64) = result
    }
}

val execute_aarch64_instrs_integer_tags_mcinsertrandomtag : forall 'd 'm 'n, (0 <= 'd & 'd <= 31 & 0 <= 'm & 'm <= 31 & 0 <= 'n & 'n <= 31).
  (int('d), int('m), int('n)) -> unit
function execute_aarch64_instrs_integer_tags_mcinsertrandomtag (d, m, n) = {
    let operand : bits(64) = if n == 31 then SP_read() else X_read(n, 64);
    let result : bits(64) = (operand[63 .. 60] @ 0x0) @ operand[55 .. 0];
    if d == 31 then {
        SP_set() = result
    } else {
        X_set(d, 64) = result
    }
}

// GMI sets the bit the address's tag numbers: a shift of 1, where the Sail sets a bit of a number only the guest knows
val execute_aarch64_instrs_integer_tags_mcinserttagmask : forall 'd 'm 'n, (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('m), int('n)) -> unit
function execute_aarch64_instrs_integer_tags_mcinserttagmask (d, m, n) = {
    let address : bits(64) = if n == 31 then SP_read() else X_read(n, 64);
    X_set(d, 64) = X_read(m, 64) | LSL(ZeroExtend(0b1, 64), MOD(UInt(address[59 .. 56]), 64))
}
