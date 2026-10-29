<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Contributing

[Build](docs/BUILD.md) · [development](docs/DEVELOPMENT.md) · [architecture](docs/ARCHITECTURE.md) ·
[agent rules](CLAUDE.md).

## Game data stays out

Nothing of a game goes into git, an issue, a pull request or a comment: keys, dumps (XCI, NSP, NCA, CIA), `work/`,
`games/` and shader or texture dumps. [CLAUDE.md](CLAUDE.md) has the full rule.

## Build and check

[BUILD.md](docs/BUILD.md) covers the toolchains. Before a pull request, from the checkout root:

```sh
cmake -DCMAKE_BUILD_TYPE=Release -Bbuild -S.
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/run_filesystem_tree.py --cc clang --sanitize
```

Nothing checks formatting. `.clang-format` (clang-format 23) has the style, and `git clang-format` formats only the
lines a change touches. Configuring with `-DSNX_TEST_SANITIZERS=ON` builds the native
regression tests, and the programs and libraries they run or link, with ASan and UBSan. That build is for tests only and
is not deployed.

A build is not a test. A runtime change is verified by running the game through the scene it affects, and the commit
says what was run. [DEVELOPMENT.md](docs/DEVELOPMENT.md) has the run and diagnose commands, and
[TESTING.md](docs/TESTING.md) the form a test report takes: the build, the machine, the scene, the runs and the result.

## Commits

The subject is a bracketed area followed by a lowercase imperative sentence. The body says why, and how it was
verified. A `Signed-off-by` line (`git commit -s`) is encouraged but not required. For example:

```
[tests] avoid the Windows near macro in the motion test

Rename the local float comparison helper so the Windows header macro does not corrupt its
declaration and calls. Verified with x86_64-w64-mingw32-clang using the CI compile flags and
the hid_motion CTest.

Signed-off-by: Your Name <you@example.org>
```

Common areas, from the history: `[gui]`, `[shader]`, `[ci]`, `[runtime]`, `[gpu]`, `[audio]`, `[cmake]`, `[docs]`,
`[cpu]`, `[hid]`, `[hle]`, `[test]`, `[build]`, `[kernel]`, `[driver]`, `[cli]`, `[android]`, `[vk]`, `[dist]`,
`[agents]`, `[3ds]`. Two areas share one bracket, as in `[gui, android]`.

Commit messages and user-facing docs read as plain prose: no em-dash splicing clauses, no semicolons joining
sentences, no spaced hyphen as a dash.

## Pull requests

Each run of the Build workflow (`.github/workflows/build.yml`) on a pull request rewrites one comment on it, marked
`<!-- scarletnx-builds -->`, that links the Linux, AppImage, Windows and Android builds of the head commit and names
any job that did not build (`scripts/pr-builds.sh`). A pull request from a fork gets no comment, since its token may
not write.

GitHub asks the owners of the files a pull request changes for their review once it is out of draft:
`.github/CODEOWNERS` names those of the CI, GUI, GPU and CPU code, and of the 3DS and the Vita.

## C code

New C follows the rules of [RFC #142](https://github.com/scarletnx/ScarletNX/issues/142): strict ISO C99, no state
another value already says, indices instead of pointer chains, one allocation per owner. The
`.claude/skills/snx-code-quality/SKILL.md` skill is the reference, with the exceptions to each rule.

## License

ScarletNX is GPL-3.0-or-later (`LICENSE`). A new file starts with the SPDX line and the project's copyright line,
in the file's comment syntax:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
```

Code ported from another project keeps its copyright lines, names where it came from and gets a row in
[docs/CREDITS.md](docs/CREDITS.md). Code under GPL-3.0, GPL-2.0-or-later (yuzu, suyu, sudachi, Citron, Eden), LGPL,
MIT (Ryujinx, LibHac), BSD, ISC, zlib or Apache 2.0 can be ported. GPL-2.0-only code can't be. Atmosphère is
GPL-2.0-only, so it is read as a reference for how Horizon behaves and never ported.
