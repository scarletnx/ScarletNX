#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Every Vulkan device extension a renderer can make its device with has a setting, vk_ext_<name>: its
`setting @NAME` line in src/nx/runtime/settings.txt, which the extension bans and the player's on and off go through.
A renderer is each source of src/ that makes a device (snx_vk_device_create or vkCreateDevice); what it can turn on is
each device extension its code names, a VK_*_EXTENSION_NAME or a string that is the extension's name, in it or in a
header of the repo it includes, comments left out. The registry (vk.xml) says which names are device extensions and
the name each macro stands for. The extensions without a line are listed with the files that name them, and the run
fails.

usage: vk_extension_settings.py --repo REPO --registry vk.xml"""

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

MAKERS = re.compile(r'\b(snx_vk_device_create|vkCreateDevice)\s*\(')
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
MACRO = re.compile(r'\b(VK_\w+_EXTENSION_NAME)\b')
LITERAL = re.compile(r'"(VK_[A-Za-z0-9_]+)"')


def code(text):
    """text without its comments, its strings kept"""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            i = n if end < 0 else end + 2
            out.append(' ')
        elif text.startswith('//', i):
            end = text.find('\n', i)
            i = n if end < 0 else end
        elif text[i] in '"\'':
            j = i + 1
            while j < n and text[j] != text[i] and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i : j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def registry(path):
    """the registry's Vulkan device extensions, and the name each *_EXTENSION_NAME macro stands for"""
    root = ET.parse(path).getroot()
    devices, macros = set(), {}
    for extension in root.find('extensions'):
        if extension.get('type') == 'device' and 'vulkan' in (extension.get('supported') or '').split(','):
            devices.add(extension.get('name'))
        for enum in extension.iter('enum'):
            value = enum.get('value') or ''
            if enum.get('name', '').endswith('_EXTENSION_NAME') and value.startswith('"'):
                macros[enum.get('name')] = value.strip('"')
    return devices, macros


def renderers(src):
    """each source of src/ that makes a device, but the one that defines snx_vk_device_create"""
    for path in sorted(src.rglob('*.c')):
        text = code(path.read_text(encoding='utf-8', errors='replace'))
        if MAKERS.search(text) and 'VkResult snx_vk_device_create(' not in text:
            yield path


def named(path, src, devices, macros, seen):
    """the device extensions path names, and those the repo's headers it includes name, each once"""
    if path in seen or not path.is_file():
        return set()
    seen.add(path)
    text = code(path.read_text(encoding='utf-8', errors='replace'))
    names = {macros[m] for m in MACRO.findall(text) if m in macros} | set(LITERAL.findall(text))
    names = {name for name in names if name in devices}
    for include in INCLUDE.findall(text):
        for base in (src, path.parent):
            names |= named((base / include).resolve(), src, devices, macros, seen)
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--registry', required=True, type=Path)
    args = parser.parse_args()
    src = (args.repo / 'src').resolve()
    devices, macros = registry(args.registry)
    settings = set(re.findall(r'^setting @(VK_\w+)', (src / 'nx/runtime/settings.txt').read_text(), re.M))
    missing, makers = {}, 0
    for path in renderers(src):
        makers += 1
        for name in named(path, src, devices, macros, set()):
            if name not in settings:
                missing.setdefault(name, []).append(str(path.relative_to(args.repo.resolve())))
    if not makers:
        print('no source of src/ makes a Vulkan device: nothing was checked')
        return 1
    for name in sorted(missing):
        print(
            f'{name} has no setting (a "setting @{name}" line in src/nx/runtime/settings.txt), and '
            f'{", ".join(missing[name])} can make the device with it'
        )
    print(f'{makers} sources make a device; {len(missing)} of their extensions have no setting')
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
