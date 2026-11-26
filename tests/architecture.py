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
    # glslc too: the shared GL layer compiles a guest's shaders with it (common/gpu/gl/gl_program.c)
    'common': {'common', 'glslc'},
    'nx': {'nx', 'common'},
    'gnuabi': {'gnuabi', 'common'},
    '3ds': {'3ds', 'common'},
    'vita': {'vita', 'common'},
    'agb': {'agb', 'common'},
    'nds': {'nds', 'common'},
    # GLSL ES into SPIR-V: the C library only, for any backend to link
    'glslc': {'glslc'},
    # scarletnx's main, every driver the build has: their entries, by the name it runs under
    'multicall': {'multicall', 'nx', 'gnuabi', '3ds', 'vita', 'agb', 'nds', 'gui', 'common'},
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
# environment variables, in src/"). An option of a program in src/ is a setting of src/nx/runtime/settings.txt, a table
# a driver for its own, which its command line takes by the key (--key=value) and the launcher passes; a run's inputs
# (a file, a script of presses) are the driver's command line's alone
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
    # the Vulkan loader's, which name its driver: a Mac's vulkan_driver=auto leaves the driver they name
    'VK_DRIVER_FILES',
    'VK_ICD_FILENAMES',
    # Mesa's, which Turnip reads: a value the environment has is kept and said, else the runtime gives the fifth
    # descriptor set back
    'tu_dont_reserve_descriptor_set',
}
ENV_SYSTEM_PREFIXES = ('XDG_',)
# The variables an older version read for what are settings or options now, still read for one version
# (docs/user/deprecations.md), each in the files named here alone. The change that removes one removes its row
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


# the programs of src/ with a command line of their own, each one's main by the path from the repository
DRIVER_MAINS = (
    'src/nx/runtime/main.c',
    'src/gnuabi/runtime/main.c',
    'src/3ds/runtime/main.c',
    'src/vita/runtime/main.c',
    'src/agb/runtime/main.c',
    'src/nds/runtime/main.c',
    'src/android/runtime/main.c',
)


def check_driver_settings(repo):
    """every driver takes the settings of its tables by their keys (settings_arg), which the launcher passes it: no
    option of a driver waits for a table of settings.txt to show in the menus"""
    errors = []
    for name in DRIVER_MAINS:
        path = repo / name
        if not path.is_file():
            errors.append(f'{name}: missing: DRIVER_MAINS names the mains that take the settings')
        elif 'settings_arg(' not in without_comments(path.read_text(encoding='utf-8', errors='replace')):
            errors.append(
                f'{name}: it takes no setting of src/nx/runtime/settings.txt: its options are settings, in a table of '
                'their own, taken by the key (settings_arg, --key=value) and passed by the launcher'
            )
    return errors


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
                'src/nx/runtime/settings.txt, which every driver takes by its key (--key=value), and a run input its '
                'command line alone (CLAUDE.md, "Settings, not environment variables, in src/")'
            )
    return errors


DETACH = re.compile(r'(?<![A-Za-z0-9_])SDL_DetachThread\s*\(')


def check_detached_threads(repo):
    """no SDL_DetachThread in src/: it reads the thread's handle after it marks the thread detached, and a thread that
    ends in between has freed the handle (SDL 3.4.18). A thread is waited for at its last answer (SDL_WaitThread)"""
    errors = []
    for path in sorted((repo / 'src').rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc', '.m', '.mm'}:
            continue
        source = without_comments(path.read_text(encoding='utf-8', errors='replace'), literals=False)
        shown = path.relative_to(repo).as_posix()
        for match in DETACH.finditer(source):
            line = source.count('\n', 0, match.start()) + 1
            errors.append(
                f'{shown}:{line}: SDL_DetachThread reads a thread that may have ended and been freed (SDL 3.4.18): keep '
                'the thread and wait for it when its last answer comes'
            )
    return errors


THREAD32_STUB = re.compile(
    r'(?<![A-Za-z0-9_])(arm_cpu|hook_a32_cpu)\s*\(\s*(?:CPU|SnxArmCPU)\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\{([^}]*)\}'
)


def check_test_threads(repo):
    """a test that gives the handlers a 32-bit thread's registers (its arm_cpu returns some) finds them from the CPU,
    and the CPU from the registers (its hook_a32_cpu), as the runtime does in one KThread: an A32 adapter holds the
    registers through a restrict pointer, and registers that are an object of the test's own, returned whatever the
    argument, are not that pointer's to the compiler, which then moves the adapter's reads past the handler's writes
    (hook_install_a32 on Windows on Arm with ThinLTO returned to a stale lr)"""
    errors = []
    for path in sorted((repo / 'tests').rglob('*.c')):
        shown = path.relative_to(repo).as_posix()
        source = without_comments(path.read_text(encoding='utf-8', errors='replace'), literals=False)
        stubs = {m.group(1): m for m in THREAD32_STUB.finditer(source)}
        registers = stubs.get('arm_cpu')
        if not registers or re.search(r'return\s+(?:\(void\)\s*\w+\s*,\s*)?(?:NULL|0|nullptr)\s*;', registers.group(3)):
            continue  # no registers for a handler to reach
        for name, match in sorted(stubs.items()):
            if re.search(r'\(void\)\s*' + re.escape(match.group(2)) + r'\b', match.group(3)):
                line = source.count('\n', 0, match.start()) + 1
                errors.append(
                    f"{shown}:{line}: {name} returns an object of the test's whatever its argument, and arm_cpu gives "
                    "the handlers a thread's registers: an A32 adapter holds them through a restrict pointer, so find "
                    'the registers from the CPU and the CPU from the registers, in one struct, as '
                    "tests/hook_install_a32.c's Thread32 (src/nx/runtime/arm.c does it in KThread)"
                )
    return errors


WINDOW_OPEN = re.compile(r'(?<![A-Za-z0-9_])(SDL_CreateWindow|SDL_CreateWindowAndRenderer)\s*\(')


def check_window_pixels(repo):
    """every window src/nx opens asks for the display's own pixels (SDL_WINDOW_HIGH_PIXEL_DENSITY among the call's
    flags): without it a display scaled 2x gets a picture of half as many pixels each way, stretched"""
    errors = []
    for path in sorted((repo / 'src/nx').rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc'}:
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        source = without_comments(text)
        shown = path.relative_to(repo).as_posix()
        for match in WINDOW_OPEN.finditer(without_comments(text, literals=False)):
            arguments = call_arguments(source, match.end() - 1)
            if not any(re.search(r'\bSDL_WINDOW_HIGH_PIXEL_DENSITY\b', argument) for argument in arguments):
                line = source.count('\n', 0, match.start()) + 1
                errors.append(
                    f'{shown}:{line}: {match.group(1)} without SDL_WINDOW_HIGH_PIXEL_DENSITY among its flags: on a '
                    'display scaled 2x the window is a picture of half as many pixels each way, which the display '
                    'server stretches. Name the flag in the call'
                )
    return errors


DYNAMIC_DESCRIPTOR = re.compile(r'\bVK_DESCRIPTOR_TYPE_(UNIFORM|STORAGE)_BUFFER_DYNAMIC\b')


# the dynamic descriptors of src/nx that follow cb_dynamic.c's vk_cb_dynamic_choose, which takes no slot on Android,
# where Turnip runs (#2780): the type it gives a slot (cb_dynamic.h), the pool's room for them (pipeline.c) and the bind
# that gives their offsets (render.c), as many as each has
DYNAMIC_DESCRIPTOR_CHOSEN = {
    'src/nx/gpu/vk/cb_dynamic.h': 1,
    'src/nx/gpu/vk/pipeline.c': 1,
    'src/nx/gpu/vk/render.c': 1,
}
# the choice's rule for Android, without which those are dynamic on Turnip too
DYNAMIC_DESCRIPTOR_ANDROID = re.compile(
    r'#if defined\(__ANDROID__\)\s*(?:/\*.*?\*/\s*)?if \(!off\)\s*off = "off on Android', re.S
)


def check_dynamic_descriptors(repo):
    """no descriptor of src/nx is a dynamic buffer: the NX runtime gives Turnip's reserved descriptor set back as a fifth
    one (gpu_driver_load.c's turnip_sets), and that set is where Turnip keeps dynamic buffers, which then read as zeros.
    A switch's case naming the type, as a recorder copying any descriptor write does, is no use of it; nor are the
    constant buffers' DYNAMIC_DESCRIPTOR_CHOSEN while cb_dynamic.c chooses none on Android. What was seen, beside the
    other drivers' quirks: docs/layers/gpu/driver-quirks.md"""
    errors = []
    chosen = 0
    for path in sorted((repo / 'src/nx').rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc'}:
            continue
        source = without_comments(path.read_text(encoding='utf-8', errors='replace'))
        shown = path.relative_to(repo).as_posix()
        allowed = DYNAMIC_DESCRIPTOR_CHOSEN.get(shown, 0)
        for match in DYNAMIC_DESCRIPTOR.finditer(source):
            line_start = source.rfind('\n', 0, match.start()) + 1
            if re.match(r'\s*case\s*$', source[line_start : match.start()]):
                continue
            if allowed:
                allowed -= 1
                chosen += 1
                continue
            line = source.count('\n', 0, match.start()) + 1
            errors.append(
                f'{shown}:{line}: {match.group(0)}: Turnip keeps dynamic buffers in the descriptor set the runtime takes '
                "back for a fifth (gpu_driver_load.c's turnip_sets), so they read as zeros there. Bind the whole "
                'buffer and give the shader its offset (ui.c and ui.vert do it with a push constant; '
                'docs/layers/gpu/driver-quirks.md#turnip)'
            )
    choose = repo / 'src/nx/gpu/vk/cb_dynamic.c'
    if chosen and not (choose.is_file() and DYNAMIC_DESCRIPTOR_ANDROID.search(choose.read_text(encoding='utf-8'))):
        errors.append(
            'src/nx/gpu/vk/cb_dynamic.c: vk_cb_dynamic_choose takes dynamic slots on Android: Turnip keeps dynamic '
            "buffers in the descriptor set the runtime takes back for a fifth (gpu_driver_load.c's turnip_sets), so "
            'they read as zeros there. Keep its #if defined(__ANDROID__) that sets off (#2780)'
        )
    return errors


# Every third-party download goes through CPMUtil (cmake/packages.cmake): pinned in cpmfile.json, by its SHA-512, and
# added with AddJsonPackage, or snx_package_file for a package of one file. These fetch without it
CMAKE_FETCH = re.compile(
    r'(?<![A-Za-z0-9_])(CPMAddPackage|CPMFindPackage|CPMDeclarePackage|CPMGetPackage|CPMUsePackageLock|'
    r'FetchContent_Declare|FetchContent_MakeAvailable|FetchContent_Populate|file\s*\(\s*DOWNLOAD)\b',
    re.IGNORECASE,
)
# the calls that name a cpmfile.json entry, and which argument does; AddJsonPackage takes NAME <key> or the key alone
CMAKE_PACKAGE_CALL = re.compile(
    r'(?<![A-Za-z0-9_])(AddJsonPackage|snx_package_file|snx_cpm_options|snx_cpmfile|SystemPackageViable)\s*\(',
    re.IGNORECASE,
)
PACKAGE_KEY_ARGUMENT = dict.fromkeys(
    ('addjsonpackage', 'snx_package_file', 'snx_cpm_options', 'systempackageviable'), 0
)
PACKAGE_KEY_ARGUMENT['snx_cpmfile'] = 1
# the fields CPMUtil reads from an entry (cmake/cpmutil/CPMUtil.cmake's parse_object): any other is a typo it ignores
CPMFILE_FIELDS = {
    'package',
    'repo',
    'ci',
    'version',
    'git_host',
    'name',
    'extension',
    'disabled_platforms',
    'hash',
    'min_version',
    'url',
    'artifact',
    'source_subdir',
    'bundled',
    'numeric_version',
    'find_args',
    'skip_updates',
    'patches',
    'options',
}
# where the build's CMake code is: the vendored CPMUtil, which defines the calls, is not
CMAKE_ROOTS = ('cmake', 'src', 'tests', 'scripts', 'tools', 'dist')
CPMUTIL_DIR = 'cmake/cpmutil'


def cmake_code(source):
    """CMake source with its comments blank, and with the insides of its quoted and bracket arguments blank too, each
    newline kept: (code, kept), kept being the source with only the comments blank"""
    code, kept, i, n = [], [], 0, len(source)

    def blank(text):
        return ''.join(ch if ch == '\n' else ' ' for ch in text)

    while i < n:
        c = source[i]
        bracket = re.match(r'\[(=*)\[', source[i + 1 :]) if c == '#' else re.match(r'\[(=*)\[', source[i:])
        if c == '#':
            if bracket:
                end = source.find(']' + bracket.group(1) + ']', i)
                end = n if end < 0 else end + len(bracket.group(1)) + 2
            else:
                end = source.find('\n', i)
                end = n if end < 0 else end
            code.append(blank(source[i:end]))
            kept.append(blank(source[i:end]))
            i = end
        elif c == '"' or bracket:
            if bracket:
                close = ']' + bracket.group(1) + ']'
                end = source.find(close, i + len(bracket.group(0)))
                end = n if end < 0 else end + len(close)
            else:
                end = i + 1
                while end < n and source[end] != '"':
                    end += 2 if source[end] == '\\' else 1
                end = min(end + 1, n)
            code.append(blank(source[i:end]))
            kept.append(source[i:end])
            i = end
        else:
            code.append(c)
            kept.append(c)
            i += 1
    return ''.join(code), ''.join(kept)


def cmake_arguments(kept, start):
    """the arguments of the CMake call whose ( is at start, each unquoted"""
    depth, i = 0, start
    while i < len(kept):
        if kept[i] == '"':
            i += 1
            while i < len(kept) and kept[i] != '"':
                i += 2 if kept[i] == '\\' else 1
        elif kept[i] == '(':
            depth += 1
        elif kept[i] == ')':
            depth -= 1
            if not depth:
                break
        i += 1
    words = re.findall(r'"(?:[^"\\]|\\.)*"|[^\s()]+', kept[start + 1 : i])
    return [word[1:-1] if word.startswith('"') else word for word in words]


def check_packages(repo):
    """every third-party download goes through CPMUtil: no CPM.cmake, FetchContent or file(DOWNLOAD) call outside the
    vendored CPMUtil, every key a call names pinned in cpmfile.json and every pin added by one, a package without a
    min_version (none the system may give) added with FORCE_BUNDLED_PACKAGE ON and none with one, and cpmfile.json in
    CPMUtil's form: lowercase keys, its fields, a version, a SHA-512, a url or a repo, written as jq --indent 4 -S
    writes it, and every patch an entry names in .patch/<key>/ and named by one"""
    errors, pins, text = [], {}, None
    path = repo / 'cpmfile.json'
    if path.exists():
        text = path.read_text(encoding='utf-8')
        try:
            pins = json.loads(text)
        except json.JSONDecodeError as error:
            errors.append(f'cpmfile.json: {error}')
            text = None
    else:
        errors.append('cpmfile.json is missing: the third-party packages are pinned there (cmake/packages.cmake)')
    if text is not None and text != json.dumps(pins, indent=4, sort_keys=True) + '\n':
        errors.append(
            "cpmfile.json is not in CPMUtil's form, which jq --indent 4 -S . writes (keys sorted, 4 spaces): format "
            'it with that, or python3 -m json.tool --indent 4 --sort-keys'
        )
    for key, entry in pins.items():
        if key != key.lower():
            errors.append(
                f'cpmfile.json: {key} has upper-case letters: a key is lowercase, as Eden\'s are, and its "package" '
                'names the CMake package where that differs'
            )
        unknown = sorted(set(entry) - CPMFILE_FIELDS)
        if unknown:
            errors.append(f'cpmfile.json: {key} has {", ".join(unknown)}, which CPMUtil does not read')
        if not entry.get('version'):
            errors.append(f'cpmfile.json: {key} has no version, which CPMUtil requires')
        if not re.fullmatch(r'[0-9a-f]{128}', str(entry.get('hash', ''))):
            errors.append(
                f"cpmfile.json: {key}'s hash is [{entry.get('hash', '')}], not a SHA-512 (128 lowercase hex digits): "
                'CPMUtil downloads a package without one unchecked'
            )
        if not entry.get('url') and not entry.get('repo'):
            errors.append(f'cpmfile.json: {key} has neither a url nor a repo to download it from')
        for patch in entry.get('patches', []):
            if not (repo / '.patch' / key / patch).is_file():
                errors.append(f'cpmfile.json: {key} names the patch {patch}, and .patch/{key}/{patch} is no file')
    if (repo / '.patch').is_dir():
        for path in sorted(p for p in (repo / '.patch').rglob('*') if p.is_file()):
            shown = path.relative_to(repo).as_posix()
            parts = path.relative_to(repo / '.patch').parts
            if len(parts) != 2 or parts[1] not in pins.get(parts[0], {}).get('patches', []):
                errors.append(
                    f'{shown} is a patch no cpmfile.json entry names in its "patches": CPMUtil never applies it'
                )
    adds, reads = [], []  # (shown, line, key pattern, forced or None)
    files = [repo / 'CMakeLists.txt'] if (repo / 'CMakeLists.txt').exists() else []
    for root in CMAKE_ROOTS:
        if (repo / root).is_dir():
            files += [p for p in (repo / root).rglob('*') if p.name == 'CMakeLists.txt' or p.suffix == '.cmake']
    for file in sorted(files):
        shown = file.relative_to(repo).as_posix()
        if shown.startswith(CPMUTIL_DIR + '/'):
            continue
        code, kept = cmake_code(file.read_text(encoding='utf-8', errors='replace'))
        for match in CMAKE_FETCH.finditer(code):
            line = code.count('\n', 0, match.start()) + 1
            call = re.sub(r'\s+', '', match.group(1))
            errors.append(
                f'{shown}:{line}: {call} fetches a third-party package outside CPMUtil: pin it in cpmfile.json and add '
                'it with AddJsonPackage, or snx_package_file for a package of one file (cmake/packages.cmake)'
            )
        for match in CMAKE_PACKAGE_CALL.finditer(code):
            line = code.count('\n', 0, match.start()) + 1
            call = match.group(1).lower()
            arguments = cmake_arguments(kept, match.end() - 1)
            if call == 'addjsonpackage' and arguments[:1] == ['NAME']:
                arguments = arguments[1:]
            index = PACKAGE_KEY_ARGUMENT[call]
            if len(arguments) <= index:
                continue  # a definition's parameters, or a call that names no key
            key = arguments[index]
            pattern = re.compile(
                ''.join('.+' if part.startswith('${') else re.escape(part) for part in re.split(r'(\$\{[^}]*\})', key))
            )
            forced = None
            if call == 'addjsonpackage':
                at = arguments.index('FORCE_BUNDLED_PACKAGE') if 'FORCE_BUNDLED_PACKAGE' in arguments else -1
                forced = at >= 0 and at + 1 < len(arguments) and arguments[at + 1].upper() in {'ON', 'YES', 'TRUE', '1'}
            record = (shown, line, key, pattern, forced)
            (adds if call in {'addjsonpackage', 'snx_package_file'} else reads).append(record)
    for shown, line, key, pattern, forced in adds + reads:
        named = [pin for pin in pins if pattern.fullmatch(pin)]
        if not named:
            errors.append(f'{shown}:{line}: {key} is not pinned in cpmfile.json')
        for pin in named if forced is not None else ():
            system = 'min_version' in pins[pin]
            if not system and not forced:
                errors.append(
                    f'{shown}:{line}: {pin} is added without FORCE_BUNDLED_PACKAGE ON, and its entry has no min_version:'
                    ' CPMUTIL_FORCE_SYSTEM would look for a system package that cannot stand in for it'
                )
            elif system and forced:
                errors.append(
                    f'{shown}:{line}: {pin} is added with FORCE_BUNDLED_PACKAGE ON, and its entry has a min_version for '
                    'the system package that option keeps out'
                )
    for pin in pins:
        if not any(pattern.fullmatch(pin) for _, _, _, pattern, _ in adds):
            errors.append(f'cpmfile.json: {pin} is pinned, and no AddJsonPackage or snx_package_file adds it')
    return errors


# The last policy of each CMake the root CMakeLists.txt may ask as its minimum (the last of cmake --help-policy-list in
# that release): a later one is set only under if(POLICY ...), or that CMake stops on it
CMAKE_LAST_POLICY = {(3, 28): 155}
CMAKE_MINIMUM = re.compile(r'(?<![\w])cmake_minimum_required\s*\(\s*VERSION\s+(\d+(?:\.\d+)+)', re.IGNORECASE)
CMAKE_CONDITION = re.compile(r'(?<![\w])(if|elseif|else|endif)\s*\(', re.IGNORECASE)
CMAKE_POLICY_SET = re.compile(r'(?<![\w])cmake_policy\s*\(\s*SET\s+CMP(\d{4})', re.IGNORECASE)


def cmake_version(text):
    """major, minor, patch, tweak: CMake reads a missing one as 0, so 3.28.0 is 3.28"""
    parts = [int(part) for part in text.split('.')]
    return tuple(parts + [0] * (4 - len(parts)))


def unguarded_policies(code):
    """(policy number, line) of each cmake_policy(SET CMPnnnn) in code that no if(POLICY CMPnnnn) it sits in guards"""
    found, branches = [], []  # branches: the condition of each if a point sits in, '' for an else
    tokens = sorted(
        [(m.start(), 'condition', m) for m in CMAKE_CONDITION.finditer(code)]
        + [(m.start(), 'policy', m) for m in CMAKE_POLICY_SET.finditer(code)],
        key=lambda token: token[0],
    )
    for start, kind, match in tokens:
        if kind == 'policy':
            policy = int(match.group(1))
            guard = re.compile(rf'(?<![\w])(NOT\s+)?POLICY\s+CMP{policy:04d}(?![\w])', re.IGNORECASE)
            if not any((g := guard.search(branch)) and not g.group(1) for branch in branches):
                found.append((policy, code.count('\n', 0, start) + 1))
            continue
        word, depth, end = match.group(1).lower(), 1, match.end()
        while end < len(code) and depth:
            depth += {'(': 1, ')': -1}.get(code[end], 0)
            end += 1
        condition = code[match.end() : end - 1]
        if word == 'if':
            branches.append(condition)
        elif word == 'endif':
            if branches:
                branches.pop()
        elif branches:
            branches[-1] = condition if word == 'elseif' else ''
    return found


def check_cmake_versions(repo):
    """no CMake file asks a newer CMake than the root CMakeLists.txt's cmake_minimum_required, a project a test writes
    included, and a policy that CMake does not have is set only under if(POLICY ...)"""
    path = repo / 'CMakeLists.txt'
    if not path.exists():
        return []
    match = CMAKE_MINIMUM.search(cmake_code(path.read_text(encoding='utf-8', errors='replace'))[1])
    if not match:
        return ['CMakeLists.txt has no cmake_minimum_required(VERSION ...): the build states the CMake it needs there']
    shown_minimum, minimum = match.group(1), cmake_version(match.group(1))
    errors, last = [], CMAKE_LAST_POLICY.get(minimum[:2])
    if last is None:
        errors.append(
            f'CMakeLists.txt asks CMake {shown_minimum}, and CMAKE_LAST_POLICY in tests/architecture.py has no last '
            f'policy for {minimum[0]}.{minimum[1]}: add the last of cmake --help-policy-list in that release'
        )
    files = [path]
    for root in CMAKE_ROOTS:
        if (repo / root).is_dir():
            files += [p for p in (repo / root).rglob('*') if p.name == 'CMakeLists.txt' or p.suffix == '.cmake']
    for file in sorted(files):
        shown = file.relative_to(repo).as_posix()
        code, kept = cmake_code(file.read_text(encoding='utf-8', errors='replace'))
        for asked in CMAKE_MINIMUM.finditer(kept):
            if cmake_version(asked.group(1)) > minimum:
                errors.append(
                    f'{shown}:{kept.count(chr(10), 0, asked.start()) + 1}: asks CMake {asked.group(1)}, and the '
                    f"build's minimum (CMakeLists.txt) is {shown_minimum}, on which it stops: ask {shown_minimum} or "
                    'raise the minimum'
                )
        if last is not None:
            for policy, line in unguarded_policies(code):
                if policy > last:
                    errors.append(
                        f'{shown}:{line}: sets CMP{policy:04d} outside if(POLICY CMP{policy:04d}), and CMake '
                        f"{shown_minimum}, the build's minimum, stops on it: its last policy is CMP{last:04d}"
                    )
    return errors


DEVICE_MAKE = re.compile(r'(?<![A-Za-z0-9_])(vkCreateDevice|volkLoadDevice)\s*\(')
DEVICE_MAKER = 'src/common/gpu/vk/device.c'


def check_device_makers(repo):
    """only common/gpu/vk/device.c makes a Vulkan device and loads its commands (snx_vk_device_make): there every
    renderer's device takes the extension bans, the vk_ext_* settings and the check of each extension's commands, and
    a renderer turns an extension on only where a setting names it. The Android apps' own Vulkan is the app's, which
    android/bridge/hle_vulkan.c makes through the pointer the app asks for"""
    errors = []
    for path in sorted((repo / 'src').rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc', '.m', '.mm'}:
            continue
        shown = path.relative_to(repo).as_posix()
        if shown == DEVICE_MAKER:
            continue
        source = without_comments(path.read_text(encoding='utf-8', errors='replace'), literals=False)
        for match in DEVICE_MAKE.finditer(source):
            line = source.count('\n', 0, match.start()) + 1
            errors.append(
                f'{shown}:{line}: {match.group(1)}: only {DEVICE_MAKER} makes a Vulkan device and loads its commands, '
                "where the extension bans, the vk_ext_* settings and the check of each extension's commands hold for "
                'every renderer: ask snx_vk_device_make (common/gpu/vk/device.h) for it'
            )
    return errors


WEAK = re.compile(r'\[\[[^\]]*\bgnu::weak\b[^\]]*\]\]|__attribute__\s*\(\(\s*weak\s*\)\)')
# what no program of ours defines: another library's (jemalloc), the ELF linker's, and ours that only Apple's programs
# have or that no build links yet
WEAK_DECLARED = {
    'mallocx',
    'rallocx',
    'dallocx',
    'sallocx',
    'mallctl',
    '__ehdr_start',
    'snx_apple_power_watch',
    'snx_module_static',
}


def check_weak_declarations(repo):
    """no function or object of ours is declared weak and left for the program to define or not: where the program
    defines it, LLVM reads the declaration's address on Windows on Arm through a GOT slot, which COFF has not, so the
    load reads the function's own first words (lld's "misaligned ldr/str offset" when it starts at an odd word, and a
    test for null that is always true otherwise). The default is a weak definition beside its caller, which the
    program's own takes the place of (driver.c's snx_setting_known); WEAK_DECLARED names what no program defines"""
    errors = []
    for path in sorted((repo / 'src').rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc', '.m', '.mm'}:
            continue
        shown = path.relative_to(repo).as_posix()
        source = without_comments(path.read_text(encoding='utf-8', errors='replace'), literals=False)
        for match in WEAK.finditer(source):
            ends = re.search(r'[;{=]', source[match.end() :])
            if not ends or ends.group(0) != ';':
                continue  # a definition: a body or a value
            declared = source[match.end() : match.end() + ends.start()]
            name = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(|\[|$)', declared.strip())
            if name and name.group(1) in WEAK_DECLARED:
                continue
            line = source.count('\n', 0, match.start()) + 1
            errors.append(
                f'{shown}:{line}: {name.group(1) if name else declared.strip()} is declared weak with no definition: '
                'where the program defines it, LLVM reads it on Windows on Arm through a GOT slot COFF has not, the '
                'function\'s own first words (lld: "misaligned ldr/str offset"). Give its default as a weak definition '
                "beside its caller, as src/nx/runtime/driver/driver.c's snx_setting_known, or name it in "
                'WEAK_DECLARED (tests/architecture.py) when no program of ours defines it'
            )
    return errors


# LLVM's headers, and what of src/ builds on them: the backend's folder, and the files named here, which include one or
# a header of the project's that does. A file leaves the list when it stops, and one joins it only with the LLVM code of
# files that leave, never new code: what wants the code the native back ends leave asks for it through
# recompiler/fallback.h (a game module's own through nx/loader/module_fallback.h), and the rest of the recompiler
# (snx_recompiler_core) builds without LLVM
LLVM_HEADER = re.compile(r'llvm(?:-c)?/')
LLVM_FOLDER = 'src/common/cpu/recompiler/llvm/'
LLVM_USERS = {
    'src/common/cpu/arm/llvm.c',
    'src/common/cpu/arm/llvm.h',
    'src/common/cpu/recompiler/fallback_llvm.c',
    'src/nx/loader/module_fallback_llvm.c',
    'src/nx/loader/module_variants.c',
    'src/nx/loader/module_variants.h',
    'src/nx/runtime/driver/disasm.c',
    'src/nx/runtime/driver/llvm_tuning.c',
    'src/nx/runtime/settings_llvm.h',
}


def check_llvm(repo):
    """only the LLVM backend and the files of LLVM_USERS include LLVM's headers, themselves or through a header of the
    project's: a build without LLVM has no file of the rest to stop at"""
    src = (repo / 'src').resolve()
    includes, by = {}, {}
    for path in sorted(src.rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc', '.m', '.mm'}:
            continue
        shown = 'src/' + path.relative_to(src).as_posix()
        includes[shown] = {}
        for match in INCLUDE.finditer(path.read_text(encoding='utf-8', errors='replace')):
            name = match.group(2)
            if LLVM_HEADER.match(name):
                by.setdefault(shown, f'<{name}>')
            for candidate in (path.parent / name, src / name):
                if candidate.is_file() and candidate.resolve().is_relative_to(src):
                    includes[shown]['src/' + candidate.resolve().relative_to(src).as_posix()] = name
    pending = list(by)
    while pending:
        reached = pending.pop()
        for shown, names in includes.items():
            if reached in names and shown not in by:
                by[shown] = f'"{names[reached]}"'
                pending.append(shown)
    errors = [
        f'{shown}: includes {by[shown]}, which takes LLVM: the recompiler builds without it but for the files of '
        'LLVM_USERS (tests/architecture.py), which none joins: ask recompiler/fallback.h'
        for shown in sorted(by)
        if not shown.startswith(LLVM_FOLDER) and shown not in LLVM_USERS
    ]
    for shown in sorted(LLVM_USERS - set(by)):
        errors.append(f'tests/architecture.py: LLVM_USERS names {shown}, which takes no LLVM header: take it out')
    return errors


# A driver, a vendor or a device compared in the code: the VkDriverId's names, a comparison or a switch on an identity
# field, a device's or driver's name matched as text, and a GPU vendor's PCI ID compared
DRIVER_ID_NAME = re.compile(r'\bVK_DRIVER_ID_[A-Z0-9_]+\b')
DRIVER_FIELD = r'\b(?:vendorID|driverID|deviceID|vendor_id|driver_id|device_id|vendor)\b'
DRIVER_COMPARED = re.compile(
    rf'{DRIVER_FIELD}[^;\n]*?(?:==|!=)|(?:==|!=)[^;\n]*?{DRIVER_FIELD}|\bswitch\s*\([^)\n]*{DRIVER_FIELD}'
)
DRIVER_NAMED = re.compile(
    r'\b(?:str(?:n|case|ncase)?cmp|strstr|strcasestr|memcmp)\s*\([^;\n]*'
    r'\b(?:deviceName|driverName|driverInfo|driver_name|driver_info|gpu_name)\b'
)
VENDOR_COMPARED = re.compile(r'(?:==|!=)\s*0x(?:10DE|1002|8086|5143|13B5|106B|1010|14E4|144D)[uU]?\b', re.IGNORECASE)
DRIVER_READER = 'src/common/gpu/vk/extension_bans.c'
DRIVER_TABLE = 'src/common/gpu/vk/extension_bans.txt'
# The comparisons that are no choice of a way by driver, each line as the file writes it (its spaces as one) with why
# it stays. A driver that breaks something or runs it slower never joins them: it is a line of the table
DRIVER_CHOICES = {
    'src/common/gpu/vk/driver_tag.c': {
        'if (vendor != props->vendorID || device != props->deviceID) {': "a pipeline cache's header is another GPU's: "
        'the identity of a file, which chooses no way',
    },
    'src/common/gpu/vk/gpu_pstate.c': {
        '(mode == SNX_GPU_PSTATE_AUTO && (d->type == SNX_GPU_TYPE_DISCRETE || d->vendor == SNX_GPU_VENDOR_QUALCOMM));': (
            "gpu_pstate holds the clocks through the interface of the GPU's vendor, and Auto takes it where that "
            "interface is the phone's"
        ),
        'int way = d->vendor == SNX_GPU_VENDOR_NVIDIA;': "which vendor's interface holds the clocks",
        'way = way || d->vendor == SNX_GPU_VENDOR_AMD;': "which vendor's interface holds the clocks",
        'way = way || d->vendor == SNX_GPU_VENDOR_QUALCOMM;': "which vendor's interface holds the clocks",
        'if (d->vendor == SNX_GPU_VENDOR_NVIDIA) {': "NVIDIA's interface: VK_NV_low_latency2's boost",
        'if (d->vendor == SNX_GPU_VENDOR_QUALCOMM) return kgsl_hold(d, head, say);': "Qualcomm's interface: kgsl",
    },
    'src/nx/gpu/vk/device.c': {
        'if (pstate.vendor == SNX_GPU_VENDOR_NVIDIA && snx_gpu_pstate_wanted(d->pstate, &pstate)) {': (
            "gpu_pstate's boost on NVIDIA needs VK_NV_low_latency2 enabled: the vendor's own interface"
        ),
    },
    'src/nx/gpu/vk/upscaler/dlss.cpp': {
        'if (props.vendorID != 0x10DE) {': "DLSS is NVIDIA's own interface (Streamline): it runs on its GPUs alone",
    },
    'src/nx/gpu/vk/ui.c': {
        'switch (g_vk.props.vendorID) {': "a GPU that renders in tiles, which no Vulkan property tells: the hardware's "
        'architecture by its maker, whatever driver runs it',
    },
    'src/nx/gpu/vk/vk.h': {
        'return vendor == 0x13B5u ? VK_ASTC_ENC_ARM : vendor == 0x5143u ? VK_ASTC_ENC_QCOM : VK_ASTC_ENC_GENERIC;': (
            'the ASTC encoder written for the GPU family, whatever driver runs it: three programs with one result'
        ),
    },
    'src/gui/gpu_driver_fetch.c': {
        'if (strncmp(gpu_name, "Adreno ", 7)) return 0;': "the model number in an Adreno's name, for the list of "
        'driver packages a phone can install',
        'if (strncmp(gpu_name, family[f].name, n)) continue;': "the GPU family in a device's name, for the same list",
    },
}


def check_driver_choices(repo):
    """no source of src/ chooses a way by comparing a driver, a vendor or a device: such a choice is a line of
    src/common/gpu/vk/extension_bans.txt, "off" where a driver breaks something or runs it slower (vk_ban) and "prefer"
    where only some drivers take a way (vk_prefer), with its source, the case that tests it again and the setting that
    goes over it. The table's reader matches the lines; DRIVER_CHOICES lists the comparisons that choose no way"""
    errors = []
    seen = {name: set() for name in DRIVER_CHOICES}
    for path in sorted((repo / 'src').rglob('*')):
        if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc', '.m', '.mm'}:
            continue
        shown = path.relative_to(repo).as_posix()
        if shown == DRIVER_READER:
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        kept = without_comments(text).split('\n')
        allowed = DRIVER_CHOICES.get(shown, {})
        for number, line in enumerate(without_comments(text, literals=False).split('\n'), 1):
            found = (
                DRIVER_ID_NAME.search(line)
                or DRIVER_COMPARED.search(line)
                or DRIVER_NAMED.search(line)
                or VENDOR_COMPARED.search(line)
            )
            if not found or re.match(r'\s*#\s*define\b', line):
                continue
            written = ' '.join(kept[number - 1].split())
            if written in allowed:
                seen[shown].add(written)
                continue
            errors.append(
                f'{shown}:{number}: "{written}" compares a driver, a vendor or a device, which only the extension '
                f'bans do: put it in {DRIVER_TABLE}, an "off" line asked with vk_ban where the driver breaks '
                'something or runs it slower, a "prefer" line asked with vk_prefer where only some drivers take a '
                'way, each with its source and the case that tests it again. A comparison that chooses no way (a '
                "cache's identity, a vendor's own interface) joins DRIVER_CHOICES in tests/architecture.py with its "
                'reason'
            )
    for name, lines in DRIVER_CHOICES.items():
        for written in lines:
            if written not in seen[name]:
                errors.append(
                    f'tests/architecture.py: DRIVER_CHOICES allows "{written}" in {name}, which has no such line: '
                    'take the entry out'
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
                'scarletnx-nds',
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
            'scarletnx-nds',
            (
                'scarletnx-nx',
                'scarletnx-gnuabi',
                'scarletnx-3ds',
                'scarletnx-agb',
                'snx_ctr_board',
                'snx_driver',
                'snx_nx_module',
            ),
        ),
        (
            'snx_nds_program',
            ('scarletnx-nx', 'scarletnx-gnuabi', 'scarletnx-3ds', 'scarletnx-agb', 'snx_driver', 'snx_nx_module'),
        ),
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
        ('snx_recompiler_core', ('snx_llvm', 'snx_arm_llvm', 'snx_recompiler_jit', 'scarletnx-common')),
        ('snx_nx_fallback_none', ('snx_llvm', 'snx_arm_llvm', 'snx_recompiler_jit', 'snx_nx_fallback_llvm')),
    ):
        if owner not in by_name:
            if owner == 'scarletnx-common':
                errors.append(f'CMake graph is missing target {owner}')
            continue
        dependencies = closure(by_name[owner])
        for target in forbidden:
            if target in dependencies:
                errors.append(f'{owner} transitively links forbidden target {target}')
        if owner in ('snx_recompiler_core', 'snx_nx_fallback_none'):
            for target in dependencies:
                if re.search(r'(?:^|[/\\]|-l|lib)LLVM', target):
                    errors.append(f'{owner} transitively links LLVM: {target}')
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
                'scarletnx-nds': 'nds',
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
                # the Windows programs' manifest and icon (src/common/platform/windows/runtime.rc), which each program
                # compiles into itself as a source of its own
                if not path.is_relative_to(src):
                    continue
                boundary = path.relative_to(src).parts[0]
                if boundary != owner and not (boundary == 'common' and path.suffix == '.rc'):
                    errors.append(f'{entry["name"]} compiles a source owned by another runtime: {path}')
    return errors


# a header whose functions only its library defines: a target compiling a source that includes it links that library,
# itself or through what it links, or a build without another target that links it fails at its link
LINK_NEEDS = {
    'common/utilities/build_info.h': 'snx_build_info',
    'common/gpu/vk/device_settings.h': 'snx_settings_runtime',  # g_vk_extension_settings
}


def check_link_needs(repo, build, graph):
    source = graph.read_text(encoding='utf-8')
    nodes = {
        node: label.split('\\n', 1)[0] for node, label in re.findall(r'"([^"]+)"\s*\[\s*label\s*=\s*"([^"]+)"', source)
    }
    edges = {}
    for left, right in re.findall(r'"([^"]+)"\s*->\s*"([^"]+)"', source):
        edges.setdefault(nodes.get(left, left), set()).add(nodes.get(right, right))

    def links(name):
        result, pending = set(), [name]
        while pending:
            for child in edges.get(pending.pop(), ()):
                if child not in result:
                    result.add(child)
                    pending.append(child)
        return result

    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text(encoding='utf-8'))
    errors = []
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            needs = set()
            for item in target.get('sources', ()):
                path = Path(item['path'])
                path = (path if path.is_absolute() else repo / path).resolve()
                if path.suffix not in {'.c', '.cpp'} or not path.is_file() or not path.is_relative_to(repo.resolve()):
                    continue
                for match in INCLUDE.finditer(path.read_text(encoding='utf-8', errors='replace')):
                    library = LINK_NEEDS.get(match.group(2))
                    if library and library != entry['name']:
                        needs.add((library, match.group(2), path.relative_to(repo.resolve())))
            linked = links(entry['name'])
            for library, header, path in sorted(needs):
                if library not in linked:
                    errors.append(
                        f'{entry["name"]} compiles {path}, which includes {header}, but does not link {library}'
                    )
    return errors


# add_library( NAME in the project's own CMake files: a library a target names that no target of this configuration
# is goes to the linker as -lNAME, which fails at the link instead of the configure
ADD_LIBRARY = re.compile(r'^\s*add_library\(\s*([A-Za-z0-9_.+-]+)', re.M)


def project_libraries(repo):
    names = set()
    for path in [repo / 'CMakeLists.txt', *(repo / folder for folder in ('src', 'tests', 'cmake', 'llvm-backend'))]:
        files = [path] if path.is_file() else [*path.rglob('CMakeLists.txt'), *path.rglob('*.cmake')]
        for file in files:
            names.update(ADD_LIBRARY.findall(file.read_text(encoding='utf-8', errors='replace')))
    return names


def check_link_names(repo, build):
    """A target that links a library of the project's by a name no target of this configuration has: the library's
    add_library is skipped here (an if, or a return() before it), and the link fails on -lNAME"""
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text(encoding='utf-8'))
    ours = project_libraries(repo)
    errors = []
    for configuration in model['configurations']:
        defined = {entry['name'] for entry in configuration['targets']}
        for entry in configuration['targets']:
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            for fragment in target.get('link', {}).get('commandFragments', ()):
                name = fragment.get('fragment', '').strip()
                if fragment.get('role') != 'libraries' or not name.startswith('-l'):
                    continue
                if name[2:] in ours and name[2:] not in defined:
                    errors.append(
                        f'{entry["name"]} links {name[2:]}, a library of the project that this configuration does not '
                        f'define (its add_library is skipped here): the link fails on {name}'
                    )
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


# a service object's dispatcher or destructor, as the HLE defines one: what a save state holds a pointer to
SVCOBJ_FN = re.compile(
    r'^(?:static\s+)?(?:int|void)\s+([A-Za-z_]\w*)\s*\(\s*(?:struct\s+)?SvcObj\s*\*\s*\w*\s*(?:,\s*IpcCtx\s*\*\s*\w*\s*)?\)\s*\{',
    re.MULTILINE,
)
STATE_FN_ENTRY = re.compile(r'STATE_FN\(\s*"(\w+)"\s*,\s*(\w+)\s*\)')
STATE_FN_TABLE = re.compile(r'STATE_FN_TABLE\(\s*(\w+)\s*,')


def check_state_fns(repo):
    """every dispatcher or destructor of a service object in src/nx/hle that the code stores (its name not called) is in
    its file's STATE_FN_TABLE, and every such table is one of state_fns.c's TABLES: a save state names the functions
    a game's objects hold, and can't save one no table names"""
    errors = []
    tables_c = without_comments((repo / 'src/nx/runtime/state_fns.c').read_text(encoding='utf-8', errors='replace'))
    for path in sorted((repo / 'src/nx/hle').rglob('*.c')):
        source = without_comments(path.read_text(encoding='utf-8', errors='replace'), literals=False)
        named = without_comments(path.read_text(encoding='utf-8', errors='replace'))
        shown = path.relative_to(repo).as_posix()
        entries = {fn for _, fn in STATE_FN_ENTRY.findall(named)}
        for match in SVCOBJ_FN.finditer(source):
            name = match.group(1)
            stored = re.search(
                r'\b' + re.escape(name) + r'\b(?!\s*\()', source[: match.start()] + source[match.end() :]
            )
            if stored and name not in entries:
                line = source.count('\n', 0, match.start()) + 1
                errors.append(
                    f"{shown}:{line}: {name} is stored as a service object's function but is in no STATE_FN_TABLE of "
                    'this file: a save state of a game holding such an object fails ("has no name in state_fns.h\'s '
                    'tables"). Add STATE_FN("<file>", ' + name + ") to the file's table"
                )
        for table in STATE_FN_TABLE.findall(named):
            if not re.search(r'&\s*' + re.escape(table) + r'\b', tables_c):
                errors.append(
                    f"{shown}: {table} is not one of src/nx/runtime/state_fns.c's TABLES: no save names its functions"
                )
    return errors


CONFIG_CALL = re.compile(
    r'(?<![A-Za-z0-9_])(snx_ini_start|snx_ini_next|snx_ini_put|snx_ini_record|snx_ini_drop|snx_ini_value|snx_ini_has|'
    r'snx_lines_add|snx_config_read|snx_config_write|snx_config_keys|snx_config_kept|snx_folders_read)\s*\('
)
CONFIG_HEAD = re.compile(r'^(?:static |const |unsigned |struct |enum |inline )*[A-Za-z_]\w*[\s*]+\**([A-Za-z_]\w*)\(', re.M)
CONFIG_NAME = re.compile(r'"([A-Za-z0-9_.-]*\.(?:ini|picks|folders|txt))"')
CONFIG_TEXT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
CONFIG_ID = re.compile(r'\.id\s*=\s*"([^"]+)"')
CONFIG_OWN = {'src/common/utilities/ini_lines.c', 'src/common/utilities/config_file.c'}


def config_functions(source):
    """each function a C source defines, with where it starts"""
    return [(m.start(), m.group(1)) for m in CONFIG_HEAD.finditer(source)]


def check_config_files(repo):
    """every function in src/ that reads or writes a settings file (the ini reader's and config_file.h's calls, and the
    names of its files) is one of tests/config_files.json's, each such file has an adapter that runs it through
    tests/config_files.h's situations, and every adapter's id is the registry's"""
    errors = []
    registry = json.loads((repo / 'tests/config_files.json').read_text(encoding='utf-8'))
    listed, names, ids = {}, set(registry['not_config']) - {'doc'}, set()
    for entry in registry['files']:
        ids.add(entry['id'])
        names.update(entry['names'])
        for fn in entry['functions']:
            listed[fn] = entry['id']
    for name, record in registry['records'].items():
        if name != 'doc':
            names.add(name)
            for fn in record['functions']:
                listed[fn] = name
    seen = set()
    for path in sorted((repo / 'src').rglob('*.[ch]')):
        shown = path.relative_to(repo).as_posix()
        text = path.read_text(encoding='utf-8', errors='replace')
        for match in CONFIG_NAME.finditer(text):
            if match.group(1) not in names:
                line = text.count('\n', 0, match.start()) + 1
                errors.append(
                    f'{shown}:{line}: "{match.group(1)}" names a file that is neither a settings file of '
                    "tests/config_files.json nor one of its not_config: a settings file follows the rule of "
                    'tests/config_files.h; list it there with its readers, or say why it is none'
                )
        if path.suffix != '.c':
            continue
        source = CONFIG_TEXT.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)
        heads = config_functions(source)
        for _, fn in heads:
            seen.add(f'{shown}:{fn}')
        if shown in CONFIG_OWN:
            continue
        for match in CONFIG_CALL.finditer(source):
            before = [fn for at, fn in heads if at < match.start()]
            where = f'{shown}:{before[-1] if before else "?"}'
            if where not in listed:
                line = source.count('\n', 0, match.start()) + 1
                errors.append(
                    f'{shown}:{line}: {match.group(1)} in {where.split(":")[1]}, which reads or writes a settings file '
                    'tests/config_files.json does not list: add the function to its file there, so '
                    "tests/config_files.h's situations run what it does"
                )
                listed[where] = None  # once
    for fn, owner in sorted(listed.items(), key=lambda item: item[0]):
        if owner is not None and fn not in seen:
            errors.append(f'tests/config_files.json: {fn} ({owner}) is no function in src/ any more: take it out')
    adapters = {}
    for path in sorted((repo / 'tests').glob('config_files_*.c')):
        if path.name == 'config_files_checker.c':  # the runner's own test, of a reader made up
            continue
        for match in CONFIG_ID.finditer(path.read_text(encoding='utf-8', errors='replace')):
            adapters[match.group(1)] = path.relative_to(repo).as_posix()
    for entry in registry['files']:
        given, has = entry['adapter'], adapters.get(entry['id'])
        if given is None:
            errors.append(
                f'tests/config_files.json: {entry["id"]} has no adapter: every settings file runs through '
                "tests/config_files.h's situations"
            )
        elif given != has:
            errors.append(
                f'tests/config_files.json: {entry["id"]}\'s adapter is {given}, and '
                + (f'its id is in {has}' if has else 'no tests/config_files_*.c gives that id')
            )
    for found, where in sorted(adapters.items()):
        if found not in ids:
            errors.append(f'{where}: {found} is no id of tests/config_files.json')
    return errors


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
        + check_driver_choices(args.repo.resolve())
        + check_include_flags(args.repo.resolve())
        + check_environment(args.repo.resolve())
        + check_driver_settings(args.repo.resolve())
        + check_test_threads(args.repo.resolve())
        + check_window_pixels(args.repo.resolve())
        + check_detached_threads(args.repo.resolve())
        + check_state_fns(args.repo.resolve())
        + check_dynamic_descriptors(args.repo.resolve())
        + check_packages(args.repo.resolve())
        + check_cmake_versions(args.repo.resolve())
        + check_device_makers(args.repo.resolve())
        + check_weak_declarations(args.repo.resolve())
        + check_config_files(args.repo.resolve())
        + check_llvm(args.repo.resolve())
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
        errors.extend(check_link_needs(args.repo.resolve(), args.build.resolve(), graph))
        errors.extend(check_link_names(args.repo.resolve(), args.build.resolve()))
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
