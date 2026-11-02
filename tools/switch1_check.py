#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Checks that a change leaves a Switch 1 game's shaders and rendering as a base build has them.

    tools/switch1_check.py gameplay                  # the shaders of scene "gameplay" against the merge base with main
    tools/switch1_check.py gameplay --run            # and a run of the scene, its screenshots and its counters
    tools/switch1_check.py gameplay --base main      # against main itself
    tools/switch1_check.py gameplay --rerecord       # record the scene's shader corpus again
    tools/switch1_check.py gameplay --run --set-reference   # keep this run's counters to compare later runs with

Run it after a change to the shader translator, the renderer or the GPU engines, not after every commit.

A scene is an entry of <data folder>/switch1_check.json, which stays on this machine (it names games):

    {"gameplay": {"game": "<title id>", "seconds": 90, "script": "15000:400,16000:1,...", "note": "needs a save"}}

"game" is the folder in recompiled/ as run_game.py takes it. "script" and "seconds" are as for run_game.py.

The shader check translates the scene's corpus with snx-shaderc of the base and of --build (default build/, with the
working tree's changes) and compares the SPIR-V byte for byte. The corpus holds the guest's shader code, which no
translator changes, so it is recorded once, with the deployed runtime, and reused. The base's snx-shaderc is built in
a worktree with SNX_DEPLOY off, so it never replaces a game's runtime.
When ShaderInfo's layout changes, --base-corpus names the same captured programs in the base build's corpus format.

--run plays the scene once with screenshots every 300 frames. It reports the exit, a crash or svcBreak, the frames,
the skipped draws and dispatches and the shaders that weren't translated, against the counters --set-reference kept.
Gameplay frames differ between runs, so no frame hash is compared: look at sheet.png.

Everything goes to <data folder>/switch1_check/<scene>/. The corpora and screenshots hold the game's shaders and
pictures: they stay there, never in the repo.
"""

import argparse
import filecmp
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_game  # noqa: E402

REPO = run_game.REPO
DATA = run_game.data_folder()
WORK = os.path.join(DATA, 'switch1_check')
SCENES = os.path.join(DATA, 'switch1_check.json')
SHADERC = 'snx-shaderc' + ('.exe' if sys.platform == 'win32' else '')


def git(*args, cwd=REPO):
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def build_shaderc(build_dir):
    """Brings snx-shaderc of an existing build folder up to date. Building that one target deploys nothing."""
    r = subprocess.run(['cmake', '--build', build_dir, '--target', 'snx-shaderc'], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
        raise SystemExit('building snx-shaderc in %s failed' % build_dir)
    return os.path.join(build_dir, SHADERC)


def base_shaderc(ref):
    """snx-shaderc of commit ref, built in a detached worktree of its own with SNX_DEPLOY off"""
    sha = git('rev-parse', '--verify', ref + '^{commit}')
    tree = os.path.join(WORK, 'base-worktree')
    if not os.path.isdir(tree):
        os.makedirs(WORK, exist_ok=True)
        git('worktree', 'add', '--detach', tree, sha)
    elif git('rev-parse', 'HEAD', cwd=tree) != sha:
        git('checkout', '--detach', sha, cwd=tree)
    build = os.path.join(tree, 'build')
    if not os.path.isfile(os.path.join(build, 'CMakeCache.txt')):
        env = dict(os.environ, CPM_SOURCE_CACHE=os.environ.get('CPM_SOURCE_CACHE', os.path.join(REPO, '.cache', 'cpm')))
        r = subprocess.run(
            ['cmake', '--preset', 'default', '-B', build, '-DSNX_DEPLOY=OFF', '-DBUILD_TESTING=OFF'],
            cwd=tree, env=env, capture_output=True, text=True,
        )
        if r.returncode:
            sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
            raise SystemExit('configuring the base build in %s failed' % build)
    return build_shaderc(build), sha


def run_scene(scene, out, corpus=None):
    """One run of the scene into out: run.log, the screenshots, and the counters of the log"""
    exe = run_game.find_game(scene['game'])
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    seconds = int(scene.get('seconds', 90))
    log = os.path.join(out, 'run.log')
    shot = os.path.join(out, 'shot')
    cmd = [exe, '--seconds=%d' % seconds, '--shot=' + shot, '--shot_every=300']
    if scene.get('script'):
        cmd.append('--script=' + scene['script'])
    if corpus:
        os.makedirs(corpus, exist_ok=True)  # the runtime writes into it but doesn't make it
        cmd.append('--shader_corpus=' + corpus)
    with open(log, 'w') as f:
        try:
            code = subprocess.run(cmd, cwd=os.path.dirname(exe), stdout=f, stderr=subprocess.STDOUT,
                                  timeout=2 * seconds + 60).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    line, bad = run_game.summarize(log, None, seconds)
    stats = {'exit': code, 'summary': line, 'failed': bool(bad or code != 0)}
    with open(log, errors='replace') as f:
        text = f.read()
    # The last of the renderer's summary lines (vk/renderer.c), printed every 600 presents and at exit
    totals = re.findall(r'vulkan: (\d+) draws, (\d+) skipped; (\d+) dispatches in \d+ batches \([^)]*\), (\d+) skipped',
                        text)
    if totals:
        draws, draw_skips, dispatches, dispatch_skips = map(int, totals[-1])
        stats.update(draws=draws, draw_skips=draw_skips, dispatches=dispatches, dispatch_skips=dispatch_skips)
    stats['not_translated'] = len(re.findall(r'^vulkan: \w+ [0-9a-f]+ not translated', text, re.M))
    stats['warnings'] = len(re.findall(r'^WARN', text, re.M))
    contact_sheet(out)
    with open(os.path.join(out, 'stats.json'), 'w') as f:
        json.dump(stats, f, indent=1)
    return stats


def contact_sheet(out):
    """sheet.png: the screenshots of a run, in order, three to a row, to look at in one picture"""
    shots = sorted(n for n in os.listdir(out) if re.fullmatch(r'shot_\d+\.ppm', n)) + ['shot.ppm']
    shots = [os.path.join(out, n) for n in shots if os.path.isfile(os.path.join(out, n))]
    try:
        from PIL import Image
    except ImportError:
        print('Pillow is not installed: no sheet.png, the screenshots stay .ppm files')
        return
    if not shots:
        return
    w, h, cols = 480, 270, 3
    sheet = Image.new('RGB', (w * cols, h * ((len(shots) + cols - 1) // cols)))
    for i, path in enumerate(shots):
        sheet.paste(Image.open(path).convert('RGB').resize((w, h)), ((i % cols) * w, (i // cols) * h))
    sheet.save(os.path.join(out, 'sheet.png'))


def translate(shaderc, corpus, out):
    """The SPIR-V of every corpus entry into out, and the entries that weren't translated, with the reason"""
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    r = subprocess.run([shaderc, corpus, '--out', out, '--verbose'], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:])
        raise SystemExit('%s failed on %s' % (shaderc, corpus))
    refused = dict(re.findall(r'^(\S+): unsupported: (.*)$', r.stdout, re.M))
    total = re.search(r'^\d+ shaders: .*$', r.stdout, re.M)
    return refused, total.group(0) if total else '?'


def show_diff(a, b, name):
    """The start of a disassembly diff of one shader that differs, when spirv-dis is on PATH"""
    if not shutil.which('spirv-dis'):
        return
    dis = [subprocess.run(['spirv-dis', '--raw-id', p], capture_output=True, text=True).stdout.splitlines(True)
           for p in (a, b)]
    import difflib

    lines = list(difflib.unified_diff(dis[0], dis[1], 'base/' + name, 'head/' + name, n=2))
    sys.stdout.writelines(lines[:40])
    if len(lines) > 40:
        print('... %d more lines of diff' % (len(lines) - 40))


def compare_shaders(base, head, corpus, folder, base_corpus):
    spv_base, spv_head = os.path.join(folder, 'spv_base'), os.path.join(folder, 'spv_head')
    listing = os.path.join(folder, 'differ.txt')
    if os.path.isfile(listing):
        os.remove(listing)  # an earlier check's, which this one may not repeat
    refused_base, total_base = translate(base, base_corpus, spv_base)
    refused_head, total_head = translate(head, corpus, spv_head)
    print('base: ' + total_base)
    print('head: ' + total_head)
    names = sorted(set(os.listdir(spv_base)) | set(os.listdir(spv_head)))
    only_base = [n for n in names if not os.path.isfile(os.path.join(spv_head, n))]
    only_head = [n for n in names if not os.path.isfile(os.path.join(spv_base, n))]
    both = [n for n in names if n not in only_base and n not in only_head]
    _, differ, errors = filecmp.cmpfiles(spv_base, spv_head, both, shallow=False)
    differ += errors
    for n in only_base:
        print('only the base translates %s; head: %s' % (n, refused_head.get(n[:-4], 'failed')))
    for n in only_head:
        print('only head translates %s; base: %s' % (n, refused_base.get(n[:-4], 'failed')))
    stages = {}
    for n in differ:
        stage = n[:-4].rsplit('_', 1)[-1]
        stages[stage] = stages.get(stage, 0) + 1
    if differ:
        print('%d shaders differ (by stage, 0 vertex to 5 compute: %s), the first is %s:'
              % (len(differ), ', '.join('%s: %d' % kv for kv in sorted(stages.items())), differ[0]))
        show_diff(os.path.join(spv_base, differ[0]), os.path.join(spv_head, differ[0]), differ[0])
        with open(listing, 'w') as f:
            f.write('\n'.join(differ + only_base + only_head) + '\n')
    else:
        print('the SPIR-V of all %d translated shaders is the same' % len(both))
    return len(differ) + len(only_base) + len(only_head)


def compare_run(stats, reference):
    """1 when the run is worse than the reference: it failed, or skipped or left untranslated more"""
    print('run: exit %s, %s' % (stats['exit'], stats['summary']))
    keys = ('draws', 'draw_skips', 'dispatches', 'dispatch_skips', 'not_translated', 'warnings')
    print('     ' + ', '.join('%s %s' % (k, stats.get(k, '?')) for k in keys))
    if not reference:
        print('     no reference: --set-reference keeps these counters to compare later runs with')
        return int(stats['failed'])
    print('ref: ' + ', '.join('%s %s' % (k, reference.get(k, '?')) for k in keys))
    worse = [k for k in ('draw_skips', 'dispatch_skips', 'not_translated')
             if stats.get(k, 0) > reference.get(k, 0)]
    if worse:
        print('     more than the reference: ' + ', '.join(worse))
    return int(stats['failed'] or bool(worse))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], allow_abbrev=False,
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split('\n\n', 1)[1])
    ap.add_argument('scene', help='an entry of %s' % SCENES)
    ap.add_argument('--base', help='the commit to compare with (default: the merge base of HEAD and main)')
    ap.add_argument('--base-corpus', help='the same captured programs in the base build\'s corpus format')
    ap.add_argument('--build', default=os.path.join(REPO, 'build'), help='the build folder of the change (default build/)')
    ap.add_argument('--run', action='store_true', help='also play the scene and compare its counters')
    ap.add_argument('--rerecord', action='store_true', help='record the scene\'s shader corpus again')
    ap.add_argument('--set-reference', action='store_true', help='keep this run\'s counters as the reference')
    ap.add_argument('--allow-diff', action='store_true', help='exit 0 when shaders differ (an intended change)')
    a = ap.parse_args()

    try:
        with open(SCENES) as f:
            scenes = json.load(f)
    except FileNotFoundError:
        raise SystemExit('no %s: make it with the scene, as the description of --help shows' % SCENES)
    if a.scene not in scenes:
        raise SystemExit('%s has no scene %s (it has: %s)' % (SCENES, a.scene, ', '.join(scenes) or 'none'))
    scene = scenes[a.scene]
    folder = os.path.join(WORK, a.scene)
    corpus = os.path.join(folder, 'corpus')
    os.makedirs(folder, exist_ok=True)
    if scene.get('note'):
        print('scene %s: %s' % (a.scene, scene['note']))

    failed = 0
    stats = None
    if a.rerecord or not os.path.isdir(corpus) or not os.listdir(corpus):
        if os.path.isdir(corpus):
            shutil.rmtree(corpus)
        # A recording translates every shader again and keeps none for the next run, so its counters are not those of
        # a normal run: it is a run of its own, never compared
        print('recording the shader corpus (one run of %s s)...' % scene.get('seconds', 90), flush=True)
        recorded = run_scene(scene, os.path.join(folder, 'record'), corpus)
        print('record: exit %s, %s' % (recorded['exit'], recorded['summary']))
        if not os.path.isdir(corpus) or not os.listdir(corpus):
            raise SystemExit('the run recorded no shaders: see %s' % os.path.join(folder, 'record', 'run.log'))
    if a.run:
        print('running the scene (%s s)...' % scene.get('seconds', 90), flush=True)
        stats = run_scene(scene, os.path.join(folder, 'run'))

    base, sha = base_shaderc(a.base or git('merge-base', 'HEAD', 'main'))
    head = build_shaderc(a.build)
    print('base %s, head %s (%s)%s' % (sha[:10], git('rev-parse', '--short=10', 'HEAD'), a.build,
                                       ' with uncommitted changes' if git('status', '--porcelain', '-uno') else ''))
    differ = compare_shaders(base, head, corpus, folder, a.base_corpus or corpus)
    if differ and not a.allow_diff:
        failed = 1

    if stats:
        reference_path = os.path.join(folder, 'reference.json')
        reference = None
        if os.path.isfile(reference_path):
            with open(reference_path) as f:
                reference = json.load(f)
        failed |= compare_run(stats, reference)
        if a.set_reference:
            shutil.copyfile(os.path.join(folder, 'run', 'stats.json'), reference_path)
            print('     kept as the reference')
        print('screenshots: %s' % os.path.join(folder, 'run', 'sheet.png'))
    return failed


if __name__ == '__main__':
    sys.exit(main())
