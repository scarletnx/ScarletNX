/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_SHADER_AMPERE_H
#define SNX_SHADER_AMPERE_H
#include "nx/gpu/shader/shader.h"

/* Lower SM70+ 16-byte instructions into a Maxwell program, including its native-version header and scheduling words.
   Returns its byte count, or 0 with the unsupported instruction and offset in si. Input and output must not overlap.
   On success, consumed (if non-NULL) receives the native extent through the main EXIT and referenced leaf returns.
   Graphics callers replace the synthetic header with the first 80 bytes of the native 128-byte header.
   No native bytes are reinterpreted as Maxwell instructions. Unrepresentable forms reject the whole program. */
u32 shader_ampere_lower(const u8* code, u32 bytes, u8* out, u32 capacity, ShaderInfo* si, u32* consumed);
/* Check instruction-local forms in the main body without producing a program. Reports every rejected site.
   Returns the number of flow/link instructions not checked locally, or ~0u when the main extent cannot be
   established. Called leaf bodies are not scanned. This is not a substitute for shader_ampere_lower. */
u32 shader_ampere_diagnose(const u8* code, u32 bytes,
                           void (*report)(void* ctx, u32 offset, const char* reason), void* ctx);
/* Discover a bounded input extent, or the minimum additional bytes needed to reach a referenced branch or return.
   The caller reads more and retries, with SHADER_AMPERE_NATIVE_MAX as its hard limit. Malformed flow stays for
   shader_ampere_lower to reject. No input beyond bytes is accessed. */
u32 shader_ampere_read_extent(const u8* code, u32 bytes);
#define SHADER_AMPERE_NATIVE_MAX 0x80000u
/* The largest lowered program, its header included: 65,536 Maxwell words, as many as shader_resolve_crs walks. A
   native program grows several times, and its matrix and shuffle forms more */
#define SHADER_AMPERE_LOWERED_MAX 0x80000u
#endif
