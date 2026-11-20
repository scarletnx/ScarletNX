val decode_bad_aarch32_instrs_test : bits(4) -> unit

function decode_bad_aarch32_instrs_test Rd = {
    throw(Error_Unpredictable())
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_bad_aarch32_instrs_test(Rd)
}
