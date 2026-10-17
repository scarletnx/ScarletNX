<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Deprecations

What an older version used that this one replaced, how a run tells you, and how it goes away.

## The rule

- Something that changes meaning (a setting, an option, a path, a file's layout) gets a **new** name. The old name is
  never given a new meaning.
- The old one becomes **deprecated**. A deprecated setting is not read (the new one is), but it is kept where it is,
  never dropped in silence; a deprecated option still works.
- **Each run says so** where you read it: stderr for the drivers, the game's `.log` and stderr for the runtime, the
  launcher's box for files an older version left. The line names what replaced it.
- **You remove it**: a setting by hand or with the launcher's **Settings > App > Clean old settings**, which takes
  the `[Deprecated]` section out of every game's `.ini` the launcher reaches (the games' config folder, `games/` and
  the folders of Settings > App > Library); a file an older version left, by moving it where the line says.
- **It is listed below** until the code that reads it goes, which is a change of its own named in the table.

A game's `.ini` keeps its deprecated keys last, under what became of each:

```ini
[Deprecated]
; keys an older version used, which do nothing now (docs/DEPRECATIONS.md): delete them, or use Settings > App >
;   Clean old settings in the launcher
; renderer: Vulkan is the only renderer
renderer=opengl
```

## Now

| Old | Replaced by | Now | How it is told |
|---|---|---|---|
| `.ini` keys `renderer`, `dynamic_resolution`, `dynamic_resolution_min`, `game_dynamic_resolution`, `low_priority_shader_compiler`, `smooth_recording`, `recording_fps`, `lsfg`, `lsfg_dll`, `lsfg_target`, `lsfg_multiplier`, `lsfg_flow`, `lsfg_resolution`, `lossless_scaling`, `lossless_path` | Remove the OpenGL renderer | nothing: they went with it | under `[Deprecated]`, named once a run |
| `.ini` key `audio_device` | [gpu/audio] Fix viewport flips and audio playback | playback follows the system's default | the same |
| `.ini` key `forced_vsync` | Remove the OpenGL renderer | `vsync` | the same |
| `.ini` keys `touch_controls`, `touch_controls_opacity` | [runtime] Isolate shared core and NX runtime | `controls`, `controls_opacity` | the same |
| `.ini` keys `modo_tv`, `idioma`, `resolucion`, `sombras`, `perfil` (SwitchRecomp's) | #185 | `docked`, `language`, `resolution`, `shadows`, `profile` | the same |
| `.ini` key `gamepad_buttons` (A B X Y by position, by label or the map) | Swap A/B in place of the A B X Y presets | `gamepad_swap_ab`; `gamepad_map` always applies | the same |
| `.ini` key `cpu_fallback` (the interpreter or the JIT for code the recompiler missed) | [recompiler] Replace the interpreter with the runtime JIT | nothing: the runtime JIT compiles that code | under `[Deprecated]`, named once a run |
| `--keep-c` | Replace Python pipeline with a C90 driver | `--keep-ir` | still taken, said on stderr each time |
| An installed copy's files in `~/.local/share/scarletnx` on Linux: `gui/last-run.log`, `gui/recent.txt`, `games/<Game>.ini`, `games/<Game>.log`, `games/threads.txt`, `games/shader_cache/vk_pipelines.bin`, `work/` | #212 | the config, state and cache folders ([USAGE](USAGE.md#folders)) | the launcher lists their folders as it opens |
| A game laid out beside the others: `games/<Game>`, `<Game>_code.so`, `<Game>.abi`, `<Game>_data/`, `<Game>.ini`, `<Game>.log` | [driver, runtime, gui] Keep each game in a folder of its own | `games/<id>/`, its settings, log and caches in their folders ([USAGE](USAGE.md#folders)) | it runs where it is, each run naming its folder now; the launcher lists it as it opens |
| The menu's `gui/` in the data folder (`~/.local/share/scarletnx/gui/`, `%APPDATA%\scarletnx\gui\`) and in Linux's state folder | [runtime, driver, gui] Split config, state and cache off Linux too | `settings.ini`, `app.ini`, `folders.txt`, `gamecontrollerdb.txt` in the config folder; `recent.txt`, `last-run.log` in the state folder | the launcher lists the folder as it opens |
| A portable copy's `games/` beside the program, and outside Linux `work/` in the data folder | the same | `.scarletnx/` beside the program; `cache/work/` | the launcher lists each folder as it opens |
| A build tree's checkout as its data folder: `games/`, `work/`, `prod.keys`, `title.keys`, `linux/`; `SNX_GAMES_DIR`'s default, the checkout's `games/` | #318 | the user's data folder, as an installed copy's ([USAGE](USAGE.md#folders)); `--repo DIR` keeps a run's files in DIR | the drivers on stderr each run and the launcher as it opens, each with where it goes; CMake's configure for `SNX_GAMES_DIR`, set back to empty |

To move a game an older version laid out beside the others: run it from its dump once, which makes `games/<id>/`;
move its saves, `<Game>_data/save/` and `<Game>_data/sdmc/`, into `games/<id>/<id>_data/`, and its settings,
`<Game>.ini`, to `<id>.ini` where the table in [USAGE](USAGE.md#folders) says; then delete the older files. The
menu's files, and a checkout's `work/`, keys and `linux/`, move as they are, each to the folder the line names. A checkout's
`games/` does too, then each game's `<id>.ini` to where the table says (the game's run names it).

A `.ini` key the runtime does not know at all (a newer version's, or a typo) is not deprecated: it stays under
`[Other]`, as it was, since a newer version may read it.
