#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The android job's steps, a function each, run as `scripts/ci/android.sh STEP`; "env:" above one names what its
# step's env gives it
set -e

# the NDK named above, NASM for FFmpeg and dav1d, meson; sdkmanager's download can end early: tried 3 times
tools() {
  for try in 1 2 3; do
    "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install "ndk;${ANDROID_NDK_ROOT##*/}" > /dev/null && break
    [ "$try" = 3 ] && exit 1
    sleep 5
  done
  sudo apt-get update && sudo apt-get install -y nasm meson ccache
}

# one key signs every APK (the ANDROID_KEYSTORE secret); a fork's pull request has none, its APKs the debug key
# env: KEYSTORE
keystore() {
  if [ -n "$KEYSTORE" ]; then
    printf '%s' "$KEYSTORE" | base64 -d > "$RUNNER_TEMP/scarletnx.keystore"
    echo "SNX_ANDROID_KEYSTORE=$RUNNER_TEMP/scarletnx.keystore" >> "$GITHUB_ENV"
  else
    echo "::warning::No ANDROID_KEYSTORE secret: the runner's own debug key signs this APK, so it does not" \
      "install over one the repository's key signed"
  fi
}

# env: SHORT SNX_ANDROID_KEYSTORE_PASSWORD
build() {
  for abi in x86_64 arm64-v8a; do
    # arm64-v8a's nightlies and releases link with ThinLTO (cmake/lto.cmake), x86_64's and a pull request's not
    lto=OFF
    [ "$GITHUB_EVENT_NAME" != pull_request ] && [ "$abi" = arm64-v8a ] && lto=ON
    JAVA_HOME=$JAVA_HOME_21_X64 scripts/android-apk.sh "$PWD/build/android-$abi" "$abi" -- \
      -DSNX_LTO=$lto -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    cp "build/android-$abi/stage/gradle/outputs/apk/debug/ScarletNX-debug.apk" \
      "ScarletNX-android-$abi-${SHORT}.apk"
  done
}

# env: SHORT SNX_ANDROID_KEYSTORE_PASSWORD
pack() {
  mkdir stage
  for abi in x86_64 arm64-v8a; do cp -a "build/android-$abi/stage/." stage/; done
  rm -rf stage/gradle
  JAVA_HOME=$JAVA_HOME_21_X64 gradle -p dist/android assembleDebug -Pscarletnx.stage="$PWD/stage" \
    ${SNX_ANDROID_KEYSTORE:+"-Pscarletnx.keystore=$SNX_ANDROID_KEYSTORE"}
  cp stage/gradle/outputs/apk/debug/ScarletNX-debug.apk ScarletNX-android-universal-${SHORT}.apk
}

# the certificate it carries, in the log; with the repository's key, the one ANDROID_CERT_SHA256 (a variable) names
# env: EXPECTED SHORT
signer() {
  set -euo pipefail
  apk=ScarletNX-android-universal-${SHORT}.apk
  apksigner=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)/apksigner
  "$apksigner" verify --print-certs "$apk" | tee "$RUNNER_TEMP/certs.txt"
  # "Signer #1 certificate ...", "V2 Signer: certificate ..." (apksigner 37) or, for a signer of some SDK
  # versions, "Signer (minSdkVersion=...) certificate ..."
  cert=$(sed -n -E 's/^(V[0-9.]+ )?Signer[^:]*:? certificate SHA-256 digest: //p' "$RUNNER_TEMP/certs.txt" |
         sort -u)
  if [ -z "$cert" ] || [ "$(wc -l <<< "$cert")" != 1 ]; then
    echo "::error::apksigner names no one certificate of $apk"
    exit 1
  fi
  echo "signed by the certificate of SHA-256 $cert"
  if [ -n "${SNX_ANDROID_KEYSTORE:-}" ] && [ -n "$EXPECTED" ] && [ "$cert" != "$EXPECTED" ]; then
    echo "::error::signed by $cert, not the repository's key ($EXPECTED)"
    exit 1
  fi
}

# A library that needs one Android 5.0 lacks fails to load there, and the app with it
# env: SHORT
libraries() {
  set -euo pipefail
  apk=ScarletNX-android-universal-${SHORT}.apk
  readelf=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
  mkdir apk && unzip -q "$apk" 'lib/*' -d apk
  system=" libc.so libm.so libdl.so liblog.so libandroid.so libz.so libEGL.so libGLESv1_CM.so libGLESv2.so libOpenSLES.so "
  for abi in x86_64 arm64-v8a; do
    for lib in libmain.so libscarletnx.so libscarletnx-run.so; do
      test -f "apk/lib/$abi/$lib" || { echo "$abi has no $lib" >&2; exit 1; }
      for needed in $("$readelf" -d "apk/lib/$abi/$lib" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        [[ "$system" == *" $needed "* || -f "apk/lib/$abi/$needed" ]] ||
          { echo "$abi/$lib needs $needed, which Android 5.0 does not have" >&2; exit 1; }
      done
    done
  done
  # x86_64's recompiler has the x86-64 back end (cmake/sdm.cmake): its library lacks the words of a build without
  # one, which arm64-v8a's holds
  none='no x86-64 back end in this build'
  grep -qaF "$none" apk/lib/arm64-v8a/libscarletnx.so ||
    { echo "arm64-v8a/libscarletnx.so does not hold \"$none\": x86_64's lacking it would prove nothing" >&2; exit 1; }
  if grep -qaF "$none" apk/lib/x86_64/libscarletnx.so; then
    echo "x86_64/libscarletnx.so holds \"$none\": it has no x86-64 back end" >&2
    exit 1
  fi
  ls -l apk/lib/*/libscarletnx.so
}

if [ $# != 1 ] || [ "$(type -t "$1")" != function ]; then
  echo "usage: $0 STEP, one of: $(declare -F | cut -d ' ' -f 3 | tr '\n' ' ')(given: ${*:-nothing})" >&2
  exit 2
fi
"$1"
