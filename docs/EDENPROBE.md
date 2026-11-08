<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# edenprobe

## Index

* [Running it](#running-it)
* [On a player's machine](#on-a-players-machine)
* [The mode words](#the-mode-words)
* [In CI](#in-ci)
* [The baseline](#the-baseline)
* [A case from a game report](#a-case-from-a-game-report)

[edenprobe](https://github.com/scarletnx/edenprobe) is a homebrew program of hardware probes: suites of cases, each
asking the machine one question and judging the answer against the console's. A run leaves `report.jsonl`, a line per
case (`pass`, `fail` or `skip`, with a detail) and a summary. `scripts/edenprobe.py` recompiles it, runs it headless and
holds the report against a baseline, how each case ended before: a case of the baseline that no longer ends that way
fails.

## Running it

```sh
python3 scripts/edenprobe.py run --runtime build/scarletnx --nro edenprobe.nro --out /tmp/edenprobe \
    --mode "quick notime fatal cpu fs video display kernel audio" \
    [--baseline tests/edenprobe/linux-x86_64.txt]
```

`--out`, empty or an earlier run's, is emptied, then gets the recompiled game and its XDG folders (`work/`, so
nothing of yours is read or written), `report.jsonl`, the failures' pictures (`images/`), the recompile's and each
launch's log, `summary.md` and `baseline.txt`: this run's passing cases, and those the baseline expects to end
another way that did. A launch that ends before the summary, crashed
or killed after `--timeout` seconds (600) without a new line in the report, is followed by another, which goes on after
the case it ended inside, reported as `crashed`. The baseline is lavapipe's: a GPU passes other cases, so on one the
run says what differs rather than what regressed.

`--setting KEY=VALUE` gives each launch that runtime setting as `--KEY=VALUE`, for example `vk_memory=2` to read guest
buffers in place, the default on an integrated GPU, where a driver can still refuse the imports and copy. Lavapipe,
which CI runs on, copies by default: its GPU is the CPU, which can't read pages the runtime traps. To run lavapipe
without installing it, point `VK_DRIVER_FILES` at its ICD file.

With `-DSNX_TEST_EDENPROBE=/path/to/edenprobe.nro`, ctest runs it as `edenprobe_vk_memory_1` (copied) and
`edenprobe_vk_memory_2` (in place) against the baseline.

## On a player's machine

```sh
scarletnx --probe=quick --probe-nro edenprobe.nro [--gpu_driver=ID]
```

`--probe` runs it headless with the program's own driver, in `probe/` of the state folder
(`~/.local/state/scarletnx/probe` on Linux). The run's data folder is a portable one there, `probe/.scarletnx`, so it
reads none of the player's settings, saves or users. `--gpu_driver` and `--freedreno_env` go to each launch, and the
package `--gpu_driver` names is the player's.
`quick` writes the mode `quick notime cpu graphics`; `full` adds `fs video audio kernel display`. Neither asks for
`net`, `service`, `pad`, `listen`, `fatal`, `hang`, `stress` or `game`.

The game made from the NRO stays in the folder and is made again when the NRO or the program changes. Each run starts
edenprobe's card over, but for the fs family's files: no report, journal or crash list of an earlier run, so a case
reported as crashed ended this run's program. A launch that ends before the summary, crashed or ended after 600 s
without a new report line, is followed by another (16 at most), which goes on after the case it ended inside.

It prints the cases as they come (`probe: 412 of 1069 cases`, the whole once a run of that NRO in that mode has ended),
then the result:

```
probe: edenprobe 0.9.0 (v0.9.0), sha256 20f2c2b78457a774f957e4debcc6fc9de8046507302a84a9e275ca0d20cc92b5, mode "quick notime cpu graphics", 1 launch, settings: defaults
probe: 1059 passed, 2 failed (tex8k 2), 8 skipped, 0 crashed
probe: report /home/user/.local/state/scarletnx/probe/report.jsonl
```

The folder keeps `report.jsonl`, `summary.txt` (the result's lines), `recompile.log` and each launch's output,
`launch-N.log`, which is the game's log: the machine's lines and every report line. The exit code is 0 once the report
reached its summary, whatever its cases, and 1 when it did not.

## The mode words

Those of the example, which CI runs; edenprobe's
[mode file](https://github.com/scarletnx/edenprobe/blob/main/docs/ModeFile.md) has them all. The judge's suites
(selfcheck, support, reference) run whatever is named:

| word | what it adds |
|---|---|
| `quick` | the run with no one in front of it: every suite named, the report, exit |
| `notime` | each drawing suite once, no timing passes, no times in the report |
| `fatal` | the cases that may end the program: SM4, CNTVCT_EL0 and DAIF reads, geometry shaders, BC4 arrays |
| `cpu` | the processor's instructions |
| `fs` | the memory card: a 64 MiB file and 256 small ones, written once and read back |
| `video` | the video image compositor, and the VP9 decoder on libvpx's test vectors |
| `display` | a layer's buffer history |
| `kernel` | the scheduler: priority inheritance, preemption, migration |
| `audio` | the audio renderer: resampler, circular buffer, limiter; headless, nothing is played |

Left out: `graphics` (the drawing suites: clears, blending, depth, textures and their formats, queries, blits),
which write their own GPU command buffers. The runtime's GPU path is NVN, so their submissions draw
nothing, and each case waits for memory that the GPU would write. `net` (servers the runner does not have), `pad` and `listen` (a person), `stress` (256 MiB lists), `hang`
(cases that never return on a machine that gets them wrong).

## In CI

The `edenprobe` job of `.github/workflows/build.yml` takes the Linux x86-64 install and runs the release
`EDENPROBE_VERSION` of edenprobe, pinned by `EDENPROBE_SHA256`, on lavapipe with the words of `EDENPROBE_MODE`, against
`tests/edenprobe/linux-x86_64.txt`. The result goes in the run's summary and in the pull request's comment of builds;
the artifact `edenprobe-<commit>-<attempt>` holds the report, pictures, logs and `baseline.txt`.

A regression there fails the Build run, and a run that failed publishes nothing: neither the nightly nor a version.

Then the job runs edenprobe's main, its `nightly` release (checked by its `.sha256`), with
`scripts/edenprobe.py run --advisory edenprobe/report.jsonl`: beside the pinned release's report it never fails. A case
the release has not got that does not pass, a case the release passes that does not pass here, a run that ends early, or
a nightly that cannot be fetched is a warning on the run, and the summary has a section of its own. A case new in main
becomes binding when a release that has it is pinned and the baseline written again.

## The baseline

`tests/edenprobe/<platform>.txt` names the edenprobe build and the mode words it was written with, then one case a
line: `suite: name` for a case that passes, `[RESULT] suite: name` for one that ends as RESULT, the result as the summary
lists it (`skip (crashed)`, `fail: DETAIL`; no `]` in it). That second form is for a case that ends the program on the
console too, as an instruction the console refuses: the line keeps the case held to that end, and any other, a pass
included, fails the job. Another build or other words fail the job until it is written again; a case that starts to
pass is listed, to be added. Write it from CI's own runs, never a desktop GPU's: download the artifact of two or three
runs (run the job again for more) and keep what ended the same way in each, so a case that passes now and then stays
out. A run given the baseline writes into its `baseline.txt` the other results it expects that the run had, so they
come through:

```sh
python3 scripts/edenprobe.py baseline run1/baseline.txt run2/baseline.txt -o tests/edenprobe/linux-x86_64.txt
```

## A case from a game report

A game report whose failure a maintainer understands gets the label `status: confirmed`, and the Game reports
workflow comments on it once: it is ready for an edenprobe case, one that asks the console the question the failure
comes down to, so every build is held to the answer. `scarletnx --mcp` has the tools an agent writes it with, in a
clone of [edenprobe](https://github.com/scarletnx/edenprobe):

- `edenprobe_scaffold` (service cases): writes the parts edenprobe's layout fixes into the clone, every file or none.
  Those are the case's enum entry and `CASES[]` row, its judge and the judge's reference check in
  `source/serviceref.[ch]` and `source/tests/t_serviceref.c`, the device side in `source/tests/t_service.c` called at
  `RunService`'s end, and a new suite's row in `source/ui.c`. Each `TODO` it leaves names the issue. What is left is
  the service call, the judge's logic, the expected answer and where it comes from (reden's commit and line, or a
  console), the wrong answers the reference check holds the judge to, ScarletNX's among them, and the docs' counts.
- `edenprobe_check`: `make -f host.mk host-run` in the clone. The judges and their checks run with the host's
  compiler, no devkitPro. Until the docs' counts in words are updated, its `test_judge_counts` fails on purpose.
- `edenprobe_run`: the NRO, built from the clone with edenprobe's pinned devkitPro image or taken from its pull
  request's CI artifact, recompiled with this program and run headless in the mode's words until its summary. It
  answers the report's lines for one suite or case and the report's path. The recompile is kept while the NRO is
  unchanged, and nothing of the person's ScarletNX folders is read or written.
- `edenprobe_finding`: the case's line in ScarletNX's report and in the reference's (Eden's, or a console's), checked
  that it fails on ScarletNX and passes on the reference, and the pull request's text: the commit subject (72
  characters at most), the issue, what each machine answered and the rules a case keeps. Nothing of a game goes in
  it: no code, no disassembly, no assets, no value only that game uses.

The commit and the pull request to scarletnx/edenprobe stay with the agent and the person, under edenprobe's
`hooks/commit-msg`.
