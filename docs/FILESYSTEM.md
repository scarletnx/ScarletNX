<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Filesystem

`src/nx/hle/filesystem/fs_tree.{c,h}` indexes `<Game>_data/` once, capturing its absolute base.
Subsequent game-data lookup, metadata and enumeration use the tree; content I/O requires indexed nodes.
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
`save/user.<uid>` (32 hex digits, `users/users.ini`'s, [USAGE](USAGE.md#users)). The first user's, while it has no
folder, is played where an older version left it: `save/user`, the one Account save before there were users, else a
nonempty `save/system` ([DEPRECATIONS](DEPRECATIONS.md)). With `save/user` and its folder both there the game stops
and leaves both, for the player to keep one.

Init rejects scan/stat errors, symlinks/junctions, regular-file hardlinks, special files and invalid names.
Tree limits: 4096-byte path buffers including NUL, 256 directory levels below the data root.
IPC requests allow 0x300-byte paths excluding NUL. Host limits also apply.

## State + I/O

- `FsTree.lock`: tree access, service I/O, shared size cache. Resolve first; record successful host mutations,
  including partial success.
- Handles retain node identity across rename; detached nodes live until last close. Removed directory handles return `FS_NOTFOUND`.
- Live directory cursor: `strcmp` order, last visited name, latched EOF; counts reflect current children.
- RomFS: prefer indexed `romfs.bin`; otherwise synthesize from `romfs/`.
- Readonly RAM copies: missing system archives become valid empty images at `system/archives/<16-hex-id>.romfs`. No
  persistent host copy. One there in the data directory is served instead: a game that draws Miis (Mario Kart 8
  Deluxe) needs the console's Mii models, system data `0100000000000802`, whose RomFS goes there from your own
  firmware. Without it, `0100000000000802` is a stand-in whose parts draw nothing: its six resource files
  (`NXTexture{Low,Mid}{Linear,SRGB}.dat`, `Shape{High,Mid}.dat`), each a magic (`NFTR` or `NFSR`, as Eden's stand-in
  has them), version 1 and each part kind's largest size and alignment, then a part table whose every texture names
  one blank 8 by 8 image and every shape one degenerate triangle. The SDK's loader reads a file whole and indexes its
  part tables from its start without checking the header; a game that composes a Mii's face registers the composed
  textures, and crashes on a null one when the parts are absent.
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
title-ID layout is `recompiled/<id>/<id>_data/fonts/FontStandard.ttf` under the [data folder](USAGE.md#folders),
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

[SHARED_FONTS.md](SHARED_FONTS.md) has where the subsets came from and how `tools/subset_fonts.py` regenerates them;
the text is under the SIL Open Font License 1.1 (`dist/licenses/Noto/OFL.txt`). Font substitution can
change text weight and spacing.
