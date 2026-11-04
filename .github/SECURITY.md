<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Reporting a security problem

Do not open a public issue for a problem that could harm someone who runs a game with ScarletNX: a crash that a
crafted game file can cause, a path that writes outside the game's folders, a service that a game can use to reach
the host, or a way to read another process's memory. Report it privately.

## How

Use GitHub's private vulnerability report: [Report a vulnerability](https://github.com/scarletnx/ScarletNX/security/advisories/new).
It opens a draft advisory that only the maintainers see. A direct message to a maintainer on the
[Discord](https://discord.gg/tRTRXXbyjp) says that a report is on its way, with nothing of the report itself there.

Include:

- the ScarletNX build (the launcher's About page, or the `build:` line of a log) and the system and GPU;
- what the game or file does to reach the problem, and what happens instead;
- the smallest input that shows it, when you can make one. Do not attach a game, a key, a dump or a save of a
  game you do not own: a file you made that shows the same problem is enough.

## What happens next

A maintainer confirms the report, says what is affected and which versions, and works on a fix in a branch that is
not public until it is ready. The advisory is published with the fix, and credit is given unless you ask otherwise.

## Scope

The runtime and its drivers, the recompiler, the launcher, the build and the workflows of this repository. A game
that behaves as it does on the console is not a security problem of ScarletNX; a report about one game goes to the
game report form, without anything of the game itself attached.
