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
            (repo / 'docs').mkdir()
            (repo / '.github/workflows/build.yml').write_text(
                'run: clang -I src src/a.c\nrun: clang -I src/common/cpu/aarch64 src/b.c\n', encoding='utf-8'
            )
            (repo / 'docs/DEVELOPMENT.md').write_text('clang -I"src/nx" x.c\nobjcopy -I binary\n', encoding='utf-8')
            errors = architecture.check_include_flags(repo)
            self.assertEqual(
                errors,
                [
                    '.github/workflows/build.yml:2: -I to a folder of src/; use -I src',
                    'docs/DEVELOPMENT.md:1: -I to a folder of src/; use -I src',
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
