struct ProcState = {
  N : bits(1),
  D : bits(1)
}

register PSTATE : ProcState

val decode_mask_aarch64_instrs_test : bits(4) -> unit

function decode_mask_aarch64_instrs_test Rd = {
    let x : bits(1) = PSTATE.N;
    let y : bits(1) = PSTATE.D;
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_mask_aarch64_instrs_test(Rd)
}
