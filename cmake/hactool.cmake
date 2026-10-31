# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# hactool (ISC, src/nx/runtime/driver/hactool) as the library scarletnx --extract calls in its own process: SciresM's hactool
# at 1d64a83 with ScarletNX's changes (decrypted containers, the build's OpenSSL for its AES, SHA and RSA, the storages
# ported from LibHac for sparse and compressed sections and bucket trees of any size, NCAs read in place inside an XCI
# or NSP). The mbedtls 2.x copy hactool bundles, its lz4.c (LZ4 1.8) and its main.c are left out: nso.c and storage.c
# include its lz4.h, found first beside them, which declares LZ4_decompress_safe, the only LZ4 function hactool calls,
# as every LZ4 since 1.7 does, and snx_lz4 defines it
set(hactool_src "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/hactool")
set(hactool_sources save sha aes extkeys rsa npdm bktr kip packages pki pfs0 hfs0 nca0_romfs romfs utils nax0 nso nca
                    xci filepath ConvertUTF storage snx_hac)
list(TRANSFORM hactool_sources PREPEND "${hactool_src}/")
list(TRANSFORM hactool_sources APPEND .c)
add_library(hactool_lib STATIC ${hactool_sources})
# its feature macros stay inside: the driver sees snx_hac.h alone, and the tests that build hactool's structures ask for
# them (hactool_internals). No include folder: hactool's files include one another from beside them, and the rest of
# the tree as paths from src/ ("nx/runtime/driver/hactool/nca.h")
set(hactool_definitions _BSD_SOURCE _POSIX_SOURCE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE __USE_MINGW_ANSI_STDIO=1
                        _FILE_OFFSET_BITS=64)
target_link_libraries(hactool_lib PUBLIC OpenSSL::Crypto snx_lz4 snx_cjson)
target_compile_definitions(hactool_lib PRIVATE ${hactool_definitions})
add_library(hactool_internals INTERFACE)
target_compile_definitions(hactool_internals INTERFACE ${hactool_definitions})
target_link_libraries(hactool_internals INTERFACE hactool_lib)
target_compile_options(hactool_lib PRIVATE -w)
# snx_io.h ahead of every file: what hactool prints, its exits and its writes go through snx_hac.c
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(hactool_lib PRIVATE "/FI${hactool_src}/snx_io.h")
else()
    target_compile_options(hactool_lib PRIVATE -include "${hactool_src}/snx_io.h")
endif()
set_target_properties(hactool_lib PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
