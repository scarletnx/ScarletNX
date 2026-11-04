# Remaining work, in order

This file is the checklist that the continuation watcher reads. A checked item is done and verified. Remove the
sentinel `docs/.work-complete` only when every box is checked. Do not push to any remote.

- [x] **Switch anti aliasing.** Give `SnxFx` a recording bridge so the Switch presenter can begin the frame's command
      buffer through `src/nx/gpu/vk/record.c`. Run the AA pass after `vk_upscale_process` and before `vk_hdr_compose`.
      Test a game at resolution 0.5 with FSR 1 and FXAA, SMAA and SSAA 2x under `--vk_validation=1`. See
      `docs/issues/switch-aa-frame-interpolation.md`.
- [ ] **Switch frame interpolation.** Keep the previous presented image and present the blend through the HDR/blit
      path, with two swapchain images per game frame. Use `auto` at 1.9x the measured game rate.
- [x] **3DS frame interpolation.** Per window, keep the previous composited pair and present the blend before the
      current pair. Use the shared blend pass. Check `3ds_windows` and a real run.
- [x] **Vita frame interpolation.** Same two-image present on the single Vita window. Check with the synthetic
      presenter path.
- [x] **NIS and SGSR on 3DS, Vita and GBA.** The shared filter picker currently falls back to FSR 1. Port the NIS and
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

## Phase 2, after the feature work and its full verification

The feature branch is documented in `docs/REMAINING.md`, `docs/issues/` and the demo folder. When every feature
box above is checked, the work continues on the repository's open issues, off the latest `origin/main`, one branch
per issue, easiest first. `docs/SIDE_ISSUES.md` is the ledger.

- [ ] Fetch `origin/main` and record its commit in `docs/SIDE_ISSUES.md`.
- [ ] List every open issue and rank it: easy, medium, hard, or blocked on hardware, keys, a game or another issue.
- [ ] For each issue that can be fixed on this device, make `fix/issue-<number>-<short>` from that `origin/main`
      commit, fix it, build, test, and commit it signed. Document the mechanism, files, tests, run commands, known
      limits, a suggested PR title and a suggested PR body in `docs/SIDE_ISSUES.md`.
- [ ] For each issue that cannot be fixed here, write why in `docs/SIDE_ISSUES.md` and leave it for the user.
- [ ] Never push a branch. The user does the PR work later, possibly with other agents, from the ledger.
- [ ] Re-read the open issues after the last fix. If a new easy issue appeared, add it and continue.
- [ ] Only when every open issue is either fixed on a branch or documented as blocked, create `docs/.work-complete`
      with the feature branch commit and the list of side branches.

## Proof across the games

- [x] Run every recompiled game with the effects off and with `--aa=fxaa --upscaler=fsr1 --hdr=auto`, and record the
      exit code, frames, validation count and screenshot in `docs/GAME-EFFECTS-PROOF.md`.
- [x] Run one representative game per platform through the modes: FSR 1, NIS, SGSR; AA off, Auto, FXAA, SMAA,
      SSAA 2x; HDR off, Auto, On. Record the command, log and screenshot.
- [x] Run the GBA game with `frame_interp=off|blend|auto` and record the log and frame counts.
- [x] Run the 3DS presenter test (`3ds_windows`) with the effects on, one window and separate, and the Vulkan draw
      validation.
- [x] Run the Vita synthetic effects test (`present_fx`) and a Vita homebrew if one is ever on the machine.
- [x] Put the logs, screenshots and a README with the exact commands in the demo folder.
