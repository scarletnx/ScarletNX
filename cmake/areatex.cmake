# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# SMAA's area and search textures, common/gpu/vk/fx/AreaTex.h and SearchTex.h (which gpu/vk/present_fx.c uploads),
# written by tools/areatexgen under generated/include, on the include path beside src. It runs during the build, so a
# cross build also builds it for the machine doing the build (SNX_HOST_CC), as ui.cmake does for uigen. Both keep each
# rounding of its double arithmetic (-ffp-contract=off): clang fuses a multiply and an add where the machine has the
# instruction (an AArch64 host), and that changes bytes of the area texture
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_areatexgen_dir "${PROJECT_BINARY_DIR}/areatexgen-host")
    set(snx_areatexgen_exe "${snx_areatexgen_dir}/bin/areatexgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_areatexgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_areatexgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/areatexgen"
        BINARY_DIR "${snx_areatexgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_areatexgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_areatexgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_areatexgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-areatexgen IMPORTED GLOBAL)
    set_target_properties(snx-areatexgen PROPERTIES IMPORTED_LOCATION "${snx_areatexgen_exe}")
    add_dependencies(snx-areatexgen snx_areatexgen_host)
else()
    add_executable(snx-areatexgen "${PROJECT_SOURCE_DIR}/tools/areatexgen/areatexgen.c")
    set_target_properties(snx-areatexgen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON
                                                    C_EXTENSIONS OFF RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-areatexgen PRIVATE -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off)
    if(NOT WIN32)
        target_link_libraries(snx-areatexgen PRIVATE m)
    endif()
endif()
set(snx_areatex_h "${PROJECT_BINARY_DIR}/generated/include/common/gpu/vk/fx/AreaTex.h")
set(snx_searchtex_h "${PROJECT_BINARY_DIR}/generated/include/common/gpu/vk/fx/SearchTex.h")
# for the generator's test (tests/areatexgen.cmake), which checks these files too
set_property(GLOBAL PROPERTY snx_areatex_h "${snx_areatex_h}")
set_property(GLOBAL PROPERTY snx_searchtex_h "${snx_searchtex_h}")
add_custom_command(OUTPUT "${snx_areatex_h}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/generated/include/common/gpu/vk/fx"
    COMMAND snx-areatexgen "${snx_areatex_h}"
    DEPENDS snx-areatexgen
    COMMENT "Generating SMAA's area texture" VERBATIM)
add_custom_command(OUTPUT "${snx_searchtex_h}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/generated/include/common/gpu/vk/fx"
    COMMAND snx-areatexgen --search "${snx_searchtex_h}"
    DEPENDS snx-areatexgen
    COMMENT "Generating SMAA's search texture" VERBATIM)
