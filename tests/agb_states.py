#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A GBA run's unattended save states, the Switch's state_save and state_load (at:slot, at:path, a comma between):
each saved or loaded between two frames by the run's loop, once, at its ms of the run's clock, and said; a slot that
holds none and an item that is no at:slot said with what was given.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from agb_keys import rom  # noqa: E402  a ROM that reads KEYINPUT forever


def run(runner, work, *options):
    """A run of 20 frames (about 335 ms) of the ROM with options: its output, failing the test on an exit not 0."""
    done = subprocess.run(
        [runner, '--frames=20', '--outdir', str(work), str(work / 'keys.gba'), *options],
        capture_output=True,
        text=True,
        timeout=600,
    )
    if done.returncode:
        sys.exit('agb_states: %s exited with %d\n%s%s' % (' '.join(options), done.returncode, done.stdout, done.stderr))
    return done.stdout + done.stderr


def names(said, verb, expected):
    """Whether said has a line "verb PATH" whose PATH is the file expected, however the runner spells it: its own
    separators (a Windows path with a / before states), the folder's real path (macOS's /private/var for /var)."""
    for path in re.findall(r'(?:^|: )%s (.+)$' % verb, said, re.MULTILINE):
        try:
            if os.path.samefile(path.strip(), expected):
                return True
        except OSError:
            continue
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-agb-states-', ignore_cleanup_errors=True) as temporary:
        work = pathlib.Path(temporary) / 'real'
        work.mkdir()
        try:  # the work folder by a link, as macOS's temporary one is: the runner names it by its real path
            (pathlib.Path(temporary) / 'link').symlink_to(work, target_is_directory=True)
            work = pathlib.Path(temporary) / 'link'
        except OSError:
            pass  # Windows without the right to make links
        (work / 'keys.gba').write_bytes(rom())
        said = run(args.runner, work, '--state-save=100:1,200:auto')
        states = work / 'keys' / 'states'
        made = sorted(p.name for p in states.glob('*.snxstate')) if states.is_dir() else []
        if made != ['keys.1.snxstate', 'keys.auto.snxstate'] or not names(said, 'Saved', states / 'keys.1.snxstate'):
            sys.exit('agb_states: state_save=100:1,200:auto made %s\n%s' % (made, said))
        said = run(args.runner, work, '--state-load=0:1')
        if not names(said, 'Loaded', states / 'keys.1.snxstate'):
            sys.exit('agb_states: state_load=0:1 did not load slot 1\n%s' % said)
        said = run(args.runner, work, '--state-load=0:%s' % (states / 'keys.auto.snxstate'))
        if not names(said, 'Loaded', states / 'keys.auto.snxstate'):
            sys.exit('agb_states: state_load=0:PATH did not load that file\n%s' % said)
        said = run(args.runner, work, '--state-load=0:7', '--state-save=junk')
        if 'Not loaded: ' not in said or 'state_save=junk: junk is not at:slot or at:path' not in said:
            sys.exit('agb_states: an empty slot and an item of no at:slot were not said\n%s' % said)
    print('agb_states: a run saves and loads its states at their times, and says what it could not')


if __name__ == '__main__':
    main()
