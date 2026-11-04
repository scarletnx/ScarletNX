#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Prove finite FP16 widening with exact rational arithmetic, independently of the C soft-float helper.

Run: python3 tests/llvm_perf/half_widen_math.py
NaN payloads and exception flags are checked by llvm_half against the exact helper.
"""
from fractions import Fraction
import json
import struct


def power(exponent):
    return Fraction(1 << exponent) if exponent >= 0 else Fraction(1, 1 << -exponent)


def encode_exact(value, sign):
    if not value:
        return sign
    value = abs(value)
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if value < power(exponent):
        exponent -= 1
    significand = value / power(exponent - 23)
    assert significand.denominator == 1
    assert -126 <= exponent <= 127
    assert 1 << 23 <= significand < 1 << 24
    return sign | (exponent + 127) << 23 | int(significand - (1 << 23))


def main():
    count = 0
    for ahp in (False, True):
        for bits in range(65536):
            exponent, fraction = bits >> 10 & 31, bits & 1023
            if exponent == 31 and not ahp:
                continue
            sign = (bits & 0x8000) << 16
            significand = fraction + (1024 if exponent else 0)
            exact = Fraction(significand, 1024) * power((exponent or 1) - 15)
            if sign:
                exact = -exact
            if exponent:
                candidate = sign | ((bits & 0x7fff) << 13) + 0x38000000
            else:
                number = float(Fraction(fraction, 1 << 24))
                candidate = sign | struct.unpack('>I', struct.pack('>f', number))[0]
            assert candidate == encode_exact(exact, sign), (hex(bits), ahp)
            count += 1
    print(json.dumps(dict(finite_encodings=count,precision_bits=11,min_exponent=-24,max_exponent=16,
                         claim='Every finite half value widens exactly, including signed zero and alternate halves.')))


if __name__ == '__main__':
    main()
