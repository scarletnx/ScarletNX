---
name: game-tag
description: "Give a game an opaque tag in place of its title and its title ID. It covers the tag of an ID, the game behind a tag, and the check of a text for titles and title IDs. Use it when you write about one game in a commit message, an issue, a pull request or a doc."
---

# Game tags

A commit message, an issue, a doc and a report do not name a game, and do not give its title ID. Say what the game
did. When the reader must find the same game again, write the tag of the game.

A tag is `game-` and 8 hex digits, for example `game-0123abcd`. `tools/game_tag.py` makes it from the title ID and a
key. The key stays on the machine of the user. Without the key, nobody can find the title ID from the tag.

Do not put a tag in code or in test data. Code and tests describe the behavior.

## Commands

```sh
tools/game_tag.py tag <ID>...                       # the tag of each ID
tools/game_tag.py which --games <DIR> <TAG>...      # the ID and the name of each tag
tools/game_tag.py scan [--games <DIR>] <FILE>...    # the title IDs and the game names in each file
```

`<ID>` is one of these:

- A Switch title ID (16 hex digits that start with `01`). An update and a DLC give the tag of their game.
- A title ID of the Switch's next generation (16 hex digits that start with `04`). An update and a DLC give the tag of
  their game.
- A 3DS title ID (16 hex digits that start with `0004`). An update and a DLC give the tag of their game.
- `gba:` and the 4 characters of the game code in the header of a GBA ROM.

`--games` names a folder of games. You can give the option more than one time. The tool reads these items in the
folder and in its subfolders:

- A folder with a title ID as its name, as in `recompiled/`. The name of the game comes from its `control.nacp`.
- A game file with a title ID in its name. The name of the game is the text before the ID.
- A `.gba` file. The game code comes from its header, and the name of the game comes from the file name.

The tool does not store a table of tags. `which` calculates the tag of each game in the folders, and compares.

## Before you publish a text

Run `scan` on the text of a commit message, an issue, a pull request or a doc. Use `-` to read the text from stdin.

```sh
git log -1 --format=%B | tools/game_tag.py scan --games <DIR> -
```

`scan` prints a line for each title ID and for each game name that it finds, with the tag. It exits with 1 when it
finds one, and with 0 when the text is clean. Replace each item with the tag, or with a description of the game.

Without `--games`, `scan` finds only title IDs. `scan` cannot find a nickname or a short form of a title. Read the
text for those.

## The key

The key is the file `game-tag.key` in the config folder of the user:

- Linux: `$XDG_CONFIG_HOME/scarletnx`, or `~/.config/scarletnx`.
- macOS: `~/Library/Application Support/scarletnx`.
- Windows: `%APPDATA%\scarletnx`.

`--key <FILE>` names a different file. The option goes after the command: `tools/game_tag.py tag --key <FILE> <ID>`.

`tag` and `scan` make the key when the file is not there, and say so on stderr. The key is 32 random bytes with mode
0600. `which` does not make a key. It tells where the key must be.

Two persons get the same tag for a game only when they have the same key. A tag from a different key finds nothing.

Never print the key. Never copy it into the repository, a log, a memory file or a message. Do not write a tag next to
the title or the title ID of its game in a file.

## Exit codes

- 0: the command did its work. For `scan`, the text is clean.
- 1: `which` did not find a tag, or `scan` found a title ID or a name.
- 2: the tool refused an ID, a tag, an option, the key, a folder or a file. The message gives the value.
