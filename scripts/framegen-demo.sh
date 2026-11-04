#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# A slow motion comparison of frame interpolation Off against Auto (or Blend), from the same save state, side by
# side. Both runs are unattended (--seconds), the state loads at 3 s (state_load), and the screen is recorded through
# X11/Xwayland at the display's own rate, then the two are slowed four times and stacked.
#
# Usage: scripts/framegen-demo.sh GAME [SECONDS] [STATE_SLOT] [OUT_DIR]
#   GAME        the game's folder (a recompiled title), or a ROM for the AGB frontend
#   SECONDS     how long each run lasts (default 15)
#   STATE_SLOT  the save state slot to load, empty for none (default 1; the Switch runtime only)
#   OUT_DIR     where the videos go (default framegen-demo)
#
# FRONTEND=build/scarletnx-switch by default; set it for another. MODE=blend for a display that is not twice the
# game's rate (Auto would stay Off there). LOAD_MS is when the state loads, 3000 by default: late enough for the
# game to boot, early enough to leave most of the run in the state's scene.
# Needs DISPLAY (X11 or Xwayland) with a window manager for fullscreen, xrandr and ffmpeg with libx264.
set -eu

GAME=${1:?usage: framegen-demo.sh GAME [SECONDS] [STATE_SLOT] [OUT_DIR]}
SECONDS=${2:-15}
SLOT=${3-1}
OUT=${4:-framegen-demo}
FRONTEND=${FRONTEND:-build/scarletnx-switch}
MODE=${MODE:-auto}
LOAD_MS=${LOAD_MS:-3000}
: "${DISPLAY:?needs an X11 display; run with SDL_VIDEODRIVER=x11}"
command -v ffmpeg >/dev/null || { echo "framegen-demo: needs ffmpeg" >&2; exit 1; }

mode_line=$(xrandr --current 2>/dev/null | awk '/\*/ { print; exit }')
SIZE=$(printf '%s\n' "$mode_line" | awk '{ print $1 }')
RATE=$(printf '%s\n' "$mode_line" | awk '{ gsub(/[^0-9.]/, "", $2); print $2 }')
[ -n "$SIZE" ] && [ -n "$RATE" ] || { echo "framegen-demo: cannot read the display's mode from xrandr" >&2; exit 1; }
mkdir -p "$OUT"

run() {
    name=$1
    interp=$2
    extra=""
    [ -n "$SLOT" ] && extra="--state_load=$LOAD_MS:$SLOT"
    # shellcheck disable=SC2086
    SDL_VIDEODRIVER=x11 "$FRONTEND" "$GAME" --fullscreen=1 --seconds="$SECONDS" --frame_interp="$interp" $extra \
        >"$OUT/$name.log" 2>&1 &
    game=$!
    ffmpeg -hide_banner -loglevel error -f x11grab -framerate "$RATE" -video_size "$SIZE" -i "$DISPLAY" \
        -t "$SECONDS" -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p "$OUT/$name.mp4" &
    shot=$!
    wait "$game" || true
    wait "$shot" || true
}

echo "framegen-demo: $SIZE at ${RATE} Hz, $SECONDS s each, state slot ${SLOT:-none}, $MODE against off"
run off off
run "$MODE" "$MODE"

# the first seconds are the boot and the load: keep from 5 s on, slow four times, and stack the two with their names
filter="[0:v]trim=start=5,setpts=4*(PTS-STARTPTS),drawtext=text='frame_interp=off':x=24:y=24:fontsize=40:fontcolor=white:box=1:boxcolor=black@0.6[a];"
filter="${filter}[1:v]trim=start=5,setpts=4*(PTS-STARTPTS),drawtext=text='frame_interp=$MODE':x=24:y=24:fontsize=40:fontcolor=white:box=1:boxcolor=black@0.6[b];"
filter="${filter}[a][b]hstack=inputs=2[v]"
ffmpeg -hide_banner -loglevel error -i "$OUT/off.mp4" -i "$OUT/$MODE.mp4" -filter_complex "$filter" -map "[v]" \
    -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p "$OUT/compare-slowmo.mp4"
echo "framegen-demo: $OUT/compare-slowmo.mp4"
