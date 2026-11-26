#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The steps of build.yml's Arch Linux jobs (appimage, linux-sanitizers, linux-unicorn, qemu-reference), a function
# each, run as `scripts/ci/linux.sh STEP`; "env:" above one names what its step's env gives it
set -eo pipefail

packaging_check() {
  sha256sum -c <<'EOF'
0aea2f589658e5db1659a6ccb8d29546c4e03028f1e29b37c6450cece917249a  /usr/local/bin/quick-sharun
d215f7676785f1a1bb8784e69085d66c1df81488ef31f1ba7198325a5be05130  /usr/local/bin/get-debloated-pkgs
86afbf5045b78020c93fcd35bf0f642765a8dc339bb2dce534a0461ce3d3508d  /usr/local/bin/make-aur-package
EOF
}

# jdk21-openjdk: the Android app driver's javac and jni.h, and the JVM for Amberlith's APK; meson: dav1d; poppler:
# pdftotext for the x86-64 instruction selection. get-debloated-pkgs reads api.github.com, so it gets the token
# env: GITHUB_TOKEN
build_tools() {
  # poppler: pdftotext, which makes the x86-64 back end's instruction selection again (below)
  for try in 1 2 3; do
    pacman -Syu --noconfirm --needed cmake ninja meson make patch nasm perl python clang llvm sdl3 libx11 mesa vulkan-icd-loader ccache glslang jdk21-openjdk poppler && break
    [ "$try" = 3 ] && exit 1
    sleep 5
  done
  get-debloated-pkgs mesa-nano opus-mini icu-mini
}

# A release leaves out scarletnx --mcp's A64 disassembler (SNX_A64_DISASM), which is for working on the runtime,
# and the test programs, which the AppImage does not carry (half of this build's time). Each build
# step's compiler cache counts are its own: zeroed before, shown after. x86-64's nightlies and releases link with
# ThinLTO (cmake/lto.cmake), and its test tree, which is this folder, then tests that build; a pull request's do not
# env: ARCH LTO
release() {
  git config --global --add safe.directory '*'
  ccache --zero-stats
  cmake --preset default -DSNX_DEPLOY=OFF -DCMAKE_INSTALL_PREFIX=/usr \
    -DLLVM_DIR="$PWD/.cache/llvm/${ARCH}/lib/cmake/llvm" -DSNX_A64_DISASM=OFF -DBUILD_TESTING=OFF \
    -DSNX_LTO=${LTO} \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset default --parallel "$(getconf _NPROCESSORS_ONLN)"
  cmake --install build/default
  ccache --show-stats
}

# Arch's Mesa back (lavapipe); lld and mold: gnuabi_litmus; gdb: gdbstub_native; jq: pr_builds; github-cli:
# Amberlith's download
# env: AMBERLITH
test_tools() {
  packages="lld mold qemu-user pkgconf libxtst libxrandr libxss vulkan-swrast vulkan-validation-layers gdb jq"
  [ "$AMBERLITH" = true ] && packages="$packages github-cli"
  for try in 1 2 3; do
    pacman -S --noconfirm mesa && pacman -S --noconfirm --needed $packages && break
    [ "$try" = 3 ] && exit 1
    sleep 5
  done
}

# Unicorn 2.1.4, the A32 and Thumb differentials' reference (SNX_TEST_ARM_DIFF), and the header and library the
# native back ends' tests link, in a venv: Arch's Python takes no pip install of its own. The wheels, by their
# SHA-256 on PyPI (x86-64's and AArch64's). PyPI drops a connection now and then, so it is tried 3 times, 5 s apart
unicorn_venv() {
  python -m venv --system-site-packages .cache/unicorn
  cat > .cache/unicorn/requirements.txt <<'EOF'
unicorn==2.1.4 \
  --hash=sha256:9d6e6dea140560de4ebd8446661f7ef84a357d428c14a3ef09dacd306ec8c239 \
  --hash=sha256:bd1fb0c9af5f57e356d8a96928b4fe045b2e18f308ef23b481d5f970008aa722
EOF
  for try in 1 2 3; do
    .cache/unicorn/bin/pip install --quiet --require-hashes --only-binary :all: -r .cache/unicorn/requirements.txt && break
    [ "$try" = 3 ] && exit 1
    sleep 5
  done
}

# The test options on the AppImage's tree, its tests on, and the headless tree, whose options change what every
# target builds, in a folder of its own with this job's LLVM. The A64 disassembler, which the AppImage leaves out,
# is back in AArch64's test tree; on x86-64 linux-sanitizers' tree has it. x86-64's has the other machines' back
# ends under Unicorn too, whose tests linux-unicorn runs. Built on every event, tested where SNX_TESTS
# env: A64_DISASM ARCH UNICORN
test_trees() {
  ccache --zero-stats
  options=()
  if [ "$UNICORN" = true ]; then
    wheel=$(.cache/unicorn/bin/python -c 'import os, unicorn; print(os.path.dirname(os.path.abspath(unicorn.__file__)))')
    options=(-DSNX_UNICORN_INCLUDE_DIR="$wheel/include" -DSNX_UNICORN_LIBRARY="$wheel/lib/libunicorn.so.2")
  fi
  cmake build/default ${SNX_TEST_OPTIONS} -DBUILD_TESTING=ON \
    -DSNX_A64_DISASM=${A64_DISASM} \
    -DPython3_EXECUTABLE="$PWD/.cache/unicorn/bin/python" "${options[@]}"
  cmake --build build/default --parallel "$(getconf _NPROCESSORS_ONLN)"
  ccache --show-stats
  ccache --zero-stats
  cmake --preset default -B build/headless ${SNX_HEADLESS_OPTIONS} -DBUILD_TESTING=ON \
    -DLLVM_DIR="$PWD/.cache/llvm/${ARCH}/lib/cmake/llvm" -DSNX_A64_DISASM=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build build/headless --parallel "$(getconf _NPROCESSORS_ONLN)"
  ccache --show-stats
}

# the SDL3 that cpmfile.json pins, as Windows builds it
sdl_fallback() {
  ccache --zero-stats
  cmake --preset default -B build/sdl-fallback -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
    -DSNX_DEPLOY=OFF -DBUILD_TESTING=OFF -DSDL_WAYLAND=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build build/sdl-fallback --target scarletnx --parallel "$(getconf _NPROCESSORS_ONLN)"
  ccache --show-stats
}

# then what the native back end made of whole programs (scripts/recompile-census.py)
tests() {
  status=0
  ctest --test-dir build/default -LE '^(unicorn|sdm)$' --output-on-failure --parallel 4 \
    --output-junit "$PWD/tests.xml" || status=$?
  python3 scripts/recompile-census.py tests.xml || status=1
  exit "$status"
}

# gh fetches through the REST API, as the releases/download link can answer 504 for hours; 3 tries each.
# The nightly's NRO is named for its version and commit, amberlith-<version>-<commit>.nro; one that cannot be
# fetched or checked is a warning, and its run is left out
# env: GH_TOKEN
amberlith_fetch() {
  fetch() { # TAG FILE SHA256
    for try in 1 2 3; do
      gh release download "$1" -R scarletnx/amberlith -p "$2" --clobber && break
      [ "$try" = 3 ] && return 1
      sleep 30
    done
    echo "$3  $2" | sha256sum -c -
  }
  if [ -z "$EDENPROBE_SHA256" ]; then
    echo "EDENPROBE_SHA256 is empty: pin the sha256 of $EDENPROBE_NRO" >&2
    exit 1
  fi
  fetch "$EDENPROBE_VERSION" "$EDENPROBE_NRO" "$EDENPROBE_SHA256" || exit 1
  fetch "$EDENPROBE_APK_VERSION" "$EDENPROBE_APK" "$EDENPROBE_APK_SHA256" || exit 1
  mkdir -p edenprobe-nightly
  if gh release download nightly -R scarletnx/amberlith -D edenprobe-nightly -p 'amberlith-*.nro' \
      -p 'amberlith-*.nro.sha256' &&
    mv edenprobe-nightly/amberlith-*.nro edenprobe-main.nro &&
    mv edenprobe-nightly/amberlith-*.nro.sha256 edenprobe-main.nro.sha256 &&
    echo "$(cut -d ' ' -f 1 edenprobe-main.nro.sha256)  edenprobe-main.nro" | sha256sum -c -; then
    echo "main=true" >> "$GITHUB_OUTPUT"
  else
    echo "::warning title=Amberlith main::Amberlith's nightly could not be fetched or checked"
  fi
}

# The NRO and the APK held to their baselines, then Amberlith's main beside the release's baseline, warnings only:
# one after the other, as the NRO's kernel family times its threads, each with its own output, summary and exit. The
# NRO runs with no FPS limit (fps-limit=0): its vsync comes every millisecond, so no case waits on a display;
# scarletnx-android has no such setting. Each summary's title is a line of the pull request's comment, a notice of
# this job that scripts/pr-builds.sh reads back by this step's name once the run ends; on one line, % is all a notice
# escapes
# env: MAIN
amberlith_run() {
  # NAME ARGS...: one run, its output in NAME.log, its status in NAME.status
  probe() {
    local name=$1
    shift
    local began=$(date +%s) code=0
    python3 scripts/edenprobe.py run --runtime stage/bin/scarletnx "$@" --summary "$name.md" \
      > "$name.log" 2>&1 || code=$?
    echo "$code" > "$name.status"
    echo "$name: $(($(date +%s) - began)) s" >> times.txt
  }
  began=$(date +%s)
  : > times.txt
  probe edenprobe --nro "$EDENPROBE_NRO" --mode "$EDENPROBE_MODE" --out edenprobe \
    --baseline tests/edenprobe/linux-x86_64.txt --setting fps-limit=0
  probe edenprobe-android --apk "$EDENPROBE_APK" --mode "$EDENPROBE_APK_MODE" \
    --out edenprobe-android --baseline tests/edenprobe/android-x86_64.txt
  [ "$MAIN" = true ] && probe edenprobe-main --nro edenprobe-main.nro --mode "$EDENPROBE_MODE" \
    --out edenprobe-main --advisory-baseline tests/edenprobe/linux-x86_64.txt --setting fps-limit=0
  status=0
  for name in edenprobe edenprobe-android edenprobe-main; do
    [ -f "$name.status" ] || continue
    echo "::group::$name, exit $(cat "$name.status")"
    cat "$name.log"
    echo "::endgroup::"
    [ -f "$name.md" ] && cat "$name.md" >> "$GITHUB_STEP_SUMMARY"
    if [ "$name" != edenprobe-main ]; then
      if [ -f "$name/summary.md" ]; then
        result=$(head -n 1 "$name/summary.md" | cut -c5-)
        echo "::notice title=edenprobe::${result//%/%25}"
      fi
      [ "$(cat "$name.status")" = 0 ] || status=1
    fi
  done
  echo "the runs: $(($(date +%s) - began)) s in all; $(tr '\n' ';' < times.txt)"
  exit "$status"
}

# The x86-64 back end's instruction selection (tools/sailgen/sdm/x64_isel.inc) made again from the Intel SDM, on a
# run by hand, a version's tag and a pull request that changes what it is made from. The manual is downloaded here alone
# (SNX_TEST_SDM), and Intel's link serves the latest revision: where that is not 093US, or it doesn't answer, this
# step fails, saying what it got and what it expected, and no other pull request does. The last of the job's
# tests, so that what it says hides none of the others
x64_isel() {
  if [ "$GITHUB_EVENT_NAME" = pull_request ]; then
    git config --global --add safe.directory "$GITHUB_WORKSPACE"
    changed=$(git diff --name-only HEAD^1 HEAD)
    if ! grep -qE '^(tools/sailgen/|cmake/sdm\.cmake$|tests/sdm_isel_test\.py$)' <<<"$changed"; then
      echo "::notice title=x64_isel.inc::not made again: the pull request changes nothing it is made from"
      exit 0
    fi
  fi
  cmake build/default -DSNX_TEST_SDM=ON
  cmake --build build/default --target snx-sailgen --parallel "$(getconf _NPROCESSORS_ONLN)"
  ctest --test-dir build/default -R '^sailgen_sdm_isel$' --output-on-failure --no-tests=error \
    --output-junit sdm_isel.xml
  # skipped is not made again
  if grep -q '<skipped' build/default/sdm_isel.xml; then exit 1; fi
}

sanitizers_build() {
  git config --global --add safe.directory '*'
  ccache --zero-stats
  cmake --preset default -B build/sanitizers -DBUILD_TESTING=ON -DSNX_TEST_SANITIZERS=ON -DSNX_TEST_VP9_VECTORS=ON \
    -DSNX_DEPLOY=OFF -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build build/sanitizers --parallel "$(getconf _NPROCESSORS_ONLN)"
  ccache --show-stats
}

unicorn_tree() {
  git config --global --add safe.directory '*'
  ccache --zero-stats
  wheel=$(.cache/unicorn/bin/python -c 'import os, unicorn; print(os.path.dirname(os.path.abspath(unicorn.__file__)))')
  cmake --preset default -B build/unicorn ${SNX_TEST_OPTIONS} -DBUILD_TESTING=ON -DSNX_DEPLOY=OFF \
    -DSNX_A64_DISASM=OFF -DPython3_EXECUTABLE="$PWD/.cache/unicorn/bin/python" \
    -DSNX_UNICORN_INCLUDE_DIR="$wheel/include" -DSNX_UNICORN_LIBRARY="$wheel/lib/libunicorn.so.2" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build build/unicorn --parallel "$(getconf _NPROCESSORS_ONLN)"
  ccache --show-stats
}

# then what each machine ran: one that ran none fails (scripts/unicorn-tests.py)
unicorn_tests() {
  status=0
  ctest --test-dir build/unicorn -L '^unicorn$' --no-tests=error --output-on-failure --parallel 4 \
    --output-junit "$PWD/unicorn.xml" || status=$?
  python3 scripts/unicorn-tests.py unicorn.xml || status=1
  exit "$status"
}

qemu_reference_build() {
  cmake --preset default -DBUILD_TESTING=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset default --parallel "$(getconf _NPROCESSORS_ONLN)" \
    --target snx-test-llvm-qemu-differential snx_qemu_ref_runner
  ccache --show-stats
}

qemu_reference_record() {
  build/default/tests/snx-test-llvm-qemu-differential qemu-aarch64 build/default/qemu_ref_runner \
    build/llvm_qemu_reference_dir tests/llvm_qemu_expected.txt --quiet \
    --record build/llvm_qemu_reference.bin
}

if [ $# != 1 ] || [ "$(type -t "$1")" != function ]; then
  echo "usage: $0 STEP, one of: $(declare -F | cut -d ' ' -f 3 | tr '\n' ' ')(given: ${*:-nothing})" >&2
  exit 2
fi
"$1"
