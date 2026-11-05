#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check the exact finite FP16 ordering behind vector comparisons and min/max.

Run: python3 tests/llvm_perf/half_order_math.py
Monotonic keys establish every finite pair's order. Signed-zero ties are handled
separately by the emitter. llvm_vfp_minmax checks NaN priority and exception flags.
"""
from fractions import Fraction
import json


def value(bits):
    exponent, fraction = bits >> 10 & 31, bits & 1023
    significand = fraction + (1024 if exponent else 0)
    scale = (exponent or 1) - 25
    number = Fraction(significand << scale) if scale >= 0 else Fraction(significand, 1 << -scale)
    return -number if bits & 0x8000 else number


def key(bits):
    return bits ^ (0xffff if bits & 0x8000 else 0x8000)


def main():
    encodings = [bits for bits in range(65536) if bits & 0x7c00 != 0x7c00]
    encodings.sort(key=key)
    previous = None
    equal = 0
    for bits in encodings:
        current = value(bits)
        if previous is not None:
            assert current >= previous
            equal += current == previous
        previous = current
    assert equal == 1  # The sole equal finite pair is negative zero and positive zero.
    for first in (0, 0x8000):
        for second in (0, 0x8000):
            assert (first | second) == (0x8000 if first or second else 0)
            assert (first & second) == (0x8000 if first and second else 0)
    print(json.dumps(dict(finite_encodings=len(encodings), equal_adjacent_pairs=equal,
                         ordered_pairs=len(encodings) ** 2,
                         claim='Monotonic keys preserve every finite half pair, with signed-zero ties explicit.')))


if __name__ == '__main__':
    main()
