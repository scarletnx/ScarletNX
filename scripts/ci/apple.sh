#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The apple job's steps, a function each, run as `scripts/ci/apple.sh STEP`; "env:" above one names what its
# step's env gives it
set -e

macos_build() {
  # the JDK the Android app driver (src/android) builds with: the runner's 21
  export JAVA_HOME="${JAVA_HOME_21_arm64:?the runner has no JDK 21}"
  # Keep the release's SDL3 static when the later shared-library check installs Homebrew's package.
  cmake --preset macos -DLLVM_DIR="$PWD/apple-llvm/lib/cmake/llvm" -DSNX_TEST_VP9_VECTORS=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset macos --parallel "$(getconf _NPROCESSORS_ONLN)"
}

ios_build() {
  cmake --preset ios -DLLVM_DIR="$PWD/ios-llvm/lib/cmake/llvm" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset ios --parallel "$(getconf _NPROCESSORS_ONLN)"
}

tests() {
  status=0
  ctest --test-dir build/macos --output-on-failure --parallel 4 --output-junit "$PWD/tests.xml" || status=$?
  python3 scripts/recompile-census.py tests.xml || status=1
  exit "$status"
}

# against Homebrew's shared SDL3, which brings no framework: plat_posix.c must name CoreFoundation itself
shared_sdl3() {
  brew install sdl3
  cmake --preset macos -B build/sdl3-shared -DSNX_SYSTEM_SDL3=ON -DSNX_DEPLOY=OFF \
    -DLLVM_DIR="$PWD/apple-llvm/lib/cmake/llvm" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build build/sdl3-shared --parallel "$(getconf _NPROCESSORS_ONLN)" \
    --target snx-test-memory-own snx-test-platform-posix snx-test-memory-alias
}

# signed ad hoc, zipped by CPack (cmake/apple_package.cmake)
# env: SHORT
macos_app() {
  curl -fL --retry 15 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o moltenvk-macos.tar \
    "https://github.com/V380-Ori/Ryujinx.MoltenVK/releases/download/$MOLTENVK_RELEASE/MoltenVK-macos.tar"
  echo "$MOLTENVK_MACOS_SHA256  moltenvk-macos.tar" | shasum -a 256 -c
  mkdir moltenvk-macos && tar -xf moltenvk-macos.tar -C moltenvk-macos
  cmake build/macos -DSNX_MOLTENVK="$PWD/moltenvk-macos"
  # The executable and the install rules must come from the same dependency configuration.
  cmake --build --preset macos --target scarletnx --parallel "$(getconf _NPROCESSORS_ONLN)"
  cpack --config build/macos/CPackConfig.cmake -B app-macos \
    -D CPACK_PACKAGE_FILE_NAME=ScarletNX-macos-arm64-${SHORT}
  mv app-macos/ScarletNX-macos-arm64-${SHORT}.zip .
}

# unsigned: AltStore, SideStore or a developer account sign it (dist/apple/ScarletNX.entitlements)
# env: SHORT
ipa() {
  curl -fL --retry 15 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o moltenvk-ios.tar \
    "https://github.com/V380-Ori/Ryujinx.MoltenVK/releases/download/$MOLTENVK_RELEASE/MoltenVK-ios.tar"
  echo "$MOLTENVK_IOS_SHA256  moltenvk-ios.tar" | shasum -a 256 -c
  mkdir moltenvk-ios && tar -xf moltenvk-ios.tar -C moltenvk-ios
  cmake build/ios -DSNX_MOLTENVK="$PWD/moltenvk-ios"
  cpack --config build/ios/CPackConfig.cmake -B app-ios \
    -D CPACK_PACKAGE_FILE_NAME=ScarletNX-ios-arm64-${SHORT}
  mv app-ios/ScarletNX-ios-arm64-${SHORT}.ipa .
}

if [ $# != 1 ] || [ "$(type -t "$1")" != function ]; then
  echo "usage: $0 STEP, one of: $(declare -F | cut -d ' ' -f 3 | tr '\n' ' ')(given: ${*:-nothing})" >&2
  exit 2
fi
"$1"
