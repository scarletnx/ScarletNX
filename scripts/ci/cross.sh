#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The appimage-cross job's steps, a function each, run as `scripts/ci/cross.sh STEP`; "env:" above one names what
# its step's env gives it. The build container (build) has the workspace at /src; the target's (target) is the
# image the AppImage is bundled in
set -e

# setup-qemu-action has no big-endian ppc64: the runner's static qemu-user, by Debian's magic and mask; F opens it
# now, for the containers too, and P keeps argv[0]
binfmt_ppc64() {
  sudo apt-get update && sudo apt-get install -y --no-install-recommends qemu-user
  # the build container has none of the runner's libraries: its qemu has to be static, as Debian's is since 9.2
  ! ldd "$(command -v qemu-ppc64)" 2>&1 | grep -q '=>' ||
    { echo "the runner's qemu-ppc64 is dynamically linked, which the build container cannot run" >&2; exit 1; }
  printf ':qemu-ppc64:M::%s:%s:%s:POCF\n' \
    '\x7f\x45\x4c\x46\x02\x02\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x15' \
    '\xff\xff\xff\xff\xff\xff\xff\xfc\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff' \
    "$(command -v qemu-ppc64)" | sudo tee /proc/sys/fs/binfmt_misc/register
  head -n 3 /proc/sys/fs/binfmt_misc/qemu-ppc64
}

# the build container has the workspace at /src; the target's / is the image, where the runtime is bundled
# env: PLATFORM
containers() {
  mkdir -p "$RUNNER_TEMP/pacman"
  docker run --detach --name build --volume "$PWD:/src" --volume "$RUNNER_TEMP/pacman:/var/cache/pacman/pkg" \
    --workdir /src ghcr.io/pkgforge-dev/archlinux:latest sleep infinity
  docker run --detach --name target --platform linux/${PLATFORM} \
    --volume "$PWD:/src" --workdir /src ghcr.io/pkgforge-dev/archlinux:latest sleep infinity
}

# env: PLATFORM
sysroot() {
  set -euo pipefail
  image=ghcr.io/pkgforge-dev/archlinux:latest
  docker pull --platform linux/${PLATFORM} "$image"
  docker exec build mkdir -p /src/sysroot
  docker export "$(docker create --platform linux/${PLATFORM} "$image")" | docker exec -i build tar -x -C /src/sysroot
}

# The pinned Anylinux-AppImages tools and the target's libraries the runtime links and bundles: binutils and
# patchelf are what quick-sharun needs on PATH. get-debloated-pkgs then replaces the full Mesa (its libgallium
# links libLLVM), ICU and Opus, so it runs last. Each pacman is tried 3 times, as in the appimage job
target_tools() {
  docker exec -i -e QUICK_SHARUN -e QUICK_SHARUN_SHA -e DEBLOATED_PACKAGES -e DEBLOATED_PACKAGES_SHA \
    target sh -s <<'EOF'
set -eu
libraries="patchelf binutils sdl3 libx11 mesa vulkan-icd-loader vulkan-radeon vulkan-swrast libxext libxcursor
  libxfixes libxi libxinerama libxrandr libxrender libxss libxtst libxkbcommon wayland wayland-protocols libdecor
  alsa-lib libpulse libpipewire hidapi libusb"
retry() { for try in 1 2 3; do "$@" && return; [ "$try" = 3 ] && return 1; sleep 5; done; }
# A port whose openssl is already 4 while some of its packages still link OpenSSL 3 refuses the upgrade
# unless its openssl-3 comes along; a port still on OpenSSL 3 has no such package
retry pacman -Sy --noconfirm
if pacman -Si openssl-3 >/dev/null 2>&1; then libraries="$libraries openssl-3"; fi
# -Suu: the image may also be ahead of the port's repo (the appimage job's Install git says how)
retry pacman -Suu --noconfirm --needed $libraries
curl -fsSL --retry 15 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o /usr/local/bin/quick-sharun "$QUICK_SHARUN"
curl -fsSL --retry 15 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o /usr/local/bin/get-debloated-pkgs "$DEBLOATED_PACKAGES"
chmod +x /usr/local/bin/quick-sharun /usr/local/bin/get-debloated-pkgs
printf '%s  %s\n' "$QUICK_SHARUN_SHA" /usr/local/bin/quick-sharun \
                 "$DEBLOATED_PACKAGES_SHA" /usr/local/bin/get-debloated-pkgs | sha256sum -c
get-debloated-pkgs mesa-nano opus-mini icu-mini
EOF
}

# The build machine's packaging tool: quick-sharun makes the AppImage, and this appimagetool compresses natively,
# takes the target's runtime from APPIMAGE_ARCH, and resolves mkdwarfs for this machine rather than the target's
packaging_tools() {
  docker exec -i -e QUICK_SHARUN -e QUICK_SHARUN_SHA -e APPIMAGETOOL_URL -e APPIMAGETOOL_SHA build sh -s <<'EOF'
set -eu
curl -fsSL --retry 15 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o /usr/local/bin/quick-sharun "$QUICK_SHARUN"
curl -fsSL --retry 15 --retry-delay 20 --retry-all-errors --connect-timeout 30 -o /usr/local/bin/appimagetool "$APPIMAGETOOL_URL"
chmod +x /usr/local/bin/quick-sharun /usr/local/bin/appimagetool
printf '%s  %s\n' "$QUICK_SHARUN_SHA" /usr/local/bin/quick-sharun \
                 "$APPIMAGETOOL_SHA" /usr/local/bin/appimagetool | sha256sum -c
EOF
}

# the build machine's tools: Clang and LLD for the target, wayland-scanner for SDL3, jdk21 for the Android app
# driver, meson for dav1d
build_tools() {
  docker exec -i build sh -s <<'EOF'
set -eu
retry() { for try in 1 2 3; do "$@" && return; [ "$try" = 3 ] && return 1; sleep 5; done; }
retry pacman -Syu --noconfirm --needed cmake ninja make patch perl python clang lld llvm glslang qemu-user \
  git pkgconf patchelf wayland ccache jdk21-openjdk meson
EOF
}

# the target's libraries, where the root missed the cache, saved before the build adds to it
sysroot_libraries() {
  docker exec -i build sh -s <<'EOF'
set -eu
scripts/appimage-sysroot.sh /src/sysroot
chown -R "$(stat -c %u:%g /src)" /src/sysroot
chmod -R u+rwX /src/sysroot
EOF
}

# Through ccache, without debug information, which no one reads in these builds and the AppImage would carry. SDL3
# is the system root's package (SNX_SYSTEM_SDL3: a root without it, or with one too old, stops the configure). The container gets a
# nightly's name (SNX_BUILD_NAME)
# env: ARCH PRESET
build() {
  docker exec -i -e PRESET -e ARCH -e CCACHE_DIR=/src/.cache/ccache -e CCACHE_BASEDIR=/src -e CCACHE_NOHASHDIR=true \
    -e SNX_DEPS_CACHE=/src/.cache/deps -e SNX_BUILD_NAME build sh -s <<'EOF'
set -eu
git config --global --add safe.directory '*'
export SNX_SYSROOT=/src/sysroot
cmake --preset ${PRESET} -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF \
  -DSNX_OPT_FLAGS="-O2 -g0" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DSNX_SYSTEM_SDL3=ON
cmake --build --preset ${PRESET} --parallel "$(getconf _NPROCESSORS_ONLN)"
DESTDIR=/src/stage cmake --install build/${PRESET}
mkdir -p /src/.cache/deps
chown -R "$(stat -c %u:%g /src)" /src/.cache/ccache /src/.cache/deps
EOF
}

# The A64 differential against the qemu-reference job's recording, of the target's native back end: the cross build's
# tests are off, so the test is configured and built alone, then run here through qemu-user
# env: ARCH PRESET QEMU
differential() {
  docker exec -i -e PRESET -e ARCH -e QEMU -e CCACHE_DIR=/src/.cache/ccache -e CCACHE_BASEDIR=/src -e CCACHE_NOHASHDIR=true \
    -e SNX_DEPS_CACHE=/src/.cache/deps build sh -s <<'EOF'
set -eu
git config --global --add safe.directory '*'
export SNX_SYSROOT=/src/sysroot
cmake --preset ${PRESET} -DBUILD_TESTING=ON \
  -DSNX_OPT_FLAGS="-O2 -g0" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DSNX_SYSTEM_SDL3=ON
cmake --build --preset ${PRESET} --parallel "$(getconf _NPROCESSORS_ONLN)" \
  --target snx-test-native-qemu-differential
# a failing run's examples go to the log: the container's /tmp goes with the job. A run takes minutes; at 20 its SIGBUS
# makes the test name the case it was in, where a hang would hold the job 3 hours
status=0
timeout -s BUS -k 30 1200 qemu-${QEMU} -L /src/sysroot build/${PRESET}/tests/snx-test-native-qemu-differential \
  /tmp/native_qemu_reference /src/tests/native_qemu_expected.txt --reference /src/reference/native_qemu_reference.bin \
  --quiet || status=$?
[ "$status" = 0 ] || cat /tmp/native_qemu_reference/*_qemu_mismatches.txt || true
exit $status
EOF
}

# The launcher's tests on the target through qemu-user, as the preset's test preset runs them (QEMU_LD_PREFIX for
# the programs a test starts, which the binfmt handler above runs): what the AppImage's launcher draws, reads and
# writes, on a big-endian host too. The tree is the differential's, with every launcher test built. gui_sdl_events
# stops at the Settings' groups under qemu in these containers, whatever the architecture (#2017)
# env: ARCH PRESET
launcher_tests() {
  docker exec -i -e PRESET -e ARCH -e CCACHE_DIR=/src/.cache/ccache -e CCACHE_BASEDIR=/src -e CCACHE_NOHASHDIR=true \
    -e SNX_DEPS_CACHE=/src/.cache/deps build sh -s <<'EOF'
set -eu
git config --global --add safe.directory '*'
export SNX_SYSROOT=/src/sysroot
cmake --preset ${PRESET} -DBUILD_TESTING=ON \
  -DSNX_OPT_FLAGS="-O2 -g0" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DSNX_SYSTEM_SDL3=ON
cmake --build --preset ${PRESET} --parallel "$(getconf _NPROCESSORS_ONLN)" --target snx_gui_tests
ctest --preset ${PRESET} -R '^gui_' -E '^gui_sdl_events$' -j"$(getconf _NPROCESSORS_ONLN)"
EOF
}

# The census of whole programs, as in the appimage job, of the target's native back end: its drivers recompile them
# through qemu-user, in the launcher tests' tree, and none is run
# env: PRESET
census() {
  docker exec -i -e PRESET -e CCACHE_DIR=/src/.cache/ccache -e CCACHE_BASEDIR=/src -e CCACHE_NOHASHDIR=true \
    -e SNX_DEPS_CACHE=/src/.cache/deps build sh -s <<'EOF'
set -eu
git config --global --add safe.directory '*'
export SNX_SYSROOT=/src/sysroot
cmake --build --preset ${PRESET} --parallel "$(getconf _NPROCESSORS_ONLN)" \
  --target scarletnx snx-test-recompile-census
status=0
ctest --preset ${PRESET} -L '^census$' --no-tests=error --output-on-failure \
  -j"$(getconf _NPROCESSORS_ONLN)" --output-junit "$PWD/census.xml" || status=$?
python3 scripts/recompile-census.py census.xml || status=1
exit "$status"
EOF
}

# only what this run compiled stays, given back to the runner's user
# env: START
ccache_trim() {
  docker exec -i -e CCACHE_DIR=/src/.cache/ccache -e START build sh -s <<'EOF'
set -eu
ccache --show-stats --verbose
ccache --evict-older-than "$(( $(date +%s) - START ))s"
ccache --zero-stats
du -sh "$CCACHE_DIR"
chown -R "$(stat -c %u:%g /src)" /src/.cache/ccache
EOF
}

# Packaging runs where / is the target's system root, so quick-sharun reads it and the licenses' packages come
# from its pacman database; the AppImage itself is compressed on the build machine
bundle() {
  for try in 1 2 3; do
    if docker exec -i target sh -s <<'EOF'
set -eu
rm -f "${TMPDIR:-/tmp}"/cross-libc-dlopen-*.tar
cp -a /src/stage/. /
SKIP_APPIMAGE=1 scripts/make-appimage.sh
EOF
    then
      break
    fi
    [ "$try" = 3 ] && exit 1
    echo "the bundle failed; trying again in 30 seconds" >&2
    sleep 30
  done
}

# The container gets the ref and a nightly's name (SNX_BUILD_NAME), whose release the AppImage updates to
# (scripts/appimage-update-info.sh)
# env: ARCH
appimage() {
  docker exec -i -e GITHUB_REPOSITORY -e GITHUB_REF_TYPE -e GITHUB_REF -e SNX_BUILD_NAME \
    -e VERSION="$(git describe --always --dirty)" \
    -e MAKE_APPIMAGE_ONLY=1 -e ARCH -e APPIMAGETOOL=/usr/local/bin/appimagetool \
    build sh -s <<'EOF'
set -eu
scripts/make-appimage.sh
EOF
}

# env: ARCH
check_appimage() {
  docker exec -i -e ARCH build sh -s <<'EOF'
set -eu
scripts/check-appimage.sh
EOF
}

# actions/cache's tar fails on what only the container's root may open (FFmpeg's tarball is 0700)
cpm_owner() {
  docker exec build chown -R "$(id -u):$(id -g)" /src/.cache/cpm
  # tar saves a link as a link, whatever it points at
  unreadable=$(find .cache/cpm ! -type l ! -readable)
  [ -z "$unreadable" ] || { printf 'actions/cache cannot read:\n%s\n' "$unreadable" >&2; exit 1; }
}

# The AppImage as a user gets it where SNX_TESTS, on this runner, which has neither the checkout nor the build
# tree where the build container had them (/src). Each game the AppImage makes is a copy of
# it, and an AppImage carries "AI\x02" in its ELF header's padding, where the target's handler wants zeros: one
# more handler, made from the AppImage's own first 20 bytes with the padding left out, runs them through
# qemu-user. P keeps argv[0], by which the AppImage's runtime runs the dwarfs tool it carries as itself; both
# handlers are printed. e_type's low byte, executable or shared, is left to either: byte 16 of a little-endian
# header, 17 of a big-endian one (EI_DATA 2, PowerPC64 BE's)
# env: QEMU
binfmt() {
  set -- $(head -c 20 dist/*.AppImage | od -An -v -tx1)
  low=$(($6 == 2 ? 17 : 16))
  magic= mask= i=0
  for byte in "$@"; do
    case $i in 7|8|9|10|11|12|13|14|15) byte=00 m=00 ;; "$low") m=fe ;; *) m=ff ;; esac
    magic=$magic\\x$byte mask=$mask\\x$m i=$((i + 1))
  done
  printf ':appimage-%s:M::%s:%s:%s:POCF\n' ${QEMU} "$magic" "$mask" "$(command -v qemu-${QEMU})" |
    sudo tee /proc/sys/fs/binfmt_misc/register
  grep -qx enabled /proc/sys/fs/binfmt_misc/appimage-${QEMU}
  head -n 3 /proc/sys/fs/binfmt_misc/qemu-${QEMU} /proc/sys/fs/binfmt_misc/appimage-${QEMU}
}

# env: ARCH QEMU
run_appimage() {
  grep -qx enabled /proc/sys/fs/binfmt_misc/qemu-${QEMU} ||
    { echo "no binfmt handler runs ${QEMU} programs here" >&2; exit 1; }
  scripts/check-package.sh dist/ScarletNX-linux-${ARCH}.AppImage
}

if [ $# != 1 ] || [ "$(type -t "$1")" != function ]; then
  echo "usage: $0 STEP, one of: $(declare -F | cut -d ' ' -f 3 | tr '\n' ' ')(given: ${*:-nothing})" >&2
  exit 2
fi
"$1"
