register GPR : vector(32, bits(64))

enum ReduceOp = {ReduceOp_FMINNUM, ReduceOp_FMAXNUM, ReduceOp_FMIN, ReduceOp_FMAX, ReduceOp_FADD, ReduceOp_ADD}

val decode_reduce_aarch64_instrs_test : bits(4) -> unit

function decode_reduce_aarch64_instrs_test Rd = {
    let op : ReduceOp = ReduceOp_FADD;
    GPR[UInt(Rd)] = ZeroExtend(Reduce(op, GPR[0], 32), 64)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_reduce_aarch64_instrs_test(Rd)
}
