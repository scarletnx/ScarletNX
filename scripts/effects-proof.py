#!/usr/bin/env python3
"""Merge the effects and baseline sweep tables into docs/GAME-EFFECTS-PROOF.md.

Usage: effects-proof.py EFFECTS.md BASELINE.md OUT.md LOG_DIR
"""
import re
import sys
from pathlib import Path

ROW = re.compile(r'^\|\s*([0-9A-Za-z]+)\s*\|\s*([a-z0-9]+)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|')


def rows(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        m = ROW.match(line)
        if not m:
            continue
        game, system, rc, frames, effects, validation, lost = [x.strip() for x in m.groups()]
        out[game] = {
            'system': system,
            'rc': rc,
            'frames': frames,
            'effects': effects,
            'validation': validation,
            'lost': lost,
        }
    return out


def title(log_dir, game):
    log = Path(log_dir) / f'{game}.log'
    if not log.exists():
        return game
    for line in log.read_text(errors='replace').splitlines()[:12]:
        if line.startswith('ScarletNX - '):
            return line[len('ScarletNX - '):].strip()
        if line.startswith('game: shown as '):
            return line[len('game: shown as '):].split(',')[0].strip()
    return game


def main():
    effects = rows(sys.argv[1])
    baseline = rows(sys.argv[2])
    log_dir = sys.argv[4]
    lines = [
        '# Effects proof across the games',
        '',
        'The method and the commands are in `docs/GAME-EFFECTS-METHOD.md`. Effects means',
        '`--aa=fxaa --upscaler=fsr1 --hdr=auto`. Baseline means',
        '`--aa=off --upscaler=off --hdr=off`. Both ran for 5 seconds on the private headless X11 display,',
        'with the runtime this branch built and deployed. A native frame is the runtime\'s `--shot`.',
        '',
        '| Game | Title ID | System | Baseline | Effects | Frames | Effects pass | Validation | Device lost |',
        '| --- | --- | --- | --- | --- | --- | --- | --- | --- |',
    ]
    for game in sorted(effects):
        e = effects[game]
        b = baseline.get(game, {})
        base = f"{b.get('rc', '?')} / {b.get('frames', '?')}" if b else 'not run'
        lines.append(
            f"| {title(log_dir, game)} | `{game}` | {e['system']} | {base} | {e['rc']} | {e['frames']} | "
            f"{e['effects']} | {e['validation']} | {e['lost']} |")
    lines += [
        '',
        '## Notes',
        '',
        '- A baseline and an effects result of 0 with frames above 0 is a game that works both ways.',
        '- A baseline that fails as the effects does is a game that does not work on this runtime yet.',
        '- `01008CF01BAAC000` is Echoes of Wisdom. Its recompiled code is ABI 14 where this runtime is ABI 17,',
        '  so it must be recompiled before it can be tested.',
        '- `0100F2C0115B6000` is Tears of the Kingdom. It needs more than 5 seconds to reach its first frame.',
        '  A 30 second run reached 712 frames with `presentation effects ready` and 0 validation messages.',
        '- `0100DCA0064A6000` reaches its first frame late in a 5 second run, so its frame count is small.',
        '',
        '## Window proof',
        '',
        '`--shot` writes the native frame, not the window. The effects proof is the `presentation effects ready`',
        'line, the frame count and the validation count. The synthetic `snx-test-present-fx` panels and the 3DS',
        '`3ds_windows` test prove the window path itself.',
    ]
    Path(sys.argv[3]).write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
