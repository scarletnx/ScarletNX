# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# OpenSSL's libcrypto and libssl as static libraries (targets OpenSSL::Crypto and OpenSSL::SSL), built from the release
# source in cmake/dependencies.cmake with OpenSSL's own Configure (Perl) and make. libcrypto does the hashing and
# ciphers (NSO, NRR and 3DS hashes, csrng, hactool's AES, SHA and RSA), libssl the ssl service's TLS. Only the libraries
# are built: no programs, docs, tests or loadable modules, and no configuration file is read at startup, so the result
# needs nothing but libc and behaves the same everywhere. OpenSSL is Apache 2.0. It needs Perl and make (gmake on the
# BSDs).
# This file is included into the top-level scope: its variables carry the snx_openssl_ prefix.
set(snx_openssl_src "${OpenSSL_SOURCE_DIR}")
set(snx_openssl_prefix "${CMAKE_BINARY_DIR}/openssl")
find_package(Perl)
find_program(SNX_OPENSSL_MAKE NAMES gmake make)
if(NOT PERL_FOUND OR NOT SNX_OPENSSL_MAKE)
    message(FATAL_ERROR "Building OpenSSL needs Perl and GNU make (gmake on the BSDs) on the PATH. On Windows, Perl "
                        "comes with Git for Windows or Strawberry Perl. OpenSSL is required.")
endif()

# Configure guesses the platform from the machine doing the build, which is right for a native build. A Windows build
# (native with llvm-mingw, or cross) names it: mingw64 is OpenSSL's target for 64-bit MinGW, x86-64 or ARM64. Android's
# targets find the NDK's Clang on PATH and name the compiler themselves (<triple><API>-clang), from ANDROID_NDK_ROOT
# and the API level, so that build passes those instead of CC, AR and RANLIB
set(snx_openssl_target "")
set(snx_openssl_env "")
if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(snx_openssl_target android-arm64)
        set(snx_openssl_android_cc aarch64-linux-android${ANDROID_PLATFORM_LEVEL}-clang)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(snx_openssl_target android-x86_64)
        set(snx_openssl_android_cc x86_64-linux-android${ANDROID_PLATFORM_LEVEL}-clang)
    else()
        message(FATAL_ERROR "OpenSSL's Android build is only set up for arm64-v8a and x86_64 (cmake/openssl.cmake)")
    endif()
    set(snx_openssl_env "${CMAKE_COMMAND}" -E env "ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK}"
                        "PATH=${ANDROID_TOOLCHAIN_ROOT}/bin:$ENV{PATH}")
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(snx_openssl_target mingw-arm64)
    else()
        set(snx_openssl_target mingw64)
    endif()
elseif(IOS)
    # 64-bit iOS: its compiler, Xcode's Clang, gets the SDK and oldest version from the flags below
    set(snx_openssl_target ios64-xcrun)
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross-building OpenSSL is only set up for Windows, Android and iOS (cmake/openssl.cmake)")
endif()
set(snx_openssl_args
    ${snx_openssl_target}
    --libdir=lib
    no-shared
    no-module
    no-apps
    no-docs
    no-tests
    no-autoload-config)
# macOS and iOS: the compiler with the architecture, SDK and oldest system version CMake's compiles get
# (cmake/apple.cmake). They go in CC, not among the arguments: Configure takes a bare word there for its target, and
# read the arm64 of -arch arm64 as one
set(snx_openssl_cc "${CMAKE_C_COMPILER}")
if(SNX_APPLE_FLAGS)
    list(JOIN SNX_APPLE_FLAGS " " snx_openssl_apple_flags)
    string(APPEND snx_openssl_cc " ${snx_openssl_apple_flags}")
endif()
if(ANDROID)
    list(APPEND snx_openssl_args -D__ANDROID_API__=${ANDROID_PLATFORM_LEVEL})
else()
    list(APPEND snx_openssl_args "CC=${snx_openssl_cc}" AR=${CMAKE_AR} RANLIB=${CMAKE_RANLIB})
endif()
if(CMAKE_RC_COMPILER)
    list(APPEND snx_openssl_args RC=${CMAKE_RC_COMPILER})
endif()
# iOS marks makecontext unavailable, which OpenSSL's async jobs are made of: nothing here starts one. Its target puts
# -mios-version-min=7.0.0 after CC's flags, and the arguments' flags come after the target's
if(IOS)
    list(APPEND snx_openssl_args no-async -miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()
if(NOT WIN32)
    # position-independent, as FFmpeg is: the libraries go into a shared library too (Android's runtime)
    list(APPEND snx_openssl_args -fPIC)
endif()

# The build's compiler launcher (ccache) wraps make's compiles only, as for FFmpeg. On Android the compiler is the
# <triple><API>-clang the target picks
set(snx_openssl_make_cc "")
list(JOIN CMAKE_C_COMPILER_LAUNCHER " " snx_openssl_launcher)
if(snx_openssl_launcher AND ANDROID)
    set(snx_openssl_make_cc "CC=${snx_openssl_launcher} ${snx_openssl_android_cc}")
elseif(snx_openssl_launcher)
    set(snx_openssl_make_cc "CC=${snx_openssl_launcher} ${snx_openssl_cc}")
endif()

# Built once for every tree that builds it the same way, when SNX_DEPS_CACHE names a folder (cmake/deps_cache.cmake).
# The install prefix stays out of the key: it is only OPENSSLDIR and MODULESDIR in the library, which a build without
# modules and without a configuration file at startup never reads
snx_deps_cached(openssl snx_openssl_cache "source: ${snx_openssl_sha256}" "configure: ${snx_openssl_args}"
                "android: ${CMAKE_ANDROID_NDK} ${snx_openssl_android_cc}" "perl: ${PERL_VERSION_STRING}")
if(snx_openssl_cache_FOUND)
    set(snx_openssl_prefix "${snx_openssl_cache_DIR}")
else()
    # OpenSSL's make does not see the build's -j
    cmake_host_system_information(RESULT snx_openssl_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    if("$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" MATCHES "^[1-9][0-9]*$")
        set(snx_openssl_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    include(ExternalProject)
    ExternalProject_Add(openssl_build
        SOURCE_DIR "${snx_openssl_src}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/openssl-build"
        INSTALL_DIR "${snx_openssl_prefix}"
        CONFIGURE_COMMAND ${snx_openssl_env} "${PERL_EXECUTABLE}" "${snx_openssl_src}/Configure"
                          --prefix=${snx_openssl_prefix} ${snx_openssl_args}
        # install_dev, the headers and libraries without the programs and manuals, is part of the build step: the
        # libraries below are its byproducts, which Ninja checks again as soon as the step ends (cmake/ffmpeg.cmake)
        BUILD_COMMAND ${snx_openssl_env} "${SNX_OPENSSL_MAKE}" -j${snx_openssl_jobs} build_libs ${snx_openssl_make_cc}
        COMMAND ${snx_openssl_env} "${SNX_OPENSSL_MAKE}" install_dev
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${snx_openssl_prefix}/lib/libcrypto.a" "${snx_openssl_prefix}/lib/libssl.a"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        LOG_OUTPUT_ON_FAILURE ON)
    snx_deps_publish(openssl_build openssl "${snx_openssl_prefix}" "${snx_openssl_cache_DIR}")
endif()

# The include folder is filled by the install step, but has to exist for the imported targets at configure time
file(MAKE_DIRECTORY "${snx_openssl_prefix}/include")
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
# SYSTEM OFF, as for FFmpeg: an -I that holds the system's OpenSSL headers (/usr/local/include on the BSDs) would win
# over -isystem ones
set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION "${snx_openssl_prefix}/lib/libcrypto.a"
                                                 INTERFACE_INCLUDE_DIRECTORIES "${snx_openssl_prefix}/include"
                                                 SYSTEM OFF)
if(TARGET openssl_build)
    add_dependencies(OpenSSL::Crypto openssl_build)
endif()
if(WIN32)
    # the random seed (BCryptGenRandom), the Windows certificate store, and sockets for BIO
    target_link_libraries(OpenSSL::Crypto INTERFACE bcrypt crypt32 ws2_32 advapi32 user32)
else()
    target_link_libraries(OpenSSL::Crypto INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
endif()
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES IMPORTED_LOCATION "${snx_openssl_prefix}/lib/libssl.a" SYSTEM OFF)
target_link_libraries(OpenSSL::SSL INTERFACE OpenSSL::Crypto)
if(TARGET openssl_build)
    add_dependencies(OpenSSL::SSL openssl_build)
endif()
