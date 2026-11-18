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
