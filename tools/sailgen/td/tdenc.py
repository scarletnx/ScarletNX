#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""tdenc: a host ISA's instruction encoders from LLVM's TableGen records, for sailgen's machine back ends.

    llvm-tblgen --dump-json -I llvm/include -I llvm/lib/Target/T llvm/lib/Target/T/T.td -o T.json
    tdenc.py T.json PREFIX NAMES OUT [--source TEXT] [--decode DECODE_OUT]

Each record named in NAMES (one per line, '#' comments; NAME=ALIAS renames its encoder) becomes

    static inline SnxU32 sge_PREFIX_alias(SnxU32 field, ...)   (SnxU64 where the ISA has longer words)

returning the instruction's word with each field's bits placed where the record's Inst puts them: the fields in the
order the record's operands name them (outputs, then inputs, a memory operand's parts in its own order), any other
field of Inst after them. A field's value is what the record's operand encodes (a branch's offset as the target's
operand holds it: RISC-V's in halfwords, LoongArch's and MIPS's in words, s390x's in halfwords). The records' bits are
LLVM's, so nothing about an encoding is written by hand; the output is checked in (the build needs no LLVM), and
names the LLVM it came from (--source).

--decode writes the same records' decode table too, for a test's interpreter of the ISA: per record, in NAMES' order,

    {"alias", mask, match}   (a word is the record's where (word & mask) == match: its fixed bits)

and an enum SGD_PREFIX_ALIAS of the indices. A record whose fixed bits another's include comes first, so the first
match is the most specific.
"""
import json
import re
import sys


def operand_fields(rec):
    """The record's operand names in order: outputs, inputs, a dag's arguments flattened."""
    names = []

    def walk(args):
        for arg in args or []:
            val, name = arg[0], arg[1]
            if isinstance(val, dict) and val.get("kind") == "dag":
                walk(val.get("args"))
            elif name:
                names.append(name)

    walk(rec.get("OutOperandList", {}).get("args"))
    walk(rec.get("InOperandList", {}).get("args"))
    return names


def fixed_bits(rec, reverse):
    inst = rec.get("Inst")
    size = rec.get("Size") or 0
    if reverse:
        inst = list(reversed(inst))
    if size and size * 8 < len(inst):
        inst = inst[: size * 8]
    mask = match = 0
    for pos, bit in enumerate(inst):
        if bit in (0, 1):
            mask |= 1 << pos
            match |= bit << pos
    return mask, match


def encoder(rec, name, prefix, reverse, wide):
    inst = rec.get("Inst")
    size = rec.get("Size") or 0
    if not inst:
        raise SystemExit("tdenc: %s has no Inst" % name)
    n = len(inst)
    if reverse:
        inst = list(reversed(inst))
    if size and size * 8 != n:
        # a record whose Inst is wider than the instruction (LLVM's SoftFail and the like): its low bytes
        inst = inst[: size * 8] if size * 8 < n else inst
        n = len(inst)
    fixed = 0
    places = {}  # field -> [(bit in word, bit of field)]
    for pos, bit in enumerate(inst):
        if isinstance(bit, dict):
            var, idx = bit.get("var"), bit.get("index", 0)
            if var is None:
                raise SystemExit("tdenc: %s bit %d is %r" % (name, pos, bit))
            places.setdefault(var, []).append((pos, idx))
        elif bit == 1:
            fixed |= 1 << pos
        elif bit in (0, None):
            pass
        else:
            raise SystemExit("tdenc: %s bit %d is %r" % (name, pos, bit))
    order = [f for f in operand_fields(rec) if f in places]
    order += [f for f in places if f not in order]
    seen = set()
    order = [f for f in order if not (f in seen or seen.add(f))]
    word = "SnxU64" if wide else "SnxU32"
    params = ", ".join("SnxU32 %s" % f for f in order) or "void"
    body = ["    %s w = 0x%xull;" % (word, fixed) if wide else "    %s w = 0x%xu;" % (word, fixed)]
    for f in order:
        # runs of consecutive bits: (word bit, field bit, length)
        runs = []
        for pos, idx in sorted(places[f], key=lambda p: p[1]):
            if runs and runs[-1][0] + runs[-1][2] == pos and runs[-1][1] + runs[-1][2] == idx:
                runs[-1][2] += 1
            else:
                runs.append([pos, idx, 1])
        for pos, idx, length in runs:
            mask = (1 << length) - 1
            src = "(%s)%s" % (word, f)
            if idx:
                src = "(%s >> %d)" % (src, idx)
            term = "(%s & 0x%xu)" % (src, mask)
            if pos:
                term = "%s << %d" % (term, pos)
            body.append("    w |= %s;" % term)
    body.append("    return w;")
    asm = (rec.get("AsmString") or "").replace("\t", " ")
    asm = re.sub(r"\{([^|}]*)\|[^}]*\}", r"\1", asm)
    head = "/* %s: %s (%d bytes) */\n" % (rec["!name"], asm.strip(), n // 8)
    fn = "static inline %s sge_%s_%s(%s) {\n" % (word, prefix, name, params)
    return head + fn + "\n".join(body) + "\n}\n"


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 2
    js, prefix, names_path, out = argv[1:5]
    source = argv[argv.index("--source") + 1] if "--source" in argv else "LLVM"
    with open(js) as f:
        d = json.load(f)
    reverse = 0
    for info in d.get("!instanceof", {}).get("InstrInfo", []):
        reverse = d[info].get("isLittleEndianEncoding", 0)
    wanted = []
    for line in open(names_path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        rec, _, alias = line.partition("=")
        wanted.append((rec.strip(), (alias.strip() or rec.strip()).lower()))
    sizes = [d[r].get("Size") or len(d[r].get("Inst") or []) // 8 for r, _ in wanted if r in d]
    wide = any(s > 4 for s in sizes)
    parts = []
    for rec, alias in wanted:
        if rec not in d:
            raise SystemExit("tdenc: no record %s" % rec)
        parts.append(encoder(d[rec], alias, prefix, reverse, wide))
    guard = "SNX_SAILGEN_%s_ENC_INC" % prefix.upper()
    with open(out, "w") as f:
        f.write("/* SPDX-License-Identifier: GPL-3.0-or-later\n   Copyright 2026 the ScarletNX contributors */\n")
        f.write("/* Generated by tools/sailgen/td/tdenc.py from %s's TableGen records\n   (%s): do not edit.\n" % (
            prefix, source))
        f.write("   The %s encoders the back end names in %s */\n" % (prefix, names_path.split("/")[-1]))
        f.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
        f.write("\n".join(parts))
        f.write("\n#endif\n")
    if "--decode" in argv:
        dec = argv[argv.index("--decode") + 1]
        rows = [(alias,) + fixed_bits(d[rec], reverse) for rec, alias in wanted]
        # the most specific first: more fixed bits before fewer
        rows.sort(key=lambda r: -bin(r[1]).count("1"))
        guard = "SNX_SAILGEN_%s_DEC_INC" % prefix.upper()
        with open(dec, "w") as f:
            f.write("/* SPDX-License-Identifier: GPL-3.0-or-later\n   Copyright 2026 the ScarletNX contributors */\n")
            f.write("/* Generated by tools/sailgen/td/tdenc.py from %s's TableGen records\n   (%s): do not edit.\n" % (
                prefix, source))
            f.write("   The decode table of the %s records %s names: a word is a row's where its fixed bits match */\n" % (
                prefix, names_path.split("/")[-1]))
            f.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
            f.write("enum {\n")
            for k, r in enumerate(rows):
                f.write("    SGD_%s_%s = %d,\n" % (prefix.upper(), r[0].upper(), k))
            f.write("    SGD_%s_COUNT\n};\n" % prefix.upper())
            f.write("static const struct {\n    const char* name;\n    unsigned mask, match;\n} sgd_%s[] = {\n" % prefix)
            for r in rows:
                f.write("    {\"%s\", 0x%08xu, 0x%08xu},\n" % r)
            f.write("};\n\n#endif\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
