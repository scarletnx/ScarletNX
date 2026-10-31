/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* src/nx/gpu/nvn/nvn.h compiles as strict ISO C99, and its objects hold the fields docs/NVN_HLE.md records: the
   command buffer's, in both SDK layouts read there, end before its reserved size. The NVN functions' types, which
   hookgen makes of gpu/nvn/nvn.hook (the generated gpu/nvn/nvn.hook.h), compile with it and are the C prototypes
   they stand for: each integer at its guest size, a pointer as the type it points to */
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/nvn/nvn.h"
#include <stdio.h>

/* the end of the last command buffer field docs/NVN_HLE.md records in each layout: the memory callback's userdata at
   +0x70 (8 bytes) in the newer, the reserve margin at +0x70 (a u32) in the older */
#define CMDBUF_FIELDS_END_NEW 0x78
#define CMDBUF_FIELDS_END_OLD 0x74

typedef char check_cmdbuf_new[sizeof(NVNcommandBuffer) >= CMDBUF_FIELDS_END_NEW ? 1 : -1];
typedef char check_cmdbuf_old[sizeof(NVNcommandBuffer) >= CMDBUF_FIELDS_END_OLD ? 1 : -1];

/* prototypes in NVN's own types, which the generated function types must be compatible with (C99 6.7.5.3): pointers
   to const and to pointers, a ptrdiff_t as int64_t, an enum and an NVNboolean, a float, a callback, and the results */
typedef void WriteTexelsStrided(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const void*, int64_t,
                                int64_t);
typedef void SetRenderTargets(NVNcommandBuffer*, int32_t, const NVNtexture* const*, const NVNtextureView* const*,
                              const NVNtexture*, const NVNtextureView*);
typedef void BindImagesDeferred(NVNcommandBuffer*, NVNshaderStage, int32_t, int32_t, const NVNimageHandle*);
typedef void InstallDebugCallback(NVNdevice*, PFNNVNDEBUGCALLBACKPROC, void*, NVNboolean);
typedef NVNboolean RegisterFastClearDepth(NVNdevice*, float);
typedef NVNmemoryPool* BufferGetMemoryPool(const NVNbuffer*);
typedef uint64_t BufferGetSize(const NVNbuffer*);
typedef NVNboolean CommandBufferInitialize(NVNcommandBuffer*, NVNdevice*);
typedef void CommandBufferFinalize(NVNcommandBuffer*);

int main(void) {
    /* an initializer converts as an assignment does: a pointer to an incompatible function type is a constraint
       violation, which -pedantic-errors refuses */
    WriteTexelsStrided* write = (PFNNVNTEXTUREWRITETEXELSSTRIDEDPROC)0;
    SetRenderTargets* targets = (PFNNVNCOMMANDBUFFERSETRENDERTARGETSPROC)0;
    BindImagesDeferred* images = (PFNNVNCOMMANDBUFFERBINDIMAGESDEFERREDPROC)0;
    InstallDebugCallback* callback = (PFNNVNDEVICEINSTALLDEBUGCALLBACKPROC)0;
    RegisterFastClearDepth* depth = (PFNNVNDEVICEREGISTERFASTCLEARDEPTHPROC)0;
    BufferGetMemoryPool* pool = (PFNNVNBUFFERGETMEMORYPOOLPROC)0;
    BufferGetSize* size = (PFNNVNBUFFERGETSIZEPROC)0;
    CommandBufferInitialize* init = (PFNNVNCOMMANDBUFFERINITIALIZEPROC)0;
    CommandBufferFinalize* fini = (PFNNVNCOMMANDBUFFERFINALIZEPROC)0;
    (void)write, (void)targets, (void)images, (void)callback, (void)depth, (void)pool, (void)size, (void)init;
    (void)fini;
    printf("nvn.h: NVNcommandBuffer is %u bytes, NVNtexture %u\n", (unsigned)sizeof(NVNcommandBuffer),
           (unsigned)sizeof(NVNtexture));
    return 0;
}
