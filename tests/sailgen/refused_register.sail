register FPCR : bits(64)

val decode_fpcr_aarch64_instrs_test : bits(4) -> unit

function decode_fpcr_aarch64_instrs_test Rd = {
    let x : bits(64) = FPCR;
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_fpcr_aarch64_instrs_test(Rd)
}
