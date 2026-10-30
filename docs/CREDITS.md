<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Licenses and credits

ScarletNX is [GPL-3.0-or-later](../LICENSE).
Generated modules include runtime ABI code. Game assets, keys and firmware are excluded.

| Component | License |
| --- | --- |
| FFmpeg | LGPL-2.1-or-later |
| libvpx's VP9 test vectors (the WebM project's test data, downloaded for the `nvdec_vp9_*` tests, not shipped) | BSD-3-Clause |
| jsmolka's gba-tests (test ROMs, downloaded for the `agb_gba_*` tests, not shipped) | MIT |
| hactool / cJSON / ConvertUTF (`src/nx/runtime/driver/hactool`, with ScarletNX's changes) | ISC / MIT / Unicode license |
| LibHac storage code, ported into hactool (`src/nx/runtime/driver/hactool/storage.c`) | MIT |
| OpenSSL | Apache 2.0 |
| jemalloc (the `scarletnx` program's malloc, with `SNX_JEMALLOC`) | BSD-2-Clause |
| Noto Sans JP, SC, TC and KR Subsets (the shared fonts' stand-ins, fetched by `cmake/dependencies.cmake`, made by `tools/subset_fonts.py` from Google Fonts commit `e7db6ca603fd4862010383ff17ffd6cc4ffac1c0`) | SIL OFL 1.1 ([text](../dist/licenses/Noto/OFL.txt)) |
| yuzu's controller button glyphs (`font_nintendo_extended.cpp`, as Eden carries it) | GPL-2.0-or-later |
| The 3DS system archives' open stand-ins (`cmake/ctr_system_archives.cmake`): the Mii data, country list, shared font and bad word list B3n30/citra_system_archives makes (Weiyi Wang's generators), as Azahar carries them | GPL-2.0-or-later |
| Montserrat and GNU Unifont 10.0.05, the glyphs of the 3DS shared font's stand-in | OFL-1.1 ([text](../dist/licenses/Montserrat/OFL.txt)) / GPL-2.0-or-later with the GNU font embedding exception |
| LZ4 (the library, `lib/`) | BSD-2-Clause |
| xxHash 0.8.3 (XXH3 and its x86 dispatcher, the renderer's content hashes: `src/nx/gpu/vk/xxh3.c`) | BSD-2-Clause |
| Vulkan-Headers | Apache 2.0 |
| volk / Vulkan Memory Allocator | MIT |
| SDL3 | zlib |
| The AppImage's runtime (uruntime) and launcher (sharun, with Anylinux-sharun's `anylinux.so` and cross-libc-dlopen) | MIT |
| Tracy (the profiler client, built only with `SNX_TRACY`) | BSD-3-Clause |
| ENet / zstd (an Eden room's connection and its datagrams, `src/common/network/net_room.c`) | MIT / BSD-3-Clause |
| miniz 3.1.2 (the zip of the launcher's Share Log, `src/gui/logzip.c`) | MIT |
| lunasvg / plutovg (the splash's SVGs and the user's picture) | MIT / MIT, with FreeType's rasterizer and stroker (FreeType License) and stb (MIT or public domain) |
| GUI menu icons (`src/gui/icons.hex`), drawn after Lucide 1.61.0's (some derived from Feather) | GPL-3.0-or-later, after Lucide's ISC and Feather's MIT (in `src/gui/FONT-NOTICE.txt`) |
| GUI VGA BIOS font (SeaBIOS `vgafont16`, originally Joseph Gil's font collection) | Public domain |
| GUI Latin letters past CP437 (GNU Unifont 18.0.01's 8x16 glyphs, `src/gui/unifont-latin.hex`, fitted to the VGA font) | GPL-2.0-or-later with the GNU font embedding exception, or OFL-1.1 |
| GUI wide letters the languages show, as Chinese (GNU Unifont 18.0.01's 16x16 glyphs, `src/gui/unifont-wide.hex`) | GPL-2.0-or-later with the GNU font embedding exception, or OFL-1.1 |

The GUI fonts' notices and pinned sources are preserved in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).

Code ported into the repository, and assets taken from other projects. In these places, the files whose header names
a source keep that source's copyright lines and license, most with the revision they came from; the drawables of
`dist/controls/` have theirs in [its README](../dist/controls/README.md):

| Where | From | License |
| --- | --- | --- |
| `src/nx/gpu/gpu.c` (safe reads of compute-inline payloads) and `src/nx/gpu/shader/spirv_translate.c` (constant geometry invocation information) | Citron, Zephyron's Princess Peach: Showtime! fixes in [`acfb1dcc1ad9adf25b960c4f8f25c5bf78849406`](https://github.com/citron-neo/emulator/commit/acfb1dcc1ad9adf25b960c4f8f25c5bf78849406), `video_core/dma_pusher.cpp` and `shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp`; original code copyright 2018 and 2021 yuzu Emulator Project | GPL-2.0-or-later |
| `src/nx/runtime/driver/hactool/storage.c` and `storage.h` (the storages added to hactool) | LibHac, as Ryubing maintains it | MIT |
| `src/3ds/gpu/`, `src/3ds/audio/`, `src/3ds/services/dsp.c`, `src/3ds/services/cfg.c` (the config blocks' defaults), `src/3ds/services/hid.c` (HID's samples, their layout and pace) | Azahar (Citra) | GPL-2.0-or-later |
| `src/vita/hle/gxm.c` (sceGxm's state and its calls' semantics), `src/vita/gpu/usse.c` and `usse_constants.inc` (the USSE shader translator), `src/vita/gpu/gxp.h` (the GXP program layout), `src/vita/gpu/texture.h` (SceGxmTexture's words), `src/vita/gpu/swizzle.glsl` and `tests/glsl/vita_swizzle_glsl.cpp` (the swizzled and tiled texture layouts), `texture_levels` in `src/vita/gpu/render.c` (where a texture's mipmaps lie), `src/vita/hle/audiodec.c` and `videodec.c` (the decoders' structures and their ATRAC9 configuration word), `src/vita/hle/trophy.c` (the trophy calls' error codes and unlock rules) | Vita3K (`modules/SceGxm/`, `shader/`, `gxm/`, `renderer/src/texture/format.cpp`, `modules/SceAudiodec/`, `modules/SceVideodec/`, `modules/SceNpTrophy/SceNpTrophy.cpp`) | GPL-2.0-or-later |
| `src/vita/hle/ngs.c` (SceNgs's voices, players, patches and master buss, the voice states and the parameter blocks) | Vita3K (`modules/SceNgsUser/SceNgs.cpp`, `ngs/`) | GPL-2.0-or-later |
| `hevag_decode` and `hevag_coefficients` in `src/vita/hle/ngs.c` (HE-ADPCM) | Vita3K (`codec/src/pcm.cpp`), after vgmstream's HEVAG decoder | GPL-2.0-or-later, vgmstream's ISC-style license |
| `src/vita/hle/` (the NIDs and the error codes) | vita-headers (`db/`, `include/psp2/`) | MIT |
| `src/3ds/gpu/shader.c` and `shader.h` (the instruction fields) | nihstro | BSD-3-Clause |
| `src/common/cpu/` (the recompiler's instruction semantics: `recompiler/llvm/ir_int.c`, `ir_ldst.c`, `ir_fpu.c`, `ir_vec.c`, `ir_vec_fp.c` and `ir_vec_crypto.c`, the integer, branch, system, load/store, scalar floating-point and AdvSIMD emitters ported from `translate-a64.c`'s `trans_*` functions, `gengvec.c`, `vec_helper.c` and `translate.c`'s `asimd_imm_const`, and the A32/T32 block compiler) | QEMU's `target/arm/tcg/` | LGPL-2.1-or-later |
| `src/common/cpu/aarch64/decode/a64.decode` (the A64 encodings, unmodified, from commit `a19f59a`, which `tools/a64dec` generates the decoder from; [README](../src/common/cpu/aarch64/decode/README.md)) | QEMU's `target/arm/tcg/a64.decode` | LGPL-2.1-or-later |
| `src/common/cpu/aarch64/` (`cpu.h`, `cpu_helpers.c` and `helpers/`, the generated code's FP, SIMD, integer and crypto helpers) | QEMU's `target/arm/` and its softfloat, with the SoftFloat-2a notice | LGPL-2.1-or-later, SoftFloat-2a, BSD |
| `src/common/cpu/aarch64/helpers/vector.c` (the SM4 S-box, GB/T 32907-2016's table) | OpenSSL's `crypto/sm4/sm4.c` | Apache 2.0 |
| `src/nx/gpu/gpu.c`, `gpu.h`, `pushbuffer.h` and `src/nx/gpu/maxwell/` (the DMA pusher and puller, the 3D, macro, DMA, 2D, inline-to-memory and compute engines, and their register layouts in the `*_regs.h` headers), with the DMA and 2D engines' CPU references in `tests/support/dma_ref.c` and `fermi2d_ref.c` | Eden and yuzu (`video_core/dma_pusher.cpp`, `engines/`: `puller.cpp`, `engine_upload.cpp`, `maxwell_3d.cpp`, `draw_manager.cpp`, `maxwell_dma.cpp`, `fermi_2d.cpp`, `kepler_compute.cpp`, `kepler_memory.cpp`, `sw_blitter/`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/texture/blocklinear.c`, `blocklinear.h` and `src/nx/gpu/maxwell/gob.h` (the GOB and block geometry) | Eden and yuzu (`video_core/textures/decoders.cpp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/vk/compute.c` (`res_bind`, `res_publish`: compute storage buffers bound where the GPU holds them and marked as the GPU's when written) and `src/nx/gpu/vk/buffer.c` (`buf_get_owned`: draws read the GPU's bytes on the GPU) | Citron Neo's buffer cache (`video_core/buffer_cache/buffer_cache.h`: `BindHostComputeStorageBuffers`, `MarkWrittenBuffer`, `SynchronizeBuffer`), the model only, on ScarletNX's own GPU ownership | GPL-3.0-or-later |
| `src/nx/gpu/shader/` (`shader.c`, `decode.c`, and `maxwell_opcodes.def`, the encodings `tools/shaderdec` generates the decoder from) | Eden and yuzu (`shader_recompiler/frontend/maxwell/`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/` (`host1x.c`, `nvdec.c`, `vic.c` and their register layouts in `host1x_regs.h`, `nvdec_regs.h` and `vic_regs.h`) and `tests/support/vic_ref.c` | Eden and yuzu (`video_core/host1x/`: `control.cpp`, `nvdec.cpp`, `vic.cpp`, `codecs/h264.cpp`, `codecs/decoder.cpp`, `ffmpeg.cpp`; `video_core/cdma_pusher.cpp`; the nvdrv host1x devices), with Skyline's host1x control (GPL-3.0-or-later) and Ryujinx's CDMA pusher as Eden carries them | GPL-3.0-or-later, GPL-2.0-or-later, MIT |
| `src/nx/loader/loader.c` (the defaults for a program without an NPDM) | Eden and yuzu (`file_sys/program_metadata.cpp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/common/utilities/patches.c` (IPS, IPS32 and IPSwitch patches: their formats, which patch a module takes, where they write) | Eden (`file_sys/ips_layer.cpp`, its IPSwitch reading by lizzie, and `file_sys/patch_manager.cpp`) | GPL-3.0-or-later |
| `src/nx/hle/kernel/`, `src/nx/hle/svc/`, `src/nx/memory/`, `src/nx/hle/services/`, `src/nx/hle/filesystem/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/services/nvdrv.c`, `nvdrv.h` and the `nvdrv_*.h` headers (the nvhost devices' commands and argument layouts, the ioctl encoding, the GMMU and VA layout, the syncpoint manager) | Eden and yuzu (`core/hle/service/nvdrv/`: `nvdata.h`, `core/syncpoint_manager.h`, `devices/nvmap`, `nvhost_as_gpu`, `nvhost_ctrl`, `nvhost_ctrl_gpu`, `nvhost_gpu`; `video_core/memory_manager.h`), with Skyline's parts as Eden carries them | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/svc/hos.svc` (the SVC declarations and type sizes) and `tools/svcgen/svcgen.c` (their parsing and register allocation) | Eden and yuzu (`tools/svc_generator.py`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/services/misc.c` (the six default Miis, from Eden's `mii/types/raw_data.cpp`, and StoreData's checksums, from Ryubing's `Mii/Helper.cs` and `Mii/Types/StoreData.cs`) | Ryujinx (the defaults as Eden carries them) | MIT ([text](../dist/licenses/Ryujinx/LICENSE.txt)) |
| `src/common/network/`, `src/nx/hle/services/network/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/common/network/net_room.c` (an Eden room's protocol) | Eden, yuzu and Citra (`src/network/room.cpp`, `room_member.cpp`, `packet.cpp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/vp9_rebuild.cpp`, `src/nx/gpu/video/vic.comp` and `vic_compose.glsl` (compiled into SPIR-V at build time) | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/texture/astc.comp` (the ASTC decoder shader, compiled into `astc_spv.h`) | Eden and yuzu (`astc_decoder.comp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `tests/support/astc.c` and `astc.h` (the CPU ASTC decoder the tests check the shader against) | Eden and yuzu (`textures/astc.cpp`), with the University of North Carolina at Chapel Hill's FasTC decoder in it | GPL-3.0-or-later, GPL-2.0-or-later, Apache 2.0 |
| `tests/ipc_message.c` (the HIPC, CMIF and TIPC request builders) | libnx | ISC |
| `src/gui/i18n.c` (`snx_mo_lookup`, the reader of the menu's catalogs; notice in [`src/gui/MUSL-NOTICE.txt`](../src/gui/MUSL-NOTICE.txt)) | musl (`src/locale/__mo_lookup.c`) | MIT |
| `dist/controls/*.xml`, and `controls.png` made from them | yuzu's Android overlay, as Eden and rEden carry it | GPL-3.0-or-later |

Dynarmic (0BSD) and its GPL-3.0-or-later fork were consulted for the recompiler's instruction semantics. No code was
ported from them.

[Nexium](https://nexium-emu.org/) (MIT) is credited for its research and development.

[Anylinux-AppImages](https://github.com/pkgforge-dev/Anylinux-AppImages) (Package Forge, MIT) makes the Linux
AppImages with its quick-sharun, its Arch images and its debloated packages, and is the reference for how they are
packaged ([FAQ](https://github.com/pkgforge-dev/Anylinux-AppImages/blob/main/FAQ.md)).

Installed license texts: `share/licenses/scarletnx/`. AppImage system-library licenses
and package manifest: `share/licenses/`. Component license files contain the full terms.

Community credits: [README](../README.md#special-thanks).

Wimsy ([SVG metadata](../dist/wimsy.svg)): drawn by Liz, GPL-3.0-or-later.
The splash's Fuddles ([SVG metadata](../dist/splash/wimsy-fuddles.svg)): Wimsy with Fuddles' tails, circuits and
colours, by AO55 after Liz's Wimsy, GPL-3.0-or-later.

The 3DS UDS command layouts and encrypted beacon framing in `src/3ds/services/uds.c`
are adapted from Azahar's `nwm_uds.cpp` and `uds_beacon.cpp` at
`18c7ff10604659edc8e82d86b7c772f6f709c466` (Citra/Azahar contributors, GPL-2.0-or-later).
The ScarletNX-to-ScarletNX room transport is separate from Azahar's room protocol.
