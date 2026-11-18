/* sail-arm's definitions that fixture.roots reaches (sailgen --extract), as they are in:
     upstream.sail
   BSD 3-Clause Clear License: LICENSE beside this file. Do not edit: an NX fixup goes in nx.sail */

enum Shade = {Shade_Dark, Shade_Light}

val flip : bits(4) -> bits(4)

function flip x = not_vec(x)

val state_write : (int, bits(64)) -> unit

function state_write (n, v) = {
    _R(n) = v
}

val decode_thing_aarch64_instrs_test : bits(4) -> unit

function decode_thing_aarch64_instrs_test Rd = {
    let s : Shade = Shade_Light;
    state_write(UInt(Rd), ZeroExtend(flip(Rd), 64))
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_thing_aarch64_instrs_test(Rd)
}

