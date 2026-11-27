/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AArch32 guests' fixups over a32.sail, each with why: what user mode on the 3DS's ARM11, the GBA's ARM7TDMI, the
   NDS's ARM946E-S, the Vita's Cortex-A9 and a 32-bit Switch program's Cortex-A57 makes of the architecture, and where
   ScarletNX's AArch32 CPU (arm.h's SnxArmCPU) keeps the registers. sailgen reads this file after a32.sail (--overlay),
   and a definition here takes the place of a32.sail's of the same name; a top-level let is a constant the generated
   code folds. a32.roots stops the extraction at the names defined here, so a32.sail holds only sail-arm's own text.
   What differs from one architecture to the next asks ArchVersion(), the guest's (mach_a32.h), as the ARMv7 ARM's ASL
   does; which encodings each one has is a32.profile's */

// ---- the registers ----

// ScarletNX's AArch32 CPU keeps R0 to R15 in one array (SnxArmCPU.r), and a guest runs in user mode alone, which banks
// none of them: sail-arm's Rmode_read and Rmode_set pick a mode's bank, here GPR's elements, which the back end loads
// and stores where the CPU keeps them (sg_GPR_read). R15 is read through R_read, the pc of the instruction plus 8 or 4
register GPR : vector(16, bits(32))
val Rmode_read : forall 'n. (int('n), bits(5)) -> bits(32)
function Rmode_read (n, mode) = GPR[n]
val Rmode_set : forall 'n. (int('n), bits(5), bits(32)) -> unit
function Rmode_set (n, mode, value_name) = {
    GPR[n] = value_name
}
// The exception level and the mode guest code runs at, which sailgen reads PSTATE.EL and PSTATE.M as (a let named for a
// register's field holds that field fixed): what only a privileged mode does (CPS, SETPAN) folds away
let PSTATE_EL : bits(2) = EL0
let PSTATE_M : bits(5) = 0b10000 // M32_User
// The instruction set a block runs in is the block's (the dispatcher picks an A32 or a Thumb block by CPSR.T): the back
// end answers it when it recompiles, and an instruction that selects the other one (BLX) has it from then on, so its
// branch aligns its target as the new set does (SetInstrSet, the back end's; PSTATE.T as sail-arm's SelectInstrSet)
val CurrentInstrSet : unit -> InstrSet
val SetInstrSet : InstrSet -> unit
val SelectInstrSet : InstrSet -> unit
function SelectInstrSet iset = {
    SetInstrSet(iset);
    PSTATE.T = if iset == InstrSet_A32 then 0b0 else 0b1
}

// ---- the condition and the IT block ----

// The block compilers decide an IT block's conditions when they compile it: ITSTATE before each
// instruction is known when the recompiler runs, the back end's (ITSTATE), as is CurrentCond (sail-arm's
// AArch32_CurrentCond, a val its runtime answers): an A32 word's condition, the T1 and T3 B<c>'s own, else the IT
// block's or AL. ConditionPassed is then sail-arm's, the run-time test of the flags that sailgen runs the rest of the
// instruction under (sg_when)
val ITSTATE : unit -> bits(8)
val InITBlock : unit -> bool
function InITBlock () = CurrentInstrSet() == InstrSet_T32 & ITSTATE()[3 .. 0] != 0b0000
val LastInITBlock : unit -> bool
function LastInITBlock () = ITSTATE()[3 .. 0] == 0b1000
// IT sets ITSTATE for the instructions after it, which the block compiler advances itself (ShouldAdvanceIT): the back
// end hands it the new ITSTATE (ITSTATE_set), known when it recompiles. SCTLR.ITD is clear: every IT block is allowed
val ITSTATE_set : bits(8) -> unit
val execute_aarch32_instrs_IT_Op_A_txt : (bits(4), bits(4)) -> unit
function execute_aarch32_instrs_IT_Op_A_txt (firstcond, mask) = ITSTATE_set(firstcond @ mask)

// ---- guest memory ----

// Guest memory is the board's own (flat, checked or hosted, with the ARM7TDMI's and the
// ARM946E-S's bus rules and hook). GuestRead and GuestWrite are one access of size bytes; aligned is MemA's and MemS's
// (the multiple transfers', whose alignment the ARMv4T and ARMv5TE buses do not check) against MemU's, which takes any
// address
val GuestRead : forall 'size. (bits(32), int('size), bool) -> bits('size * 8)
val GuestWrite : forall 'size. (bits(32), int('size), bool, bits('size * 8)) -> unit
val MemU_read : forall 'size, 'size in {1, 2, 4, 8, 16}. (bits(32), int('size)) -> bits(8 * 'size)
function MemU_read (address, size) = GuestRead(address, size, false)
val MemU_set : forall 'size, 8 * 'size >= 0. (bits(32), int('size), bits(8 * 'size)) -> unit
function MemU_set (address, size, value_name) = GuestWrite(address, size, false, value_name)
val MemA_read : forall 'size, 'size in {1, 2, 4, 8, 16}. (bits(32), int('size)) -> bits(8 * 'size)
function MemA_read (address, size) = GuestRead(address, size, true)
val MemA_set : forall 'size, 8 * 'size >= 0. (bits(32), int('size), bits(8 * 'size)) -> unit
function MemA_set (address, size, value_name) = GuestWrite(address, size, true, value_name)
val MemS_read : forall 'size, 'size in {1, 2, 4, 8, 16}. (bits(32), int('size)) -> bits(8 * 'size)
function MemS_read (address, size) = GuestRead(address, size, true)
val MemS_set : forall 'size, 8 * 'size >= 0. (bits(32), int('size), bits(8 * 'size)) -> unit
function MemS_set (address, size, value_name) = GuestWrite(address, size, true, value_name)
// The ARM7TDMI's halfword loads at an odd address (ARMv4's UNPREDICTABLE) are its bus's: LDRH reads the aligned
// halfword rotated a byte through its 32 bits, LDRSH the byte there sign extended, which the GBA's games and tests read
// so. GuestReadHalfwordV4(address, signed) is that load's 32 bits; these are sail-arm's three T16 halfword loads with
// it in the place of ZeroExtend or SignExtend of MemU on ArchVersion() 4, the rest as they are
val GuestReadHalfwordV4 : (bits(32), bool) -> bits(32)
val execute_aarch32_instrs_LDRH_i_OpT_A_txt : forall ('add : Bool) ('index : Bool) 'n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15).
  (bool('add), bits(32), bool('index), int('n), int('t), bool('wback)) -> unit
function execute_aarch32_instrs_LDRH_i_OpT_A_txt (add, imm32, index, n, t, wback) = {
    let offset_addr : bits(32) = if add then R_read(n) + imm32 else
      R_read(n) - imm32;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(2, false, t, false)
    };
    let data : bits(32) = if ArchVersion() == 4 then GuestReadHalfwordV4(address, false) else
      ZeroExtend(MemU_read(address, 2), 32);
    if wback then {
        R_set(n) = offset_addr
    };
    R_set(t) = data
}
val execute_aarch32_instrs_LDRH_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit
function execute_aarch32_instrs_LDRH_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if t != 15 then {
        AArch32_SetLSInstructionSyndrome(2, false, t, false)
    };
    let data : bits(32) = if ArchVersion() == 4 then GuestReadHalfwordV4(address, false) else
      ZeroExtend(MemU_read(address, 2), 32);
    if wback then {
        R_set(n) = offset_addr
    };
    R_set(t) = data
}
val execute_aarch32_instrs_LDRSH_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit
function execute_aarch32_instrs_LDRSH_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(2, true, t, false)
    };
    let data : bits(32) = if ArchVersion() == 4 then GuestReadHalfwordV4(address, true) else
      SignExtend(MemU_read(address, 2), 32);
    if wback then {
        R_set(n) = offset_addr
    };
    R_set(t) = data
}
// No data abort's syndrome is recorded: the block stops with the address (arm.h's SNX_ARM_DATA_ABORT)
val AArch32_SetLSInstructionSyndrome : forall 'Rt ('acq_rel : Bool) ('sign_extend : Bool) 'size.
  (int('size), bool('sign_extend), int('Rt), bool('acq_rel)) -> unit
function AArch32_SetLSInstructionSyndrome (size, sign_extend, Rt, acq_rel) = ()

// A multiple transfer checks its whole range before its first access, and moves nothing when any of it would fault
// (ScarletNX's flat-memory fault policy, arm/README.md's "Limits": not an MMU's partial transfer):
// GuestTransfer(address, bytes, write) opens it, and the accesses after it in the instruction are within it. These are
// sail-arm's LDM, STM, PUSH and POP with that call first. A register stored that the instruction writes back before
// (UNKNOWN in ARMv6 and later) is stored as it was, where the ARM7TDMI (ARMv4T) stores the base written back
// (mGBA's); ARMv6 made that store UNPREDICTABLE (UnpredictableEncoding, below), ARMv7 UNKNOWN
val GuestTransfer : (bits(32), int, bool) -> unit
// An empty list (UNPREDICTABLE) ends an LDM or an STM UNDEFINED, as the block compilers always had it: the board whose
// core does something of its own with one runs it in its interpreter (the GBA's ARM7TDMI transfers R15 and moves the
// base by 0x40, agb/cpu/cpu.c). An empty PUSH or POP transfers nothing, its address checked, but on checked memory,
// whose bus has no transaction of no bytes (GuestTransferEmpty(), the back end's)
val GuestTransferEmpty : unit -> bool
val execute_aarch32_instrs_LDM_Op_A_txt : forall 'n ('wback : Bool), (0 <= 'n & 'n <= 15).
  (int('n), bits(16), bool('wback)) -> unit
function execute_aarch32_instrs_LDM_Op_A_txt (n, registers, wback) = {
    if BitCount(registers) == 0 then {
        throw(Error_Undefined())
    };
    address : bits(32) = R_read(n);
    GuestTransfer(address, 4 * BitCount(registers), false);
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            R_set(i) = MemS_read(address, 4);
            address = address + 4
        };
        ()
    };
    if [registers[15]] == 0b1 then {
        LoadWritePC(MemS_read(address, 4))
    };
    if wback & [registers[n]] == 0b0 then {
        R_set(n) = R_read(n) + 4 * BitCount(registers)
    };
    if wback & [registers[n]] == 0b1 then {
        R_set(n) = __UNKNOWN_bits(32)
    };
    ()
}
val execute_aarch32_instrs_STM_Op_A_txt : forall 'n ('wback : Bool), (0 <= 'n & 'n <= 15).
  (int('n), bits(16), bool('wback)) -> unit
function execute_aarch32_instrs_STM_Op_A_txt (n, registers, wback) = {
    if BitCount(registers) == 0 then {
        throw(Error_Undefined())
    };
    if ((wback & [registers[n]] == 0b1) & n != LowestSetBit(registers)) & ArchVersion() == 6 then {
        UnpredictableEncoding()
    };
    address : bits(32) = R_read(n);
    GuestTransfer(address, 4 * BitCount(registers), true);
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            if ((i == n & wback) & i != LowestSetBit(registers)) & ArchVersion() == 4 then {
                MemS_set(address, 4) = R_read(n) + 4 * BitCount(registers)
            } else {
                MemS_set(address, 4) = R_read(i)
            };
            address = address + 4
        };
        ()
    };
    if [registers[15]] == 0b1 then {
        MemS_set(address, 4) = PCStoreValue()
    };
    if wback then {
        R_set(n) = R_read(n) + 4 * BitCount(registers)
    };
    ()
}
val execute_aarch32_instrs_PUSH_Op_A_txt : forall ('UnalignedAllowed : Bool).
  (bool('UnalignedAllowed), bits(16)) -> unit
function execute_aarch32_instrs_PUSH_Op_A_txt (UnalignedAllowed, registers) = {
    if BitCount(registers) == 0 & not_bool(GuestTransferEmpty()) then {
        throw(Error_Undefined())
    };
    address : bits(32) = R_read(13) - 4 * BitCount(registers);
    GuestTransfer(address, 4 * BitCount(registers), true);
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            if i == 13 & i != LowestSetBit(registers) then {
                MemA_set(address, 4) = __UNKNOWN_bits(32)
            } else {
                if UnalignedAllowed then {
                    MemU_set(address, 4) = R_read(i)
                } else {
                    MemA_set(address, 4) = R_read(i)
                }
            };
            address = address + 4
        };
        ()
    };
    if [registers[15]] == 0b1 then {
        if UnalignedAllowed then {
            MemU_set(address, 4) = PCStoreValue()
        } else {
            MemA_set(address, 4) = PCStoreValue()
        }
    };
    R_set(13) = R_read(13) - 4 * BitCount(registers)
}
val execute_aarch32_instrs_POP_Op_A_txt : forall ('UnalignedAllowed : Bool).
  (bool('UnalignedAllowed), bits(16)) -> unit
function execute_aarch32_instrs_POP_Op_A_txt (UnalignedAllowed, registers) = {
    if BitCount(registers) == 0 & not_bool(GuestTransferEmpty()) then {
        throw(Error_Undefined())
    };
    address : bits(32) = R_read(13);
    GuestTransfer(address, 4 * BitCount(registers), false);
    foreach (i from 0 to 14 by 1 in inc) {
        if [registers[i]] == 0b1 then {
            R_set(i) = if UnalignedAllowed then MemU_read(address, 4) else MemA_read(address, 4);
            address = address + 4
        };
        ()
    };
    if [registers[15]] == 0b1 then {
        if UnalignedAllowed then {
            if address[1 .. 0] == 0b00 then {
                LoadWritePC(MemU_read(address, 4))
            } else {
                throw(Error_Unpredictable())
            }
        } else {
            LoadWritePC(MemA_read(address, 4))
        }
    };
    if [registers[13]] == 0b0 then {
        R_set(13) = R_read(13) + 4 * BitCount(registers)
    };
    if [registers[13]] == 0b1 then {
        R_set(13) = __UNKNOWN_bits(32)
    };
    ()
}

// ---- the architectures ----

// The guest's architecture version, as the ARMv7 ARM's ASL asks it: 4 for the ARM7TDMI, 5 for the ARM946E-S, 6 for the
// ARM11, 7 for the Cortex-A9, 8 for the Cortex-A57 (the back end's, from the block's profile)
val ArchVersion : unit -> int
// An interworking branch takes the state from the target's bit 0 and clears it; with no CONSTRAINED UNPREDICTABLE
// choice (ConstrainUnpredictableBool, below) an A32 target's bit 1 stays. sail-arm's sets T in each way of an if on
// that bit, and sailgen writes no register under a condition known only when the guest runs, so T is the bit itself
// here
val BXWritePC : (bits(32), BranchType) -> unit
function BXWritePC (address_in, branch_type) = {
    address : bits(32) = address_in;
    PSTATE.T = [address[0]];
    address[0] = Bit(0b0);
    let branch_conditional : bool = not_bool((match AArch32_CurrentCond() {
      [bitone, bitone, bitone, _] => true,
      _ => false
    }) : bool);
    BranchTo(address, branch_type, branch_conditional)
}
// A data processing write of the PC in A32 interworks from ARMv7 on; ARMv4T to ARMv6 branch in A32, to the word its
// bits 1 and 0 cleared leave (the ARMv7 ARM's ALUWritePC); a Thumb one branches in Thumb
val ALUWritePC : bits(32) -> unit
function ALUWritePC address = if CurrentInstrSet() == InstrSet_A32 & ArchVersion() >= 7 then
                                  BXWritePC(address, BranchType_INDIR)
                              else BranchWritePC(address, BranchType_INDIR)
// A load into the PC branches, with the state from bit 0 from ARMv5 on; the ARM7TDMI (ARMv4T) stays in its state, its
// bit 0 dropped (the ARMv7 ARM's LoadWritePC, A2.3.2)
val LoadWritePC : bits(32) -> unit
function LoadWritePC address = if ArchVersion() >= 5 then BXWritePC(address, BranchType_INDIR)
                               else BranchWritePC(address, BranchType_INDIR)

// The ARM7TDMI's MULS sets C as its multiplier leaves it (ARMv4's UNKNOWN: the runtime's MultiplyCarry, from the
// operands before Rd changes, snx_arm_multiply_carry), and a block that times itself keeps the multiplier, whose bytes
// time it, in its trace (TraceMultiplier: Thumb's Rd, the m here). sail-arm's MUL with those two first
val MultiplyCarry : unit -> unit
val TraceMultiplier : bits(32) -> unit
val execute_aarch32_instrs_MUL_Op_A_txt : forall 'd 'm 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags)) -> unit
function execute_aarch32_instrs_MUL_Op_A_txt (d, m, n, setflags) = {
    if ArchVersion() == 4 then {
        TraceMultiplier(R_read(m));
        if setflags then {
            MultiplyCarry()
        }
    };
    let 'operand1 = SInt(R_read(n));
    let 'operand2 = SInt(R_read(m));
    let 'result = operand1 * operand2;
    R_set(d) = result[31 .. 0];
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result[31 .. 0])
    };
    ()
}

// Thumb's BL and BLX before Thumb-2 (ArchVersion() < 7: the ARM7TDMI, the ARM946E-S, the ARM11) are two 16-bit
// instructions, each one run alone (a game calls through LR into a suffix), as the ARMv5 ARM's pseudocode has them: the
// prefix (11110) puts the PC plus the high offset, sign extended, in LR; a suffix branches to LR plus its offset, and
// leaves the address after it in LR with bit 0 set: BL's (11111) stays in Thumb, BLX's (11101, from ARMv5, the offset's
// bit 0 clear) goes to A32 at a word. Thumb-2 made the pair one 32-bit instruction, which is sail-arm's
val decode_aarch32_legacy_BL_prefix : bits(11) -> unit
function decode_aarch32_legacy_BL_prefix offset = {
    if ConditionPassed() then {
        R_set(14) = PC_read__1() + SignExtend(offset @ Zeros(12), 32)
    };
    ()
}
function clause __DecodeT16 ((pc, ([bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if ArchVersion() < 7) = {
    decode_aarch32_legacy_BL_prefix(Slice(__opcode, 0, 11))
}
val decode_aarch32_legacy_BL_suffix : bits(11) -> unit
function decode_aarch32_legacy_BL_suffix offset = {
    if ConditionPassed() then {
        let target : bits(32) = R_read(14) + ZeroExtend(offset @ 0b0, 32);
        R_set(14) = (PC_read__1() - 2)[31 .. 1] @ 0b1;
        BranchWritePC(target, BranchType_DIRCALL)
    };
    ()
}
function clause __DecodeT16 ((pc, ([bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if ArchVersion() < 7) = {
    decode_aarch32_legacy_BL_suffix(Slice(__opcode, 0, 11))
}
val decode_aarch32_legacy_BLX_suffix : bits(11) -> unit
function decode_aarch32_legacy_BLX_suffix offset = {
    if ConditionPassed() then {
        let target : bits(32) = R_read(14) + ZeroExtend(offset @ 0b0, 32);
        R_set(14) = (PC_read__1() - 2)[31 .. 1] @ 0b1;
        PSTATE.T = 0b0;
        BranchTo(target[31 .. 2] @ 0b00, BranchType_DIRCALL, false)
    };
    ()
}
function clause __DecodeT16 ((pc, ([bitone, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, bitzero] as __opcode)) if ArchVersion() < 7) = {
    decode_aarch32_legacy_BLX_suffix(Slice(__opcode, 0, 11))
}

// The ARM7TDMI's BX takes no notice of bits 2 to 0, which should be zero (the later architectures' (0)(0)(0): no clause
// has such a halfword there): it branches with them set, before ARMv5. Sail-arm's BX T1 with no IT block to check
val decode_aarch32_legacy_BX : bits(4) -> unit
function decode_aarch32_legacy_BX Rm = {
    if ConditionPassed() then {
        execute_aarch32_instrs_BX_Op_A_txt(UInt(Rm))
    };
    ()
}
function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _] as __opcode)) if ArchVersion() < 5) = {
    decode_aarch32_legacy_BX(Slice(__opcode, 3, 4))
}

// ---- the hints and the exceptions ----

// The hints Thumb-2 leaves unallocated (1011 1111 hint 0000, hint 6 to 15) run as NOP, as sail-arm's fetch has them
// (UnallocatedT32_16_Instruction), outside its decode: a clause here, after the assigned hints' (a32.profile: since 7)
val decode_aarch32_unallocated_hint_T1 : unit -> unit
function decode_aarch32_unallocated_hint_T1 () = ()
function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero] as __opcode))) = {
    decode_aarch32_unallocated_hint_T1()
}

// The hints complete at once, as they always have here (arm/README.md): no event register is kept, WFE and WFI never
// wait or trap, SEV and SEVL signal nothing, YIELD gives the host nothing
val IsEventRegisterSet : unit -> bool
function IsEventRegisterSet () = false
val ClearEventRegister : unit -> unit
function ClearEventRegister () = ()
val InterruptPending : unit -> bool
function InterruptPending () = false
val AArch32_CheckForWFxTrap : (bits(2), WFxType) -> unit
function AArch32_CheckForWFxTrap (target_el, wfxtype) = ()
val WaitForEvent : forall 'localtimeout. int('localtimeout) -> unit
function WaitForEvent localtimeout = ()
val WaitForInterrupt : forall 'localtimeout. int('localtimeout) -> unit
function WaitForInterrupt localtimeout = ()
val SendEvent : unit -> unit
function SendEvent () = ()
val SendEventLocal : unit -> unit
function SendEventLocal () = ()
val Hint_Yield : unit -> unit
function Hint_Yield () = ()
// No EL2 or EL3 runs under the guest, and no host OS at EL2 (IsInHost)
val EL2Enabled : unit -> bool
function EL2Enabled () = false
val HaveEL : bits(2) -> bool
function HaveEL el = el == EL0 | el == EL1
val IsInHost : unit -> bool
function IsInHost () = false
// No guest has FEAT_PAN in AArch32 (a32.profile names it nowhere): SETPAN is UNDEFINED
val HavePANExt : unit -> bool
function HavePANExt () = IsFeatureImplemented(FEAT_PAN)
// The Thumb block compiler runs no SETEND: a block reads CPSR.E once, as it starts, and a Thumb block does not end at
// one to be entered again, as an A32 one does (arm/README.md), so Thumb's is UNDEFINED, as it always was here
val AArch32_CheckSETENDEnabled : unit -> unit
function AArch32_CheckSETENDEnabled () = if CurrentInstrSet() == InstrSet_T32 then throw(Error_Undefined())
// SVC and BKPT end the block with the exit the runtime takes (arm.h's SNX_ARM_SVC, SNX_ARM_BREAKPOINT): HCR.TGE and
// HSTR do not trap them to an EL2
val AArch32_CheckForSVCTrap : bits(16) -> unit
function AArch32_CheckForSVCTrap immediate = ()
val AArch32_CallSupervisor : bits(16) -> unit
val AArch32_SoftwareBreakpoint : bits(16) -> unit
// A write to the PC with an exception return (SUBS PC, LR) is a privileged mode's: user mode has no SPSR
val ALUExceptionReturn : bits(32) -> unit
function ALUExceptionReturn address = throw(Error_Undefined())
// An UNPREDICTABLE encoding (sail-arm's throw of Error_Unpredictable, which sailgen walks as UnpredictableEncoding())
// runs as it is written: a high-register CMP or ADD of two low registers, a write of the PC inside an IT block, an
// empty PUSH or POP's transfer of nothing (an empty LDM's or STM's is above). Old toolchains and hand-written code
// leave such forms in games, and the guests' cores run them so. The 3DS's ARM11 (ArchVersion() 6) refuses them: they
// are UNDEFINED there, as its profile has always decoded them (profile.c); and so is every A32 one, on every guest, as
// the A32 decoder has always had them (a32.decode: a PC operand of a register-shifted register, ...)
val UnpredictableEncoding : unit -> unit
function UnpredictableEncoding () = if ArchVersion() == 6 | CurrentInstrSet() == InstrSet_A32 then
                                        throw(Error_Undefined())
// An UNKNOWN value is zero, as sailgen starts an undefined local
val __UNKNOWN_bits : forall 'n, 'n >= 0. int('n) -> bits('n)
function __UNKNOWN_bits n = Zeros(n)
val ConstrainUnpredictableBool : Unpredictable -> bool
function ConstrainUnpredictableBool which = false
