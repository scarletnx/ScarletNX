# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A test option (SNX_TEST_*, docs/dev/build.md) without the tests would do nothing: the configure stops and says which.
# An option is a BOOL or a value -D gave no type (a path); what a find_* call kept (SNX_TEST_PKG_CONFIG) is neither
# SNX_TEST_AMBERLITH was SNX_TEST_EDENPROBE before the probe became Amberlith (1.0.0): the old name is still read as the
# new one, said where it is taken (the next configure finds the new one in the cache)
if(DEFINED SNX_TEST_EDENPROBE AND NOT DEFINED SNX_TEST_AMBERLITH)
    message(NOTICE "SNX_TEST_EDENPROBE is the old name of SNX_TEST_AMBERLITH: SNX_TEST_AMBERLITH=${SNX_TEST_EDENPROBE} "
                   "is taken from it; write -DSNX_TEST_AMBERLITH")
    set(SNX_TEST_AMBERLITH "${SNX_TEST_EDENPROBE}" CACHE STRING
        "Amberlith's NRO, run headless against its baseline (docs/dev/amberlith.md)")
endif()
if(NOT BUILD_TESTING)
    get_cmake_property(snx_cache_names CACHE_VARIABLES)
    foreach(name ${snx_cache_names})
        if(name MATCHES "^SNX_TEST_")
            get_property(type CACHE ${name} PROPERTY TYPE)
            if(type MATCHES "^(BOOL|STRING|UNINITIALIZED)$" AND ${name})
                message(FATAL_ERROR "${name}=${${name}} needs the tests, which this configure leaves out "
                                    "(BUILD_TESTING=${BUILD_TESTING}): pass -DBUILD_TESTING=ON, or -D${name}= to go "
                                    "without it")
            endif()
        endif()
    endforeach()
endif()
