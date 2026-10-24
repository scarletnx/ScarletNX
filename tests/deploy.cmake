# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake/deploy.cmake copies the runtime beside each game of SNX_RECOMPILED_DIR, else of the data folder a build tree's
# driver uses (snx_data_dir): a .scarletnx/ beside the runtime, else the user's ($XDG_DATA_HOME/scarletnx, a relative
# one ignored). Without a home folder it deploys nothing and says so. Run by ctest as runtime_deploy, with no
# runtime: any file is copied the same way
set(dir "${CMAKE_CURRENT_BINARY_DIR}/runtime-deploy")
set(script "${CMAKE_CURRENT_LIST_DIR}/../cmake/deploy.cmake")
file(REMOVE_RECURSE "${dir}")
file(WRITE "${dir}/bin/scarletnx" "runtime")
# a recompiled game in games, with its update's module beside its own, and another in a folder whose name a glob
# would read as a pattern
function(game games)
    file(WRITE "${games}/Game/Game_code.so" "")
    file(WRITE "${games}/Game/Game_01000000000AB800_code.so" "")
    file(WRITE "${games}/Game [JP]/Game [JP]_code.so" "")
endfunction()
# deploy.cmake with SNX_RECOMPILED_DIR games, in an environment without HOME and XDG_DATA_HOME but for ARGN's NAME=VALUE
function(deploy games)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=XDG_DATA_HOME --unset=HOME ${ARGN}
                            "${CMAKE_COMMAND}" "-DRUNTIME=${dir}/bin/scarletnx" "-DGAMES=${games}" -DWINDOWS=0 -DMACOS=0
                            -P "${script}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    set(result "${result}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
endfunction()
function(deployed games what)
    if(NOT result EQUAL 0 OR NOT EXISTS "${games}/Game/Game" OR NOT EXISTS "${games}/Game/Game_01000000000AB800"
       OR NOT EXISTS "${games}/Game [JP]/Game [JP]")
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "${what}: the runtime is not beside each game module in ${games} (${result}):\n${out}")
    endif()
endfunction()

game("${dir}/named")
deploy("${dir}/named")
deployed("${dir}/named" "SNX_RECOMPILED_DIR")
game("${dir}/na[me]d*")
deploy("${dir}/na[me]d*")
deployed("${dir}/na[me]d*" "an SNX_RECOMPILED_DIR whose name a glob would read as a pattern")
# a 3DS game, named after its title ID: the 3DS runtime's (CTR_RUNTIME), never the Switch runtime's
set(ctr "0004000000ABCD00")
file(WRITE "${dir}/bin/scarletnx-3ds" "3ds runtime")
file(WRITE "${dir}/ctr/${ctr}/${ctr}_code.so" "")
deploy("${dir}/ctr")
if(NOT result EQUAL 0 OR EXISTS "${dir}/ctr/${ctr}/${ctr}")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "the Switch runtime was deployed to a 3DS game (${result}):\n${out}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCTR_RUNTIME=${dir}/bin/scarletnx-3ds" "-DGAMES=${dir}/ctr" -DWINDOWS=0
                        -DMACOS=0 -P "${script}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(EXISTS "${dir}/ctr/${ctr}/${ctr}")
    file(READ "${dir}/ctr/${ctr}/${ctr}" copied)
endif()
if(NOT result EQUAL 0 OR NOT copied STREQUAL "3ds runtime")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "the 3DS runtime is not in ${dir}/ctr/${ctr}/ (${result}):\n${out}")
endif()
# a GBA game, named any way, its .ini saying so (system=, the last one): the GBA runtime's (AGB_RUNTIME), never the
# Switch runtime's
file(WRITE "${dir}/bin/scarletnx-agb" "gba runtime")
file(WRITE "${dir}/agb/Homebrew/Homebrew_code.so" "")
file(WRITE "${dir}/agb/Homebrew/Homebrew.ini" "[Game]\nsystem=switch\nsource=/roms/Homebrew.gba\nsystem=gba\n")
deploy("${dir}/agb")
if(NOT result EQUAL 0 OR EXISTS "${dir}/agb/Homebrew/Homebrew"
   OR NOT out MATCHES "the 1 game module\\(s\\) in .* are another")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "the Switch runtime was deployed to a GBA game, or no line said why not (${result}):\n${out}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DAGB_RUNTIME=${dir}/bin/scarletnx-agb" "-DGAMES=${dir}/agb" -DWINDOWS=0
                        -DMACOS=0 -P "${script}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
set(copied "")
if(EXISTS "${dir}/agb/Homebrew/Homebrew")
    file(READ "${dir}/agb/Homebrew/Homebrew" copied)
endif()
if(NOT result EQUAL 0 OR NOT copied STREQUAL "gba runtime")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "the GBA runtime is not in ${dir}/agb/Homebrew/ (${result}):\n${out}")
endif()
# on Windows too, where its module is a .so all the same: Homebrew.exe; a .so of a folder that says no gba, nothing
file(WRITE "${dir}/agb-win/Homebrew/Homebrew_code.so" "")
file(WRITE "${dir}/agb-win/Homebrew/Homebrew.ini" "[Game]\nsystem=gba\n")
file(WRITE "${dir}/agb-win/Stray/Stray_code.so" "")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=${dir}/bin/scarletnx" "-DAGB_RUNTIME=${dir}/bin/scarletnx-agb"
                        "-DGAMES=${dir}/agb-win" -DWINDOWS=1 -DMACOS=0 -P "${script}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
set(copied "")
if(EXISTS "${dir}/agb-win/Homebrew/Homebrew.exe")
    file(READ "${dir}/agb-win/Homebrew/Homebrew.exe" copied)
endif()
if(NOT result EQUAL 0 OR NOT copied STREQUAL "gba runtime" OR EXISTS "${dir}/agb-win/Stray/Stray.exe")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "on Windows the GBA runtime is not Homebrew.exe, or a stray .so got one (${result}):\n${out}")
endif()
# a console this build has no word for: no runtime, and the line says which
file(WRITE "${dir}/odd/Odd/Odd_code.so" "")
file(WRITE "${dir}/odd/Odd/Odd.ini" "system=nds\n")
deploy("${dir}/odd")
if(NOT result EQUAL 0 OR EXISTS "${dir}/odd/Odd/Odd" OR NOT out MATCHES "Odd.ini says system=nds")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "a game of system=nds got a runtime, or no line said why not (${result}):\n${out}")
endif()
game("${dir}/xdg/scarletnx/recompiled")
deploy("" "XDG_DATA_HOME=${dir}/xdg" "HOME=${dir}/home")
deployed("${dir}/xdg/scarletnx/recompiled" "the user's data folder")
game("${dir}/home/.local/share/scarletnx/recompiled")
deploy("" "XDG_DATA_HOME=relative" "HOME=${dir}/home")
deployed("${dir}/home/.local/share/scarletnx/recompiled" "a relative XDG_DATA_HOME")
game("${dir}/bin/.scarletnx/recompiled")
deploy("" "XDG_DATA_HOME=${dir}/xdg")
deployed("${dir}/bin/.scarletnx/recompiled" "a portable copy's")
file(REMOVE_RECURSE "${dir}/bin/.scarletnx")
deploy("")
file(REMOVE_RECURSE "${dir}")
if(NOT result EQUAL 0 OR NOT out MATCHES "Runtime not deployed: no home folder to find the games in")
    message(FATAL_ERROR "without a home folder deploy.cmake ended with ${result}, not saying so:\n${out}")
endif()
