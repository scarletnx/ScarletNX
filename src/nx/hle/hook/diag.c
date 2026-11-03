/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::diag's abort (hooks.hook's diag family). Each of the SDK's NN_ABORT overloads ends in VAbortImpl with the
   condition that failed, the SDK function and source position that checked it, the Result and the message's format:
   the handler logs them as the SDK passed them, then runs the function's own code, which stops the game as it would.
   It changes nothing the game can see */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/platform/log.h"

#define DIAG_VABORT "_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list"

/* the guest C string at a, cut to n - 1 bytes and its bytes outside printable ASCII as '?', or "none" for a null
   pointer. The SDK passes its own string literals; a pointer outside guest memory faults here as the function's own
   code would on it (crash.c reports it) */
static const char* text(const CPU* c, u64 a, char* out, size_t n) {
    size_t i;
    if (!a) return "none";
    for (i = 0; i + 1 < n && c->mem[a + i]; i++) {
        u8 b = c->mem[a + i];
        out[i] = b >= 32 && b < 127 ? (char)b : '?';
    }
    out[i] = 0;
    return out;
}

void diag_abort(CPU* c, u64 condition, u64 function, u64 file, i32 line, u64 result, u64 exception, u64 format,
                u64 args) {
    char cond[160], func[160], path[200], fmt[200], res[64] = "no result";
    u64 own_at = kernel_find_symbol_exact(DIAG_VABORT);
    GuestFn own = hook_original(own_at);
    (void)exception, (void)args;
    if (result) {
        u32 r = snx_ld_le32(c->mem + result);
        snprintf(res, sizeof res, "result 0x%x (error %04u-%04u)", r, 2000 + (r & 0x1ff), (r >> 9) & 0x1fff);
    }
    WARN("nn::diag abort: \"%s\" failed in %s (%s, line %d) with %s, message format \"%s\"\n",
         text(c, condition, cond, sizeof cond), text(c, function, func, sizeof func), text(c, file, path, sizeof path),
         line, res, text(c, format, fmt, sizeof fmt));
    if (!own)
        fatal("nn::diag abort: %s at %llx has no code of its own to run", DIAG_VABORT, (unsigned long long)own_at);
    own(c, own_at); /* the registers as the call left them: the function stops the game as it does unhooked */
}
