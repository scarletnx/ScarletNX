enum Mode = {Mode_Near, Mode_Far}

enum Unused = {Unused_A, Unused_B}

val helper_of_sum : bits(8) -> bits(8)

function helper_of_sum x = not_vec(x)

val Sum : (bits(8), bits(8), Mode) -> bits(8)

function Sum (a, b, m) = {
    let u : Unused = Unused_A;
    helper_of_sum(a)
}

val decode_sum_aarch64_instrs_test : bits(4) -> unit

function decode_sum_aarch64_instrs_test Rd = {
    let r : bits(8) = Sum(0x01, 0x02, Mode_Near);
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_sum_aarch64_instrs_test(Rd)
}
