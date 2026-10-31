# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# scarletnx -r --KEY=VALUE, a setting of the recompile (settings.txt's recompile and llvm tables): a value the setting
# does not take is refused before anything runs, saying what it takes; one it takes reaches the input's check. Run by
# ctest as runtime_recompile_settings
set(dir "${CMAKE_CURRENT_BINARY_DIR}/recompile-settings")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
foreach(case "--aot_threads=abc|aot_threads=abc is not a whole number from 0 to 1024"
             "--aot_threads=1.5|aot_threads=1.5 is not a whole number from 0 to 1024"
             "--aot_threads=2000|aot_threads=2000 is not a whole number from 0 to 1024"
             "--aot_threads=|aot_threads= is not a whole number from 0 to 1024"
             "--llvm_inliner_threshold=50x|llvm_inliner_threshold=50x is not a whole number from -2147483647 to 2147483647"
             "--opt=5|opt = 5: not 0, 1, 2, 3, s or z"
             "--unit_cache_gb=-1|unit_cache_gb=-1 is not a whole number from 0 to 100000"
             "--unit_cache_gb=2.5|unit_cache_gb=2.5 is not a whole number from 0 to 100000"
             "--aot_threads=4|holds no exefs/main" "--llvm_inliner_threshold=50|holds no exefs/main"
             "--opt=z|holds no exefs/main" "--unit_cache_gb=0|holds no exefs/main"
             "--llvm_batch=fast|llvm_batch = fast: not full or ipo"
             "--opt=0 --llvm_batch=ipo|llvm_batch = ipo takes the place of -O1 to -O3, s and z's passes: opt = 0"
             "--llvm_batch=ipo|holds no exefs/main" "--llvm_batch=full|holds no exefs/main")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 option)
    list(GET case 1 expected)
    separate_arguments(option UNIX_COMMAND "${option}") # a case of two options
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" -r missing.nro ${option}
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out
                    ERROR_VARIABLE out)
    string(FIND "${out}" "${expected}" at)
    if(result EQUAL 0 OR at LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx -r missing.nro ${option} ended with ${result}, not \"${expected}\":\n${out}")
    endif()
endforeach()
# AOT pack build: a prepared game, recompiled with the cache the launcher names. --jit-cache and
# --hook-direct-calls are the recompiler's, not extra dump inputs; before this case the automatic
# preparation check took them for game arguments and stopped with "accepts one dump file".
set(aot "${dir}/aot")
set(aot_game "${aot}/0100000000000000")
file(MAKE_DIRECTORY "${aot_game}" "${aot_game}/0100000000000000_data")
file(WRITE "${aot_game}/0100000000000000" "")
file(WRITE "${aot}/profile.jitcache" "")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                        "${RUNTIME}" --repo "${aot}" --build-dir "${aot}" -r "${aot_game}"
                        --jit-cache "${aot}/profile.jitcache" --hook-direct-calls -O 2
                WORKING_DIRECTORY "${aot}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out
                ERROR_VARIABLE out)
string(FIND "${out}" "automatic preparation accepts one dump file" bad)
if(bad GREATER -1)
    message(FATAL_ERROR "AOT build's --jit-cache/--hook-direct-calls were taken for extra dumps:\n${out}")
endif()
string(FIND "${out}" "holds no exefs/main" reached)
if(reached LESS 0)
    message(FATAL_ERROR "AOT build did not reach the recompiler:\n${out}")
endif()
file(REMOVE_RECURSE "${dir}")
