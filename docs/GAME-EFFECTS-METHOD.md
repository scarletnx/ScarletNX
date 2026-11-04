# How the effects were checked on the games

## Environment

- Build: branch `greatgreenstar/feat/hdr-780`, based on PR #1104.
- Runtime: `build/scarletnx`, deployed to every `recompiled/<id>/<id>` by the build.
- Display: a private headless KWin Wayland session at 1920x1080, with Xwayland, and `SDL_VIDEODRIVER=x11`.
  No game ran on the user's desktop.
- GPU: AMD Radeon RX 9060 XT, RADV, Vulkan 1.3.

## Commands

For one game:

```bash
cd ~/.local/share/scarletnx/recompiled/<id>
DISPLAY=:2 XDG_RUNTIME_DIR=/tmp/opencode/rt SDL_VIDEODRIVER=x11 \
  ./<id> --seconds=5 --shot=/tmp/<id> \
  --aa=fxaa --upscaler=fsr1 --hdr=auto
```

The sweep runs the same command for every folder:

```bash
scripts/effects-sweep.sh --quick
SNX_SWEEP_SETTINGS='--aa=off --upscaler=off --hdr=off' scripts/effects-sweep.sh --quick
```

## What the table records

- **Exit**: 0 is a clean timed run. 124 is the timeout, which means the game did not finish its shader warmup or
  did not present.
- **Frames**: the `frames=N` line of a clean run.
- **Effects**: `presentation effects ready` in the log, one for the first frame that ran the shared pass.
- **Validation**: `Validation Error` lines under `--vk_validation=1`. The sweep uses the normal run; the modes
  matrix uses validation.
- **Device lost**: `device was lost` lines.
- **Screenshot**: the runtime's own `--shot` PPM, the native frame and not the window.

## Baseline

The same sweep with `--aa=off --upscaler=off --hdr=off` is the control. A game that fails in both sweeps does not
work with this runtime yet. A game that passes the baseline and fails with the effects is a regression in the
effects and is fixed before the table is accepted.

## Limits

`--shot` writes the native frame, so it proves the game rendered, not the upscaled, anti aliased or HDR window. The
window proof is the `presentation effects ready` line, the frame count and the validation count, plus the synthetic
`present_fx` panels and the GBA/3DS presenter tests. A real window capture is added where the headless compositor
allows one.
