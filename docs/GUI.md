<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# GUI launcher

## Index

* [Setup](#setup)
* [Games: the games of recompiled/ and the player's folders](#games-the-games-of-recompiled-and-the-players-folders)
  * [The Compile tab](#the-compile-tab)
* [Output and resource use](#output-and-resource-use)
* [Themes](#themes)
  * [Theme formats](#theme-formats)
* [Languages](#languages)
* [Building and testing](#building-and-testing)

`scarletnx` started with no arguments (or a model's name, or the AppImage), with no game of its
name beside it, opens its launcher: a small SDL3 menu for the command-line drivers. A copy named
after a game, without its code, says so and never opens it. It uses an
embedded public-domain 8x16 VGA BIOS font, with GNU Unifont's 8x16 glyphs fitted to it for the Latin letters its CP437
lacks and its 16x16 ones, two columns each, for the wide letters a language shows (Chinese), and nearest-neighbor
integer scaling; its words are English or a language compiled into it ([Languages](#languages)). An item of a menu
has an icon beside its name, 16x16 in the font's stroke (`src/gui/icons.hex`): the menu's, the groups of the Settings and
their rows that open a screen or do something (the others keep its columns), a game's box's actions, the setup's pages
and the game's menu's. No font files or localization packages are needed.

```sh
./build/scarletnx
```

The window's title tells which build it is, as `ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)`, for example
`ScarletNX main 43bf71a (clang 22.1.8, Linux x86_64)`; the commit ends in `-dirty` when the tree had changes, a
build of a detached checkout outside CI says `detached`, and both read `unknown` outside a git checkout. A game's window carries the same line after the game's name.

It opens on a splash: Wimsy, or Fuddles (`dist/splash/`), picked at random each start, a third of the window's
shorter side on the theme's `[splash] page` for its `ms`; a key, a click or a finger ends it. The drawings are SVGs
built in, drawn at the window's size ([lunasvg](https://github.com/sammycage/lunasvg)).

- Up/Down and Left/Right move the yellow focus, wrapping at the ends.
- Tab and Shift+Tab go round the menu, Games and the Settings, the tabs at the top, and **Compile** while the
  queue of compiles holds one ([below](#the-compile-tab)); a click on a tab opens it. A theme edited
  and not saved keeps its screen.
- A list with rows out of sight (the menu with big letters, `recompiled/`'s pages, the Settings, the theme
  editor) shows an arrow at the right end of its first row, up, and of its last, down, in the soft colour.
- Moving the mouse over an item focuses it in yellow without activating it; in the Settings, a row of the list the
  keys move in (the groups, or the rows of the one open), the other list, a tab and Back waiting for a click.
- A gamepad drives the menu as the keys do (`src/gui/pad_menu.c`): the D-pad and the left stick are the arrows, the
  button labelled A (or Cross) is Enter, B (or Circle) ESC, X (or Square) Del and Y (or Triangle) R, wherever the pad
  has them (a Switch pad's A is east, its X north), LB and RB are Shift+Tab and Tab, LT and RT are PgUp and PgDn, L3
  and R3 are M and B (Games' mods and AOT pack), the right stick pushed any way is L (Games' Play, once until it is
  back), Start is F5 and Back (View, Minus, Select) F1. On Games, X opens
  the focused game's save states, as S does, and in that box it deletes one, as Del does. A held arrow or
  page repeats. A press on the pad hides the mouse's cursor and the foot names the pad's buttons, a PlayStation pad's
  by their shapes; a key or the mouse moving past a few pixels shows the keyboard's again, and so do a click and a
  finger once they have pressed what the foot showed under them: the press that switched does what it does. The Button
  map page and a Controller row waiting for a press read the pads themselves. While a run goes the menu lets go of the
  pads, for the game.
- The keys at the foot are buttons: a click or a tap on one presses it, as the keyboard does (ESC or
  Back goes back, Tab to the next screen, Enter or Tap selects; Drag says what the finger does). A key of
  words between slashes presses the word clicked, `Left` or `Right` of `Left/Right`; the text after it
  presses the key when its words all press one. `Arrows`, `mouse`, `pad` and `D-pad` press nothing.
- The menu, top to bottom: the three loads, **Install Keys**, **Settings**, **Share Log**,
  **Folders**, and **Exit**. The games, and the recent ones, are the Games tab's. A load whose driver
  is neither beside ScarletNX nor on PATH, as those an app leaves out ([APPLE](APPLE.md)), is greyed out, still there,
  and Enter or a tap on it says which driver it misses rather than opening its screen; the drivers are looked for
  again each second, and with F5.
- Space, Enter or a left click explicitly activates an item, marking it bright
  pink. Activation is tracked separately from hover/focus, so the activated item
  stays pink while another item is hovered in yellow. Mouse controls also work
  on Games' rows.
- A finger taps an item anywhere on its row, as a click does on its label. A drag up or down on any list whose rows
  do not all show, Games' grid too, moves its rows with the finger, the focus where it was, and let go fast they glide
  on, slowing to a stop; on the Settings, the side panel a list, the drag that passes onto it opens the group in
  focus there, its rows taking the focus, and back onto the groups they take it back, the panel showing the group in
  focus again. A tap on a finished run returns to the menu, and one on a
  running run's ESC line aborts it. Android's back button is ESC. On a screen packed with
  pixels (a phone) the text takes the display's own scale, as far as the menu still fits.
- **Load Switch NSP/XCI/NRO**, **Load Linux ARM ELF**, **Load 3DS game**, **Load Vita VPK** and
  **Load GBA ROM** open a screen as the keys': a click on its blinking **[ Pick or drop a file ]**, or Enter, opens the system's file
  picker (on Linux the desktop's, else zenity's). Dropping a file on the window and Ctrl+V with
  a path work too. ESC returns to the menu. On Android the picker opens at once. A dump is read where it lies
  and takes no room, unless *Extract dump* is on. Then one not extracted yet takes about its size, a Switch
  dump's extracted straight into its game's folder (as much again only with an **Extractions** folder picked, which
  keeps a copy, and in the temporary folder with *Extract to tmpfs*); when its disks have less free, the menu says how
  much it needs and *Run Anyway* takes a second press. With more than one folder of the recompiled library that can
  take a game (its Main, and each Extra that can be written and runs programs), a box asks which the dump's game goes
  in, **Recompile** on its tab: each folder with what it is and its free room, the Main first and in focus; ESC runs
  nothing. The one taken is the run's `--outdir`, and the room asked about is that folder's.
- **Load 3DS game** uses `scarletnx-3ds`. Its `games/` browser lists CIA, CCI, 3DS, CXI, APP, HBB, BCCI/BCXI/BCIA and ZCCI/ZCXI/ZCIA inputs,
  not host executables. The driver reads supported decrypted application CIAs, cartridge images (a card's
  update partition played as the update) and bare NCCHs where they lie, or extracts them with *Extract dump*.
  The driver then compiles the ARM11 code ahead of time and runs it, on Linux and
  Windows. Larger images use bounded conservative discovery (16 MiB text cap).
  SuperTux shows its title screen and plays its music, but takes no input yet.
  Full HLE/devices remain incomplete. Like the Switch driver, it uses an
  extraction (or the CIA itself) and a compiled module already there, and recompiles only when needed.
  See [3DS port](3DS.md) for limits.
- **Load Vita VPK** uses `scarletnx-vita`, with the menu's data folder as its `--data`. The driver extracts the VPK
  into its game's folder of `recompiled/` (named after its title ID), compiles its code ahead of time and plays it,
  compiling at run time what it missed. A VPK extracted and compiled before plays at once. See
  [Vita driver](VITA.md) for limits.
- **Load GBA ROM** uses `scarletnx-agb`, the AGB board runtime, with the menu's data folder as its `--data`: it makes
  the game's folder of `recompiled/` (named after the ROM's file), its module and its copy of the runtime, and plays
  it in a window. See [AGB board](AGB.md).
- **Install Keys**, immediately after the five load entries, blinks red (the theme's `failure` colour, at its
  `blink_ms`) while no `prod.keys` is in use. It prompts you to
  pick `prod.keys` (a click on the blinking **[ Pick or drop prod.keys ]** in the middle of
  the screen, the theme's `[prompt]`, or Enter), drop it, or paste its path with Ctrl+V. It installs the file to `~/.switch/prod.keys`, or
  `%USERPROFILE%\.switch\prod.keys` on Windows; on Linux, in the user's data folder, it puts it there
  instead (`~/.local/share/scarletnx/prod.keys`), which the driver reads before
  `~/.switch`, and names the keys of `~/.switch` it would use until then. It creates the
  folder if needed. Existing keys trigger the bright-red warning
  **Keys already setup. This will overwrite the previous ones**. The screen then says what the try
  did, *Installed* or *Not installed* and why, and goes back to the menu by itself after 3 seconds
  (10 after a failure, while another file can still be picked); ESC cancels without changing them.
- **Settings** opens on its groups, Enter opening one and ESC coming back: **App**, then
  **Launch** (the launcher's own and the recompiler's, from `src/nx/runtime/settings.txt`), then a group per section of the game's `.ini` (Display,
  Graphics, Performance, Overlay, Controllers, Network, Debug; Overlay is the runtime's On-screen controls, the
  name its page in the game and `settings.ini` keep), as `scarletnx --settings`
  prints them from the runtime's own tables: read from them in the same process where the menu is built into
  scarletnx (iOS too), so every section shows at once; a menu built alone asks `scarletnx --settings`. The game's menu's settings have its names and words
  (*Scale* 100%, *Touch screen* Fingers), and its **Buttons** page, whose **All** steps every row;
  *Edit layout* and *Reset layout* stay in the game's menu, as the layout is each game's own.
  Controllers' **Button map** draws the pad as the game's menu does: each control in a box down either
  side with the console button it sends (where the console's is, unless the player's map says otherwise, A and B
  swapped when the group's **Swap A/B** is on), and what the focus does at the bottom. Up/Down move, Left/Right
  change a control's button and Enter (or the pad's A) waits for a press on the pad, which takes it; a change keeps
  the whole map as `gamepad_map`, and Del puts it back to the `.ini`'s. The first pad plugged in is shown, on a
  player's map that player's, as the games give the pads their players (each in the order it came takes the player
  whose *Controller* names it, else the first naming none), its held buttons lit; with none, the middle says why (a joystick SDL has no mapping for is named, with its GUID and the
  file its line goes in). The launcher reads pads only on this page.
  Controllers' **Player** row, first, goes round *All* and 1 to 8, as Eden's tabs (Left/Right or Enter): *All*
  shows the settings every player shares; a player its own, *Controller*, *Connected*, *Type*, *Swap A/B*,
  *Vibration* and *Button map*, each *As All* until picked, then the rows no player has its own of (the
  dead zone, background input, Joy-Cons). A player's *Controller* is *Any*, the next to connect, until Enter on it
  and a press on a controller give it that one (ESC leaves it as it was, Del makes it *Any* again); its *Button
  map* is the page above on its own keys, its A and B as its *Swap A/B* says.
  **App** holds **Theme** and **Language**, which Left/Right or Enter go round either way, **UI scale**, a dialog
  (below), **Animations** (On by default, `animations = 1` in `app.ini`: a screen comes in from the
  page's colour and a box's shade comes and goes, 0.15 s each; Off shows each at once), **Side panel** (below, F2 too),
  **Background recompile** (*Full*, `recompile_background = full` in `app.ini`: the queue's recompiles take every
  core, or the AOT threads picked; *Gentle*, `gentle`: half of them, at a low priority, ten steps of nice under the
  launcher's, on Windows the class under the launcher's, for using the computer meanwhile;
  [the Compile tab](#the-compile-tab)),
**Theme Editor**, which comes
  back to App, and **Folders**, every folder on one screen, the whole of it, a box a group:
  - **App Folders**: **Data** (`recompiled/`, `dumps/`, `themes/` and `prod.keys`), **Settings**, shown only
    (`app.ini`, which names the others, and each game's `.ini`: `XDG_CONFIG_HOME` moves it, a portable copy keeps its
    own), **Logs** (the games' logs and `threads.txt`, `recent.txt`, `last-run.log`, `last-recompile.log` and the
    launcher's `scarletnx-console-<date>.log` and `scarletnx-error.log`), **Caches** (the recompile's
    `units/`, the shader caches, the dumps' icons, and Extractions by default) and **Extractions**;
  - **Dumps Library**: its **Main**, the data folder's `dumps/` (with its subfolders, going with the data folder), then its
    **Extra** folders, whose dumps Games lists, and **Add a folder**. Main's box, or Del on it, has **Remove from
    Library**, which asks first: Games no longer lists it and ScarletNX no longer makes it as it starts, its files kept;
    its row then says `not listed`, greyed, and **Add to Library** puts it back;
  - **Recompiled Library**: its **Main**, `recompiled/`, where recompiled games go, then its **Extra** folders, whose games
    ready to run Games lists too, and **Add a folder**. Main's box has **Remove from Library** too, asked first: its
    games stay where they are, unlisted, and a new game of any load goes to an Extra, asked among several, a run
    refused with none; **Add to Library** puts it back;
  - **Reset to Defaults**, last, which asks first.

  A row with nothing to do now is greyed, still there, and Enter on it says why: Clean Old Settings with nothing to
  clean, Move Old Files with nothing left, App's Reset to Defaults with every setting at its default, Folders' with
  every folder its default.

  A row is its name, its folder (one too long for the row cut in its middle: its root and first folder, `...`, as
  many of its last folders as fit; the whole path under the boxes) and what it is. Data, Logs, Caches, Extractions and
  the recompiled Main are each their default or a folder picked: Enter opens the folder picker, a folder dropped or
  pasted with Ctrl+V takes the row, Del puts the default back (asking first with the folders it changes) and the last
  row all five. Logs and Caches reach the drivers and the games through `app.ini`, so when it cannot be written they
  stay as they were, said in a box; the row says *default*, *custom* or why its folder cannot be, at its end, the tips what it holds and its disk's free
  space. A folder not there, not a folder, that cannot be written or, for the games, on a disk that runs no programs
  (`noexec`) is refused, and a run whose picked folder no longer can be stops, naming it: the default never takes its
  place. On Android a folder outside the app's own storage is asked about first, since Android checks every file
  access there and games may run slower: *Use it* takes a second tap, and its row says *outside the app*. A library's
  **Add a folder** opens the folder picker, and a folder dropped on, or pasted on, a row of a library is added to it; one
  with folders in it asks first: *Only This Folder*, or *With Subfolders*, whose games are listed too, four levels
  down, hidden folders left alone (its row says *+ subfolders*). Del or Enter asks before an Extra is taken off its
  library (the folder keeps its games). A folder added from Games goes in the library of what it holds, in both for
  both, the dumps one with no games of either. The Extras are kept in `folders.txt` beside `settings.ini`, a line each,
  the library's word, a space and the path (`recompiled /mnt/games`, `dumps /mnt/dumps/**`), `/**` after one kept with
  its subfolders; one that cannot be read is named when the games are listed. Under App's rows, the three folders in
  use: the data folder, the games' and the extractions'.
  Nothing is moved, the folder before keeps what it has. The menu's own files stay where they are. **Clean Old Settings** asks first, with
  how many it would take out, then takes out the settings nothing reads any more: the `[Deprecated]` section the runtime
  gives a game's `.ini` for the keys it no longer uses (in the config folder's `recompiled/`, in `recompiled/`
  and in the player's folders), the keys of `settings.ini` that no setting of the runtime's takes
  (only while the runtime lists its settings) and those of `app.ini` the menu does not know. A box
  over the screen then lists each file and what was done to it, or why not, Up/Down and the wheel going over those
  that do not fit, until another key closes it;
  Share Log lists them too. **Move Old Files** (its value how many folders an older version left) asks first, with
  how many entries would move and how many stay, then moves what *Left by older versions* lists an entry at a time
  (`scarletnx --migrate=yes` without the menu, [USAGE](USAGE.md#folders)), the same box saying what moved and why the
  rest did not: one already there, one on another disk, a game laid out beside others, which moves from its dump.
  **Reset to Defaults**, the last row, puts every setting of the groups after App back to its default, asking first with how
  many change. A group of settings shows
  a row each, its help above, Left/Right going round its values, Enter typing one. A value typed (a number, a text, a
  nickname, a theme's key, a color's `#RRGGBB`) is typed in a dialog, as UI scale's, never in the row: the row's label
  its title, the text with its cursor in a field, what the row takes under it with its numbers (`A number from 0.25 to
  16, two decimals at most; empty, the default 1`), then OK and Cancel. Enter or A is OK, ESC or B Cancel, Backspace
  takes a letter out; a click off the dialog is Cancel. A phone's keyboard comes up with it, its number pad for a whole
  number never under 0 (SDL's pad has no point and no minus). Nothing changes before OK, and OK takes nothing the
  row does not take whole, a number past its range too: the dialog stays, saying the value typed and what holds
  (`20 is not a number from 0.25 to 16, two decimals at most`, `Ab is not a nickname: ...`). A setting not picked shows
  the runtime's default and `[default]` (*Off [default]*):
  the run takes the game's `.ini`'s, which starts as that; one picked goes to every
  Switch run as `--key=value`, over the `.ini` ([USAGE](USAGE.md)), and is kept for every game:
  what was changed, by the `.ini`'s sections and keys, in `settings.ini` of the config folder on
  Linux (`~/.config/scarletnx/`), else the data folder's `config/`. A run takes a setting from what that
  run picks, then from the game's own picks (to come), then from these, and last from the game's
  `.ini`. Del puts a setting back to its default, and App's **Reset to Defaults** every one. The launcher's own choices start picked: `fullscreen` 0, `controls` 0 (on Android 1,
  and `fullscreen` puts the launcher itself on the whole screen too).
  **gdbstub** passes `-S` and `-s`, the
  runtime's stub on port 1234 waiting for the debugger at startup (UNIX only), and **Extract to tmpfs**
  passes `--tmpfs`: a dump's extraction goes to `/tmp/scarletnx/work/<title ID>` (`$TMPDIR` when set, the
  temporary folder on Windows) instead of the game's folder, and the game reads its RomFS from there, its ExeFS and
  saves staying in its folder. Off, the default, it is extracted to disk. The room a run asks about is then the temporary
  folder's, without the second copy. Either way the game's `.ini` records the dump as `source=`, which the RomFS is
  extracted again from when tmpfs was emptied (a reboot), or moved by when the setting changes. A game that records
  none (made before, without the `<name>.source` link older versions made) says it can't be extracted to tmpfs and
  runs from what it has.
  **Extract dump** passes `--extract-container`, to the Switch and the 3DS driver both. Off, the default,
  nothing of a dump is extracted: the game's `.ini` records the dump, the recompile reads the ExeFS from it, and the
  game reads its ExeFS and RomFS out of the dump on the fly, decrypting them as it reads ([USAGE](USAGE.md)). A CIA
  is read the same way. On, a dump is extracted as before, into the game's folder or with *Extract to tmpfs* into
  tmpfs, which implies it. Turning it off again takes a game's extraction away on its next run.
  **Optimization level** (0, 1, 2, 3 or z, 2 by default) is LLVM's level for the game's code, **AOT threads** the
  threads compiling it (0, the default, one for each core), **Unit cache limit** the GB the compiled code units kept
  for the next recompile may hold (20 by default, 0 for no limit; [USAGE](USAGE.md)), and the **LLVM Internals** page
  turns each pass of LLVM's pipelines off and sets the options LLVM's C API takes, its **All** stepping every row. They
  are passed as `--key=value` when the game is recompiled: by **R**, or by a run that has to (a dump played the first
  time, an NRO each time). A game made already keeps its code until then. The first two settings and these reach
  neither the Linux nor the 3DS driver.
- The Status box, after the RAM and the CPU, says what ScarletNX's folders take (recompiled/,
  work/ and themes/ of the data folder, and on Linux its config, state and cache folders, else
  the menu's own) and what is left on that disk, counted as the menu opens and after each run. While a recompile of
  the queue runs its tree's RAM and CPU are in the first line, and a last one says how it goes:
  `Background recompile: Full`, `Gentle`, `Gentle, a game plays` or `Gentle until it ends:` and why, the threads it
  was started with when they were halved, and when the taskbar cannot show its progress, SDL's reason.
- Settings > App's **Theme** switches to the next theme, or the one before: the built-in Default
  and YoRHa, then each folder of `themes/` (see [Themes](#themes)). The menu opens in YoRHa until one is picked.
  It is kept for the next start,
  as are UI scale and Language, in `app.ini` beside `settings.ini`: `[app]` with `theme` (a theme's folder
  name), `ui_scale` (1 to 4; a start without it takes twice the display's
  scale, as many as fit 16 rows (30 on a phone), and writes it; `text_size` before, still read and written again as
  `ui_scale`, said once; `auto`, `0.25`, `0.5` and `0.75` of older versions read as that number and written again so,
  said once), `recompile_background` (`full`, the default, or `gentle`; a word it does not know named
  with the one kept, `recompile_background = slow (it keeps full)`), `dumps_subfolders` (`on`, the default: dumps/'s
  subfolders' dumps listed too; `off`: its own alone; Folders' row of it switches it), `dumps_main` (`on`, the default:
  dumps/ listed and made as ScarletNX starts; `off`: neither, Remove from Library's), `recompiled_main` (`on`, the
  default; `off`: recompiled/ not listed, new games going to an Extra) and `language` (`auto`, `en` or a
  language's BCP 47
  tag, as `pt-BR` or `zh-Hans-CN`), and once one
  is picked `[folders]` with Folders' `data`, `recompiled`, `work`, `state` (Logs) and `cache` (Caches) (a full path,
  nothing for the default). A value the
  menu does not know, or a theme no longer there, is named in the menu's message as it opens and
  left out; a file that cannot be written is named in App's note. A language in another form of its tag (`pt_BR`,
  `zh_CN`, which builds before the tags wrote) is read as that language and written as its tag, the message saying
  the two once (`language = pt_BR is now pt-BR`).
- Settings > App's **Theme Editor** edits the current theme: **Save** first, the folder it is saved in, a row
  per `theme.ini` key, a colour's with the colour after its value, and beside them a
  preview of the menu or a run (Ctrl+P, or a click on it, switches). Left/Right change a word, a switch or a
  number, Enter types a text or a number and opens a colour's picker. The picker sits over the keys, so the preview
  shows the colour as it changes: a bar per channel, R, G and B, which Left/Right step by 1 and PgUp/PgDn by 16, or
  a click, a drag or a finger sets, then the colour as `#RRGGBB`, which Enter there types. Enter keeps the colour, as
  does a click or a tap off the picker; ESC (Back on Android) puts back the one it had.
  Save (or Ctrl+S) writes `themes/<folder>/theme.ini` in the data folder and switches to it;
  a built-in is saved as a new theme, `mytheme` until renamed. ESC leaves, asking first if
  there are unsaved changes.
- Settings > App's **UI scale** row shows the size in use, 1x to 4x, the letters' whole sizes. Enter, a click or a
  tap on it opens its dialog, and so do Left/Right, a step that way: a box in the middle of the screen whatever the
  theme's dialogs, as the View box, its title on its head, then a slider, a tick a size, as wide whatever the size,
  the size picked named after it, and under them OK and Cancel. A size the window is too short for (16 rows of its
  letters, 256 pixels high a scale: 3x and 4x in one 720 high) is greyed, its tick and its name: the slider still
  goes over it, saying why, `4x needs a window 1024 pixels high: this one is 720`, and OK, greyed too, takes nothing
  there. A size kept that the window cannot hold stays kept, shown at the largest that fits, the row saying
  `4x (2x in this window)`. Left/Right, a click or a tap
  on the bar (a drag along it following) pick a size, and nothing else changes until OK; Cancel or ESC close it as it
  was. OK uses the size picked, and the dialog asks there to keep it,
  Back in focus: Keep keeps it in `app.ini`; Back, ESC or no answer in 15 s bring the one before it back. Bigger
  letters scroll the menu. The foot's letters (its keys, and the tips when they are
  there) are a size under the rest's, 1x at the least.
- Settings > App's **Language** goes round Auto, English and each language compiled in, by its own name. Auto,
  the default, takes the first of the system's languages (SDL's preferred locales, as `pt_BR`, `zh-Hant_TW`) the
  menu has: that language for that country, else the language with no country of its own, else the language for
  another country (Portuguese of Portugal reads `pt-BR` when it is the only Portuguese), else English; the row says
  which it took. A language written in two scripts goes by its script, written or implied by its country (`zh-CN`,
  `zh-SG` simplified, `zh-TW`, `zh-HK`, `zh-MO` traditional): a system in one never takes a catalog of the other. The
  words change at once; a phrase the language has not translated stays English.
- **Share Log** asks how. **Share Zip** saves `scarletnx-logs-2026-10-28_14-05-33.zip`. Its `summary.txt` holds the message on the screen,
  the newest game log's summary, the last run's output, the queue's recompiles' (`last-recompile.log`), the launcher's
  own log and its fatal errors (below) and the game log. Beside it, whole, the launcher's logs (`launcher/`) and the
  games' logs of the last 14 days, each game's runs under `games/<its name> [<its folder>]/` by when each was last
  written; in every file a home folder is written `~`. **Share Log** saves that summary alone as `scarletnx.log`. Either
  goes in `share/` of Folders' Logs (`~/.local/state/scarletnx/share/`; on iOS Documents' `logs/share/`, which the Files
  app shows), emptied of what was shared before, and that folder opens in the system's file manager, to share from
  there. **Open Logs Folder** opens Folders' Logs, where they all are. After a run or a recompile of the queue that
  ended abnormally (not by ESC) it blinks red, as Install Keys does, until its logs are saved or a run ends well.
- **Folders** opens Settings > App > Folders (ESC comes back to the menu). There Enter, or a tap, on a folder gives
  **Open Folder**, which opens it in the system's file manager (on Android its Files app, the data folder through the
  app's own storage, [ANDROID](ANDROID.md)), **Change Folder** and **Reset to Default**; on an Extra of a library
  Open Folder and **Remove from Library**; on dumps/ and Settings, Open Folder. A row of the Dumps Library, dumps/ or an
  Extra, also gives **Subfolders**, `(*)` while its subfolders' dumps are listed and `( )` while not: Enter switches
  it, the box staying, kept at once (dumps/ in `app.ini`, an Extra in `folders.txt`). Del still puts a folder back to
  its default, or takes an Extra off.
- The launcher's log, a file a day, `scarletnx-console-2026-10-28.log` in its state folder (Settings > App > Folders'
  Logs; on iOS in `logs/` of the app's Documents, [APPLE](APPLE.md)), the 14 newest days kept (a day's first line
  deletes the older ones; the `scarletnx-console.log` older versions wrote stays as it is), holds what it says: its
  start (its build and data folder), each failure
  once in a session (what the box over the screen says, shown or not), the warnings it gives as it opens, a dump whose
  icon the driver did not give with what the driver said. Each line starts with the time and the launcher's process
  ID, `2026-10-25 11:30:02 [4242] `, so two launchers open at once write theirs apart, and the lines go to stderr
  too. A fatal error, the launcher's that stops it as it starts or a game's, is also kept in `scarletnx-error.log`
  beside it, a game's after its name (a game of no data folder, an `--outdir`'s, beside its own log). Each start adds
  to them. Launchers open at once on one Logs folder each keep their runs' output apart: the first `last-run.log` and
  `last-recompile.log`, the next `last-run-2.log` and `last-recompile-2.log`, and on to 8, each held while its
  launcher is open (a lock on the file, which leaves it to be read); the log says which a launcher took, and Share Log
  sends its own.
- **Exit** closes the launcher, once asked (ESC on the menu asks too, and while a recompile of the queue runs or waits
  the question names it), a screen saying for half a second which of its
  files it keeps for the next start (Settings, the picks of Settings > App, the libraries' folders, the recent games), a second
  close skipping it. Closing its window during a run asks first: Exit terminates the entire owned process tree (a
  recompile's work lost), so let the game exit normally to preserve its saves.
- As it opens, the menu shows in a box what an older version left where this one no longer looks, a
  folder at a time: where it is (`- `, in the failure colour, a path too long for a row going on in the next) and
  under it the space it takes, counted as the box shows (*counting* until then). They are the menu's `gui/`, `work/`, each game laid out
  beside the others in `games/` or a library's Extra (`games/<id>/` now), the data folder's `games/` (`recompiled/` now,
  its games still listed, each row marked `(games/)`, until they are moved), a portable copy's `games/`
  beside the program, and a build tree's `games/`, `work/` and keys in its checkout
  ([USAGE](USAGE.md#folders)). Up/Down, PgUp/PgDn and the wheel go over those that do not fit (an arrow at the
  right of the first says some are above it, *and N more* the ones under), any other key, click or tap closes it;
  nothing is moved, and it shows again at the next start for as long as they are there.
  **Share Log** lists them all, each with where it goes now. On Android the menu's own files an older version kept
  in the app's own files, out of the Files app's reach, are listed a file at a time, and under them the box says how
  they go while the app is in testing: Android's Settings > Apps > ScarletNX > Storage > Clear storage, which takes
  every game, save and key in the app with them. The menu never reads them: it starts from its defaults.
- **Recent Games**, at the top of Games, lists the last games run, newest first, three and a row that
  shows the rest, by the name their run gave (the runner's `ScarletNX - <name>` line), else their file's. A dump not
  made into a game yet stays in the list but has no row, as opening it would compile it; once made, by any run, its
  game takes its place.
  The focus moves between them and the games in one ring, and the tips show the focused
  game's path. Choosing one runs it again with the driver it was
  loaded with and the options the menu has now; a game whose file is gone leaves the
  list instead. The list is kept in `recent.txt`, beside `last-run.log`, at the root of the menu's state
  folder ([USAGE](USAGE.md#folders)): `$XDG_STATE_HOME/scarletnx/` on Linux, by default
  `~/.local/state/scarletnx/`, `%APPDATA%\scarletnx\state\` on Windows, or the data folder's `state/` on Android. An
  older version's, in a `gui/` folder, stay where they are. A line per
  game: `switch`, `linux` or `3ds`, `file` or `games` (a program from recompiled/, run as it is: the token keeps its old name),
  the path and the name, separated by tabs. A list that cannot be written there is named in the
  message, after a run's end too, until it can be.

The launcher runs Switch inputs with the `scarletnx` it is part of, and looks beside it, then on
PATH, for `scarletnx-gnuabi` or `scarletnx-3ds`. Files are passed directly
as argv entries, never through a shell. Spaces, Unicode paths and shell punctuation
are not evaluated. Local `file://` text drops are decoded, but lists and remote URI
authorities are rejected. The working directory is not changed and is shown in the
Status box. See [GNUABI](GNUABI.md) for supported Linux ELF applications.

## Setup

The first start opens on the setup, not the menu, a page at a time, each a tab at the top under its number: the
notices of what older versions did that this one does otherwise (only said, nothing moves:
[DEPRECATIONS](DEPRECATIONS.md)), then **Appearance**, **Install**, **Folders**, **Keys** and **User**
([USAGE](USAGE.md#setup) has what each asks, the terminal's setup too). Its boxes say what the page is about; its
question and choices come as the theme's `[body] options-dialog` places a question ([Themes](#themes)), over the boxes
without darkening them; its tip, and what a choice did or why it did not, are in the foot. Up/Down (or the mouse)
move between the choices, Enter takes one and ESC goes back a page, on the first page asking whether to exit. The
notices come one a page, numbered (Changed 1/8), each read before the setup goes on: Next the next one, Back (from the
second) the one before, and OK on the last. The first page has **Skip** after its choices: it takes every page left as
its defaults have it, what Next, OK or Enter on each would take (the notices read, the appearance and the install as
they are, Keep, Later, the nickname `Player`), writes `setup.ini` and opens the menu; a page whose default fails (a
`users.ini` that cannot be written) stays, saying why.

- **Appearance**: a box of Settings > App's Language, Theme, UI scale and Side panel, and Games' View and Icon size
  as its View box has them (Icon size greyed in the list), a row each, and Next (with Skip, on the first page) under
  them. Up/Down go over the rows and Next; Left/Right (or a click, on the left arrow back) change the language among
  the menu's, the theme and the others, at once, the screen with them; UI scale opens App's dialog, the size taken
  asked about for 15 s as App's is; Enter does as App's (the language or the theme a step on, UI scale's dialog), and on
  Next goes on. What was changed is kept in `app.ini` once the setup is done, in the folders Install picks; Auto
  stays Auto while the language is the system's.
- **Install**: Installed, or Portable, which makes `.scarletnx/` beside the program and moves the menu's own files
  there from then on. A box over the two says which the copy is now: `Now: portable, its files in <folder>/.scarletnx`
  while that folder is there, the focus then on Portable, else `Now: installed, its files in` the user's data folder.
  Installed is refused while a `.scarletnx/` is there, which keeps the copy portable. An app (Android, iOS) has no such
  page.
- **Folders**: Keep, Change recompiled folder (Settings > App > Folders, the recompiled library's Main) or Add a folder
  (an Extra of Settings > App > Folders), each through the folder picker, the page listing what is there now: Recompiled, Dumps
  (the data folder's `dumps/`, with its subfolders), then the player's folders.
- **Keys**: `prod.keys` and `title.keys` where the driver reads them; Use these, Choose folder (a folder with
  `prod.keys`, and `title.keys` if it has one, installed where Install Keys installs them) or Later.
- **User**: with `users/users.ini` there, its users with their pictures and uids, then Keep or Overwrite; without one,
  or to overwrite it, the nickname typed (`Player` to start with; Backspace takes a letter out) and Enter. A nickname
  the rooms' rule refuses is said, and the page stays.

Once the last page is done the menu opens, `setup.ini` written. After an update that brings a notice, the setup
shows that notice alone (1/1), with OK, before the menu. `scarletnx --no-setup` opens the menu with no
setup, writing nothing.

## Games: the games of recompiled/ and the player's folders

**Games**, the tab, lists the games: the ones ready to run (a recompiled game runs directly), the dumps to prepare
on Android, CIA files, which go to the 3DS driver, VPK files, which go to the Vita driver, and GBA ROMs, which go to the
GBA's. It reads the `recompiled/` of the
folder the driver works in, which the menu passes it as `--data`: a portable copy's `.scarletnx/` beside the
AppImage (or the GUI), else the user's data folder, a build tree's menu's too ([USAGE](USAGE.md#folders)), then each
Extra of Settings > App > Folders. Each game is shown by the name in its `control.nacp`, which the recompile copies into
its data, else by its folder's or file's name, sorted by it in each folder, with its icon (Wimsy in grey for one with
none): a game's own, a dump's
the one its driver gives (`scarletnx -x --icon`), asked once a session into Caches' `icons/`. A dump the driver gave
none for says why after its kind (the side box, the tips), and the launcher's log has all the driver said (above); the
next start asks again. Only immediate files are listed:
executables (execute permission on UNIX, `.exe` on Windows), `.cia`, `.cci`, `.3ds`, `.cxi`, `.app`, `.hbb`, `.bcci`, `.bcxi`, `.bcia`, `.zcci`, `.zcxi` and `.zcia` files
(case-insensitive, no execute permission needed), which run with the 3DS driver, and `.vpk` files, which run with the Vita driver. Hidden files, shared libraries and
directories are excluded. The folders are looked at once a second while the menu is open (their
names only), so a game put in or taken out shows in the open list; a folder that
cannot be read is named and the others still listed.

- **P** (on a phone a tap on *Folders* at the foot) opens Settings > App > Folders at the recompiled library's Add a
  folder, where ESC comes back to Games and its list read again. The recent games are over the games. ESC returns to
  the menu.
- **V** (on a phone a tap on *View* at the foot) opens the View box over the list, an option a row, its value between
  the arrows: **Recent Games** (*On*, `recent_games = on` in `app.ini`: the recent games over the list; *Off*: none),
  **Dumps** (*On*, `games_dumps = on`: the dumps of `dumps/` and of the dumps library's folders under their
  heading; *Off*: the games ready to run alone, a CIA or a VPK left out with the dumps) and **Sort** (*Title*,
  `games_sort = title`: by the name shown, in each folder; *Last opened*, `games_sort = last_opened`: the game whose
  log a run wrote last first, as every run writes its game's log again, then the games never run in their order,
  each group on its own, its dumps, CIAs and VPKs in theirs) and **View** (*List*, `games_view = list`: a row a game,
  all on one page that scrolls; *Grid*, `games_view = grid`: a tile a game, its icon over its name, as many a line as fit, on one page
  that scrolls, the list three fifths of the screen as a run's output is, Recent Games and a group's heading each on
  a line of its own; the arrows go to the tile beside, above or under the focus, its column or the line's last,
  PgUp and PgDn by the lines that show, a click or a tap on a tile opens it, and the side box says the focused one as
  the list's does); **Icon size**, the grid's, greyed in the list and saying so there (*Medium*, `games_icons = medium`: a tile 12 cells by 4 rows;
  *Small*, `small`: 8 by 3; *Large*, `large`: 16 by 6), then the **Side panel** of Settings > App (`side_box`, which
  F2 turns too). Enter, Left, Right or
  a click change the focused one,
  kept in `app.ini` at once, and the list is read again by it; ESC or a click off the box closes it. On iOS Recent
  Games and Dumps start *Off*, as a phone keeps the games made in the app's `recompiled/` and prepares a dump from the
  Menu; the player turns them on there. An `app.ini` without them, an older version's, has these defaults, and a word
  it does not know is named in the message with the one kept: `recent_games = maybe (it keeps on)`.
- In the list, Up/Down go round the recent games and the page's files. Enter/Space opens one. A game no game of the
  recompiled library was made from yet (a dump, a package, a ROM) compiles first, through the queue of compiles R's
  goes through: the Compile tab shows it, and it plays once its compile ended if the tab still shows it; sent away
  first (Tab), the tips say it is ready, and Enter then plays it. One a game was made from already, in `recompiled/` or
  a recompiled folder of the Library, asks first, naming the game and its folder: **Play**, in focus, plays it,
  **Recompile** recompiles it where it is, as R does, and Back does neither.
- **L** (on Android a tap on *Play* at the foot, on a pad the right stick) plays the game in focus, recent or on the
  page, as Enter does, but a game with updates, DLCs, mods or cheats beside it plays as its box's **Play** does, with
  the ones the box keeps for it (its `.ini`'s `addons=`, `mods_off=` and `cheats_off=`), without showing the box or
  writing them. The queue of compiles and the question of a game made already come as they do for Enter. On a heading,
  or the row that shows every recent game, it says there is no game to play.
- **R** (on Android a tap on *Recompile* at the foot) recompiles the game in focus, recent or on the page, whichever
  console's, where it is, or makes its dump or package again: its driver's `-r` (`scarletnx -r`, `scarletnx-3ds -r`,
  `scarletnx-vita -r`, `scarletnx-agb -r`). It plays nothing, and goes in the queue of compiles, which runs them one
  at a time behind the screens: with none running it starts at once, the Compile tab showing it; else the tips say
  where it waits. A game of the queue says it on its row (`42%`, `next`, `3rd in queue`) and in the side box; Enter
  on it, or R again, shows it on the Compile tab, and it plays once its compile ended. On a Linux
  program, or another row, R says why it does nothing. **B** builds the focused game's AOT pack from its JIT cache
  ([AOT packs](AOT_PACKS.md)) through the same queue.
- **M** (on Android a tap on *Mods* at the foot) opens the box of the recompiled Switch game in focus, recent or on
  the page, that Enter opens for a game with updates, DLCs, mods or cheats beside it, and for a game with none of them
  too, whose **Add Mod** and **Add Cheats** copy a folder or a `.txt` the system's picker gives into the game's mods
  and cheats, **Open Folder** lists the game's folders to open (its dump's, its own, saves, settings, logs, caches; one
  not there greyed), **Save States** opens the box of its save states that S opens (greyed for a dump, which has
  none; on a phone the way to them, with a tap on *Save states* at the foot) and **Clean Cache** deletes its shader
  and code caches, asked first ([USAGE](USAGE.md#mods)). On
  another row, a dump, a package or another console's game, it says what it opens. The side panel of a recompiled
  game says how its code was made (`Made with: O2, AOT 81%`, `?` for one an older version made), or, in the failure
  colour, `Needs Recompile` when Play compiles it first.
- The list is one page that scrolls, by pixels: past its height a scroll bar at its right, as every list has (below),
  which a click and a finger take. Up/Down move the focus
  and the rows glide to show it (at once with Animations off); Left/Right and PgUp/PgDown move it a screen of rows,
  stopping at the first and the last; a notch of the wheel glides the rows three rows, the focus staying. The count
  of the games is shown beside them.
- Empty or missing directories show a diagnostic, and the side box says P adds a folder.

A recompiled game, in a folder of its own (`recompiled/<id>/<id>`), runs with the Settings through the menu's own
`scarletnx` and is never extracted as a dump: it plays as it is when this build made it with the mods it plays, and
when another build did, or other mods, it compiles first, where it is, in the queue as a dump's first compile does
(its `<id>.abi` says which: [USAGE](USAGE.md#extract--aot--run)), then plays if the Compile tab still shows it. A game
an older version laid out beside the others runs directly, as every game does on iOS. A game another console's
driver made says so in its `.ini` (`system=` of `[Game]`: `3ds`, `vita` or `gba`; an older one by
its layout, a 3DS `.ini`'s `model=`, a Vita folder's `ux0/`): its row says "3DS game, ready to run" and the like, and
Enter and R go through that driver, in the game's folder (`--game=`), with none of the Switch's Settings. A CIA goes
to `scarletnx-3ds`, which is never executed on it directly. Output capture, resource accounting and the return to
the main menu work as for a file picked by a load.

### The Compile tab

**Compile**, the fourth tab, shows the queue while it holds a compile (R's recompile, B's AOT pack, or the first of a
game opened before it was made), in the frame of every screen: on the left
the one in focus, as a theme's loading screen (YoRHa's, its colours, its usage and how it goes on the footer's band
at its foot) or its output under *Output* (the default's), in letters a half step under the text's; on the right the
**Queue**, a row each in the order asked (`42%`, `next`, `3rd`, `OK`, `FAILED`, B's saying *AOT pack*), and the
Status box. Up/Down go over the queue, the panel following. **Tab** goes back to Games, the recompile going on;
**ESC** aborts the one running (asked first on Android), its line gone with it, or on one that ended goes back to
Games, its line seen and gone; **Del** takes out one waiting, or the line of one that ended; **Enter**, or a click on
its row once in focus, plays the game of one that ended well. When one ends the next starts by itself, and the tab
says `Compile OK` for five seconds before the next one's percent; while one that failed is listed and none runs it
says `Compile FAILED`, on the failure colour, blinking as Share Log does, which blinks too. The tab goes with the
queue's last line. The window's icon in the taskbar (Windows', a Linux desktop's launcher entry, through SDL) fills
as the one running goes, and turns red once one failed and none runs. `last-recompile.log`, beside `last-run.log`,
holds the queue's output since one started with no ended line left. Move Old Files and Clean Old Settings wait until
the queue ends; the window's close, Exit and ESC on the menu ask first, naming the one running.

With Background recompile *Gentle* every recompile of the queue goes gently; with *Full*, one goes gently by itself
while a game the launcher started plays: the one running drops to the low priority at once (its threads stay as it
started), the next starts with half of them, and once the game ended it goes back where the system lets a program
raise a priority it lowered (Windows does; Linux and macOS without the privilege keep it low until it ends, the
Status box saying why). On iOS, where the driver runs on a thread of the app and a game plays once the menu closes,
a game plays once the queue ends.

## Output and resource use

While a driver runs, the window displays its merged stdout/stderr pipe. Descendants
inherit the same pipe, including extraction, recompilation and the final game.
The console consumes bytes as they arrive, even without a newline. Long lines wrap
at the right edge. At the bottom, rows move up and the oldest row is discarded.
Only the visible screen is retained, with no scrollback or accumulating log file.
ANSI color/title escapes are ignored. UTF-8 characters available in CP437 use the
corresponding VGA glyph, and other characters are displayed as `?`.

After the driver and its descendants finish, the initial menu returns automatically
with the exit status and the last visible output. Nothing is restarted automatically.
A missing driver or invalid dropped path leaves the prompt open with a diagnostic.
Drivers use line-buffered stdout so progress remains visible when piped. A third-party
program that buffers or privately redirects its output still controls when it flushes.

The Status box samples combined GUI and descendant resource use every half second:

- RAM is the sum of resident sets (Windows working sets), in MB or GB. Shared pages may
  be counted more than once. It is not the machine's total installed memory.
- CPU is a share of the whole machine, 100% being every core busy, then how many cores'
  worth of time that is.

Linux and FreeBSD track a private child session, including the separate process
groups created by the driver's stage supervisor and descendants that outlive their
immediate parent. Linux uses `/proc`, FreeBSD uses `sysctl`. Windows assigns the
suspended driver to a Job Object before letting it run, containing its descendants
and providing job-wide CPU accounting. No elevated privileges are needed.

## Themes

A theme colours and dresses the screens: the head (the tabs and a title), the boxes (the list, the box
beside it about the focus, the Status box: resource use, the disk and the working folder), the tips (what the
focus is about or the screen's message) and the foot (the screen's keys). It applies to the menu screens
and to the run, which is **compiling** until the runner prints its `ScarletNX - <name>` banner and
**loading** from then on.

The screens are boxes side by side, nothing over another, the default framing them in thin lines and YoRHa
filling them. At the top the tabs, Menu, Games and Settings (and Recompile while its queue holds a recompile), and
the screen's title after them against the right, and at its right the user games play as
(Settings > User, which lists them under **Current user**, Left/Right picking, and renames each: Enter types its
nickname, the rule [USAGE](USAGE.md#users) gives checked, then written to `users.ini`): a box framed as the others are
holding their nickname and their picture, `users/<uid>.jpg` or Wimsy
without one ([USAGE](USAGE.md#users)), square or round (`[head] picture`; `none` for no box). A click on it opens
the users over the screen, each with their picture; the one picked is Settings' user from then on, and ESC or a click
off them leaves it as it was. The head is the same on every screen of one window: when the main screens' titles
(the menu's, Games', the keys', `SCARLETNX  /  SETTINGS` and while its tab shows `SCARLETNX  /  RECOMPILE`) do not all
fit beside the tabs, every one takes letters a
size smaller, down to the text's, else leaves out the same first parts (`SCARLETNX  /` first), else shows no title.
The size is the one the English titles take, so the head is as big in every language, and a translation too long for
it leaves out first parts too (its last part alone, big, where `SCARLETNX  /  SETTINGS` fits whole); a
screen deeper in
the Settings leaves out what more it must. Under them the list, each entry a box
of its own, the focused one framed in the focus colour or inverted, with the theme's cursor in the margin;
beside it a box with what the focus is about (a game's path whole, Games' folders, the keys in use, the last
run's output), never what the tips already say (what the row does, a game's kind), and under that one the Status
box. The tips have a row of their own under the head, their letters a half step under the text's (1.5x by a 2x text),
as the Status box's and the note at the foot of a Settings box are; the setup's go to the foot beside its keys, where they take one row,
a long one sliding, or wrap over its rows with `[tips] wrap`, the last sliding what does not fit. At the foot the
screen's keys, the whole width (the setup's beside its tips), in letters a size under the rest's: its own keys, ESC alone
at the left of the row, **F1** last, which opens a box over the screen with every key it has, those every screen has too
(the arrows and the mouse, Enter, Space and a click, the pages, Tab and F2), until any key or a click closes it; on a
phone, the words to tap, whole, in the text's letters, each row of them as tall as a list's row (48 pixels times
the text's size) for a finger, and so are the tabs. The tips and the keys each in a box with a bar down
its left (`[tips] box`, `[footer] box`) or on the page, each key with what it does a button as a tab is: a box as the
theme's are (filled in the dim colour, framed in it, or on the foot's band framed in the soft one), its text a column
inside, a column between two, as tall as a tab where it has a row of its own. Games lists the recent games and the
games, as many as the list's box holds and the rest a scroll away, the focused one's path and kind beside them; the keys'
screen, and a load's, is one box with where the keys go, the waiting line in its middle (a click in the box
opens the picker). The Settings list their groups, and beside them the focused group's
rows: Enter, or a click on a row, opens the group, its rows taking the focus, the group shown as the focused one; a
click on another group opens that one. No screen has a Back row: ESC goes back, and so does a click or a tap on the
foot's ESC. A failure of the system's (a file not written, a folder not made, a picker that did not open, the driver
not found, the log not saved) shows whole in a box over the screen until OK, once in a session; the tips or the
note go on saying it after. F5, or the foot's (Refresh on a phone), looks at the disk again at once rather than at the next look:
Games lists its folders again, Folders says which of its folders are there, the menu counts the disk
again, and the box of what an older version left looks again. The theme editor lists its keys, the preview beside them. The list's column is two fifths of
every screen, 20 columns at the least, and a window under 45 columns shows the list alone, or with app.ini's
`side_box = on` (F2) the side box under it; a run's output and the theme editor's keys take three fifths, the side box
24 columns at the least, under them on a window under 45 columns. The screens are laid out by `src/gui/ui.txt`'s
Screen and Wide (a theme's `[head] place`, `[tips] place` and `[body] sizing` are deprecated, said as it loads:
[DEPRECATIONS](DEPRECATIONS.md)), and so is every list
(a theme's `[body] gap` and `[body] scroll` are deprecated too): the menu's, Games',
the Settings' two, the theme editor's, the choices of a question, of UI scale's dialog, of the Recompile box and of the
setup's pages, the setup's Appearance rows, the box of users, that of a game's updates, DLCs and mods, and the OK of
a box of files. Their rows are 2 pixels apart times the text's size (on Android and iOS 48 pixels high: a phone held wide gets
2x by default, so 96, about 37 dp at a density of 2.6, Android's 48 dp touch target at 3x; the desktop's are 17), the focused one kept in the middle (on a
group's screen, the group open in the groups' list; Games' grid by its lines) as the keys, a pad or the wheel move
it, and a list still when the mouse, a pen or a tap moves it, the cursor inside each row, which a click takes
whole as a tap does. Every list whose rows do not all show, Games' grid too, has a scroll bar a letter wide at its
right, the rows beside it: its thumb as tall as the share of the rows shown and where they are, which a click takes
there and drags, and a finger too from two letters of the bar's right edge; the rows stay where the thumb or a finger
put them, the focus where it was, until a key, a pad or the wheel moves the focus. Under a box over the screen, only the box's
bars take a press. That is the **Side panel**'s *Auto*, the
default (`side_box = auto` in `app.ini`, Settings > App); *On* shows it on every screen, under the list where both do
not fit, and *Off* never, the list taking the whole width without the Status box, a group's rows its column. F2 shows
it where it is hidden and hides it where it shows, kept as *On* or *Off*. A text too long for its box wraps over
its rows, its last sliding what is left, or takes one that slides with `[body] text = slide`. A run of the console layout shows the driver's output in a box sized to it,
beside it what runs and how it ended over the status, the file in the tips and the ESC line where the keys
go, which a tap aborts. The button map draws the pad in the box between the head and the foot. A question comes
over the screen darkened, in a box in its middle, or with `[body] options-dialog = yorha` (YoRHa's) in a box at its
foot, a tab with its label on the box's edge, the choices at its right above it, as the game's dialogs. The files an
older version left come in a box over the screen darkened.

The built-ins are `default`, the look above, and `yorha`: paper and ink, a title twice the text's size where it fits whole, a faint
grid on the page, square markers, the focused box inverted, the tips in a box under the head, the keys in one on a
band of ink at the foot, and a run drawn as a
dark loading screen in its own colours (`[run]`): the title, the game's name with turning
dots and a turning ring; each step the driver and the runner report (unpack, extract,
recompile with its blocks, built, launch, GPU) typed down the page, with turning dots
while it runs and `...OK` or `...FAILED` once it ends, the cursor blinking on the current
one, and the other lines it printed indented under it (`[run] bar` adds a progress bar
with the running step's percentage). A step that counts, the extraction by
what hactool has written, the recompile by its blocks, shows how far it is on its line as
`[run] progress` says: `amount` (`1.2 / 4.0 GB`, `3 / 6`), `percent` (`30%`, YoRHa's) or
`time`, what is left at its pace so far (`01:23 left`). With `layout = console` the
driver's own row says it, `extracted 1229 of 4096 MB (30%)`, written over itself each second. The menu fades out to black as a run starts and the run fades in from it; ESC
fades the run out and the menu back in.

The runtime draws a loading screen of its own in the same look while it prepares the shaders before a game starts
(`prepare_shaders = 2`, the default). It shows steps with their progress, the lines of the work scrolling under
them, and a progress bar in every theme. A key that types, an arrow or the keypad, a gamepad button other than Home,
or a tap starts the game at once, and the rest is prepared while it runs. Esc quits, or on Android back opens the
menu, whose Exit quits. The screen appears only when the work takes longer than a quarter second, and its words are
in English whatever language the launcher uses. It reads the theme the launcher picked from the launcher's
`app.ini`, from the built-ins, the `themes/` of the data folder (the one Folders picked, when it did) and
the themes shipped beside the launcher, which it finds when the launcher started the game. It uses the theme's
`[run]` colours, `layout`, `texture` and `progress`, and its loading title. A theme that is no longer there draws as
YoRHa. So does a runtime built without the launcher, which on Android is every runtime: the launcher is the app's
own library there, and the game's runtime can't ask it.

A theme is also a folder holding a `theme.ini`, the folder's name its id, in
`themes/` in that same folder or in `themes/` beside the GUI; the first one found
wins, and a folder named like a built-in replaces it. The built-ins are such files, and
nothing else: each folder of `src/gui/themes/` is one, its `theme.ini` compiled into the GUI
(`cmake/gui_themes.cmake`) and read as any other, setting every key. The runtime's game menu
takes YoRHa's colours from the same file at build time, and so does its loading screen when it can't read the
launcher's theme. The files are copied beside the GUI
in the build and installed in `share/scarletnx/themes`: copy one under a new name to make a theme, or edit one
to change the built-in. `[theme] base` picks the built-in a file starts from, and a key it
leaves out, or that cannot be read, keeps the base's value. What a file has that the theme does not take as written
is said, the file named, in the menu's message as it opens and in Settings > App's note when the theme is picked: a
value it cannot read with the one it keeps (`[body] focus = middle is not color or bar: it keeps color`), a number
past its most with the most it takes, a base that is no built-in with the one it starts from, and a key no theme has
(an ErET file's own keys aside). A line with no `=` is no setting. The menu's **Theme Editor**
writes one with every key.

```ini
[theme]
; the theme file's format, 1; a newer file's keys this GUI knows still apply
format = 1
name = Night ink
author = me
; default | yorha
base = yorha
[colors]
; #RRGGBB. soft: hints, secondary text, rules. focus: the focused box, or its band.
; dim: the boxes' frames, a filled box, the grid and the title's shadow
page = #101418
ink = #D8D4C4
soft = #8A8676
focus = #D8D4C4
active = #C8A040
failure = #D05040
dim = #3E3E40
; a diff's added lines, as failure its removed ones; no screen uses it yet
added = #6A9A50
[head]
; plain | bar: the tabs and the title on a band of ink; rule: a line under the head
style = bar
rule = true
; the title's letters, times the text's (1 to 3), and a shadow under them in the dim colour
size = 2
shadow = true
; the user at the head's right, their nickname and picture in a box: none | square | round
picture = round
; the title per screen: menu, games, keys, compiling, loading
menu = NIGHT  /  MENU
[body]
; before each item; quotes keep spaces
marker = "> "
; the focused box: color | bar
focus = bar
; the boxes outline (framed) or fill (filled)
panels = fill
; a text too long for its box: wrap (over its rows) | slide (on one row)
text = wrap
; in the list's margin, beside the focused box
cursor = "> "
; a question and its choices: default (a box in the middle) | yorha (the question in a box at the foot, the
; choices at its right above it)
options-dialog = yorha
[backdrop]
; a faint grid on the page behind the boxes, in the dim colour
grid = true
[tips]
; in a box with a bar at its left, or on the page
box = false
; over the foot's rows, or on one row that slides
wrap = false
[footer]
; plain | bar: on a band of ink | none: no Status box; rule: a line over the foot
style = plain
rule = true
; the keys in a box with a bar at its left, or on the page
box = true
[run]
; console: the driver's output as it comes; steps: the loading screen above, in its own colours
layout = steps
page = #141416
ink = #E6E4DC
soft = #8E8C86
dim = #3E3E40
failure = #D2463A
warning = #D9B24A
; faint lines over the page, the progress bar and percentage, and the ring: none | left | right
texture = true
bar = true
ring = right
; a running step's progress: amount | percent | time
progress = percent
; detailed: each step's other lines under it, down the screen; collapse: a done step's
; folded away, a failed one's kept
detailed = true
collapse = false
[prompt]
; a line that waits on the user, when its key is true: framed, left | center | right on its
; row, blinking at blink_ms. keys: the keys screen's drop line; done, failed: the ESC line
; after a run; aborting: a run being aborted
align = center
open = "[ "
close = " ]"
blink = true
keys = true
failed = true
[transitions]
; milliseconds, 0 to 10000; 0 turns the thing off. fade_out_ms: the menu as a run starts,
; and a run ESC leaves; fade_in_ms: what comes after
fade_in_ms = 250
fade_out_ms = 500
; how a step's line comes in: none | typing | slide | fade
line_in = typing
line_in_ms = 220
typing_ms = 5
; async: every line types from when it comes; sync: one at a time. A running step's newest line
; types a character each typing_ms, any other line whole in typing_ms
typing = async
blink_ms = 500
; a line too long for its row (big letters) slides left a column every scroll_ms and back; 0 cuts it
scroll_ms = 120
[splash]
; the boot's: one of Wimsy's drawings, at random, on page for ms milliseconds (0: none). chance: the
; boots in 100 that show this theme's, the default theme's the others
page = #DAD6BE
ms = 1000
chance = 25
```

Comments take a line of their own, starting with `;` or `#`.

The format is rEden's ErET format 1, extended, so one file can serve both: each program
reads the keys it knows and leaves the others. From ErET's, the GUI also reads:

| ErET key | here |
|---|---|
| `[theme] layout` | the base when no `base` names one: `menu` and `loading` are `yorha`, `plain`, `debug` and `skip` are `default` |
| `[colors]` | with `layout = loading` and no `base`, the run's colours, as `[run]`: ErET's loading screen is YoRHa's runs |
| `[colors] texture`, `warning` | as `[run] texture`, and the colour of a run being aborted |
| `[header] ring_side` | `right`, `left` or `right-near-text` (right after the name and its dots), as `[run] ring` |
| `[transitions] typing` | `async` or `sync`, as above |
| `[loading] title` | the loading screen's title |

ErET needs `[theme] layout`, so the built-in files carry one: `debug` and `menu`.

### Theme formats

`[theme] format = N` names the theme format a file was written for; this GUI's is 1
(`GUI_THEME_FORMAT`).

- A file of a format the GUI knows reads the same in it and in every later GUI: each key
  keeps its meaning, and each value reads to the same theme.
- A new key, or a new word for a key, keeps N: an older GUI leaves what it does not know, and
  a file that does not name it takes its base's value, so an older theme gets the new look of
  the built-in it starts from (the title's size, the tips' box, the foot's boxes).
- A key a newer one replaces is deprecated, never given a new meaning: it still reads as the new keys say, and the
  GUI never writes it again. The format has none so far.
- N goes up only when a key that exists has to mean something else. The GUI then reads the
  older format's files with the older meaning.
- A file of a newer format than the GUI's: the keys it knows still apply, the others are left.
- The theme editor writes every key and the newest format it knows (`gui_theme_write`), so its
  files never lean on a built-in's values, which may change. It reads an older file as that
  format reads, and writes it in the current one.

## Languages

The menu's phrases are English in its code, each through `snx_tr` (`src/gui/i18n.h`), and so are the setup's notices,
marked in `src/common/utilities/setup.c`, which the terminal shows as they are. `po/scarletnx.pot` lists
them, made from the code by `scripts/build-languages.py --pot`; the `gui_languages_pot` test fails when it is not what
the code says. A language is a `po/<tag>.po`, named by its BCP 47 tag as language-Script-REGION, each part only
when it tells the language apart (`de.po`, `pt-BR.po`, `zh-Hans-CN.po`), in gettext's format: the build makes each one a
catalog of gettext's `.mo` format compiled into the menu, which reads it with musl's `__mo_lookup`
(`src/gui/i18n.c`). A new one starts as a copy of the `.pot` whose header names it, in UTF-8:

```po
"Language: de\n"
"Content-Type: text/plain; charset=UTF-8\n"
"X-Language-Name: Deutsch\n"
```

`X-Language-Name` is what Settings > App > Language shows. `msgmerge -U po/de.po po/scarletnx.pot` (gettext's tools)
brings a translation up to the code; a phrase it leaves empty, or marks `fuzzy`, stays English. The build refuses a
`.po` that:

- changes a phrase's printf conversions (`%s`, `%d`, `%zu`, `%.*s`) in number, order or type, uses numbered ones
  (`%1$s`) or `%n`, or has a `%` its English does not: C's printf takes the arguments in the order the code gives;
- changes the keys of a foot's sentences (`#, snx-keys`, "Key: what it does."): each key stays as it is, or as the
  same file translates it (`#, snx-key`, as `Back`), since a click on the foot finds a key by its word, and a sentence
  greyed (`~`, one its screen cannot do now) stays greyed;
- has a letter the menu's font does not draw, which would show as `?`: the font is CP437, the Latin letters of
  U+00A0 to U+024F past it and the wide letters the languages show, GNU Unifont's. `scripts/build-glyphs.py --from`
  keeps them from a Unifont `.hex` (`unifont-<version>.hex` or `unifont_all-<version>.hex`): the Latin ones in
  `src/gui/unifont-latin.hex`, which the build draws with the VGA font's stems and baseline, and in
  `src/gui/unifont-wide.hex` the 16x16 glyphs of every other letter of `po/*.po` and `tests/po/*.po`, as Chinese,
  which the menu draws two columns wide: every width, cut, wrap and slide of its text counts them so (`src/gui/wide.h`),
  and a row breaks after one as at a space. A language that shows a new letter runs it again; the `gui_glyphs_kept`
  test checks the files hold each of them once, and no other;
- has contexts (`msgctxt`), plurals (`msgid_plural`), a phrase twice, or is not UTF-8.

The runtime's settings stay as the runtime names them (their groups, names, help and words come from
`scarletnx --settings`), as do the Controllers' button map page, drawn as the game's menu draws it, the theme
editor's rows (the file's own keys) and the lines Share Log adds for us.

## Building and testing

The NX build enables `SNX_BUILD_GUI` by default. Disable it with
`-DSNX_BUILD_GUI=OFF`. Tools-only builds do not enable the GUI.
The GUI itself needs only SDL3, not Vulkan or FFmpeg. The driver binaries can be
built separately and installed beside it or made available on PATH.

To build it alongside the NX runtime (which supports `-H` for no-window runs):

```sh
cmake -S . -B build/gui -DSNX_DEPLOY=OFF -DSNX_BUILD_GUI=ON
cmake --build build/gui
ctest --test-dir build/gui -R '^gui_' --output-on-failure
```

For just the GUI and Linux runtime, use `-DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=ON`: the launcher is
then `scarletnx-gui`, a program of its own, which looks for `scarletnx` as it does for the other drivers.
Tools-only builds cannot enable the GUI.
Supported host platforms are Linux, FreeBSD, Windows (llvm-mingw) and Android (`dist/android`).
SDL3 handles window creation, keyboard/mouse input, rendering, clipboard and drag-and-drop
on every host. The platform-specific code only handles child processes and resource
accounting. See [BUILD](BUILD.md) for SDL3's desktop dependencies.

The menu's list, Games' and its grid, the Settings', the setup's Appearance, the choices of the questions and the
setup, the boxes of users, of a game's add-ons and of Games' View, and the OK of a box of files are scenes of
`src/gui/ui.txt`, which `tools/uigen` turns into `gui/ui.h` and
`gui/ui.c` as the GUI builds (the format is in `tools/uigen/uigen.c`'s first comment): their layout computed at build
time, the drawing and callbacks main.c's. The screens' boxes (the head, the tips, the list, the side box, the Status
box and the foot) are its frames Screen and Wide, laid out as they are drawn, from what main.c measures. `uigen` tests the generator's output and refusals, `gui_ui` runs what it
makes of `tests/ui_test.txt`.

The tests cover menu transitions, bounded console scrolling, split UTF-8/ANSI
sequences, path parsing, argument quoting, inherited grandchild output, resource
accounting and process-tree cleanup. Browser tests cover filtering, missing/empty
directories, pagination, resizing, Back/ESC, launching selected files and R's recompile.
Key setup tests cover installation, replacement, invalid drops, failed writes,
self-copy safety, cancellation, and the bright-red overwrite warning. Setup tests (`gui_setup`) go through every
page of a first start in a home of their own and check what each writes (a portable copy, the language, the folders,
the keys, the first user) and refuses, users.ini kept or overwritten, the fixed uid kept for its saves, a notice
alone after an update, `--no-setup`, and the question where `options-dialog` puts it.
Language tests cover a catalog's search in either byte order, catalogs cut or pointing out of themselves, the language
Auto picks from the system's locales, English where a language has none, the Language row and `app.ini`'s key, and
the script's refusals; they use a language of their own, `tests/po/xx.po`. Glyph tests cover the Latin and the wide
letters' lookup, each way the menu draws a line with them (a wide one's two columns in widths, cuts, wraps, slides, a
foot's keys and typed text), UTF-8 read with a `?` for each byte no character starts, and the script that keeps
them.
Settings tests cover the runtime's list read into rows, picks, the options each combination
passes, and the screens of its groups and App; menu tests cover mouse hover versus explicit
activation, the menu without the games and Games with its recent games' show-more row and P,
yellow/pink highlighting and the tips. Theme tests cover the built-ins, a `theme.ini` over its base
(unreadable values and unknown keys included), the `themes/` folders listed, the
compiling and loading screens, and the YoRHa bars and band in the real renderer. Step
tests cover each driver and runner line, the other lines kept under their step, lines
split across reads, the progress and how a run ends; the renderer tests check the menu
fading out into a run, the loading screen, its typing, detailed and collapsed steps,
texture and fades at a fixed clock.
SDL tests use the dummy video driver and the
software renderer, exercising the real font and event handlers without a display.
The font's provenance and public-domain notice are in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).
