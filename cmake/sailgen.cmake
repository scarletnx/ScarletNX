# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# sailgen (tools/sailgen/) generates the A64 and AArch32 instructions' LLVM emitters from the Sail of the Arm
# architecture in src/common/cpu/aarch64/sail/ and src/common/cpu/arm/sail/ (sail-arm's, with each system's fixups
# over it). It runs during the build, so a cross build
# also builds it for the machine doing the build, with that machine's C compiler (SNX_HOST_CC), as swipc.cmake does
# for swipc
include_guard(GLOBAL)
set(snx_sailgen_sources "${PROJECT_SOURCE_DIR}/tools/sailgen/main.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/parse.c"
                        "${PROJECT_SOURCE_DIR}/tools/sailgen/extract.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/gen.c"
                        "${PROJECT_SOURCE_DIR}/tools/sailgen/gpu.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/sdm.c"
                        "${PROJECT_SOURCE_DIR}/tools/sailgen/sdm_code.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/term.c"
                        "${PROJECT_SOURCE_DIR}/tools/sailgen/isel.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/enc.c")
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_sailgen_dir "${PROJECT_BINARY_DIR}/sailgen-host")
    set(snx_sailgen_exe "${snx_sailgen_dir}/bin/sailgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_sailgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_sailgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/sailgen"
        BINARY_DIR "${snx_sailgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_sailgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_sailgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_sailgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-sailgen IMPORTED GLOBAL)
    set_target_properties(snx-sailgen PROPERTIES IMPORTED_LOCATION "${snx_sailgen_exe}")
    add_dependencies(snx-sailgen snx_sailgen_host)
else()
    add_executable(snx-sailgen ${snx_sailgen_sources})
    set_target_properties(snx-sailgen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                                 RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-sailgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_sailgen_a64(<target>): generates a64_sail.inc (the A64 subsets' emitters, sailgen's back end at its top) into
# the build folder from a64.roots, a64.sail and nx.sail, with the features the decoder's profile names, and gives
# <target> its include root
function(snx_sailgen_a64 target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail")
    set(bits "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h")
    set(core "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h")
    set(backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h")
    set(profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    add_custom_command(OUTPUT "${out}/a64_sail.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a64.roots" --backend "${bits}" --backend "${core}" --backend "${backend}"
                --profile "${profile}"
                -o "${out}/a64_sail.inc" "${dir}/a64.sail" --overlay "${dir}/nx.sail"
        DEPENDS snx-sailgen "${dir}/a64.roots" "${dir}/a64.sail" "${dir}/nx.sail" "${bits}" "${core}" "${backend}" "${profile}"
        COMMENT "Generating the A64 emitters from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_sail.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_a64_encoders(<target>): generates a64_enc.inc (an encoder for each decode clause of a64.sail: the
# instruction table of the back end that writes A64 itself, the arm64 host's, with what each clause's code reaches of
# FPCR and FPSR under nx.sail and the decoder's profile) into the build folder, and gives <target> its include root
function(snx_sailgen_a64_encoders target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail")
    set(profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    add_custom_command(OUTPUT "${out}/a64_enc.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --encoders __DecodeA64 --profile "${profile}" -o "${out}/a64_enc.inc" "${dir}/a64.sail"
                --overlay "${dir}/nx.sail"
        DEPENDS snx-sailgen "${dir}/a64.sail" "${dir}/nx.sail" "${profile}"
        COMMENT "Generating the A64 encoders from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_enc.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_a32(<target>): generates a32_sail.inc (the AArch32 subsets' emitters, sailgen's back end at its top) into
# the build folder from a32.roots, a32.sail and arm.sail, with the architectures' a32.profile, and gives <target> its
# include root
function(snx_sailgen_a32 target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail")
    set(bits "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h")
    set(core "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h")
    set(backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm_arm.h")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    add_custom_command(OUTPUT "${out}/a32_sail.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a32.roots" --backend "${bits}" --backend "${core}" --backend "${backend}"
                --profile "${dir}/a32.profile" -o "${out}/a32_sail.inc" "${dir}/a32.sail" --overlay "${dir}/arm.sail"
        DEPENDS snx-sailgen "${dir}/a32.roots" "${dir}/a32.sail" "${dir}/arm.sail" "${dir}/a32.profile" "${bits}" "${core}"
                "${backend}"
        COMMENT "Generating the AArch32 emitters from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a32_sail.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_gpu(<target> <isa> [EVAL] [DIR <dir>]): GPU mode (sailgen --gpu, apart from the CPUs' --generate):
# generates <isa>_sail.inc (the instructions and fusions <isa>.roots names, emitted as SPIR-V) into the build folder
# from gpu.sail (src/nx/gpu/shader/sail/, what every GPU's Sail shares) and <isa>.sail in the ISA's folder (DIR, by
# default src/nx/gpu/shader/sail/, with its lut3.sail where it has one), with the back end spirv.h and the ISA's
# <isa>_spirv.h, and gives <target> its include root. The translator of that ISA includes it after its accessors.
# With EVAL, <isa>_sail_eval.inc: the same Sail run on values (eval.h and <isa>_eval_state.h) for the roots of
# <isa>_eval.roots, for the tests that hold it to a reference
function(snx_sailgen_gpu target isa)
    cmake_parse_arguments(PARSE_ARGV 2 arg "EVAL" "DIR" "")
    set(common "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sail/gpu.sail")
    if(arg_DIR)
        set(dir "${arg_DIR}")
    else()
        set(dir "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sail")
    endif()
    if(EXISTS "${dir}/lut3.sail")
        list(APPEND common "${dir}/lut3.sail")
    endif()
    if(arg_EVAL)
        set(core "${PROJECT_SOURCE_DIR}/tools/sailgen/eval.h")
        set(state "${dir}/${isa}_eval_state.h")
        set(name "${isa}_sail_eval.inc")
        set(what "run on values")
        set(roots "${dir}/${isa}_eval.roots") # the instructions the tests run on values
    else()
        set(core "${PROJECT_SOURCE_DIR}/tools/sailgen/spirv.h")
        set(state "${dir}/${isa}_spirv.h")
        set(name "${isa}_sail.inc")
        set(what "emitted as SPIR-V")
        set(roots "${dir}/${isa}.roots")
    endif()
    # an ISA's families may each have their Sail, roots and back end of their own: <isa>_<family>.sail, .roots
    # (which <isa>.roots includes) and _spirv.h
    file(GLOB family_sail CONFIGURE_DEPENDS "${dir}/${isa}_*.sail")
    file(GLOB family_roots CONFIGURE_DEPENDS "${dir}/${isa}_*.roots")
    set(states "${state}")
    if(NOT arg_EVAL)
        file(GLOB family_states CONFIGURE_DEPENDS "${dir}/${isa}_*_spirv.h")
        list(APPEND states ${family_states})
    endif()
    set(backends)
    foreach(f ${states})
        list(APPEND backends --backend "${f}")
    endforeach()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    add_custom_command(OUTPUT "${out}/${name}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --gpu "${roots}" --backend "${core}" ${backends}
                -o "${out}/${name}" ${common} "${dir}/${isa}.sail" ${family_sail}
        DEPENDS snx-sailgen "${roots}" ${family_roots} ${common} "${dir}/${isa}.sail" ${family_sail} ${states}
                "${core}"
        COMMENT "Generating the ${isa} instructions from Sail, ${what} (GPU mode)" VERBATIM)
    target_sources(${target} PRIVATE "${out}/${name}")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# the machine back end's files sailgen copies at the top of a guest's emitters, in their order (tools/sailgen/README.md):
# MACHINE's (x64, arm64) and GUEST's (a64, a32), the guest's part every machine shares before the machine's own, as
# paths in out, and the --backend arguments that name them in args
# The RISC machines (riscv64...) share risc.h, risc_asm.h and risc_<guest>.h after their own header and assembly,
# and have a guest part of their own only where it exists
function(snx_sailgen_machine_backends machine guest out args)
    set(back "${PROJECT_SOURCE_DIR}/tools/sailgen")
    set(risc rv64 ppc64 mips64 la64 s390x)
    if(machine IN_LIST risc)
        set(files "${back}/bits.h" "${back}/machdef.h" "${back}/${machine}.h" "${back}/risc.h" "${back}/mach.h"
                  "${back}/risc_asm.h" "${back}/mach_${guest}.h" "${back}/risc_${guest}.h")
        if(EXISTS "${back}/${machine}_${guest}.h")
            list(APPEND files "${back}/${machine}_${guest}.h")
        endif()
    else()
        set(files "${back}/bits.h" "${back}/machdef.h" "${back}/${machine}.h" "${back}/mach.h" "${back}/${machine}_asm.h"
                  "${back}/mach_${guest}.h" "${back}/${machine}_${guest}.h")
    endif()
    set(list "")
    foreach(file IN LISTS files)
        list(APPEND list --backend "${file}")
    endforeach()
    set(${out} "${files}" PARENT_SCOPE)
    set(${args} "${list}" PARENT_SCOPE)
endfunction()

# snx_sailgen_a64_arm64(<target>): generates a64_arm64.inc (the A64 emitters of a64_x64.roots with sailgen's arm64
# back end at its top: snx_sailgen_machine_backends' files, over the encoders of a64.sail, a64_enc.inc) into the build
# folder, and gives <target> its include root. It builds on any host: the code it writes is arm64's
function(snx_sailgen_a64_arm64 target)
    snx_sailgen_a64_encoders(${target})
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail")
    set(profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    snx_sailgen_machine_backends(arm64 a64 files args)
    add_custom_command(OUTPUT "${out}/a64_arm64.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a64_x64.roots" ${args} --profile "${profile}" -o "${out}/a64_arm64.inc"
                "${dir}/a64.sail" --overlay "${dir}/nx.sail"
        DEPENDS snx-sailgen "${dir}/a64_x64.roots" "${dir}/a64.sail" "${dir}/nx.sail" ${files} "${profile}"
        COMMENT "Generating the A64 emitters' arm64 code from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_arm64.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_a32_arm64(<target>): generates a32_arm64.inc (the AArch32 emitters of a32.roots with sailgen's arm64 back
# end at its top, over the A64 Sail's encoders) into the build folder, and gives <target> its include root. It builds
# on any host: the code it writes is arm64's
function(snx_sailgen_a32_arm64 target)
    snx_sailgen_a64_encoders(${target})
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    snx_sailgen_machine_backends(arm64 a32 files args)
    add_custom_command(OUTPUT "${out}/a32_arm64.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a32.roots" ${args} --profile "${dir}/a32.profile"
                -o "${out}/a32_arm64.inc" "${dir}/a32.sail" --overlay "${dir}/arm.sail"
        DEPENDS snx-sailgen "${dir}/a32.roots" "${dir}/a32.sail" "${dir}/arm.sail" "${dir}/a32.profile" ${files}
        COMMENT "Generating the AArch32 emitters' arm64 code from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a32_arm64.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_a64_risc(<target> <machine>): generates a64_<machine>.inc (the A64 emitters of a64_risc.roots with a RISC
# machine's back end at its top: snx_sailgen_machine_backends' files, over its encoders from LLVM's TableGen records,
# tools/sailgen/td/<machine>_enc.inc) into the build folder, and gives <target> its include roots. It builds on any
# host: the code it writes is the machine's
function(snx_sailgen_a64_risc target machine)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail")
    set(profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    snx_sailgen_machine_backends(${machine} a64 files args)
    # one rule a directory: a big-endian host's targets (ppc64be...) share the machine's emitters
    get_property(made DIRECTORY PROPERTY SNX_SAILGEN_A64_${machine})
    if(NOT made)
        set_property(DIRECTORY PROPERTY SNX_SAILGEN_A64_${machine} 1)
        add_custom_command(OUTPUT "${out}/a64_${machine}.inc"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
            COMMAND snx-sailgen --generate "${dir}/a64_risc.roots" ${args} --profile "${profile}"
                    -o "${out}/a64_${machine}.inc" "${dir}/a64.sail" --overlay "${dir}/nx.sail"
            DEPENDS snx-sailgen "${dir}/a64_risc.roots" "${dir}/a64.sail" "${dir}/nx.sail" ${files} "${profile}"
                    "${PROJECT_SOURCE_DIR}/tools/sailgen/td/${machine}_enc.inc"
            COMMENT "Generating the A64 emitters' ${machine} code from Sail" VERBATIM)
    endif()
    target_sources(${target} PRIVATE "${out}/a64_${machine}.inc")
    target_include_directories(${target} PRIVATE "${out}" "${PROJECT_SOURCE_DIR}/tools/sailgen/td")
endfunction()
# snx_sailgen_a32_risc(<target> <machine>): a32_<machine>.inc, the AArch32 emitters' (as snx_sailgen_a32_arm64)
function(snx_sailgen_a32_risc target machine)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    snx_sailgen_machine_backends(${machine} a32 files args)
    # one rule a directory: a big-endian host's targets (ppc64be...) share the machine's emitters
    get_property(made DIRECTORY PROPERTY SNX_SAILGEN_A32_${machine})
    if(NOT made)
        set_property(DIRECTORY PROPERTY SNX_SAILGEN_A32_${machine} 1)
        add_custom_command(OUTPUT "${out}/a32_${machine}.inc"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
            COMMAND snx-sailgen --generate "${dir}/a32.roots" ${args} --profile "${dir}/a32.profile"
                    -o "${out}/a32_${machine}.inc" "${dir}/a32.sail" --overlay "${dir}/arm.sail"
            DEPENDS snx-sailgen "${dir}/a32.roots" "${dir}/a32.sail" "${dir}/arm.sail" "${dir}/a32.profile" ${files}
                    "${PROJECT_SOURCE_DIR}/tools/sailgen/td/${machine}_enc.inc"
            COMMENT "Generating the AArch32 emitters' ${machine} code from Sail" VERBATIM)
    endif()
    target_sources(${target} PRIVATE "${out}/a32_${machine}.inc")
    target_include_directories(${target} PRIVATE "${out}" "${PROJECT_SOURCE_DIR}/tools/sailgen/td")
endfunction()
