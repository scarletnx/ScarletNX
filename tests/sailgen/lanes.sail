register GPR : vector(32, bits(64))

val decode_lanes_aarch64_instrs_test : bits(4) -> unit

function decode_lanes_aarch64_instrs_test Rd = {
    let operand : bits(64) = GPR[0];
    result : bits(64) = undefined;
    foreach (e from 0 to 7 by 1 in inc) {
        result = Elem_set(result, e, 8, Elem_read(operand, e, 8) + 0x01)
    };
    GPR[UInt(Rd)] = result
}

val decode_carried_aarch64_instrs_test : bits(4) -> unit

function decode_carried_aarch64_instrs_test Rd = {
    let operand : bits(64) = GPR[0];
    result : bits(64) = undefined;
    shift : int = 0;
    foreach (e from 0 to 7 by 1 in inc) {
        result = Elem_set(result, e, 8, Elem_read(operand, e, 8) + 0x01);
        shift = shift + 8
    };
    GPR[UInt(Rd)] = result
}

val decode_last_aarch64_instrs_test : bits(4) -> unit

function decode_last_aarch64_instrs_test Rd = {
    let operand : bits(64) = GPR[0];
    element : bits(8) = undefined;
    foreach (e from 0 to 7 by 1 in inc) {
        element = Elem_read(operand, e, 8)
    };
    GPR[UInt(Rd)] = ZeroExtend(element, 64)
}

val decode_pairs_aarch64_instrs_test : bits(4) -> unit

function decode_pairs_aarch64_instrs_test Rd = {
    let operand1 : bits(64) = GPR[0];
    let operand2 : bits(64) = GPR[1];
    let concat : bits(128) = operand2 @ operand1;
    result : bits(64) = undefined;
    foreach (e from 0 to 7 by 1 in inc) {
        result = Elem_set(result, e, 8, Elem_read(concat, 2 * e, 8) + Elem_read(concat, 2 * e + 1, 8))
    };
    GPR[UInt(Rd)] = result
}

val decode_fold_aarch64_instrs_test : bits(4) -> unit

function decode_fold_aarch64_instrs_test Rd = {
    let operand : bits(64) = GPR[0];
    element : int = undefined;
    sum : int = UInt(Elem_read(operand, 0, 8));
    foreach (e from 1 to 7 by 1 in inc) {
        element = UInt(Elem_read(operand, e, 8));
        sum = sum + element
    };
    GPR[UInt(Rd)] = ZeroExtend(sum[15 .. 0], 64)
}

val decode_start_aarch64_instrs_test : bits(4) -> unit

function decode_start_aarch64_instrs_test Rd = {
    let operand : bits(64) = GPR[0];
    element : int = undefined;
    sum : int = UInt(Elem_read(operand, 1, 8));
    foreach (e from 1 to 7 by 1 in inc) {
        element = UInt(Elem_read(operand, e, 8));
        sum = sum + element
    };
    GPR[UInt(Rd)] = ZeroExtend(sum[15 .. 0], 64)
}

val decode_fixed_aarch64_instrs_test : (bits(4), bits(1)) -> unit

function decode_fixed_aarch64_instrs_test (Rd, U) = {
    let operand : bits(64) = GPR[0];
    result : bits(64) = undefined;
    element : bits(8) = undefined;
    foreach (e from 0 to 7 by 1 in inc) {
        element = Elem_read(operand, e, 8);
        if U == 0b1 then {
            foreach (i from 0 to 1 by 1 in inc) {
                element = element + 0x01
            }
        };
        result = Elem_set(result, e, 8, element)
    };
    GPR[UInt(Rd)] = result
}

val decode_free_aarch64_instrs_test : (bits(4), bits(1)) -> unit

function decode_free_aarch64_instrs_test (Rd, U) = {
    let operand : bits(64) = GPR[0];
    result : bits(64) = undefined;
    element : bits(8) = undefined;
    foreach (e from 0 to 7 by 1 in inc) {
        element = Elem_read(operand, e, 8);
        if U == 0b1 then {
            foreach (i from 0 to 1 by 1 in inc) {
                element = element + 0x01
            }
        };
        result = Elem_set(result, e, 8, element)
    };
    GPR[UInt(Rd)] = result
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_lanes_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2) = {
    SEE = 2;
    let Rd = Slice(__opcode, 0, 4);
    decode_carried_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3) = {
    SEE = 3;
    let Rd = Slice(__opcode, 0, 4);
    decode_last_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 4) = {
    SEE = 4;
    let Rd = Slice(__opcode, 0, 4);
    decode_pairs_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 5) = {
    SEE = 5;
    let Rd = Slice(__opcode, 0, 4);
    decode_fold_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 6) = {
    SEE = 6;
    let Rd = Slice(__opcode, 0, 4);
    decode_start_aarch64_instrs_test(Rd)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 7) = {
    SEE = 7;
    let Rd = Slice(__opcode, 0, 4);
    let U = Slice(__opcode, 19, 1);
    decode_fixed_aarch64_instrs_test(Rd, U)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 8) = {
    SEE = 8;
    let Rd = Slice(__opcode, 0, 4);
    let U = Slice(__opcode, 19, 1);
    decode_free_aarch64_instrs_test(Rd, U)
}
