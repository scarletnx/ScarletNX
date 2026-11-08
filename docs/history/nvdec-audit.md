<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVDEC H.264 packet submission audit

Green video can result from damaged decoding or unwritten/stale YUV surfaces. This
change fixes concrete submission defects in `src/nx/gpu/video/nvdec.c`; without a
failing game's capture it does not establish which defect caused that game's symptom.

## Findings and changes

- The old reusable allocation reserved only eight extra bytes and did not zero the
  packet tail. FFmpeg requires `AV_INPUT_BUFFER_PADDING_SIZE` accessible, zeroed
  bytes beyond `AVPacket.size`. Smaller jobs could expose old payload bytes to the
  bit reader. Use `av_new_packet` and `av_shrink_packet`, giving each packet owned,
  reference-counted storage and zero padding at its actual end.
- `avcodec_send_packet` returning `EAGAIN` means the packet was **not consumed**.
  Previously it was dropped after draining output. Drain and retry the same packet
  once; fail rather than spin if the API still refuses it. Frames captured during
  that drain must not be attributed to the current surface, including second fields.
- Receive errors were discarded. Now only receive-side `EAGAIN` ends a successful
  drain; decode errors and unexpected EOF fail the job. Do not send NULL packets or
  flush between jobs: subsequent P/B pictures need the decoder's reference history.
- SPS/PPS bytes were cached before the decoder accepted the job. Commit the cache
  after successful submission/draining; invalidate it after any failed job so the
  next job resends the parameter sets.
- A `get_buffer2` callback did not mean the job succeeded. Previously rejected
  packets could still publish their captured buffer. Never publish after a send or
  receive error, clear failed-job captures, propagate frame-reference allocation
  failure, and report failed status when no output surface was written.
- Four-byte AVCC lengths of 256..511 also begin with `00 00 01`. Check a complete,
  valid length-prefixed sequence before the Annex B heuristic. Preserve Annex B
  leading zeros, validate bare NAL headers, and reject invalid/truncated prefixes
  instead of blindly adding a start code. One-/two-byte AVCC lengths are not added.

These rules follow FFmpeg's [send/receive API contract](https://ffmpeg.org/doxygen/9.0/group__lavc__encdec.html)
and [packet API](https://ffmpeg.org/doxygen/9.0/group__lavc__packet.html).

## Tests

`nvdec_packets` uses the production implementation with a small, isolated FFmpeg
API double. It covers packet bytes/padding, shorter consecutive packets, ambiguous
lengths, malformed inputs, allocation failures, backpressure, bounded retry, receive
errors/EOF, header-cache recovery, stale callbacks/fields and surface failure status.
The double's headers are private to that target, never used by the runtime.

```sh
cmake -S . -B build/packet-tests -DSNX_TOOLS_ONLY=ON -DSNX_TEST_SANITIZERS=ON
cmake --build build/packet-tests --target snx-test-nvdec-packets
ctest --test-dir build/packet-tests -R '^nvdec_packets$' --output-on-failure
```

`nvdec_decode` builds against real FFmpeg when `SNX_TOOLS_ONLY=OFF`.
It decodes synthetic 16x16 I_PCM and reference-dependent
P-skip pictures, checks exact luma/interleaved chroma values, reuses SPS/PPS and
exercises Annex B, four-byte AVCC and bare-NAL submission. No copyrighted video or
encoder dependency is required.

```sh
cmake --build build --target snx-test-nvdec-decode
ctest --test-dir build -R '^nvdec_(packets|decode)$' --output-on-failure
```

Validation here: 16 tools-only CTests passed with Clang 21.1.5 and ASan/UBSan;
the real-decoder harness passed 2,290 checks against the pinned FFmpeg 9.0.2.
The locally built FFmpeg library itself was not sanitizer-instrumented. A `-no-pie`
executable link flag was needed for ASan's address-space reservation in the sandbox.

`nvdec_vp9_*` (one test a vector, with `SNX_TOOLS_ONLY=OFF` as `nvdec_decode`) feeds `nvdec_execute` the VP9 jobs of
16 of libvpx's test vectors (BSD-3-Clause, downloaded at configure time into `CPM_SOURCE_CACHE`, pinned by SHA-256
in `tests/CMakeLists.txt`) and checks every picture on its surface right after its own job, a hidden one too,
against the MD5 libvpx lists for the image (the reference decoder's picture for a hidden frame). FFmpeg's decoder,
as released, works each job out of the vector (`tests/nvdec_vp9_jobs.c`): the picture setup from its parsed headers
and the probability table from its state after the frame, which a game's player works out itself before it hands
NVDEC the job. That part reads FFmpeg's private `VP9Context`, so it builds against the patched source and the
`config.h` `cmake/ffmpeg.cmake` keeps with the install, and follows FFmpeg's struct when the pin moves. The vectors
cover hidden frames, contexts that adapt, frame parallel mode, superframes, frames shown again, intra-only frames,
spatial layers, tiles, size changes and segmentation; taking any of the three hunks out of
`cmake/ffmpeg-vp9-nvdec.patch` fails several of them.

```sh
ctest --test-dir build -R '^nvdec_vp9_' --output-on-failure
```

## Limits

The tests substitute linear surface addressing; they verify publication and NV12
interleaving, not actual GOB layout, VIC conversion or GPU presentation. Actual
interlaced/B-frame game streams, alternate SPS/PPS IDs and unsupported chroma formats
still need capture-based coverage. This patch does not change SPS/PPS reconstruction,
the decode-order `get_buffer2` strategy, or recover reference pictures already lost
to malformed input. Framing autodetection remains a heuristic without explicit
container metadata. A game-level green-screen fix still needs in-game verification.
