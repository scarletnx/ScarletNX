#!/usr/bin/env bash
# Run every recompiled game with the presentation effects and keep the proof:
# the log, the frames line, the screenshot and any validation or device loss.
#
# Usage:
#   scripts/effects-sweep.sh [--quick|--full]
#
# The games are in the user's data folder. The script never builds, never pushes
# and never changes a game's .ini. Run it from a checkout with a deployed runtime,
# on a private display (DISPLAY or WAYLAND_DISPLAY set by the caller).
set -u

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
games="${SNX_GAMES:-$HOME/.local/share/scarletnx/recompiled}"
out="${SNX_SWEEP_OUT:-$HOME/Downloads/compiled_scarlet/Hotkey_stuff_forALL/games}"
mode="${1:---quick}"
seconds="${SNX_SWEEP_SECONDS:-5}"
display="${DISPLAY:-:2}"
mkdir -p "$out"

settings="--aa=fxaa --upscaler=fsr1 --hdr=auto"
if [ "$mode" = --full ]; then
    settings="--aa=auto --upscaler=fsr1 --hdr=auto"
fi

printf '# Effects sweep\n\n'
printf 'Mode: `%s`, seconds: `%s`, display: `%s`, games: `%s`\n\n' "$mode" "$seconds" "$display" "$games"
printf '| Game | System | Exit | Frames | Effects | Validation | Device lost | Log | Screenshot |\n'
printf '| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n'

run_one() {
    local id="$1" system="$2" exe="$3" dir="$4"
    local log="$out/$id.log" shot="$out/$id"
    local start end rc frames effects validation lost
    : >"$log"
    if [ "$system" = 3ds ]; then
        (
            cd "$dir" || exit 1
            DISPLAY="$display" timeout $((seconds + 90)) "./$exe" --seconds="$seconds" $settings
        ) >>"$log" 2>&1
    else
        (
            cd "$dir" || exit 1
            DISPLAY="$display" timeout $((seconds + 90)) "./$exe" --seconds="$seconds" --shot="$shot" $settings
        ) >>"$log" 2>&1
    fi
    rc=$?
    frames="$(grep -oE 'frames=[0-9]+' "$log" | tail -1)"
    effects="$(grep -c 'presentation effects ready' "$log")"
    validation="$(grep -c 'Validation Error' "$log")"
    lost="$(grep -c 'device was lost' "$log")"
    [ -f "$shot" ] && mv "$shot" "$out/$id.ppm" 2>/dev/null || true
    printf '| %s | %s | %s | %s | %s | %s | %s | `%s` | `%s` |\n' "$id" "$system" "$rc" "${frames:-none}" \
        "$effects" "$validation" "$lost" "$log" "$out/$id.ppm"
}

for dir in "$games"/*/; do
    [ -d "$dir" ] || continue
    id="$(basename "$dir")"
    case "$id" in
        0004*) system=3ds ;;
        0100*) system=switch ;;
        *) system=gba ;;
    esac
    exe="$id"
    [ -x "$dir/$exe" ] || exe="$(find "$dir" -maxdepth 1 -type f -perm -u+x -printf '%f\n' | head -1)"
    [ -n "$exe" ] || continue
    if [ "$system" = 3ds ] || [ "$system" = gba ]; then
        # these drivers take --seconds and --shot too, and the 3DS dumps its frames
        run_one "$id" "$system" "$exe" "$dir"
    else
        run_one "$id" "$system" "$exe" "$dir"
    fi
done
