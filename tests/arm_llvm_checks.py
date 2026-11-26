#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The checks of LLVM's AArch32 blocks that stay beside the native back ends' until LLVM leaves, each still registered
and each still refusing what it is there to refuse:

- LLVM's -O0 and -O2 blocks against Unicorn: the arm_llvm_* tests, each on snx-test-arm-blocks-llvm at both levels
  (the fuzzer at -O2), as this build's options have them;
- LLVM's verifier before the passes and after them: the runner and arm_checked_llvm.c refuse a module spoiled there;
- a hosted block's noalias cpu: arm_checked_llvm.c refuses blocks without it (with SNX_TEST_ARM_AOT);
- llvm-mc's verdict: arm_llvm_mc_words compares the catalogues' words with its, and the fuzzer runs only the words
  llvm-objdump has for the A57 and counts no difference llvm-mc warns of; where llvm-mc is not there those two say so,
  and so does this.

    python3 tests/arm_llvm_checks.py --ctest ctest --tree build --blocks build/tests/snx-test-arm-blocks-llvm \
        --diff ON --aot ON --checked build/tests/snx-test-arm-checked-llvm --mc llvm-mc --objdump llvm-objdump

No game assets.
"""

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

import arm_fuzz
import arm_mc_words
from arm_blocks import SIZE

BOTH = ['--opt', '0', '--opt', '2']


def expected(diff, aot):
    """the tests that must be there, each with what its command must hold"""
    tests = {
        'arm_llvm_witnesses': ['arm_test.py', *BOTH],
        'arm_llvm_7tdmi_witnesses': ['arm7tdmi_test.py', *BOTH],
        'arm_llvm_mc_words': ['arm_mc_words.py', '--mc'],
    }
    if diff:
        tests['arm_llvm_differential'] = ['arm_test.py', '--unicorn', *BOTH]
        tests['arm_llvm_vector'] = ['arm_vector_test.py', *BOTH]
        tests['arm_llvm_thumb'] = ['thumb_test.py', *BOTH]
        tests['arm_llvm_fuzz'] = ['arm_fuzz.py', '--opt', '2', '--mc', '--objdump']
    if aot:
        tests['arm_llvm_checked_matrix'] = ['arm_checked_matrix.py', *BOTH]
        tests['arm_checked_static_O0'] = []
        tests['arm_checked_static_O2'] = []
        if diff:
            tests['arm_llvm_arm11_differential'] = ['arm11_test.py', *BOTH]
            tests['arm_llvm_arm11_vfp'] = ['arm11_vfp.py', *BOTH]
    return tests


def holds(command, wanted):
    """whether the command has each of wanted, in its order (a script by the end of its path)"""
    at = 0
    for word in wanted:
        while at < len(command) and not (command[at] == word or command[at].endswith('/' + word)):
            at += 1
        if at == len(command):
            return False
        at += 1
    return True


def registered(args, failures):
    listed = subprocess.run(
        [args.ctest, '--test-dir', args.tree, '--show-only=json-v1'], check=True, capture_output=True, text=True
    ).stdout
    commands = {test['name']: test.get('command', []) for test in json.loads(listed)['tests']}
    for name, wanted in expected(args.diff == 'ON', args.aot == 'ON').items():
        if name not in commands:
            failures.append(f'{name} is not registered')
        elif wanted and not holds(commands[name], wanted):
            failures.append(f'{name} runs {" ".join(commands[name])}, which has not {" ".join(wanted)}')
        elif wanted and 'mc_words' not in name and not holds(commands[name], ['--blocks', args.blocks]):
            failures.append(f'{name} runs {" ".join(commands[name])}, not on {args.blocks}')
    print(f'registered: {len(expected(args.diff == "ON", args.aot == "ON"))} tests looked for', flush=True)


def refused(command, said, failures):
    """the program ends with a failure that says said"""
    run = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=300, check=False)
    if not run.returncode or said not in run.stdout + run.stderr:
        failures.append(f'{" ".join(command)}: status {run.returncode}, not refused with "{said}": {run.stderr[-400:]}')


def verifier(args, root, failures):
    listed, state = root / 'blocks.txt', root / 'state'
    listed.write_text('arm 1000 e0810002\nthumb 2000 1888\n')
    state.write_bytes(bytes(4096 + SIZE))
    intact = subprocess.run(
        [args.blocks, str(listed), str(state)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    if 'ready LLVM' not in intact.stdout:
        failures.append(f'the LLVM runner made no blocks: status {intact.returncode}, {intact.stderr[-400:]}')
    for opt in '02':
        command = [args.blocks, str(listed), str(state), '--opt', opt, '--break']
        refused(command + ['before'], "LLVM's verifier, before the passes", failures)
        refused(command + ['after'], f"LLVM's verifier, after the -O{opt} passes", failures)
    if args.checked:
        out = [str(root / 'checked.o'), str(root / 'counts.inc')]
        if subprocess.run([args.checked, *out, '2'], capture_output=True, timeout=300, check=False).returncode:
            failures.append(f'{args.checked} made no object')
        refused([args.checked, *out, '2', '--break', 'before'], "LLVM's verifier, before the passes", failures)
        refused([args.checked, *out, '0', '--break', 'after'], "LLVM's verifier, after the -O0 passes", failures)
        refused([args.checked, *out, '2', '--break', 'noalias'], "a hosted block's cpu is not noalias", failures)
    print(f'verifier{" and noalias" if args.checked else ""}: each refuses what it is there for', flush=True)


def llvm_mc(args, failures):
    tools = (args.mc, args.objdump)
    if not all(os.access(tool, os.X_OK) for tool in tools):
        print(f'llvm-mc: not bitten here, {args.mc} or {args.objdump} is not there', flush=True)
        return
    vector, thumb, vfp2 = [(0xF2000D00, 'vadd.f32 d0, d0, d0')], [('adds r0, r1, r2', 1, (0x1889,))], []
    differ = arm_mc_words.compare(args.mc, vector, thumb, vfp2)
    if len(differ) != 1 or 'adds r0, r1, r2' not in differ[0]:
        failures.append(f'arm_mc_words.compare does not find the one wrong halfword: {differ}')
    if arm_fuzz.a57_words(tools, 'arm', [[0xE0810002], [0xFFFFFFFF]]) != [[0xE0810002]]:
        failures.append('llvm-objdump has ADD r0, r1, r2 for the A57 and not ffffffff: not so in arm_fuzz.a57_words')
    if not arm_fuzz.uncounted(tools, 'arm', [0xE4900004]) or arm_fuzz.uncounted(None, 'arm', [0xE4900004]):
        failures.append("arm_fuzz.uncounted leaves LDR r0, [r0], #4 uncounted only for llvm-mc's warning: not so")
    if args.diff == 'ON':
        command = [sys.executable, str(pathlib.Path(__file__).with_name('arm_fuzz.py')), '--blocks', args.blocks]
        command += ['--opt', '0', '--mc', args.mc, '--objdump', args.objdump, '--class', 'a32-neon', '--count', '40']
        fuzz = subprocess.run(command + ['--states', '1'], capture_output=True, text=True, timeout=600, check=False)
        left = re.search(r'\((\d+) of (\d+) drawn left out: llvm-objdump has no A57 instruction\)', fuzz.stdout)
        if fuzz.returncode or not left or not int(left[1]):
            failures.append(
                f'the fuzzer had not llvm-objdump leave words out: {fuzz.stdout[-400:]}{fuzz.stderr[-400:]}'
            )
    print("llvm-mc: the words' comparison and the fuzzer's verdict each refuse what they are there for", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ctest', required=True)
    parser.add_argument('--tree', required=True, help='the build tree whose tests are listed')
    parser.add_argument('--blocks', required=True, help='snx-test-arm-blocks-llvm')
    parser.add_argument('--diff', choices=('ON', 'OFF'), required=True, help='SNX_TEST_ARM_DIFF')
    parser.add_argument('--aot', choices=('ON', 'OFF'), required=True, help='SNX_TEST_ARM_AOT')
    parser.add_argument('--checked', help='snx-test-arm-checked-llvm, with SNX_TEST_ARM_AOT')
    parser.add_argument('--mc', required=True)
    parser.add_argument('--objdump', required=True)
    args = parser.parse_args()
    if (args.aot == 'ON') != bool(args.checked):
        parser.error(f'--aot {args.aot} and --checked {args.checked}: the program is there with SNX_TEST_ARM_AOT alone')
    failures = []
    registered(args, failures)
    with tempfile.TemporaryDirectory(prefix='snx-arm-llvm-checks-') as path:
        verifier(args, pathlib.Path(path), failures)
    llvm_mc(args, failures)
    for line in failures:
        print(f'FAIL: {line}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
