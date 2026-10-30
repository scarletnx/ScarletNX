#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Index local NVN declarations, runtime handlers and possible Vulkan call paths.

Only names, signatures, locations and call edges are retained. No source body is
copied into the output. Static call paths are navigation aids, not proof of
rendering equivalence. Command recording defers work to nvn_replay.

--contracts accepts reviewed hook declarations in a JSON list. --emit-bindings
emits those declarations only after checking the reference ABI and an existing
C handler. It never invents a handler body or marks an unimplemented API supported.
--log also records actual calls that stopped nvn_direct and nvn_verify differences.
Keep reports and caches outside the checkout when indexing private references.
"""

import argparse
from collections import deque
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import time

SCHEMA = 1
LEX = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
PROTO = re.compile(r'NVNAPI\s+(\w+(?:\s*\*)?)\s+NVNAPIENTRY\s+(nvn\w+)\s*\(([^;{}]*)\)\s*;')
DEF = re.compile(r'(?m)^[ \t]*((?:(?:static|inline|extern|const|unsigned|signed|struct)\s+)*'
                 r'[A-Za-z_]\w*\s*\*?)\s+([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
HOOK = re.compile(r'hook (nvn\w+)\(([^()]*)\)(?:\s*->\s*([^=]+?))?(?:\s*=\s*(\w+))?$')
SKIP = {'if', 'for', 'while', 'switch', 'sizeof', 'return'}


def masked(text):
    return LEX.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]), text)


def parameters(text):
    return [] if text.strip() in ('', 'void') else [p.strip() for p in text.split(',')]


def registered_bindings(path):
    bindings = {}
    for line in path.read_text().splitlines():
        m = HOOK.fullmatch(line.strip())
        if m:
            bindings[m[1]] = m[4] or 'nvn_' + m[1][3:]
    return bindings


def scan_file(task):
    role, filename = task
    path = Path(filename)
    raw = path.read_bytes()
    text = masked(raw.decode('utf-8', errors='replace'))
    result = {'file': filename, 'sha256': hashlib.sha256(raw).hexdigest(), 'apis': [], 'functions': []}
    if role == 'reference':
        result['apis'] = [{'name': m[2], 'return': m[1], 'args': parameters(m[3]),
                           'file': filename, 'line': text.count('\n', 0, m.start()) + 1}
                          for m in PROTO.finditer(text)]
    else:
        for m in DEF.finditer(text):
            end, depth = m.end(), 1
            while end < len(text) and depth:
                depth += (text[end] == '{') - (text[end] == '}')
                end += 1
            if depth:
                continue
            body = text[m.end():end - 1]
            calls = sorted(set(CALL.findall(body)) - SKIP - {m[2]})
            result['functions'].append({'name': m[2], 'return': m[1].strip(), 'args': parameters(m[3]),
                                        'file': filename, 'line': text.count('\n', 0, m.start()) + 1,
                                        'calls': calls})
    return result


def index_files(tasks, cache_path, jobs):
    try:
        cache = json.loads(cache_path.read_text())
        if cache.get('schema') != SCHEMA:
            cache = {}
    except (OSError, ValueError):
        cache = {}
    old = cache.get('files', {})
    rows, pending, stats = {}, [], {}
    for role, filename in tasks:
        st = Path(filename).stat()
        stamp = [role, st.st_size, st.st_mtime_ns, st.st_ctime_ns]
        stats[filename] = stamp
        if filename in old and old[filename]['stamp'] == stamp:
            rows[filename] = old[filename]['index']
        else:
            pending.append((role, filename))
    if pending:
        with concurrent.futures.ProcessPoolExecutor(max_workers=min(jobs, len(pending))) as pool:
            for row in pool.map(scan_file, pending, chunksize=max(1, len(pending) // (jobs * 4))):
                rows[row['file']] = row
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=cache_path.parent, prefix=cache_path.name, delete=False) as out:
        temp = Path(out.name)
        json.dump({'schema': SCHEMA, 'files': {
            name: {'stamp': stats[name], 'index': row} for name, row in sorted(rows.items())}}, out)
    temp.replace(cache_path)
    return list(rows.values()), len(tasks) - len(pending)


def abi_type(declaration, argument=False):
    text = re.sub(r'\bconst\b|\brestrict\b', '', declaration).strip()
    if argument:
        text = re.sub(r'\b[A-Za-z_]\w*\s*$', '', text).strip()
    if '*' in text or text.startswith('PFNNVN') or text in ('NVNstring', 'NVNnativeWindow'):
        return 'addr'
    known = {'void': 'void', 'NVNboolean': 'bool', 'int': 'i32', 'int32_t': 'i32', 'u32': 'u32',
             'uint32_t': 'u32', 'i32': 'i32', 'float': 'f32', 'f32': 'f32', 'double': 'f64',
             'uint64_t': 'u64', 'u64': 'u64', 'size_t': 'u64', 'i64': 'i64', 'int64_t': 'i64',
             'ptrdiff_t': 'i64', 'NVNbufferAddress': 'u64', 'NVNtextureHandle': 'u64',
             'NVNimageHandle': 'u64', 'NVNcommandHandle': 'u64'}
    return known.get(text)


def checked_binding(line, apis, functions):
    m = HOOK.fullmatch(line)
    if not m or m[1] not in apis:
        raise ValueError('binding needs a known API and an implemented handler: ' + line)
    name, args, ret, handler = m.groups()
    handler = handler or 'nvn_' + name[3:]
    if handler not in functions:
        raise ValueError('binding needs a known API and an implemented handler: ' + line)
    args = parameters(args)
    if any(not re.fullmatch(r'(addr|bool|i32|u32|i64|u64|f32|f64) [A-Za-z_]\w*', a) for a in args):
        raise ValueError('invalid hook arguments: ' + line)
    types = [a.split()[0] for a in args]
    api = apis[name]
    expected = [abi_type(a, True) for a in api['args']]
    if expected != types or abi_type(api['return']) != (ret or 'void'):
        raise ValueError('reference ABI differs or is unknown: ' + line)
    # Refuse ambiguous definitions, including an accidental static name collision.
    impl = functions[handler]
    if len(impl) != 1 or not impl[0]['args'] or not re.fullmatch(r'CPU\s*\*\s*\w+', impl[0]['args'][0]):
        raise ValueError('handler needs one CPU adapter definition: ' + line)
    native = impl[0]
    actual = [abi_type(a, True) for a in native['args'][1:]]
    # Hook bool adapters use int on the host. Do not permit other width or sign changes.
    compatible = lambda a, b: a == b or (a == 'bool' and b == 'i32') or (a == 'addr' and b == 'u64')
    if len(actual) != len(types) or any(not compatible(a, b) for a, b in zip(types, actual)) or not compatible(
            ret or 'void', abi_type(native['return'])):
        raise ValueError('handler ABI differs: ' + line)
    return line


def vk_paths(start, functions):
    queue, seen, found = deque([(start, [start])]), set(), []
    while queue and len(found) < 8 and len(seen) < 4096:
        name, path = queue.popleft()
        if name in seen or len(path) > 12:
            continue
        seen.add(name)
        if name.startswith(('vk_', 'vkCmd')):
            found.append(path)
            continue
        for fn in functions.get(name, []):
            queue.extend((call, path + [call]) for call in fn['calls'] if call not in seen)
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True, help='local public NVN header or its folder')
    p.add_argument('--runtime', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cache', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=getattr(os, 'process_cpu_count', os.cpu_count)() or 1)
    p.add_argument('--log', type=Path, action='append', default=[])
    p.add_argument('--contracts', type=Path, help='JSON list of reviewed hook declarations')
    p.add_argument('--emit-bindings', type=Path, help='write checked hook declarations, never handler bodies')
    a = p.parse_args()
    if a.jobs < 1 or a.jobs > 256:
        p.error('--jobs must be between 1 and 256')
    if a.emit_bindings and not a.contracts:
        p.error('--emit-bindings needs --contracts')
    if not a.reference.exists():
        p.error('--reference does not exist')
    start = time.perf_counter()
    reference = [a.reference] if a.reference.is_file() else sorted(a.reference.rglob('*.h'))
    runtime = sorted((a.runtime / 'src/nx/gpu').rglob('*.c'))
    tasks = [('reference', str(f.resolve())) for f in reference] + [('runtime', str(f.resolve())) for f in runtime]
    rows, reused = index_files(tasks, a.cache, a.jobs)
    apis, functions = {}, {}
    for row in rows:
        for api in row['apis']:
            previous = apis.get(api['name'])
            if previous and (previous['args'], previous['return']) != (api['args'], api['return']):
                raise ValueError('ambiguous reference declarations: ' + api['name'])
            apis[api['name']] = api
        for fn in row['functions']:
            functions.setdefault(fn['name'], []).append(fn)
    if not apis:
        p.error('no NVN API declarations found in --reference')
    bindings = registered_bindings(a.runtime / 'src/nx/gpu/nvn/nvn.hook')
    called, resolved, differences = set(), set(), []
    for log in a.log:
        for line in log.read_text(errors='replace').splitlines():
            called.update(re.findall(r'game called (nvn\w+)', line))
            resolved.update(re.findall(r'game resolved (nvn\w+)', line))
            if 'nvn_verify:' in line or 'nvn verify:' in line:
                differences.append(line)
    names = sorted(set(apis) | set(bindings) | called | resolved)
    report = []
    for name in names:
        handler = bindings.get(name)
        report.append({'api': name, 'bound': bool(handler), 'handler': handler,
                       'handler_found': bool(functions.get(handler)), 'reference': apis.get(name),
                       'called_unimplemented': name in called, 'resolved_unimplemented': name in resolved,
                       'possible_vulkan_paths': vk_paths(handler, functions) if handler else []})
    generated = [checked_binding(line, apis, functions) for line in
                 json.loads(a.contracts.read_text())] if a.contracts else []
    if a.emit_bindings:
        a.emit_bindings.parent.mkdir(parents=True, exist_ok=True)
        a.emit_bindings.write_text('# Checked declarations for review. Insert into the appropriate family.\n' +
                                   '\n'.join(generated) + '\n')
    a.out.parent.mkdir(parents=True, exist_ok=True)
    result = {'schema': SCHEMA, 'files': len(rows), 'cached_files': reused, 'jobs': a.jobs,
              'seconds': round(time.perf_counter() - start, 4), 'apis': report, 'verify_differences': differences,
              'limits': 'Lexical index, not semantic equivalence. Indirect calls and deferred replay need review.'}
    a.out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(names)} APIs, {len(bindings)} bindings, {len(called)} called blockers, '
          f'{reused}/{len(rows)} files cached, {result["seconds"]:.3f}s, {a.jobs} workers')


if __name__ == '__main__':
    main()
