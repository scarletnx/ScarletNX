/* sail-arm's definitions that a64.roots reaches (sailgen --extract), as they are in:
     instrs64.sail
     v8_base.sail
   BSD 3-Clause Clear License: LICENSE beside this file. Do not edit: an NX fixup goes in nx.sail */

val execute_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_sisd : forall 'd 'datasize 'elements 'esize 'n ('neg : Bool),
  (0 <= 'n & 'n <= 31 & 'esize in {8, 16, 32, 64} & 'datasize in {8, 16, 32, 64, 128} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('elements), int('esize), int('n), bool('neg)) -> unit

function execute_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_sisd (d, datasize, elements, esize, n, neg) = {
    CheckFPAdvSIMDEnabled64();
    let operand : bits('datasize) = V_read(n, datasize);
    result : bits('datasize) = undefined;
    element : int = undefined;
    foreach (e from 0 to (elements - 1) by 1 in inc) {
        element = SInt(Elem_read(operand, e, esize));
        if neg then {
            element = negate(element)
        } else {
            element = Abs(element)
        };
        result = Elem_set(result, e, esize, element[esize - 1 .. 0])
    };
    V_set(d, datasize) = result
}

val decode_abs_advsimd_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_simd : (bits(5), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_abs_advsimd_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_simd (Rd, Rn, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let neg : bool = U == 0b1;
    execute_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_sisd(d, datasize, elements, esize, n, neg)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 3) = {
    SEE = 3;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_abs_advsimd_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_simd(Rd, Rn, size, U, Q)
}

val decode_neg_advsimd_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_simd : (bits(5), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_neg_advsimd_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_simd (Rd, Rn, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let neg : bool = U == 0b1;
    execute_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_sisd(d, datasize, elements, esize, n, neg)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 5) = {
    SEE = 5;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_neg_advsimd_aarch64_instrs_vector_arithmetic_unary_diff_neg_int_simd(Rd, Rn, size, U, Q)
}

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

val execute_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_sisd : forall 'd 'datasize 'elements 'esize 'm 'n ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {8, 16, 32, 64} & 'datasize in {8, 16, 32, 64, 128} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('elements), int('esize), int('m), int('n), bool('sub_op)) -> unit

function execute_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_sisd (d, datasize, elements, esize, m, n, sub_op) = {
    CheckFPAdvSIMDEnabled64();
    let operand1 : bits('datasize) = V_read(n, datasize);
    let operand2 : bits('datasize) = V_read(m, datasize);
    result : bits('datasize) = undefined;
    element1 : bits('esize) = undefined;
    element2 : bits('esize) = undefined;
    foreach (e from 0 to (elements - 1) by 1 in inc) {
        element1 = Elem_read(operand1, e, esize);
        element2 = Elem_read(operand2, e, esize);
        if sub_op then {
            result = Elem_set(result, e, esize, element1 - element2)
        } else {
            result = Elem_set(result, e, esize, element1 + element2)
        }
    };
    V_set(d, datasize) = result
}

val decode_add_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd : (bits(5), bits(5), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_add_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd (Rd, Rn, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let sub_op : bool = U == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_sisd(d, datasize, elements, esize, m, n, sub_op)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 11) = {
    SEE = 11;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_add_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd(Rd, Rn, Rm, size, U, Q)
}

val decode_sub_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd : (bits(5), bits(5), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_sub_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd (Rd, Rn, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let sub_op : bool = U == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_sisd(d, datasize, elements, esize, m, n, sub_op)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 13) = {
    SEE = 13;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_sub_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_add_wrapping_single_simd(Rd, Rn, Rm, size, U, Q)
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

val execute_aarch64_instrs_vector_cvt_bf16_scalar : forall 'd 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'd & 'd <= 31).
  (int('d), int('n)) -> unit

function execute_aarch64_instrs_vector_cvt_bf16_scalar (d, n) = {
    CheckFPEnabled64();
    let operand : bits(32) = V_read(n, 32);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(d, 128) else Zeros(128);
    result = Elem_set(result, 0, 16, FPConvertBF__1(operand, fpcr));
    V_set(d, 128) = result
}

val decode_bfcvt_float_aarch64_instrs_vector_cvt_bf16_scalar : (bits(5), bits(5)) -> unit

function decode_bfcvt_float_aarch64_instrs_vector_cvt_bf16_scalar (Rd, Rn) = {
    if not_bool(HaveBF16Ext()) then {
        throw(Error_Undefined())
    };
    let 'n = UInt(Rn);
    let 'd = UInt(Rd);
    execute_aarch64_instrs_vector_cvt_bf16_scalar(d, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 72) = {
    SEE = 72;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    decode_bfcvt_float_aarch64_instrs_vector_cvt_bf16_scalar(Rd, Rn)
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

val execute_aarch64_instrs_memory_atomicops_cas_single : forall ('acquire : Bool) 'datasize 'n 'regsize ('release : Bool) 's 't ('tagchecked : Bool),
  (0 <= 't & 't <= 31 & 0 <= 's & 's <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (bool('acquire), int('datasize), int('n), int('regsize), bool('release), int('s), int('t), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_atomicops_cas_single (acquire, datasize, n, regsize, release, s, t, tagchecked) = {
    address : bits(64) = undefined;
    let accdesc : AccessDescriptor = CreateAccDescAtomicOp(MemAtomicOp_CAS, acquire, release, tagchecked);
    let comparevalue : bits('datasize) = X_read(s, datasize);
    let newvalue : bits('datasize) = X_read(t, datasize);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    let data : bits('datasize) = MemAtomic(address, comparevalue, newvalue, accdesc);
    assert(constraint('regsize >= 'datasize));
    X_set(s, regsize) = ZeroExtend(data, regsize);
    if SPESampleInFlight then {
        let ar : bits(1) = if acquire | release then 0b1 else 0b0;
        let excl : bits(1) = 0b0;
        let at : bits(1) = 0b1;
        let is_load : bool = true;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_cas_aarch64_instrs_memory_atomicops_cas_single : (bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_cas_aarch64_instrs_memory_atomicops_cas_single (Rt, Rn, o0, Rs, L, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = L == 0b1;
    let release : bool = o0 == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_cas_single(acquire, datasize, n, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, _, bitone, _, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 113) = {
    SEE = 113;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    decode_cas_aarch64_instrs_memory_atomicops_cas_single(Rt, Rn, o0, Rs, L, size)
}

val decode_casb_aarch64_instrs_memory_atomicops_cas_single : (bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_casb_aarch64_instrs_memory_atomicops_cas_single (Rt, Rn, o0, Rs, L, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = L == 0b1;
    let release : bool = o0 == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_cas_single(acquire, datasize, n, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, _, bitone, _, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 114) = {
    SEE = 114;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    decode_casb_aarch64_instrs_memory_atomicops_cas_single(Rt, Rn, o0, Rs, L, size)
}

val decode_cash_aarch64_instrs_memory_atomicops_cas_single : (bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_cash_aarch64_instrs_memory_atomicops_cas_single (Rt, Rn, o0, Rs, L, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = L == 0b1;
    let release : bool = o0 == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_cas_single(acquire, datasize, n, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, _, bitone, _, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 115) = {
    SEE = 115;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    decode_cash_aarch64_instrs_memory_atomicops_cas_single(Rt, Rn, o0, Rs, L, size)
}

val execute_aarch64_instrs_memory_atomicops_cas_pair : forall ('acquire : Bool) 'datasize 'n ('release : Bool) 's 't ('tagchecked : Bool),
  (0 <= 't & 't <= 31 & 0 <= 's & 's <= 31 & 0 <= 'n & 'n <= 31 & 'datasize in {32, 64}).
  (bool('acquire), int('datasize), int('n), bool('release), int('s), int('t), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_atomicops_cas_pair (acquire, datasize, n, release, s, t, tagchecked) = {
    address : bits(64) = undefined;
    let s1 : bits('datasize) = X_read(s, datasize);
    let s2 : bits('datasize) = X_read(s + 1, datasize);
    let t1 : bits('datasize) = X_read(t, datasize);
    let t2 : bits('datasize) = X_read(t + 1, datasize);
    let accdesc : AccessDescriptor = CreateAccDescAtomicOp(MemAtomicOp_CAS, acquire, release, tagchecked);
    let comparevalue : bits(2 * 'datasize) = if BigEndian(accdesc.acctype) then
      s1 @ s2
    else
      s2 @ s1;
    let newvalue : bits(2 * 'datasize) = if BigEndian(accdesc.acctype) then
      t1 @ t2
    else
      t2 @ t1;
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    let data : bits(2 * 'datasize) = MemAtomic(address, comparevalue, newvalue, accdesc);
    if BigEndian(accdesc.acctype) then {
        X_set(s, datasize) = data[2 * datasize - 1 .. datasize];
        X_set(s + 1, datasize) = data[datasize - 1 .. 0]
    } else {
        X_set(s, datasize) = data[datasize - 1 .. 0];
        X_set(s + 1, datasize) = data[2 * datasize - 1 .. datasize]
    };
    if SPESampleInFlight then {
        let ar : bits(1) = if acquire | release then 0b1 else 0b0;
        let excl : bits(1) = 0b0;
        let at : bits(1) = 0b1;
        let is_load : bool = true;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_casp_aarch64_instrs_memory_atomicops_cas_pair : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(1)) -> unit

function decode_casp_aarch64_instrs_memory_atomicops_cas_pair (Rt, Rn, Rt2, o0, Rs, L, sz) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    if [Rs[0]] == 0b1 then {
        throw(Error_Undefined())
    };
    if [Rt[0]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'datasize = (32 << UInt(sz));
    let 'regsize = datasize;
    let acquire : bool = L == 0b1;
    let release : bool = o0 == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_cas_pair(acquire, datasize, n, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, bitone, _, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 116) = {
    SEE = 116;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let sz = Slice(__opcode, 30, 1);
    decode_casp_aarch64_instrs_memory_atomicops_cas_pair(Rt, Rn, Rt2, o0, Rs, L, sz)
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

val execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_sisd : forall ('and_test : Bool) 'd 'datasize 'elements 'esize 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {8, 16, 32, 64} & 'datasize in {8, 16, 32, 64, 128} & 0 <= 'd & 'd <= 31).
  (bool('and_test), int('d), int('datasize), int('elements), int('esize), int('m), int('n)) -> unit

function execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_sisd (and_test, d, datasize, elements, esize, m, n) = {
    CheckFPAdvSIMDEnabled64();
    let operand1 : bits('datasize) = V_read(n, datasize);
    let operand2 : bits('datasize) = V_read(m, datasize);
    result : bits('datasize) = undefined;
    element1 : bits('esize) = undefined;
    element2 : bits('esize) = undefined;
    test_passed : bool = undefined;
    foreach (e from 0 to (elements - 1) by 1 in inc) {
        element1 = Elem_read(operand1, e, esize);
        element2 = Elem_read(operand2, e, esize);
        if and_test then {
            test_passed = not_bool(IsZero(element1 & element2))
        } else {
            test_passed = element1 == element2
        };
        result = Elem_set(result, e, esize, if test_passed then Ones(esize) else
          Zeros(esize))
    };
    V_set(d, datasize) = result
}

val decode_cmeq_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_simd : (bits(5), bits(5), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_cmeq_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_simd (Rd, Rn, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let and_test : bool = U == 0b0;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_sisd(and_test, d, datasize, elements, esize, m, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 138) = {
    SEE = 138;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_cmeq_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_simd(Rd, Rn, Rm, size, U, Q)
}

val decode_cmtst_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_simd : (bits(5), bits(5), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_cmtst_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_simd (Rd, Rn, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let and_test : bool = U == 0b0;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_sisd(and_test, d, datasize, elements, esize, m, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 140) = {
    SEE = 140;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_cmtst_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_bitwise_simd(Rd, Rn, Rm, size, U, Q)
}

val execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_sisd : forall ('cmp_eq : Bool) 'd 'datasize 'elements 'esize ('is_unsigned : Bool) 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {8, 16, 32, 64} & 'datasize in {8, 16, 32, 64, 128} & 0 <= 'd & 'd <= 31).
  (bool('cmp_eq), int('d), int('datasize), int('elements), int('esize), int('m), int('n), bool('is_unsigned)) -> unit

function execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_sisd (cmp_eq, d, datasize, elements, esize, m, n, is_unsigned) = {
    CheckFPAdvSIMDEnabled64();
    let operand1 : bits('datasize) = V_read(n, datasize);
    let operand2 : bits('datasize) = V_read(m, datasize);
    result : bits('datasize) = undefined;
    element1 : int = undefined;
    element2 : int = undefined;
    test_passed : bool = undefined;
    foreach (e from 0 to (elements - 1) by 1 in inc) {
        element1 = asl_Int(Elem_read(operand1, e, esize), is_unsigned);
        element2 = asl_Int(Elem_read(operand2, e, esize), is_unsigned);
        test_passed = if cmp_eq then element1 >= element2 else
          element1 > element2;
        result = Elem_set(result, e, esize, if test_passed then Ones(esize) else
          Zeros(esize))
    };
    V_set(d, datasize) = result
}

val decode_cmge_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_cmge_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd (Rd, Rn, eq, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let cmp_eq : bool = eq == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_sisd(cmp_eq, d, datasize, elements, esize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 142) = {
    SEE = 142;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let eq = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_cmge_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd(Rd, Rn, eq, Rm, size, U, Q)
}

val decode_cmgt_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_cmgt_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd (Rd, Rn, eq, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let cmp_eq : bool = eq == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_sisd(cmp_eq, d, datasize, elements, esize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 144) = {
    SEE = 144;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let eq = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_cmgt_advsimd_reg_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd(Rd, Rn, eq, Rm, size, U, Q)
}

val decode_cmhi_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_cmhi_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd (Rd, Rn, eq, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let cmp_eq : bool = eq == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_sisd(cmp_eq, d, datasize, elements, esize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 146) = {
    SEE = 146;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let eq = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_cmhi_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd(Rd, Rn, eq, Rm, size, U, Q)
}

val decode_cmhs_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_cmhs_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd (Rd, Rn, eq, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if (size @ Q) == 0b110 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let cmp_eq : bool = eq == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_sisd(cmp_eq, d, datasize, elements, esize, m, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 148) = {
    SEE = 148;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let eq = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_cmhs_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_cmp_int_simd(Rd, Rn, eq, Rm, size, U, Q)
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

val execute_aarch64_instrs_float_arithmetic_unary : forall 'd 'esize 'n,
  (0 <= 'n & 'n <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), FPUnaryOp, int('n)) -> unit

function execute_aarch64_instrs_float_arithmetic_unary (d, esize, fpop, n) = {
    CheckFPEnabled64();
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = fpop != FPUnaryOp_MOV & IsMerging(fpcr);
    result : bits(128) = if merge then V_read(d, 128) else 0[127 .. 0];
    let operand : bits('esize) = V_read(n, esize);
    match fpop {
      FPUnaryOp_MOV => {
          result = Elem_set(result, 0, esize, operand)
      },
      FPUnaryOp_ABS => {
          result = Elem_set(result, 0, esize, FPAbs(operand))
      },
      FPUnaryOp_NEG => {
          result = Elem_set(result, 0, esize, FPNeg(operand))
      },
      FPUnaryOp_SQRT => {
          result = Elem_set(result, 0, esize, FPSqrt(operand, fpcr))
      }
    };
    V_set(d, 128) = result
}

val decode_fabs_float_aarch64_instrs_float_arithmetic_unary : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_fabs_float_aarch64_instrs_float_arithmetic_unary (Rd, Rn, opc, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    fpop : FPUnaryOp = undefined;
    match opc {
      0b00 => {
          fpop = FPUnaryOp_MOV
      },
      0b01 => {
          fpop = FPUnaryOp_ABS
      },
      0b10 => {
          fpop = FPUnaryOp_NEG
      },
      0b11 => {
          fpop = FPUnaryOp_SQRT
      }
    };
    let fpop = fpop;
    execute_aarch64_instrs_float_arithmetic_unary(d, esize, fpop, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 211) = {
    SEE = 211;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_fabs_float_aarch64_instrs_float_arithmetic_unary(Rd, Rn, opc, ftype)
}

val decode_fmov_float_aarch64_instrs_float_arithmetic_unary : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_fmov_float_aarch64_instrs_float_arithmetic_unary (Rd, Rn, opc, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    fpop : FPUnaryOp = undefined;
    match opc {
      0b00 => {
          fpop = FPUnaryOp_MOV
      },
      0b01 => {
          fpop = FPUnaryOp_ABS
      },
      0b10 => {
          fpop = FPUnaryOp_NEG
      },
      0b11 => {
          fpop = FPUnaryOp_SQRT
      }
    };
    let fpop = fpop;
    execute_aarch64_instrs_float_arithmetic_unary(d, esize, fpop, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 212) = {
    SEE = 212;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmov_float_aarch64_instrs_float_arithmetic_unary(Rd, Rn, opc, ftype)
}

val decode_fneg_float_aarch64_instrs_float_arithmetic_unary : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_fneg_float_aarch64_instrs_float_arithmetic_unary (Rd, Rn, opc, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    fpop : FPUnaryOp = undefined;
    match opc {
      0b00 => {
          fpop = FPUnaryOp_MOV
      },
      0b01 => {
          fpop = FPUnaryOp_ABS
      },
      0b10 => {
          fpop = FPUnaryOp_NEG
      },
      0b11 => {
          fpop = FPUnaryOp_SQRT
      }
    };
    let fpop = fpop;
    execute_aarch64_instrs_float_arithmetic_unary(d, esize, fpop, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 213) = {
    SEE = 213;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_fneg_float_aarch64_instrs_float_arithmetic_unary(Rd, Rn, opc, ftype)
}

val decode_fsqrt_float_aarch64_instrs_float_arithmetic_unary : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_fsqrt_float_aarch64_instrs_float_arithmetic_unary (Rd, Rn, opc, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    fpop : FPUnaryOp = undefined;
    match opc {
      0b00 => {
          fpop = FPUnaryOp_MOV
      },
      0b01 => {
          fpop = FPUnaryOp_ABS
      },
      0b10 => {
          fpop = FPUnaryOp_NEG
      },
      0b11 => {
          fpop = FPUnaryOp_SQRT
      }
    };
    let fpop = fpop;
    execute_aarch64_instrs_float_arithmetic_unary(d, esize, fpop, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 214) = {
    SEE = 214;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_fsqrt_float_aarch64_instrs_float_arithmetic_unary(Rd, Rn, opc, ftype)
}

val execute_aarch64_instrs_float_arithmetic_add_sub : forall 'd 'esize 'm 'n ('sub_op : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), int('m), int('n), bool('sub_op)) -> unit

function execute_aarch64_instrs_float_arithmetic_add_sub (d, esize, m, n, sub_op) = {
    CheckFPEnabled64();
    let operand1 : bits('esize) = V_read(n, esize);
    let operand2 : bits('esize) = V_read(m, esize);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(n, 128) else Zeros(128);
    if sub_op then {
        result = Elem_set(result, 0, esize, FPSub(operand1, operand2, fpcr))
    } else {
        result = Elem_set(result, 0, esize, FPAdd(operand1, operand2, fpcr))
    };
    V_set(d, 128) = result
}

val decode_fadd_float_aarch64_instrs_float_arithmetic_add_sub : (bits(5), bits(5), bits(1), bits(5), bits(2)) -> unit

function decode_fadd_float_aarch64_instrs_float_arithmetic_add_sub (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let sub_op : bool = op == 0b1;
    execute_aarch64_instrs_float_arithmetic_add_sub(d, esize, m, n, sub_op)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 243) = {
    SEE = 243;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 12, 1);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fadd_float_aarch64_instrs_float_arithmetic_add_sub(Rd, Rn, op, Rm, ftype)
}

val decode_fsub_float_aarch64_instrs_float_arithmetic_add_sub : (bits(5), bits(5), bits(1), bits(5), bits(2)) -> unit

function decode_fsub_float_aarch64_instrs_float_arithmetic_add_sub (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let sub_op : bool = op == 0b1;
    execute_aarch64_instrs_float_arithmetic_add_sub(d, esize, m, n, sub_op)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 244) = {
    SEE = 244;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 12, 1);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fsub_float_aarch64_instrs_float_arithmetic_add_sub(Rd, Rn, op, Rm, ftype)
}

val execute_aarch64_instrs_float_compare_cond : forall 'datasize 'm 'n ('signal_all_nans : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {16, 32, 64}).
  (bits(4), int('datasize), bits(4), int('m), int('n), bool('signal_all_nans)) -> unit

function execute_aarch64_instrs_float_compare_cond (condition, datasize, flags__arg, m, n, signal_all_nans) = {
    flags : bits(4) = flags__arg;
    CheckFPEnabled64();
    let operand1 : bits('datasize) = V_read(n, datasize);
    let operand2 : bits('datasize) = V_read(m, datasize);
    taken : bool = false;
    if ConditionHolds(condition) then {
        flags = FPCompare(operand1, operand2, signal_all_nans, FPCR_read());
        taken = true
    };
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = flags;
    if SPESampleInFlight then {
        let conditional : bool = true;
        SPESampleAddOpOther(conditional, taken)
    };
    ()
}

val decode_fccmp_float_aarch64_instrs_float_compare_cond : (bits(4), bits(1), bits(5), bits(4), bits(5), bits(2)) -> unit

function decode_fccmp_float_aarch64_instrs_float_compare_cond (nzcv, op, Rn, cond, Rm, ftype) = {
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    datasize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          datasize = 32
      },
      0b01 => {
          datasize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              datasize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'datasize = datasize;
    let signal_all_nans : bool = op == 0b1;
    let condition : bits(4) = cond;
    let flags : bits(4) = nzcv;
    execute_aarch64_instrs_float_compare_cond(condition, datasize, flags, m, n, signal_all_nans)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, bitzero, _, _, _, _] as __opcode)) if SEE < 248) = {
    SEE = 248;
    let nzcv = Slice(__opcode, 0, 4);
    let op = Slice(__opcode, 4, 1);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fccmp_float_aarch64_instrs_float_compare_cond(nzcv, op, Rn, cond, Rm, ftype)
}

val decode_fccmpe_float_aarch64_instrs_float_compare_cond : (bits(4), bits(1), bits(5), bits(4), bits(5), bits(2)) -> unit

function decode_fccmpe_float_aarch64_instrs_float_compare_cond (nzcv, op, Rn, cond, Rm, ftype) = {
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    datasize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          datasize = 32
      },
      0b01 => {
          datasize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              datasize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'datasize = datasize;
    let signal_all_nans : bool = op == 0b1;
    let condition : bits(4) = cond;
    let flags : bits(4) = nzcv;
    execute_aarch64_instrs_float_compare_cond(condition, datasize, flags, m, n, signal_all_nans)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, bitone, _, _, _, _] as __opcode)) if SEE < 249) = {
    SEE = 249;
    let nzcv = Slice(__opcode, 0, 4);
    let op = Slice(__opcode, 4, 1);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fccmpe_float_aarch64_instrs_float_compare_cond(nzcv, op, Rn, cond, Rm, ftype)
}

val execute_aarch64_instrs_float_compare_uncond : forall ('cmp_with_zero : Bool) 'datasize 'm 'n ('signal_all_nans : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {16, 32, 64}).
  (bool('cmp_with_zero), int('datasize), int('m), int('n), bool('signal_all_nans)) -> unit

function execute_aarch64_instrs_float_compare_uncond (cmp_with_zero, datasize, m, n, signal_all_nans) = {
    CheckFPEnabled64();
    let operand1 : bits('datasize) = V_read(n, datasize);
    let operand2 : bits('datasize) = if cmp_with_zero then FPZero(0b0, datasize)
    else
      V_read(m, datasize);
    (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = FPCompare(operand1, operand2, signal_all_nans, FPCR_read())
}

val decode_fcmp_float_aarch64_instrs_float_compare_uncond : (bits(2), bits(5), bits(5), bits(2)) -> unit

function decode_fcmp_float_aarch64_instrs_float_compare_uncond (opc, Rn, Rm, ftype) = {
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    datasize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          datasize = 32
      },
      0b01 => {
          datasize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              datasize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'datasize = datasize;
    let signal_all_nans : bool = [opc[1]] == 0b1;
    let cmp_with_zero : bool = [opc[0]] == 0b1;
    execute_aarch64_instrs_float_compare_uncond(cmp_with_zero, datasize, m, n, signal_all_nans)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, _, bitzero, bitzero, bitzero] as __opcode)) if SEE < 272) = {
    SEE = 272;
    let opc = Slice(__opcode, 3, 2);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fcmp_float_aarch64_instrs_float_compare_uncond(opc, Rn, Rm, ftype)
}

val decode_fcmpe_float_aarch64_instrs_float_compare_uncond : (bits(2), bits(5), bits(5), bits(2)) -> unit

function decode_fcmpe_float_aarch64_instrs_float_compare_uncond (opc, Rn, Rm, ftype) = {
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    datasize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          datasize = 32
      },
      0b01 => {
          datasize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              datasize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'datasize = datasize;
    let signal_all_nans : bool = [opc[1]] == 0b1;
    let cmp_with_zero : bool = [opc[0]] == 0b1;
    execute_aarch64_instrs_float_compare_uncond(cmp_with_zero, datasize, m, n, signal_all_nans)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, bitone, _, bitzero, bitzero, bitzero] as __opcode)) if SEE < 273) = {
    SEE = 273;
    let opc = Slice(__opcode, 3, 2);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fcmpe_float_aarch64_instrs_float_compare_uncond(opc, Rn, Rm, ftype)
}

val execute_aarch64_instrs_float_move_fp_select : forall 'd 'datasize 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (bits(4), int('d), int('datasize), int('m), int('n)) -> unit

function execute_aarch64_instrs_float_move_fp_select (condition, d, datasize, m, n) = {
    CheckFPEnabled64();
    let result : bits('datasize) = if ConditionHolds(condition) then
      V_read(n, datasize)
    else
      V_read(m, datasize);
    V_set(d, datasize) = result
}

val decode_fcsel_float_aarch64_instrs_float_move_fp_select : (bits(5), bits(5), bits(4), bits(5), bits(2)) -> unit

function decode_fcsel_float_aarch64_instrs_float_move_fp_select (Rd, Rn, cond, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    datasize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          datasize = 32
      },
      0b01 => {
          datasize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              datasize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'datasize = datasize;
    let condition : bits(4) = cond;
    execute_aarch64_instrs_float_move_fp_select(condition, d, datasize, m, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 274) = {
    SEE = 274;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let cond = Slice(__opcode, 12, 4);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fcsel_float_aarch64_instrs_float_move_fp_select(Rd, Rn, cond, Rm, ftype)
}

val execute_aarch64_instrs_float_convert_fp : forall 'd 'dstsize 'n 'srcsize,
  ('srcsize in {16, 32, 64} & 0 <= 'n & 'n <= 31 & 'dstsize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('dstsize), int('n), int('srcsize)) -> unit

function execute_aarch64_instrs_float_convert_fp (d, dstsize, n, srcsize) = {
    CheckFPEnabled64();
    let operand : bits('srcsize) = V_read(n, srcsize);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(d, 128) else Zeros(128);
    result = Elem_set(result, 0, dstsize, FPConvert__1(operand, fpcr, dstsize));
    V_set(d, 128) = result
}

val decode_fcvt_float_aarch64_instrs_float_convert_fp : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_fcvt_float_aarch64_instrs_float_convert_fp (Rd, Rn, opc, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    srcsize : {16, 32, 64} = 16;
    dstsize : {16, 32, 64} = 16;
    if ftype == opc then {
        throw(Error_Undefined())
    };
    match ftype {
      0b00 => {
          srcsize = 32
      },
      0b01 => {
          srcsize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          srcsize = 16
      }
    };
    let 'srcsize = srcsize;
    match opc {
      0b00 => {
          dstsize = 32
      },
      0b01 => {
          dstsize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          dstsize = 16
      }
    };
    let 'dstsize = dstsize;
    execute_aarch64_instrs_float_convert_fp(d, dstsize, n, srcsize)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitone, _, _, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 275) = {
    SEE = 275;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_fcvt_float_aarch64_instrs_float_convert_fp(Rd, Rn, opc, ftype)
}

val execute_aarch64_instrs_float_convert_int : forall 'd 'fltsize 'intsize ('is_unsigned : Bool) 'n 'part,
  ('part in {0, 1} & 0 <= 'n & 'n <= 31 & 'intsize in {32, 64} & 'fltsize in {16, 32, 64, 128} & 0 <= 'd & 'd <= 31).
  (int('d), int('fltsize), int('intsize), int('n), FPConvOp, int('part), FPRounding, bool('is_unsigned)) -> unit

function execute_aarch64_instrs_float_convert_int (d, fltsize, intsize, n, op, part, rounding, is_unsigned) = {
    if op == FPConvOp_CVT_FtoI_JS then {
        CheckFPAdvSIMDEnabled64()
    } else {
        CheckFPEnabled64()
    };
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    let 'fsize = if op == FPConvOp_CVT_ItoF & merge then 128 else fltsize;
    fltval : bits('fsize) = undefined;
    intval : bits('intsize) = undefined;
    match op {
      FPConvOp_CVT_FtoI => {
          let fltval : bits('fsize) = V_read(n, fsize);
          assert(constraint(('fsize in {16, 32, 64} & 'intsize in {16, 32, 64})));
          let intval : bits('intsize) = FPToFixed(fltval, 0, is_unsigned, fpcr, rounding, intsize);
          X_set(d, intsize) = intval
      },
      FPConvOp_CVT_ItoF => {
          let intval : bits('intsize) = X_read(n, intsize);
          fltval : bits('fsize) = if merge then V_read(d, fsize) else
            Zeros(fsize);
          assert(constraint(('fltsize in {16, 32, 64} & 'intsize in {16, 32, 64})));
          fltval = Elem_set(fltval, 0, fltsize, FixedToFP(intval, 0, is_unsigned, fpcr, rounding, fltsize));
          V_set(d, fsize) = fltval
      },
      FPConvOp_MOV_FtoI => {
          let fltval : bits('fsize) = Vpart_read(n, part, fsize);
          assert(constraint(('fsize >= 0 & 'intsize >= 'fsize)));
          let intval : bits('intsize) = ZeroExtend(fltval, intsize);
          X_set(d, intsize) = intval
      },
      FPConvOp_MOV_ItoF => {
          let intval : bits('intsize) = X_read(n, intsize);
          assert(constraint((0 <= 'fsize - 1 & 'fsize - 1 < 'intsize)));
          let fltval : bits('fsize) = intval[fsize - 1 .. 0];
          Vpart_set(d, part, fsize) = fltval
      },
      FPConvOp_CVT_FtoI_JS => {
          z : bits(1) = undefined;
          let fltval : bits('fsize) = V_read(n, fsize);
          assert(constraint('fsize == 64));
          assert(constraint('intsize == 32));
          (intval, z) = FPToFixedJS(fltval, fpcr, true, intsize);
          (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V) = (0b0 @ z) @ 0b00;
          X_set(d, intsize) = intval
      }
    }
}

val decode_fcvtas_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtas_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 284) = {
    SEE = 284;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtas_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtau_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtau_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 285) = {
    SEE = 285;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtau_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtms_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtms_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 286) = {
    SEE = 286;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtms_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtmu_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtmu_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 287) = {
    SEE = 287;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtmu_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtns_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtns_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 288) = {
    SEE = 288;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtns_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtnu_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtnu_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 289) = {
    SEE = 289;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtnu_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtps_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtps_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 290) = {
    SEE = 290;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtps_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtpu_float_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtpu_float_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 291) = {
    SEE = 291;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtpu_float_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtzs_float_int_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtzs_float_int_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 292) = {
    SEE = 292;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtzs_float_int_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fcvtzu_float_int_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtzu_float_int_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 293) = {
    SEE = 293;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtzu_float_int_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fjcvtzs_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fjcvtzs_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 294) = {
    SEE = 294;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fjcvtzs_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_fmov_float_gen_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fmov_float_gen_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, _, bitone, bitone, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 295) = {
    SEE = 295;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fmov_float_gen_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_scvtf_float_int_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_scvtf_float_int_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 296) = {
    SEE = 296;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_scvtf_float_int_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val decode_ucvtf_float_int_aarch64_instrs_float_convert_int : (bits(5), bits(5), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_ucvtf_float_int_aarch64_instrs_float_convert_int (Rd, Rn, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64, 128} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    part : {0, 1} = 0;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          if (opcode[2 .. 1] @ rmode) != 0b1101 then {
              throw(Error_Undefined())
          };
          fltsize = 128
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    match opcode[2 .. 1] @ rmode {
      [bitzero, bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode);
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      0b1000 => {
          rounding = FPRounding_TIEAWAY;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b1100 => {
          if fltsize != 16 & fltsize != intsize then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 0
      },
      0b1101 => {
          if intsize != 64 | fltsize != 128 then {
              throw(Error_Undefined())
          };
          op = if [opcode[0]] == 0b1 then FPConvOp_MOV_ItoF else
            FPConvOp_MOV_FtoI;
          part = 1;
          fltsize = 64
      },
      0b1111 => {
          if not_bool(HaveFJCVTZSExt()) then {
              throw(Error_Undefined())
          };
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI_JS
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    let op = op;
    let 'part = part;
    let 'fltsize = fltsize;
    match fltsize {
      16 => {
          execute_aarch64_instrs_float_convert_int(d, 16, intsize, n, op, part, rounding, is_unsigned)
      },
      32 => {
          execute_aarch64_instrs_float_convert_int(d, 32, intsize, n, op, part, rounding, is_unsigned)
      },
      64 => {
          execute_aarch64_instrs_float_convert_int(d, 64, intsize, n, op, part, rounding, is_unsigned)
      },
      128 => {
          execute_aarch64_instrs_float_convert_int(d, 128, intsize, n, op, part, rounding, is_unsigned)
      },
      _ => {
          assert(false)
      }
    }
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 297) = {
    SEE = 297;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_ucvtf_float_int_aarch64_instrs_float_convert_int(Rd, Rn, opcode, rmode, ftype, sf)
}

val execute_aarch64_instrs_float_convert_fix : forall 'd 'fltsize 'fracbits 'intsize ('is_unsigned : Bool) 'n,
  (0 <= 'n & 'n <= 31 & 'intsize in {32, 64} & 1 <= 'fracbits & 'fracbits <= 64 & 'fltsize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('fltsize), int('fracbits), int('intsize), int('n), FPConvOp, FPRounding, bool('is_unsigned)) -> unit

function execute_aarch64_instrs_float_convert_fix (d, fltsize, fracbits, intsize, n, op, rounding, is_unsigned) = {
    CheckFPEnabled64();
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    let 'fsize = if op == FPConvOp_CVT_ItoF & merge then 128 else fltsize;
    fltval : bits('fsize) = undefined;
    intval : bits('intsize) = undefined;
    match op {
      FPConvOp_CVT_FtoI => {
          let fltval : bits('fsize) = V_read(n, fsize);
          assert(constraint(('fsize in {16, 32, 64} & 'intsize in {16, 32, 64})));
          let intval : bits('intsize) = FPToFixed(fltval, fracbits, is_unsigned, fpcr, rounding, intsize);
          X_set(d, intsize) = intval
      },
      FPConvOp_CVT_ItoF => {
          let intval : bits('intsize) = X_read(n, intsize);
          fltval : bits('fsize) = if merge then V_read(d, fsize) else
            Zeros(fsize);
          fltval = Elem_set(fltval, 0, fltsize, FixedToFP(intval, fracbits, is_unsigned, fpcr, rounding, fltsize));
          V_set(d, fsize) = fltval
      },
      _ => ()
    }
}

val decode_fcvtzs_float_fix_aarch64_instrs_float_convert_fix : (bits(5), bits(5), bits(6), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtzs_float_fix_aarch64_instrs_float_convert_fix (Rd, Rn, scale, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'fltsize = fltsize;
    if sf == 0b0 & [scale[5]] == 0b0 then {
        throw(Error_Undefined())
    };
    let 'fracbits = 64 - UInt(scale);
    match opcode[2 .. 1] @ rmode {
      0b0011 => {
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    execute_aarch64_instrs_float_convert_fix(d, fltsize, fracbits, intsize, n, op, rounding, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 334) = {
    SEE = 334;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let scale = Slice(__opcode, 10, 6);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtzs_float_fix_aarch64_instrs_float_convert_fix(Rd, Rn, scale, opcode, rmode, ftype, sf)
}

val decode_fcvtzu_float_fix_aarch64_instrs_float_convert_fix : (bits(5), bits(5), bits(6), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_fcvtzu_float_fix_aarch64_instrs_float_convert_fix (Rd, Rn, scale, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'fltsize = fltsize;
    if sf == 0b0 & [scale[5]] == 0b0 then {
        throw(Error_Undefined())
    };
    let 'fracbits = 64 - UInt(scale);
    match opcode[2 .. 1] @ rmode {
      0b0011 => {
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    execute_aarch64_instrs_float_convert_fix(d, fltsize, fracbits, intsize, n, op, rounding, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 335) = {
    SEE = 335;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let scale = Slice(__opcode, 10, 6);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_fcvtzu_float_fix_aarch64_instrs_float_convert_fix(Rd, Rn, scale, opcode, rmode, ftype, sf)
}

val decode_scvtf_float_fix_aarch64_instrs_float_convert_fix : (bits(5), bits(5), bits(6), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_scvtf_float_fix_aarch64_instrs_float_convert_fix (Rd, Rn, scale, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'fltsize = fltsize;
    if sf == 0b0 & [scale[5]] == 0b0 then {
        throw(Error_Undefined())
    };
    let 'fracbits = 64 - UInt(scale);
    match opcode[2 .. 1] @ rmode {
      0b0011 => {
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    execute_aarch64_instrs_float_convert_fix(d, fltsize, fracbits, intsize, n, op, rounding, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 336) = {
    SEE = 336;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let scale = Slice(__opcode, 10, 6);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_scvtf_float_fix_aarch64_instrs_float_convert_fix(Rd, Rn, scale, opcode, rmode, ftype, sf)
}

val decode_ucvtf_float_fix_aarch64_instrs_float_convert_fix : (bits(5), bits(5), bits(6), bits(3), bits(2), bits(2), bits(1)) -> unit

function decode_ucvtf_float_fix_aarch64_instrs_float_convert_fix (Rd, Rn, scale, opcode, rmode, ftype, sf) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'intsize = if sf == 0b1 then 64 else 32;
    fltsize : {16, 32, 64} = 16;
    op : FPConvOp = undefined;
    rounding : FPRounding = undefined;
    is_unsigned : bool = undefined;
    match ftype {
      0b00 => {
          fltsize = 32
      },
      0b01 => {
          fltsize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              fltsize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'fltsize = fltsize;
    if sf == 0b0 & [scale[5]] == 0b0 then {
        throw(Error_Undefined())
    };
    let 'fracbits = 64 - UInt(scale);
    match opcode[2 .. 1] @ rmode {
      0b0011 => {
          rounding = FPRounding_ZERO;
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_FtoI
      },
      0b0100 => {
          rounding = FPRoundingMode(FPCR_read());
          is_unsigned = [opcode[0]] == 0b1;
          op = FPConvOp_CVT_ItoF
      },
      _ => {
          throw(Error_Undefined())
      }
    };
    execute_aarch64_instrs_float_convert_fix(d, fltsize, fracbits, intsize, n, op, rounding, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([_, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 337) = {
    SEE = 337;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let scale = Slice(__opcode, 10, 6);
    let opcode = Slice(__opcode, 16, 3);
    let rmode = Slice(__opcode, 19, 2);
    let ftype = Slice(__opcode, 22, 2);
    let sf = Slice(__opcode, 31, 1);
    decode_ucvtf_float_fix_aarch64_instrs_float_convert_fix(Rd, Rn, scale, opcode, rmode, ftype, sf)
}

val execute_aarch64_instrs_float_arithmetic_div : forall 'd 'esize 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), int('m), int('n)) -> unit

function execute_aarch64_instrs_float_arithmetic_div (d, esize, m, n) = {
    CheckFPEnabled64();
    let operand1 : bits('esize) = V_read(n, esize);
    let operand2 : bits('esize) = V_read(m, esize);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(n, 128) else Zeros(128);
    result = Elem_set(result, 0, esize, FPDiv(operand1, operand2, FPCR_read()));
    V_set(d, 128) = result
}

val decode_fdiv_float_aarch64_instrs_float_arithmetic_div : (bits(5), bits(5), bits(5), bits(2)) -> unit

function decode_fdiv_float_aarch64_instrs_float_arithmetic_div (Rd, Rn, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    execute_aarch64_instrs_float_arithmetic_div(d, esize, m, n)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 344) = {
    SEE = 344;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fdiv_float_aarch64_instrs_float_arithmetic_div(Rd, Rn, Rm, ftype)
}

val execute_aarch64_instrs_float_arithmetic_mul_add_sub : forall 'a 'd 'esize 'm 'n ('op1_neg : Bool) ('opa_neg : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31 & 0 <= 'a & 'a <= 31).
  (int('a), int('d), int('esize), int('m), int('n), bool('op1_neg), bool('opa_neg)) -> unit

function execute_aarch64_instrs_float_arithmetic_mul_add_sub (a, d, esize, m, n, op1_neg, opa_neg) = {
    CheckFPEnabled64();
    operanda : bits('esize) = V_read(a, esize);
    operand1 : bits('esize) = V_read(n, esize);
    let operand2 : bits('esize) = V_read(m, esize);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(a, 128) else Zeros(128);
    if opa_neg then {
        operanda = FPNeg(operanda)
    };
    if op1_neg then {
        operand1 = FPNeg(operand1)
    };
    result = Elem_set(result, 0, esize, FPMulAdd(operanda, operand1, operand2, fpcr));
    V_set(d, 128) = result
}

val decode_fmadd_float_aarch64_instrs_float_arithmetic_mul_add_sub : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_fmadd_float_aarch64_instrs_float_arithmetic_mul_add_sub (Rd, Rn, Ra, o0, Rm, o1, ftype) = {
    let 'd = UInt(Rd);
    let 'a = UInt(Ra);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let opa_neg : bool = o1 == 0b1;
    let op1_neg : bool = o0 != o1;
    execute_aarch64_instrs_float_arithmetic_mul_add_sub(a, d, esize, m, n, op1_neg, opa_neg)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, bitzero, _, _, _, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 345) = {
    SEE = 345;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let o1 = Slice(__opcode, 21, 1);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmadd_float_aarch64_instrs_float_arithmetic_mul_add_sub(Rd, Rn, Ra, o0, Rm, o1, ftype)
}

val decode_fmsub_float_aarch64_instrs_float_arithmetic_mul_add_sub : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_fmsub_float_aarch64_instrs_float_arithmetic_mul_add_sub (Rd, Rn, Ra, o0, Rm, o1, ftype) = {
    let 'd = UInt(Rd);
    let 'a = UInt(Ra);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let opa_neg : bool = o1 == 0b1;
    let op1_neg : bool = o0 != o1;
    execute_aarch64_instrs_float_arithmetic_mul_add_sub(a, d, esize, m, n, op1_neg, opa_neg)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, bitzero, _, _, _, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 346) = {
    SEE = 346;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let o1 = Slice(__opcode, 21, 1);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmsub_float_aarch64_instrs_float_arithmetic_mul_add_sub(Rd, Rn, Ra, o0, Rm, o1, ftype)
}

val decode_fnmadd_float_aarch64_instrs_float_arithmetic_mul_add_sub : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_fnmadd_float_aarch64_instrs_float_arithmetic_mul_add_sub (Rd, Rn, Ra, o0, Rm, o1, ftype) = {
    let 'd = UInt(Rd);
    let 'a = UInt(Ra);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let opa_neg : bool = o1 == 0b1;
    let op1_neg : bool = o0 != o1;
    execute_aarch64_instrs_float_arithmetic_mul_add_sub(a, d, esize, m, n, op1_neg, opa_neg)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, bitone, _, _, _, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 347) = {
    SEE = 347;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let o1 = Slice(__opcode, 21, 1);
    let ftype = Slice(__opcode, 22, 2);
    decode_fnmadd_float_aarch64_instrs_float_arithmetic_mul_add_sub(Rd, Rn, Ra, o0, Rm, o1, ftype)
}

val decode_fnmsub_float_aarch64_instrs_float_arithmetic_mul_add_sub : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_fnmsub_float_aarch64_instrs_float_arithmetic_mul_add_sub (Rd, Rn, Ra, o0, Rm, o1, ftype) = {
    let 'd = UInt(Rd);
    let 'a = UInt(Ra);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let opa_neg : bool = o1 == 0b1;
    let op1_neg : bool = o0 != o1;
    execute_aarch64_instrs_float_arithmetic_mul_add_sub(a, d, esize, m, n, op1_neg, opa_neg)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, bitone, _, _, _, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 348) = {
    SEE = 348;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Ra = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let o1 = Slice(__opcode, 21, 1);
    let ftype = Slice(__opcode, 22, 2);
    decode_fnmsub_float_aarch64_instrs_float_arithmetic_mul_add_sub(Rd, Rn, Ra, o0, Rm, o1, ftype)
}

val execute_aarch64_instrs_float_arithmetic_max_min : forall 'd 'esize 'm 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), int('m), int('n), FPMaxMinOp) -> unit

function execute_aarch64_instrs_float_arithmetic_max_min (d, esize, m, n, operation) = {
    CheckFPEnabled64();
    let operand1 : bits('esize) = V_read(n, esize);
    let operand2 : bits('esize) = V_read(m, esize);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(n, 128) else Zeros(128);
    match operation {
      FPMaxMinOp_MAX => {
          result = Elem_set(result, 0, esize, FPMax(operand1, operand2, fpcr))
      },
      FPMaxMinOp_MIN => {
          result = Elem_set(result, 0, esize, FPMin(operand1, operand2, fpcr))
      },
      FPMaxMinOp_MAXNUM => {
          result = Elem_set(result, 0, esize, FPMaxNum(operand1, operand2, fpcr))
      },
      FPMaxMinOp_MINNUM => {
          result = Elem_set(result, 0, esize, FPMinNum(operand1, operand2, fpcr))
      }
    };
    V_set(d, 128) = result
}

val decode_fmax_float_aarch64_instrs_float_arithmetic_max_min : (bits(5), bits(5), bits(2), bits(5), bits(2)) -> unit

function decode_fmax_float_aarch64_instrs_float_arithmetic_max_min (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    operation : FPMaxMinOp = undefined;
    match op {
      0b00 => {
          operation = FPMaxMinOp_MAX
      },
      0b01 => {
          operation = FPMaxMinOp_MIN
      },
      0b10 => {
          operation = FPMaxMinOp_MAXNUM
      },
      0b11 => {
          operation = FPMaxMinOp_MINNUM
      }
    };
    let operation = operation;
    execute_aarch64_instrs_float_arithmetic_max_min(d, esize, m, n, operation)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 349) = {
    SEE = 349;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 12, 2);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmax_float_aarch64_instrs_float_arithmetic_max_min(Rd, Rn, op, Rm, ftype)
}

val decode_fmaxnm_float_aarch64_instrs_float_arithmetic_max_min : (bits(5), bits(5), bits(2), bits(5), bits(2)) -> unit

function decode_fmaxnm_float_aarch64_instrs_float_arithmetic_max_min (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    operation : FPMaxMinOp = undefined;
    match op {
      0b00 => {
          operation = FPMaxMinOp_MAX
      },
      0b01 => {
          operation = FPMaxMinOp_MIN
      },
      0b10 => {
          operation = FPMaxMinOp_MAXNUM
      },
      0b11 => {
          operation = FPMaxMinOp_MINNUM
      }
    };
    let operation = operation;
    execute_aarch64_instrs_float_arithmetic_max_min(d, esize, m, n, operation)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 350) = {
    SEE = 350;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 12, 2);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmaxnm_float_aarch64_instrs_float_arithmetic_max_min(Rd, Rn, op, Rm, ftype)
}

val decode_fmin_float_aarch64_instrs_float_arithmetic_max_min : (bits(5), bits(5), bits(2), bits(5), bits(2)) -> unit

function decode_fmin_float_aarch64_instrs_float_arithmetic_max_min (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    operation : FPMaxMinOp = undefined;
    match op {
      0b00 => {
          operation = FPMaxMinOp_MAX
      },
      0b01 => {
          operation = FPMaxMinOp_MIN
      },
      0b10 => {
          operation = FPMaxMinOp_MAXNUM
      },
      0b11 => {
          operation = FPMaxMinOp_MINNUM
      }
    };
    let operation = operation;
    execute_aarch64_instrs_float_arithmetic_max_min(d, esize, m, n, operation)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 351) = {
    SEE = 351;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 12, 2);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmin_float_aarch64_instrs_float_arithmetic_max_min(Rd, Rn, op, Rm, ftype)
}

val decode_fminnm_float_aarch64_instrs_float_arithmetic_max_min : (bits(5), bits(5), bits(2), bits(5), bits(2)) -> unit

function decode_fminnm_float_aarch64_instrs_float_arithmetic_max_min (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    operation : FPMaxMinOp = undefined;
    match op {
      0b00 => {
          operation = FPMaxMinOp_MAX
      },
      0b01 => {
          operation = FPMaxMinOp_MIN
      },
      0b10 => {
          operation = FPMaxMinOp_MAXNUM
      },
      0b11 => {
          operation = FPMaxMinOp_MINNUM
      }
    };
    let operation = operation;
    execute_aarch64_instrs_float_arithmetic_max_min(d, esize, m, n, operation)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 352) = {
    SEE = 352;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 12, 2);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fminnm_float_aarch64_instrs_float_arithmetic_max_min(Rd, Rn, op, Rm, ftype)
}

val execute_aarch64_instrs_float_move_fp_imm : forall 'd 'datasize,
  ('datasize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), bits('datasize)) -> unit

function execute_aarch64_instrs_float_move_fp_imm (d, datasize, imm) = {
    CheckFPEnabled64();
    V_set(d, datasize) = imm
}

val decode_fmov_float_imm_aarch64_instrs_float_move_fp_imm : (bits(5), bits(8), bits(2)) -> unit

function decode_fmov_float_imm_aarch64_instrs_float_move_fp_imm (Rd, imm8, ftype) = {
    let 'd = UInt(Rd);
    datasize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          datasize = 32
      },
      0b01 => {
          datasize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              datasize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'datasize = datasize;
    let imm : bits('datasize) = VFPExpandImm(imm8, datasize);
    execute_aarch64_instrs_float_move_fp_imm(d, datasize, imm)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _] as __opcode)) if SEE < 407) = {
    SEE = 407;
    let Rd = Slice(__opcode, 0, 5);
    let imm8 = Slice(__opcode, 13, 8);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmov_float_imm_aarch64_instrs_float_move_fp_imm(Rd, imm8, ftype)
}

val execute_aarch64_instrs_float_arithmetic_mul_product : forall 'd 'esize 'm 'n ('negated : Bool),
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), int('m), int('n), bool('negated)) -> unit

function execute_aarch64_instrs_float_arithmetic_mul_product (d, esize, m, n, negated) = {
    CheckFPEnabled64();
    let operand1 : bits('esize) = V_read(n, esize);
    let operand2 : bits('esize) = V_read(m, esize);
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(n, 128) else Zeros(128);
    product : bits('esize) = FPMul(operand1, operand2, fpcr);
    if negated then {
        product = FPNeg(product)
    };
    result = Elem_set(result, 0, esize, product);
    V_set(d, 128) = result
}

val decode_fmul_float_aarch64_instrs_float_arithmetic_mul_product : (bits(5), bits(5), bits(1), bits(5), bits(2)) -> unit

function decode_fmul_float_aarch64_instrs_float_arithmetic_mul_product (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let negated : bool = op == 0b1;
    execute_aarch64_instrs_float_arithmetic_mul_product(d, esize, m, n, negated)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 410) = {
    SEE = 410;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fmul_float_aarch64_instrs_float_arithmetic_mul_product(Rd, Rn, op, Rm, ftype)
}

val decode_fnmul_float_aarch64_instrs_float_arithmetic_mul_product : (bits(5), bits(5), bits(1), bits(5), bits(2)) -> unit

function decode_fnmul_float_aarch64_instrs_float_arithmetic_mul_product (Rd, Rn, op, Rm, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    let negated : bool = op == 0b1;
    execute_aarch64_instrs_float_arithmetic_mul_product(d, esize, m, n, negated)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 411) = {
    SEE = 411;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 15, 1);
    let Rm = Slice(__opcode, 16, 5);
    let ftype = Slice(__opcode, 22, 2);
    decode_fnmul_float_aarch64_instrs_float_arithmetic_mul_product(Rd, Rn, op, Rm, ftype)
}

val execute_aarch64_instrs_float_arithmetic_round_frint_32_64 : forall 'd 'esize 'intsize 'n,
  (0 <= 'n & 'n <= 31 & 'intsize in {32, 64} & 'esize in {32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), int('intsize), int('n), FPRounding) -> unit

function execute_aarch64_instrs_float_arithmetic_round_frint_32_64 (d, esize, intsize, n, rounding) = {
    CheckFPEnabled64();
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(d, 128) else Zeros(128);
    let operand : bits('esize) = V_read(n, esize);
    result = Elem_set(result, 0, esize, FPRoundIntN(operand, fpcr, rounding, intsize));
    V_set(d, 128) = result
}

val decode_frint32x_float_aarch64_instrs_float_arithmetic_round_frint_32_64 : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_frint32x_float_aarch64_instrs_float_arithmetic_round_frint_32_64 (Rd, Rn, op, ftype) = {
    if not_bool(HaveFrintExt()) then {
        throw(Error_Undefined())
    };
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {32, 64} = 32;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      [bitone, _] => {
          throw(Error_Undefined())
      }
    };
    let 'esize = esize;
    let 'intsize = if [op[1]] == 0b0 then 32 else 64;
    let rounding : FPRounding = if [op[0]] == 0b0 then FPRounding_ZERO else
      FPRoundingMode(FPCR_read());
    execute_aarch64_instrs_float_arithmetic_round_frint_32_64(d, esize, intsize, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, _, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 438) = {
    SEE = 438;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_frint32x_float_aarch64_instrs_float_arithmetic_round_frint_32_64(Rd, Rn, op, ftype)
}

val decode_frint32z_float_aarch64_instrs_float_arithmetic_round_frint_32_64 : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_frint32z_float_aarch64_instrs_float_arithmetic_round_frint_32_64 (Rd, Rn, op, ftype) = {
    if not_bool(HaveFrintExt()) then {
        throw(Error_Undefined())
    };
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {32, 64} = 32;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      [bitone, _] => {
          throw(Error_Undefined())
      }
    };
    let 'esize = esize;
    let 'intsize = if [op[1]] == 0b0 then 32 else 64;
    let rounding : FPRounding = if [op[0]] == 0b0 then FPRounding_ZERO else
      FPRoundingMode(FPCR_read());
    execute_aarch64_instrs_float_arithmetic_round_frint_32_64(d, esize, intsize, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, _, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 439) = {
    SEE = 439;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_frint32z_float_aarch64_instrs_float_arithmetic_round_frint_32_64(Rd, Rn, op, ftype)
}

val decode_frint64x_float_aarch64_instrs_float_arithmetic_round_frint_32_64 : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_frint64x_float_aarch64_instrs_float_arithmetic_round_frint_32_64 (Rd, Rn, op, ftype) = {
    if not_bool(HaveFrintExt()) then {
        throw(Error_Undefined())
    };
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {32, 64} = 32;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      [bitone, _] => {
          throw(Error_Undefined())
      }
    };
    let 'esize = esize;
    let 'intsize = if [op[1]] == 0b0 then 32 else 64;
    let rounding : FPRounding = if [op[0]] == 0b0 then FPRounding_ZERO else
      FPRoundingMode(FPCR_read());
    execute_aarch64_instrs_float_arithmetic_round_frint_32_64(d, esize, intsize, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, _, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 440) = {
    SEE = 440;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_frint64x_float_aarch64_instrs_float_arithmetic_round_frint_32_64(Rd, Rn, op, ftype)
}

val decode_frint64z_float_aarch64_instrs_float_arithmetic_round_frint_32_64 : (bits(5), bits(5), bits(2), bits(2)) -> unit

function decode_frint64z_float_aarch64_instrs_float_arithmetic_round_frint_32_64 (Rd, Rn, op, ftype) = {
    if not_bool(HaveFrintExt()) then {
        throw(Error_Undefined())
    };
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {32, 64} = 32;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      [bitone, _] => {
          throw(Error_Undefined())
      }
    };
    let 'esize = esize;
    let 'intsize = if [op[1]] == 0b0 then 32 else 64;
    let rounding : FPRounding = if [op[0]] == 0b0 then FPRounding_ZERO else
      FPRoundingMode(FPCR_read());
    execute_aarch64_instrs_float_arithmetic_round_frint_32_64(d, esize, intsize, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, _, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 441) = {
    SEE = 441;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let op = Slice(__opcode, 15, 2);
    let ftype = Slice(__opcode, 22, 2);
    decode_frint64z_float_aarch64_instrs_float_arithmetic_round_frint_32_64(Rd, Rn, op, ftype)
}

val execute_aarch64_instrs_float_arithmetic_round_frint : forall 'd 'esize ('exact : Bool) 'n,
  (0 <= 'n & 'n <= 31 & 'esize in {16, 32, 64} & 0 <= 'd & 'd <= 31).
  (int('d), int('esize), bool('exact), int('n), FPRounding) -> unit

function execute_aarch64_instrs_float_arithmetic_round_frint (d, esize, exact, n, rounding) = {
    CheckFPEnabled64();
    let fpcr : FPCRType = FPCR_read();
    let merge : bool = IsMerging(fpcr);
    result : bits(128) = if merge then V_read(d, 128) else Zeros(128);
    let operand : bits('esize) = V_read(n, esize);
    result = Elem_set(result, 0, esize, FPRoundInt(operand, fpcr, rounding, exact));
    V_set(d, 128) = result
}

val decode_frinta_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frinta_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 456) = {
    SEE = 456;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frinta_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
}

val decode_frinti_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frinti_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 457) = {
    SEE = 457;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frinti_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
}

val decode_frintm_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frintm_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 458) = {
    SEE = 458;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frintm_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
}

val decode_frintn_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frintn_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 459) = {
    SEE = 459;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frintn_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
}

val decode_frintp_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frintp_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 460) = {
    SEE = 460;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frintp_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
}

val decode_frintx_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frintx_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 461) = {
    SEE = 461;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frintx_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
}

val decode_frintz_float_aarch64_instrs_float_arithmetic_round_frint : (bits(5), bits(5), bits(3), bits(2)) -> unit

function decode_frintz_float_aarch64_instrs_float_arithmetic_round_frint (Rd, Rn, rmode, ftype) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    esize : {16, 32, 64} = 16;
    match ftype {
      0b00 => {
          esize = 32
      },
      0b01 => {
          esize = 64
      },
      0b10 => {
          throw(Error_Undefined())
      },
      0b11 => {
          if HaveFP16Ext() then {
              esize = 16
          } else {
              throw(Error_Undefined())
          }
      }
    };
    let 'esize = esize;
    exact : bool = false;
    rounding : FPRounding = undefined;
    match rmode {
      [bitzero, _, _] => {
          rounding = FPDecodeRounding(rmode[1 .. 0])
      },
      0b100 => {
          rounding = FPRounding_TIEAWAY
      },
      0b101 => {
          throw(Error_Undefined())
      },
      0b110 => {
          rounding = FPRoundingMode(FPCR_read());
          exact = true
      },
      0b111 => {
          rounding = FPRoundingMode(FPCR_read())
      }
    };
    let rounding = rounding;
    execute_aarch64_instrs_float_arithmetic_round_frint(d, esize, exact, n, rounding)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitzero, _, _, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 462) = {
    SEE = 462;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let rmode = Slice(__opcode, 15, 3);
    let ftype = Slice(__opcode, 22, 2);
    decode_frintz_float_aarch64_instrs_float_arithmetic_round_frint(Rd, Rn, rmode, ftype)
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

val execute_aarch64_instrs_memory_atomicops_ld : forall ('acquire : Bool) 'datasize ('is_load : Bool) 'n 'regsize ('release : Bool) 's 't ('tagchecked : Bool),
  (0 <= 't & 't <= 31 & 0 <= 's & 's <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (bool('acquire), int('datasize), bool('is_load), int('n), MemAtomicOp, int('regsize), bool('release), int('s), int('t), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_atomicops_ld (acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked) = {
    address : bits(64) = undefined;
    let accdesc : AccessDescriptor = CreateAccDescAtomicOp(op, acquire, release, tagchecked);
    let value_name : bits('datasize) = X_read(s, datasize);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    let comparevalue : bits('datasize) = __UNKNOWN_bits(datasize);
    let data : bits('datasize) = MemAtomic(address, comparevalue, value_name, accdesc);
    if t != 31 then {
        assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
        X_set(t, regsize) = ZeroExtend(data, regsize)
    };
    if SPESampleInFlight then {
        let ar : bits(1) = if acquire | release then 0b1 else 0b0;
        let excl : bits(1) = 0b0;
        let at : bits(1) = 0b1;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_ldadd_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldadd_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 525) = {
    SEE = 525;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldadd_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldaddb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldaddb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 526) = {
    SEE = 526;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldaddb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldaddh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldaddh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 527) = {
    SEE = 527;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldaddh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldclr_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldclr_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 528) = {
    SEE = 528;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldclr_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldclrb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldclrb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 529) = {
    SEE = 529;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldclrb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldclrh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldclrh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 530) = {
    SEE = 530;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldclrh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldeor_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldeor_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 531) = {
    SEE = 531;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldeor_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldeorb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldeorb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 532) = {
    SEE = 532;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldeorb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldeorh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldeorh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 533) = {
    SEE = 533;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldeorh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldset_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldset_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 534) = {
    SEE = 534;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldset_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsetb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsetb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 535) = {
    SEE = 535;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsetb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldseth_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldseth_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 536) = {
    SEE = 536;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldseth_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsmax_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsmax_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 537) = {
    SEE = 537;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsmax_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsmaxb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsmaxb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 538) = {
    SEE = 538;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsmaxb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsmaxh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsmaxh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 539) = {
    SEE = 539;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsmaxh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsmin_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsmin_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 540) = {
    SEE = 540;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsmin_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsminb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsminb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 541) = {
    SEE = 541;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsminb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldsminh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldsminh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 542) = {
    SEE = 542;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldsminh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldumax_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldumax_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 543) = {
    SEE = 543;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldumax_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldumaxb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldumaxb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 544) = {
    SEE = 544;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldumaxb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldumaxh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldumaxh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 545) = {
    SEE = 545;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldumaxh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_ldumin_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_ldumin_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 546) = {
    SEE = 546;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_ldumin_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_lduminb_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_lduminb_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 547) = {
    SEE = 547;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_lduminb_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val decode_lduminh_aarch64_instrs_memory_atomicops_ld : (bits(5), bits(5), bits(3), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_lduminh_aarch64_instrs_memory_atomicops_ld (Rt, Rn, opc, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let is_load : bool = Rt != 0b11111;
    let release : bool = R == 0b1;
    op : MemAtomicOp = undefined;
    match opc {
      0b000 => {
          op = MemAtomicOp_ADD
      },
      0b001 => {
          op = MemAtomicOp_BIC
      },
      0b010 => {
          op = MemAtomicOp_EOR
      },
      0b011 => {
          op = MemAtomicOp_ORR
      },
      0b100 => {
          op = MemAtomicOp_SMAX
      },
      0b101 => {
          op = MemAtomicOp_SMIN
      },
      0b110 => {
          op = MemAtomicOp_UMAX
      },
      0b111 => {
          op = MemAtomicOp_UMIN
      }
    };
    let op = op;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_ld(acquire, datasize, is_load, n, op, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 548) = {
    SEE = 548;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let opc = Slice(__opcode, 12, 3);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_lduminh_aarch64_instrs_memory_atomicops_ld(Rt, Rn, opc, Rs, R, A, size)
}

val execute_aarch64_instrs_memory_ordered_rcpc : forall 'datasize 'n 'offset 'regsize 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 'offset in {0, 1, 2, 4, 8} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), int('n), int('offset), int('regsize), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_ordered_rcpc (datasize, n, offset, regsize, t, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    let 'dbytes = DIV(datasize, 8);
    let accdesc : AccessDescriptor = CreateAccDescLDAcqPC(tagchecked);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    let data : bits('datasize) = Mem_read(address, dbytes, accdesc);
    assert(constraint('regsize >= 'datasize));
    X_set(t, regsize) = ZeroExtend(data, regsize);
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else {
            address = address + offset
        };
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        let ar : bits(1) = 0b1;
        let excl : bits(1) = 0b0;
        let at : bits(1) = 0b0;
        let is_load : bool = true;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_ldapr_aarch64_instrs_memory_ordered_rcpc : (bits(5), bits(5), bits(5), bits(2)) -> unit

function decode_ldapr_aarch64_instrs_memory_ordered_rcpc (Rt, Rn, Rs, size) = {
    let wback : bool = false;
    let 'offset = 0;
    let wb_unknown : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered_rcpc(datasize, n, offset, regsize, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 551) = {
    SEE = 551;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rs = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldapr_aarch64_instrs_memory_ordered_rcpc(Rt, Rn, Rs, size)
    }
}

val decode_ldaprb_aarch64_instrs_memory_ordered_rcpc : (bits(5), bits(5), bits(5), bits(2)) -> unit

function decode_ldaprb_aarch64_instrs_memory_ordered_rcpc (Rt, Rn, Rs, size) = {
    let wback : bool = false;
    let 'offset = 0;
    let wb_unknown : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered_rcpc(datasize, n, offset, regsize, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 553) = {
    SEE = 553;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rs = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldaprb_aarch64_instrs_memory_ordered_rcpc(Rt, Rn, Rs, size)
    }
}

val decode_ldaprh_aarch64_instrs_memory_ordered_rcpc : (bits(5), bits(5), bits(5), bits(2)) -> unit

function decode_ldaprh_aarch64_instrs_memory_ordered_rcpc (Rt, Rn, Rs, size) = {
    let wback : bool = false;
    let 'offset = 0;
    let wb_unknown : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 's = UInt(Rs);
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered_rcpc(datasize, n, offset, regsize, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 554) = {
    SEE = 554;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rs = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldaprh_aarch64_instrs_memory_ordered_rcpc(Rt, Rn, Rs, size)
    }
}

val execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : forall 'datasize ('is_signed : Bool) 'n ('postindex : Bool) 'regsize ('rt_unknown : Bool) 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), MemOp, int('n), bits(64), bool('postindex), int('regsize), bool('rt_unknown), bool('is_signed), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    accdesc : AccessDescriptor = undefined;
    if memop == MemOp_LOAD then {
        accdesc = CreateAccDescLDAcqPC(tagchecked)
    } else if memop == MemOp_STORE then {
        accdesc = CreateAccDescAcqRel(memop, tagchecked)
    } else ();
    if n == 31 then {
        if memop != MemOp_PREFETCH then {
            CheckSPAlignment()
        };
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), false, t, regsize == 64, false)
          };
          Mem_set(address, DIV(datasize, 8), accdesc) = data
      },
      MemOp_LOAD => {
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), is_signed, t, regsize == 64, false)
          };
          let data : bits('datasize) = Mem_read(address, DIV(datasize, 8), accdesc);
          if is_signed then {
              assert(constraint(('datasize > 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = SignExtend(data, regsize)
          } else {
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      }
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldapur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldapur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 555) = {
    SEE = 555;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldapur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_ldapurb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldapurb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 556) = {
    SEE = 556;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldapurb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_ldapurh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldapurh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 557) = {
    SEE = 557;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldapurh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_ldapursb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldapursb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 558) = {
    SEE = 558;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldapursb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_ldapursh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldapursh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 559) = {
    SEE = 559;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldapursh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_ldapursw_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldapursw_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 560) = {
    SEE = 560;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldapursw_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_stlur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_stlur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 561) = {
    SEE = 561;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_stlur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_stlurb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_stlurb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 562) = {
    SEE = 562;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_stlurb_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val decode_stlurh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_stlurh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(datasize, memop, n, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 563) = {
    SEE = 563;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_stlurh_aarch64_instrs_memory_single_general_immediate_signed_offset_lda_stl(Rt, Rn, imm9, opc, size)
}

val execute_aarch64_instrs_memory_ordered : forall 'datasize ('limitedordered : Bool) 'n 'offset 'regsize ('rt_unknown : Bool) 't ('tagchecked : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), bool('limitedordered), MemOp, int('n), int('offset), int('regsize), bool('rt_unknown), int('t), bool('tagchecked), bool('wback)) -> unit

function execute_aarch64_instrs_memory_ordered (datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let 'dbytes = DIV(datasize, 8);
    accdesc : AccessDescriptor = undefined;
    if limitedordered then {
        accdesc = CreateAccDescLOR(memop, tagchecked)
    } else {
        accdesc = CreateAccDescAcqRel(memop, tagchecked)
    };
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    match memop {
      MemOp_STORE => {
          let address : bits(64) = address + offset;
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          AArch64_SetLSInstructionSyndrome(dbytes, false, t, regsize == 64, true);
          Mem_set(address, dbytes, accdesc) = data;
          if wback then {
              if n == 31 then {
                  SP_set() = address
              } else {
                  X_set(n, 64) = address
              }
          };
          ()
      },
      MemOp_LOAD => {
          AArch64_SetLSInstructionSyndrome(dbytes, false, t, regsize == 64, true);
          let data : bits('datasize) = Mem_read(address, dbytes, accdesc);
          assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
          X_set(t, regsize) = ZeroExtend(data, regsize)
      },
      _ => ()
    };
    if SPESampleInFlight then {
        let ar : bits(1) = 0b1;
        let excl : bits(1) = 0b0;
        let at : bits(1) = 0b0;
        let is_load : bool = memop == MemOp_LOAD;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_ldar_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldar_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 566) = {
    SEE = 566;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldar_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldarb_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldarb_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 567) = {
    SEE = 567;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldarb_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldarh_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldarh_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 568) = {
    SEE = 568;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldarh_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldlar_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldlar_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 569) = {
    SEE = 569;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldlar_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldlarb_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldlarb_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 570) = {
    SEE = 570;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldlarb_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldlarh_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldlarh_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 571) = {
    SEE = 571;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldlarh_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stllr_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stllr_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 572) = {
    SEE = 572;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stllr_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stllrb_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stllrb_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 573) = {
    SEE = 573;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stllrb_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stllrh_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stllrh_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 574) = {
    SEE = 574;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stllrh_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stlr_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stlr_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 575) = {
    SEE = 575;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stlr_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stlrb_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stlrb_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 577) = {
    SEE = 577;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stlrb_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stlrh_aarch64_instrs_memory_ordered : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stlrh_aarch64_instrs_memory_ordered (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let wback : bool = false;
    let 'offset = 0;
    let rt_unknown : bool = false;
    let limitedordered : bool = o0 == 0b0;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = elsize;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_ordered(datasize, limitedordered, memop, n, offset, regsize, rt_unknown, t, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 578) = {
    SEE = 578;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stlrh_aarch64_instrs_memory_ordered(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val execute_aarch64_instrs_memory_exclusive_pair : forall ('acqrel : Bool) 'datasize 'elsize 'n ('pair : Bool) 'regsize ('rn_unknown : Bool) ('rt_unknown : Bool) 's 't 't2 ('tagchecked : Bool),
  (0 <= 't2 & 't2 <= 31 & 0 <= 't & 't <= 31 & 0 <= 's & 's <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'elsize in {32, 64} & 'datasize in {32, 64, 128}).
  (bool('acqrel), int('datasize), int('elsize), MemOp, int('n), bool('pair), int('regsize), bool('rn_unknown), bool('rt_unknown), int('s), int('t), int('t2), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_exclusive_pair (acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let 'dbytes = DIV(datasize, 8);
    let accdesc : AccessDescriptor = CreateAccDescExLDST(memop, acqrel, tagchecked);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else if rn_unknown then {
        address = __UNKNOWN_bits(64)
    } else {
        address = X_read(n, 64)
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else if pair then {
              let el1 : bits(div('datasize, 2)) = X_read(t, DIV(datasize, 2));
              let el2 : bits(div('datasize, 2)) = X_read(t2, DIV(datasize, 2));
              data = if BigEndian(accdesc.acctype) then el1 @ el2 else el2 @ el1
          } else {
              assert(constraint('datasize in {8, 16, 32, 64}));
              data = X_read(t, datasize)
          };
          status : bits(1) = 0b1;
          if AArch64_ExclusiveMonitorsPass(address, dbytes) then {
              Mem_set(address, dbytes, accdesc) = data;
              status = ExclusiveMonitorsStatus()
          } else if SPESampleInFlight then {
              SPESetDataVirtualAddress(address)
          } else ();
          X_set(s, 32) = ZeroExtend(status, 32)
      },
      MemOp_LOAD => {
          AArch64_SetExclusiveMonitors(address, dbytes);
          if pair then {
              if rt_unknown then {
                  assert(constraint('datasize in {32, 64}));
                  X_set(t, datasize) = __UNKNOWN_bits(datasize)
              } else if elsize == 32 then {
                  let data : bits('datasize) = Mem_read(address, dbytes, accdesc);
                  if BigEndian(accdesc.acctype) then {
                      assert(constraint((0 <= 'elsize & 'elsize <= 'datasize - 1 & 'datasize - 1 < 'datasize)));
                      assert(constraint(('datasize - 'elsize == 32 | 'datasize - 'elsize == 64)));
                      X_set(t, datasize - elsize) = data[datasize - 1 .. elsize];
                      X_set(t2, elsize) = data[elsize - 1 .. 0]
                  } else {
                      X_set(t, elsize) = data[elsize - 1 .. 0];
                      assert(constraint((0 <= 'elsize & 'elsize <= 'datasize - 1 & 'datasize - 1 < 'datasize)));
                      assert(constraint(('datasize - 'elsize == 32 | 'datasize - 'elsize == 64)));
                      X_set(t2, datasize - elsize) = data[datasize - 1 .. elsize]
                  }
              } else {
                  if not_bool(IsAligned__1(address, dbytes)) then {
                      AArch64_Abort(address, AlignmentFault(accdesc))
                  };
                  X_set(t, 64) = Mem_read(address + 0, 8, accdesc);
                  X_set(t2, 64) = Mem_read(address + 8, 8, accdesc)
              }
          } else {
              let data : bits('datasize) = Mem_read(address, dbytes, accdesc);
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      _ => ()
    };
    if SPESampleInFlight then {
        let ar : bits(1) = if acqrel then 0b1 else 0b0;
        let excl : bits(1) = 0b1;
        let at : bits(1) = 0b0;
        let is_load : bool = memop == MemOp_LOAD;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_ldaxp_aarch64_instrs_memory_exclusive_pair : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(1)) -> unit

function decode_ldaxp_aarch64_instrs_memory_exclusive_pair (Rt, Rn, Rt2, o0, Rs, L, sz) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = true;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (32 << UInt(sz));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_pair(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 579) = {
    SEE = 579;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let sz = Slice(__opcode, 30, 1);
    if ((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldaxp_aarch64_instrs_memory_exclusive_pair(Rt, Rn, Rt2, o0, Rs, L, sz)
    }
}

val decode_ldxp_aarch64_instrs_memory_exclusive_pair : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(1)) -> unit

function decode_ldxp_aarch64_instrs_memory_exclusive_pair (Rt, Rn, Rt2, o0, Rs, L, sz) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = true;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (32 << UInt(sz));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_pair(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 580) = {
    SEE = 580;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let sz = Slice(__opcode, 30, 1);
    if ((([__opcode[16]] != 0b1 | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldxp_aarch64_instrs_memory_exclusive_pair(Rt, Rn, Rt2, o0, Rs, L, sz)
    }
}

val decode_stlxp_aarch64_instrs_memory_exclusive_pair : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(1)) -> unit

function decode_stlxp_aarch64_instrs_memory_exclusive_pair (Rt, Rn, Rt2, o0, Rs, L, sz) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = true;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (32 << UInt(sz));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_pair(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 581) = {
    SEE = 581;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let sz = Slice(__opcode, 30, 1);
    decode_stlxp_aarch64_instrs_memory_exclusive_pair(Rt, Rn, Rt2, o0, Rs, L, sz)
}

val decode_stxp_aarch64_instrs_memory_exclusive_pair : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(1)) -> unit

function decode_stxp_aarch64_instrs_memory_exclusive_pair (Rt, Rn, Rt2, o0, Rs, L, sz) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = true;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (32 << UInt(sz));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_pair(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 582) = {
    SEE = 582;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let sz = Slice(__opcode, 30, 1);
    decode_stxp_aarch64_instrs_memory_exclusive_pair(Rt, Rn, Rt2, o0, Rs, L, sz)
}

val execute_aarch64_instrs_memory_exclusive_single : forall ('acqrel : Bool) 'datasize 'elsize 'n ('pair : Bool) 'regsize ('rn_unknown : Bool) ('rt_unknown : Bool) 's 't 't2 ('tagchecked : Bool),
  (0 <= 't2 & 't2 <= 31 & 0 <= 't & 't <= 31 & 0 <= 's & 's <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'elsize in {8, 16, 32, 64} & 'datasize in {8, 16, 32, 64, 128}).
  (bool('acqrel), int('datasize), int('elsize), MemOp, int('n), bool('pair), int('regsize), bool('rn_unknown), bool('rt_unknown), int('s), int('t), int('t2), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_exclusive_single (acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let 'dbytes = DIV(datasize, 8);
    let accdesc : AccessDescriptor = CreateAccDescExLDST(memop, acqrel, tagchecked);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else if rn_unknown then {
        address = __UNKNOWN_bits(64)
    } else {
        address = X_read(n, 64)
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else if pair then {
              assert(constraint((div('datasize, 2) == 8 | div('datasize, 2) == 16 | div('datasize, 2) == 32 | div('datasize, 2) == 64)));
              let el1 : bits(div('datasize, 2)) = X_read(t, DIV(datasize, 2));
              let el2 : bits(div('datasize, 2)) = X_read(t2, DIV(datasize, 2));
              data = if BigEndian(accdesc.acctype) then el1 @ el2 else el2 @ el1
          } else {
              assert(constraint('datasize in {8, 16, 32, 64}));
              data = X_read(t, datasize)
          };
          status : bits(1) = 0b1;
          if AArch64_ExclusiveMonitorsPass(address, dbytes) then {
              Mem_set(address, dbytes, accdesc) = data;
              status = ExclusiveMonitorsStatus()
          } else if SPESampleInFlight then {
              SPESetDataVirtualAddress(address)
          } else ();
          X_set(s, 32) = ZeroExtend(status, 32)
      },
      MemOp_LOAD => {
          AArch64_SetExclusiveMonitors(address, dbytes);
          if pair then {
              if rt_unknown then {
                  assert(constraint('datasize in {32, 64}));
                  X_set(t, datasize) = __UNKNOWN_bits(datasize)
              } else if elsize == 32 then {
                  let data : bits('datasize) = Mem_read(address, dbytes, accdesc);
                  if BigEndian(accdesc.acctype) then {
                      assert(constraint((0 <= 'elsize & 'elsize <= 'datasize - 1 & 'datasize - 1 < 'datasize)));
                      assert(constraint(('datasize - 'elsize == 32 | 'datasize - 'elsize == 64)));
                      X_set(t, datasize - elsize) = data[datasize - 1 .. elsize];
                      X_set(t2, elsize) = data[elsize - 1 .. 0]
                  } else {
                      assert(constraint((0 <= 'elsize - 1 & 'elsize - 1 < 'datasize)));
                      X_set(t, elsize) = data[elsize - 1 .. 0];
                      assert(constraint((0 <= 'elsize & 'elsize <= 'datasize - 1 & 'datasize - 1 < 'datasize)));
                      assert(constraint(('datasize - 'elsize == 32 | 'datasize - 'elsize == 64)));
                      X_set(t2, datasize - elsize) = data[datasize - 1 .. elsize]
                  }
              } else {
                  if not_bool(IsAligned__1(address, dbytes)) then {
                      AArch64_Abort(address, AlignmentFault(accdesc))
                  };
                  X_set(t, 64) = Mem_read(address + 0, 8, accdesc);
                  X_set(t2, 64) = Mem_read(address + 8, 8, accdesc)
              }
          } else {
              let data : bits('datasize) = Mem_read(address, dbytes, accdesc);
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      _ => ()
    };
    if SPESampleInFlight then {
        let ar : bits(1) = if acqrel then 0b1 else 0b0;
        let excl : bits(1) = 0b1;
        let at : bits(1) = 0b0;
        let is_load : bool = memop == MemOp_LOAD;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_ldaxr_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldaxr_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 583) = {
    SEE = 583;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldaxr_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldaxrb_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldaxrb_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 584) = {
    SEE = 584;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldaxrb_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldaxrh_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldaxrh_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 585) = {
    SEE = 585;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldaxrh_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldxr_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldxr_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 586) = {
    SEE = 586;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldxr_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldxrb_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldxrb_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 587) = {
    SEE = 587;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldxrb_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_ldxrh_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_ldxrh_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 588) = {
    SEE = 588;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if (((((((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1) | [__opcode[16]] != 0b1) | [__opcode[17]] != 0b1) | [__opcode[18]] != 0b1) | [__opcode[19]] != 0b1) | [__opcode[20]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_ldxrh_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stlxr_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stlxr_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 589) = {
    SEE = 589;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stlxr_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stlxrb_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stlxrb_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 590) = {
    SEE = 590;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stlxrb_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stlxrh_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stlxrh_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitone, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 591) = {
    SEE = 591;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stlxrh_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stxr_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stxr_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 592) = {
    SEE = 592;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stxr_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stxrb_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stxrb_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 593) = {
    SEE = 593;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stxrb_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val decode_stxrh_aarch64_instrs_memory_exclusive_single : (bits(5), bits(5), bits(5), bits(1), bits(5), bits(1), bits(2)) -> unit

function decode_stxrh_aarch64_instrs_memory_exclusive_single (Rt, Rn, Rt2, o0, Rs, L, size) = {
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let 's = UInt(Rs);
    let acqrel : bool = o0 == 0b1;
    let pair : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    let 'elsize = (8 << UInt(size));
    let 'regsize = if elsize == 64 then 64 else 32;
    let 'datasize = if pair then elsize * 2 else elsize;
    let tagchecked : bool = n != 31;
    rt_unknown : bool = false;
    rn_unknown : bool = false;
    if (memop == MemOp_LOAD & pair) & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_STORE then {
        if s == t | pair & s == t2 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_DATAOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rt_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        if s == n & n != 31 then {
            let c : Constraint = ConstrainUnpredictable(Unpredictable_BASEOVERLAP);
            assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
            match c {
              Constraint_UNKNOWN => {
                  rn_unknown = true
              },
              Constraint_UNDEF => {
                  throw(Error_Undefined())
              },
              Constraint_NOP => {
                  EndOfInstruction()
              },
              _ => ()
            }
        };
        ()
    };
    execute_aarch64_instrs_memory_exclusive_single(acqrel, datasize, elsize, memop, n, pair, regsize, rn_unknown, rt_unknown, s, t, t2, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, bitzero, bitone, bitone, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 594) = {
    SEE = 594;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let o0 = Slice(__opcode, 15, 1);
    let Rs = Slice(__opcode, 16, 5);
    let L = Slice(__opcode, 22, 1);
    let size = Slice(__opcode, 30, 2);
    if ((([__opcode[10]] != 0b1 | [__opcode[11]] != 0b1) | [__opcode[12]] != 0b1) | [__opcode[13]] != 0b1) | [__opcode[14]] != 0b1 then {
        throw(Error_ConstrainedUnpredictable())
    } else {
        decode_stxrh_aarch64_instrs_memory_exclusive_single(Rt, Rn, Rt2, o0, Rs, L, size)
    }
}

val execute_aarch64_instrs_memory_pair_general_no_alloc : forall 'datasize 'n ('nontemporal : Bool) ('postindex : Bool) ('rt_unknown : Bool) 't 't2 ('tagchecked : Bool) ('wback : Bool),
  (0 <= 't2 & 't2 <= 31 & 0 <= 't & 't <= 31 & 0 <= 'n & 'n <= 31 & 'datasize in {32, 64}).
  (int('datasize), MemOp, int('n), bool('nontemporal), bits(64), bool('postindex), bool('rt_unknown), int('t), int('t2), bool('tagchecked), bool('wback)) -> unit

function execute_aarch64_instrs_memory_pair_general_no_alloc (datasize, memop, n, nontemporal, offset, postindex, rt_unknown, t, t2, tagchecked, wback) = {
    address : bits(64) = undefined;
    data1 : bits('datasize) = undefined;
    data2 : bits('datasize) = undefined;
    let 'dbytes = DIV(datasize, 8);
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown & t == n then {
              data1 = __UNKNOWN_bits(datasize)
          } else {
              data1 = X_read(t, datasize)
          };
          if rt_unknown & t2 == n then {
              data2 = __UNKNOWN_bits(datasize)
          } else {
              data2 = X_read(t2, datasize)
          };
          Mem_set(address + 0, dbytes, accdesc) = data1;
          Mem_set(address + dbytes, dbytes, accdesc) = data2
      },
      MemOp_LOAD => {
          if HaveLSE2Ext() then {
              let ispair : bool = true;
              let full_data : bits(2 * 'datasize) = Mem_read__1(address, 2 * dbytes, accdesc, ispair);
              if BigEndian(accdesc.acctype) then {
                  data2 = full_data[datasize - 1 .. 0];
                  data1 = full_data[2 * datasize - 1 .. datasize]
              } else {
                  data1 = full_data[datasize - 1 .. 0];
                  data2 = full_data[2 * datasize - 1 .. datasize]
              }
          } else {
              data1 = Mem_read(address + 0, dbytes, accdesc);
              data2 = Mem_read(address + dbytes, dbytes, accdesc)
          };
          if rt_unknown then {
              data1 = __UNKNOWN_bits(datasize);
              data2 = __UNKNOWN_bits(datasize)
          };
          X_set(t, datasize) = data1;
          X_set(t2, datasize) = data2
      },
      _ => ()
    };
    if wback then {
        if postindex then {
            address = address + offset
        };
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldnp_gen_aarch64_instrs_memory_pair_general_no_alloc : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldnp_gen_aarch64_instrs_memory_pair_general_no_alloc (Rt, Rn, Rt2, imm7, L, opc) = {
    let wback : bool = false;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = true;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if [opc[0]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_no_alloc(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, t, t2, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 599) = {
    SEE = 599;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldnp_gen_aarch64_instrs_memory_pair_general_no_alloc(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_stnp_gen_aarch64_instrs_memory_pair_general_no_alloc : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_stnp_gen_aarch64_instrs_memory_pair_general_no_alloc (Rt, Rn, Rt2, imm7, L, opc) = {
    let wback : bool = false;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = true;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if [opc[0]] == 0b1 then {
        throw(Error_Undefined())
    };
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_no_alloc(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, t, t2, tagchecked, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 600) = {
    SEE = 600;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_stnp_gen_aarch64_instrs_memory_pair_general_no_alloc(Rt, Rn, Rt2, imm7, L, opc)
}

val execute_aarch64_instrs_memory_pair_general_post_idx : forall 'datasize ('is_signed : Bool) 'n ('nontemporal : Bool) ('postindex : Bool) ('rt_unknown : Bool) 't 't2 ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't2 & 't2 <= 31 & 0 <= 't & 't <= 31 & 0 <= 'n & 'n <= 31 & 'datasize in {32, 64}).
  (int('datasize), MemOp, int('n), bool('nontemporal), bits(64), bool('postindex), bool('rt_unknown), bool('is_signed), int('t), int('t2), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_pair_general_post_idx (datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    data1 : bits('datasize) = undefined;
    data2 : bits('datasize) = undefined;
    let 'dbytes = DIV(datasize, 8);
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown & t == n then {
              data1 = __UNKNOWN_bits(datasize)
          } else {
              data1 = X_read(t, datasize)
          };
          if rt_unknown & t2 == n then {
              data2 = __UNKNOWN_bits(datasize)
          } else {
              data2 = X_read(t2, datasize)
          };
          if HaveLSE2Ext() then {
              full_data : bits(2 * 'datasize) = undefined;
              if BigEndian(accdesc.acctype) then {
                  full_data = data1 @ data2
              } else {
                  full_data = data2 @ data1
              };
              let ispair : bool = true;
              Mem_set__1(address, 2 * dbytes, accdesc, ispair) = full_data
          } else {
              Mem_set(address + 0, dbytes, accdesc) = data1;
              Mem_set(address + dbytes, dbytes, accdesc) = data2
          }
      },
      MemOp_LOAD => {
          if HaveLSE2Ext() & not_bool(is_signed) then {
              let ispair : bool = true;
              let full_data : bits(2 * 'datasize) = Mem_read__1(address, 2 * dbytes, accdesc, ispair);
              if BigEndian(accdesc.acctype) then {
                  data2 = full_data[datasize - 1 .. 0];
                  data1 = full_data[2 * datasize - 1 .. datasize]
              } else {
                  data1 = full_data[datasize - 1 .. 0];
                  data2 = full_data[2 * datasize - 1 .. datasize]
              }
          } else {
              data1 = Mem_read(address + 0, dbytes, accdesc);
              data2 = Mem_read(address + dbytes, dbytes, accdesc)
          };
          if rt_unknown then {
              data1 = __UNKNOWN_bits(datasize);
              data2 = __UNKNOWN_bits(datasize)
          };
          if is_signed then {
              X_set(t, 64) = SignExtend(data1, 64);
              X_set(t2, 64) = SignExtend(data2, 64)
          } else {
              X_set(t, datasize) = data1;
              X_set(t2, datasize) = data2
          }
      },
      _ => ()
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldp_gen_aarch64_instrs_memory_pair_general_post_idx : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldp_gen_aarch64_instrs_memory_pair_general_post_idx (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 609) = {
    SEE = 609;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldp_gen_aarch64_instrs_memory_pair_general_post_idx(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_ldp_gen_aarch64_instrs_memory_pair_general_pre_idx : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldp_gen_aarch64_instrs_memory_pair_general_pre_idx (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 610) = {
    SEE = 610;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldp_gen_aarch64_instrs_memory_pair_general_pre_idx(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_ldp_gen_aarch64_instrs_memory_pair_general_offset : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldp_gen_aarch64_instrs_memory_pair_general_offset (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 611) = {
    SEE = 611;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldp_gen_aarch64_instrs_memory_pair_general_offset(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_ldpsw_aarch64_instrs_memory_pair_general_post_idx : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldpsw_aarch64_instrs_memory_pair_general_post_idx (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 612) = {
    SEE = 612;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldpsw_aarch64_instrs_memory_pair_general_post_idx(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_ldpsw_aarch64_instrs_memory_pair_general_pre_idx : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldpsw_aarch64_instrs_memory_pair_general_pre_idx (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 613) = {
    SEE = 613;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldpsw_aarch64_instrs_memory_pair_general_pre_idx(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_ldpsw_aarch64_instrs_memory_pair_general_offset : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_ldpsw_aarch64_instrs_memory_pair_general_offset (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 614) = {
    SEE = 614;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_ldpsw_aarch64_instrs_memory_pair_general_offset(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_stp_gen_aarch64_instrs_memory_pair_general_post_idx : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_stp_gen_aarch64_instrs_memory_pair_general_post_idx (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 615) = {
    SEE = 615;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_stp_gen_aarch64_instrs_memory_pair_general_post_idx(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_stp_gen_aarch64_instrs_memory_pair_general_pre_idx : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_stp_gen_aarch64_instrs_memory_pair_general_pre_idx (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 616) = {
    SEE = 616;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_stp_gen_aarch64_instrs_memory_pair_general_pre_idx(Rt, Rn, Rt2, imm7, L, opc)
}

val decode_stp_gen_aarch64_instrs_memory_pair_general_offset : (bits(5), bits(5), bits(5), bits(7), bits(1), bits(2)) -> unit

function decode_stp_gen_aarch64_instrs_memory_pair_general_offset (Rt, Rn, Rt2, imm7, L, opc) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 't2 = UInt(Rt2);
    let nontemporal : bool = false;
    let memop : MemOp = if L == 0b1 then MemOp_LOAD else MemOp_STORE;
    if (L @ [opc[0]]) == 0b01 | opc == 0b11 then {
        throw(Error_Undefined())
    };
    let is_signed : bool = [opc[0]] != 0b0;
    let 'scale = 2 + UInt([opc[1]]);
    let 'datasize = (8 << scale);
    let offset : bits(64) = LSL(SignExtend(imm7, 64), scale);
    let tagchecked : bool = wback | n != 31;
    rt_unknown : bool = false;
    wb_unknown : bool = false;
    if ((memop == MemOp_LOAD & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & (t == n | t2 == n)) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if memop == MemOp_LOAD & t == t2 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_LDPOVERLAP);
        assert(c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    assert(constraint('datasize in {32, 64}));
    execute_aarch64_instrs_memory_pair_general_post_idx(datasize, memop, n, nontemporal, offset, postindex, rt_unknown, is_signed, t, t2, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([_, bitzero, bitone, bitzero, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 617) = {
    SEE = 617;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rt2 = Slice(__opcode, 10, 5);
    let imm7 = Slice(__opcode, 15, 7);
    let L = Slice(__opcode, 22, 1);
    let opc = Slice(__opcode, 30, 2);
    decode_stp_gen_aarch64_instrs_memory_pair_general_offset(Rt, Rn, Rt2, imm7, L, opc)
}

val execute_aarch64_instrs_memory_literal_general : forall ('is_signed : Bool) ('nontemporal : Bool) 'size 't ('tagchecked : Bool),
  (0 <= 't & 't <= 31 & 'size in {4, 8}).
  (MemOp, bool('nontemporal), bits(64), bool('is_signed), int('size), int('t), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_literal_general (memop, nontemporal, offset, is_signed, size, t, tagchecked) = {
    let address : bits(64) = PC_read() + offset;
    data : bits('size * 8) = undefined;
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    match memop {
      MemOp_LOAD => {
          let data : bits('size * 8) = Mem_read(address, size, accdesc);
          if is_signed then {
              X_set(t, 64) = SignExtend(data, 64)
          } else {
              X_set(t, size * 8) = data
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      },
      _ => ()
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldr_lit_gen_aarch64_instrs_memory_literal_general : (bits(5), bits(19), bits(2)) -> unit

function decode_ldr_lit_gen_aarch64_instrs_memory_literal_general (Rt, imm19, opc) = {
    let 't = UInt(Rt);
    memop : MemOp = MemOp_LOAD;
    is_signed : bool = false;
    let nontemporal : bool = false;
    size : {4, 8} = 4;
    match opc {
      0b00 => {
          size = 4
      },
      0b01 => {
          size = 8
      },
      0b10 => {
          size = 4;
          is_signed = true
      },
      0b11 => {
          memop = MemOp_PREFETCH
      }
    };
    let 'size = size;
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    let tagchecked : bool = false;
    execute_aarch64_instrs_memory_literal_general(memop, nontemporal, offset, is_signed, size, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, _, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 624) = {
    SEE = 624;
    let Rt = Slice(__opcode, 0, 5);
    let imm19 = Slice(__opcode, 5, 19);
    let opc = Slice(__opcode, 30, 2);
    decode_ldr_lit_gen_aarch64_instrs_memory_literal_general(Rt, imm19, opc)
}

val decode_ldrsw_lit_aarch64_instrs_memory_literal_general : (bits(5), bits(19), bits(2)) -> unit

function decode_ldrsw_lit_aarch64_instrs_memory_literal_general (Rt, imm19, opc) = {
    let 't = UInt(Rt);
    memop : MemOp = MemOp_LOAD;
    is_signed : bool = false;
    let nontemporal : bool = false;
    size : {4, 8} = 4;
    match opc {
      0b00 => {
          size = 4
      },
      0b01 => {
          size = 8
      },
      0b10 => {
          size = 4;
          is_signed = true
      },
      0b11 => {
          memop = MemOp_PREFETCH
      }
    };
    let 'size = size;
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    let tagchecked : bool = false;
    execute_aarch64_instrs_memory_literal_general(memop, nontemporal, offset, is_signed, size, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 625) = {
    SEE = 625;
    let Rt = Slice(__opcode, 0, 5);
    let imm19 = Slice(__opcode, 5, 19);
    let opc = Slice(__opcode, 30, 2);
    decode_ldrsw_lit_aarch64_instrs_memory_literal_general(Rt, imm19, opc)
}

val decode_prfm_lit_aarch64_instrs_memory_literal_general : (bits(5), bits(19), bits(2)) -> unit

function decode_prfm_lit_aarch64_instrs_memory_literal_general (Rt, imm19, opc) = {
    let 't = UInt(Rt);
    memop : MemOp = MemOp_LOAD;
    is_signed : bool = false;
    let nontemporal : bool = false;
    size : {4, 8} = 4;
    match opc {
      0b00 => {
          size = 4
      },
      0b01 => {
          size = 8
      },
      0b10 => {
          size = 4;
          is_signed = true
      },
      0b11 => {
          memop = MemOp_PREFETCH
      }
    };
    let 'size = size;
    let offset : bits(64) = SignExtend(imm19 @ 0b00, 64);
    let tagchecked : bool = false;
    execute_aarch64_instrs_memory_literal_general(memop, nontemporal, offset, is_signed, size, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitone, bitzero, bitone, bitone, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 626) = {
    SEE = 626;
    let Rt = Slice(__opcode, 0, 5);
    let imm19 = Slice(__opcode, 5, 19);
    let opc = Slice(__opcode, 30, 2);
    decode_prfm_lit_aarch64_instrs_memory_literal_general(Rt, imm19, opc)
}

val execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx : forall 'datasize ('is_signed : Bool) 'n ('nontemporal : Bool) ('postindex : Bool) 'regsize ('rt_unknown : Bool) 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), MemOp, int('n), bool('nontemporal), bits(64), bool('postindex), int('regsize), bool('rt_unknown), bool('is_signed), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx (datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        if memop != MemOp_PREFETCH then {
            CheckSPAlignment()
        };
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), false, t, regsize == 64, false)
          };
          Mem_set(address, DIV(datasize, 8), accdesc) = data
      },
      MemOp_LOAD => {
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), is_signed, t, regsize == 64, false)
          };
          let data : bits('datasize) = Mem_read(address, DIV(datasize, 8), accdesc);
          if is_signed then {
              assert(constraint(('datasize > 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = SignExtend(data, regsize)
          } else {
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      }
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 628) = {
    SEE = 628;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 629) = {
    SEE = 629;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 630) = {
    SEE = 630;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldr_imm_gen_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 631) = {
    SEE = 631;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 632) = {
    SEE = 632;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 633) = {
    SEE = 633;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrb_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 634) = {
    SEE = 634;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 635) = {
    SEE = 635;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 636) = {
    SEE = 636;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrh_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 637) = {
    SEE = 637;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 638) = {
    SEE = 638;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 639) = {
    SEE = 639;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsb_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 640) = {
    SEE = 640;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 641) = {
    SEE = 641;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 642) = {
    SEE = 642;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsh_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 643) = {
    SEE = 643;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 644) = {
    SEE = 644;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 645) = {
    SEE = 645;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsw_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 646) = {
    SEE = 646;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 647) = {
    SEE = 647;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 648) = {
    SEE = 648;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_str_imm_gen_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_strb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_strb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 649) = {
    SEE = 649;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strb_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_strb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_strb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 650) = {
    SEE = 650;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strb_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_strb_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_strb_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 651) = {
    SEE = 651;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strb_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val decode_strh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_strh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = true;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 652) = {
    SEE = 652;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strh_imm_aarch64_instrs_memory_single_general_immediate_signed_post_idx(Rt, Rn, imm9, opc, size)
}

val decode_strh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_strh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx (Rt, Rn, imm9, opc, size) = {
    wback : bool = true;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 653) = {
    SEE = 653;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strh_imm_aarch64_instrs_memory_single_general_immediate_signed_pre_idx(Rt, Rn, imm9, opc, size)
}

val decode_strh_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_strh_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_post_idx(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 654) = {
    SEE = 654;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strh_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
}

val execute_aarch64_instrs_memory_single_general_register : forall 'datasize ('is_signed : Bool) 'm 'n ('nontemporal : Bool) ('postindex : Bool) 'regsize ('rt_unknown : Bool) 'shift 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'shift in {0, 1, 2, 3} & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), ExtendType, int('m), MemOp, int('n), bool('nontemporal), bool('postindex), int('regsize), bool('rt_unknown), int('shift), bool('is_signed), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_single_general_register (datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback) = {
    let offset : bits(64) = ExtendReg(m, extend_type, shift, 64);
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        if memop != MemOp_PREFETCH then {
            CheckSPAlignment()
        };
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), false, t, regsize == 64, false)
          };
          Mem_set(address, DIV(datasize, 8), accdesc) = data
      },
      MemOp_LOAD => {
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), is_signed, t, regsize == 64, false)
          };
          let data : bits('datasize) = Mem_read(address, DIV(datasize, 8), accdesc);
          if is_signed then {
              assert(constraint(('datasize > 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = SignExtend(data, regsize)
          } else {
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      }
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldr_reg_gen_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_ldr_reg_gen_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 655) = {
    SEE = 655;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldr_reg_gen_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_ldrb_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_ldrb_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 656) = {
    SEE = 656;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrb_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_ldrh_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_ldrh_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 657) = {
    SEE = 657;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrh_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_ldrsb_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_ldrsb_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 658) = {
    SEE = 658;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsb_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_ldrsh_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_ldrsh_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 659) = {
    SEE = 659;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsh_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_ldrsw_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_ldrsw_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 660) = {
    SEE = 660;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldrsw_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_prfm_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_prfm_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    assert(not_bool((match Rt {
      [bitone, bitone, _, _, _] => true,
      _ => false
    }) : bool));
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitone, _, _, _, _, _, _, bitone, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if not_bool((match Slice(__opcode, 0, 5) {
  [bitone, bitone, _, _, _] => true,
  _ => false
}) : bool) & SEE < 661) = {
    SEE = 661;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_prfm_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_str_reg_gen_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_str_reg_gen_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 662) = {
    SEE = 662;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_str_reg_gen_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_strb_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_strb_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 663) = {
    SEE = 663;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strb_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val decode_strh_reg_aarch64_instrs_memory_single_general_register : (bits(5), bits(5), bits(1), bits(3), bits(5), bits(2), bits(2)) -> unit

function decode_strh_reg_aarch64_instrs_memory_single_general_register (Rt, Rn, S, option_name, Rm, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    if [option_name[1]] == 0b0 then {
        throw(Error_Undefined())
    };
    let extend_type : ExtendType = DecodeRegExtend(option_name);
    let 'shift = if S == 0b1 then scale else 0;
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let 'm = UInt(Rm);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH;
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_register(datasize, extend_type, m, memop, n, nontemporal, postindex, regsize, rt_unknown, shift, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 664) = {
    SEE = 664;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let S = Slice(__opcode, 12, 1);
    let option_name = Slice(__opcode, 13, 3);
    let Rm = Slice(__opcode, 16, 5);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_strh_reg_aarch64_instrs_memory_single_general_register(Rt, Rn, S, option_name, Rm, opc, size)
}

val execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : forall 'datasize ('is_signed : Bool) 'n ('nontemporal : Bool) ('postindex : Bool) 'regsize ('rt_unknown : Bool) 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), MemOp, int('n), bool('nontemporal), bits(64), bool('postindex), int('regsize), bool('rt_unknown), bool('is_signed), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let privileged : bool = AArch64_IsUnprivAccessPriv();
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        if memop != MemOp_PREFETCH then {
            CheckSPAlignment()
        };
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), false, t, regsize == 64, false)
          };
          Mem_set(address, DIV(datasize, 8), accdesc) = data
      },
      MemOp_LOAD => {
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), is_signed, t, regsize == 64, false)
          };
          let data : bits('datasize) = Mem_read(address, DIV(datasize, 8), accdesc);
          if is_signed then {
              assert(constraint(('datasize > 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = SignExtend(data, regsize)
          } else {
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      }
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldtr_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldtr_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 669) = {
    SEE = 669;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldtr_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_ldtrb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldtrb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 670) = {
    SEE = 670;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldtrb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_ldtrh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldtrh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 671) = {
    SEE = 671;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldtrh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_ldtrsb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldtrsb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 672) = {
    SEE = 672;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldtrsb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_ldtrsh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldtrsh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 673) = {
    SEE = 673;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldtrsh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_ldtrsw_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldtrsw_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 674) = {
    SEE = 674;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldtrsw_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_sttr_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_sttr_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 675) = {
    SEE = 675;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_sttr_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_sttrb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_sttrb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 676) = {
    SEE = 676;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_sttrb_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val decode_sttrh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_sttrh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            throw(Error_Undefined())
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitone, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 677) = {
    SEE = 677;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_sttrh_aarch64_instrs_memory_single_general_immediate_signed_offset_unpriv(Rt, Rn, imm9, opc, size)
}

val execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : forall 'datasize ('is_signed : Bool) 'n ('nontemporal : Bool) ('postindex : Bool) 'regsize ('rt_unknown : Bool) 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), MemOp, int('n), bool('nontemporal), bits(64), bool('postindex), int('regsize), bool('rt_unknown), bool('is_signed), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        if memop != MemOp_PREFETCH then {
            CheckSPAlignment()
        };
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), false, t, regsize == 64, false)
          };
          Mem_set(address, DIV(datasize, 8), accdesc) = data
      },
      MemOp_LOAD => {
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), is_signed, t, regsize == 64, false)
          };
          let data : bits('datasize) = Mem_read(address, DIV(datasize, 8), accdesc);
          if is_signed then {
              assert(constraint(('datasize > 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = SignExtend(data, regsize)
          } else {
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      }
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_ldur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 680) = {
    SEE = 680;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_ldurb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldurb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 681) = {
    SEE = 681;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldurb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_ldurh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldurh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitone, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 682) = {
    SEE = 682;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldurh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_ldursb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldursb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 683) = {
    SEE = 683;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldursb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_ldursh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldursh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, _, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 684) = {
    SEE = 684;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldursh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_ldursw_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_ldursw_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 685) = {
    SEE = 685;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_ldursw_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_prfum_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_prfum_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitone, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 686) = {
    SEE = 686;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_prfum_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_stur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_stur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 687) = {
    SEE = 687;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_stur_gen_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_sturb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_sturb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 688) = {
    SEE = 688;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_sturb_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
}

val decode_sturh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal : (bits(5), bits(5), bits(9), bits(2), bits(2)) -> unit

function decode_sturh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal (Rt, Rn, imm9, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = SignExtend(imm9, 64);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 689) = {
    SEE = 689;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm9 = Slice(__opcode, 12, 9);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_sturh_aarch64_instrs_memory_single_general_immediate_signed_offset_normal(Rt, Rn, imm9, opc, size)
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

val execute_aarch64_instrs_memory_single_general_immediate_unsigned : forall 'datasize ('is_signed : Bool) 'n ('nontemporal : Bool) ('postindex : Bool) 'regsize ('rt_unknown : Bool) 't ('tagchecked : Bool) ('wb_unknown : Bool) ('wback : Bool),
  (0 <= 't & 't <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (int('datasize), MemOp, int('n), bool('nontemporal), bits(64), bool('postindex), int('regsize), bool('rt_unknown), bool('is_signed), int('t), bool('tagchecked), bool('wb_unknown), bool('wback)) -> unit

function execute_aarch64_instrs_memory_single_general_immediate_unsigned (datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback) = {
    address : bits(64) = undefined;
    data : bits('datasize) = undefined;
    let privileged : bool = PSTATE.EL != EL0;
    let accdesc : AccessDescriptor = CreateAccDescGPR(memop, nontemporal, privileged, tagchecked);
    if n == 31 then {
        if memop != MemOp_PREFETCH then {
            CheckSPAlignment()
        };
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    if not_bool(postindex) then {
        address = address + offset
    };
    match memop {
      MemOp_STORE => {
          if rt_unknown then {
              data = __UNKNOWN_bits(datasize)
          } else {
              data = X_read(t, datasize)
          };
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), false, t, regsize == 64, false)
          };
          Mem_set(address, DIV(datasize, 8), accdesc) = data
      },
      MemOp_LOAD => {
          if not_bool(wback) then {
              AArch64_SetLSInstructionSyndrome(DIV(datasize, 8), is_signed, t, regsize == 64, false)
          };
          let data : bits('datasize) = Mem_read(address, DIV(datasize, 8), accdesc);
          if is_signed then {
              assert(constraint(('datasize > 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = SignExtend(data, regsize)
          } else {
              assert(constraint(('datasize >= 0 & 'regsize >= 'datasize)));
              X_set(t, regsize) = ZeroExtend(data, regsize)
          }
      },
      MemOp_PREFETCH => {
          Prefetch(address, t[4 .. 0])
      }
    };
    if wback then {
        if wb_unknown then {
            address = __UNKNOWN_bits(64)
        } else if postindex then {
            address = address + offset
        } else ();
        if n == 31 then {
            SP_set() = address
        } else {
            X_set(n, 64) = address
        }
    };
    if SPESampleInFlight then {
        SPESampleGeneralPurposeLoadStore()
    };
    ()
}

val decode_prfm_imm_aarch64_instrs_memory_single_general_immediate_unsigned : (bits(5), bits(5), bits(12), bits(2), bits(2)) -> unit

function decode_prfm_imm_aarch64_instrs_memory_single_general_immediate_unsigned (Rt, Rn, imm12, opc, size) = {
    wback : bool = false;
    let postindex : bool = false;
    let 'scale = UInt(size);
    let offset : bits(64) = LSL(ZeroExtend(imm12, 64), scale);
    let 'n = UInt(Rn);
    let 't = UInt(Rt);
    let nontemporal : bool = false;
    memop : MemOp = undefined;
    is_signed : bool = undefined;
    regsize : {32, 64} = 32;
    if [opc[1]] == 0b0 then {
        memop = if [opc[0]] == 0b1 then MemOp_LOAD else MemOp_STORE;
        regsize = if size == 0b11 then 64 else 32;
        is_signed = false
    } else {
        if size == 0b11 then {
            memop = MemOp_PREFETCH;
            if [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            ()
        } else {
            memop = MemOp_LOAD;
            if size == 0b10 & [opc[0]] == 0b1 then {
                throw(Error_Undefined())
            };
            regsize = if [opc[0]] == 0b1 then 32 else 64;
            is_signed = true
        }
    };
    let 'regsize = regsize;
    let 'datasize = (8 << scale);
    let tagchecked : bool = memop != MemOp_PREFETCH & (wback | n != 31);
    wb_unknown : bool = false;
    rt_unknown : bool = false;
    c : Constraint = undefined;
    if ((memop == MemOp_LOAD & wback) & n == t) & n != 31 then {
        c = ConstrainUnpredictable(Unpredictable_WBOVERLAPLD);
        assert(c == Constraint_WBSUPPRESS | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_WBSUPPRESS => {
              wback = false
          },
          Constraint_UNKNOWN => {
              wb_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    if ((memop == MemOp_STORE & wback) & n == t) & n != 31 then {
        let c : Constraint = ConstrainUnpredictable(Unpredictable_WBOVERLAPST);
        assert(c == Constraint_NONE | c == Constraint_UNKNOWN | c == Constraint_UNDEF | c == Constraint_NOP);
        match c {
          Constraint_NONE => {
              rt_unknown = false
          },
          Constraint_UNKNOWN => {
              rt_unknown = true
          },
          Constraint_UNDEF => {
              throw(Error_Undefined())
          },
          Constraint_NOP => {
              EndOfInstruction()
          },
          _ => ()
        }
    };
    execute_aarch64_instrs_memory_single_general_immediate_unsigned(datasize, memop, n, nontemporal, offset, postindex, regsize, rt_unknown, is_signed, t, tagchecked, wb_unknown, wback)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, bitone, bitone, bitone, bitone, bitzero, bitzero, bitone, bitone, bitzero, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 716) = {
    SEE = 716;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let imm12 = Slice(__opcode, 10, 12);
    let opc = Slice(__opcode, 22, 2);
    let size = Slice(__opcode, 30, 2);
    decode_prfm_imm_aarch64_instrs_memory_single_general_immediate_unsigned(Rt, Rn, imm12, opc, size)
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

val execute_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single : forall 'd 'datasize 'elements 'esize ('is_unsigned : Bool) 'm ('minimum : Bool) 'n,
  (0 <= 'n & 'n <= 31 & 0 <= 'm & 'm <= 31 & 'esize in {8, 16, 32, 64} & 'datasize in {64, 128} & 0 <= 'd & 'd <= 31).
  (int('d), int('datasize), int('elements), int('esize), int('m), bool('minimum), int('n), bool('is_unsigned)) -> unit

function execute_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single (d, datasize, elements, esize, m, minimum, n, is_unsigned) = {
    CheckFPAdvSIMDEnabled64();
    let operand1 : bits('datasize) = V_read(n, datasize);
    let operand2 : bits('datasize) = V_read(m, datasize);
    result : bits('datasize) = undefined;
    element1 : int = undefined;
    element2 : int = undefined;
    maxmin : int = undefined;
    foreach (e from 0 to (elements - 1) by 1 in inc) {
        element1 = asl_Int(Elem_read(operand1, e, esize), is_unsigned);
        element2 = asl_Int(Elem_read(operand2, e, esize), is_unsigned);
        maxmin = if minimum then Min(element1, element2) else
          Max(element1, element2);
        result = Elem_set(result, e, esize, maxmin[esize - 1 .. 0])
    };
    V_set(d, datasize) = result
}

val decode_smax_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_smax_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single (Rd, Rn, o1, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if size == 0b11 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let minimum : bool = o1 == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(d, datasize, elements, esize, m, minimum, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 834) = {
    SEE = 834;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o1 = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_smax_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(Rd, Rn, o1, Rm, size, U, Q)
}

val decode_smin_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_smin_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single (Rd, Rn, o1, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if size == 0b11 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let minimum : bool = o1 == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(d, datasize, elements, esize, m, minimum, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitzero, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 835) = {
    SEE = 835;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o1 = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_smin_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(Rd, Rn, o1, Rm, size, U, Q)
}

val decode_umax_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_umax_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single (Rd, Rn, o1, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if size == 0b11 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let minimum : bool = o1 == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(d, datasize, elements, esize, m, minimum, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitzero, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 836) = {
    SEE = 836;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o1 = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_umax_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(Rd, Rn, o1, Rm, size, U, Q)
}

val decode_umin_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single : (bits(5), bits(5), bits(1), bits(5), bits(2), bits(1), bits(1)) -> unit

function decode_umin_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single (Rd, Rn, o1, Rm, size, U, Q) = {
    let 'd = UInt(Rd);
    let 'n = UInt(Rn);
    let 'm = UInt(Rm);
    if size == 0b11 then {
        throw(Error_Undefined())
    };
    let 'esize = (8 << UInt(size));
    let 'datasize = if Q == 0b1 then 128 else 64;
    let 'elements = DIV(datasize, esize);
    let is_unsigned : bool = U == 0b1;
    let minimum : bool = o1 == 0b1;
    execute_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(d, datasize, elements, esize, m, minimum, n, is_unsigned)
}

function clause __DecodeA64_DataProcFPSIMD ((pc, ([bitzero, _, bitone, bitzero, bitone, bitone, bitone, bitzero, _, _, bitone, _, _, _, _, _, bitzero, bitone, bitone, bitzero, bitone, bitone, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 837) = {
    SEE = 837;
    let Rd = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let o1 = Slice(__opcode, 11, 1);
    let Rm = Slice(__opcode, 16, 5);
    let size = Slice(__opcode, 22, 2);
    let U = Slice(__opcode, 29, 1);
    let Q = Slice(__opcode, 30, 1);
    decode_umin_advsimd_aarch64_instrs_vector_arithmetic_binary_uniform_max_min_single(Rd, Rn, o1, Rm, size, U, Q)
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

val execute_aarch64_instrs_memory_atomicops_swp : forall ('acquire : Bool) 'datasize 'n 'regsize ('release : Bool) 's 't ('tagchecked : Bool),
  (0 <= 't & 't <= 31 & 0 <= 's & 's <= 31 & 'regsize in {32, 64} & 0 <= 'n & 'n <= 31 & 'datasize in {8, 16, 32, 64}).
  (bool('acquire), int('datasize), int('n), int('regsize), bool('release), int('s), int('t), bool('tagchecked)) -> unit

function execute_aarch64_instrs_memory_atomicops_swp (acquire, datasize, n, regsize, release, s, t, tagchecked) = {
    address : bits(64) = undefined;
    let accdesc : AccessDescriptor = CreateAccDescAtomicOp(MemAtomicOp_SWP, acquire, release, tagchecked);
    if n == 31 then {
        CheckSPAlignment();
        address = SP_read()
    } else {
        address = X_read(n, 64)
    };
    let store_value : bits('datasize) = X_read(s, datasize);
    let comparevalue : bits('datasize) = __UNKNOWN_bits(datasize);
    let data : bits('datasize) = MemAtomic(address, comparevalue, store_value, accdesc);
    assert(constraint('regsize >= 'datasize));
    X_set(t, regsize) = ZeroExtend(data, regsize);
    if SPESampleInFlight then {
        let ar : bits(1) = if acquire | release then 0b1 else 0b0;
        let excl : bits(1) = 0b0;
        let at : bits(1) = 0b1;
        let is_load : bool = true;
        SPESampleExtendedLoadStore(ar, excl, at, is_load)
    };
    ()
}

val decode_swp_aarch64_instrs_memory_atomicops_swp : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_swp_aarch64_instrs_memory_atomicops_swp (Rt, Rn, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let release : bool = R == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_swp(acquire, datasize, n, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitone, _, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1000) = {
    SEE = 1000;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_swp_aarch64_instrs_memory_atomicops_swp(Rt, Rn, Rs, R, A, size)
}

val decode_swpb_aarch64_instrs_memory_atomicops_swp : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_swpb_aarch64_instrs_memory_atomicops_swp (Rt, Rn, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let release : bool = R == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_swp(acquire, datasize, n, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitzero, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1001) = {
    SEE = 1001;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_swpb_aarch64_instrs_memory_atomicops_swp(Rt, Rn, Rs, R, A, size)
}

val decode_swph_aarch64_instrs_memory_atomicops_swp : (bits(5), bits(5), bits(5), bits(1), bits(1), bits(2)) -> unit

function decode_swph_aarch64_instrs_memory_atomicops_swp (Rt, Rn, Rs, R, A, size) = {
    if not_bool(HaveAtomicExt()) then {
        throw(Error_Undefined())
    };
    let 't = UInt(Rt);
    let 'n = UInt(Rn);
    let 's = UInt(Rs);
    let 'datasize = (8 << UInt(size));
    let 'regsize = if datasize == 64 then 64 else 32;
    let acquire : bool = A == 0b1 & Rt != 0b11111;
    let release : bool = R == 0b1;
    let tagchecked : bool = n != 31;
    execute_aarch64_instrs_memory_atomicops_swp(acquire, datasize, n, regsize, release, s, t, tagchecked)
}

function clause __DecodeA64_LoadStore ((pc, ([bitzero, bitone, bitone, bitone, bitone, bitzero, bitzero, bitzero, _, _, bitone, _, _, _, _, _, bitone, bitzero, bitzero, bitzero, bitzero, bitzero, _, _, _, _, _, _, _, _, _, _] as __opcode)) if SEE < 1002) = {
    SEE = 1002;
    let Rt = Slice(__opcode, 0, 5);
    let Rn = Slice(__opcode, 5, 5);
    let Rs = Slice(__opcode, 16, 5);
    let R = Slice(__opcode, 22, 1);
    let A = Slice(__opcode, 23, 1);
    let size = Slice(__opcode, 30, 2);
    decode_swph_aarch64_instrs_memory_atomicops_swp(Rt, Rn, Rs, R, A, size)
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

bitfield FPCRType : bits(64) =
  {
    FIZ : 0,
    AH : 1,
    NEP : 2,
    IOE : 8,
    DZE : 9,
    OFE : 10,
    UFE : 11,
    IXE : 12,
    EBF : 13,
    IDE : 15,
    length : 18..16,
    FZ16 : 19,
    Stride : 21..20,
    RMode : 23..22,
    FZ : 24,
    DN : 25,
    AHP : 26,
    IOC : 32,
    DZC : 33,
    OFC : 34
  }

enum SecurityState = {SS_NonSecure, SS_Root, SS_Realm, SS_Secure}

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

type PARTIDtype = bits(16)

type PMGtype = bits(8)

enum PARTIDspaceType = {
  PIdSpace_Secure,
  PIdSpace_Root,
  PIdSpace_Realm,
  PIdSpace_NonSecure
}

struct MPAMinfo = {
  mpam_sp : PARTIDspaceType,
  partid : PARTIDtype,
  pmg : PMGtype
}

enum AccessType = {
  AccessType_IFETCH,
  AccessType_GPR,
  AccessType_ASIMD,
  AccessType_SVE,
  AccessType_SME,
  AccessType_IC,
  AccessType_DC,
  AccessType_DCZero,
  AccessType_AT,
  AccessType_NV2,
  AccessType_SPE,
  AccessType_GCS,
  AccessType_GPTW,
  AccessType_TTW
}

enum MemOp = {MemOp_LOAD, MemOp_STORE, MemOp_PREFETCH}

enum VARange = {VARange_LOWER, VARange_UPPER}

enum MemAtomicOp = {
  MemAtomicOp_GCSSS1,
  MemAtomicOp_ADD,
  MemAtomicOp_BIC,
  MemAtomicOp_EOR,
  MemAtomicOp_ORR,
  MemAtomicOp_SMAX,
  MemAtomicOp_SMIN,
  MemAtomicOp_UMAX,
  MemAtomicOp_UMIN,
  MemAtomicOp_SWP,
  MemAtomicOp_CAS
}

enum CacheOp = {CacheOp_Clean, CacheOp_Invalidate, CacheOp_CleanInvalidate}

enum CacheOpScope = {
  CacheOpScope_SetWay,
  CacheOpScope_PoU,
  CacheOpScope_PoC,
  CacheOpScope_PoE,
  CacheOpScope_PoP,
  CacheOpScope_PoDP,
  CacheOpScope_PoPA,
  CacheOpScope_ALLU,
  CacheOpScope_ALLUIS
}

enum CacheType = {
  CacheType_Data,
  CacheType_Tag,
  CacheType_Data_Tag,
  CacheType_Instruction
}

struct AccessDescriptor = {
  acctype : AccessType,
  el : bits(2),
  ss : SecurityState,
  acqsc : bool,
  acqpc : bool,
  relsc : bool,
  limitedordered : bool,
  exclusive : bool,
  atomicop : bool,
  modop : MemAtomicOp,
  nontemporal : bool,
  read : bool,
  write : bool,
  cacheop : CacheOp,
  opscope : CacheOpScope,
  cachetype : CacheType,
  pan : bool,
  transactional : bool,
  nonfault : bool,
  firstfault : bool,
  first : bool,
  contiguous : bool,
  streamingsve : bool,
  ls64 : bool,
  mops : bool,
  rcw : bool,
  rcws : bool,
  toplevel : bool,
  varange : VARange,
  a32lsmd : bool,
  tagchecked : bool,
  tagaccess : bool,
  mpam : MPAMinfo
}

enum MBReqDomain = {
  MBReqDomain_Nonshareable,
  MBReqDomain_InnerShareable,
  MBReqDomain_OuterShareable,
  MBReqDomain_FullSystem
}

enum MBReqTypes = {MBReqTypes_Reads, MBReqTypes_Writes, MBReqTypes_All}

enum Unpredictable = {
  Unpredictable_VMSR,
  Unpredictable_WBOVERLAPLD,
  Unpredictable_WBOVERLAPST,
  Unpredictable_LDPOVERLAP,
  Unpredictable_BASEOVERLAP,
  Unpredictable_DATAOVERLAP,
  Unpredictable_DEVPAGE2,
  Unpredictable_INSTRDEVICE,
  Unpredictable_RESCPACR,
  Unpredictable_RESMAIR,
  Unpredictable_S1CTAGGED,
  Unpredictable_S2RESMEMATTR,
  Unpredictable_RESTEXCB,
  Unpredictable_RESPRRR,
  Unpredictable_RESDACR,
  Unpredictable_RESVTCRS,
  Unpredictable_RESTnSZ,
  Unpredictable_RESTCF,
  Unpredictable_DEVICETAGSTORE,
  Unpredictable_OORTnSZ,
  Unpredictable_LARGEIPA,
  Unpredictable_ESRCONDPASS,
  Unpredictable_ILZEROIT,
  Unpredictable_ILZEROT,
  Unpredictable_BPVECTORCATCHPRI,
  Unpredictable_VCMATCHHALF,
  Unpredictable_VCMATCHDAPA,
  Unpredictable_WPMASKANDBAS,
  Unpredictable_WPBASCONTIGUOUS,
  Unpredictable_RESWPMASK,
  Unpredictable_WPMASKEDBITS,
  Unpredictable_RESBPWPCTRL,
  Unpredictable_BPNOTIMPL,
  Unpredictable_RESBPTYPE,
  Unpredictable_RESMDSELR,
  Unpredictable_BPNOTCTXCMP,
  Unpredictable_BPMATCHHALF,
  Unpredictable_BPMISMATCHHALF,
  Unpredictable_BPLINKINGDISABLED,
  Unpredictable_RESBPMASK,
  Unpredictable_BPMASK,
  Unpredictable_BPMASKEDBITS,
  Unpredictable_BPLINKEDADDRMATCH,
  Unpredictable_RESTARTALIGNPC,
  Unpredictable_RESTARTZEROUPPERPC,
  Unpredictable_ZEROUPPER,
  Unpredictable_ERETZEROUPPERPC,
  Unpredictable_A32FORCEALIGNPC,
  Unpredictable_SMD,
  Unpredictable_NONFAULT,
  Unpredictable_SVEZEROUPPER,
  Unpredictable_SVELDNFDATA,
  Unpredictable_SVELDNFZERO,
  Unpredictable_CHECKSPNONEACTIVE,
  Unpredictable_SMEZEROUPPER,
  Unpredictable_NVNV1,
  Unpredictable_Shareability,
  Unpredictable_AFUPDATE,
  Unpredictable_DBUPDATE,
  Unpredictable_IESBinDebug,
  Unpredictable_BADPMSFCR,
  Unpredictable_ZEROBTYPE,
  Unpredictable_EL2TIMESTAMP,
  Unpredictable_EL1TIMESTAMP,
  Unpredictable_RESERVEDNSxB,
  Unpredictable_WFxTDEBUG,
  Unpredictable_LS64UNSUPPORTED,
  Unpredictable_MISALIGNEDATOMIC,
  Unpredictable_CLEARERRITEZERO,
  Unpredictable_ALUEXCEPTIONRETURN,
  Unpredictable_IGNORETRAPINDEBUG,
  Unpredictable_DBGxVR_RESS,
  Unpredictable_PMUEVENTCOUNTER,
  Unpredictable_PMSCR_PCT,
  Unpredictable_CounterReservedForEL2,
  Unpredictable_BRBFILTRATE,
  Unpredictable_MOPSOVERLAP31,
  Unpredictable_STOREONLYTAGCHECKEDCAS,
  Unpredictable_RESTC
}

enum Constraint = {
  Constraint_NONE,
  Constraint_UNKNOWN,
  Constraint_UNDEF,
  Constraint_UNDEFEL0,
  Constraint_NOP,
  Constraint_TRUE,
  Constraint_FALSE,
  Constraint_DISABLED,
  Constraint_UNCOND,
  Constraint_COND,
  Constraint_ADDITIONAL_DECODE,
  Constraint_WBSUPPRESS,
  Constraint_FAULT,
  Constraint_LIMITED_ATOMICITY,
  Constraint_NVNV1_00,
  Constraint_NVNV1_01,
  Constraint_NVNV1_11,
  Constraint_EL1TIMESTAMP,
  Constraint_EL2TIMESTAMP,
  Constraint_OSH,
  Constraint_ISH,
  Constraint_NSH,
  Constraint_NC,
  Constraint_WT,
  Constraint_WB,
  Constraint_FORCE,
  Constraint_FORCENOSLCHECK,
  Constraint_MAPTOALLOCATED,
  Constraint_PMSCR_PCT_VIRT
}

enum FPRounding = {
  FPRounding_TIEEVEN,
  FPRounding_POSINF,
  FPRounding_NEGINF,
  FPRounding_ZERO,
  FPRounding_TIEAWAY,
  FPRounding_ODD
}

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

enum FPMaxMinOp = {
  FPMaxMinOp_MAX,
  FPMaxMinOp_MIN,
  FPMaxMinOp_MAXNUM,
  FPMaxMinOp_MINNUM
}

enum FPUnaryOp = {FPUnaryOp_ABS, FPUnaryOp_MOV, FPUnaryOp_NEG, FPUnaryOp_SQRT}

enum FPConvOp = {
  FPConvOp_CVT_FtoI,
  FPConvOp_CVT_ItoF,
  FPConvOp_MOV_FtoI,
  FPConvOp_MOV_ItoF,
  FPConvOp_CVT_FtoI_JS
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

val CreateAccDescGPR : forall ('nontemporal : Bool) ('privileged : Bool) ('tagchecked : Bool).
  (MemOp, bool('nontemporal), bool('privileged), bool('tagchecked)) -> AccessDescriptor

function CreateAccDescGPR (memop, nontemporal, privileged, tagchecked) = {
    accdesc : AccessDescriptor = NewAccDesc(AccessType_GPR);
    accdesc.el = if not_bool(privileged) then EL0 else PSTATE.EL;
    accdesc.nontemporal = nontemporal;
    accdesc.read = memop == MemOp_LOAD;
    accdesc.write = memop == MemOp_STORE;
    accdesc.pan = true;
    accdesc.tagchecked = tagchecked;
    accdesc.transactional = HaveTME() & TSTATE.depth > 0;
    return(accdesc)
}

val CreateAccDescExLDST : forall ('acqrel : Bool) ('tagchecked : Bool).
  (MemOp, bool('acqrel), bool('tagchecked)) -> AccessDescriptor

function CreateAccDescExLDST (memop, acqrel, tagchecked) = {
    accdesc : AccessDescriptor = NewAccDesc(AccessType_GPR);
    accdesc.acqsc = acqrel & memop == MemOp_LOAD;
    accdesc.relsc = acqrel & memop == MemOp_STORE;
    accdesc.exclusive = true;
    accdesc.read = memop == MemOp_LOAD;
    accdesc.write = memop == MemOp_STORE;
    accdesc.pan = true;
    accdesc.tagchecked = tagchecked;
    accdesc.transactional = HaveTME() & TSTATE.depth > 0;
    return(accdesc)
}

val CreateAccDescAtomicOp : forall ('acquire : Bool) ('release : Bool) ('tagchecked : Bool).
  (MemAtomicOp, bool('acquire), bool('release), bool('tagchecked)) -> AccessDescriptor

function CreateAccDescAtomicOp (modop, acquire, release, tagchecked) = {
    accdesc : AccessDescriptor = NewAccDesc(AccessType_GPR);
    accdesc.acqsc = acquire;
    accdesc.relsc = release;
    accdesc.atomicop = true;
    accdesc.modop = modop;
    accdesc.read = true;
    accdesc.write = true;
    accdesc.pan = true;
    accdesc.tagchecked = tagchecked;
    accdesc.transactional = HaveTME() & TSTATE.depth > 0;
    return(accdesc)
}

val CreateAccDescAcqRel : forall ('tagchecked : Bool).
  (MemOp, bool('tagchecked)) -> AccessDescriptor

function CreateAccDescAcqRel (memop, tagchecked) = {
    accdesc : AccessDescriptor = NewAccDesc(AccessType_GPR);
    accdesc.acqsc = memop == MemOp_LOAD;
    accdesc.relsc = memop == MemOp_STORE;
    accdesc.read = memop == MemOp_LOAD;
    accdesc.write = memop == MemOp_STORE;
    accdesc.pan = true;
    accdesc.tagchecked = tagchecked;
    accdesc.transactional = HaveTME() & TSTATE.depth > 0;
    return(accdesc)
}

val CreateAccDescLOR : forall ('tagchecked : Bool).
  (MemOp, bool('tagchecked)) -> AccessDescriptor

function CreateAccDescLOR (memop, tagchecked) = {
    accdesc : AccessDescriptor = NewAccDesc(AccessType_GPR);
    accdesc.acqsc = memop == MemOp_LOAD;
    accdesc.relsc = memop == MemOp_STORE;
    accdesc.limitedordered = true;
    accdesc.read = memop == MemOp_LOAD;
    accdesc.write = memop == MemOp_STORE;
    accdesc.pan = true;
    accdesc.tagchecked = tagchecked;
    accdesc.transactional = HaveTME() & TSTATE.depth > 0;
    return(accdesc)
}

val CreateAccDescLDAcqPC : forall ('tagchecked : Bool).
  bool('tagchecked) -> AccessDescriptor

function CreateAccDescLDAcqPC tagchecked = {
    accdesc : AccessDescriptor = NewAccDesc(AccessType_GPR);
    accdesc.acqpc = true;
    accdesc.read = true;
    accdesc.pan = true;
    accdesc.tagchecked = tagchecked;
    accdesc.transactional = HaveTME() & TSTATE.depth > 0;
    return(accdesc)
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

val HaveAtomicExt : unit -> bool

function HaveAtomicExt () = {
    return(IsFeatureImplemented(FEAT_LSE))
}

val FPZero : forall 'N, 'N in {16, 32, 64}. (bits(1), int('N)) -> bits('N)

function FPZero (sign, N) = {
    assert(N == 16 | N == 32 | N == 64);
    let 'E = if N == 16 then 5 else if N == 32 then 8 else 11;
    let 'F = N - (E + 1);
    let exp : bits('E) = Zeros(E);
    let frac : bits('F) = Zeros(F);
    let result : bits(1 + 'E + 'F) = (sign @ exp) @ frac;
    return(result)
}

val HaveAltFP : unit -> bool

function HaveAltFP () = {
    return(IsFeatureImplemented(FEAT_AFP))
}

val FPDecodeRounding : bits(2) -> FPRounding

function FPDecodeRounding rmode = {
    match rmode {
      0b00 => {
          return(FPRounding_TIEEVEN)
      },
      0b01 => {
          return(FPRounding_POSINF)
      },
      0b10 => {
          return(FPRounding_NEGINF)
      },
      0b11 => {
          return(FPRounding_ZERO)
      },
      _ => {
          return(undefined : FPRounding)
      }
    }
}

val FPRoundingMode : FPCRType -> FPRounding

val FixedToFP : forall 'M 'N 'fbits ('is_unsigned : Bool),
  ('N in {16, 32, 64} & 'M in {16, 32, 64}).
  (bits('M), int('fbits), bool('is_unsigned), FPCRType, FPRounding, int('N)) -> bits('N)

val FPAbs : forall 'N, 'N in {16, 32, 64}. bits('N) -> bits('N)

function FPAbs op = {
    fptype : FPType = undefined;
    assert('N == 16 | 'N == 32 | 'N == 64);
    if not_bool(UsingAArch32()) & HaveAltFP() then {
        let fpcr : FPCRType = FPCR_read();
        if fpcr[AH] == 0b1 then {
            let (__tup_0 : FPType, _, _) = FPUnpack__1(op, fpcr, false) in
              {
                  fptype = __tup_0
              };
            if fptype == FPType_SNaN | fptype == FPType_QNaN then {
                return(op)
            };
            ()
        };
        ()
    };
    return(0b0 @ op['N - 2 .. 0])
}

val FPAdd : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPCompare : forall 'N ('signal_nans : Bool),
  'N in {16, 32, 64}.
  (bits('N), bits('N), bool('signal_nans), FPCRType) -> bits(4)

val FPConvert__1 : forall 'M 'N,
  ('M in {16, 32, 64} & 'N in {16, 32, 64}).
  (bits('N), FPCRType, int('M)) -> bits('M)

val FPDiv : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPMax : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPMaxNum : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPMin : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPMinNum : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPMul : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPMulAdd : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), bits('N), FPCRType) -> bits('N)

val FPNeg : forall 'N, 'N in {16, 32, 64}. bits('N) -> bits('N)

function FPNeg op = {
    fptype : FPType = undefined;
    assert('N == 16 | 'N == 32 | 'N == 64);
    if not_bool(UsingAArch32()) & HaveAltFP() then {
        let fpcr : FPCRType = FPCR_read();
        if fpcr[AH] == 0b1 then {
            let (__tup_0 : FPType, _, _) = FPUnpack__1(op, fpcr, false) in
              {
                  fptype = __tup_0
              };
            if fptype == FPType_SNaN | fptype == FPType_QNaN then {
                return(op)
            };
            ()
        };
        ()
    };
    return(not_vec([op['N - 1]]) @ op['N - 2 .. 0])
}

val FPRoundInt : forall 'N ('exact : Bool),
  'N in {16, 32, 64}.
  (bits('N), FPCRType, FPRounding, bool('exact)) -> bits('N)

val FPRoundIntN : forall 'N 'intsize, 'N in {32, 64}.
  (bits('N), FPCRType, FPRounding, int('intsize)) -> bits('N)

val FPSqrt : forall 'N, 'N in {16, 32, 64}. (bits('N), FPCRType) -> bits('N)

val FPSub : forall 'N, 'N in {16, 32, 64}.
  (bits('N), bits('N), FPCRType) -> bits('N)

val FPToFixed : forall 'M 'N 'fbits ('is_unsigned : Bool),
  ('N in {16, 32, 64} & 'M in {16, 32, 64}).
  (bits('N), int('fbits), bool('is_unsigned), FPCRType, FPRounding, int('M)) -> bits('M)

val FPToFixedJS : forall ('Is64 : Bool) 'M 'N,
  ('M == 64 & 'N == 32).
  (bits('M), FPCRType, bool('Is64), int('N)) -> (bits('N), bits(1))

val VFPExpandImm : forall 'N, 'N in {16, 32, 64}. (bits(8), int('N)) -> bits('N)

function VFPExpandImm (imm8, N) = {
    assert(N == 16 | N == 32 | N == 64);
    let 'E = if N == 16 then 5 else if N == 32 then 8 else 11;
    let 'F = N - E - 1;
    let sign : bits(1) = [imm8[7]];
    let exp : bits(1 + ('E - 3) + 2) = (not_vec([imm8[6]]) @ Replicate([imm8[6]], E - 3)) @ imm8[5 .. 4];
    let frac : bits(4 + ('F - 4)) = imm8[3 .. 0] @ Zeros(F - 4);
    let result : bits(1 + (1 + ('E - 3) + 2) + (4 + ('F - 4))) = (sign @ exp) @ frac;
    return(result)
}

val HaveFJCVTZSExt : unit -> bool

function HaveFJCVTZSExt () = {
    return(IsFeatureImplemented(FEAT_JSCVT))
}

val HaveFlagManipulateExt : unit -> bool

function HaveFlagManipulateExt () = {
    return(IsFeatureImplemented(FEAT_FlagM))
}

val HaveSelfHostedTrace : unit -> bool

function HaveSelfHostedTrace () = {
    return(IsFeatureImplemented(FEAT_TRF))
}

val HaveFrintExt : unit -> bool

function HaveFrintExt () = {
    return(IsFeatureImplemented(FEAT_FRINTTS))
}

val HaveDGHExt : unit -> bool

function HaveDGHExt () = {
    return(IsFeatureImplemented(FEAT_DGH))
}

val HaveBF16Ext : unit -> bool

function HaveBF16Ext () = {
    return(IsFeatureImplemented(FEAT_BF16))
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

val Vpart_read : forall 'n 'part 'width,
  'width >= 0.
  (int('n), int('part), int('width)) -> bits('width)

function Vpart_read (n, part, width) = {
    assert(n >= 0 & n <= 31);
    assert(part == 0 | part == 1);
    if part == 0 then {
        assert(width < 128);
        assert(constraint('width in {8, 16, 32, 64, 128}));
        return(V_read(n, width))
    } else {
        assert(width == 32 | width == 64);
        let vreg : bits(128) = V_read(n, 128);
        return(vreg[width * 2 - 1 .. width])
    }
}

val Vpart_set : forall 'n 'part 'width,
  'width >= 0.
  (int('n), int('part), int('width), bits('width)) -> unit

function Vpart_set (n, part, width, value_name) = {
    assert(n >= 0 & n <= 31);
    assert(part == 0 | part == 1);
    if part == 0 then {
        assert(width < 128);
        assert(constraint('width in {8, 16, 32, 64, 128}));
        V_set(n, width) = value_name
    } else {
        assert(width == 64);
        let vreg : bits(64) = V_read(n, 64);
        V_set(n, 128) = value_name[63 .. 0] @ vreg
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

val FPConvertBF__1 : forall 'N, 'N == 32.
  (bits('N), FPCRType) -> bits(div('N, 2))

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

