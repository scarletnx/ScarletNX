# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check source include ownership, the environment variables src/ reads, and optional CMake Graphviz dependency
closure."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

BOUNDARIES = {
    'common': {'common'},
    'nx': {'nx', 'common'},
    'gnuabi': {'gnuabi', 'common'},
    '3ds': {'3ds', 'common'},
    'vita': {'vita', 'common'},
    'agb': {'agb', 'common'},
    # GLSL ES into SPIR-V: the C library only, for any backend to link
    'glslc': {'glslc'},
    # scarletnx's main, every driver the build has: their entries, by the name it runs under
    'multicall': {'multicall', 'nx', 'gnuabi', '3ds', 'vita', 'agb', 'gui', 'common'},
}
INCLUDE = re.compile(r'^\s*#\s*include\s*(["<])([^">]+)[">]', re.MULTILINE)


def check_sources(repo):
    errors = []
    src = repo / 'src'
    include_suffixes = {}
    for header in src.rglob('*'):
        if header.suffix not in {'.h', '.hpp', '.inc'}:
            continue
        parts = header.relative_to(src).parts[1:]
        for offset in range(len(parts)):
            include_suffixes.setdefault('/'.join(parts[offset:]), set()).add(header.resolve())
    for owner, allowed in BOUNDARIES.items():
        root = src / owner
        if not root.is_dir():
            errors.append(f'missing source boundary: {root}')
            continue
        for path in sorted(root.rglob('*')):
            if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc'}:
                continue
            source = path.read_text(encoding='utf-8')
            for match in INCLUDE.finditer(source):
                name = match.group(2)
                candidates = [path.parent / name, src / name]
                # Resolve against every source boundary, even forbidden ones: a
                # private include root must not hide a dependency from this test.
                candidates.extend(src / boundary / name for boundary in BOUNDARIES)
                targets = {candidate.resolve() for candidate in candidates if candidate.is_file()}
                if not targets and match.group(1) == '"':
                    # Runtime targets may expose a deeper include root. Check a
                    # unique header suffix even if that include root was omitted
                    # here; system angle-bracket headers keep host resolution.
                    targets = include_suffixes.get(name.replace('\\', '/'), set())
                for target in targets:
                    if not target.is_relative_to(src.resolve()):
                        continue
                    dependency = target.relative_to(src.resolve()).parts[0]
                    if dependency in BOUNDARIES and dependency not in allowed:
                        line = source.count('\n', 0, match.start()) + 1
                        errors.append(f'{path.relative_to(repo)}:{line}: {owner} includes {dependency}: {name}')
    for legacy in ('runtime', 'scarletnx'):
        if any(path.suffix in {'.c', '.h', '.cpp', '.hpp', '.inc'} for path in (repo / legacy).rglob('*')):
            errors.append(f'legacy source files still present: {legacy}/')
    return errors


# The environment variables the code in src/ reads: the system's, SDL's and the package's (CLAUDE.md, "Settings, not
# environment variables, in src/"). An option of a program in src/ is a setting of src/nx/runtime/settings.txt, or a
# command-line option of a driver with no settings table
ENV_SYSTEM = {
    'HOME',
    'USERPROFILE',
    'APPDATA',
    'PATH',
    'TMPDIR',
    'LANG',
    'APPIMAGE',
    'APPDIR',
    'SSL_CERT_FILE',
    'SSL_CERT_DIR',
    'SDL_GAMECONTROLLERCONFIG_FILE',
}
ENV_SYSTEM_PREFIXES = ('XDG_',)
# The variables an older version read for what are settings or options now, still read for one version
# (docs/DEPRECATIONS.md), each in the files named here alone. The change that removes one removes its row
ENV_DEPRECATED = {
    'SNX_WW': {'src/nx/runtime/deprecated_env.c'},
    'SNX_NO_WW': {'src/nx/runtime/deprecated_env.c'},
    'SNX_TRAPS': {'src/nx/runtime/deprecated_env.c'},
    'SNX_NO_TRAPS': {'src/nx/runtime/deprecated_env.c'},
    'SNX_HOST_PAGE': {'src/nx/runtime/deprecated_env.c', 'src/gnuabi/runtime/main.c'},
    'SNX_ALIAS_CHECK': {'src/nx/runtime/deprecated_env.c'},
    'SNX_DIRTY_CHECK': {'src/nx/runtime/deprecated_env.c'},
    'SNX_LLVM_BRANCH_PROFILE': {
        'src/nx/runtime/deprecated_env.c',
        'src/nx/runtime/driver/recompile.c',
        'src/gnuabi/runtime/main.c',
    },
    'SNX_3DS_UDS_PACKET_TRACE': {'src/3ds/runtime/main.c'},
    'SNX_3DS_ROOM': {'src/3ds/services/uds.c'},
    'SNX_3DS_ROOM_PASSWORD': {'src/3ds/services/uds.c'},
    'SNX_3DS_NICKNAME': {'src/3ds/services/uds.c'},
    'VITA_LOCKSTEP': {'src/vita/runtime/main.c'},
    'VITA_LOCKSTEP_STEPS': {'src/vita/runtime/main.c'},
    'SNX_GNU_STRACE': {'src/gnuabi/runtime/main.c'},
    'SNX_GNU_SAMPLE': {'src/gnuabi/runtime/main.c'},
    'SNX_LINUX_SYSROOT': {'src/gnuabi/runtime/main.c'},
}
# the calls that read one variable, and which argument names it (_dupenv_s: the third)
ENV_READ = re.compile(
    r'(?<![A-Za-z0-9_])(getenv|SDL_getenv|SDL_getenv_unsafe|secure_getenv|_wgetenv|_dupenv_s|_wdupenv_s|'
    r'GetEnvironmentVariable[AW]?)\s*\('
)
ENV_NAME_ARGUMENT = {'_dupenv_s': 2, '_wdupenv_s': 2}
ENV_LITERAL = re.compile(r'^(?:L|u8)?"([^"\\]*)"$')


def without_comments(source, literals=True):
    """source with each comment's characters blank and its newlines kept; string and character literals as they are,
    or with literals=False their insides blank too"""
    out, i, n = [], 0, len(source)
    while i < n:
        c = source[i]
        if source.startswith('//', i):
            end = source.find('\n', i)
            end = n if end < 0 else end
            out.append(' ' * (end - i))
            i = end
        elif source.startswith('/*', i):
            end = source.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in source[i:end]))
            i = end
        elif c in '"\'':
            j = i + 1
            while j < n and source[j] != c and source[j] != '\n':
                j += 2 if source[j] == '\\' else 1
            text = source[i : j + 1]
            if not literals and len(text) > 1:
                text = c + ' ' * (len(text) - 2) + text[-1]
            out.append(text)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def call_arguments(source, start):
    """the top-level arguments of the call whose ( is at start, each stripped"""
    arguments, depth, i, begin = [], 0, start, start + 1
    while i < len(source):
        c = source[i]
        if c in '"\'':
            j = i + 1
            while j < len(source) and source[j] != c:
                j += 2 if source[j] == '\\' else 1
            i = j
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if not depth:
                arguments.append(source[begin:i].strip())
                return arguments
        elif c == ',' and depth == 1:
            arguments.append(source[begin:i].strip())
            begin = i + 1
        i += 1
    return arguments


def check_environment(repo):
    """every read of an environment variable in src/ names one of ENV_SYSTEM, or a deprecated one in its own files"""
    errors = []
    src = repo / 'src'
    for path in sorted(src.rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc'}:
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        source = without_comments(text)
        shown = path.relative_to(repo).as_posix()
        for match in ENV_READ.finditer(without_comments(text, literals=False)):  # a call, not a string naming one
            line = source.count('\n', 0, match.start()) + 1
            call = match.group(1)
            arguments = call_arguments(source, match.end() - 1)
            index = ENV_NAME_ARGUMENT.get(call, 0)
            literal = ENV_LITERAL.match(arguments[index]) if index < len(arguments) else None
            if not literal:
                errors.append(
                    f'{shown}:{line}: {call} of a name that is not a string literal: name the variable, so this check '
                    "can tell whether it is the system's"
                )
                continue
            name = literal.group(1)
            if name in ENV_SYSTEM or name.startswith(ENV_SYSTEM_PREFIXES):
                continue
            if shown in ENV_DEPRECATED.get(name, ()):
                continue
            where = (
                f'deprecated, and read only in {", ".join(sorted(ENV_DEPRECATED[name]))}'
                if name in ENV_DEPRECATED
                else "not the system's, SDL's or the package's"
            )
            errors.append(
                f'{shown}:{line}: {call}("{name}"): {name} is {where}. An option is a setting of '
                'src/nx/runtime/settings.txt, or a command-line option of a driver with no settings table (CLAUDE.md, '
                '"Settings, not environment variables, in src/")'
            )
    return errors


def check_graph(graph):
    source = graph.read_text(encoding='utf-8')
    nodes = {
        node: label.split('\\n', 1)[0] for node, label in re.findall(r'"([^"]+)"\s*\[\s*label\s*=\s*"([^"]+)"', source)
    }
    edges = {}
    for left, right in re.findall(r'"([^"]+)"\s*->\s*"([^"]+)"', source):
        edges.setdefault(left, set()).add(right)
    by_name = {name: node for node, name in nodes.items()}

    def closure(node):
        result, pending = set(), [node]
        while pending:
            current = pending.pop()
            for child in edges.get(current, ()):
                if child not in result:
                    result.add(child)
                    pending.append(child)
        return {nodes.get(item, item) for item in result}

    errors = []
    for owner, forbidden in (
        (
            'scarletnx-common',
            (
                'scarletnx-nx',
                'scarletnx-gnuabi',
                'scarletnx-3ds',
                'scarletnx-agb',
                'snx_ctr_board',
                'snx_ctr_memory',
                'snx_ctr_loader',
            ),
        ),
        (
            'scarletnx-nx',
            ('scarletnx-gnuabi', 'scarletnx-3ds', 'scarletnx-agb', 'snx_ctr_board', 'snx_ctr_memory', 'snx_ctr_loader'),
        ),
        (
            'scarletnx-gnuabi',
            ('scarletnx-nx', 'scarletnx-3ds', 'scarletnx-agb', 'snx_ctr_board', 'snx_ctr_memory', 'snx_ctr_loader'),
        ),
        ('scarletnx-3ds', ('scarletnx-nx', 'scarletnx-gnuabi', 'scarletnx-agb', 'snx_driver', 'snx_nx_module')),
        (
            'scarletnx-agb',
            (
                'scarletnx-nx',
                'scarletnx-gnuabi',
                'scarletnx-3ds',
                'snx_ctr_board',
                'snx_ctr_memory',
                'snx_ctr_loader',
                'snx_driver',
                'snx_nx_module',
            ),
        ),
        ('snx_ctr_program', ('scarletnx-nx', 'scarletnx-gnuabi', 'scarletnx-agb', 'snx_driver', 'snx_nx_module')),
        ('snx_agb_program', ('scarletnx-nx', 'scarletnx-gnuabi', 'scarletnx-3ds', 'snx_driver', 'snx_nx_module')),
        (
            'snx_switch_program',
            ('scarletnx-gnuabi', 'scarletnx-3ds', 'scarletnx-agb', 'snx_ctr_board', 'snx_ctr_memory', 'snx_ctr_loader'),
        ),
        ('snx_ctr_board', ('scarletnx-nx', 'scarletnx-gnuabi')),
        ('snx_ctr_memory', ('scarletnx-nx', 'scarletnx-gnuabi')),
        ('snx_ctr_loader', ('scarletnx-nx', 'scarletnx-gnuabi', 'snx_driver', 'snx_nx_module')),
        ('snx_ctr_cpu', ('scarletnx-nx', 'scarletnx-gnuabi', 'snx_driver', 'snx_nx_module', 'snx_arm_llvm')),
        ('snx_ctr_process', ('scarletnx-nx', 'scarletnx-gnuabi', 'snx_driver', 'snx_nx_module', 'snx_arm_llvm')),
        ('snx_ctr_native', ('scarletnx-nx', 'scarletnx-gnuabi', 'snx_driver', 'snx_nx_module', 'snx_arm_llvm')),
    ):
        if owner not in by_name:
            if owner == 'scarletnx-common':
                errors.append(f'CMake graph is missing target {owner}')
            continue
        dependencies = closure(by_name[owner])
        for target in forbidden:
            if target in dependencies:
                errors.append(f'{owner} transitively links forbidden target {target}')
        if owner in ('scarletnx-common', 'scarletnx-gnuabi', 'scarletnx-agb'):
            for target in dependencies:
                if re.search(
                    r'(?:^|[-_:])(gpu|maxwell|vulkan|volk|glslang|sdl3|ffmpeg|spirv)(?:$|[-_:])', target, re.IGNORECASE
                ) or target in {'snx_vk', 'snx_null', 'snx_shader', 'snx_video', 'snx_platform'}:
                    errors.append(f'{owner} transitively links graphics/media target {target}')
    return errors


# A project header with one of these names, on an include path, hides the system one
SYSTEM_HEADERS = {
    'assert.h',
    'complex.h',
    'ctype.h',
    'errno.h',
    'fenv.h',
    'float.h',
    'inttypes.h',
    'iso646.h',
    'limits.h',
    'locale.h',
    'math.h',
    'setjmp.h',
    'signal.h',
    'stdalign.h',
    'stdarg.h',
    'stdatomic.h',
    'stdbool.h',
    'stddef.h',
    'stdint.h',
    'stdio.h',
    'stdlib.h',
    'stdnoreturn.h',
    'string.h',
    'tgmath.h',
    'threads.h',
    'time.h',
    'uchar.h',
    'wchar.h',
    'wctype.h',
    'dirent.h',
    'dlfcn.h',
    'fcntl.h',
    'poll.h',
    'pthread.h',
    'sched.h',
    'semaphore.h',
    'termios.h',
    'unistd.h',
    'conio.h',
    'direct.h',
    'excpt.h',
    'intrin.h',
    'io.h',
    'malloc.h',
    'memory.h',
    'process.h',
    'windows.h',
}


def check_include_shadows(repo, build):
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text(encoding='utf-8'))
    repo = repo.resolve()
    seen, errors = set(), []
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            for group in target.get('compileGroups', ()):
                for include in group.get('includes', ()):
                    directory = Path(include['path'])
                    if not directory.is_absolute():
                        directory = repo / directory
                    directory = directory.resolve()
                    if not directory.is_relative_to(repo) or not directory.is_dir():
                        continue
                    for header in sorted(directory.iterdir()):
                        if header.is_file() and header.name.lower() in SYSTEM_HEADERS and header not in seen:
                            seen.add(header)
                            path = header.relative_to(repo).as_posix()  # the same text on Windows
                            errors.append(
                                f"{path} is on {entry['name']}'s include path, where it hides the system "
                                f'<{header.name.lower()}>'
                            )
    return errors


# a compiler's -I to a folder of src/, in the CI, the docs or the scripts: src/ is the one include root
SOURCE_FLAG = re.compile(r'-I\s*["\']?src/[^\s"\']')


def check_include_flags(repo):
    errors = []
    places = [repo / '.github', repo / 'docs', repo / 'scripts']
    for path in sorted(p for place in places if place.is_dir() for p in place.rglob('*') if p.is_file()):
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if SOURCE_FLAG.search(line):
                errors.append(f'{path.relative_to(repo).as_posix()}:{number}: -I to a folder of src/; use -I src')
    return errors


def check_include_roots(repo, build):
    """Every C and C++ include root in src/ is src/ itself: a local include is a path from it"""
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text(encoding='utf-8'))
    repo = repo.resolve()
    src = repo / 'src'
    errors = []
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            for group in target.get('compileGroups', ()):
                if group.get('language') not in ('C', 'CXX'):
                    continue  # the Windows resource compiler finds its files from a folder of its own
                for include in group.get('includes', ()):
                    directory = Path(include['path'])
                    if not directory.is_absolute():
                        directory = repo / directory
                    directory = directory.resolve()
                    if directory != src and directory.is_relative_to(src):
                        error = (
                            f"{directory.relative_to(repo).as_posix()} is on {entry['name']}'s include path: "
                            'include its headers as paths from src/'
                        )
                        if error not in errors:
                            errors.append(error)
    return errors


def check_target_sources(repo, build):
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model_file = index['reply']['codemodel-v2']['jsonFile']
    model = json.loads((reply / model_file).read_text(encoding='utf-8'))
    errors = []
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            owner = {
                'scarletnx-common': 'common',
                'scarletnx-nx': 'nx',
                'scarletnx-gnuabi': 'gnuabi',
                'scarletnx-agb': 'agb',
            }.get(entry['name'])
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            directory = (repo / target.get('paths', {}).get('source', '')).resolve()
            src = (repo / 'src').resolve()
            if not owner and directory.is_relative_to(src):
                owner = directory.relative_to(src).parts[0]
            if owner not in BOUNDARIES:
                continue
            for source in target.get('sources', ()):
                path = Path(source['path'])
                if not path.is_absolute():
                    path = repo / path
                path = path.resolve()
                # the Windows programs' manifest and icon (src/common/platform/runtime.rc), which each program
                # compiles into itself as a source of its own
                if not path.is_relative_to(src):
                    continue
                boundary = path.relative_to(src).parts[0]
                if boundary != owner and not (boundary == 'common' and path.suffix == '.rc'):
                    errors.append(f'{entry["name"]} compiles a source owned by another runtime: {path}')
    return errors


SANITIZERS = {'address', 'undefined'}
LIBRARIES = {'STATIC_LIBRARY', 'SHARED_LIBRARY', 'MODULE_LIBRARY', 'OBJECT_LIBRARY'}


def sanitizers_of(fragments):
    """The sanitizers a compile line ends with, and those a -fno-sanitize named"""
    on, named_off = set(), set()
    for fragment in fragments:
        for flag in fragment.split():
            if flag.startswith('-fsanitize='):
                on.update(flag.split('=', 1)[1].split(','))
            elif flag.startswith('-fno-sanitize='):
                off = set(flag.split('=', 1)[1].split(','))
                named_off |= SANITIZERS if 'all' in off else off
                on = set() if 'all' in off else on - off
    return on, named_off


def check_sanitizers(repo, build, tests):
    """A sanitized build (SNX_TEST_SANITIZERS): every program a test runs, and every library it links, builds the
    repo's code with ASan and UBSan. A dependency's code (the CPM cache, FetchContent's _deps) is not checked. Returns
    the errors, and the sources of ours a -fno-sanitize takes the sanitizers off (third-party code in them)"""
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot'], []
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text(encoding='utf-8'))
    repo, build = repo.resolve(), build.resolve()
    targets, artifacts = {}, {}
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            targets[entry['id']] = target
            for artifact in target.get('artifacts', ()):
                artifacts[(build / artifact['path']).resolve()] = target

    def ours(path):
        if path.is_relative_to(build):
            return path.relative_to(build).parts[:1] != ('_deps',)
        return path.is_relative_to(repo) and not path.is_relative_to(repo / '.cache')

    programs = {}  # each program a test runs, with the first test that runs it
    for test in tests:
        for word in test.get('command', ()):
            for candidate in (word, word.split('=', 1)[-1]):  # an argument, or a -DNAME=VALUE's value
                path = Path(candidate)
                target = artifacts.get(path.resolve()) if path.is_absolute() else None
                if target and target['type'] == 'EXECUTABLE':
                    programs.setdefault(target['id'], (target, test['name']))
    if not programs:
        return ['no test runs a program of the build: the sanitizers would be checked on nothing'], []
    errors, exempt, seen = [], [], set()
    for program, test in programs.values():
        linked, pending = {program['id']}, [program]
        while pending:
            target = pending.pop()
            for dependency in target.get('dependencies', ()):
                library = targets.get(dependency['id'])
                if library and library['type'] in LIBRARIES and library['id'] not in linked:
                    linked.add(library['id'])
                    pending.append(library)
            groups = target.get('compileGroups', ())
            for source in target.get('sources', ()):
                if 'compileGroupIndex' not in source:
                    continue
                group = groups[source['compileGroupIndex']]
                if group.get('language') not in ('C', 'CXX', 'OBJC', 'OBJCXX'):
                    continue  # assembly, Swift and resources have nothing for the sanitizers to instrument
                path = Path(source['path'])
                path = (path if path.is_absolute() else repo / path).resolve()
                on, named_off = sanitizers_of(
                    fragment['fragment'] for fragment in group.get('compileCommandFragments', ())
                )
                if not ours(path) or SANITIZERS <= on or (target['name'], path) in seen:
                    continue
                seen.add((target['name'], path))
                shown = path.relative_to(repo if path.is_relative_to(repo) else build).as_posix()
                if SANITIZERS - on <= named_off:
                    exempt.append(f'{shown}: {target["name"]} turns off -fsanitize={",".join(sorted(SANITIZERS - on))}')
                    continue
                how = 'is' if target is program else 'links'
                errors.append(
                    f'{shown}: {target["name"]} builds it without -fsanitize=address,undefined, and '
                    f'{program["name"]}, which the test {test} runs, {how} it'
                )
    return errors, exempt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--graph', type=Path, help='CMake --graphviz=PATH output, including transitive dependencies')
    parser.add_argument(
        '--build', type=Path, help='Configured build tree: generate and validate its actual CMake graph'
    )
    parser.add_argument('--cmake', default='cmake', help='CMake executable for --build')
    parser.add_argument(
        '--sanitizers', action='store_true', help='--build is sanitized: check what its tests run builds with them'
    )
    parser.add_argument('--ctest', default='ctest', help='CTest executable that lists the tests of --build')
    args = parser.parse_args()
    if args.sanitizers and not args.build:
        parser.error('--sanitizers checks the build tree --build names')
    errors = (
        check_sources(args.repo.resolve())
        + check_include_flags(args.repo.resolve())
        + check_environment(args.repo.resolve())
    )
    if args.build:
        graph = args.build.resolve() / 'architecture-targets.dot'
        query = args.build.resolve() / '.cmake/api/v1/query/codemodel-v2'
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        result = subprocess.run(
            [args.cmake, '-S', str(args.repo.resolve()), '-B', str(args.build.resolve()), f'--graphviz={graph}'],
            capture_output=True,
            check=False,
            timeout=120,
        )
        if result.returncode:
            sys.stderr.buffer.write(result.stderr)
            sys.stderr.buffer.write(result.stdout)
            return result.returncode
        errors.extend(check_graph(graph))
        errors.extend(check_target_sources(args.repo.resolve(), args.build.resolve()))
        errors.extend(check_include_shadows(args.repo.resolve(), args.build.resolve()))
        errors.extend(check_include_roots(args.repo.resolve(), args.build.resolve()))
        if args.sanitizers:
            listed = subprocess.run(
                [args.ctest, '--test-dir', str(args.build.resolve()), '--show-only=json-v1'],
                capture_output=True,
                check=False,
                timeout=120,
            )
            if listed.returncode:
                sys.stderr.buffer.write(listed.stderr)
                sys.stderr.buffer.write(listed.stdout)
                return listed.returncode
            unsanitized, exempt = check_sanitizers(
                args.repo.resolve(), args.build.resolve(), json.loads(listed.stdout)['tests']
            )
            errors.extend(unsanitized)
            for line in exempt:
                print(f'sanitizers off on purpose: {line}')
    if args.graph:
        errors.extend(check_graph(args.graph))
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Runtime source boundaries, the environment src/ reads and requested CMake dependency graph passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
