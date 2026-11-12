<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Filesystem

## Index

* [Paths](#paths)
* [State + I/O](#state--io)
* [Shared fonts](#shared-fonts)
  * [The Noto stand-ins](#the-noto-stand-ins)

The tree (`src/common/filesystem/tree.c`, which `src/nx/hle/filesystem/fs_tree.h` names for the Switch) indexes
`<Game>_data/` once, capturing its absolute base. Subsequent game-data lookup, metadata and enumeration use the tree; content I/O requires indexed nodes.
No host rediscovery. External edits require restart.

Synthetic tree + IPC/RomFS checks, without game data/GPU:

```bash
python3 tests/run_filesystem_tree.py --cc clang --sanitize
```

Windows: `py -3`; `--cc` accepts a quoted path. Harness selection: `--only tree` / `--only service`.

## Paths

Top-level directories are drives; global lookup accepts data-relative paths:

| Guest path | Data-directory path |
| --- | --- |
| `drive:/file` | `drive/file` |
| `/file` | `root/file` |

`IFileSystem`: mount-relative `/`; reject drive prefixes and escaping `..`. Normalize separators/`.`; case-sensitive.
Missing `root/` is virtual until mutation. Saves use `save/<type>`, and an Account save its user's
`save/user.<uid>` (32 hex digits, `users/users.ini`'s, [Usage](../../user/usage.md#users)). The first user's, while it has no
folder, is played where an older version left it: `save/user`, the one Account save before there were users, else a
nonempty `save/system` ([Deprecations](../../user/deprecations.md)). With `save/user` and its folder both there the game stops
and leaves both, for the player to keep one.

`save_layout=global` (the default) keeps the saves apart from the game, in the data folder's `save/` (`users/`'s
folder), as the console keeps them:

| Save | Folder |
| --- | --- |
| Account | `save/user.<uid>/<title ID>/` |
| Device | `save/device/<title ID>/` |
| BCAT | `save/bcat/<title ID>/` |
| Cache storage | `save/cache/<title ID>/<index>/` |
| System, SystemBcat | `save/system/<save ID>/`, `save/sysbcat/<save ID>/` |

Temporary storage stays the game's `save/temp`, and every save of a program with no title ID of its own (a homebrew's)
stays in its own folder. `save_layout=game` keeps each in the game's `save/<type>` as above. Each of those folders is a
graft of the index, writable: the ones there at startup are indexed then (`fs_saves_init`, after the users), and one
made later is made empty, so nothing is enumerated after startup (`fs_tree_graft_new`). A save only the other layout
holds is played where it is, the game's log and stderr saying once a run where it goes; both holding one stops the
game, which leaves both.

An Account, Device or BCAT save's Commit writes its note, `<its folder>.version` beside it on the host and out of every
mount: `program=` (the title ID that ran, an update's own), `version=` (its content meta's, when known) and `display=`.
Opening a save whose note names a newer version of the same game, by the content meta versions when both are known,
else by the numbers of the display versions, warns once with both, and the crash, svcBreak and BRK reports repeat the
line (`fs_save_report`).

A save is kept as the console keeps one in a folder of the host (its `DirectorySaveDataFileSystem`). Its folder holds
`0/`, the save's last commit, `1/`, the copy the game works in and the only one it sees, and the file `snx-journaled`,
which says the folder is laid out so:

| When | What happens |
| --- | --- |
| The save is opened | `1/` becomes a copy of `0/`: what a run wrote and did not commit is gone. With no `0/`, a commit was cut short: it is finished from `1/`, and the log says so |
| The game writes | in `1/` alone |
| `Commit`, `CommitSaveData` | refused (WriteModeFileNotClosed, the file named in the log, which the SDK stops the game on) while a file of the save is open for writing. Else `0/` is renamed `_/`, `_/` becomes a copy of `1/` and is renamed `0/` |

The first rename is the commit. A game that dies before it leaves the last commit; one that dies after it leaves `1/`
whole and no `0/`, which the next open or commit finishes. `0/` is never written in place, so the game's death at any
moment leaves the last commit or the new one, never part of a save (`filesystem_save_kill` kills it at each step).
Nothing is synced to the disk: a host that loses power during a commit, while the system still holds the bytes of
`1/`, can lose both. Each open and each commit copies the save whole. A save opened while it is open shares the one
working copy (the console refuses the second mount; here `save/system` can be several saves' folder). Temporary
storage has no commit and is written where it is. What the folder holds beside `0/`, `1/` and `snx-journaled` (a save
pasted into it by hand) the game does not read: the log names the first such entry once a run for each save type, and
that a save's files go in `0/`. A commit of several saves at once (`nn::fs::Commit` of a list of
mount names) has no native answer: its SDK code asks fsp-srv, and sm stops the game.

A save's folder with files in it and no `snx-journaled` is one an older version kept, the game's own files in the
folder itself (a game's own folders may be named `0` and `1`). It is played where it is and written in place, so a
game killed while it saves there leaves part of a save; the log names the folder once a run for each save type, the
launcher lists it, and Settings > App > Move Old Files moves its files into `0/` and writes `snx-journaled`
([Deprecations](../../user/deprecations.md)).

Init rejects scan/stat errors, symlinks/junctions, regular-file hardlinks, special files and invalid names, and the
game stops with the path it stopped at and why in the line before (`fs: could not index the game's files in ...:
.../sdmc/shared is a symbolic link, which the index follows only as a folder at the top of the game's data folder`).
A mod's RomFS refused so is left out, the line saying why.

One link is followed: a symbolic link (on Windows a junction too) to a folder at the top of the data folder, such as
an `sdmc/` or a `save/` kept on another disk or shared with another program. The folder it points at is indexed in its
place as a writable graft (`link_folder`), so the game reads and changes the files where they are. Still refused: a
link anywhere below the top, in the data folder or in the folder a link points at (nothing scanned can lead back, so
there is no cycle to detect), a link to a file, a link to nothing, and a link to a folder the index already holds, to
one in it or around it (the data folder, another link's folder, a save's or the RomFS's graft): two names for one file
would each keep a size of their own, which is why hard links are refused. Nothing removes or renames the link or the
folder it points at: `fs_tree_remove` on it empties the folder, and removing the game's folder (`snx_remove_tree`)
takes the link and leaves the folder. `mods/` is read apart from the index: as a link it is read through, but a mod
that is a link, or whose `exefs/` or `romfs/` is one, is left out, a line in the log naming it.
Tree limits: 4096-byte path buffers including NUL, 256 directory levels below the data root.
IPC requests allow 0x300-byte paths excluding NUL. Host limits also apply.

## State + I/O

- `FsTree.lock`: tree access, service I/O, shared size cache. Resolve first; record successful host mutations,
  including partial success.
- Handles retain node identity across rename; detached nodes live until last close. Removed directory handles return `SNX_RC_FS_PATH_NOT_FOUND`.
- Live directory cursor: `strcmp` order, last visited name, latched EOF; counts reflect current children.
- RomFS: prefer indexed `romfs.bin`; otherwise synthesize from `romfs/`.
- Storage reads that stream take a console's time: an `IStorage::Read` of 64 KiB or less that starts where the same
  thread's last read of the same storage ended waits what 8 MB/s takes (`fs.c`'s `fs_pace_ns`, 8.2 ms for 64 KiB). A
  stream's first read, a bigger read (a load) and a read anywhere else wait nothing, so loads stay as fast as the host.
  A game's read-ahead is timed against the console's storage: a movie library that caches its file a 64 KiB page at a
  time, as fast as the reads come back, filled the heap its game sized for the console before the decoder let go of
  the buffers of its first configuration, and the game stopped at its own out-of-memory trap. The decoder lets go of
  them once the game takes its first picture, which a long frame there delayed by up to 1.2 s at opt 0; at 8 MB/s
  the cache stays under what fits for 1.8 s. The log says it the first time a read waits, with the size and the
  rate.
- Readonly RAM copies: missing system archives become valid empty images at `system/archives/<16-hex-id>.romfs`. No
  persistent host copy. One there in the data directory is served instead.
- `0100000000000802` (MiiModel), the Mii parts' shapes and textures a game that draws Miis builds their heads from, is
  generated when absent (`hle/filesystem/mii_model.c`), every Mii in a plush's look: a wide round head, thick hair
  falling straight to the jaw, big eyes set low and the mouth close under them (one map of the face takes the features
  the SDK composes there, on the mask, on the faceline's texture and in the launcher's picture). Every shape is a
  parametric surface (the head and the face's front
  per faceline, 24 hair forms in normal and cap form whose edge ends in pointed locks, tails for the twin-tail and
  ponytail styles (wide flat locks, high or low, the side ponytail and the one at the back too), the curled twin tails as chunky drills, one of
  them with an ahoge, noses, a beard, the caps, ties, bands, bow and clip of the hat parts, the noseline's and the
  glasses' quads) and every texture drawn from distance functions (eyes with a
  catchlight, brows, mouths, glasses, mustaches, beards, lines, make-up, the mole, the noselines, the hats), none of
  the console's own. Its six files
  (`NXTexture{Low,Mid}{Linear,SRGB}.dat`, `Shape{Mid,High}.dat`, the SRGB copies sharing their Linear ones' bytes, each
  shape with as many quads as fit the console's largest part of its kind in that file) keep the console's
  layout: the part tables with each kind's largest part and alignment, the console's list of parts (which are
  absent), each texture in its format (BC4, BC5 or ASTC 4x4), size and levels, block linear, and each part a zlib
  stream, deflated as the console's are. The SDK reads a file whole into a heap the game sizes for the console's, so
  none is bigger than the console's of its name. It is about 1.8 MiB, made at the game's first open of it. The
  player's own firmware's (its RomFS as `system/archives/0100000000000802.romfs`) is still served first and looks like
  the console.
- `0100000000000823` (NgWord2) is the exception: when absent, it is synthesized with firmware 11.0.1's 52-file
  profanity-filter set (the `ac_*` pattern files and `version.dat` 0x1A), from Eden's stand-in archive, in place of an
  empty image. A game that calls `CheckProfanityWords` (Yo-kai Watch) reads the pattern files, and stops on the empty
  stand-in. Later firmware carries more files (`ac_16`, `ac_17`, `ac_similar_form_nx`, the `trie_*` tables), which the
  11.0.1 image does not have: a game built on that SDK is served by the console's own `.romfs` in the data directory
  instead, which still takes priority.

## Shared fonts

The six fonts of `pl:u` (FontStandard, the two simplified Chinese fonts, traditional Chinese, Korean and
NintendoExtended) live in one shared memory, each behind the 8-byte header the SDK reads its size from. The
console's memory is 0x1100000 bytes and the SDK maps exactly that much, as Eden's `pl_u.cpp` does, so the
fonts have to fit in it.

A console font can be placed in the game's data directory first: `fonts/FontStandard.ttf` for the default
title-ID layout is `recompiled/<id>/<id>_data/fonts/FontStandard.ttf` under the [data folder](../../user/usage.md#folders),
and the same for `FontChineseSimplified`, `FontExtendedChineseSimplified`, `FontChineseTraditional`,
`FontKorean` and `FontNintendoExtended`. Named or older deployments use their existing `<Game>_data/`
directory. `.bfttf` is accepted too and takes priority over `.ttf`; both take priority over the stand-ins.

Without one, the runtime uses the stand-ins compiled in (`cmake/shared_fonts.cmake`): Noto Sans JP, SC, TC
and KR subsets the build fetches, one per script, and yuzu's controller button glyphs for
`FontNintendoExtended`. The subsets cover ASCII, Latin-1, common punctuation and the script's standard set
(JIS X 0208, GB 2312, Big5, KS X 1001); they total about 11 MB. The console's private-use icon block,
U+E000 to U+E1FF (buttons, symbols), is not in any free font, so `pl_font.c` gives every stand-in blank
glyphs there: a game's font map that names one gets a glyph instead of a null pointer, and the icon draws
as nothing. A console font is not touched.

Font substitution can change text weight and spacing.

### The Noto stand-ins

The console's fonts are the firmware's and cannot be shipped. `cmake/shared_fonts.cmake` takes yuzu's controller
button glyphs from Eden's source, and `cmake/dependencies.cmake` fetches the four subsets below, pinned by their
SHA-256, only in a build with the Switch runtime (`SNX_BUILD_NX` without `SNX_TOOLS_ONLY`).
`cmake/shared_fonts.cmake` compiles them into the runtime.

| File | Slot | Covers |
| --- | --- | --- |
| `NotoSansJP-Subset.ttf` | `FontStandard` | ASCII, Latin-1, punctuation, JIS X 0208 (kana and kanji) |
| `NotoSansSC-Subset.ttf` | `FontChineseSimplified`, `FontExtendedChineseSimplified` | ASCII, Latin-1, punctuation, GB 2312 |
| `NotoSansTC-Subset.ttf` | `FontChineseTraditional` | ASCII, Latin-1, punctuation, Big5 |
| `NotoSansKR-Subset.ttf` | `FontKorean` | ASCII, Latin-1, punctuation, KS X 1001 (Hangul and Hanja) |

The four total about 11 MB, which fits the shared memory. The full Noto Sans JP/SC/TC/KR variable fonts do not
fit, and their pan-CJK sibling is CFF, which the SDK's font parser did not load in a test (the game crashed at
boot). The subsets are TrueType `glyf` at the Regular weight, the script's standard character set, with hinting
dropped.

#### Regenerating the subsets

The files the build fetches are the ones commit `777f377013806bc4cdbbc0306eca6e7e9c835df2` of this
repository kept in `dist/fonts`, and the build takes them from there. `tools/subset_fonts.py` makes them
from Google Fonts' Noto Sans JP, SC, TC and KR at commit `e7db6ca603fd4862010383ff17ffd6cc4ffac1c0`
(`ofl/notosansjp`, `ofl/notosanssc`, `ofl/notosanstc`, `ofl/notosanskr`, `NotoSansXX[wght].ttf`). It needs
`fontTools` and network access, neither of which the build needs:

```sh
python3 tools/subset_fonts.py --out fonts
```

It instantiates each variable font at weight 400, keeps ASCII, Latin-1, common punctuation and the
script's standard set, drops hinting and the variation tables, and writes the four files above. A new
set is published at a fixed URL (a release asset, or a commit that holds the files), and the URL and the
four hashes in `cmake/dependencies.cmake` change with it. The
sources are under the SIL Open Font License 1.1; the text is `dist/licenses/Noto/OFL.txt`.
