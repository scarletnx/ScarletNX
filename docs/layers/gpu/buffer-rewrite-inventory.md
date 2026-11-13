<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Regression inventory for the buffer cache rewrite

This list belongs to step 1 of issue #1714. The rewrite replaces the buffer cache, the staging rings and the texture
trim. The comments of the files below record what earlier fixes repaired. Each entry here is one behavior from those
comments that the new code must keep.

## Index

* [How to use this list](#how-to-use-this-list)
* [The cache and its keys](#the-cache-and-its-keys)
* [Change detection](#change-detection)
* [Results that the GPU writes](#results-that-the-gpu-writes)
* [Waits for the GPU](#waits-for-the-gpu)
* [Staging and rings](#staging-and-rings)
* [Memory limits and the trim](#memory-limits-and-the-trim)
* [Mappings and locks](#mappings-and-locks)
* [Comments that this list leaves out](#comments-that-this-list-leaves-out)
* [State of the rewrite](#state-of-the-rewrite)

## How to use this list

The entries come from the comments of `buffer.c`, `cmd.c`, `surface.c`, `owned.c`, `mirror.c`, `compute.c`,
`writeback.c` and `writelog.c` in `src/nx/gpu/vk/`. They agree with commit f5c2fa84d and the working tree of
2026-11-11. `buffer.c` and `cmd.c` changed while this list was written, so a symbol can have a new name by now.

Each entry has four parts:

- The rule, in bold.
- What went wrong without the rule. The numbers are the ones that the comment gives.
- The comment that records it, by file and symbol.
- How to check the rule after the rewrite.

Most of the tests are checks of the ctest `vulkan_headless`, which a build with `-DSNX_TEST_VULKAN=ON` makes
([development](../../dev/development.md)). An entry names the check function and its file in `tests/`. Where an entry
says that no test covers the rule, the check is a game run with the counter or the profile that the entry names. A
rule about speed with no counter needs a profile of the GPU thread before and after the change.

The comments say "one game" and this list keeps that form. [GPU-owned guest memory](ownership.md) explains the
owners, the traps and the landings that many entries mention.

## The cache and its keys

**K1. The index of the cache is open addressed, and the entries are in one array.**

- Without the rule: entries live twenty seconds after their last use, and draws make new ranges all the time. Chained
  buckets of separately allocated entries grew long. The walk cost a cache miss for each entry, a twentieth of the GPU
  thread in one game. A lookup now reads one slot of the index, rarely two.
- Comment: `buffer.c`, above `g_vb`.
- Check: no test covers it. Compare `g_vb_probes` with `g_vb_lookups` in a scene with many draws.

**K2. The hash of a range mixes every bit of its start and its length.**

- Without the rule: the comment gives the reason and no failure. Constant buffers are 256-byte aligned and buffers
  are often 4K aligned, so the low bits of the start carry nothing.
- Comment: `buffer.c`, `vb_home`.
- Check: no test covers it. The probe count of K1 shows a weak hash.

**K3. A caller that knows its ranges first fetches every index slot, then every entry.**

- Without the rule: a draw looked its buffers up one after another. Each lookup waited on two cache misses in a row,
  88% of the own time of `buf_get` in one game.
- Comment: `buffer.c`, `vk_buf_prefetch`.
- Check: no test covers it. Profile `buf_get` in a scene with many draws.

**K4. An entry with no buffer of its own goes after two seconds, and one with a buffer after twenty seconds.**

- Without the rule: ranges that are read once made most of the entries. Constants streamed through a ring and
  vertices at new offsets are such ranges. The lookups of the entries in use missed the cache over them. The sweep
  takes a sixteenth of the entries every fourth present, so an entry goes at most 64 presents after its time.
- Comment: `buffer.c`, `buf_frame`.
- Check: no test covers it. Watch the count of live entries over a minute of play.

**K5. A storage buffer with no size of its own binds inside a shared window of guest memory.**

- Without the rule: each such buffer took the 16 MiB after its address. One game's terrain tiles, each about 1 MB
  after the last, made thirty overlapping copies of 16 MiB, 430 MB. A write under them made up to sixteen of them
  copy again. A window has its full size from the start, because each growth would copy the whole window again.
  Windows go when the GPU mappings change and when they are unused for a while, before the entries for them age out.
- Comment: `buffer.c`, above `WIN_N` (the code is `win_get` and `vk_buf_get_window`).
- Check: no test names `vk_buf_get_window`. Bind two bare addresses 1 MB apart and count the copies.

**K6. A range whose offset in a window does not meet the alignment of the device is copied alone.**

- Without the rule: the comment gives no failure. A binding offset must be a multiple of the alignment.
- Comment: `buffer.c`, `win_get`.
- Check: no test covers it.

**K7. A vertex or index range of unknown extent binds in a window, as a bare storage buffer does.**

- Without the rule: the comment gives no failure. The GPU reads the count of an indirect draw, so the binding must
  read on to the end of the window, and draws over the same memory share one copy.
- Comment: `buffer.c`, `vk_buf_get_window_input`.
- Check: no test names it.

**K8. A vertex or index buffer binds at any offset that keeps its elements aligned.**

- Without the rule: the lookup took only the alignment of uniform and storage bindings. A draw that read an earlier
  draw's storage result at another offset composed it over guest memory. In one game 216,000 vertex buffer bindings
  in 600 frames were composed that way, each a copy into staging and a GPU copy of the result over it.
- Comment: `buffer.c`, `vk_buf_get_input`.
- Check: no test covers it.

**K9. A stable range gets a buffer of its own in device memory, filled by a GPU copy that runs ahead of the
submission.**

- Without the rule: mapped memory for these buffers is the BAR window on most GPUs, 256 MB. Thousands of own buffers
  there crowded out the staging rings. One game's rings took writes at a twentieth of their speed for minutes, 18 to
  25 fps against 43. The copy runs in the upload command buffer so that the open rendering pass goes on. On a tiling
  GPU an end of the pass stores every target, and the next begin loads them again.
- Comment: `buffer.c`, above `make_own_`.
- Check: `check_pass_breaks` in `tests/vulkan_pass_breaks.inc`, the case of a range that held still for three checks.

**K10. With unified memory, a lookup reads guest memory in place only when no result is pending and no byte is GPU
owned.**

- Without the rule: a read in place misses bytes that only the GPU holds. A cached copy is also not valid while an
  in-place write is pending over it, because the draw would read a copy from before the write.
- Comment: `buffer.c`, `buf_get`.
- Check: `check_uma` in `tests/vulkan_headless.c` and `check_uma_writes` in `tests/vulkan_uma_imports.inc`.

**K11. A read-only storage buffer of a dispatch is checked with no delay, and with unified memory it binds in
place.**

- Without the rule: a changed word changes the result of the program, so the check cannot be late. Copied, every
  binding of one tensor took a 16 MiB GPU copy out of the import, then a hash and a 16 MiB copy on the GPU thread.
  That was about 35 copies, 540 MB and 15 ms of GPU time for each frame of one game's upscaled intro movie. The
  binding uses the alignment of storage bindings, not that of the staging ring, which can be larger.
- Comment: `buffer.c`, `vk_buf_get_strict`. The comment also lists the order rules that keep the read correct.
- Check: `check_uma_dispatch` in `tests/vulkan_uma_imports.inc`.

**K12. A constant buffer binds in place, but the pages that the runtime wrote are copied.**

- Without the rule: the comment gives no failure. The constant buffer updates of the runtime are among those pages.
- Comment: `buffer.c`, `vk_buf_get_uniform`.
- Check: `check_uma_constants` in `tests/vulkan_uma_imports.inc`.

**K13. The load of a save state drops every buffer, texture and render target that the caches keep.**

- Without the rule: guest memory under them is that of the state, and the host addresses of the entries are stale.
- Comment: `buffer.c`, `vk_buf_forget_all`, and `surface.c`, `vk_surf_forget_all`.
- Check: `tests/vulkan_nvn_savestate.inc` loads a state through the renderer. No test names the two functions.

## Change detection

**D1. Write tracking keeps one generation for each 4K page in a shared table.**

- Without the rule: a scan protects again the pages that it reports. With no shared table, the question of one cache
  entry hid a write from another entry over the same memory.
- Comment: `buffer.c`, above `WW_LEAF_SHIFT`.
- Check: `check_wt_two_consumers` in `tests/vulkan_write_tracking.inc`.

**D2. A 4 MB leaf is scanned at most one time in each write epoch.**

- Without the rule: each texture and buffer check made its own scan. One game made 5,600 scans in a frame. An epoch
  starts at each submission and at the end of each wait of the stream.
- Comment: `buffer.c`, above `g_wleaf_seen`.
- Check: `vk_ww_stats` counts the leaves that the scanner and the checks scan. No test holds the count.

**D3. A scanner thread scans the leaves, each scan claims its leaves, and a check waits only for a leaf in scan.**

- Without the rule: a scan of a leaf takes about 9 us in one game. Done by the checks, the scans cost 2.4 k cycles of
  each draw's setup on the GPU thread. One lock over all scans made checks wait out whole passes, 0.5 ms each. The
  claim also keeps the order that correctness needs. The written pages are in `g_wres` before the leaf says that it
  was scanned, so a check of the same leaf cannot miss a page that the scan protected again.
- Comment: `buffer.c`, above `WW_RES_N`.
- Check: `check_wt_concurrent` in `tests/vulkan_write_tracking.inc`.

**D4. The scanner visits only the leaves that a check needed at some time.**

- Without the rule: one game had 15 used leaves under a top of 18090. Passes that tested every leaf below the top
  were a sixth of the samples of the runtime. A pass to the last leaf of the address space read 131072 leaves at
  512 GiB.
- Comment: `buffer.c`, above `WW_EVER_WORDS` and above `g_wleaf_top`.
- Check: no test covers it. `vk_ww_stats` gives the time of the scanner.

**D5. A job on the GPU thread checks against the epoch that was newest when the job started.**

- Without the rule: a later submission can start an epoch while the job runs. What the CPU wrote for that submission
  is for the later job to see. The rule also gives the scanner the time until the job starts.
- Comment: `buffer.c`, the member `ww_stream` of `t_buffer`.
- Check: `check_stream_epoch` in `tests/vulkan_headless.c`.

**D6. The pages that a scan found get a generation newer than every baseline taken before.**

- Without the rule: those baselines were taken before the scan protected the pages again, and their caches possibly
  read memory before the writes. The write would be lost for them.
- Comment: `buffer.c`, `ww_take_scanned`.
- Check: `check_wt_record_submit` in `tests/vulkan_write_tracking.inc`.

**D7. A write through one address of a page is marked at the other addresses of that page.**

- Without the rule: a view and its source are the same pages. A cache over one address did not learn of a write
  through the other. Past 64 other addresses the answer for the range is -1, and the caches compare contents.
- Comment: `buffer.c`, above `WW_PEER_N` and above `WW_PEERS`.
- Check: `check_wt_alias`, `check_wt_shared` and `check_wt_hot_alias` in `tests/vulkan_write_tracking.inc`.

**D8. Memory that changes with no write that the tracker sees is marked written by the next query.**

- Without the rule: decommitted pages read as zero, and no scan reports them.
- Comment: `buffer.c`, above `WW_CHG_N` (the code is `vk_mem_changed`).
- Check: `check_wt_runtime_writes` in `tests/vulkan_write_tracking.inc`.

**D9. A full list of written ranges grows its last range, so the tracker marks more pages and never fewer.**

- Without the rule: a dropped range is a lost write. With the rule, a cache over the extra pages only checks its
  contents one time.
- Comment: `buffer.c`, above `WW_PEER_N`, above `WW_CHG_N` and at `ww_note`.
- Check: no test covers the full lists.

**D10. A scan skips the pages that an import of unified memory holds.**

- Without the rule: the driver takes those pages again at each submission, writable. A scan reported that as writes
  of the CPU. After a scan protected them, the driver faulted every one of them in again. `vk_written_since` cannot
  answer for those pages.
- Comment: `buffer.c`, `ww_scan_free`.
- Check: no test covers it directly. `tests/vulkan_uma_imports.inc` runs imports with the tracker on.

**D11. A page that a landing wrote counts as written by the CPU only when its bytes differ from the landing's.**

- Without the rule: the ownership lets go of landed pages, and a page that is watched again reads as written. The
  next scan reported all of them. Taken for a CPU write, the report made targets that were drawn there after the
  publish give up their pixels for the older bytes of memory. One game's reflection array, drawn whole over the
  memory of the previous one, kept seven layers of it, NaN included. The bytes are read through the mirror of the
  pool, which no trap covers.
- Comment: `buffer.c`, above `WwLanded` (the code is `vk_ww_landed` and `landed_unchanged`).
- Check: `check_wt_owned` in `tests/vulkan_write_tracking.inc`.

**D12. The list of landed pages stays sorted by a merge of the new entries.**

- Without the rule: each landing sorted all the entries again with `qsort` for its few new ones. That was a
  fourteenth of one game's GPU thread.
- Comment: `buffer.c`, above `g_wland_sorted` and `landed_sort`.
- Check: `check_wt_landed_order` in `tests/vulkan_write_tracking.inc`.

**D13. The clock of the tracker moves only when a query marks pages, and at its wrap the caches compare contents
for good.**

- Without the rule: baselines cannot be ordered against a wrapped clock. A clock that moved at each query would reach
  its wrap in a session, at thousands of queries a frame.
- Comment: `buffer.c`, `vk_written_since`.
- Check: no test covers the wrap.

**D14. A leaf that a range covers whole, and that has no table, gets only its newest generation.**

- Without the rule: the kernel side can unmap gigabytes at once, and a table for each leaf would be made for nothing.
- Comment: `buffer.c`, `ww_mark_pages`.
- Check: no test covers it. `check_wt_runtime_writes` covers a decommit of a small range.

**D15. With write tracking, a buffer of its own is checked only when its memory was written.**

- Without the rule: each check hashed the range. The exception is a source of the copy engine on pool pages, which
  is compared. Those pages can be written where the tracker does not look, and what a copy lands in keeps a stale
  byte for good.
- Comment: `buffer.c`, `buf_get`, at `tracked`.
- Check: `check_dma_untracked_source` in `tests/vulkan_headless.c`.

**D16. With no write tracking, a stable buffer of its own is checked every few submits, and a big one is sampled
between.**

- Without the rule: each check hashed the whole range. The cost of the rule is that a CPU write shows that many
  submits late. A write of the GPU front end always shows at once.
- Comment: `buffer.c`, `buf_get`.
- Check: no test runs the path with no tracking. `check_wt_sampled` covers the samples.

**D17. The sample hash serves only the checks that write tracking cannot answer.**

- Without the rule: a stable range that the CPU rewrites with the same bytes took the samples at each write. When
  the tracker stops its answers for a range, the next check hashes it whole first.
- Comment: `buffer.c`, `buf_get`, at `told`.
- Check: `check_wt_sampled` in `tests/vulkan_write_tracking.inc`.

**D18. After a GPU-side write, a buffer of its own copies the stamped pages again and does not hash the range.**

- Without the rule: a write to one page of a big buffer hashed all of it, most of one game's GPU thread. A small
  range is hashed all the same. GPU-side writes beside it stamp its pages, and in one game 760 such ranges a frame
  were 78% unchanged. Each copy ended the rendering pass.
- Comment: `buffer.c`, `patch_dirty`, and `buf_get` at the call.
- Check: no test names `patch_dirty`. The copy statistics of the buffer cache give the copies by reason.

**D19. A big buffer of its own takes only the pages written since it last matched memory.**

- Without the rule: a write anywhere in a 64 MiB window made the cache hash and copy all of it, again at each submit
  until it held still for two checks. In one game's underground, where terrain streams in, that was most of each
  hitch. The limit is `PATCH_MIN`, 256 KiB.
- Comment: `buffer.c`, above `PATCH_MIN` (the code is `patch_own`).
- Check: `check_pass_breaks` in `tests/vulkan_pass_breaks.inc`, the case of a write between two draws.

**D20. The written runs of a patch are copied as they are, however many they are.**

- Without the rule: past 64 runs, they were joined into one copy from the first to the end of the last. One game's
  CPU writes pages all over a 64 MiB window of indices. It copied about 50 MB a frame that way, a quarter of its GPU
  thread.
- Comment: `buffer.c`, above `Patch`.
- Check: no test covers more than 64 runs.

**D21. The copy of a patch waits for the draws before it and for an earlier patch.**

- Without the rule: the draws recorded before must read the old bytes, and the draws after must read the new bytes.
- Comment: `buffer.c`, `patch_apply`.
- Check: `check_pass_breaks` in `tests/vulkan_pass_breaks.inc`. The first draw reads the old bytes and the second
  the new.

**D22. Write tracking answers by GPU address, run by run of the host memory behind the range.**

- Without the rule: a range that the GPU maps from several pieces of guest memory has no one host pointer. It had no
  answer and was hashed whole at every check. In one game's matches that was 140 GB every 600 frames, nearly all of
  its GPU thread.
- Comment: `buffer.c`, `written_since_gva`.
- Check: `check_wt_pieces` in `tests/vulkan_write_tracking.inc` asks the same question for a texture.

**D23. Data with no buffer of its own goes into staging in one pass that hashes and copies.**

- Without the rule: the bytes were read two times, for the hash and then for the copy. That was half of one game's
  GPU thread. The bytes are hashed where they are, because the destination can be write-combined staging that is slow
  to read back.
- Comment: `buffer.c`, `buf_get`, and `cmd.c`, `vk_hash_copy`.
- Check: `check_hash_copy` in `tests/vulkan_headless.c`.

**D24. The content hash is XXH3 with the loop that the CPU supports.**

- Without the rule: the checks of the buffers hashed hundreds of megabytes a frame in one game. XXH3 keeps the
  quality of XXH64 at about three times its speed, 60 GB/s on a Zen 4 core against 19.
- Comment: `cmd.c`, above the XXH3 include block before `vk_hash`.
- Check: `check_hash_copy` holds the hash values equal. No test holds the speed.

**D25. A compare of a pending result with its original bytes reads only the pages that tracking saw written.**

- Without the rule: a storage range of an Ounce program is up to 16 MiB. One written page in it had every byte
  compared, a tenth of one game's CPU time.
- Comment: `buffer.c`, above `org_holds`.
- Check: `check_wt_compares` in `tests/vulkan_write_tracking.inc`.

**D26. Write tracking stands in for the compare that tells if a pending result can be copied over guest memory.**

- Without the rule: every draw that binds memory under pending results asks this of each result. The compare made
  the walk quadratic. A frame of 200 composed bindings spent a fifth of the GPU thread in `memcmp`. The tracker sees
  a write from the next epoch on. A host with no tracking compares.
- Comment: `buffer.c`, above `copyable_span`.
- Check: `check_wt_compares` in `tests/vulkan_write_tracking.inc`.

**D27. A span on a page that the result does not map counts as unchanged.**

- Without the rule: such a page holds no guest bytes, and the landing writes nothing there. One game's storage
  results end 336 bytes into an unmapped page. Answered "changed", each binding of them landed, 15 waits for the GPU
  in a frame.
- Comment: `buffer.c`, `copyable_span`.
- Check: `check_unmapped_tail` in `tests/vulkan_unmapped_tail.inc`.

**D28. All images take their write generations from one count.**

- Without the rule: a surface that counted its own writes ran ahead of the count of the uploads. An upload then gave
  it a generation that its image would reach again with guest memory still at it. The next write looked written back
  already and never landed.
- Comment: `surface.c`, above `g_surf_gen`.
- Check: no test covers it.

**D29. A texture with nothing left to take from drawn targets is passed over until the take count moves.**

- Without the rule: a texture searched the drawn targets each time it was bound and found the ones that it copied
  already. That was most of its searches in one game.
- Comment: `surface.c`, above `g_take_gen`.
- Check: `check_take_after_writes` in `tests/vulkan_headless.c`.

**D30. A render target keeps the baseline of its first draw until the next present.**

- Without the rule: a memory write after the first draw can leave untouched pixels stale even when later draws touch
  the target. A new baseline at each draw also scanned the whole target each time. With no write tracking the
  fallback is a hash, only for a target that a texture takes pixels from. A hash of every target at its first draw
  of each present took most of the GPU thread on FreeBSD and on Linux before 6.7.
- Comment: `surface.c`, `vk_rt_drawn`.
- Check: `check_wt_hot_rt_fresh` in `tests/vulkan_write_tracking.inc`.

**D31. A baseline takes in the earlier writes first, and then protects the hot pages again.**

- Without the rule: writes from before the baseline counted as later ones. A hot page that is cooled before the
  take-in stays hot past the baseline, so the target is not fresh although nothing was written.
- Comment: `surface.c`, `mem_baseline`, `run_take` and `run_cool`.
- Check: `check_wt_hot_rt_take_in` and `check_wt_hot_page` in `tests/vulkan_write_tracking.inc`.

**D32. A render target loads from memory the GPU-side writes that the draws of the 3D engine do not see.**

- Without the rule: inline uploads, constant updates and compute results land in memory. DMA and 2D copies make the
  target load them, but those writes did not.
- Comment: `surface.c`, `vk_rt_note_read`.
- Check: no test names it.

**D33. GPU-owned bytes read as the identity of their owner, in whole words.**

- Without the rule: a texture hash over GPU-owned memory read the tag byte by byte, a third of the GPU thread. An
  owner that is written again in place, or a new owner at a freed address, must give a different identity, or a
  texture over it keeps its old texels.
- Comment: `owned.c`, `vk_guest_peek`.
- Check: `check_owned_peek_identity` and `check_owned_whole_identity` in `tests/vulkan_owned_readback.inc`.

**D34. After a merge that the CPU won in part, the surface changes its hash so that the next bind uploads.**

- Without the rule: the image still holds the shader result where the CPU won. The write-back hashed the merged
  bytes, so the next bind found the hash equal and kept the image.
- Comment: `surface.c`, `flush_land`.
- Check: no test covers it.

**D35. The mirror copies in only the bytes that the CPU changed, by a compare with a shadow in host memory.**

- Without the rule: a copy of whole pages replaces the bytes that a program wrote there and that did not land yet.
- Comment: `mirror.c`, the file header.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`.

**D36. A mirror page with no pending program write is copied whole when a byte changed.**

- Without the rule: a page that the CPU wrote in scattered bytes made one copy region for each run. One copy had
  194,545 regions.
- Comment: `mirror.c`, `diff_run`.
- Check: no test holds the region count. `check_mirror` covers the whole-page copy.

**D37. The mirror looks at the pages of a binding 64 at a time, by mask word.**

- Without the rule: a binding is up to 16 MiB for each storage buffer of each draw and dispatch. A loop over the bit
  of each page was a fourteenth of one game's GPU thread.
- Comment: `mirror.c`, above `vk_mirror_slice`.
- Check: no test covers the cost.

**D38. A mirror range that was brought up to date in this epoch needs no second walk in the epoch.**

- Without the rule: a draw binds the same 16 MiB ranges as the draws before it. Three walks over 4096 pages at each
  binding were 8% of an Ounce game's GPU thread.
- Comment: `mirror.c`, `vk_mirror_slice`, at `ck_epoch`.
- Check: no test covers the cost.

**D39. A page that landed loses its shadow, and the next binding brings it up to date whole from guest memory.**

- Without the rule: the shadow no longer holds what guest memory holds, and a compare with it copies the wrong bytes.
- Comment: `mirror.c`, `vk_mirror_landed`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the binding after the CPU touch.

## Results that the GPU writes

**R1. A result byte lands only where the shader changed it and guest memory still holds what the shader read.**

- Without the rule: on the console the GPU stores while it runs, and a later CPU write wins. A landing of the whole
  result puts old bytes over that CPU write. Each writable binding keeps its original bytes until the frame of its
  draw completes.
- Comment: `writeback.c`, the file header, `compute.c`, the file header, and `buffer.c`, above `writer_of`.
- Check: `check_writeback_merge` in `tests/vulkan_release.c`, and `check_owned_writable` in
  `tests/vulkan_owned_readback.inc`.

**R2. A landing compares two host copies and reads guest memory only where the shader changed a byte.**

- Without the rule: a compare against guest memory can fault on a page that the GPU holds. A result that changed
  nothing must read no guest memory, allocate nothing and flush nothing. The merge takes eight bytes at a time.
  Byte by byte, it was a tenth of one game's GPU thread.
- Comment: `writeback.c`, `vk_write_changed_runs`.
- Check: `check_runs` and `check_writeback_merge` in `tests/vulkan_release.c`.

**R3. A logged result lands the bytes that its write logs give, and no others.**

- Without the rule: a logged result has no original bytes for a compare. A byte that no writer wrote must keep what
  guest memory holds. A run that the CPU wrote and the shader did not is not touched.
- Comment: `buffer.c`, `land_logged`, `writeback.c`, `vk_write_runs`, and `writelog.c`, the file header.
- Check: `check_storage_logged` in `tests/vulkan_storage_logged.inc`.

**R4. A logged result is written in place only by a writer that also logs.**

- Without the rule: a writer with no log leaves bytes that nothing lands. Such a writer composes a result of its own
  over the logged one. A reader of a write-only result also composes, because that staging holds no guest memory.
- Comment: `buffer.c`, `gfx_gpu_src`, and `out_take_org` for the writer that joins in the same draw.
- Check: `check_storage_logged` in `tests/vulkan_storage_logged.inc`, the cases of a writer that logs nothing.

**R5. The pending results are indexed by the guest pages that they cover.**

- Without the rule: a binding or a read compared each of the hundreds of results that a frame keeps pending. The
  search also narrows by the longest run in the index.
- Comment: `buffer.c`, above `OutIndex`.
- Check: `check_pending_index` in `tests/vulkan_pending_index.inc`. `--bench-storage-lookup` of the same program
  times it.

**R6. A set of the pages that pending results cover answers most reads and writes with no walk.**

- Without the rule: almost every read or write of guest memory meets no result, and each one walked the results. A
  change of the GMMU still checks every result, so a result over a page about to be mapped lands first.
- Comment: `buffer.c`, above `g_out_pages`, `compute.c`, above `g_pages`, and `mirror.c`, above `g_mir_pend`.
- Check: the ctest `pending_pages`, and `check_map_lands_covered` in `tests/vulkan_headless.c`.

**R7. The block of a result is not zeroed when it is made.**

- Without the rule: the caller fills the original bytes and the page lists. The zero pass before that was a tenth of
  the copy time of one game's GPU thread.
- Comment: `buffer.c`, `gfx_out_new`.
- Check: no test covers the cost.

**R8. Two ranges that are each one run of guest memory meet in one span, with no walk by page.**

- Without the rule: the walk looked up each page of the range in the pages of each result. A storage range of half
  a megabyte against a few dozen results was a sixth of one game's GPU thread.
- Comment: `buffer.c`, `out_spans`.
- Check: no test covers the cost. `check_pending_index` covers the answers.

**R9. The guest pages of a range are translated one time for a walk of the pending results.**

- Without the rule: the translation for every result was the top cost of the GPU thread when results stayed pending
  through a frame.
- Comment: `buffer.c`, above `RangePages`.
- Check: no test covers the cost.

**R10. A range meets a result only where the two share bytes, not where they share a page.**

- Without the rule: a result lands its own bytes alone, so the rest of its page is free to read and write. One game
  writes a word next to its transform feedback captures before each capture. Two storage buffers that share a page
  made each read of one compose with the results of the other, which gave it nothing.
- Comment: `buffer.c`, above `out_meets`, and above `gfx_gpu_src`.
- Check: `check_pending_index` in `tests/vulkan_pending_index.inc` (a read beside a result on its page), and
  `check_storage_suffix` in `tests/vulkan_storage_suffix.inc`.

**R11. When a result lands, each later composed result takes the changed bytes as its own original bytes.**

- Without the rule: the later result read the earlier result where they share memory, not the bytes that its
  original copy holds. It took the landing for a guest write and dropped its own writes over those bytes.
- Comment: `buffer.c`, above `ChgRun` (the code is `out_rebase` and `out_rebase_later`).
- Check: no test covers the rebase alone. `check_draw_storage_stages` and `check_storage_suffix` use composed
  results.

**R12. A rebase uses the changed runs that the landing found.**

- Without the rule: the rebase of every later composed result compared all the bytes of the landed result again.
  One game landed 20,000 results in 10 s that changed 4 MB of their 590. The rebases read 42 GB, most of the release
  thread, under `vk_lock`, which the GPU thread waits for.
- Comment: `buffer.c`, above `ChgRun`.
- Check: no test covers the cost. `vk_buf_checks` gives the bytes compared.

**R13. A rebase copies the changed bytes eight at a time.**

- Without the rule: a result is mostly the bytes that it read. A byte loop was 84% of the time of the release thread
  in one game.
- Comment: `buffer.c`, above `rebase_bytes`.
- Check: no test covers the cost.

**R14. The GPU copies of pending results run in the order of the results, with a barrier where two copies
overlap.**

- Without the rule: one copy command must not overlap itself, and two copies with no order land in either order. The
  later result must end on top.
- Comment: `buffer.c`, above `CopyBatch`.
- Check: `check_storage_suffix` in `tests/vulkan_storage_suffix.inc` reads a range under more than one result.

**R15. A composition takes each byte from the newest result over it and skips the older results there.**

- Without the rule: the copy of the older results, only to overwrite them, made a composition quadratic. One game
  composed 200 bindings a frame over a few hundred results each. Its GPU copied 955 MB a frame, 147 ms.
- Comment: `buffer.c`, above `Covered`.
- Check: no test holds the copied bytes.

**R16. A composition writes on the CPU only the gaps that no result gives.**

- Without the rule: the CPU copied the whole range first. A few results over storage ranges of half a megabyte,
  bound and read a few hundred times a frame, made that a fifth of one game's GPU thread.
- Comment: `buffer.c`, above `gfx_overlay`.
- Check: no test holds the cost.

**R17. A draw can write a result of an earlier submit of the frame again in place.**

- Without the rule: one game binds a suffix of a storage buffer for each of its passes, about thirty submits a
  frame. Nearly every one made a new result, half a megabyte copied from guest memory and compared at the landing.
  The rule holds only when no later result has bytes of the earlier one. Such a later result lands first, and the
  moved result would land over its bytes.
- Comment: `buffer.c`, above `out_take_into_submit`.
- Check: `check_storage_suffix` in `tests/vulkan_storage_suffix.inc`.

**R18. A result that is kept in its owner can be taken into a submit of a later frame.**

- Without the rule: the kept result is in its owner, not in the staging of its frame, and it copies nothing back.
  When the take was refused, each frame's binding composed again while the GPU ran a frame behind, and landed the
  owner.
- Comment: `buffer.c`, `out_take_into_submit`.
- Check: `check_storage_buffer_keep` in `tests/vulkan_storage_buffer_keep.inc`, the second binding after the fence.

**R19. When several results cover a binding inside one of them, the later ones merge into that one.**

- Without the rule: one game writes a storage range of 384 KB whole, then 16 bytes in it at an address that a
  binding offset cannot have. It reads the range in windows of 4 KB. Each read composed all the results again and
  ended the rendering pass, 133 times a frame. The merged result compares with its original bytes when it lands, so
  that the bytes of the dropped results are not lost. Either all the later results merge or none does.
- Comment: `buffer.c`, above `out_absorb`.
- Check: no test covers it.

**R20. A read takes the newest result in place when that result covers the whole range.**

- Without the rule: the code required a single result over the range. Every binding after a second result composed
  again. In one game each storage range was copied, written back and landed about 16 times a frame. That was 114,000
  compositions in 600 frames, against 3,600 with the rule.
- Comment: `buffer.c`, `gfx_gpu_src`.
- Check: `check_storage_suffix` in `tests/vulkan_storage_suffix.inc`.

**R21. A range inside the last result is found with no translation and no search.**

- Without the rule: the comment gives no numbers. A new binding of a suffix is common for NVN storage draws, and the
  full search selected the same result each time. Other addresses still use the search that knows the aliases.
- Comment: `buffer.c`, `gfx_gpu_src`, at `g_out_tail`.
- Check: `--bench-storage-lookup` in `tests/vulkan_storage_lookup_bench.inc` times recent and old ranges apart.

**R22. A result of the frame before is read where it is, and the rings of that frame are held for the reader.**

- Without the rule: transform feedback captures the same buffers every frame, over the capture of the last frame.
  A copy through guest memory would land each one.
- Comment: `buffer.c`, above `gfx_gpu_src` (the hold is `vk_frame_hold` in `cmd.c`).
- Check: `check_nvn_xfb` in `tests/vulkan_nvn_xfb.inc` captures and draws again with no CPU read between.

**R23. Maxwell bindings keep the automatic read and write order, and NVN bindings take their order from explicit
barriers.**

- Without the rule: NVN permits draws that overlap, and a writable view alone makes no dependency there. A barrier
  for each one ends the pass. Read-only consumers and new submissions keep their automatic checks.
- Comment: `buffer.c`, `gfx_gpu_src`, and `vk_buf_order_pending`.
- Check: `tests/vulkan_nvn_storage.inc`.

**R24. A setup that is abandoned can add a barrier, but it cannot remove one or count a write.**

- Without the rule: the write count would move for a draw that was never recorded. The count moves only after the
  draw or dispatch is recorded. A stamp or a read flag that an abandoned setup leaves only adds a barrier.
- Comment: `buffer.c`, `vk_buf_writable_commit` and `gfx_gpu_src`.
- Check: no test covers an abandoned setup.

**R25. Repeated bindings of one writable range in one draw name the same storage.**

- Without the rule: the comment gives no failure. Two storages for one range would each land.
- Comment: `buffer.c`, `copy_writable`.
- Check: `check_storage_logged` covers two writers of one draw. No test binds the same range two times.

**R26. A writable binding stays on the GPU in an owner, and the trap lands it when the CPU touches the bytes.**

- Without the rule: the copies wrote the bytes of a range two times on the CPU before its draw and compared all of
  them after it, even when the CPU never read them. The rule needs a host with traps, a range of whole pages in one
  run of guest memory, no unified memory, and a range that the CPU did not make land in the last 1200 presents.
- Comment: `buffer.c`, above `KEEP_CPU_PRESENTS` (the code is `keep_new`).
- Check: `check_storage_buffer_keep` in `tests/vulkan_storage_buffer_keep.inc`.

**R27. A kept result is written again in place only while its owner still holds the whole range.**

- Without the rule: a CPU touch lands the owner and lets go of it. A draw that writes it again would write bytes
  that nothing lands.
- Comment: `buffer.c`, above `keep_intact`.
- Check: `check_storage_buffer_keep`, the case of a CPU read before the fence.

**R28. On unified memory, a logged result that cannot be written in place is written back on the GPU after its
dispatch.**

- Without the rule: such a result waited in staging for the CPU to land it. One game binds 16 MiB windows of a
  tensor pool, and the last reaches 185 KB into the GPU-owned span of a movie frame. The next dispatch could not
  tell if the CPU wrote the window, so it landed it with a `vk_finish`, 72 ms each time, about two times a frame.
- Comment: `buffer.c`, above the `BACK_IMPORT` enum (the code is `back_record`).
- Check: `check_uma_logged_back` in `tests/vulkan_uma_imports.inc`.

**R29. On unified memory, a shader writes guest memory in place, but not over bytes that the GPU holds in another
place.**

- Without the rule: owned bytes, a compute or storage image result and a queued release must land first. A write in
  place would be under them.
- Comment: `buffer.c`, `copy_writable`.
- Check: `check_uma_writes` and `check_uma_feedback` in `tests/vulkan_uma_imports.inc`.

**R30. The staging of a write-only logged binding is filled with zeros by the GPU.**

- Without the rule: the stores are partial. The GPU reads the cache line around a store, and uninitialized ring
  memory there can hang the GPU. A `memset` by the CPU of up to 16 MiB a binding was a twentieth of one game's GPU
  thread.
- Comment: `buffer.c`, `copy_writable`.
- Check: no test covers it.

**R31. All the bindings of a draw that aliases a writable page index one page atlas.**

- Without the rule: a write through one GPU address must reach the same word through the other, also with fragmented
  mappings. The alias question is answered from the page ranges first. The sorted page list for every draw with
  storage was a twentieth of the GPU thread in one game.
- Comment: `buffer.c`, above `vk_buf_draw_prepare` and above `draw_may_alias`.
- Check: `check_draw_storage_stages` in `tests/vulkan_draw_storage_stages.inc`.

**R32. A range that one owner holds whole binds where the owner is.**

- Without the rule: the range was copied, and each such draw ended the rendering pass, about 22 of the 380 of a
  frame in one game. Before that, draws over the results of the GPU engines landed them, a wait for the GPU each.
  A range with owned parts is copied into the device ring. It is not hashed and gets no buffer of its own, because
  the CPU copy of the owned bytes is stale.
- Comment: `buffer.c`, above `buf_get_owned`.
- Check: `check_pass_breaks` in `tests/vulkan_pass_breaks.inc`, and `check_dma_owners` in `tests/vulkan_headless.c`.

**R33. A read over program writes that the mirror holds reads them in the mirror.**

- Without the rule: a landing took a submit and a wait for the GPU, 150 a second in one game's menus. At an offset
  that the use cannot bind, the GPU copies the bytes from the mirror into device memory. Bytes that the mirror owns
  are read there too. Read in another place, the CPU bytes between them were copied again for every draw, 16 MiB a
  binding, about half of one game's GPU thread.
- Comment: `buffer.c`, `buf_get`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the case of constants over a pending program write.

**R34. A result that covers a range only in part lands before the cached copy is used.**

- Without the rule: the cache would use its copy without the bytes of the result.
- Comment: `buffer.c`, `buf_get`, and `compute.c`, `vk_compute_gpu_src`.
- Check: no test names the partial case for the buffer cache.

**R35. A write-back of a render target keeps the pages that the CPU wrote after the draws of the target.**

- Without the rule: a game uses the memory of a target again when its sync wait says that the GPU is done. One game
  records command lists where a target was. A GPU write beside the target wrote it back over the records of a queued
  job. The cost of the page rule is that a page written between two draws loses the pixels of the later draw.
- Comment: `surface.c`, above `CPU_RUNS`.
- Check: `check_wt_hot_page` and `check_wt_hot_same_frame` in `tests/vulkan_write_tracking.inc`.

**R36. The CPU runs of a write-back count from the moment when the image last matched memory.**

- Without the rule: the draw baseline alone missed a target that a copy changed after a load, with no draw.
- Comment: `surface.c`, above `run_newer` (the code is `cpu_newer`).
- Check: `check_wt_hot_rt_overlap` in `tests/vulkan_write_tracking.inc` covers the baselines of two targets.

**R37. A write-back takes a new baseline after it, and tells the tracker which pages it wrote.**

- Without the rule: the writes of the write-back counted as CPU writes. Where pages were kept for the CPU, the image
  is not memory there, so the target takes no baseline and loads from memory at its next use.
- Comment: `surface.c`, `flush_rebase` and `run_landed`.
- Check: `check_wt_hot_page` in `tests/vulkan_write_tracking.inc`.

**R38. Before a target lands, the older pending targets over its bytes land first, and newer ones stay on top.**

- Without the rule: memory that was freed and given to a new target still has the pixels of the old target pending.
  The bytes must land in the order in which the GPU of the console wrote them.
- Comment: `surface.c`, `rt_pending_over` and `rt_flush_older`.
- Check: `check_rt_overlap` in `tests/vulkan_headless.c`.

**R39. A landing under a newer target, also one at another GPU address of the same memory, is not a write for that
target.**

- Without the rule: one game maps one physical pool through several virtual pools. A shadow map that landed late
  left the newer target with the look of a written one. Its next use loaded the old bytes over its pixels, and a
  whole frame of a cutscene showed pieces of old frames.
- Comment: `surface.c`, above `rt_newer_over`.
- Check: `check_alias_newer_target` in `tests/vulkan_headless.c`.

**R40. Before a producer publishes an owner over memory, every older image over that memory lands.**

- Without the rule: a later read of part of an old image can write its old pixels over the result of the producer.
- Comment: `surface.c`, `vk_rt_before_write_cpu`.
- Check: `check_alias_newer_target` covers the order for a compute result. No test names the function.

**R41. The result of a storage image is published over its guest bytes and waits for nothing.**

- Without the rule: each write waited for the GPU. A 3D volume that a game writes two times a frame and only
  samples took a wait each time, a fifth of the run.
- Comment: `surface.c`, above `surf_publishable` (the code is `vk_surf_publish`).
- Check: `check_storage_image_keep` in `tests/vulkan_storage_image_keep.inc`.

**R42. A publish stamps only the bytes of the layers of the image itself.**

- Without the rule: the bytes between the layers keep their owners, but they were stamped written. One game writes
  each level of its reflection cubes through a view whose layers are a whole mip chain apart. Every view and every
  cube loaded all its memory again each frame, 30 uploads and 12 MB.
- Comment: `surface.c`, above `publish_own`.
- Check: no test covers it.

**R43. The pending targets inside a published span are retired, and are not flushed again.**

- Without the rule: the publish flushed each one again, a wait for the GPU and a readback. One game draws four
  32x32 targets of 256 KB and writes them with a compute program every frame. That was four waits and four landings
  a frame.
- Comment: `surface.c`, above `rt_retire_over`.
- Check: no test covers it.

**R44. The span of a storage image that a draw writes is GPU owned from the bind on.**

- Without the rule: the write-back read all the bytes on the CPU before the draw and copied the image back after
  it. The conditions are those of R26. A span that the CPU touches every frame would trap and wait for the GPU each
  time.
- Comment: `surface.c`, above `KEEP_CPU_PRESENTS` (the code is `vk_surf_keep_begin`).
- Check: `check_storage_image_keep` in `tests/vulkan_storage_image_keep.inc`.

**R45. A release publishes the pitch-linear color targets drawn since the last release, and watches the
block-linear ones.**

- Without the rule: the CPU cannot ask for the pixels. One game draws its ambient light into seven 8x8 RGBA32F
  targets and reads them after a fence. It lit its scenes with the old bytes, values up to 1e31. Another game draws
  the thumbnail of its save into a 320x180 block-linear target and reads the texels with no call to the GPU. A
  publish of every block-linear target would copy tens of megabytes a frame, so those are watched.
- Comment: `surface.c`, above `DrawnList`.
- Check: `check_rt_watch` in `tests/vulkan_headless.c`.

**R46. A target whose load failed has undefined contents and is never written back.**

- Without the rule: the image holds old pixels or some bands of the new ones, and a write-back puts them over
  memory.
- Comment: `surface.c`, `rt_reload`.
- Check: no test covers a failed load.

**R47. A dispatch reads a read-only input through the buffer cache.**

- Without the rule: the inputs were copied whole at every dispatch. That was nearly all of one game's GPU thread,
  whose passes read four buffers for each one that they write.
- Comment: `compute.c`, `res_bind`.
- Check: `check_stream_epoch` in `tests/vulkan_headless.c` covers the strict lookup. No test counts the copies of a
  dispatch.

**R48. A dispatch that writes an owner in place stamps the owner with its submission.**

- Without the rule: no publish follows such a write. A landing of the range must wait for the submission that
  records the write, also on a compute queue of its own.
- Comment: `compute.c`, `res_wrote`, and `owned.c`, `vk_own_res_wrote`.
- Check: `check_owned_in_place_write` in `tests/vulkan_owned_readback.inc`.

**R49. A read-only input inside a pending result of the same batch is read from the arena after a barrier.**

- Without the rule: the result landed first. Skinning in several passes reads what the pass before wrote.
- Comment: `compute.c`, `vk_dispatch`.
- Check: no test covers it.

**R50. Each dispatch that marks its stores binds the write log.**

- Without the rule: the atomic OR of the stores reads a descriptor that was never written, an Xid 13 on NVIDIA.
- Comment: `compute.c`, `vk_dispatch`.
- Check: `check_mirror_fold` in `tests/vulkan_mirror.inc` binds a log. No test runs a dispatch with no log bound.

**R51. A buffer that is written and not logged still gets its input copied in.**

- Without the rule: its result is what differs from the input after the dispatch. A store of the same value over
  zeroed bytes would differ from nothing and never land.
- Comment: `compute.c`, `vk_dispatch`.
- Check: no test covers it.

**R52. The outputs of a dispatch are made after every input is read, and the slots of one buffer view one copy.**

- Without the rule: an input can alias the output of another storage buffer. Loads, atomics and stores of one buffer
  must each see the writes of the others.
- Comment: `compute.c`, `vk_dispatch`.
- Check: no test covers it.

**R53. Only a program that logs every buffer that it writes takes the mirror, and a Switch program does not take
it.**

- Without the rule: a buffer written with no log never lands from the mirror. With the mirror, the props of one
  Switch game vanished for one frame in 3 to 9 of 250 shots in each of three runs. They vanished in none of 250
  with no mirror, and in none on a host with no traps. The comment says that the cause is not found.
- Comment: `compute.c`, above `mirror_plan`.
- Check: no test covers the Switch case. Repeat the 250 shots if the rewrite changes which programs take the
  mirror.

**R54. The mirror holds the storage buffers of the programs that log, in place, one dispatch after another.**

- Without the rule: each buffer is all the memory mapped from its address, up to 16 MiB. Made new for each dispatch,
  with a copy in and a copy back, it cost a game most of its CPU and GPU time and gigabytes of rings. Hundreds of
  16 MiB bindings a frame came at addresses a few KiB apart.
- Comment: `mirror.c`, the file header, and `compute.c`, above `mirror_plan`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`.

**R55. Before the mirror publishes pages as its own, it takes in the CPU changes, and a byte that both wrote keeps
the program's value.**

- Without the rule: the first CPU touch lands a page whole. That is exact only when the page holds the CPU bytes
  that the program did not log.
- Comment: `mirror.c`, above `vk_mirror_publish`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the case of a byte that the CPU wrote beside the program's.

**R56. The write log of a dispatch in the mirror is in GPU memory, and a host log is cleared only where it was
written.**

- Without the rule: on a discrete GPU each store crossed the bus two times. One game's upscaler program took 47 ms
  of an RTX 5080 for a dispatch, against 0.14 ms with the log in GPU memory. The folds took 14 ms each, against
  0.3 ms. A host log that is cleared whole costs the 16 MiB of the binding, not what the stores wrote.
- Comment: `writelog.c`, the file header.
- Check: `check_mirror_fold` in `tests/vulkan_mirror.inc` runs with the log in GPU memory.

**R57. A publish on a host with no traps lands the neighbors on the edge pages first.**

- Without the rule: other owned bytes can still trap the edge pages after the range is dropped, and the fallback
  writes through the guest pointer.
- Comment: `owned.c`, `own_publish`.
- Check: the ctest `memory_own_no_traps`. No renderer test names the edge pages.

**R58. On unified memory, a publish over memory that an import holds is a GPU copy in place, with no owner and no
trap.**

- Without the rule: each trap let go of the imports over its pages, and the next use made them again. A watch on
  such pages would take them from the import.
- Comment: `owned.c`, `own_publish` and `vk_own_watch`.
- Check: `check_uma_publish` in `tests/vulkan_uma_imports.inc`.

**R59. A run with pending in-place writes that no one import holds whole is read part by part.**

- Without the rule: a logged result written back across the edge of an import left such runs. The wait for its
  submit took all the work recorded before it.
- Comment: `owned.c`, above `inplace_part`.
- Check: `check_uma_logged_back` in `tests/vulkan_uma_imports.inc` writes across an import edge.

## Waits for the GPU

**W1. The setup of a draw or a dispatch does not submit and does not wait for the GPU.**

- Without the rule: a wait lands results into guest memory that the setup possibly read before. When the submission
  number changed during the setup, the setup starts again. The frame check runs before the setup for this reason. A
  hash of a target reads the backing memory directly, because `gmem_read` could submit a draw in the middle of a
  setup.
- Comment: `cmd.c`, above `frame_rings` and above `vk_submit_upload_wait`. `compute.c`, `vk_dispatch`. `surface.c`,
  above `rt_mem_hash`.
- Check: `check_descriptor_room` in `tests/vulkan_headless.c`.

**W2. A read of guest memory under results waits only for the newest submit that holds one of them.**

- Without the rule: the comment gives no failure. With the rule the GPU goes on with what came after. Only a
  result in the commands that are being recorded makes the read finish them all.
- Comment: `buffer.c`, `mem_need_reason`.
- Check: `check_landing_wait` in `tests/vulkan_lock_waits.inc` covers a wait for one submission.

**W3. What is recorded goes to the GPU when it holds a millisecond of the work of the GPU thread.**

- Without the rule: submissions came only when something needed results. In one game the GPU sat idle through the
  first 12 ms of the 21 ms of a frame, then got all of it at the end and finished 6 ms after the GPU thread. The
  game ran at 34.7 fps, and at 40.3 with the rule. Limits of 2 ms and 0.5 ms were both slower. Earlier submissions
  also land their results sooner, so fewer results are pending at a lookup.
- Comment: `cmd.c`, above `frame_rings` (the limit is `SUBMIT_EVERY_NS`, the code is `frame_check`).
- Check: `check_pass_breaks` has a case that depends on this submit. No test holds the limit.

**W4. Each submit lands the results of the submissions that the GPU finished.**

- Without the rule: a later release found them pending.
- Comment: `cmd.c`, `submit`.
- Check: `tests/vulkan_release.c` runs releases against completed submissions.

**W5. A wait for work that is already done is not counted as a stall.**

- Without the rule: the wait statistics blame waits that cost nothing.
- Comment: `cmd.c`, `timeline_wait`.
- Check: no test covers it. `check_stats_gpu_time` covers other statistics.

**W6. A wait for the GPU runs in slices of a second, and the run ends when the GPU is stuck for 15 s.**

- Without the rule: a stopped GPU signals nothing, and the driver did not report a lost device. The frontend and the
  release thread waited for good, and one game's window froze with no message. The clock of the wait runs only while
  the queue holds work that the GPU did not do. In one race on a loaded host a thread waited past 15 s for a
  pipeline with the GPU idle, and the run ended as "GPU stopped" with nothing for the GPU to do. A lost device also
  ends the run, because the next Vulkan call would crash in the driver and the log would blame the game.
- Comment: `cmd.c`, above `VK_GPU_STUCK_NS`, and `vk_wait_semaphore`.
- Check: no test covers it.

**W7. Each `vk_finish` is counted by its call site, with its time.**

- Without the rule: one game made nine a frame, each a round trip with the GPU idle after it, and nothing showed
  where they came from.
- Comment: `cmd.c`, above `FINISH_SITES`.
- Check: `vk_finishes_log`. Several checks of `vulkan_headless` read the finish count.

**W8. A draw that aliases a writable page copies the pending results into its atlas on the GPU.**

- Without the rule: the draw waited for earlier results to land in guest memory. One game drew about four such
  draws a frame, each a landing of everything with a `vk_finish`. A result whose original bytes the guest wrote
  since still lands first, and the draw starts its setup again.
- Comment: `buffer.c`, above `out_near_atlas` (the code is `atlas_fill`).
- Check: `check_draw_storage_stages` in `tests/vulkan_draw_storage_stages.inc`.

**W9. A draw reads its bytes past the samples-passed reports that are queued over them.**

- Without the rule: a read of the bytes as they are waited for the GPU to run everything recorded up to the reports.
  The GPU writes the counts where the bytes go.
- Comment: `buffer.c`, above `draw_bytes`.
- Check: `pb_samples_case` in `tests/vulkan_pass_breaks.inc` draws with queued reports. It is not confirmed that
  the case reads draw bytes under a report.

**W10. The reads of the mirror take one barrier after the program writes, not one for each binding.**

- Without the rule: a barrier for each binding ended the rendering pass at nearly every draw. One game's race had
  about 470 passes a frame, and the GPU drained at each, 30 ms a frame. In `mirror.c` a binding that recorded no
  copy also takes no barrier. With one for each, two thirds of the passes of one game's run on another GPU ended
  there.
- Comment: `buffer.c`, `buf_get`, at `barrier_gen`, and `mirror.c`, `vk_mirror_slice`.
- Check: no test counts the passes of mirror reads. `check_pass_breaks` counts passes for other copies.

**W11. A compute batch is submitted when something needs it and lands when something needs its results.**

- Without the rule: each batch landed at its submit with a fence wait, 4 waits a frame in one game.
- Comment: `compute.c`, above `CsBatch`.
- Check: no test counts the waits.

**W12. A constant buffer under a pending in-place write is copied for the dispatch on the GPU, after the write.**

- Without the rule: the CPU read waited for the submit. One game's dispatches bound 256 bytes of such constants one
  time a frame. The wait held the GPU thread for about 18 ms of each frame with the GPU idle behind it. The CPU
  still reads the words that a translation depends on.
- Comment: `compute.c`, above `g_cb_later`.
- Check: `check_uma_dispatch_constants` in `tests/vulkan_uma_imports.inc`.

**W13. A dispatch on the graphics queue binds as a draw does, and its results stay pending.**

- Without the rule: each one landed at once with a `vk_finish`, about 9 a frame in one game.
- Comment: `compute.c`, `vk_dispatch`, at `deferred`.
- Check: `check_uma_dispatch` covers a dispatch on the graphics queue. No test counts the finishes.

**W14. A dispatch of storage buffers alone keeps them on the GPU where the host traps guest memory.**

- Without the rule: on the compute queue each result landed at the next dispatch that read it. One game's passes
  update the same buffers dispatch after dispatch. It waited for the GPU at nearly every one and ran at about 14
  frames a second on any machine.
- Comment: `compute.c`, `vk_dispatch`, at `resident`, and above `res_bind`.
- Check: `check_owned_in_place_write` in `tests/vulkan_owned_readback.inc`.

**W15. A range that the CPU made land in the last 1200 presents is not kept on the GPU.**

- Without the rule: each CPU read of a kept range trapped and waited for the GPU. One game reads its compute results
  every frame. It had 34,000 landings in two minutes, and its frame rate fell from 54 to 39. The same limit serves
  the writable bindings of draws and the storage images.
- Comment: `compute.c`, `vk_dispatch`, and above `CS_CPU_READ_PRESENTS`. `owned.c`, above `cpu_read_note`.
  `buffer.c` and `surface.c`, at `KEEP_CPU_PRESENTS`.
- Check: `check_storage_buffer_keep` in `tests/vulkan_storage_buffer_keep.inc`, the case of a range that the CPU
  wrote lately.

**W16. A dispatch whose storage buffer is under a pending result of a draw runs on the graphics queue.**

- Without the rule: the compute queue waited for the result to land, three times a frame in one game.
- Comment: `compute.c`, `vk_dispatch`.
- Check: no test covers it.

**W17. A deferred dispatch is submitted at once, but a dispatch in the mirror is not.**

- Without the rule: a reader of the results waited for the draws recorded after the dispatch. One game writes
  command lists with a dispatch and fetches them a little later, one time a frame. In the mirror the results land
  only where the CPU first touches them. Submitted each, one game's Ounce programs made about 300 submissions a
  present, each with a harvest, and the GPU idled between them.
- Comment: `compute.c`, `vk_dispatch`.
- Check: no test counts the submissions.

**W18. Where the device has a compute queue of its own, a landing copies there and waits only for the writer's
submission.**

- Without the rule: the copy waited behind every submission queued after the writer. One game's landings waited 30
  to 130 ms.
- Comment: `owned.c`, above `land_apart`.
- Check: `check_owned_in_place_write` in `tests/vulkan_owned_readback.inc`.

**W19. A landing never waits for a submission value past the last submission.**

- Without the rule: an owner with no stamp, or one stamped while nothing was recorded, gave a value that is never
  signaled. The wait never ended.
- Comment: `owned.c`, `own_land`.
- Check: no test covers it.

**W20. The pending counts of mirror pages stay right when two written ranges overlap.**

- Without the rule: the pages of two buffers that overlap stayed pending for good, and each CPU read of them waited
  for the GPU. The joined ranges take their count before the written ranges lose theirs.
- Comment: `mirror.c`, `vk_mirror_harvest`.
- Check: `check_mirror_fold` covers two buffers of one dispatch. No test reads the count after the batch.

**W21. A mirror page that the CPU touched lately comes back with its bytes at each submit.**

- Without the rule: the page was published again at each submit and landed at each CPU touch. One game's CPU writes
  pages beside the results of its programs each frame. That was about 180 landings a second, each a copy and a wait
  for the GPU on the thread that touched the page.
- Comment: `mirror.c`, `vk_mirror_harvest`, and above `MIR_HOT_PRESENTS`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the case of a page touched lately.

**W22. The mirror pages that the CPU did not touch stay GPU owned and do not land at each submit.**

- Without the rule: a landing of each written page at every submit took most of the GPU thread in one game's menus,
  for pages that the CPU mostly never read.
- Comment: `mirror.c`, above `own_entries`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the case of a page that is not landed.

**W23. The run ends of a mirror page mask are found a word at a time.**

- Without the rule: found a bit at a time, the runs of the 24,000 pages that one game's upscaler writes in a frame
  took about 30 ms of the 51 ms of their landing. A GPU with no memory traps lands every page.
- Comment: `mirror.c`, above `run_end`.
- Check: no test covers the cost.

## Staging and rings

**S1. The rings, the descriptor pools and the dead list of a frame are not used again before its last submission
finished.**

- Without the rule: an end of the frame at each submit would reset them in the middle of a draw and wait for the
  GPU at every submit.
- Comment: `cmd.c`, the file header.
- Check: `check_chunk_after_done` and `check_staging_overflow` in `tests/vulkan_headless.c`.

**S2. The rings take chunks of one size from a pool, and the pool keeps what the last frames used.**

- Without the rule: a ring that doubled up to the allocation limit stayed there for 300 frames. The pool keeps as
  many chunks and big buffers as the most that a frame of the last `VK_POOL_FRAMES` took. A request bigger than a
  chunk gets a buffer of the chunk size doubled until it fits.
- Comment: `cmd.c`, above `Chunk`, and `pool_recycle`.
- Check: `check_staging_burst`, `check_small_uploads`, `check_big_uploads` and `check_chunks_both_slots` in
  `tests/vulkan_headless.c`.

**S3. Of the pooled chunks, the ring takes first the one that the CPU wrote most.**

- Without the rule: the first CPU write to each page of a chunk is a page fault. An 8192x8192 texture's upload took
  43 ms in a new 32 MiB buffer, and 28 ms in a ring written before. An ASTC texture's 16 MiB copy source took 14 ms
  in a chunk written 64 KiB into, where its 8 MiB level took 0.3 ms in one written whole.
- Comment: `cmd.c`, above `pool_before`.
- Check: `check_chunk_written_first` and `check_big_reuse` in `tests/vulkan_headless.c`.

**S4. A ring with no pooled chunk left hands out of its smallest pooled big buffer.**

- Without the rule: a frame of more small requests than the pool kept chunks for wrote new chunks, with a page
  fault on each page.
- Comment: `cmd.c`, above `chunk_take`.
- Check: `check_chunks_from_big` in `tests/vulkan_headless.c`.

**S5. Streamed uploads and buffer snapshots go to host memory, not to the mapped window of device memory.**

- Without the rule: with no resizable BAR that window is a heap of its own, 246 MiB on an RTX 2080, which the whole
  desktop shares. Past its room the driver moves the memory, and every CPU write to it faulted in the kernel. One
  game ran at 10 frames a second for twenty seconds, its GPU thread half in the kernel.
- Comment: `cmd.c`, above `BAR_RING_MAX`, and above `vk_stage_upload`.
- Check: `check_guest_staging` in `tests/vulkan_guest_staging.inc`.

**S6. The room in the window comes from the budget in the way that the driver counts it.**

- Without the rule: the NVIDIA driver (615.78) counts the use of the whole system in the window. A process that
  allocated nothing is told 155 of 246 MiB used and 91 budgeted. Read as the use of this process, that looked past
  the budget all the time. Each frame's ring was freed and made again in the window, a 16 MiB allocation and free
  every frame, about a millisecond. A menu at 760 frames a second ran at 3200 without it. A chunk goes there only
  with room for it and a sixteenth of the heap to spare.
- Comment: `cmd.c`, above `BAR_RING_MAX` (the code is `bar_room`).
- Check: no test covers `bar_room`. The ctest `vk_memory_model` covers where the memory of a device is.

**S7. A ring takes at most 32 MiB of the window, and the process at most a quarter of it.**

- Without the rule: within the budget, the NVIDIA driver failed to map a 64 MiB ring with 86 MiB budgeted and
  reported success. CPU writes to it ran at 18 MB/s against 5 GB/s. Every frame of that slot took half a second, in
  two games, in most runs. No 16 or 32 MiB ring failed. With resizable BAR too, the rings grew to 256 MiB there,
  and one game's draws took up to 1.5 s each to write their data.
- Comment: `cmd.c`, above `BAR_RING_MAX`, and `bar_room`.
- Check: no test covers it. The staging statistics of `vk_buffer_stats` give the bytes by memory kind.

**S8. Guest input that a copy or a conversion reads one time stays near the GPU when the GPU owns it.**

- Without the rule: CPU-only bytes that go through the device window are slow CPU writes, and GPU-owned bytes that
  go through host memory cross the bus. Both rings share the fence of the frame.
- Comment: `cmd.c`, above `vk_stage_guest_input`.
- Check: `check_guest_staging` in `tests/vulkan_guest_staging.inc`.

**S9. The storage results of a frame are copied on the GPU into host-cached memory before the CPU reads them.**

- Without the rule: the staging ring can be device memory behind the BAR, where every CPU read crosses PCIe
  uncached. The compare of a frame's results there cost one game most of its GPU thread. For logged results the
  reads of the logged runs there were 19% of a Switch game's GPU thread, with 728 results a frame of 3.8 KB on
  average. Consecutive results share one copy command.
- Comment: `cmd.c`, above `vk_stage_back`, and `buffer.c`, `vk_buf_host_barrier`.
- Check: no test holds the memory kind. `check_storage_logged` covers the bytes.

**S10. What the GPU makes for itself to read goes in the device ring of the frame.**

- Without the rule: in host memory each byte crossed PCIe two times or more. Composed storage ranges were about
  45 MB a frame in one game, most of its traffic from the GPU to the host.
- Comment: `cmd.c`, above `vk_stage_device`. `buffer.c`, above `gfx_composed`, and `buf_get_owned`.
- Check: `check_guest_staging` in `tests/vulkan_guest_staging.inc`.

**S11. A read of guest memory into device memory copies the owned bytes on the GPU and takes staging only from the
first CPU byte.**

- Without the rule: the GPU copied its own results and render targets out to host memory and read them back, about
  34 MB a frame in one game's menu. In a texture test 60% of the destinations of the copy engine were held whole by
  owners. The staging chunks that they took and never wrote cost a page fault for each page later, 7 ms for an
  8 MiB copy source.
- Comment: `owned.c`, above `DevRead`.
- Check: `check_owned_read_device` in `tests/vulkan_owned_readback.inc`.

**S12. The span of a published storage image is staged straight into the buffer that becomes its owner.**

- Without the rule: staged in host memory, the span went out and came back several times, 12 to 14 MB a frame in
  one game.
- Comment: `surface.c`, `vk_surf_publish`.
- Check: `check_storage_image_keep` covers the publish. No test holds the bytes that cross the bus.

**S13. The original bytes of an atlas page are read from guest memory, not from staging.**

- Without the rule: staging can be device memory behind the BAR. That copy was most of the `memcpy` time of the GPU
  thread in one game.
- Comment: `buffer.c`, `draw_output_page`.
- Check: no test covers it.

**S14. Each submit makes the host writes to the rings visible to the GPU.**

- Without the rule: staging memory is not always host coherent, and the GPU reads stale bytes. The compute arena
  has the same rule.
- Comment: `cmd.c`, `rings_flush` and `submit`. `compute.c`, `vk_dispatch`.
- Check: no test runs on memory that is not coherent.

**S15. The upload command buffer runs first in its submission, so its copies do not end a rendering pass.**

- Without the rule: each fill of a new buffer ended the pass. A copy goes there only when nothing before it reads
  the destination. A read of owned bytes goes there only when all of them were written before this submission.
- Comment: `cmd.c`, above `vk_cmd_upload`, and `owned.c`, above `read_settled`.
- Check: `check_pass_breaks` in `tests/vulkan_pass_breaks.inc`.

**S16. A resource that is deferred for destroy waits for the frame that is recorded from now on.**

- Without the rule: after a frame advance the current slot still holds the work of an older frame. A resource
  pushed there went with that older frame while newer work could still use it. The pooled buffer of an owner
  returns at the recycle of the frame for the same reason.
- Comment: `cmd.c`, above `vk_defer_image`. `owned.c`, `res_release`. `surface.c`, above `surf_free`.
- Check: no test names `vk_defer_image`. `check_chunk_after_done` covers the same rule for chunks.

**S17. A compute arena that draws read waits for the frame of those draws, and the arena grows only between
batches.**

- Without the rule: the draws can still run after the batch landed, and within a batch the recorded commands point
  into the arena.
- Comment: `compute.c`, above `cs_arena_done`, and above `g_ar_want`.
- Check: no test covers it.

## Memory limits and the trim

**M1. A frame with no present ends when it wrote `VK_FRAME_RING_CAPS` allocations to its rings.**

- Without the rule: the chunks and big buffers of a frame come back only with its slot. A game's load that drew
  with no present for 8 s kept every ring that it made. GPU memory went from 270 to 1062 MiB, and a phone's system
  ended the app at its 4 GiB limit.
- Comment: `cmd.c`, above `frame_rings` (the code is `frame_check`).
- Check: `check_rings_without_present` in `tests/vulkan_headless.c`.

**M2. The bands of a copy that is too big for one allocation also run the frame check.**

- Without the rule: nothing is set up between two bands, so no draw ended the frame. The upload of the eight levels
  of an 8192x8192 RGBA8 texture wrote 1365 MiB to the rings of one frame.
- Comment: `cmd.c`, above `frame_rings`.
- Check: `check_dma_bands_frames` in `tests/vulkan_headless.c`.

**M3. On a GPU that shares the memory of the system, a frame ends at an eighth of the budget in its rings.**

- Without the rule: the rings are memory that the caches budget there. The limit is never less than two chunks, and
  the code asks for the budget only past those two.
- Comment: `cmd.c`, above `frame_ring_cap`.
- Check: `check_rings_budget_share` in `tests/vulkan_headless.c`, and the ctest `vk_uma_budget`.

**M4. The frame check does not end a fresh frame.**

- Without the rule: a fresh frame recorded nothing, and its counts are still those of its last use. An end would
  only move on to the slot whose work was just submitted, and wait for it.
- Comment: `cmd.c`, `frame_check`.
- Check: `check_fresh_frame_kept` in `tests/vulkan_headless.c`.

**M5. A frame with many submissions, descriptor pools or descriptor tables ends at the start of a draw.**

- Without the rule: a loading screen with no present never hands its pools and rings back. The limit on submissions
  leaves room for the submits of W3. A frame that reaches it records the rest in the other slot.
- Comment: `cmd.c`, above `frame_rings`.
- Check: `check_descriptor_room` in `tests/vulkan_headless.c`.

**M6. A source of the copy engine past a sixty-fourth of the budget does not take a buffer of its own at once.**

- Without the rule: the upload of a texture reads its source one time. The levels of an 8192x8192 RGBA8 texture
  took 760 MiB of own buffers that nothing read again, where a phone had 1 GB available. Such a source takes a
  buffer when it stays the same, as each other range does.
- Comment: `buffer.c`, `buf_get`, at `at_once`, and `copy_src_at_once`.
- Check: `check_dma_source_kept_when_stable` in `tests/vulkan_headless.c`.

**M7. The readback buffer grows to the biggest readback and shrinks again after many small ones.**

- Without the rule: the comment gives no failure. A readback past the allocation cap is read back in parts.
- Comment: `cmd.c`, `vk_readback_buffer`.
- Check: `check_flush_parts` in `tests/vulkan_headless.c` covers a readback in parts. No test covers the shrink.

**M8. The freed buffers of owners go to a pool of at most 512 entries.**

- Without the rule: each published storage image made one buffer with an allocation of its own, about 24 a frame
  in an Ounce game's race. A publish now takes the nearest freed buffer of at least its size.
- Comment: `owned.c`, above `OWN_POOL_MAX`.
- Check: no test covers the pool.

**M9. A write log that is free for 600 presents goes.**

- Without the rule: the pool kept what the dispatches in flight took at their peak. One game peaked at 290 MB of
  host memory for them while it loaded.
- Comment: `writelog.c`, above `WLOG_IDLE` (the code is `vk_wlog_trim`).
- Check: no test covers it. `vk_wlog_stats` gives the bytes.

**M10. The room of the mirror harvest follows what the last submits wrote.**

- Without the rule: every page of room takes 4.5 KiB of the readback ring at each submit that wrote a page. Kept at
  the most that it ever was, one game's harvests took 2.6 GB of the ring every 10 s for a few pages each. A page
  past the room takes a pass of its own that waits for the GPU, and the room grows at once.
- Comment: `mirror.c`, above `MIR_ROOM_MIN`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the case of more pages than the room. `vk_mirror_stats`
  gives the room.

**M11. A device whose `maxBufferSize` is below the address space makes the mirror only with the setting, and never
under the validation layer.**

- Without the rule: a buffer past the limit is invalid, though RADV makes it and runs it sparse. With no mirror the
  results land through the CPU. The report of the validation layer would be above the errors that it must show.
- Comment: `mirror.c`, `vk_mirror_on`.
- Check: no test covers it.

**M12. A surface whose pixels can be had again goes back from the aged list to the first list.**

- Without the rule: an eviction that takes only lossless surfaces would not find it, and the idle eviction would
  keep it for five minutes.
- Comment: `surface.c`, above `vk_surf_relist`.
- Check: no test covers it.

**M13. An evicted render target is not written to guest memory.**

- Without the rule: memory is shared by targets that the GPU drew over one another, and by textures and buffers
  that copied from them. The renderer does not know them all. A write of an evicted target's pixels, which a game's
  Mii icons wanted, put older pixels over newer ones in another target. The result was a black block over the
  player character, and red and green blocks over its textures. A surface with a storage image result still to land
  is passed over.
- Comment: `surface.c`, `surf_reclaim`.
- Check: no test covers it.

**M14. A target that memory does not hold goes only under budget pressure when nothing lossless is left, or after
minutes with no use.**

- Without the rule: its pixels are lost. A target that holds a draw which a later texture at its address would take
  goes only under budget pressure. Age does not make guest memory the truth, because a generated texture can be
  sampled first several frames after its draw.
- Comment: `surface.c`, `surf_reclaim`, above `keeps_drawn`, above `lossless`, and `vk_rt_drawn_in`.
- Check: no test covers the lossy steps.

**M15. A target that later targets drew all over goes first, with no loss.**

- Without the rule: a game with dynamic resolution makes its scene targets again at another size each frame over
  the same memory. Each old one held a draw that memory never got, so it was kept as precious. A few thousand took
  the budget from the targets that are. One game's ink maps, drawn one time at the start of a level, went under
  that pressure with a lowered budget, and its walls took no ink.
- Comment: `surface.c`, above `rt_superseded`.
- Check: no test covers it.

**M16. The trim frees the working set only to stay half a head under the budget, and only what is unused for
eight presents.**

- A head is a sixteenth of the budget, 128 MiB at least and 256 MiB at most (`trim_head`). The trim starts a head
  under the budget. What is unused for a second goes down to two heads under it. Lossy surfaces go only within a
  head and within half a head of it.
- Without the rule: a trim of the working set to 80% made one game load hundreds of textures and targets again
  every 600 presents under a lowered budget. With two presents the trim took textures that a game uses every few
  frames. Of 3,000 that it evicted in one game's menus, 2,358 were unused for three presents, and each came back at
  once with a BC7 encode.
- The marks were shares of the budget before: a start at 90% and a stop at 80%. Each trim then emptied a tenth of
  the GPU's memory. In one game's race, with the budget set to 4200 MB, the fixed head and M16a together took the
  textures made again from 3.4 to 2.5 for each present (three runs of each, 300 s).
- Comment: `surface.c`, `vk_mem_trim` and `trim_head`.
- Check: `check_trim_working_set` in `tests/vulkan_trim.inc`.

**M16a. A texture whose texture object the game ended goes before any texture that the game still has.**

- The front end counts the live texture objects at each address (`gpu/guest_textures.h`). When the last object at
  an address ends, the cache puts the textures at that address on a list (`ended_collect`).
- A trim takes the textures of the list first, when memory can give their pixels again and two presents passed.
- While four fifths of the budget are in use, such a texture also goes two seconds and 30 presents after the end
  of its object (`vk_surf_ended_frame`). With memory to spare it stays until it ages out. One game ends textures
  and makes them again at the same address some seconds later.
- A texture at whose address the game made an object again leaves the list.
- Comment: `surface.c`, `ended_reclaim` and `vk_surf_ended_frame`.
- Check: `check_trim_ended` in `tests/vulkan_trim.inc`, and `tests/guest_textures.c` for the list of ended addresses.

**M17. A forced trim runs at most one time in a present.**

- Without the rule: what a trim frees is released when the frame is done. A second trim in the same present would
  find the same shortfall and take more.
- Comment: `surface.c`, `vk_mem_trim`.
- Check: no test covers it.

**M18. An image bigger than the whole heap is never made, and an image as big as one that failed is not tried
again in the present.**

- Without the rule: a garbage size in a header emptied the caches for nothing. After a failure, what was freed is
  released only when the frame is done, so the same try fails again.
- Comment: `surface.c`, `vk_cache_image_create`.
- Check: no test covers it.

**M19. A scaled target past the allocation cap gets room under the budget.**

- Without the rule: a 4x scale of 4K is 506 MiB in RGBA8, and the create fails. The caller holds only surfaces that
  it used in this present, and those stay.
- Comment: `surface.c`, `surf_create_`.
- Check: no test covers it.

**M20. A trim that finds the images short of memory suggests `astc_mode=2` in the log, one time in a run.**

- Without the rule: a game on a GPU that samples BCn and not ASTC stays slow by eviction, and the log does not say
  why. The comment defines "short".
- Comment: `surface.c`, above `astc_bc_suggest`.
- Check: no test covers the message.

## Mappings and locks

**L1. A change of the GPU mappings lands the pending results first, under `vk_lock`, with the mappings that they
were made with.**

- Without the rule: two GPU ranges can name the same guest pages, and the page lists of a result are true only
  while the mappings stay. A compute result that overlaps the change only on unmapped GPU pages lands too.
- Comment: `buffer.c`, above `page_cmp`, and above `ChgRun`. `compute.c`, above `vk_compute_mem_need_map`.
- Check: `check_map_lands_covered` in `tests/vulkan_headless.c`, `check_mapping_landing` in
  `tests/vulkan_lock_waits.inc`, and the ctest `pending_pages`.

**L2. A change of guest memory that the renderer holds nothing of does not wait for `vk_lock`.**

- Without the rule: the comment gives no failure. A publish takes `pend_lock`, so that a change that skipped
  `vk_lock` finds no new owner on its pages before it ends.
- Comment: `owned.c`, `own_publish`.
- Check: `check_mapping_lock` in `tests/vulkan_lock_waits.inc`, and `check_map_waits` in `tests/vulkan_headless.c`.

**L3. The question "is a result pending here" is asked first with no lock.**

- Without the rule: the asker waited for a lock that another thread held, mostly to find nothing. The comment of
  the test says that a dispatch asked this of each of its buffers. Results are added on the GPU thread, so a miss
  with no lock is safe. A hit is asked again with the lock.
- Comment: `buffer.c`, `mem_need_reason`.
- Check: `check_need_unlocked` in `tests/vulkan_storage_logged.inc`.

**L4. The thread that lands passes the lookups, and other readers wait under `vk_lock` for the landing.**

- Without the rule: the landing thread would look up the results that it lands. A reader on another thread must not
  see memory before the results are in it. The count of pending compute outputs goes to zero only after that.
- Comment: `buffer.c`, the member `out_landing` of `t_buffer`. `compute.c`, above `cs_mem_need`, and `cs_land`.
- Check: `tests/vulkan_release.c` covers the recursion. `check_landing_wait` covers a reader on another thread.

**L5. A landing does not start inside another landing.**

- Without the rule: the read of guest memory by a landing can finish the work of the GPU. That landed the results
  of the next submissions inside this one, before the older results. The inner landing rewrote the runs that the
  outer one wrote from. A run that ended before it began made a write stamp of 2^64 bytes. The walk of the GPU
  address space page by page took minutes under `vk_lock`.
- Comment: `cmd.c`, above `g_landing`, and `writeback.c`, `vk_write_runs`.
- Check: `tests/vulkan_release.c`, whose header names the recursion. No test makes the 2^64 stamp.

**L6. A result can flush a render target that overlaps it while it lands.**

- Without the rule: the comment gives no failure. The depth of results is a counter for each thread, not a flag.
- Comment: `writeback.c`, above `t_result_depth`.
- Check: `tests/vulkan_release.c`.

**L7. Nothing records into the frame's command buffer inside a landing, a recycle or a submission.**

- Without the rule: the mirror took its pages in inside a landing, which can run where a command buffer is begun.
  The pool reset dropped the copies, and the run crashed in the driver. The pass for pages past the harvest room
  took its sets from the frame, which started the frame's command buffer inside a landing. That buffer outlived the
  present, the next frame recorded into it, and the run crashed in the driver. The mirror now publishes at a point
  where nothing is being recorded, and that pass has a pool of its own.
- Comment: `cmd.c`, above `g_rec_unsafe`. `mirror.c`, above `own_entries`, and `harvest_left`.
- Check: `check_mirror` in `tests/vulkan_mirror.inc`, the case "not published inside the landing".

**L8. The mirror takes a batch out of its list before it takes the batch in.**

- Without the rule: a publish of a page can write a render target back, which can finish the commands and land
  through the mirror again. The inner call would take the same batch.
- Comment: `mirror.c`, `vk_mirror_done`, and above `vk_mirror_publish`.
- Check: no test covers the nested call.

**L9. The GPU thread waits for its next frame slot with no lock held.**

- Without the rule: it waited in the recycle with `vk_lock` held, up to 3.1 s at a time in one game's menus. The
  game threads that landed results, changed memory or performed releases waited behind it.
- Comment: `cmd.c`, above `g_recycle_at`.
- Check: `check_frame_ready` in `tests/vulkan_lock_waits.inc`.

**L10. A landing waits for the GPU with no lock held, and then takes `vk_lock`.**

- Without the rule: the wait held `vk_lock` while the recording thread waited for a pipeline of a draw. When the
  work is done on the GPU already, the copy waits for nothing of the recording thread.
- Comment: `owned.c`, `own_land`, and above `own_wait`.
- Check: `check_landing_wait` in `tests/vulkan_lock_waits.inc`.

**L11. A landing uses a command pool and a host buffer of its own, apart from the pools and rings of the frames.**

- Without the rule: the result callbacks of a frame can acquire the owner that is being read back. The readback
  flushes only an open command buffer. A start or a recycle of a frame there could acquire the same ranges and wait
  for its own thread.
- Comment: `owned.c`, the member `pool` of `g_owned`, and `own_land`. `cmd.c`, above `vk_submit_for_readback`.
- Check: `check_owned_readback` in `tests/vulkan_owned_readback.inc`.

**L12. The hooks of the ownership layer call `vk_lock` and `vk_unlock`, not the lock object.**

- Without the rule: the outermost unlock also signals the events that the releases under the hold reached. A direct
  unlock skips them.
- Comment: `owned.c`, `vk_own_init`.
- Check: `tests/vulkan_release.c` checks the signaled events and the lock depth.

**L13. The submission stamp of an owner is atomic.**

- Without the rule: the ownership layer reads it on a thread with no `vk_lock`, before the landing takes the lock.
- Comment: `owned.c`, `vk_own_res_wrote` and `own_writer`.
- Check: `check_owned_in_place_write` in `tests/vulkan_owned_readback.inc`.

**L14. The host address of an entry is kept with the generation of the GPU mappings.**

- Without the rule: a buffer of its own translated its range each time it was checked, 1.6% of one game's GPU
  thread. A change of the mappings moves the generation, and the windows of K5 go.
- Comment: `buffer.c`, above `entry_host`.
- Check: no test covers it.

**L15. The marks at the other addresses of a page are made after the scan locks are released.**

- Without the rule: the comment gives the lock order and no failure. The lock of the guest memory layer comes
  before the scan locks, because a landing under it notes its pages.
- Comment: `buffer.c`, above `ww_take_peers`.
- Check: `check_wt_alias` runs the path. No test forces the lock order.

**L16. The pending targets at the aliases of a range are collected under the alias lock and flushed after it.**

- Without the rule: a flush writes guest memory and asks the alias index about those writes itself.
- Comment: `surface.c`, above `rt_pending_alias`.
- Check: `check_alias_newer_target` in `tests/vulkan_headless.c` runs the path.

## Comments that this list leaves out

These comments are in the same files, but they are about code that the rewrite does not replace:

- The translation and the keys of compute programs in `compute.c`: the code windows, shared memory sizes, subgroup
  sizes, pointer table windows, numeric types of images and the pipeline file.
- The formats, the scale blits, the depth blits and resolves, the sRGB views and the multisample paths in
  `surface.c`.
- The lookup tables of the live and drawn render targets in `surface.c` (`RtSlot`, `g_rts`, `g_drawn`,
  `g_rt_pages`, `g_drawn_pages`, `vk_rt_for_cpu`) and the take of pixels from a parked target (`rt_take_pixels`).
- The depth targets of a save state in `surface.c`.
- The descriptor pools and the profiler zones in `cmd.c` and `compute.c`.

If a later step of #1714 moves one of these, read its comments then.

## State of the rewrite

The plan of issue #1714 has six steps, and step 1 is the gate for the others: a step goes ahead only when the
measurement shows the problem that it removes. This is what each step has in the code, and what stays open with the
number that decided it. The numbers are from an RTX 2080 with one Switch game in its city and one Ounce game in a
race, in November 2026.

| Step | In the code | Open |
| --- | --- | --- |
| 1. Measure | The byte ledger (`ledger.c`), the texture census (`tex_census.c`, `tex_report.c`), the statistics of the page cache, the rings and the allocator, and this list. | A Tracy capture of the slow frames of the Switch game. It needs a machine with no other load. |
| 2. Cap the pools | A trim of the kept results (`owned.c`), of the mirror's granules (`mirror.c`) and of the write logs (`writelog.c`) when memory is short. A limit of bytes and of age on the retired compute arenas (`compute.c`). | Nothing that the ledger shows. |
| 3. Texture lifetime and target | M16 and M16a: the textures that the game ended go first, by the end of their texture object or, with no texture objects, by their unmap. The trim keeps a fixed head below the budget. Sparse blocks that show the same memory share an allocation. | A target taken from the bytes used over the last seconds. The Switch game loads textures again when they go early with memory to spare, so the trim acts only near the budget. A smaller host format for ASTC is separate work. |
| 4. Page-keyed read cache | `pagespace.c`, on by default, with sparse buffers or with slabs where the device has none (`vk_buffer_slabs=1` forces slabs). | The windows and the buffers for one range stay. A host with no write tracking and a device that shares memory with the CPU still use them. |
| 5. Stream rings | `vk_stream_upload` in `cmd.c` carries the bytes that work recorded at once reads, in chunks that no frame owns (`vk_stream_ring=0` gives the frame's rings). | The other rings keep their frame slot: a draw's data, the results that the CPU reads back, and the bytes that a holder keeps past a submission. Each such holder would have to stamp its region at each use. The rings hold 128 to 176 MiB at the median in the two games. |
| 6. Results onto the pages | A draw writes in place in the page cache with `vk_buffer_pages=2`. | The other kinds of result, and the removal of the paths that compose them. The gate is a wait in a draw for a result, and the two games show none: 0 waits in 600 presents. The write in place kept 1,200 to 1,800 of 180,000 bindings in the Switch game, which writes those pages with its CPU in each frame, and the Ounce game does not reach it. |

Two costs that the plan lists show in the Ounce game's race and have no step that removes them yet. Its draws whose
storage bindings share a page go through a page atlas: 64 draws a present, with 5.6 MB copied before the draws and
2.8 MB read back. Its results that land by a compare took 20 MB of compares a present, of which 0.1% changed. Its
GPU heap is full in the race, so the trim and the allocator work at the budget whatever the caches do.
