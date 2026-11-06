#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Independent trace oracles and memory/model gates. No native or GPU code runs."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('descriptor_cache', Path(__file__).resolve().parents[1] / 'tools/analyze_descriptor_cache.py')
cache = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cache)


def oracle(trace, buckets, ways):
    """Dense reference: flush an epoch, then search all resident entries as explicit dictionaries."""
    resident = [[None for _ in range(ways)] for _ in range(buckets)]
    hits = misses = collisions = 0
    frame = 0
    for event in trace['events']:
        if event['frame'] != frame:
            resident = [[None for _ in range(ways)] for _ in range(buckets)]
            frame = event['frame']
        if trace['descriptor_family'] == 'buffer':
            words = [event['buffer'], event['address'], event['offset'], event['range'],
                     event['format'] + event['type'] * 2 ** 32, event['bytes']]
        else:
            words = [event['view'], event['sampler'], event['layout'] + event['type'] * 2 ** 32]
        hashed = 14695981039346656037
        for word in words:
            hashed = ((hashed ^ word) * 1099511628211) % (2 ** 64)
        bucket = resident[(hashed ^ (hashed // 2 ** 32)) % buckets]
        if event in bucket:
            hits += 1
            continue
        misses += 1
        for index, stored in enumerate(bucket):
            if stored is None or ('device' in event and stored['device'] != event['device']):
                bucket[index] = dict(event)
                break
        else:
            collisions += 1
            bucket[ways - 1 - hashed // (2 ** 64 // ways)] = dict(event)
    return hits, misses, collisions


class Replay(unittest.TestCase):
    def test_exact_epoch_hits_and_unique_pressure(self):
        for family in cache.FIELDS:
            trace = cache.synthetic('repeated', family, requests=80, frames=4)
            for ways in (1, 2, 4):
                result = cache.replay(trace, 8, ways)
                self.assertEqual((result['hits'], result['misses'], result['collision_replacements']), (76, 4, 0))
                self.assertEqual(result['empty_fills'] + result['stale_fills'], 4)
                self.assertLessEqual(result['key_probes'], 80 * ways)
            unique = cache.replay(cache.synthetic('unique', family, requests=80, frames=1), 1, 4)
            self.assertEqual((unique['hits'], unique['misses'], unique['collision_replacements']), (0, 80, 76))

    def test_differential_oracle_with_epochs_devices_and_collisions(self):
        randomizer = random.Random(31017)
        for family in cache.FIELDS:
            trace = cache.synthetic('working_set', family, requests=512, working_set=19, frames=8)
            for event in trace['events']:
                if family == 'buffer':
                    event['offset'] = randomizer.randrange(3) * 64
                    event['device'] = randomizer.randrange(1, 3)
                else:
                    event['sampler'] = randomizer.randrange(3) * 32
            for buckets in (1, 2, 8, 32):
                for ways in (1, 2, 4):
                    with self.subTest(family=family, buckets=buckets, ways=ways):
                        row = cache.replay(trace, buckets, ways)
                        self.assertEqual((row['hits'], row['misses'], row['collision_replacements']), oracle(trace, buckets, ways))
                        self.assertEqual(row['misses'], row['empty_fills'] + row['stale_fills'] + row['collision_replacements'])
                        self.assertEqual(row['hits'] + row['misses'], 512)

    def test_every_runtime_key_field_prevents_reuse(self):
        for family in cache.FIELDS:
            original = cache.synthetic('repeated', family, requests=3, frames=1)
            for field in cache.FIELDS[family]:
                if family == 'image' and field == 'bytes':
                    continue  # Fixed by descriptor type on the device, not part of the image key.
                trace = copy.deepcopy(original)
                trace['events'][1][field] += 1
                if field == 'frame':
                    trace['events'][2]['frame'] = 3
                if field == 'bytes':
                    trace['payload_capacity_bytes'] = 65
                row = cache.replay(trace, 1, 1)
                self.assertEqual((row['hits'], row['misses']), (0, 3), (family, field))

    def test_collision_generator_is_deterministic_and_shared_across_bucket_sweep(self):
        for family in cache.FIELDS:
            trace = cache.synthetic('collision', family, requests=128, working_set=5, frames=1, collision_buckets=64)
            self.assertEqual(trace, cache.synthetic('collision', family, 128, 5, 1, 64))
            for buckets in (1, 2, 16, 64):
                row = cache.replay(trace, buckets, 4)
                self.assertGreater(row['collision_replacements'], 0)
                self.assertEqual((row['hits'], row['misses'], row['collision_replacements']), oracle(trace, buckets, 4))


class Sizing(unittest.TestCase):
    def test_runtime_stride_payload_and_metadata(self):
        for family, metadata, alignment in [('buffer', 64, 8), ('image', 32, 32)]:
            trace = cache.synthetic('repeated', family, requests=4, frames=1)
            trace['payload_capacity_bytes'] = 65
            row = cache.replay(trace, 16, 2)
            stride = (metadata + 65 + alignment - 1) // alignment * alignment
            self.assertEqual(row['allocated_bytes'], 32 * stride)
            self.assertEqual(row['metadata_bytes'], 32 * metadata)
            self.assertEqual(row['payload_bytes'], 32 * 65)
            self.assertEqual(row['allocated_bytes'], sum(row[name] for name in ('metadata_bytes', 'payload_bytes', 'padding_bytes')))
        del trace['payload_capacity_bytes']
        self.assertEqual(cache.analyze(trace)['payload_capacity_basis'], 'maximum_observed_trace_floor')

    def test_pareto_choices_include_ties_and_exclude_only_strictly_dominated(self):
        rows = [{'allocated_bytes': memory, 'misses': misses} for memory, misses in [(1, 9), (2, 5), (3, 5), (4, 2), (2, 5), (8, 8)]]
        self.assertEqual(cache.nondominated(rows), [0, 1, 3, 4])
        trace = cache.synthetic('working_set', requests=128, working_set=8, frames=1)
        result = cache.analyze(trace, [1, 4, 16], [1, 2, 4])
        self.assertTrue(result['nondominated_choices'])
        self.assertNotIn('fastest', result)
        self.assertIn('No universal', result['conclusion'])

    def test_measured_costs_produce_a_model_only(self):
        trace = cache.synthetic('repeated', requests=10, frames=1)
        result = cache.analyze(trace, [1], [2], cache.measured_costs(2, 100, 'fixture CPU timestamp samples'))
        self.assertEqual(result['results'][0]['modeled_time_ns'], 118)
        self.assertEqual(result['cost_model']['status'], 'modeled_from_measured_costs')
        self.assertEqual(result['trace_kind'], 'synthetic')
        for bad in (None, True, -1, float('nan'), float('inf'), 10 ** 1000):
            with self.assertRaises(ValueError):
                cache.measured_costs(bad, 10, 'test')
        with self.assertRaises(ValueError):
            cache.measured_costs(1, 10, '')
        with self.assertRaisesRegex(ValueError, 'modeled time'):
            cache.analyze(trace, [1], [2], cache.measured_costs(1e308, 1e308, 'test'))


class InputOutput(unittest.TestCase):
    def test_fixed_runtime_hash_vectors(self):
        for family, expected in [('buffer', 0x4E374A58BCC319E5), ('image', 0x6D970BB3E916AE38)]:
            trace = cache.synthetic('repeated', family, requests=4, frames=1)
            key = tuple(trace['events'][0][name] for name in cache.FIELDS[family])
            self.assertEqual(cache.descriptor_hash(family, key), expected)

    def test_invalid_epochs_geometry_keys_and_sizes(self):
        trace = cache.synthetic('repeated', requests=4, frames=1)
        for buckets, ways in [(3, 2), (0, 1), (2, 3), (True, 2)]:
            with self.assertRaises(ValueError):
                cache.replay(trace, buckets, ways)
        for field, value in [('frame', 0), ('bytes', 0), ('offset', -1), ('format', 2 ** 32), ('device', True)]:
            changed = copy.deepcopy(trace)
            changed['events'][0][field] = value
            with self.assertRaises(ValueError):
                cache.prepare(changed)
        trace['events'][0]['frame'] = 2
        with self.assertRaisesRegex(ValueError, 'monotonic'):
            cache.prepare(trace)
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            cache.analyze(cache.synthetic('repeated', requests=4, frames=1), artifact_sha256='invalid')
        image = cache.synthetic('repeated', 'image', requests=4, frames=1)
        image['events'][0]['bytes'] = 32
        with self.assertRaisesRegex(ValueError, 'fixed byte size'):
            cache.prepare(image)
        for text in ['{"a":1,"a":2}', '{"x":NaN}', '{"x":Infinity}']:
            with self.assertRaises(ValueError):
                cache.load_json(text)

    def test_cli_preserves_input_artifact_hash_and_labels_synthetic_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace, report = root / 'keys.json', root / 'report.json'
            args = ['analyze_descriptor_cache.py', '--synthetic', 'working_set', '--requests', '128',
                    '--working-set', '8', '--frames', '2', '--buckets', '1,16', '--ways', '1,2,4',
                    '--write-trace', str(trace), '--output', str(report)]
            with patch.object(sys, 'argv', args), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cache.main(), 0)
            first = json.loads(report.read_text())
            self.assertEqual(first['trace_artifact_sha256'], hashlib.sha256(trace.read_bytes()).hexdigest())
            self.assertEqual(first['trace_kind'], 'synthetic')
            with patch.object(sys, 'argv', ['analyze_descriptor_cache.py', '--trace', str(trace), '--buckets', '1,16', '--ways', '1,2,4']), contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(cache.main(), 0)
            self.assertEqual(first, json.loads(out.getvalue()))


if __name__ == '__main__':
    unittest.main()
