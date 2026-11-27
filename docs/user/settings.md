<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Settings

## Index

* [How a settings file is read](#how-a-settings-file-is-read)
* [Game](#game)
* [Display](#display)
* [Graphics](#graphics)
  * [Vulkan Extensions](#vulkan-extensions)
* [Audio](#audio)
* [Performance](#performance)
* [On-screen controls](#on-screen-controls)
* [Controllers](#controllers)
* [Shortcuts](#shortcuts)
* [Network](#network)
* [Online](#online)
* [User](#user)
* [Debug](#debug)
* [Timed runs](#timed-runs)
* [3DS driver](#3ds-driver)
* [Vita driver](#vita-driver)
* [Linux driver](#linux-driver)
* [Android driver](#android-driver)
* [DS driver](#ds-driver)
* [Launch](#launch)
* [GBA](#gba)

A game keeps its settings in its `.ini` (`recompiled/<id>/<id>.ini`), a section each as below, and the launcher's
Settings show a group for each section ([GUI launcher](gui.md)).

`Game.ini`: help rewritten at startup, most settings hot-reload. A setting given on the command line overrides one
run: its key with a `-` for each `_` (`state_slots`: `--state-slots=5`), on every driver; a value it does not take is
said by its key, as the `.ini`'s is. The key as the `.ini` writes it (`--state_slots=5`) is the older spelling, still
read and said once a run ([Deprecations](deprecations.md)).

A game's own picks, `<id>.picks` beside its `.ini` (`recompiled/<id>/<id>.picks`), hold what was set for that game
alone, by section as the `.ini` has them: each setting its menu changes (TV mode, Stats, Volume, the controls' and
controllers' pages, the shortcuts' keys, F4, F6) is kept there, the controls' places and the rest of the game's own
(`[Game]`, `in-game` in `settings.txt`) in its `.ini`. A run takes a setting from its own command line, then from the
game's picks, then from the launcher's Settings, which every game gets (the launcher passes them on the command line
with `--launcher-picks=1`, which puts the game's picks over them), then from the `.ini`, else its default. The `.ini`
keeps its own values meanwhile. The log's `settings: this game's own, from <path>:` line lists the picks, and says the
value a run's own `--key=value` plays over one. A change to the file is read while the game runs, as the `.ini`'s is; a
key no pick takes is kept in the file and does nothing ([below](#how-a-settings-file-is-read)); with no pick left the
file is removed.
In the game's menu a value the picks hold has `*` after it, and Delete or a controller's Y takes it out: the value
goes back to the launcher's, else the `.ini`'s, at once. Its **Settings** page sets the rest of these sections for the
game alone ([Usage](usage.md#playing)).

From the game's first frame, for 30 seconds, a notification over the game names each setting off its default that
changes how fast the game runs where nothing on the screen shows it, so a choice left from a test is not forgotten:
`perf` in `settings.txt`. They are TV mode, `tv_refresh_rate`, `tv_resolution`, `resolution`, `render_upscaler`,
`internal_resolution`, `fsr_sharpness`, `vsync`, `frame_pacing`, `anisotropy`, `vulkan_video`, `apple_video`,
`vulkan_driver`, `vertex_input_dynamic_state`, `vulkan_ban_ignore`, `astc_mode`, `texture_shed`, `texture_half_res`,
`astc_lines_bc1`, `texture_sysmem`, `bc7_queue`, `astc_compute_queue`, `astc_quick_mtexels`, `astc_background`, every
`vk_ext_*`, every Performance setting, `gpu_driver`, `freedreno_env`, `microphone`, `mic_noise_reduction`, and how the
game's code was made: `unit_instructions`, as its stamp's made line says it
([Usage](usage.md)). The screen shows the others by itself (`stats`, `showcase`, `fullscreen`, `controls`, `shadows`,
`upscaler`, `anti_aliasing`, `hdr_game`), and the `[Debug]` settings are named at the top of the picture the whole
run. The notification gives their keys alone, a line each: after a gamepad the game's own (its picks, its `.ini`),
after a globe the launcher's for every game, this run's command line's and how the code was made. A line names as
many as fit it, three lines at most, and the last says how many more there are. A value the runtime gives a kind of
game, its setting's Auto, has a line of its own after an info mark, with its word: `emulated_cores_mode`'s Auto on an
Ounce program, `emulated_cores_mode Off, for this kind of game`. The log's `settings:` line says it too, with how to run
it otherwise.

Restart for `docked` (`Auto` follows the host's power while the game runs, and `hotkey_tv_mode` and the game's menu's
**TV mode** change the mode for the run), `tv_refresh_rate`, `tv_resolution`, `fullscreen`, `transition_screen`, `language`, `resolution`,
`anisotropy`, `hdr_game`, `hdr_paperwhite`, `hdr_peak`, `memory_gb`, `memory_fit_mode`, `heap_max_mb`, `thread_stack_mb`, `map_limit_mb`, `map_limit_files`,
`airplane_mode`, `ldn`, `room`, `pipeline_threads`, `pipeline_libraries`, `prepare_shaders`,
`compress_prepared_shaders`, `jit_threads`, `gpu_pstate`, `vertex_input_dynamic_state`, `astc_mode`, every `vk_ext_*`, and
the `[Debug]` settings other than `log`, `auto_stub_ipc`, `diag` and `tracy_capture_seconds`.
`scarletnx --settings` prints each setting the launcher's Settings offer, one a line: its section, key, default,
type, range and help, tab-separated. Every setting, the recompiler's too, is described once in
`src/nx/runtime/settings.txt`, which the build generates the tables and `g_settings` from.

## How a settings file is read

Every file of settings ScarletNX writes and reads follows one rule: a game's `.ini` and picks, the launcher's
`settings.ini`, `app.ini`, `folders.txt`, `recent.txt`, `removed.txt` and `joycon2.txt`, `users/users.ini` and
`users/online.ini`, `setup.ini`, a theme's `theme.ini`, a game's `.folders`, a 3DS game's `.ini` and picks, and the
launcher's record of its last run.

- A key, a section or a line it does not know (another version's, another program's, a typo) changes nothing it
  reads, stays in the file as it was when the file is written again, and is named once a run in the log or the
  launcher's message by its key alone, never by its value, which can be a secret. A line with no `=` is named by its
  number.
- A key given twice takes its last line's value, and both lines are said, with their values but in a file of secrets.
- A value a setting does not take is said with the value it gives and the one in effect.
- A file that is not there says nothing: each setting takes its default. One that is there and cannot be read is said
  with the system's reason, and nothing is written over it.
- A file is written beside itself first, then put in its place, so a stop in the middle leaves the one before whole.
- What a file's own rule refuses stays refused: `users.ini`'s users out of order, a uid or a nickname the account
  service would not keep; `online.ini`, which holds a secret, when others can read it, when it has a section, or a key
  that starts with `room_` and is not `room_password`.

Clean Old Settings takes out the keys an older version wrote that this one dropped ([Deprecations](deprecations.md)),
never another version's.

## Game

| Key | Default | Effect |
| --- | --- | --- |
| `source` | none | The dump the game was made from, which the recompile writes. Without `--extract-container` the game reads its ExeFS and RomFS from it. Left out, the `<id>.source` link older versions made is read instead |
| `state_slots` | `10` | The save state slots the menu's Save states page lists and F9 steps through, 1 to 32 |
| `state_auto_backup` | `1` | `1` saves the game to the auto slot before a save state is loaded over it, so the load can be undone |
| `save_layout` | `global` | Where the saves are: `global` is one folder of every game's, `save/` of the data folder, as the console keeps them (an Account save in `user.<uid>/<title ID>/`); `game` is each game's own, `save/` in its `<id>_data/`. A save the other one holds is played where it is, said once a run, until Settings > App > Move Old Files moves it ([Filesystem](../platforms/nx/filesystem.md#paths)) |
| `addons` | empty | The game's updates and DLCs played with it, made beside it in its folder: their title IDs, a comma between two. The launcher's list of them writes it ([Updates and DLC](usage.md#updates-and-dlc)). An update runs from its own program, which plays with this list's DLCs |
| `mods_off` | empty | The game's mods left out of play, folders of `mods/` in its data folder: their names, a `/` between two. Every other mod there is played ([Mods](usage.md#mods)) |
| `cheats_off` | empty | The game's cheats left out, of the Atmosphere-format `cheats/<build id>.txt` in its data folder: their names as their `[name]` lines give them, a `\|` between two. Every other cheat there runs, once per frame shown |
| `gpu_driver` | empty | The Vulkan driver the game runs with: empty is the system's, else a driver package the launcher's GPU Drivers added, by its folder's name in `gpu_drivers/` of the user's data folder ([Android](../hosts/android-app.md)). Left out, the launcher passes the one its GPU Drivers picked for every game. The Vita runtime takes it too (`--gpu-driver=`), through the same loader; a Vita game on a driver without `VK_EXT_external_memory_host` copies guest memory, which is slower |
| `freedreno_env` | empty | Mesa's Freedreno and Turnip variables, set as the game starts: `NAME=value`, a `;` between two (`TU_DEBUG=sysmem`), names starting `TU_`, `FD_` or `IR3_`. The launcher's Freedreno Settings write it; left out, the launcher passes its own for every game |
| `microphone` | `off` | The microphone an Ounce game hears as the console's own: `off` none, so the game hears silence until the player picks one, empty the system's default recording device, else the first recording device whose name holds this text, in any case. It records only while the game records, and a change takes effect at the game's next recording. The launcher lists the recording devices by name, read as the Audio group opens. The log names the device by its format, never by its name |
| `mic_noise_reduction` | `1` | How the noise the microphone picks up is lowered before the game hears it (the console's audio system has a noise reduction for its microphone, of a strength not known). `1`, Neural: RNNoise 0.1.1, a small recurrent network trained on speech, built in, in frames of 10 ms at 48 kHz; a stream at another rate gets `2`. On the test's synthetic speech it lowered a steady noise by 27 dB and background voices by 6 dB between words, and cost about 0.1 ms a frame. `2`, Spectral: the Speex DSP library's noise suppression (the system's `libspeexdsp`, else none), up to 30 dB, which lowered the steady noise by 18 dB and the voices by 1 dB. Both add 10 ms of delay. `0` gives the game the microphone as it is. It is read every 10 ms, so a change takes effect at once. The log names the method that runs, and the setting's value and why when another runs |

## Display

| Key | Default | Effect |
| --- | --- | --- |
| `docked` | `1` | `1` is TV mode, `0` handheld mode (1280x720), `2` Auto: TV mode while the host is on its charger, handheld mode on its battery. Auto switches while the game runs. `hotkey_tv_mode` (Ctrl+D) and the game's menu's **TV mode** step On, Off and Auto for the run, the `.ini`'s value as it was |
| `tv_refresh_rate` | `1` | The refresh rates an Ounce game is told its display takes: `0` is 60 Hz, `1` is 120 Hz too where the window's display refreshes at 119 Hz or more, `2` is 120 Hz too on any display. A game that turns 120 Hz on is then paced at 120 frames a second with `fps_limit=game`, and the log says `gsv: the display runs at 120 Hz` |
| `tv_resolution` | `0` | The picture an Ounce game is told the TV shows in TV mode (its default display resolution): `0` is 1920x1080 (a game's parts made for a 4K TV cost performance, so 4K is the player's choice), `1` is 3840x2160 where the window's display's mode is that large (its pixels, whatever the desktop's scale), else 1920x1080, `2` is 3840x2160 on any display. A Switch game is told 1920x1080 |
| `fullscreen` | `0` | `1` starts fullscreen, except in timed test runs (`--seconds`, `--frames`) whose command line has no `-f` or `--fullscreen=1`. `-f` or `--fullscreen=0` sets it for one run. F11 or Alt+Enter switches without changing it. A 3DS game takes it too (`--fullscreen=1`, not in `--seconds`, `--frames` or `--max-blocks` runs, nor in a `-H` run), with the same keys |
| `transition_screen` | `1` | `1`: when a collection's program starts another (a game picked in its menu), the new program's window shows the loading screen from a quarter second after it opens until the game draws: the game's name and `(Program N)`, the game's cover, and as its steps the program's number, the one that started it, the user, the version and update, the DLCs, what was handed over, the GPU, the title ID and the build, `?` where not known. The launcher's run screen shows that program's page while no window is open (with the recompile of a program not made). `0`: the window stays black until the game draws, and the run screen shows the log |
| `language` | `1` | The game's language, English (US). The `.ini` lists the others, `0` to `17`. One the runtime doesn't know is English (US) |
| `showcase` | `0` | `1` draws player 1's buttons and sticks, as the game gets them, and the frames a second in a corner of the window, for a video. F6 turns it on and off |
| `showcase_corner` | `br` | The corner `showcase` draws in: `tl` top left, `tr` top right, `bl` bottom left, `br` bottom right |
| `stats` | `0` | `1` draws the frames a second, the last frame's time, the processor and memory the game takes, the share of the time the GPU's graphics queue was busy and its busy time a frame over the last second, from Vulkan timestamps, the video memory the game uses of the driver's budget for it, from `VK_EXT_memory_budget` (without it, what the game took from the device's heap and the heap's size, marked `ours/heap`; `?` where the system or the device does not tell), and a graph of its last 120 frames' times (guides at 60 and 30 FPS, a frame slower than 30 FPS in red) in a corner over the game. Under the graph, while they apply: the work that holds frames back, a line each while the last second had some, that second's and the run's (`Compiling: 3 / 1240`, the pipelines no earlier run left in the shader cache; `Encoding: 12 / 5300`, the BC7 encoder's runs on ASTC levels, `astc_mode`), that the capture key (`hotkey_capture`) takes a RenderDoc capture (`renderdoc`), the captures written and the last one's file, and the debugger tracing the game (its name on Linux, Android and FreeBSD, `?` on Windows and macOS). F12 or the game's menu turns it on and off |
| `stats_corner` | `tl` | The corner `stats` draws in, as `showcase_corner`'s; in showcase's own corner it goes beside showcase's box, toward the middle |

## Graphics

| Key | Default | Effect |
| --- | --- | --- |
| `resolution` | `1` | Resolution scale: the scale of every 3D render target the game draws, `0.25` to `16`: `1` native, `2` double, `0.75` and `0.5` faster. It scales the game's whole rendering, its internal picture and its output alike, as an emulator's resolution scaler does, and is not the size a game renders at before its own upscaler (`internal_resolution`). At `1`, its default, a game module may set it from `internal_resolution` (the game's own size times it reaching the target), and the log says so (`hooks: module ... sets the resolution multiplier to ...`). Any other value is the user's, which a module's leaves as it is. The launcher's Settings takes only the digits, three in all with two decimals at most, and refuses a value past the range, its dialog saying so. It warns below `0.5` and above `4`, where a game may use too much GPU memory or fall apart |
| `internal_resolution` | `0` | The internal resolution target: the size a game renders at before its own upscaler (a Switch 2 game's DLSS, or the one a game module replaces) turns it into the output. `0` = the game's choice, `1` = the output's size (sharpest, slowest), `2` = 1/1.5, `3` = 1/1.7, `4` = 1/2, `5` = 1/3 of it (fastest). A Switch 2 game is told it as the render size its DLSS asks for. A game that picks its sizes itself takes it through its game module, which may set the output to the display's size and the resolution multiplier too. `render_upscaler` turns that picture into the output, and `resolution` scales both. It takes the place of `dlss_render_size` ([Deprecations](deprecations.md)) |
| `render_upscaler` | `0` | Game's upscaler (DLSS, FSR 2): what turns the game's internal picture (`internal_resolution`) into its output: a Switch 2 game's DLSS evaluations, and the upscalers that hook modules replace ([Upscalers](../layers/gpu/upscalers.md)). A game that has neither takes nothing from it: `upscaler` is the window's filter. `0` = the best one that can run, `1` = FSR 2.2 (temporal), `2` = FSR 1 (spatial: one frame, no jitter or motion vectors needed), `3` = none: a filtered copy of the picture onto the output (1:1 at the native size, soft below it, and the picture shakes with the game's camera jitter), `4` = NVIDIA DLSS (Windows, an NVIDIA GPU and NVIDIA's runtime in `dlss_runtime`'s folder: [DLSS](dlss.md)), `5` FSR 3.1 or 4 and `6` XeSS, which are not built in yet. Where the one asked for can't run, or its caller can't take it (a DLSS evaluation takes a temporal one or None, a hook module the kinds it names), the best one that can stands in, and the log says why. Its `3` takes the place of `dlss_upscaler`'s `0` ([Deprecations](deprecations.md)) |
| `render_anti_aliasing` | `0` | Anti-aliasing of the game's 3D picture where no temporal upscaler runs (`render_upscaler` `3` None, or a hook module's copy or spatial upscale), on the picture at the render size, before the upscale and before the game draws its interface over it. `1` = SMAA 1x. `2` = SMAA T2x: each frame taken back by the camera's jitter, then 1x, then resolved with the frame before, reprojected by the game's motion vectors and weighted by how alike their motion is: steady, whatever `jitter` says, and smoother than 1x where the jitter moved edges. It takes a game's DLSS evaluations: where no motion vectors are given (a hook module's upscale) it is 1x, said once in the log. A temporal upscaler (FSR 2, DLSS) resolves edges itself and takes none. `0` = none. SMAA S2x and 4x need the game drawn with multisampling, which a game made without it can't be |
| `smaa_quality` | `2` | SMAA's preset: its edge threshold and how far it searches along an edge. `0` = Low (threshold 0.15, 4 steps), `1` = Medium (0.1, 8), both without the diagonal and corner searches, `2` = High (0.1, 16, diagonals 8), `3` = Ultra (0.05, 32, diagonals 16). Higher finds more edges and costs more |
| `smaa_edges` | `0` | What SMAA finds edges in: `0` = the picture's luma, `1` = its color (more edges, between colors of one brightness), `2` = the depth (geometry's edges alone, none inside textures), where the game's depth is given at the render size, else luma, said once in the log |
| `jitter` | `0` | The camera's jitter where no temporal upscaler runs: a game made for DLSS moves its camera by a fraction of a pixel each frame, which a temporal upscaler resolves and a copy shows as a picture that shakes. `0` = as the game draws it, `1` = steady: the picture is taken back by the jitter the game gives (a resample, a little softer). SMAA T2x (`render_anti_aliasing=2`) does that itself |
| `fsr_sharpness` | `0` | The sharpening FSR 2 or FSR 1 applies after upscaling (RCAS), 0 to 1: `0` = none, and no sharpening pass runs |
| `dlss_runtime` | empty | Windows: the folder that holds NVIDIA's DLSS runtime, which the user downloads from NVIDIA and which is not part of ScarletNX (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`: [DLSS](dlss.md)). Empty = no DLSS. With it, `render_upscaler` `0` or `4` runs DLSS on an NVIDIA GPU. Elsewhere, or where the files are missing or not signed by NVIDIA, the next upscaler stands in |
| `dlss_preset` | `2` | Windows: the DLSS model, `0` = NVIDIA's choice for each mode, `1` to `4` = the presets J, K, L, M. K by default: at 2x it takes less memory than NVIDIA's choice there (M), 158 against 214 MB at 2560x1440 |
| `shadows` | `1` | `0` skips shadow maps |
| `vsync` | `1` | Images are shown only on the display's refresh, so nothing tears. `0` doesn't wait for it: the newest image is shown on the next refresh where the display can do that, else at once (which may tear). A display that can do neither waits for the refresh as `1` does. A 3DS game takes it too (`--vsync=0`), its window saying at start which it got |
| `frame_pacing` | `1` | With VSync and `fps_limit=game`: `0` paces the game's frames by a timer; `1` also has the next frame wait until the one before is on the display (`VK_KHR_present_wait` or `VK_GOOGLE_display_timing`), so frames don't queue for it; `2` also holds each frame of a game that asks for two refreshes or more a frame until its refresh, where the display can say when (`VK_EXT_present_timing` or `VK_GOOGLE_display_timing`). Where the display can't do one, the one before it |
| `anisotropy` | `0` | Texture filtering as the game asks. `2` to `16` sharpens surfaces seen at an angle. A 3DS game takes it too (`--anisotropy=N`), for its mipmapped textures filtered linearly, and says at start the value in effect |
| `upscaler` | `off` | The filter that grows the game's frame to the window: `off` as the window always drew it (smooth on the Switch and the Vita, sharp pixels on the 3DS), `bilinear` smooth, `fsr1` AMD FSR 1, `nis` NVIDIA NIS and `sgsr` Qualcomm SGSR with sharper edges. Where the frame is not shown larger, the smooth scale runs; NIS grows a frame twice at most, and past that FSR 1 runs. The log says the filter in effect, and why where it is not the one asked. A 3DS game (`--upscaler=`) and a Vita one take it too. Another word is said at start, with `off` in its place. A change while the game runs (the game's menu) takes effect at the next frame |
| `upscaler_sharpness` | `75` | How sharp FSR 1, NIS and SGSR draw the frame's edges, `0` the softest to `100` the sharpest |
| `anti_aliasing` | `off` | Smoothing of the shown frame's edges, after the upscaler: `auto` is FXAA where the window shows the frame at 1.25 times its size or more each way, and none elsewhere; `fxaa` and `smaa` filter the frame at the window's size (with `upscaler` `off`, scaled as the window scales it: the 3DS in sharp pixels); `ssaa2` and `ssaa4` draw it 2 or 4 times as large and shrink it (where the device holds an image that large, else the step under it, said). Only an SDR frame: a game's own HDR frames are shown without it, which the log says. A 3DS game and a Vita one take it too. A change while the game runs (the game's menu) takes effect at the next frame |
| `hdr_game` | `0` | `1` lets a game output HDR. A game that asks is then told the display is HDR, and the frames it draws as HDR10 (PQ in BT.2020's primaries) or scRGB (linear light, `1.0` at 80 nits) are shown as they are, in a swapchain of their own color space with the HDR metadata of the game's window. It takes a window whose display lists both kinds (`VK_EXT_swapchain_colorspace`) on a device with `VK_EXT_hdr_metadata`; where one is missing, and with `0`, the game is told the display is SDR and draws for that. The log says which, and names what is missing. A window system that lists them for a display with no HDR converts the image itself. A screenshot of an HDR frame holds its values as they are |
| `hdr_paperwhite` | `0` | With `hdr_game`: the light of white a game is told to draw its SDR parts at, in nits, `0` to `480`. The game's menu and the on-screen controls are drawn at it too. `0` is BT.2408's reference white of 203 nits |
| `hdr_peak` | `1000` | With `hdr_game`: the brightest light a game is told the display shows, in nits, `400` to `4000`, which the game tone maps its frames for. The display's own peak: `1000` for most TVs |
| `vulkan_video` | `0` | `1` decodes H.264 and AV1 movies on the GPU with Vulkan Video (FFmpeg's Vulkan decoder), each where the GPU and its driver have a decode queue for it; an AV1 movie the GPU's decoder refuses goes on with the CPU's, and the log says which decodes each movie and why. Experimental. `0` decodes them on the CPU, as VP8 and VP9 movies always are |
| `guest_vulkan` | `0` | `1` executes the Vulkan calls of a supported 64-bit game on the host after compatibility fixes ([Architecture](../dev/architecture.md)). Experimental. Use it only with trusted dumps. Requires a restart, a little-endian host, and `VK_EXT_external_memory_host` or Linux dma-buf export. If the runtime reports a missing route, recompile with `--hook-direct-calls`. Calling an unimplemented entry point stops the game and names the function. Save states are unavailable while active. `0` runs the game's driver through the Maxwell front end |
| `apple_video` | `1` | `1` decodes AV1 movies on the Apple GPU's media engine with VideoToolbox, and the CPU only copies the picture into the game's image. A stream the media engine does not take decodes on the CPU with dav1d either way, and the log says which one has it. `0` decodes them on the CPU with dav1d. macOS only |
| `vulkan_driver` | `auto` | The Vulkan driver on macOS. `auto` is KosmicKrisp where the Mac has it (the app's, else Homebrew's `mesa` or a LunarG Vulkan SDK's) and can run it (Apple silicon, macOS 26 or later), else the MoltenVK the app carries, else the system's Vulkan loader; a driver that `VK_DRIVER_FILES` names comes before them. `moltenvk` is a MoltenVK library, carried or installed on the Mac (Homebrew, `/usr/local`, the Vulkan SDK). `kosmickrisp` is Mesa's KosmicKrisp through the Vulkan loader. KosmicKrisp is experimental, and not the faster driver in every scene: one game's station scene runs at about twice the frame rate on `moltenvk` ([macOS and iOS](../hosts/apple.md#kosmickrisp-experimental)). The log says which driver runs and why. A value that is not one of the three stops the game with a line saying so, and a named driver that is not there is said in the log, with the driver that plays in its place. On every other system the game runs on the system's driver whatever the value (on Android, `gpu_driver`'s), and the log says so for `moltenvk` and `kosmickrisp` |
| `vertex_input_dynamic_state` | `auto` | How a draw gives the GPU its vertex layout. `auto` sets it as dynamic state (`VK_EXT_vertex_input_dynamic_state`) where the GPU's driver has the extension and its `vertexInputDynamicState` feature, but not on the drivers the extension bans (`src/common/gpu/vk/extension_bans.txt`) name for it: NVIDIA's before 580.119.02, Intel's Windows driver before 27.20.100.0 (of whose four numbers the driver gives the last two: below 100.0), Arm's Mali before r48 and Qualcomm's on Android, whose 512.800.48 draws it wrong; and not on Android, whatever the driver, as Eden. `on` sets it as dynamic state wherever the driver has it, those too. `off` puts it in the pipeline: each layout a program is drawn with takes a pipeline of its own, where dynamic state has one serve them all. The log says which, with the device, its driver and version. `on` on a device without the extension says so and puts the layout in the pipeline; any value but `auto`, `on` and `off` stops the game, saying so. Pipelines kept for the next run (`.vkshaders`) are made again when the choice changes |
| `vulkan_ban_ignore` | empty | Keys of the [extension bans](../dev/development.md#driver-bans) (`src/common/gpu/vk/extension_bans.txt`), separated by commas, whose bans the run goes over, to test again whether the driver still breaks them: `VK_KHR_present_id2`, or `VK_KHR_present_id2,shaderFloat16`. The log says each, as a warning with the ban it goes over where one holds for the device. Empty: every ban holds. A key the table has not stops the game, naming the key and the three nearest it has. The launcher's Settings have a box for each key, with what the bans on it say; the game's menu shows the value |
| `astc_mode` | `0` | How a GPU that samples BCn but not ASTC (most desktop ones) holds ASTC textures. `0` (Auto) follows the GPU's memory as the game runs: the textures that load are Exact while there is room, BC7 from the moment the caches fill to where they start freeing memory (a little under the budget), and Exact again once more room comes back than the caches' own trims leave and than the textures held as BC7 would take more as RGBA8; those loaded Exact go to BC7 too at the turn, read again at their next use where their memory is unchanged. The log says each change with the memory in use, the budget and the room. Whatever the room, a texture whose blocks hold luminance alone is BC4 as it loads, luminance and alpha BC5, and opaque colour on one line a 4x4 block BC1 (sRGB luminance too): an eighth to a quarter of RGBA8's memory, close to what the blocks decode to ([By what the blocks hold](../layers/gpu/astc.md#by-what-the-blocks-hold)). `1` (Exact) decodes them to RGBA8, exactly as the game's blocks decode, in four times the memory. `2` (BC7) shows them at once decoded to RGBA8, then encodes them again as BC7 between frames, a few at a time, into a quarter of RGBA8's memory, close to what the blocks decode to but not always the same ([ASTC](../layers/gpu/astc.md#auto-exact-and-bc7)). A later run loads them from the BC7 cache. `3` (BC7 at load) encodes them as BC7 as each texture loads, with no RGBA8 image before it, as feat/ounce did. Where the GPU samples ASTC it does nothing. It takes the place of `astc_bc`, whose `1` is Auto ([Deprecations](deprecations.md)) |
| `texture_shed` | `0` | `1` makes a texture of 1 MiB or more mapped whole, with mipmaps, that the GPU only reads with sparse residency (where the GPU has it, and `VK_EXT_image_view_min_lod`): when the GPU's memory runs short its finest level gives way, the texture sampled at half its resolution for a while, in place of the whole texture evicted and loaded again; the level comes back when there is room. `0` keeps textures whole |
| `texture_half_res` | `0` | `0` (Auto) makes such a texture (1 MiB or more, mapped whole, with mipmaps, only read) while the GPU's memory is under pressure (its use at 90% of the budget, or short in the last 300 presents) with sparse residency and its finest level never bound: sampled at half its resolution, a quarter of its memory, its size still the game's for `textureSize` and `texelFetch`. The level comes back when there is room. `1` never, `2` always |
| `astc_lines_bc1` | `1` | Under `astc_mode`'s Auto, an opaque colour ASTC texture whose 4x4 blocks hold more colours than BC1's line (held as BC7) is held as BC1 instead, half BC7's memory and coarser. `0` (Auto) once the GPU's memory has run short, for the textures loaded from then on; `1` never (BC7); `2` from the start |
| `texture_sysmem` | `1` | `1` moves a texture the cache would evict, where the GPU's memory runs short, into system memory instead, where the GPU still samples it (more slowly, over the bus), and back when it is used and there is room. `0` evicts it, to be loaded again at its next use |
| `bc7_queue` | `0` | The thousands of blocks the BC7 encoder may have queued on the GPU before an upload waits for it to do them. `0` (Auto) gives 256 for every 2 GB of the GPU's own memory, from 256 to 4096, and 256 on a GPU that shares the computer's memory (a GT 1030, 2 GB, lost the device with more queued). `1` to `16384` is that many thousand |
| `astc_compute_queue` | `1` | `1` decodes and encodes a new ASTC texture on the GPU's compute queue, where it has one of its own, beside the drawing, and a load waits only for those encodes. `0` does it on the graphics queue, after the drawing before it. The textures are the same either way |
| `astc_quick_mtexels` | `8` | With ASTC textures encoded as BC7 as they load (`astc_mode` `0` for an Ounce game, or `3`), a texture level of this many megatexels (2^20 texels) or more, all its layers or slices counted (`8`: a 4096x2048 level, or two layers of 2048x2048), is encoded quickly in the frame that first needs it, in the BC7 encoder's quick build (mode 6 from one line: softer edges), and its better version (the fast build) is made in the background by what `astc_background` picks. The better blocks replace the quick ones in the texture as they come, and go into the BC7 cache in pieces, which the next run copies with no encode (from a disk too slow for that frame, it encodes quickly again and loads the pieces over the next frames). The value is where a run starts: it stands for the time the fast build takes a Radeon 890M over that many megatexels (`8`: 0.21 s), and once the encoder has been timed on this GPU over as many texels, the size is the one this GPU encodes in that time, bigger on a faster one; the log says the size in use and where it came from ([ASTC](../layers/gpu/astc.md#the-size-from-the-encoders-time)). `0` never encodes quickly: every level takes the fast build at once, which stopped one game's first 3D scene for 10 seconds on a Radeon 890M ([ASTC](../layers/gpu/astc.md#very-large-levels)) |
| `astc_background` | `1` | Who makes the better version of the levels `astc_quick_mtexels` encodes quickly. `1`, the GPU: a piece of 256 KiB of blocks after each present, in a submission of its own, the next once it is done. `0`, nobody: those levels keep the quick encode, and the next run encodes them quickly again |

### Vulkan Extensions

Each Vulkan extension a renderer can make its device with has a setting of its own in `[Graphics]`, its name in
lowercase: `vk_ext_transform_feedback` for `VK_EXT_transform_feedback`, `vk_khr_maintenance5` for `VK_KHR_maintenance5`
(the `vk_extension_settings` test fails for one without). The launcher's Graphics group shows them on its Vulkan
Extensions page. A 3DS or Vita game takes them too, and the same bans, for the extensions its renderer uses
(`--vk-ext-shader-object=off`); the Android apps' renderer takes the bans' check of each extension's commands alone.
Each is `auto` by default:

| Value | Effect |
| --- | --- |
| `auto` | ScarletNX decides: the extension is used where the driver has it and no [driver ban](../dev/development.md#driver-bans) holds |
| `on` | Used wherever the driver has it, over a ban: the log warns with the ban it goes over |
| `off` | Never used: the extension leaves the device's list before the renderer reads it, so it does without |

The driver says whether an extension is there: `on` can't add one it lacks. Turning off one ScarletNX can't run
without on the device (`VK_KHR_swapchain` with a window, `VK_KHR_portability_subset` where the driver lists it, and on
a Vulkan 1.2 device the extensions Vulkan 1.3 took in) stops the game, saying which and why; so does a value other
than these three. An extension that is core in the Vulkan version the renderer runs the device at (its help says
which it was promoted to) leaves only its name out when off. The settings are read when the game starts, and the
log has a line for each, with its value in effect and why:

```
vulkan: VK_EXT_transform_feedback: off (vk_ext_transform_feedback=off)
vulkan: VK_EXT_custom_border_color: on (auto: the driver has it, and no ban holds)
vulkan: VK_KHR_cooperative_matrix: off (the driver has it not)
```

The settings, each with its extension and, where one took it in, the Vulkan version it was promoted to:

| Key | Default | Extension |
| --- | --- | --- |
| `vk_android_external_memory_android_hardware_buffer` | `auto` | `VK_ANDROID_external_memory_android_hardware_buffer` |
| `vk_ext_attachment_feedback_loop_dynamic_state` | `auto` | `VK_EXT_attachment_feedback_loop_dynamic_state` |
| `vk_ext_attachment_feedback_loop_layout` | `auto` | `VK_EXT_attachment_feedback_loop_layout` |
| `vk_ext_blend_operation_advanced` | `auto` | `VK_EXT_blend_operation_advanced` |
| `vk_ext_calibrated_timestamps` | `auto` | `VK_EXT_calibrated_timestamps` |
| `vk_ext_conditional_rendering` | `auto` | `VK_EXT_conditional_rendering` |
| `vk_ext_conservative_rasterization` | `auto` | `VK_EXT_conservative_rasterization` |
| `vk_ext_custom_border_color` | `auto` | `VK_EXT_custom_border_color` |
| `vk_ext_depth_clamp_control` | `auto` | `VK_EXT_depth_clamp_control` |
| `vk_ext_depth_clip_control` | `auto` | `VK_EXT_depth_clip_control` |
| `vk_ext_depth_clip_enable` | `auto` | `VK_EXT_depth_clip_enable` |
| `vk_ext_depth_range_unrestricted` | `auto` | `VK_EXT_depth_range_unrestricted` |
| `vk_ext_descriptor_buffer` | `auto` | `VK_EXT_descriptor_buffer` |
| `vk_ext_descriptor_heap` | `auto` | `VK_EXT_descriptor_heap` |
| `vk_ext_device_fault` | `auto` | `VK_EXT_device_fault` |
| `vk_ext_dynamic_rendering_unused_attachments` | `auto` | `VK_EXT_dynamic_rendering_unused_attachments`: a pipeline leaves out the color targets its draws write nothing to, so passes with other formats there share it |
| `vk_ext_extended_dynamic_state` | `auto` | `VK_EXT_extended_dynamic_state`, promoted to Vulkan 1.3 |
| `vk_ext_extended_dynamic_state2` | `auto` | `VK_EXT_extended_dynamic_state2`, promoted to Vulkan 1.3 |
| `vk_ext_extended_dynamic_state3` | `auto` | `VK_EXT_extended_dynamic_state3` |
| `vk_ext_external_memory_dma_buf` | `auto` | `VK_EXT_external_memory_dma_buf` |
| `vk_ext_external_memory_host` | `auto` | `VK_EXT_external_memory_host` |
| `vk_ext_fragment_shader_interlock` | `auto` | `VK_EXT_fragment_shader_interlock` |
| `vk_ext_graphics_pipeline_library` | `auto` | `VK_EXT_graphics_pipeline_library` |
| `vk_ext_hdr_metadata` | `auto` | `VK_EXT_hdr_metadata` |
| `vk_ext_image_drm_format_modifier` | `auto` | `VK_EXT_image_drm_format_modifier`, which a game's own Vulkan (`guest_vulkan`) takes for its linear images in dma-bufs |
| `vk_ext_image_view_min_lod` | `auto` | `VK_EXT_image_view_min_lod` |
| `vk_ext_index_type_uint8` | `auto` | `VK_EXT_index_type_uint8` |
| `vk_ext_legacy_vertex_attributes` | `auto` | `VK_EXT_legacy_vertex_attributes`: a vertex stream whose stride isn't a multiple of its attributes' components is bound as the game has it; off, each draw of it reads a copy restrided on the GPU |
| `vk_ext_load_store_op_none` | `auto` | `VK_EXT_load_store_op_none` |
| `vk_ext_map_memory_placed` | `auto` | `VK_EXT_map_memory_placed` |
| `vk_ext_memory_budget` | `auto` | `VK_EXT_memory_budget` |
| `vk_ext_memory_priority` | `auto` | `VK_EXT_memory_priority`, which `VK_EXT_pageable_device_local_memory` needs: off turns both off |
| `vk_ext_mesh_shader` | `auto` | `VK_EXT_mesh_shader` |
| `vk_ext_mutable_descriptor_type` | `auto` | `VK_EXT_mutable_descriptor_type`, whose host-only descriptor pools hold the twins of a game's own descriptor sets (`guest_vulkan`), so its descriptor copies read host memory |
| `vk_ext_pageable_device_local_memory` | `auto` | `VK_EXT_pageable_device_local_memory`: where the GPU's own memory is short because other programs take video memory, the driver moves textures loaded from memory to system memory first, and render targets last. ScarletNX keeps its own use under the driver's budget either way. Off, the renderer gives no priorities |
| `vk_ext_physical_device_drm` | `auto` | `VK_EXT_physical_device_drm`: the render node of the GPU's driver, which `gpu_pstate` holds an AMD GPU's clocks on. It is only read, the device is not made with it; off, no clocks are held there |
| `vk_ext_pipeline_creation_feedback` | `auto` | `VK_EXT_pipeline_creation_feedback`, promoted to Vulkan 1.3 |
| `vk_ext_pipeline_robustness` | `auto` | `VK_EXT_pipeline_robustness`, promoted to Vulkan 1.4 |
| `vk_ext_post_depth_coverage` | `auto` | `VK_EXT_post_depth_coverage` |
| `vk_ext_present_timing` | `auto` | `VK_EXT_present_timing` |
| `vk_ext_primitive_topology_list_restart` | `auto` | `VK_EXT_primitive_topology_list_restart` |
| `vk_ext_provoking_vertex` | `auto` | `VK_EXT_provoking_vertex` |
| `vk_ext_rasterization_order_attachment_access` | `auto` | `VK_EXT_rasterization_order_attachment_access` |
| `vk_ext_robustness2` | `auto` | `VK_EXT_robustness2` |
| `vk_ext_sample_locations` | `auto` | `VK_EXT_sample_locations` |
| `vk_ext_shader_atomic_float` | `auto` | `VK_EXT_shader_atomic_float` |
| `vk_ext_shader_demote_to_helper_invocation` | `auto` | `VK_EXT_shader_demote_to_helper_invocation`, promoted to Vulkan 1.3 |
| `vk_ext_shader_object` | `auto` | `VK_EXT_shader_object`, which a 3DS game draws with: no pipeline is made for its shaders and state |
| `vk_ext_subgroup_size_control` | `auto` | `VK_EXT_subgroup_size_control`, promoted to Vulkan 1.3: on a 1.2 device, the compute subgroups of 32 and the mesh draws |
| `vk_ext_transform_feedback` | `auto` | `VK_EXT_transform_feedback` |
| `vk_ext_vertex_attribute_divisor` | `auto` | `VK_EXT_vertex_attribute_divisor` |
| `vk_ext_vertex_input_dynamic_state` | `auto` | `VK_EXT_vertex_input_dynamic_state` |
| `vk_google_display_timing` | `auto` | `VK_GOOGLE_display_timing` |
| `vk_khr_calibrated_timestamps` | `auto` | `VK_KHR_calibrated_timestamps` |
| `vk_khr_cooperative_matrix` | `auto` | `VK_KHR_cooperative_matrix` |
| `vk_khr_copy_memory_indirect` | `auto` | `VK_KHR_copy_memory_indirect` |
| `vk_khr_driver_properties` | `auto` | `VK_KHR_driver_properties`, promoted to Vulkan 1.2 |
| `vk_khr_dynamic_rendering` | `auto` | `VK_KHR_dynamic_rendering`, promoted to Vulkan 1.3 |
| `vk_khr_dynamic_rendering_local_read` | `auto` | `VK_KHR_dynamic_rendering_local_read`, promoted to Vulkan 1.4 |
| `vk_khr_extended_flags` | `auto` | `VK_KHR_extended_flags` |
| `vk_khr_external_memory_fd` | `auto` | `VK_KHR_external_memory_fd` |
| `vk_khr_external_memory_win32` | `auto` | `VK_KHR_external_memory_win32` |
| `vk_khr_format_feature_flags2` | `auto` | `VK_KHR_format_feature_flags2`, promoted to Vulkan 1.3 |
| `vk_khr_fragment_shader_barycentric` | `auto` | `VK_KHR_fragment_shader_barycentric` |
| `vk_khr_fragment_shading_rate` | `auto` | `VK_KHR_fragment_shading_rate` |
| `vk_khr_index_type_uint8` | `auto` | `VK_KHR_index_type_uint8`, promoted to Vulkan 1.4 |
| `vk_khr_load_store_op_none` | `auto` | `VK_KHR_load_store_op_none`, promoted to Vulkan 1.4 |
| `vk_khr_maintenance4` | `auto` | `VK_KHR_maintenance4`, promoted to Vulkan 1.3 |
| `vk_khr_maintenance5` | `auto` | `VK_KHR_maintenance5`, promoted to Vulkan 1.4 |
| `vk_khr_maintenance8` | `auto` | `VK_KHR_maintenance8` |
| `vk_khr_map_memory2` | `auto` | `VK_KHR_map_memory2`, promoted to Vulkan 1.4 |
| `vk_khr_pipeline_executable_properties` | `auto` | `VK_KHR_pipeline_executable_properties`, on only with `pipeline_stats` |
| `vk_khr_pipeline_library` | `auto` | `VK_KHR_pipeline_library` |
| `vk_khr_portability_subset` | `auto` | `VK_KHR_portability_subset` |
| `vk_khr_present_id` | `auto` | `VK_KHR_present_id` |
| `vk_khr_present_id2` | `auto` | `VK_KHR_present_id2` |
| `vk_khr_present_wait` | `auto` | `VK_KHR_present_wait` |
| `vk_khr_present_wait2` | `auto` | `VK_KHR_present_wait2` |
| `vk_khr_shader_clock` | `auto` | `VK_KHR_shader_clock` |
| `vk_khr_shader_maximal_reconvergence` | `auto` | `VK_KHR_shader_maximal_reconvergence` |
| `vk_khr_swapchain` | `auto` | `VK_KHR_swapchain` |
| `vk_khr_synchronization2` | `auto` | `VK_KHR_synchronization2`, promoted to Vulkan 1.3 |
| `vk_khr_timeline_semaphore` | `auto` | `VK_KHR_timeline_semaphore`, promoted to Vulkan 1.2 |
| `vk_khr_unified_image_layouts` | `auto` | `VK_KHR_unified_image_layouts` |
| `vk_khr_video_decode_av1` | `auto` | `VK_KHR_video_decode_av1` |
| `vk_khr_video_decode_h264` | `auto` | `VK_KHR_video_decode_h264` |
| `vk_khr_video_decode_queue` | `auto` | `VK_KHR_video_decode_queue` |
| `vk_khr_video_queue` | `auto` | `VK_KHR_video_queue` |
| `vk_nv_device_diagnostic_checkpoints` | `auto` | `VK_NV_device_diagnostic_checkpoints` |
| `vk_nv_device_diagnostics_config` | `auto` | `VK_NV_device_diagnostics_config`: with `vk_checkpoints=1` on NVIDIA, the driver also keeps shader debug info and what each resource is, for the report of a lost device |
| `vk_nv_fragment_coverage_to_color` | `auto` | `VK_NV_fragment_coverage_to_color` |
| `vk_nv_framebuffer_mixed_samples` | `auto` | `VK_NV_framebuffer_mixed_samples` |
| `vk_nv_low_latency2` | `auto` | `VK_NV_low_latency2`: the boost `gpu_pstate` holds an NVIDIA GPU's clocks with, on each swapchain. Off, no clocks are held there |
| `vk_nv_shading_rate_image` | `auto` | `VK_NV_shading_rate_image` |
| `vk_valve_shader_mixed_float_dot_product` | `auto` | `VK_VALVE_shader_mixed_float_dot_product` |

## Audio

| Key | Default | Effect |
| --- | --- | --- |
| `volume` | `100` | How loud the game plays, in percent of its own sound, `0` to `100` (the menus' steps 5 apart): `0` mutes it. The system's volume stays as it is, and the game's own volume (its options, the console's volume it sets) is taken at this share. A change is heard at once, from the game's `.ini` too. The game's menu (Home, F2) has it on a slider ([Usage](usage.md#playing)), kept in the game's own picks, over the launcher's Settings > Audio > Volume, a slider too, which sets it for every game ([GUI launcher](gui.md)). The log's `settings:` line names it when it is not `100`, again after a reload that changes it, and each output the game opens says the volume it plays at. A 3DS game takes it too (`--volume=N`) |

## Performance

| Key | Default | Effect |
| --- | --- | --- |
| `fps_limit` | `game` | The pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30. `0` is unlimited, and `10` to `1000` is that many FPS. A 3DS game takes it too (`--fps-limit=N`): its clock runs at N/60 of the console's pace (the 3DS shows 60 frames a second), its sound with it; `0` skips every wait, its sound not played. F4 turns it off and on there too |
| `frame_rate` | `0` | The frame rate a game module asks the game to run at: `0` = the game's own, `1` = 30, `2` = 60 frames a second. Only a game whose module knows how the game paces itself follows it, keeping its logic in real time ([Hooks, modules in libraries](../platforms/nx/hooks.md#modules-in-libraries)). Without such a module it does nothing: `fps_limit` paces the frames, not the game's logic |
| `memory_gb` | `0` | Memory the game sees. Below `6` it is the console's 3.2 GB, `6` gives 4.8 GB and `8` gives 6.8 GB. Games that run out of memory on 3.2 GB need `6`. `12` is an experimental 10 GiB application budget with a 12 GiB heap region for newer games. It needs enough host memory and address space. |
| `memory_fit_mode` | `0` | `0` (Auto) tells the game at least the console's memory, and of what `memory_gb` adds above it no more than the device can still give, measured as the game starts, less what the rest of ScarletNX takes: a device with less than the console has runs as with `1`, and one that can give it all tells it all of it. The log says what it measured. `1` (Off) tells it all of `memory_gb`'s. It takes the place of `memory_fit`, whose `0` was Off ([Deprecations](deprecations.md)) |
| `heap_max_mb` | `0` | The most heap the game can allocate, in MB. `0` is as much as the memory allows |
| `map_limit_mb` | `0` (iOS `256`) | The most of the game's RomFS and ExeFS files mapped into memory, in MB: the ExeFS and a `romfs.bin` at start, a `romfs` folder's files as the game first reads them. `0` is no limit. A file past it is read from disk when the game asks, which is slower and leaves address space for threads and graphics where a device has little |
| `map_limit_files` | `0` (iOS `2048`) | The most of those files mapped. `0` is no limit. Each mapped file holds a descriptor, of which a device may give the whole app a few thousand |
| `thread_stack_mb` | `32` (iOS `8`) | The host stack of each of the game's threads, in MB, which a game's deep calls take. Less leaves address space for more threads where a device has little, and a game that calls very deep may then stop |
| `gpu_pstate` | `auto` | Holds the GPU's clocks up while the game runs. A frame keeps the GPU half idle (it waits for the frame's commands, then the game waits for it), and a driver that sets the clocks by how busy the GPU is lowers them, which makes every frame slower. `auto` holds them on a discrete GPU where its driver lets a program do it and on a phone's Adreno, and not on a computer's integrated GPU, which shares its power with the CPU; `on` holds that one too; `off` leaves them to the driver. AMD's GPUs on Linux are held at amdgpu's peak stable pstate (`AMDGPU_CTX_OP_SET_STABLE_PSTATE`, on a context of ScarletNX's own: no root needed), only from amdgpu's level `auto` (`power_dpm_force_performance_level`): a level set on the PC (LACT, CoreCtrl, by hand) is left as it is, as the kernel would put `auto` back over it when the game ends. The kernel gives the level back when the game ends, a crash or a kill too. NVIDIA's driver is asked for its boost on each swapchain (`VK_NV_low_latency2`'s `lowLatencyBoost`, its low latency mode off), which needs presents with ids (`VK_KHR_present_id`) and ends with the swapchain. An Adreno under kgsl (Android) is held at its top power level while frames are presented, as the constraint of a context of ScarletNX's own (`KGSL_PROP_PWR_CONSTRAINT`), which kgsl drops with the context when the game ends, a crash or a kill too. The log says which, with the render node and the level before and after, or why none is held and the error the kernel gave; any value but `auto`, `on` and `off` stops the game, saying so ([GPU clocks](../layers/gpu/gpu-clocks.md)) |
| `pipeline_threads` | `0` | Threads that make the GPU's new pipelines while the game goes on, and those of earlier runs before it starts with `prepare_shaders=2`. `0` uses all the computer's threads but two, two to twelve. `1` to `32` is that many. Past the first two they run at the system's low priority (nice +10 on Linux and Android, the utility QoS class on macOS and iOS, below normal on Windows, the idle class where FreeBSD allows it), so more make a first run's new pipelines sooner and the game's own threads still go first |
| `async_pipelines` | `0` | `1` skips what a new shader draws while its pipeline is made, where the game would otherwise pause until it is ready. The game no longer stops where a new shader first appears, and the things that shader draws show up a few frames later. Only into what the game draws over every frame (drawn 8 frames in a row and cleared whole in this one); into anything else, such as a menu's layer drawn once and kept, the draw waits for its pipeline. `0` waits for every pipeline. A 3DS game takes it too, and `pipeline_threads` (the launcher passes them to `scarletnx-3ds` as `--async-pipelines=1` and `--pipeline-threads=N`) |
| `pipeline_libraries` | `1` | `1` makes a new pipeline from parts of its shaders that compile once, where the GPU has graphics pipeline libraries, and makes an optimized one in the background. A new state of a known shader then costs a link of a few milliseconds, not a compile. `0` compiles each pipeline whole |
| `prepare_shaders` | `2` | Prepares the shaders the game ships in its files and executables ahead of time, so the game's draws needn't make them and it stutters less. They are prepared again after ScarletNX, the GPU or its driver is updated. `0` (Off) makes each shader only when the game first draws with it. `1` (While playing) prepares them in the background, and the game uses them once all are ready. `2` (Before starting) prepares them behind a loading screen in the launcher theme's look, which also waits for the shader cache of earlier sessions, so an opening movie doesn't stutter. A key that types, an arrow or the keypad, a gamepad button other than Home, or a tap starts the game at once, and the rest is prepared while it runs. Esc twice quits (on Android, back opens the menu, whose Exit quits). F2 or a gamepad's Home opens the menu, and F11 or Alt+Enter switches fullscreen, without starting the game. The screen shows only when the wait passes a quarter second, its words are in English, and a timed run (`--seconds`) starts its clock after it |
| `compress_prepared_shaders` | `1` | `1` packs each shader `prepare_shaders` makes (`shader_cache/<id>.<driver>.vkshipped`) with zstd on its own: about a quarter of the space, about 50 microseconds more for each as it is prepared, and 15 the first time a draw uses it. `0` saves them as they are made. What is packed already is read either way |
| `nce` | `1` | On an AArch64 Linux host or Android 10 and later on arm64, `1` runs the game's 64-bit code natively on the host's CPU (native code execution), with the words that cannot run as they are patched: SVCs, the TLS registers, the counter registers, and the exclusives made ordered, as Eden patches them. The recompile then makes no code ahead of time. `0` recompiles the code. Other hosts, 32-bit games and, on Android, games of the 36-bit address space always recompile. The next recompile takes it ([NCE](../platforms/nx/nce.md)) |
| `jit_threads` | `0` | Threads compiling the game code the recompiler missed, which a thread waits for when it reaches it. `0` uses every CPU the game may run on but two, at most 4; `1` to `16` is that many. They run below the game's threads; one more thread, at the game's priority, compiles only what a game thread is waiting for |
| `jit_region_instructions` | `0` | The most instructions one branch graph of missed code may hold when the JIT compiles it, so a huge function cannot stall the game for seconds; branches out of it resume through the dispatch table. `0` is up to 4096, a larger graph cut into regions of 64 |
| `jit_live_budget` | `0` | The instructions beyond the missed code itself (its callees) that a miss the game waits on discovers. The background queue keeps its larger budget. `0` is 64 |

## On-screen controls

| Key | Default | Effect |
| --- | --- | --- |
| `touch` | `2` | The game's touch screen: `2` fingers and the mouse's left button, `1` fingers, `0` off. A 3DS game takes it too, for its bottom screen (`--touch=N`), and a DS game, for its lower one: one finger or the mouse at a time, the first pressed holding it until it lets go. A Vita game takes it for its front panel. A GBA game has no touch screen, and a value given it is said to do nothing |
| `controls_<ID>` (ID one of `l`, `r`, `zl`, `zr`, `minus`, `plus`, `capture`, `home`, `l3`, `r3`, `dpad`, `stick_l`, `stick_r`, `a`, `b`, `x`, `y`) | empty | A control's place, written once it is moved or reset: `x y size`, its middle across the screen from 0 to 1 and its size in percent (50 to 200), as `controls_a=0.930 0.470 100` |
| `controls_<ID>_shown` | `1` | `1` draws the control, `0` hides it; written once it is shown or hidden. A GBA, DS or Vita game takes its console's controls' alone, of these and of the places (`plus` is its Start, `minus` its Select; `a`, `b`, `x` and `y` the Vita's Circle, Cross, Triangle and Square), and another's value is said to do nothing |
| `controls` | `2` | The [on-screen controls](usage.md#on-screen-controls): `1` shown, `0` hidden, `2` (Auto) shown while the device has a touch screen and no gamepad is connected, hidden while one is; the log says what Auto shows and why as it changes. The launcher gives every run `2` |
| `controls_snap`, `controls_scale`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics` | `0`, `50`, `1`, `1`, `1` | As [On-screen controls](usage.md#on-screen-controls) describes them. `controls_scale` runs from `0` to `100` for 50% to 150%. A GBA or DS game has no stick, and a `controls_rel_center` given it is said to do nothing |

## Controllers

| Key | Default | Effect |
| --- | --- | --- |
| `gamepad_swap_ab` | `0` | `1` swaps a gamepad's A and B. `0` sends A, B, X and Y where the console's are. A 3DS game takes it too (the launcher passes it to `scarletnx-3ds` as `--gamepad-swap-ab=1`) |
| `gamepad_map` | none | The player's map over that, below. A 3DS game takes it too (`--gamepad-map=MAP`), Plus as Start and Minus as Select; a control sent to L3 or R3, which the 3DS has none of, sends nothing, and the log says so |
| `gamepad_deadzone` | `20` | The sticks' dead zone, in percent of their travel (`0` to `90`). A 3DS game takes it too, for its circle pad (`--gamepad-deadzone=N`) |
| `gamepad_rumble` | `100` | How strongly gamepads vibrate when the game asks, in percent. `0` is never. A Pro Controller or a single Joy-Con plays the game's HD rumble, with its frequencies, when SDL reads it through its HIDAPI driver (the log says "plays HD rumble"). A Pro Controller plays each side on its own actuator, and a single Joy-Con plays each band of the stronger side on its one actuator. Other controllers, two paired Joy-Cons, and a Switch controller that only the kernel driver reads get the strength of the vibration only. The log says why for each of them |
| `gamepad_background` | `0` | `1` keeps gamepads playing while the window is not focused |
| `gamepad_joycon_pair` | `1` | `1` plays a left and right Joy-Con together, `0` separately. Restart the game after changing it |
| `gamepad_assign` | `1` | How controllers take players. `0`, manual: as each player's `gamepad_p<N>_device` names it, the next to connect where it names none. `1`, as a console's (Eden's Modern Console): each controller that connects takes the lowest free player, one that leaves keeps its player 45 seconds and takes it back when it returns, the game pauses once a player's controller has been gone 3 seconds (until it returns, or + is let go on another controller to go on without it), and a notice says each controller that joins or leaves, with its player ([Playing](usage.md#playing)) |
| `controller_applet` | `0` | What happens when the game asks for its controllers. `0`, Dialog: a dialog over the game says what the game expects (how many players, how each one holds its controller, and the game's own text and color for each player), each player takes a controller with the console's gestures (SL+SR on one Joy-Con held sideways, L and R on two Joy-Cons for a pair, L+R on a pad), and the game goes on when the dialog closes. `1`, Quick: the game gets its answer at once when the players it needs are connected, and the dialog shows only when they are not. A run without a window, with a time limit or with a script of presses answers at once |
| `game_mouse` | `0` | How the mouse reaches an Ounce game as its Joy-Con mouse. `0`, Hotkey: nothing reaches the game until `hotkey_mouse` (Ctrl+G) declares mouse mode, which holds the mouse and gives its motion, buttons and wheel to the game, and a second press ends it. `1`, Auto: the mouse always reaches the game, held while the game plays with it, and `hotkey_mouse` lets it go |
| `joycon2` | `0` | Linux only. `1` makes the runtime connect Joy-Con 2 controllers and Pro Controller 2 over Bluetooth itself, without pairing them, so a controller stays paired with its console. Each Joy-Con 2, or a left and right one together when `gamepad_joycon_pair` is `1`, then plays as a gamepad, a Pro Controller 2 as a Pro Controller, and an Ounce game also gets the Joy-Con 2 mouse sensor and the vibration. Outside the controller applet's dialog it does not search for controllers itself: it tries those that `joycon2.txt` in the user's config folder (`~/.config/scarletnx`) lists, a line each (`AA:BB:CC:DD:EE:FF L`, `R` for a right Joy-Con 2, `P` for a Pro Controller 2), every 10 seconds, and one answers while it advertises (a button pressed, or its sync button held). The launcher's Settings > Controllers > Bluetooth finds them and writes that list ([GUI](gui.md)). It ran against a left and a right Joy-Con 2. A controller reports, and takes its vibration, once a connection interval: `MinConnectionInterval=6` and `MaxConnectionInterval=9` (7.5 to 11.25 ms) in the `[LE]` section of `/etc/bluetooth/main.conf`, set as root with bluetoothd restarted, gave about 114 reports a second. `0` leaves Bluetooth alone |
| `joycon2_mouse_scale` | `1` | How far a Joy-Con 2's mouse sensor moves an Ounce game's mouse against the sensor's own counts (`0.1` to `10`). It adds to the host mouse that `game_mouse` gives |
| `gamepad_p<N>_device` (N from 1 to 8) | empty | Player N's controller: empty takes the next to connect, else its SDL GUID as the log names it, `/2` after it for the second of two alike |
| `gamepad_p<N>_connected` | `1` | `0`: the game never sees player N. It always sees player 1, which the keyboard drives |
| `gamepad_p<N>_type` | `0` | The controller the game sees for player N: `0` as it is, `1` Pro Controller, `2` two Joy-Cons, `3` a left Joy-Con, `4` a right one |
| `gamepad_p<N>_swap_ab`, `gamepad_p<N>_rumble`, `gamepad_p<N>_map` | `-1`, `-10`, empty | Player N's own `gamepad_swap_ab`, `gamepad_rumble` and `gamepad_map`; at these values, every player's |

## Shortcuts

The runtime's keys while a game plays. Each is a key as SDL names it (`F1`, `Pause`, `Return`, `Escape`, `A`), after
`Ctrl+`, `Alt+`, `Shift+` or `GUI+` for the modifiers held with it (`Ctrl+Shift+F12`); empty is no key. A key held with
Shift also does what the key alone is set to, unless a setting names it with Shift. A key two settings share does the
first's in this table, and the log says so. A name SDL doesn't know is refused, and the log says the key in effect.
F1's page in the game's menu lists them as they are set, and its **Shortcuts** page sets them: Enter on a row, then
the key (with its modifiers held); Esc cancels, Backspace puts the default back, and a key another row has is refused.

| Key | Default | Effect |
| --- | --- | --- |
| `hotkey_help` | `F1` | Shows the keys' help |
| `hotkey_menu` | `F2` | Opens and closes the game's menu; Back and a controller's Home open it too |
| `hotkey_screenshot` | `F3` | Takes a screenshot |
| `hotkey_fps_limit` | `F4` | Turns the FPS limit off and back on, and says over the game the limit now (`FPS limit: off`), the log the limit before it too |
| `hotkey_thread_dump` | `F5` | Writes every thread's state to the log |
| `hotkey_showcase` | `F6` | Turns `showcase` on and off, said over the game (`Showcase: on`) |
| `hotkey_save_state` | `F7` | Saves the game's state to the selected slot |
| `hotkey_load_state` | `F8` | Loads the selected slot's state |
| `hotkey_next_slot` | `F9` | Selects the next save state slot |
| `hotkey_tracy` | `F10` | Records a Tracy capture, in a build with `SNX_TRACY`. Its length is `tracy_capture_seconds` |
| `hotkey_pause` | `Pause` | Pauses the whole game and resumes it, said over the game (`Pause: on`), or why it did not pause |
| `hotkey_fullscreen` | `F11` | Switches fullscreen |
| `hotkey_fullscreen_2` | `Alt+Return` | A second key that switches fullscreen |
| `hotkey_stats` | `F12` | Turns `stats` on and off, said over the game (`Stats: on`) |
| `hotkey_capture` | `Ctrl+Shift+F12` | Captures the next frame with RenderDoc while RenderDoc is in the game (`renderdoc`) |
| `hotkey_mouse` | `Ctrl+G` | With `game_mouse` at `0`, declares an Ounce game's Joy-Con mouse mode, and ends it. At `1`, lets the mouse go from a game that holds it, and holds it again |
| `hotkey_quit` | `Escape` | Quits the game: a first press warns, and a second within 3 seconds quits |
| `hotkey_tv_mode` | `Ctrl+D` | Steps the TV mode round On, Off and Auto for the run, as the game's menu's **TV mode** row does; `docked` keeps the `.ini`'s choice |
| `hotkey_upscaler` | `Ctrl+U` | Steps the upscaler round Off, Bilinear, AMD FSR 1, NVIDIA NIS and Qualcomm SGSR for the run, said over the game; `upscaler` keeps the `.ini`'s choice |
| `hotkey_anti_aliasing` | `Ctrl+Shift+A` | Steps the anti-aliasing round Off, Auto, FXAA, SMAA, SSAA 2x and SSAA 4x for the run, said over the game; `anti_aliasing` keeps the `.ini`'s choice |

## Network

| Key | Default | Effect |
| --- | --- | --- |
| `network` | `0` | `0` reports offline and lets the game reach only loopback, which is not a network sandbox. `1` uses the computer's connection, and the resolver blocks known console service domains. Local wireless works either way (`ldn`) |
| `airplane_mode` | `0` | `1` is the console in airplane mode: offline whatever `network` says, no local wireless, as the game hears it, and in no Eden room |
| `ldn` | `lan` | Local wireless. `lan` is over the LAN with ldn_mitm's protocol, with another ScarletNX, a Ryujinx or a console running ldn_mitm: with `network=1` the console is the computer's address, with `0` it is `127.0.0.N`, one per ScarletNX on the machine, so two of them play together through loopback. `room` is through the Eden room `room` names, with the Edens and ScarletNXs in it. `off` is local wireless disabled, as the game hears it. Another word stops the game |
| `room` | empty | The Eden room `ldn=room` joins at start: `host` or `host:port` (`24872` without one); empty with `ldn=room`, or with a space in it, stops the game. The console is there as the user playing, by its nickname ([Users](usage.md#users)), at the address the room gives it (`192.168.x.y`), which nifm gives the game too, and the game's datagrams go through the room. A room with a password takes it from `users/online.ini` ([Folders](usage.md#folders)), a file only you may read (`chmod 600`, not checked on Windows) that holds `room_password=...`: another key that starts with `room_`, or a section, stops the game, and a 3DS game joins with no password, saying why. A key of another version or program is named in the log, never its value, and stays. The launcher's Room password row, beside Eden room, writes that file and leaves its other lines as they were (stars as it is typed; Del takes the password off). A room that refuses stops the game and says why |

## Online

Each console's online service, the launcher's Online group.

| Key | Default | Effect |
| --- | --- | --- |
| `nds_wfc` | `off` | The DS's WFC service, the community's replacement of the closed original, by the DNS server set in the firmware's first Wi-Fi connection: `auto` is the service whose server answers first as the game starts (each asked once, for 1.5 s at most; the log says each one's time and the one taken, and with no answer the connections stay unset), `kaeru` (Kaeru WFC), `wiilink` (WiiLink), `wiimmfi` (Wiimmfi), `wiilink_wfc` (WiiLink WFC), `altwfc` (AltWFC), `newwfc` (NewWFC), `skytemple` (SkyTemple WFC), `diyfi` (DIYFI), `sudomemo` (Sudomemo); `custom` is the server `nds_wfc_dns` names, `off` the DNS of the DS's network. The DS driver reads it from the command line (`--nds-wfc`) or `settings.ini`'s `[Online]`. Another word stops the game, naming it. The DS driver has no wireless yet: a game's Wi-Fi settings show the connection, and no game connects through it ([DS](../platforms/nds/README.md#wfc)) |
| `nds_wfc_dns` | empty | The DNS server `nds_wfc=custom` sets, an IPv4 address (`178.62.43.212`); with `custom`, empty or anything but an address stops the game |
| `ctr_online` | `pretendo` | The 3DS's online service: `pretendo` (Pretendo Network, the community's replacement of the closed original) logs a game in to its server through Pretendo's NASC with the console's own files (`ctr_console`), as a console with Pretendo's Nimbus does; `off` logs in nowhere. The console maker's own domains resolve to nothing either way, and a game reaches nothing off loopback while `network` is `0`. Another word stops the game ([3DS](../platforms/3ds/README.md#online-sockets-and-pretendo-network)) |
| `ctr_console` | empty | The folder of your own 3DS's files, laid out as Azahar's NAND folder (its `nand/`): `rw/sys/LocalFriendCodeSeed_B` and `rw/sys/SecureInfo_A`, which Azahar's Artic Setup Tool copies from the console, and the friends module's save holding the Pretendo account, which it does not copy: Nimbus makes it when it runs in Azahar. Empty: no account, and a game's login does not go out, said once in the log |
| `switch_online` | `0` | The Switch's online service of the console maker (its accounts, friends and game servers) is not offered: the row stays greyed, `Not offered`, on every system, and `0` (Off) is its one value. ScarletNX connects to none of its servers, nor to a service that stands in for them. A game's own internet access is `network` |

## User

| Key | Default | Effect |
| --- | --- | --- |
| `user_uid` | empty | The console's user the game plays as, by its uid: 32 hex digits, as [Users](usage.md#users) lists them; empty is its `[user1]` (`Player` without one). A uid no user has stops the game. `user`, the user's number before it, is read while `user_uid` names none ([Deprecations](deprecations.md)) |

## Debug

The profiler rows whose keys start with `tracy_` only do something in a build with `SNX_TRACY`; `perf_map` is a Linux perf feature and works in any build (`cmake --preset tracy`): the launcher's Settings show them greyed in another build, and the game's menu shows them but picking one there has no effect.


| Key | Default | Effect |
| --- | --- | --- |
| `fsr_input_conventions` | `0` | Reverses what the runtime takes a game's DLSS inputs to mean when it gives them to FSR 2, a bit each: `1` the jitter's sign in x, `2` in y, `4` the motion vectors' sign, `8` inverted depth, `16` motion vectors that include the jitter, `32` FSR's own exposure, `64` HDR color input. For a game whose picture smears. The launcher's Settings have a box for each bit; the game's menu shows the value |
| `log` | `1` | Log detail, `0` to `3`. `2` logs everything, which adds runtime overhead. `3` also logs the profiler's counters every window: the CPU work each kind of resource takes a frame, and the GPU's time in each pass, which needs the device's timestamps and is `vk_gpu_profile`'s work |
| `log_max` | `16` | The game log's size in MiB, `1` to `1024`. Past it the log keeps the run's start and its last lines, saying so in the log and once over the game. The 5 last runs' logs are kept, the ones before this run's in `logs/` beside it |
| `emulated_cores_mode` | `2` | `1` (On) runs a guest thread only while it holds one of the console's cores, one thread per core at a time, in priority order. `0` (Off) runs every guest thread as soon as the host schedules it (`svcGetCurrentProcessorNumber` still answers the thread's core). `2` (Auto) is `0` for an Ounce program, whose eight cores run best without the ordering, and `1` for the others; an Ounce game says so as it starts |
| `auto_stub_ipc` | `1` | A service command the runtime doesn't know answers success, logged as `[undeclared]`, and a service it doesn't have answers every command that way. `0` answers them with the console's UnknownCommandId instead, which shows the gap where the game makes the call |
| `diag` | `0` | `1` saves the data of each new compute shader to `captures/compute`, and each new shader's snx-shaderc corpus entry to `captures/shaders`, for debugging |
| `vk_validation` | `0` | `1` turns on the Vulkan validation layer (`VK_LAYER_KHRONOS_validation`, from the Vulkan SDK or the distribution's validation layers package), which logs each misuse of Vulkan it finds. It slows the game down a lot. Without the layer installed the run only warns. `2` also turns on GPU-assisted validation, which finds out-of-bounds and unwritten descriptor accesses in shaders, and is slower again |
| `vk_pipeline_cache` | `1` | `1` keeps the GPU driver's compiled pipelines between runs, in `vk_pipelines.<driver>.bin` of the game's shader cache, one file for each driver, and its shader objects' binaries in `vk_shaders.<driver>.bin`; a file another GPU or another build of the driver wrote, or one cut inside its header, is not given to the driver, and the log says why. A 3DS game takes it too, and where it draws with shader objects keeps their binaries instead, in `vk_shaders.<driver>.bin`. `0` neither reads nor writes those files, so every pipeline or shader compiles again as in a first run, to measure one |
| `vk_mirror_past_limit` | `1` | `1` makes the GPU mirror of guest memory, where an Ounce game's programs read and write their storage, even where it is bigger than the device's maxBufferSize, as feat/ounce makes it: RADV (4 GiB) makes and runs it, though the validation layer calls it invalid. `0` makes it only where it fits; elsewhere the storage results land through the CPU, several times slower in one game. With `vk_validation` on it is made only where it fits, as the layer would report it. Temporary, to compare |
| `vk_api` | `auto` | The Vulkan version the renderer runs at. `auto` takes the device's, up to 1.3: a device that has only 1.2 runs the 1.2 way where it has the extensions 1.3 took in (`VK_KHR_synchronization2`, `VK_KHR_dynamic_rendering`, the extended dynamic state 1 and 2, `VK_KHR_maintenance4`, `VK_KHR_format_feature_flags2`, demotion, pipeline creation feedback and store op NONE), and is refused, naming what it lacks, where it has not. `1.2` runs the 1.2 way on a 1.3 device too, to test it there: those extensions in place of the 1.3 core, shaders in SPIR-V 1.5, no Vulkan Video decode (FFmpeg's decoder needs 1.3), and the subgroup sizes of the mesh draws and of the compute subgroups of 32 from `VK_EXT_subgroup_size_control` where the device has it. The log says the way and why (`vk_api=1.2 runs the Vulkan 1.2 way on this 1.3 device`); any value but `auto` and `1.2` stops the game, naming it and those two |
| `native_astc` | `1` | `1` samples ASTC textures as the game's blocks where the GPU samples ASTC (most phone and tablet GPUs), in a quarter to a 36th of the memory. `0` decodes them to RGBA8 on the GPU, as on GPUs without ASTC (most desktop ones), to compare the two. On such a GPU `1` is a warning at start ([ASTC](../layers/gpu/astc.md)) |
| `native_bc` | `1` | `1` samples BC1 to BC7 textures as the game's blocks where the GPU samples BCn (desktop GPUs and some phone ones). A GPU without BCn (most phone ones, Mali and Adreno's own driver among them) gets them encoded again on the GPU as ETC2 and EAC as they load, the same bytes a texel, where it samples those (`native_etc2=1`), BC6H decoded to half floats; else all decoded, four to eight times the memory ([BCn without BCn](../layers/gpu/bcn.md)). `0` sends BCn down that path on a GPU that samples BCn too, to compare the two on a desktop GPU |
| `bcn_astc` | `1` | On a GPU that samples ASTC and not BCn (most phone and tablet ones): `1` (Auto) encodes BC1, BC2, BC3 and BC7 textures again as ASTC 4x4 as they load, several times faster than as ETC2 and closer to the game's blocks ([BCn without BCn](../layers/gpu/bcn.md)). ASTC 4x4 is twice BC1's bytes, so once the GPU's memory runs short the BC1 textures loaded from then on are encoded as ETC2 (`native_etc2`) again, half the bytes and slower to load. `2` (Always) keeps BC1 as ASTC whatever the memory. `0` encodes all of them as ETC2. BC4 and BC5 are encoded as EAC either way |
| `native_etc2` | `1` | `1` samples ETC2 and EAC textures as the game's blocks where the GPU samples ETC2 (most phone and tablet GPUs), in a quarter to an eighth of the memory, and so are the BCn textures a GPU without BCn gets encoded again as ETC2 (`native_bc`). `0` decodes them on the GPU, as on GPUs without ETC2 (most desktop ones), and BCn too there, to compare the two. On such a GPU `1` is a warning at start |
| `cpu_extensions` | `0` | `1` decodes LSE atomics, RCpc loads, and PAuth branches and signing with authentication off, for a Switch game. An Ounce game takes its Cortex-A78C's instructions without it ([Ounce](../platforms/nx/ounce.md#the-platform)). It gives no pointer authentication protection. Recompile the game with the same value as each run |
| `gdb_jit` | `0` | `1`, with the GDB stub (`-S` or `-s`), hands the code the JIT compiles to GDB ([GDB](usage.md#gdb-linux)). GDB stops the game for each compile while it is attached, and each compile's object stays in memory for the run |
| `alias_pool` | `1` | `1` has a view of guest memory (MapMemory, MapProcessMemory, code memory's owner and slave) share its source's host pages, as the console's do, where the host's pages are 4 KiB (Linux and Android). `0` makes every view a copy, which reaches the source at its unmap or at a flushed line after the write, as hosts without a pool run (Windows among them): for tests of that path, and a diagnosis |
| `guest_files` | `0` (iOS `1`) | `1` puts the game's memory in unlinked files of 1 GiB (larger for a large address space) in the temporary folder (Apple), so the system counts its pages as file pages, which it can write to the disk, and not against the app's memory limit (iOS ends an app past it). The system writes the changed memory to the disk from time to time and under memory pressure, up to the game's memory: it needs that much free disk and 2 GiB more, or the game keeps anonymous memory. The files go away when the game ends. `0` is anonymous memory. macOS and iOS only |
| `write_tracking` | `0` | How the runtime finds the guest pages written since the caches last looked ([Renderer coherence](../dev/architecture.md#renderer-coherence--lifetime)). `0` is the host's own: userfaultfd's write protection on Linux 6.7 and later, the reservation's write watch on Windows, page protection on the other hosts. `1` is page protection and the fault handler, as macOS, FreeBSD, Android and older Linux track (not on Windows, which says so at start). `2` is none: the caches compare guest memory's contents, to measure and test that path |
| `write_tracking_hot_pages` | `2` | `1`, with page protection (`write_tracking=1`, or the host's own on macOS, iOS, FreeBSD, Android and Linux before 6.7), leaves a page the scans find written twice in a row writable, without its fault, and reports it written at each scan, for 32 scans; then it is protected again. A render target protects its pages again before each baseline, so its write-back keeps the GPU's pixels ([Renderer coherence](../dev/architecture.md#renderer-coherence--lifetime)). `0` protects every written page again at each scan. `2`, Auto, is `1` where page protection tracks the writes. Nothing where userfaultfd or Windows' write watch tracks them, which `1` says at start. In place of `write_tracking_hot` ([deprecations](deprecations.md)) |
| `memory_traps` | `0` | How the runtime traps guest memory the GPU owns until the CPU touches it ([GPU ownership](../layers/gpu/ownership.md#the-trap-model)). `0` is userfaultfd's minor faults where Linux has them, else page protection, which Windows uses with an exception handler; none yet on macOS and FreeBSD. `1` is page protection and the fault handler, as Android, Linux before 5.13 and Windows trap (Linux, Android and Windows only: elsewhere it says so at start). `2` is none: a GPU result lands in guest memory at its fence, to measure and test that path |
| `host_page` | `0` | The page size the runtime works in, in bytes: a power of two larger than the host's own, up to 1 MiB. `16384` or `65536` runs as a host with 16 or 64 KiB pages does, where views copy and a host page holds several guest pages ([PowerPC64](../hosts/ppc64.md#host-pages-larger-than-4-kib)). `0` is the host's own; any other value runs with the host's own too, said at start |
| `nce_foreign_scan` | `0` | Under NCE, the seconds between two looks at what of the host's process lies in the game's addresses and is not the game's (another owner's mapping over its memory, a run nothing maps), `1` to `3600`; each new finding is logged once. `0` is off: it looks only as the game's code starts and in a crash report ([NCE](../platforms/nx/nce.md#the-guests-addresses-moved-above-64-gib)). Each look reads `/proc/self/maps`, a few milliseconds |
| `gpu_page_check` | `0` | `1` counts the GPU's written pages (`gpu/maxwell/maxwell3d.c` and `write_pages.c`) and the renderer's pending results (`gpu/vk/pending_pages.c`), which the runtime counts by GPU page, by guest address too, and each lookup translates its range through the GMMU as well: the log warns of each write or result only that finds. Slow: for checking those tables |
| `draw_state_check` | `0` | `1` has a draw that keeps an earlier draw's shaders and pipeline make them again to compare, and the log names each difference (a register missing from a group, an input that is not compared, a kept key that is not the state's bytes). A kept constant buffer's bytes are hashed again (where the CPU wrote nothing since), a kept texture view or sampler is made again, and a descriptor kept from an earlier frame is encoded again, and compared too. A texture the quick check finds unchanged is checked the full way too, and the log counts where the full check found a change. The buffers and images of a draw past the count it has of each, which a draw leaves unwritten, hold a pattern, so a read past a count shows. Slow: for checking what a draw keeps |
| `pool_map_shadow` | `0` | `1` has the renderer keep a record of what each texture and sampler header of the game's pools resolved to, which the header's writes, surface changes, draws into a cropped target, the scale, image swaps and views destroyed clear, and compare each draw's textures with the records it holds. The log counts the agreements and the differences by cause every 600 presents, and with `draw_state_check=1` names the first differences. Draws still use their own textures, so nothing on the screen changes. Slow: for checking the map that bindless textures will read |
| `tex_events_shadow` | `0` | `1` judges each texture a draw keeps by the generations by address of the writes over its memory (the CPU's, the GPU side's, results added, targets drawn, surfaces made or freed): unchanged generations claim its texels are current. The renderer still checks each texture as before, and the log counts every 600 presents the claims each check contradicted, and with `draw_state_check=1` names the first ones. Nothing on the screen changes. Slow: for checking the events that bindless textures will take (`docs/dev/draw-cost.md`, item 7b) |
| `pool_map` | `0` | `1` has an Ounce game's textures, where the GPU reads its global memory by address and has unified image layouts, come to each draw through a map of the game's texture and sampler pools on the GPU, which the draws that sampled them before taught: a draw writes no word for a texture the map holds, and leaves out the read barriers that the barriers at the writer cover (`writer_barriers`, on with it). The log counts the textures through the map, and why the others are not, every 600 presents. For measuring the step to bindless textures |
| `writer_barriers` | `0` | `1`, with unified image layouts, puts an image that a pass, a copy, an upload or a storage write changes on a list, and the next rendering pass starts with one barrier that makes those writes visible to every later read. The barriers each read makes come too, so nothing on the screen changes: the log counts the images in the batches and the read barriers they cover every 600 presents. For measuring the step to bindless textures that moves the barriers to the writer |
| `profile` | `0` | `1` writes `profile.txt` every 10 seconds. The CPU profiler is Windows only |
| `hooks` | `1` | `1` runs the native handler of each of the game's exported functions the runtime has one for (`nn/hooks.hook`: the C library's `memcpy`, `memmove` and `memset`) instead of its own code. `0` runs every function's own code, to tell whether a problem comes from a hook ([Hooks](../platforms/nx/hooks.md)) |
| `hooks_off` | empty | The families of hooks (`hooks.hook`'s family lines, such as `libc`) whose functions run their own code, and the hook modules turned off ([Hooks, Modules](../platforms/nx/hooks.md#modules)), separated by commas or spaces, to tell which a problem comes from. Empty, every family and module is on. The launcher's Settings have a box for each family; the game's menu shows the value |
| `hook_modules_dir` | empty | The folder of the hook modules built as libraries ([Hooks, Modules in libraries](../platforms/nx/hooks.md#modules-in-libraries)): each `NAME.so` there (`NAME.dll` on Windows, `NAME.dylib` on macOS) is the module `NAME`, loaded at start when its hook ABI is the runtime's and `hooks_off` doesn't name it. Empty, `hook_modules` in the user's data folder. The recompile reads it too, and routes the functions the modules' CPU hooks name, so a recompile after installing a module gives its CPU hooks their calls ([CPU hooks in libraries](../platforms/nx/hooks.md#cpu-hooks-in-libraries)) |
| `hooks_verify` | empty | The families of hooks whose pure hooks are checked on every call: the function's own code runs on copies of the memory it writes, then the handler on the game's, and the log names each call whose result, registers or bytes differ ([Hooks](../platforms/nx/hooks.md#verify-mode)). Slow: for checking a hook, never for playing. The launcher's Settings have a box for each family; the game's menu shows the value |
| `native_cond_var` | `1` | `1` has the game's `std::condition_variable`, `std::mutex` and `std::recursive_mutex` wait on the host's own (`hooks.hook`'s `cond_var` family), and save states are refused, as a state can't hold a thread waiting there. `0` has the game's own code wait, and save states can be taken and loaded ([Hooks](../platforms/nx/hooks.md)) |
| `native_os_sync` | `1` | `1` has nn::os's sleeps, yields, locks and condition variables (`hooks.hook`'s `os_sync` family) and its light events, semaphores and message queues (`os_light`) run on the host with no SVC, a lock's waiters waiting on its word with the host's futex, and save states are refused, as a state can't hold a thread waiting there. `0` has the game's own code make the SVCs, and save states can be taken and loaded ([Hooks](../platforms/nx/hooks.md)) |
| `gpu_hook_trace` | `0` | How many of an NVN game's draws and dispatches the log names: each dispatch's program hash, call site in main, grid, block, buffers and the images its handles name, each draw's fragment program hash, targets and viewport, and the records between them that aren't binds ([Hooks, Finding a game's passes](../platforms/nx/hooks.md#finding-a-games-passes)). For finding the passes a hook module replaces and the game's code that records them. `0` = none |
| `gpu_hook_trace_from` | `0` | How many presents `gpu_hook_trace` lets go by before it logs, to trace a scene the game reaches later. It marks each present while it logs. `0` = from the start |
| `gpu_hook_trace_words` | empty | With `gpu_hook_trace`, words of a constant buffer each traced draw binds, as floats: its stage (0 vertex to 4 fragment, 5 task, 6 mesh), slot, byte offset and how many 32-bit words (at most 128), separated by spaces or commas, such as `0,1,0x40,16`. A draw whose words are the ones logged before says so |
| `shader_corpus` | empty | A folder that keeps each translated shader for `snx-shaderc`. With it every shader is translated, and none is kept for the next run |
| `pipeline_stats` | empty | A CSV file that gets the driver's statistics of each whole and optimized graphics pipeline, and of each compute pipeline: for each shader stage, the instructions, registers, spills and the others that the driver reports, by the program hashes of the log's lines (a shader corpus file of the program starts with the same first 8 hex digits). Draws that bind shader objects get no rows. On some drivers, pipelines made with it are not the ones in the pipeline cache of runs without it |
| `gl_call_census` | empty | A TSV file that gets, for a game that draws through its own OpenGL driver, each GL function it calls: the calls, the frames it is called in, the most in one frame, the first and last frame, and the driver's own calls of it apart. The driver's exported `_gl` functions and the addresses its `eglGetProcAddress` answers are counted in their dispatch slots, then run as before. Written every 600 frames and as the game ends |
| `astc_dump` | empty | A folder that keeps each whole ASTC texture level the game loads, as an `.astc` file (astcenc's format, the layers or slices as its depth), for `snx-test-astc-vulkan --bench` and the BC7 encoder's measurements (the game's own textures: keep them local). At most 2 GiB a run |
| `sm86_dump` | empty | A folder that keeps the bytes of each Ounce program (SM86) the translator refuses, and of each block of shader memory with no whole program in it, for `tools/snx_sm86.c` (the game's own code: keep it local) |
| `gpu_trace_target` | empty | `address[:bytes]`: logs the clears and draws that write a render target starting in that GPU address range |
| `vk_early_copy_back` | `1` | `1`: a render target result that the game read back in the last 1200 presents is also copied to system memory as it is made, so the game's next read copies those bytes with no readback of its own. Discrete GPUs only |
| `vk_early_landing` | `1` | `1`: a result copied back early lands in guest memory on a thread of its own once the GPU has made it, so the game's read finds the bytes there and does not stop to land them |
| `vk_gpu_profile` | `0` | `1` logs the GPU's time per rendering pass every 10 seconds, with the slowest passes and what they held. `log=3` asks for the same counters, the CPU's per resource as well |
| `vk_checkpoints` | `0` | `1` logs the work each queue reached when the device is lost (`VK_NV_device_diagnostic_checkpoints`). On NVIDIA the driver also keeps shader debug info and what each resource is for the fault's report (`VK_NV_device_diagnostics_config`), at a cost of speed. `0` makes the device without that extension, so a RenderDoc capture also replays on a driver that lacks it |
| `vk_debug_hide_commands` | empty | Vulkan device commands, separated by commas, that the check of each extension's commands takes as missing from the driver, to test that path: an extension that requires one goes off, and the device is made again without it, as with a driver that lists the extension without the command ([Driver bans](../dev/development.md#driver-bans)). The log says each. Empty: none. A name that is no device command of a Vulkan extension stops the game, naming the nearest ones. The launcher's Settings have a box for each command of the extensions a renderer can use; the game's menu shows the value |
| `vk_memory` | `0` | How the GPU reads and writes guest buffers (vertices, indices, storage, the engines' results; [GPU ownership](../layers/gpu/ownership.md)). `1` copies them into the renderer's buffers when they change, and back. `2` reads and writes them where they are in guest memory (`VK_EXT_external_memory_host`; on Android without it, guest memory moves into AHardwareBuffers), as the console's GPU does: for a GPU that shares system memory; on a discrete GPU it goes across the bus. `0` is `2` on a GPU that shares system memory, but never the moves into AHardwareBuffers (unless `uma_memory=2`), and on a discrete GPU of amdgpu's drivers through Linux's dma-bufs (an import of a host pointer there costs every submission), and on Windows on a discrete GPU with resizable BAR and host imports where the GPU mirror can be (sparse residency of buffers): what the GPU only reads is copied there (`uma_copies`), and what it writes stays in its own memory or in the mirror. Else `1`. A 3DS game takes it too (the launcher passes it to `scarletnx-3ds` as `--vk-memory=N`), for the board's RAM: `1` has the GPU work on a copy of it in its own memory, kept in step by page, `2` on the RAM in place, `0` the same choice by the device ([3DS](../platforms/3ds/README.md)) |
| `uma_memory` | `0` | Android only. Where `vk_memory` has the GPU read guest memory in place, how it reaches it, to compare the two ways on one device. `0` (Auto) imports it where it is (`VK_EXT_external_memory_host`), and without that moves it into AHardwareBuffers when `vk_memory=2`. `1` (Import) only imports: without `VK_EXT_external_memory_host` guest memory is copied. `2` (Move) only moves it into AHardwareBuffers, on a driver that imports too and with `vk_memory=0` as well; elsewhere than Android there are none, and guest memory is copied. The log's `guest buffers` line says the way taken |
| `uma_copies` | `0` | Where `vk_memory` reads guest memory in place, whether vertices, indices and constant buffers are read there too. `0` (Auto) copies them on amdgpu's drivers (RADV, AMDVLK, AMD's on Linux), where each import costs every submission, and on a discrete GPU that imports host memory, whose reads cross the bus: there read-only storage (a draw's and a dispatch's) is copied too. It reads them in place on a discrete GPU through dma-bufs, and elsewhere. `1` reads them in place, `2` copies them, and read-only storage too on a discrete GPU. What the GPU writes, and a range a write in place is pending over, stay in place either way ([GPU ownership](../layers/gpu/ownership.md)) |
| `vk_buffer_stats` | `0` | `1` logs what the renderer copies between guest memory and its buffers |
| `vk_draw_runs` | `0` | `1` logs every 600 presents how many direct draws came in runs that one Vulkan call could draw: with nothing recorded between two draws, with the index buffer's offset alone, and with descriptors, push constants and vertex buffer offsets too. Counted on the recording thread's ring (`vk_record=1`) |
| `vk_sparse_share` | `1` | `1` lets the blocks of a sparse texture that show the same memory of the game share one piece of GPU memory, where the GPU can bind memory to more than one place of an image. `0` gives each block its own |
| `vk_stage_memory` | `0` | Which host memory holds the buffers that the CPU fills each frame for the GPU to read. `1` is memory for writes in order, which a driver can map write-combined, where small copies are slow. `2` is cached memory, where the GPU has such a type. `0` is `2` on Intel's Mesa driver, else `1` |
| `vk_buffer_pages` | `1` | `1` keeps the guest memory that draws read by page, in sparse buffers, or in slabs on a GPU with no sparse buffers. Ranges that overlap share their bytes, and a range that did not change is not copied again. `2` also keeps what a draw writes to guest memory there, on the GPU, until the game's CPU code touches it. It needs working write tracking. In slabs it only reads, so `2` acts as `1` there |
| `vk_stream_ring` | `1` | `1` sends the bytes that a copy or a draw recorded at once reads (texture uploads, the pages of `vk_buffer_pages`, the copies of the buffer cache) through chunks that are free again when that work is done. `0` sends them through the frame's rings, which come back when the frame's slot does |
| `vk_buffer_slabs` | `0` | `1` makes `vk_buffer_pages` keep its bytes in slabs, buffers with all their memory, on every GPU. That is its way on a GPU with no sparse buffers, and the value is for testing. `0` uses slabs only on such a GPU |
| `nvn_storage_order` | `2` | How NVN draws that write storage buffers are ordered. `2` lets draws whose writable views cover the same buffer overlap, as NVN does until the game's own barrier, and puts the Vulkan barrier that barrier asks for at the next buffer use that depends on it. `1` puts it where the game's barrier is. `0` orders each such draw after the ones before it, as Maxwell draws are, to compare ([NVN storage ordering](../history/gpu-hotpath-bench.md#measure-nvn-storage-ordering)) |
| `vk_descriptors_mode` | `3` | `3`, Automatic, is `2` on a GPU that shares the system's memory (an integrated one, a phone's) and `0` on one with memory of its own, where heaps would write each draw's descriptors on the GPU thread and sets let the recording thread write them. `2`: graphics descriptors in heaps where the device has them, else buffers, else sets, and a heap draw whose resource cannot be encoded uses buffers or sets. `1` uses descriptor buffers where supported, else sets. Heap encodings, index tables and buffer descriptors stay unchanged until their frame retires. Compute keeps its own pools. With buffers or heaps in use, the guest fragment barriers that target/buffer tracking orders are skipped. `0` uses descriptor sets, where a fragment barrier ends the render pass only after storage writes or reads that are still pending, or after guest memory used in place. See [descriptor measurements](../history/gpu-descriptor-bench.md) |
| `vk_cb_address` | `1` | `1`: with descriptor heaps, a constant buffer that a shader reads only at constant offsets is mapped by its device address, and its draws write no descriptor for it. A buffer the shader reads at an offset from a register keeps its descriptor, whose range bounds the read. `0` gives every constant buffer a descriptor |
| `vk_cb_windows` | `2` | How the vertex and fragment programs' constant buffers are bound. `2`, Automatic: as 64 KiB windows of the staging ring with their offsets in push constants, where the GPU takes 224 bytes of push constants and uniform ranges of 64 KiB and its descriptors are not in heaps, but not on lavapipe, whose linear rasterizer smashes its stack with them. `1` takes windows on lavapipe too, which needs Mesa's `LP_PERF=no_rast_linear` (a test runs it so); a GPU that can't take them binds them one by one and says why. `0` binds each constant buffer one by one |
| `vk_dynamic_cbufs` | `1` | `1`: with descriptor sets (not descriptor buffers or heaps), the vertex and fragment programs' constant buffers that move most are dynamic uniform buffers. Their offsets go with the bind of the set, not in it, so a draw whose constant buffers only moved keeps its set. The device's limit on dynamic uniform buffers caps how many. The game's moves of each constant buffer are kept in its shader cache when it closes (`<game>.vkcbmoves`), and the next start chooses from them. `0` binds every constant buffer as a plain uniform buffer |
| `vk_copy_indirect` | `1` | `1` makes the copies of GPU-owned results into one buffer with one command, from a list the GPU reads, where the device has `VK_KHR_copy_memory_indirect` and device addresses. `0` uses one copy command each |
| `vk_inplace_loads` | `1` | `1` makes a block linear texture load read the bytes that GPU results hold where they are, where the device has device addresses, and the CPU's bytes from staging. `0` copies the results' bytes into staging first |
| `vk_record` | `1` | `0` records Vulkan commands on the GPU thread instead of a thread of their own |
| `vk_mesh_path` | `0` | How a mesh draw with a task program runs, where the device has `VK_EXT_mesh_shader` and no ban holds. `0`, Auto: the task program runs as a compute dispatch that writes its payload into a buffer, and the draw has no task stage, which removes the task+mesh gang submission that faulted NVIDIA's driver and hangs RADV on RDNA4 ([Driver quirks](../layers/gpu/driver-quirks.md#mesh-shaders)). `1`, Native, submits the two stages together, as the console's hardware runs them. `2`, Compute, is what Auto takes. A direct and an indirect draw both take it; a device that binds graphics descriptors in heaps keeps the native path, because a heap's bind is the graphics pipeline's |
| `gpu_split` | `2` | `1` sets up draws on a renderer thread of their own, while the GPU thread reads the game's commands. `0` does both on the GPU thread. `2` (auto) runs each now and then for a moment and keeps the one that measures faster |
| `compute` | `1` | `0` skips every compute shader dispatch |
| `hmma_f16` | `0` | `1` sums an SM86 HMMA's products in half floats, two a packed `OpFma`, where the console's tensor core keeps the sum in f32 and an f32 emulation needs two instructions for the same pair: about 2 and a half times the frames a second on a device whose driver offers no matrix unit, and a result is then up to 0.4% of its products' size off (more than that of itself where they cancel), a product or partial sum past 65504 an infinity. `0` is the f32 sum. A device with the matrix unit (`VK_KHR_cooperative_matrix`) uses it in compute; one with the mixed dot product (`VK_VALVE_shader_mixed_float_dot_product`) or without `shaderFloat16` keeps the f32 sum, and the log says so |
| `hmma_mode` | `0` | How an SM86 HMMA runs where the device's driver offers no matrix unit (`VK_KHR_cooperative_matrix`). The fastest way differs by GPU and driver: the HMMA bench ran A and B through shared memory 35 times faster than the gather on an Adreno 610 (Turnip) and 2.2 times slower on an RX 6700 XT, and A-stationary 5.6 times faster than the gather on a GT 1030. `0`, Auto: at the first start of an Ounce game with a GPU and driver version, the HMMA bench times each way (a second or less) and the fastest is kept in the cache folder's `shader_cache/hmma.<driver>.txt`, which later starts read (delete it to time them again); the rules' way stays unless another is 3% faster, and where the bench cannot run. The log says the times and the way. `1` Gather: each lane takes its A and B words from the others by shuffles. `2` Pairs: lanes 4 apart share B's words, half each. `3` Staged: A and B through a slot of shared memory a warp. `4` A-stationary: each product in the lane that holds its A words, only B's words from the others, by shuffles. `5` A-stationary staged: the same, B through the slot. They differ only in the order of the f32 sums (A-stationary's a tree of at most 6 roundings a term, the others' a chain of 16), all within an HMMA's bound. `hmma_f16=1` keeps its half sums in the first three ways. It takes the place of `hmma_astat`, which is not read ([Deprecations](deprecations.md)) |
| `hmma_narrow_lds` | `0` | `1` reads an SM86 LDSM transposed tile's halves with 16-bit shared loads instead of a 32-bit load and a shift and mask per half. RADV compiles those to `ds_read_u16_d16`: a Steam Deck's HMMA gather runs 3% and its pairs 9% faster, the stage-only path unchanged. The module keeps a 16-bit view of shared memory in sync with its whole-word stores; a program with a shared atomic or a sub-word shared store takes the general path. `0` = the general path |
| `vk_submit_every` | `512` | The draws, dispatches and copy bands that a submission holds at most: what was recorded goes to the GPU after this much work, so the GPU starts on a frame while the rest of it is recorded (Eden's renderer submits every 4096 draws). `0` submits only where the GPU's results are needed (a present, a wait, a readback) |
| `vk_max_allocation_mb`, `vk_budget_mb` | `0`, `0` | A smaller cap on each Vulkan allocation and on the GPU memory the caches keep, in MiB, for testing. `0` is the device's |
| `renderdoc`, `renderdoc_frames` | empty | RenderDoc captures to that path's stem of the presents listed (`500,800`, or `45s` for the first after 45 seconds), and the capture key (`hotkey_capture`) or the game's menu's Capture frame captures the frame after the next present. RenderDoc writes a capture inside the present that ends it, and the game stops meanwhile (a minute for a frame of 1.4 GB): a notice over the game says so as the capture is asked or RenderDoc's program starts its own, the log says how long it has been writing every 5 s, and the log and a notice say how long it took, the file and its size once written, or that it failed. A game RenderDoc's own program started takes them and the presents listed too, its captures where that program keeps them when `renderdoc` is empty; F12 stays the stats' key |
| `tracy_profiler` | `0` | `1` starts the run with the Tracy client up, in a build with `SNX_TRACY` ([Profiling](../dev/development.md#profiling)): connect the GUI to `127.0.0.1:8086`. A capture and the GUI cannot both have the run |
| `tracy_capture` | `0` | `1` records this run to a trace beside the game and opens it in the profiler when the run ends; the client starts for it, so the GUI cannot have the run as well. `tracy-capture` must be on PATH. F10 starts a capture while a run goes |
| `tracy_capture_seconds` | `60` | Seconds both a capture started by F10 or the game's menu and a capture of the whole run (`tracy_capture`) record for. The client starts for the capture, so the GUI cannot have the run at the same time. A run capture can also end at game exit, before the seconds pass |
| `tracy_sampling` | `0` | `1` starts the sampler at the first connection, for the trace's flame graphs |
| `tracy_zone_callstack` | `0` | The call stack depth, `0` to `16` frames, on zones written with `SNX_ZONE_CS`: `pipeline make compile callstack`, `shader translate maxwell`, `kern core switch`, `kern core handoff`, `kern mutex handoff`, `kern svc unknown`, `svc hid cmd`, `state memory check`, `state memory load` and `state compress`. `0` takes no call stack. A larger depth gives flame graphs more frames but costs more time and space in the trace ([Profiling](../dev/profiling.md#tracy-the-whole-run-on-one-timeline)) |
| `tracy_gpu_zone_callstack` | `0` | The call stack depth, `0` to `16` frames, added to each pass zone on the GPU track: `0` adds none, and a larger depth walks the CPU stack where the pass is recorded. This costs much time, so use it on pass-level zones only ([Profiling](../dev/profiling.md#sections-lock-marks-and-callstacks)) |
| `perf_map` | `0` | `1` writes a Linux perf map (`/tmp/perf-<pid>.map`) for the code the runtime's JIT compiles, so a sampled flame graph names those frames instead of `[unknown]`. The map is written as loads compile, and any lines not yet emitted are appended at exit; `0` writes no map |
| `tracy_memory_detail` | `0` | `1` emits a Memory view event for each game `malloc`, audio and video buffer, and GPU object, so the trace shows which pool holds the bytes. Every game `malloc` and `free` then pays for a Tracy event, so leave this off unless the trace needs those pools; guest mappings and Vulkan device memory are always tracked ([Memory view](../dev/profiling.md#memory-view-which-allocations-have-a-pool)) |
| `tracy_memory_callstack` | `0` | The call stack depth, `0` to `16` frames, added to the detailed memory events of `tracy_memory_detail`: `0` adds none, and a larger depth shows the code that made each allocation. Every game `malloc` and `free` then pays for the stack ([Profiling](../dev/profiling.md#sections-lock-marks-and-callstacks)) |
| `tracy_context_switches` | `0` | `1` puts the OS's context switches on the threads' timelines, so a thread's wait and preemption show against the game's zones. On Linux this needs permission to read perf events and tracefs. Without it Tracy writes a debug line and leaves the feature off. Tracy starts its sampler with it, so the sample stacks come too |
| `tracy_lock_marks` | `0` | `1` marks the source line that holds each named lock, so a trace shows which lock a thread holds or waits on and both results of a try lock. Each lock acquisition then costs one Tracy event, so leave this off unless the trace needs lock waits ([Profiling](../dev/profiling.md#sections-lock-marks-and-callstacks)) |
| `tracy_frame_images` | `0` | Seconds between the pictures of the presented frame the trace gets. A picture is a readback of the frame the GPU has just presented, which stops the frame it lands in for as long as that takes: off unless the trace needs to say which zone a picture goes with |
| `tracy_port` | empty | The port the client listens on and the capture connects to, `1` to `65535`. Empty is Tracy's 8086, or one of the run's own ports when a capture is requested |
| `tracy_capture_tool` | empty | The `tracy-capture` to start: a path, or a name on PATH. Empty looks beside the runtime, then on PATH |
| `tracy_open_trace` | `1` | `1` opens the trace in `tracy-profiler` when the run ends |

## Timed runs

The unattended runs' settings ([Run / diagnose](../dev/development.md#run--diagnose)): read from the command line as the
`.ini`'s are, `--key=value`, but in no section, so never written back.

| Key | Default | Effect |
| --- | --- | --- |
| `seconds` | `0` | The run ends after this many seconds; `0`, it doesn't |
| `frames` | `-1` | The run ends after this many presents; `-1`, it doesn't |
| `shot` | empty | Where the run's screenshots go: the path's stem |
| `shot_every` | `0` | A screenshot every this many presents; `0`, at the end only |
| `press_len` | `300` | How long a scripted press is held, in ms |
| `script` | empty | The scripted input: `when:buttons`, a comma between two, when in ms or a present's number and `f` (`300f`) |
| `input_record` | `0` | The game's slot player 1's input is recorded to as it runs, `1` to `9` (`0`, none): `tas/<id>.<slot>.snxtas` in the game's folder, replaced, a line `frame:buttons:lx,ly,rx,ry` each present the input changed (frame the presents since the game's start, buttons in hex as the script's) |
| `input_replay` | `0` | The game's slot whose recording is played as player 1's input present by present, `1` to `9` (`0`, none): neutral before its first line, the controls again at the present after its last line's |
| `swkbd_text` | empty | The text the software keyboard answers instead of asking (shorter than the game's minimum: cancelled) |
| `pause` | empty | Pauses of the whole game, a comma between two: `at:length` pauses it at that many ms of the scripted input's clock for `length` ms, and logs whether guest memory changed while it was paused |
| `state_save` | empty | Save states the run writes, a comma between two: `at:slot` saves to that slot at that many ms of the scripted input's clock |
| `state_load` | empty | Save states the run loads, a comma between two: `at:slot` loads that slot at that many ms of the scripted input's clock |
| `state_check` | empty | Save state round trips, a comma between two: `at` saves the game into the check slot at that many ms of the scripted input's clock, loads it back at once, and logs whether guest memory and guest time came back the same |

## 3DS driver

`scarletnx-3ds --key=value` ([3DS](../platforms/3ds/README.md)). The launcher's Settings show them in the 3DS group,
the screens' rows on it and the others on its Debug page, and pass each one picked to a 3DS game's run. `ctr_script`
alone is a run's own, given on the command line (`--script=` too) and on no page.

| Key | Default | Effect |
| --- | --- | --- |
| `ctr_nasc_url` | empty | The NASC a 3DS game's login posts to with `ctr_online=pretendo`: empty is Pretendo's, `https://nasc.pretendo.cc/ac/`; another `http://` or `https://` URL is a NASC of your own (a Pretendo server run locally) |
| `ctr_ipc_trace` | `0` | `1` logs each service request the 3DS game sends, its service and header, in the order sent |
| `ctr_hooks` | `1` | `1` runs the 3DS game's IPC stubs (the SDK's functions that send a service request) in the runtime, which sends their requests to the services without the TLS command buffer and the SVC, and runs a GX command at the SDK's enqueue. `0` leaves every request to the game's code |
| `ctr_svc_trace` | `0` | `1` logs each SVC the game makes that answers a failure (`r0` with its top bit set), `2` every SVC, with the thread, its pc and `r0` to `r3` before and after |
| `ctr_stand_in` | `1` | With `async_pipelines=1` on a GPU with shader objects (`VK_EXT_shader_object`), `1` draws a draw whose shaders are still being made with stand-in shaders: one that runs the vertex program from a buffer, and one that reads the fragment state as it goes. They draw it as its own shaders would, so it shows at once instead of a few frames late. A draw with a procedural texture, gas or shadows is still skipped. `0` skips every such draw until its shaders are made. `2` gives the stand-ins every draw they can draw, to test them |
| `ctr_gpu_trace` | `0` | `1` logs each GX command the game queues, and each framebuffer swap the display takes or drops, and why |
| `ctr_uds_trace` | `0` | `1` logs each local wireless packet of the game that goes through an Eden room ([local wireless](../platforms/3ds/README.md#experimental-local-wireless-over-rooms)): its direction (sent, received, delivered), the time, its size and its first 96 bytes in hex |
| `ctr_jit_trace` | `0` | `1` logs each block the runtime compiles while the game runs (the static module lacked it), Thumb ones with the pc's low bit set as the hints file writes them |
| `ctr_auto_stub_ipc` | `1` | `1` answers success to a command of a 3DS service the runtime doesn't know, and has a stand-in answer each command of a service it doesn't have, each logged the first time, as `auto_stub_ipc` does for a Switch game. `0` stops the run at the first, saying which, but for the services `--ipc-stubs` names |
| `ctr_script` | empty | The game's scripted input (`--script=` too): `when:buttons`, a comma between two, when in ms from the start and buttons in hex as `hid:USER`'s pad has them (1 A, 2 B, 4 Select, 8 Start, 10 Right, 20 Left, 40 Up, 80 Down, 100 R, 200 L, 400 X, 800 Y, 4000 ZL, 8000 ZR), each held for `press_len` ms |
| `ctr_model` | `auto` | The console the game runs on: `auto` the one it was played on before, which its `.ini` records (`model=`), and for its first run a New 3DS where the game is New 3DS exclusive or asks for the New 3DS's 178 MiB memory mode, else an Old 3DS; `old` and `new` that console, as `--model`, which wins over it. Another value stops the game, saying so. A game's own Settings (M on it in Games) pick `old` or `new` for that game alone, `ctr_model=` in its picks (`<title ID>.picks` beside its `.ini`): under `--model` and a `--ctr-model=` on the command line, over this one, which the launcher gives with `--launcher-picks=1`; another value there is said with the one it keeps |
| `ctr_screen_layout` | `0` | `0` stacked, `1` one screen, `2` large screen, `3` side by side, `4` separate windows |
| `ctr_screen_swap` | `0` | `1` swaps the screens' places, and shows the bottom one alone in the single-screen layout |
| `ctr_top_resolution` | `1` | The scale of the top screen's 3D rendering, 1 to 16: `1` is the console's 400x240, `2` 800x480; the screen is scaled to its window |
| `ctr_bottom_resolution` | `1` | The same for the bottom screen: `1` is the console's 320x240, `2` 640x480 |

The Switch's `upscaler`, `upscaler_sharpness` and `anti_aliasing` (Graphics) scale each screen to its place in the
window, and the start says which run.

## Vita driver

`scarletnx-vita --key=value` ([PS Vita](../platforms/vita/README.md)). The launcher's Settings show them on the Vita
group's Debug page and pass each one picked to a Vita game's run; the lockstep rows are greyed in a build without
`-DSNX_VITA_LOCKSTEP=ON`. `vita_script`, `vita_press_len` and `vita_shot_every` are a run's own, given on the command
line and on no page.

| Key | Default | Effect |
| --- | --- | --- |
| `vita_hle_trace` | `0` | `1` logs each call of an imported function the runtime doesn't implement, `2` every call, with its thread, `r0` to `r3` and its result |
| `vita_jit_trace` | `0` | `1` logs each block the runtime compiles while the game runs (the game's module lacked it), Thumb ones with the pc's low bit set as the hints file writes them |
| `vita_gxm_trace` | `0` | `1` logs each scene, draw and display queue entry the game makes through sceGxm |
| `vita_shader_corpus` | empty | A folder the runtime writes each GXP program it translates to, with what it was translated for, as `<hash>.gxp`, which `tools/snx_vita_usse.c` translates again (the game's own code: keep it local) |
| `vita_script` | empty | The game's scripted input (`--script=` too): `when:buttons`, a comma between two, when in ms from the start and buttons in hex as SceCtrl has them (1 Select, 8 Start, 10 Up, 20 Right, 40 Down, 80 Left, 100 L, 200 R, 1000 Triangle, 2000 Circle, 4000 Cross, 8000 Square, and the left stick pushed all the way: 1000000 right, 2000000 left, 4000000 up, 8000000 down), each held for `vita_press_len` ms |
| `vita_press_len` | `300` | How long each press of the script is held, in ms (`--press-len=` too) |
| `vita_shot_every` | `0` | With `--shot=PATH`, also a screenshot every this many vblanks (60 a second), `PATH_0000.ppm` on; `0`, only the last frame's |
| `vita_lockstep` | `-1` | From this import call on (`0` is the first), every instruction group of the game also runs in Unicorn, and the first difference in registers, flags or written memory stops the run with both states ([Checking the CPU backend](../platforms/vita/README.md#checking-the-cpu-backend)). Only in a build configured with `-DSNX_VITA_LOCKSTEP=ON`: another build says so at start. `-1` is off |
| `vita_lockstep_steps` | `0` | With `vita_lockstep`, how many instruction groups are compared before the game runs on without Unicorn. `0` has no limit |

The Switch's `upscaler`, `upscaler_sharpness` and `anti_aliasing` (Graphics) scale the frame to the window, the
launcher passing them, and the start says which run.

## Linux driver

`scarletnx-gnuabi --key=value` ([GNU ABI](../platforms/gnuabi/README.md)), a - for each `_` (`--linux-strace=1`). The
launcher's Settings show them on the Linux group, the diagnostics on its Debug page, and pass each one picked to a Linux
program's run, before its name. Each is what the option of its own is (`--strace`), and the last given counts.

| Key | Default | Effect |
| --- | --- | --- |
| `linux_isolated` | `0` | `1` is no host files and no variables of the host's: `/tmp` in memory, the program alone as `/program`, and `PATH` and `LANG` in its environment (`--isolated`) |
| `linux_root` | empty | The guest's root (`-L DIR`, `--sysroot=DIR`): a guest path names its file when it has one, the host's otherwise, as with QEMU's `-L`. Empty: `linux/` in the data folder. `linux_isolated` takes none |
| `linux_jit_cache` | `1` | `0` keeps no compiled code between runs (`--no-jit-cache`) |
| `linux_host_page` | `0` | Works as a host whose pages have that many bytes, a power of two from the host's page to 1048576 (`--host-page=`); `0` the host's. Not on Windows, which works in 4096-byte pages and refuses another |
| `linux_strace` | `0` | `1` writes each system call, its arguments and its result to stderr, with the process id (`--strace`) |
| `linux_sample_pc` | `0` | `1` writes the guest's program counter to stderr once a second, which shows a loop with no system call (`--sample-pc`). Not on Windows |
| `linux_verbose_jit` | `0` | `1` says what the JIT compiles (`--verbose-jit`) |

## Android driver

`scarletnx-android --key=value` ([Android apps](../platforms/android-apps/README.md)), a - for each `_`. The launcher's
Settings show them on the Android group, the diagnostics on its Debug page, and pass each one picked to an Android app's
run. Each is what the option of its own is, and the last given counts; the JDK is `java_home` ([Launch](#launch)).

| Key | Default | Effect |
| --- | --- | --- |
| `android_java_options` | empty | Options for the JVM, a space between two, as `-J` gives one: `-Xmx2g` |
| `android_log_level` | `D` | The least `android.util.Log` level printed, `V`, `D`, `I`, `W`, `E` or `A` (`--log-level=`); the least `Log.isLoggable` is true for too |
| `android_trace_frames` | `0` | `1` writes each frame the app's windows make to stderr (`--trace-frames`) |
| `android_log_signals` | `0` | `1` logs each guest signal action, send and delivery (`--log-signals`) |
| `android_sample` | `0` | `1` writes, every 5 seconds, each guest thread's pc, x30 and HLE call, and the count of Vulkan presents (`--sample`) |
| `android_jit_verbose` | `0` | `1` says each unit the JIT compiles, before and after, with its time (`--jit-verbose`) |
| `android_linker_verbose` | `0` | `1` says each native library and its load address (`--linker-verbose`) |

## DS driver

`scarletnx-nds --key=value` ([DS](../platforms/nds/README.md)), a - for each `_`. The launcher's Settings show them on
the DS group, the diagnostics on its Debug page, and pass each one picked to a DS game's run. Each is what the option of
its own is (`--aot=0`), and the last given counts. The WFC service is the Online group's ([Online](#online)), which the
driver reads from the launcher's `settings.ini` itself.

| Key | Default | Effect |
| --- | --- | --- |
| `nds_aot` | `1` | `0` compiles no module before a game with none starts: every block compiles as the run reaches it (a quick look at a game, `--aot=0`; a recompile does it later, from the hints) |
| `nds_trace` | `0` | `1` says each register access and ARM7 request the board does not know (`--trace`), `2` each interrupt taken too |
| `nds_wav` | empty | A WAV file that gets the run's sound, 16 bits stereo (`--wav=`) |
| `nds_dump_ram` | empty | A file that gets `0x01FF8000` to `0x02FFFFFF` when the run ends (`--dump_ram=`) |

## Launch

The launcher's own settings, and the recompiler's, which the launcher and the driver pass on as `--key=value`
([Usage](usage.md)): the launcher's Settings > Launch.

| Key | Default | Effect |
| --- | --- | --- |
| `gdbstub` | `off` | `on` starts the game with the GDB stub on port 1234 ([GDB](usage.md#gdb-linux)) |
| `tmpfs` | `off` | `on` extracts to tmpfs, in memory (the temporary folder's `scarletnx/work/<title ID>`), which the game reads its RomFS from (`--tmpfs`); on Linux and FreeBSD, a temporary folder that is not a tmpfs reads the dump in place instead |
| `extract` | `off` | `on` extracts a dump's ExeFS and RomFS into its game's folder (`--extract-container`). Off, the game reads them from the dump |
| `java_home` | empty | The JDK, 21 or later, that Android apps run on: the folder that holds its `lib/server` (`bin/server` on Windows). Empty is the one the build found; set, the launcher gives it to `scarletnx-android` as `--java-home` |
| `aot_threads` | `0` | The threads compiling the game's code when it is recompiled (`--jobs`): `0` (Auto) starts on 1 and opens one more each second while the memory the code units are seen to take leaves room for it and the system reports no memory pressure, up to the computer's cores with a quarter left free (`aot_threads_most`); `N` is that many, whatever the memory |
| `aot_threads_most` | `0` | The most threads `aot_threads`' Auto opens: `0` (Auto) the computer's cores with a quarter left free; `N` that many at most, whatever the cores. Only Auto takes it: beside `aot_threads=N` the recompile runs on `N`, and says so |
| `unit_instructions` | `90000` (Android `20000`, iOS `20000`) | The instructions a code unit takes, whole functions at a time, when the game is recompiled (`--chunk`). Each is compiled on its own, and a smaller one takes less memory at once, which a phone needs |
| `unit_cache_ceil_mb` | `512` | The most memory, in MB, the x86-64 back end's code units are held in while ScarletNX runs, so that a unit made again (an NRO a game loads again, the launcher's next recompile) is copied rather than made. They take 1% of the machine's memory at most, and this ceiling over that; the ones used longest ago go first. `0` holds none. Nothing is kept on the disk |

## GBA

The launcher's own settings for a GBA game it makes: the launcher's Settings > GBA.

| Key | Default | Effect |
| --- | --- | --- |
| `gba_color` | `gba` | The colors a GBA game shows. `gba` is the original GBA's unlit screen, by near's color emulation article (Talarubi's formula: each channel raised to the screen's gamma 4.0, the three mixed, back to 2.2, scaled by 255/280): the game's colors, painted strong for that screen, come out softer and darker; `raw` is as the game writes them, as mGBA shows them without a shader |
| `gba_covers` | `none` | The picture a GBA game made from a ROM shows in Games. `none` is Wimsy, as a game with no icon; `boxart` is its box from [libretro-thumbnails](https://github.com/libretro-thumbnails/Nintendo_-_Game_Boy_Advance), downloaded as the game is made: the ROM's CRC-32 names the game in [libretro-database](https://github.com/libretro/libretro-database)'s No-Intro list, which the cache keeps (`covers/gba-no-intro.dat`, fetched again once a session when it lacks a ROM), and the box under that name goes in the game's folder as `icon.png`. A ROM the list does not have (a hack, a translation, a bad dump), or a game with no box there, keeps Wimsy, and its row says which ROM, CRC-32 or name was looked for |
