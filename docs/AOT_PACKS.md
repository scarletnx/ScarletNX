# AOT packs from the runtime JIT cache

ScarletNX runs a Switch game with one runtime. A recompiled game module is the default code path (AOT); when
the game reaches a function the module does not have, the runtime compiles that function with the JIT, runs it,
and returns to the AOT caller. The JIT is always present. The interpreter is not a fallback.

The JIT also records what it compiled in a cache file. That cache is a profile of a real play session. An **AOT
pack** is a normal game build that links the cached functions into the game module as a finished AOT variant
table. It is still the same hybrid runtime: cached functions run AOT, anything missed still runs JIT. An AOT pack
is a faster starting point, not a different runtime and not a pure-AOT build.

## The optimization rule

The profile cache and the AOT pack must be built with the same optimization level. The launcher enforces this
before it starts a pack build:

- **Game AOT level**: the launcher's `Optimization level` (`opt`) in Settings. This is the `-O` used when the
  game is recompiled.
- **Cache level**: the runtime's `jit_opt` setting that was active when the cache was written. The cache key
  records it as `opt=`.

`Game O2 / cache O2` is accepted. `Game O2 / cache O0` is refused, and the same is true in reverse. The pack
build would otherwise contain AOT code generated at one level and cached code generated at another; the two must
agree for the pack to be coherent. The level the cache records is the JIT's, and the level the build was made with is
the AOT's. The launcher names both: its side panel says `AOT build: O3, its JIT cache O2: a pack needs one level` in
the failure colour, and the build itself says the same, with the two ways to align them. That refusal is the whole of
it. The two levels are independent everywhere else: the JIT cache is keyed by its own level, the ABI, the LLVM, the
runtime library, the CPU and the hooks, so another AOT level neither drops it nor makes it stale, and the shaders and
their cache are keyed by the GPU, its driver and the build, so no level touches them. A game plays the same at any
pair of levels until a pack is built.

On Android and iOS the JIT compiles at O0, as any other level breaks it there, and its level is no setting at all: a
phone's launcher lists no `JIT optimization` row. Every cache a phone writes is O0, and the launcher does not compare
the two levels there, as a phone's AOT level could only be made to match by recompiling every game at 0. A pack built
on a phone links the cached code as it is.

The key also contains the ABI, IR object version, target triple, LLVM version, runtime library, host CPU and its
features, the SVC/hook mode, and whether the SLP vectorizer, FPSR's flags and volatile guest accesses are on. Those
must match too; the driver validates the full key when it opens the cache. A runtime rebuild can therefore invalidate
an old cache even when the optimization level still matches. The build fails with the driver's reason; playing once
with the current runtime writes a fresh cache.

## Before building

1. Recompile the game once so it has a prepared game folder.
2. Set the game's `JIT optimization` (`jit_opt`) to the level you want the pack at.
3. Play the game normally for a while. The runtime writes `<title>.jitcache` into its cache folder as it
   compiles missed code in batches of up to 256 PCs, with bounded discovery work. A longer, representative session
   gives the pack more functions.
4. Set the launcher's `Optimization level` (`opt`) to the same level.

The cache is additive. A pack build never consumes or deletes it, so the same profile can be used again, and a
later session adds to it.

## Build from the launcher

1. Open **Games**.
2. Select the prepared Switch game. The side panel shows how its code was made (its stamp's made line: the `-O`, and
   the share of the modules' code the analysis made ahead of time, `?` for a game an older version made) and the cache
   it found:

   ```
   Made with: O2, AOT 81%
   JIT cache: 49 KB, O2
   AOT build: O2
   ```

   `Needs Recompile` in the failure colour in the first line's place means another build made the game, or with other
   mods: Play compiles it first.

   `levels differ` in the failure colour means the cache and the current AOT level do not match; the build is
   refused until they do. `JIT cache: none yet` means the game has not been played with the current runtime;
   play it once to collect a cache.
3. Press **B** (the footer says `B: build AOT pack` while a readable cache exists). On a touch screen, use the
   **AOT pack** item in the footer.
4. The normal recompile screen runs. The build does not launch the game. On success the footer reports
   `AOT pack built from N cached functions.` A zero-match build says so; the pack is still usable, but it has
   no cached variants. On failure the previous game module is left in place.
5. Play the game normally. Cached functions run from the pack; new or changed functions fall back to the JIT.

The launcher passes the cache file, the matched `-O` level, and `--hook-direct-calls` when the cache key says
the profile hooked direct calls. It builds the base game from the game's folder; add-on selection is not part
of a pack build.

## Build from the command line

The launcher action is the same build the driver already supports:

```sh
scarletnx -r path/to/game --jit-cache path/to/game.jitcache -O 2
```

Add `--hook-direct-calls` when the cache was written with `nvn_trace` on:

```sh
scarletnx -r path/to/game --jit-cache path/to/game.jitcache -O 2 --hook-direct-calls
```

`-O` and `--opt=2` set the same recompiler level; the launcher passes `-O` after the settings so the matched
level is authoritative. The driver writes the game module in place; the cache is only read.

The GUI locates the cache the same way the runtime does:

- `<game folder>/<title>.folders` names a `cache=` folder, if that folder exists; otherwise
- the runtime's user cache path for the game folder, which for an installed or portable data folder is
  `<data>/cache/recompiled/<title>/`; otherwise
- the game folder itself.

The file is `<that folder>/<title>.jitcache`.

## What the pack contains

The build reads the objects and function metadata from the cache and links them with the normal AOT module. The
runtime loads the resulting game module as a finished module. It does not link at runtime, does not read the
cache, and has no in-game pack menu. Missed functions are handled by the JIT exactly as they are in a build
without a pack.

The build prints the number of cached functions it used:

```
AOT pack: 1834 cached functions
```

Zero means the cache did not contribute a variant to this build. Check that the cache belongs to the same game
and runtime key, then play once more with the current runtime.

## Troubleshooting

| Message | What it means | What to do |
| --- | --- | --- |
| `levels differ` / `The JIT cache is O0, but the game's AOT optimization is O2` | The cache and the AOT build levels disagree. | Set `Optimization level` to the cache's level, or set `JIT optimization` to the AOT level and play once to write a matching cache. |
| `JIT cache: none yet` | No cache for this game under the current runtime. | Play the game once with the runtime you are building with. |
| `The JIT cache is unreadable` | The file is not a `SNXJIT01` cache, or its header is damaged. | Play the game again; the runtime writes a new cache. |
| `AOT pack: 0 cached functions (the cache did not match the game)` | The key or the game code changed; no cached function matched. | Play the current build again, or rebuild after the game/runtime change. |
| `JIT cache: ... key differs` from the driver | ABI, IR object version, target triple, LLVM, runtime library, CPU/features, optimization, hook mode or a codegen setting changed. | Use a cache written by the same runtime build, with matching settings. |

The cache can always be deleted safely. It only affects how much of the game runs from the pack; a missing or
partial cache is a normal state, and the runtime's JIT fills the gaps.
