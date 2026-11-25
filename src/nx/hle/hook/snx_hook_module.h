/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A hook module in a library of its own (docs/platforms/nx/hooks.md, "Modules in libraries"): the one header a module
   built outside ScarletNX includes. It names no type of the runtime. The runtime gives the module a table of functions
   (SnxHookHost), and the module gives back what it is (SnxHookModule) from its one export, snx_hook_module.

   The ABI has a version of its own. A new major changes what one side reads of the other, and the runtime refuses a
   module of another major. A new minor only adds: functions at the end of SnxHookHost, fields at the end of
   SnxHookModule and of SnxHookRecordedUpscale (which carries its own size), new flags. The runtime takes a module of
   its own minor or of an older one, and reads only the fields the module's minor has. In every major the first fields
   of SnxHookHost and SnxHookModule and the signature of snx_hook_module stay as they are. Within a major the structs
   that make arrays (SnxHookCode, SnxHookCpuHook) never grow.

   A module's work is its CPU hooks: functions of the game's code, by their offsets in one build of one of the game's
   modules, whose calls run the module's handlers with the guest CPU, as an address hook of the runtime's own does
   (docs/platforms/nx/hooks.md). A handler can record an upscale into the game's command buffer, which the runtime's
   upscalers run in order with the game's other commands. Every string and array the module gives stays where it is
   for the whole run: the runtime never unloads a module it took */
#ifndef SNX_HOOK_MODULE_H
#define SNX_HOOK_MODULE_H
#include <stddef.h> /* NULL, which a module gives for the parts it leaves out */
#include <stdint.h>

#define SNX_HOOK_ABI_MAJOR 1
#define SNX_HOOK_ABI_MINOR 2

#if defined(_WIN32)
#define SNX_HOOK_EXPORT __declspec(dllexport)
#else
#define SNX_HOOK_EXPORT [[gnu::visibility("default")]]
#endif

enum { SNX_HOOK_CODES = 64, SNX_HOOK_CPU_HOOKS = 32 };

typedef struct SnxHookHost SnxHookHost;
/* the guest CPU at a hooked function's entry, the runtime's, read and written through the host's functions */
typedef struct SnxHookCpu SnxHookCpu;

/* Code the module checks before it applies: bytes at an offset from the base of one of the game's modules, a byte as
   two hex digits or ?? for any, spaces between them free ("fd 7b bf a9 ?? ?? 00 94"). module: the game's module by its
   name, NULL for the module SnxHookModule.module names */
typedef struct {
    const char* module;
    uint64_t offset;
    const char* bytes;
} SnxHookCode;

/* A CPU hook: the function at offset from the base of the game's module SnxHookModule.module names (main for NULL), in
   the build its build_id names. offset is the function's entry, 4-aligned. Its handler runs on the game's thread that
   called the function, at the entry, with the registers as the call left them. When it returns, the function returns
   to its caller with the registers as the handler left them, and its own code runs only if the handler called the
   host's run_original */
typedef struct {
    uint64_t offset;
    const char* name; /* for the log */
    void (*handler)(const SnxHookHost* host, SnxHookCpu* cpu);
} SnxHookCpuHook;

/* a texture of the game's, by the guest address of its NVNtexture object, and of an NVNtextureView for it (0 for the
   whole texture) */
typedef struct {
    uint64_t texture, view;
} SnxHookTexture;

/* An upscale a CPU hook records into the game's command buffer (the host's record_upscale, docs/layers/gpu/
   upscalers.md). color and output are required, depth, motion, exposure and reactive optional (texture 0): a temporal
   upscaler takes the frame only with depth and motion. render: the color's size, from its corner. out: the output's
   size, from out_base. render_max: the largest render size a dynamic resolution may give, 0 for the color's size.
   jitter: the camera's offset in render pixels. motion_scale brings a motion vector to pixels of the motion vectors'
   size. reset: the history isn't this frame's (a cut, a new scene). size is sizeof (SnxHookRecordedUpscale) as the
   module was built. The runtime picks the upscaler from the settings, among those flags says the module takes: a
   temporal one always, a spatial one or a copy when flags say so.
   1.2, reprojection: for a game with depth and a camera but no motion vectors of its own, with
   SNX_HOOK_UPSCALE_REPROJECTED_MOTION and depth given and motion not, the runtime makes the motion vectors from the
   depth: reprojection (row-major) takes a render pixel of this frame, as (x, y, depth, 1) with x and y its place in the
   color's render rectangle as -1 to 1 (y down) and depth as the depth texture stores it, to where it was in the frame
   before, in the same terms (homogeneous, divided by w). It is the frame before's view-projection times the inverse of
   this one's, with the picture's coordinates and the depth's range folded in. The camera's motion alone: things that
   move by themselves get the camera's motion at their pixels. A 1.1 module's record (a smaller size) has none */
typedef struct {
    uint32_t size;
    uint32_t flags; /* SNX_HOOK_UPSCALE_ */
    SnxHookTexture color, output, depth, motion, exposure, reactive;
    uint32_t render[2], out[2], render_max[2];
    int32_t out_base[2];
    float jitter[2], motion_scale[2], pre_exposure;
    int32_t reset;
    float reprojection[16]; /* 1.2 */
} SnxHookRecordedUpscale;
enum {
    SNX_HOOK_UPSCALE_HDR = 1,             /* the color is HDR (with RANGE_GIVEN, else its format says) */
    SNX_HOOK_UPSCALE_RENDER_MOTION = 2,   /* motion vectors at the render size */
    SNX_HOOK_UPSCALE_JITTERED_MOTION = 4, /* motion vectors that include the jitter */
    SNX_HOOK_UPSCALE_INVERTED_DEPTH = 8,
    SNX_HOOK_UPSCALE_AUTO_EXPOSURE = 0x40,
    SNX_HOOK_UPSCALE_REPROJECTED_MOTION = 0x100, /* 1.2: motion vectors from depth and reprojection, above */
    /* 1.2: a game without a jitter of its own: the runtime moves the scene's draws (those that test depth into the
       depth texture given, or another of its size) by a sub-pixel offset (Halton 2, 3) each frame, and the upscaler
       gets it (jitter is then the runtime's, not the record's) */
    SNX_HOOK_UPSCALE_RUNTIME_JITTER = 0x200,
    SNX_HOOK_UPSCALE_RANGE_GIVEN = 0x10000, /* HDR says the color's range */
    SNX_HOOK_UPSCALE_SPATIAL_OK = 0x40000,  /* a spatial upscaler will do */
    SNX_HOOK_UPSCALE_COPY_OK = 0x80000      /* a plain copy will do */
};

/* The module. name is its word in the hooks_off setting and in the log, and its library's file name without the
   suffix (name.so, name.dll, name.dylib): letters a to z, digits and _, at most 63. title: the title ID it applies to,
   0 any. module: the game's module its build ID, checks and CPU hooks name, NULL for main. build_id: the hex of that
   module's build ID, its whole or its first bytes. All of them must hold, and every code check, or the module leaves
   the game as it is, with the reason in the log */
typedef struct {
    uint32_t abi_major, abi_minor; /* SNX_HOOK_ABI_MAJOR and SNX_HOOK_ABI_MINOR as the module was built */
    const char* name;
    const char* what; /* what it does, for the log */
    uint64_t title;
    const char* module;
    const char* build_id;
    const SnxHookCode* code;
    uint32_t ncode; /* at most SNX_HOOK_CODES */
    const SnxHookCpuHook* cpu_hooks;
    uint32_t ncpu_hooks; /* 1 to SNX_HOOK_CPU_HOOKS */
} SnxHookModule;

enum { SNX_HOOK_LOG_INFO, SNX_HOOK_LOG_WARN };

/* The runtime's functions. Those of a cpu are for a CPU hook's handler, its cpu valid during that call alone, and give
   0 (or change nothing) outside it */
struct SnxHookHost {
    uint32_t abi_major, abi_minor; /* the runtime's */
    uint32_t size;                 /* sizeof (SnxHookHost) as the runtime was built */
    /* a line in the runtime's log, printf's format, no newline needed */
    void (*log)(int level, const char* format, ...);
    /* a setting's value as text, as the game's .ini would write it: 1, or 0 when no setting has that key or out is too
       small (size bytes, with its NUL) */
    int (*setting)(const char* key, char* out, uint32_t size);
    /* x0 to x30, and sp for n 31. set_reg writes one of them */
    uint64_t (*reg)(const SnxHookCpu* cpu, uint32_t n);
    void (*set_reg)(SnxHookCpu* cpu, uint32_t n, uint64_t value);
    /* v0 to v31, the guest's 16 bytes */
    void (*vreg)(const SnxHookCpu* cpu, uint32_t n, uint8_t out[16]);
    void (*set_vreg)(SnxHookCpu* cpu, uint32_t n, const uint8_t in[16]);
    /* the hooked function's guest address */
    uint64_t (*entry)(const SnxHookCpu* cpu);
    /* size bytes of guest memory at addr: 1, or 0 when some of it isn't mapped (out zeroed, nothing written) */
    int (*read)(const SnxHookCpu* cpu, uint64_t addr, void* out, uint64_t size);
    int (*write)(SnxHookCpu* cpu, uint64_t addr, const void* in, uint64_t size);
    /* The function's own code, now, with the registers as they are (the handler may change the arguments first, never
       x30 or sp), until it returns to the caller's return address: then the registers hold what it left, and the
       handler goes on. At most once in a call */
    void (*run_original)(SnxHookCpu* cpu);
    /* the guest address of the base of the game's module of that name (main for NULL), 0 when it has none */
    uint64_t (*module_base)(const char* module);
    /* One upscale appended to the game's command buffer at command_buffer (the guest address of its NVNcommandBuffer,
       which must be recording), replayed in order with its other commands through the runtime's upscaler. Where no
       upscaler the flags allow can run then, a filtered copy of the color onto the output stands in, so the output is
       always written. 1 when recorded, 0 when not (no recording command buffer there, a texture that isn't live, a
       size of 0, or a call outside a CPU hook's handler): the handler then leaves the frame to run_original */
    int (*record_upscale)(SnxHookCpu* cpu, uint64_t command_buffer, const SnxHookRecordedUpscale* upscale);
    /* 1.1: the display's largest picture as the game is told it, the output a game sizes its frames for: 1 with its
       width and height in out */
    int (*display_size)(uint32_t out[2]);
    /* 1.1: a resolution multiplier, which a module asks for from the internal resolution target the user picked: in a
       CPU hook's handler (its cpu), or at its load from snx_hook_module (cpu NULL), where the settings and display_size
       already answer. Every render target the renderer makes from then on is the game's size times scale, so a game
       rendering at render reaches target (both for the log). It is taken while the user's resolution setting is at its
       default (1), until the renderer makes its first render target: a module asks before the game draws. The
       multiplier in effect after the call, scale when taken, else the user's or the one the renderer keeps (said in
       the log) */
    float (*resolution_scale)(SnxHookCpu* cpu, float scale, const uint32_t target[2], const uint32_t render[2]);
};

/* The module's one export. The runtime calls it once, at start, before the game's threads, with a host that stays for
   the run (the recompile calls it too, with a host that has the log alone, to read the CPU hooks). NULL: the module
   refuses this runtime (a host of an older minor than it needs), after it logs why */
#define SNX_HOOK_ENTRY "snx_hook_module"
SNX_HOOK_EXPORT const SnxHookModule* snx_hook_module(const SnxHookHost* host);
#endif
