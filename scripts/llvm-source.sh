#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's release source for the LLVM builds (scripts/linux-llvm.sh, android-llvm.sh, apple-llvm.sh): FILE as
# llvm-project-VERSION.src.tar.xz was released, by the SHA-256 scripts/llvm-source.sha256 pins for it.
#
#   scripts/llvm-source.sh VERSION FILE
#
# A FILE with that SHA-256 is used as it is; one with another (a download cut short) is downloaded again. The download
# goes to FILE.part, which becomes FILE only once it is checked. A VERSION without a line in the list is refused: add
# the line, its hash the digest the release's page shows for the file, never the hash of what this would download.
set -eu
[ $# -eq 2 ] || { sed -n '4,11p' "$0"; exit 2; }
version=$1 file=$2
name=llvm-project-$version.src.tar.xz
list=$(dirname "$0")/llvm-source.sha256
want="" pinned=""
while read -r hash listed; do
    case $listed in
        llvm-project-*.src.tar.xz)
            [ "$listed" = "$name" ] && want=$hash
            listed=${listed#llvm-project-}
            pinned="$pinned ${listed%.src.tar.xz}"
            ;;
    esac
done < "$list"
if [ -z "$want" ]; then
    echo "LLVM $version has no SHA-256 in $list, which pins$pinned: add the line of $name there, or pass the" \
        "script a source of your own" >&2
    exit 1
fi
# Linux has sha256sum, a Mac shasum
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1
}
if [ -f "$file" ]; then
    have=$(sha256 "$file")
    [ "$have" = "$want" ] && exit 0
    echo "$file has the SHA-256 $have, and $name $want: downloading it again" >&2
fi
# --retry: a 504 from the release's host is tried again
curl -fL --retry 3 -o "$file.part" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name" ||
    { rm -f "$file.part"; exit 1; }
have=$(sha256 "$file.part")
if [ "$have" != "$want" ]; then
    rm -f "$file.part"
    echo "$name: the download has the SHA-256 $have, and the release $want. It is not used." >&2
    exit 1
fi
mv "$file.part" "$file"
