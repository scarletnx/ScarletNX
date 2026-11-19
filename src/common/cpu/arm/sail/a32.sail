/* sail-arm's definitions that a32.roots reaches (sailgen --extract), as they are in:
     instrs32.sail
     v8_base.sail
   BSD 3-Clause Clear License: LICENSE beside this file. Do not edit: a system's fixup goes in its overlay
   (README.md beside this file) */

val execute_aarch32_instrs_ADC_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_ADC_i_Op_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), imm32, PSTATE.C);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_ADC_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_ADC_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_ADC_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2813) = {
    SEE = 2813;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_ADC_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_ADC_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_ADC_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), shifted, PSTATE.C);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_ADC_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ADC_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ADC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2815) = {
    SEE = 2815;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ADC_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_ADC_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ADC_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ADC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 2816) = {
    SEE = 2816;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ADC_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_ADC_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_ADC_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), shifted, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_ADC_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ADC_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_ADC_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2818) = {
    SEE = 2818;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ADC_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_ADD_i_OpA_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_ADD_i_OpA_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), imm32, 0b0);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_ADD_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_ADD_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        if Rn == 0b1111 & S == 0b0 then {
            throw(Error_See("ADR"))
        };
        if Rn == 0b1101 then {
            throw(Error_See("ADD (SP plus immediate)"))
        };
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_ADD_i_OpA_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2819) = {
    SEE = 2819;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_ADD_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_ADD_i_OpT_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_ADD_i_OpT_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), imm32, 0b0);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_ADD_i_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ADD_i_T1enc_A_txt (imm3, Rn, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = not_bool(InITBlock());
        let imm32 : bits(32) = ZeroExtend(imm3, 32);
        execute_aarch32_instrs_ADD_i_OpT_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2820) = {
    SEE = 2820;
    let imm3 = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ADD_i_T1enc_A_txt(imm3, Rn, Rd)
}

val decode_aarch32_instrs_ADD_i_T2enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_ADD_i_T2enc_A_txt (Rdn, imm8) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let setflags : bool = not_bool(InITBlock());
        let imm32 : bits(32) = ZeroExtend(imm8, 32);
        execute_aarch32_instrs_ADD_i_OpT_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2821) = {
    SEE = 2821;
    let Rdn = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_ADD_i_T2enc_A_txt(Rdn, imm8)
}

val execute_aarch32_instrs_ADD_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_ADD_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), shifted, 0b0);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_ADD_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ADD_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        if Rn == 0b1101 then {
            throw(Error_See("ADD (SP plus register)"))
        };
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ADD_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2824) = {
    SEE = 2824;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ADD_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_ADD_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ADD_r_T1enc_A_txt (Rm, Rn, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ADD_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2825) = {
    SEE = 2825;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ADD_r_T1enc_A_txt(Rm, Rn, Rd)
}

val decode_aarch32_instrs_ADD_r_T2enc_A_txt : (bits(1), bits(4), bits(3)) -> unit

function decode_aarch32_instrs_ADD_r_T2enc_A_txt (DN, Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        if (DN @ Rdn) == 0b1101 | Rm == 0b1101 then {
            throw(Error_See("ADD (SP plus register)"))
        };
        let 'd = UInt(DN @ Rdn);
        let 'n = d;
        let 'm = UInt(Rm);
        let setflags : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        if n == 15 & m == 15 then {
            throw(Error_Unpredictable())
        };
        if (d == 15 & InITBlock()) & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_ADD_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2826) = {
    SEE = 2826;
    let DN = Slice(__opcode, 7, 1);
    let Rm = Slice(__opcode, 3, 4);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ADD_r_T2enc_A_txt(DN, Rm, Rdn)
}

val execute_aarch32_instrs_ADD_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_ADD_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), shifted, 0b0);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_ADD_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ADD_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_ADD_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2828) = {
    SEE = 2828;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ADD_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_ADD_SP_i_Op_A_txt : forall 'd ('setflags : Bool), (0 <= 'd & 'd <= 15).
  (int('d), bits(32), bool('setflags)) -> unit

function execute_aarch32_instrs_ADD_SP_i_Op_A_txt (d, imm32, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(13), imm32, 0b0);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_ADD_SP_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_ADD_SP_i_A1enc_A_txt (cond, S, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_ADD_SP_i_Op_A_txt(d, imm32, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2829) = {
    SEE = 2829;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_ADD_SP_i_A1enc_A_txt(cond, S, Rd, imm12)
}

val decode_aarch32_instrs_ADD_SP_i_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_ADD_SP_i_T1enc_A_txt (Rd, imm8) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let setflags : bool = false;
        let imm32 : bits(32) = ZeroExtend(imm8 @ 0b00, 32);
        execute_aarch32_instrs_ADD_SP_i_Op_A_txt(d, imm32, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2830) = {
    SEE = 2830;
    let Rd = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_ADD_SP_i_T1enc_A_txt(Rd, imm8)
}

val decode_aarch32_instrs_ADD_SP_i_T2enc_A_txt : bits(7) -> unit

function decode_aarch32_instrs_ADD_SP_i_T2enc_A_txt imm7 = {
    if ConditionPassed() then {
        let 'd = 13;
        let setflags : bool = false;
        let imm32 : bits(32) = ZeroExtend(imm7 @ 0b00, 32);
        execute_aarch32_instrs_ADD_SP_i_Op_A_txt(d, imm32, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _] as __opcode)) if SEE < 2831) = {
    SEE = 2831;
    let imm7 = Slice(__opcode, 0, 7);
    decode_aarch32_instrs_ADD_SP_i_T2enc_A_txt(imm7)
}

val execute_aarch32_instrs_ADD_SP_r_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_ADD_SP_r_Op_A_txt (d, m, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(13), shifted, 0b0);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_ADD_SP_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ADD_SP_r_A1enc_A_txt (cond, S, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ADD_SP_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2834) = {
    SEE = 2834;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ADD_SP_r_A1enc_A_txt(cond, S, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_ADD_SP_r_T1enc_A_txt : (bits(1), bits(3)) -> unit

function decode_aarch32_instrs_ADD_SP_r_T1enc_A_txt (DM, Rdm) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(DM @ Rdm);
        let 'm = UInt(DM @ Rdm);
        let setflags : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        if (d == 15 & InITBlock()) & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        let shift_t = shift_t;
        let shift_n = shift_n;
        execute_aarch32_instrs_ADD_SP_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitone, bitone, bitzero, bitone, _, _, _] as __opcode)) if SEE < 2835) = {
    SEE = 2835;
    let DM = Slice(__opcode, 7, 1);
    let Rdm = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ADD_SP_r_T1enc_A_txt(DM, Rdm)
}

val decode_aarch32_instrs_ADD_SP_r_T2enc_A_txt : bits(4) -> unit

function decode_aarch32_instrs_ADD_SP_r_T2enc_A_txt Rm = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        if Rm == 0b1101 then {
            throw(Error_See("encoding T1"))
        };
        let 'd = 13;
        let 'm = UInt(Rm);
        let setflags : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ADD_SP_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, bitone, bitzero, bitone] as __opcode)) if SEE < 2836) = {
    SEE = 2836;
    let Rm = Slice(__opcode, 3, 4);
    decode_aarch32_instrs_ADD_SP_r_T2enc_A_txt(Rm)
}

val execute_aarch32_instrs_ADR_Op_A_txt : forall ('add : Bool) 'd, (0 <= 'd & 'd <= 15).
  (bool('add), int('d), bits(32)) -> unit

function execute_aarch32_instrs_ADR_Op_A_txt (add, d, imm32) = {
    let result : bits(32) = if add then Align(PC_read__1(), 4) + imm32 else
      Align(PC_read__1(), 4) - imm32;
    if d == 15 then {
        ALUWritePC(result)
    } else {
        R_set(d) = result
    }
}

val decode_aarch32_instrs_ADR_A1enc_A_txt : (bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_ADR_A1enc_A_txt (cond, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let imm32 : bits(32) = A32ExpandImm(imm12);
        let add : bool = true;
        execute_aarch32_instrs_ADR_Op_A_txt(add, d, imm32)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2838) = {
    SEE = 2838;
    let cond = Slice(__opcode, 28, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_ADR_A1enc_A_txt(cond, Rd, imm12)
}

val decode_aarch32_instrs_ADR_A2enc_A_txt : (bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_ADR_A2enc_A_txt (cond, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let imm32 : bits(32) = A32ExpandImm(imm12);
        let add : bool = false;
        execute_aarch32_instrs_ADR_Op_A_txt(add, d, imm32)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2839) = {
    SEE = 2839;
    let cond = Slice(__opcode, 28, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_ADR_A2enc_A_txt(cond, Rd, imm12)
}

val decode_aarch32_instrs_ADR_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_ADR_T1enc_A_txt (Rd, imm8) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let imm32 : bits(32) = ZeroExtend(imm8 @ 0b00, 32);
        let add : bool = true;
        execute_aarch32_instrs_ADR_Op_A_txt(add, d, imm32)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2840) = {
    SEE = 2840;
    let Rd = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_ADR_T1enc_A_txt(Rd, imm8)
}

val execute_aarch32_instrs_AND_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (bits(1), int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_AND_i_Op_A_txt (carry, d, imm32, n, setflags) = {
    let result : bits(32) = R_read(n) & imm32;
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_AND_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_AND_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_AND_i_Op_A_txt(carry, d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2843) = {
    SEE = 2843;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_AND_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_AND_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_AND_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) & shifted;
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_AND_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_AND_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_AND_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2845) = {
    SEE = 2845;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_AND_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_AND_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_AND_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_AND_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 2846) = {
    SEE = 2846;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_AND_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_AND_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_AND_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) & shifted;
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_AND_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_AND_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_AND_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2848) = {
    SEE = 2848;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_AND_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_ASR_i_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  ('m in {0, 1, 2, 3, 4, 5, 6, 7} & 'd in {0, 1, 2, 3, 4, 5, 6, 7}).
  (int('d), int('m), bool('setflags), int('shift_n)) -> unit

function execute_aarch32_instrs_ASR_i_Op_A_txt (d, m, setflags, shift_n) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    (result, carry) = Shift_C(R_read(m), SRType_ASR, shift_n, PSTATE.C);
    if d == 15 then {
        ALUWritePC(result)
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_ASR_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ASR_i_T1enc_A_txt (imm5, Rm, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        let (_, __tup_1 : int) = DecodeImmShift(0b10, imm5) in
          {
              shift_n = __tup_1
          };
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ASR_i_Op_A_txt(d, m, setflags, shift_n)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2849) = {
    SEE = 2849;
    let imm5 = Slice(__opcode, 6, 5);
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ASR_i_T1enc_A_txt(imm5, Rm, Rd)
}

val execute_aarch32_instrs_ASR_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool),
  ('n in {0, 1, 2, 3, 4, 5, 6, 7} & 'm in {0, 1, 2, 3, 4, 5, 6, 7} & 'd in {0, 1, 2, 3, 4, 5, 6, 7}).
  (int('d), int('m), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_ASR_r_Op_A_txt (d, m, n, setflags) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(m)[7 .. 0]);
    (result, carry) = Shift_C(R_read(n), SRType_ASR, shift_n, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_ASR_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ASR_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        execute_aarch32_instrs_ASR_r_Op_A_txt(d, m, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 2850) = {
    SEE = 2850;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ASR_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_B_Op_A_txt : bits(32) -> unit

function execute_aarch32_instrs_B_Op_A_txt imm32 = {
    BranchWritePC(PC_read__1() + imm32, BranchType_DIR)
}

val decode_aarch32_instrs_B_A1enc_A_txt : (bits(4), bits(24)) -> unit

function decode_aarch32_instrs_B_A1enc_A_txt (cond, imm24) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let imm32 : bits(32) = SignExtend(imm24 @ 0b00, 32);
        execute_aarch32_instrs_B_Op_A_txt(imm32)
    };
    ()
}

function clause __DecodeA32_BranchBlock ((pc, ([_, _, _, _, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2851) = {
    SEE = 2851;
    let cond = Slice(__opcode, 28, 4);
    let imm24 = Slice(__opcode, 0, 24);
    decode_aarch32_instrs_B_A1enc_A_txt(cond, imm24)
}

val decode_aarch32_instrs_B_T1enc_A_txt : (bits(4), bits(8)) -> unit

function decode_aarch32_instrs_B_T1enc_A_txt (cond, imm8) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        if cond == 0b1110 then {
            throw(Error_See("UDF"))
        };
        if cond == 0b1111 then {
            throw(Error_See("SVC"))
        };
        let imm32 : bits(32) = SignExtend(imm8 @ 0b0, 32);
        if InITBlock() then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_B_Op_A_txt(imm32)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 8, 4) != 0b1111 & SEE < 2852) = {
    SEE = 2852;
    let cond = Slice(__opcode, 8, 4);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_B_T1enc_A_txt(cond, imm8)
}

val decode_aarch32_instrs_B_T2enc_A_txt : bits(11) -> unit

function decode_aarch32_instrs_B_T2enc_A_txt imm11 = {
    if ConditionPassed() then {
        let imm32 : bits(32) = SignExtend(imm11 @ 0b0, 32);
        if InITBlock() & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_B_Op_A_txt(imm32)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2853) = {
    SEE = 2853;
    let imm11 = Slice(__opcode, 0, 11);
    decode_aarch32_instrs_B_T2enc_A_txt(imm11)
}

val execute_aarch32_instrs_BIC_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (bits(1), int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_BIC_i_Op_A_txt (carry, d, imm32, n, setflags) = {
    let result : bits(32) = R_read(n) & not_vec(imm32);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_BIC_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_BIC_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_BIC_i_Op_A_txt(carry, d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2860) = {
    SEE = 2860;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_BIC_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_BIC_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_BIC_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) & not_vec(shifted);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_BIC_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_BIC_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_BIC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2862) = {
    SEE = 2862;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_BIC_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_BIC_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_BIC_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_BIC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 2863) = {
    SEE = 2863;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_BIC_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_BIC_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_BIC_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) & not_vec(shifted);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_BIC_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_BIC_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_BIC_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2865) = {
    SEE = 2865;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_BIC_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_BKPT_Op_A_txt : bits(16) -> unit

function execute_aarch32_instrs_BKPT_Op_A_txt imm16 = {
    AArch32_SoftwareBreakpoint(imm16)
}

val decode_aarch32_instrs_BKPT_T1enc_A_txt : bits(8) -> unit

function decode_aarch32_instrs_BKPT_T1enc_A_txt imm8 = {
    let imm16 : bits(16) = ZeroExtend(imm8, 16);
    execute_aarch32_instrs_BKPT_Op_A_txt(imm16)
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2867) = {
    SEE = 2867;
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_BKPT_T1enc_A_txt(imm8)
}

val execute_aarch32_instrs_BL_i_Op_A_txt : (bits(32), InstrSet) -> unit

function execute_aarch32_instrs_BL_i_Op_A_txt (imm32, targetInstrSet) = {
    if CurrentInstrSet() == InstrSet_A32 then {
        LR_write() = PC_read__1() - 4
    } else {
        LR_write() = PC_read__1()[31 .. 1] @ 0b1
    };
    targetAddress : bits(32) = undefined;
    if targetInstrSet == InstrSet_A32 then {
        targetAddress = Align(PC_read__1(), 4) + imm32
    } else {
        targetAddress = PC_read__1() + imm32
    };
    SelectInstrSet(targetInstrSet);
    BranchWritePC(targetAddress, BranchType_DIRCALL)
}

val decode_aarch32_instrs_BL_i_A1enc_A_txt : (bits(4), bits(24)) -> unit

function decode_aarch32_instrs_BL_i_A1enc_A_txt (cond, imm24) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let imm32 : bits(32) = SignExtend(imm24 @ 0b00, 32);
        let targetInstrSet : InstrSet = InstrSet_A32;
        execute_aarch32_instrs_BL_i_Op_A_txt(imm32, targetInstrSet)
    };
    ()
}

function clause __DecodeA32_BranchBlock ((pc, ([_, _, _, _, bitone, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2868) = {
    SEE = 2868;
    let cond = Slice(__opcode, 28, 4);
    let imm24 = Slice(__opcode, 0, 24);
    decode_aarch32_instrs_BL_i_A1enc_A_txt(cond, imm24)
}

val decode_aarch32_instrs_BL_i_A2enc_A_txt : (bits(1), bits(24)) -> unit

function decode_aarch32_instrs_BL_i_A2enc_A_txt (H, imm24) = {
    if ConditionPassed() then {
        let imm32 : bits(32) = SignExtend((imm24 @ H) @ 0b0, 32);
        let targetInstrSet : InstrSet = InstrSet_T32;
        execute_aarch32_instrs_BL_i_Op_A_txt(imm32, targetInstrSet)
    };
    ()
}

function clause __DecodeA32_BranchBlock ((pc, ([bitone, bitone, bitone, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2869) = {
    SEE = 2869;
    let H = Slice(__opcode, 24, 1);
    let imm24 = Slice(__opcode, 0, 24);
    decode_aarch32_instrs_BL_i_A2enc_A_txt(H, imm24)
}

val execute_aarch32_instrs_BLX_r_Op_A_txt : forall 'm, (0 <= 'm & 'm <= 15).
  int('m) -> unit

function execute_aarch32_instrs_BLX_r_Op_A_txt m = {
    let target : bits(32) = R_read(m);
    next_instr_addr : bits(32) = undefined;
    if CurrentInstrSet() == InstrSet_A32 then {
        let next_instr_addr : bits(32) = PC_read__1() - 4;
        LR_write() = next_instr_addr
    } else {
        let next_instr_addr : bits(32) = PC_read__1() - 2;
        LR_write() = next_instr_addr[31 .. 1] @ 0b1
    };
    BXWritePC(target, BranchType_INDCALL)
}

val decode_aarch32_instrs_BLX_r_A1enc_A_txt : (bits(4), bits(4)) -> unit

function decode_aarch32_instrs_BLX_r_A1enc_A_txt (cond, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'm = UInt(Rm);
        if m == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_BLX_r_Op_A_txt(m)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2872) = {
    SEE = 2872;
    let cond = Slice(__opcode, 28, 4);
    let Rm = Slice(__opcode, 0, 4);
    if (((((((((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[15]] != 0b1) | [__opcode[8]] != 0b1) | [__opcode[9]] != 0b1) | [__opcode[10]] != 0b1) | [__opcode[11]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_BLX_r_A1enc_A_txt(cond, Rm)
    }
}

val decode_aarch32_instrs_BLX_r_T1enc_A_txt : bits(4) -> unit

function decode_aarch32_instrs_BLX_r_T1enc_A_txt Rm = {
    if ConditionPassed() then {
        let 'm = UInt(Rm);
        if m == 15 then {
            throw(Error_Unpredictable())
        };
        if InITBlock() & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_BLX_r_Op_A_txt(m)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero] as __opcode)) if SEE < 2873) = {
    SEE = 2873;
    let Rm = Slice(__opcode, 3, 4);
    if ([__opcode[0]] != 0b0 | [__opcode[1]] != 0b0) | [__opcode[2]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_BLX_r_T1enc_A_txt(Rm)
    }
}

val execute_aarch32_instrs_BX_Op_A_txt : forall 'm, (0 <= 'm & 'm <= 15).
  int('m) -> unit

function execute_aarch32_instrs_BX_Op_A_txt m = {
    BXWritePC(R_read(m), BranchType_INDIR)
}

val decode_aarch32_instrs_BX_A1enc_A_txt : (bits(4), bits(4)) -> unit

function decode_aarch32_instrs_BX_A1enc_A_txt (cond, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'm = UInt(Rm);
        execute_aarch32_instrs_BX_Op_A_txt(m)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2874) = {
    SEE = 2874;
    let cond = Slice(__opcode, 28, 4);
    let Rm = Slice(__opcode, 0, 4);
    if (((((((((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[15]] != 0b1) | [__opcode[8]] != 0b1) | [__opcode[9]] != 0b1) | [__opcode[10]] != 0b1) | [__opcode[11]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_BX_A1enc_A_txt(cond, Rm)
    }
}

val decode_aarch32_instrs_BX_T1enc_A_txt : bits(4) -> unit

function decode_aarch32_instrs_BX_T1enc_A_txt Rm = {
    if ConditionPassed() then {
        let 'm = UInt(Rm);
        if InITBlock() & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_BX_Op_A_txt(m)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, bitzero, bitzero, bitzero] as __opcode)) if SEE < 2875) = {
    SEE = 2875;
    let Rm = Slice(__opcode, 3, 4);
    if ([__opcode[0]] != 0b0 | [__opcode[1]] != 0b0) | [__opcode[2]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_BX_T1enc_A_txt(Rm)
    }
}

val execute_aarch32_instrs_CBNZ_Op_A_txt : forall 'n ('nonzero : Bool),
  'n in {0, 1, 2, 3, 4, 5, 6, 7}.
  (bits(32), int('n), bool('nonzero)) -> unit

function execute_aarch32_instrs_CBNZ_Op_A_txt (imm32, n, nonzero) = {
    if nonzero != IsZero(R_read(n)) then {
        CBWritePC(PC_read__1() + imm32)
    };
    ()
}

val decode_aarch32_instrs_CBNZ_T1enc_A_txt : (bits(1), bits(1), bits(5), bits(3)) -> unit

function decode_aarch32_instrs_CBNZ_T1enc_A_txt (op, i, imm5, Rn) = {
    let 'n = UInt(Rn);
    let imm32 : bits(32) = ZeroExtend((i @ imm5) @ 0b0, 32);
    let nonzero : bool = op == 0b1;
    if InITBlock() then {
        throw(Error_Unpredictable())
    };
    execute_aarch32_instrs_CBNZ_Op_A_txt(imm32, n, nonzero)
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, _, bitzero, _, bitone, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2878) = {
    SEE = 2878;
    let op = Slice(__opcode, 11, 1);
    let i = Slice(__opcode, 9, 1);
    let imm5 = Slice(__opcode, 3, 5);
    let Rn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_CBNZ_T1enc_A_txt(op, i, imm5, Rn)
}

val execute_aarch32_instrs_CMN_i_Op_A_txt : forall 'n, (0 <= 'n & 'n <= 15).
  (bits(32), int('n)) -> unit

function execute_aarch32_instrs_CMN_i_Op_A_txt (imm32, n) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), imm32, 0b0);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
}

val decode_aarch32_instrs_CMN_i_A1enc_A_txt : (bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_CMN_i_A1enc_A_txt (cond, Rn, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_CMN_i_Op_A_txt(imm32, n)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2883) = {
    SEE = 2883;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm12 = Slice(__opcode, 0, 12);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_CMN_i_A1enc_A_txt(cond, Rn, imm12)
    }
}

val execute_aarch32_instrs_CMN_r_Op_A_txt : forall 'm 'n 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_CMN_r_Op_A_txt (m, n, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), shifted, 0b0);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
}

val decode_aarch32_instrs_CMN_r_A1enc_A_txt : (bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_CMN_r_A1enc_A_txt (cond, Rn, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_CMN_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2885) = {
    SEE = 2885;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_CMN_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
    }
}

val decode_aarch32_instrs_CMN_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_CMN_r_T1enc_A_txt (Rm, Rn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_CMN_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 2886) = {
    SEE = 2886;
    let Rm = Slice(__opcode, 3, 3);
    let Rn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_CMN_r_T1enc_A_txt(Rm, Rn)
}

val execute_aarch32_instrs_CMN_rr_Op_A_txt : forall 'm 'n 's,
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('s), SRType) -> unit

function execute_aarch32_instrs_CMN_rr_Op_A_txt (m, n, s, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), shifted, 0b0);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
}

val decode_aarch32_instrs_CMN_rr_A1enc_A_txt : (bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_CMN_rr_A1enc_A_txt (cond, Rn, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let shift_t : SRType = DecodeRegShift(stype);
        if (n == 15 | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_CMN_rr_Op_A_txt(m, n, s, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2888) = {
    SEE = 2888;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_CMN_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
    }
}

val execute_aarch32_instrs_CMP_i_Op_A_txt : forall 'n, (0 <= 'n & 'n <= 15).
  (bits(32), int('n)) -> unit

function execute_aarch32_instrs_CMP_i_Op_A_txt (imm32, n) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(imm32), 0b1);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
}

val decode_aarch32_instrs_CMP_i_A1enc_A_txt : (bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_CMP_i_A1enc_A_txt (cond, Rn, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_CMP_i_Op_A_txt(imm32, n)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2889) = {
    SEE = 2889;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm12 = Slice(__opcode, 0, 12);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_CMP_i_A1enc_A_txt(cond, Rn, imm12)
    }
}

val decode_aarch32_instrs_CMP_i_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_CMP_i_T1enc_A_txt (Rn, imm8) = {
    if ConditionPassed() then {
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm8, 32);
        execute_aarch32_instrs_CMP_i_Op_A_txt(imm32, n)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2890) = {
    SEE = 2890;
    let Rn = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_CMP_i_T1enc_A_txt(Rn, imm8)
}

val execute_aarch32_instrs_CMP_r_Op_A_txt : forall 'm 'n 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_CMP_r_Op_A_txt (m, n, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(shifted), 0b1);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
}

val decode_aarch32_instrs_CMP_r_A1enc_A_txt : (bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_CMP_r_A1enc_A_txt (cond, Rn, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_CMP_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2892) = {
    SEE = 2892;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_CMP_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
    }
}

val decode_aarch32_instrs_CMP_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_CMP_r_T1enc_A_txt (Rm, Rn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_CMP_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 2893) = {
    SEE = 2893;
    let Rm = Slice(__opcode, 3, 3);
    let Rn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_CMP_r_T1enc_A_txt(Rm, Rn)
}

val decode_aarch32_instrs_CMP_r_T2enc_A_txt : (bits(1), bits(4), bits(3)) -> unit

function decode_aarch32_instrs_CMP_r_T2enc_A_txt (N, Rm, Rn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(N @ Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        if n < 8 & m < 8 then {
            throw(Error_Unpredictable())
        };
        if n == 15 | m == 15 then {
            throw(Error_Unpredictable())
        };
        let shift_t = shift_t;
        let shift_n = shift_n;
        execute_aarch32_instrs_CMP_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2894) = {
    SEE = 2894;
    let N = Slice(__opcode, 7, 1);
    let Rm = Slice(__opcode, 3, 4);
    let Rn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_CMP_r_T2enc_A_txt(N, Rm, Rn)
}

val execute_aarch32_instrs_CMP_rr_Op_A_txt : forall 'm 'n 's,
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('s), SRType) -> unit

function execute_aarch32_instrs_CMP_rr_Op_A_txt (m, n, s, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(shifted), 0b1);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
}

val decode_aarch32_instrs_CMP_rr_A1enc_A_txt : (bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_CMP_rr_A1enc_A_txt (cond, Rn, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let shift_t : SRType = DecodeRegShift(stype);
        if (n == 15 | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_CMP_rr_Op_A_txt(m, n, s, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2896) = {
    SEE = 2896;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_CMP_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
    }
}

val execute_aarch32_instrs_EOR_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (bits(1), int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_EOR_i_Op_A_txt (carry, d, imm32, n, setflags) = {
    let result : bits(32) = EOR(R_read(n), imm32);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_EOR_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_EOR_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_EOR_i_Op_A_txt(carry, d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2903) = {
    SEE = 2903;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_EOR_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_EOR_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_EOR_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = EOR(R_read(n), shifted);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_EOR_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_EOR_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_EOR_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2905) = {
    SEE = 2905;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_EOR_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_EOR_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_EOR_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_EOR_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 2906) = {
    SEE = 2906;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_EOR_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_EOR_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_EOR_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = EOR(R_read(n), shifted);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_EOR_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_EOR_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_EOR_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 2908) = {
    SEE = 2908;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_EOR_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val decode_aarch32_instrs_IT_T1enc_A_txt : (bits(4), bits(4)) -> unit

function decode_aarch32_instrs_IT_T1enc_A_txt (firstcond, mask) = {
    if mask == 0b0000 then {
        throw(Error_See("Related encodings"))
    };
    if firstcond == 0b1111 | firstcond == 0b1110 & BitCount(mask) != 1 then {
        throw(Error_Unpredictable())
    };
    if InITBlock() then {
        throw(Error_Unpredictable())
    };
    execute_aarch32_instrs_IT_Op_A_txt(firstcond, mask)
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2911) = {
    SEE = 2911;
    let firstcond = Slice(__opcode, 4, 4);
    let mask = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_IT_T1enc_A_txt(firstcond, mask)
}

val decode_aarch32_instrs_LDM_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_LDM_T1enc_A_txt (Rn, register_list) = {
    if ConditionPassed() then {
        let 'n = UInt(Rn);
        let registers : bits(16) = 0b00000000 @ register_list;
        let wback : bool = [registers[n]] == 0b0;
        if BitCount(registers) < 1 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_LDM_Op_A_txt(n, registers, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2917) = {
    SEE = 2917;
    let Rn = Slice(__opcode, 8, 3);
    let register_list = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_LDM_T1enc_A_txt(Rn, register_list)
}

val execute_aarch32_instrs_LDRB_i_OpT_A_txt : forall ('add : Bool) ('index : Bool) 'n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15).
  (bool('add), bits(32), bool('index), int('n), int('t), bool('wback)) -> unit

function execute_aarch32_instrs_LDRB_i_OpT_A_txt (add, imm32, index, n, t, wback) = {
    let offset_addr : bits(32) = if add then R_read(n) + imm32 else
      R_read(n) - imm32;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(1, false, t, false)
    };
    R_set(t) = ZeroExtend(MemU_read(address, 1), 32);
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_LDRB_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDRB_i_T1enc_A_txt (imm5, Rn, Rt) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm5, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_LDRB_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2924) = {
    SEE = 2924;
    let imm5 = Slice(__opcode, 6, 5);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDRB_i_T1enc_A_txt(imm5, Rn, Rt)
}

val execute_aarch32_instrs_LDRB_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit

function execute_aarch32_instrs_LDRB_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(1, false, t, false)
    };
    R_set(t) = ZeroExtend(MemU_read(address, 1), 32);
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_LDRB_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDRB_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LDRB_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2930) = {
    SEE = 2930;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDRB_r_T1enc_A_txt(Rm, Rn, Rt)
}

val decode_aarch32_instrs_LDRH_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDRH_i_T1enc_A_txt (imm5, Rn, Rt) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm5 @ 0b0, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_LDRH_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2949) = {
    SEE = 2949;
    let imm5 = Slice(__opcode, 6, 5);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDRH_i_T1enc_A_txt(imm5, Rn, Rt)
}

val decode_aarch32_instrs_LDRH_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDRH_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LDRH_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2955) = {
    SEE = 2955;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDRH_r_T1enc_A_txt(Rm, Rn, Rt)
}

val execute_aarch32_instrs_LDR_i_OpT_A_txt : forall ('add : Bool) ('index : Bool) 'n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15).
  (bool('add), bits(32), bool('index), int('n), int('t), bool('wback)) -> unit

function execute_aarch32_instrs_LDR_i_OpT_A_txt (add, imm32, index, n, t, wback) = {
    let offset_addr : bits(32) = if add then R_read(n) + imm32 else
      R_read(n) - imm32;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(4, false, t, false)
    };
    let data : bits(32) = MemU_read(address, 4);
    if wback then {
        R_set(n) = offset_addr
    };
    if t == 15 then {
        if address[1 .. 0] == 0b00 then {
            LoadWritePC(data)
        } else {
            throw(Error_Unpredictable())
        }
    } else {
        R_set(t) = data
    }
}

val decode_aarch32_instrs_LDR_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDR_i_T1enc_A_txt (imm5, Rn, Rt) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm5 @ 0b00, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_LDR_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2961) = {
    SEE = 2961;
    let imm5 = Slice(__opcode, 6, 5);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDR_i_T1enc_A_txt(imm5, Rn, Rt)
}

val decode_aarch32_instrs_LDR_i_T2enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_LDR_i_T2enc_A_txt (Rt, imm8) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = 13;
        let imm32 : bits(32) = ZeroExtend(imm8 @ 0b00, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_LDR_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2962) = {
    SEE = 2962;
    let Rt = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_LDR_i_T2enc_A_txt(Rt, imm8)
}

val execute_aarch32_instrs_LDR_l_Op_A_txt : forall ('add : Bool) 't, (0 <= 't & 't <= 15).
  (bool('add), bits(32), int('t)) -> unit

function execute_aarch32_instrs_LDR_l_Op_A_txt (add, imm32, t) = {
    let base : bits(32) = Align(PC_read__1(), 4);
    let address : bits(32) = if add then base + imm32 else base - imm32;
    if t != 15 then {
        AArch32_SetLSInstructionSyndrome(4, false, t, false)
    };
    let data : bits(32) = MemU_read(address, 4);
    if t == 15 then {
        if address[1 .. 0] == 0b00 then {
            LoadWritePC(data)
        } else {
            throw(Error_Unpredictable())
        }
    } else {
        R_set(t) = data
    }
}

val decode_aarch32_instrs_LDR_l_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_LDR_l_T1enc_A_txt (Rt, imm8) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let imm32 : bits(32) = ZeroExtend(imm8 @ 0b00, 32);
        let add : bool = true;
        execute_aarch32_instrs_LDR_l_Op_A_txt(add, imm32, t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2966) = {
    SEE = 2966;
    let Rt = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_LDR_l_T1enc_A_txt(Rt, imm8)
}

val execute_aarch32_instrs_LDR_r_OpT_A_txt : forall 'm 'n 'shift_n 't,
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('shift_n), SRType, int('t)) -> unit

function execute_aarch32_instrs_LDR_r_OpT_A_txt (m, n, shift_n, shift_t, t) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = R_read(n) + offset;
    let address : bits(32) = offset_addr;
    if t != 15 then {
        AArch32_SetLSInstructionSyndrome(4, false, t, false)
    };
    let data : bits(32) = MemU_read(address, 4);
    if t == 15 then {
        if address[1 .. 0] == 0b00 then {
            LoadWritePC(data)
        } else {
            throw(Error_Unpredictable())
        }
    } else {
        R_set(t) = data
    }
}

val decode_aarch32_instrs_LDR_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDR_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LDR_r_OpT_A_txt(m, n, shift_n, shift_t, t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2969) = {
    SEE = 2969;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDR_r_T1enc_A_txt(Rm, Rn, Rt)
}

val execute_aarch32_instrs_LDRSB_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit

function execute_aarch32_instrs_LDRSB_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(1, true, t, false)
    };
    R_set(t) = SignExtend(MemU_read(address, 1), 32);
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_LDRSB_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDRSB_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LDRSB_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2977) = {
    SEE = 2977;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDRSB_r_T1enc_A_txt(Rm, Rn, Rt)
}

val decode_aarch32_instrs_LDRSH_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LDRSH_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LDRSH_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2988) = {
    SEE = 2988;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LDRSH_r_T1enc_A_txt(Rm, Rn, Rt)
}

val execute_aarch32_instrs_LSL_i_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  ('m in {0, 1, 2, 3, 4, 5, 6, 7} & 'd in {0, 1, 2, 3, 4, 5, 6, 7}).
  (int('d), int('m), bool('setflags), int('shift_n)) -> unit

function execute_aarch32_instrs_LSL_i_Op_A_txt (d, m, setflags, shift_n) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    (result, carry) = Shift_C(R_read(m), SRType_LSL, shift_n, PSTATE.C);
    if d == 15 then {
        ALUWritePC(result)
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_LSL_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LSL_i_T1enc_A_txt (imm5, Rm, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        if imm5 == 0b00000 then {
            throw(Error_See("MOV (register)"))
        };
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        let (_, __tup_1 : int) = DecodeImmShift(0b00, imm5) in
          {
              shift_n = __tup_1
          };
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LSL_i_Op_A_txt(d, m, setflags, shift_n)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2996) = {
    SEE = 2996;
    let imm5 = Slice(__opcode, 6, 5);
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LSL_i_T1enc_A_txt(imm5, Rm, Rd)
}

val execute_aarch32_instrs_LSL_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_LSL_r_Op_A_txt (d, m, n, setflags) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(m)[7 .. 0]);
    (result, carry) = Shift_C(R_read(n), SRType_LSL, shift_n, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_LSL_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LSL_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        execute_aarch32_instrs_LSL_r_Op_A_txt(d, m, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 2997) = {
    SEE = 2997;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LSL_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_LSR_i_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  ('m in {0, 1, 2, 3, 4, 5, 6, 7} & 'd in {0, 1, 2, 3, 4, 5, 6, 7}).
  (int('d), int('m), bool('setflags), int('shift_n)) -> unit

function execute_aarch32_instrs_LSR_i_Op_A_txt (d, m, setflags, shift_n) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    (result, carry) = Shift_C(R_read(m), SRType_LSR, shift_n, PSTATE.C);
    if d == 15 then {
        ALUWritePC(result)
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_LSR_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LSR_i_T1enc_A_txt (imm5, Rm, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        let (_, __tup_1 : int) = DecodeImmShift(0b01, imm5) in
          {
              shift_n = __tup_1
          };
        let 'shift_n = shift_n;
        execute_aarch32_instrs_LSR_i_Op_A_txt(d, m, setflags, shift_n)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 2999) = {
    SEE = 2999;
    let imm5 = Slice(__opcode, 6, 5);
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LSR_i_T1enc_A_txt(imm5, Rm, Rd)
}

val execute_aarch32_instrs_LSR_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool),
  ('n in {0, 1, 2, 3, 4, 5, 6, 7} & 'm in {0, 1, 2, 3, 4, 5, 6, 7} & 'd in {0, 1, 2, 3, 4, 5, 6, 7}).
  (int('d), int('m), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_LSR_r_Op_A_txt (d, m, n, setflags) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(m)[7 .. 0]);
    (result, carry) = Shift_C(R_read(n), SRType_LSR, shift_n, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_LSR_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_LSR_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        execute_aarch32_instrs_LSR_r_Op_A_txt(d, m, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3000) = {
    SEE = 3000;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_LSR_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_MOV_i_Op_A_txt : forall 'd ('setflags : Bool), (0 <= 'd & 'd <= 15).
  (bits(1), int('d), bits(32), bool('setflags)) -> unit

function execute_aarch32_instrs_MOV_i_Op_A_txt (carry, d, imm32, setflags) = {
    let result : bits(32) = imm32;
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_MOV_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_MOV_i_A1enc_A_txt (cond, S, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'd = UInt(Rd);
        let setflags : bool = S == 0b1;
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_MOV_i_Op_A_txt(carry, d, imm32, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, bitone, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3009) = {
    SEE = 3009;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    if (([__opcode[16]] != 0b0 | [__opcode[17]] != 0b0) | [__opcode[18]] != 0b0) | [__opcode[19]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_MOV_i_A1enc_A_txt(cond, S, Rd, imm12)
    }
}

val decode_aarch32_instrs_MOV_i_A2enc_A_txt : (bits(4), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_MOV_i_A2enc_A_txt (cond, imm4, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        let 'd = UInt(Rd);
        let setflags : bool = false;
        let imm32 : bits(32) = ZeroExtend(imm4 @ imm12, 32);
        if d == 15 then {
            throw(Error_Unpredictable())
        };
        let carry = carry;
        execute_aarch32_instrs_MOV_i_Op_A_txt(carry, d, imm32, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3010) = {
    SEE = 3010;
    let cond = Slice(__opcode, 28, 4);
    let imm4 = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_MOV_i_A2enc_A_txt(cond, imm4, Rd, imm12)
}

val decode_aarch32_instrs_MOV_i_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_MOV_i_T1enc_A_txt (Rd, imm8) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let setflags : bool = not_bool(InITBlock());
        let imm32 : bits(32) = ZeroExtend(imm8, 32);
        let carry : bits(1) = PSTATE.C;
        execute_aarch32_instrs_MOV_i_Op_A_txt(carry, d, imm32, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3011) = {
    SEE = 3011;
    let Rd = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_MOV_i_T1enc_A_txt(Rd, imm8)
}

val execute_aarch32_instrs_MOV_r_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_MOV_r_Op_A_txt (d, m, setflags, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = shifted;
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_MOV_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_MOV_r_A1enc_A_txt (cond, S, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_MOV_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3014) = {
    SEE = 3014;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[16]] != 0b0 | [__opcode[17]] != 0b0) | [__opcode[18]] != 0b0) | [__opcode[19]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_MOV_r_A1enc_A_txt(cond, S, Rd, imm5, stype, Rm)
    }
}

val decode_aarch32_instrs_MOV_r_T1enc_A_txt : (bits(1), bits(4), bits(3)) -> unit

function decode_aarch32_instrs_MOV_r_T1enc_A_txt (D, Rm, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(D @ Rd);
        let 'm = UInt(Rm);
        let setflags : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        if (d == 15 & InITBlock()) & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        let shift_t = shift_t;
        let shift_n = shift_n;
        execute_aarch32_instrs_MOV_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3015) = {
    SEE = 3015;
    let D = Slice(__opcode, 7, 1);
    let Rm = Slice(__opcode, 3, 4);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_MOV_r_T1enc_A_txt(D, Rm, Rd)
}

val decode_aarch32_instrs_MOV_r_T2enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_MOV_r_T2enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = true;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        if InITBlock() then {
            throw(Error_Unpredictable())
        };
        let shift_t = shift_t;
        let shift_n = shift_n;
        execute_aarch32_instrs_MOV_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3016) = {
    SEE = 3016;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_MOV_r_T2enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_MOV_rr_Op_A_txt : forall 'd 'm 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_MOV_rr_Op_A_txt (d, m, s, setflags, shift_t) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (result, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_MOV_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_MOV_rr_A1enc_A_txt (cond, S, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if (d == 15 | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_MOV_rr_Op_A_txt(d, m, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3018) = {
    SEE = 3018;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[16]] != 0b0 | [__opcode[17]] != 0b0) | [__opcode[18]] != 0b0) | [__opcode[19]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_MOV_rr_A1enc_A_txt(cond, S, Rd, Rs, stype, Rm)
    }
}

val decode_aarch32_instrs_MOV_rr_T1enc_A_txt : (bits(4), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_MOV_rr_T1enc_A_txt (op, Rs, Rdm) = {
    if ConditionPassed() then {
        if not_bool(op == 0b0010 | op == 0b0011 | op == 0b0100 | op == 0b0111) then {
            throw(Error_See("Related encodings"))
        };
        let 'd = UInt(Rdm);
        let 'm = UInt(Rdm);
        let 's = UInt(Rs);
        let setflags : bool = not_bool(InITBlock());
        let shift_t : SRType = DecodeRegShift([op[2]] @ [op[0]]);
        execute_aarch32_instrs_MOV_rr_Op_A_txt(d, m, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3019) = {
    SEE = 3019;
    let op = Slice(__opcode, 6, 4);
    let Rs = Slice(__opcode, 3, 3);
    let Rdm = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_MOV_rr_T1enc_A_txt(op, Rs, Rdm)
}

val execute_aarch32_instrs_MOVT_Op_A_txt : forall 'd, (0 <= 'd & 'd <= 15).
  (int('d), bits(16)) -> unit

function execute_aarch32_instrs_MOVT_Op_A_txt (d, imm16) = {
    R_set(d) = [R_read(d) with 31 .. 16 = imm16]
}

val decode_aarch32_instrs_MOVT_A1enc_A_txt : (bits(4), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_MOVT_A1enc_A_txt (cond, imm4, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let imm16 : bits(16) = imm4 @ imm12;
        if d == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_MOVT_Op_A_txt(d, imm16)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3021) = {
    SEE = 3021;
    let cond = Slice(__opcode, 28, 4);
    let imm4 = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_MOVT_A1enc_A_txt(cond, imm4, Rd, imm12)
}

val decode_aarch32_instrs_MUL_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_MUL_T1enc_A_txt (Rn, Rdm) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdm);
        let 'n = UInt(Rn);
        let 'm = UInt(Rdm);
        let setflags : bool = not_bool(InITBlock());
        execute_aarch32_instrs_MUL_Op_A_txt(d, m, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3028) = {
    SEE = 3028;
    let Rn = Slice(__opcode, 3, 3);
    let Rdm = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_MUL_T1enc_A_txt(Rn, Rdm)
}

val execute_aarch32_instrs_MVN_i_Op_A_txt : forall 'd ('setflags : Bool), (0 <= 'd & 'd <= 15).
  (bits(1), int('d), bits(32), bool('setflags)) -> unit

function execute_aarch32_instrs_MVN_i_Op_A_txt (carry, d, imm32, setflags) = {
    let result : bits(32) = not_vec(imm32);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_MVN_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_MVN_i_A1enc_A_txt (cond, S, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'd = UInt(Rd);
        let setflags : bool = S == 0b1;
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_MVN_i_Op_A_txt(carry, d, imm32, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3030) = {
    SEE = 3030;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    if (([__opcode[16]] != 0b0 | [__opcode[17]] != 0b0) | [__opcode[18]] != 0b0) | [__opcode[19]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_MVN_i_A1enc_A_txt(cond, S, Rd, imm12)
    }
}

val execute_aarch32_instrs_MVN_r_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_MVN_r_Op_A_txt (d, m, setflags, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = not_vec(shifted);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_MVN_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_MVN_r_A1enc_A_txt (cond, S, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_MVN_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3032) = {
    SEE = 3032;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[16]] != 0b0 | [__opcode[17]] != 0b0) | [__opcode[18]] != 0b0) | [__opcode[19]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_MVN_r_A1enc_A_txt(cond, S, Rd, imm5, stype, Rm)
    }
}

val decode_aarch32_instrs_MVN_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_MVN_r_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_MVN_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3033) = {
    SEE = 3033;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_MVN_r_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_MVN_rr_Op_A_txt : forall 'd 'm 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_MVN_rr_Op_A_txt (d, m, s, setflags, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = not_vec(shifted);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_MVN_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_MVN_rr_A1enc_A_txt (cond, S, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if (d == 15 | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_MVN_rr_Op_A_txt(d, m, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3035) = {
    SEE = 3035;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[16]] != 0b0 | [__opcode[17]] != 0b0) | [__opcode[18]] != 0b0) | [__opcode[19]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_MVN_rr_A1enc_A_txt(cond, S, Rd, Rs, stype, Rm)
    }
}

val execute_aarch32_instrs_NOP_Op_A_txt : unit -> unit

function execute_aarch32_instrs_NOP_Op_A_txt () = {
    ()
}

val decode_aarch32_instrs_NOP_T1enc_A_txt : unit -> unit

function decode_aarch32_instrs_NOP_T1enc_A_txt () = {
    if ConditionPassed() then {
        execute_aarch32_instrs_NOP_Op_A_txt()
    };
    ()
}

function clause __DecodeT16 ((pc, (0b1011111100000000 as __opcode)) if SEE < 3037) = {
    SEE = 3037;
    decode_aarch32_instrs_NOP_T1enc_A_txt()
}

val execute_aarch32_instrs_ORR_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (bits(1), int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_ORR_i_Op_A_txt (carry, d, imm32, n, setflags) = {
    let result : bits(32) = R_read(n) | imm32;
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_ORR_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_ORR_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_ORR_i_Op_A_txt(carry, d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3041) = {
    SEE = 3041;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_ORR_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_ORR_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_ORR_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) | shifted;
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            PSTATE.N = [result[31]];
            PSTATE.Z = IsZeroBit(result);
            PSTATE.C = carry
        };
        ()
    }
}

val decode_aarch32_instrs_ORR_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ORR_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ORR_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3043) = {
    SEE = 3043;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ORR_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_ORR_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ORR_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_ORR_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3044) = {
    SEE = 3044;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ORR_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_ORR_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_ORR_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) | shifted;
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_ORR_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_ORR_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_ORR_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3046) = {
    SEE = 3046;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_ORR_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val decode_aarch32_instrs_POP_T1enc_A_txt : (bits(1), bits(8)) -> unit

function decode_aarch32_instrs_POP_T1enc_A_txt (P, register_list) = {
    if ConditionPassed() then {
        let registers : bits(16) = (P @ 0b0000000) @ register_list;
        let UnalignedAllowed : bool = false;
        if BitCount(registers) < 1 then {
            throw(Error_Unpredictable())
        };
        if ([registers[15]] == 0b1 & InITBlock()) & not_bool(LastInITBlock()) then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_POP_Op_A_txt(UnalignedAllowed, registers)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3062) = {
    SEE = 3062;
    let P = Slice(__opcode, 8, 1);
    let register_list = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_POP_T1enc_A_txt(P, register_list)
}

val decode_aarch32_instrs_PUSH_T1enc_A_txt : (bits(1), bits(8)) -> unit

function decode_aarch32_instrs_PUSH_T1enc_A_txt (M, register_list) = {
    if ConditionPassed() then {
        let registers : bits(16) = ((0b0 @ M) @ 0b000000) @ register_list;
        let UnalignedAllowed : bool = false;
        if BitCount(registers) < 1 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_PUSH_Op_A_txt(UnalignedAllowed, registers)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3063) = {
    SEE = 3063;
    let M = Slice(__opcode, 8, 1);
    let register_list = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_PUSH_T1enc_A_txt(M, register_list)
}

val execute_aarch32_instrs_REV16_Op_A_txt : forall 'd 'm,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m)) -> unit

function execute_aarch32_instrs_REV16_Op_A_txt (d, m) = {
    result : bits(32) = undefined;
    result[31 .. 24] = R_read(m)[23 .. 16];
    result[23 .. 16] = R_read(m)[31 .. 24];
    result[15 .. 8] = R_read(m)[7 .. 0];
    result[7 .. 0] = R_read(m)[15 .. 8];
    R_set(d) = result
}

val decode_aarch32_instrs_REV16_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_REV16_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        execute_aarch32_instrs_REV16_Op_A_txt(d, m)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3087) = {
    SEE = 3087;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_REV16_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_REV_Op_A_txt : forall 'd 'm,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m)) -> unit

function execute_aarch32_instrs_REV_Op_A_txt (d, m) = {
    result : bits(32) = undefined;
    result[31 .. 24] = R_read(m)[7 .. 0];
    result[23 .. 16] = R_read(m)[15 .. 8];
    result[15 .. 8] = R_read(m)[23 .. 16];
    result[7 .. 0] = R_read(m)[31 .. 24];
    R_set(d) = result
}

val decode_aarch32_instrs_REV_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_REV_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        execute_aarch32_instrs_REV_Op_A_txt(d, m)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3090) = {
    SEE = 3090;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_REV_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_REVSH_Op_A_txt : forall 'd 'm,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m)) -> unit

function execute_aarch32_instrs_REVSH_Op_A_txt (d, m) = {
    result : bits(32) = undefined;
    result[31 .. 8] = SignExtend(R_read(m)[7 .. 0], 24);
    result[7 .. 0] = R_read(m)[15 .. 8];
    R_set(d) = result
}

val decode_aarch32_instrs_REVSH_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_REVSH_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        execute_aarch32_instrs_REVSH_Op_A_txt(d, m)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3093) = {
    SEE = 3093;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_REVSH_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_ROR_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool),
  ('n in {0, 1, 2, 3, 4, 5, 6, 7} & 'm in {0, 1, 2, 3, 4, 5, 6, 7} & 'd in {0, 1, 2, 3, 4, 5, 6, 7}).
  (int('d), int('m), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_ROR_r_Op_A_txt (d, m, n, setflags) = {
    carry : bits(1) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(m)[7 .. 0]);
    (result, carry) = Shift_C(R_read(n), SRType_ROR, shift_n, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        PSTATE.N = [result[31]];
        PSTATE.Z = IsZeroBit(result);
        PSTATE.C = carry
    };
    ()
}

val decode_aarch32_instrs_ROR_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_ROR_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        execute_aarch32_instrs_ROR_r_Op_A_txt(d, m, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3095) = {
    SEE = 3095;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_ROR_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_RSB_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_RSB_i_Op_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(not_vec(R_read(n)), imm32, 0b1);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_RSB_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_RSB_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_RSB_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3096) = {
    SEE = 3096;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_RSB_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val decode_aarch32_instrs_RSB_i_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_RSB_i_T1enc_A_txt (Rn, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = not_bool(InITBlock());
        let imm32 : bits(32) = Zeros(32);
        execute_aarch32_instrs_RSB_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3097) = {
    SEE = 3097;
    let Rn = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_RSB_i_T1enc_A_txt(Rn, Rd)
}

val execute_aarch32_instrs_RSB_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_RSB_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(not_vec(R_read(n)), shifted, 0b1);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_RSB_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_RSB_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_RSB_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3099) = {
    SEE = 3099;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_RSB_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val execute_aarch32_instrs_RSB_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_RSB_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(not_vec(R_read(n)), shifted, 0b1);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_RSB_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_RSB_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_RSB_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3101) = {
    SEE = 3101;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_RSB_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_RSC_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_RSC_i_Op_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(not_vec(R_read(n)), imm32, PSTATE.C);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_RSC_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_RSC_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_RSC_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3102) = {
    SEE = 3102;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_RSC_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_RSC_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_RSC_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(not_vec(R_read(n)), shifted, PSTATE.C);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_RSC_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_RSC_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_RSC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3103) = {
    SEE = 3103;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_RSC_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val execute_aarch32_instrs_RSC_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_RSC_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(not_vec(R_read(n)), shifted, PSTATE.C);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_RSC_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_RSC_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_RSC_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3104) = {
    SEE = 3104;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_RSC_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_SBC_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_SBC_i_Op_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(imm32), PSTATE.C);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_SBC_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_SBC_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_SBC_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3113) = {
    SEE = 3113;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_SBC_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val execute_aarch32_instrs_SBC_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_SBC_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(shifted), PSTATE.C);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_SBC_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_SBC_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_SBC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3115) = {
    SEE = 3115;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_SBC_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_SBC_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_SBC_r_T1enc_A_txt (Rm, Rdn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_SBC_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3116) = {
    SEE = 3116;
    let Rm = Slice(__opcode, 3, 3);
    let Rdn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_SBC_r_T1enc_A_txt(Rm, Rdn)
}

val execute_aarch32_instrs_SBC_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_SBC_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(shifted), PSTATE.C);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_SBC_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_SBC_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_SBC_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3118) = {
    SEE = 3118;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_SBC_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_SETEND_Op_A_txt : forall ('set_bigend : Bool).
  bool('set_bigend) -> unit

function execute_aarch32_instrs_SETEND_Op_A_txt set_bigend = {
    AArch32_CheckSETENDEnabled();
    PSTATE.E = if set_bigend then 0b1 else 0b0
}

val decode_aarch32_instrs_SETEND_T1enc_A_txt : bits(1) -> unit

function decode_aarch32_instrs_SETEND_T1enc_A_txt E = {
    let set_bigend : bool = E == 0b1;
    if InITBlock() then {
        throw(Error_Unpredictable())
    };
    execute_aarch32_instrs_SETEND_Op_A_txt(set_bigend)
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, bitzero, bitzero, bitzero] as __opcode)) if SEE < 3126) = {
    SEE = 3126;
    let E = Slice(__opcode, 3, 1);
    if ([__opcode[0]] != 0b0 | [__opcode[1]] != 0b0) | [__opcode[2]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_SETEND_T1enc_A_txt(E)
    }
}

val execute_aarch32_instrs_SEV_Op_A_txt : unit -> unit

function execute_aarch32_instrs_SEV_Op_A_txt () = {
    SendEvent()
}

val decode_aarch32_instrs_SEV_T1enc_A_txt : unit -> unit

function decode_aarch32_instrs_SEV_T1enc_A_txt () = {
    if ConditionPassed() then {
        execute_aarch32_instrs_SEV_Op_A_txt()
    };
    ()
}

function clause __DecodeT16 ((pc, (0b1011111101000000 as __opcode)) if SEE < 3128) = {
    SEE = 3128;
    decode_aarch32_instrs_SEV_T1enc_A_txt()
}

val decode_aarch32_instrs_STM_T1enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_STM_T1enc_A_txt (Rn, register_list) = {
    if ConditionPassed() then {
        let 'n = UInt(Rn);
        let registers : bits(16) = 0b00000000 @ register_list;
        let wback : bool = true;
        if BitCount(registers) < 1 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_STM_Op_A_txt(n, registers, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3187) = {
    SEE = 3187;
    let Rn = Slice(__opcode, 8, 3);
    let register_list = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_STM_T1enc_A_txt(Rn, register_list)
}

val execute_aarch32_instrs_STRB_i_OpT_A_txt : forall ('add : Bool) ('index : Bool) 'n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15).
  (bool('add), bits(32), bool('index), int('n), int('t), bool('wback)) -> unit

function execute_aarch32_instrs_STRB_i_OpT_A_txt (add, imm32, index, n, t, wback) = {
    let offset_addr : bits(32) = if add then R_read(n) + imm32 else
      R_read(n) - imm32;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(1, false, t, false)
    };
    MemU_set(address, 1) = R_read(t)[7 .. 0];
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_STRB_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_STRB_i_T1enc_A_txt (imm5, Rn, Rt) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm5, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_STRB_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3194) = {
    SEE = 3194;
    let imm5 = Slice(__opcode, 6, 5);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_STRB_i_T1enc_A_txt(imm5, Rn, Rt)
}

val execute_aarch32_instrs_STRB_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit

function execute_aarch32_instrs_STRB_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(1, false, t, false)
    };
    MemU_set(address, 1) = R_read(t)[7 .. 0];
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_STRB_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_STRB_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_STRB_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3198) = {
    SEE = 3198;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_STRB_r_T1enc_A_txt(Rm, Rn, Rt)
}

val execute_aarch32_instrs_STRH_i_OpT_A_txt : forall ('add : Bool) ('index : Bool) 'n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15).
  (bool('add), bits(32), bool('index), int('n), int('t), bool('wback)) -> unit

function execute_aarch32_instrs_STRH_i_OpT_A_txt (add, imm32, index, n, t, wback) = {
    let offset_addr : bits(32) = if add then R_read(n) + imm32 else
      R_read(n) - imm32;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(2, false, t, false)
    };
    MemU_set(address, 2) = R_read(t)[15 .. 0];
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_STRH_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_STRH_i_T1enc_A_txt (imm5, Rn, Rt) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm5 @ 0b0, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_STRH_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3215) = {
    SEE = 3215;
    let imm5 = Slice(__opcode, 6, 5);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_STRH_i_T1enc_A_txt(imm5, Rn, Rt)
}

val execute_aarch32_instrs_STRH_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit

function execute_aarch32_instrs_STRH_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(2, false, t, false)
    };
    MemU_set(address, 2) = R_read(t)[15 .. 0];
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_STRH_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_STRH_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_STRH_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3219) = {
    SEE = 3219;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_STRH_r_T1enc_A_txt(Rm, Rn, Rt)
}

val execute_aarch32_instrs_STR_i_OpT_A_txt : forall ('add : Bool) ('index : Bool) 'n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15).
  (bool('add), bits(32), bool('index), int('n), int('t), bool('wback)) -> unit

function execute_aarch32_instrs_STR_i_OpT_A_txt (add, imm32, index, n, t, wback) = {
    let offset_addr : bits(32) = if add then R_read(n) + imm32 else
      R_read(n) - imm32;
    let address : bits(32) = if index then offset_addr else R_read(n);
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(4, false, t, false)
    };
    MemU_set(address, 4) = R_read(t);
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_STR_i_T1enc_A_txt : (bits(5), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_STR_i_T1enc_A_txt (imm5, Rn, Rt) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let imm32 : bits(32) = ZeroExtend(imm5 @ 0b00, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_STR_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3225) = {
    SEE = 3225;
    let imm5 = Slice(__opcode, 6, 5);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_STR_i_T1enc_A_txt(imm5, Rn, Rt)
}

val decode_aarch32_instrs_STR_i_T2enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_STR_i_T2enc_A_txt (Rt, imm8) = {
    if ConditionPassed() then {
        let 't = UInt(Rt);
        let 'n = 13;
        let imm32 : bits(32) = ZeroExtend(imm8 @ 0b00, 32);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        execute_aarch32_instrs_STR_i_OpT_A_txt(add, imm32, index, n, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3226) = {
    SEE = 3226;
    let Rt = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_STR_i_T2enc_A_txt(Rt, imm8)
}

val execute_aarch32_instrs_STR_r_Op_A_txt : forall ('add : Bool) ('index : Bool) 'm 'n 'shift_n 't ('wback : Bool),
  (0 <= 't & 't <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (bool('add), bool('index), int('m), int('n), int('shift_n), SRType, int('t), bool('wback)) -> unit

function execute_aarch32_instrs_STR_r_Op_A_txt (add, index, m, n, shift_n, shift_t, t, wback) = {
    let offset : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    let offset_addr : bits(32) = if add then R_read(n) + offset else
      R_read(n) - offset;
    let address : bits(32) = if index then offset_addr else R_read(n);
    data : bits(32) = undefined;
    if t == 15 then {
        data = PCStoreValue()
    } else {
        data = R_read(t)
    };
    if not_bool(wback) & t != 15 then {
        AArch32_SetLSInstructionSyndrome(4, false, t, false)
    };
    MemU_set(address, 4) = data;
    if wback then {
        R_set(n) = offset_addr
    };
    ()
}

val decode_aarch32_instrs_STR_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_STR_r_T1enc_A_txt (Rm, Rn, Rt) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 't = UInt(Rt);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let index : bool = true;
        let add : bool = true;
        let wback : bool = false;
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_STR_r_Op_A_txt(add, index, m, n, shift_n, shift_t, t, wback)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3230) = {
    SEE = 3230;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rt = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_STR_r_T1enc_A_txt(Rm, Rn, Rt)
}

val execute_aarch32_instrs_SUB_i_Op_A_txt : forall 'd 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), bits(32), int('n), bool('setflags)) -> unit

function execute_aarch32_instrs_SUB_i_Op_A_txt (d, imm32, n, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(imm32), 0b1);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_SUB_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_SUB_i_A1enc_A_txt (cond, S, Rn, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        if Rn == 0b1111 & S == 0b0 then {
            throw(Error_See("ADR"))
        };
        if Rn == 0b1101 then {
            throw(Error_See("SUB (SP minus immediate)"))
        };
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_SUB_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3235) = {
    SEE = 3235;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_SUB_i_A1enc_A_txt(cond, S, Rn, Rd, imm12)
}

val decode_aarch32_instrs_SUB_i_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_SUB_i_T1enc_A_txt (imm3, Rn, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let setflags : bool = not_bool(InITBlock());
        let imm32 : bits(32) = ZeroExtend(imm3, 32);
        execute_aarch32_instrs_SUB_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3236) = {
    SEE = 3236;
    let imm3 = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_SUB_i_T1enc_A_txt(imm3, Rn, Rd)
}

val decode_aarch32_instrs_SUB_i_T2enc_A_txt : (bits(3), bits(8)) -> unit

function decode_aarch32_instrs_SUB_i_T2enc_A_txt (Rdn, imm8) = {
    if ConditionPassed() then {
        let 'd = UInt(Rdn);
        let 'n = UInt(Rdn);
        let setflags : bool = not_bool(InITBlock());
        let imm32 : bits(32) = ZeroExtend(imm8, 32);
        execute_aarch32_instrs_SUB_i_Op_A_txt(d, imm32, n, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3237) = {
    SEE = 3237;
    let Rdn = Slice(__opcode, 8, 3);
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_SUB_i_T2enc_A_txt(Rdn, imm8)
}

val execute_aarch32_instrs_SUB_r_Op_A_txt : forall 'd 'm 'n ('setflags : Bool) 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_SUB_r_Op_A_txt (d, m, n, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(shifted), 0b1);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_SUB_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_SUB_r_A1enc_A_txt (cond, S, Rn, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        if Rn == 0b1101 then {
            throw(Error_See("SUB (SP minus register)"))
        };
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_SUB_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3241) = {
    SEE = 3241;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_SUB_r_A1enc_A_txt(cond, S, Rn, Rd, imm5, stype, Rm)
}

val decode_aarch32_instrs_SUB_r_T1enc_A_txt : (bits(3), bits(3), bits(3)) -> unit

function decode_aarch32_instrs_SUB_r_T1enc_A_txt (Rm, Rn, Rd) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let setflags : bool = not_bool(InITBlock());
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_SUB_r_Op_A_txt(d, m, n, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3242) = {
    SEE = 3242;
    let Rm = Slice(__opcode, 6, 3);
    let Rn = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_SUB_r_T1enc_A_txt(Rm, Rn, Rd)
}

val execute_aarch32_instrs_SUB_rr_Op_A_txt : forall 'd 'm 'n 's ('setflags : Bool),
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('n), int('s), bool('setflags), SRType) -> unit

function execute_aarch32_instrs_SUB_rr_Op_A_txt (d, m, n, s, setflags, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(n), not_vec(shifted), 0b1);
    R_set(d) = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    ()
}

val decode_aarch32_instrs_SUB_rr_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_SUB_rr_A1enc_A_txt (cond, S, Rn, Rd, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let setflags : bool = S == 0b1;
        let shift_t : SRType = DecodeRegShift(stype);
        if ((d == 15 | n == 15) | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_SUB_rr_Op_A_txt(d, m, n, s, setflags, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3244) = {
    SEE = 3244;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rn = Slice(__opcode, 16, 4);
    let Rd = Slice(__opcode, 12, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_SUB_rr_A1enc_A_txt(cond, S, Rn, Rd, Rs, stype, Rm)
}

val execute_aarch32_instrs_SUB_SP_i_Op_A_txt : forall 'd ('setflags : Bool), (0 <= 'd & 'd <= 15).
  (int('d), bits(32), bool('setflags)) -> unit

function execute_aarch32_instrs_SUB_SP_i_Op_A_txt (d, imm32, setflags) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    (result, nzcv) = AddWithCarry(R_read(13), not_vec(imm32), 0b1);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_SUB_SP_i_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_SUB_SP_i_A1enc_A_txt (cond, S, Rd, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'd = UInt(Rd);
        let setflags : bool = S == 0b1;
        let imm32 : bits(32) = A32ExpandImm(imm12);
        execute_aarch32_instrs_SUB_SP_i_Op_A_txt(d, imm32, setflags)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3245) = {
    SEE = 3245;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm12 = Slice(__opcode, 0, 12);
    decode_aarch32_instrs_SUB_SP_i_A1enc_A_txt(cond, S, Rd, imm12)
}

val decode_aarch32_instrs_SUB_SP_i_T1enc_A_txt : bits(7) -> unit

function decode_aarch32_instrs_SUB_SP_i_T1enc_A_txt imm7 = {
    if ConditionPassed() then {
        let 'd = 13;
        let setflags : bool = false;
        let imm32 : bits(32) = ZeroExtend(imm7 @ 0b00, 32);
        execute_aarch32_instrs_SUB_SP_i_Op_A_txt(d, imm32, setflags)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _] as __opcode)) if SEE < 3246) = {
    SEE = 3246;
    let imm7 = Slice(__opcode, 0, 7);
    decode_aarch32_instrs_SUB_SP_i_T1enc_A_txt(imm7)
}

val execute_aarch32_instrs_SUB_SP_r_Op_A_txt : forall 'd 'm ('setflags : Bool) 'shift_n,
  (0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), bool('setflags), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_SUB_SP_r_Op_A_txt (d, m, setflags, shift_n, shift_t) = {
    nzcv : bits(4) = undefined;
    result : bits(32) = undefined;
    let shifted : bits(32) = Shift(R_read(m), shift_t, shift_n, PSTATE.C);
    (result, nzcv) = AddWithCarry(R_read(13), not_vec(shifted), 0b1);
    if d == 15 then {
        if setflags then {
            ALUExceptionReturn(result)
        } else {
            ALUWritePC(result)
        }
    } else {
        R_set(d) = result;
        if setflags then {
            (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
        };
        ()
    }
}

val decode_aarch32_instrs_SUB_SP_r_A1enc_A_txt : (bits(4), bits(1), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_SUB_SP_r_A1enc_A_txt (cond, S, Rd, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let setflags : bool = S == 0b1;
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_SUB_SP_r_Op_A_txt(d, m, setflags, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3249) = {
    SEE = 3249;
    let cond = Slice(__opcode, 28, 4);
    let S = Slice(__opcode, 20, 1);
    let Rd = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    decode_aarch32_instrs_SUB_SP_r_A1enc_A_txt(cond, S, Rd, imm5, stype, Rm)
}

val execute_aarch32_instrs_SVC_Op_A_txt : bits(32) -> unit

function execute_aarch32_instrs_SVC_Op_A_txt imm32 = {
    AArch32_CheckForSVCTrap(imm32[15 .. 0]);
    AArch32_CallSupervisor(imm32[15 .. 0])
}

val decode_aarch32_instrs_SVC_T1enc_A_txt : bits(8) -> unit

function decode_aarch32_instrs_SVC_T1enc_A_txt imm8 = {
    if ConditionPassed() then {
        let imm32 : bits(32) = ZeroExtend(imm8, 32);
        execute_aarch32_instrs_SVC_Op_A_txt(imm32)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3252) = {
    SEE = 3252;
    let imm8 = Slice(__opcode, 0, 8);
    decode_aarch32_instrs_SVC_T1enc_A_txt(imm8)
}

val execute_aarch32_instrs_SXTB_Op_A_txt : forall 'd 'm 'rotation,
  (0 <= 'rotation & 'rotation <= 31 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('rotation)) -> unit

function execute_aarch32_instrs_SXTB_Op_A_txt (d, m, rotation) = {
    let rotated : bits(32) = ROR(R_read(m), rotation);
    R_set(d) = SignExtend(rotated[7 .. 0], 32)
}

val decode_aarch32_instrs_SXTB_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_SXTB_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let 'rotation = 0;
        execute_aarch32_instrs_SXTB_Op_A_txt(d, m, rotation)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3262) = {
    SEE = 3262;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_SXTB_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_SXTH_Op_A_txt : forall 'd 'm 'rotation,
  (0 <= 'rotation & 'rotation <= 31 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('rotation)) -> unit

function execute_aarch32_instrs_SXTH_Op_A_txt (d, m, rotation) = {
    let rotated : bits(32) = ROR(R_read(m), rotation);
    R_set(d) = SignExtend(rotated[15 .. 0], 32)
}

val decode_aarch32_instrs_SXTH_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_SXTH_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let 'rotation = 0;
        execute_aarch32_instrs_SXTH_Op_A_txt(d, m, rotation)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3265) = {
    SEE = 3265;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_SXTH_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_TEQ_i_Op_A_txt : forall 'n, (0 <= 'n & 'n <= 15).
  (bits(1), bits(32), int('n)) -> unit

function execute_aarch32_instrs_TEQ_i_Op_A_txt (carry, imm32, n) = {
    let result : bits(32) = EOR(R_read(n), imm32);
    PSTATE.N = [result[31]];
    PSTATE.Z = IsZeroBit(result);
    PSTATE.C = carry
}

val decode_aarch32_instrs_TEQ_i_A1enc_A_txt : (bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_TEQ_i_A1enc_A_txt (cond, Rn, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'n = UInt(Rn);
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_TEQ_i_Op_A_txt(carry, imm32, n)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3268) = {
    SEE = 3268;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm12 = Slice(__opcode, 0, 12);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_TEQ_i_A1enc_A_txt(cond, Rn, imm12)
    }
}

val execute_aarch32_instrs_TEQ_r_Op_A_txt : forall 'm 'n 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_TEQ_r_Op_A_txt (m, n, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = EOR(R_read(n), shifted);
    PSTATE.N = [result[31]];
    PSTATE.Z = IsZeroBit(result);
    PSTATE.C = carry
}

val decode_aarch32_instrs_TEQ_r_A1enc_A_txt : (bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_TEQ_r_A1enc_A_txt (cond, Rn, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_TEQ_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3270) = {
    SEE = 3270;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_TEQ_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
    }
}

val execute_aarch32_instrs_TEQ_rr_Op_A_txt : forall 'm 'n 's,
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('s), SRType) -> unit

function execute_aarch32_instrs_TEQ_rr_Op_A_txt (m, n, s, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = EOR(R_read(n), shifted);
    PSTATE.N = [result[31]];
    PSTATE.Z = IsZeroBit(result);
    PSTATE.C = carry
}

val decode_aarch32_instrs_TEQ_rr_A1enc_A_txt : (bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_TEQ_rr_A1enc_A_txt (cond, Rn, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let shift_t : SRType = DecodeRegShift(stype);
        if (n == 15 | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_TEQ_rr_Op_A_txt(m, n, s, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3272) = {
    SEE = 3272;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_TEQ_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
    }
}

val execute_aarch32_instrs_TST_i_Op_A_txt : forall 'n, (0 <= 'n & 'n <= 15).
  (bits(1), bits(32), int('n)) -> unit

function execute_aarch32_instrs_TST_i_Op_A_txt (carry, imm32, n) = {
    let result : bits(32) = R_read(n) & imm32;
    PSTATE.N = [result[31]];
    PSTATE.Z = IsZeroBit(result);
    PSTATE.C = carry
}

val decode_aarch32_instrs_TST_i_A1enc_A_txt : (bits(4), bits(4), bits(12)) -> unit

function decode_aarch32_instrs_TST_i_A1enc_A_txt (cond, Rn, imm12) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        carry : bits(1) = undefined;
        imm32 : bits(32) = undefined;
        let 'n = UInt(Rn);
        (imm32, carry) = A32ExpandImm_C(imm12, PSTATE.C);
        execute_aarch32_instrs_TST_i_Op_A_txt(carry, imm32, n)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3273) = {
    SEE = 3273;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm12 = Slice(__opcode, 0, 12);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_TST_i_A1enc_A_txt(cond, Rn, imm12)
    }
}

val execute_aarch32_instrs_TST_r_Op_A_txt : forall 'm 'n 'shift_n,
  (0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('shift_n), SRType) -> unit

function execute_aarch32_instrs_TST_r_Op_A_txt (m, n, shift_n, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) & shifted;
    PSTATE.N = [result[31]];
    PSTATE.Z = IsZeroBit(result);
    PSTATE.C = carry
}

val decode_aarch32_instrs_TST_r_A1enc_A_txt : (bits(4), bits(4), bits(5), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_TST_r_A1enc_A_txt (cond, Rn, imm5, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = DecodeImmShift(stype, imm5);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_TST_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3275) = {
    SEE = 3275;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let imm5 = Slice(__opcode, 7, 5);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_TST_r_A1enc_A_txt(cond, Rn, imm5, stype, Rm)
    }
}

val decode_aarch32_instrs_TST_r_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_TST_r_T1enc_A_txt (Rm, Rn) = {
    if ConditionPassed() then {
        shift_n : int = undefined;
        shift_t : SRType = undefined;
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        (shift_t, shift_n) = (SRType_LSL, 0);
        let 'shift_n = shift_n;
        execute_aarch32_instrs_TST_r_Op_A_txt(m, n, shift_n, shift_t)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3276) = {
    SEE = 3276;
    let Rm = Slice(__opcode, 3, 3);
    let Rn = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_TST_r_T1enc_A_txt(Rm, Rn)
}

val execute_aarch32_instrs_TST_rr_Op_A_txt : forall 'm 'n 's,
  (0 <= 's & 's <= 15 & 0 <= 'n & 'n <= 15 & 0 <= 'm & 'm <= 15).
  (int('m), int('n), int('s), SRType) -> unit

function execute_aarch32_instrs_TST_rr_Op_A_txt (m, n, s, shift_t) = {
    carry : bits(1) = undefined;
    shifted : bits(32) = undefined;
    let 'shift_n = UInt(R_read(s)[7 .. 0]);
    (shifted, carry) = Shift_C(R_read(m), shift_t, shift_n, PSTATE.C);
    let result : bits(32) = R_read(n) & shifted;
    PSTATE.N = [result[31]];
    PSTATE.Z = IsZeroBit(result);
    PSTATE.C = carry
}

val decode_aarch32_instrs_TST_rr_A1enc_A_txt : (bits(4), bits(4), bits(4), bits(2), bits(4)) -> unit

function decode_aarch32_instrs_TST_rr_A1enc_A_txt (cond, Rn, Rs, stype, Rm) = {
    if ConditionPassed() then {
        assert(cond != 0b1111);
        let 'n = UInt(Rn);
        let 'm = UInt(Rm);
        let 's = UInt(Rs);
        let shift_t : SRType = DecodeRegShift(stype);
        if (n == 15 | m == 15) | s == 15 then {
            throw(Error_Unpredictable())
        };
        execute_aarch32_instrs_TST_rr_Op_A_txt(m, n, s, shift_t)
    };
    ()
}

function clause __DecodeA32_DataProMisc ((pc, ([_, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, _, _, _, _, bitzero, bitzero, bitzero, bitzero, _, _, _, _, bitzero, _, _, bitone, _, _, _, _] as __opcode)) if Slice(__opcode, 28, 4) != 0b1111 & SEE < 3278) = {
    SEE = 3278;
    let cond = Slice(__opcode, 28, 4);
    let Rn = Slice(__opcode, 16, 4);
    let Rs = Slice(__opcode, 8, 4);
    let stype = Slice(__opcode, 5, 2);
    let Rm = Slice(__opcode, 0, 4);
    if (([__opcode[12]] != 0b0 | [__opcode[13]] != 0b0) | [__opcode[14]] != 0b0) | [__opcode[15]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_TST_rr_A1enc_A_txt(cond, Rn, Rs, stype, Rm)
    }
}

val execute_aarch32_instrs_UXTB_Op_A_txt : forall 'd 'm 'rotation,
  (0 <= 'rotation & 'rotation <= 31 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('rotation)) -> unit

function execute_aarch32_instrs_UXTB_Op_A_txt (d, m, rotation) = {
    let rotated : bits(32) = ROR(R_read(m), rotation);
    R_set(d) = ZeroExtend(rotated[7 .. 0], 32)
}

val decode_aarch32_instrs_UXTB_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_UXTB_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let 'rotation = 0;
        execute_aarch32_instrs_UXTB_Op_A_txt(d, m, rotation)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, _, _, _, _, _, _] as __opcode)) if SEE < 3345) = {
    SEE = 3345;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_UXTB_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_UXTH_Op_A_txt : forall 'd 'm 'rotation,
  (0 <= 'rotation & 'rotation <= 31 & 0 <= 'm & 'm <= 15 & 0 <= 'd & 'd <= 15).
  (int('d), int('m), int('rotation)) -> unit

function execute_aarch32_instrs_UXTH_Op_A_txt (d, m, rotation) = {
    let rotated : bits(32) = ROR(R_read(m), rotation);
    R_set(d) = ZeroExtend(rotated[15 .. 0], 32)
}

val decode_aarch32_instrs_UXTH_T1enc_A_txt : (bits(3), bits(3)) -> unit

function decode_aarch32_instrs_UXTH_T1enc_A_txt (Rm, Rd) = {
    if ConditionPassed() then {
        let 'd = UInt(Rd);
        let 'm = UInt(Rm);
        let 'rotation = 0;
        execute_aarch32_instrs_UXTH_Op_A_txt(d, m, rotation)
    };
    ()
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _] as __opcode)) if SEE < 3348) = {
    SEE = 3348;
    let Rm = Slice(__opcode, 3, 3);
    let Rd = Slice(__opcode, 0, 3);
    decode_aarch32_instrs_UXTH_T1enc_A_txt(Rm, Rd)
}

val execute_aarch32_instrs_WFE_Op_A_txt : unit -> unit

function execute_aarch32_instrs_WFE_Op_A_txt () = {
    if IsEventRegisterSet() then {
        ClearEventRegister()
    } else {
        if PSTATE.EL == EL0 then {
            AArch32_CheckForWFxTrap(EL1, WFxType_WFE)
        };
        if ((PSTATE.EL == EL0 | PSTATE.EL == EL1) & EL2Enabled()) & not_bool(IsInHost()) then {
            AArch32_CheckForWFxTrap(EL2, WFxType_WFE)
        };
        if HaveEL(EL3) & PSTATE.M != M32_Monitor then {
            AArch32_CheckForWFxTrap(EL3, WFxType_WFE)
        };
        let 'localtimeout = (1 << 64);
        WaitForEvent(localtimeout)
    }
}

val decode_aarch32_instrs_WFE_T1enc_A_txt : unit -> unit

function decode_aarch32_instrs_WFE_T1enc_A_txt () = {
    if ConditionPassed() then {
        execute_aarch32_instrs_WFE_Op_A_txt()
    };
    ()
}

function clause __DecodeT16 ((pc, (0b1011111100100000 as __opcode)) if SEE < 3761) = {
    SEE = 3761;
    decode_aarch32_instrs_WFE_T1enc_A_txt()
}

val execute_aarch32_instrs_WFI_Op_A_txt : unit -> unit

function execute_aarch32_instrs_WFI_Op_A_txt () = {
    if not_bool(InterruptPending()) then {
        if PSTATE.EL == EL0 then {
            AArch32_CheckForWFxTrap(EL1, WFxType_WFI)
        };
        if ((PSTATE.EL == EL0 | PSTATE.EL == EL1) & EL2Enabled()) & not_bool(IsInHost()) then {
            AArch32_CheckForWFxTrap(EL2, WFxType_WFI)
        };
        if HaveEL(EL3) & PSTATE.M != M32_Monitor then {
            AArch32_CheckForWFxTrap(EL3, WFxType_WFI)
        };
        let 'localtimeout = (1 << 64);
        WaitForInterrupt(localtimeout)
    };
    ()
}

val decode_aarch32_instrs_WFI_T1enc_A_txt : unit -> unit

function decode_aarch32_instrs_WFI_T1enc_A_txt () = {
    if ConditionPassed() then {
        execute_aarch32_instrs_WFI_Op_A_txt()
    };
    ()
}

function clause __DecodeT16 ((pc, (0b1011111100110000 as __opcode)) if SEE < 3764) = {
    SEE = 3764;
    decode_aarch32_instrs_WFI_T1enc_A_txt()
}

val execute_aarch32_instrs_YIELD_Op_A_txt : unit -> unit

function execute_aarch32_instrs_YIELD_Op_A_txt () = {
    Hint_Yield()
}

val decode_aarch32_instrs_YIELD_T1enc_A_txt : unit -> unit

function decode_aarch32_instrs_YIELD_T1enc_A_txt () = {
    if ConditionPassed() then {
        execute_aarch32_instrs_YIELD_Op_A_txt()
    };
    ()
}

function clause __DecodeT16 ((pc, (0b1011111100010000 as __opcode)) if SEE < 3767) = {
    SEE = 3767;
    decode_aarch32_instrs_YIELD_T1enc_A_txt()
}

val execute_aarch32_instrs_CPS_OpT_AS_txt : forall ('affectA : Bool) ('affectF : Bool) ('affectI : Bool) ('changemode : Bool) ('disable : Bool) ('enable : Bool).
  (bool('affectA), bool('affectF), bool('affectI), bool('changemode), bool('disable), bool('enable), bits(5)) -> unit

function execute_aarch32_instrs_CPS_OpT_AS_txt (affectA, affectF, affectI, changemode, disable, enable, mode) = {
    if PSTATE.EL != EL0 then {
        if enable then {
            if affectA then {
                PSTATE.A = 0b0
            };
            if affectI then {
                PSTATE.I = 0b0
            };
            if affectF then {
                PSTATE.F = 0b0
            };
            ()
        };
        if disable then {
            if affectA then {
                PSTATE.A = 0b1
            };
            if affectI then {
                PSTATE.I = 0b1
            };
            if affectF then {
                PSTATE.F = 0b1
            };
            ()
        };
        if changemode then {
            AArch32_WriteModeByInstr(mode)
        };
        ()
    };
    ()
}

val decode_aarch32_instrs_CPS_T1enc_AS_txt : (bits(1), bits(1), bits(1), bits(1)) -> unit

function decode_aarch32_instrs_CPS_T1enc_AS_txt (im, A, I, F) = {
    mode : bits(5) = undefined;
    if ((A @ I) @ F) == 0b000 then {
        throw(Error_Unpredictable())
    };
    let mode = mode;
    let enable : bool = im == 0b0;
    let disable : bool = im == 0b1;
    let changemode : bool = false;
    let affectA : bool = A == 0b1;
    let affectI : bool = I == 0b1;
    let affectF : bool = F == 0b1;
    if InITBlock() then {
        throw(Error_Unpredictable())
    };
    execute_aarch32_instrs_CPS_OpT_AS_txt(affectA, affectF, affectI, changemode, disable, enable, mode)
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, bitzero, _, _, _] as __opcode)) if SEE < 3770) = {
    SEE = 3770;
    let im = Slice(__opcode, 4, 1);
    let A = Slice(__opcode, 2, 1);
    let I = Slice(__opcode, 1, 1);
    let F = Slice(__opcode, 0, 1);
    decode_aarch32_instrs_CPS_T1enc_AS_txt(im, A, I, F)
}

val execute_aarch32_instrs_SEVL_Op_A_txt : unit -> unit

function execute_aarch32_instrs_SEVL_Op_A_txt () = {
    SendEventLocal()
}

val decode_aarch32_instrs_SEVL_T1enc_A_txt : unit -> unit

function decode_aarch32_instrs_SEVL_T1enc_A_txt () = {
    if ConditionPassed() then {
        execute_aarch32_instrs_SEVL_Op_A_txt()
    };
    ()
}

function clause __DecodeT16 ((pc, (0b1011111101010000 as __opcode)) if SEE < 3830) = {
    SEE = 3830;
    decode_aarch32_instrs_SEVL_T1enc_A_txt()
}

val execute_aarch32_instrs_SETPAN_Op_A_txt : bits(1) -> unit

function execute_aarch32_instrs_SETPAN_Op_A_txt value_name = {
    if PSTATE.EL != EL0 then {
        PSTATE.PAN = value_name
    };
    ()
}

val decode_aarch32_instrs_SETPAN_T1enc_A_txt : bits(1) -> unit

function decode_aarch32_instrs_SETPAN_T1enc_A_txt imm1 = {
    if InITBlock() then {
        throw(Error_Unpredictable())
    };
    if not_bool(HavePANExt()) then {
        throw(Error_Undefined())
    };
    let value_name : bits(1) = imm1;
    execute_aarch32_instrs_SETPAN_Op_A_txt(value_name)
}

function clause __DecodeT16 ((pc, ([bitone, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, bitzero, bitzero, bitzero] as __opcode)) if SEE < 3889) = {
    SEE = 3889;
    let imm1 = Slice(__opcode, 3, 1);
    if (([__opcode[4]] != 0b1 | [__opcode[0]] != 0b0) | [__opcode[1]] != 0b0) | [__opcode[2]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_aarch32_instrs_SETPAN_T1enc_A_txt(imm1)
    }
}

let M32_Monitor : bits(5) = 0b10110

let EL3 : bits(2) = 0b11

let EL2 : bits(2) = 0b10

let EL1 : bits(2) = 0b01

let EL0 : bits(2) = 0b00

struct ProcState = {
  N : bits(1),
  Z : bits(1),
  C : bits(1),
  V : bits(1),
  D : bits(1),
  A : bits(1),
  I : bits(1),
  F : bits(1),
  EXLOCK : bits(1),
  PAN : bits(1),
  UAO : bits(1),
  DIT : bits(1),
  TCO : bits(1),
  PM : bits(1),
  PPEND : bits(1),
  BTYPE : bits(2),
  ZA : bits(1),
  SM : bits(1),
  ALLINT : bits(1),
  SS : bits(1),
  IL : bits(1),
  EL : bits(2),
  nRW : bits(1),
  SP : bits(1),
  Q : bits(1),
  GE : bits(4),
  SSBS : bits(1),
  IT : bits(8),
  J : bits(1),
  T : bits(1),
  E : bits(1),
  M : bits(5)
}

register PSTATE : ProcState

enum InstrSet = {InstrSet_A64, InstrSet_A32, InstrSet_T32}

enum WFxType = {WFxType_WFE, WFxType_WFI, WFxType_WFET, WFxType_WFIT}

enum FPType = {
  FPType_Zero,
  FPType_Denormal,
  FPType_Nonzero,
  FPType_Infinity,
  FPType_QNaN,
  FPType_SNaN
}

/* register _R : vector(31, inc, bits(64)) */

overload _R = {get_R, set_R, set_R_bits}

enum BranchType = {
  BranchType_DIRCALL,
  BranchType_INDCALL,
  BranchType_ERET,
  BranchType_DBGEXIT,
  BranchType_RET,
  BranchType_DIR,
  BranchType_INDIR,
  BranchType_EXCEPTION,
  BranchType_TMFAIL,
  BranchType_RESET,
  BranchType_UNKNOWN
}

enum SRType = {SRType_LSL, SRType_LSR, SRType_ASR, SRType_ROR, SRType_RRX}

val R_read : forall 'n. int('n) -> bits(32)

function R_read n = {
    offset : int = undefined;
    if n == 15 then {
        let 'offset = if CurrentInstrSet() == InstrSet_A32 then 8 else 4;
        return(_PC[31 .. 0] + offset)
    } else {
        return(Rmode_read(n, PSTATE.M))
    }
}

val PC_read__1 : unit -> bits(32)

function PC_read__1 () = {
    return(R_read(15))
}

val ConditionHolds : bits(4) -> bool

function ConditionHolds cond = {
    result : bool = undefined;
    match cond[3 .. 1] {
      0b000 => {
          result = PSTATE.Z == 0b1
      },
      0b001 => {
          result = PSTATE.C == 0b1
      },
      0b010 => {
          result = PSTATE.N == 0b1
      },
      0b011 => {
          result = PSTATE.V == 0b1
      },
      0b100 => {
          result = PSTATE.C == 0b1 & PSTATE.Z == 0b0
      },
      0b101 => {
          result = PSTATE.N == PSTATE.V
      },
      0b110 => {
          result = PSTATE.N == PSTATE.V & PSTATE.Z == 0b0
      },
      0b111 => {
          result = true
      }
    };
    if [cond[0]] == 0b1 & cond != 0b1111 then {
        result = not_bool(result)
    };
    return(result)
}

val AArch32_CurrentCond : unit -> bits(4)

val AddWithCarry : forall 'N, 'N > 0.
  (bits('N), bits('N), bits(1)) -> (bits('N), bits(4))

function AddWithCarry (x, y, carry_in) = {
    let 'unsigned_sum = UInt(x) + UInt(y) + UInt(carry_in);
    let 'signed_sum = SInt(x) + SInt(y) + UInt(carry_in);
    let result : bits('N) = unsigned_sum['N - 1 .. 0];
    let n : bits(1) = [result['N - 1]];
    let z : bits(1) = if IsZero(result) then 0b1 else 0b0;
    let c : bits(1) = if UInt(result) == unsigned_sum then 0b0 else 0b1;
    let v : bits(1) = if SInt(result) == signed_sum then 0b0 else 0b1;
    return((result, ((n @ z) @ c) @ v))
}

val R_set : forall 'n. (int('n), bits(32)) -> unit

function R_set (n, value_name) = {
    Rmode_set(n, PSTATE.M) = value_name;
    return()
}

val LR_write : bits(32) -> unit

function LR_write value_name = {
    R_set(14) = value_name;
    return()
}

val RRX_C : forall 'N, 'N > 1. (bits('N), bits(1)) -> (bits('N), bits(1))

function RRX_C (x, carry_in) = {
    let result : bits('N) = [(x >> 1) with ('N - 1) = Bit(carry_in)];
    let carry_out : bits(1) = [x[0]];
    return((result, carry_out))
}

val Shift_C : forall 'N 'amount, 'N >= 0.
  (bits('N), SRType, int('amount), bits(1)) -> (bits('N), bits(1))

function Shift_C (value_name, srtype, amount, carry_in) = {
    assert(not_bool(srtype == SRType_RRX & amount != 1));
    result : bits('N) = undefined;
    carry_out : bits(1) = undefined;
    if amount == 0 then {
        (result, carry_out) = (value_name, carry_in)
    } else {
        match srtype {
          SRType_LSL => {
              (result, carry_out) = LSL_C(value_name, amount)
          },
          SRType_LSR => {
              (result, carry_out) = LSR_C(value_name, amount)
          },
          SRType_ASR => {
              assert(constraint(('N >= 0 & 'amount >= 0)));
              (result, carry_out) = ASR_C(value_name, amount)
          },
          SRType_ROR => {
              assert(constraint('N > 0));
              (result, carry_out) = ROR_C(value_name, amount)
          },
          SRType_RRX => {
              assert(constraint('N > 1));
              (result, carry_out) = RRX_C(value_name, carry_in)
          }
        }
    };
    return((result, carry_out))
}

val A32ExpandImm_C : (bits(12), bits(1)) -> (bits(32), bits(1))

function A32ExpandImm_C (imm12, carry_in) = {
    carry_out : bits(1) = undefined;
    imm32 : bits(32) = undefined;
    let unrotated_value : bits(32) = ZeroExtend(imm12[7 .. 0], 32);
    (imm32, carry_out) = Shift_C(unrotated_value, SRType_ROR, 2 * UInt(imm12[11 .. 8]), carry_in);
    return((imm32, carry_out))
}

val A32ExpandImm : bits(12) -> bits(32)

function A32ExpandImm imm12 = {
    imm32 : bits(32) = undefined;
    let (__tup_0 : bits(32), _) = A32ExpandImm_C(imm12, PSTATE.C) in
      {
          imm32 = __tup_0
      };
    return(imm32)
}

val DecodeImmShift : (bits(2), bits(5)) -> (SRType, int)

function DecodeImmShift (srtype, imm5) = {
    shift_t : SRType = undefined;
    shift_n : range(0, 32) = undefined;
    match srtype {
      0b00 => {
          shift_t = SRType_LSL;
          shift_n = UInt(imm5)
      },
      0b01 => {
          shift_t = SRType_LSR;
          shift_n = if imm5 == 0b00000 then 32 else UInt(imm5)
      },
      0b10 => {
          shift_t = SRType_ASR;
          shift_n = if imm5 == 0b00000 then 32 else UInt(imm5)
      },
      0b11 => {
          if imm5 == 0b00000 then {
              shift_t = SRType_RRX;
              shift_n = 1
          } else {
              shift_t = SRType_ROR;
              shift_n = UInt(imm5)
          }
      }
    };
    let 'shift_n = shift_n;
    return((shift_t, shift_n))
}

val DecodeRegShift : bits(2) -> SRType

function DecodeRegShift srtype = {
    shift_t : SRType = undefined;
    match srtype {
      0b00 => {
          shift_t = SRType_LSL
      },
      0b01 => {
          shift_t = SRType_LSR
      },
      0b10 => {
          shift_t = SRType_ASR
      },
      0b11 => {
          shift_t = SRType_ROR
      }
    };
    let shift_t = shift_t;
    return(shift_t)
}

val Shift : forall 'N 'amount, 'N >= 0.
  (bits('N), SRType, int('amount), bits(1)) -> bits('N)

function Shift (value_name, srtype, amount, carry_in) = {
    result : bits('N) = undefined;
    let (__tup_0 : bits('N), _) = Shift_C(value_name, srtype, amount, carry_in)
    in
      {
          result = __tup_0
      };
    return(result)
}

val BranchWritePC : (bits(32), BranchType) -> unit

function BranchWritePC (address_in, branch_type) = {
    address : bits(32) = address_in;
    if CurrentInstrSet() == InstrSet_A32 then {
        address[1 .. 0] = 0b00
    } else {
        address[0] = Bit(0b0)
    };
    let branch_conditional : bool = not_bool((match AArch32_CurrentCond() {
      [bitone, bitone, bitone, _] => true,
      _ => false
    }) : bool);
    BranchTo(address, branch_type, branch_conditional)
}

val CBWritePC : bits(32) -> unit

function CBWritePC address_in = {
    address : bits(32) = address_in;
    assert(CurrentInstrSet() == InstrSet_T32);
    address[0] = Bit(0b0);
    let branch_conditional : bool = true;
    BranchTo(address, BranchType_DIR, branch_conditional)
}

val PCStoreValue : unit -> bits(32)

function PCStoreValue () = {
    return(PC_read__1())
}

val ConditionPassed : unit -> bool

function ConditionPassed () = {
    return(ConditionHolds(AArch32_CurrentCond()))
}

