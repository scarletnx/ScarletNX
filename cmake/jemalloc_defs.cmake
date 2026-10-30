# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# Run on the header jemalloc's configure writes (cmake/jemalloc.cmake), before its make: takes background threads out,
# which configure turns on wherever there are pthreads and has no switch for. Their boot initialises condition
# variables inside malloc's first call, and a preloaded library that wraps pthread_cond_init and allocates there (the
# AppImage's cross-libc-dlopen.so) enters malloc again and waits forever on the lock that first call holds. Nothing
# here turns background threads on, and they are off unless asked for.
# DEFS: the build folder's include/jemalloc/internal/jemalloc_internal_defs.h.
if(NOT DEFS OR NOT EXISTS "${DEFS}")
    message(FATAL_ERROR "jemalloc_defs.cmake: DEFS=${DEFS} names no file")
endif()
file(READ "${DEFS}" defs)
string(REGEX REPLACE "#define JEMALLOC_BACKGROUND_THREAD[ ]*\n" "/* #undef JEMALLOC_BACKGROUND_THREAD */\n" changed
                     "${defs}")
if(changed STREQUAL defs AND NOT defs MATCHES "/\\* #undef JEMALLOC_BACKGROUND_THREAD \\*/")
    message(FATAL_ERROR "jemalloc_defs.cmake: ${DEFS} neither defines nor leaves out JEMALLOC_BACKGROUND_THREAD: "
                        "jemalloc's configure changed, so background threads may be in the library")
endif()
if(NOT changed STREQUAL defs)
    file(WRITE "${DEFS}" "${changed}")
endif()
