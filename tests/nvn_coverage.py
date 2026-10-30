#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Coverage tooling must reject unimplemented handlers and ABI mismatches."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('nvn_coverage', Path(__file__).resolve().parents[1] / 'tools/nvn_coverage.py')
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class CoverageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.header = root / 'api.h'
        self.header.write_text('''/* NVNAPI void NVNAPIENTRY nvnFake(void); */
NVNAPI size_t NVNAPIENTRY nvnCommandBufferGetCommandMemorySize(const NVNcommandBuffer *cb);
NVNAPI NVNboolean NVNAPIENTRY nvnDeviceIsExternalDebuggerAttached(const NVNdevice *d);
''')
        self.source = root / 'runtime.c'
        self.source.write_text('''u64 nvn_CommandBufferGetCommandMemorySize(CPU* c, u64 cb) {
    const char *misleading = "fake() { vk_bad(); }";
    /* imaginary(); */
    return vk_size(cb);
}
int nvn_DeviceIsExternalDebuggerAttached(CPU* c, u64 d) { return 0; }
''')
        reference = coverage.scan_file(('reference', str(self.header)))
        self.apis = {r['name']: r for r in reference['apis']}
        runtime = coverage.scan_file(('runtime', str(self.source)))
        self.functions = {r['name']: [r] for r in runtime['functions']}
        self.binding = ('hook nvnCommandBufferGetCommandMemorySize(addr cb) -> u64 = '
                        'nvn_CommandBufferGetCommandMemorySize')

    def test_lexical_boundaries(self):
        self.assertNotIn('nvnFake', self.apis)
        fn = self.functions['nvn_CommandBufferGetCommandMemorySize'][0]
        self.assertEqual(fn['calls'], ['vk_size'])
        self.assertNotIn('fake', self.functions)
        self.assertEqual(coverage.vk_paths(fn['name'], self.functions), [[fn['name'], 'vk_size']])

    def test_checked_codegen(self):
        self.assertEqual(coverage.checked_binding(self.binding, self.apis, self.functions), self.binding)
        boolean = ('hook nvnDeviceIsExternalDebuggerAttached(addr d) -> bool = '
                   'nvn_DeviceIsExternalDebuggerAttached')
        self.assertEqual(coverage.checked_binding(boolean, self.apis, self.functions), boolean)

    def test_typed_hooks_and_implicit_handlers(self):
        hook = Path(self.temp.name) / 'nvn.hook'
        hook.write_text('''family device
hook nvnDeviceGetTextureHandle(const NVNdevice* d, i32 texture, i32 sampler) -> u64
hook nvnMemoryPoolGetMemory(const NVNmemoryPool* p) -> void*
hook nvnDeviceFinalize(NVNdevice* d)
hook nvnDeviceGetInteger(addr d, i32 item, addr out) = custom_get_integer
''')
        self.assertEqual(coverage.registered_bindings(hook), {
            'nvnDeviceGetTextureHandle': 'nvn_DeviceGetTextureHandle',
            'nvnMemoryPoolGetMemory': 'nvn_MemoryPoolGetMemory',
            'nvnDeviceFinalize': 'nvn_DeviceFinalize',
            'nvnDeviceGetInteger': 'custom_get_integer',
        })
        implicit = self.binding.split(' = ')[0]
        self.assertEqual(coverage.checked_binding(implicit, self.apis, self.functions), implicit)

    def test_reject_width_sign_and_absent_handler(self):
        for bad in (self.binding.replace('-> u64', '-> i64'),
                    self.binding.replace('addr cb', 'u32 cb'),
                    self.binding.replace('= nvn_', '= absent_')):
            with self.subTest(binding=bad), self.assertRaises(ValueError):
                coverage.checked_binding(bad, self.apis, self.functions)
        self.functions['nvn_CommandBufferGetCommandMemorySize'][0]['args'][1] = 'u32 cb'
        with self.assertRaises(ValueError):
            coverage.checked_binding(self.binding, self.apis, self.functions)

    def test_reject_ambiguous_handler(self):
        handler = self.functions['nvn_CommandBufferGetCommandMemorySize']
        handler.append(handler[0])
        with self.assertRaises(ValueError):
            coverage.checked_binding(self.binding, self.apis, self.functions)

    def test_cached_warm_scan(self):
        tasks = [('reference', str(self.header)), ('runtime', str(self.source))]
        cache = Path(self.temp.name) / 'cache.json'
        # Avoid a process pool here: test incremental reads using an already populated cache.
        import json
        records = {}
        for role, name in tasks:
            st = Path(name).stat()
            records[name] = {'stamp': [role, st.st_size, st.st_mtime_ns, st.st_ctime_ns],
                             'index': coverage.scan_file((role, name))}
        cache.write_text(json.dumps({'schema': coverage.SCHEMA, 'files': records}))
        rows, reused = coverage.index_files(tasks, cache, 8)
        self.assertEqual(reused, 2)
        self.assertEqual(len(rows), 2)


if __name__ == '__main__':
    unittest.main()
