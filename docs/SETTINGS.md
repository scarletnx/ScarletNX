<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Settings

## Index

* [Game](#game)
* [Display](#display)
* [Graphics](#graphics)
* [Performance](#performance)
* [On-screen controls](#on-screen-controls)
* [Controllers](#controllers)
* [Network](#network)
* [User](#user)
* [Debug](#debug)
* [Timed runs](#timed-runs)
* [3DS driver](#3ds-driver)
* [Vita driver](#vita-driver)
* [Launch](#launch)
  * [LLVM Internals](#llvm-internals)

A game keeps its settings in its `.ini` (`recompiled/<id>/<id>.ini`), a section each as below, and the launcher's
Settings show a group for each section ([GUI launcher](GUI.md)).

`Game.ini`: help rewritten at startup, most settings hot-reload. `--key=value` overrides one run.
Restart for `docked`, `fullscreen`, `language`, `resolution`, `anisotropy`, `memory_gb`, `memory_fit`, `heap_max_mb`,
`thread_stack_mb`, `map_limit_mb`, `map_limit_files`, `airplane_mode`, `ldn`, `room`, `pipeline_threads`, `prepare_shaders`, `jit_threads`, and the `[Debug]` settings other than `log`, `auto_stub_ipc` and `diag`.
`scarletnx --settings` prints each setting the launcher's Settings offer, one a line: its section, key, default,
type, range and help, tab-separated. Every setting, the recompiler's too, is described once in
`src/nx/runtime/settings.txt`, which the build generates the tables and `g_settings` from.

## Game

| Key | Default | Effect |
| --- | --- | --- |
| `source` | none | The dump the game was made from, which the recompile writes. Without `--extract-container` the game reads its ExeFS and RomFS from it. Left out, the `<id>.source` link older versions made is read instead |
| `state_slots` | `10` | The save state slots the menu's Save states page lists and F9 steps through, 1 to 32 |
| `state_auto_backup` | `1` | `1` saves the game to the auto slot before a save state is loaded over it, so the load can be undone |
| `save_layout` | `global` | Where the saves are: `global` is one folder of every game's, `save/` of the data folder, as the console keeps them (an Account save in `user.<uid>/<title ID>/`); `game` is each game's own, `save/` in its `<id>_data/`. A save the other one holds is played where it is, said once a run, until Settings > App > Move Old Files moves it ([Filesystem](FILESYSTEM.md#paths)) |
| `addons` | empty | The game's updates and DLCs played with it, made beside it in its folder: their title IDs, a comma between two. The launcher's list of them writes it ([Updates and DLC](USAGE.md#updates-and-dlc)). An update runs from its own program, which plays with this list's DLCs |
| `mods_off` | empty | The game's mods left out of play, folders of `mods/` in its data folder: their names, a `/` between two. Every other mod there is played ([Mods](USAGE.md#mods)) |
| `cheats_off` | empty | The game's cheats left out, of the Atmosphere-format `cheats/<build id>.txt` in its data folder: their names as their `[name]` lines give them, a `\|` between two. Every other cheat there runs, once per frame shown |
| `gpu_driver` | empty | The Vulkan driver the game runs with: empty is the system's, else a driver package the launcher's GPU Drivers added, by its folder's name in `gpu_drivers/` of the user's data folder ([Android](ANDROID.md)). Left out, the launcher passes the one its GPU Drivers picked for every game |
| `freedreno_env` | empty | Mesa's Freedreno and Turnip variables, set as the game starts: `NAME=value`, a `;` between two (`TU_DEBUG=sysmem`), names starting `TU_`, `FD_` or `IR3_`. The launcher's Freedreno Settings write it; left out, the launcher passes its own for every game |

## Display

| Key | Default | Effect |
| --- | --- | --- |
| `docked` | `1` | `1` is TV mode, `0` handheld mode (1280x720) |
| `fullscreen` | `0` | `1` starts fullscreen, except in timed test runs (`--seconds`, `--frames`). `-f` or `--fullscreen=0` sets it for one run. F11 or Alt+Enter switches without changing it |
| `language` | `1` | The game's language, English (US). The `.ini` lists the others, `0` to `17`. One the runtime doesn't know is English (US) |
| `showcase` | `0` | `1` draws player 1's buttons and sticks, as the game gets them, and the frames a second in a corner of the window, for a video. F6 turns it on and off |
| `showcase_corner` | `br` | The corner `showcase` draws in: `tl` top left, `tr` top right, `bl` bottom left, `br` bottom right |
| `stats` | `0` | `1` draws the frames a second, the last frame's time, the processor and memory the game takes (`?` where the system does not tell) and a graph of its last 120 frames' times (guides at 60 and 30 FPS, a frame slower than 30 FPS in red) in a corner over the game. F12 or the game's menu turns it on and off |
| `stats_corner` | `tl` | The corner `stats` draws in, as `showcase_corner`'s; in showcase's own corner it goes beside showcase's box, toward the middle |

## Graphics

| Key | Default | Effect |
| --- | --- | --- |
| `resolution` | `1` | Internal 3D scale, `0.25` to `16`: `1` native, `2` double, `0.75` and `0.5` faster. The launcher's Settings takes only the digits, three in all with two decimals at most, and refuses a value past the range, its dialog saying so. It warns below `0.5` and above `4`, where a game may use too much GPU memory or fall apart |
| `shadows` | `1` | `0` skips shadow maps |
| `vsync` | `1` | Images are shown only on the display's refresh, so nothing tears. `0` doesn't wait for it: the newest image is shown on the next refresh where the display can do that, else at once (which may tear). A display that can do neither waits for the refresh as `1` does |
| `frame_pacing` | `1` | With VSync and `fps_limit=game`: `0` paces the game's frames by a timer; `1` also has the next frame wait until the one before is on the display (`VK_KHR_present_wait` or `VK_GOOGLE_display_timing`), so frames don't queue for it; `2` also holds each frame of a game that asks for two refreshes or more a frame until its refresh, where the display can say when (`VK_EXT_present_timing` or `VK_GOOGLE_display_timing`). Where the display can't do one, the one before it |
| `anisotropy` | `0` | Texture filtering as the game asks. `2` to `16` sharpens surfaces seen at an angle |
| `vulkan_video` | `0` | `1` decodes H.264 movies on the GPU with Vulkan Video (FFmpeg's Vulkan decoder), where the GPU and its driver have an H.264 decode queue. Experimental and untested. `0` decodes them on the CPU, as VP9 movies always are |

## Performance

| Key | Default | Effect |
| --- | --- | --- |
| `fps_limit` | `game` | The pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30. `0` is unlimited, and `10` to `1000` is that many FPS |
| `memory_gb` | `0` | Memory the game sees. Below `6` it is the console's 3.2 GB, `6` gives 4.8 GB and `8` gives 6.8 GB. `12` is an experimental 10 GiB application budget with a 12 GiB heap region for newer games. It needs enough host memory and address space. |
| `memory_fit` | `0` (iOS `1`) | `1` tells the game no more of `memory_gb`'s memory than the device can still give it, measured as the game starts, less what the rest of ScarletNX takes, so a device with less than the console has is not run out of memory. The log says what it measured. `0` tells it all of it |
| `heap_max_mb` | `0` | The most heap the game can allocate, in MB. `0` is as much as the memory allows |
| `map_limit_mb` | `0` (iOS `256`) | The most of the game's RomFS and ExeFS files mapped into memory, in MB: the ExeFS and a `romfs.bin` at start, a `romfs` folder's files as the game first reads them. `0` is no limit. A file past it is read from disk when the game asks, which is slower and leaves address space for threads and graphics where a device has little |
| `map_limit_files` | `0` (iOS `2048`) | The most of those files mapped. `0` is no limit. Each mapped file holds a descriptor, of which a device may give the whole app a few thousand |
| `thread_stack_mb` | `32` (iOS `8`) | The host stack of each of the game's threads, in MB, which a game's deep calls take. Less leaves address space for more threads where a device has little, and a game that calls very deep may then stop |
| `pipeline_threads` | `0` | Threads that make the GPU's new pipelines while the game goes on, and those of earlier runs before it starts with `prepare_shaders=2`. `0` uses all the computer's threads but two, two to twelve. `1` to `32` is that many. More make a first run's new pipelines sooner, with less CPU left for the game |
| `async_pipelines` | `0` | `1` skips what a new shader draws while its pipeline is made, where the game would otherwise pause until it is ready. The game no longer stops where a new shader first appears, and the things that shader draws show up a few frames later. `0` waits for every pipeline |
| `prepare_shaders` | `2` | Prepares the shaders the game ships in its files and executables ahead of time, so the game's draws needn't make them and it stutters less. They are prepared again after ScarletNX, the GPU or its driver is updated. `0` (Off) makes each shader only when the game first draws with it. `1` (While playing) prepares them in the background, and the game uses them once all are ready. `2` (Before starting) prepares them behind a loading screen in the launcher theme's look, which also waits for the shader cache of earlier sessions, so an opening movie doesn't stutter. A key that types, an arrow or the keypad, a gamepad button other than Home, or a tap starts the game at once, and the rest is prepared while it runs. Esc quits (on Android, back opens the menu, whose Exit quits). F2 or a gamepad's Home opens the menu, and F11 or Alt+Enter switches fullscreen, without starting the game. The screen shows only when the wait passes a quarter second, its words are in English, and a timed run (`--seconds`) starts its clock after it |
| `jit_threads` | `0` | Threads compiling the game code the recompiler missed, which a thread waits for when it reaches it. `0` uses every CPU the game may run on but two, at most 4. They run below the game's threads |
| `jit_region_instructions` | `0` | The most instructions one branch graph of missed code may hold when the JIT compiles it, so a huge function cannot stall the game for seconds; branches out of it resume through the dispatch table. `0` is up to 4096, a larger graph cut into regions of 64 |
| `jit_live_budget` | `0` | The instructions beyond the missed code itself (its callees) that a miss the game waits on discovers. The background queue keeps its larger budget. `0` is 64 |
| `jit_opt` | `2` | LLVM's optimization of the code the JIT compiles: `0` none, `3` the most, `s` or `z` the smallest, as `opt` takes them. An [AOT pack](AOT_PACKS.md) built from the JIT cache needs it to match the recompile's. The next run compiles its JIT code again with it |

## On-screen controls

| Key | Default | Effect |
| --- | --- | --- |
| `touch` | `2` | The game's touch screen: `2` fingers and the mouse's left button, `1` fingers, `0` off |
| `controls_<ID>` (ID one of `l`, `r`, `zl`, `zr`, `minus`, `plus`, `capture`, `home`, `l3`, `r3`, `dpad`, `stick_l`, `stick_r`, `a`, `b`, `x`, `y`) | empty | A control's place, written once it is moved or reset: `x y size`, its middle across the screen from 0 to 1 and its size in percent (50 to 200), as `controls_a=0.930 0.470 100` |
| `controls_<ID>_shown` | `1` | `1` draws the control, `0` hides it; written once it is shown or hidden |
| `controls` | `0` (Android `1`) | The [on-screen controls](USAGE.md#on-screen-controls) shown |
| `controls_snap`, `controls_scale`, `controls_opacity`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics` | `0`, `50`, `100`, `1`, `1`, `1` | As [On-screen controls](USAGE.md#on-screen-controls) describes them. `controls_scale` runs from `0` to `100` for 50% to 150% |

## Controllers

| Key | Default | Effect |
| --- | --- | --- |
| `gamepad_swap_ab` | `0` | `1` swaps a gamepad's A and B. `0` sends A, B, X and Y where the console's are |
| `gamepad_map` | none | The player's map over that, below |
| `gamepad_deadzone` | `20` | The sticks' dead zone, in percent of their travel (`0` to `90`) |
| `gamepad_rumble` | `100` | How strongly gamepads vibrate when the game asks, in percent. `0` is never |
| `gamepad_background` | `0` | `1` keeps gamepads playing while the window is not focused |
| `gamepad_joycon_pair` | `1` | `1` plays a left and right Joy-Con together, `0` separately. Restart the game after changing it |
| `gamepad_p<N>_device` (N from 1 to 8) | empty | Player N's controller: empty takes the next to connect, else its SDL GUID as the log names it, `/2` after it for the second of two alike |
| `gamepad_p<N>_connected` | `1` | `0`: the game never sees player N. It always sees player 1, which the keyboard drives |
| `gamepad_p<N>_type` | `0` | The controller the game sees for player N: `0` as it is, `1` Pro Controller, `2` two Joy-Cons, `3` a left Joy-Con, `4` a right one |
| `gamepad_p<N>_swap_ab`, `gamepad_p<N>_rumble`, `gamepad_p<N>_map` | `-1`, `-10`, empty | Player N's own `gamepad_swap_ab`, `gamepad_rumble` and `gamepad_map`; at these values, every player's |

## Network

| Key | Default | Effect |
| --- | --- | --- |
| `network` | `0` | `0` reports offline and lets the game reach only loopback, which is not a network sandbox. `1` uses the computer's connection, and the resolver blocks known console service domains. Local wireless works either way (`ldn`) |
| `airplane_mode` | `0` | `1` is the console in airplane mode: offline whatever `network` says, no local wireless, as the game hears it, and in no Eden room |
| `ldn` | `lan` | Local wireless. `lan` is over the LAN with ldn_mitm's protocol, with another ScarletNX, a Ryujinx or a console running ldn_mitm: with `network=1` the console is the computer's address, with `0` it is `127.0.0.N`, one per ScarletNX on the machine, so two of them play together through loopback. `room` is through the Eden room `room` names, with the Edens and ScarletNXs in it. `off` is local wireless disabled, as the game hears it. Another word stops the game |
| `room` | empty | The Eden room `ldn=room` joins at start: `host` or `host:port` (`24872` without one); empty with `ldn=room`, or with a space in it, stops the game. The console is there as the user playing, by its nickname ([Users](USAGE.md#users)), at the address the room gives it (`192.168.x.y`), which nifm gives the game too, and the game's datagrams go through the room. A room with a password takes it from `users/online.ini` ([Folders](USAGE.md#folders)), a file only you may read (`chmod 600`, not checked on Windows) that holds `room_password=...` alone: another key or a section stops the game. A room that refuses stops the game and says why |

## User

| Key | Default | Effect |
| --- | --- | --- |
| `user` | `1` | The console's user the game plays as, `1` to `8`, as [Users](USAGE.md#users) lists them. A number with no user stops the game |

## Debug

| Key | Default | Effect |
| --- | --- | --- |
| `log` | `1` | Log detail. `2` logs everything, which adds runtime overhead |
| `log_max` | `16` | The game log's size in MiB, `1` to `1024`. Past it the log keeps the run's start and its last lines, saying so in the log and once over the game. The 5 last runs' logs are kept, the ones before this run's in `logs/` beside it |
| `emulated_cores` | `1` | `1` runs a guest thread only while it holds one of the console's cores, one thread per core at a time, in priority order. `0` runs every guest thread as soon as the host schedules it (`svcGetCurrentProcessorNumber` still answers the thread's core) |
| `auto_stub_ipc` | `1` | A service command the runtime doesn't know answers success, logged as `[undeclared]`, and a service it doesn't have answers every command that way. `0` answers them with the console's UnknownCommandId instead, which shows the gap where the game makes the call |
| `diag` | `0` | `1` saves the data of each new compute shader to `captures/compute`, and each new shader's snx-shaderc corpus entry to `captures/shaders`, for debugging |
| `vk_validation` | `0` | `1` turns on the Vulkan validation layer (`VK_LAYER_KHRONOS_validation`, from the Vulkan SDK or the distribution's validation layers package), which logs each misuse of Vulkan it finds. It slows the game down a lot. Without the layer installed the run only warns |
| `native_astc` | `1` | `1` samples ASTC textures as the game's blocks where the GPU samples ASTC (most phone and tablet GPUs), in a quarter to a 36th of the memory. `0` decodes them to RGBA8 on the GPU, as on GPUs without ASTC (most desktop ones), to compare the two. On such a GPU `1` is a warning at start ([ASTC](ASTC.md)) |
| `astc_bc` | `0` | `1`, on a GPU that samples BCn but not ASTC (most desktop ones), decodes ASTC textures and encodes them again as BC7, in a quarter of RGBA8's memory, close to what the blocks decode to but not always the same. `0` decodes them to RGBA8 exactly. Where the GPU samples ASTC it does nothing |
| `native_bc` | `1` | `1` samples BC1 to BC7 textures as the game's blocks where the GPU samples BCn (desktop GPUs and some phone ones). `0` decodes them on the GPU, as on GPUs without BCn (most phone ones), to compare the two on a desktop GPU |
| `native_etc2` | `1` | `1` samples ETC2 and EAC textures as the game's blocks where the GPU samples ETC2 (most phone and tablet GPUs), in a quarter to an eighth of the memory. `0` decodes them on the GPU, as on GPUs without ETC2 (most desktop ones), to compare the two. On such a GPU `1` is a warning at start |
| `cpu_extensions` | `0` | `1` decodes LSE atomics, RCpc loads, and PAuth branches and signing with authentication off, as an Ounce game needs. It gives no pointer authentication protection. Recompile the game with the same value as each run |
| `slp_vectorizer` | `0` | `1` runs LLVM's SLP vectorizer, which packs scalar instructions into SIMD ones, in the recompile and the JIT; the next recompile and the JIT's next run take it. Off while LLVM 23 can break a module with it and stop the recompile (llvm-project issue 231525). LLVM Internals' `llvm_slp_vectorization` leaves it running |
| `fpsr_flags` | `1` | `1` keeps FPSR's cumulative flags (IOC, DZC, OFC, UFC, IXC and QC) as the console raises them, which costs every floating-point or saturating operation a test of its result. `0` keeps only the results exact; a game that never reads FPSR (few do) runs floating-point and saturating code faster. The next recompile and the JIT's next run take it |
| `volatile_memory` | `0` | `1` makes every load and store of the game's code happen exactly where the game has it. `0` lets LLVM merge two loads of one address, or a store and a load, between the game's barriers, as the console's memory model allows, which is faster. `1` finds whether a bug is one of those. The next recompile and the JIT's next run take it |
| `ir_stats` | `0` | `1` has a recompile's log (one not run with `-q`) count each code unit's IR as the recompiler emits it: functions and those whose registers were promoted, calls by kind, register stores and reloads around calls, lookups, polls and code checks, before LLVM's passes |
| `ir_dump_pc` | empty | The address in hex (`100000`) of a function the recompiler found: the recompile prints its leaders, where its flags come from, the registers it promoted and its IR before and after the promotion |
| `ir_branch_profile` | empty | A file of branch counts, a line each: the branch's address and instruction word in hex, then how many times it was taken and not taken. The recompiler gives LLVM those odds for each branch whose address and word match. The next recompile and the JIT's next run take it |
| `gdb_jit` | `0` | `1`, with the GDB stub (`-S` or `-s`), hands the code the JIT compiles to GDB ([GDB](USAGE.md#gdb-linux)). GDB stops the game for each compile while it is attached, and each compile's object stays in memory for the run |
| `alias_pool` | `1` | `1` has a view of guest memory (MapMemory, MapProcessMemory, code memory's owner and slave) share its source's host pages, as the console's do, where the host's pages are 4 KiB (Linux and Android). `0` makes every view a copy, which reaches the source at its unmap or at a flushed line after the write, as hosts without a pool run (Windows among them): for tests of that path, and a diagnosis |
| `write_tracking` | `0` | How the runtime finds the guest pages written since the caches last looked ([Renderer coherence](ARCHITECTURE.md#renderer-coherence--lifetime)). `0` is the host's own: userfaultfd's write protection on Linux 6.7 and later, the reservation's write watch on Windows, page protection on the other hosts. `1` is page protection and the fault handler, as macOS, FreeBSD, Android and older Linux track (not on Windows, which says so at start). `2` is none: the caches compare guest memory's contents, to measure and test that path |
| `memory_traps` | `0` | How the runtime traps guest memory the GPU owns until the CPU touches it ([GPU ownership](GPU_OWNERSHIP.md#the-trap-model)). `0` is userfaultfd's minor faults where Linux has them, else page protection; none yet on Windows, macOS and FreeBSD. `1` is page protection and the fault handler, as Android and Linux before 5.13 trap (Linux and Android only: elsewhere it says so at start). `2` is none: a GPU result lands in guest memory at its fence, to measure and test that path |
| `host_page` | `0` | The page size the runtime works in, in bytes: a power of two larger than the host's own, up to 1 MiB. `16384` or `65536` runs as a host with 16 or 64 KiB pages does, where views copy and a host page holds several guest pages ([PowerPC64](PPC64.md#host-pages-larger-than-4-kib)). `0` is the host's own; any other value runs with the host's own too, said at start |
| `gpu_page_check` | `0` | `1` counts the GPU's written pages (`gpu/writes.c`) and the renderer's pending results (`gpu/vk/pending_pages.c`), which the runtime counts by GPU page, by guest address too, and each lookup translates its range through the GMMU as well: the log warns of each write or result only that finds. Slow: for checking those tables |
| `draw_state_check` | `0` | `1` has a draw that keeps the last draw's shaders and pipeline make them again to compare, and the log names each difference (a register missing from a group, an input that is not compared). Slow: for checking what a draw keeps |
| `profile` | `0` | `1` writes `profile.txt` every 10 seconds. The CPU profiler is Windows only |
| `hooks` | `1` | `1` runs the native handler of each of the game's exported functions the runtime has one for (`hle/hook/hooks.hook`: the C library's `memcpy`, `memmove` and `memset`) instead of its own code. `0` runs every function's own code, to tell whether a problem comes from a hook ([Hooks](HOOKS.md)) |
| `hooks_off` | empty | The families of hooks (`hooks.hook`'s family lines, such as `libc`) whose functions run their own code, separated by commas or spaces, to tell which family a problem comes from. Empty, every family is on |
| `hooks_verify` | empty | The families of hooks whose pure hooks are checked on every call: the function's own code runs on copies of the memory it writes, then the handler on the game's, and the log names each call whose result, registers or bytes differ ([Hooks](HOOKS.md#verify-mode)). Slow: for checking a hook, never for playing |
| `shader_corpus` | empty | A folder that keeps each translated shader for `snx-shaderc`. With it every shader is translated, and none is kept for the next run |
| `sm70_dump` | empty | A folder that keeps the bytes of each Ounce program (SM70+) the translator refuses, and of each block of shader memory with no whole program in it, for `tools/snx_sm70.c` (the game's own code: keep it local) |
| `gpu_trace_target` | empty | `address[:bytes]`: logs the clears and draws that write a render target starting in that GPU address range |
| `vk_gpu_profile` | `0` | `1` logs the GPU's time per rendering pass every 10 seconds |
| `vk_memory` | `0` | How the GPU reads guest buffers (vertices, indices, read-only storage; [GPU ownership](GPU_OWNERSHIP.md)). `1` copies them into the renderer's buffers when they change. `2` reads them where they are in guest memory (`VK_EXT_external_memory_host`), as the console's GPU does: for a GPU that shares system memory; on a discrete GPU it reads them across the bus, slowly, which still shows that the path is correct. `0` is `2` on a GPU that shares system memory, else `1` |
| `vk_buffer_stats` | `0` | `1` logs what the renderer copies between guest memory and its buffers |
| `vk_descriptors` | `0` | `1` gives graphics descriptors addressable Vulkan buffers where supported, with no descriptor-set allocations (compute keeps its own pools), and skips the guest's fragment barriers, which the tracking of targets and buffers orders. `0` uses descriptor sets for graphics too, and every fragment barrier |
| `vk_record` | `1` | `0` records Vulkan commands on the GPU thread instead of a thread of their own |
| `compute` | `1` | `0` skips every compute shader dispatch |
| `vk_max_allocation_mb`, `vk_budget_mb` | `0`, `0` | A smaller cap on each Vulkan allocation and on the GPU memory the caches keep, in MiB, for testing. `0` is the device's |
| `renderdoc`, `renderdoc_frames` | empty | RenderDoc captures to that path's stem of the presents listed (`500,800`, or `45s` for the first after 45 seconds) |
| `tracy_profiler` | `0` | `1` starts the run with the Tracy client up, in a build with `SNX_TRACY` ([Profiling](DEVELOPMENT.md#profiling)): connect the GUI to `127.0.0.1:8086`. A capture and the GUI cannot both have the run |
| `tracy_sampling` | `0` | `1` starts the sampler at the first connection, for the trace's flame graphs |
| `tracy_port` | empty | The port the client listens on and the capture connects to, `1` to `65535`. Empty is one of the run's own, so an earlier capture or GUI on 8086 is not in the way |
| `tracy_capture_tool` | empty | The `tracy-capture` to start: a path, or a name on PATH. Empty looks beside the runtime, then on PATH |
| `tracy_open_trace` | `1` | `1` opens the trace in `tracy-profiler` when the run ends |

## Timed runs

The unattended runs' settings ([Run / diagnose](DEVELOPMENT.md#run--diagnose)): read from the command line as the
`.ini`'s are, `--key=value`, but in no section, so never written back.

| Key | Default | Effect |
| --- | --- | --- |
| `seconds` | `0` | The run ends after this many seconds; `0`, it doesn't |
| `frames` | `-1` | The run ends after this many presents; `-1`, it doesn't |
| `shot` | empty | Where the run's screenshots go: the path's stem |
| `shot_every` | `0` | A screenshot every this many presents; `0`, at the end only |
| `press_len` | `300` | How long a scripted press is held, in ms |
| `script` | empty | The scripted input: `when:buttons`, a comma between two, when in ms or a present's number and `f` (`300f`) |
| `swkbd_text` | empty | The text the software keyboard answers instead of asking (shorter than the game's minimum: cancelled) |
| `pause` | empty | Pauses of the whole game, a comma between two: `at:length` pauses it at that many ms of the scripted input's clock for `length` ms, and logs whether guest memory changed while it was paused |
| `state_save` | empty | Save states the run writes, a comma between two: `at:slot` saves to that slot at that many ms of the scripted input's clock |
| `state_load` | empty | Save states the run loads, a comma between two: `at:slot` loads that slot at that many ms of the scripted input's clock |
| `state_check` | empty | Save state round trips, a comma between two: `at` saves the game into the check slot at that many ms of the scripted input's clock, loads it back at once, and logs whether guest memory and guest time came back the same |

## 3DS driver

`scarletnx-3ds --key=value` ([3DS](3DS.md)), in no section of the game's `.ini`.

| Key | Default | Effect |
| --- | --- | --- |
| `ctr_ipc_trace` | `0` | `1` logs each service request the 3DS game sends, its service and header, in the order sent |
| `ctr_svc_trace` | `0` | `1` logs each SVC the game makes that answers a failure (`r0` with its top bit set), `2` every SVC, with the thread, its pc and `r0` to `r3` before and after |
| `ctr_gpu_trace` | `0` | `1` logs each GX command the game queues, and each framebuffer swap the display takes or drops, and why |
| `ctr_uds_trace` | `0` | `1` logs each local wireless packet of the game that goes through an Eden room ([local wireless](3DS.md#experimental-local-wireless-over-rooms)): its direction (sent, received, delivered), the time, its size and its first 96 bytes in hex |
| `ctr_jit_trace` | `0` | `1` logs each block the runtime compiles while the game runs (the static module lacked it), Thumb ones with the pc's low bit set as the hints file writes them |
| `ctr_auto_stub_ipc` | `1` | `1` answers success to a command of a 3DS service the runtime doesn't know, and has a stand-in answer each command of a service it doesn't have, each logged the first time, as `auto_stub_ipc` does for a Switch game. `0` stops the run at the first, saying which, but for the services `--ipc-stubs` names |
| `ctr_script` | empty | The game's scripted input (`--script=` too): `when:buttons`, a comma between two, when in ms from the start and buttons in hex as `hid:USER`'s pad has them (1 A, 2 B, 4 Select, 8 Start, 10 Right, 20 Left, 40 Up, 80 Down, 100 R, 200 L, 400 X, 800 Y), each held for `press_len` ms |

## Vita driver

`scarletnx-vita --key=value` ([PS Vita](VITA.md)), in no section of the game's `.ini`.

| Key | Default | Effect |
| --- | --- | --- |
| `vita_hle_trace` | `0` | `1` logs each call of an imported function the runtime doesn't implement, `2` every call, with its thread, `r0` to `r3` and its result |
| `vita_jit_trace` | `0` | `1` logs each block the runtime compiles while the game runs (the game's module lacked it), Thumb ones with the pc's low bit set as the hints file writes them |
| `vita_gxm_trace` | `0` | `1` logs each scene, draw and display queue entry the game makes through sceGxm |
| `vita_shader_corpus` | empty | A folder the runtime writes each GXP program it translates to, with what it was translated for, as `<hash>.gxp`, which `tools/snx_vita_usse.c` translates again (the game's own code: keep it local) |
| `vita_script` | empty | The game's scripted input (`--script=` too): `when:buttons`, a comma between two, when in ms from the start and buttons in hex as SceCtrl has them (1 Select, 8 Start, 10 Up, 20 Right, 40 Down, 80 Left, 100 L, 200 R, 1000 Triangle, 2000 Circle, 4000 Cross, 8000 Square, and the left stick pushed all the way: 1000000 right, 2000000 left, 4000000 up, 8000000 down), each held for `vita_press_len` ms |
| `vita_press_len` | `300` | How long each press of the script is held, in ms (`--press_len=` too) |
| `vita_shot_every` | `0` | With `--shot=PATH`, also a screenshot every this many vblanks (60 a second), `PATH_0000.ppm` on; `0`, only the last frame's |
| `vita_lockstep` | `-1` | From this import call on (`0` is the first), every instruction group of the game also runs in Unicorn, and the first difference in registers, flags or written memory stops the run with both states ([Checking the CPU backend](VITA.md#checking-the-cpu-backend)). Only in a build configured with `-DSNX_VITA_LOCKSTEP=ON`: another build says so at start. `-1` is off |
| `vita_lockstep_steps` | `0` | With `vita_lockstep`, how many instruction groups are compared before the game runs on without Unicorn. `0` has no limit |

## Launch

The launcher's own settings, and the recompiler's, which the launcher and the driver pass on as `--key=value`
([Usage](USAGE.md)): the launcher's Settings > Launch.

| Key | Default | Effect |
| --- | --- | --- |
| `gdbstub` | `off` | `on` starts the game with the GDB stub on port 1234 ([GDB](USAGE.md#gdb-linux)) |
| `tmpfs` | `off` | `on` extracts to tmpfs, in memory (the temporary folder's `scarletnx/work/<title ID>`), which the game reads its RomFS from (`--tmpfs`) |
| `extract` | `off` | `on` extracts a dump's ExeFS and RomFS into its game's folder (`--extract-container`). Off, the game reads them from the dump |
| `tracy_capture` | `off` | `on` records Switch runs to `captures/` beside the game and opens the trace in the profiler when the run ends, in a build with `SNX_TRACY`; `tracy-capture` must be on PATH. Off, the client does not start, and F10 starts a 60-second capture |
| `opt` | `2` | LLVM's optimization of the game's code when it is recompiled (`-O`): `0` none, `3` the most, `s` or `z` the smallest. A game made already keeps its code until it is recompiled |
| `llvm_batch` | `full` | What LLVM runs over each code unit once its functions are cleaned, when the game is recompiled: `full`, the passes of the optimization level; `ipo`, only inlining and a cleanup after it, which recompiles in about two thirds of the time for slightly bigger code, up to a third faster or slower than `full`'s. Not at `-O0` |
| `aot_threads` | `0` | The threads compiling the game's code when it is recompiled (`--jobs`): `0` is one for each of the computer's cores |
| `unit_instructions` | `90000` (Android `20000`, iOS `20000`) | The instructions a code unit takes, whole functions at a time, when the game is recompiled (`--chunk`). Each is compiled on its own, and a smaller one takes less memory at once, which a phone needs |
| `unit_cache_gb` | `0` | The most the cache of compiled code units may hold, in GB (`units/` of the cache folder): past it the units reused longest ago are removed. `0` is no limit. The cache always leaves a tenth of its disk free |

### LLVM Internals

Every pass of LLVM's pipelines can be turned off for the game's code, `llvm_pass_<pass>=0`, and each option LLVM's C
API sets on the pass builder and the target machine set, `llvm_<option>=N`. `scarletnx --settings=recompile` lists
them all, as the launcher's LLVM Internals page shows them: the passes are those the build's LLVM prints for each
level. These have names of their own in the launcher:

| Key | Default | Effect |
| --- | --- | --- |
| `llvm_pass_instcombine` | `1` | Instruction combining: combines instructions into fewer, simpler ones |
| `llvm_pass_inline` | `1` | Inlining: inlines calls into their callers |
| `llvm_pass_sroa` | `1` | Scalar replacement of aggregates: splits stack objects into values kept in registers |
| `llvm_pass_gvn` | `1` | Global value numbering: removes redundant computations and loads |
| `llvm_pass_simplifycfg` | `1` | Control flow simplification: merges, removes and simplifies basic blocks |
| `llvm_pass_loop_unroll` | `1` | Loop unrolling pass: unrolls loops with known trip counts |
| `llvm_pass_loop_vectorize` | `1` | Loop vectorization pass: vectorizes loops with SIMD instructions |
| `llvm_pass_slp_vectorizer` | `1` | SLP vectorization pass: packs independent scalar instructions into SIMD ones |
| `llvm_pass_licm` | `1` | Loop invariant code motion: hoists computations that do not change across a loop out of it |
| `llvm_loop_vectorization` | LLVM's own | Whether the pipeline vectorizes loops (`LLVMPassBuilderOptionsSetLoopVectorization`) |
| `llvm_slp_vectorization` | LLVM's own | Whether the pipeline packs scalar instructions into SIMD ones (`LLVMPassBuilderOptionsSetSLPVectorization`) |
| `llvm_loop_unrolling` | LLVM's own | Whether the pipeline unrolls loops (`LLVMPassBuilderOptionsSetLoopUnrolling`) |
| `llvm_fast_isel` | LLVM's own | Whether code generation selects instructions with FastISel, quicker and worse (`LLVMSetTargetMachineFastISel`) |
| `llvm_global_isel` | LLVM's own | Whether code generation selects instructions with GlobalISel (`LLVMSetTargetMachineGlobalISel`) |
| `llvm_inliner_threshold` | LLVM's own | How large a function the inliner inlines; LLVM picks one for the level until it is set (`LLVMPassBuilderOptionsSetInlinerThreshold`) |
