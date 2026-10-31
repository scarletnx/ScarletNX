# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# libcurl as a static library (target CURL::libcurl), built from the release source in cmake/dependencies.cmake with
# its own CMake, after OpenSSL (cmake/openssl.cmake), which it takes for TLS: the launcher's downloads
# (src/common/network/https.c). HTTP and HTTPS only, no other protocol, no compression, IDN, PSL, SSH or HTTP/2
# library, and no CA path of the machine that builds it: the roots are Mozilla's, which the build carries
# (cmake/cacert.cmake) and the client hands to each transfer, as Eden does with its bundled OpenSSL. curl's license is
# MIT-like. OpenSSL's libraries only exist once its build step ran, so libcurl builds as a project of its own after it.
# This file is included into the top-level scope: its variables carry the snx_curl_ prefix.
set(snx_curl_src "${curl_SOURCE_DIR}")
set(snx_curl_prefix "${PROJECT_BINARY_DIR}/curl")

# the build's own toolchain, carried into curl's configure: the compiler and its launcher, a toolchain file (the NDK's,
# a cross one), the platform a cross build targets, and macOS and iOS' architecture, SDK and oldest version. A list's
# items are joined by |, which ExternalProject turns back into ;
set(snx_curl_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${snx_curl_prefix}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_AR=${CMAKE_AR}
    -DCMAKE_RANLIB=${CMAKE_RANLIB})
if(CMAKE_C_COMPILER_LAUNCHER)
    list(JOIN CMAKE_C_COMPILER_LAUNCHER "|" snx_curl_launcher)
    list(APPEND snx_curl_args "-DCMAKE_C_COMPILER_LAUNCHER=${snx_curl_launcher}")
endif()
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND snx_curl_args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()
if(CMAKE_CROSSCOMPILING)
    list(APPEND snx_curl_args -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
         -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR})
endif()
if(ANDROID)
    list(APPEND snx_curl_args -DANDROID_ABI=${ANDROID_ABI} -DANDROID_PLATFORM=${ANDROID_PLATFORM})
endif()
if(APPLE)
    list(JOIN CMAKE_OSX_ARCHITECTURES "|" snx_curl_archs)
    list(APPEND snx_curl_args "-DCMAKE_OSX_ARCHITECTURES=${snx_curl_archs}"
         -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
endif()
set(snx_curl_c_flags "${CMAKE_C_FLAGS}")
if(SNX_TARGET_FLAGS)
    list(JOIN SNX_TARGET_FLAGS " " snx_curl_target_flags)
    string(APPEND snx_curl_c_flags " ${snx_curl_target_flags}")
endif()
list(APPEND snx_curl_args "-DCMAKE_C_FLAGS=${snx_curl_c_flags}")
# OpenSSL by its files, not a search: the NDK's toolchain looks for libraries inside its sysroot only. FindOpenSSL would
# also ask pkg-config, which knows the system's OpenSSL and its dependencies (zlib on some distributions), not the
# build's
list(APPEND snx_curl_args
     -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
     -DCURL_USE_OPENSSL=ON
     -DOPENSSL_USE_STATIC_LIBS=ON
     -DOPENSSL_INCLUDE_DIR=${snx_openssl_prefix}/include
     -DOPENSSL_CRYPTO_LIBRARY=${snx_openssl_prefix}/lib/libcrypto.a
     -DOPENSSL_SSL_LIBRARY=${snx_openssl_prefix}/lib/libssl.a)
list(APPEND snx_curl_args
     -DBUILD_SHARED_LIBS=OFF
     -DBUILD_STATIC_LIBS=ON
     -DBUILD_CURL_EXE=OFF
     -DBUILD_TESTING=OFF
     -DBUILD_EXAMPLES=OFF
     -DBUILD_LIBCURL_DOCS=OFF
     -DBUILD_MISC_DOCS=OFF
     -DENABLE_CURL_MANUAL=OFF
     -DCURL_USE_PKGCONFIG=OFF
     -DCURL_CA_BUNDLE=none
     -DCURL_CA_PATH=none
     -DCURL_ZLIB=OFF
     -DCURL_BROTLI=OFF
     -DCURL_ZSTD=OFF
     -DUSE_NGHTTP2=OFF
     -DUSE_LIBIDN2=OFF
     -DUSE_WIN32_IDN=OFF
     -DUSE_APPLE_IDN=OFF
     -DCURL_USE_LIBPSL=OFF
     -DCURL_USE_LIBSSH2=OFF
     -DCURL_USE_LIBSSH=OFF
     -DCURL_USE_GSSAPI=OFF
     -DENABLE_UNIX_SOCKETS=OFF
     -DCURL_DISABLE_DICT=ON
     -DCURL_DISABLE_FILE=ON
     -DCURL_DISABLE_FTP=ON
     -DCURL_DISABLE_GOPHER=ON
     -DCURL_DISABLE_IMAP=ON
     -DCURL_DISABLE_LDAP=ON
     -DCURL_DISABLE_LDAPS=ON
     -DCURL_DISABLE_MQTT=ON
     -DCURL_DISABLE_POP3=ON
     -DCURL_DISABLE_IPFS=ON
     -DCURL_DISABLE_RTSP=ON
     -DCURL_DISABLE_SMB=ON
     -DCURL_DISABLE_SMTP=ON
     -DCURL_DISABLE_TELNET=ON
     -DCURL_DISABLE_TFTP=ON
     -DCURL_DISABLE_WEBSOCKETS=ON)

snx_deps_cached(curl snx_curl_cache "source: ${snx_curl_sha256}" "configure: ${snx_curl_args}"
                "openssl: ${snx_openssl_sha256}")
if(snx_curl_cache_FOUND)
    set(snx_curl_prefix "${snx_curl_cache_DIR}")
else()
    include(ExternalProject)
    set(snx_curl_depends "")
    if(TARGET openssl_build)
        set(snx_curl_depends openssl_build)
    endif()
    ExternalProject_Add(curl_build
        SOURCE_DIR "${snx_curl_src}"
        BINARY_DIR "${PROJECT_BINARY_DIR}/curl-build"
        INSTALL_DIR "${snx_curl_prefix}"
        DEPENDS ${snx_curl_depends}
        LIST_SEPARATOR |
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_ARGS ${snx_curl_args}
        BUILD_BYPRODUCTS "${snx_curl_prefix}/lib/libcurl.a"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        LOG_OUTPUT_ON_FAILURE ON)
    snx_deps_publish(curl_build curl "${snx_curl_prefix}" "${snx_curl_cache_DIR}")
endif()

# The include folder is filled by the install step, but has to exist for the imported target at configure time
file(MAKE_DIRECTORY "${snx_curl_prefix}/include")
add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION "${snx_curl_prefix}/lib/libcurl.a"
                                               INTERFACE_INCLUDE_DIRECTORIES "${snx_curl_prefix}/include"
                                               INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB)
target_link_libraries(CURL::libcurl INTERFACE OpenSSL::SSL Threads::Threads)
if(WIN32)
    target_link_libraries(CURL::libcurl INTERFACE ws2_32 iphlpapi bcrypt crypt32)
elseif(APPLE AND NOT IOS)
    target_link_libraries(CURL::libcurl INTERFACE "-framework SystemConfiguration" "-framework CoreFoundation"
                                                  "-framework CoreServices")
endif()
if(TARGET curl_build)
    add_dependencies(CURL::libcurl curl_build)
endif()
