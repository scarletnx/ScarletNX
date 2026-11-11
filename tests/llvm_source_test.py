#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/llvm-source.sh against a fake curl and a list that pins a small file: the release downloaded and checked, a
file that has the pinned SHA-256 used without a download, one cut short downloaded again, a download of other bytes, one
that breaks off and a 504 refused with nothing left in the file's place, and a version the list lacks refused before any
download, naming it and the versions pinned. Then the checkout: the list's lines, the default version of each LLVM
script (scripts/linux-llvm.sh, android-llvm.sh, apple-llvm.sh) pinned in it, no download in those scripts themselves,
and each of them stopping on a version without a hash before it downloads or builds anything. Offline."""

import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent / 'scripts'
LLVM_SCRIPTS = ('linux-llvm.sh', 'android-llvm.sh', 'apple-llvm.sh')
RELEASE = b'the release\n' * 1000
PINNED = hashlib.sha256(RELEASE).hexdigest()
OTHER = b'other bytes\n' * 1000
URL = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-9.9.9/llvm-project-9.9.9.src.tar.xz'
# -o FILE and the URL last, as the script calls it. The mode file chooses what the server does: ok, the release;
# other, other bytes; cut, half the release and curl's status for a transfer that broke off; 504, curl -f's for it
FAKE_CURL = """#!/bin/sh
dir=$(dirname "$0")
while [ $# -gt 1 ]; do
    [ "$1" = -o ] && out=$2
    shift
done
echo "$1" >> "$dir/curl.log"
case $(cat "$dir/curl.mode") in
    ok) cp "$dir/release" "$out" ;;
    other) cp "$dir/other" "$out" ;;
    cut) head -c 6000 "$dir/release" > "$out"; exit 18 ;;
    504) echo "curl: (22) The requested URL returned error: 504" >&2; exit 22 ;;
esac
"""
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


def executable(path, text):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bin_dir, scripts, work = tmp / 'bin', tmp / 'scripts', tmp / 'work'
        for folder in (bin_dir, scripts, work):
            folder.mkdir()
        executable(bin_dir / 'curl', FAKE_CURL)
        # a build that started would be the script going on without its source
        executable(bin_dir / 'cmake', '#!/bin/sh\necho "$@" >> "$(dirname "$0")/cmake.log"\n')
        (bin_dir / 'release').write_bytes(RELEASE)
        (bin_dir / 'other').write_bytes(OTHER)
        log = bin_dir / 'curl.log'
        shutil.copy(SCRIPTS / 'llvm-source.sh', scripts)
        (scripts / 'llvm-source.sha256').write_text(
            f'# a comment\n{"0" * 64}  llvm-project-9.9.8.src.tar.xz\n{PINNED}  llvm-project-9.9.9.src.tar.xz\n'
        )
        env = dict(os.environ, PATH=f'{bin_dir}{os.pathsep}{os.environ.get("PATH", "")}')
        file = work / 'llvm-project-9.9.9.src.tar.xz'
        part = work / 'llvm-project-9.9.9.src.tar.xz.part'

        def run(mode, version='9.9.9', target=file):
            (bin_dir / 'curl.mode').write_text(mode + '\n')
            log.write_text('')
            return subprocess.run(
                ['sh', str(scripts / 'llvm-source.sh'), version, str(target)], env=env, capture_output=True, text=True
            )

        def downloads():
            return log.read_text().split()

        out = run('ok')
        check('a first run downloads the release', out.returncode == 0 and file.read_bytes() == RELEASE, out.stderr)
        check('from the release of its version, once', downloads() == [URL], downloads())
        check('and leaves no .part', not part.exists())

        out = run('other')  # a download now would replace it with other bytes
        check('a file with the pinned SHA-256 is used as it is', out.returncode == 0 and downloads() == [], downloads())
        check('and stays', file.read_bytes() == RELEASE)

        file.write_bytes(RELEASE[:6000])
        cut = hashlib.sha256(RELEASE[:6000]).hexdigest()
        out = run('ok')
        check('a file cut short is downloaded again', out.returncode == 0 and downloads() == [URL], out.stderr)
        check('and is the release afterwards', file.read_bytes() == RELEASE)
        check('saying the SHA-256 it had and the pinned one', cut in out.stderr and PINNED in out.stderr, out.stderr)

        for mode, why in (('other', 'of other bytes'), ('cut', 'that breaks off'), ('504', 'answered 504')):
            file.unlink(missing_ok=True)
            out = run(mode)
            check(f'a download {why} is refused', out.returncode != 0, out.stderr)
            check(f'a download {why} leaves no file', not file.exists() and not part.exists(), sorted(work.iterdir()))
        out = run('other')
        other = hashlib.sha256(OTHER).hexdigest()
        check('other bytes: the message has their SHA-256 and the pinned one', other in out.stderr and PINNED in out.stderr,
              out.stderr)  # fmt: skip

        file.write_bytes(RELEASE[:6000])
        out = run('504')
        check('a file cut short is not used when its download fails', out.returncode != 0, out.stderr)

        target = work / 'llvm-project-1.2.3.src.tar.xz'
        out = run('ok', '1.2.3', target)
        check('a version the list lacks is refused', out.returncode != 0, out.stderr)
        check('before any download', downloads() == [] and not target.exists(), downloads())
        check('naming it and the versions pinned', '1.2.3' in out.stderr and '9.9.8 9.9.9' in out.stderr, out.stderr)

        # the checkout's list and the three scripts that read it through llvm-source.sh
        lines = [line for line in (SCRIPTS / 'llvm-source.sha256').read_text().splitlines() if not line.startswith('#')]
        versions = []
        for line in lines:
            listed = re.fullmatch(r'[0-9a-f]{64}  llvm-project-(\d+\.\d+\.\d+)\.src\.tar\.xz', line)
            check('a line of llvm-source.sha256 is a SHA-256, two spaces and a release', listed, repr(line))
            if listed:
                versions.append(listed[1])
        check('llvm-source.sha256 lists a version once', len(versions) == len(set(versions)), versions)
        (tmp / 'ndk' / 'build' / 'cmake').mkdir(parents=True)
        (tmp / 'ndk' / 'build' / 'cmake' / 'android.toolchain.cmake').write_text('')
        env.update(ANDROID_NDK_ROOT=str(tmp / 'ndk'), SNX_SYSROOT=str(tmp), LLVM_VERSION='0.0.0')
        for name, first in zip(LLVM_SCRIPTS, ('x86_64', 'x86_64', 'macos')):
            text = (SCRIPTS / name).read_text()
            default = re.search(r'\$\{LLVM_VERSION:-([0-9.]+)\}', text)
            check(f'{name} has a default version', default)
            if default:
                check(f"{name}'s default version is pinned in llvm-source.sha256", default[1] in versions,
                      f'{default[1]}, and the list has {versions}')  # fmt: skip
            check(
                f'{name} downloads through llvm-source.sh alone',
                'curl' not in text and '/llvm-source.sh" "$version" "$source"' in text,
            )
            (bin_dir / 'curl.mode').write_text('ok\n')
            log.write_text('')
            (bin_dir / 'cmake.log').write_text('')
            out = subprocess.run(
                ['sh', str(SCRIPTS / name), first, str(tmp / f'prefix-{name}')], env=env, capture_output=True, text=True
            )
            check(
                f'{name} stops on a version without a hash', out.returncode != 0 and '0.0.0' in out.stderr, out.stderr
            )
            check(f'{name} then downloads and builds nothing', downloads() == [] and (bin_dir / 'cmake.log').read_text() == '',
                  downloads() + [(bin_dir / 'cmake.log').read_text()])  # fmt: skip
    if failures:
        print('\n'.join(f'FAIL: {failure}' for failure in failures))
        return 1
    print(f'PASS llvm-source.sh: the release by its SHA-256, {len(versions)} versions pinned ({", ".join(versions)})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
