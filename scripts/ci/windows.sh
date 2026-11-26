#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The windows job's steps, a function each, run in MSYS2's bash as `bash scripts/ci/windows.sh STEP`; "env:"
# above one names what its step's env gives it
set -eo pipefail

dependencies() {
  set -euo pipefail
  clang --version
  llvm-config --version
  test -f "$(llvm-config --includedir)/llvm-c/Core.h"
  test -d "$(llvm-config --libdir)"
  llvm-config --libs core native passes target analysis
  make --version
  perl --version
  nasm --version
  meson --version
}

# env: JDK_ARCH LTO
build() {
  set -euo pipefail
  # the JDK the Android app driver (src/android) builds with: the runner's 21, as the android job's Gradle takes
  jdk=JAVA_HOME_21_${JDK_ARCH}
  export JAVA_HOME="${!jdk:?the runner has no JDK 21 in $jdk}"
  # the nightlies and releases link with ThinLTO (cmake/lto.cmake), a pull request's build does not
  cmake --preset default -DSNX_LTO=${LTO} \
    -DSNX_LLVM_CONFIG="$(cygpath -m "$(command -v llvm-config)")" \
    -DSNX_FFMPEG_SH="$(cygpath -m /usr/bin/sh.exe)" \
    -DSNX_FFMPEG_MAKE="$(cygpath -m /usr/bin/make.exe)" \
    -DPERL_EXECUTABLE="$(cygpath -m /usr/bin/perl.exe)" \
    -DSNX_OPENSSL_MAKE="$(cygpath -m /usr/bin/make.exe)" \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DSNX_DEPLOY=OFF -DBUILD_TESTING=ON -DSNX_TEST_VP9_VECTORS=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset default --parallel "$(getconf _NPROCESSORS_ONLN)"
  cmake --install build/default --prefix stage
}

# the build line says -dirty where a tracked file changed: none did, and one changed now does
build_line() {
  set -euo pipefail
  # the commit's define alone: a branch's name may end in -dirty too
  dirty() { grep -q '^#define SNX_COMMIT "[^"]*-dirty"' "$1"; }
  printf '#define SNX_BRANCH "a-dirty"\n#define SNX_COMMIT "abc1234"\n' > build/default/commit-check.h
  if dirty build/default/commit-check.h; then
    echo "a branch named a-dirty reads as a commit with changes" >&2
    exit 1
  fi
  ctest --test-dir build/default -R '^build_info$' --verbose | grep -F 'ScarletNX '
  if dirty build/default/generated/snx_commit.h; then
    echo "the build line says -dirty; the tracked files MSYS2's git sees changed:" >&2
    git status --porcelain --untracked-files=no | head -20 >&2
    echo "$(git status --porcelain --untracked-files=no | wc -l) files; read with CR LF line ends:" \
      "$(git -c core.autocrlf=true status --porcelain --untracked-files=no | wc -l)" >&2
    for g in git '/c/Program Files/Git/bin/git.exe'; do # MSYS2's, and the checkout's
      echo "core.autocrlf, $g: $("$g" config --show-origin core.autocrlf || echo unset)" >&2
    done
    exit 1
  fi
  echo >> README.md
  cmake -DSRC=. -DOUT=build/default/commit-check.h -P cmake/commit.cmake
  git checkout -- README.md
  if ! dirty build/default/commit-check.h; then
    echo "README.md changed, and the build line would say $(cat build/default/commit-check.h)" >&2
    exit 1
  fi
}

# Explorer shows the first icon, and Windows reads the manifest there (runtime.rc)
resources() {
  set -euo pipefail
  for exe in stage/bin/scarletnx.exe; do
    # the whole listing first: grep -q stopping early would fail the pipe under pipefail
    resources=$(llvm-readobj --coff-resources "$exe")
    if ! grep -q 'Type: GROUP_ICON' <<<"$resources"; then
      echo "$exe has no icon" >&2
      exit 1
    fi
    # the manifest is kept there as text
    if ! grep -aqF '>SegmentHeap</heapType>' "$exe"; then
      echo "$exe has no manifest with heapType SegmentHeap (runtime.manifest)" >&2
      exit 1
    fi
  done
}

driver() {
  set -euo pipefail
  build/default/scarletnx.exe --help > /dev/null
  build/default/scarletnx.exe --extract --help > /dev/null
  build/default/scarletnx.exe --recomp --help > /dev/null
}

tests() {
  set -euo pipefail
  status=0
  ctest --test-dir build/default --output-on-failure --parallel 4 --output-junit "$PWD/tests.xml" || status=$?
  python scripts/recompile-census.py tests.xml || status=1
  exit "$status"
}

bundle() {
  set -euo pipefail
  shopt -s nullglob
  # Follow imports recursively so the artifact works without MSYS2 on PATH.
  # Windows system DLLs are supplied by Windows, never copied from the runner.
  binaries=(stage/bin/*.exe stage/bin/*.dll)
  for ((i = 0; i < ${#binaries[@]}; ++i)); do
    imports=$(llvm-objdump -p "${binaries[i]}")
    while IFS= read -r dll; do
      source="$MINGW_PREFIX/bin/$dll"
      destination="stage/bin/$dll"
      if [[ -f "$source" && ! -f "$destination" ]]; then
        cp "$source" "$destination"
        binaries+=("$destination")
      fi
    done < <(printf '%s\n' "$imports" | awk '/DLL Name:/ { sub(/\r$/, "", $3); print $3 }')
  done
  mkdir -p stage/share/licenses/msys2
  cp -R "$MINGW_PREFIX/share/licenses/." stage/share/licenses/msys2/
  pacman -Q > stage/share/licenses/msys2/build-packages.txt
}

if [ $# != 1 ] || [ "$(type -t "$1")" != function ]; then
  echo "usage: $0 STEP, one of: $(declare -F | cut -d ' ' -f 3 | tr '\n' ' ')(given: ${*:-nothing})" >&2
  exit 2
fi
"$1"
