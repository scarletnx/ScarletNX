#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Prints the update information an AppImage of ARCH carries (scripts/make-appimage.sh's UPINFO): the GitHub release
# whose .zsync AppImageUpdate takes it to, from the build GitHub Actions describes (GITHUB_REPOSITORY, GITHUB_REF_TYPE,
# GITHUB_REF; scripts/publish-builds.py publishes each):
# - a version's tag: the latest release of the repository;
# - a pull request (refs/pull/N/merge): its own prerelease in the owner's nightly repository, pr-N, so an update brings
#   that pull request's newest push and never swaps it for a build without its change;
# - a push to main (refs/heads/main with no nightly's name, SNX_BUILD_NAME): the repository's newest prerelease, main's
#   one, main-<commit>, so an update brings main's next push and never a nightly older than it;
# - anything else, a nightly and a build outside Actions: the latest release of the owner's nightly repository.
#
#   scripts/appimage-update-info.sh ARCH
set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
    echo "usage: $0 ARCH" >&2
    exit 2
fi
arch=$1
repository=${GITHUB_REPOSITORY:-scarletnx/ScarletNX}
owner=${repository%/*} name=${repository#*/}
ref=${GITHUB_REF:-}
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
    echo "gh-releases-zsync|$owner|$name|latest|*$arch.AppImage.zsync"
elif [[ $ref =~ ^refs/pull/([0-9]+)/(merge|head)$ ]]; then
    echo "gh-releases-zsync|$owner|nightly|pr-${BASH_REMATCH[1]}|*$arch.AppImage.zsync"
elif [ "$ref" = refs/heads/main ] && [[ ${SNX_BUILD_NAME:-} != nightly-* ]]; then
    echo "gh-releases-zsync|$owner|$name|latest-pre|*$arch.AppImage.zsync"
else
    echo "gh-releases-zsync|$owner|nightly|latest|*$arch.AppImage.zsync"
fi
