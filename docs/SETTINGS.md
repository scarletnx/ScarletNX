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
| `anisotropy` | `0` | Texture filtering as the game asks. `2` to `16` sharpens surfaces seen at an angle |

## Performance

| Key | Default | Effect |
| --- | --- | --- |
| `fps_limit` | `game` | The pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30. `0` is unlimited, and `10` to `1000` is that many FPS |
| `memory_gb` | `0` | Memory the game sees. Below `6` it is the console's 3.2 GB, `6` gives 4.8 GB and `8` gives 6.8 GB. Games that run out of memory on 3.2 GB need `6` |
| `memory_fit` | `0` (iOS `1`) | `1` tells the game no more of `memory_gb`'s memory than the device can still give it, measured as the game starts, less what the rest of ScarletNX takes, so a device with less than the console has is not run out of memory. The log says what it measured. `0` tells it all of it |
| `heap_max_mb` | `0` | The most heap the game can allocate, in MB. `0` is as much as the memory allows |
| `map_limit_mb` | `0` (iOS `256`) | The most of the game's RomFS and ExeFS files mapped into memory, in MB: the ExeFS and a `romfs.bin` at start, a `romfs` folder's files as the game first reads them. `0` is no limit. A file past it is read from disk when the game asks, which is slower and leaves address space for threads and graphics where a device has little |
| `map_limit_files` | `0` (iOS `2048`) | The most of those files mapped. `0` is no limit. Each mapped file holds a descriptor, of which a device may give the whole app a few thousand |
| `thread_stack_mb` | `32` (iOS `8`) | The host stack of each of the game's threads, in MB, which a game's deep calls take. Less leaves address space for more threads where a device has little, and a game that calls very deep may then stop |
| `pipeline_threads` | `0` | Threads that make the GPU's new pipelines while the game goes on, and those of earlier runs before it starts with `prepare_shaders=2`. `0` uses all the computer's threads but two, two to twelve. `1` to `32` is that many. More make a first run's new pipelines sooner, with less CPU left for the game |
| `async_pipelines` | `0` | `1` skips what a new shader draws while its pipeline is made, where the game would otherwise pause until it is ready. The game no longer stops where a new shader first appears, and the things that shader draws show up a few frames later. `0` waits for every pipeline |
| `prepare_shaders` | `2` | Prepares the shaders the game ships in its files and executables ahead of time, so the game's draws needn't make them and it stutters less. They are prepared again after ScarletNX, the GPU or its driver is updated. `0` (Off) makes each shader only when the game first draws with it. `1` (While playing) prepares them in the background, and the game uses them once all are ready. `2` (Before starting) prepares them behind a loading screen in the launcher theme's look, which also waits for the shader cache of earlier sessions, so an opening movie doesn't stutter. A key that types, an arrow or the keypad, a gamepad button other than Home, or a tap starts the game at once, and the rest is prepared while it runs. Esc quits (on Android, back opens the menu, whose Exit quits). F2 or a gamepad's Home opens the menu, and F11 or Alt+Enter switches fullscreen, without starting the game. The screen shows only when the wait passes a quarter second, its words are in English, and a timed run (`--seconds`) starts its clock after it |
| `jit_threads` | `0` | Threads compiling the game code the recompiler missed, which a thread waits for when it reaches it. `0` uses every CPU the game may run on but two, at most 4. They run below the game's threads |

## On-screen controls

| Key | Default | Effect |
| --- | --- | --- |
| `touch` | `2` | The game's touch screen: `2` fingers and the mouse's left button, `1` fingers, `0` off |
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
| `auto_stub_ipc` | `1` | A service command the runtime doesn't know answers success, logged as `[undeclared]`, and a service it doesn't have answers every command that way. `0` answers them with the console's UnknownCommandId instead, which shows the gap where the game makes the call |
| `diag` | `0` | `1` saves the data of each new compute shader to `captures/compute`, and each new shader's snx-shaderc corpus entry to `captures/shaders`, for debugging |
| `vk_validation` | `0` | `1` turns on the Vulkan validation layer (`VK_LAYER_KHRONOS_validation`, from the Vulkan SDK or the distribution's validation layers package), which logs each misuse of Vulkan it finds. It slows the game down a lot. Without the layer installed the run only warns |
| `slp_vectorizer` | `0` | `1` runs LLVM's SLP vectorizer, which packs scalar instructions into SIMD ones, in the recompile and the JIT; the next recompile and the JIT's next run take it. Off while LLVM 23 can break a module with it and stop the recompile (llvm-project issue 231525). LLVM Internals' `llvm_slp_vectorization` leaves it running |
| `fpsr_flags` | `1` | `1` keeps FPSR's cumulative flags (IOC, DZC, OFC, UFC, IXC and QC) as the console raises them, which costs every floating-point or saturating operation a test of its result. `0` keeps only the results exact; a game that never reads FPSR (few do) runs floating-point and saturating code faster. The next recompile and the JIT's next run take it |
| `gdb_jit` | `0` | `1`, with the GDB stub (`-S` or `-s`), hands the code the JIT compiles to GDB ([GDB](USAGE.md#gdb-linux)). GDB stops the game for each compile while it is attached, and each compile's object stays in memory for the run |
| `profile` | `0` | `1` writes `profile.txt` every 10 seconds. The CPU profiler is Windows only |
| `shader_corpus` | empty | A folder that keeps each translated shader for `snx-shaderc`. With it every shader is translated, and none is kept for the next run |
| `gpu_trace_target` | empty | `address[:bytes]`: logs the clears and draws that write a render target starting in that GPU address range |
| `vk_gpu_profile` | `0` | `1` logs the GPU's time per rendering pass every 10 seconds |
| `vk_buffer_stats` | `0` | `1` logs what the renderer copies between guest memory and its buffers |
| `vk_record` | `1` | `0` records Vulkan commands on the GPU thread instead of a thread of their own |
| `compute` | `1` | `0` skips every compute shader dispatch |
| `vk_max_allocation_mb`, `vk_budget_mb` | `0`, `0` | A smaller cap on each Vulkan allocation and on the GPU memory the caches keep, in MiB, for testing. `0` is the device's |
| `renderdoc`, `renderdoc_frames` | empty | RenderDoc captures to that path's stem of the presents listed (`500,800`, or `45s` for the first after 45 seconds) |
