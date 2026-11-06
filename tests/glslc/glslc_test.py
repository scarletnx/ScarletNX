# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""glslc on whole shaders, beside the host's tools.

    glslc_test.py errors   --cli CLI [--glslang G]
    glslc_test.py shaders  --cli CLI --spirv-val V --spirv-dis D [--glslang G]
    glslc_test.py keywords --cli CLI --glslang G
    glslc_test.py semantics --cli CLI --spirv-opt O --spirv-dis D --glslang G
    glslc_test.py sources

errors: every case of errors.glsl is refused with its line, column and message, and glslangValidator refuses it too,
but where the case says glslang accepts it (an error glslang only finds when it links), which is checked as well.
shaders: every shader of shaders/ compiles to SPIR-V that spirv-val takes for Vulkan 1.1, also through the CLI's
binding map; glslangValidator takes the
shader too (or refuses it where its first lines say so), and for GLSL ES 3.10 and later lays out every block, member
by member, as glslc does, with the same interface decorations, but where a line "// glslang -V: WHY" says its
module is no reference. keywords: every word glslc's table knows is a name, a keyword or a reserved word in the
same GLSL ES versions for glslc and for glslangValidator. semantics: each shader of semantics/ stores expressions on
constants in slots of a buffer; spirv-opt folds glslc's module and glslangValidator's into the constants they store,
which must be the same in every slot both fold (and both must fold enough of them for the test to mean something).
A shader whose second line is "// compare: samples" is compared another way: spirv-opt inlines glslc's module and
glslangValidator -R's (which gives a struct's samplers variables of their own too), and each texture call must read
the same sampler at the same coordinate, and the same uniform members be read, named by their paths.
sources: src/glslc calls no allocator: a compilation uses the caller's workspace only. Without glslangValidator the
comparisons are left out, and the test says so.
"""

import argparse
import concurrent.futures
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXTENSIONS = ('.vert', '.frag', '.comp')


def run(command):
    return subprocess.run(command, check=False, capture_output=True, text=True)  # the callers read the status


def version_of(source):
    match = re.search(r'^\s*#\s*version\s+(\d+)', source, re.MULTILINE)
    return int(match.group(1)) if match else 100


def ours_failures(cli, paths):
    """glslc on many files at once: each refused file's LINE:COLUMN: MESSAGE"""
    out = {}
    for line in run([cli, *map(str, paths)]).stdout.splitlines():
        path, sep, rest = line.partition(':')
        while sep and path not in out and not rest[:1].isdigit():
            more, sep, rest = rest.partition(':')
            path += ':' + more
        if sep:
            out[path] = rest
    return out


def glslang_failures(glslang, paths):
    """glslangValidator on many files at once, which it compiles one by one, naming each before its errors"""
    names = {str(p) for p in paths}
    failed, current = set(), None
    for line in run([glslang, *map(str, paths)]).stdout.splitlines():
        if line in names:
            current = line
        elif line.startswith('ERROR') and current:
            failed.add(current)
    return failed


def glslang_command(glslang, path, output=None):
    """glslangValidator checking the language only, or with an output, compiling it for Vulkan (3.10 and later)"""
    if output:
        return [glslang, '-V', '--target-env', 'vulkan1.1', '-o', str(output), str(path)]
    return [glslang, str(path)]


def check_errors(args, failures):
    text = (HERE / 'errors.glsl').read_text(encoding='utf-8')
    cases = re.split(r'^--- (?=\S+\.(?:vert|frag|comp) )', text, flags=re.MULTILINE)[1:]
    if not cases:
        failures.append('errors.glsl has no cases')
    with tempfile.TemporaryDirectory(prefix='snx-glslc-errors-') as directory:
        parsed = []
        for case in cases:
            header, _, body = case.partition('\n')
            name, want = header.split(' ', 1)
            accepts = None
            if body.startswith('--- glslang accepts: '):
                accepts, _, body = body.partition('\n')
                accepts = accepts[len('--- glslang accepts: ') :]
            path = Path(directory) / name
            path.write_text(body, encoding='utf-8')
            parsed.append((name, want, accepts, path))
        paths = [path for _, _, _, path in parsed]
        ours = ours_failures(args.cli, paths)
        theirs = glslang_failures(args.glslang, paths) if args.glslang else set()
        for name, want, accepts, path in parsed:
            got = ours.get(str(path), 'nothing: glslc compiles it')
            if got != want:
                failures.append(f'{name}: glslc gives "{got}", and the case wants "{want}"')
            if args.glslang:
                refused = str(path) in theirs
                if accepts is None and not refused:
                    failures.append(f'{name}: glslangValidator accepts it, and glslc refuses it')
                if accepts is not None and refused:
                    failures.append(f'{name}: glslangValidator refuses it, and the case says it accepts it ({accepts})')
    print(f'errors: {len(cases)} cases refused as errors.glsl says')


def layouts(dis):
    """every block's member offsets, matrix strides and majorness, and its arrays' strides, by path from the block"""
    names, structs, arrays, member, stride, blocks = {}, {}, {}, {}, {}, []
    for line in dis.splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'OpName':
            names[t[1]] = line.split('"')[1]
        if len(t) > 2 and t[1] == '=' and t[2] == 'OpTypeStruct':
            structs[t[0]] = t[3:]
        if len(t) > 3 and t[1] == '=' and t[2] in ('OpTypeArray', 'OpTypeRuntimeArray'):
            arrays[t[0]] = t[3]
        if t[0] == 'OpMemberDecorate' and t[3] in ('Offset', 'MatrixStride', 'RowMajor', 'ColMajor'):
            member.setdefault(t[1], {})[(int(t[2]), t[3])] = t[4] if len(t) > 4 else ''
        if t[0] == 'OpDecorate' and t[2] == 'ArrayStride':
            stride[t[1]] = t[3]
        if t[0] == 'OpDecorate' and t[2] == 'Block':
            blocks.append(t[1])
    out = {}

    def walk(type_id, path, depth):
        if depth > 16:
            return
        if type_id in arrays:
            out[path + '[] stride'] = stride.get(type_id)
            walk(arrays[type_id], path + '[]', depth + 1)
        elif type_id in structs:
            for (index, kind), value in member.get(type_id, {}).items():
                out[f'{path}.{index} {kind}'] = value
            for index, child in enumerate(structs[type_id]):
                walk(child, f'{path}.{index}', depth + 1)

    for block in blocks:
        walk(block, names.get(block, block), 0)
    return out


INTERFACE = ('Location', 'Binding', 'DescriptorSet', 'BuiltIn', 'Flat', 'Centroid', 'Sample', 'Invariant')


def interface(dis):
    """the capabilities, and each named variable's interface decorations"""
    names = {m.group(1): m.group(2) for m in re.finditer(r'OpName %(\d+) "([^"]*)"', dis)}
    capabilities = sorted(re.findall(r'OpCapability (\w+)', dis))
    decorations = {}
    for m in re.finditer(r'OpDecorate %(\d+) (\w+) ?(\w*)', dis):
        name = names.get(m.group(1), '')
        if name and m.group(2) in INTERFACE:
            decorations.setdefault(name, set()).add(f'{m.group(2)} {m.group(3)}'.strip())
    # an in or out block's members (gl_PerVertex is glslang's own block of built-ins)
    for m in re.finditer(r'OpMemberDecorate %(\d+) (\d+) (\w+) ?(\w*)', dis):
        name = names.get(m.group(1), '')
        if name and not name.startswith('gl_') and m.group(3) in ('Location', 'Flat', 'Centroid', 'Sample'):
            decorations.setdefault(f'{name}.{m.group(2)}', set()).add(f'{m.group(3)} {m.group(4)}'.strip())
    return capabilities, decorations


def check_shaders(args, failures):
    shaders = sorted(p for p in (HERE / 'shaders').iterdir() if p.suffix in EXTENSIONS)
    compared = 0
    with tempfile.TemporaryDirectory(prefix='snx-glslc-shaders-') as directory:
        for shader in shaders:
            source = shader.read_text(encoding='utf-8')
            spv = Path(directory) / (shader.name + '.spv')
            result = run([args.cli, '-o', str(spv), str(shader)])
            if result.returncode != 0:
                failures.append(f'{shader.name}: glslc refuses it: {result.stdout.strip()}')
                continue
            valid = run([args.spirv_val, '--target-env', 'vulkan1.1', str(spv)])
            if valid.returncode != 0:
                failures.append(f'{shader.name}: spirv-val refuses the module: {(valid.stdout + valid.stderr).strip()}')
            mapped = Path(directory) / (shader.name + '.mapped.spv')  # through a binding map: each kind its own set
            result = run([args.cli, '-m', '-o', str(mapped), str(shader)])
            valid = run([args.spirv_val, '--target-env', 'vulkan1.1', str(mapped)])
            if result.returncode != 0 or valid.returncode != 0:
                failures.append(f'{shader.name}: with a binding map: {result.stdout.strip()} {valid.stdout.strip()}')
            if not args.glslang:
                continue
            note = re.search(r'^// glslang: rejects \((.*)\)$', source, re.MULTILINE)
            # a line "// glslang -V: WHY" keeps glslang to the language: its Vulkan module is no reference there
            vulkan = version_of(source) >= 310 and not re.search(r'^// glslang -V: ', source, re.MULTILINE)
            ref_spv = Path(directory) / (shader.name + '.ref.spv')
            reference = run(glslang_command(args.glslang, shader, ref_spv if vulkan else None))
            if note and reference.returncode == 0:
                failures.append(f'{shader.name}: glslangValidator accepts it, and its first lines say it refuses it')
            if not note and reference.returncode != 0:
                failures.append(f'{shader.name}: glslangValidator refuses it: {reference.stdout.strip()[:300]}')
            if vulkan and reference.returncode == 0:
                ours_dis = run([args.spirv_dis, '--raw-id', str(spv)]).stdout
                theirs_dis = run([args.spirv_dis, '--raw-id', str(ref_spv)]).stdout
                ours, theirs = layouts(ours_dis), layouts(theirs_dis)
                ours_caps, ours_vars = interface(ours_dis)
                theirs_caps, theirs_vars = interface(theirs_dis)
                if ours_caps != theirs_caps:
                    failures.append(
                        f'{shader.name}: capabilities {ours_caps} in glslc, {theirs_caps} in glslangValidator'
                    )
                for name, decorations in theirs_vars.items():  # glslang leaves an anonymous block's variable unnamed
                    if ours_vars.get(name) != decorations:
                        failures.append(
                            f'{shader.name}: {name} is {sorted(ours_vars.get(name, ()))} in glslc and '
                            f'{sorted(decorations)} in glslangValidator'
                        )
                for key in sorted(set(ours) | set(theirs)):
                    if ours.get(key) != theirs.get(key):
                        failures.append(
                            f'{shader.name}: {key} is {ours.get(key)} in glslc and {theirs.get(key)} in glslangValidator'
                        )
                compared += len(ours)
    print(
        f'shaders: {len(shaders)} compiled and valid for Vulkan 1.1; {compared} block layout entries, the capabilities '
        'and the interface compared'
    )


def keyword_words():
    """the words of glslc's keyword table (src/glslc/builtins.c)"""
    text = (HERE.parent.parent / 'src' / 'glslc' / 'builtins.c').read_text(encoding='utf-8')
    table = text[text.index('const GlslcKeyword glslc_keywords[] = {') :]
    table = table[: table.index('};')]
    return re.findall(r'\{"(\w+)", ', table)


def check_keywords(args, failures):
    words = keyword_words()
    versions = ('100', '300 es', '310 es', '320 es')
    with tempfile.TemporaryDirectory(prefix='snx-glslc-keywords-') as directory:

        def one(version):
            paths = []
            for word in words:
                path = Path(directory) / f'{word}-{version.split()[0]}.frag'
                path.write_text(f'#version {version}\nprecision mediump float;\nint {word};\nvoid main() {{}}\n')
                paths.append(path)
            return version, paths, ours_failures(args.cli, paths), glslang_failures(args.glslang, paths)

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            for version, paths, ours, theirs in pool.map(one, versions):
                for word, path in zip(words, paths):
                    took, they_took = str(path) not in ours, str(path) not in theirs
                    if took != they_took:
                        failures.append(
                            f'"int {word};" in GLSL ES {version}: glslc {"takes" if took else "refuses"} it, and '
                            f'glslangValidator {"takes" if they_took else "refuses"} it'
                        )
    print(f'keywords: {len(words)} words in 4 versions, the same as glslangValidator')


def folded_stores(dis):
    """each store of a constant through a constant access chain: (variable, indices...) -> the constant, written out"""
    constants, chains, stored = {}, {}, {}
    for line in dis.splitlines():
        t = line.split()
        if len(t) > 3 and t[1] == '=':
            if t[2] == 'OpConstant':
                constants[t[0]] = t[4]
            elif t[2] in ('OpConstantTrue', 'OpConstantFalse'):
                constants[t[0]] = t[2][len('OpConstant') :].lower()
            elif t[2] == 'OpConstantComposite':
                constants[t[0]] = t[4:]
            elif t[2] == 'OpAccessChain':
                chains[t[0]] = t[4:]
        elif len(t) > 2 and t[0] == 'OpStore' and t[1] in chains:
            indices = chains[t[1]][1:]
            if all(isinstance(constants.get(i), str) for i in indices):
                stored[tuple(constants[i] for i in indices)] = t[2]

    def value(i, depth=0):
        c = constants.get(i)
        if c is None or depth > 8:
            return None
        if isinstance(c, str):
            return c
        parts = [value(x, depth + 1) for x in c]
        return None if None in parts else '(' + ', '.join(parts) + ')'

    return {slot: value(v) for slot, v in stored.items()}


SAMPLES = '// compare: samples'
NUMERIC = ('OpTypeFloat', 'OpTypeInt', 'OpTypeBool', 'OpTypeVector', 'OpTypeMatrix')


def reads(dis):
    """what each image instruction samples, at which coordinate, and which members of the default uniform block are
    read: each a path from its variable by the names of the module (glslang's mats[1].a written as glslc's mats.a[1])"""
    names, members, defs = {}, {}, {}
    for line in dis.splitlines():
        t = line.split()
        if len(t) > 2 and t[0] == 'OpName':
            names[t[1]] = line.split('"')[1]
        elif len(t) > 3 and t[0] == 'OpMemberName':
            members[(t[1], int(t[2]))] = line.split('"')[1]
        elif len(t) > 2 and t[1] == '=':
            defs[t[0]] = t[2:]

    def walk(path, type_id, steps):
        for step in steps:
            d = defs.get(type_id, [])
            if d[:1] == ['OpTypeStruct'] and step is not None:
                name = members.get((type_id, int(step)), step)
                path, type_id = (f'{path}.{name}' if path else name), d[1 + int(step)]
            elif d[:1] in (['OpTypeArray'], ['OpTypeRuntimeArray'], ['OpTypeVector'], ['OpTypeMatrix']):
                path, type_id = f'{path}[{"?" if step is None else step}]', d[1]
            else:
                return None, None
        return path, type_id

    def value(i, depth=0):
        """(path, type, root variable) of i, or (None, None, None) when it is no read"""
        d = defs.get(i, [])
        if depth > 32 or not d:
            return None, None, None
        if d[0] == 'OpVariable':
            pointee = defs.get(d[1], [None, None, None])[2]
            return re.sub(r'^(\w+)\[(\d+)\]\.(\w+)$', r'\1.\3[\2]', names.get(i, '?')), pointee, i
        if d[0] in ('OpAccessChain', 'OpInBoundsAccessChain', 'OpCompositeExtract'):
            path, type_id, root = value(d[2], depth + 1)
            if path is None:
                return None, None, None
            if d[0] == 'OpCompositeExtract':
                steps = d[3:]
            else:
                steps = [defs[s][2] if defs.get(s, [''])[0] == 'OpConstant' else None for s in d[3:]]
            path, type_id = walk(path, type_id, steps)
            return path, type_id, root
        if d[0] == 'OpLoad':
            return value(d[2], depth + 1)
        if d[0] == 'OpConstant':
            return d[2], d[1], None
        if d[0] == 'OpConstantComposite':
            return '(' + ', '.join(str(value(c, depth + 1)[0]) for c in d[2:]) + ')', d[1], None
        return None, None, None

    samples, uniforms = [], set()
    for i, d in defs.items():
        if d[0].startswith('OpImageSample'):
            samples.append(f'{value(d[2])[0]} at {value(d[3])[0]}')
        elif d[0] in ('OpLoad', 'OpCompositeExtract') and defs.get(d[1], [''])[0] in NUMERIC:
            path, _, root = value(i)
            if root and names.get(root) == '' and defs[root][2] == 'Uniform':
                uniforms.add(path or '?')
    return sorted(samples), uniforms


def check_samples(args, shader, directory, failures):
    found = {}
    for who in ('glslc', 'glslang'):
        spv, opt = Path(directory) / f'{shader.name}.{who}.spv', Path(directory) / f'{shader.name}.{who}.opt'
        if who == 'glslc':
            made = run([args.cli, '-o', str(spv), str(shader)])
        else:
            made = run(
                [args.glslang, '-V', '-R', '--target-env', 'vulkan1.1', '--amb', '--aml', '-o', str(spv), str(shader)]
            )
        if made.returncode:
            failures.append(f'{shader.name}: {who} refuses it: {made.stdout.strip()[:300]}')
            return
        inlined = run([args.spirv_opt, '-O', str(spv), '-o', str(opt)])
        if inlined.returncode:
            failures.append(f'{shader.name}: spirv-opt refuses the {who} module: {inlined.stderr.strip()}')
            return
        found[who] = reads(run([args.spirv_dis, '--raw-id', str(opt)]).stdout)
    (ours, ours_uniforms), (theirs, theirs_uniforms) = found['glslc'], found['glslang']
    for sample in sorted(set(ours) | set(theirs)):
        if ours.count(sample) != theirs.count(sample):
            failures.append(
                f'{shader.name}: {sample}: {ours.count(sample)} times in glslc, {theirs.count(sample)} in glslang'
            )
    if '?' in ours_uniforms | theirs_uniforms:
        failures.append(f'{shader.name}: a uniform read glslc_test.py cannot name by its path')
    for path in sorted(ours_uniforms ^ theirs_uniforms):
        failures.append(f'{shader.name}: {path} is read by {"glslc" if path in ours_uniforms else "glslang"} only')
    if len(theirs) < 12 or len(theirs_uniforms) < 4:
        failures.append(
            f'{shader.name}: {len(theirs)} samples and {len(theirs_uniforms)} uniforms read, too few to mean'
        )
    print(f'semantics: {shader.name}: {len(ours)} samples and {len(ours_uniforms)} uniforms read, compared')


def check_semantics(args, failures):
    shaders = sorted(p for p in (HERE / 'semantics').iterdir() if p.suffix in EXTENSIONS)
    with tempfile.TemporaryDirectory(prefix='snx-glslc-semantics-') as directory:
        for shader in shaders:
            if SAMPLES in shader.read_text(encoding='utf-8').splitlines()[1:2]:
                check_samples(args, shader, directory, failures)
                continue
            spv = {who: Path(directory) / f'{shader.name}.{who}.spv' for who in ('glslc', 'glslang')}
            made = run([args.cli, '-o', str(spv['glslc']), str(shader)])
            reference = run(glslang_command(args.glslang, shader, spv['glslang']))
            if made.returncode or reference.returncode:
                failures.append(
                    f'{shader.name}: glslc: {made.stdout.strip()} glslangValidator: {reference.stdout.strip()}'
                )
                continue
            stores = {}
            for who, path in spv.items():
                opt = Path(str(path) + '.opt')
                folded = run([args.spirv_opt, '-O', str(path), '-o', str(opt)])
                if folded.returncode:
                    failures.append(f'{shader.name}: spirv-opt refuses the {who} module: {folded.stderr.strip()}')
                    break
                stores[who] = folded_stores(run([args.spirv_dis, '--raw-id', str(opt)]).stdout)
            if len(stores) != 2:
                continue
            ours, theirs = stores['glslc'], stores['glslang']
            both = [s for s in ours if ours[s] is not None and theirs.get(s) is not None]
            for slot in both:
                if ours[slot] != theirs[slot]:
                    failures.append(
                        f'{shader.name}: slot {slot} holds {ours[slot]} from glslc, {theirs[slot]} from glslang'
                    )
            if len(both) < 40:
                failures.append(f'{shader.name}: spirv-opt folds {len(both)} slots of both modules, fewer than 40')
            print(f'semantics: {shader.name}: {len(both)} of {len(theirs)} slots folded in both, compared')


ALLOCATION = re.compile(r'\b(malloc|calloc|realloc|free|strdup|strndup|aligned_alloc|posix_memalign)\s*\(')


def check_sources(failures):
    files = sorted((HERE.parent.parent / 'src' / 'glslc').glob('*.[ch]'))
    for path in files:
        text = re.sub(
            r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), path.read_text(encoding='utf-8'), flags=re.DOTALL
        )
        for number, line in enumerate(text.splitlines(), 1):
            if ALLOCATION.search(line):
                failures.append(f'{path.name}:{number}: {line.strip()}: glslc allocates nothing, it has the workspace')
    print(f'sources: {len(files)} files of src/glslc, no allocation')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('errors', 'shaders', 'keywords', 'semantics', 'sources'))
    parser.add_argument('--cli')
    parser.add_argument('--spirv-val')
    parser.add_argument('--spirv-dis')
    parser.add_argument('--spirv-opt')
    parser.add_argument('--glslang')
    args = parser.parse_args()
    failures = []
    if args.mode != 'sources' and not args.cli:
        parser.error(f'{args.mode} needs --cli')
    if args.mode != 'sources' and not args.glslang:
        print('glslangValidator not given: the comparisons with it are left out')
    if args.mode == 'sources':
        check_sources(failures)
    elif args.mode == 'errors':
        check_errors(args, failures)
    elif args.mode == 'shaders':
        if not args.spirv_val or not args.spirv_dis:
            parser.error('shaders needs --spirv-val and --spirv-dis')
        check_shaders(args, failures)
    elif args.mode == 'keywords':
        if not args.glslang:
            parser.error('keywords needs --glslang')
        check_keywords(args, failures)
    else:
        if not args.glslang or not args.spirv_opt or not args.spirv_dis:
            parser.error('semantics needs --glslang, --spirv-opt and --spirv-dis')
        check_semantics(args, failures)
    for failure in failures:
        print('FAIL', failure)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
