#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Extract exact graphics pipeline compile inputs from a local ScarletNX shader cache, without using a GPU.

  python3 tools/prepare_pipeline_probe.py --cache /private/Game.vkshaders --largest 4 --out /tmp/pipeline-probes

Each package contains the captured PipeKey and its exact SPIR-V stages. It is local game-derived data: keep it
private. Checksummed complete records are used; an interrupted trailing record is ignored. Shipped shader references
are resolved from the adjacent .vkshipped file when available. The source files are read only and their contents
are snapshotted in memory before parsing. No runtime cache, save or live process is modified.

Later, while no game is running, use fresh headless-test processes with --pipeline-probe=FILE and
--pipeline-probe-disable-opt=0 or1. Disable NVIDIA disk caching for both processes (__GL_SHADER_DISK_CACHE=0).
The probe creates an empty application pipeline cache and measures compile latency only. It submits no draws and
therefore cannot establish rendering equivalence or justify enabling disabled optimization in normal gameplay.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
from pathlib import Path
import re
import struct

MASK64 = (1 << 64) - 1
MAX_BYTES = 256 << 20
MAGIC = b'SNXPPB1\n'


def checksum(kind, payload):
    h = 0xCBF29CE484222325
    for word in (kind, len(payload)):
        h = ((h ^ word) * 0x100000001B3) & MASK64
    for (word,) in struct.iter_unpack('<I', payload):
        h = ((h ^ word) * 0x100000001B3) & MASK64
    h ^= h >> 31
    h = (h * 0x9E3779B97F4A7C15) & MASK64
    return h ^ (h >> 29)


def decompress(packed, size):
    if size > MAX_BYTES:
        raise ValueError('compressed record exceeds the shader-cache size limit')
    name = ctypes.util.find_library('zstd')
    if not name:
        raise ValueError('libzstd is needed to unpack this cache')
    zstd = ctypes.CDLL(name)
    zstd.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    zstd.ZSTD_decompress.restype = ctypes.c_size_t
    out = ctypes.create_string_buffer(size)
    if zstd.ZSTD_decompress(out, size, packed, len(packed)) != size:
        raise ValueError('invalid zstd shader-cache record')
    return out.raw


def header(data):
    if len(data) < 16 or data[:8] != b'SNXVKSH\n':
        raise ValueError('not a ScarletNX shader cache')
    version, size = struct.unpack_from('<II', data, 8)
    end = (16 + size + 7) & ~7
    if version not in (2, 3) or end > len(data) or size > 65536:
        raise ValueError('unsupported or incomplete shader-cache header')
    return data[16:16 + size].decode(), end


def records(data, start):
    at = start
    while len(data) - at >= 16:
        kind, size, check = struct.unpack_from('<IIQ', data, at)
        if size % 8 or size > len(data) - at - 16:
            return  # A live append may not have completed yet.
        payload = data[at + 16:at + 16 + size]
        if checksum(kind, payload) != check:
            return
        if kind == 6:
            if len(payload) < 16:
                raise ValueError('incomplete packed unit')
            raw, packed, expected, expected_shaders = struct.unpack_from('<4I', payload)
            if packed > len(payload) - 16:
                raise ValueError('incomplete compressed unit')
            unpacked = list(records(decompress(payload[16:16 + packed], raw), 0))
            if len(unpacked) != expected or sum(k in (1, 4) for k, *_ in unpacked) != expected_shaders:
                raise ValueError('compressed unit has incomplete or inconsistent records')
            yield from unpacked
        else:
            yield kind, payload, check, at
        at += 16 + size


def shader(payload, shader_size, zipped=False):
    if shader_size < 152 or len(payload) < shader_size:
        raise ValueError('shader metadata does not match this cache')
    reads, words = struct.unpack_from('<II', payload, 36)
    if zipped:
        if len(payload) < shader_size + 8:
            raise ValueError('incomplete compressed shader')
        raw, packed = struct.unpack_from('<II', payload, shader_size)
        if packed > len(payload) - shader_size - 8:
            raise ValueError('incomplete compressed shader data')
        body = decompress(payload[shader_size + 8:shader_size + 8 + packed], raw)
    else:
        body = payload[shader_size:]
    end = reads * 16 + words * 4
    if words < 5 or words > (8 << 20) // 4 or end > len(body):
        raise ValueError('shader SPIR-V range is invalid')
    code = body[reads * 16:end]
    if struct.unpack_from('<I', code)[0] != 0x07230203:
        raise ValueError('shader is not SPIR-V')
    return code


def extract(data, shipped=None):
    key, start = header(data)
    match = re.search(r' sizes=\d+,(\d+)(?: |$)', key)
    if not match:
        raise ValueError('cache does not identify its shader metadata size')
    shader_size = int(match[1])
    refs = {}
    if shipped is not None:
        ship_key, ship_start = header(shipped)
        if ship_key != key:
            raise ValueError('shipped shader cache does not match the main cache')
        refs = {at: (kind, payload, check) for kind, payload, check, at in records(shipped, ship_start)}
    shaders, pipelines = [(None, 0)], []
    for kind, payload, check, at in records(data, start):
        if kind == 1:
            shaders.append((shader(payload, shader_size), check))
        elif kind == 4:
            if len(payload) != 16:
                raise ValueError('invalid shipped shader reference')
            offset, expected = struct.unpack('<QQ', payload)
            ref = refs.get(offset)
            shaders.append((shader(ref[1], shader_size, ref[0] == 7)
                            if ref and ref[0] in (1, 7) and ref[2] == expected else None, check))
        elif kind == 2:
            if len(payload) < 64:
                raise ValueError('incomplete pipeline record')
            ids = struct.unpack_from('<5I', payload, 40)
            expected_checks = struct.unpack_from('<5Q', payload)
            size = struct.unpack_from('<I', payload, 60)[0]
            if size > len(payload) - 64 or size < 40:
                raise ValueError('pipeline key range is invalid')
            raw_key = payload[64:64 + size]
            if any(raw_key[:40]):
                raise ValueError('pipeline record contains native shader handles')
            stages = [b'' if not index else shaders[index][0]
                      if index < len(shaders) and shaders[index][1] == expected_checks[stage] else None
                      for stage, index in enumerate(ids)]
            pipelines.append({'index': len(pipelines), 'key': raw_key, 'stages': stages,
                              'complete': bool(ids[0] and all(s is not None for s in stages)),
                              'spv_words': sum(len(s or b'') // 4 for s in stages)})
    return key, pipelines


def package(key, pipeline):
    if not pipeline['complete']:
        raise ValueError('pipeline is missing a referenced shader')
    encoded = key.encode()
    stages = pipeline['stages']
    return (MAGIC + struct.pack('<7I', len(encoded), len(pipeline['key']), *(len(s) // 4 for s in stages)) +
            encoded + pipeline['key'] + b''.join(stages))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    select = parser.add_mutually_exclusive_group(required=True)
    select.add_argument('--index', type=int, help='zero-based PF_PIPE record index')
    select.add_argument('--largest', type=int, help='extract this many pipelines with the largest combined SPIR-V')
    args = parser.parse_args(argv)
    if (args.index is not None and args.index < 0) or (args.largest is not None and args.largest < 1):
        parser.error('index must be nonnegative and largest must be positive')
    data = args.cache.read_bytes()
    shipped_path = args.cache.with_suffix('.vkshipped')
    shipped = shipped_path.read_bytes() if shipped_path.is_file() else None
    try:
        key, pipelines = extract(data, shipped)
        if args.index is not None:
            chosen = [p for p in pipelines if p['index'] == args.index]
        else:
            chosen = sorted((p for p in pipelines if p['complete']), key=lambda p: p['spv_words'], reverse=True)[:args.largest]
        if not chosen:
            raise ValueError('no matching complete pipeline was found')
        args.out.mkdir(parents=True, exist_ok=False, mode=0o700)
        report = {'cache_sha256': hashlib.sha256(data).hexdigest(), 'file_key': key,
                  'pipeline_count': len(pipelines), 'scope': 'Compile inputs only; no rendering-equivalence claim.', 'packages': []}
        for pipeline in chosen:
            payload = package(key, pipeline)
            path = args.out / f'pipeline-{pipeline["index"]}.snxppb'
            path.write_bytes(payload)
            path.chmod(0o600)
            report['packages'].append({'index': pipeline['index'], 'spv_words': pipeline['spv_words'],
                                       'path': str(path.resolve()), 'sha256': hashlib.sha256(payload).hexdigest()})
        (args.out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(f'pipeline extraction rejected: {error}', file=__import__('sys').stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
