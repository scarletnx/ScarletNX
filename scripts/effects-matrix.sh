#!/usr/bin/env bash
# One game through the effects modes, with the log of each. Run on the private display.
set -u
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
id="${1:?usage: effects-matrix.sh GAME_ID [OUT_DIR]}"
out="${2:-$HOME/Downloads/compiled_scarlet/Hotkey_stuff_forALL/games/matrix-$id}"
dir="$HOME/.local/share/scarletnx/recompiled/$id"
display="${DISPLAY:-:2}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/opencode/rt}"
mkdir -p "$out"
run() {
    name="$1"; shift
    log="$out/$name.log"
    (cd "$dir" && DISPLAY="$display" timeout 180 "./$id" --seconds=4 --shot="$out/$name" "$@") >"$log" 2>&1
    printf '%-10s exit %-3s frames %-6s effects %-2s validation %-2s lost %s\n' "$name" "$?" \
        "$(grep -oE 'frames=[0-9]+' "$log" | tail -1 | cut -d= -f2)" \
        "$(grep -c 'presentation effects ready' "$log")" "$(grep -c 'Validation Error' "$log")" \
        "$(grep -c 'device was lost' "$log")"
}
for aa in off auto fxaa smaa ssaa2; do run "aa-$aa" --aa="$aa" --upscaler=fsr1 --hdr=off; done
for up in off bilinear fsr1 nis sgsr; do run "up-$up" --aa=off --upscaler="$up" --hdr=off; done
for hdr in off auto on; do run "hdr-$hdr" --aa=off --upscaler=fsr1 --hdr="$hdr"; done
run "hdr-on-validation" --aa=fxaa --upscaler=fsr1 --hdr=on --vk_validation=1
