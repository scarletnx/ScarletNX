#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Run the checks that CI runs, as far as this machine can run them.

CI builds on Linux x86-64 and AArch64, Windows, macOS, iOS and Android, and runs the tests under the sanitizers and
lavapipe. A developer's machine is one host: `native` runs that host's checks, and the other commands run the checks of
another host where a cross build, a container or a device can. `doctor` says which of them this machine can run, and
what a missing tool would add.

    scripts/ci-local.py doctor
    scripts/ci-local.py static
    scripts/ci-local.py native
    scripts/ci-local.py native --vulkan --sanitizers
    scripts/ci-local.py preflight                 # quick: merge, docs and the needed host tests
    scripts/ci-local.py preflight --containers    # plus the Linux job of this architecture
    scripts/ci-local.py preflight --both --sanitizers  # the full gate
    scripts/ci-local.py preflight --dry-run       # the plan, without a build
    scripts/ci-local.py preflight --merge-only
    scripts/ci-local.py linux --arch x86_64
    scripts/ci-local.py android --install
    scripts/ci-local.py ios --simulator
    scripts/ci-local.py windows --test

A check that this machine cannot run is said, not failed, unless --strict is given. A check that runs and fails makes
the command exit nonzero.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IS_MAC = platform.system() == 'Darwin'
IS_LINUX = platform.system() == 'Linux'
MACHINE = platform.machine().lower()
IS_ARM = MACHINE in ('arm64', 'aarch64')


# ---- process handling ----
class Failures:
    """the checks that ran and failed, and the checks this machine could not run"""

    def __init__(self) -> None:
        self.failed: list[str] = []
        self.skipped: list[str] = []

    def failed_check(self, what: str, why: str = '') -> None:
        self.failed.append(what)
        print(f'FAILED: {what}' + (f': {why}' if why else ''), file=sys.stderr)

    def skip(self, what: str, why: str) -> None:
        self.skipped.append(what)
        print(f'not run: {what}: {why}')


def run(cmd: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None,
        timeout: float | None = None, check: bool = True) -> int:
    """run a command, its output on this terminal; its status, which `check` may turn into an exit"""
    print('+ ' + ' '.join(shlex.quote(str(c)) for c in cmd), flush=True)
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    try:
        status = subprocess.run([str(c) for c in cmd], cwd=cwd, env=full_env, timeout=timeout).returncode
    except FileNotFoundError:
        status = 127
        print(f'{cmd[0]} is not on PATH', file=sys.stderr)
    except subprocess.TimeoutExpired:
        status = 124
        print(f'{cmd[0]} did not end in {timeout} s', file=sys.stderr)
    if check and status:
        raise SystemExit(status)
    return status


def output(cmd: list[str], *, cwd: Path = ROOT) -> str:
    """a command's output, empty when it cannot run"""
    try:
        return subprocess.run([str(c) for c in cmd], cwd=cwd, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=60).stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return ''


def quiet_ok(cmd: list[str], *, cwd: Path = ROOT) -> bool:
    """whether a command exits 0, without its output"""
    try:
        return subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=60).returncode == 0
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False


def git(args: list[str], *, env: dict[str, str] | None = None, input_text: str | None = None):
    """git for a check: its output captured, and its status on the result"""
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    return subprocess.run(['git', *args], cwd=ROOT, env=full_env, text=True, input=input_text,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def tool(*names: str) -> str | None:
    """the first name on PATH"""
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


BATTERY_ANNOUNCED = False


def battery_note() -> None:
    """say once that the heavy checks are expensive on battery"""
    global BATTERY_ANNOUNCED
    if BATTERY_ANNOUNCED or power_state() != 'battery':
        return
    BATTERY_ANNOUNCED = True
    print('warning: this machine runs on battery power. The build and container checks are expensive. '
          'Use --static-only, or --jobs 2 --cpus 2, and connect power for the full gate.')


def power_state() -> str | None:
    """AC, battery, or None when the machine has no battery"""
    if IS_MAC:
        state = output(['pmset', '-g', 'batt'])
        if 'Battery Power' in state:
            return 'battery'
        if 'AC Power' in state:
            return 'ac'
        return None
    if IS_LINUX:
        supplies = Path('/sys/class/power_supply')
        statuses = list(supplies.glob('BAT*/status')) if supplies.is_dir() else []
        if not statuses:
            return None
        for status in statuses:
            try:
                if status.read_text().strip().lower() == 'discharging':
                    return 'battery'
            except OSError:
                continue
        return 'ac'
    return None


def jobs() -> int:
    return max(1, os.cpu_count() or 2)


def parallel_jobs(args: argparse.Namespace | None = None) -> int:
    """the build and test parallelism: four by default, as the CI runners, and --jobs overrides it"""
    return max(1, getattr(args, 'jobs', 0) or min(4, jobs()))


def modern_python() -> str:
    """a Python 3.10 or newer, as the tools ask for: the Mac's /usr/bin/python3 is older"""
    if sys.version_info >= (3, 10):
        return sys.executable
    for name in ('python3.15', 'python3.14', 'python3.13', 'python3.12', 'python3.11', 'python3.10', 'python3'):
        found = tool(name)
        if found:
            minor = output([found, '-c', 'import sys; print(sys.version_info[1])'])
            if minor.isdigit() and int(minor) >= 10:
                return found
    return sys.executable


PYTHON = modern_python()


def llvm_config() -> str | None:
    """the llvm-config of the LLVM the recompiler links, as cmake/llvm.cmake looks for one"""
    named = os.environ.get('SNX_LLVM_CONFIG')
    if named:
        return named
    found = tool('llvm-config') or tool(*(f'llvm-config-{n}' for n in range(30, 16, -1)))
    if found:
        return found
    if IS_MAC:
        brew = tool('brew')
        if brew:
            prefix = output([brew, '--prefix', 'llvm'])
            candidate = Path(prefix) / 'bin/llvm-config' if prefix else None
            if candidate and candidate.is_file():
                return str(candidate)
    return None


def llvm_flag() -> list[str]:
    """the CMake argument that names the host's LLVM, empty when the configure can find it itself"""
    config = llvm_config()
    if not config:
        return []
    return [f'-DSNX_LLVM_CONFIG={config}']


def ccache_flag() -> list[str]:
    return ['-DCMAKE_C_COMPILER_LAUNCHER=ccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache'] if tool('ccache') else []


def unicorn_ready() -> bool:
    """whether unicorn is in this Python, or in the .cache/ci-unicorn-native the script makes"""
    return quiet_ok([PYTHON, '-c', 'import unicorn']) or quiet_ok(
        [str(ROOT / '.cache/ci-unicorn-native/bin/python'), '-c', 'import unicorn'])


def unicorn_python() -> str | None:
    """the Python with unicorn 2.1.4, in .cache/ci-unicorn-native: the A32 and Thumb differentials' reference"""
    python = ROOT / '.cache/ci-unicorn-native/bin/python'
    if python.is_file() and quiet_ok([str(python), '-c', 'import unicorn']):
        return str(python)
    venv = ROOT / '.cache/ci-unicorn-native'
    if run([PYTHON, '-m', 'venv', str(venv)], check=False):
        return None
    for _ in range(3):
        if not run([str(python), '-m', 'pip', 'install', '--quiet', 'unicorn==2.1.4'], check=False):
            return str(python)
    return None


def preset_build_dir(preset: str) -> Path:
    return ROOT / 'build' / preset


STATIC_PATHS = ('docs/', '.claude/', '.github/', 'scripts/')
STATIC_FILES = {'CLAUDE.md', 'README.md', 'CONTRIBUTING.md', 'ROADMAP.md', 'LICENSE', '.gitignore', '.clang-format',
                '.mcp.json', 'pyproject.toml'}


def changed_files(base: str) -> list[str] | None:
    """the tracked and untracked files that differ from the merge base, or None when git cannot say"""
    merge_base = git(['merge-base', 'HEAD', base])
    if merge_base.returncode == 0 and merge_base.stdout.strip():
        base = merge_base.stdout.strip()
    tracked = git(['diff', '--name-only', base])
    if tracked.returncode:
        return None
    names = [line for line in tracked.stdout.splitlines() if line]
    untracked = git(['ls-files', '--others', '--exclude-standard'])
    if untracked.returncode == 0:
        names.extend(line for line in untracked.stdout.splitlines() if line)
    return sorted(set(names))


def static_only(paths: list[str]) -> bool:
    """whether every changed file is docs, agent text or a script that the static checks cover"""
    return bool(paths) and all(path in STATIC_FILES or path.startswith(STATIC_PATHS) for path in paths)


def check_merge(args: argparse.Namespace, failures: Failures) -> None:
    """the working tree, uncommitted files included, against the latest main

    A temporary index and commit stand for the working tree, so no branch, index or file changes. git merge-tree then
    says whether that tree merges with the base. The files docs/, CLAUDE.md and .claude/ are part of it.
    """
    if not args.merge:
        failures.skip('the merge with main', '--no-merge')
        return
    if args.fetch:
        remotes = output(['git', 'remote']).split()
        if 'origin' in remotes:
            if run(['git', 'fetch', '--quiet', 'origin', 'main'], check=False):
                print('warning: origin/main could not be fetched; the local ref is used', file=sys.stderr)
    base = args.base
    if not quiet_ok(['git', 'rev-parse', '--verify', base]):
        for alternate in ('origin/main', 'main'):
            if quiet_ok(['git', 'rev-parse', '--verify', alternate]):
                base = alternate
                break
        else:
            failures.skip('the merge with main', f'{args.base} is not here: fetch it, or pass --base')
            return
    index = tempfile.NamedTemporaryFile(prefix='ci-local-index-', delete=False)
    index.close()
    env = {'GIT_INDEX_FILE': index.name}
    try:
        for command in (['read-tree', 'HEAD'], ['add', '-A']):
            result = git(command, env=env)
            if result.returncode:
                failures.failed_check('the merge with ' + base, f'git {command[0]} failed')
                print(result.stdout)
                return
        tree = git(['write-tree'], env=env)
        if tree.returncode:
            failures.failed_check('the merge with ' + base, 'git write-tree failed')
            print(tree.stdout)
            return
        commit = git(['commit-tree', tree.stdout.strip(), '-p', 'HEAD'], input_text='ci-local preflight\n')
        if commit.returncode:
            failures.failed_check('the merge with ' + base, 'git commit-tree failed')
            print(commit.stdout)
            return
        merged = git(['merge-tree', '--write-tree', '--name-only', base, commit.stdout.strip()])
        if merged.returncode and 'unknown option' in merged.stdout:
            failures.skip('the merge with main', 'this git has no merge-tree --write-tree')
            return
        if merged.returncode:
            paths = [line for line in merged.stdout.splitlines()[1:] if line.strip()]
            failures.failed_check('the merge with ' + base, 'the changes conflict with it')
            for path in paths:
                print('  ' + path)
            if not paths:
                print(merged.stdout)
            return
        print(f'the working tree merges with {base}')
    finally:
        for path in (index.name, index.name + '.lock'):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass


# ---- the checks that need no build ----
def static_checks(failures: Failures) -> None:
    print('\n== tracked material and docs, as pr-checks.yml runs them ==')
    commands = [
        ([PYTHON, 'tests/tracked_material.py', '--self-test'], 'tracked material self-test'),
        ([PYTHON, 'tests/tracked_material.py', '.'], 'tracked material'),
        ([PYTHON, 'tests/docs_test.py'], 'docs_test'),
        ([PYTHON, 'scripts/check-docs.py'], 'check-docs'),
    ]
    for cmd, what in commands:
        status = run(cmd, check=False)
        if status == 77:
            failures.skip(what, 'the check skipped itself (no git or no checkout)')
        elif status:
            failures.failed_check(what)

    print('\n== the Python the CI runs from tools/ ==')
    if run([PYTHON, '-m', 'compileall', '-q', 'tools'], check=False):
        failures.failed_check('python -m compileall tools')
    if run([PYTHON, 'tools/run_game.py', '--help'], check=False):
        failures.failed_check('tools/run_game.py --help')


# ---- ctest and its report ----
def ctest(build_dir: Path, failures: Failures, *, what: str, regex: str | None = None,
          env: dict[str, str] | None = None, timeout: float | None = None, parallel: int | None = None) -> None:
    """ctest on a build folder, its JUnit report naming what failed and what the host skipped"""
    report = build_dir / 'ci-local-junit.xml'
    cmd = ['ctest', '--test-dir', str(build_dir), '--output-on-failure',
           '--parallel', str(parallel or min(4, jobs())), '--output-junit', str(report)]
    if regex:
        cmd.extend(['-R', regex])
    if run(cmd, env=env, timeout=timeout, check=False):
        failures.failed_check(what)
    if not report.is_file():
        return
    try:
        root = ET.parse(report).getroot()
    except ET.ParseError:
        return
    cases = root.iter('testcase')
    skipped = [case.get('name', '?') for case in cases if case.find('skipped') is not None]
    if skipped:
        print(f'{len(skipped)} test(s) skipped by the host: ' + ', '.join(sorted(skipped)))
        print('a skip is coverage this machine did not give; doctor names the tools that add some')


def system_sdl3_tests(args: argparse.Namespace, failures: Failures) -> None:
    """the Mac's platform tests against Homebrew's shared SDL3, as the apple job builds them"""
    if not IS_MAC:
        return
    if not args.system_sdl3:
        failures.skip('the shared SDL3 tests', '--no-system-sdl3')
        return
    brew = tool('brew')
    if not brew or not output([brew, '--prefix', 'sdl3']):
        failures.skip('the shared SDL3 tests', 'no Homebrew sdl3: brew install sdl3')
        return
    print('\n== the platform tests against Homebrew\'s shared SDL3 ==')
    build = ROOT / 'build' / 'sdl3-shared'
    cmd = ['cmake', '--preset', 'macos', '-B', str(build), '-DSNX_SYSTEM_SDL3=ON', '-DSNX_DEPLOY=OFF']
    cmd += llvm_flag() + ccache_flag()
    if run(cmd, check=False):
        failures.failed_check('the shared SDL3 configure')
        return
    if run(['cmake', '--build', str(build), '--parallel', str(parallel_jobs(args)), '--target',
            'snx-test-memory-own', 'snx-test-platform-posix', 'snx-test-memory-alias'], check=False):
        failures.failed_check('the shared SDL3 platform tests')


def screen_tests(build_dir: Path, failures: Failures, *, requested: bool = False) -> None:
    """the menu's screen tests, which CMake registers on Linux and Windows only"""
    if not IS_MAC:
        return
    if not requested:
        print('note: CMake registers the screen tests (gui_sdl_events, gui_setup) on Linux and Windows only; '
              'pass --screen for the macOS branch, or run the Linux job')
        return
    print('\n== the screen tests macOS builds but ctest does not register ==')
    # the test's own temporary folder: the Mac's /var/folders/... path is long enough to wrap the screen's text, and
    # /tmp is a link to /private/tmp, which the test compares as a string. /private/tmp is short and real
    env = {
        'TMPDIR': '/private/tmp',
        'SDL_VIDEO_DRIVER': 'dummy',
        'SDL_RENDER_DRIVER': 'software',
        'SDL_FILE_DIALOG_DRIVER': 'none',
        'SDL_PREFERRED_LOCALES': 'en_US',
        'SNX_GUI_FAILED_SHOT': str(build_dir / 'gui-failed.bmp'),
    }
    for binary, what in (('src/gui/snx-test-gui-sdl', 'gui_sdl_events'),
                         ('src/gui/snx-test-gui-setup', 'gui_setup')):
        path = build_dir / binary
        if not path.is_file():
            failures.skip(what, f'{path} is not built')
            continue
        shot = Path(env['SNX_GUI_FAILED_SHOT'])
        shot.unlink(missing_ok=True)
        print(f'+ {path} (the macOS branch of {what})')
        status = run([path], env=env, timeout=300, check=False)
        if status == 77:
            failures.skip(what, 'the test skipped itself on this machine')
        elif status:
            failures.failed_check(what, f'exit {status}')
            if shot.is_file():
                png = shot.with_suffix('.png')
                if tool('sips') and not run(['sips', '-s', 'format', 'png', str(shot), '--out', str(png)], check=False):
                    print(f'the frame of the failure is {png}')
                else:
                    print(f'the frame of the failure is {shot}')


# ---- native ----
def configure_native(args: argparse.Namespace, build_dir: Path, *, vulkan: bool = False) -> None:
    preset = 'macos' if IS_MAC else 'default'
    cmd = ['cmake', '--preset', preset, '-B', str(build_dir)]
    cmd += llvm_flag()
    cmd += ccache_flag()
    if args.vp9:
        cmd.append('-DSNX_TEST_VP9_VECTORS=ON')
    if args.arm_diff:
        cmd.append('-DSNX_TEST_ARM_DIFF=ON')
        python = unicorn_python()
        if python:
            cmd.append(f'-DPython3_EXECUTABLE={python}')
    if args.sanitizers:
        cmd += ['-DSNX_TEST_SANITIZERS=ON', '-DSNX_DEPLOY=OFF']
    if vulkan:
        cmd += ['-DSNX_BUILD_GUI=OFF', '-DSNX_DEPLOY=OFF', '-DSNX_TEST_VULKAN=ON']
    run(cmd)


def native(args: argparse.Namespace, failures: Failures, *, static: bool = True) -> None:
    if not (IS_MAC or IS_LINUX):
        failures.skip('native', f'no preset for {platform.system()}')
        return
    if not llvm_config():
        failures.skip('native', 'no llvm-config: install LLVM, or set SNX_LLVM_CONFIG')
        return
    if args.no_build and args.build_dir:
        build_dir = Path(args.build_dir).resolve()
    else:
        preset = 'macos' if IS_MAC else 'default'
        build_dir = (Path(args.build_dir).resolve() if args.build_dir
                     else preset_build_dir('sanitizers' if args.sanitizers else preset))
    battery_note()
    print(f'\n== the native build and tests in {build_dir} ==')
    if not args.no_build:
        configure_native(args, build_dir)
        run(['cmake', '--build', str(build_dir), '--parallel', str(parallel_jobs(args))])
    if run(['clang', '-fsyntax-only', '-Wall', '-I', 'src', 'src/common/cpu/aarch64/cpu_helpers.c'], check=False):
        failures.failed_check('the ABI helpers compile')
    if args.sanitizers:
        env = {'ASAN_OPTIONS': 'halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
               'LSAN_OPTIONS': 'use_stacks=0:use_registers=0'}
    else:
        env = None
    ctest(build_dir, failures, what='ctest (native)', env=env, parallel=parallel_jobs(args))
    system_sdl3_tests(args, failures)
    screen_tests(build_dir, failures, requested=args.screen)
    if static:
        static_checks(failures)
    if args.vulkan:
        headless = ROOT / 'build' / 'headless-ci'
        print(f'\n== headless Vulkan in {headless} ==')
        configure_native(args, headless, vulkan=True)
        run(['cmake', '--build', str(headless), '--parallel', str(parallel_jobs(args))])
        ctest(headless, failures, what='ctest (headless Vulkan)',
              regex='^(vulkan_headless|vulkan_headless_options|vulkan_headless_vic_validation|nx_runtime_homebrew|'
                    'nx_runtime_homebrew_64k_pages)$', parallel=parallel_jobs(args))
    if IS_LINUX and not args.arm_diff:
        failures.skip('the A32/Thumb differential', 'pass --arm-diff and install unicorn 2.1.4')


# ---- Linux in a container ----
def container_engine(preferred: str | None = None) -> str | None:
    """the container engine to use: Podman first, its machine is rootless and has no root daemon"""
    if preferred:
        return tool(preferred)
    return tool('podman') or tool('docker')


def rootless_engine(engine: str) -> bool | None:
    """whether the engine runs rootless: None when the engine cannot say or its machine is down"""
    name = os.path.basename(engine)
    option = '{{.Host.Security.Rootless}}' if name == 'podman' else '{{json .SecurityOptions}}'
    try:
        result = subprocess.run([engine, 'info', '--format', option], text=True, capture_output=True, timeout=60)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    if result.returncode:
        return None  # a stopped Podman machine writes its version banner and exits nonzero
    state = result.stdout.strip().lower()
    if name == 'podman':
        return True if state == 'true' else False if state == 'false' else None
    if name == 'docker':
        return 'rootless' in state
    return None


def worktree_mounts() -> list[str]:
    """the mounts a worktree needs: its .git file and the common .git it points to are outside the checkout"""
    common = output(['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'])
    if not common:
        return []
    inside = common == str(ROOT) or common.startswith(str(ROOT) + os.sep)
    if inside:
        return []
    # at their real paths, because the gitfile and gitdir files hold absolute paths; read-only, since git only reads
    return ['-v', f'{ROOT}:{ROOT}:ro', '-v', f'{common}:{common}:ro']


def linux(args: argparse.Namespace, failures: Failures) -> None:
    engine = container_engine(args.engine)
    if not engine:
        if args.dry_run:
            engine = args.engine or 'podman'  # the command a user without an engine would run
        else:
            failures.skip('linux', 'no Podman or Docker: install one, or run the commands on a Linux host')
            return
    if not args.dry_run and not quiet_ok([engine, 'info']):
        failures.skip('linux', f'{os.path.basename(engine)} is installed but not running: '
                               'start its machine or daemon (podman machine start, or Docker Desktop)')
        return
    arch = args.arch or ('aarch64' if IS_ARM else 'x86_64')
    if arch not in ('x86_64', 'aarch64'):
        raise SystemExit('--arch is x86_64 or aarch64')
    platform_flag = ['--platform', 'linux/amd64'] if arch == 'x86_64' else ['--platform', 'linux/arm64']
    # the jobs' own images: Arch Linux's own, and pkgforge's Arch Linux ARM for AArch64
    image = args.image or os.environ.get('SNX_CI_IMAGE') or (
        'ghcr.io/pkgforge-dev/archlinux:latest' if arch == 'aarch64' else 'archlinux:latest')
    build = f'build/ci-linux-{arch}'
    pacman = ROOT / f'.cache/ci-pacman-{arch}'
    ccache_dir = ROOT / f'.cache/ci-ccache-{arch}'
    deps = ROOT / f'.cache/ci-deps-{arch}'
    unicorn = f'/src/.cache/ci-unicorn-linux-{arch}'
    for folder in (pacman, ccache_dir, deps):
        os.makedirs(folder, exist_ok=True)
    # pacman 7's seccomp download sandbox fails under Rosetta with "restricting syscalls via seccomp: 22"; the
    # container is rootless, so the emulated x86-64 job turns the sandbox off
    pacman_opts = '--disable-sandbox ' if arch == 'x86_64' and IS_ARM else ''
    cpus = args.cpus or min(4, jobs())
    build_jobs = args.jobs or max(1, min(4, int(cpus)))
    script = f'''set -eux
retry() {{ for try in 1 2 3; do "$@" && return; [ "$try" = 3 ] && return 1; sleep 5; done; }}
retry pacman {pacman_opts}-Syu --noconfirm --needed cmake ninja make patch nasm perl python git clang lld llvm qemu-user pkgconf \\
  sdl3 libxtst libxrandr libxss vulkan-swrast vulkan-validation-layers ccache gdb jq glslang
git config --global --add safe.directory '*'
if ! {unicorn}/bin/python -c 'import unicorn' 2>/dev/null; then
  [ -x {unicorn}/bin/python ] || python -m venv --system-site-packages {unicorn}
  retry {unicorn}/bin/pip install --quiet unicorn==2.1.4
fi
export CCACHE_DIR=/src/.cache/ci-ccache-{arch} CCACHE_BASEDIR=/src CCACHE_NOHASHDIR=true \\
  SNX_DEPS_CACHE=/src/.cache/ci-deps-{arch}
cmake --preset default -B {build} -DSNX_TEST_VP9_VECTORS=ON -DSNX_TEST_ARM_DIFF=ON \\
  -DPython3_EXECUTABLE={unicorn}/bin/python \\
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build {build} --parallel {build_jobs}
ctest --test-dir {build} --output-on-failure --parallel {build_jobs}
clang -fsyntax-only -Wall -I src src/common/cpu/aarch64/cpu_helpers.c
python -m compileall -q tools
python tools/run_game.py --help >/dev/null
if [ "{'0' if args.quick else '1'}" = 1 ]; then
  cmake --preset default -B {build}-headless -DSNX_BUILD_GUI=OFF -DSNX_DEPLOY=OFF -DSNX_TEST_VULKAN=ON \\
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build {build}-headless --parallel {build_jobs}
  headless='^(vulkan_headless|vulkan_headless_options|vulkan_headless_vic_validation|nx_runtime_homebrew|nx_runtime_homebrew_64k_pages)$'
  ctest --test-dir {build}-headless -R "$headless" --output-on-failure
fi
if [ "{'1' if args.sanitizers else '0'}" = 1 ]; then
  cmake --preset default -B {build}-sanitizers -DSNX_TEST_SANITIZERS=ON -DSNX_TEST_VP9_VECTORS=ON -DSNX_DEPLOY=OFF \\
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build {build}-sanitizers --parallel {build_jobs}
  ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \\
    LSAN_OPTIONS=use_stacks=0:use_registers=0 \\
    ctest --test-dir {build}-sanitizers --output-on-failure --parallel {build_jobs}
fi
'''
    rootless = rootless_engine(engine)
    if IS_LINUX and os.path.basename(engine) == 'docker' and rootless is False and not args.allow_rootful:
        failures.skip('linux', 'rootful Docker writes the checkout as root: use rootless Docker or Podman, or pass '
                               '--allow-rootful')
        return
    mount = f'{ROOT}:/src' + (':Z' if IS_LINUX else '')  # SELinux on a Linux host
    cmd = [engine, 'run', '--rm', '--pull=missing', '--security-opt=no-new-privileges', *platform_flag,
           '-v', mount, '-w', '/src', '-v', f'{pacman}:/var/cache/pacman/pkg', *worktree_mounts(), image,
           'sh', '-eu', '-c', script]
    cmd[3:3] = ['--cpus', f'{cpus:g}']
    if args.memory:
        cmd[3:3] = ['--memory', args.memory]
    battery_note()
    state = 'incremental' if (ROOT / build).is_dir() else 'first run'
    print(f'\n== the Linux ({arch}) job in {os.path.basename(engine)}'
          f'{" (rootless)" if rootless else ""}: {build_jobs} jobs, {cpus:g} CPUs, {state} ==')
    print(f'   the build and caches stay in {build}/, .cache/ci-ccache-{arch}/ and .cache/ci-deps-{arch}/; '
          f'only this container arch reuses them')
    if args.dry_run:
        print(' '.join(shlex.quote(str(c)) for c in cmd))
        return
    if run(cmd, check=False):
        failures.failed_check(f'the Linux ({arch}) job')


# ---- Android ----
def android_home() -> Path | None:
    for name in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
                 str(Path.home() / 'Library/Android/sdk'), str(Path.home() / 'Android/Sdk')):
        if name and Path(name).is_dir():
            return Path(name)
    return None


def android_ndk() -> Path | None:
    named = os.environ.get('ANDROID_NDK_ROOT')
    if named and Path(named).is_dir():
        return Path(named)
    home = android_home()
    if home:
        versions = sorted((home / 'ndk').glob('*'), key=lambda p: [int(n) for n in re.findall(r'\d+', p.name)])
        if versions:
            return versions[-1]
    return None


def android(args: argparse.Namespace, failures: Failures) -> None:
    home, ndk = android_home(), android_ndk()
    if not ndk or not (ndk / 'build/cmake/android.toolchain.cmake').is_file():
        failures.skip('android', 'no NDK: set ANDROID_NDK_ROOT, or install one with sdkmanager "ndk;28.2.13676358"')
        return
    gradle = tool('gradle')
    if not gradle:
        failures.skip('android', 'no Gradle 9: install it, or set GRADLE')
        return
    java_home = os.environ.get('JAVA_HOME')
    if not java_home and IS_MAC and tool('/usr/libexec/java_home'):
        java_home = output(['/usr/libexec/java_home', '-v', '21'])
    abis = args.abi or ['x86_64', 'arm64-v8a']
    prefix = Path(os.environ.get('ANDROID_LLVM', str(Path.home() / 'android-llvm')))
    out = ROOT / 'build' / 'android-ci'
    env = {'ANDROID_NDK_ROOT': str(ndk), 'GRADLE': gradle}
    if home:
        env['ANDROID_HOME'] = str(home)
    if java_home:
        env['JAVA_HOME'] = java_home
    if not args.no_build:
        for abi in abis:
            if not (prefix / abi / 'lib/cmake/llvm').is_dir():
                if args.no_llvm:
                    failures.skip(f'android {abi}',
                                  f'no LLVM at {prefix / abi}: run scripts/android-llvm.sh {abi} {prefix / abi}')
                    continue
                print(f'\n== LLVM for Android {abi}, about 10 minutes the first time ==')
                run(['scripts/android-llvm.sh', abi, str(prefix / abi)])
        built = [abi for abi in abis if (prefix / abi / 'lib/cmake/llvm').is_dir()]
        if not built:
            return
        print('\n== the Android APK, as build.yml makes it ==')
        apk_cmd = ['scripts/android-apk.sh', str(out), str(prefix), *built]
        if ccache_flag():
            apk_cmd += ['--', *ccache_flag()]
        status = run(apk_cmd, env=env, check=False)
        if status:
            failures.failed_check('android APK')
            return
    apks = sorted((out / 'stage/gradle/outputs/apk/debug').glob('*.apk')) if (out / 'stage').is_dir() else []
    if not apks:
        failures.failed_check('android APK', f'no APK under {out}/stage/gradle/outputs/apk/debug')
        return
    apk = apks[-1]
    print(f'APK: {apk}')
    if not args.install:
        return
    adb = tool('adb')
    if not adb:
        failures.skip('android install', 'no adb: install Android platform-tools')
        return
    serial = args.serial or ''
    base = [adb] + (['-s', serial] if serial else [])
    devices = [line.split()[0] for line in output([adb, 'devices']).splitlines()[1:] if line.endswith('\tdevice')]
    if not devices:
        failures.skip('android install', 'no device or emulator: start one, or connect a phone with adb')
        return
    base = [adb, '-s', serial or devices[0]]
    print(f'\n== install and start the menu on {serial or devices[0]} ==')
    if run([*base, 'install', '-r', str(apk)], check=False):
        failures.failed_check('android install')
        return
    # the menu asks for all files on Android 11, and the check below would see that screen instead of the menu
    run([*base, 'shell', 'appops', 'set', 'org.scarletnx', 'MANAGE_EXTERNAL_STORAGE', 'allow'], check=False)
    run([*base, 'shell', 'pm', 'grant', 'org.scarletnx', 'android.permission.WRITE_EXTERNAL_STORAGE'], check=False)
    run([*base, 'logcat', '-b', 'all', '-c'], check=False)
    run([*base, 'shell', 'am', 'start', '-n', 'org.scarletnx/.MainActivity'], check=False)
    if args.wait:
        import time
        time.sleep(args.wait)
    pid = output([*base, 'shell', 'pidof', 'org.scarletnx'])
    if not pid:
        failures.failed_check('android start', 'the app is not running')
        logcat = output([*base, 'logcat', '-d', '-t', '500'])
        print('\n'.join(logcat.splitlines()[-80:]))
        return
    resumed = output([*base, 'shell', 'dumpsys', 'activity', 'activities'])
    if 'org.scarletnx/.MainActivity' not in resumed:
        failures.failed_check('android start', 'the menu is not the resumed activity')
    crash = output([*base, 'logcat', '-d', '-b', 'crash'])
    if 'org.scarletnx' in crash:
        failures.failed_check('android start', 'the crash buffer holds the app')
        print(crash)
    shot = out / 'menu.png'
    try:
        with shot.open('wb') as png:
            subprocess.run([*base, 'exec-out', 'screencap', '-p'], stdout=png, timeout=60)
        print(f'the screen is {shot}')
    except (OSError, subprocess.TimeoutExpired):
        pass
    run([*base, 'shell', 'am', 'force-stop', 'org.scarletnx'], check=False)


# ---- iOS ----
def ios(args: argparse.Namespace, failures: Failures) -> None:
    if not IS_MAC:
        failures.skip('ios', 'iOS builds on a Mac with Xcode only')
        return
    if not tool('xcodebuild'):
        failures.skip('ios', 'no Xcode: install it from the App Store')
        return
    simulator = args.simulator
    kind = 'iossim' if simulator else 'ios'
    preset = 'ios-simulator' if simulator else 'ios'
    prefix = Path(os.environ.get('IOS_LLVM', str(Path.home() / 'apple-llvm' / kind)))
    if not (prefix / 'lib/cmake/llvm').is_dir():
        if args.no_llvm:
            failures.skip(f'{preset}', f'no LLVM at {prefix}: run scripts/apple-llvm.sh {kind} {prefix}')
            return
        print(f'\n== LLVM for {kind}, about 10 minutes the first time ==')
        run(['scripts/apple-llvm.sh', kind, str(prefix)])
    build = ROOT / 'build' / preset
    print(f'\n== the {preset} build ==')
    cmd = ['cmake', '--preset', preset, '-DLLVM_DIR=' + str(prefix / 'lib/cmake/llvm')] + ccache_flag()
    if os.environ.get('SNX_MOLTENVK'):
        cmd.append('-DSNX_MOLTENVK=' + os.environ['SNX_MOLTENVK'])
    if run(cmd, check=False) or run(['cmake', '--build', str(build), '--parallel', str(parallel_jobs(args))],
                                    check=False):
        failures.failed_check(preset)
        return
    if not simulator:
        return
    runtimes = output(['xcrun', 'simctl', 'list', 'runtimes', 'available'])
    if not re.search(r'^iOS ', runtimes, re.M):
        failures.skip('iOS Simulator', 'no iOS runtime: xcodebuild -downloadPlatform iOS')
        return
    devices = output(['xcrun', 'simctl', 'list', 'devices', 'available'])
    udids = re.findall(r'\(([0-9A-F-]{36})\)', devices)
    if not udids:
        failures.skip('iOS Simulator', 'no iPhone simulator: add one in Xcode')
        return
    device = args.device or udids[0]
    run(['xcrun', 'simctl', 'boot', device], check=False)
    run(['xcrun', 'simctl', 'bootstatus', device, '-b'], check=False)
    apps = sorted(build.glob('**/ScarletNX.app'))
    if not apps:
        failures.failed_check('iOS Simulator', f'no ScarletNX.app under {build}')
        return
    app = apps[0]
    # a device build names iPhoneOS; the simulator is another platform (the app is not signed for the store)
    plist = app / 'Info.plist'
    if plist.is_file():
        run(['/usr/libexec/PlistBuddy', '-c', 'Set :CFBundleSupportedPlatforms:0 iPhoneSimulator', str(plist)],
            check=False)
    if run(['xcrun', 'simctl', 'install', device, str(app)], check=False):
        failures.failed_check('iOS Simulator install')
        return
    run(['xcrun', 'simctl', 'terminate', device, 'org.scarletnx'], check=False)
    print('+ xcrun simctl launch --console ' + device + ' org.scarletnx')
    try:
        ended = subprocess.run(['xcrun', 'simctl', 'launch', '--console', device, 'org.scarletnx'],
                               capture_output=True, text=True, timeout=max(1, args.wait))
        if ended.returncode:
            failures.failed_check('iOS Simulator start', f'exit {ended.returncode}')
            print('\n'.join(ended.stdout.splitlines()[-40:]))
        else:
            failures.failed_check('iOS Simulator start', 'the menu ended before the wait')
    except subprocess.TimeoutExpired:
        # still running after the wait: that is the check
        pass
    shot = ROOT / 'build' / 'ios-simulator-menu.png'
    run(['xcrun', 'simctl', 'io', device, 'screenshot', str(shot)], check=False)
    print(f'the screen is {shot}')
    run(['xcrun', 'simctl', 'terminate', device, 'org.scarletnx'], check=False)


# ---- Windows ----
def windows(args: argparse.Namespace, failures: Failures) -> None:
    cross = tool('x86_64-w64-mingw32-clang')
    if not cross:
        failures.skip('windows',
                      'no llvm-mingw: put its bin/ on PATH (x86_64-w64-mingw32-clang), or use a Windows host')
        return
    # llvm-mingw's own: the Mac's LLVM describes macOS libraries, not Windows ones
    config = os.environ.get('SNX_LLVM_CONFIG') or tool('x86_64-w64-mingw32-llvm-config')
    if not config:
        candidate = tool('llvm-config')
        if candidate and 'mingw' in output([candidate, '--host-target']):
            config = candidate
    if not config:
        failures.skip('windows', 'no llvm-config for the Windows LLVM: use the one in the llvm-mingw bin/')
        return
    build = ROOT / 'build' / 'windows-cross'
    print(f'\n== the Windows cross build in {build} ==')
    cmd = ['cmake', '--preset', 'windows-cross', f'-DSNX_LLVM_CONFIG={config}'] + ccache_flag()
    if args.vp9:
        cmd.append('-DSNX_TEST_VP9_VECTORS=ON')
    wine = tool('wine64', 'wine')
    if wine:
        cmd.append(f'-DCMAKE_CROSSCOMPILING_EMULATOR={wine}')
    if run(cmd, check=False) or run(['cmake', '--build', str(build), '--parallel', str(parallel_jobs(args))],
                                    check=False):
        failures.failed_check('windows build')
        return
    if not args.test:
        return
    if not wine:
        failures.skip('windows tests', 'no Wine: install it to run the Windows tests')
        return
    ctest(build, failures, what='ctest (Windows under Wine)')


# ---- preflight ----
def preflight(args: argparse.Namespace, failures: Failures) -> None:
    """the checks before a pull request: the merge, the host, and the Linux jobs when they are asked for"""
    check_merge(args, failures)
    if args.merge_only:
        return
    if failures.failed:
        print('preflight: resolve the merge conflict before the build and tests')
        return
    paths = changed_files(args.base)
    if paths is None:
        print('warning: git could not name the changed files; the build and tests run', file=sys.stderr)
        code = True
    else:
        code = not static_only(paths)
        if not paths:
            print('preflight: no changed files')
        elif code:
            print(f'preflight: {len(paths)} changed file(s), code or build files')
        else:
            print(f'preflight: {len(paths)} changed file(s), docs, agent files and scripts only')
    if args.static_only and (args.containers or args.both or args.sanitizers):
        print('note: --static-only skips the container checks')
    containers = (args.containers or args.both or args.sanitizers) and not args.static_only
    build_needed = (code or args.force_build or args.full) and not args.static_only
    if args.dry_run:
        print('dry run:')
        if build_needed:
            print(f'  would run the host build and ctest with {parallel_jobs(args)} jobs')
        elif args.static_only:
            print('  no host build: static-only')
        else:
            print('  no host build: the change is docs, agent files and scripts')
        if containers:
            host = 'aarch64' if IS_ARM else 'x86_64'
            arches = [host] + (['x86_64' if host == 'aarch64' else 'aarch64'] if args.both else [])
            for arch in arches:
                linux_args = argparse.Namespace(arch=arch, engine=args.engine, image=args.image, quick=not args.full,
                                                sanitizers=args.sanitizers and arch == host, cpus=args.cpus,
                                                memory=args.memory, allow_rootful=args.allow_rootful, dry_run=True,
                                                jobs=args.jobs)
                linux(linux_args, failures)
        else:
            print('  no container checks: add --containers, or --both --sanitizers')
        return
    if not args.static_only:
        battery_note()
    static_checks(failures)
    if not build_needed:
        if args.static_only:
            print('static-only: the host build and tests are skipped')
        else:
            print('the host build and tests are not needed for docs, agent files and scripts')
    else:
        print('\n== the host build and tests ==')
        native_args = argparse.Namespace(build_dir=None, no_build=False, vp9=True, arm_diff=False, sanitizers=False,
                                         vulkan=args.full, screen=False, system_sdl3=True, jobs=args.jobs)
        native(native_args, failures, static=False)
    if args.no_container or not containers:
        if args.no_container:
            print('note: --no-container: the Linux container checks did not run')
        else:
            print('note: the Linux container checks did not run; add --containers for this architecture, or '
                  '--both --sanitizers for the full gate')
        return
    engine = container_engine(args.engine)
    if not engine:
        failures.skip('the Linux containers', 'no Podman or Docker: the host checks are the fallback; '
                                               'the container checks are optional')
        return
    if not quiet_ok([engine, 'info']):
        failures.skip('the Linux containers', f'{os.path.basename(engine)} is installed but its machine is not '
                                               'running: start it, or use --no-container')
        return
    host = 'aarch64' if IS_ARM else 'x86_64'
    arches = [host]
    if args.both:
        arches.append('x86_64' if host == 'aarch64' else 'aarch64')
    if args.both and IS_MAC and IS_ARM:
        print('note: the x86-64 job runs through Rosetta and is much slower; its build and ccache are reused')
    if args.sanitizers:
        print('note: the ASan and UBSan build is a second full build; it reuses ccache')
    for arch in arches:
        linux_args = argparse.Namespace(arch=arch, engine=args.engine, image=args.image, quick=not args.full,
                                        sanitizers=args.sanitizers and arch == host, cpus=args.cpus,
                                        memory=args.memory, allow_rootful=args.allow_rootful, dry_run=False,
                                        jobs=args.jobs)
        linux(linux_args, failures)


# ---- doctor ----
def doctor(_: argparse.Namespace) -> None:
    print(f'{platform.system()} {platform.release()} on {MACHINE}, {jobs()} CPUs')
    rosetta = IS_MAC and IS_ARM and quiet_ok(['arch', '-x86_64', '/usr/bin/true'])
    power = power_state()
    engine = container_engine()
    rootless = rootless_engine(engine) if engine else None
    engine_name = (engine or '') + (' (rootless)' if rootless else ' (rootful)' if rootless is False else '')
    rows = [
        ('cmake', tool('cmake'), 'every build'),
        ('ninja', tool('ninja'), 'every build'),
        ('clang', tool('clang'), 'every build'),
        ('python 3.10+', PYTHON, 'the scripted tests and the tools'),
        ('llvm-config', llvm_config(), 'the recompiler; install LLVM or set SNX_LLVM_CONFIG'),
        ('ccache', tool('ccache'), 'optional for the host build; the container has its own'),
        ('nasm', tool('nasm'), 'FFmpeg assembly on x86-64'),
        ('spirv-val', tool('spirv-val'), 'shader translation checks'),
        ('glslangValidator', tool('glslangValidator'), 'the compute shaders'),
        ('gdb', tool('gdb'), 'gdbstub_native'),
        ('jq', tool('jq'), 'pr_builds'),
        ('qemu-aarch64', tool('qemu-aarch64'), 'nce_host under qemu'),
        ('perl', tool('perl'), 'FFmpeg and OpenSSL configure'),
        ('make', tool('make'), 'FFmpeg and OpenSSL'),
        ('unicorn 2.1.4', 'yes' if unicorn_ready() else None, 'the A32 and Thumb differentials (--arm-diff)'),
        ('vulkaninfo', tool('vulkaninfo'), 'SNX_TEST_VULKAN needs a Vulkan 1.3 ICD'),
        ('podman or docker', engine_name or None, 'the Linux and ASan jobs'),
        ('x86_64-w64-mingw32-clang', tool('x86_64-w64-mingw32-clang'), 'the Windows cross build'),
        ('wine', tool('wine64', 'wine'), 'the Windows tests'),
        ('adb', tool('adb'), 'the Android device'),
        ('gradle', tool('gradle'), 'the Android APK'),
        ('xcodebuild', tool('xcodebuild'), 'the macOS and iOS builds'),
    ]
    if IS_MAC and IS_ARM:
        rows.append(('Rosetta (x86-64)', 'yes' if rosetta else None, 'x86-64 containers on Apple silicon'))
    if power:
        rows.append(('power', power, 'battery makes the full checks expensive'))
    width = max(len(row[0]) for row in rows)
    for name, found, why in rows:
        state = found or 'MISSING'
        print(f'  {name:<{width}}  {state}')
        if not found:
            print(f'  {"":<{width}}  -> {why}')
    if IS_MAC:
        runtimes = output(['xcrun', 'simctl', 'list', 'runtimes', 'available'])
        devices = output(['xcrun', 'devicectl', 'list', 'devices'])
        print(f'  {"iOS runtimes":<{width}}  ' +
              ('yes' if re.search(r'^iOS ', runtimes, re.M) else 'MISSING (xcodebuild -downloadPlatform iOS)'))
        connected = 'yes' if 'No devices' not in devices and devices else 'none connected'
        print(f'  {"iOS devices":<{width}}  ' + connected)
    home, ndk = android_home(), android_ndk()
    print(f'  {"ANDROID_HOME":<{width}}  {home or "MISSING"}')
    print(f'  {"ANDROID_NDK_ROOT":<{width}}  {ndk or "MISSING"}')
    if not engine:
        print('\nNo container engine. Podman is preferred: its machine is rootless, and it starts no root daemon.')
        if IS_MAC:
            print('  brew install podman')
            if IS_ARM:
                print('  softwareupdate --install-rosetta --agree-to-license')
                print('  mkdir -p ~/.config/containers')
                print('  printf \'[machine]\\nrosetta = true\\n\' >> ~/.config/containers/containers.conf')
            print('  podman machine init --cpus 4 --memory 8192 --disk-size 100')
            print('  podman machine start')
            print('  # x86-64 containers then run through Rosetta; libkrun with FEX is the alternative')
        elif IS_LINUX:
            print('  install Podman with the distribution\'s package manager: it runs as your user, with no root daemon')
        else:
            print('  install Podman, or Docker Desktop')
    elif os.path.basename(engine) == 'podman' and rootless is None:
        print('\nPodman cannot report its machine: run `podman machine start` (or `podman machine init` the first time).')
    if IS_MAC and IS_ARM and engine and os.path.basename(engine) == 'podman':
        try:
            machines = json.loads(output([engine, 'machine', 'inspect']) or '[]')
        except json.JSONDecodeError:
            machines = []
        if machines and not any(machine.get('Rosetta') for machine in machines):
            print('\nThis Podman machine has no Rosetta: x86-64 containers run under qemu, or not at all. Recreate it '
                  'with `[machine]`, `rosetta = true` in ~/.config/containers/containers.conf.')
    if power == 'battery':
        print('\nThis machine runs on battery. The full build and container checks drain it; connect power, or use '
              '--static-only and lower --jobs and --cpus.')
    print('\nThe commands: preflight, static, native, linux, android, ios, windows. A check this machine cannot run is '
          'said, not failed; --strict makes it a failure.')


# ---- main ----
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--strict', action='store_true',
                        help='fail on a check this machine cannot run, not only on one that fails')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('doctor', help='what this machine can run').set_defaults(func=doctor)
    p = sub.add_parser('static', help='tracked material, docs and the Python under tools/')
    p.set_defaults(func=lambda a, f: static_checks(f))
    p = sub.add_parser('native', help='the host build and its tests, as the host job runs them')
    p.add_argument('--build-dir')
    p.add_argument('--no-build', action='store_true', help='test an existing build')
    p.add_argument('--no-vp9', dest='vp9', action='store_false')
    p.add_argument('--arm-diff', action='store_true', help='A32/Thumb against unicorn 2.1.4')
    p.add_argument('--sanitizers', action='store_true', help='ASan and UBSan, as the linux-sanitizers job')
    p.add_argument('--vulkan', action='store_true', help='a headless Vulkan build, as the Linux job')
    p.add_argument('--screen', action='store_true', help='the macOS branch of the screen tests, which CI does not run')
    p.add_argument('--no-system-sdl3', dest='system_sdl3', action='store_false',
                   help='skip the platform tests against Homebrew\'s shared SDL3')
    p.add_argument('--jobs', type=int, help='build and test parallelism (default 4)')
    p.set_defaults(func=native, system_sdl3=True)
    p = sub.add_parser('preflight', help='the quick checks before a pull request: merge, host, optional Linux jobs')
    p.add_argument('--base', default='origin/main', help='the branch to check the merge against')
    p.add_argument('--no-fetch', dest='fetch', action='store_false', help='use the local base ref, without fetching')
    p.add_argument('--no-merge', dest='merge', action='store_false', help='skip the merge check')
    p.add_argument('--merge-only', action='store_true', help='the merge check alone')
    p.add_argument('--no-container', action='store_true', help='the host checks alone')
    p.add_argument('--containers', action='store_true', help='the Linux job of this architecture too')
    p.add_argument('--both', action='store_true', help='the other architecture too, when the engine can run it')
    p.add_argument('--sanitizers', action='store_true', help='the ASan and UBSan build on the host architecture')
    p.add_argument('--force-build', action='store_true', help='the build and tests even for docs-only changes')
    p.add_argument('--static-only', action='store_true', help='the merge and static checks alone')
    p.add_argument('--dry-run', action='store_true', help='print the plan and the container command, and run nothing')
    p.add_argument('--full', action='store_true', help='the headless Vulkan build too')
    p.add_argument('--engine', choices=['podman', 'docker'])
    p.add_argument('--image', help='the container image (SNX_CI_IMAGE overrides)')
    p.add_argument('--cpus', type=float, help='the container\'s CPU limit (default 4)')
    p.add_argument('--memory', help='the container\'s memory limit, such as 8g')
    p.add_argument('--jobs', type=int, help='build and test parallelism (default 4)')
    p.add_argument('--allow-rootful', action='store_true', help='use a rootful Docker on Linux')
    p.set_defaults(func=preflight, fetch=True, merge=True)
    p = sub.add_parser('linux', help='the Linux job in Podman or Docker')
    p.add_argument('--arch', choices=['x86_64', 'aarch64'])
    p.add_argument('--engine', choices=['podman', 'docker'], help='the engine to use (default: Podman, else Docker)')
    p.add_argument('--image', help='the image (default: the job\'s; SNX_CI_IMAGE overrides)')
    p.add_argument('--quick', action='store_true', help='the default build only')
    p.add_argument('--sanitizers', action='store_true', help='the ASan and UBSan build too')
    p.add_argument('--cpus', type=float, help='the container\'s CPU limit (default 4)')
    p.add_argument('--memory', help='the container\'s memory limit, such as 8g')
    p.add_argument('--jobs', type=int, help='build and test parallelism (default 4)')
    p.add_argument('--allow-rootful', action='store_true',
                   help='use a rootful Docker on Linux, which writes the checkout as root')
    p.add_argument('--dry-run', action='store_true', help='print the container command, and run nothing')
    p.set_defaults(func=linux)
    p = sub.add_parser('android', help='the Android APK, and its start on a device')
    p.add_argument('--abi', action='append', choices=['x86_64', 'arm64-v8a'])
    p.add_argument('--install', action='store_true', help='install and start it on the connected device')
    p.add_argument('--serial')
    p.add_argument('--no-build', action='store_true')
    p.add_argument('--no-llvm', action='store_true', help='do not build LLVM for an ABI that lacks it')
    p.add_argument('--wait', type=float, default=8, help='seconds before the app is looked at')
    p.set_defaults(func=android)
    p = sub.add_parser('ios', help='the iOS build, and the Simulator')
    p.add_argument('--simulator', action='store_true')
    p.add_argument('--device')
    p.add_argument('--no-llvm', action='store_true')
    p.add_argument('--wait', type=float, default=10)
    p.set_defaults(func=ios)
    p = sub.add_parser('windows', help='the Windows cross build, and its tests under Wine')
    p.add_argument('--test', action='store_true')
    p.add_argument('--no-vp9', dest='vp9', action='store_false')
    p.set_defaults(func=windows)
    args = parser.parse_args(argv)
    failures = Failures()
    if args.command == 'doctor':
        args.func(args)
        return 0
    args.func(args, failures)
    print()
    if failures.failed:
        print(f'{len(failures.failed)} check(s) failed:', file=sys.stderr)
        for what in failures.failed:
            print(f'  {what}', file=sys.stderr)
    if failures.skipped:
        print(f'{len(failures.skipped)} check(s) did not run here:')
        for what in failures.skipped:
            print(f'  {what}')
    if not failures.failed:
        print('the checks that ran hold')
    return 1 if failures.failed or (args.strict and failures.skipped) else 0


if __name__ == '__main__':
    sys.exit(main())
