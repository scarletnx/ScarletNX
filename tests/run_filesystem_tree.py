# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Compile and exercise the production filesystem tree using disposable synthetic data."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang', help='Clang executable (default: clang)')
    parser.add_argument('--sanitize', action='store_true', help='Enable address and undefined-behavior sanitizers')
    parser.add_argument('--only', choices=('tree', 'service'), help='Run just one test harness')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    temp_parent = Path(tempfile.gettempdir()).resolve()
    scratch = Path(tempfile.mkdtemp(prefix='scarletnx-fs-tree-', dir=temp_parent)).resolve()
    token = uuid.uuid4().hex
    marker = scratch / '.snx-fs-test'
    marker.write_text(token, encoding='ascii')
    try:
        data = scratch / 'data'
        files = {
            'drive/seed.bin': b'initial',
            'drive/folder/nested.bin': b'nested',
            'drive/folder/sub/deep.bin': b'deep',
            'drive/partial/a.bin': b'first',
            'drive/partial/blocked/known.bin': b'known',
            'root/root.txt': b'root',
            'romfs/asset.bin': b'asset',
            'romfs.bin': b'raw',
        }
        for name, content in files.items():
            path = data / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        (data / 'drive/empty').mkdir()
        (scratch / 'without-root').mkdir()
        bad_links = scratch / 'hardlinks'
        bad_links.mkdir()
        (bad_links / 'original.bin').write_bytes(b'shared')
        os.link(bad_links / 'original.bin', bad_links / 'alias.bin')
        for name, content in {
            'sdmc/seed.bin': b'seed',
            'romfs/asset.bin': b'romfs test',
            'romfs/empty.bin': b'',
            'romfs/zulu.bin': b'tail',
            'root/root.txt': b'root',
        }.items():
            path = scratch / 'service-data' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        (scratch / 'raw-data').mkdir()
        (scratch / 'raw-data/romfs.bin').write_bytes(b'RAW\x00ROMFS\xff bytes')

        # Intercept native enumeration only in the production translation unit. The
        # harness aborts if initialization has finished and another scan is attempted.
        probe = scratch / 'scan_probe.h'
        probe.write_text(
            '#ifdef _WIN32\n'
            '#define WIN32_LEAN_AND_MEAN\n'
            '#include <windows.h>\n'
            'HANDLE WINAPI fs_test_FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA);\n'
            'HANDLE WINAPI fs_test_FindFirstFileW(LPCWSTR, LPWIN32_FIND_DATAW);\n'
            '#define FindFirstFileA fs_test_FindFirstFileA\n'
            '#define FindFirstFileW fs_test_FindFirstFileW\n'
            '#else\n'
            '#include <dirent.h>\n'
            'DIR* fs_test_opendir(const char*);\n'
            '#define opendir fs_test_opendir\n'
            '#endif\n',
            encoding='ascii',
        )
        flags = [
            '-std=gnu23',
            '-O1',
            '-g',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-I',
            str(repo / 'src'),
        ]
        if os.name == 'nt':
            flags += ['-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE']
        else:
            flags += ['-pthread', '-D_FILE_OFFSET_BITS=64']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        run_env = os.environ.copy()
        if args.sanitize and os.name == 'nt':
            resource_dir = subprocess.check_output([args.cc, '-print-resource-dir'], text=True).strip()
            runtime_dir = Path(resource_dir) / 'lib/windows'
            run_env['PATH'] = str(runtime_dir) + os.pathsep + run_env.get('PATH', '')
        tree_obj = scratch / ('fs_tree.obj' if os.name == 'nt' else 'fs_tree.o')
        subprocess.run(
            [
                args.cc,
                *flags,
                '-include',
                str(probe),
                '-c',
                str(repo / 'src/common/filesystem/tree.c'),
                '-o',
                str(tree_obj),
            ],
            check=True,
            cwd=scratch,
        )
        # The service suite includes fs.c, whose IPC dispatchers tools/swipc generates from the .swipc files,
        # as the CMake build does (cmake/swipc.cmake). A handler takes every argument of its command, used or not
        gen = scratch / 'swipc-out'
        if args.only != 'tree':
            swipc = scratch / ('swipc.exe' if os.name == 'nt' else 'swipc')
            subprocess.run(
                [args.cc, '-std=c23', str(repo / 'tools/swipc/swipc.c'), '-o', str(swipc)], check=True, cwd=scratch
            )
            swipc_files = ['hle/services/types.swipc', 'hle/filesystem/fs.swipc']
            for f in swipc_files:
                (gen / f).parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(swipc), '--root', str(repo / 'src/nx'), '--out', str(gen), *swipc_files], check=True)
        for suite in (args.only,) if args.only else ('tree', 'service'):
            name = f'filesystem_{suite}'
            exe = scratch / (name + '.exe' if os.name == 'nt' else name)
            suite_flags = ['-I', str(gen), '-Wno-unused-parameter'] if suite == 'service' else []
            subprocess.run(
                [args.cc, *flags, *suite_flags, str(repo / f'tests/{name}.c'), str(tree_obj), '-o', str(exe)],
                check=True,
                cwd=scratch,
            )
            subprocess.run([str(exe), str(scratch), token], check=True, cwd=scratch, env=run_env)
            if suite == 'service':
                subprocess.run([str(exe), str(scratch), token, 'raw'], check=True, cwd=scratch, env=run_env)
    finally:
        # Resolve and verify the unique scratch directory before recursive cleanup.
        resolved = scratch.resolve()
        if (
            resolved != scratch
            or resolved.parent != temp_parent
            or not resolved.name.startswith('scarletnx-fs-tree-')
            or marker.read_text(encoding='ascii') != token
        ):
            raise RuntimeError(f'Refusing to clean up an unverified scratch directory: {scratch}')
        shutil.rmtree(resolved)


if __name__ == '__main__':
    main()
