#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check integer-vector half rounding against exact rational rounding for every finite encoding.

Run: python3 tests/llvm_perf/half_round_math.py
NaN payloads and flags use the full-state C oracle in llvm_round.
"""
from fractions import Fraction
import json
import math
import struct


def reference(bits, mode, flush):
    magnitude = bits & 0x7fff
    exponent, fraction = magnitude >> 10, magnitude & 1023
    sign = bits & 0x8000
    if not exponent and flush:
        return sign
    scale = (exponent or 1) - 25
    significand = fraction + (1024 if exponent else 0)
    value = Fraction(significand << scale) if scale >= 0 else Fraction(significand, 1 << -scale)
    if sign:
        value = -value
    if mode == 0:
        integer = round(value)
    elif mode == 1:
        integer = math.ceil(value)
    elif mode == 2:
        integer = math.floor(value)
    elif mode == 3:
        integer = math.trunc(value)
    else:
        integer = math.floor(abs(value) + Fraction(1, 2)) * (-1 if sign else 1)
    return struct.unpack('>H', struct.pack('>e', integer))[0] if integer else sign


def candidate(bits, mode, flush):
    sign, magnitude = bits & 0x8000, bits & 0x7fff
    if flush and magnitude < 1024:
        magnitude = 0
    exponent = magnitude >> 10
    if exponent >= 25:
        return magnitude | sign
    if exponent < 15:
        inexact = magnitude != 0
        nearest, away = magnitude > 0x3800, magnitude >= 0x3800
        base, unit = 0, 0x3c00
    else:
        count = 25 - exponent
        unit = 1 << count
        rest = magnitude & (unit - 1)
        base = magnitude & ~(unit - 1)
        inexact = rest != 0
        nearest = rest > unit // 2 or rest == unit // 2 and (base >> count) & 1
        away = rest >= unit // 2
    up = (nearest, not sign, bool(sign), False, away)[mode] and inexact
    return sign | (base + (unit if up else 0))


def main():
    count = 0
    for bits in range(65536):
        if bits & 0x7c00 == 0x7c00:
            continue
        for mode in range(5):
            for flush in (False, True):
                assert candidate(bits, mode, flush) == reference(bits, mode, flush), (hex(bits), mode, flush)
                count += 1
    print(json.dumps(dict(cases=count, rounding_modes=5, flush_modes=2,
                         claim='Integer half rounding matches exact rational rounding for every finite encoding.')))


if __name__ == '__main__':
    main()
