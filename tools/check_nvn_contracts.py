#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Index API declarations and rendering contracts without reference text or paths.

An explicit reference root is required. The report contains classifications and
counts, plus identifiers already present in the public runtime. Unknown API
names use stable IDs unless --include-reference-names is requested for local use.
Lexical evidence is not a runtime test or a compatibility claim.
Optional hardware results come from check_nvn_rendering.py, not source scanning.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re


SOURCE_SUFFIXES = {'.c', '.h', '.cpp', '.hpp', '.cc', '.cxx', '.hxx', '.inl', '.inc', '.glsl', '.comp'}
API_NAME = re.compile(r'\bnvn[A-Z][A-Za-z0-9_]*\b')
API_DECLARATION = re.compile(r'\b(?:NVNAPI|NVNINLINE)\b[^;{}]*?\b(nvn[A-Z][A-Za-z0-9_]*)\s*\(', re.S)
LOOKUP = re.compile(r'^\s*(?:@version\([^)]*\)\s*)?(hook|route)\s+(nvn[A-Z]\w*)\b')
C_LITERAL = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
C_FUNCTION = re.compile(r'(?m)^[ \t]*(?:[A-Za-z_]\w*[\s*]+)+([A-Za-z_]\w*)\s*\(')
LIMITATION = re.compile(r'\b(?:unsupported|unimplemented|not implemented|not supported|TODO|FIXME|stub|no[- ]op|ignored)\b',
                        re.I)

INVENTORY_LIMITS = (
    'API declarations use NVNAPI or NVNINLINE and a function declarator. Mentions and calls do not count.',
    'Inline fastpath names share their ordinary API lookup. Every distinct declaration name receives one row.',
    'The C scanner does not preprocess conditional code or expand macros. Missing definitions require review.',
    'Empty bodies and constant returns are exact static shapes, not proof that an API needs more behavior.',
    'Unsupported strings, comments and direct calls are static signals, not proof that every call fails.',
    'A nontrivial handler is not a feature-completeness claim. Recorded commands can fail later during replay.',
    'Priority uses API-name categories and unresolved static signals. It does not estimate execution frequency or speedup.',
    'Reference-only identifiers are SHA256 IDs by default. Reference text and paths never appear in the report.',
)


def mask_c(source):
    """Keep offsets while excluding comments, strings and preprocessor lines."""
    def blank(match):
        return re.sub(r'[^\n]', ' ', match.group())
    code = C_LITERAL.sub(blank, source)
    return re.sub(r'(?m)^[ \t]*#(?:[^\n\\]|\\(?:\r?\n|[^\n]))*', blank, code)


def delimiter_pairs(code, opening, closing):
    stack, result = [], {}
    for index, character in enumerate(code):
        if character == opening:
            stack.append(index)
        elif character == closing and stack:
            result[stack.pop()] = index
    return result


def c_definitions(source, relative):
    """A small declarator/body scanner. No preprocessing or private dependency."""
    code = mask_c(source)
    parens, braces = delimiter_pairs(code, '(', ')'), delimiter_pairs(code, '{', '}')
    result = []
    for match in C_FUNCTION.finditer(code):
        close = parens.get(match.end() - 1)
        if close is None:
            continue
        start = close + 1
        while start < len(code) and code[start].isspace():
            start += 1
        end = braces.get(start)
        if end is None:
            continue
        body = code[start + 1:end]
        remaining = re.sub(r'\(\s*void\s*\)\s*[A-Za-z_]\w*\s*;', '', body).strip()
        kind = 'nontrivial'
        if not remaining or re.fullmatch(r'return\s*;', remaining):
            kind = 'possible_noop'
        elif re.fullmatch(r'return\s+(?:0[xX][\da-fA-F]+[uUlL]*|\d+[uUlL]*|NULL|true|false)\s*;', remaining):
            kind = 'constant_return_only'
        markers = [{'kind': 'comment' if token.group().startswith('/') else 'string'}
                   for token in C_LITERAL.finditer(source, start + 1, end) if LIMITATION.search(token.group())]
        result.append({'name': match.group(1), 'path': relative,
                       'line': source.count('\n', 0, match.start(1)) + 1,
                       'body_kind': kind, 'limitation_markers': markers,
                       'calls': [{'name': item.group(1)} for item in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', body)]})
    return result


@dataclass(frozen=True)
class Contract:
    name: str
    classification: str
    api: tuple
    runtime: tuple = ()
    tests: tuple = ()
    gpu_feature: str = ''


# These are independently stated contracts and public runtime identifiers.
# A restriction is not a missing renderer feature. A matching token is evidence
# for review, never proof that the behavior or its numerical result is correct.
CONTRACTS = (
    Contract('custom_sample_locations', 'device_dependent',
             ('nvnMultisampleStateSetSampleLocations', 'nvnMultisampleStateSetSampleLocationsEnable'),
             (('src/nx/gpu/draw_state.h', 'sample_locations'),
              ('src/nx/gpu/nvn/nvn_draw.c', 'DS_MS_SAMPLE_LOCATIONS'),
              ('src/nx/gpu/vk/device.c', 'VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME')),
             (('tests/nvn_names.c', 'sample_locations'),), 'sample_locations'),
    Contract('sample_location_grid', 'device_dependent',
             ('nvnMultisampleStateSetSampleLocationsGridEnable', 'nvnMultisampleStateGetSampleLocationsGrid'),
             (('src/nx/gpu/nvn/nvn_draw.c', 'DS_MS_SAMPLE_GRID'),),
             (('tests/nvn_names.c', 'DS_MS_SAMPLE_GRID'),), 'sample_locations'),
    Contract('coverage_to_color', 'device_dependent',
             ('nvnMultisampleStateSetCoverageToColorEnable', 'nvnMultisampleStateSetCoverageToColorOutput'),
             (('src/nx/gpu/nvn/nvn_draw.c', 'coverage_to_color_output'),),
             (('tests/nvn_names.c', 'DS_MS_COVERAGE_TO_COLOR'),), 'coverage_to_color'),
    Contract('alpha_to_coverage_dither', 'device_dependent',
             ('nvnMultisampleStateSetAlphaToCoverageDither',),
             (('src/nx/gpu/nvn/nvn_obj.h', 'NVN_MS_A2C_DITHER'),
              ('src/nx/gpu/nvn/nvn_draw.c', 'DS_MS_A2C_DITHER')),
             (('tests/nvn_names.c', 'nvn_MultisampleStateGetAlphaToCoverageDither'),), 'alpha_coverage_dither'),
    Contract('conservative_precision_bias', 'device_dependent',
             ('nvnCommandBufferSetSubpixelPrecisionBias',),
             (('src/nx/gpu/nvn/nvn_cmd.c', 'nvn_CommandBufferSetSubpixelPrecisionBias'),
              ('src/nx/gpu/nvn/nvn_draw.c', 'subpixel_bias_x')),
             (('tests/nvn_draw_banks.c', 'subpixel_bias_y'),), 'conservative_precision'),
    Contract('multisample_state_limits', 'valid_api',
             ('nvnMultisampleStateSetSampleLocations', 'nvnMultisampleStateSetRasterSamples',
              'nvnMultisampleStateSetCoverageToColorOutput'),
             (('src/nx/gpu/nvn/nvn_object.c', 'nvn_MultisampleStateSetSampleLocations'),),
             (('tests/nvn_names.c', 'invalid multisample arguments preserve the whole state'),)),
    Contract('geometry_capture_streams', 'device_dependent',
             ('nvnProgramSetShaders', 'nvnCommandBufferBeginTransformFeedback'),
             (('src/nx/gpu/nvn/nvn_object.c', 'e.x.stream[i]'),
              ('src/nx/gpu/shader/shader.h', 'stream[4]')),
             (('tests/nvn_names.c', 'test_geometry_stream_metadata'),), 'geometry_streams'),
    Contract('evaluation_only_tessellation_builtins', 'device_dependent',
             ('nvnCommandBufferSetInnerTessellationLevels', 'nvnCommandBufferSetOuterTessellationLevels'),
             (('src/nx/gpu/vk/tess_passthrough.c', 'vk_tess_passthrough'),),
             (('tests/vulkan_nvn_tess.inc', 'check_nvn_default_tessellation'),), 'evaluation_only_tessellation'),
    Contract('generated_tessellation_module_cache', 'implementation_work',
             ('nvnProgramSetShaders',),
             (('src/nx/gpu/vk/tess_passthrough.c', 'vk_tess_passthrough'),),
             (('tests/vk_tess_passthrough.c', 'vk_tess_passthrough_spirv'),)),
    Contract('advanced_blend_depth', 'device_dependent',
             ('nvnBlendStateSetAdvancedMode',),
             (('src/nx/gpu/shader/spirv_translate.c', 'advanced_blend_epilogue'),),
             (('tests/vulkan_nvn_advanced_shader.inc', 'check_nvn_advanced_shader'),), 'programmable_blend'),
    Contract('advanced_blend_multisample', 'device_dependent',
             ('nvnBlendStateSetAdvancedMode', 'nvnMultisampleStateSetSamples'),
             (('src/nx/gpu/shader/spirv_translate.c', 'advanced_blend_epilogue'),),
             (('tests/vulkan_nvn_advanced_shader.inc', 'check_nvn_advanced_shader'),), 'programmable_blend'),
    Contract('advanced_blend_srgb', 'device_dependent',
             ('nvnBlendStateSetAdvancedMode',),
             (('src/nx/gpu/shader/spirv_translate.c', 'advanced_blend_epilogue'),),
             (('tests/vulkan_nvn_advanced_shader.inc', 'check_nvn_advanced_shader'),), 'programmable_blend'),
    Contract('advanced_blend_guest_images', 'device_dependent',
             ('nvnBlendStateSetAdvancedMode', 'nvnCommandBufferBindImage'),
             (('src/nx/gpu/shader/spirv_translate.c', 'advanced_blend_epilogue'),),
             (('tests/vulkan_advanced_blend_images.inc', 'check_advanced_blend_images'),), 'blend_images'),
    Contract('advanced_blend_indexed_feedback', 'device_dependent',
             ('nvnBlendStateSetAdvancedMode', 'nvnCommandBufferBindTexture'),
             (('src/nx/gpu/vk/render.c', 'draw_images'),),
             (('tests/vulkan_indexed_feedback.inc', 'check_indexed_texture_feedback'),), 'indexed_programmable_blend'),
    Contract('mixed_sample_coverage', 'device_dependent',
             ('nvnMultisampleStateSetRasterSamples', 'nvnMultisampleStateSetCoverageModulationMode'),
             (('src/nx/gpu/vk/coverage.h', 'vk_coverage_key'),),
             (('tests/vulkan_nvn_coverage.inc', 'check_nvn_coverage'),), 'mixed_sample_coverage'),
    Contract('shader_subroutine_selection', 'valid_api',
             ('nvnProgramSetSubroutineLinkage', 'nvnCommandBufferSetProgramSubroutines'),
             (('src/nx/gpu/nvn/nvn_object.c', 'nvn_subroutine_apply'),
              ('src/nx/gpu/shader/emit_flow.c', 'indirect_target')),
             (('tests/nvn_names.c', 'test_subroutines'),
              ('tests/vulkan_nvn_subroutines.inc', 'check_nvn_subroutine_calls')), 'subroutine_calls'),
    Contract('volume_nearest_color_conversion', 'valid_api',
             ('nvnCommandBufferCopyTextureToTexture',),
             (('src/nx/gpu/nvn/nvn_copy.c', 'copy_pixel_format'),
              ('src/nx/gpu/texture/pixel_convert.glsl', 'tc_pixel_byte')),
             (('tests/vulkan_nvn_copy_conversions.inc', 'nvn_conversion_volume'),), 'copy_conversions'),
    Contract('volume_linear_copy', 'api_restriction', ('nvnCommandBufferCopyTextureToTexture',)),
    Contract('excluded_cross_format_copy', 'api_restriction', ('nvnCommandBufferCopyTextureToTexture',)),
    Contract('advanced_blend_multiple_targets', 'api_restriction', ('nvnBlendStateSetAdvancedMode',)),
)


def regular_sources(root):
    """Do not follow directory or file symlinks outside the supplied tree."""
    for path in sorted(root.rglob('*')):
        if path.is_file() and not path.is_symlink() and path.suffix.lower() in SOURCE_SUFFIXES:
            if not any(parent.is_symlink() for parent in path.parents if parent != root.parent):
                yield path


def reference_counts(root):
    counts = Counter()
    files = 0
    for path in regular_sources(root):
        files += 1
        counts.update(API_NAME.findall(path.read_text(encoding='utf-8', errors='replace')))
    return files, counts


def reference_declarations(root):
    """Collect declaration identifiers only. Keep source bytes inside this call."""
    declarations = Counter()
    for path in regular_sources(root):
        code = mask_c(path.read_text(encoding='utf-8', errors='replace'))
        parens = delimiter_pairs(code, '(', ')')
        for match in API_DECLARATION.finditer(code):
            close = parens.get(match.end() - 1)
            if close is not None and re.match(r'\s*[;{]', code[close + 1:]):
                declarations[match.group(1)] += 1
    return declarations


def runtime_lookup(root):
    path = root / 'src/nx/gpu/nvn/nvn.hook'
    lookups = {}
    family = ''
    if not path.is_file() or path.is_symlink():
        return lookups
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.split('#', 1)[0]
        if line.startswith('family '):
            family = line.split()[1]
        match = LOOKUP.match(line)
        if match:
            kind, name = match.groups()
            alias = re.search(r'=\s*([A-Za-z_]\w*)', line)
            lookups[name] = {'kind': kind, 'family': family,
                             'handler': alias.group(1) if alias else 'nvn_' + name[3:]}
    return lookups


def runtime_definitions(root):
    definitions = {}
    folder = root / 'src/nx/gpu/nvn'
    if not folder.is_dir():
        return definitions
    for path in regular_sources(folder):
        relative = path.relative_to(root).as_posix()
        for symbol in c_definitions(path.read_text(encoding='utf-8', errors='replace'), relative):
            definitions.setdefault(symbol['name'], []).append(symbol)
    return definitions


def inventory_priority(name, classification, findings):
    """A transparent triage order, with no claim about measured GPU or CPU cost."""
    basis = []
    debug = bool(re.search(r'Debug|Checkpoint|Label|DebugGroup', name))
    hot = bool(re.match(r'nvnCommandBuffer(?:Draw|Dispatch|Bind|Set|Copy|Clear|Barrier|Wait|Signal)', name) or
               re.match(r'nvnQueue(?:Submit|Present|Acquire|Finish|Flush|Wait)', name))
    dependency = bool(re.match(r'nvn(?:DeviceInitialize|ProgramSetShaders|MemoryPool(?:Initialize|Map)|'
                               r'Texture(?:Initialize|BuilderSetStorage)|BufferInitialize|QueueInitialize)', name))
    unresolved = classification in ('lookup_missing', 'handler_definition_missing')
    if unresolved:
        basis.append('lookup_or_definition_missing')
    if hot:
        basis.append('command_or_submission_name_heuristic')
    if dependency:
        basis.append('resource_or_program_dependency_name_heuristic')
    if findings:
        basis.append('static_body_findings_require_review')
    if debug:
        basis.append('debug_or_label_name_heuristic')
    issue = unresolved or bool(findings)
    priority = 'high' if issue and not debug and (hot or dependency) else 'medium' if issue and not debug else 'low'
    return {'classification': priority, 'basis': basis}


def declaration_scope(name):
    """Identifier conventions describe scope, not whether an extension is usable."""
    if name.startswith(('nvnDebugPreValidate_', 'nvnDebugPostValidate_')):
        return 'internal_debug_validation'
    if name.endswith('NVX'):
        return 'vendor_extension'
    return 'core_api'


STATIC_REVIEW_POLICIES = (
    (r'Debug|Checkpoint|DebugID|Label', 'release_debug_behavior',
     'Review the advertised debug-layer contract. A release debug no-op is not a rendering defect.'),
    (r'CommandBufferDiscard(?:Color|DepthStencil)$', 'discard_hint',
     'Discard makes attachment contents undefined. Retaining the old contents does not itself break the contract.'),
    (r'RegisterFastClear|Subtile|TiledCache|ColorReduction|StencilCull|ZCull|ResolveDepthBuffer',
     'host_compression_or_raster_hint',
     'Review the host compression and raster model. A hardware hint need not create a guest-visible state change.'),
    (r'FlushMappedRange|InvalidateMappedRange|Texture(?:FlushTexels|InvalidateTexels)$', 'memory_coherence_model',
     'Verify runtime memory visibility and barriers. Do not infer a missing copy from an empty API body.'),
    (r'Queue(?:Flush|GetTotal(?:Command|Control|Compute)MemoryUsed|ResetMemoryUsageCounts)$',
     'queue_submission_and_allocation_model',
     'Submissions are forwarded immediately and the runtime does not allocate from the supplied queue memory.'),
    (r'(?:Sampler|Window)BuilderSetDevice$', 'single_device_model',
     'The runtime uses one device. Review default and getter behavior before changing builder storage.'),
    (r'(?:Sampler|Texture)PoolFinalize$|DeviceFinalizeCommandHandle$', 'runtime_owned_metadata',
     'The runtime owns descriptor and recording metadata. Review its lifetime instead of assuming an allocation leak.'),
    (r'DeviceSetIntermediateShaderCache$|CommandBufferSetShaderScratchMemory$', 'host_shader_resource_model',
     'The host compiler and runtime own shader scratch and caches. Guest backing storage does not directly map to them.'),
)


def static_review_policy(name, findings):
    if not findings:
        return {'classification': 'no_static_body_finding'}
    for pattern, classification, reason in STATIC_REVIEW_POLICIES:
        if re.search(pattern, name):
            return {'classification': classification, 'basis': reason}
    return {'classification': 'contract_review_required',
            'basis': 'No reviewed contract explains this body shape. Inspect the API and its runtime dependencies.'}


def api_inventory(reference_root, source_root, include_reference_names=False):
    declared = reference_declarations(reference_root)
    lookup = runtime_lookup(source_root)
    definitions = runtime_definitions(source_root)
    rows, totals = [], Counter({'lookup_missing_core_api': 0, 'lookup_missing_vendor_extension': 0,
                                'lookup_missing_internal_debug_validation': 0, 'handler_definition_missing': 0})
    for name, mentions in sorted(declared.items()):
        base = name[:-9] if name.endswith('_fastpath') else name
        item = lookup.get(base)
        symbols = definitions.get(item['handler'], []) if item else []
        findings = Counter()
        for symbol in symbols:
            if symbol['body_kind'] == 'possible_noop':
                findings['noop_body'] += 1
            if symbol['body_kind'] == 'constant_return_only':
                findings['constant_return_body'] += 1
            for marker in symbol['limitation_markers']:
                findings['limitation_' + marker['kind'] + '_signal'] += 1
            findings['unsupported_callee_signal'] += sum(
                bool(re.search(r'unsupported|unimplemented|stub', call['name'], re.I)) for call in symbol['calls'])
        findings += Counter()  # Remove zero counts without changing the classification of a body.
        classification = 'lookup_missing' if not item else 'resolver_route' if item['kind'] == 'route' else \
            'handler_definition_missing' if not symbols else 'handler_body_indexed'
        row = {'api_id': hashlib.sha256(name.encode('ascii')).hexdigest(),
               'variant': 'inline_fastpath' if name != base else 'ordinary',
               'declaration_scope': declaration_scope(base),
               'classification': classification, 'declaration_count': mentions,
               'handler_definition_count': len(symbols), 'static_findings': dict(sorted(findings.items())),
               'static_review': static_review_policy(base, findings),
               'priority': inventory_priority(base, classification, findings)}
        if item or include_reference_names:
            row['name'] = name
        if item:
            row['lookup_name'] = base
            row['family'] = item['family']
            if item['kind'] == 'hook':
                row['handler'] = item['handler']
        if symbols:
            row['runtime_locations'] = [{'path': symbol['path'], 'line': symbol['line']} for symbol in symbols]
        rows.append(row)
        totals[classification] += 1
        if classification == 'lookup_missing':
            totals['lookup_missing_' + row['declaration_scope']] += 1
        totals['with_static_findings'] += bool(findings)
        totals['declaration_instances'] += mentions
        totals['inline_fastpath_names'] += name != base
        for finding, count in findings.items():
            totals[finding] += count
    return {'classification': 'static_api_inventory', 'limits': list(INVENTORY_LIMITS),
            'reference_names_included': include_reference_names,
            'counts': {'declared_api_names': len(declared),
                       'canonical_api_names': len({name[:-9] if name.endswith('_fastpath') else name for name in declared}),
                       'runtime_lookup_names': len(lookup),
                       'runtime_lookup_names_without_declaration': len(set(lookup) - set(declared)), **dict(totals)},
            'apis': rows}


# These runtime restrictions were reviewed independently. A missing source pattern means review is needed,
# never that the restriction was fixed. The source can change while this inventory remains useful.
RENDERING_GAPS = (
    ('advanced_blend_late_depth_discard', 'valid_api_combination',
     'src/nx/gpu/vk/render.c', r'!zt\s*\|\|\s*blend_early_safe',
     ('Native advanced blending or exact manual depth, stencil and blend operations are required.',
      'Forced early tests change the depth and stencil effects of shader discard.')),
    ('portable_mixed_sample_coverage', 'device_dependent',
     'src/nx/gpu/vk/coverage.h', r'!available\s*\|\|\s*depth',
     ('Preserve primitive order and native shader invocation frequency.',
      'A final resolve after a whole draw does not preserve overlap-dependent blending.')),
    ('geometry_nonpoint_streams', 'device_dependent',
     'src/nx/gpu/shader/spirv_translate.c', r'!g_shader_geometry_streams_lines_triangles',
     ('Query transformFeedbackStreamsLinesTriangles separately from geometryStreams.',)),
    ('coverage_to_color_late_depth', 'device_dependent',
     'src/nx/gpu/vk/render.c', r'safe post-depth shader coverage',
     ('PostDepthCoverage requires EarlyFragmentTests and cannot replace general late-depth behavior.',)),
    ('gradient_sampling_dynamic_texel_offsets', 'valid_api_combination',
     'src/nx/gpu/shader/emit_texture.c', r"base level.s size as an approximation",
     ('Dynamic offsets must use the selected mip texel size under minification.',
      'A normalized-coordinate shift using the base-level size is not exact. Constant ConstOffset remains exact.')),
)


def rendering_gaps(source_root):
    rows = []
    for name, validity, relative, pattern, requirements in RENDERING_GAPS:
        path = source_root / relative
        content = path.read_text(encoding='utf-8', errors='replace') if path.is_file() else ''
        found = bool(re.search(pattern, content))
        rows.append({'name': name, 'api_classification': validity,
                     'classification': 'restriction_pattern_present' if found else 'source_review_required',
                     'proof_requirements': list(requirements),
                     'priority': {'classification': 'high', 'basis': ['curated_rendering_dependency']}})
    return rows


def evidence_counts(root, requirements):
    """Count fixed public tokens only in specified repository files."""
    counts = []
    for relative, token in requirements:
        path = root / relative
        content = path.read_text(encoding='utf-8', errors='replace') if path.is_file() else ''
        counts.append(content.count(token))
    return sum(value > 0 for value in counts), sum(counts)


def hardware_counts(results, feature):
    counts = Counter()
    if not feature:
        return counts
    checks = results.get('checks', [])
    if not isinstance(checks, list):
        raise ValueError('results')
    for check in checks:
        if not isinstance(check, dict) or not isinstance(check.get('features', []), list):
            raise ValueError('results')
        for row in check.get('features', []):
            if not isinstance(row, dict):
                raise ValueError('results')
            if row.get('name') == feature:
                status = row.get('status')
                if status not in ('pass', 'skip', 'fail') or check.get('status') != 'pass':
                    status = 'fail'
                counts[status] += 1
    return counts


def index_contracts(reference_root, source_root, results=None, contracts=CONTRACTS, include_reference_names=False):
    if results is not None and not isinstance(results, dict):
        raise ValueError('results')
    files, reference = reference_counts(reference_root)
    report = {'schema_version': 2, 'classification': 'static_contract_and_api_evidence', 'counts': {
        'reference_files': files, 'reference_api_names': len(reference),
        'reference_api_mentions': sum(reference.values()), 'contracts': len(contracts)}, 'contracts': []}
    totals = Counter()
    for contract in contracts:
        present = sum(reference[api] > 0 for api in contract.api)
        source_present, source_mentions = evidence_counts(source_root, contract.runtime)
        test_present, test_mentions = evidence_counts(source_root, contract.tests)
        restriction = contract.classification == 'api_restriction'
        complete = present == len(contract.api) and source_present == len(contract.runtime) and test_present == len(contract.tests)
        classification = 'restriction' if restriction else 'evidence_present' if complete else 'evidence_missing'
        hardware = hardware_counts(results or {}, contract.gpu_feature)
        row = {'name': contract.name, 'classification': contract.classification,
               'evidence_classification': classification,
               'counts': {'reference_api_required': len(contract.api), 'reference_api_present': present,
                          'reference_api_mentions': sum(reference[api] for api in contract.api),
                          'runtime_required': len(contract.runtime), 'runtime_present': source_present,
                          'runtime_mentions': source_mentions,
                          'tests_required': len(contract.tests), 'tests_present': test_present,
                          'test_mentions': test_mentions,
                          'hardware_pass': hardware['pass'], 'hardware_skip': hardware['skip'],
                          'hardware_fail': hardware['fail']}}
        report['contracts'].append(row)
        totals[classification] += 1
        if present != len(contract.api):
            totals['reference_evidence_missing'] += 1
        totals['hardware_fail'] += hardware['fail']
    report['counts'].update(totals)
    report['api_inventory'] = api_inventory(reference_root, source_root, include_reference_names)
    report['rendering_gaps'] = rendering_gaps(source_root)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-root', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--gpu-results', type=Path, help='existing rendering-harness results.json')
    parser.add_argument('--out', type=Path, help='write the same classifications/counts as JSON')
    parser.add_argument('--include-reference-names', action='store_true',
                        help='include declaration names absent from public runtime lookups, for local review only')
    parser.add_argument('--check', action='store_true',
                        help='fail on absent contract evidence, unresolved API lookups/handlers or reported GPU failures')
    args = parser.parse_args(argv)
    try:
        if not args.reference_root.is_dir() or not args.root.is_dir():
            raise ValueError('root')
        results = json.loads(args.gpu_results.read_text(encoding='utf-8')) if args.gpu_results else None
        report = index_contracts(args.reference_root, args.root, results,
                                 include_reference_names=args.include_reference_names)
        output = json.dumps(report, indent=2) + '\n'
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(output, encoding='utf-8')
        print(output, end='')
        counts = report['counts']
        inventory = report['api_inventory']['counts']
        return int(bool(args.check and (counts.get('evidence_missing', 0) or counts.get('reference_evidence_missing', 0)
                                  or counts.get('hardware_fail', 0) or inventory.get('lookup_missing', 0)
                                  or inventory.get('handler_definition_missing', 0))))
    except (OSError, ValueError, TypeError):
        # Do not echo a private input path or reference bytes in error output.
        print(json.dumps({'classification': 'input_error', 'counts': {'errors': 1}}))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
