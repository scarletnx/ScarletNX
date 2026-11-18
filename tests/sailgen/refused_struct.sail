struct ProcState = {
  N : bits(1)
}

struct Desc = {
  ordered : bool,
  flag : bits(1)
}

register PSTATE : ProcState

val decode_desc_aarch64_instrs_test : bits(4) -> unit

function decode_desc_aarch64_instrs_test Rd = {
    d : Desc = undefined;
    d.ordered = Rd == 0b0000;
    d.flag = PSTATE.N;
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_desc_aarch64_instrs_test(Rd)
}
