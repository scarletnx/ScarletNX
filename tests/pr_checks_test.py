#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""pr-checks.yml's events: a push to a pull request (synchronize) runs Tracked material and docs alone, on
pull_request, and never Title and assignees, whose title, assignees and reviews a push does not change; that job runs on
what changes them (opened, edited, reopened, assigned, unassigned, a review) and on a close, which cancels the pull
request's runs. Offline."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


workflow = (ROOT / '.github' / 'workflows' / 'pr-checks.yml').read_text()
on = workflow.split('\non:\n', 1)[1].split('\n\n', 1)[0]
events = {}
for name, rest in re.findall(r'^  ([a-z_]+):(.*(?:\n    .*)*)', on, re.MULTILINE):
    found = re.search(r'types: \[([^\]]*)\]', rest)
    events[name] = {word.strip() for word in found.group(1).split(',')} if found else None
target = events.get('pull_request_target') or set()
check(
    'a push runs no pull_request_target run: Title and assignees waits for no runner on it',
    'synchronize' not in target,
    sorted(target),
)
for action in ('opened', 'edited', 'reopened', 'assigned', 'unassigned', 'closed'):
    check(f'Title and assignees runs on {action}', action in target, sorted(target))
check(
    'a review runs it', 'submitted' in (events.get('pull_request_review') or set()), events.get('pull_request_review')
)
check(
    'pull_request takes its default actions, a push among them',
    'pull_request' in events and events['pull_request'] is None,
    events.get('pull_request'),
)
tracked = workflow.split('\n  tracked-material-and-docs:\n', 1)[1]
check('Tracked material and docs runs on pull_request', "if: github.event_name == 'pull_request'" in tracked[:400])

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('pr checks: ok')
