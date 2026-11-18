register GPR : vector(32, bits(64))

val Peek : bits(64) -> int

val decode_peek_aarch64_instrs_test : bits(5) -> unit

function decode_peek_aarch64_instrs_test Rn = {
    let n : int = Peek(GPR[UInt(Rn)]);
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rn = Slice(__opcode, 0, 5);
    decode_peek_aarch64_instrs_test(Rn)
}
