/* sail-arm's definitions that a64.roots reaches (sailgen --extract), as they are in:
     instrs64.sail
     v8_base.sail
   BSD 3-Clause Clear License: LICENSE beside this file. Do not edit: an NX fixup goes in nx.sail */

val execute_aarch64_instrs_integer_arithmetic_add_sub_carry : forall 'd 'datasize 'm 'n ('setflags : Bool) ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('m), int('n), bool('setflags), bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_arithmetic_add_sub_carry (d, datasize, m, n, setflags, sub_op) = {
    result : bits('datasize) = undefined;
    let operand1 : bits('datasize) = X_read(n, datasize);
    operand2 : bits('datasize) = X_read(m, datasize);
    nzcv : bits(4) = undefined;
    if sub_op then {
        operand2 = not_vec(operand2)
    };
    (result, nzcv) = AddWithCarry(operand1, operand2, PSTATE.C);
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    X_set(d, datasize) = result
}

val decode_adc_aarch64_instrs_integer_arithmetic_add_sub_carry : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_adc_aarch64_instrs_integer_arithmetic_add_sub_carry (Rd, Rn, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    execute_aarch64_instrs_integer_arithmetic_add_sub_carry(d, datasize, m, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 6) = {
    SEE = 6;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_adc_aarch64_instrs_integer_arithmetic_add_sub_carry(Rd, Rn, Rm, S, op, sf)
}

val decode_adcs_aarch64_instrs_integer_arithmetic_add_sub_carry : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_adcs_aarch64_instrs_integer_arithmetic_add_sub_carry (Rd, Rn, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    execute_aarch64_instrs_integer_arithmetic_add_sub_carry(d, datasize, m, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 7) = {
    SEE = 7;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_adcs_aarch64_instrs_integer_arithmetic_add_sub_carry(Rd, Rn, Rm, S, op, sf)
}

val decode_sbc_aarch64_instrs_integer_arithmetic_add_sub_carry : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_sbc_aarch64_instrs_integer_arithmetic_add_sub_carry (Rd, Rn, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    execute_aarch64_instrs_integer_arithmetic_add_sub_carry(d, datasize, m, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 8) = {
    SEE = 8;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_sbc_aarch64_instrs_integer_arithmetic_add_sub_carry(Rd, Rn, Rm, S, op, sf)
}

val decode_sbcs_aarch64_instrs_integer_arithmetic_add_sub_carry : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_sbcs_aarch64_instrs_integer_arithmetic_add_sub_carry (Rd, Rn, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    execute_aarch64_instrs_integer_arithmetic_add_sub_carry(d, datasize, m, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 9) = {
    SEE = 9;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_sbcs_aarch64_instrs_integer_arithmetic_add_sub_carry(Rd, Rn, Rm, S, op, sf)
}

val execute_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg : forall 'd 'datasize 'm 'n ('setflags : Bool) 'shift_amount ('sub_op : Bool),
  (0 <= 'shift_amount & 'shift_amount <= 63 & 0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('m), int('n), bool('setflags), int('shift_amount), ShiftType, bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg (d, datasize, m, n, setflags, shift_amount, shift_type, sub_op) = {
    result : bits('datasize) = undefined;
    let operand1 : bits('datasize) = X_read(n, datasize);
    operand2 : bits('datasize) = ShiftReg(m, shift_type, shift_amount, datasize);
    nzcv : bits(4) = undefined;
    carry_in : bits(1) = undefined;
    if sub_op then {
        operand2 = not_vec(operand2);
        carry_in = 0b1
    } else {
        carry_in = 0b0
    };
    (result, nzcv) = AddWithCarry(operand1, operand2, carry_in);
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    X_set(d, datasize) = result
}

val decode_add_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(2), bits(1), bits(1), bits(1)) -> unit

function decode_add_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg (Rd, Rn, imm6, Rm, shift, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    if shift == 0b11 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    execute_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(d, datasize, m, n, setflags, shift_amount, shift_type, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 14) = {
    SEE = 14;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let shift = Slice(__opcode, 22, 2);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_add_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(Rd, Rn, imm6, Rm, shift, S, op, sf)
}

val decode_adds_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(2), bits(1), bits(1), bits(1)) -> unit

function decode_adds_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg (Rd, Rn, imm6, Rm, shift, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    if shift == 0b11 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    execute_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(d, datasize, m, n, setflags, shift_amount, shift_type, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitone, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 15) = {
    SEE = 15;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let shift = Slice(__opcode, 22, 2);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_adds_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(Rd, Rn, imm6, Rm, shift, S, op, sf)
}

val decode_sub_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(2), bits(1), bits(1), bits(1)) -> unit

function decode_sub_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg (Rd, Rn, imm6, Rm, shift, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    if shift == 0b11 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    execute_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(d, datasize, m, n, setflags, shift_amount, shift_type, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 16) = {
    SEE = 16;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let shift = Slice(__opcode, 22, 2);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_sub_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(Rd, Rn, imm6, Rm, shift, S, op, sf)
}

val decode_subs_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(2), bits(1), bits(1), bits(1)) -> unit

function decode_subs_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg (Rd, Rn, imm6, Rm, shift, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    if shift == 0b11 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    execute_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(d, datasize, m, n, setflags, shift_amount, shift_type, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 17) = {
    SEE = 17;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let shift = Slice(__opcode, 22, 2);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_subs_addsub_shift_aarch64_instrs_integer_arithmetic_add_sub_shiftedreg(Rd, Rn, imm6, Rm, shift, S, op, sf)
}

val execute_aarch64_instrs_integer_arithmetic_add_sub_immediate : forall 'd 'datasize 'n ('setflags : Bool) ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), bits('datasize), int('n), bool('setflags), bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_arithmetic_add_sub_immediate (d, datasize, imm, n, setflags, sub_op) = {
    result : bits('datasize) = undefined;
    let operand1 : bits('datasize) = if n == 31 then
      SP_read()[datasize - 1 .. 0]
    else
      X_read(n, datasize);
    operand2 : bits('datasize) = imm;
    nzcv : bits(4) = undefined;
    carry_in : bits(1) = undefined;
    if sub_op then {
        operand2 = not_vec(operand2);
        carry_in = 0b1
    } else {
        carry_in = 0b0
    };
    (result, nzcv) = AddWithCarry(operand1, operand2, carry_in);
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    if d == 31 & not_bool(setflags) then {
        SP_set() = ZeroExtend(result, 64)
    } else {
        X_set(d, datasize) = result
    }
}

val decode_add_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate : (bits(5), bits(5), bits(12), bits(1), bits(1), bits(1), bits(1)) -> unit

function decode_add_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate (Rd, Rn, imm12, sh, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    imm : bits('datasize) = undefined;
    match sh {
      0b0 => {
          imm = ZeroExtend(imm12, datasize)
      },
      0b1 => {
          imm = ZeroExtend(imm12 @ Zeros(12), datasize)
      }
    };
    let imm = imm;
    execute_aarch64_instrs_integer_arithmetic_add_sub_immediate(d, datasize, imm, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 18) = {
    SEE = 18;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let sh = Slice(__opcode, 22, 1);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_add_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate(Rd, Rn, imm12, sh, S, op, sf)
}

val decode_adds_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate : (bits(5), bits(5), bits(12), bits(1), bits(1), bits(1), bits(1)) -> unit

function decode_adds_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate (Rd, Rn, imm12, sh, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    imm : bits('datasize) = undefined;
    match sh {
      0b0 => {
          imm = ZeroExtend(imm12, datasize)
      },
      0b1 => {
          imm = ZeroExtend(imm12 @ Zeros(12), datasize)
      }
    };
    let imm = imm;
    execute_aarch64_instrs_integer_arithmetic_add_sub_immediate(d, datasize, imm, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 19) = {
    SEE = 19;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let sh = Slice(__opcode, 22, 1);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_adds_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate(Rd, Rn, imm12, sh, S, op, sf)
}

val decode_sub_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate : (bits(5), bits(5), bits(12), bits(1), bits(1), bits(1), bits(1)) -> unit

function decode_sub_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate (Rd, Rn, imm12, sh, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    imm : bits('datasize) = undefined;
    match sh {
      0b0 => {
          imm = ZeroExtend(imm12, datasize)
      },
      0b1 => {
          imm = ZeroExtend(imm12 @ Zeros(12), datasize)
      }
    };
    let imm = imm;
    execute_aarch64_instrs_integer_arithmetic_add_sub_immediate(d, datasize, imm, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 20) = {
    SEE = 20;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let sh = Slice(__opcode, 22, 1);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_sub_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate(Rd, Rn, imm12, sh, S, op, sf)
}

val decode_subs_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate : (bits(5), bits(5), bits(12), bits(1), bits(1), bits(1), bits(1)) -> unit

function decode_subs_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate (Rd, Rn, imm12, sh, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    imm : bits('datasize) = undefined;
    match sh {
      0b0 => {
          imm = ZeroExtend(imm12, datasize)
      },
      0b1 => {
          imm = ZeroExtend(imm12 @ Zeros(12), datasize)
      }
    };
    let imm = imm;
    execute_aarch64_instrs_integer_arithmetic_add_sub_immediate(d, datasize, imm, n, setflags, sub_op)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 21) = {
    SEE = 21;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let sh = Slice(__opcode, 22, 1);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_subs_addsub_imm_aarch64_instrs_integer_arithmetic_add_sub_immediate(Rd, Rn, imm12, sh, S, op, sf)
}

val execute_aarch64_instrs_integer_arithmetic_add_sub_extendedreg : forall 'd 'datasize 'm 'n ('setflags : Bool) 'shift ('sub_op : Bool),
  ('shift in {0, 1, 2, 3, 4, 5, 6, 7} & 0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), ExtendType, int('m), int('n), bool('setflags), int('shift), bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_arithmetic_add_sub_extendedreg (d, datasize, extend_type, m, n, setflags, shift, sub_op) = {
    result : bits('datasize) = undefined;
    let operand1 : bits('datasize) = if n == 31 then
      SP_read()[datasize - 1 .. 0]
    else
      X_read(n, datasize);
    operand2 : bits('datasize) = ExtendReg(m, extend_type, shift, datasize);
    nzcv : bits(4) = undefined;
    carry_in : bits(1) = undefined;
    if sub_op then {
        operand2 = not_vec(operand2);
        carry_in = 0b1
    } else {
        carry_in = 0b0
    };
    (result, nzcv) = AddWithCarry(operand1, operand2, carry_in);
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = nzcv
    };
    if d == 31 & not_bool(setflags) then {
        SP_set() = ZeroExtend(result, 64)
    } else {
        X_set(d, datasize) = result
    }
}

val decode_add_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg : (bits(5), bits(5), bits(3), bits(3), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_add_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg (Rd, Rn, imm3, option_name, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = UInt(imm3);
    if shift > 4 then {
        throw(Error_Undefined())
    };
    execute_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(d, datasize, extend_type, m, n, setflags, shift, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 22) = {
    SEE = 22;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm3 = Slice(__opcode, 10, 3);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_add_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(Rd, Rn, imm3, option_name, Rm, S, op, sf)
}

val decode_adds_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg : (bits(5), bits(5), bits(3), bits(3), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_adds_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg (Rd, Rn, imm3, option_name, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = UInt(imm3);
    if shift > 4 then {
        throw(Error_Undefined())
    };
    execute_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(d, datasize, extend_type, m, n, setflags, shift, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 23) = {
    SEE = 23;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm3 = Slice(__opcode, 10, 3);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_adds_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(Rd, Rn, imm3, option_name, Rm, S, op, sf)
}

val decode_sub_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg : (bits(5), bits(5), bits(3), bits(3), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_sub_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg (Rd, Rn, imm3, option_name, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = UInt(imm3);
    if shift > 4 then {
        throw(Error_Undefined())
    };
    execute_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(d, datasize, extend_type, m, n, setflags, shift, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 24) = {
    SEE = 24;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm3 = Slice(__opcode, 10, 3);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_sub_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(Rd, Rn, imm3, option_name, Rm, S, op, sf)
}

val decode_subs_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg : (bits(5), bits(5), bits(3), bits(3), bits(5), bits(1), bits(1), bits(1)) -> unit

function decode_subs_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg (Rd, Rn, imm3, option_name, Rm, S, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let setflags : bool = S == 0b1;
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = UInt(imm3);
    if shift > 4 then {
        throw(Error_Undefined())
    };
    execute_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(d, datasize, extend_type, m, n, setflags, shift, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 25) = {
    SEE = 25;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm3 = Slice(__opcode, 10, 3);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let S = Slice(__opcode, 29, 1);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_subs_addsub_ext_aarch64_instrs_integer_arithmetic_add_sub_extendedreg(Rd, Rn, imm3, option_name, Rm, S, op, sf)
}

val execute_aarch64_instrs_integer_arithmetic_address_pc_rel : forall 'd ('page : Bool), (0 <= 'd & 'd <= 31).
  (int('d), bits(64), bool('page)) -> unit

function execute_aarch64_instrs_integer_arithmetic_address_pc_rel (d, imm, page) = {
    base : bits(64) = PC_read();
    if page then {
        base[11 .. 0] = Zeros(12)
    };
    X_set(d, 64) = base + imm
}

val decode_adr_aarch64_instrs_integer_arithmetic_address_pc_rel : (bits(5), bits(19), bits(2), bits(1)) -> unit

function decode_adr_aarch64_instrs_integer_arithmetic_address_pc_rel (Rd, immhi, immlo, op) = {
    let 'd = UInt(Rd);
    let page : bool = op == 0b1;
    imm : bits(64) = undefined;
    if page then {
        imm = SignExtend((immhi @ immlo) @ Zeros(12), 64)
    } else {
        imm = SignExtend(immhi @ immlo, 64)
    };
    execute_aarch64_instrs_integer_arithmetic_address_pc_rel(d, imm, page)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 34) = {
    SEE = 34;
    let Rd = Slice(__opcode, 0, 5);
    let immhi = Slice(__opcode, 5, 19);
    let immlo = Slice(__opcode, 29, 2);
    let op = Slice(__opcode, 31, 1);
    decode_adr_aarch64_instrs_integer_arithmetic_address_pc_rel(Rd, immhi, immlo, op)
}

val decode_adrp_aarch64_instrs_integer_arithmetic_address_pc_rel : (bits(5), bits(19), bits(2), bits(1)) -> unit

function decode_adrp_aarch64_instrs_integer_arithmetic_address_pc_rel (Rd, immhi, immlo, op) = {
    let 'd = UInt(Rd);
    let page : bool = op == 0b1;
    imm : bits(64) = undefined;
    if page then {
        imm = SignExtend((immhi @ immlo) @ Zeros(12), 64)
    } else {
        imm = SignExtend(immhi @ immlo, 64)
    };
    execute_aarch64_instrs_integer_arithmetic_address_pc_rel(d, imm, page)
}

function clause __DecodeA64_DataProcImm ((pc, ([bitone, _, _, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 35) = {
    SEE = 35;
    let Rd = Slice(__opcode, 0, 5);
    let immhi = Slice(__opcode, 5, 19);
    let immlo = Slice(__opcode, 29, 2);
    let op = Slice(__opcode, 31, 1);
    decode_adrp_aarch64_instrs_integer_arithmetic_address_pc_rel(Rd, immhi, immlo, op)
}

val execute_aarch64_instrs_integer_logical_immediate : forall 'd 'datasize 'n ('setflags : Bool),
  (0 <= 'n & 'n <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), bits('datasize), int('n), LogicalOp, bool('setflags)) -> unit

function execute_aarch64_instrs_integer_logical_immediate (d, datasize, imm, n, op, setflags) = {
    result : bits('datasize) = undefined;
    let operand1 : bits('datasize) = X_read(n, datasize);
    let operand2 : bits('datasize) = imm;
    match op {
      LogicalOp_AND => {
          result = operand1 & operand2
      },
      LogicalOp_ORR => {
          result = operand1 | operand2
      },
      LogicalOp_EOR => {
          result = EOR(operand1, operand2)
      }
    };
    let result = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = ([result[datasize - 1]] @ IsZeroBit(result)) @ 0b00
    };
    if d == 31 & not_bool(setflags) then {
        SP_set() = ZeroExtend(result, 64)
    } else {
        X_set(d, datasize) = result
    }
}

val decode_and_log_imm_aarch64_instrs_integer_logical_immediate : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_and_log_imm_aarch64_instrs_integer_logical_immediate (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    imm : bits('datasize) = undefined;
    if sf == 0b0 & N != 0b0 then {
        throw(Error_Undefined())
    };
    let (__tup_0 : bits('datasize), _) =
      DecodeBitMasks(N, imms, immr, true, datasize)
    in
      {
          imm = __tup_0
      };
    execute_aarch64_instrs_integer_logical_immediate(d, datasize, imm, n, op, setflags)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 40) = {
    SEE = 40;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_and_log_imm_aarch64_instrs_integer_logical_immediate(Rd, Rn, imms, immr, N, opc, sf)
}

val decode_ands_log_imm_aarch64_instrs_integer_logical_immediate : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_ands_log_imm_aarch64_instrs_integer_logical_immediate (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    imm : bits('datasize) = undefined;
    if sf == 0b0 & N != 0b0 then {
        throw(Error_Undefined())
    };
    let (__tup_0 : bits('datasize), _) =
      DecodeBitMasks(N, imms, immr, true, datasize)
    in
      {
          imm = __tup_0
      };
    execute_aarch64_instrs_integer_logical_immediate(d, datasize, imm, n, op, setflags)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 41) = {
    SEE = 41;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_ands_log_imm_aarch64_instrs_integer_logical_immediate(Rd, Rn, imms, immr, N, opc, sf)
}

val decode_eor_log_imm_aarch64_instrs_integer_logical_immediate : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_eor_log_imm_aarch64_instrs_integer_logical_immediate (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    imm : bits('datasize) = undefined;
    if sf == 0b0 & N != 0b0 then {
        throw(Error_Undefined())
    };
    let (__tup_0 : bits('datasize), _) =
      DecodeBitMasks(N, imms, immr, true, datasize)
    in
      {
          imm = __tup_0
      };
    execute_aarch64_instrs_integer_logical_immediate(d, datasize, imm, n, op, setflags)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 42) = {
    SEE = 42;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_eor_log_imm_aarch64_instrs_integer_logical_immediate(Rd, Rn, imms, immr, N, opc, sf)
}

val decode_orr_log_imm_aarch64_instrs_integer_logical_immediate : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_orr_log_imm_aarch64_instrs_integer_logical_immediate (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    imm : bits('datasize) = undefined;
    if sf == 0b0 & N != 0b0 then {
        throw(Error_Undefined())
    };
    let (__tup_0 : bits('datasize), _) =
      DecodeBitMasks(N, imms, immr, true, datasize)
    in
      {
          imm = __tup_0
      };
    execute_aarch64_instrs_integer_logical_immediate(d, datasize, imm, n, op, setflags)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 43) = {
    SEE = 43;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_orr_log_imm_aarch64_instrs_integer_logical_immediate(Rd, Rn, imms, immr, N, opc, sf)
}

val execute_aarch64_instrs_integer_logical_shiftedreg : forall 'd 'datasize ('invert : Bool) 'm 'n ('setflags : Bool) 'shift_amount,
  (0 <= 'shift_amount & 'shift_amount <= 63 & 0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), bool('invert), int('m), int('n), LogicalOp, bool('setflags), int('shift_amount), ShiftType) -> unit

function execute_aarch64_instrs_integer_logical_shiftedreg (d, datasize, invert, m, n, op, setflags, shift_amount, shift_type) = {
    let operand1 : bits('datasize) = X_read(n, datasize);
    operand2 : bits('datasize) = ShiftReg(m, shift_type, shift_amount, datasize);
    result : bits('datasize) = undefined;
    if invert then {
        operand2 = not_vec(operand2)
    };
    match op {
      LogicalOp_AND => {
          result = operand1 & operand2
      },
      LogicalOp_ORR => {
          result = operand1 | operand2
      },
      LogicalOp_EOR => {
          result = EOR(operand1, operand2)
      }
    };
    let result = result;
    if setflags then {
        (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = ([result[datasize - 1]] @ IsZeroBit(result)) @ 0b00
    };
    X_set(d, datasize) = result
}

val decode_and_log_shift_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_and_log_shift_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 48) = {
    SEE = 48;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_and_log_shift_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_ands_log_shift_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_ands_log_shift_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 49) = {
    SEE = 49;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_ands_log_shift_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_bic_log_shift_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_bic_log_shift_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 50) = {
    SEE = 50;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_bic_log_shift_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_bics_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_bics_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 51) = {
    SEE = 51;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_bics_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_eon_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_eon_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 52) = {
    SEE = 52;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_eon_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_eor_log_shift_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_eor_log_shift_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 53) = {
    SEE = 53;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_eor_log_shift_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_orn_log_shift_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_orn_log_shift_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 54) = {
    SEE = 54;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_orn_log_shift_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val decode_orr_log_shift_aarch64_instrs_integer_logical_shiftedreg : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(2), bits(2), bits(1)) -> unit

function decode_orr_log_shift_aarch64_instrs_integer_logical_shiftedreg (Rd, Rn, imm6, Rm, N, shift, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    setflags : bool = undefined;
    op : LogicalOp = undefined;
    match opc {
      0b00 => {
          op = LogicalOp_AND;
          setflags = false
      },
      0b01 => {
          op = LogicalOp_ORR;
          setflags = false
      },
      0b10 => {
          op = LogicalOp_EOR;
          setflags = false
      },
      0b11 => {
          op = LogicalOp_AND;
          setflags = true
      }
    };
    let op = op;
    let setflags = setflags;
    if sf == 0b0 & [imm6[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let shift_type : ShiftType = DecodeShift(shift);
    let 'shift_amount = UInt(imm6);
    let invert : bool = N == 0b1;
    execute_aarch64_instrs_integer_logical_shiftedreg(d, datasize, invert, m, n, op, setflags, shift_amount, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 55) = {
    SEE = 55;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 21, 1);
    let shift = Slice(__opcode, 22, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_orr_log_shift_aarch64_instrs_integer_logical_shiftedreg(Rd, Rn, imm6, Rm, N, shift, opc, sf)
}

val execute_aarch64_instrs_integer_shift_variable : forall 'd 'datasize 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('m), int('n), ShiftType) -> unit

function execute_aarch64_instrs_integer_shift_variable (d, datasize, m, n, shift_type) = {
    let operand2 : bits('datasize) = X_read(m, datasize);
    let result : bits('datasize) = ShiftReg(n, shift_type, MOD(UInt(operand2), datasize), datasize);
    X_set(d, datasize) = result
}

val decode_asrv_aarch64_instrs_integer_shift_variable : (bits(5), bits(5), bits(2), bits(5), bits(1)) -> unit

function decode_asrv_aarch64_instrs_integer_shift_variable (Rd, Rn, op2, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let shift_type : ShiftType = DecodeShift(op2);
    execute_aarch64_instrs_integer_shift_variable(d, datasize, m, n, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 56) = {
    SEE = 56;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op2 = Slice(__opcode, 10, 2);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_asrv_aarch64_instrs_integer_shift_variable(Rd, Rn, op2, Rm, sf)
}

val decode_lslv_aarch64_instrs_integer_shift_variable : (bits(5), bits(5), bits(2), bits(5), bits(1)) -> unit

function decode_lslv_aarch64_instrs_integer_shift_variable (Rd, Rn, op2, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let shift_type : ShiftType = DecodeShift(op2);
    execute_aarch64_instrs_integer_shift_variable(d, datasize, m, n, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 57) = {
    SEE = 57;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op2 = Slice(__opcode, 10, 2);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_lslv_aarch64_instrs_integer_shift_variable(Rd, Rn, op2, Rm, sf)
}

val decode_lsrv_aarch64_instrs_integer_shift_variable : (bits(5), bits(5), bits(2), bits(5), bits(1)) -> unit

function decode_lsrv_aarch64_instrs_integer_shift_variable (Rd, Rn, op2, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let shift_type : ShiftType = DecodeShift(op2);
    execute_aarch64_instrs_integer_shift_variable(d, datasize, m, n, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 58) = {
    SEE = 58;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op2 = Slice(__opcode, 10, 2);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_lsrv_aarch64_instrs_integer_shift_variable(Rd, Rn, op2, Rm, sf)
}

val decode_rorv_aarch64_instrs_integer_shift_variable : (bits(5), bits(5), bits(2), bits(5), bits(1)) -> unit

function decode_rorv_aarch64_instrs_integer_shift_variable (Rd, Rn, op2, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let shift_type : ShiftType = DecodeShift(op2);
    execute_aarch64_instrs_integer_shift_variable(d, datasize, m, n, shift_type)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 59) = {
    SEE = 59;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op2 = Slice(__opcode, 10, 2);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_rorv_aarch64_instrs_integer_shift_variable(Rd, Rn, op2, Rm, sf)
}

val execute_aarch64_instrs_integer_pac_autda_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_autda_dp_1src (d, n, source_is_sp) = {
    let auth_then_branch : bool = false;
    if HavePACExt() then {
        if source_is_sp then {
            X_set(d, 64) = AuthDA(X_read(d, 64), SP_read(), auth_then_branch)
        } else {
            X_set(d, 64) = AuthDA(X_read(d, 64), X_read(n, 64), auth_then_branch)
        }
    };
    ()
}

val decode_autda_aarch64_instrs_integer_pac_autda_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_autda_aarch64_instrs_integer_pac_autda_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_autda_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 60) = {
    SEE = 60;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_autda_aarch64_instrs_integer_pac_autda_dp_1src(Rd, Rn, Z)
}

val execute_aarch64_instrs_integer_pac_autdb_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_autdb_dp_1src (d, n, source_is_sp) = {
    let auth_then_branch : bool = false;
    if HavePACExt() then {
        if source_is_sp then {
            X_set(d, 64) = AuthDB(X_read(d, 64), SP_read(), auth_then_branch)
        } else {
            X_set(d, 64) = AuthDB(X_read(d, 64), X_read(n, 64), auth_then_branch)
        }
    };
    ()
}

val decode_autdb_aarch64_instrs_integer_pac_autdb_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_autdb_aarch64_instrs_integer_pac_autdb_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_autdb_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 61) = {
    SEE = 61;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_autdb_aarch64_instrs_integer_pac_autdb_dp_1src(Rd, Rn, Z)
}

val execute_aarch64_instrs_integer_pac_autia_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_autia_dp_1src (d, n, source_is_sp) = {
    let auth_then_branch : bool = false;
    if HavePACExt() then {
        if source_is_sp then {
            X_set(d, 64) = AuthIA(X_read(d, 64), SP_read(), auth_then_branch)
        } else {
            X_set(d, 64) = AuthIA(X_read(d, 64), X_read(n, 64), auth_then_branch)
        }
    };
    ()
}

val decode_autia_aarch64_instrs_integer_pac_autia_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_autia_aarch64_instrs_integer_pac_autia_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_autia_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 62) = {
    SEE = 62;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_autia_aarch64_instrs_integer_pac_autia_dp_1src(Rd, Rn, Z)
}

val decode_autia_aarch64_instrs_integer_pac_autia_hint : (bits(3), bits(4)) -> unit

function decode_autia_aarch64_instrs_integer_pac_autia_hint (op2, CRm) = {
    d : {17, 30} = 17;
    n : int = undefined;
    source_is_sp : bool = false;
    match CRm @ op2 {
      0b0011100 => {
          d = 30;
          n = 31
      },
      0b0011101 => {
          d = 30;
          source_is_sp = true
      },
      0b0001100 => {
          d = 17;
          n = 16
      },
      0b0001000 => {
          throw(Error_See("PACIA"))
      },
      0b0001010 => {
          throw(Error_See("PACIB"))
      },
      0b0001110 => {
          throw(Error_See("AUTIB"))
      },
      [bitzero, bitzero, bitone, bitone, bitzero, bitzero, _] => {
          throw(Error_See("PACIA"))
      },
      [bitzero, bitzero, bitone, bitone, bitzero, bitone, _] => {
          throw(Error_See("PACIB"))
      },
      [bitzero, bitzero, bitone, bitone, bitone, bitone, _] => {
          throw(Error_See("AUTIB"))
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      _ => {
          throw(Error_See("HINT"))
      }
    };
    let 'n = n;
    let 'd = d;
    assert(constraint((0 <= 'n & 'n <= 31)));
    execute_aarch64_instrs_integer_pac_autia_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, bitone, bitone, bitzero, _, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 63) = {
    SEE = 63;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_autia_aarch64_instrs_integer_pac_autia_hint(op2, CRm)
}

val execute_aarch64_instrs_integer_pac_autib_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_autib_dp_1src (d, n, source_is_sp) = {
    let auth_then_branch : bool = false;
    if HavePACExt() then {
        if source_is_sp then {
            X_set(d, 64) = AuthIB(X_read(d, 64), SP_read(), auth_then_branch)
        } else {
            X_set(d, 64) = AuthIB(X_read(d, 64), X_read(n, 64), auth_then_branch)
        }
    };
    ()
}

val decode_autib_aarch64_instrs_integer_pac_autib_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_autib_aarch64_instrs_integer_pac_autib_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_autib_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 64) = {
    SEE = 64;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_autib_aarch64_instrs_integer_pac_autib_dp_1src(Rd, Rn, Z)
}

val decode_autib_aarch64_instrs_integer_pac_autib_hint : (bits(3), bits(4)) -> unit

function decode_autib_aarch64_instrs_integer_pac_autib_hint (op2, CRm) = {
    d : {17, 30} = 17;
    n : int = undefined;
    source_is_sp : bool = false;
    match CRm @ op2 {
      0b0011110 => {
          d = 30;
          n = 31
      },
      0b0011111 => {
          d = 30;
          source_is_sp = true
      },
      0b0001110 => {
          d = 17;
          n = 16
      },
      0b0001000 => {
          throw(Error_See("PACIA"))
      },
      0b0001010 => {
          throw(Error_See("PACIB"))
      },
      0b0001100 => {
          throw(Error_See("AUTIA"))
      },
      [bitzero, bitzero, bitone, bitone, bitzero, bitzero, _] => {
          throw(Error_See("PACIA"))
      },
      [bitzero, bitzero, bitone, bitone, bitzero, bitone, _] => {
          throw(Error_See("PACIB"))
      },
      [bitzero, bitzero, bitone, bitone, bitone, bitzero, _] => {
          throw(Error_See("AUTIA"))
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      _ => {
          throw(Error_See("HINT"))
      }
    };
    let 'n = n;
    let 'd = d;
    assert(constraint((0 <= 'n & 'n <= 31)));
    execute_aarch64_instrs_integer_pac_autib_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, bitone, bitone, bitone, _, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 65) = {
    SEE = 65;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_autib_aarch64_instrs_integer_pac_autib_hint(op2, CRm)
}

val execute_aarch64_instrs_branch_conditional_cond : (bits(4), bits(64)) -> unit

function execute_aarch64_instrs_branch_conditional_cond (condition, offset) = {
    let branch_conditional : bool = true;
    if ConditionHolds(condition) then {
        BranchTo(PC_read() + offset, BranchType_DIR, branch_conditional)
    } else {
        BranchNotTaken(BranchType_DIR, branch_conditional)
    }
}

val decode_b_cond_aarch64_instrs_branch_conditional_cond : (bits(4), bits(19)) -> unit

function decode_b_cond_aarch64_instrs_branch_conditional_cond (cond, imm19) = {
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    let condition : bits(4) = cond;
    execute_aarch64_instrs_branch_conditional_cond(condition, offset)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 69) = {
    SEE = 69;
    let cond = Slice(__opcode, 0, 4);
    let imm19 = Slice(__opcode, 5, 19);
    decode_b_cond_aarch64_instrs_branch_conditional_cond(cond, imm19)
}

val execute_aarch64_instrs_branch_conditional_hinted : (bits(4), bits(64)) -> unit

function execute_aarch64_instrs_branch_conditional_hinted (condition, offset) = {
    let branch_conditional : bool = true;
    if ConditionHolds(condition) then {
        BranchTo(PC_read() + offset, BranchType_DIR, branch_conditional)
    } else {
        BranchNotTaken(BranchType_DIR, branch_conditional)
    }
}

val decode_bc_cond_aarch64_instrs_branch_conditional_hinted : (bits(4), bits(19)) -> unit

function decode_bc_cond_aarch64_instrs_branch_conditional_hinted (cond, imm19) = {
    if not_bool(HaveFeatHBC()) then {
        throw(Error_Undefined())
    };
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    let condition : bits(4) = cond;
    execute_aarch64_instrs_branch_conditional_hinted(condition, offset)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, bitone, _, _, _, _] as __opcode)) if SEE < 70) = {
    SEE = 70;
    let cond = Slice(__opcode, 0, 4);
    let imm19 = Slice(__opcode, 5, 19);
    decode_bc_cond_aarch64_instrs_branch_conditional_hinted(cond, imm19)
}

val execute_aarch64_instrs_integer_bitfield : forall 'd 'datasize ('extend : Bool) ('inzero : Bool) 'n 'r 's,
  (0 <= 's & 's <= 63 & 0 <= 'r & 'r <= 63 & 0 <= 'n & 'n <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), bool('extend), bool('inzero), int('n), int('r), int('s), bits('datasize), bits('datasize)) -> unit

function execute_aarch64_instrs_integer_bitfield (d, datasize, extend, inzero, n, r, s, tmask, wmask) = {
    let dst : bits('datasize) = if inzero then Zeros(datasize) else
      X_read(d, datasize);
    let src : bits('datasize) = X_read(n, datasize);
    let bot : bits('datasize) = dst & not_vec(wmask) | ROR(src, r) & wmask;
    let top : bits('datasize) = if extend then {
        assert(constraint((0 <= 's & 's < 'datasize)));
        Replicate([src[s]], datasize)
    } else dst;
    X_set(d, datasize) = top & not_vec(tmask) | bot & tmask
}

val decode_bfm_aarch64_instrs_integer_bitfield : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_bfm_aarch64_instrs_integer_bitfield (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    inzero : bool = undefined;
    extend : bool = undefined;
    wmask : bits('datasize) = undefined;
    tmask : bits('datasize) = undefined;
    match opc {
      0b00 => {
          inzero = true;
          extend = true
      },
      0b01 => {
          inzero = false;
          extend = false
      },
      0b10 => {
          inzero = true;
          extend = false
      },
      0b11 => {
          throw(Error_Undefined())
      }
    };
    let extend = extend;
    let inzero = inzero;
    if sf == 0b1 & N != 0b1 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & ((N != 0b0 | [immr[5]] != 0b0) | [imms[5]] != 0b0) then {
        throw(Error_Undefined())
    };
    let 'r = UInt(immr);
    let 's = UInt(imms);
    (wmask, tmask) = DecodeBitMasks(N, imms, immr, false, datasize);
    execute_aarch64_instrs_integer_bitfield(d, datasize, extend, inzero, n, r, s, tmask, wmask)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 76) = {
    SEE = 76;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_bfm_aarch64_instrs_integer_bitfield(Rd, Rn, imms, immr, N, opc, sf)
}

val decode_sbfm_aarch64_instrs_integer_bitfield : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_sbfm_aarch64_instrs_integer_bitfield (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    inzero : bool = undefined;
    extend : bool = undefined;
    wmask : bits('datasize) = undefined;
    tmask : bits('datasize) = undefined;
    match opc {
      0b00 => {
          inzero = true;
          extend = true
      },
      0b01 => {
          inzero = false;
          extend = false
      },
      0b10 => {
          inzero = true;
          extend = false
      },
      0b11 => {
          throw(Error_Undefined())
      }
    };
    let extend = extend;
    let inzero = inzero;
    if sf == 0b1 & N != 0b1 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & ((N != 0b0 | [immr[5]] != 0b0) | [imms[5]] != 0b0) then {
        throw(Error_Undefined())
    };
    let 'r = UInt(immr);
    let 's = UInt(imms);
    (wmask, tmask) = DecodeBitMasks(N, imms, immr, false, datasize);
    execute_aarch64_instrs_integer_bitfield(d, datasize, extend, inzero, n, r, s, tmask, wmask)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 77) = {
    SEE = 77;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_sbfm_aarch64_instrs_integer_bitfield(Rd, Rn, imms, immr, N, opc, sf)
}

val decode_ubfm_aarch64_instrs_integer_bitfield : (bits(5), bits(5), bits(6), bits(6), bits(1), bits(2), bits(1)) -> unit

function decode_ubfm_aarch64_instrs_integer_bitfield (Rd, Rn, imms, immr, N, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    inzero : bool = undefined;
    extend : bool = undefined;
    wmask : bits('datasize) = undefined;
    tmask : bits('datasize) = undefined;
    match opc {
      0b00 => {
          inzero = true;
          extend = true
      },
      0b01 => {
          inzero = false;
          extend = false
      },
      0b10 => {
          inzero = true;
          extend = false
      },
      0b11 => {
          throw(Error_Undefined())
      }
    };
    let extend = extend;
    let inzero = inzero;
    if sf == 0b1 & N != 0b1 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & ((N != 0b0 | [immr[5]] != 0b0) | [imms[5]] != 0b0) then {
        throw(Error_Undefined())
    };
    let 'r = UInt(immr);
    let 's = UInt(imms);
    (wmask, tmask) = DecodeBitMasks(N, imms, immr, false, datasize);
    execute_aarch64_instrs_integer_bitfield(d, datasize, extend, inzero, n, r, s, tmask, wmask)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 78) = {
    SEE = 78;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let immr = Slice(__opcode, 16, 6);
    let N = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_ubfm_aarch64_instrs_integer_bitfield(Rd, Rn, imms, immr, N, opc, sf)
}

val execute_aarch64_instrs_branch_unconditional_register : forall 'm 'n ('pac : Bool) ('source_is_sp : Bool) ('use_key_a : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31).
  (BranchType, int('m), int('n), bool('pac), bool('source_is_sp), bool('use_key_a)) -> unit

function execute_aarch64_instrs_branch_unconditional_register (branch_type, m, n, pac, source_is_sp, use_key_a) = {
    inst_type : GCSInstruction = undefined;
    target : bits(64) = X_read(n, 64);
    let auth_then_branch : bool = true;
    if pac then {
        let modifier : bits(64) = if source_is_sp then SP_read() else
          X_read(m, 64);
        if use_key_a then {
            target = AuthIA(target, modifier, auth_then_branch)
        } else {
            target = AuthIB(target, modifier, auth_then_branch)
        }
    };
    if (branch_type == BranchType_RET & HaveGCS()) & GCSPCREnabled(PSTATE.EL) then {
        if not_bool(pac) then {
            inst_type = GCSInstType_PRET
        } else {
            if use_key_a then {
                inst_type = GCSInstType_PRETAA
            } else {
                inst_type = GCSInstType_PRETAB
            }
        };
        target = LoadCheckGCSRecord(target, inst_type);
        SetCurrentGCSPointer(GetCurrentGCSPointer() + 8)
    };
    if branch_type == BranchType_INDCALL then {
        if HaveGCS() & GCSPCREnabled(PSTATE.EL) then {
            AddGCSRecord(PC_read() + 4)
        };
        X_set(30, 64) = PC_read() + 4
    };
    match branch_type {
      BranchType_INDIR => {
          if InGuardedPage then {
              if n == 16 | n == 17 then {
                  BTypeNext = 0b01
              } else {
                  BTypeNext = 0b11
              }
          } else {
              BTypeNext = 0b01
          }
      },
      BranchType_INDCALL => {
          BTypeNext = 0b10
      },
      BranchType_RET => {
          BTypeNext = 0b00
      },
      _ => ()
    };
    let branch_conditional : bool = false;
    BranchTo(target, branch_type, branch_conditional)
}

val decode_blr_aarch64_instrs_branch_unconditional_register : (bits(5), bits(5), bits(1), bits(1), bits(2), bits(1)) -> unit

function decode_blr_aarch64_instrs_branch_unconditional_register (Rm, Rn, M, A, op, Z) = {
    n : range(0, 31) = UInt(Rn);
    branch_type : BranchType = undefined;
    let 'm = UInt(Rm);
    let pac : bool = A == 0b1;
    let use_key_a : bool = M == 0b0;
    source_is_sp : bool = Z == 0b1 & m == 31;
    if not_bool(pac) & m != 0 then {
        throw(Error_Undefined())
    } else if pac & not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    } else ();
    match op {
      0b00 => {
          branch_type = BranchType_INDIR
      },
      0b01 => {
          branch_type = BranchType_INDCALL
      },
      0b10 => {
          branch_type = BranchType_RET
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let branch_type = branch_type;
    if pac then {
        if Z == 0b0 & m != 31 then {
            throw(Error_Undefined())
        };
        if branch_type == BranchType_RET then {
            if n != 31 then {
                throw(Error_Undefined())
            };
            n = 30;
            source_is_sp = true
        };
        ()
    };
    let 'n = n;
    execute_aarch64_instrs_branch_unconditional_register(branch_type, m, n, pac, source_is_sp, use_key_a)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero] as __opcode)) if SEE < 90) = {
    SEE = 90;
    let Rm = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let M = Slice(__opcode, 10, 1);
    let A = Slice(__opcode, 11, 1);
    let op = Slice(__opcode, 21, 2);
    let Z = Slice(__opcode, 24, 1);
    decode_blr_aarch64_instrs_branch_unconditional_register(Rm, Rn, M, A, op, Z)
}

val decode_blra_aarch64_instrs_branch_unconditional_register : (bits(5), bits(5), bits(1), bits(1), bits(2), bits(1)) -> unit

function decode_blra_aarch64_instrs_branch_unconditional_register (Rm, Rn, M, A, op, Z) = {
    n : range(0, 31) = UInt(Rn);
    branch_type : BranchType = undefined;
    let 'm = UInt(Rm);
    let pac : bool = A == 0b1;
    let use_key_a : bool = M == 0b0;
    source_is_sp : bool = Z == 0b1 & m == 31;
    if not_bool(pac) & m != 0 then {
        throw(Error_Undefined())
    } else if pac & not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    } else ();
    match op {
      0b00 => {
          branch_type = BranchType_INDIR
      },
      0b01 => {
          branch_type = BranchType_INDCALL
      },
      0b10 => {
          branch_type = BranchType_RET
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let branch_type = branch_type;
    if pac then {
        if Z == 0b0 & m != 31 then {
            throw(Error_Undefined())
        };
        if branch_type == BranchType_RET then {
            if n != 31 then {
                throw(Error_Undefined())
            };
            n = 30;
            source_is_sp = true
        };
        ()
    };
    let 'n = n;
    execute_aarch64_instrs_branch_unconditional_register(branch_type, m, n, pac, source_is_sp, use_key_a)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, _, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 91) = {
    SEE = 91;
    let Rm = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let M = Slice(__opcode, 10, 1);
    let A = Slice(__opcode, 11, 1);
    let op = Slice(__opcode, 21, 2);
    let Z = Slice(__opcode, 24, 1);
    decode_blra_aarch64_instrs_branch_unconditional_register(Rm, Rn, M, A, op, Z)
}

val decode_br_aarch64_instrs_branch_unconditional_register : (bits(5), bits(5), bits(1), bits(1), bits(2), bits(1)) -> unit

function decode_br_aarch64_instrs_branch_unconditional_register (Rm, Rn, M, A, op, Z) = {
    n : range(0, 31) = UInt(Rn);
    branch_type : BranchType = undefined;
    let 'm = UInt(Rm);
    let pac : bool = A == 0b1;
    let use_key_a : bool = M == 0b0;
    source_is_sp : bool = Z == 0b1 & m == 31;
    if not_bool(pac) & m != 0 then {
        throw(Error_Undefined())
    } else if pac & not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    } else ();
    match op {
      0b00 => {
          branch_type = BranchType_INDIR
      },
      0b01 => {
          branch_type = BranchType_INDCALL
      },
      0b10 => {
          branch_type = BranchType_RET
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let branch_type = branch_type;
    if pac then {
        if Z == 0b0 & m != 31 then {
            throw(Error_Undefined())
        };
        if branch_type == BranchType_RET then {
            if n != 31 then {
                throw(Error_Undefined())
            };
            n = 30;
            source_is_sp = true
        };
        ()
    };
    let 'n = n;
    execute_aarch64_instrs_branch_unconditional_register(branch_type, m, n, pac, source_is_sp, use_key_a)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero] as __opcode)) if SEE < 92) = {
    SEE = 92;
    let Rm = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let M = Slice(__opcode, 10, 1);
    let A = Slice(__opcode, 11, 1);
    let op = Slice(__opcode, 21, 2);
    let Z = Slice(__opcode, 24, 1);
    decode_br_aarch64_instrs_branch_unconditional_register(Rm, Rn, M, A, op, Z)
}

val decode_bra_aarch64_instrs_branch_unconditional_register : (bits(5), bits(5), bits(1), bits(1), bits(2), bits(1)) -> unit

function decode_bra_aarch64_instrs_branch_unconditional_register (Rm, Rn, M, A, op, Z) = {
    n : range(0, 31) = UInt(Rn);
    branch_type : BranchType = undefined;
    let 'm = UInt(Rm);
    let pac : bool = A == 0b1;
    let use_key_a : bool = M == 0b0;
    source_is_sp : bool = Z == 0b1 & m == 31;
    if not_bool(pac) & m != 0 then {
        throw(Error_Undefined())
    } else if pac & not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    } else ();
    match op {
      0b00 => {
          branch_type = BranchType_INDIR
      },
      0b01 => {
          branch_type = BranchType_INDCALL
      },
      0b10 => {
          branch_type = BranchType_RET
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let branch_type = branch_type;
    if pac then {
        if Z == 0b0 & m != 31 then {
            throw(Error_Undefined())
        };
        if branch_type == BranchType_RET then {
            if n != 31 then {
                throw(Error_Undefined())
            };
            n = 30;
            source_is_sp = true
        };
        ()
    };
    let 'n = n;
    execute_aarch64_instrs_branch_unconditional_register(branch_type, m, n, pac, source_is_sp, use_key_a)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, _, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 93) = {
    SEE = 93;
    let Rm = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let M = Slice(__opcode, 10, 1);
    let A = Slice(__opcode, 11, 1);
    let op = Slice(__opcode, 21, 2);
    let Z = Slice(__opcode, 24, 1);
    decode_bra_aarch64_instrs_branch_unconditional_register(Rm, Rn, M, A, op, Z)
}

val decode_ret_aarch64_instrs_branch_unconditional_register : (bits(5), bits(5), bits(1), bits(1), bits(2), bits(1)) -> unit

function decode_ret_aarch64_instrs_branch_unconditional_register (Rm, Rn, M, A, op, Z) = {
    n : range(0, 31) = UInt(Rn);
    branch_type : BranchType = undefined;
    let 'm = UInt(Rm);
    let pac : bool = A == 0b1;
    let use_key_a : bool = M == 0b0;
    source_is_sp : bool = Z == 0b1 & m == 31;
    if not_bool(pac) & m != 0 then {
        throw(Error_Undefined())
    } else if pac & not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    } else ();
    match op {
      0b00 => {
          branch_type = BranchType_INDIR
      },
      0b01 => {
          branch_type = BranchType_INDCALL
      },
      0b10 => {
          branch_type = BranchType_RET
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let branch_type = branch_type;
    if pac then {
        if Z == 0b0 & m != 31 then {
            throw(Error_Undefined())
        };
        if branch_type == BranchType_RET then {
            if n != 31 then {
                throw(Error_Undefined())
            };
            n = 30;
            source_is_sp = true
        };
        ()
    };
    let 'n = n;
    execute_aarch64_instrs_branch_unconditional_register(branch_type, m, n, pac, source_is_sp, use_key_a)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero] as __opcode)) if SEE < 94) = {
    SEE = 94;
    let Rm = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let M = Slice(__opcode, 10, 1);
    let A = Slice(__opcode, 11, 1);
    let op = Slice(__opcode, 21, 2);
    let Z = Slice(__opcode, 24, 1);
    decode_ret_aarch64_instrs_branch_unconditional_register(Rm, Rn, M, A, op, Z)
}

val decode_reta_aarch64_instrs_branch_unconditional_register : (bits(5), bits(5), bits(1), bits(1), bits(2), bits(1)) -> unit

function decode_reta_aarch64_instrs_branch_unconditional_register (Rm, Rn, M, A, op, Z) = {
    n : range(0, 31) = UInt(Rn);
    branch_type : BranchType = undefined;
    let 'm = UInt(Rm);
    let pac : bool = A == 0b1;
    let use_key_a : bool = M == 0b0;
    source_is_sp : bool = Z == 0b1 & m == 31;
    if not_bool(pac) & m != 0 then {
        throw(Error_Undefined())
    } else if pac & not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    } else ();
    match op {
      0b00 => {
          branch_type = BranchType_INDIR
      },
      0b01 => {
          branch_type = BranchType_INDCALL
      },
      0b10 => {
          branch_type = BranchType_RET
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let branch_type = branch_type;
    if pac then {
        if Z == 0b0 & m != 31 then {
            throw(Error_Undefined())
        };
        if branch_type == BranchType_RET then {
            if n != 31 then {
                throw(Error_Undefined())
            };
            n = 30;
            source_is_sp = true
        };
        ()
    };
    let 'n = n;
    execute_aarch64_instrs_branch_unconditional_register(branch_type, m, n, pac, source_is_sp, use_key_a)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 95) = {
    SEE = 95;
    let Rm = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let M = Slice(__opcode, 10, 1);
    let A = Slice(__opcode, 11, 1);
    let op = Slice(__opcode, 21, 2);
    let Z = Slice(__opcode, 24, 1);
    decode_reta_aarch64_instrs_branch_unconditional_register(Rm, Rn, M, A, op, Z)
}

val execute_aarch64_instrs_system_hints : SystemHintOp -> unit

function execute_aarch64_instrs_system_hints op = {
    match op {
      SystemHintOp_YIELD => {
          Hint_Yield()
      },
      SystemHintOp_DGH => {
          Hint_DGH()
      },
      SystemHintOp_WFE => {
          let 'localtimeout = (1 << 64);
          Hint_WFE(localtimeout, WFxType_WFE)
      },
      SystemHintOp_WFI => {
          let 'localtimeout = (1 << 64);
          Hint_WFI(localtimeout, WFxType_WFI)
      },
      SystemHintOp_SEV => {
          SendEvent()
      },
      SystemHintOp_SEVL => {
          SendEventLocal()
      },
      SystemHintOp_ESB => {
          if HaveTME() & TSTATE.depth > 0 then {
              FailTransaction(TMFailure_ERR, false)
          };
          SynchronizeErrors();
          AArch64_ESBOperation();
          if (PSTATE.EL == EL0 | PSTATE.EL == EL1) & EL2Enabled() then {
              AArch64_vESBOperation()
          };
          TakeUnmaskedSErrorInterrupts()
      },
      SystemHintOp_PSB => {
          ProfilingSynchronizationBarrier()
      },
      SystemHintOp_TSB => {
          TraceSynchronizationBarrier()
      },
      SystemHintOp_GCSB => {
          GCSSynchronizationBarrier()
      },
      SystemHintOp_CHKFEAT => {
          X_set(16, 64) = AArch64_ChkFeat(X_read(16, 64))
      },
      SystemHintOp_CSDB => {
          ConsumptionOfSpeculativeDataBarrier()
      },
      SystemHintOp_CLRBHB => {
          Hint_CLRBHB()
      },
      SystemHintOp_BTI => {
          SetBTypeNext(0b00)
      },
      SystemHintOp_NOP => {
          return()
      },
      _ => {
          Unreachable()
      }
    }
}

val decode_bti_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_bti_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, bitzero, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 97) = {
    SEE = 97;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_bti_aarch64_instrs_system_hints(op2, CRm)
}

val decode_chkfeat_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_chkfeat_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010010100011111 as __opcode)) if SEE < 98) = {
    SEE = 98;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_chkfeat_aarch64_instrs_system_hints(op2, CRm)
}

val decode_clrbhb_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_clrbhb_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010001011011111 as __opcode)) if SEE < 99) = {
    SEE = 99;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_clrbhb_aarch64_instrs_system_hints(op2, CRm)
}

val decode_csdb_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_csdb_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010001010011111 as __opcode)) if SEE < 100) = {
    SEE = 100;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_csdb_aarch64_instrs_system_hints(op2, CRm)
}

val decode_dgh_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_dgh_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010000011011111 as __opcode)) if SEE < 101) = {
    SEE = 101;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_dgh_aarch64_instrs_system_hints(op2, CRm)
}

val decode_esb_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_esb_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010001000011111 as __opcode)) if SEE < 102) = {
    SEE = 102;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_esb_aarch64_instrs_system_hints(op2, CRm)
}

val decode_gcsb_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_gcsb_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010001001111111 as __opcode)) if SEE < 103) = {
    SEE = 103;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_gcsb_aarch64_instrs_system_hints(op2, CRm)
}

val decode_hint_aarch64_instrs_system_hints : (bits(3), bits(4)) -> unit

function decode_hint_aarch64_instrs_system_hints (op2, CRm) = {
    op : SystemHintOp = undefined;
    match CRm @ op2 {
      0b0000000 => {
          op = SystemHintOp_NOP
      },
      0b0000001 => {
          op = SystemHintOp_YIELD
      },
      0b0000010 => {
          op = SystemHintOp_WFE
      },
      0b0000011 => {
          op = SystemHintOp_WFI
      },
      0b0000100 => {
          op = SystemHintOp_SEV
      },
      0b0000101 => {
          op = SystemHintOp_SEVL
      },
      0b0000110 => {
          if not_bool(HaveDGHExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_DGH
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      [bitzero, bitzero, bitzero, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIA1716"))
            },
            0b010 => {
                throw(Error_See("PACIB1716"))
            },
            0b100 => {
                throw(Error_See("AUTIA1716"))
            },
            0b110 => {
                throw(Error_See("AUTIB1716"))
            },
            _ => {
                EndOfInstruction()
            }
          }
      },
      0b0010000 => {
          if not_bool(HaveRASExt()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_ESB
      },
      0b0010001 => {
          if not_bool(HaveStatisticalProfiling()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_PSB
      },
      0b0010010 => {
          if not_bool(HaveSelfHostedTrace()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_TSB
      },
      0b0010011 => {
          if not_bool(IsFeatureImplemented(FEAT_GCS)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_GCSB
      },
      0b0010100 => {
          op = SystemHintOp_CSDB
      },
      0b0010110 => {
          if not_bool(HaveFeatCLRBHB()) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CLRBHB
      },
      [bitzero, bitzero, bitone, bitone, _, _, _] => {
          match op2 {
            0b000 => {
                throw(Error_See("PACIAZ"))
            },
            0b001 => {
                throw(Error_See("PACIASP"))
            },
            0b010 => {
                throw(Error_See("PACIBZ"))
            },
            0b011 => {
                throw(Error_See("PACIBSP"))
            },
            0b100 => {
                throw(Error_See("AUTIAZ"))
            },
            0b101 => {
                throw(Error_See("AUTIASP"))
            },
            0b110 => {
                throw(Error_See("AUTIBZ"))
            },
            0b111 => {
                throw(Error_See("AUTIBSP"))
            }
          }
      },
      [bitzero, bitone, bitzero, bitzero, _, _, bitzero] => {
          op = SystemHintOp_BTI;
          SetBTypeCompatible(BTypeCompatible_BTI(op2[2 .. 1]))
      },
      0b0101000 => {
          if not_bool(IsFeatureImplemented(FEAT_CHK)) then {
              EndOfInstruction()
          };
          op = SystemHintOp_CHKFEAT
      },
      _ => {
          EndOfInstruction()
      }
    };
    execute_aarch64_instrs_system_hints(op)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 104) = {
    SEE = 104;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_hint_aarch64_instrs_system_hints(op2, CRm)
}

val execute_aarch64_instrs_branch_conditional_compare : forall 'datasize ('iszero : Bool) 't,
  (0 <= 't & 't <= 31 & 'datasize in {32, 64}).
  (int('datasize), bool('iszero), bits(64), int('t)) -> unit

function execute_aarch64_instrs_branch_conditional_compare (datasize, iszero, offset, t) = {
    let operand1 : bits('datasize) = X_read(t, datasize);
    let branch_conditional : bool = true;
    if IsZero(operand1) == iszero then {
        BranchTo(PC_read() + offset, BranchType_DIR, branch_conditional)
    } else {
        BranchNotTaken(BranchType_DIR, branch_conditional)
    }
}

val decode_cbnz_aarch64_instrs_branch_conditional_compare : (bits(5), bits(19), bits(1), bits(1)) -> unit

function decode_cbnz_aarch64_instrs_branch_conditional_compare (Rt, imm19, op, sf) = {
    let 't = UInt(Rt);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let iszero : bool = op == 0b0;
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    execute_aarch64_instrs_branch_conditional_compare(datasize, iszero, offset, t)
}

function clause __DecodeA64_BranchExcSys ((pc, ([_, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 117) = {
    SEE = 117;
    let Rt = Slice(__opcode, 0, 5);
    let imm19 = Slice(__opcode, 5, 19);
    let op = Slice(__opcode, 24, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_cbnz_aarch64_instrs_branch_conditional_compare(Rt, imm19, op, sf)
}

val decode_cbz_aarch64_instrs_branch_conditional_compare : (bits(5), bits(19), bits(1), bits(1)) -> unit

function decode_cbz_aarch64_instrs_branch_conditional_compare (Rt, imm19, op, sf) = {
    let 't = UInt(Rt);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let iszero : bool = op == 0b0;
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    execute_aarch64_instrs_branch_conditional_compare(datasize, iszero, offset, t)
}

function clause __DecodeA64_BranchExcSys ((pc, ([_, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 118) = {
    SEE = 118;
    let Rt = Slice(__opcode, 0, 5);
    let imm19 = Slice(__opcode, 5, 19);
    let op = Slice(__opcode, 24, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_cbz_aarch64_instrs_branch_conditional_compare(Rt, imm19, op, sf)
}

val execute_aarch64_instrs_integer_conditional_compare_register : forall 'datasize 'm 'n ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64}).
  (bits(4), int('datasize), bits(4), int('m), int('n), bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_conditional_compare_register (condition, datasize, flags__arg, m, n, sub_op) = {
    flags : bits(4) = flags__arg;
    taken : bool = false;
    if ConditionHolds(condition) then {
        let operand1 : bits('datasize) = X_read(n, datasize);
        operand2 : bits('datasize) = X_read(m, datasize);
        carry_in : bits(1) = 0b0;
        if sub_op then {
            operand2 = not_vec(operand2);
            carry_in = 0b1
        };
        let (_, __tup_1 : bits(4)) = AddWithCarry(operand1, operand2, carry_in)
        in
          {
              flags = __tup_1
          };
        taken = true
    };
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = flags;
    if SPESampleInFlight then {
        let conditional : bool = true;
        SPESampleAddOpOther(conditional, taken)
    };
    ()
}

val decode_ccmn_reg_aarch64_instrs_integer_conditional_compare_register : (bits(4), bits(5), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_ccmn_reg_aarch64_instrs_integer_conditional_compare_register (nzcv, Rn, cond, Rm, op, sf) = {
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let condition : bits(4) = cond;
    let flags : bits(4) = nzcv;
    execute_aarch64_instrs_integer_conditional_compare_register(condition, datasize, flags, m, n, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 119) = {
    SEE = 119;
    let nzcv = Slice(__opcode, 0, 4);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_ccmn_reg_aarch64_instrs_integer_conditional_compare_register(nzcv, Rn, cond, Rm, op, sf)
}

val decode_ccmp_reg_aarch64_instrs_integer_conditional_compare_register : (bits(4), bits(5), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_ccmp_reg_aarch64_instrs_integer_conditional_compare_register (nzcv, Rn, cond, Rm, op, sf) = {
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let condition : bits(4) = cond;
    let flags : bits(4) = nzcv;
    execute_aarch64_instrs_integer_conditional_compare_register(condition, datasize, flags, m, n, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 120) = {
    SEE = 120;
    let nzcv = Slice(__opcode, 0, 4);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_ccmp_reg_aarch64_instrs_integer_conditional_compare_register(nzcv, Rn, cond, Rm, op, sf)
}

val execute_aarch64_instrs_integer_conditional_compare_immediate : forall 'datasize 'n ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 'datasize in {32, 64}).
  (bits(4), int('datasize), bits(4), bits('datasize), int('n), bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_conditional_compare_immediate (condition, datasize, flags__arg, imm, n, sub_op) = {
    flags : bits(4) = flags__arg;
    taken : bool = false;
    if ConditionHolds(condition) then {
        let operand1 : bits('datasize) = X_read(n, datasize);
        operand2 : bits('datasize) = imm;
        carry_in : bits(1) = 0b0;
        if sub_op then {
            operand2 = not_vec(operand2);
            carry_in = 0b1
        };
        let (_, __tup_1 : bits(4)) = AddWithCarry(operand1, operand2, carry_in)
        in
          {
              flags = __tup_1
          };
        taken = true
    };
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = flags;
    if SPESampleInFlight then {
        let conditional : bool = true;
        SPESampleAddOpOther(conditional, taken)
    };
    ()
}

val decode_ccmn_imm_aarch64_instrs_integer_conditional_compare_immediate : (bits(4), bits(5), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_ccmn_imm_aarch64_instrs_integer_conditional_compare_immediate (nzcv, Rn, cond, imm5, op, sf) = {
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let condition : bits(4) = cond;
    let flags : bits(4) = nzcv;
    let imm : bits('datasize) = ZeroExtend(imm5, datasize);
    execute_aarch64_instrs_integer_conditional_compare_immediate(condition, datasize, flags, imm, n, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 121) = {
    SEE = 121;
    let nzcv = Slice(__opcode, 0, 4);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_ccmn_imm_aarch64_instrs_integer_conditional_compare_immediate(nzcv, Rn, cond, imm5, op, sf)
}

val decode_ccmp_imm_aarch64_instrs_integer_conditional_compare_immediate : (bits(4), bits(5), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_ccmp_imm_aarch64_instrs_integer_conditional_compare_immediate (nzcv, Rn, cond, imm5, op, sf) = {
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let sub_op : bool = op == 0b1;
    let condition : bits(4) = cond;
    let flags : bits(4) = nzcv;
    let imm : bits('datasize) = ZeroExtend(imm5, datasize);
    execute_aarch64_instrs_integer_conditional_compare_immediate(condition, datasize, flags, imm, n, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 122) = {
    SEE = 122;
    let nzcv = Slice(__opcode, 0, 4);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let imm5 = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_ccmp_imm_aarch64_instrs_integer_conditional_compare_immediate(nzcv, Rn, cond, imm5, op, sf)
}

val execute_aarch64_instrs_integer_flags_cfinv : unit -> unit

function execute_aarch64_instrs_integer_flags_cfinv () = {
    PSTATE.C = not_vec(PSTATE.C)
}

val decode_cfinv_aarch64_instrs_integer_flags_cfinv : bits(4) -> unit

function decode_cfinv_aarch64_instrs_integer_flags_cfinv CRm = {
    if not_bool(HaveFlagManipulateExt()) then {
        throw(Error_Undefined())
    };
    execute_aarch64_instrs_integer_flags_cfinv()
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000000100000000011111 as __opcode)) if SEE < 123) = {
    SEE = 123;
    let CRm = Slice(__opcode, 8, 4);
    if (([__opcode[8]] != 0b0 | [__opcode[9]] != 0b0) | [__opcode[10]] != 0b0) | [__opcode[11]] != 0b0 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_cfinv_aarch64_instrs_integer_flags_cfinv(CRm)
    }
}

val execute_aarch64_instrs_system_monitors : unit -> unit

function execute_aarch64_instrs_system_monitors () = {
    ClearExclusiveLocal(ProcessorID())
}

val decode_clrex_aarch64_instrs_system_monitors : bits(4) -> unit

function decode_clrex_aarch64_instrs_system_monitors CRm = {
    execute_aarch64_instrs_system_monitors()
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 124) = {
    SEE = 124;
    let CRm = Slice(__opcode, 8, 4);
    decode_clrex_aarch64_instrs_system_monitors(CRm)
}

val execute_aarch64_instrs_integer_arithmetic_cnt : forall 'd 'datasize 'n,
  (0 <= 'n & 'n <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('n), CountOp) -> unit

function execute_aarch64_instrs_integer_arithmetic_cnt (d, datasize, n, opcode) = {
    result : int = undefined;
    let operand1 : bits('datasize) = X_read(n, datasize);
    if opcode == CountOp_CLZ then {
        result = CountLeadingZeroBits(operand1)
    } else {
        result = CountLeadingSignBits(operand1)
    };
    let 'result = result;
    X_set(d, datasize) = result[datasize - 1 .. 0]
}

val decode_cls_int_aarch64_instrs_integer_arithmetic_cnt : (bits(5), bits(5), bits(1), bits(1)) -> unit

function decode_cls_int_aarch64_instrs_integer_arithmetic_cnt (Rd, Rn, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let opcode : CountOp = if op == 0b0 then CountOp_CLZ else CountOp_CLS;
    execute_aarch64_instrs_integer_arithmetic_cnt(d, datasize, n, opcode)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 127) = {
    SEE = 127;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 10, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_cls_int_aarch64_instrs_integer_arithmetic_cnt(Rd, Rn, op, sf)
}

val decode_clz_int_aarch64_instrs_integer_arithmetic_cnt : (bits(5), bits(5), bits(1), bits(1)) -> unit

function decode_clz_int_aarch64_instrs_integer_arithmetic_cnt (Rd, Rn, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let opcode : CountOp = if op == 0b0 then CountOp_CLZ else CountOp_CLS;
    execute_aarch64_instrs_integer_arithmetic_cnt(d, datasize, n, opcode)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 128) = {
    SEE = 128;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 10, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_clz_int_aarch64_instrs_integer_arithmetic_cnt(Rd, Rn, op, sf)
}

val execute_aarch64_instrs_integer_crc : forall ('crc32c : Bool) 'd 'm 'n 'size,
  ('size in {8, 16, 32, 64} & 0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 0 <= 'd & 'd <= 31).
  (bool('crc32c), int('d), int('m), int('n), int('size)) -> unit

function execute_aarch64_instrs_integer_crc (crc32c, d, m, n, size) = {
    let acc : bits(32) = X_read(n, 32);
    let val_name : bits('size) = X_read(m, size);
    let poly : bits(32) = (if crc32c then UInt(0x1EDC6F41) else UInt(0x04C11DB7))[31 .. 0];
    let tempacc : bits(32 + 'size) = BitReverse(acc) @ Zeros(size);
    let tempval : bits('size + 32) = BitReverse(val_name) @ Zeros(32);
    X_set(d, 32) = BitReverse(Poly32Mod2(EOR(tempacc, tempval), poly))
}

val decode_crc32_aarch64_instrs_integer_crc : (bits(5), bits(5), bits(2), bits(1), bits(5), bits(1)) -> unit

function decode_crc32_aarch64_instrs_integer_crc (Rd, Rn, sz, C, Rm, sf) = {
    if not_bool(HaveCRCExt()) then {
        throw(Error_Undefined())
    };
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if sf == 0b1 & sz != 0b11 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & sz == 0b11 then {
        throw(Error_Undefined())
    };
    let 'size = (8 << UInt(sz));
    let crc32c : bool = C == 0b1;
    execute_aarch64_instrs_integer_crc(crc32c, d, m, n, size)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 185) = {
    SEE = 185;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let sz = Slice(__opcode, 10, 2);
    let C = Slice(__opcode, 12, 1);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_crc32_aarch64_instrs_integer_crc(Rd, Rn, sz, C, Rm, sf)
}

val decode_crc32c_aarch64_instrs_integer_crc : (bits(5), bits(5), bits(2), bits(1), bits(5), bits(1)) -> unit

function decode_crc32c_aarch64_instrs_integer_crc (Rd, Rn, sz, C, Rm, sf) = {
    if not_bool(HaveCRCExt()) then {
        throw(Error_Undefined())
    };
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if sf == 0b1 & sz != 0b11 then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & sz == 0b11 then {
        throw(Error_Undefined())
    };
    let 'size = (8 << UInt(sz));
    let crc32c : bool = C == 0b1;
    execute_aarch64_instrs_integer_crc(crc32c, d, m, n, size)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 186) = {
    SEE = 186;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let sz = Slice(__opcode, 10, 2);
    let C = Slice(__opcode, 12, 1);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_crc32c_aarch64_instrs_integer_crc(Rd, Rn, sz, C, Rm, sf)
}

val execute_aarch64_instrs_integer_conditional_select : forall 'd 'datasize ('else_inc : Bool) ('else_inv : Bool) 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (bits(4), int('d), int('datasize), bool('else_inc), bool('else_inv), int('m), int('n)) -> unit

function execute_aarch64_instrs_integer_conditional_select (condition, d, datasize, else_inc, else_inv, m, n) = {
    taken : bool = false;
    result : bits('datasize) = undefined;
    if ConditionHolds(condition) then {
        result = X_read(n, datasize);
        taken = true
    } else {
        result = X_read(m, datasize);
        if else_inv then {
            result = not_vec(result)
        };
        if else_inc then {
            result = result + 1
        };
        ()
    };
    X_set(d, datasize) = result;
    if SPESampleInFlight then {
        let conditional : bool = true;
        SPESampleAddOpOther(conditional, taken)
    };
    ()
}

val decode_csel_aarch64_instrs_integer_conditional_select : (bits(5), bits(5), bits(1), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_csel_aarch64_instrs_integer_conditional_select (Rd, Rn, o2, cond, Rm, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let condition : bits(4) = cond;
    let else_inv : bool = op == 0b1;
    let else_inc : bool = o2 == 0b1;
    execute_aarch64_instrs_integer_conditional_select(condition, d, datasize, else_inc, else_inv, m, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 187) = {
    SEE = 187;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o2 = Slice(__opcode, 10, 1);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_csel_aarch64_instrs_integer_conditional_select(Rd, Rn, o2, cond, Rm, op, sf)
}

val decode_csinc_aarch64_instrs_integer_conditional_select : (bits(5), bits(5), bits(1), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_csinc_aarch64_instrs_integer_conditional_select (Rd, Rn, o2, cond, Rm, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let condition : bits(4) = cond;
    let else_inv : bool = op == 0b1;
    let else_inc : bool = o2 == 0b1;
    execute_aarch64_instrs_integer_conditional_select(condition, d, datasize, else_inc, else_inv, m, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 188) = {
    SEE = 188;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o2 = Slice(__opcode, 10, 1);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_csinc_aarch64_instrs_integer_conditional_select(Rd, Rn, o2, cond, Rm, op, sf)
}

val decode_csinv_aarch64_instrs_integer_conditional_select : (bits(5), bits(5), bits(1), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_csinv_aarch64_instrs_integer_conditional_select (Rd, Rn, o2, cond, Rm, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let condition : bits(4) = cond;
    let else_inv : bool = op == 0b1;
    let else_inc : bool = o2 == 0b1;
    execute_aarch64_instrs_integer_conditional_select(condition, d, datasize, else_inc, else_inv, m, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 189) = {
    SEE = 189;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o2 = Slice(__opcode, 10, 1);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_csinv_aarch64_instrs_integer_conditional_select(Rd, Rn, o2, cond, Rm, op, sf)
}

val decode_csneg_aarch64_instrs_integer_conditional_select : (bits(5), bits(5), bits(1), bits(4), bits(5), bits(1), bits(1)) -> unit

function decode_csneg_aarch64_instrs_integer_conditional_select (Rd, Rn, o2, cond, Rm, op, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let condition : bits(4) = cond;
    let else_inv : bool = op == 0b1;
    let else_inc : bool = o2 == 0b1;
    execute_aarch64_instrs_integer_conditional_select(condition, d, datasize, else_inc, else_inv, m, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 190) = {
    SEE = 190;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o2 = Slice(__opcode, 10, 1);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let op = Slice(__opcode, 30, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_csneg_aarch64_instrs_integer_conditional_select(Rd, Rn, o2, cond, Rm, op, sf)
}

val execute_aarch64_instrs_system_barriers_dmb : (MBReqDomain, MBReqTypes) -> unit

function execute_aarch64_instrs_system_barriers_dmb (domain, types) = {
    DataMemoryBarrier(domain, types)
}

val decode_dmb_aarch64_instrs_system_barriers_dmb : (bits(2), bits(4)) -> unit

function decode_dmb_aarch64_instrs_system_barriers_dmb (opc, CRm) = {
    domain : MBReqDomain = undefined;
    types : MBReqTypes = undefined;
    match CRm[3 .. 2] {
      0b00 => {
          domain = MBReqDomain_OuterShareable
      },
      0b01 => {
          domain = MBReqDomain_Nonshareable
      },
      0b10 => {
          domain = MBReqDomain_InnerShareable
      },
      0b11 => {
          domain = MBReqDomain_FullSystem
      }
    };
    match CRm[1 .. 0] {
      0b00 => {
          types = MBReqTypes_All;
          domain = MBReqDomain_FullSystem
      },
      0b01 => {
          types = MBReqTypes_Reads
      },
      0b10 => {
          types = MBReqTypes_Writes
      },
      0b11 => {
          types = MBReqTypes_All
      }
    };
    let types = types;
    execute_aarch64_instrs_system_barriers_dmb(domain, types)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 195) = {
    SEE = 195;
    let opc = Slice(__opcode, 5, 2);
    let CRm = Slice(__opcode, 8, 4);
    decode_dmb_aarch64_instrs_system_barriers_dmb(opc, CRm)
}

val execute_aarch64_instrs_system_barriers_dsb : forall ('nXS : Bool).
  (DSBAlias, MBReqDomain, bool('nXS), MBReqTypes) -> unit

function execute_aarch64_instrs_system_barriers_dsb (alias, domain, nXS__arg, types) = {
    nXS : bool = nXS__arg;
    match alias {
      DSBAlias_SSBB => {
          SpeculativeStoreBypassBarrierToVA()
      },
      DSBAlias_PSSBB => {
          SpeculativeStoreBypassBarrierToPA()
      },
      DSBAlias_DSB => {
          if HaveTME() & TSTATE.depth > 0 then {
              FailTransaction(TMFailure_ERR, false)
          };
          if not_bool(nXS) & HaveFeatXS() then {
              nXS = ((PSTATE.EL == EL0 | PSTATE.EL == EL1) & IsHCRXEL2Enabled()) & HCRX_EL2[FnXS] == 0b1
          };
          DataSynchronizationBarrier(domain, types, nXS)
      },
      _ => {
          Unreachable()
      }
    }
}

val decode_dsb_aarch64_instrs_system_barriers_dsb : (bits(2), bits(4)) -> unit

function decode_dsb_aarch64_instrs_system_barriers_dsb (opc, CRm) = {
    let nXS : bool = false;
    alias : DSBAlias = undefined;
    match CRm {
      0b0000 => {
          alias = DSBAlias_SSBB
      },
      0b0100 => {
          alias = DSBAlias_PSSBB
      },
      _ => {
          alias = DSBAlias_DSB
      }
    };
    let alias = alias;
    domain : MBReqDomain = undefined;
    match CRm[3 .. 2] {
      0b00 => {
          domain = MBReqDomain_OuterShareable
      },
      0b01 => {
          domain = MBReqDomain_Nonshareable
      },
      0b10 => {
          domain = MBReqDomain_InnerShareable
      },
      0b11 => {
          domain = MBReqDomain_FullSystem
      }
    };
    types : MBReqTypes = undefined;
    match CRm[1 .. 0] {
      0b00 => {
          types = MBReqTypes_All;
          domain = MBReqDomain_FullSystem
      },
      0b01 => {
          types = MBReqTypes_Reads
      },
      0b10 => {
          types = MBReqTypes_Writes
      },
      0b11 => {
          types = MBReqTypes_All
      }
    };
    execute_aarch64_instrs_system_barriers_dsb(alias, domain, nXS, types)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 197) = {
    SEE = 197;
    let opc = Slice(__opcode, 5, 2);
    let CRm = Slice(__opcode, 8, 4);
    decode_dsb_aarch64_instrs_system_barriers_dsb(opc, CRm)
}

val execute_aarch64_instrs_integer_ins_ext_extract_immediate : forall 'd 'datasize 'lsb 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 0 <= 'lsb & 'lsb <= 63 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('lsb), int('m), int('n)) -> unit

function execute_aarch64_instrs_integer_ins_ext_extract_immediate (d, datasize, lsb, m, n) = {
    let operand1 : bits('datasize) = X_read(n, datasize);
    let operand2 : bits('datasize) = X_read(m, datasize);
    let concat : bits(2 * 'datasize) = operand1 @ operand2;
    assert(constraint('lsb + 'datasize - 1 < 2 * 'datasize));
    let result : bits('datasize) = concat[lsb + datasize - 1 .. lsb];
    X_set(d, datasize) = result
}

val decode_extr_aarch64_instrs_integer_ins_ext_extract_immediate : (bits(5), bits(5), bits(6), bits(5), bits(1), bits(1)) -> unit

function decode_extr_aarch64_instrs_integer_ins_ext_extract_immediate (Rd, Rn, imms, Rm, N, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    if N != sf then {
        throw(Error_Undefined())
    };
    if sf == 0b0 & [imms[5]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'lsb = UInt(imms);
    execute_aarch64_instrs_integer_ins_ext_extract_immediate(d, datasize, lsb, m, n)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 206) = {
    SEE = 206;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imms = Slice(__opcode, 10, 6);
    let Rm = Slice(__opcode, 16, 5);
    let N = Slice(__opcode, 22, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_extr_aarch64_instrs_integer_ins_ext_extract_immediate(Rd, Rn, imms, Rm, N, sf)
}

val execute_aarch64_instrs_system_barriers_isb : unit -> unit

function execute_aarch64_instrs_system_barriers_isb () = {
    InstructionSynchronizationBarrier();
    if HaveBRBExt() & BRBEBranchOnISB() then {
        BRBEISB()
    };
    if HaveStatisticalProfiling() then {
        SPEISB()
    };
    ()
}

val decode_isb_aarch64_instrs_system_barriers_isb : (bits(2), bits(4)) -> unit

function decode_isb_aarch64_instrs_system_barriers_isb (opc, CRm) = {
    execute_aarch64_instrs_system_barriers_isb()
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 483) = {
    SEE = 483;
    let opc = Slice(__opcode, 5, 2);
    let CRm = Slice(__opcode, 8, 4);
    decode_isb_aarch64_instrs_system_barriers_isb(opc, CRm)
}

val execute_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub : forall 'a 'd 'datasize 'destsize 'm 'n ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'destsize in {32, 64} & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31 & 0 <= 'a & 'a <= 31).
  (int('a), int('d), int('datasize), int('destsize), int('m), int('n), bool('sub_op)) -> unit

function execute_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub (a, d, datasize, destsize, m, n, sub_op) = {
    let operand1 : bits('datasize) = X_read(n, datasize);
    let operand2 : bits('datasize) = X_read(m, datasize);
    let operand3 : bits('destsize) = X_read(a, destsize);
    result : int = undefined;
    if sub_op then {
        result = UInt(operand3) - UInt(operand1) * UInt(operand2)
    } else {
        result = UInt(operand3) + UInt(operand1) * UInt(operand2)
    };
    let 'result = result;
    X_set(d, destsize) = result[destsize - 1 .. 0]
}

val decode_madd_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_madd_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub (Rd, Rn, Ra, o0, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = if sf == 0b1 then 64 else 32;
    let 'datasize = destsize;
    let sub_op : bool = o0 == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub(a, d, datasize, destsize, m, n, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 690) = {
    SEE = 690;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_madd_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub(Rd, Rn, Ra, o0, Rm, sf)
}

val decode_msub_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_msub_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub (Rd, Rn, Ra, o0, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = if sf == 0b1 then 64 else 32;
    let 'datasize = destsize;
    let sub_op : bool = o0 == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub(a, d, datasize, destsize, m, n, sub_op)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 691) = {
    SEE = 691;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_msub_aarch64_instrs_integer_arithmetic_mul_uniform_add_sub(Rd, Rn, Ra, o0, Rm, sf)
}

val execute_aarch64_instrs_integer_ins_ext_insert_movewide : forall 'd 'datasize 'pos,
  (0 <= 'pos & 'pos <= 63 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), bits(16), MoveWideOp, int('pos)) -> unit

function execute_aarch64_instrs_integer_ins_ext_insert_movewide (d, datasize, imm, opcode, pos) = {
    result : bits('datasize) = undefined;
    if opcode == MoveWideOp_K then {
        result = X_read(d, datasize)
    } else {
        result = Zeros(datasize)
    };
    assert(constraint('pos + 15 < 'datasize));
    result[pos + 15 .. pos] = imm;
    if opcode == MoveWideOp_N then {
        result = not_vec(result)
    };
    let result = result;
    X_set(d, datasize) = result
}

val decode_movk_aarch64_instrs_integer_ins_ext_insert_movewide : (bits(5), bits(16), bits(2), bits(2), bits(1)) -> unit

function decode_movk_aarch64_instrs_integer_ins_ext_insert_movewide (Rd, imm16, hw, opc, sf) = {
    let 'd = UInt(Rd);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let imm : bits(16) = imm16;
    opcode : MoveWideOp = undefined;
    match opc {
      0b00 => {
          opcode = MoveWideOp_N
      },
      0b10 => {
          opcode = MoveWideOp_Z
      },
      0b11 => {
          opcode = MoveWideOp_K
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let opcode = opcode;
    if sf == 0b0 & [hw[1]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'pos = UInt(hw @ 0b0000);
    execute_aarch64_instrs_integer_ins_ext_insert_movewide(d, datasize, imm, opcode, pos)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 696) = {
    SEE = 696;
    let Rd = Slice(__opcode, 0, 5);
    let imm16 = Slice(__opcode, 5, 16);
    let hw = Slice(__opcode, 21, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_movk_aarch64_instrs_integer_ins_ext_insert_movewide(Rd, imm16, hw, opc, sf)
}

val decode_movn_aarch64_instrs_integer_ins_ext_insert_movewide : (bits(5), bits(16), bits(2), bits(2), bits(1)) -> unit

function decode_movn_aarch64_instrs_integer_ins_ext_insert_movewide (Rd, imm16, hw, opc, sf) = {
    let 'd = UInt(Rd);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let imm : bits(16) = imm16;
    opcode : MoveWideOp = undefined;
    match opc {
      0b00 => {
          opcode = MoveWideOp_N
      },
      0b10 => {
          opcode = MoveWideOp_Z
      },
      0b11 => {
          opcode = MoveWideOp_K
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let opcode = opcode;
    if sf == 0b0 & [hw[1]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'pos = UInt(hw @ 0b0000);
    execute_aarch64_instrs_integer_ins_ext_insert_movewide(d, datasize, imm, opcode, pos)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 697) = {
    SEE = 697;
    let Rd = Slice(__opcode, 0, 5);
    let imm16 = Slice(__opcode, 5, 16);
    let hw = Slice(__opcode, 21, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_movn_aarch64_instrs_integer_ins_ext_insert_movewide(Rd, imm16, hw, opc, sf)
}

val decode_movz_aarch64_instrs_integer_ins_ext_insert_movewide : (bits(5), bits(16), bits(2), bits(2), bits(1)) -> unit

function decode_movz_aarch64_instrs_integer_ins_ext_insert_movewide (Rd, imm16, hw, opc, sf) = {
    let 'd = UInt(Rd);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let imm : bits(16) = imm16;
    opcode : MoveWideOp = undefined;
    match opc {
      0b00 => {
          opcode = MoveWideOp_N
      },
      0b10 => {
          opcode = MoveWideOp_Z
      },
      0b11 => {
          opcode = MoveWideOp_K
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let opcode = opcode;
    if sf == 0b0 & [hw[1]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'pos = UInt(hw @ 0b0000);
    execute_aarch64_instrs_integer_ins_ext_insert_movewide(d, datasize, imm, opcode, pos)
}

function clause __DecodeA64_DataProcImm ((pc, ([_, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 698) = {
    SEE = 698;
    let Rd = Slice(__opcode, 0, 5);
    let imm16 = Slice(__opcode, 5, 16);
    let hw = Slice(__opcode, 21, 2);
    let opc = Slice(__opcode, 29, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_movz_aarch64_instrs_integer_ins_ext_insert_movewide(Rd, imm16, hw, opc, sf)
}

val execute_aarch64_instrs_integer_pac_pacda_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_pacda_dp_1src (d, n, source_is_sp) = {
    if source_is_sp then {
        X_set(d, 64) = AddPACDA(X_read(d, 64), SP_read())
    } else {
        X_set(d, 64) = AddPACDA(X_read(d, 64), X_read(n, 64))
    }
}

val decode_pacda_aarch64_instrs_integer_pac_pacda_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_pacda_aarch64_instrs_integer_pac_pacda_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_pacda_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 708) = {
    SEE = 708;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_pacda_aarch64_instrs_integer_pac_pacda_dp_1src(Rd, Rn, Z)
}

val execute_aarch64_instrs_integer_pac_pacdb_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_pacdb_dp_1src (d, n, source_is_sp) = {
    if source_is_sp then {
        X_set(d, 64) = AddPACDB(X_read(d, 64), SP_read())
    } else {
        X_set(d, 64) = AddPACDB(X_read(d, 64), X_read(n, 64))
    }
}

val decode_pacdb_aarch64_instrs_integer_pac_pacdb_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_pacdb_aarch64_instrs_integer_pac_pacdb_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_pacdb_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 709) = {
    SEE = 709;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_pacdb_aarch64_instrs_integer_pac_pacdb_dp_1src(Rd, Rn, Z)
}

val execute_aarch64_instrs_integer_pac_pacga_dp_2src : forall 'd 'm 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('m), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_pacga_dp_2src (d, m, n, source_is_sp) = {
    if source_is_sp then {
        X_set(d, 64) = AddPACGA(X_read(n, 64), SP_read())
    } else {
        X_set(d, 64) = AddPACGA(X_read(n, 64), X_read(m, 64))
    }
}

val decode_pacga_aarch64_instrs_integer_pac_pacga_dp_2src : (bits(5), bits(5), bits(5)) -> unit

function decode_pacga_aarch64_instrs_integer_pac_pacga_dp_2src (Rd, Rn, Rm) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if m == 31 then {
        source_is_sp = true
    };
    execute_aarch64_instrs_integer_pac_pacga_dp_2src(d, m, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 710) = {
    SEE = 710;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    decode_pacga_aarch64_instrs_integer_pac_pacga_dp_2src(Rd, Rn, Rm)
}

val execute_aarch64_instrs_integer_pac_pacia_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_pacia_dp_1src (d, n, source_is_sp) = {
    if HavePACExt() then {
        if source_is_sp then {
            X_set(d, 64) = AddPACIA(X_read(d, 64), SP_read())
        } else {
            X_set(d, 64) = AddPACIA(X_read(d, 64), X_read(n, 64))
        }
    };
    ()
}

val decode_pacia_aarch64_instrs_integer_pac_pacia_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_pacia_aarch64_instrs_integer_pac_pacia_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_pacia_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 711) = {
    SEE = 711;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_pacia_aarch64_instrs_integer_pac_pacia_dp_1src(Rd, Rn, Z)
}

val decode_pacia_aarch64_instrs_integer_pac_pacia_hint : (bits(3), bits(4)) -> unit

function decode_pacia_aarch64_instrs_integer_pac_pacia_hint (op2, CRm) = {
    d : {17, 30} = 17;
    n : int = undefined;
    source_is_sp : bool = false;
    match CRm @ op2 {
      0b0011000 => {
          d = 30;
          n = 31
      },
      0b0011001 => {
          d = 30;
          source_is_sp = true;
          if HaveBTIExt() then {
              SetBTypeCompatible(BTypeCompatible_PACIXSP())
          };
          ()
      },
      0b0001000 => {
          d = 17;
          n = 16
      },
      0b0001010 => {
          throw(Error_See("PACIB"))
      },
      0b0001100 => {
          throw(Error_See("AUTIA"))
      },
      0b0001110 => {
          throw(Error_See("AUTIB"))
      },
      [bitzero, bitzero, bitone, bitone, bitzero, bitone, _] => {
          throw(Error_See("PACIB"))
      },
      [bitzero, bitzero, bitone, bitone, bitone, bitzero, _] => {
          throw(Error_See("AUTIA"))
      },
      [bitzero, bitzero, bitone, bitone, bitone, bitone, _] => {
          throw(Error_See("AUTIB"))
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      _ => {
          throw(Error_See("HINT"))
      }
    };
    let 'n = n;
    let 'd = d;
    assert(constraint((0 <= 'n & 'n <= 31)));
    execute_aarch64_instrs_integer_pac_pacia_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, bitone, bitzero, bitzero, _, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 712) = {
    SEE = 712;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_pacia_aarch64_instrs_integer_pac_pacia_hint(op2, CRm)
}

val execute_aarch64_instrs_integer_pac_pacib_dp_1src : forall 'd 'n ('source_is_sp : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n), bool('source_is_sp)) -> unit

function execute_aarch64_instrs_integer_pac_pacib_dp_1src (d, n, source_is_sp) = {
    if HavePACExt() then {
        if source_is_sp then {
            X_set(d, 64) = AddPACIB(X_read(d, 64), SP_read())
        } else {
            X_set(d, 64) = AddPACIB(X_read(d, 64), X_read(n, 64))
        }
    };
    ()
}

val decode_pacib_aarch64_instrs_integer_pac_pacib_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_pacib_aarch64_instrs_integer_pac_pacib_dp_1src (Rd, Rn, Z) = {
    source_is_sp : bool = false;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if Z == 0b0 then {
        if n == 31 then {
            source_is_sp = true
        };
        ()
    } else {
        if n != 31 then {
            throw(Error_Undefined())
        };
        ()
    };
    execute_aarch64_instrs_integer_pac_pacib_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 713) = {
    SEE = 713;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Z = Slice(__opcode, 13, 1);
    decode_pacib_aarch64_instrs_integer_pac_pacib_dp_1src(Rd, Rn, Z)
}

val decode_pacib_aarch64_instrs_integer_pac_pacib_hint : (bits(3), bits(4)) -> unit

function decode_pacib_aarch64_instrs_integer_pac_pacib_hint (op2, CRm) = {
    d : {17, 30} = 17;
    n : int = undefined;
    source_is_sp : bool = false;
    match CRm @ op2 {
      0b0011010 => {
          d = 30;
          n = 31
      },
      0b0011011 => {
          d = 30;
          source_is_sp = true;
          if HaveBTIExt() then {
              SetBTypeCompatible(BTypeCompatible_PACIXSP())
          };
          ()
      },
      0b0001010 => {
          d = 17;
          n = 16
      },
      0b0001000 => {
          throw(Error_See("PACIA"))
      },
      0b0001100 => {
          throw(Error_See("AUTIA"))
      },
      0b0001110 => {
          throw(Error_See("AUTIB"))
      },
      [bitzero, bitzero, bitone, bitone, bitzero, bitzero, _] => {
          throw(Error_See("PACIA"))
      },
      [bitzero, bitzero, bitone, bitone, bitone, bitzero, _] => {
          throw(Error_See("AUTIA"))
      },
      [bitzero, bitzero, bitone, bitone, bitone, bitone, _] => {
          throw(Error_See("AUTIB"))
      },
      0b0000111 => {
          throw(Error_See("XPACLRI"))
      },
      _ => {
          throw(Error_See("HINT"))
      }
    };
    let 'n = n;
    let 'd = d;
    assert(constraint((0 <= 'n & 'n <= 31)));
    execute_aarch64_instrs_integer_pac_pacib_dp_1src(d, n, source_is_sp)
}

function clause __DecodeA64_BranchExcSys ((pc, ([bitone, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, bitone, bitzero, bitone, _, bitone, bitone, bitone, bitone, bitone] as __opcode)) if SEE < 714) = {
    SEE = 714;
    let op2 = Slice(__opcode, 5, 3);
    let CRm = Slice(__opcode, 8, 4);
    decode_pacib_aarch64_instrs_integer_pac_pacib_hint(op2, CRm)
}

val execute_aarch64_instrs_integer_arithmetic_rbit : forall 'd 'datasize 'n,
  (0 <= 'n & 'n <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('n)) -> unit

function execute_aarch64_instrs_integer_arithmetic_rbit (d, datasize, n) = {
    let operand : bits('datasize) = X_read(n, datasize);
    result : bits('datasize) = undefined;
    foreach (i from 0 to (datasize - 1) by 1 in inc) {
        result[datasize - 1 - i] = Bit([operand[i]])
    };
    X_set(d, datasize) = result
}

val decode_rbit_int_aarch64_instrs_integer_arithmetic_rbit : (bits(5), bits(5), bits(1)) -> unit

function decode_rbit_int_aarch64_instrs_integer_arithmetic_rbit (Rd, Rn, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    execute_aarch64_instrs_integer_arithmetic_rbit(d, datasize, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 719) = {
    SEE = 719;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_rbit_int_aarch64_instrs_integer_arithmetic_rbit(Rd, Rn, sf)
}

val execute_aarch64_instrs_integer_arithmetic_rev : forall 'container_size 'd 'datasize 'n,
  (0 <= 'n & 'n <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31 & 'container_size in {16, 32, 64}).
  (int('container_size), int('d), int('datasize), int('n)) -> unit

function execute_aarch64_instrs_integer_arithmetic_rev (container_size, d, datasize, n) = {
    let operand : bits('datasize) = X_read(n, datasize);
    result : bits('datasize) = undefined;
    let 'containers = DIV(datasize, container_size);
    let 'elements_per_container = DIV(container_size, 8);
    index : int = 0;
    rev_index : int = undefined;
    foreach (c from 0 to (containers - 1) by 1 in inc) {
        rev_index = index + (elements_per_container - 1) * 8;
        foreach (e from 0 to (elements_per_container - 1) by 1 in inc) {
            let 'rev_index = rev_index in
              {
                  let 'index = index;
                  assert(constraint((0 <= 'rev_index & 'rev_index + 7 < 'datasize)));
                  assert(constraint((0 <= 'index & 'index + 7 < 'datasize)));
                  result[rev_index + 7 .. rev_index] = operand[index + 7 .. index]
              };
            index = index + 8;
            rev_index = rev_index - 8
        }
    };
    X_set(d, datasize) = result
}

val decode_rev_aarch64_instrs_integer_arithmetic_rev : (bits(5), bits(5), bits(2), bits(1)) -> unit

function decode_rev_aarch64_instrs_integer_arithmetic_rev (Rd, Rn, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    container_size : {16, 32, 64} = 16;
    match opc {
      0b00 => {
          Unreachable()
      },
      0b01 => {
          container_size = 16
      },
      0b10 => {
          container_size = 32
      },
      0b11 => {
          if sf == 0b0 then {
              throw(Error_Undefined())
          };
          container_size = 64
      }
    };
    let 'container_size = container_size;
    execute_aarch64_instrs_integer_arithmetic_rev(container_size, d, datasize, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 736) = {
    SEE = 736;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 10, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_rev_aarch64_instrs_integer_arithmetic_rev(Rd, Rn, opc, sf)
}

val decode_rev16_int_aarch64_instrs_integer_arithmetic_rev : (bits(5), bits(5), bits(2), bits(1)) -> unit

function decode_rev16_int_aarch64_instrs_integer_arithmetic_rev (Rd, Rn, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    container_size : {16, 32, 64} = 16;
    match opc {
      0b00 => {
          Unreachable()
      },
      0b01 => {
          container_size = 16
      },
      0b10 => {
          container_size = 32
      },
      0b11 => {
          if sf == 0b0 then {
              throw(Error_Undefined())
          };
          container_size = 64
      }
    };
    let 'container_size = container_size;
    execute_aarch64_instrs_integer_arithmetic_rev(container_size, d, datasize, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 737) = {
    SEE = 737;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 10, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_rev16_int_aarch64_instrs_integer_arithmetic_rev(Rd, Rn, opc, sf)
}

val decode_rev32_int_aarch64_instrs_integer_arithmetic_rev : (bits(5), bits(5), bits(2), bits(1)) -> unit

function decode_rev32_int_aarch64_instrs_integer_arithmetic_rev (Rd, Rn, opc, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'datasize = if sf == 0b1 then 64 else 32;
    container_size : {16, 32, 64} = 16;
    match opc {
      0b00 => {
          Unreachable()
      },
      0b01 => {
          container_size = 16
      },
      0b10 => {
          container_size = 32
      },
      0b11 => {
          if sf == 0b0 then {
              throw(Error_Undefined())
          };
          container_size = 64
      }
    };
    let 'container_size = container_size;
    execute_aarch64_instrs_integer_arithmetic_rev(container_size, d, datasize, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 738) = {
    SEE = 738;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 10, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_rev32_int_aarch64_instrs_integer_arithmetic_rev(Rd, Rn, opc, sf)
}

val execute_aarch64_instrs_integer_flags_rmif : forall 'lsb 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'lsb & 'lsb <= 63).
  (int('lsb), bits(4), int('n)) -> unit

function execute_aarch64_instrs_integer_flags_rmif (lsb, mask, n) = {
    let tmpreg : bits(64) = X_read(n, 64);
    let tmp : bits(4) = (tmpreg @ tmpreg)[lsb + 3 .. lsb];
    if [mask[3]] == 0b1 then {
        PSTATE.N = [tmp[3]]
    };
    if [mask[2]] == 0b1 then {
        PSTATE.Z = [tmp[2]]
    };
    if [mask[1]] == 0b1 then {
        PSTATE.C = [tmp[1]]
    };
    if [mask[0]] == 0b1 then {
        PSTATE.V = [tmp[0]]
    };
    ()
}

val decode_rmif_aarch64_instrs_integer_flags_rmif : (bits(4), bits(5), bits(6), bits(1)) -> unit

function decode_rmif_aarch64_instrs_integer_flags_rmif (mask, Rn, imm6, sf) = {
    if not_bool(HaveFlagManipulateExt()) | sf != 0b1 then {
        throw(Error_Undefined())
    };
    let 'lsb = UInt(imm6);
    let 'n = UInt(Rn);
    execute_aarch64_instrs_integer_flags_rmif(lsb, mask, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 742) = {
    SEE = 742;
    let mask = Slice(__opcode, 0, 4);
    let Rn = Slice(__opcode, 5, 5);
    let imm6 = Slice(__opcode, 15, 6);
    let sf = Slice(__opcode, 31, 1);
    decode_rmif_aarch64_instrs_integer_flags_rmif(mask, Rn, imm6, sf)
}

val execute_aarch64_instrs_integer_arithmetic_div : forall 'd 'datasize ('is_unsigned : Bool) 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('m), int('n), bool('is_unsigned)) -> unit

function execute_aarch64_instrs_integer_arithmetic_div (d, datasize, m, n, is_unsigned) = {
    let operand1 : bits('datasize) = X_read(n, datasize);
    let operand2 : bits('datasize) = X_read(m, datasize);
    result : int = undefined;
    if IsZero(operand2) then {
        result = 0
    } else {
        result = RoundTowardsZero(Real(asl_Int(operand1, is_unsigned)) / Real(asl_Int(operand2, is_unsigned)))
    };
    let 'result = result;
    X_set(d, datasize) = result[datasize - 1 .. 0]
}

val decode_sdiv_aarch64_instrs_integer_arithmetic_div : (bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_sdiv_aarch64_instrs_integer_arithmetic_div (Rd, Rn, o1, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let is_unsigned : bool = o1 == 0b0;
    execute_aarch64_instrs_integer_arithmetic_div(d, datasize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 781) = {
    SEE = 781;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o1 = Slice(__opcode, 10, 1);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_sdiv_aarch64_instrs_integer_arithmetic_div(Rd, Rn, o1, Rm, sf)
}

val decode_udiv_aarch64_instrs_integer_arithmetic_div : (bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_udiv_aarch64_instrs_integer_arithmetic_div (Rd, Rn, o1, Rm, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'datasize = if sf == 0b1 then 64 else 32;
    let is_unsigned : bool = o1 == 0b0;
    execute_aarch64_instrs_integer_arithmetic_div(d, datasize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([_, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 782) = {
    SEE = 782;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o1 = Slice(__opcode, 10, 1);
    let Rm = Slice(__opcode, 16, 5);
    let sf = Slice(__opcode, 31, 1);
    decode_udiv_aarch64_instrs_integer_arithmetic_div(Rd, Rn, o1, Rm, sf)
}

val execute_aarch64_instrs_integer_flags_setf : forall 'msb 'n,
  (0 <= 'n & 'n <= 31 & 'msb in {7, 15}).
  (int('msb), int('n)) -> unit

function execute_aarch64_instrs_integer_flags_setf (msb, n) = {
    let tmpreg : bits(32) = X_read(n, 32);
    PSTATE.N = [tmpreg[msb]];
    PSTATE.Z = if tmpreg[msb .. 0] == Zeros(msb + 1) then 0b1 else 0b0;
    PSTATE.V = EOR([tmpreg[msb + 1]], [tmpreg[msb]])
}

val decode_setf_aarch64_instrs_integer_flags_setf : (bits(5), bits(1), bits(1)) -> unit

function decode_setf_aarch64_instrs_integer_flags_setf (Rn, sz, sf) = {
    if not_bool(HaveFlagManipulateExt()) | sf != 0b0 then {
        throw(Error_Undefined())
    };
    let 'msb = if sz == 0b1 then 15 else 7;
    let 'n = UInt(Rn);
    execute_aarch64_instrs_integer_flags_setf(msb, n)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitone] as __opcode)) if SEE < 791) = {
    SEE = 791;
    let Rn = Slice(__opcode, 5, 5);
    let sz = Slice(__opcode, 14, 1);
    let sf = Slice(__opcode, 31, 1);
    decode_setf_aarch64_instrs_integer_flags_setf(Rn, sz, sf)
}

val execute_aarch64_instrs_integer_arithmetic_mul_widening_32_64 : forall 'a 'd 'datasize 'destsize ('is_unsigned : Bool) 'm 'n ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'destsize == 64 & 'datasize == 32 & 0 <= 'd & 'd <= 31 & 0 <= 'a & 'a <= 31).
  (int('a), int('d), int('datasize), int('destsize), int('m), int('n), bool('sub_op), bool('is_unsigned)) -> unit

function execute_aarch64_instrs_integer_arithmetic_mul_widening_32_64 (a, d, datasize, destsize, m, n, sub_op, is_unsigned) = {
    let operand1 : bits('datasize) = X_read(n, datasize);
    let operand2 : bits('datasize) = X_read(m, datasize);
    let operand3 : bits('destsize) = X_read(a, destsize);
    result : int = undefined;
    if sub_op then {
        result = asl_Int(operand3, is_unsigned) - asl_Int(operand1, is_unsigned) * asl_Int(operand2, is_unsigned)
    } else {
        result = asl_Int(operand3, is_unsigned) + asl_Int(operand1, is_unsigned) * asl_Int(operand2, is_unsigned)
    };
    let 'result = result;
    X_set(d, 64) = result[63 .. 0]
}

val decode_smaddl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_smaddl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 (Rd, Rn, Ra, o0, Rm, U) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = 64;
    let 'datasize = 32;
    let sub_op : bool = o0 == 0b1;
    let is_unsigned : bool = U == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_widening_32_64(a, d, datasize, destsize, m, n, sub_op, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 828) = {
    SEE = 828;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let U = Slice(__opcode, 23, 1);
    decode_smaddl_aarch64_instrs_integer_arithmetic_mul_widening_32_64(Rd, Rn, Ra, o0, Rm, U)
}

val decode_smsubl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_smsubl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 (Rd, Rn, Ra, o0, Rm, U) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = 64;
    let 'datasize = 32;
    let sub_op : bool = o0 == 0b1;
    let is_unsigned : bool = U == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_widening_32_64(a, d, datasize, destsize, m, n, sub_op, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 829) = {
    SEE = 829;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let U = Slice(__opcode, 23, 1);
    decode_smsubl_aarch64_instrs_integer_arithmetic_mul_widening_32_64(Rd, Rn, Ra, o0, Rm, U)
}

val decode_umaddl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_umaddl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 (Rd, Rn, Ra, o0, Rm, U) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = 64;
    let 'datasize = 32;
    let sub_op : bool = o0 == 0b1;
    let is_unsigned : bool = U == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_widening_32_64(a, d, datasize, destsize, m, n, sub_op, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitzero, bitone, _, _, _, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 830) = {
    SEE = 830;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let U = Slice(__opcode, 23, 1);
    decode_umaddl_aarch64_instrs_integer_arithmetic_mul_widening_32_64(Rd, Rn, Ra, o0, Rm, U)
}

val decode_umsubl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1)) -> unit

function decode_umsubl_aarch64_instrs_integer_arithmetic_mul_widening_32_64 (Rd, Rn, Ra, o0, Rm, U) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = 64;
    let 'datasize = 32;
    let sub_op : bool = o0 == 0b1;
    let is_unsigned : bool = U == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_widening_32_64(a, d, datasize, destsize, m, n, sub_op, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitzero, bitone, _, _, _, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 831) = {
    SEE = 831;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let U = Slice(__opcode, 23, 1);
    decode_umsubl_aarch64_instrs_integer_arithmetic_mul_widening_32_64(Rd, Rn, Ra, o0, Rm, U)
}

val execute_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi : forall 'd 'datasize ('is_unsigned : Bool) 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize == 64 & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('m), int('n), bool('is_unsigned)) -> unit

function execute_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi (d, datasize, m, n, is_unsigned) = {
    let operand1 : bits('datasize) = X_read(n, datasize);
    let operand2 : bits('datasize) = X_read(m, datasize);
    let 'result = asl_Int(operand1, is_unsigned) * asl_Int(operand2, is_unsigned);
    X_set(d, 64) = result[127 .. 64]
}

val decode_smulh_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi : (bits(5), bits(5), bits(5), bits(5), bits(1)) -> unit

function decode_smulh_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi (Rd, Rn, Ra, Rm, U) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = 64;
    let 'datasize = destsize;
    let is_unsigned : bool = U == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi(d, datasize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 861) = {
    SEE = 861;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let Rm = Slice(__opcode, 16, 5);
    let U = Slice(__opcode, 23, 1);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_smulh_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi(Rd, Rn, Ra, Rm, U)
    }
}

val decode_umulh_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi : (bits(5), bits(5), bits(5), bits(5), bits(1)) -> unit

function decode_umulh_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi (Rd, Rn, Ra, Rm, U) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    let 'a = UInt(Ra);
    let 'destsize = 64;
    let 'datasize = destsize;
    let is_unsigned : bool = U == 0b1;
    execute_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi(d, datasize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 862) = {
    SEE = 862;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let Rm = Slice(__opcode, 16, 5);
    let U = Slice(__opcode, 23, 1);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_umulh_aarch64_instrs_integer_arithmetic_mul_widening_64_128hi(Rd, Rn, Ra, Rm, U)
    }
}

val execute_aarch64_instrs_branch_conditional_test : forall 'bit_pos 'datasize 't,
  (0 <= 't & 't <= 31 & 'datasize in {32, 64} & 0 <= 'bit_pos & 'bit_pos <= 63).
  (int('bit_pos), bits(1), int('datasize), bits(64), int('t)) -> unit

function execute_aarch64_instrs_branch_conditional_test (bit_pos, bit_val, datasize, offset, t) = {
    let operand : bits('datasize) = X_read(t, datasize);
    let branch_conditional : bool = true;
    assert(constraint('bit_pos < 'datasize));
    if [operand[bit_pos]] == bit_val then {
        BranchTo(PC_read() + offset, BranchType_DIR, branch_conditional)
    } else {
        BranchNotTaken(BranchType_DIR, branch_conditional)
    }
}

val decode_tbnz_aarch64_instrs_branch_conditional_test : (bits(5), bits(14), bits(5), bits(1), bits(1)) -> unit

function decode_tbnz_aarch64_instrs_branch_conditional_test (Rt, imm14, b40, op, b5) = {
    let 't = UInt(Rt);
    let 'datasize = if b5 == 0b1 then 64 else 32;
    let 'bit_pos = UInt(b5 @ b40);
    let bit_val : bits(1) = op;
    let offset : bits(64) = SignExtend(imm14 @ 0b00, 64);
    execute_aarch64_instrs_branch_conditional_test(bit_pos, bit_val, datasize, offset, t)
}

function clause __DecodeA64_BranchExcSys ((pc, ([_, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1009) = {
    SEE = 1009;
    let Rt = Slice(__opcode, 0, 5);
    let imm14 = Slice(__opcode, 5, 14);
    let b40 = Slice(__opcode, 19, 5);
    let op = Slice(__opcode, 24, 1);
    let b5 = Slice(__opcode, 31, 1);
    decode_tbnz_aarch64_instrs_branch_conditional_test(Rt, imm14, b40, op, b5)
}

val decode_tbz_aarch64_instrs_branch_conditional_test : (bits(5), bits(14), bits(5), bits(1), bits(1)) -> unit

function decode_tbz_aarch64_instrs_branch_conditional_test (Rt, imm14, b40, op, b5) = {
    let 't = UInt(Rt);
    let 'datasize = if b5 == 0b1 then 64 else 32;
    let 'bit_pos = UInt(b5 @ b40);
    let bit_val : bits(1) = op;
    let offset : bits(64) = SignExtend(imm14 @ 0b00, 64);
    execute_aarch64_instrs_branch_conditional_test(bit_pos, bit_val, datasize, offset, t)
}

function clause __DecodeA64_BranchExcSys ((pc, ([_, bitzero, bitone, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1010) = {
    SEE = 1010;
    let Rt = Slice(__opcode, 0, 5);
    let imm14 = Slice(__opcode, 5, 14);
    let b40 = Slice(__opcode, 19, 5);
    let op = Slice(__opcode, 24, 1);
    let b5 = Slice(__opcode, 31, 1);
    decode_tbz_aarch64_instrs_branch_conditional_test(Rt, imm14, b40, op, b5)
}

val execute_aarch64_instrs_integer_pac_strip_dp_1src : forall 'd ('data : Bool), (0 <= 'd & 'd <= 31).
  (int('d), bool('data)) -> unit

function execute_aarch64_instrs_integer_pac_strip_dp_1src (d, data) = {
    if HavePACExt() then {
        X_set(d, 64) = Strip(X_read(d, 64), data)
    };
    ()
}

val decode_xpac_aarch64_instrs_integer_pac_strip_dp_1src : (bits(5), bits(5), bits(1)) -> unit

function decode_xpac_aarch64_instrs_integer_pac_strip_dp_1src (Rd, Rn, D) = {
    let data : bool = D == 0b1;
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if not_bool(HavePACExt()) then {
        throw(Error_Undefined())
    };
    if n != 31 then {
        throw(Error_Undefined())
    };
    execute_aarch64_instrs_integer_pac_strip_dp_1src(d, data)
}

function clause __DecodeA64_DataProcReg ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, _, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _] as __opcode)) if SEE < 1032) = {
    SEE = 1032;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let D = Slice(__opcode, 10, 1);
    decode_xpac_aarch64_instrs_integer_pac_strip_dp_1src(Rd, Rn, D)
}

val decode_xpac_aarch64_instrs_integer_pac_strip_hint : unit -> unit

function decode_xpac_aarch64_instrs_integer_pac_strip_hint () = {
    let 'd = 30;
    let data : bool = false;
    execute_aarch64_instrs_integer_pac_strip_dp_1src(d, data)
}

function clause __DecodeA64_BranchExcSys ((pc, (0b11010101000000110010000011111111 as __opcode)) if SEE < 1033) = {
    SEE = 1033;
    decode_xpac_aarch64_instrs_integer_pac_strip_hint()
}

enum Feature = {
  FEAT_AA32EL0,
  FEAT_AA32EL1,
  FEAT_AA32EL2,
  FEAT_AA32EL3,
  FEAT_AA64EL0,
  FEAT_AA64EL1,
  FEAT_AA64EL2,
  FEAT_AA64EL3,
  FEAT_EL0,
  FEAT_EL1,
  FEAT_EL2,
  FEAT_EL3,
  FEAT_AES,
  FEAT_AdvSIMD,
  FEAT_CSV2_1p1,
  FEAT_CSV2_1p2,
  FEAT_CSV2_2,
  FEAT_CSV2_3,
  FEAT_DoubleLock,
  FEAT_ETMv4,
  FEAT_ETMv4p1,
  FEAT_ETMv4p2,
  FEAT_ETMv4p3,
  FEAT_ETMv4p4,
  FEAT_ETMv4p5,
  FEAT_ETMv4p6,
  FEAT_ETS2,
  FEAT_FP,
  FEAT_GICv3,
  FEAT_GICv3_LEGACY,
  FEAT_GICv3_TDIR,
  FEAT_GICv3p1,
  FEAT_GICv4,
  FEAT_GICv4p1,
  FEAT_IVIPT,
  FEAT_PCSRv8,
  FEAT_PMULL,
  FEAT_PMUv3,
  FEAT_PMUv3_EXT,
  FEAT_PMUv3_EXT32,
  FEAT_SHA1,
  FEAT_SHA256,
  FEAT_TRC_EXT,
  FEAT_TRC_SR,
  FEAT_nTLBPA,
  FEAT_CRC32,
  FEAT_Debugv8p1,
  FEAT_HAFDBS,
  FEAT_HPDS,
  FEAT_LOR,
  FEAT_LSE,
  FEAT_PAN,
  FEAT_PMUv3p1,
  FEAT_RDM,
  FEAT_VHE,
  FEAT_VMID16,
  FEAT_AA32BF16,
  FEAT_AA32HPD,
  FEAT_AA32I8MM,
  FEAT_ASMv8p2,
  FEAT_DPB,
  FEAT_Debugv8p2,
  FEAT_EDHSR,
  FEAT_F32MM,
  FEAT_F64MM,
  FEAT_FP16,
  FEAT_HPDS2,
  FEAT_I8MM,
  FEAT_IESB,
  FEAT_LPA,
  FEAT_LSMAOC,
  FEAT_LVA,
  FEAT_MPAM,
  FEAT_PAN2,
  FEAT_PCSRv8p2,
  FEAT_RAS,
  FEAT_SHA3,
  FEAT_SHA512,
  FEAT_SM3,
  FEAT_SM4,
  FEAT_SPE,
  FEAT_SVE,
  FEAT_TTCNP,
  FEAT_UAO,
  FEAT_VPIPT,
  FEAT_XNX,
  FEAT_CCIDX,
  FEAT_CONSTPACFIELD,
  FEAT_EPAC,
  FEAT_FCMA,
  FEAT_FPAC,
  FEAT_FPACCOMBINE,
  FEAT_JSCVT,
  FEAT_LRCPC,
  FEAT_NV,
  FEAT_PACIMP,
  FEAT_PACQARMA3,
  FEAT_PACQARMA5,
  FEAT_PAuth,
  FEAT_SPEv1p1,
  FEAT_AMUv1,
  FEAT_BBM,
  FEAT_CNTSC,
  FEAT_DIT,
  FEAT_Debugv8p4,
  FEAT_DotProd,
  FEAT_DoubleFault,
  FEAT_FHM,
  FEAT_FlagM,
  FEAT_IDST,
  FEAT_LRCPC2,
  FEAT_LSE2,
  FEAT_NV2,
  FEAT_PMUv3p4,
  FEAT_RASSAv1p1,
  FEAT_RASv1p1,
  FEAT_S2FWB,
  FEAT_SEL2,
  FEAT_TLBIOS,
  FEAT_TLBIRANGE,
  FEAT_TRF,
  FEAT_TTL,
  FEAT_TTST,
  FEAT_BTI,
  FEAT_CSV2,
  FEAT_CSV3,
  FEAT_DPB2,
  FEAT_E0PD,
  FEAT_EVT,
  FEAT_ExS,
  FEAT_FRINTTS,
  FEAT_FlagM2,
  FEAT_GTG,
  FEAT_MTE,
  FEAT_MTE2,
  FEAT_PMUv3p5,
  FEAT_RNG,
  FEAT_RNG_TRAP,
  FEAT_SB,
  FEAT_SPECRES,
  FEAT_SSBS,
  FEAT_SSBS2,
  FEAT_AMUv1p1,
  FEAT_BF16,
  FEAT_DGH,
  FEAT_ECV,
  FEAT_FGT,
  FEAT_HPMN0,
  FEAT_MPAMv0p1,
  FEAT_MPAMv1p1,
  FEAT_MTPMU,
  FEAT_PAuth2,
  FEAT_TWED,
  FEAT_AFP,
  FEAT_EBF16,
  FEAT_HCX,
  FEAT_LPA2,
  FEAT_LS64,
  FEAT_LS64_ACCDATA,
  FEAT_LS64_V,
  FEAT_MTE3,
  FEAT_PAN3,
  FEAT_PMUv3p7,
  FEAT_RPRES,
  FEAT_SPEv1p2,
  FEAT_WFxT,
  FEAT_XS,
  FEAT_CMOW,
  FEAT_Debugv8p8,
  FEAT_GICv3_NMI,
  FEAT_HBC,
  FEAT_MOPS,
  FEAT_NMI,
  FEAT_PMUv3_EXT64,
  FEAT_PMUv3_TH,
  FEAT_PMUv3p8,
  FEAT_SCTLR2,
  FEAT_SPEv1p3,
  FEAT_TCR2,
  FEAT_TIDCP1,
  FEAT_ADERR,
  FEAT_AIE,
  FEAT_ANERR,
  FEAT_CLRBHB,
  FEAT_CSSC,
  FEAT_Debugv8p9,
  FEAT_DoubleFault2,
  FEAT_ECBHB,
  FEAT_FGT2,
  FEAT_HAFT,
  FEAT_LRCPC3,
  FEAT_MTE4,
  FEAT_MTE_ASYM_FAULT,
  FEAT_MTE_ASYNC,
  FEAT_MTE_CANONICAL_TAGS,
  FEAT_MTE_NO_ADDRESS_TAGS,
  FEAT_MTE_PERM,
  FEAT_MTE_STORE_ONLY,
  FEAT_MTE_TAGGED_FAR,
  FEAT_PCSRv8p9,
  FEAT_PFAR,
  FEAT_PMUv3_EDGE,
  FEAT_PMUv3_ICNTR,
  FEAT_PMUv3_SS,
  FEAT_PMUv3p9,
  FEAT_PRFMSLC,
  FEAT_RASSAv2,
  FEAT_RASv2,
  FEAT_RPRFM,
  FEAT_S1PIE,
  FEAT_S1POE,
  FEAT_S2PIE,
  FEAT_S2POE,
  FEAT_SPECRES2,
  FEAT_SPE_CRR,
  FEAT_SPE_FDS,
  FEAT_SPEv1p4,
  FEAT_SPMU,
  FEAT_THE,
  FEAT_DoPD,
  FEAT_ETE,
  FEAT_SVE2,
  FEAT_SVE_AES,
  FEAT_SVE_BitPerm,
  FEAT_SVE_PMULL128,
  FEAT_SVE_SHA3,
  FEAT_SVE_SM4,
  FEAT_TME,
  FEAT_TRBE,
  FEAT_ETEv1p1,
  FEAT_BRBE,
  FEAT_ETEv1p2,
  FEAT_RME,
  FEAT_SME,
  FEAT_SME_F64F64,
  FEAT_SME_FA64,
  FEAT_SME_I16I64,
  FEAT_BRBEv1p1,
  FEAT_MEC,
  FEAT_SME2,
  FEAT_ABLE,
  FEAT_CHK,
  FEAT_D128,
  FEAT_EBEP,
  FEAT_ETEv1p3,
  FEAT_GCS,
  FEAT_ITE,
  FEAT_LSE128,
  FEAT_LVA3,
  FEAT_SEBEP,
  FEAT_SME2p1,
  FEAT_SME_F16F16,
  FEAT_SVE2p1,
  FEAT_SVE_B16B16,
  FEAT_SYSINSTR128,
  FEAT_SYSREG128,
  FEAT_TRBE_EXT,
  FEAT_TRBE_MPAM
}

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

enum DSBAlias = {DSBAlias_SSBB, DSBAlias_PSSBB, DSBAlias_DSB}

enum WFxType = {WFxType_WFE, WFxType_WFI, WFxType_WFET, WFxType_WFIT}

enum MBReqDomain = {
  MBReqDomain_Nonshareable,
  MBReqDomain_InnerShareable,
  MBReqDomain_OuterShareable,
  MBReqDomain_FullSystem
}

enum MBReqTypes = {MBReqTypes_Reads, MBReqTypes_Writes, MBReqTypes_All}

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

register BTypeNext : bits(2)

register BTypeCompatible : bool

enum GCSInstruction = {
  GCSInstType_PRET,
  GCSInstType_POPM,
  GCSInstType_PRETAA,
  GCSInstType_PRETAB,
  GCSInstType_SS1,
  GCSInstType_SS2,
  GCSInstType_POPCX,
  GCSInstType_POPX
}

enum TMFailure = {
  TMFailure_CNCL,
  TMFailure_DBG,
  TMFailure_ERR,
  TMFailure_NEST,
  TMFailure_SIZE,
  TMFailure_MEM,
  TMFailure_TRIVIAL,
  TMFailure_IMP
}

enum CountOp = {CountOp_CLZ, CountOp_CLS, CountOp_CNT}

enum ExtendType = {
  ExtendType_SXTB,
  ExtendType_SXTH,
  ExtendType_SXTW,
  ExtendType_SXTX,
  ExtendType_UXTB,
  ExtendType_UXTH,
  ExtendType_UXTW,
  ExtendType_UXTX
}

enum MoveWideOp = {MoveWideOp_N, MoveWideOp_Z, MoveWideOp_K}

enum ShiftType = {ShiftType_LSL, ShiftType_LSR, ShiftType_ASR, ShiftType_ROR}

enum LogicalOp = {LogicalOp_AND, LogicalOp_EOR, LogicalOp_ORR}

enum SystemHintOp = {
  SystemHintOp_NOP,
  SystemHintOp_YIELD,
  SystemHintOp_WFE,
  SystemHintOp_WFI,
  SystemHintOp_SEV,
  SystemHintOp_SEVL,
  SystemHintOp_DGH,
  SystemHintOp_ESB,
  SystemHintOp_PSB,
  SystemHintOp_TSB,
  SystemHintOp_BTI,
  SystemHintOp_WFET,
  SystemHintOp_WFIT,
  SystemHintOp_CLRBHB,
  SystemHintOp_GCSB,
  SystemHintOp_CHKFEAT,
  SystemHintOp_CSDB
}

val Unreachable : unit -> unit

function Unreachable () = {
    assert(false)
}

val HavePACExt : unit -> bool

function HavePACExt () = {
    return(IsFeatureImplemented(FEAT_PAuth))
}

val HaveTME : unit -> bool

function HaveTME () = {
    return(IsFeatureImplemented(FEAT_TME))
}

val PC_read : unit -> bits(64)

function PC_read () = {
    return(_PC)
}

val HaveBRBExt : unit -> bool

function HaveBRBExt () = {
    return(IsFeatureImplemented(FEAT_BRBE))
}

val HaveStatisticalProfiling : unit -> bool

function HaveStatisticalProfiling () = {
    return(IsFeatureImplemented(FEAT_SPE))
}

val SP_set : bits(64) -> unit

function SP_set value_name = {
    if PSTATE.SP == 0b0 then {
        SP_EL0 = value_name
    } else {
        match PSTATE.EL {
          ? if ? == EL0 => {
              SP_EL0 = value_name
          },
          ? if ? == EL1 => {
              SP_EL1 = value_name
          },
          ? if ? == EL2 => {
              SP_EL2 = value_name
          },
          ? if ? == EL3 => {
              SP_EL3 = value_name
          },
          _ => ()
        }
    };
    return()
}

val X_set : forall 'n 'width,
  'width in {32, 64}.
  (int('n), int('width), bits('width)) -> unit

function X_set (n, width, value_name) = {
    assert(n >= 0 & n <= 31);
    assert(width == 32 | width == 64);
    if n != 31 then {
        _R(n) = ZeroExtend(value_name, 64)
    };
    return()
}

val HaveBTIExt : unit -> bool

function HaveBTIExt () = {
    return(IsFeatureImplemented(FEAT_BTI))
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

val AArch64_ChkFeat : bits(64) -> bits(64)

function AArch64_ChkFeat feat_select = {
    feat_en : bits(64) = Zeros(64);
    feat_en[0] = Bit(if HaveGCS() & GCSEnabled(PSTATE.EL) then 0b1 else 0b0);
    return(feat_select & not_vec(feat_en))
}

val HaveCRCExt : unit -> bool

function HaveCRCExt () = {
    return(IsFeatureImplemented(FEAT_CRC32))
}

val BitReverse : forall 'N, 'N >= 0. bits('N) -> bits('N)

function BitReverse data = {
    result : bits('N) = undefined;
    foreach (i from 0 to ('N - 1) by 1 in inc) {
        result['N - i - 1] = Bit([data[i]])
    };
    return(result)
}

val Poly32Mod2 : forall 'N, 'N > 32. (bits('N), bits(32)) -> bits(32)

function Poly32Mod2 (data_in, poly) = {
    assert('N > 32);
    data : bits('N) = data_in;
    foreach (i from ('N - 1) to 32 by 1 in dec) {
        if [data[i]] == 0b1 then {
            data[i - 1 .. 0] = EOR(data[i - 1 .. 0], poly @ Zeros(i - 32))
        };
        ()
    };
    return(data[31 .. 0])
}

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

val HaveRASExt : unit -> bool

function HaveRASExt () = {
    return(IsFeatureImplemented(FEAT_RAS))
}

val HaveFlagManipulateExt : unit -> bool

function HaveFlagManipulateExt () = {
    return(IsFeatureImplemented(FEAT_FlagM))
}

val HaveSelfHostedTrace : unit -> bool

function HaveSelfHostedTrace () = {
    return(IsFeatureImplemented(FEAT_TRF))
}

val HaveDGHExt : unit -> bool

function HaveDGHExt () = {
    return(IsFeatureImplemented(FEAT_DGH))
}

val HaveFeatXS : unit -> bool

function HaveFeatXS () = {
    return(IsFeatureImplemented(FEAT_XS))
}

val HaveFeatHBC : unit -> bool

function HaveFeatHBC () = {
    return(IsFeatureImplemented(FEAT_HBC))
}

val HaveFeatCLRBHB : unit -> bool

function HaveFeatCLRBHB () = {
    return(IsFeatureImplemented(FEAT_CLRBHB))
}

val X_read : forall 'n 'width,
  'width in {8, 16, 32, 64}.
  (int('n), int('width)) -> bits('width)

function X_read (n, width) = {
    assert(n >= 0 & n <= 31);
    assert(width == 8 | width == 16 | width == 32 | width == 64);
    if n != 31 then {
        return(_R(n)[width - 1 .. 0])
    } else {
        return(Zeros(width))
    }
}

val BTypeCompatible_BTI : bits(2) -> bool

function BTypeCompatible_BTI hintcode = {
    match hintcode {
      0b00 => {
          return(false)
      },
      0b01 => {
          return(PSTATE.BTYPE != 0b11)
      },
      0b10 => {
          return(PSTATE.BTYPE != 0b10)
      },
      0b11 => {
          return(true)
      },
      _ => {
          return(undefined : bool)
      }
    }
}

val SetBTypeCompatible : forall ('x : Bool). bool('x) -> unit

function SetBTypeCompatible x = {
    BTypeCompatible = x
}

val SetBTypeNext : bits(2) -> unit

function SetBTypeNext x = {
    BTypeNext = x
}

val SP_read : unit -> bits(64)

function SP_read () = {
    if PSTATE.SP == 0b0 then {
        return(SP_EL0)
    } else {
        match PSTATE.EL {
          ? if ? == EL0 => {
              return(SP_EL0)
          },
          ? if ? == EL1 => {
              return(SP_EL1)
          },
          ? if ? == EL2 => {
              return(SP_EL2)
          },
          ? if ? == EL3 => {
              return(SP_EL3)
          },
          _ => {
              return(undefined : bits(64))
          }
        }
    }
}

val DecodeBitMasks : forall 'M ('immediate : Bool), 'M >= 0.
  (bits(1), bits(6), bits(6), bool('immediate), int('M)) -> (bits('M), bits('M))

function DecodeBitMasks (immN, imms, immr, immediate, M) = {
    let 'len = HighestSetBit(immN @ not_vec(imms));
    if len < 1 then {
        throw(Error_Undefined())
    };
    assert(M >= (1 << len));
    assert(constraint(6 >= 'len));
    let levels : bits(6) = ZeroExtend(Ones(len), 6);
    if immediate & (imms & levels) == levels then {
        throw(Error_Undefined())
    };
    let 's = UInt(imms & levels);
    let 'r = UInt(immr & levels);
    let 'diff = s - r;
    let 'esize = (1 << len);
    let 'd = UInt(diff[len - 1 .. 0]);
    assert(constraint('esize >= 's + 1));
    let welem : bits('esize) = ZeroExtend(Ones(s + 1), esize);
    assert(constraint('esize >= 'd + 1));
    let telem : bits('esize) = ZeroExtend(Ones(d + 1), esize);
    assert(constraint('esize * div('M, 'esize) == 'M));
    let wmask : bits('M) = Replicate(ROR(welem, r), DIV(M, esize));
    let tmask : bits('M) = Replicate(telem, DIV(M, esize));
    return((wmask, tmask))
}

val GCSSynchronizationBarrier : unit -> unit

function GCSSynchronizationBarrier () = {
    return()
}

val DecodeRegExtend : bits(3) -> ExtendType

function DecodeRegExtend op = {
    match op {
      0b000 => {
          return(ExtendType_UXTB)
      },
      0b001 => {
          return(ExtendType_UXTH)
      },
      0b010 => {
          return(ExtendType_UXTW)
      },
      0b011 => {
          return(ExtendType_UXTX)
      },
      0b100 => {
          return(ExtendType_SXTB)
      },
      0b101 => {
          return(ExtendType_SXTH)
      },
      0b110 => {
          return(ExtendType_SXTW)
      },
      0b111 => {
          return(ExtendType_SXTX)
      },
      _ => {
          return(undefined : ExtendType)
      }
    }
}

val ExtendReg : forall 'N 'reg 'shift,
  'N >= 0 & ('N == 8 | 'N == 16 | 'N == 32 | 'N == 64 | not(('shift >= 0 & 'shift <= 4))).
  (int('reg), ExtendType, int('shift), int('N)) -> bits('N)

function ExtendReg (reg, exttype, shift, N) = {
    assert(shift >= 0 & shift <= 4);
    let val_name : bits('N) = X_read(reg, N);
    is_unsigned : bool = undefined;
    len : int = undefined;
    match exttype {
      ExtendType_SXTB => {
          is_unsigned = false;
          len = 8
      },
      ExtendType_SXTH => {
          is_unsigned = false;
          len = 16
      },
      ExtendType_SXTW => {
          is_unsigned = false;
          len = 32
      },
      ExtendType_SXTX => {
          is_unsigned = false;
          len = 64
      },
      ExtendType_UXTB => {
          is_unsigned = true;
          len = 8
      },
      ExtendType_UXTH => {
          is_unsigned = true;
          len = 16
      },
      ExtendType_UXTW => {
          is_unsigned = true;
          len = 32
      },
      ExtendType_UXTX => {
          is_unsigned = true;
          len = 64
      }
    };
    let is_unsigned = is_unsigned;
    let 'len = Min(len, N - shift);
    assert(constraint((0 <= 'len - 1 | not(('shift >= 0 & 'shift <= 4)))));
    return(Extend(val_name[len - 1 .. 0] @ Zeros(shift), N, is_unsigned))
}

val DecodeShift : bits(2) -> ShiftType

function DecodeShift op = {
    match op {
      0b00 => {
          return(ShiftType_LSL)
      },
      0b01 => {
          return(ShiftType_LSR)
      },
      0b10 => {
          return(ShiftType_ASR)
      },
      0b11 => {
          return(ShiftType_ROR)
      },
      _ => {
          return(undefined : ShiftType)
      }
    }
}

val ShiftReg : forall 'N 'amount 'reg,
  'N in {8, 16, 32, 64}.
  (int('reg), ShiftType, int('amount), int('N)) -> bits('N)

function ShiftReg (reg, shiftype, amount, N) = {
    result : bits('N) = X_read(reg, N);
    match shiftype {
      ShiftType_LSL => {
          result = LSL(result, amount)
      },
      ShiftType_LSR => {
          result = LSR(result, amount)
      },
      ShiftType_ASR => {
          result = ASR(result, amount)
      },
      ShiftType_ROR => {
          assert(constraint(('N > 0 & 'amount >= 0)));
          result = ROR(result, amount)
      }
    };
    return(result)
}

