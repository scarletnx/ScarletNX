# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The x86-64 back end is configured with no manual, no pdftotext and no network (cmake/sdm.cmake): a small project takes
# sdm.cmake beside an empty sailgen.cmake and no package functions, so a configure that asked for the manual would stop at
# a command it doesn't know. SNX_TEST_SDM, which does ask, stops where it lacks pdftotext, and where Intel's link doesn't
# give the pinned revision or doesn't answer says what was expected, what came and what to do; a file already there that
# is another stops it too. Run by ctest as sdm_configure, with -DSRC and -DWORK
cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "sdm_configure_test.cmake needs -DSRC=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/manual.pdf" "the manual\n")
file(WRITE "${WORK}/other.pdf" "another revision\n")
file(SHA256 "${WORK}/manual.pdf" sha)
file(SHA256 "${WORK}/other.pdf" other)
file(READ "${SRC}/cmake/sdm.cmake" text)
file(WRITE "${WORK}/src/cmake/sdm.cmake" "${text}")
file(WRITE "${WORK}/src/cmake/sailgen.cmake" "")
# the pinned revision is the test's manual
file(READ "${SRC}/tools/sailgen/sdm/isel.py" isel)
string(REGEX REPLACE "(^|\n)SHA256 = '[0-9a-f]+'" "\\1SHA256 = '${sha}'" isel "${isel}")
if(NOT isel MATCHES "SHA256 = '${sha}'")
    message(FATAL_ERROR "tools/sailgen/sdm/isel.py has no SHA256 = '...' to replace")
endif()
file(WRITE "${WORK}/src/tools/sailgen/sdm/isel.py" "${isel}")
file(WRITE "${WORK}/src/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)\nproject(sdm LANGUAGES NONE)\n"
     "include(cmake/sdm.cmake)\nmessage(STATUS \"SNX_X64 is \${SNX_X64}\")\n"
     "if(ASK)\nfunction(get_json_object key)\nset(object \"{}\" PARENT_SCOPE)\nendfunction()\n"
     "macro(parse_object object)\nset(package IntelSDM)\nset(version 1)\nset(url \"\${URL}\")\nendmacro()\n"
     "function(get_cache_path package version out)\nset(\${out} \"\${CACHE_FOLDER}/\${package}/\${version}\" PARENT_SCOPE)\n"
     "endfunction()\nfunction(snx_package_file key)\nmessage(STATUS \"checked \${key}\")\nendfunction()\n"
     "snx_sdm_pdf(pdf pdftotext)\nmessage(STATUS \"pdf is \${pdf}, pdftotext is [\${pdftotext}]\")\nendif()\n")

# configure(<name> <stops|goes> <texts the output has to have, a list> <cmake args>...): the build folder is <WORK>/<name>,
# the package cache <WORK>/<name>-cache
function(configure name want says)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/src" -B "${WORK}/${name}" -DSNX_SAIL_X64=ON
                            "-DCACHE_FOLDER=${WORK}/${name}-cache" ${ARGN}
                    RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX REPLACE "[ \n]+" " " out "${out}")
    if(failed AND want STREQUAL "goes")
        message(FATAL_ERROR "${name}: the configure stopped: ${out}")
    elseif(NOT failed AND want STREQUAL "stops")
        message(FATAL_ERROR "${name}: the configure went through: ${out}")
    endif()
    foreach(text ${says})
        string(FIND "${out}" "${text}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${name}: the configure didn't say \"${text}\": ${out}")
        endif()
    endforeach()
endfunction()
# a find_program is not run again over a value that is no -NOTFOUND: a program the project is told there is none of, or
# one it is told there is
set(none -DSNX_PDFTOTEXT=OFF)
set(with "-DSNX_PDFTOTEXT=${CMAKE_COMMAND}")
configure(on goes "SNX_X64 is ON" ${none})
configure(off goes "SNX_X64 is OFF" -DSNX_SAIL_X64=OFF ${none})
configure(asks goes "pdf is ${WORK}/asks-cache/IntelSDM/1/manual.pdf, pdftotext is []" -DASK=ON
          "-DURL=https://sdm.test/manual.pdf" ${none})
configure(named goes "pdf is /a/manual.pdf, pdftotext is [${CMAKE_COMMAND}]" -DASK=ON -DSNX_SDM_PDF=/a/manual.pdf ${with})
configure(test_needs stops "SNX_TEST_SDM=ON needs pdftotext (poppler's), which is missing: install it (docs/dev/build.md)"
          -DASK=ON -DSNX_TEST_SDM=ON ${none})
configure(test_named goes "pdf is /a/manual.pdf" -DASK=ON -DSNX_TEST_SDM=ON -DSNX_SDM_PDF=/a/manual.pdf ${with})
configure(test_downloads goes "checked intel-sdm-vol2;pdf is ${WORK}/test_downloads-cache/IntelSDM/1/manual.pdf" -DASK=ON -DSNX_TEST_SDM=ON
          "-DURL=file://${WORK}/manual.pdf" ${with})
if(NOT EXISTS "${WORK}/test_downloads-cache/IntelSDM/1/manual.pdf")
    message(FATAL_ERROR "test_downloads: the cache has no file")
endif()
set(todo "-DSNX_SDM_PDF=" "cpmfile.json's intel-sdm-vol2" "SHA256 of tools/sailgen/sdm/isel.py")
configure(other_revision stops "gives a file whose SHA-256 is ${other}, not revision;SHA-256 is ${sha};${todo}" -DASK=ON
          -DSNX_TEST_SDM=ON "-DURL=file://${WORK}/other.pdf" ${with})
configure(no_answer stops "did not download 3 times, the last with status;SHA-256 is ${sha};${todo}" -DASK=ON
          -DSNX_TEST_SDM=ON "-DURL=file://${WORK}/none.pdf" ${with})
file(MAKE_DIRECTORY "${WORK}/kept-cache/IntelSDM/1")
file(COPY_FILE "${WORK}/other.pdf" "${WORK}/kept-cache/IntelSDM/1/manual.pdf")
configure(kept stops "isn't revision;its SHA-256 is ${other};SHA-256 is ${sha};${todo}" -DASK=ON -DSNX_TEST_SDM=ON
          "-DURL=https://sdm.test/manual.pdf" ${with})
message("sdm_configure ok")
