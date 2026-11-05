<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Contributing

## Index

* [Game data stays out](#game-data-stays-out)
* [Build and check](#build-and-check)
* [Commits](#commits)
* [Pull requests](#pull-requests)
* [C code](#c-code)
* [License](#license)

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

Run the preflight before a pull request:

```sh
python3 scripts/ci-local.py preflight
```

It checks the merge with the latest `origin/main`, the docs and agent files included, and the build and tests that
the change needs. Docs-only changes skip the build.

When Podman or Docker is present and the change touches code, add the Linux job:

```sh
python3 scripts/ci-local.py preflight --containers
```

It runs the Linux job of this architecture with 4 CPUs and 4 build jobs. The first run fills the container build,
dependency and ccache folders, and later runs reuse them. `--both --sanitizers` is the full gate for both
architectures and ASan, and it
takes much longer. The container checks are recommended, not required. Without an engine, `preflight` runs the host
checks and names the checks that did not run. The command never installs an engine.
[DEVELOPMENT.md](docs/DEVELOPMENT.md#local-ci-containers) gives the container setup.

The container installs its own `ccache`, in `.cache/ci-ccache-<arch>`. A host `ccache` is optional for the native
build. Setup is one time. The first container build is the slow one, and later runs reuse its caches. Android and
iOS LLVM builds are optional and only for local platform builds; the CI artifacts prove those builds.

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

When a run of the Build workflow (`.github/workflows/build.yml`) on a pull request ends, the Pull request checks
workflow rewrites one comment on it, marked `<!-- scarletnx-builds -->`, that links the Linux, AppImage, Windows and
Android builds of the head commit and names any job that did not build (`scripts/pr-builds.sh`). A pull request from a
fork gets no comment, since its Build ran the fork's own workflow.

A pull request, a draft as one ready for review, is built and tested on every platform: Linux on x86-64 and AArch64
(with edenprobe, and ASan and UBSan), Windows, Android, macOS and iOS, and the cross-built AppImages (PowerPC64,
RISC-V, LoongArch).

A pull request leaves draft when someone with write access to the repository marks it ready for review. One opened,
reopened or marked ready by anyone else goes back to a draft, with one comment saying so, unless a maintainer marked it
ready before (`.github/workflows/pr-ready.yml`, `scripts/pr-ready.py`). Say in a comment when yours is ready, and a
maintainer marks it.

A push to a pull request cancels the run of its commit before, and a pull request merged or closed cancels its runs
still going (`.github/workflows/pr-closed.yml`, `scripts/cancel-pr-runs.py`), so no runner builds what nobody will
merge.

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
