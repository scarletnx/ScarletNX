struct ProcState = {
  N : bits(1),
  EL : bits(2)
}

register PSTATE : ProcState

let PSTATE_EL : bits(2) = 0b00

val decode_pin_aarch64_instrs_test : bits(4) -> unit

function decode_pin_aarch64_instrs_test Rd = {
    if PSTATE.EL == 0b00 then {
        PSTATE.EL = 0b01
    };
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_pin_aarch64_instrs_test(Rd)
}
