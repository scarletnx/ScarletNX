# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Each hook of src/nx/hle/hook/ext_skyline.hooks, a mod's trampoline's native hook, is the function's own native hook
for a 64-bit game: the same symbol, argument types, result, handler and SDK range as a 64-bit hook of the native table
(hle/hook/hooks.hook, nn/*.hook and patches/*.hook, cmake/hookgen.cmake's snx_hookgen) declares it. hook.c gives the
trampoline's slot that handler, with the arguments the mod's call of the original passes, and hook_select picks the
hook by the SDK's version as the native table does."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NX = ROOT / "src" / "nx"
EXT = NX / "hle" / "hook" / "ext_skyline.hooks"
# the native table's files: nvn.hook is a table of its own, whose symbols never reach the trampolines' table
NATIVE = [NX / "hle" / "hook" / "hooks.hook", *sorted((NX / "nn").glob("*.hook")), *sorted((NX / "patches").glob("*.hook"))]
# a hook line: what comes before "hook" (an @version for 64-bit games, or "a32" for a 32-bit game's hook alone), then
# the symbol, the arguments, the result and the handler. What comes after the handler (a32 and its @version) is for
# 32-bit games
HOOK = re.compile(r"^\s*(?P<pre>.*?)\bhook\s+(?P<sym>\S+?)\((?P<args>.*?)\)\s*(?:->\s*(?P<ret>\S+))?\s*=\s*(?P<fn>\w+)")


def arg_types(args):
    """the types of an argument list, without the parameters' names, annotations and spacing around *"""
    types = []
    for a in filter(None, (x.strip() for x in args.split(","))):
        a = re.sub(r"\s*\*\s*", "* ", a)
        words = [w for w in a.split() if not re.fullmatch(r"\w+\[.*\]", w)]  # in[size], out[size]
        if len(words) > 1 and re.fullmatch(r"[A-Za-z_]\w*", words[-1]):
            words = words[:-1]  # the parameter's name
        types.append(" ".join(words).replace("* ", "*").strip())
    return tuple(types)


def declarations(path):
    """the (symbol, argument types, result, handler, version) of each 64-bit hook line of path"""
    found = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#"):
            continue
        m = HOOK.search(line)
        if not m:
            continue
        pre = m.group("pre").split()
        if "a32" in pre:  # a 32-bit game's hook alone
            continue
        version = " ".join(w for w in m.group("pre").split() if w != "a32")
        found.append((m.group("sym"), arg_types(m.group("args")), m.group("ret") or "", m.group("fn"), version))
    return found


def main():
    native = {}
    for path in NATIVE:
        for d in declarations(path):
            native.setdefault(d[0], []).append(d)
    ext = declarations(EXT)
    if not ext:
        print(f"FAIL: {EXT} has no hook")
        return 1
    failures = 0
    for d in ext:
        own = native.get(d[0], [])
        if not own:
            print(f"FAIL: {d[0]} has no 64-bit hook in the native table")
            failures += 1
        elif d not in own:
            print(f"FAIL: {d[0]} is declared {d[1:]} in ext_skyline.hooks, but its native hooks are {[n[1:] for n in own]}")
            failures += 1
    print(f"{len(ext)} ext_skyline hooks, {failures} not as their native hooks")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
