register GPR : vector(32, bits(64))

val InstructionSynchronizationBarrier : unit -> unit

val decode_path_aarch64_instrs_test : bits(4) -> unit

function decode_path_aarch64_instrs_test Rd = {
    if GPR[0] == 0x0000000000000000 then {
        InstructionSynchronizationBarrier()
    };
    ()
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1) = {
    SEE = 1;
    let Rd = Slice(__opcode, 0, 4);
    decode_path_aarch64_instrs_test(Rd)
}
