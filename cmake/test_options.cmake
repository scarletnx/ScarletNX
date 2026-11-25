# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A test option (SNX_TEST_*, docs/dev/build.md) without the tests would do nothing: the configure stops and says which.
# An option is a BOOL or a value -D gave no type (a path); what a find_* call kept (SNX_TEST_PKG_CONFIG) is neither
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
