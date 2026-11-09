#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Makes the shared fonts' Noto Sans stand-ins (see docs/platforms/nx/filesystem.md).

    tools/subset_fonts.py --out DIR

Each is Noto Sans JP, SC, TC or KR, instantiated at weight 400, kept to ASCII, Latin-1, common
punctuation and the script's standard set (JIS X 0208, GB 2312, Big5, KS X 1001), with hinting and the
variation tables dropped: a TrueType glyf font small enough for the 0x1100000-byte shared memory the
console's SDK maps. Needs fontTools and network access; the build fetches the files this wrote from where
they were published (cmake/dependencies.cmake).
"""

import argparse
import os
import urllib.request

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

# Google Fonts, the commit the published files came from
FONTS = 'https://raw.githubusercontent.com/google/fonts/e7db6ca603fd4862010383ff17ffd6cc4ffac1c0/ofl'
SOURCES = [
    ('notosansjp', 'NotoSansJP[wght].ttf', 'shift_jis', 'NotoSansJP-Subset.ttf'),
    ('notosanssc', 'NotoSansSC[wght].ttf', 'gb2312', 'NotoSansSC-Subset.ttf'),
    ('notosanstc', 'NotoSansTC[wght].ttf', 'big5', 'NotoSansTC-Subset.ttf'),
    ('notosanskr', 'NotoSansKR[wght].ttf', 'euc_kr', 'NotoSansKR-Subset.ttf'),
]
# the punctuation a game's UI uses besides the script's own set
PUNCTUATION = [0xA5, 0x2026, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x3000, 0x3001, 0x3002, 0xFF01, 0xFF0C, 0xFF1F]


def codec_chars(codec):
    """every character the codec's two-byte codes decode to, its standard character set"""
    found = set()
    for high in range(0x81, 0xFF):
        for low in range(0x40, 0xFF):
            try:
                text = bytes([high, low]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(text) == 1:
                found.add(ord(text))
    return found


def make(name, font_path, codec, out_path):
    font = TTFont(font_path)
    instantiateVariableFont(font, {'wght': 400}, inplace=True)
    options = subset.Options()
    options.layout_features = []
    options.name_IDs = ['*']
    options.notdef_outline = True
    options.recalc_bounds = True
    options.hinting = False
    options.drop_tables += ['DSIG', 'MVAR', 'STAT', 'fvar', 'gvar', 'avar', 'HVAR', 'VVAR']
    codes = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100)) | set(PUNCTUATION) | codec_chars(codec)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codes)
    subsetter.subset(font)
    font.save(out_path)
    print('%s: %u characters, %u bytes' % (name, len(font.getBestCmap()), os.path.getsize(out_path)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--out', required=True)
    parser.add_argument('--cache', default=os.path.join(os.path.expanduser('~'), '.cache', 'scnx-fonts'))
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.cache, exist_ok=True)
    for folder, file, codec, out in SOURCES:
        path = os.path.join(args.cache, file)
        if not os.path.exists(path):
            url = '%s/%s/%s' % (FONTS, folder, file.replace('[', '%5B').replace(']', '%5D'))
            print('fetching', url)
            urllib.request.urlretrieve(url, path)
        make(file, path, codec, os.path.join(args.out, out))


if __name__ == '__main__':
    main()
