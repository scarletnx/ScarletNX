# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Validate direct SASS shader translation across stages and regression fixtures."""

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
import sys
from pathlib import Path


def disassemble(tool, fixture):
    try:
        return subprocess.run([tool, str(fixture)], check=True, capture_output=True, text=True).stdout
    except subprocess.CalledProcessError as error:
        print(error.stderr, file=sys.stderr)
        raise


def check_1d_array_gradient(words):
    """Route all16 packed offsets to native samples, with the array layer outside the dispatch."""
    instructions, definitions, labels = [], {}, {}
    at = 5
    while at < len(words):
        length, opcode = words[at] >> 16, words[at] & 0xffff
        if not length or at + length > len(words):
            raise AssertionError('invalid dynamic TXD module')
        instruction = words[at:at + length]
        instructions.append(instruction)
        if opcode in (43, 44, 61, 80, 112, 124, 194, 199):
            definitions[instruction[2]] = instruction
        if opcode == 248:
            labels[instruction[1]] = len(instructions)
        at += length

    def require(condition, reason):
        if not condition:
            raise AssertionError('dynamic 1D array TXD: ' + reason)

    def constant(identifier):
        value = definitions.get(identifier, ())
        require(len(value) == 4 and value[0] & 0xffff == 43, 'offset and selector operands must be constants')
        return value[3]

    switches = [instruction for instruction in instructions if instruction[0] & 0xffff == 251]
    samples = [instruction for instruction in instructions if instruction[0] & 0xffff == 88]
    phis = [instruction for instruction in instructions if instruction[0] & 0xffff == 245]
    require(len(switches) == 1 and len(samples) == 16 and len(phis) == 1, 'expected one16-way native sample dispatch')
    require(not any(instruction[0] & 0xffff in (103, 136) for instruction in instructions),
            'base-level size queries or coordinate division cannot replace mip-relative offsets')
    switch, phi = switches[0], phis[0]
    require(len(switch) == 33 and len(phi) == 35, 'every offset needs one branch and one result input')
    selector = definitions.get(switch[1], ())
    require(len(selector) == 5 and selector[0] & 0xffff == 199 and constant(selector[4]) == 15,
            'the dispatch must select only the X-offset nibble')
    shifted = definitions.get(selector[3], ())
    require(len(shifted) == 5 and shifted[0] & 0xffff == 194 and constant(shifted[4]) == 16,
            'the X offset starts at packed bit16')
    targets = dict(zip(switch[3::2], switch[4::2]))
    require(set(targets) == set(range(1, 16)), 'the selector must cover every nonzero signed nibble')
    targets[0] = switch[2]
    require(len(set(targets.values())) == 16 and all(target in labels for target in targets.values()),
            'each selector must reach its own valid leaf')
    merged = dict(zip(phi[4::2], phi[3::2]))
    require(set(merged) == set(targets.values()), 'the result must include every real predecessor')
    common = None
    for nibble, target in targets.items():
        body = instructions[labels[target]:]
        branch = next((index for index, instruction in enumerate(body) if instruction[0] & 0xffff in (249, 250, 251)), None)
        require(branch is not None and body[branch][0] & 0xffff == 249, 'each selected leaf must branch to the merge')
        selected = [instruction for instruction in body[:branch] if instruction[0] & 0xffff == 88]
        require(len(selected) == 1 and merged[target] == selected[0][2], 'the merge must return exactly the selected sample')
        sample = selected[0]
        require(len(sample) == 9 and sample[5] == 12, 'native sampling must retain Grad|ConstOffset')
        offset = definitions.get(sample[8], ())
        require(len(offset) == 5 and offset[0] & 0xffff == 44, 'the texel offset must be a constant two-vector')
        require(constant(offset[3]) == (nibble if nibble < 8 else nibble - 16) & 0xffffffff and constant(offset[4]) == 0,
                'the signed X offset or zero Y offset is wrong')
        image = definitions.get(sample[3], ())
        require(len(image) == 4 and image[0] & 0xffff == 61, 'every leaf must load the same sampled-image binding')
        operands = (sample[1], image[3], sample[4], sample[6], sample[7], body[branch][1])
        common = operands if common is None else common
        require(operands == common, 'descriptor, coordinates, gradients and merge must be shared by every leaf')
        require(common[-1] in labels and instructions[labels[common[-1]]] == phi, 'the selected value must reach the dispatch result')
    coordinate = definitions.get(common[2], ())
    require(len(coordinate) == 6 and coordinate[0] & 0xffff == 80 and constant(coordinate[4]) == 0x3f000000,
            'the emulated 1D texture must keep Y=0.5 and a separate array layer')
    layer = definitions.get(coordinate[5], ())
    require(len(layer) == 4 and layer[0] & 0xffff == 112, 'the array layer must convert the packed low half')
    low_half = definitions.get(layer[3], ())
    require(len(low_half) == 5 and low_half[0] & 0xffff == 199 and low_half[3] == shifted[3] and
            constant(low_half[4]) == 65535, 'offset selection must not change the packed array layer')
    for gradient in common[3:5]:
        vector = definitions.get(gradient, ())
        require(len(vector) == 5 and vector[0] & 0xffff == 80 and constant(vector[4]) == 0,
                'the emulated 1D texture must keep zero Y derivatives')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', required=True)
    parser.add_argument('--validator', required=True)
    parser.add_argument('--disassembler', required=True)
    parser.add_argument('--regressions', required=True)
    args = parser.parse_args()
    directory = tempfile.mkdtemp(prefix='snx-direct-shader-')
    try:
        path = Path(directory) / 'fragment.spv'
        vertex = Path(directory) / 'vertex.spv'
        abs_fragment = Path(directory) / 'abs_fragment.spv'
        stack_fragment = Path(directory) / 'stack_fragment.spv'
        subgroup = Path(directory) / 'subgroup.spv'
        storage_write = Path(directory) / 'storage_write.spv'
        storage_atomic = Path(directory) / 'storage_atomic.spv'
        storage_arrays = tuple(Path(directory) / f'storage_{kind}_array.spv' for kind in ('write', 'atomic'))
        nro_raw = Path(directory) / 'nro_raw.spv'
        nro_ms = Path(directory) / 'nro_ms.spv'
        nro_ms_array = Path(directory) / 'nro_ms_array.spv'
        nro_exchange = Path(directory) / 'nro_exchange.spv'
        dc_color = Path(directory) / 'dc_color.spv'
        issue126 = tuple(Path(directory) / f'issue126_{i}.spv' for i in range(17))
        stages = tuple(Path(directory) / f'stage_{i}.spv' for i in range(3))
        queries = tuple(Path(directory) / f'query_{i}.spv' for i in range(2))
        opcodes = tuple(Path(directory) / f'opcode_{i}.spv' for i in range(3))
        gradients = tuple(Path(directory) / f'gradient_{i}.spv' for i in range(3))
        textures = tuple(Path(directory) / f'texture_{i}.spv' for i in range(6))
        arithmetic = Path(directory) / 'arithmetic.spv'
        beyond = tuple(Path(directory) / f'beyond_{i}.spv' for i in range(4))
        attributes = tuple(Path(directory) / f'attribute_{i}.spv' for i in range(7))
        pexit = Path(directory) / 'pexit.spv'
        subprocess.run(
            [args.generator, str(path), str(vertex), str(abs_fragment), str(stack_fragment), str(subgroup)], check=True
        )
        fixtures = (
            storage_write,
            storage_atomic,
            *storage_arrays,
            *(Path(directory) / f'storage_signed_{i}.spv' for i in range(9)),
            *(Path(directory) / f'storage_volume_atomic_{i}.spv' for i in range(4)),
            *(Path(directory) / f'storage_layer_load_{i}.spv' for i in range(8)),
            nro_raw,
            nro_ms,
            nro_ms_array,
            nro_exchange,
            dc_color,
            *issue126,
            *stages,
            *queries,
            *opcodes,
            *gradients,
            *textures,
            arithmetic,
            *beyond,
            *attributes,
            pexit,
            Path(directory) / 'global_dynamic.spv',
            Path(directory) / 'xfb.spv',
            Path(directory) / 'xfb_stores.spv',
            *(Path(directory) / f'passthrough_{i}.spv' for i in range(6)),
            *(Path(directory) / f'audit_{i}.spv' for i in range(7)),
            *(Path(directory) / f'structured_{i}.spv' for i in range(2)),
            Path(directory) / 'fswzadd.spv',
            Path(directory) / 'saturate.spv',
            Path(directory) / 'cbuf_storage.spv',
            *(Path(directory) / f'surface_op_{i}.spv' for i in range(7)),
            *(Path(directory) / f'legacy_{i}.spv' for i in range(4)),
            *(Path(directory) / f'rect_{i}.spv' for i in range(3)),
            *(Path(directory) / f'minmax_{i}.spv' for i in range(2)),
            *(Path(directory) / f'window_{i}.spv' for i in range(3)),
            *(Path(directory) / f'lone_lane_{i}.spv' for i in range(9)),
        )
        subprocess.run([args.regressions, directory], check=True)
        for fixture in (path, vertex, abs_fragment, stack_fragment, subgroup, *fixtures):
            subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(fixture)], check=True)
        for fixture in storage_arrays:
            if ' 2D 0 1 0 2 ' not in disassemble(args.disassembler, fixture):
                raise AssertionError('a 2D array surface must be an arrayed storage image')
        for i in range(6):
            signed_code = disassemble(args.disassembler, Path(directory) / f'storage_signed_{i}.spv')
            if not re.search(r'OpTypeImage %int (2D|3D) 0 [01] 0 2 Unknown', signed_code):
                raise AssertionError('a signed image store must declare a signed integer image')
            if 'OpImageWrite' not in signed_code or 'OpBitcast %int' not in signed_code:
                raise AssertionError('a signed image store must retain the register bits as signed integers')
        raw_code = disassemble(args.disassembler, nro_raw)
        if 'OpTypeImage %uint 2D 0 0 0 1 Unknown' not in raw_code or 'OpImageFetch %v4uint' not in raw_code:
            raise AssertionError('raw surface load must fetch an unsigned texel')
        for fixture, array in ((nro_ms, 0), (nro_ms_array, 1)):
            ms_code = disassemble(args.disassembler, fixture)
            if f'OpTypeImage %float 2D 0 {array} 1 1 Unknown' not in ms_code or ' Sample %' not in ms_code:
                raise AssertionError('multisample texture load must fetch a specified sample')
        exchange_code = disassemble(args.disassembler, nro_exchange)
        # both words of shared memory: read through workgroup pointers (register reads may reuse a stored value)
        shared = set(re.findall(r'(%\w+) = OpAccessChain %_ptr_Workgroup_uint', exchange_code))
        shared_loads = [m for m in re.findall(r'= OpLoad %uint (%\w+)', exchange_code) if m in shared]
        if len(shared) < 4 or len(shared_loads) < 2:
            raise AssertionError('64-bit shared exchange must access both words')
        dc_code = subprocess.run([args.disassembler, str(dc_color)], check=True, capture_output=True, text=True).stdout
        if 'Dref' in dc_code or 'OpFOrdGreaterThan' not in dc_code or 'OpSelect %float' not in dc_code:
            raise AssertionError('a depth compare of a color texture must sample it and compare in the shader')
        gradient_code = [disassemble(args.disassembler, fixture) for fixture in gradients]
        if not any(
            'OpImageSampleExplicitLod' in line and 'Grad|ConstOffset' in line for line in gradient_code[0].splitlines()
        ):
            raise AssertionError('2D gradients must retain the constant texture offset')
        if not all(' Grad' in code for code in gradient_code):
            raise AssertionError('TXD must supply explicit gradients')
        packed_gradient = gradients[2].read_bytes()
        check_1d_array_gradient(struct.unpack(f'<{len(packed_gradient) // 4}I', packed_gradient))
        texture_code = [disassemble(args.disassembler, fixture) for fixture in textures]
        if not all('OpImageSampleDrefExplicitLod' in code for code in texture_code[:3]):
            raise AssertionError('depth compare with explicit level must retain its reference')
        if ' Sample ' not in texture_code[3] or ' Sample ' not in texture_code[4]:
            raise AssertionError('multisample fetch must retain its sample operand')
        if texture_code[4].count('OpBitFieldSExtract') < 3 or texture_code[4].count('OpIAdd') < 3:
            raise AssertionError('3D texture fetch must add all three packed offset components')
        if not any('OpImageGather' in line and ' Offset ' in line for line in texture_code[5].splitlines()):
            raise AssertionError('1D gather must retain its texture offset')
        opcode_code = disassemble(args.disassembler, opcodes[0])
        if 'OpShiftLeftLogical %uint' not in opcode_code or '%uint_3' not in opcode_code:
            raise AssertionError('ISCADD32I must shift by its encoded amount')
        result = disassemble(args.disassembler, path)
        for expected in (
            'OpEntryPoint Fragment',
            'OriginUpperLeft',
            'NoPerspective',
            'OpCompositeExtract',
            'Location 0',
            'DescriptorSet 1',
            'Binding 0',
            'OpAccessChain',
            'OpFMul',
            'Fma',
            'Exp2',
            'NoContraction',
            'OpImageSampleImplicitLod',
            'OpImageSampleDrefImplicitLod',
            'OpFUnordLessThan',
            'OpSelectionMerge',
            'OpBranchConditional',
        ):
            if expected not in result:
                raise AssertionError(f'missing {expected} in direct SASS shader')
        vs = disassemble(args.disassembler, vertex)
        for expected in (
            'OpEntryPoint Vertex',
            'BuiltIn Position',
            'BuiltIn PointSize',
            'Invariant',
            'OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_1',
        ):
            if expected not in vs:
                raise AssertionError(f'missing {expected} in direct vertex shader')
        abs_code = disassemble(args.disassembler, abs_fragment)
        if 'FAbs' not in abs_code:
            raise AssertionError('missing left-operand absolute value in FADD32I shader')
        if not re.search(r'OpFAdd %float %\w+ %float_1\b', abs_code):
            raise AssertionError('FADD32I with an absolute -1.0 immediate must add the constant 1.0')
        subgroup_code = disassemble(args.disassembler, subgroup)
        for expected in (
            'GroupNonUniformBallot',
            'GroupNonUniformShuffle',
            'BuiltIn SubgroupLocalInvocationId',
            'OpGroupNonUniformBallot',
            'OpGroupNonUniformShuffle',
        ):
            if expected not in subgroup_code:
                raise AssertionError(f'missing {expected} in subgroup shader')
    except Exception:
        print(f'Shader fixtures retained in {directory}', file=sys.stderr)
        raise
    else:
        shutil.rmtree(directory)


if __name__ == '__main__':
    main()
