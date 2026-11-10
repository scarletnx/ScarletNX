<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# What ScarletNX isolates

ScarletNX is not a sandbox. A game it runs is a program of your user, as any other you start: run only what you
would run as a program.

## A game is a native program

The recompiler turns the game's code into the computer's own, and that code runs in the same process as the runtime,
with your user's rights. Nothing of the system stands between the two: no second process, no container, no filter on
what the process may open. A mod's `exefs/`, a patch and a cheat change that code, and a GPU driver package is loaded
into the same process, so each is trusted as the game is. On an AArch64 host [NCE](../platforms/nx/nce.md) runs the
game's code as it is, and its seccomp filter is there to catch the game's system calls for the runtime, not to
confine the game.

On Android and iOS the system keeps the whole app in its own sandbox. That is the system's isolation, not
ScarletNX's.

## What the services give a game

The runtime answers the game as the console's system would, so a game that behaves sees only what a console has:

- **Files.** The file services serve the game's data folder (its files, its SD card `sdmc/`) and the save folders,
  and refuse a path that leaves a mount through `..` ([Filesystem](../platforms/nx/filesystem.md)).
- **Network.** `network=0`, the default, reports offline and still lets the game reach this computer's loopback
  addresses; `network=1` uses the computer's connection, and listeners bind its interfaces. Local wireless (`ldn`)
  works either way, over loopback with `network=0` and over the LAN with `network=1` ([Settings](settings.md),
  [Networking](../platforms/nx/networking.md)).
- **The debugger.** `-S` opens a GDB stub on loopback port 1234, which any program of this computer can connect to
  ([Usage](usage.md#gdb-linux)).

These are the console's behaviour, there so that games work, and they hold for the code that goes through them.
They are not a boundary against code that sets out to cross them: a path found to leave its folder is a bug, and is
fixed, but no one should rely on it to run a game they do not trust. For that, use the system's own tools: another
user, a container or a virtual machine.

## What the launcher downloads

The launcher downloads only what you ask for, and sends nothing by itself. **Download Driver** ([GUI
launcher](gui.md)) asks GitHub for the releases of the driver repositories it lists and downloads the package you
pick, over HTTPS checked against the root certificates the build carries. A package is checked for its form (its
`meta.json`, its library's architecture) before it is unpacked. Who made it is not checked: a driver is code, and
its repository is the one trusted.

## What is a security problem, then

A way for something you did not choose to run as a program to harm you: a file that harms when the launcher only
lists it, shows its icon or extracts it; a download that lands outside its folder or is taken from a host that fails
the certificate check; something on the network that reaches the runtime's own code through a socket it opened; a
shared log that carries a key or a private path; the project's builds, releases and workflows.
