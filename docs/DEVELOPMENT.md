<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Development

[Architecture](ARCHITECTURE.md) · [build/toolchains](BUILD.md) · [usage](USAGE.md) · [agent rules](../CLAUDE.md) · [contributing](../CONTRIBUTING.md).
Run from checkout. POSIX examples: Windows uses `python`, `.exe`, PowerShell env syntax.
The build tree's drivers work in the user's data folder, as an installed copy does ([Folders](USAGE.md#folders));
the examples name its folders on Linux, and `--repo DIR` keeps a run's files in DIR instead:

```sh
games=~/.local/share/scarletnx/games work=~/.cache/scarletnx/work
```

## Rebuild / checks

```sh
cmake --build build
./build/scarletnx -r "$work/Game" Game --jobs 8
ctest --test-dir build --output-on-failure
scripts/format.sh --check
python3 tests/run_filesystem_tree.py --cc clang --sanitize
clang -fsyntax-only -Wall -fno-strict-aliasing -fwrapv -I src src/common/cpu/aarch64/cpu_helpers.c
```

Runtime relink deploys to the data folder's `recompiled/` by default (`SNX_RECOMPILED_DIR`). Recompiler/`src/common/cpu/aarch64/` edits require `-r`.
Avoid builds during timing runs. Formatting: clang-format 23 + `uv`/pinned Ruff; omit `--check` to write.
FS tests use production tree/HLE + synthetic temp data; `--only tree|service` selects a suite.

CI: Linux/Windows builds, ABI syntax, native tool/FS regressions, optional Python diagnostics, the Linux AppImages
and the Windows zip (each then run as installed), the Android APK, and [edenprobe](EDENPROBE.md)'s probes on lavapipe
against the cases that passed before (`scripts/edenprobe.py`). `.github/workflows/publish.yml` publishes what
builds everywhere, each said on Discord (`scripts/publish-builds.py`): a version tag pushed (`v1.0.0`; `v1.0.0-rc1` a
prerelease) as this repository's release of it, the latest, its notes listing the changes since the version before;
each push to `main` as a release of [scarletnx/nightly](https://github.com/scarletnx/nightly/releases), the newest 30
kept (this repository's `nightly` prerelease shows the latest's notes); each pull request as a prerelease there,
`pr-<number>`. The notes and the messages link each download with its size and list what changed (a pull request's
commits). A fork's pull request is published once someone of the team labels it `publish builds`, after each
push. The Discord cards of each pull request, each issue and each push to `main` are ReiWimsy's, the bot of
[scarletnx/discord-bots](https://github.com/scarletnx/discord-bots), which asks GitHub what changed every 30 s: no job
of this repository draws them. The secrets: `NIGHTLY_TOKEN` (writes that repository's contents) and
`DISCORD_WEBHOOK_RELEASES` (a version's message). Formatting is checked locally (`scripts/format.sh --check`).
Verify affected scenes manually, repeat intermittent failures, record evidence/scope.
Test guest semantics + malformed IPC with synthetic cases.

## Run / diagnose

```sh
(cd "$games/<id>" && ./<id> --seconds=60 --script=15000:400,16000:1 --press_len=300 --shot=out)
python3 tools/run_game.py <id> --runs 5 --seconds 60 --dump "$work/Game" --script ""
python3 tools/symbols.py "$work/Game" cf34380
python3 tools/disasm.py "$work/Game" NvRmStreamFlushSync
python3 tools/disasm.py "$work/Game" cf34240 cf343c0
build/scarletnx --scan-log "$games/<id>/<id>.log" [--format markdown]
```

- `--scan-log` (`src/common/utilities/logscan.c`, also built as `build/snx-logscan`): a log, or the launcher's Share
  log file (the game log in it), as a summary: the build, title ID, game version and GPU from the log's header, how
  the run ended, the game function it is pinned on (for `svcBreak`, the first frame past the SDK's abort), the service
  commands the runtime doesn't implement, warnings, the frames over 50 ms (how many, the slowest, and what the
  slowest did, from the renderer's frame lines), and a signature that is the same for every log failing the same way.
  JSON (a line a log, versioned by `schema`) or a markdown summary for an issue. Paths are shortened to `~`, and the
  crash report's code and memory dumps are left out. The launcher's Share log puts the same summary at the top of
  `scarletnx.log`. A log is read once, a line at a time, in fixed memory: a gigabyte takes about two seconds.
- `scarletnx --mcp [--repo DIR]` (`src/nx/runtime/driver/mcp.c`): an MCP server over stdio for an agent looking into a
  failure on this machine, built into the program so a player's agent has it too. `.mcp.json` starts it for agents
  opened in the checkout (`build/scarletnx --mcp --repo .`). Its tools: `scan_log` (the summary above), `read_log`
  (a log's lines holding a text, `|` between alternatives, with context, or a range, or its last lines, only under
  the data folder and its state, cache and settings folders), `find_game` (the extracted games in `work/` with their
  title IDs and versions), `symbolize` and `disassemble` (as `tools/symbols.py` and `tools/disasm.py`, A64 through
  LLVM's disassembler where LLVM has the AArch64 target). The last game loaded stays loaded for the next call. Its
  edenprobe tools turn a confirmed report into an edenprobe case ([EDENPROBE.md](EDENPROBE.md)). Its GDB tools
  (`mcp_gdb.c`, [USAGE.md](USAGE.md#gdb-through-the-mcp-server)) run one GDB over its machine interface (MI 3,
  resumes as `-exec-*` commands in the background). They find a guest thread's `KThread` through its host thread's
  `tls_thread`, by the runtime's debug information, and guest memory at `g_mem`. The game reports
  on GitHub come from GitHub's own MCP server or `scripts/game-reports.py index`.
- Game reports: `.github/workflows/game-reports.yml` builds `snx-logscan` and runs `scripts/game-reports.py triage` on
  each issue and comment. An issue with a log attached (`.log`, `.txt`, `.gz`, `.zip`) or pasted in a fenced block gets
  one comment with the summary of each, edited as logs are added, the labels `game report` and `log: <how it ended>`,
  and the other reports with the same signature named. `triage --issue N --dry-run --logscan build/snx-logscan` shows
  what it would post. `scripts/game-reports.py index [--title-id ID] [--signature TEXT]` lists the reports and their
  logs as JSON lines. A maintainer's `status: confirmed` label on a report gets one comment: ready for an edenprobe
  case, with the steps.

- Runtime: `--seconds=N` / `--frames=N`, counted from the game's start, after the shader loading screen
  (`prepare_shaders=2`, which the first run after a rebuild spends translating the game's shaders again);
  `--shot=out` → CWD `out.ppm`; `--shot_every=N` → every N frames.
  `--script=ms:hexbuttons,...` (or `Nf:hexbuttons` to press at present N), `--press_len=ms` (default 300).
  `--swkbd_text=TEXT`: the software keyboard answers TEXT instead of opening the host's dialog.
  Masks: A/B/X/Y `1/2/4/8`, L/R `40/80`, ZL/ZR `100/200`, Plus/Minus `400/800`,
  left/up/right/down `1000/2000/4000/8000`.
- `run_game.py`: Catherine New Game sequence by default; **`--script ""` disables it**.
  `--out DIR` → `runN.log` + PNG (PPM without Pillow); default fresh temp dir.
  Summary: exit, PC/symbol (`--dump`), frames/FPS. Unknown args pass through, e.g. `--fps_limit=0`.
- Guest VAs: hex; disasm function queries: substrings. Disasm requires `llvm-objcopy` + `llvm-objdump`.
  `symbols.py` and `disasm.py` read the modules with the recompiler's loader, through
  `build/scarletnx --dump-modules PROGRAM DIR` (`SNX_SCARLETNX` names another binary), cached per game folder in
  `~/.cache/scarletnx/module-dumps`. The dump's format is described in `src/nx/loader/loader.h`.
  `--log=2`: verbose; Windows `--profile=1`: `profile.txt` every 10 s.

The game's `[Debug]` settings ([USAGE.md](USAGE.md#settings--input)) are for investigating. Each is also given for one run as
`--key=value` on the game's command line:

- `--gpu_trace_target=0x5acf62e00[:bytes]`: logs (`gpu-trace-target:`, at most 64 lines of each kind) the clears
  and draws that write a render target starting in that GPU address range, with its size, vertex or index count and
  shader addresses. A color clear lists every bound target in the range. Write the address with `0x`; without a size
  the range is one byte. It finds what produced a wrong intermediate target.
- `--nvn_trace=1`: logs each NVN function the game resolves (`nvn: NAME -> ADDRESS`), the number of GPFIFO
  submissions (by any thread) during each queue call, and at exit the call count of every function used
  (`nvn-used:`). Calls by direct branch are counted only in a game recompiled with `--hook-direct-calls`.
  [NVN_HLE.md](NVN_HLE.md) has what the traces have shown.

## Shaders

```sh
"$games/Game/Game" --shader_corpus="$PWD/corpus"
build/snx-shaderc corpus --validate --out translated
```

`--validate` needs `spirv-val`. `--out` writes SPIR-V modules for build comparisons.

The instruction decoder is generated at build time. `src/nx/gpu/shader/maxwell_opcodes.def` lists each encoding
(opcode name, display name, 16-bit pattern), and `tools/shaderdec/shaderdec.c` (strict C99, built for the build
machine in a cross build, as swipc is) checks it and writes the opcode enum and a table-driven matcher into the
build folder, so nothing is set up at run time. Two patterns may overlap only when one is strictly narrower, and
that one is tried first. A new opcode is a row there (its `O_` name then exists for the translator), and
`ctest -R 'shaderdec|shader_decode'` checks the spec and decodes every row back to itself.
`[Debug] shader_corpus=` in the game's INI does the same for every run. Where the game gets no command line
(Android), its INI's `[Debug] diag=1` keeps each shader's entry in `captures/shaders` beside the game, for the same `snx-shaderc`.

## SVCs

`src/nx/hle/svc/hos.svc` declares the kernel's SVCs as Eden's SVC generator does, and `tools/svcgen/svcgen.c`
(strict C99, built for the build machine in a cross build, as swipc is) lays out each SVC's registers in the
64-bit and 32-bit ABIs from the declaration. It writes `hle/svc/hos.svc.inc` into the build folder: the handlers'
prototypes, a wrapper per SVC and ABI, and the tables by number that both kinds of thread dispatch through. A new
SVC is implemented by dropping its `stub` mark in `hos.svc` and defining `svc_<Name>` in `svc.c`, with the
declaration's arguments. `build/snx-svcgen --layout src/nx/hle/svc/hos.svc` prints the registers, and
`ctest -R svcgen_layout` checks them against `tests/svcgen_layout.txt`. The NX modules call an SVC through
`RtAPI.svcs`, so a change to the tables' size or signature is an `SNX_ABI` change that raises `SNX_ABI_OLDEST`.

## Provenance

Code ported from another project keeps its copyright lines and gets a row in [credits](CREDITS.md), with its
license text installed when the binaries carry it (`CMakeLists.txt`). Exclude dumps/keys/generated
modules/dependency builds from Git.
