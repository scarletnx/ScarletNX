# Remaining work, in order

This file is the checklist that the continuation watcher reads. A checked item is done and verified. Remove the
sentinel `docs/.work-complete` only when every box is checked. Do not push to any remote.

- [ ] **Switch anti aliasing.** Give `SnxFx` a recording bridge so the Switch presenter can begin the frame's command
      buffer through `src/nx/gpu/vk/record.c`. Run the AA pass after `vk_upscale_process` and before `vk_hdr_compose`.
      Test a game at resolution 0.5 with FSR 1 and FXAA, SMAA and SSAA 2x under `--vk_validation=1`. See
      `docs/issues/switch-aa-frame-interpolation.md`.
- [ ] **Switch frame interpolation.** Keep the previous presented image and present the blend through the HDR/blit
      path, with two swapchain images per game frame. Use `auto` at 1.9x the measured game rate.
- [ ] **3DS frame interpolation.** Per window, keep the previous composited pair and present the blend before the
      current pair. Use the shared blend pass. Check `3ds_windows` and a real run.
- [ ] **Vita frame interpolation.** Same two-image present on the single Vita window. Check with the synthetic
      presenter path.
- [ ] **NIS and SGSR on 3DS, Vita and GBA.** The shared filter picker currently falls back to FSR 1. Port the NIS and
      SGSR pipelines into `present_fx` with the same staging and coefficient upload the Switch presenter uses.
- [ ] **3DS PICA shadow rendering.** Implement register `0x22f` and the shadow path so a retail title reaches the
      presenter. See `docs/issues/3ds-pica-shadow-rendering.md`.
- [ ] **Full verification.** `ctest` for every non-device test, the three validation runs, a headless GBA run with
      Blend and Auto, and a Switch run with AA under `--vk_validation=1`. Update the demo folder, panels and patches.
- [ ] **Docs and issues.** Every fixed issue gets a status section. Update `docs/SETTINGS.md`, `docs/USAGE.md`,
      `docs/3DS.md`, `docs/VITA.md`, `docs/AGB.md`, `README`, and the demo folder.

## Rules for the continuation

- Read `CLAUDE.md` and the skills `snx-build`, `snx-run`, `snx-code-quality`, `snx-gpu-shader` and `ste100` first.
- Work in `/home/greenstar/.local/share/opencode/worktree/515586/shiny-comet` on branch `greatgreenstar/feat/hdr-780`.
- Commit every verified step, signed with the SSH key. Never force-push and never push.
- Test with the headless private KWin session or `-H`. Do not use the user's desktop.
- If a test fails, find the mechanism before changing code. Record anything that cannot be finished in `docs/issues/`.
- When every box is checked and the full verification passed, run the demo update, then create
  `docs/.work-complete` with the date and the commit hash.
