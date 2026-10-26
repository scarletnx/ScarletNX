<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Testing and reporting

[Usage](USAGE.md) · [development](DEVELOPMENT.md) · [contributing](../CONTRIBUTING.md) · [edenprobe](EDENPROBE.md).
What to say in a test report so it can be compared with the last one. Nothing of a game goes into a report: no keys,
dumps, game files or shader and texture dumps ([CONTRIBUTING](../CONTRIBUTING.md#game-data-stays-out)).

## Index

* [What to test](#what-to-test)
* [Which build](#which-build)
* [A game that does not run as it should](#a-game-that-does-not-run-as-it-should)
* [A test of a pull request or a fix](#a-test-of-a-pull-request-or-a-fix)
* [Performance regression tests](#performance-regression-tests)
* [Naming your machine](#naming-your-machine)
* [What happens to a report](#what-happens-to-a-report)

## What to test

Say what ran, for how long and how it ended:

- The scene, in words someone can reach: "New Game, first cutscene, then the first fight".
- The input: a `--script=` ([Run / diagnose](DEVELOPMENT.md#run--diagnose)) or what was pressed.
- The result: ran to the end, crashed, froze, looked or sounded wrong, and the time it took.
- How many runs. One passing run shows nothing about a crash that comes one run in five.
  `tools/run_game.py <id> --runs 5 --seconds 60` repeats a scene and summarizes each run.

## Which build

- A nightly: its tag, `nightly-<date>-<commit>`.
- A pull request's build: `pr-<number>` and the commit in its release notes. The next push replaces it.
- A build from source: `git rev-parse --short HEAD`, and whether the tree was clean.

The game's log starts with a `build:` line that says the same. A game's code is recompiled only when the recompiler
changes ([CLAUDE.md](../CLAUDE.md)), so a `games/` folder made by an older build can still hold old code. Say so if you
tested with one.

## A game that does not run as it should

Use the [Game report](https://github.com/scarletnx/ScarletNX/issues/new?template=game-report.yml) form. The title is
`[game] <name>: <symptom in a few words>`.

| Field | What goes in it |
| --- | --- |
| Game | Its name, and its version or update if known |
| What happens | The closest of: does not start, crashes, freezes, looks wrong, sounds wrong, controls, slow |
| What you did and what you saw | Where in the game, what was pressed before, what you expected |
| Log | The game's `.log` or the launcher's *Share Log* file. Zip a log over 25 MB |
| Save | Optional. The zipped save folder |
| Screenshots or video | Optional |

The log carries the build, OS, processor, RAM and GPU with its driver, so the form does not ask for them. Without a
log, [name your machine](#naming-your-machine).

## A test of a pull request or a fix

A comment or `Testing:` section that reports a test uses this shape:

```
Tested <commit> on <OS and version>, <processor>, <graphics card> (driver <version>), <RAM>.

Build: <nightly tag, or pr-<number> and commit, or built from source>
Game: <name>, version <version>, title ID <id>
Scene: <where, and what was pressed>
Runs: <number> of <number> ended as expected
Before: <how the same scene ended without the change, with the build that shows it>
Result: <what happened, with the lines of the log that decide it>
Not tested: <what the change touches that this run did not reach>
```

- `Before` makes a fix visible: the failing build and its first decisive log line, or the same frame without and with
  the change.
- `Result` quotes the decisive log lines (`build:`, `vulkan:`, crash, `FATAL`) and attaches the log, or puts it in a
  `<details>` block.
- A fix for an intermittent crash needs enough runs to show it is gone.

## Performance regression tests

A change to the recompiler, to a GPU shader or to the recompile path is also held by `perf_cpu`, `perf_gpu` and
`perf_compile`, which time a CPU path, a GPU path and a real `scarletnx -r` compile. They use little of the machine and
skip locally when it is too busy to trust a timing, while CI fails rather than skips. See
[performance regression tests](PERF.md) for what they measure, how to run them and what a ceiling means.

## Naming your machine

With no log attached, open the report with one line:

```
Windows 10 (10.0.19044), Xeon E5-2640 v3 (8 cores, 16 threads), GeForce RTX 3070 (driver 596.49), 16 GB RAM
```

- Linux: `lscpu` and `vulkaninfo --summary`. Windows: *Settings > System > About*, and the driver from the vendor's
  panel or `vulkaninfo`. Android: the model and the Android version.
- Name a software renderer (lavapipe, llvmpipe) as such. It is not a GPU result.
- Leave out the machine's name, user name and home folder.

## What happens to a report

A game report with a log gets one comment from the workflow within minutes: the build, game, GPU, how the run ended,
the game function it is pinned on, unimplemented service commands, frames over 50 ms, and other reports that failed
the same way. It labels the issue `game report` and `log: <how it ended>`. `build/scarletnx --scan-log LOG` gives the
same summary locally ([Run / diagnose](DEVELOPMENT.md#run--diagnose)).

A maintainer who reproduces the failure labels it `status: confirmed`, which posts that it is ready to become an
[edenprobe](EDENPROBE.md) case. A fix refers to the report.
