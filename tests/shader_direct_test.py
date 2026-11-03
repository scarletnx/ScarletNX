# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Validate direct SASS shader translation across stages and regression fixtures."""

import argparse
import re
import shutil
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
        beyond = tuple(Path(directory) / f'beyond_{i}.spv' for i in range(3))
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
            *(Path(directory) / f'passthrough_{i}.spv' for i in range(6)),
            *(Path(directory) / f'audit_{i}.spv' for i in range(7)),
            *(Path(directory) / f'structured_{i}.spv' for i in range(2)),
            Path(directory) / 'fswzadd.spv',
            Path(directory) / 'saturate.spv',
            *(Path(directory) / f'surface_op_{i}.spv' for i in range(7)),
            *(Path(directory) / f'legacy_{i}.spv' for i in range(4)),
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
        if 'OpImageQuerySizeLod' not in gradient_code[2]:
            raise AssertionError('dynamic 1D array offset must query texture size')
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
            raise AssertionError('missing right-operand absolute value in FADD32I shader')
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
