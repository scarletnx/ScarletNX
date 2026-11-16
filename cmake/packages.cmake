# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The third-party packages, through CPMUtil (cmake/cpmutil, crueter's wrapper of CPM.cmake): cpmfile.json pins each
# one, by its version and the SHA-512 of the file downloaded, and AddJsonPackage(<key>) adds it, downloaded once into
# CPM_SOURCE_CACHE (.cache/cpm in the repository by default, shared by every build folder) and taken from there by
# every configure after. Configuring offline works once the cache is filled, or with -D<package>_CUSTOM_DIR naming a
# folder that holds a package already.
#
# Where a package comes from is CPMUtil's to say, by its options. A package whose entry has a min_version may be the
# system's: find_package looks for it at that version or newer when the entry is not "bundled" (SDL3), or when
# -D<package>_FORCE_SYSTEM=ON or -DCPMUTIL_FORCE_SYSTEM=ON ask (LZ4, the Vulkan headers); <package>_FORCE_BUNDLED and
# CPMUTIL_FORCE_BUNDLED keep the pinned one. Every other package is added with FORCE_BUNDLED_PACKAGE ON: the build
# compiles its sources into a target of its own, patches it, or reads it as data, so no system package stands in for
# it (snx_cpm_check_pinned says so when one is asked for).
include_guard(GLOBAL)

# CPMUtil reads CPM_SOURCE_CACHE from the cache alone; the environment's came next before it, as CPM.cmake reads it
if(NOT DEFINED CPM_SOURCE_CACHE AND DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "$ENV{CPM_SOURCE_CACHE}" CACHE PATH "Directory to download CPM dependencies")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cpmutil")
include(CPMUtil)

# CPM_USE_LOCAL_PACKAGES as given, for snx_cpm_options to read; then off, with CPM_LOCAL_PACKAGES_ONLY, in this scope:
# where a project that adds ScarletNX brings the whole CPM.cmake, which CPMUtil then calls, those would have it look for
# every package on the system first, a pinned source CPMUtil hands it among them
set(snx_cpm_use_local_packages "${CPM_USE_LOCAL_PACKAGES}")
set(snx_cpm_use_local_packages_from "CPM_USE_LOCAL_PACKAGES=${CPM_USE_LOCAL_PACKAGES}")
if(NOT DEFINED CPM_USE_LOCAL_PACKAGES AND DEFINED ENV{CPM_USE_LOCAL_PACKAGES})
    set(snx_cpm_use_local_packages "$ENV{CPM_USE_LOCAL_PACKAGES}")
    set(snx_cpm_use_local_packages_from "CPM_USE_LOCAL_PACKAGES=$ENV{CPM_USE_LOCAL_PACKAGES} in the environment")
endif()
set(CPM_USE_LOCAL_PACKAGES OFF)
set(CPM_LOCAL_PACKAGES_ONLY OFF)

# CPMUtil takes a <package>_CUSTOM_DIR as it is, without its entry's patches: refused for a patched package, which would
# build unpatched otherwise
function(snx_cpm_check_custom_dirs)
    string(JSON count LENGTH "${CPMFILE_CONTENT}")
    if(NOT count)
        return()
    endif()
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        string(JSON key MEMBER "${CPMFILE_CONTENT}" ${i})
        string(JSON patches ERROR_VARIABLE error GET "${CPMFILE_CONTENT}" "${key}" patches)
        if(error)
            continue()
        endif()
        string(JSON package ERROR_VARIABLE error GET "${CPMFILE_CONTENT}" "${key}" package)
        if(error)
            set(package "${key}")
        endif()
        if(NOT ${package}_CUSTOM_DIR)
            continue()
        endif()
        string(JSON n LENGTH "${patches}")
        math(EXPR n "${n} - 1")
        set(names "")
        foreach(j RANGE ${n})
            string(JSON name GET "${patches}" ${j})
            list(APPEND names "${name}")
        endforeach()
        list(JOIN names ", " names)
        message(FATAL_ERROR "${package}_CUSTOM_DIR=${${package}_CUSTOM_DIR}: CPMUtil takes that folder as it is, and "
                            "cpmfile.json patches ${package} with ${names} (.patch/${key}). Configure with "
                            "-D${package}_CUSTOM_DIR= for the pinned release, which CPMUtil downloads and patches.")
    endforeach()
endfunction()
snx_cpm_check_custom_dirs()

# A field of a cpmfile.json entry (a version a message names, a hash a cache key holds), stopping the configure when
# the entry has none
function(snx_cpmfile out key field)
    string(JSON value ERROR_VARIABLE error GET "${CPMFILE_CONTENT}" "${key}" "${field}")
    if(error)
        message(FATAL_ERROR "cpmfile.json: ${key} has no ${field} (${error})")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# CPM.cmake's options for where a package comes from, which this build took before CPMUtil, read for the package
# <key> before it is added: CPM_DOWNLOAD_<package>=ON as <package>_FORCE_BUNDLED=ON, and CPM_USE_LOCAL_PACKAGES=ON (or
# in the environment), for a package pinned unless the system's is asked for, as <package>_FORCE_SYSTEM=ON where
# find_package finds the system's at min_version or newer, else the pinned one, as CPM took it. CPMUtil's own options
# win. Each reading is said once per build tree. The variable it sets is the caller's
function(snx_cpm_options key)
    string(JSON package ERROR_VARIABLE error GET "${CPMFILE_CONTENT}" "${key}" package)
    if(error)
        set(package "${key}")
    endif()
    string(JSON bundled ERROR_VARIABLE error GET "${CPMFILE_CONTENT}" "${key}" bundled)
    if(error)
        set(bundled OFF)
    endif()
    snx_cpmfile(version "${key}" version)
    if(CPM_DOWNLOAD_${package})
        set(old "CPM_DOWNLOAD_${package}=${CPM_DOWNLOAD_${package}}")
    elseif(snx_cpm_use_local_packages AND bundled)
        set(old "${snx_cpm_use_local_packages_from}")
    else()
        return()
    endif()
    set(cpmutil "")
    foreach(option ${package}_FORCE_SYSTEM ${package}_FORCE_BUNDLED CPMUTIL_FORCE_SYSTEM CPMUTIL_FORCE_BUNDLED)
        if(${option})
            set(cpmutil "${option}=ON")
            break()
        endif()
    endforeach()
    if(cpmutil)
        set(note "${old} is CPM.cmake's option, which CPMUtil's replace, and ${cpmutil} says where ${package} comes "
                 "from instead.")
    elseif(CPM_DOWNLOAD_${package})
        set(${package}_FORCE_BUNDLED ON PARENT_SCOPE)
        set(note "${old} is CPM.cmake's option, which CPMUtil's replace: read as ${package}_FORCE_BUNDLED=ON, the "
                 "pinned ${version}. Configure with -D${package}_FORCE_BUNDLED=ON instead.")
    else()
        SystemPackageViable(${key})
        snx_cpmfile(minimum "${key}" min_version)
        if(${key}_VIABLE)
            set(${package}_FORCE_SYSTEM ON PARENT_SCOPE)
            set(found "the system's, which find_package finds at ${minimum} or newer (${package}_FORCE_SYSTEM=ON)")
        else()
            set(found "the pinned ${version}: find_package finds no ${package} ${minimum} or newer")
        endif()
        set(note "${old} is CPM.cmake's option, which CPMUtil's replace: ${package} is ${found}. Configure with "
                 "-D${package}_FORCE_SYSTEM=ON, or -DCPMUTIL_FORCE_SYSTEM=ON for every package the system may give, "
                 "instead.")
    endif()
    snx_cpm_note(${package} ${note})
endfunction()

# A note on <package>, its text the arguments after it, said once per build tree and again only when the text changes:
# how an option from before CPMUtil is read
function(snx_cpm_note package)
    string(JOIN "" note ${ARGN})
    if(NOT "$CACHE{snx_cpm_noted_${package}}" STREQUAL note)
        message(NOTICE "${note}")
        set(snx_cpm_noted_${package} "${note}" CACHE INTERNAL "The note on the options for ${package} said last")
    endif()
endfunction()

# The registry (vk.xml) a system's Vulkan-Headers package installs beside its headers, in share/vulkan/registry of the
# same prefix: the Vulkan settings are checked against it (cmake/settings.cmake). Its absence stops the configure, with
# the option that took the system's headers
function(snx_system_vulkan_registry out)
    get_target_property(include Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    list(GET include 0 include)
    cmake_path(SET registry NORMALIZE "${include}/../share/vulkan/registry/vk.xml")
    if(NOT EXISTS "${registry}")
        set(why Vulkan-Headers_FORCE_SYSTEM)
        if(NOT Vulkan-Headers_FORCE_SYSTEM)
            set(why CPMUTIL_FORCE_SYSTEM)
        endif()
        message(FATAL_ERROR "${why}=ON took the system's Vulkan headers in ${include}, whose registry ${registry} is "
                            "not a file. Install the package that has it, or configure with -D${why}=OFF for the "
                            "pinned headers.")
    endif()
    set(${out} "${registry}" PARENT_SCOPE)
endfunction()

#   snx_package_file(<key>)
#
# A package of one file, which CPMUtil would take for an archive to extract: its entry's url, or the release artifact
# of repo and version, kept as downloaded in <package>_SOURCE_DIR, the folder CPMUtil would extract it into, or the
# folder <package>_CUSTOM_DIR names. The file is checked against the entry's SHA-512 on every configure, and downloaded
# again when it is missing or another (a copy cut short, a file put there by hand), next to its place and moved there
# once checked, while no other configure sharing the cache fetches it
function(snx_package_file key)
    get_json_object("${key}")
    set(JSON_NAME "${key}")
    parse_object("${object}")
    if(NOT hash MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "cpmfile.json: ${key}'s hash is [${hash}], not the SHA-512 its download is checked against")
    endif()
    if(url)
        set(from "${url}")
    else()
        get_package_url(URL_OUT from GIT_HOST "${git_host}" REPO "${repo}" VERSION "${version}"
                        ARTIFACT "${artifact}" PACKAGE "${package}")
    endif()
    get_filename_component(name "${from}" NAME)
    set(${package}_CUSTOM_DIR "" CACHE STRING "Path to a separately-downloaded copy of ${package}")
    if(${package}_CUSTOM_DIR)
        set(dir "${${package}_CUSTOM_DIR}")
    else()
        get_cache_path(${package} "${version}" dir)
    endif()
    set(have "")
    if(EXISTS "${dir}/${name}")
        file(SHA512 "${dir}/${name}" have)
    endif()
    if(NOT have STREQUAL hash AND ${package}_CUSTOM_DIR)
        message(FATAL_ERROR "${package}_CUSTOM_DIR=${dir}: ${dir}/${name} has the SHA-512 [${have}], and "
                            "cpmfile.json pins ${hash}")
    endif()
    if(NOT have STREQUAL hash)
        file(LOCK "${dir}.lock" GUARD FUNCTION)
        if(EXISTS "${dir}/${name}")
            file(SHA512 "${dir}/${name}" have)
        endif()
        if(NOT have STREQUAL hash)
            cpm_utils_message(STATUS "Downloading ${package}@${version} from ${from}")
            cpm_download("${from}" "${dir}/${name}.part" ${hash})
            file(RENAME "${dir}/${name}.part" "${dir}/${name}")
        endif()
    endif()
    get_property(added GLOBAL PROPERTY CPM_PACKAGE_NAMES)
    if(NOT package IN_LIST added)
        cpmutil_register_package(${package} "${from}" "${version}")
    endif()
    set(${package}_ADDED YES PARENT_SCOPE)
    set(${package}_SOURCE_DIR "${dir}" PARENT_SCOPE)
endfunction()

# Each package added with FORCE_BUNDLED_PACKAGE ON is the pinned one, whatever <package>_FORCE_SYSTEM and
# CPMUTIL_FORCE_SYSTEM say, which CPMUtil does without a word: once every package is added, an asked one stops the
# configure, and CPMUTIL_FORCE_SYSTEM=ON is said with the packages it leaves pinned
function(snx_cpm_check_pinned)
    get_property(names GLOBAL PROPERTY CPM_PACKAGE_NAMES)
    get_property(versions GLOBAL PROPERTY CPM_PACKAGE_SHAS)
    set(asked "")
    set(pinned "")
    foreach(name version IN ZIP_LISTS names versions)
        if(version MATCHES " \\(system\\)$" OR "${name}@${version}" IN_LIST pinned)
            continue()
        endif()
        if(${name}_FORCE_SYSTEM)
            list(APPEND asked "${name}_FORCE_SYSTEM=ON, and ${name} is the pinned ${version}")
        endif()
        list(APPEND pinned "${name}@${version}")
    endforeach()
    if(asked)
        list(JOIN asked "; " asked)
        message(FATAL_ERROR "${asked}: the build compiles it into a target of its own, patches it or reads it as "
                            "data, so no system package stands in for it. Configure with -D<package>_FORCE_SYSTEM=OFF, "
                            "or give a copy of the pinned release with -D<package>_CUSTOM_DIR=<folder>.")
    endif()
    if(CPMUTIL_FORCE_SYSTEM AND pinned)
        list(JOIN pinned ", " pinned)
        message(STATUS "CPMUTIL_FORCE_SYSTEM=ON takes from the system the packages it may give, and leaves these "
                       "pinned, as no system package stands in for them (-D<package>_CUSTOM_DIR=<folder> gives one "
                       "without a download): ${pinned}")
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL snx_cpm_check_pinned)
