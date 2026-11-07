#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Replay explicit descriptor keys to compare bounded cache memory and misses.

Examples:
  python3 tools/analyze_descriptor_cache.py --synthetic working_set --working-set 128
  python3 tools/analyze_descriptor_cache.py --trace descriptor-keys.json --buckets 64,256,1024 --ways 1,2,4

Trace schema scarletnx.descriptor-key-trace.v1 has descriptor_family buffer/image,
trace_kind measured/synthetic, source, and events containing the exact fields in FIELDS.
An optional payload_capacity_bytes sets the slot payload capacity. Without it, the
maximum observed descriptor size is a trace-specific memory floor. No runtime runs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

MASK = (1 << 64) - 1
BASIS = 0xCBF29CE484222325
PRIME = 0x100000001B3
TRACE_SCHEMA = "scarletnx.descriptor-key-trace.v1"
FIELDS = {
    "buffer": ("frame", "device", "buffer", "address", "offset", "range", "format", "type", "bytes"),
    "image": ("frame", "view", "sampler", "layout", "type", "bytes"),
}


def integer(value, name, minimum=0, maximum=MASK):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")
    return value


def load_json(text):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON field: {key}")
            result[key] = value
        return result

    def reject(value):
        raise ValueError(f"non-finite JSON number: {value}")

    return json.loads(text, object_pairs_hook=unique, parse_constant=reject)


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")


def descriptor_hash(family, key):
    """Same whole-word FNV steps and final fold as the runtime, without frame/device."""
    if family == "buffer":
        _, _, buffer, address, offset, length, fmt, kind, size = key
        words = (buffer, address, offset, length, fmt | kind << 32, size)
    else:
        _, view, sampler, layout, kind, _ = key
        words = (view, sampler, layout | kind << 32)
    value = BASIS
    for word in words:
        value = ((value ^ word) * PRIME) & MASK
    return value


def prepare(trace):
    if not isinstance(trace, dict) or trace.get("schema") != TRACE_SCHEMA:
        raise ValueError(f"trace schema must be {TRACE_SCHEMA}")
    family = trace.get("descriptor_family")
    if family not in FIELDS:
        raise ValueError("descriptor_family must be buffer or image")
    if trace.get("trace_kind") not in ("measured", "synthetic"):
        raise ValueError("trace_kind must explicitly be measured or synthetic")
    if not isinstance(trace.get("source"), str) or not trace["source"].strip():
        raise ValueError("trace source must identify where its events came from")
    if not isinstance(trace.get("events"), list) or not trace["events"]:
        raise ValueError("events must be a nonempty list of explicit descriptor keys")
    events, last_frame, sizes = [], 0, {}
    for index, event in enumerate(trace["events"]):
        if not isinstance(event, dict) or set(event) != set(FIELDS[family]):
            raise ValueError(f"event {index} must contain exactly {', '.join(FIELDS[family])}")
        key = tuple(integer(event[name], f"event {index}.{name}",
                            1 if name in ("frame", "bytes") else 0,
                            (1 << 32) - 1 if name in ("format", "type", "layout", "bytes") else MASK)
                    for name in FIELDS[family])
        if key[0] < last_frame:
            raise ValueError("frame epochs must be nonzero and monotonic; an old epoch cannot return")
        last_frame = key[0]
        if family == "image":
            if key[-2] in sizes and sizes[key[-2]] != key[-1]:
                raise ValueError("an image descriptor type must have one fixed byte size on this device")
            sizes[key[-2]] = key[-1]
        events.append((key, descriptor_hash(family, key)))
    maximum = max(key[-1] for key, _ in events)
    capacity = integer(trace.get("payload_capacity_bytes", maximum), "payload_capacity_bytes", 1, (1 << 32) - 1)
    if capacity < maximum:
        raise ValueError("payload capacity is smaller than an observed descriptor")
    return family, events, capacity


def geometry(buckets, ways):
    integer(buckets, "buckets", 1, 1 << 24)
    if buckets & (buckets - 1):
        raise ValueError("bucket counts must be powers of two")
    if type(ways) is not int or ways not in (1, 2, 4):
        raise ValueError("ways must be 1, 2 or 4")


def replay_prepared(family, events, capacity, buckets, ways):
    geometry(buckets, ways)
    # Sparse storage changes only the analyzer's own memory, never the modeled allocation.
    slots, seen = {}, set()
    hits = misses = replacements = stale = empty = probes = repeat_misses = 0
    bits = ways.bit_length() - 1
    for key, hashed in events:
        bucket = (hashed ^ (hashed >> 32)) & (buckets - 1)
        entries = slots.setdefault(bucket, [None] * ways)
        identity = key if family == "buffer" else key[:-1]
        found = False
        for entry in entries:
            probes += 1
            if entry == identity:
                hits += 1
                found = True
                break
        if found:
            continue
        misses += 1
        repeat_misses += identity in seen
        seen.add(identity)
        victim = next((i for i, entry in enumerate(entries)
                       if entry is None or entry[0] != key[0] or
                       (family == "buffer" and entry[1] != key[1])), None)
        if victim is None:
            # For two ways this is exactly hash>>63 ? slot0 : slot1 in the runtime.
            # Four ways is an explicit candidate extension, not a claim about current code.
            victim = ways - 1 - (hashed >> (64 - bits)) if bits else 0
            replacements += 1
        elif entries[victim] is None:
            empty += 1
        else:
            stale += 1
        entries[victim] = identity
    metadata = 64 if family == "buffer" else 32
    alignment = 8 if family == "buffer" else 32
    stride = (metadata + capacity + alignment - 1) // alignment * alignment
    count = buckets * ways
    runtime_ways = 2 if family == "buffer" else 1
    return {"buckets": buckets, "ways": ways, "slots": count, "requests": len(events),
            "hits": hits, "misses": misses, "miss_fraction": misses / len(events),
            "first_seen_misses": misses - repeat_misses, "repeat_misses": repeat_misses,
            "collision_replacements": replacements, "empty_fills": empty, "stale_fills": stale,
            "key_probes": probes, "max_key_probes_per_request": ways,
            "metadata_bytes": count * metadata, "payload_bytes": count * capacity,
            "padding_bytes": count * (stride - metadata - capacity), "allocated_bytes": count * stride,
            "slot_stride": stride, "slot_payload_capacity": capacity,
            "policy": "current_runtime" if ways == runtime_ways else "bounded_associative_candidate",
            "matches_current_runtime_geometry": ways == runtime_ways and buckets == (1024 if family == "buffer" else 4096)}


def replay(trace, buckets, ways):
    return replay_prepared(*prepare(trace), buckets, ways)


def nondominated(results):
    return [index for index, row in enumerate(results) if not any(
        other["allocated_bytes"] <= row["allocated_bytes"] and other["misses"] <= row["misses"] and
        (other["allocated_bytes"] < row["allocated_bytes"] or other["misses"] < row["misses"])
        for other in results)]


def measured_costs(hit_ns, miss_ns, source):
    if not isinstance(source, str) or not source.strip():
        raise ValueError("measured cost source is required")
    for value in (hit_ns, miss_ns):
        try:
            valid = not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value) and value >= 0
        except OverflowError:
            valid = False
        if not valid:
            raise ValueError("measured hit/miss costs must be finite nonnegative nanoseconds")
    return {"hit_ns": hit_ns, "miss_ns": miss_ns, "source": source}


def analyze(trace, buckets=(16, 64, 256, 1024, 4096), ways=(1, 2, 4), costs=None, artifact_sha256=None):
    family, events, capacity = prepare(trace)
    if not buckets or not ways:
        raise ValueError("bucket and way sweeps must not be empty")
    if artifact_sha256 is not None and (not isinstance(artifact_sha256, str) or len(artifact_sha256) != 64 or
                                      any(ch not in "0123456789abcdef" for ch in artifact_sha256)):
        raise ValueError("trace artifact SHA256 must contain 64 lowercase hexadecimal digits")
    results = [replay_prepared(family, events, capacity, b, w) for b in sorted(set(buckets)) for w in sorted(set(ways))]
    if costs is not None:
        costs = measured_costs(costs.get("hit_ns"), costs.get("miss_ns"), costs.get("source"))
        for row in results:
            row["modeled_time_ns"] = row["hits"] * costs["hit_ns"] + row["misses"] * costs["miss_ns"]
            if not math.isfinite(row["modeled_time_ns"]):
                raise ValueError("modeled time exceeds the finite number range")
    frontier = nondominated(results)
    for index, row in enumerate(results):
        row["nondominated_memory_misses"] = index in frontier
    return {"schema": "scarletnx.descriptor-cache-analysis.v1", "status": "trace_replay",
            "trace_kind": trace["trace_kind"], "source": trace["source"], "descriptor_family": family,
            "trace_artifact_sha256": artifact_sha256 or hashlib.sha256(canonical(trace)).hexdigest(),
            "trace_hash_basis": "file_bytes" if artifact_sha256 else "canonical_json",
            "requests": len(events), "frame_epochs": len({key[0] for key, _ in events}),
            "payload_capacity_basis": "explicit" if "payload_capacity_bytes" in trace else "maximum_observed_trace_floor",
            "memory_basis": "64-bit runtime key layout; opaque payload bytes plus runtime stride alignment",
            "cost_model": {"status": "modeled_from_measured_costs", **costs,
                           "assumption": "the same measured hit and miss costs apply to every candidate; probe and locality costs may change"}
                          if costs else {"status": "unavailable", "reason": "no measured hit/miss costs supplied"},
            "conclusion": "No universal smallest or fastest cache. Compare the memory/miss frontier for this trace; replay and cost models are not measured NVN speedups.",
            "nondominated_choices": [{"buckets": results[i]["buckets"], "ways": results[i]["ways"]} for i in frontier],
            "results": results}


def synthetic(pattern, family="buffer", requests=4096, working_set=8, frames=4, collision_buckets=4096):
    if pattern not in ("repeated", "unique", "working_set", "collision") or family not in FIELDS:
        raise ValueError("unknown synthetic pattern or descriptor family")
    integer(requests, "requests", 1, 2_000_000)
    integer(working_set, "working_set", 1, 4096)
    integer(frames, "frames", 1, requests)
    geometry(collision_buckets, 1)

    def event(identifier, frame=1):
        if family == "buffer":
            return dict(zip(FIELDS[family], (frame, 1, 1000 + identifier, 0x100000 + identifier * 4096, 0, 256, 0, 6, 64)))
        return dict(zip(FIELDS[family], (frame, 0x1000 + identifier * 64, 0x100, 5, 1, 64)))

    identifiers = list(range(working_set))
    if pattern == "collision":
        identifiers, candidate = [], 0
        while len(identifiers) < working_set:
            if candidate >= 20_000_000:
                raise ValueError("collision generator search budget exhausted; reduce the working set or collision buckets")
            key = tuple(event(candidate)[name] for name in FIELDS[family])
            hashed = descriptor_hash(family, key)
            if not ((hashed ^ (hashed >> 32)) & (collision_buckets - 1)):
                identifiers.append(candidate)
            candidate += 1
    events = []
    for index in range(requests):
        identifier = 0 if pattern == "repeated" else index if pattern == "unique" else identifiers[index % working_set]
        events.append(event(identifier, 1 + index * frames // requests))
    return {"schema": TRACE_SCHEMA, "trace_kind": "synthetic", "descriptor_family": family,
            "source": f"deterministic {pattern} pressure generator; no runtime measurements",
            "synthetic_parameters": {"pattern": pattern, "requests": requests, "working_set": working_set,
                                     "frames": frames, "collision_buckets": collision_buckets},
            "payload_capacity_bytes": 64, "events": events}


def csv_integers(value):
    try:
        values = [int(item) for item in value.split(",")]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use comma-separated integers") from exc
    if not values:
        raise argparse.ArgumentTypeError("the list must not be empty")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--trace", type=Path, help="explicit descriptor-key trace JSON")
    group.add_argument("--synthetic", choices=("repeated", "unique", "working_set", "collision"))
    parser.add_argument("--family", choices=tuple(FIELDS), default="buffer", help="synthetic trace family")
    parser.add_argument("--requests", type=int, default=4096)
    parser.add_argument("--working-set", type=int, default=8)
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--collision-buckets", type=int, default=4096)
    parser.add_argument("--buckets", type=csv_integers, default=[16, 64, 256, 1024, 4096])
    parser.add_argument("--ways", type=csv_integers, default=[1, 2, 4])
    parser.add_argument("--measured-hit-ns", type=float)
    parser.add_argument("--measured-miss-ns", type=float)
    parser.add_argument("--cost-source", help="source of the supplied measured costs")
    parser.add_argument("--write-trace", type=Path, help="save the exact synthetic/input trace artifact")
    parser.add_argument("--output", type=Path, help="save analysis JSON; it is also printed")
    args = parser.parse_args()
    try:
        raw = args.trace.read_bytes() if args.trace else canonical(synthetic(
            args.synthetic, args.family, args.requests, args.working_set, args.frames, args.collision_buckets))
        trace = load_json(raw)
        costs = None
        if any(value is not None for value in (args.measured_hit_ns, args.measured_miss_ns, args.cost_source)):
            costs = measured_costs(args.measured_hit_ns, args.measured_miss_ns, args.cost_source)
        report = analyze(trace, args.buckets, args.ways, costs, hashlib.sha256(raw).hexdigest())
        if args.write_trace:
            args.write_trace.parent.mkdir(parents=True, exist_ok=True)
            args.write_trace.write_bytes(raw)
        text = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        print(text, end="")
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
