#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compile probes must use complete, checksummed shader/key associations from a private cache snapshot."""
import ctypes
import ctypes.util
import importlib.util
import json
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('probe', Path(__file__).resolve().parents[1] / 'tools/prepare_pipeline_probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
bench_spec = importlib.util.spec_from_file_location('bench', Path(__file__).resolve().parents[1] / 'tools/bench_pipeline_compile.py')
bench = importlib.util.module_from_spec(bench_spec)
bench_spec.loader.exec_module(bench)
KEY = 'guest=5 sizes=64,152,64,32; test sizes=0,152 vertex_bounds=0'
CODE = struct.pack('<5I', 0x07230203, 0x10600, 0, 1, 0)


def header(key=KEY):
    data = b'SNXVKSH\n' + struct.pack('<II', 3, len(key)) + key.encode()
    return data + bytes(-len(data) % 8)


def record(kind, payload):
    payload += bytes(-len(payload) % 8)
    return struct.pack('<IIQ', kind, len(payload), probe.checksum(kind, payload)) + payload


def shader():
    metadata = bytearray(152)
    struct.pack_into('<II', metadata, 36, 0, 5)
    return record(1, metadata + CODE)


def pipeline(shader_record, expected=None):
    check = struct.unpack_from('<Q', shader_record, 8)[0] if expected is None else expected
    payload = struct.pack('<5Q5II', check, 0, 0, 0, check, 1, 0, 0, 0, 1, 64) + bytes(64)
    return record(2, payload)


def compress(data):
    name = ctypes.util.find_library('zstd')
    if not name:
        raise unittest.SkipTest('libzstd not installed')
    zstd = ctypes.CDLL(name)
    zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
    zstd.ZSTD_compressBound.restype = ctypes.c_size_t
    size = zstd.ZSTD_compressBound(len(data))
    out = ctypes.create_string_buffer(size)
    zstd.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    zstd.ZSTD_compress.restype = ctypes.c_size_t
    actual = zstd.ZSTD_compress(out, size, data, len(data), 1)
    if actual > size:
        raise ValueError('test compression failed')
    return out.raw[:actual]


class ProbeInputs(unittest.TestCase):
    def test_complete_pipeline_preserves_exact_stages_and_key(self):
        s = shader()
        key, pipelines = probe.extract(header() + s + pipeline(s))
        self.assertEqual(key, KEY)
        self.assertEqual(len(pipelines), 1)
        self.assertTrue(pipelines[0]['complete'])
        self.assertEqual(pipelines[0]['stages'], [CODE, b'', b'', b'', CODE])
        data = probe.package(key, pipelines[0])
        self.assertEqual(data[:8], probe.MAGIC)
        self.assertEqual(struct.unpack_from('<7I', data, 8), (len(KEY), 64, 5, 0, 0, 0, 5))
        self.assertEqual(data[36:36 + len(KEY)], KEY.encode())
        self.assertEqual(data[-2 * len(CODE):], CODE + CODE)

    def test_incomplete_or_corrupt_tail_is_not_used(self):
        s, p = shader(), pipeline(shader())
        for tail in (p[:-1], p[:20] + b'wrong' + p[25:]):
            with self.subTest(tail=tail):
                self.assertFalse(probe.extract(header() + s + tail)[1])

    def test_shader_reference_check_must_match(self):
        s = shader()
        _, pipelines = probe.extract(header() + s + pipeline(s, expected=42))
        self.assertFalse(pipelines[0]['complete'])
        with self.assertRaises(ValueError):
            probe.package(KEY, pipelines[0])

    def test_missing_shipped_references_cannot_compete(self):
        shipped_shader = shader()
        reference = record(4, struct.pack('<QQ', len(header()), struct.unpack_from('<Q', shipped_shader, 8)[0]))
        data = header() + reference + pipeline(reference)
        self.assertFalse(probe.extract(data)[1][0]['complete'])
        self.assertTrue(probe.extract(data, header() + shipped_shader)[1][0]['complete'])
        with self.assertRaises(ValueError):
            probe.extract(data, header(KEY + 'different') + shipped_shader)

    def test_packed_units_validate_completeness_before_exposing_records(self):
        s = shader()
        raw = s + pipeline(s)
        packed = compress(raw)
        unit = record(6, struct.pack('<4I', len(raw), len(packed), 2, 1) + packed)
        self.assertTrue(probe.extract(header() + unit)[1][0]['complete'])
        bad = record(6, struct.pack('<4I', len(raw), len(packed), 3, 1) + packed)
        with self.assertRaises(ValueError):
            probe.extract(header() + bad)

    def test_native_module_handles_cannot_escape_cache(self):
        s = shader()
        p = bytearray(pipeline(s)[16:])
        p[64] = 1
        with self.assertRaises(ValueError):
            probe.extract(header() + s + record(2, p))

    def test_compile_result_requires_matching_flags_inputs_and_empty_cache(self):
        value = {'ns': 100, 'disable_optimization': 0, 'shader_words': 42, 'empty_application_cache': 1,
                 'rendered': 0, 'application_cache_hits': 0, 'validation_messages': 0, 'vendor': 1, 'device': 2, 'driver': 3}
        text = 'PIPELINE_COMPILE_PROBE ' + json.dumps(value)
        self.assertEqual(bench.parse(text, 0, 42)['ns'], 100)
        for field, invalid in (('ns', 0), ('disable_optimization', 1), ('shader_words', 43),
                               ('empty_application_cache', 0), ('rendered', 1), ('application_cache_hits', 1),
                               ('validation_messages', 1), ('vendor', 'missing')):
            bad = dict(value, **{field: invalid})
            with self.subTest(field=field), self.assertRaises(ValueError):
                bench.parse('PIPELINE_COMPILE_PROBE ' + json.dumps(bad), 0, 42)
        with self.assertRaises(ValueError):
            bench.parse(text + '\n' + text, 0, 42)

    def test_compile_summary_uses_paired_latencies(self):
        rows = [{'index': 9, 'round': i, 'disable_optimization': mode, 'ns': 100 if mode else 200}
                for i in range(1, 4) for mode in (0, 1)]
        result = bench.summarize(rows)[0]
        self.assertEqual(result['median_normal_over_disabled'], 2)
        self.assertTrue(result['disabled_at_least_3_percent_faster_every_pair'])
        with self.assertRaises(ValueError):
            bench.summarize(rows[:-1])


if __name__ == '__main__':
    unittest.main()
