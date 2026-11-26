#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The scripts of build.yml's steps (scripts/ci/*.sh) held to the steps that run them.

A job's file holds its steps as functions, `scripts/ci/FILE.sh STEP` runs one; a file without functions is run whole.
Each step's call names a file there and one of its functions; each function and each file is run by some step; the
"# env:" line above a function names the variables its step's env gives it, the same for every step that calls it;
each file reads in bash (bash -n), and refuses a step it has not, naming it. Then the checks on copies made wrong.

    tests/ci_scripts_test.py [REPO]   (default: the checkout this file is in)"""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_caches_test import jobs  # noqa: E402

WORKFLOW = Path('.github/workflows/build.yml')
CALL = re.compile(r'(?:bash )?scripts/ci/([\w-]+)\.sh(?: (\w+))?')
FUNCTION = re.compile(r'^(\w+)\(\) \{$', re.M)


def env_keys(step):
    return sorted(re.findall(r'^([A-Za-z_]\w*):', step.get('env', ''), re.M))


def check(text, scripts):
    """what is wrong between a workflow's text and {file name: its text}, one line each"""
    wrong, calls = [], {}
    for job, steps in jobs(text).items():
        for step in steps:
            run = step.get('run', '').strip()
            called = CALL.fullmatch(run)
            if 'scripts/ci/' in run and not called:
                wrong.append(f'{job}: "{run}" calls scripts/ci by another form than `scripts/ci/FILE.sh [STEP]`')
                continue
            if not called:
                continue
            name, func = called[1], called[2]
            if name not in scripts:
                wrong.append(f'{job}: "{run}" runs scripts/ci/{name}.sh, which is not there')
                continue
            functions = FUNCTION.findall(scripts[name])
            if functions and not func:
                wrong.append(f'{job}: "{run}" runs {name}.sh whole, and it holds steps ({", ".join(functions)})')
            elif func and func not in functions:
                wrong.append(f'{job}: "{run}" runs the step {func}, which {name}.sh has not')
            calls.setdefault((name, func), []).append((job, env_keys(step)))
    for name, body in scripts.items():
        functions = FUNCTION.findall(body) or [None]
        for func in functions:
            if (name, func) not in calls:
                wrong.append(f'{name}.sh {func or "(whole)"}: no step of the workflow runs it')
                continue
            given = {tuple(keys) for _, keys in calls[(name, func)]}
            if len(given) > 1:
                wrong.append(f'{name}.sh {func}: its steps give it different env: {sorted(given)}')
            if func is None:
                continue
            above = body[: body.index(f'\n{func}() {{')].rsplit('\n', 1)[-1]
            said = above[len('# env: ') :].split() if above.startswith('# env: ') else []
            keys = sorted(next(iter(given)))
            if said != keys:
                wrong.append(f'{name}.sh {func}: "# env:" says {said or "nothing"}, its step gives {keys or "nothing"}')
    return wrong


def check_files(folder):
    """each file's head, its bash syntax, and the refusal of a step it has not"""
    wrong = []
    bash = shutil.which('bash')
    for path in sorted(folder.glob('*.sh')):
        head = path.read_text(encoding='utf-8').splitlines()[:3]
        if head != [
            '#!/usr/bin/env bash',
            '# SPDX-License-Identifier: GPL-3.0-or-later',
            '# Copyright 2026 the ScarletNX contributors',
        ]:
            wrong.append(f'{path.name}: starts {head}, not the shebang, SPDX and copyright lines')
        if not bash:
            continue
        syntax = subprocess.run([bash, '-n', str(path)], capture_output=True, text=True)
        if syntax.returncode:
            wrong.append(f'{path.name}: bash -n: {syntax.stderr.strip()}')
            continue
        if not FUNCTION.search(path.read_text(encoding='utf-8')):
            continue
        refused = subprocess.run([bash, str(path), 'no_such_step'], capture_output=True, text=True, timeout=60)
        if refused.returncode != 2 or '(given: no_such_step)' not in refused.stderr:
            wrong.append(
                f'{path.name} no_such_step: exit {refused.returncode} and "{refused.stderr.strip()}", not 2 and a'
                ' line naming the step given'
            )
    return wrong


def self_test(text, scripts):
    """copies made wrong in one way each: the check names each"""
    linux = scripts['linux']
    cases = [
        (
            'a call of a step the file has not',
            text.replace('linux.sh release\n', 'linux.sh relase\n', 1),
            scripts,
            'runs the step relase, which linux.sh has not',
        ),
        (
            'a function no step runs',
            text,
            {**scripts, 'linux': linux + '\nunused() {\n  true\n}\n'},
            'linux.sh unused: no step of the workflow runs it',
        ),
        ('a file no step runs', text, {**scripts, 'spare': 'true\n'}, 'spare.sh (whole): no step of the workflow'),
        (
            'a file not there',
            text,
            {k: v for k, v in scripts.items() if k != 'apple'},
            'runs scripts/ci/apple.sh, which is not there',
        ),
        (
            'an env line that differs from the step',
            text,
            {**scripts, 'linux': linux.replace('# env: ARCH LTO\n', '# env: ARCH\n', 1)},
            "\"# env:\" says ['ARCH'], its step gives ['ARCH', 'LTO']",
        ),
        (
            'a step env the line leaves out',
            text.replace('          LTO: ', '          LTO_NEW: ', 1),
            scripts,
            "its step gives ['ARCH', 'LTO_NEW']",
        ),
        (
            'a file of steps run whole',
            text.replace('scripts/ci/linux.sh release\n', 'scripts/ci/linux.sh\n', 1),
            scripts,
            'runs linux.sh whole, and it holds steps',
        ),
    ]
    missed = []
    for name, copy_text, copy_scripts, want in cases:
        if copy_text == text and copy_scripts is scripts:
            missed.append(f'{name}: the copy is the workflow as it is (this test no longer reads it)')
            continue
        wrong = check(copy_text, copy_scripts)
        if not any(want in line for line in wrong):
            missed.append(f'{name}: the check says {wrong or "nothing"}, not "{want}"')
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / 'bad.sh'
        bad.write_text(
            '#!/usr/bin/env bash\n# SPDX-License-Identifier: GPL-3.0-or-later\n'
            '# Copyright 2026 the ScarletNX contributors\nstep() {\n  if true; then\n}\n'
        )
        if shutil.which('bash') and not any('bash -n' in line for line in check_files(Path(tmp))):
            missed.append('a file bash cannot read: check_files says nothing')
    return missed, len(cases) + 1


def main():
    if len(sys.argv) > 2:
        sys.exit('usage: tests/ci_scripts_test.py [REPO]')
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    text = (repo / WORKFLOW).read_text(encoding='utf-8')
    folder = repo / 'scripts' / 'ci'
    scripts = {path.stem: path.read_text(encoding='utf-8') for path in sorted(folder.glob('*.sh'))}
    if len(scripts) < 6:
        sys.exit(
            f'ci_scripts: {len(scripts)} files in scripts/ci, and there were 6: this test reads fewer than are there'
        )
    wrong = check(text, scripts) + check_files(folder)
    if wrong:
        sys.exit('ci_scripts:\n  ' + '\n  '.join(wrong))
    missed, cases = self_test(text, scripts)
    if missed:
        sys.exit('ci_scripts: the self-test\n  ' + '\n  '.join(missed))
    steps = sum(len(FUNCTION.findall(body)) or 1 for body in scripts.values())
    print(
        f'ci_scripts: {len(scripts)} files, {steps} steps, each run by build.yml with the env it names; {cases} wrong'
        ' copies named'
    )


if __name__ == '__main__':
    main()
