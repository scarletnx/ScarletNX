# Side issue ledger

## Index

* [Workflow](#workflow)
* [The base](#the-base)
* [Fixed in this pass](#fixed-in-this-pass)
* [Open issues, ranked](#open-issues-ranked)
* [Blocked or not for this device](#blocked-or-not-for-this-device)
* [Main feature branch](#main-feature-branch)
* [Counts](#counts)

Every open issue that is not part of the HDR, upscaling, anti aliasing or frame interpolation work goes here. The
work is off the latest `origin/main`, one branch per issue, easiest first. Nothing is pushed. The user or another
agent can take a row and make the pull request from the notes.

## Workflow

1. `git fetch origin main`
2. `git switch -c fix/issue-<number>-<short> origin/main`
3. Fix the smallest complete cause. Follow `CLAUDE.md` and `docs/issues/` practices.
4. Build, run the tests and, where a game or device is needed, run it. Put the commands and the result in the row.
5. Commit signed. Do not push.
6. Fill the row. The suggested PR title starts with the area, as the repository's commits do.

## The base

`origin/main` at `d0c88be9` (`3c60b932..d0c88be9` fetched on 2026-11-03). Every side branch below is based on that
commit. The feature branch is separate; nothing here was pushed.

## Fixed in this pass

| Issue | Title | Branch | Commit | Mechanism | Tests / run | Suggested PR |
| --- | --- | --- | --- | --- | --- | --- |
| #627 | [loader] read_cie reads past the image when a CIE length is below 5 | `fix/issue-627-cie-length` | `8524e42` | The CIE length must hold the CIE id, version and augmentation NUL (6 bytes); a shorter one is `truncated CIE` before the augmentation search. | loader, elf, module, unwind and driver_workflow tests pass. | `[loader] Refuse a CIE whose length cannot hold its fields` |
| #634 | [nvdrv] syncpt_add scans every event slot even when none is armed | `fix/issue-634-armed-events` | `1a3a7c1` | An atomic count of armed event slots, changed under nvev_lk with the slots, makes nv_events_check return at once when none is armed. | nvdrv_service and the GPU service tests pass. | `[nvdrv] Scan the event slots only while one is armed` |
| #733 | [runtime] A numeric fps_limit catches up after a slow frame, so presents go above it | `fix/issue-733-fps-limit-ceiling` | `6fa18fa` | A numeric limit is a ceiling: a frame already due restarts the schedule at once instead of letting the frames after it run to catch up. | runtime_present checks a slow frame under fps_limit=30 waits the next whole period. | `[runtime] Hold a numeric fps_limit to its period after a slow frame` |
| #492 | [driver] Options after -- are ignored, so a timed run never ends | `fix/issue-492-options-after-dashdash` | `8690ac4` | The driver and the runtime stop with the option and say to put runtime options before the `--`; a timed run given after it never ended otherwise. | runtime_unknown_option and driver_workflow pass. | `[driver] Say when a runtime option comes after the --` |
| #673 | [meta] Add SECURITY.md and issue templates for bugs and feature requests | `fix/issue-673-templates` | `d57017e` | SECURITY.md (GitHub's private advisory), bug.yml, feature.yml and config.yml, each saying nothing of a game is attached. | tracked_material passes; the YAML parses. | `[meta] Add a security policy and the bug and feature forms` |

## Open issues, ranked

The rank is easy, medium, hard, or blocked with the thing it waits for. `Not attempted in this pass` means the
cause is known and small enough for one branch, but this pass ended first; take it as the next row.

| Issue | Title | Rank | Why it is not fixed here |
| --- | --- | --- | --- |
| #1135 | Xenoblade Chronicles DE - Framerate drops to 14-18 FPS in certain spots | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #1119 | [qol] A hotkey that cycles TV mode: Auto, docked and handheld, while a game runs | done: feature branch | The `docked_key` hotkey (Ctrl+D, On -> Off -> Auto) and its toast and tests are on `greatgreenstar/feat/hdr-780`. |
| #1116 | [3ds] The GPU doesn't start when the driver can't import the VRAM region, and the game stops at its first draw | done: feature branch | The host-pointer import and its udmabuf fallback are on `greatgreenstar/feat/hdr-780` (`docs/issues/1116-radv-host-pointer-memfd.md`). |
| #1115 | File Picker Broken iOS | blocked: iOS | No iOS device or toolchain on this machine. |
| #1114 | [meta] Tests that need a device or a long run, and the PRs they wait on | medium | Medium; not attempted in this pass. |
| #1112 | [3ds] HDR and the spatial upscaler on the 3DS frontend | done: feature branch | The 3DS per-screen upscaler, anti aliasing and HDR are on `greatgreenstar/feat/hdr-780`. |
| #1106 | [apple] Run the Mac renderer once on KosmicKrisp and record what it lacks | blocked: macOS | No macOS device or toolchain on this machine. |
| #1080 | [vk] Unified memory on Android: the AHardwareBuffer backend on main's UMA | hard | A large design or feature; it needs its own plan and several branches. |
| #1079 | [vk] Unified memory, part B: results published in place, constant buffers read in place, runtime writes wait | hard | A large design or feature; it needs its own plan and several branches. |
| #1067 | [recompiler] TBL/TBX and shifts by register on hosts without PSHUFB, AArch64 TBL or AVX2 | medium | Medium; not attempted in this pass. |
| #1066 | [recompiler] Host features read and never used: SHA-NI, AArch64 SHA2, AVX-512, and none for the other hosts | easy | Small; not attempted in this pass. |
| #1065 | [recompiler] Inline FMA, square root, rounding and conversions on PowerPC64, RISC-V and LoongArch hosts | medium | Medium; not attempted in this pass. |
| #1064 | [test] Run the A64 differential on the cross-built hosts against a recorded QEMU reference | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #1062 | [recompiler] A lowering census of the A64 SIMD, FP and crypto emitters, per host | easy | Small; not attempted in this pass. |
| #1061 | [recompiler] Host-aware A64 SIMD lowering: what main has and what is left | hard | A large design or feature; it needs its own plan and several branches. |
| #1058 | [gpu] A racing game draws its vehicle thumbnails black and its track washed out in white | medium | Medium; not attempted in this pass. |
| #1052 | [gpu] Characters stand sunk into the ground up to their shins | blocked: needs info | The report asks for a log, a build or a reproduction that is not here. |
| #1038 | [shader] Audit against Ryujinx's shader compiler: what we have and what is left | hard | A large design or feature; it needs its own plan and several branches. |
| #1035 | xenoblade chronicles 2 rendering issue | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #1018 | [apple] The app's bundled MoltenVK cannot drive the 3DS GPU, and the installed app cannot find Homebrew's loader | blocked: macOS | No macOS device or toolchain on this machine. |
| #1008 | pokemon mystery dungeon rescue team dx crashes when enter partner name | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #1006 | [vk] Sparse 3D volumes: residency on the GPUs that lack it, and the bind's wait | medium | Medium; not attempted in this pass. |
| #1004 | pikmin 4 not renders nvdec on title screen | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #1002 | [vk] Turnip: no subgroup operations in geometry shaders | blocked: Android | No Android device or toolchain on this machine. |
| #999 | [vk] Without attachment feedback loops, a draw that samples its own target copies it and ends the pass | blocked: Android | No Android device or toolchain on this machine. |
| #998 | [vk] Patch BCn support into Adreno drivers that lack it, through libadrenotools | blocked: Android | No Android device or toolchain on this machine. |
| #996 | [nce] Count outcalls by kind, and the game threads' CPU time, in the stats lines | easy | Small; not attempted in this pass. |
| #993 | [ipc] Count requests per service and command before choosing IPC functions to hook | easy | Small; not attempted in this pass. |
| #968 | [gpu] Disable a game's dynamic resolution | medium | Medium; not attempted in this pass. |
| #966 | [qol] Quality-of-life: upscaling, TV mode Auto, dynamic resolution | partly: feature branch | Upscaling and TV-mode Auto are on `greatgreenstar/feat/hdr-780`; dynamic resolution is still open. |
| #950 | [gpu] Run OpenGL games through NVN instead of decoding their GPFIFO | hard | A large design or feature; it needs its own plan and several branches. |
| #937 | [game] Fire Emblem Fates (Birthright) | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #885 | [shader] A dedicated SM 5.3 instruction decoder | hard | A large design or feature; it needs its own plan and several branches. |
| #884 | [gpu] Optimize NVN direct for GPUs that do not share memory with the CPU | hard | A large design or feature; it needs its own plan and several branches. |
| #882 | Final Fantasy Resonance | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #878 | todo: jemalloc as scarletnx's malloc on the systems past Linux and FreeBSD | blocked: macOS | No macOS device or toolchain on this machine. |
| #862 | [test] RFC: check the A64 emitters against an oracle that is not QEMU | hard | A large design or feature; it needs its own plan and several branches. |
| #854 | [ios] RFC: ship a game's code as a signed dylib, so iOS needs no JIT | blocked: iOS | No iOS device or toolchain on this machine. |
| #853 | [vk] RFC: a Metal renderer on Apple devices, beside the Vulkan renderer | blocked: iOS | No iOS device or toolchain on this machine. |
| #852 | [ios] Back guest memory with a file so iOS can page it out | blocked: iOS | No iOS device or toolchain on this machine. |
| #851 | [shader] Test the translator against nxgpucatch's hardware results | medium | Medium; not attempted in this pass. |
| #850 | [apple] Runtime threads have no QoS class | blocked: iOS | No iOS device or toolchain on this machine. |
| #849 | [ios] The runtime only logs the thermal state | blocked: iOS | No iOS device or toolchain on this machine. |
| #848 | [apple] Runtime threads have no autorelease pool | blocked: iOS | No iOS device or toolchain on this machine. |
| #836 | [vk] On MoltenVK the pipeline cache file makes the loading screen about 1 s slower, and its hit count is always 0 | blocked: macOS | No macOS device or toolchain on this machine. |
| #833 | [vk] A cutscene's first visit waits for 77 pipelines in one frame: 0.8 s on a Mac, about 9 s on an iPhone | medium | Medium; not attempted in this pass. |
| #832 | [vk] Descriptor pools on MoltenVK hold ~470 MiB of malloc in one game's first cutscene | blocked: macOS | No macOS device or toolchain on this machine. |
| #820 | [game] Super Mario Bros. Wonder: a GPU hang on macOS entering the opening cutscene | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #801 | [runtime] The dispatch table takes 203 MiB for one game, and the pack is read whole before it is loaded | medium | Medium; not attempted in this pass. |
| #800 | [ios] One game's first cutscene needs more memory than the iPhone gives the app | blocked: iOS | No iOS device or toolchain on this machine. |
| #795 | [runtime] Record a player's input as a --script, for test routes | medium | Medium; not attempted in this pass. |
| #793 | [gpu] One game's indirect draws ask for 555,554 instances on macOS: the frame fills with stretched triangles | blocked: macOS | No macOS device or toolchain on this machine. |
| #788 | [fs] A dump is mapped whole, so a 4 GB NSP doesn't fit on an iPhone | blocked: iOS | No iOS device or toolchain on this machine. |
| #787 | [ios] Stored paths keep the old app container after a reinstall | blocked: iOS | No iOS device or toolchain on this machine. |
| #780 | [vk] Spatial upscaling for the presented frame | done: feature branch | The spatial upscaler (FSR 1, NIS, SGSR, Bilinear) and its settings and proof are on `greatgreenstar/feat/hdr-780`. |
| #778 | [macos] No write tracking or traps on Apple hosts: new texels in a settled texture show up late | blocked: macOS | No macOS device or toolchain on this machine. |
| #777 | [vk] Without conditional rendering, each occlusion-tested draw waits for the GPU | medium | Medium; not attempted in this pass. |
| #776 | [vk] Own buffers on an integrated GPU are filled by a copy that ends the rendering pass | medium | Medium; not attempted in this pass. |
| #775 | [vk] Depth blits go through guest memory where D32F S8 stands in for D24S8 | medium | Medium; not attempted in this pass. |
| #770 | [game] The Legend of Zelda: Breath of the Wild | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #769 | [game] Fire Emblem Engage | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #759 | todo: nothing exclusive to one platform: the same UI, logs and files on every one | medium | Medium; not attempted in this pass. |
| #758 | todo: organize the launcher's UI: tabs, footer, setup, the game's box, icons, its log | hard | A large design or feature; it needs its own plan and several branches. |
| #743 | todo: bring back gui_sdl's compile_first, with a played game the test holds | easy | Small; not attempted in this pass. |
| #741 | [game] Uppercase letters arrive late, after the text has moved on | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #739 | [am] Follow ExecuteProgram to the program it names | medium | Medium; not attempted in this pass. |
| #737 | [ios] An in-app CPU sampler for where the threads spend their time | blocked: iOS | No iOS device or toolchain on this machine. |
| #736 | [runtime] The stats corner could show GPU memory and the GPU thread's wait | easy | Small; not attempted in this pass. |
| #735 | [vk] A report of long GPU waits and what holds them, for slow frames | medium | Medium; not attempted in this pass. |
| #734 | [vk] A Metal capture of a chosen frame on Apple devices | blocked: macOS | No macOS device or toolchain on this machine. |
| #731 | [vk] A setting to load textures from a smaller mip level, for low-memory devices | medium | Medium; not attempted in this pass. |
| #722 | [vk] shadows=0 skips depth prepasses too: one game's floors flicker or vanish | blocked: iOS | No iOS device or toolchain on this machine. |
| #720 | [vk] Occlusion queries split each rendering pass into extra Metal encoders on MoltenVK | blocked: macOS | No macOS device or toolchain on this machine. |
| #719 | [macos, ios] Transform feedback isn't captured on MoltenVK: skinned models explode | blocked: iOS | No iOS device or toolchain on this machine. |
| #713 | [gpu] A 2D blit from memory larger than the allocation cap is skipped | easy | Small; not attempted in this pass. |
| #711 | [vk] vk_gpu_profile makes MoltenVK about 10 times slower, and its timestamps are unreliable there | blocked: macOS | No macOS device or toolchain on this machine. |
| #710 | [gpu] Draws over bytes the GPU holds end the rendering pass and copy them through staging each time | hard | A large design or feature; it needs its own plan and several branches. |
| #699 | [runtime] The host's limits, measured or set, on every system | hard | A large design or feature; it needs its own plan and several branches. |
| #698 | [3ds] Dual Screen separate windows and render res control option | done: feature branch | The 3DS separate windows and the per-screen render resolution are on `greatgreenstar/feat/hdr-780`. |
| #692 | [settings] make settings.txt generator also generate cmdline args (with - instead of _) | medium | Medium; not attempted in this pass. |
| #672 | [ci] Welcome first-time contributors | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #671 | [ci] Track performance across nightly builds | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #669 | [ci] Check formatting and the SPDX header on new files | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #668 | [ci] Close game reports that stay without a log | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #666 | [ci] Add CodeQL for C | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #663 | [game] FF Resonance loses the GPU device 16 s after start (Adreno 735, Android) | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #662 | [game] Paper Mario: The Thousand-Year Door loses the GPU device after about 6 minutes (RADV) | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #657 | [ci] git.scarletnx.dev: a Forgejo copy of the GitHub repositories, issues and pull requests included | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #654 | Showcase Overlay does not change rendered resolution when display resolution varies | easy | Small; not attempted in this pass. |
| #653 | [ci] Move build.yml's repeated steps into composite actions | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #651 | [game] Echos of Wisdom has incorrect color values upon opening | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #644 | [ui] Update ScarletNX from inside the launcher, on a nightly or stable channel | hard | A large design or feature; it needs its own plan and several branches. |
| #639 | [ci] Run the linux job's three trees in parallel, and skip identical deploy copies | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #637 | [ci] Add a ThreadSanitizer job | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #636 | [kernel] Full handle table and thread pool answer success or abort | medium | Medium; not attempted in this pass. |
| #635 | [kernel] Guest pointers are checked against the address space, not against mapped pages | hard | A large design or feature; it needs its own plan and several branches. |
| #633 | [gpu] A draw with a storage image copies the whole surface through the CPU | medium | Medium; not attempted in this pass. |
| #631 | [recompiler] Emitter loose ends: V-register tags, DMB options, invariant CPU fields, flag stores | medium | Medium; not attempted in this pass. |
| #630 | [recompiler] Runtime JIT compiles at O0 on workers below the thread waiting for them | easy | Small; not attempted in this pass. |
| #622 | [ai] Harness to it adds `assert(x)` everywhere instead of logspam | medium | Medium; not attempted in this pass. |
| #604 | [driver] Read NSZ and XCZ dumps | hard | A large design or feature; it needs its own plan and several branches. |
| #598 | [ios] Attach the app's own debugger so JIT needs no StikDebug | blocked: iOS | No iOS device or toolchain on this machine. |
| #590 | [ux] Recompiling games with updates doesn't recompile both base and update | blocked: needs info | The report asks for a log, a build or a reproduction that is not here. |
| #589 | [gui] Finish the UI generator: the screens' frame and what is left | hard | A large design or feature; it needs its own plan and several branches. |
| #585 | [android] Aspect ratio not 16:9 while run without on-screen controller or showcase overlay | blocked: Android | No Android device or toolchain on this machine. |
| #575 | [game] The Legend of Zelda: Breath of the Wild 1.9.0 stays on a black screen while the GPU draws, on Windows | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #565 | [graphics] Metroid Prime 4 Beyond All assets are black you can't see anything | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #564 | Fold pipeline.c's and sdl3.c's loose statics into one state struct each (RFC #142 rule 6) | medium | Medium; not attempted in this pass. |
| #560 | [game] Super Mario Party Jamboree 2.3.0 crashes on a null pointer in recompiled code (83298f0), also with memory_gb 6 and 8, on Windows | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #558 | [game] Super Mario Odyssey 1.4.1 stops with "Pure virtual function called" and svcBreak 2 seconds in, on Windows | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #555 | [game] Pokemon Scarlet version; Black Screen in-game. | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #554 | [game] Hollow Knight: Silksong 1.0.30000 stops with svcBreak in the SDK 6 seconds after it starts, on Windows | blocked: needs info | The report asks for a log, a build or a reproduction that is not here. |
| #549 | [docs] Report coverage numbers generated from the repo | easy | Small; not attempted in this pass. |
| #544 | [runtime] After a crash on a runtime thread the window freezes with no error panel and the process stays alive | medium | Medium; not attempted in this pass. |
| #535 | [runtime] Extract to tmpfs fills a disk when /tmp is not a tmpfs | easy | Small; not attempted in this pass. |
| #533 | [game] Metroid Prime 4 1.1.0 Freezing PC | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #532 | [macos] Title shows only its background: no logo, characters or prompt | blocked: macOS | No macOS device or toolchain on this machine. |
| #525 | [ux] Remove installed DLC/Updates | medium | Medium; not attempted in this pass. |
| #520 | [ux] drag and drop multiple DLCs | easy | Small; not attempted in this pass. |
| #519 | [gpu] A depth prepass followed by an EQUAL color pass draws cap meshes over faces on Intel HD 520 | hard | A large design or feature; it needs its own plan and several branches. |
| #516 | [gui] Folders: clearer names and layout; the right box shown or hidden | medium | Medium; not attempted in this pass. |
| #511 | [game] Tears of the Kingdom 1.0.0 crashed when picking the master sword | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #504 | Splatoon 2 freezes up when entering octo canyon. | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #503 | Tomodachi Life: Living the Dream crashes mid-game | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #500 | [Flatpak, CI] Issue tracking for integrating a nightly flatpak to the workflow | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #493 | [driver] Playing a base dump over a game folder made from other content stops with "recompile the game" | easy | Small; not attempted in this pass. |
| #477 | [settings] source= on .ini doesn't properly work on android | blocked: needs info | The report asks for a log, a build or a reproduction that is not here. |
| #474 | [ui] audio slider to make audio volume more convenient | easy | Small; not attempted in this pass. |
| #471 | [crash] Super Smash Bros. Ultimate crashs on boot | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #469 | [meta] Game reports from before the report form (#434), to retest | medium | Medium; not attempted in this pass. |
| #467 | [graphics] The visuals are majorly broken | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #453 | [shader] F2I with the condition code bit is refused, dropping a compute program | easy | Small; not attempted in this pass. |
| #442 | [game]  Minecraft's main graphics are squashed into the left side of the screen | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #439 | [graphics] There's a bit of some graphic flickering and what seems like the colors might be off? | blocked: game data | Needs the game's dump and a run on this machine's GPU; the report's game is not here. |
| #435 | [core] Drop usage of `snx_strdup`, `snx_alloc` and `snx_realloc` | medium | Medium; not attempted in this pass. |
| #421 | [macos] liquid glass icon | blocked: macOS | No macOS device or toolchain on this machine. |
| #409 | [ci] WoA builds | blocked: CI | Needs the repository's CI and hosting; nothing here runs it. |
| #406 | [cheats] the launcher's and the game menu's side of #365 | medium | Medium; not attempted in this pass. |
| #405 | RFC: A room server of ScarletNX's own, compatible with Eden's, or Eden's eden-room | medium | Medium; not attempted in this pass. |
| #389 | Smaller findings: stale CLAUDE.md, formatting drift, committed test output, misleading messages | easy | Small; not attempted in this pass. |
| #388 | Report timestamps measure the GPU thread, not GPU work: dynamic resolution drops | medium | Medium; not attempted in this pass. |
| #386 | Buffer aliasing by guest page forces a vk_finish per draw for packed buffers | medium | Medium; not attempted in this pass. |
| #368 | [optimize] Metroid Prime 4 gigantic ASTC textures shall use sparse memory | medium | Medium; not attempted in this pass. |
| #349 | [test] driver_workflow: removing the test's repo fails now and then on Windows | easy | Small; not attempted in this pass. |
| #328 | [macos, ios] No game runs yet: 4 KiB guest pages on 16 KiB hosts, and no Mach-O modules | hard | A large design or feature; it needs its own plan and several branches. |
| #312 | todo: Wimsy from a formula, its variants made on the fly | hard | A large design or feature; it needs its own plan and several branches. |
| #274 | [shader] Track renderer work deferred from #244 | hard | A large design or feature; it needs its own plan and several branches. |
| #215 | [build] a version number for ScarletNX releases | medium | Medium; not attempted in this pass. |
| #201 | [meta] Human maintainable | hard | A large design or feature; it needs its own plan and several branches. |
| #184 | todo: rtlib should be embedded as bitcode | hard | A large design or feature; it needs its own plan and several branches. |
| #182 | todo: Make appimages and APK smaller | hard | A large design or feature; it needs its own plan and several branches. |
| #178 | todo: Platforms to implement emulators for | hard | A large design or feature; it needs its own plan and several branches. |
| #154 | todo: Full ARMv8-A compliance check for A32, T{16,32} | hard | A large design or feature; it needs its own plan and several branches. |
| #142 | RFC: Code quality | easy | Small; not attempted in this pass. |
| #124 | todo: port the rest of rEden's tests | medium | Medium; not attempted in this pass. |
| #112 | todo: Zero-copy UMA | hard | A large design or feature; it needs its own plan and several branches. |

## Blocked or not for this device

The rows whose rank starts with `blocked` are the ones this machine cannot settle: a phone, a Mac, a Windows
host, the repository's CI, a game that is not here, or a report that still needs its log. Each row names the
thing it waits for. A game report also needs the same game and driver, so a fix made blind is not one.

## Main feature branch

The HDR, upscaling, anti aliasing, frame interpolation, shadow-map and `docked_key` work is on
`greatgreenstar/feat/hdr-780`, based on PR #1104. Its checklist is `docs/REMAINING.md`, its defects are in
`docs/issues/`, and its runnable build is in
`/home/greenstar/Downloads/compiled_scarlet/Hotkey_stuff_forALL/`. The 34 local commits are also in `patches/`
there. Nothing was pushed.

## Counts

- Fixed in this pass, on its own branch: 5.
- Done on the feature branch: 5 (the rows marked `done: feature branch`).
- Open and ranked: 151.
  - blocked: 3DS game: 2
  - blocked: Android: 4
  - blocked: CI: 12
  - blocked: game data: 26
  - blocked: iOS: 14
  - blocked: macOS: 12
  - blocked: needs info: 4
  - easy: 18
  - hard: 28
  - medium: 36
