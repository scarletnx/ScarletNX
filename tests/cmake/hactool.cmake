# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of hactool's internals (src/nx/runtime/driver/hactool), included by CMakeLists.txt where they were: they run
# in its scope, so its variables hold here, and a relative path is the top level's
if(BUILD_TESTING AND TARGET hactool_lib)
    add_executable(snx-test-hactool-decrypted tests/hactool_decrypted.c)
    snx_test_target(snx-test-hactool-decrypted 11)
    target_link_libraries(snx-test-hactool-decrypted PRIVATE hactool_internals)
    add_test(NAME hactool_decrypted COMMAND snx-test-hactool-decrypted)
    # the storages ported from LibHac (src/nx/runtime/driver/hactool/storage.c), and NCAs read in place in a container
    add_executable(snx-test-hactool-layers tests/hactool_layers.c)
    snx_test_target(snx-test-hactool-layers 11)
    target_link_libraries(snx-test-hactool-layers PRIVATE hactool_internals)
    add_test(NAME hactool_layers COMMAND snx-test-hactool-layers)
    snx_sanitizer_path() # tests/CMakeLists.txt
endif()
