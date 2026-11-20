/* sail-arm's definitions that lower.roots reaches (sailgen --extract), as they are in:
     lower.sail
   BSD 3-Clause Clear License: LICENSE beside this file. Do not edit: a system's fixup goes in its overlay
   (README.md beside this file) */

enum Mode = {Mode_Near, Mode_Far}

val Sum : (bits(8), bits(8), Mode) -> bits(8)

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

