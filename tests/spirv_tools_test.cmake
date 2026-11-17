# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Where cmake/spirv_tools.cmake takes spirv-opt, spirv-val and spirv-dis from: SNX_SPIRV_OPT and SNX_SPIRV_VAL, the
# system's at the pinned release or newer when SPIRV-Tools_FORCE_SYSTEM or CPMUTIL_FORCE_SYSTEM ask (or
# CPM_USE_LOCAL_PACKAGES, from before CPMUtil), else the pinned release, and what it says when it does not take what it
# was given or found. A small project includes it with cmake/packages.cmake and cpmfile.json's pins, fake tools (shell
# scripts that print a version) and AddJsonPackage replaced by one that only records its arguments, so nothing is
# downloaded or built. Run by ctest as spirv_tools, with -DSRC, -DWORK and -DCC (the C compiler the dependency cache's
# key runs)
if(NOT DEFINED SRC OR NOT DEFINED WORK OR NOT DEFINED CC)
    message(FATAL_ERROR "spirv_tools_test.cmake needs -DSRC=... -DWORK=... -DCC=<C compiler>")
endif()
file(REMOVE_RECURSE "${WORK}")

file(READ "${SRC}/cpmfile.json" cpmfile)
string(JSON v GET "${cpmfile}" spirv-tools version)
string(REGEX REPLACE "^v" "" v "${v}")

# a folder of fake tools, each printing VERSION as SPIRV-Tools does (on stderr, as spirv-opt does)
function(tools dir version)
    foreach(tool IN LISTS ARGN)
        file(WRITE "${dir}/spirv-${tool}" "#!/bin/sh\necho '${version}' >&2\n")
        file(CHMOD "${dir}/spirv-${tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endforeach()
endfunction()
tools("${WORK}/given" "SPIRV-Tools v2020.1" opt val dis)
tools("${WORK}/given-opt" "SPIRV-Tools v2020.1" opt)
tools("${WORK}/given-val" "SPIRV-Tools v2020.1" val)
tools("${WORK}/older" "SPIRV-Tools v2020.1 unknown hash" opt val dis)
tools("${WORK}/newer" "SPIRV-Tools v2099.1 unknown hash" opt val dis)
tools("${WORK}/newer-nodis" "SPIRV-Tools v2099.1 unknown hash" opt val)
tools("${WORK}/noversion" "spirv-opt from somewhere" opt val dis)

# the folders the replaced AddJsonPackage names, which ExternalProject_Add wants to hold something
foreach(name SPIRV-Tools SPIRV-Headers)
    file(WRITE "${WORK}/source-${name}/CMakeLists.txt" "message(FATAL_ERROR \"the test builds nothing\")\n")
endforeach()
file(COPY "${SRC}/cpmfile.json" DESTINATION "${WORK}/project")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(spirv_tools_test LANGUAGES NONE)
include(\"${SRC}/cmake/packages.cmake\")
macro(AddJsonPackage)
    cmake_parse_arguments(stub \"DOWNLOAD_ONLY\" \"NAME;FORCE_BUNDLED_PACKAGE\" \"\" \${ARGN})
    string(JSON stub_package GET \"\${CPMFILE_CONTENT}\" \${stub_NAME} package)
    set(\${stub_package}_SOURCE_DIR \"${WORK}/source-\${stub_package}\")
    set_property(GLOBAL APPEND PROPERTY stub_added
                 \"\${stub_NAME} FORCE_BUNDLED_PACKAGE=\${stub_FORCE_BUNDLED_PACKAGE}\")
endmacro()
include(\"${SRC}/cmake/deps_cache.cmake\")
include(\"${SRC}/cmake/spirv_tools.cmake\")
include(\"${SRC}/cmake/spirv_tools.cmake\") # again, as each folder that compiles shaders does: once is kept
set(line \"\")
foreach(tool opt val dis)
    if(TARGET snx-spirv-\${tool})
        get_target_property(at snx-spirv-\${tool} IMPORTED_LOCATION)
        string(APPEND line \"\${tool}=[\${at}] \")
    else()
        string(APPEND line \"\${tool}=[none] \")
    endif()
endforeach()
get_property(added GLOBAL PROPERTY stub_added)
list(JOIN added \" | \" added)
if(TARGET snx_spirv_tools_build)
    set(built yes)
else()
    set(built no)
endif()
message(STATUS \"RESULT \${line}built=[\${built}] added=[\${added}] \"
               \"option=[\$CACHE{SNX_SPIRV_OPT}|\$CACHE{SNX_SPIRV_VAL}]\")
")

# check(label PATH dir [FAIL] SAYS text... ARGS cmake-arg...): configures the project in a folder of its own with the
# ARGS and dir first on PATH, and checks it said each text, and that it failed when FAIL is given (else that it passed)
function(check label)
    cmake_parse_arguments(arg "FAIL" "PATH" "SAYS;ARGS" ${ARGN})
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_USE_LOCAL_PACKAGES --unset=SNX_DEPS_CACHE
                            "PATH=${arg_PATH}:$ENV{PATH}" "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/${label}"
                            "-DPython3_EXECUTABLE=${CMAKE_COMMAND}" ${arg_ARGS}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REPLACE "${WORK}/${label}/spirv-tools/bin" "PINNED" out "${out}")
    string(REPLACE "${WORK}" "WORK" out "${out}")
    string(REGEX REPLACE "\n *" " " flat "${out}") # CMake wraps an error's text
    if((arg_FAIL AND result EQUAL 0) OR (NOT arg_FAIL AND NOT result EQUAL 0))
        message(SEND_ERROR "${label}: the configure exited ${result}:\n${out}")
        set_property(GLOBAL APPEND PROPERTY failed "${label}")
        return()
    endif()
    foreach(want IN LISTS arg_SAYS)
        string(FIND "${flat}" "${want}" at)
        if(at LESS 0)
            message(SEND_ERROR "${label}: the configure did not say \"${want}\":\n${out}")
            set_property(GLOBAL APPEND PROPERTY failed "${label}")
        endif()
    endforeach()
endfunction()

# The texts name WORK for the test's folder and PINNED for where a tree builds the pinned tools
set(added "added=[spirv-tools FORCE_BUNDLED_PACKAGE=ON | spirv-headers FORCE_BUNDLED_PACKAGE=ON]")
set(all_pinned "RESULT opt=[PINNED/spirv-opt] val=[PINNED/spirv-val] dis=[PINNED/spirv-dis] built=[yes] ${added}")
set(given "opt=[WORK/given/spirv-opt] val=[WORK/given/spirv-val] dis=[WORK/given/spirv-dis] built=[no] added=[]")
set(newer "val=[WORK/newer/spirv-val] dis=[WORK/newer/spirv-dis] built=[no] added=[]")
set(local -DCPM_USE_LOCAL_PACKAGES=ON)

# nothing given: the three from the pinned release, built in the tree, whatever is on PATH
check(pinned PATH "${WORK}/newer" SAYS "${all_pinned}" "spirv-opt: PINNED/spirv-opt (pinned v${v}).")
# both given: those, spirv-dis beside spirv-val, nothing downloaded
check(given PATH "${WORK}/older" SAYS "RESULT ${given}"
      ARGS "-DSNX_SPIRV_OPT=${WORK}/given/spirv-opt" "-DSNX_SPIRV_VAL=${WORK}/given/spirv-val")
# spirv-opt given alone: spirv-val and spirv-dis beside it
check(given-beside PATH "${WORK}/older" SAYS "RESULT ${given}" ARGS "-DSNX_SPIRV_OPT=${WORK}/given/spirv-opt")
# spirv-opt given with no spirv-val beside it: the pinned spirv-val and spirv-dis
check(given-opt PATH "${WORK}/older"
      SAYS "RESULT opt=[WORK/given-opt/spirv-opt] val=[PINNED/spirv-val] dis=[PINNED/spirv-dis] built=[yes]"
      ARGS "-DSNX_SPIRV_OPT=${WORK}/given-opt/spirv-opt")
# both given, no spirv-dis beside spirv-val: none, said with where it was looked for
check(given-nodis PATH "${WORK}/older"
      SAYS "RESULT opt=[WORK/given-opt/spirv-opt] val=[WORK/given-val/spirv-val] dis=[none] built=[no] added=[]"
           "spirv-dis: none beside WORK/given-val/spirv-val, so the tests that need it are left out."
      ARGS "-DSNX_SPIRV_OPT=${WORK}/given-opt/spirv-opt" "-DSNX_SPIRV_VAL=${WORK}/given-val/spirv-val")
# SNX_SPIRV_DIS names spirv-dis too
check(given-dis PATH "${WORK}/older"
      SAYS "RESULT opt=[WORK/given-opt/spirv-opt] val=[WORK/given-val/spirv-val] dis=[WORK/given/spirv-dis] built=[no]"
      ARGS "-DSNX_SPIRV_OPT=${WORK}/given-opt/spirv-opt" "-DSNX_SPIRV_VAL=${WORK}/given-val/spirv-val"
           "-DSNX_SPIRV_DIS=${WORK}/given/spirv-dis")
# a name that is no file: refused with it
check(given-missing FAIL PATH "${WORK}/older" SAYS "SNX_SPIRV_OPT=WORK/nowhere/spirv-opt is not a file."
      ARGS "-DSNX_SPIRV_OPT=${WORK}/nowhere/spirv-opt")
check(given-missing-dis FAIL PATH "${WORK}/older" SAYS "SNX_SPIRV_DIS=WORK/nowhere/spirv-dis is not a file."
      ARGS "-DSNX_SPIRV_DIS=${WORK}/nowhere/spirv-dis")

# a tree configured before the pin: what it found on PATH sits in SNX_SPIRV_OPT and SNX_SPIRV_VAL beside its search's
# entries, and the pinned release replaces it, said once with the value; a value other than the search's was given
set(old_tree "-DSNX_SPIRV_OPT=${WORK}/older/spirv-opt" "-Dsnx_spirv_opt_path=${WORK}/older/spirv-opt"
             "-DSNX_SPIRV_VAL=${WORK}/older/spirv-val" "-Dsnx_spirv_val_path=${WORK}/older/spirv-val")
string(CONCAT from_path "SNX_SPIRV_OPT=WORK/older/spirv-opt was found on PATH by a configure from before SPIRV-Tools "
                        "was pinned, not given: the build uses the pinned v${v} now")
check(old-tree PATH "${WORK}/older" SAYS "${all_pinned} option=[|]" "${from_path}" ARGS ${old_tree})
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/old-tree" OUTPUT_VARIABLE again
                ERROR_VARIABLE again)
if(again MATCHES "configure from before")
    message(SEND_ERROR "old-tree: the second configure said the note again:\n${again}")
    set_property(GLOBAL APPEND PROPERTY failed old-tree-again)
endif()
file(STRINGS "${WORK}/old-tree/CMakeCache.txt" searches REGEX "^snx_spirv_(opt|val)_path:")
if(searches)
    message(SEND_ERROR "old-tree: the old configure's search entries are still in the cache: ${searches}")
    set_property(GLOBAL APPEND PROPERTY failed old-tree-entries)
endif()
check(old-tree-given PATH "${WORK}/older" SAYS "RESULT ${given}"
      ARGS "-DSNX_SPIRV_OPT=${WORK}/given/spirv-opt" "-Dsnx_spirv_opt_path=${WORK}/older/spirv-opt"
           "-DSNX_SPIRV_VAL=${WORK}/given/spirv-val" "-Dsnx_spirv_val_path=${WORK}/older/spirv-val")

# a tree whose tests' find_program found them first: that command's help text marks each
file(WRITE "${WORK}/old-search.cmake" "")
foreach(tool opt val dis)
    string(TOUPPER "${tool}" key)
    file(APPEND "${WORK}/old-search.cmake"
         "set(SNX_SPIRV_${key} \"${WORK}/older/spirv-${tool}\" CACHE FILEPATH \"Path to a program.\")\n")
endforeach()
string(REPLACE "SNX_SPIRV_OPT=WORK/older/spirv-opt" "SNX_SPIRV_DIS=WORK/older/spirv-dis" from_search "${from_path}")
check(old-search PATH "${WORK}/older" SAYS "${all_pinned} option=[|]" "${from_path}" "${from_search}"
      ARGS -C "${WORK}/old-search.cmake")

# CPM_USE_LOCAL_PACKAGES, from a configure before CPMUtil: the system's at the pin or newer, all three; older, without
# spirv-dis, or with no version in its --version, the pinned release, said with what was found and the pin
check(local-newer PATH "${WORK}/newer" SAYS "RESULT opt=[WORK/newer/spirv-opt] ${newer}"
      "spirv-opt: WORK/newer/spirv-opt (the system's v2099.1, CPM_USE_LOCAL_PACKAGES)."
      "CPM_USE_LOCAL_PACKAGES=ON is CPM.cmake's option, which CPMUtil's replace: read for SPIRV-Tools as" ARGS ${local})
string(CONCAT older "CPM_USE_LOCAL_PACKAGES=ON: WORK/older/spirv-opt is SPIRV-Tools v2020.1, older than the pinned "
                    "v${v}, so the build builds the pinned SPIRV-Tools v${v}")
check(local-older PATH "${WORK}/older" SAYS "${all_pinned}" "${older}" ARGS ${local})
check(local-nodis PATH "${WORK}/newer-nodis" SAYS "${all_pinned}"
      "WORK/newer-nodis/spirv-opt (v2099.1) has no spirv-dis beside it, so the build builds the pinned" ARGS ${local})
string(CONCAT noversion "WORK/noversion/spirv-opt --version printed \"spirv-opt from somewhere\" (status 0), which "
                        "names no SPIRV-Tools version")
check(local-noversion PATH "${WORK}/noversion" SAYS "${all_pinned}" "${noversion}" ARGS ${local})
# spirv-opt given: the system's fills spirv-val and spirv-dis
check(local-given-opt PATH "${WORK}/newer" SAYS "RESULT opt=[WORK/given-opt/spirv-opt] ${newer}"
      ARGS ${local} "-DSNX_SPIRV_OPT=${WORK}/given-opt/spirv-opt")
# CPMUtil's options: SPIRV-Tools' own and the global one ask for the system's, which must then be there; the pinned ones
# asked for win over CPM_USE_LOCAL_PACKAGES
check(system-newer PATH "${WORK}/newer" SAYS "RESULT opt=[WORK/newer/spirv-opt] ${newer}"
      "spirv-opt: WORK/newer/spirv-opt (the system's v2099.1, SPIRV-Tools_FORCE_SYSTEM)."
      ARGS -DSPIRV-Tools_FORCE_SYSTEM=ON)
check(all-system-newer PATH "${WORK}/newer" SAYS "RESULT opt=[WORK/newer/spirv-opt] ${newer}"
      "spirv-opt: WORK/newer/spirv-opt (the system's v2099.1, CPMUTIL_FORCE_SYSTEM)." ARGS -DCPMUTIL_FORCE_SYSTEM=ON)
string(CONCAT forced_older "SPIRV-Tools_FORCE_SYSTEM=ON: WORK/older/spirv-opt is SPIRV-Tools v2020.1, older than the "
                           "pinned v${v}.")
check(system-older FAIL PATH "${WORK}/older" SAYS "${forced_older}" "Put SPIRV-Tools v${v} or newer on PATH"
      ARGS -DSPIRV-Tools_FORCE_SYSTEM=ON)
check(local-bundled PATH "${WORK}/newer" SAYS "${all_pinned}" ARGS ${local} -DSPIRV-Tools_FORCE_BUNDLED=ON)

# the dependency cache's key: the same build of the tools for two trees whose Python is another file of one version
# (a venv's and the system's), another for another Python version
foreach(python a:3.14.0 b:3.14.0 c:3.13.0)
    string(REPLACE ":" ";" python "${python}")
    list(GET python 0 name)
    list(GET python 1 version)
    file(WRITE "${WORK}/python-${name}" "#!/bin/sh\necho 'Python ${version}'\n")
    file(CHMOD "${WORK}/python-${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    check(key-${name} PATH "${WORK}/older" SAYS "built=[yes]"
          ARGS "-DSNX_DEPS_CACHE=${WORK}/deps" "-DCMAKE_C_COMPILER=${CC}" "-DPython3_EXECUTABLE=${WORK}/python-${name}")
    file(READ "${WORK}/key-${name}/snx-deps/spirv-tools.dir" dir_${name})
endforeach()
if(NOT dir_a STREQUAL dir_b OR dir_a STREQUAL dir_c)
    message(SEND_ERROR "the tools' cache folder is ${dir_a}, ${dir_b} and ${dir_c} for Pythons 3.14.0, 3.14.0 and "
                       "3.13.0")
    set_property(GLOBAL APPEND PROPERTY failed key)
endif()

get_property(failed GLOBAL PROPERTY failed)
if(failed)
    message(FATAL_ERROR "spirv_tools.cmake: ${failed} failed (the configures are in ${WORK})")
endif()
file(REMOVE_RECURSE "${WORK}")
