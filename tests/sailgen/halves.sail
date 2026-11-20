register GPR : vector(32, bits(64))

enum FoldOp = {FoldOp_ADD, FoldOp_EOR, FoldOp_MORE}

val Fold : forall 'N 'esize, 'N >= 'esize. (FoldOp, bits('N), int('esize)) -> bits('esize)

function Fold (op, input, esize) = {
    result : bits('esize) = undefined;
    if 'N == esize then {
        return(input[esize - 1 .. 0])
    };
    let 'half = DIV('N, 2);
    let hi : bits('esize) = Fold(op, input['N - 1 .. half], esize);
    let lo : bits('esize) = Fold(op, input[half - 1 .. 0], esize);
    match op {
      FoldOp_ADD => {
          result = lo + hi
      },
      FoldOp_EOR => {
          result = EOR(lo, hi)
      },
      FoldOp_MORE => {
          result = (lo + hi) + 0x01
      }
    };
    return(result)
}

val Lower : forall 'N 'esize, 'N >= 'esize. (bits('N), int('esize)) -> bits('esize)

function Lower (input, esize) = {
    if 'N == esize then {
        return(input[esize - 1 .. 0])
    };
    let 'half = DIV('N, 2);
    let hi : bits('esize) = Lower(input[half - 1 .. 0], esize);
    let lo : bits('esize) = Lower(input[half - 1 .. 0], esize);
    return(lo + hi)
}

val Base : forall 'N 'esize, 'N >= 'esize. (bits('N), int('esize)) -> bits('esize)

function Base (input, esize) = {
    if 'N == esize then {
        return(Zeros(esize))
    };
    let 'half = DIV('N, 2);
    let hi : bits('esize) = Base(input['N - 1 .. half], esize);
    let lo : bits('esize) = Base(input[half - 1 .. 0], esize);
    return(lo + hi)
}

val Marks : forall 'N 'esize, 'N >= 'esize. (bits('N), int('esize)) -> bits('esize)

function Marks (input, esize) = {
    if 'N == esize then {
        return(input[esize - 1 .. 0])
    };
    let 'half = DIV('N, 2);
    let hi : bits('esize) = Marks(input['N - 1 .. half], esize);
    let lo : bits('esize) = Marks(input[half - 1 .. 0], esize);
    GPR[1] = Zeros(64);
    return(lo + hi)
}

val Some : forall 'N 'esize, 'N >= 'esize. (bits('N), int('esize)) -> bits('esize)

function Some (input, esize) = {
    result : bits('esize) = undefined;
    if 'N == esize then {
        return(input[esize - 1 .. 0])
    };
    let 'half = DIV('N, 2);
    let hi : bits('esize) = Some(input['N - 1 .. half], esize);
    let lo : bits('esize) = Some(input[half - 1 .. 0], esize);
    if esize == 8 then {
        result = lo + hi
    };
    return(result)
}

val decode_sum_aarch64_instrs_test : bits(4) -> unit

function decode_sum_aarch64_instrs_test Rd = {
    GPR[UInt(Rd)] = ZeroExtend(Fold(FoldOp_ADD, GPR[0], 8), 64)
}

val decode_op_aarch64_instrs_test : bits(4) -> unit

function decode_op_aarch64_instrs_test Rd = {
    GPR[UInt(Rd)] = ZeroExtend(Fold(FoldOp_EOR, GPR[0], 8), 64)
}

val decode_more_aarch64_instrs_test : bits(4) -> unit

function decode_more_aarch64_instrs_test Rd = {
    GPR[UInt(Rd)] = ZeroExtend(Fold(FoldOp_MORE, GPR[0], 8), 64)
}

val decode_sides_aarch64_instrs_test : bits(4) -> unit

function decode_sides_aarch64_instrs_test Rd = {
    GPR[UInt(Rd)] = ZeroExtend(Lower(GPR[0], 8), 64)
}

val decode_base_aarch64_instrs_test : bits(4) -> unit

function decode_base_aarch64_instrs_test Rd = {
    GPR[UInt(Rd)] = ZeroExtend(Base(GPR[0], 8), 64)
}

val decode_state_aarch64_instrs_test : bits(4) -> unit

function decode_state_aarch64_instrs_test Rd = {
    GPR[UInt(Rd)] = ZeroExtend(Marks(GPR[0], 8), 64)
}

val decode_some_aarch64_instrs_test : (bits(4), bits(1)) -> unit

function decode_some_aarch64_instrs_test (Rd, size) = {
    let 'esize = if size == 0b1 then 16 else 8;
    GPR[UInt(Rd)] = ZeroExtend(Some(GPR[0], esize), 64)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_sum_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2) = {
    SEE = 2;
    let Rd = Slice(__opcode, 0, 4);
    decode_op_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3) = {
    SEE = 3;
    let Rd = Slice(__opcode, 0, 4);
    decode_more_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 4) = {
    SEE = 4;
    let Rd = Slice(__opcode, 0, 4);
    decode_sides_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 5) = {
    SEE = 5;
    let Rd = Slice(__opcode, 0, 4);
    decode_base_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 6) = {
    SEE = 6;
    let Rd = Slice(__opcode, 0, 4);
    decode_state_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 7) = {
    SEE = 7;
    let Rd = Slice(__opcode, 0, 4);
    let size = Slice(__opcode, 4, 1);
    decode_some_aarch64_instrs_test(Rd, size)
}
