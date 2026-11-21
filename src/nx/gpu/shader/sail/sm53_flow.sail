/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's control flow (sm53_flow.roots; back end sm53_flow_spirv.h): the branches, calls and returns, the CRS stack's
   pushes and pops, KIL and EXIT. A program with flow the structure does not place runs in a dispatch loop: a branch is
   a store of its target to the pc, and the loop goes there. The structure's branches (BRA, SYNC, BRK, CONT, RET with
   it) are to_spirv's, which does not hand them here */

/* the call stack (a program with CAL): its pointer and its 32 entries */
val HasCallStack : unit -> bool
val CallPtr : unit -> bits(32)
val CallPtrWrite : bits(32) -> unit
val CallStackRead : int -> bits(32)
val CallStackWrite : (int, bits(32)) -> unit
/* the CRS stack at run time (a program whose pops the static walk did not resolve): its pointer and entries; the
   nearest token of a kind popped, with the tokens pushed after it, as Maxwell's does (shader_dynamic_pop): whether one
   was found, where it goes (fallback where none), the pointer after it */
val DynamicStack : unit -> bool
val StackPtr : unit -> bits(32)
val StackPtrWrite : bits(32) -> unit
val StackRead : int -> bits(32)
val StackWrite : (int, bits(32)) -> unit
val DynamicPop : (int, bits(32)) -> unit
val PopFound : unit -> bool
val PopDestination : unit -> bits(32)
val PopStackPtr : unit -> bits(32)
/* the dispatch loop's pc: read, and the instruction it goes to next */
val Pc : unit -> bits(32)
val PcWrite : bits(32) -> unit
/* the target the static walk found for a pop (a RET's PRET), 0 for none; a relative branch's target; the
   instruction after this one; this one's index; the GPU address of word 0 (an absolute branch's base) */
val PopTarget : unit -> int
val BraTarget : unit -> int
val NextInsn : unit -> int
val InsnIndex : unit -> int
val FlowBase : unit -> int
/* a constant buffer's word at a run-time byte address */
val CbufReadDynamic : (int, bits(32)) -> bits(32)
/* the program's flow is structured: a BRX's target goes to the structure's switch */
val Structured : unit -> bool
val BranchTarget : bits(32) -> unit
/* (the register r plus the signed 24-bit offset) / 8, toward zero, of i32s: BRX's index in words */
val SignedWords : (int, int) -> bits(32)
/* KIL: the lane goes on as a helper, so that its quad's derivatives stay defined, and nothing it writes lands: what
   the hardware gives, and what Eden does. Inside the flow loop it goes on as a lane that only remembers it, and is
   discarded after the loop (DirectCtx.discarded) */
val Kill : unit -> unit
/* EXIT's end of the program (shader_exit) */
val Exit : unit -> unit

/* the CRS stack's token kinds: SSY/SYNC 0, PBK/BRK 1, PCNT/CONT 2, PEXIT 3 */
function CrsKind() -> int = match Form() { 0 => 0, 1 => 1, 2 => 2, _ => 3 }

/* Relative CAL through cbuf counts bytes from the next instruction. Absolute JCAL/JMP/JMX count from the shader pool.
   FlowBase is the GPU address represented by the translator's word0, including compute's fake SPH. A target whose
   word is a scheduling word (its index 2 mod 4 past the header's ten) goes to the instruction after it */
function IndirectTarget(cal : bool, jmx : bool) -> bits(32) = {
    let value0 = if Field(5, 1) == 0b1 then {
        let offset = ZeroExtend(Field(20, 16), 32);
        CbufReadDynamic(UInt(Field(36, 5)), if jmx then offset + R_read(RegA()) else offset)
    } else Field(20, 32);
    let value1 = if jmx then value0 + R_read(RegA()) else value0;
    let value = if cal then value1 + to_bits(32, (InsnIndex() + 1) * 8) else value1 - to_bits(32, FlowBase());
    let target = value >> 3;
    let schedule = (target & 0x00000003) == 0x00000002;
    target + (if schedule then 0x00000001 else 0x00000000)
}

/* CAL, JCAL and PRET: CAL pushes the instruction after it and jumps; PRET only pushes its target, where a later RET
   returns. Without a call stack, the CRS stack's static walk followed the program's returns: a PRET pushes nothing at
   run time */
function execute_CAL() -> unit = if HasCallStack() then {
    let pret = Form() == 2;
    let sp = CallPtr();
    let room = UInt(sp) < 32;
    let address = to_bits(32, if pret then BraTarget() else NextInsn());
    foreach (k from 0 to 31) {
        let at = sp == to_bits(32, k);
        let old = CallStackRead(k);
        CallStackWrite(k, if at then address else old)
    };
    CallPtrWrite(sp + (if room then 0x00000001 else 0x00000000));
    if not_bool(pret) then
        PcWrite(if room then (if Form() == 1 | Field(5, 1) == 0b1 then IndirectTarget(Form() == 0, false)
                              else to_bits(32, BraTarget()))
                else 0xffffffff)
}

/* RET: to the entry on top of the call stack; without one, to its PRET's target as the static walk found it, or the
   program's end where it found none */
function execute_RET() -> unit = if not_bool(HasCallStack()) then
    PcWrite(if PopTarget() != 0 then to_bits(32, PopTarget()) else 0xffffffff)
else {
    let sp = CallPtr();
    let has_entry = 0 < UInt(sp);
    let index = sp - 0x00000001;
    var destination : bits(32) = 0xffffffff;
    foreach (k from 0 to 31) {
        let at = index == to_bits(32, k);
        let old = CallStackRead(k);
        destination = if at then old else destination
    };
    CallPtrWrite(if has_entry then index else 0x00000000);
    PcWrite(destination)
}

/* SSY, PBK, PCNT and PEXIT on the CRS stack at run time: a token of the target and the kind, pushed. With the static
   walk, nothing at run time */
function execute_PUSH() -> unit = if DynamicStack() then {
    let sp = StackPtr();
    let token = to_bits(32, BraTarget() + CrsKind() * 65536);
    foreach (k from 0 to 31) {
        let at = sp == to_bits(32, k);
        let old = StackRead(k);
        StackWrite(k, if at then token else old)
    };
    let room = UInt(sp) < 32;
    let inc = if room then 0x00000001 else 0x00000000;
    StackPtrWrite(sp + inc)
}

/* SYNC, BRK and CONT: the nearest token of their kind popped at run time; with the static walk, to its target */
function execute_POP() -> unit = if DynamicStack() then {
    DynamicPop(CrsKind(), Pc());
    PcWrite(PopDestination());
    StackPtrWrite(PopStackPtr())
} else PcWrite(to_bits(32, PopTarget()))

function execute_BRA() -> unit = PcWrite(to_bits(32, BraTarget()))
function execute_JMP() -> unit = PcWrite(IndirectTarget(false, Form() == 1))

/* BRX: the register at 8 plus the signed offset at 20, in bytes from the next instruction, as words; a target on a
   scheduling word goes to the instruction after it */
function execute_BRX() -> unit = {
    let words = SignedWords(RegA(), UInt(Field(20, 24)));
    let target0 = words + to_bits(32, InsnIndex() + 1);
    let after_ten = target0 - 0x0000000a;
    let aligned = (after_ten & 0x00000003) == 0x00000000;
    let in_range = not_bool(UInt(target0) < 10);
    let needs_skip = aligned & in_range;
    let target = target0 + (if needs_skip then 0x00000001 else 0x00000000);
    if Structured() then BranchTarget(target) else PcWrite(target)
}

/* CCTL, CCTLL, IDE and BPT: cache and breakpoint hints, which have no shader-visible result */
function execute_HINT() -> unit = ()
function execute_KIL() -> unit = Kill()
function execute_EXIT() -> unit = Exit()
