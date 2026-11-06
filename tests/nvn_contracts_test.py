#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Independent synthetic inputs for the contract-index and privacy gates."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location('check_nvn_contracts', Path(__file__).resolve().parents[1] /
                                             'tools/check_nvn_contracts.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ContractIndex(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.reference = self.root / 'private-marker-root'
        self.source = self.root / 'source'
        self.reference.mkdir()
        self.source.mkdir()
        (self.reference / 'contract.h').write_text('void nvnContractProbe(void);\nPRIVATE_SOURCE_MARKER\n')
        (self.source / 'implementation.c').write_text('runtime_probe\n')
        (self.source / 'fixture.c').write_text('fixture_probe\n')
        self.contract = MODULE.Contract('synthetic_contract', 'device_dependent', ('nvnContractProbe',),
                                        (('implementation.c', 'runtime_probe'),),
                                        (('fixture.c', 'fixture_probe'),), 'synthetic_gpu')

    def report(self, **kwargs):
        return MODULE.index_contracts(self.reference, self.source, contracts=(self.contract,), **kwargs)

    def test_counts_without_source_or_paths(self):
        report = self.report()
        row = report['contracts'][0]
        self.assertEqual(row['evidence_classification'], 'evidence_present')
        self.assertEqual(row['counts']['reference_api_present'], 1)
        encoded = json.dumps(report)
        for forbidden in ('PRIVATE_SOURCE_MARKER', 'private-marker-root', 'contract.h', 'implementation.c',
                          str(self.root), 'void nvnContractProbe'):
            self.assertNotIn(forbidden, encoded)
        self.assertNotIn('passed', encoded)

    def test_root_beneath_a_symlink_keeps_its_sources(self):
        physical = self.root / 'physical'
        physical.mkdir()
        (physical / 'reference').mkdir()
        (physical / 'reference/contract.h').write_text('NVNAPI void nvnContractProbe(void);\n')
        link = self.root / 'system-parent'
        try:
            link.symlink_to(physical, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest('directory symlinks are unavailable')
        reference = link / 'reference'
        files, names = MODULE.reference_counts(reference)
        self.assertEqual(files, 1)
        self.assertEqual(names['nvnContractProbe'], 1)
        self.assertEqual(MODULE.reference_declarations(reference)['nvnContractProbe'], 1)

    def test_runtime_mutation_removes_evidence(self):
        (self.source / 'implementation.c').write_text('different_token\n')
        row = self.report()['contracts'][0]
        self.assertEqual(row['evidence_classification'], 'evidence_missing')
        self.assertEqual(row['counts']['runtime_present'], 0)

    def inventory_inputs(self, declarations, hooks='', implementation=''):
        (self.reference / 'contract.h').write_text(declarations)
        folder = self.source / 'src/nx/gpu/nvn'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'nvn.hook').write_text(hooks)
        (folder / 'handlers.c').write_text(implementation)
        return MODULE.api_inventory(self.reference, self.source)

    def test_all_declarations_ignore_mentions_comments_and_calls(self):
        report = self.inventory_inputs('''
            NVNAPI void NVNAPIENTRY nvnCommandBufferDraw(void);
            NVNAPI void NVNAPIENTRY
                nvnCommandBufferDraw(void);
            NVNINLINE void nvnCommandBufferDraw_fastpath(void);
            NVNAPI int nvnPrivateOnlyProbe(void);
            /* NVNAPI void nvnCommentProbe(void); */
            const char* text = "NVNAPI void nvnStringProbe(void);";
            void ordinary(void) { nvnCallOnlyProbe(); }
        ''', 'family command\nhook nvnCommandBufferDraw()\n',
            'void nvn_CommandBufferDraw(void) { draw(); }\n')
        self.assertEqual(report['counts']['declared_api_names'], 3)
        self.assertEqual(report['counts']['declaration_instances'], 4)
        self.assertEqual(report['counts']['handler_body_indexed'], 2)
        self.assertEqual(report['counts']['lookup_missing'], 1)
        rows = {row.get('name'): row for row in report['apis'] if 'name' in row}
        self.assertEqual(rows['nvnCommandBufferDraw']['declaration_count'], 2)
        self.assertEqual(rows['nvnCommandBufferDraw_fastpath']['lookup_name'], 'nvnCommandBufferDraw')
        self.assertEqual(rows['nvnCommandBufferDraw_fastpath']['variant'], 'inline_fastpath')
        unknown = next(row for row in report['apis'] if 'name' not in row)
        self.assertEqual(unknown['api_id'], hashlib.sha256(b'nvnPrivateOnlyProbe').hexdigest())
        encoded = json.dumps(report)
        self.assertNotIn('nvnPrivateOnlyProbe', encoded)
        self.assertNotIn('private-marker-root', encoded)
        self.assertNotIn('draw();', encoded)

    def test_static_body_shapes_and_unsupported_signals_stay_distinct(self):
        report = self.inventory_inputs('''
            NVNAPI void nvnDeviceSetDebugLabel(void* p);
            NVNAPI int nvnDeviceInitialize(void* p);
            NVNAPI void nvnCommandBufferDraw(void* p);
            NVNAPI void nvnCommandBufferCopy(void* p);
        ''', '''
            hook nvnDeviceSetDebugLabel()
            hook nvnDeviceInitialize()
            hook nvnCommandBufferDraw()
            hook nvnCommandBufferCopy()
        ''', '''
            void nvn_DeviceSetDebugLabel(void* p) { (void)p; }
            int nvn_DeviceInitialize(void* p) { (void)p; return 1; }
            void nvn_CommandBufferDraw(void* p) {
                if (!p) unsupported();
                draw(p); /* unsupported subtype requires review */
            }
            void nvn_CommandBufferCopy(void* p) {
                log("not implemented for this format");
                copy(p);
            }
        ''')
        rows = {row['name']: row for row in report['apis']}
        self.assertEqual(rows['nvnDeviceSetDebugLabel']['static_findings'], {'noop_body': 1})
        self.assertEqual(rows['nvnDeviceInitialize']['static_findings'], {'constant_return_body': 1})
        self.assertEqual(rows['nvnCommandBufferDraw']['static_findings'],
                         {'limitation_comment_signal': 1, 'unsupported_callee_signal': 1})
        self.assertEqual(rows['nvnCommandBufferCopy']['static_findings'], {'limitation_string_signal': 1})
        self.assertEqual(rows['nvnDeviceSetDebugLabel']['priority']['classification'], 'low')
        self.assertEqual(rows['nvnDeviceSetDebugLabel']['static_review']['classification'], 'release_debug_behavior')
        self.assertEqual(rows['nvnDeviceInitialize']['static_review']['classification'], 'contract_review_required')
        self.assertEqual(rows['nvnDeviceInitialize']['priority']['classification'], 'high')
        self.assertNotIn('complete', json.dumps(rows))
        self.assertNotIn('speedup_percent', json.dumps(rows))

    def test_routes_aliases_missing_definitions_and_conditional_bodies(self):
        report = self.inventory_inputs('''
            NVNAPI void* nvnBootstrapLoader(void);
            NVNAPI void nvnCommandBufferDraw(void);
            NVNAPI void nvnCommandBufferDispatch(void);
        ''', '''
            route nvnBootstrapLoader
            hook nvnCommandBufferDraw() = shared_draw
            hook nvnCommandBufferDispatch()
        ''', '''
            #if FIRST_VARIANT
            void shared_draw(void) { draw_first(); }
            #else
            void shared_draw(void) { draw_second(); }
            #endif
        ''')
        rows = {row['name']: row for row in report['apis']}
        self.assertEqual(rows['nvnBootstrapLoader']['classification'], 'resolver_route')
        self.assertEqual(rows['nvnCommandBufferDraw']['handler'], 'shared_draw')
        self.assertEqual(rows['nvnCommandBufferDraw']['handler_definition_count'], 2)
        self.assertEqual(rows['nvnCommandBufferDispatch']['classification'], 'handler_definition_missing')
        self.assertEqual(rows['nvnCommandBufferDispatch']['priority']['classification'], 'high')

    def test_reference_names_are_explicit_local_opt_in(self):
        self.inventory_inputs('NVNAPI void nvnPrivateOnlyProbe(void);')
        report = MODULE.api_inventory(self.reference, self.source, include_reference_names=True)
        self.assertTrue(report['reference_names_included'])
        self.assertEqual(report['apis'][0]['name'], 'nvnPrivateOnlyProbe')
        self.assertNotIn(str(self.reference), json.dumps(report))

    def test_unresolved_vendor_and_debug_names_are_not_core_gaps(self):
        report = self.inventory_inputs('''
            NVNAPI void nvnExtensionProbeNVX(void);
            NVNAPI void nvnDebugPreValidate_Probe(void);
            NVNAPI void nvnCommandBufferMissing(void);
        ''')
        self.assertEqual(report['counts']['lookup_missing_vendor_extension'], 1)
        self.assertEqual(report['counts']['lookup_missing_internal_debug_validation'], 1)
        self.assertEqual(report['counts']['lookup_missing_core_api'], 1)
        self.assertTrue(all('name' not in row for row in report['apis']))

    def test_curated_gap_pattern_absence_is_not_support(self):
        rows = MODULE.rendering_gaps(self.source)
        self.assertTrue(rows)
        self.assertTrue(all(row['classification'] == 'source_review_required' for row in rows))
        folder = self.source / 'src/nx/gpu/vk'
        folder.mkdir(parents=True)
        (folder / 'coverage.h').write_text('if (!available || depth) return 0;')
        row = next(row for row in MODULE.rendering_gaps(self.source) if row['name'] == 'portable_mixed_sample_coverage')
        self.assertEqual(row['classification'], 'restriction_pattern_present')

    def test_inventory_excludes_symlinked_source_and_reference(self):
        outside = self.root / 'external.h'
        outside.write_text('NVNAPI void nvnOutsideContract(void);')
        (self.reference / 'external.h').symlink_to(outside)
        report = MODULE.api_inventory(self.reference, self.source)
        self.assertEqual(report['counts']['declared_api_names'], 0)

    def test_check_mode_detects_unresolved_declared_api(self):
        self.fill_contract_evidence('NVNAPI void nvnCommandBufferMissing(void);\n')
        arguments = ['--reference-root', str(self.reference), '--root', str(self.source), '--check']
        with contextlib.redirect_stdout(io.StringIO()):
            report = MODULE.index_contracts(self.reference, self.source)
            self.assertEqual(report['counts'].get('evidence_missing', 0), 0)
            self.assertEqual(MODULE.main(arguments), 1)

    def test_missing_reference_is_not_complete(self):
        (self.reference / 'contract.h').write_text('unrelated\n')
        report = self.report()
        self.assertEqual(report['counts']['reference_evidence_missing'], 1)
        self.assertEqual(report['contracts'][0]['evidence_classification'], 'evidence_missing')

    def test_api_restrictions_need_no_renderer(self):
        restriction = MODULE.Contract('synthetic_restriction', 'api_restriction', ('nvnContractProbe',))
        report = MODULE.index_contracts(self.reference, self.source, contracts=(restriction,))
        self.assertEqual(report['contracts'][0]['evidence_classification'], 'restriction')
        self.assertEqual(report['contracts'][0]['counts']['runtime_required'], 0)

    def test_hardware_skip_and_failed_parent_stay_visible(self):
        results = {'checks': [
            {'status': 'pass', 'features': [{'name': 'synthetic_gpu', 'status': 'skip'}]},
            {'status': 'fail', 'features': [{'name': 'synthetic_gpu', 'status': 'pass'}]},
            {'status': 'pass', 'features': [{'name': 'synthetic_gpu', 'status': 'pass'}]},
        ]}
        counts = self.report(results=results)['contracts'][0]['counts']
        self.assertEqual((counts['hardware_pass'], counts['hardware_skip'], counts['hardware_fail']), (1, 1, 1))

    def test_symlink_does_not_export_external_evidence(self):
        other = self.root / 'other.h'
        other.write_text('nvnOutsideContract\n')
        (self.reference / 'outside.h').symlink_to(other)
        files, names = MODULE.reference_counts(self.reference)
        self.assertEqual(files, 1)
        self.assertNotIn('nvnOutsideContract', names)

    def test_input_error_hides_private_path(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = MODULE.main(['--reference-root', str(self.reference / 'absent')])
        self.assertEqual(status, 2)
        self.assertEqual(json.loads(output.getvalue()), {'classification': 'input_error', 'counts': {'errors': 1}})

    def test_absent_explicit_reference_argument_fails(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            MODULE.main([])

    def test_malformed_hardware_results_do_not_report_pass(self):
        for results in ([], {'checks': 'invalid'}, {'checks': [None]}, {'checks': [{'features': [None]}]}):
            with self.subTest(results=results), self.assertRaises(ValueError):
                self.report(results=results)

    def fill_contract_evidence(self, extra_reference=''):
        tokens = {}
        apis = set()
        for contract in MODULE.CONTRACTS:
            apis.update(contract.api)
            for path, token in contract.runtime + contract.tests:
                tokens.setdefault(path, set()).add(token)
        (self.reference / 'contract.h').write_text(extra_reference + '\n'.join(sorted(apis)))
        for relative, names in tokens.items():
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('\n'.join(sorted(names)))

    def test_check_mode_detects_absent_evidence(self):
        self.fill_contract_evidence()
        arguments = ['--reference-root', str(self.reference), '--root', str(self.source), '--check']
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(MODULE.main(arguments), 0)
            (self.source / 'src/nx/gpu/draw_state.h').unlink()
            self.assertEqual(MODULE.main(arguments), 1)


if __name__ == '__main__':
    unittest.main()
