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
- **You remove it**: a setting by hand or with the launcher's **Settings > App > Clean Old Settings**, which takes
  the `[Deprecated]` section out of every game's `.ini` the launcher reaches (the games' config folder, `games/` and
  the libraries of Settings > App > Folders); a file an older version left, by moving it where the line says.
- **It is listed below** until the code that reads it goes, which is a change of its own named in the table.

A game's `.ini` keeps its deprecated keys last, under what became of each:

```ini
[Deprecated]
; keys an older version used, which do nothing now (docs/DEPRECATIONS.md): delete them, or use Settings > App >
;   Clean Old Settings in the launcher
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
| `.ini` keys `modo_tv`, `idioma`, `resolucion`, `sombras`, `perfil` (the settings' first, Spanish names) | #185 | `docked`, `language`, `resolution`, `shadows`, `profile` | the same |
| `.ini` key `gamepad_buttons` (A B X Y by position, by label or the map) | Swap A/B in place of the A B X Y presets | `gamepad_swap_ab`; `gamepad_map` always applies | the same |
| `.ini` key `cpu_fallback` (the interpreter or the JIT for code the recompiler missed) | [recompiler] Replace the interpreter with the runtime JIT | nothing: the runtime JIT compiles that code | under `[Deprecated]`, named once a run |
| `--keep-c` | Replace Python pipeline with a C90 driver | `--keep-ir` | still taken, said on stderr each time |
| `--recomp` | [cli] the options' names | `--recompile` | still taken, said on stderr once a run |
| `--repo DIR` | [cli] the options' names | `--data DIR` | still taken, said on stderr once a run |
| `--debug`, `--skip --skip` | [cli] the options' names | `--no-setup` | still taken, said on stderr once a run |
| `-r`'s `-o`, `--out`, `--outname` NAME | [cli] the options' names | `--name NAME` | still taken, said on stderr once a run |
| `--interp-only` | [cli] the options' names | `--jit-only` | still taken, said on stderr once a run |
| An installed copy's files in `~/.local/share/scarletnx` on Linux: `gui/last-run.log`, `gui/recent.txt`, `games/<Game>.ini`, `games/<Game>.log`, `games/threads.txt`, `games/shader_cache/vk_pipelines.bin`, `work/` | #212 | the config, state and cache folders ([USAGE](USAGE.md#folders)) | the launcher lists their folders as it opens |
| A game laid out beside the others: `games/<Game>`, `<Game>_code.so`, `<Game>.abi`, `<Game>_data/`, `<Game>.ini`, `<Game>.log` | [driver, runtime, gui] Keep each game in a folder of its own | `games/<id>/`, its settings, log and caches in their folders ([USAGE](USAGE.md#folders)) | it runs where it is, each run naming its folder now; the launcher lists it as it opens |
| The menu's `gui/` in the data folder (`~/.local/share/scarletnx/gui/`, `%APPDATA%\scarletnx\gui\`) and in Linux's state folder | [runtime, driver, gui] Split config, state and cache off Linux too | `settings.ini`, `app.ini`, `folders.txt`, `gamecontrollerdb.txt` in the config folder; `recent.txt`, `last-run.log` in the state folder | the launcher lists the folder as it opens |
| A portable copy's `games/` beside the program, and outside Linux `work/` in the data folder | the same | `.scarletnx/` beside the program; `cache/work/` | the launcher lists each folder as it opens |
| Android: the menu's `settings.ini`, `app.ini`, `folders.txt`, `gamecontrollerdb.txt`, `recent.txt`, `last-run.log` in the app's own files, outside its data folder, where the Files app does not reach | [gui, runtime] Keep the menu's files in the data folder on Android | the data folder's `config/` and `state/` | the launcher lists each as it opens, with its size; nothing reads them. Out of the player's reach, the box says that while the app is in testing clearing its storage takes them (Settings > Apps > ScarletNX > Storage > Clear storage), with all else of the app's. The check goes in a change of its own once the app is out of testing |
| A build tree's checkout as its data folder: `games/`, `work/`, `prod.keys`, `title.keys`, `linux/`; `SNX_GAMES_DIR`'s default, the checkout's `games/` | #318 | the user's data folder, as an installed copy's ([USAGE](USAGE.md#folders)); `--repo DIR` keeps a run's files in DIR | the drivers on stderr each run and the launcher as it opens, each with where it goes; CMake's configure for `SNX_GAMES_DIR`, set back to empty |
| A game's Account saves in `<id>_data/save/user` (before there were other users) or, with none there, in `save/system` (the versions that read the save type from a byte that is always 0) | [filesystem] a folder of Account saves per user | `save/user.<uid>`, here the uid `0123456789abcdeffedcba9876543210`'s, user 1's without a users.ini that gives it another ([USAGE](USAGE.md#users)) | played where they are while that uid has no folder, the game's log and stderr naming both folders once a run; with `save/user` and the user's folder both there the game stops, for you to keep one |
| A theme's `[body] gap` and `[body] scroll` (the lists' spacing and scrolling) | [gui] a box of files' OK: a scene of ui.txt, the theme's last list gone | nothing: each list's rows are laid out by `src/gui/ui.txt` | the launcher as it loads the theme, the file named, in its message and Settings > App's note |
| A theme's `[head] place`, `[tips] place` and `[body] sizing` (where the title, the tips and the list's column go) | [gui] the screens' boxes from ui.txt's Screen and Wide | nothing: `src/gui/ui.txt` lays out each screen as both built-ins had them, the title after the tabs, the tips on a row under the head, the list two fifths of the screen | the launcher as it loads the theme, the file named, in its message and Settings > App's note |
| `text_size =` in `app.ini`'s `[app]`, Settings > App's **Text size** | [gui] Text size is UI scale, ui_scale in app.ini | `ui_scale =`, the same values; Settings > App's **UI scale** | the launcher as it opens: `app.ini` is written again with `ui_scale =`, said once |
| A line of `folders.txt` with a path alone (before the recompiled and the dumps libraries) | [gui] A folder of the player's is in the recompiled or the dumps library | `recompiled <path>` or `dumps <path>`: each such folder goes in the library of what it holds, in both for both, in the dumps one with neither | the launcher as it opens, naming each folder with its library; `folders.txt` is written again so, once |
| The data folder's `games/` of recompiled games, `[folders] games =` in `app.ini`, the CMake option `SNX_GAMES_DIR` | [gui] Recompiled games go to recompiled/, games/ still read | `recompiled/`, the dumps in `dumps/` beside it; `[folders] recompiled =`; `SNX_RECOMPILED_DIR` | the launcher lists `games/` as it opens and still lists and runs its games, the driver says so on stderr; `app.ini` is written again once, said; CMake says so as it configures |
| LLVM's SLP vectorizer, on in the recompile and the JIT (LLVM's own) | [recompiler] LLVM's SLP vectorizer off by default | `slp_vectorizer = 0` in `[Debug]`; Settings > Debug > SLP vectorizer turns it on, which the next recompile and the JIT's next run take: LLVM 23 can break a module with it and stop the recompile (llvm-project issue 231525) | the setup's notice, once |
| `ui_scale = auto`, `0.25`, `0.5` or `0.75` in `app.ini`'s `[app]` (`text_size =` before), Settings > App's UI scale **Auto** and its parts (the letters have whole sizes only) | [gui] Take out UI scale's Auto, an old one read as its number | `ui_scale =` 1 to 4: the number Auto gave on that screen, which a start without one takes | the launcher as it opens: `app.ini` is written again with the number, said once |
| `astc_bc = 0` and `memory_fit = 0` in a game's `.ini`: Off (on iOS `memory_fit` was 1, On) | [vk] astc_bc's Auto, [runtime] memory_fit's Auto | `0` is Auto, the default, as the other Auto settings: BC7 on a GPU that samples BCn but not ASTC with less than 8192 MB of budget for its images; the game told no more than the device can give. `1` On, `2` Off. Every game's `.ini` has the `0` written, so every game takes Auto | the setup's notice, once |

Settings > App > Move Old Files in the launcher, or `scarletnx --migrate=yes`, moves the rest of these, an entry at a
time, each to the folder its line names; `scarletnx --migrate` says first what would move ([USAGE](USAGE.md#folders)).

To move a game an older version laid out beside the others: run it from its dump once, which makes `games/<id>/`;
move its saves, `<Game>_data/save/` and `<Game>_data/sdmc/`, into `games/<id>/<id>_data/`, and its settings,
`<Game>.ini`, to `<id>.ini` where the table in [USAGE](USAGE.md#folders) says; then delete the older files. The
menu's files, and a checkout's `work/`, keys and `linux/`, move as they are, each to the folder the line names (on
Android the menu's files are out of reach: the line says how they go). A checkout's
`games/` does too, then each game's `<id>.ini` to where the table says (the game's run names it).

A `.ini` key the runtime does not know at all (a newer version's, or a typo) is not deprecated: it stays under
`[Other]`, as it was, since a newer version may read it.
