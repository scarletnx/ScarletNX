#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Orders a game module's functions by a profile, for the recompiler's --layout.

    tools/layout.py record PID SECONDS perf.data
    tools/layout.py order perf.data GAME_code.so OUT.layout
    build/scarletnx -r work/Game --layout OUT.layout

record samples the game's user cycles for SECONDS with the last branch records of the CPU (perf record -j any,u: Intel
LBR, AMD BRS or LBRv2). Run it in the scene to make fast, and only then: the order is as good as the scene is like
play. order reads the branch records, names their ends by the module's symbols (llvm-nm), and writes the guest
addresses of the module's functions (F_<address>) in the order their code should take.

The order is Call-Chain Clustering (C3: Ottoni and Maher, "Optimizing Function Placement for Large-Scale Data-Center
Applications", CGO 2017), as lld's CallGraphSort.cpp has it. Each function, the densest first, goes after the function
that calls it most, unless the merged cluster passes 1 MiB or its density falls below an eighth of the caller's. Then
the clusters go in the order of their densities. The heat of a function is the number of taken branches that land in
it, and its density that heat over its code's size: the branch records count what runs, where cycle samples count
where the CPU waits. A call or tail call is a branch from one function to the first instruction of another. A return
lands inside its caller, so it is no edge.

Nothing here leaves the machine: the profile and the order are the game's.
"""

import bisect
import os
import re
import subprocess
import sys

MAX_CLUSTER_SIZE = 1 << 20
MAX_DENSITY_DEGRADATION = 8
ALIGN = 16  # each function's alignment in the module


def record(pid, seconds, out):
    return subprocess.call(['perf', 'record', '-q', '-j', 'any,u', '-e', 'cycles:u', '-c', '1000003', '-p', str(pid),
                            '-o', out, '--', 'sleep', str(seconds)])


def symbols(module):
    """(starts, ends, names) of the module's code symbols, by address"""
    out = subprocess.run(['llvm-nm', '-n', '-S', '--defined-only', module], capture_output=True, text=True,
                         check=True).stdout
    starts, ends, names = [], [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) != 4 or p[2] not in 'tT':
            continue
        start, size = int(p[0], 16), int(p[1], 16)
        starts.append(start)
        ends.append(start + size)
        names.append(p[3])
    return starts, ends, names


MMAP = re.compile(r'PERF_RECORD_MMAP2? .*\[(0x[0-9a-f]+)\((0x[0-9a-f]+)\) @ (0x[0-9a-f]+|0)\b.*\]: r-xp (.+)$')
BRANCH = re.compile(r'(0x[0-9a-f]+)/(0x[0-9a-f]+)/')


def profile(data, module):
    """heat per symbol index, and calls[(caller, callee)], from the branch records of perf.data"""
    starts, ends, names = symbols(module)
    real = os.path.realpath(module)
    base = None
    heat, calls = {}, {}
    records = outside = 0

    def find(address):
        i = bisect.bisect_right(starts, address) - 1
        return i if i >= 0 and address < ends[i] else -1

    proc = subprocess.Popen(['perf', 'script', '-i', data, '-F', 'brstack', '--show-mmap-events'],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1 << 20)
    for line in proc.stdout:
        if 'PERF_RECORD_MMAP' in line:
            m = MMAP.search(line)
            # the module's code is at the same offset in its file as in its image (link_elf.c)
            if m and os.path.realpath(m.group(4).strip()) == real:
                base = int(m.group(1), 16) - int(m.group(3), 16)
            continue
        if base is None:
            continue
        for m in BRANCH.finditer(line):
            records += 1
            to = int(m.group(2), 16) - base
            t = find(to)
            if t < 0:
                outside += 1
                continue
            heat[t] = heat.get(t, 0) + 1
            if to != starts[t]:
                continue
            f = find(int(m.group(1), 16) - base)
            if f >= 0 and f != t:
                calls[(f, t)] = calls.get((f, t), 0) + 1
    proc.wait()
    if base is None:
        sys.exit('layout.py: %s is not mapped in %s' % (module, data))
    print('%d branch records, %d of them land outside the module' % (records, outside), file=sys.stderr)
    return starts, ends, names, heat, calls


def order(starts, ends, names, heat, calls):
    """C3 over the guest functions (F_*): the symbol indices in the order of the code"""
    guest = {i for i in heat if names[i].startswith('F_')}
    size = {i: max(ends[i] - starts[i], 1) + ALIGN - 1 & ~(ALIGN - 1) for i in guest}
    best = {}  # callee -> (weight, caller), its most frequent caller
    calls_in = {}
    for (f, t), w in calls.items():
        if f in guest and t in guest:
            calls_in[t] = calls_in.get(t, 0) + w
            if w > best.get(t, (0, -1))[0]:
                best[t] = (w, f)
    leader = {i: i for i in guest}
    members = {i: [i] for i in guest}
    weight = {i: heat[i] for i in guest}
    csize = dict(size)

    def find(i):
        while leader[i] != i:
            leader[i] = leader[leader[i]]
            i = leader[i]
        return i

    for t in sorted(guest, key=lambda i: (-heat[i] / size[i], starts[i])):
        if t not in best or best[t][0] * 10 <= calls_in[t]:
            continue  # no caller, or none that makes a tenth of the calls
        p, c = find(best[t][1]), find(t)
        if p == c or csize[p] + csize[c] > MAX_CLUSTER_SIZE:
            continue
        merged = (weight[p] + weight[c]) / (csize[p] + csize[c])
        if merged < weight[p] / csize[p] / MAX_DENSITY_DEGRADATION:
            continue
        leader[c] = p
        members[p] += members.pop(c)
        weight[p] += weight.pop(c)
        csize[p] += csize.pop(c)
    clusters = sorted(members, key=lambda c: (-weight[c] / csize[c], starts[c]))
    return [i for c in clusters for i in members[c]], size


def main():
    if len(sys.argv) == 5 and sys.argv[1] == 'record':
        return record(int(sys.argv[2]), float(sys.argv[3]), sys.argv[4])
    if len(sys.argv) != 5 or sys.argv[1] != 'order':
        sys.exit(__doc__)
    starts, ends, names, heat, calls = profile(sys.argv[2], sys.argv[3])
    functions, size = order(starts, ends, names, heat, calls)
    total = sum(heat[i] for i in functions)
    with open(sys.argv[4], 'w') as out:
        for i in functions:
            out.write('%s\n' % names[i][2:])
    # how much code holds how much of the heat, when the hottest functions are packed first
    marks, done, used, line = [0.5, 0.9, 0.99, 1.0], 0, 0, []
    for i in sorted(functions, key=lambda i: -heat[i] / size[i]):
        done += heat[i]
        used += size[i]
        while marks and done >= marks[0] * total:
            line.append('%g%% in %.2f MiB' % (marks.pop(0) * 100, used / 1048576))
    print('%d functions, %d calls between them; %s' % (len(functions), sum(calls.values()), ', '.join(line)),
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
