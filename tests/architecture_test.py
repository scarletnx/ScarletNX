# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Prove forbidden includes and indirect graphics linkage fail architecture checks."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('architecture', Path(__file__).with_name('architecture.py'))
architecture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(architecture)


# a cpmfile.json entry CPMUtil takes
PIN = {'hash': 'ab' * 64, 'repo': 'a/b', 'version': 'v1'}


class ArchitectureTests(unittest.TestCase):
    def environment_errors(self, files):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_environment(root)

    def test_an_option_read_from_the_environment_is_rejected(self):
        errors = self.environment_errors(
            {'src/nx/gpu/check.c': 'int on;\nvoid f(void) { on = !!getenv("SNX_CHECK"); }\n'}
        )
        self.assertEqual(len(errors), 1)
        self.assertIn('src/nx/gpu/check.c:2: getenv("SNX_CHECK"): SNX_CHECK is not the system\'s', errors[0])
        self.assertIn('src/nx/runtime/settings.txt', errors[0])

    def test_the_systems_variables_pass(self):
        files = {
            'src/common/home.c': 'const char* h(void) { return getenv("HOME") ? getenv("XDG_CONFIG_HOME") : 0; }\n',
            'src/gui/pads.c': 'int p(void) { return SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE") != 0; }\n',
            'src/gui/win.cpp': 'void w(char* b) { std::getenv("APPDATA"); GetEnvironmentVariableW(L"PATH", b, 9); }\n',
            'src/common/msvc.c': 'void m(char** v, size_t* n) { _dupenv_s(v, n, "TMPDIR"); }\n',
        }
        self.assertEqual(self.environment_errors(files), [])

    def test_every_reading_call_is_seen(self):
        files = {
            'src/a.c': 'void a(char** v, size_t* n) { _dupenv_s(v, n, "SNX_A"); }\n',
            'src/b.cpp': 'void b() { std::getenv("SNX_B"); }\n',
            'src/c.c': 'void c(void) { secure_getenv ("SNX_C"); SDL_getenv("SNX_D"); }\n',
        }
        errors = self.environment_errors(files)
        self.assertEqual(len(errors), 4)
        for name in ('SNX_A', 'SNX_B', 'SNX_C', 'SNX_D'):
            self.assertTrue(any(f'("{name}"): {name} is not' in error for error in errors), (name, errors))

    def test_a_name_that_is_no_literal_is_rejected(self):
        errors = self.environment_errors({'src/common/x.c': 'const char* x(const char* v) { return getenv(v); }\n'})
        self.assertEqual(len(errors), 1)
        self.assertIn('src/common/x.c:1: getenv of a name that is not a string literal', errors[0])

    def test_comments_and_strings_do_not_read(self):
        text = '/* getenv("SNX_X") */\n// getenv("SNX_Y")\nconst char* u = "set getenv(\\"SNX_Z\\")";\n'
        self.assertEqual(self.environment_errors({'src/common/u.c': text}), [])

    def test_a_deprecated_variable_is_read_only_where_it_is_listed(self):
        listed = next(iter(architecture.ENV_DEPRECATED['SNX_WW']))
        read = 'const char* o(void) { return getenv("SNX_WW"); }\n'
        self.assertEqual(self.environment_errors({listed: read}), [])
        errors = self.environment_errors({'src/nx/runtime/platform/plat_posix.c': read})
        self.assertEqual(len(errors), 1)
        self.assertIn(f'SNX_WW is deprecated, and read only in {listed}', errors[0])

    def window_errors(self, files):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_window_pixels(root)

    def test_a_window_of_points_is_rejected(self):
        opened = 'void o(void) {\n    w = SDL_CreateWindow("t", 1, 1, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);\n}\n'
        errors = self.window_errors({'src/nx/runtime/platform/backend/sdl3.c': opened})
        self.assertEqual(len(errors), 1)
        self.assertIn(
            'src/nx/runtime/platform/backend/sdl3.c:2: SDL_CreateWindow without SDL_WINDOW_HIGH_PIXEL_DENSITY',
            errors[0],
        )
        both = 'void o(void) { SDL_CreateWindowAndRenderer("t", 1, 1, 0, &w, &r); }\n'
        self.assertEqual(len(self.window_errors({'src/nx/a.c': both})), 1)

    def thread_errors(self, files):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_test_threads(root)

    def test_a_tests_registers_apart_from_its_cpu_are_rejected(self):
        files = {
            'tests/hook_a.c': 'static SnxArmCPU a;\nstatic CPU cpu32;\n'
            'SnxArmCPU* arm_cpu(CPU* c) { return (void)c, &a; }\n'
            'CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu32; }\n',
        }
        errors = self.thread_errors(files)
        self.assertEqual(len(errors), 2)
        self.assertIn("tests/hook_a.c:3: arm_cpu returns an object of the test's whatever its argument", errors[0])
        self.assertIn('restrict pointer', errors[0])
        self.assertIn('tests/hook_a.c:4: hook_a32_cpu returns an object', errors[1])

    def test_one_thread_and_a_test_with_no_registers_pass(self):
        files = {
            'tests/hook_b.c': 'typedef struct { CPU cpu; SnxArmCPU arm; } Thread32;\n'
            'SnxArmCPU* arm_cpu(CPU* c) { return &((Thread32*)(void*)((char*)c - offsetof(Thread32, cpu)))->arm; }\n'
            'CPU* hook_a32_cpu(SnxArmCPU* arm) {\n'
            '    return &((Thread32*)(void*)((char*)arm - offsetof(Thread32, arm)))->cpu;\n}\n',
            'tests/hook_c.c': 'static CPU cpu;\nSnxArmCPU* arm_cpu(CPU* c) { return (void)c, NULL; }\n'
            'CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu; }\n',
            'tests/hook_d.c': 'static CPU cpu;\nCPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu; }\n'
            'SnxArmCPU* arm_cpu(CPU* c) {\n    (void)c;\n    return NULL;\n}\n',
        }
        self.assertEqual(self.thread_errors(files), [])

    def device_errors(self, files):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_device_makers(root)

    def test_a_device_made_outside_the_common_path_is_rejected(self):
        files = {
            'src/3ds/gpu/vk/device.c': 'static bool make(void) {\n    return vkCreateDevice(p, &i, NULL, &d) == 0;\n}\n',
            'src/nds/gpu/vk/device.c': 'void up(void) { volkLoadDevice (d); }\n',
        }
        errors = self.device_errors(files)
        self.assertEqual(len(errors), 2)
        self.assertIn('src/3ds/gpu/vk/device.c:2: vkCreateDevice: only src/common/gpu/vk/device.c makes', errors[0])
        self.assertIn('ask snx_vk_device_make (common/gpu/vk/device.h)', errors[0])
        self.assertIn('src/nds/gpu/vk/device.c:1: volkLoadDevice: only', errors[1])

    def test_the_common_path_a_pointer_and_a_comment_pass(self):
        files = {
            'src/common/gpu/vk/device.c': 'void m(void) { vkCreateDevice(p, &i, NULL, &d); volkLoadDevice(d); }\n',
            'src/android/bridge/hle_vulkan.c': 'PFN_vkCreateDevice create = VKF(vkCreateDevice);\n'
            'static const struct { const char* n; } t[] = {{"vkCreateDevice"}};\n',
            'src/nx/gpu/vk/a.c': '/* made by vkCreateDevice() in common/gpu/vk/device.c */\n'
            'int snx_vkCreateDevice(int x);\nint y = snx_vkCreateDevice(1);\n',
        }
        self.assertEqual(self.device_errors(files), [])

    def dynamic_errors(self, files):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_dynamic_descriptors(root)

    def test_a_dynamic_buffer_descriptor_is_rejected(self):
        layout = (
            'static VkDescriptorSetLayoutBinding b = {\n'
            '    .binding = 2,\n    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC};\n'
        )
        errors = self.dynamic_errors({'src/nx/gpu/vk/ui.c': layout})
        self.assertEqual(len(errors), 1)
        self.assertIn('src/nx/gpu/vk/ui.c:3: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: Turnip keeps', errors[0])
        self.assertIn('docs/layers/gpu/driver-quirks.md#turnip', errors[0])
        pool = 'VkDescriptorPoolSize s = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3};\n'
        self.assertEqual(len(self.dynamic_errors({'src/nx/gpu/vk/a.c': pool})), 1)

    def test_a_recorders_case_and_a_comment_pass(self):
        files = {
            'src/nx/gpu/vk/record.c': 'switch (t) {\n    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:\n'
            '    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return 1;\n}\n',
            'src/nx/gpu/vk/b.c': '/* not VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC */\n'
            'static int t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;\n',
        }
        self.assertEqual(self.dynamic_errors(files), [])

    def package_errors(self, files, pins):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            (root / 'cpmfile.json').write_text(json.dumps(pins, indent=4, sort_keys=True) + '\n', encoding='ascii')
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_packages(root)

    def test_a_download_outside_cpmutil_is_rejected(self):
        files = {
            'cmake/deps.cmake': 'CPMAddPackage(NAME x VERSION 1 URL https://x/x.tgz)\n',
            'src/CMakeLists.txt': 'if(A)\n  cpmfindpackage (NAME y)\nendif()\nfile(DOWNLOAD https://x/y y)\n',
            'tests/cmake/t.cmake': 'FetchContent_Declare(z URL https://x/z.tgz)\n',
        }
        errors = self.package_errors(files, {})
        self.assertEqual(len(errors), 4, errors)
        self.assertIn('cmake/deps.cmake:1: CPMAddPackage fetches a third-party package outside CPMUtil', errors[0])
        self.assertIn('src/CMakeLists.txt:2: cpmfindpackage fetches', errors[1])
        self.assertIn('src/CMakeLists.txt:4: file(DOWNLOAD fetches', errors[2])
        self.assertIn('tests/cmake/t.cmake:1: FetchContent_Declare fetches', errors[3])

    def test_cpmutil_comments_and_strings_do_not_fetch(self):
        files = {
            'cmake/cpmutil/CPMUtil.cmake': 'function(AddPackage)\n  CPMAddPackage(NAME ${n})\nendfunction()\n',
            'cmake/a.cmake': '# CPMAddPackage(NAME x)\n#[[ file(DOWNLOAD u f) ]]\nmessage("CPMAddPackage(x)")\n'
            'file(WRITE p [=[FetchContent_Declare(y)]=])\n',
        }
        self.assertEqual(self.package_errors(files, {}), [])

    def test_every_pin_is_added_and_every_key_pinned(self):
        files = {
            'cmake/a.cmake': 'AddJsonPackage(NAME lib DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)\n'
            'AddJsonPackage(gone)\nsnx_cpmfile(v lib version)\nsnx_cpmfile(v absent hash)\n',
            'tests/CMakeLists.txt': 'foreach(n a b)\n  snx_package_file(vec_${n})\nendforeach()\n',
        }
        pins = {'lib': PIN, 'vec_a': PIN, 'vec_b': PIN, 'unused': PIN}
        errors = self.package_errors(files, pins)
        self.assertEqual(len(errors), 3, errors)
        self.assertIn('cmake/a.cmake:2: gone is not pinned in cpmfile.json', errors[0])
        self.assertIn('cmake/a.cmake:4: absent is not pinned in cpmfile.json', errors[1])
        self.assertIn('cpmfile.json: unused is pinned, and no AddJsonPackage or snx_package_file adds it', errors[2])

    def test_only_a_package_the_system_may_give_is_left_to_cpmutils_options(self):
        system = dict(PIN, min_version='1')
        files = {
            'cmake/a.cmake': 'AddJsonPackage(NAME pinned DOWNLOAD_ONLY)\nAddJsonPackage(local)\n'
            'AddJsonPackage(NAME first FORCE_BUNDLED_PACKAGE ON)\nAddJsonPackage(NAME kept FORCE_BUNDLED_PACKAGE ON)\n',
        }
        errors = self.package_errors(files, {'pinned': PIN, 'local': system, 'first': system, 'kept': PIN})
        self.assertEqual(len(errors), 2, errors)
        self.assertIn(
            'cmake/a.cmake:1: pinned is added without FORCE_BUNDLED_PACKAGE ON, and its entry has no', errors[0]
        )
        self.assertIn('cmake/a.cmake:3: first is added with FORCE_BUNDLED_PACKAGE ON, and its entry has a', errors[1])

    def test_every_patch_is_named_and_there(self):
        pins = {'lib': dict(PIN, patches=['0001-fix.patch', '0002-gone.patch'])}
        files = {
            'cmake/a.cmake': 'AddJsonPackage(NAME lib FORCE_BUNDLED_PACKAGE ON)\n',
            '.patch/lib/0001-fix.patch': '--- a/x\n+++ b/x\n',
            '.patch/lib/0003-stray.patch': '--- a/y\n+++ b/y\n',
            '.patch/other/0001-x.patch': '--- a/z\n+++ b/z\n',
        }
        errors = self.package_errors(files, pins)
        self.assertEqual(len(errors), 3, errors)
        self.assertIn(
            'cpmfile.json: lib names the patch 0002-gone.patch, and .patch/lib/0002-gone.patch is no file', errors[0]
        )
        self.assertIn('.patch/lib/0003-stray.patch is a patch no cpmfile.json entry names', errors[1])
        self.assertIn('.patch/other/0001-x.patch is a patch no cpmfile.json entry names', errors[2])

    def test_a_key_is_lowercase(self):
        pins = {'OpenSSL': dict(PIN), 'openssl-cmake': dict(PIN, package='OpenSSL')}
        files = {
            'cmake/a.cmake': 'AddJsonPackage(NAME OpenSSL FORCE_BUNDLED_PACKAGE ON)\n'
            'AddJsonPackage(NAME openssl-cmake FORCE_BUNDLED_PACKAGE ON)\n'
        }
        errors = self.package_errors(files, pins)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn('cpmfile.json: OpenSSL has upper-case letters: a key is lowercase', errors[0])

    def test_the_cpmfile_is_in_cpmutils_form(self):
        pins = {'a': dict(PIN, hash='0' * 64, sha256='x'), 'b': {'hash': 'cd' * 64}}
        files = {'cmake/a.cmake': 'AddJsonPackage(NAME a FORCE_BUNDLED_PACKAGE ON)\nsnx_package_file(b)\n'}
        errors = self.package_errors(files, pins)
        self.assertEqual(len(errors), 4, errors)
        self.assertIn('cpmfile.json: a has sha256, which CPMUtil does not read', errors[0])
        self.assertIn("cpmfile.json: a's hash is [" + '0' * 64 + '], not a SHA-512', errors[1])
        self.assertIn('cpmfile.json: b has no version', errors[2])
        self.assertIn('cpmfile.json: b has neither a url nor a repo', errors[3])
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            (root / 'cpmfile.json').write_text(json.dumps({'a': PIN}), encoding='ascii')
            (root / 'CMakeLists.txt').write_text('AddJsonPackage(NAME a FORCE_BUNDLED_PACKAGE ON)\n', encoding='ascii')
            errors = architecture.check_packages(root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("cpmfile.json is not in CPMUtil's form, which jq --indent 4 -S . writes", errors[0])

    def version_errors(self, files):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='ascii')
            return architecture.check_cmake_versions(root)

    def test_a_cmake_file_asking_more_than_the_minimum_is_rejected(self):
        files = {
            'CMakeLists.txt': '# cmake_minimum_required(VERSION 3.31)\ncmake_minimum_required(VERSION 3.28)\n',
            'cmake/cpmutil/CPMUtil.cmake': 'cmake_minimum_required(VERSION 3.31)\n',
            'tests/t.cmake': '# cmake_minimum_required(VERSION 4.0)\n'
            'file(WRITE p "cmake_minimum_required(VERSION 3.28.6)\n")\n',
            'tools/x/CMakeLists.txt': 'CMAKE_MINIMUM_REQUIRED(VERSION 3.25)\ncmake_minimum_required(VERSION 3.28.0)\n',
        }
        errors = self.version_errors(files)
        self.assertEqual(len(errors), 2, errors)
        self.assertEqual(
            errors[0],
            "cmake/cpmutil/CPMUtil.cmake:1: asks CMake 3.31, and the build's minimum (CMakeLists.txt) is 3.28, on "
            'which it stops: ask 3.28 or raise the minimum',
        )
        self.assertIn("tests/t.cmake:2: asks CMake 3.28.6, and the build's minimum (CMakeLists.txt) is 3.28", errors[1])

    def test_a_policy_the_minimum_lacks_needs_if_policy(self):
        files = {
            'CMakeLists.txt': 'cmake_minimum_required(VERSION 3.28)\n',
            'cmake/a.cmake': 'macro(m)\n'
            '  cmake_policy(SET CMP0150 NEW)\n'
            '  cmake_policy(SET CMP0168 NEW)\n'
            '  if(POLICY CMP0168)\n'
            '    cmake_policy(SET CMP0168 NEW)\n'
            '    if(A)\n'
            '      cmake_policy(SET CMP0168 NEW)\n'
            '    endif()\n'
            '  elseif(POLICY CMP0174)\n'
            '    cmake_policy(SET CMP0168 NEW)\n'
            '  endif()\n'
            '  if(NOT POLICY CMP0170)\n'
            '  else()\n'
            '    cmake_policy(SET CMP0170 NEW)\n'
            '  endif()\n'
            '  if(NOT POLICY CMP0171)\n'
            '    cmake_policy(SET CMP0171 NEW)\n'
            '  endif()\n'
            '  # cmake_policy(SET CMP0172 NEW)\n'
            '  message("cmake_policy(SET CMP0173 NEW)")\n'
            'endmacro()\n',
        }
        errors = self.version_errors(files)
        self.assertEqual(len(errors), 4, errors)
        self.assertEqual(
            errors[0],
            "cmake/a.cmake:3: sets CMP0168 outside if(POLICY CMP0168), and CMake 3.28, the build's minimum, stops on "
            'it: its last policy is CMP0155',
        )
        self.assertIn('cmake/a.cmake:10: sets CMP0168 outside if(POLICY CMP0168)', errors[1])
        self.assertIn('cmake/a.cmake:14: sets CMP0170 outside if(POLICY CMP0170)', errors[2])
        self.assertIn('cmake/a.cmake:17: sets CMP0171 outside if(POLICY CMP0171)', errors[3])

    def test_a_minimum_with_no_last_policy_is_named(self):
        self.assertEqual(
            self.version_errors({'CMakeLists.txt': 'cmake_minimum_required(VERSION 3.31...4.0)\n'}),
            [
                (
                    'CMakeLists.txt asks CMake 3.31, and CMAKE_LAST_POLICY in tests/architecture.py has no last policy '
                    'for 3.31: add the last of cmake --help-policy-list in that release'
                )
            ],
        )
        self.assertEqual(
            self.version_errors({'CMakeLists.txt': 'project(x)\n'}),
            ['CMakeLists.txt has no cmake_minimum_required(VERSION ...): the build states the CMake it needs there'],
        )

    def test_a_window_of_the_displays_pixels_passes(self):
        files = {
            'src/nx/a.c': 'void o(void) { w = SDL_CreateWindow("t", 1, 1, SDL_WINDOW_VULKAN |\n'
            '    SDL_WINDOW_HIGH_PIXEL_DENSITY); }\n',
            'src/nx/b.c': '/* SDL_CreateWindow("t", 1, 1, 0) */\nconst char* s = "SDL_CreateWindow(0)";\n',
            'src/nx/c.c': 'void o(void) { SDL_CreateWindowWithProperties(p); snx_SDL_CreateWindow(1); }\n',
        }
        self.assertEqual(self.window_errors(files), [])

    def test_compiling_nx_source_into_common_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            build = repo / 'build'
            reply = build / '.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            documents = {
                'index-test.json': {'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}},
                'model.json': {
                    'configurations': [{'targets': [{'name': 'scarletnx-common', 'jsonFile': 'common.json'}]}]
                },
                'common.json': {'paths': {'source': 'src/common'}, 'sources': [{'path': 'src/nx/horizon.c'}]},
            }
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding='utf-8')
            errors = architecture.check_target_sources(repo, build)
            self.assertEqual(len(errors), 1)
            self.assertIn('scarletnx-common compiles a source owned by another runtime', errors[0])

    def test_common_cannot_include_nx(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for owner in architecture.BOUNDARIES:
                (root / 'src' / owner).mkdir(parents=True)
            (root / 'src/nx/horizon.h').write_text('/* private */\n', encoding='ascii')
            (root / 'src/common/core.c').write_text('#include "../nx/horizon.h"\n', encoding='ascii')
            errors = architecture.check_sources(root)
            self.assertEqual(len(errors), 1)
            self.assertIn('common includes nx', errors[0])

    def test_3ds_cannot_include_nx(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for owner in architecture.BOUNDARIES:
                (root / 'src' / owner).mkdir(parents=True)
            (root / 'src/nx/horizon.h').write_text('/* private */\n', encoding='ascii')
            (root / 'src/3ds/board.c').write_text('#include "../nx/horizon.h"\n', encoding='ascii')
            errors = architecture.check_sources(root)
            self.assertEqual(len(errors), 1)
            self.assertIn('3ds includes nx', errors[0])

    def test_3ds_cannot_link_nx_indirectly(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            graph = Path(temporary) / 'targets.dot'
            graph.write_text(
                'digraph {\n"a" [label = "scarletnx-common"];\n'
                '"b" [label = "scarletnx-3ds"];\n"c" [label = "adapter"];\n'
                '"d" [label = "scarletnx-nx"];\n'
                '"b" -> "a";\n"b" -> "c";\n"c" -> "d";\n}\n',
                encoding='ascii',
            )
            errors = architecture.check_graph(graph)
            self.assertEqual(errors, ['scarletnx-3ds transitively links forbidden target scarletnx-nx'])

    def test_ctr_cpu_cannot_link_legacy_compiler_runner(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            graph = Path(temporary) / 'targets.dot'
            graph.write_text(
                'digraph {\n"a" [label = "scarletnx-common"];\n'
                '"b" [label = "snx_ctr_cpu"];\n"c" [label = "adapter"];\n'
                '"d" [label = "snx_arm_llvm"];\n'
                '"b" -> "c";\n"c" -> "d";\n}\n',
                encoding='ascii',
            )
            self.assertEqual(
                architecture.check_graph(graph), ['snx_ctr_cpu transitively links forbidden target snx_arm_llvm']
            )

    def test_transitive_graphics_link_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            graph = Path(temporary) / 'targets.dot'
            graph.write_text(
                'digraph {\n"a" [label = "scarletnx-common", shape = octagon];\n'
                '"b" [label = "scarletnx-gnuabi", shape = octagon];\n'
                '"c" [label = "adapter", shape = octagon];\n'
                '"d" [label = "snx_gpu", shape = octagon];\n'
                '"b" -> "a";\n"b" -> "c";\n"c" -> "d";\n}\n',
                encoding='ascii',
            )
            errors = architecture.check_graph(graph)
            self.assertEqual(errors, ['scarletnx-gnuabi transitively links graphics/media target snx_gpu'])

    def test_project_header_named_like_a_system_one_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            for header in ('src/gui/process.h', 'src/gui/gui_model.h', 'src/runner/process/process.h'):
                (repo / header).parent.mkdir(parents=True, exist_ok=True)
                (repo / header).write_text('', encoding='utf-8')
            reply = repo / 'build/.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            documents = {
                'index-test.json': {'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}},
                'model.json': {
                    'configurations': [
                        {
                            'targets': [
                                {'name': 'gui-test', 'jsonFile': 'gui.json'},
                                {'name': 'runner', 'jsonFile': 'runner.json'},
                            ]
                        }
                    ]
                },
                'gui.json': {'compileGroups': [{'includes': [{'path': str(repo / 'src/gui')}]}]},
                # only src/runner is on the path, so src/runner/process/process.h hides nothing
                'runner.json': {'compileGroups': [{'includes': [{'path': 'src/runner'}]}]},
            }
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding='utf-8')
            errors = architecture.check_include_shadows(repo, repo / 'build')
            self.assertEqual(len(errors), 1, errors)
            self.assertIn('src/gui/process.h is on gui-test', errors[0])
            self.assertIn('hides the system <process.h>', errors[0])

    def test_include_root_inside_src_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            (repo / 'src/3ds/common').mkdir(parents=True)
            reply = repo / 'build/.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            documents = {
                'index-test.json': {'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}},
                'model.json': {
                    'configurations': [
                        {'targets': [{'name': 'ctr', 'jsonFile': 'ctr.json'}, {'name': 'app', 'jsonFile': 'app.json'}]}
                    ]
                },
                # src/ itself, a generated folder and the resource compiler's own folder pass
                'app.json': {
                    'compileGroups': [
                        {'language': 'C', 'includes': [{'path': 'src'}, {'path': str(repo / 'build/generated')}]},
                        {'language': 'RC', 'includes': [{'path': 'src/3ds/common'}]},
                    ]
                },
                'ctr.json': {
                    'compileGroups': [
                        {'language': 'C', 'includes': [{'path': 'src'}, {'path': str(repo / 'src/3ds/common')}]},
                        {'language': 'CXX', 'includes': [{'path': 'src/3ds/common'}]},
                    ]
                },
            }
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding='utf-8')
            errors = architecture.check_include_roots(repo, repo / 'build')
            self.assertEqual(
                errors, ["src/3ds/common is on ctr's include path: include its headers as paths from src/"]
            )

    def test_include_flag_to_a_folder_of_src_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            (repo / '.github/workflows').mkdir(parents=True)
            (repo / 'docs/dev').mkdir(parents=True)
            (repo / '.github/workflows/build.yml').write_text(
                'run: clang -I src src/a.c\nrun: clang -I src/common/cpu/aarch64 src/b.c\n', encoding='utf-8'
            )
            (repo / 'docs/dev/development.md').write_text('clang -I"src/nx" x.c\nobjcopy -I binary\n', encoding='utf-8')
            errors = architecture.check_include_flags(repo)
            self.assertEqual(
                errors,
                [
                    '.github/workflows/build.yml:2: -I to a folder of src/; use -I src',
                    'docs/dev/development.md:1: -I to a folder of src/; use -I src',
                ],
            )

    def test_sanitized_build_names_the_code_its_tests_reach_without_them(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            build = repo / 'build'
            reply = build / '.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            sanitized = {'language': 'C', 'compileCommandFragments': [{'fragment': '-O2 -fsanitize=address,undefined'}]}
            plain = {'language': 'C', 'compileCommandFragments': [{'fragment': '-O2'}]}
            turned_off = {
                'language': 'CXX',
                'compileCommandFragments': [
                    {'fragment': '-fsanitize=address,undefined'},
                    {'fragment': '-w -fno-sanitize=address,undefined'},
                ],
            }
            address_only = {'language': 'C', 'compileCommandFragments': [{'fragment': '-fsanitize=address'}]}
            assembly = {'language': 'ASM', 'compileCommandFragments': [{'fragment': '-O2'}]}

            def target(name, kind, sources, groups, artifact=None, dependencies=()):
                document = {
                    'id': name,
                    'name': name,
                    'type': kind,
                    'compileGroups': groups,
                    'sources': [{'path': path, 'compileGroupIndex': group} for path, group in sources],
                    'dependencies': [{'id': dependency} for dependency in dependencies],
                }
                if artifact:
                    document['artifacts'] = [{'path': artifact}]
                return document

            targets = [
                # a test program, sanitized, linking a library of ours, a dependency's, and built after a generator
                target(
                    'snx-test-a',
                    'EXECUTABLE',
                    [('tests/a.c', 0)],
                    [sanitized],
                    'tests/snx-test-a',
                    ('snx_b', 'SDL3-static', 'snx-gen'),
                ),
                target(
                    'snx_b',
                    'STATIC_LIBRARY',
                    [
                        ('src/b.c', 0),
                        (str(build / 'generated/c.c'), 0),
                        ('src/vendor.cpp', 1),
                        ('src/half.c', 2),
                        ('src/b.S', 3),
                    ],
                    [plain, turned_off, address_only, assembly],
                ),
                target(
                    'SDL3-static',
                    'STATIC_LIBRARY',
                    [('.cache/cpm/sdl3/src/SDL.c', 0), (str(build / '_deps/sdl3-build/SDL_revision.c'), 0)],
                    [plain],
                ),
                # a generator the build runs but no test: not linked into one
                target('snx-gen', 'EXECUTABLE', [('tools/gen.c', 0)], [plain], 'snx-gen'),
                # a program a cmake -P test runs, named by its -D value
                target('snx-tool', 'EXECUTABLE', [('src/tool.c', 0)], [plain], 'snx-tool'),
            ]
            documents = {
                'index-test.json': {'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}},
                'model.json': {
                    'configurations': [
                        {
                            'targets': [
                                {'id': t['id'], 'name': t['name'], 'jsonFile': f'{t["id"]}.json'} for t in targets
                            ]
                        }
                    ]
                },
            }
            documents.update({f'{t["id"]}.json': t for t in targets})
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding='utf-8')
            tests = [
                {'name': 'a', 'command': [str(build / 'tests/snx-test-a')]},
                {
                    'name': 'tool_list',
                    'command': ['/usr/bin/cmake', f'-DRUNTIME={build / "snx-tool"}', '-P', 'x.cmake'],
                },
            ]
            errors, exempt = architecture.check_sanitizers(repo, build, tests)
            links = 'and snx-test-a, which the test a runs, links it'
            self.assertEqual(
                sorted(errors),
                [
                    f'build/generated/c.c: snx_b builds it without -fsanitize=address,undefined, {links}',
                    f'src/b.c: snx_b builds it without -fsanitize=address,undefined, {links}',
                    f'src/half.c: snx_b builds it without -fsanitize=address,undefined, {links}',
                    'src/tool.c: snx-tool builds it without -fsanitize=address,undefined, and snx-tool, which the '
                    'test tool_list runs, is it',
                ],
            )
            self.assertEqual(exempt, ['src/vendor.cpp: snx_b turns off -fsanitize=address,undefined'])
            # no test running a program of the build checks nothing, which is an error
            self.assertEqual(
                architecture.check_sanitizers(repo, build, [{'name': 'script', 'command': ['/usr/bin/python3']}]),
                (['no test runs a program of the build: the sanitizers would be checked on nothing'], []),
            )

    def test_a_library_of_ours_this_configuration_skips_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            (repo / 'tests').mkdir()
            # snx_common_test is defined after a return() some configurations take, snx_kept before it
            (repo / 'tests/CMakeLists.txt').write_text(
                'add_library(snx_kept STATIC kept.c)\nreturn()\nadd_library(snx_common_test STATIC common.c)\n',
                encoding='ascii',
            )
            reply = repo / 'build/.cmake/api/v1/reply'
            reply.mkdir(parents=True)

            def target(name, *libraries):
                fragments = [{'fragment': library, 'role': 'libraries'} for library in libraries]
                return {'id': name, 'name': name, 'type': 'EXECUTABLE', 'link': {'commandFragments': fragments}}

            targets = [
                target('snx-test-probe', '-lsnx_common_test', 'libsnx_kept.a', '-lm'),
                target('snx-test-other', 'libsnx_kept.a', '-ldl'),
                {'id': 'snx_kept', 'name': 'snx_kept', 'type': 'STATIC_LIBRARY'},
            ]
            documents = {
                'index-test.json': {'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}},
                'model.json': {
                    'configurations': [
                        {
                            'targets': [
                                {'id': t['id'], 'name': t['name'], 'jsonFile': f'{t["id"]}.json'} for t in targets
                            ]
                        }
                    ]
                },
            }
            documents.update({f'{t["id"]}.json': t for t in targets})
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding='utf-8')
            self.assertEqual(
                architecture.check_link_names(repo, repo / 'build'),
                [
                    'snx-test-probe links snx_common_test, a library of the project that this configuration does not '
                    'define (its add_library is skipped here): the link fails on -lsnx_common_test'
                ],
            )

    def test_sanitizers_of_a_compile_line(self):
        self.assertEqual(
            architecture.sanitizers_of(['-fsanitize=address,undefined -fno-sanitize=vptr']),
            ({'address', 'undefined'}, {'vptr'}),
        )
        self.assertEqual(
            architecture.sanitizers_of(['-fsanitize=address', '-fno-sanitize=all', '-fsanitize=undefined']),
            ({'undefined'}, {'address', 'undefined'}),
        )


if __name__ == '__main__':
    unittest.main()
