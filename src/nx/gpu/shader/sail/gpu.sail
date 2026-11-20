/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* What every GPU ISA's Sail shares (sailgen --gpu, tools/sailgen/README.md's "GPU mode"): the operations of a 32-bit
   word each of them has, which the back end (tools/sailgen/spirv.h) lowers as one instruction each, and the truth
   tables of three inputs. Sail's own operators (+, -, *, &, |, @, slices, ZeroExtend, the comparisons of UInt and SInt)
   need no val */

/* the bits set */
val PopCount : bits(32) -> bits(32)
/* the bits in the other order */
val BitReverse32 : bits(32) -> bits(32)
/* the highest bit that is 1 (FindUMsb), 0xffffffff where there is none */
val HighestSetBit32 : bits(32) -> bits(32)
/* the highest bit that differs from the sign (FindSMsb), 0xffffffff where there is none */
val HighestSignBit32 : bits(32) -> bits(32)

/* A truth table of three inputs, lut's bit 4x + 2y + z the result for those bits of x, y and z (x the 0xf0 input,
   y 0xcc and z 0xaa): an operand where it is one, an AND or an OR of two where it is one of those, else the OR of a
   term for each minterm, in the order of the minterms. Of a word's bits */
function Lut3(lut : bits(8), x : bits(32), y : bits(32), z : bits(32)) -> bits(32) =
    match lut {
        0x00 => 0x00000000,
        0xff => 0xffffffff,
        0xf0 => x,
        0xcc => y,
        0xaa => z,
        0xc0 => x & y,
        0xa0 => x & z,
        0x88 => y & z,
        0xfc => x | y,
        0xfa => x | z,
        0xee => y | z,
        /* the tables of one or two operations more, each the operations that make it (checked against the minterms
           for all 256 tables: tests/shader_sail_sm86.c) */
        0x3c => EOR(x, y),
        0x5a => EOR(x, z),
        0x66 => EOR(y, z),
        0xc3 => not_vec(EOR(x, y)),
        0xa5 => not_vec(EOR(x, z)),
        0x99 => not_vec(EOR(y, z)),
        0x96 => EOR(EOR(x, y), z),
        0x69 => not_vec(EOR(EOR(x, y), z)),
        0x0f => not_vec(x),
        0x33 => not_vec(y),
        0x55 => not_vec(z),
        0x30 => x & not_vec(y),
        0x0c => not_vec(x) & y,
        0x50 => x & not_vec(z),
        0x0a => not_vec(x) & z,
        0x44 => y & not_vec(z),
        0x22 => not_vec(y) & z,
        0x80 => (x & y) & z,
        0xfe => (x | y) | z,
        0xf8 => x | (y & z),
        0xec => y | (x & z),
        0xea => z | (x & y),
        0xe0 => x & (y | z),
        0xc8 => y & (x | z),
        0xa8 => z & (x | y),
        0x78 => EOR(x, y & z),
        0x6c => EOR(y, x & z),
        0x6a => EOR(z, x & y),
        0xca => (x & y) | (not_vec(x) & z),
        _ => {
            let nx = not_vec(x);
            let ny = not_vec(y);
            let nz = not_vec(z);
            var result : bits(32) = x;
            var first : bool = true;
            foreach (i from 0 to 7) {
                if lut[i] == 0b1 then {
                    let term = ((if MOD(DIV(i, 4), 2) == 1 then x else nx) & (if MOD(DIV(i, 2), 2) == 1 then y else ny))
                               & (if MOD(i, 2) == 1 then z else nz);
                    if first then { result = term } else { result = result | term };
                    first = false
                }
            };
            result
        }
    }

/* the same of three bools */
function Lut3Bool(lut : bits(8), x : bool, y : bool, z : bool) -> bool =
    match lut {
        0x00 => false,
        0xff => true,
        0xf0 => x,
        0xcc => y,
        0xaa => z,
        0xc0 => x & y,
        0xa0 => x & z,
        0x88 => y & z,
        0xfc => x | y,
        0xfa => x | z,
        0xee => y | z,
        0x3c => x != y,
        0x5a => x != z,
        0x66 => y != z,
        0xc3 => x == y,
        0xa5 => x == z,
        0x99 => y == z,
        0x96 => (x != y) != z,
        0x69 => (x != y) == z,
        0x0f => not_bool(x),
        0x33 => not_bool(y),
        0x55 => not_bool(z),
        0x30 => x & not_bool(y),
        0x0c => not_bool(x) & y,
        0x50 => x & not_bool(z),
        0x0a => not_bool(x) & z,
        0x44 => y & not_bool(z),
        0x22 => not_bool(y) & z,
        0x80 => (x & y) & z,
        0xfe => (x | y) | z,
        0xf8 => x | (y & z),
        0xec => y | (x & z),
        0xea => z | (x & y),
        0xe0 => x & (y | z),
        0xc8 => y & (x | z),
        0xa8 => z & (x | y),
        0x78 => x != (y & z),
        0x6c => y != (x & z),
        0x6a => z != (x & y),
        0xca => (x & y) | (not_bool(x) & z),
        _ => {
            let nx = not_bool(x);
            let ny = not_bool(y);
            let nz = not_bool(z);
            var result : bool = x;
            var first : bool = true;
            foreach (i from 0 to 7) {
                if lut[i] == 0b1 then {
                    let term = ((if MOD(DIV(i, 4), 2) == 1 then x else nx) & (if MOD(DIV(i, 2), 2) == 1 then y else ny))
                               & (if MOD(i, 2) == 1 then z else nz);
                    if first then { result = term } else { result = result | term };
                    first = false
                }
            };
            result
        }
    }
