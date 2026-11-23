<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVIDIA DLSS

On Windows with an NVIDIA GeForce RTX GPU, a Switch 2 game's DLSS evaluations (and any other upscale the
[upscalers](../layers/gpu/upscalers.md) run) can run NVIDIA DLSS Super Resolution, the upscaler the game was made for.
Elsewhere, or without the files below, they run AMD FSR 2.2 as before.

ScarletNX does not include DLSS. It is NVIDIA's software under NVIDIA's licences, which you download from NVIDIA and
keep in a folder of your own. Only NVIDIA Streamline's headers, which are MIT, are built into the program.

## Getting the files

1. Download `streamline-sdk-v2.14.16.zip` from NVIDIA's Streamline releases,
   <https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16>. Read its `license.txt` (Streamline, MIT) and
   `bin/x64/nvngx_dlss.license.txt` (DLSS, the NVIDIA RTX SDKs licence) first: using the files means taking them.
2. Copy these four files from its `bin/x64` folder into one folder, for example `D:\dlss`:
   `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` and `nvngx_dlss.dll`. The `development` builds are not needed.
3. Set the game's `dlss_runtime` to that folder (in the game's `.ini`, or `--dlss_runtime=D:\dlss` on the command line).

Nothing is installed: the program loads the files from that folder when the game starts, after checking that
`sl.interposer.dll` is signed by NVIDIA. Streamline writes its log in a `data` folder inside it.

## Settings

| Key | Default | What it does |
| --- | --- | --- |
| `dlss_runtime` | empty | The folder above. Empty = no DLSS |
| `render_upscaler` | `0` | `0` (Best) takes DLSS when it can run, `4` asks for it. `1` asks for FSR 2 even with the files there, `3` (None) a filtered copy |
| `dlss_preset` | `2` | The DLSS model: `0` NVIDIA's choice for each mode, `1` to `4` the presets J, K, L, M. K by default: at 2x it takes less memory than NVIDIA's choice there (M), 158 against 214 MB at 2560x1440 |
| `internal_resolution` | `0` | The size the game renders at before DLSS: `0` the game's choice, `1` the output's size, `2` to `5` Quality to Ultra Performance |

The mode follows the game: its render size against the output's picks DLAA, Quality, Balanced, Performance or Ultra
Performance.

## What the log says

- `vulkan: DLSS: Streamline 2.14.16, DLSS 310.9.16 started from ...`: the files were found and Streamline started.
- `vulkan: DLSS: ... runs on this device`: the GPU and driver take it.
- `vulkan: DLSS upscales up to ...`, then `vulkan: DLSS: Performance mode for 1280x720 to 2560x1440 ...` and
  `vulkan: DLSS: N evaluations recorded, 0 failed` (the first three, then every 600th).
- `vulkan: no DLSS: ...` says why it can't run (no folder, a file missing or not signed, not an NVIDIA GPU, a Vulkan
  extension the driver lacks), and FSR 2 runs instead. A failure during the game turns DLSS off until the next start,
  and FSR 2 takes over from the next evaluation.
