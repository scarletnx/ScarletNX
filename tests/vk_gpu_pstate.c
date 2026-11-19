/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_pstate (common/gpu/vk/gpu_pstate.c) with no GPU: its values, and the hold against a stand-in kernel (ioctl
   wrapped at link time, -Wl,--wrap=ioctl) on a stand-in /sys and /dev, each case in a process of its own, as the hold
   lasts the process:
   - off, a GPU of another vendor, and auto on an integrated GPU hold nothing and open nothing; on holds an
     integrated one;
   - a discrete AMD GPU at the level auto is held: the node DEVNAME names opened close-on-exec and left open (the
     kernel puts the level back when the process ends), its driver's name asked, a context made, the peak stable
     pstate set on it, the level read back; a second hold opens nothing;
   - a level set on the PC ("high", "manual") is left as it is; one that does not read holds with on, not with auto;
   - each refusal (EBUSY, EINVAL, ENOTTY, a radeon node, a node that does not open, no render node) said as a warning
     with the error's words and the node, the node closed again;
   - an NVIDIA GPU is held through VK_NV_low_latency2's boost where its device was made with it, said with why
     where it was not, a warning with on; the kernel is asked nothing for it;
   - an Adreno (kgsl), integrated, is held by auto: a context of our own with the PWR_MAX constraint, a CP_NOP written
     in a page of its own, then renewed by one command per keeper step while a frame was presented in the last 250 ms,
     none after; a refused renewal said once and the hold given up; a refused step of the hold said and closed;
   - every case says exactly one line, naming the value given.
   On FreeBSD, which has no sysfs, devname(3) is wrapped too and names the node; the level never reads, so auto holds
   nothing and on holds, and the cases of a level set on the PC or of sysfs's files are Linux's alone */
#include "common/gpu/vk/gpu_pstate.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
/* struct drm_version as drm.h declares it */
struct version {
    int major, minor, patchlevel;
    size_t name_len;
    char* name;
    size_t date_len;
    char* date;
    size_t desc_len;
    char* desc;
};
#if __has_include(<drm/amdgpu_drm.h>) /* the system's uapi headers, where it has them, check the numbers */
#include <drm/amdgpu_drm.h>
#include <drm/drm.h>
#define VERSION_REQUEST DRM_IOCTL_VERSION
#define CTX_REQUEST     DRM_IOCTL_AMDGPU_CTX
#else /* the kernel's numbers, as its uapi headers make them */
#define VERSION_REQUEST _IOWR('d', 0x00, struct version)
#define CTX_REQUEST     _IOWR('d', 0x42, char[16])
#endif

static struct {
    unsigned checks, failures;
    char root[256];
    /* the stand-in kernel: what it answers, and what it was asked */
    const char* driver; /* DRM_IOCTL_VERSION's name */
    int version_err, alloc_err, set_err;
    int versions, allocs, sets, others;
    unsigned set_flags, set_ctx;
    int fd; /* the fd the context was asked on */
    /* kgsl: what it answers and was asked */
    int kgsl_err_at, kgsl_err; /* the request (its number) that answers kgsl_err */
    int creates, properties, allocs_k, infos, commands;
    unsigned ctx_flags, constraint_type, constraint_ctx, constraint_level;
    size_t constraint_size, property_size;
    uint64_t command_flags, command_addr, command_size;
    unsigned command_ctx, command_n, command_objsize, command_ibflags;
    /* the lines said */
    int lines, warn;
    char line[2048];
} g;

#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g.checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g.failures++;                                                                                              \
            printf("FAIL line %d: %s\n  said: %s\n", __LINE__, what, g.line);                                          \
        }                                                                                                              \
    } while (0)

/* the stand-in kernel: DRM_IOCTL_VERSION and DRM_IOCTL_AMDGPU_CTX's ALLOC_CTX and SET_STABLE_PSTATE, as amdgpu answers
   them; a set that holds writes the level the kernel would show */
int __wrap_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (request == (unsigned long)VERSION_REQUEST) {
        g.versions++;
        g.fd = fd;
        if (g.version_err) return errno = g.version_err, -1;
        struct version* v = arg;
        snprintf(v->name, v->name_len + 1, "%s", g.driver);
        return 0;
    }
    if (request == (unsigned long)CTX_REQUEST) {
        uint32_t* in = arg; /* op, flags, ctx_id, priority */
        g.fd = fd;
        if (in[0] == 1) {
            g.allocs++;
            if (g.alloc_err) return errno = g.alloc_err, -1;
            in[0] = 7; /* out.alloc.ctx_id */
            return 0;
        }
        if (in[0] == 6) {
            g.sets++;
            g.set_flags = in[1], g.set_ctx = in[2];
            if (g.set_err) return errno = g.set_err, -1;
            char path[512];
            snprintf(path, sizeof path, "%s/sys/dev/char/226:128/device/power_dpm_force_performance_level", g.root);
            FILE* f = fopen(path, "w");
            if (f) fputs("profile_peak\n", f), fclose(f);
            return 0;
        }
    }
    if ((request >> 8 & 0xff) == 0x09) { /* kgsl's, the numbers msm_kgsl.h makes on a 64-bit system */
        if ((int)(request & 0xff) == g.kgsl_err_at && g.kgsl_err) return errno = g.kgsl_err, -1;
        g.fd = fd;
        switch (request) {
            case 0xc0080913ul: { /* IOCTL_KGSL_DRAWCTXT_CREATE */
                unsigned* c = arg;
                g.creates++, g.ctx_flags = c[0], c[1] = 3;
                return 0;
            }
            case 0x40180932ul: { /* IOCTL_KGSL_SETPROPERTY */
                struct {
                    unsigned type;
                    void* value;
                    size_t size;
                }* p = arg;
                struct {
                    unsigned type, context;
                    unsigned* data;
                    size_t size;
                }* k = p->value;
                g.properties++, g.property_size = p->size;
                if (p->type != 0x12) return errno = EINVAL, -1;
                g.constraint_type = k->type, g.constraint_ctx = k->context, g.constraint_size = k->size;
                g.constraint_level = k->data[0];
                return 0;
            }
            case 0xc0300945ul: { /* IOCTL_KGSL_GPUOBJ_ALLOC: size, flags, va_len, mmapsize, then the id */
                uint64_t* a = arg;
                g.allocs_k++;
                if (a[0] != 4096) return errno = EINVAL, -1;
                ((unsigned*)(a + 4))[0] = 2;
                return 0;
            }
            case 0xc0300947ul: { /* IOCTL_KGSL_GPUOBJ_INFO: gpuaddr first, the id last */
                uint64_t* i = arg;
                g.infos++;
                if (((unsigned*)(i + 5))[0] != 2) return errno = EINVAL, -1;
                i[0] = 0x500030000ull;
                return 0;
            }
            case 0xc040094aul: { /* IOCTL_KGSL_GPU_COMMAND */
                struct {
                    uint64_t flags, cmdlist;
                    unsigned cmdsize, numcmds;
                    uint64_t objlist;
                    unsigned objsize, numobjs;
                    uint64_t synclist;
                    unsigned syncsize, numsyncs, context, timestamp;
                }* c = arg;
                const uint64_t* ib = (const uint64_t*)(uintptr_t)c->cmdlist; /* offset, gpuaddr, size, then flags, id */
                g.commands++, g.command_flags = c->flags, g.command_ctx = c->context, g.command_n = c->numcmds;
                g.command_objsize = c->cmdsize, g.command_addr = ib[1], g.command_size = ib[2];
                g.command_ibflags = ((const unsigned*)(ib + 3))[0];
                c->timestamp = (unsigned)g.commands;
                return 0;
            }
        }
    }
    g.others++;
    return errno = ENOTTY, -1;
}

#if defined(__FreeBSD__)
/* the stand-in devfs: 226:128 is dri/renderD128, other numbers none */
char* __wrap_devname(dev_t dev, mode_t type);
char* __wrap_devname(dev_t dev, mode_t type) {
    static char name[32];
    snprintf(name, sizeof name, "%s",
             major(dev) == 226 && minor(dev) == 128 && type == S_IFCHR ? "dri/renderD128" : "#C:0x0");
    return name;
}
/* the mode that holds a GPU at a level that does not read, and how the line says that level */
#define HOLDS      SNX_GPU_PSTATE_ON
#define LEVEL_READ "of FreeBSD's amdgpu, which has no sysfs, does not read (Operation not supported)"
#else
#define HOLDS      SNX_GPU_PSTATE_AUTO
#define LEVEL_READ "does not read (No such file or directory)"
#endif

static void say(const char* line, int warn) {
    g.lines++;
    g.warn = warn;
    snprintf(g.line, sizeof g.line, "%s", line);
}

static int has(const char* text) { return strstr(g.line, text) != NULL; }

static void put(const char* rel, const char* text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g.root, rel);
    for (char* s = strchr(path + strlen(g.root) + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(path, 0755);
        *s = '/';
    }
    FILE* f = fopen(path, "w");
    if (f) fputs(text, f), fclose(f);
}

/* the stand-in /sys and /dev of a render node 226:128, dri/renderD128, at level */
static void machine(const char* level) {
    put("sys/dev/char/226:128/uevent", "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n");
    if (level) put("sys/dev/char/226:128/device/power_dpm_force_performance_level", level);
    put("dev/dri/renderD128", "");
    g.driver = "amdgpu";
}

#if defined(__linux__)
/* the stand-in kgsl device: a file of 16 KiB, which mmap maps the hold's page of (object 2, at 8 KiB) from */
static void kgsl_machine(void) {
    char path[512];
    put("dev/kgsl-3d0", "");
    snprintf(path, sizeof path, "%s/dev/kgsl-3d0", g.root);
    if (truncate(path, 16384)) printf("truncate %s: %s\n", path, strerror(errno));
}
#endif

static SnxGpuPstateDevice device(int type) {
    static char sys[300], dev[300];
    snprintf(sys, sizeof sys, "%s/sys", g.root);
    snprintf(dev, sizeof dev, "%s/dev", g.root);
    return (SnxGpuPstateDevice){"AMD Radeon RX 6700 XT", SNX_GPU_VENDOR_AMD, type, 1, 226, 128, sys, dev, NULL};
}

static int hold(int mode, SnxGpuPstateDevice d) {
    int before = g.lines;
    int held = snx_gpu_pstate_hold(mode, &d, say);
    CHECK(g.lines == before + 1, "one line a hold");
    return held;
}

/* the stand-in's files and folders, each removed by name, the deepest first; 0 or the first that did not go */
static const char* clean(void) {
    static const char* const made[] = {"sys/dev/char/226:128/device/power_dpm_force_performance_level",
                                       "sys/dev/char/226:128/device",
                                       "sys/dev/char/226:128/uevent",
                                       "sys/dev/char/226:128",
                                       "sys/dev/char",
                                       "sys/dev",
                                       "sys",
                                       "dev/dri/renderD128",
                                       "dev/kgsl-3d0",
                                       "dev/dri",
                                       "dev",
                                       ""};
    static char path[512];
    for (size_t i = 0; i < sizeof made / sizeof *made; i++) {
        snprintf(path, sizeof path, "%s/%s", g.root, made[i]);
        if (remove(path) && errno != ENOENT) return path;
    }
    return NULL;
}

static int is_open(int fd) { return fd > 0 && fcntl(fd, F_GETFD) != -1; }

enum { OTHER, INTEGRATED, DISCRETE };

static void case_values(void) {
    static const struct {
        const char* text;
        int mode;
    } cases[] = {{"auto", SNX_GPU_PSTATE_AUTO},
                 {"on", SNX_GPU_PSTATE_ON},
                 {"off", SNX_GPU_PSTATE_OFF},
                 {"Auto", -1},
                 {"OFF", -1},
                 {"1", -1},
                 {"0", -1},
                 {"", -1},
                 {"auto ", -1},
                 {"peak", -1}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        CHECK(snx_gpu_pstate_mode(cases[i].text) == cases[i].mode, cases[i].text);
}

static void case_off(void) {
    machine("auto\n");
    CHECK(!hold(SNX_GPU_PSTATE_OFF, device(DISCRETE)), "off holds nothing");
    CHECK(!g.versions && !g.allocs && !g.sets, "off asks the kernel nothing");
    CHECK(!g.warn && has("gpu_pstate=off: AMD Radeon RX 6700 XT's driver chooses its clocks"), "off's line");
}

static void case_auto_integrated(void) {
    machine("auto\n");
    CHECK(!hold(SNX_GPU_PSTATE_AUTO, device(INTEGRATED)), "auto holds no integrated GPU");
    CHECK(!g.versions && !g.allocs && !g.sets, "auto asks the kernel nothing for an integrated GPU");
    CHECK(!g.warn && has("gpu_pstate=auto: AMD Radeon RX 6700 XT, an integrated GPU: its driver chooses its clocks") &&
              has("gpu_pstate=on holds them") && has("shares its power with the CPU"),
          "auto's line on an integrated GPU");
    CHECK(!hold(SNX_GPU_PSTATE_AUTO, device(OTHER)) && !g.sets && has("a GPU of another kind"),
          "auto holds a GPU of another kind neither");
}

static void case_on_integrated(void) {
    machine("auto\n");
    CHECK(hold(SNX_GPU_PSTATE_ON, device(INTEGRATED)), "on holds an integrated GPU");
    CHECK(g.sets == 1 && g.set_flags == 4, "the peak stable pstate set");
    CHECK(!g.warn && has("gpu_pstate=on: AMD Radeon RX 6700 XT, an integrated GPU: its clocks held"), "on's line");
}

static void case_held(void) {
    machine("auto\n");
    CHECK(hold(HOLDS, device(DISCRETE)), "auto holds a discrete GPU at the level auto");
    CHECK(g.versions == 1 && g.allocs == 1 && g.sets == 1 && !g.others, "its name asked, a context made and set");
    CHECK(g.set_flags == 4 && g.set_ctx == 7, "the peak stable pstate (4) set on the context made (7)");
    CHECK(is_open(g.fd), "the node left open: the kernel puts the level back when the process ends");
    CHECK(is_open(g.fd) && (fcntl(g.fd, F_GETFD) & FD_CLOEXEC), "the node closed in a program the game starts");
    char node[400];
#if defined(__FreeBSD__)
    snprintf(node, sizeof node, "(%s/dev/dri/renderD128) until the game ends, from a level that does not read", g.root);
#else
    snprintf(node, sizeof node, "(%s/dev/dri/renderD128, the level auto, now \"profile_peak\")", g.root);
#endif
    CHECK(!g.warn && has(": AMD Radeon RX 6700 XT, a discrete GPU: its clocks held at amdgpu's peak stable pstate ") &&
              has(node) && has("a crash too"),
          "the held line: the node, the level before and after");
    CHECK(hold(HOLDS, device(DISCRETE)) && g.versions == 1 && g.sets == 1 && has("held already"),
          "a second hold opens nothing and says it is held already");
}

#if !defined(__FreeBSD__)
static void case_level_set(void) {
    static const char* const levels[] = {"high", "manual", "profile_standard", ""};
    for (int i = 0; i < 4; i++) {
        char text[64], quoted[80];
        snprintf(text, sizeof text, "%s\n", levels[i]);
        snprintf(quoted, sizeof quoted, "the level set on this PC, \"%s\" (", levels[i]);
        machine(text);
        for (int mode = SNX_GPU_PSTATE_AUTO; mode <= SNX_GPU_PSTATE_ON; mode++) {
            CHECK(!hold(mode, device(DISCRETE)), "a level set on the PC is left as it is, by on too");
            CHECK(!g.versions && !g.sets, "nothing asked of the kernel");
            CHECK(!g.warn && has(quoted) && has("power_dpm_force_performance_level") && has("leave auto behind"),
                  "the line names the level and where it is read");
        }
    }
}
#endif

static void case_level_unread(void) {
    machine(NULL);
    CHECK(!hold(SNX_GPU_PSTATE_AUTO, device(DISCRETE)), "auto holds nothing from a level that does not read");
    CHECK(!g.versions && !g.sets, "auto asks the kernel nothing then");
    CHECK(!g.warn && has(LEVEL_READ) && has("gpu_pstate=on holds them anyway"), "auto's line names the error and on");
    CHECK(hold(SNX_GPU_PSTATE_ON, device(DISCRETE)) && g.sets == 1, "on holds it anyway");
    CHECK(!g.warn && has("from a level that does not read"), "on's line");
}

/* a refusal: a warning with words and the node, nothing held, the node closed */
static void refused(int mode, const char* words, const char* what) {
    char node[400];
    snprintf(node, sizeof node, "%s/dev/dri/renderD128", g.root);
    CHECK(!hold(mode, device(DISCRETE)), what);
    CHECK(g.warn && has(words) && has(node) && has("its driver chooses the clocks"), what);
    CHECK(!is_open(g.fd), "the node closed again");
}

static void case_busy(void) {
    machine("auto\n");
    g.set_err = EBUSY;
    char words[160];
    snprintf(words, sizeof words, "%s: another process holds one on this GPU", strerror(EBUSY)); /* FreeBSD's differ */
    refused(HOLDS, words, "EBUSY");
    CHECK(has("the kernel refused the peak stable pstate (AMDGPU_CTX_OP_SET_STABLE_PSTATE)"), "what was refused");
}

static void case_old_kernel(void) {
    machine("auto\n");
    g.set_err = EINVAL;
    refused(SNX_GPU_PSTATE_ON, "Invalid argument: a kernel before Linux 5.19 has none", "EINVAL");
}

static void case_no_context(void) {
    machine("auto\n");
    g.alloc_err = ENOMEM;
    refused(HOLDS, "a context of the runtime's own (AMDGPU_CTX_OP_ALLOC_CTX)", "ALLOC_CTX refused");
    CHECK(has("Cannot allocate memory") && !g.sets, "its error, and no set after");
}

static void case_not_drm(void) {
    machine("auto\n");
    g.version_err = ENOTTY;
    refused(HOLDS, "its driver's name (DRM_IOCTL_VERSION)", "a node that is no DRM node");
    CHECK(has("Inappropriate ioctl for device") && !g.allocs, "its error, and no context after");
}

static void case_radeon(void) {
    machine("auto\n");
    g.driver = "radeon";
    refused(HOLDS, "kernel driver is radeon, not amdgpu", "a radeon node");
    CHECK(!g.allocs && !g.sets, "no amdgpu call on it");
}

static void case_no_node(void) {
    machine("auto\n");
    SnxGpuPstateDevice d = device(DISCRETE);
    d.has_render = 0;
    for (int mode = SNX_GPU_PSTATE_AUTO; mode <= SNX_GPU_PSTATE_ON; mode++) {
        CHECK(!snx_gpu_pstate_hold(mode, &d, say), "no render node, no hold");
        CHECK(g.warn == (mode == SNX_GPU_PSTATE_ON) &&
                  has("names no render node (VK_EXT_physical_device_drm, whose line says why)"),
              "said, a warning with on");
    }
    d = device(DISCRETE);
    d.render_minor = 129;
    char where[400];
#if defined(__FreeBSD__)
    snprintf(where, sizeof where, "no render node 226:129 in devname(3) (No such file");
    CHECK(!hold(HOLDS, d) && g.warn && has(where), "a device number devfs has no node for");
#else
    snprintf(where, sizeof where, "no render node 226:129 in %s/sys/dev/char/226:129/uevent (No such file", g.root);
    CHECK(!hold(HOLDS, d) && g.warn && has(where), "a device number sysfs has no node for");
    put("sys/dev/char/226:128/uevent", "MAJOR=226\nMINOR=128\n");
    CHECK(!hold(HOLDS, device(DISCRETE)) && g.warn && has("uevent (No such file"), "a uevent without DEVNAME");
#endif
    machine("auto\n");
    char path[400];
    snprintf(path, sizeof path, "%s/dev/dri/renderD128", g.root);
    unlink(path);
    snprintf(where, sizeof where, "%s does not open (No such file or directory)", path);
    CHECK(!hold(HOLDS, device(DISCRETE)) && g.warn && has(where) && !g.versions, "a node that does not open");
}

static void case_other_vendor(void) {
    machine("auto\n");
    SnxGpuPstateDevice d = device(DISCRETE);
    d.name = "Intel(R) Arc(tm) A770 Graphics (DG2)", d.vendor = 0x8086;
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say) && !g.versions, "no way for another vendor");
    CHECK(!g.warn && has("gpu_pstate=auto: Intel(R) Arc(tm) A770 Graphics (DG2), a discrete GPU: no way to hold this "
                         "GPU's clocks on "),
          "said");
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_ON, &d, say) && g.warn, "a warning with on");
}

static void case_nvidia(void) {
    SnxGpuPstateDevice d = {
        "NVIDIA GeForce RTX 2080", SNX_GPU_VENDOR_NVIDIA, DISCRETE, 0, 0, 0, "/nowhere", "/nowhere", NULL};
    CHECK(snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say), "a discrete NVIDIA GPU made with the boost is held");
    CHECK(!g.warn &&
              has("gpu_pstate=auto: NVIDIA GeForce RTX 2080, a discrete GPU: its clocks boosted while the game "
                  "runs, through VK_NV_low_latency2's lowLatencyBoost on each swapchain") &&
              has("ends with the swapchain, a crash too"),
          "the boost's line");
    CHECK(!g.versions && !g.allocs && !g.sets, "nothing asked of the kernel for it");
    d.boost_lacks = "its device has no VK_NV_low_latency2 (the extension's line says why)";
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say), "no boost without the extension");
    CHECK(!g.warn && has("a discrete GPU: its device has no VK_NV_low_latency2 (the extension's line says why): its "
                         "driver chooses its clocks"),
          "said why");
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_ON, &d, say) && g.warn, "a warning with on");
    d.type = INTEGRATED, d.boost_lacks = NULL;
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say) && has("an integrated GPU: its driver chooses"),
          "auto boosts no integrated NVIDIA GPU");
    CHECK(snx_gpu_pstate_hold(SNX_GPU_PSTATE_ON, &d, say) && has("its clocks boosted"), "on boosts it");
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_OFF, &d, say) && has("gpu_pstate=off: NVIDIA GeForce RTX 2080's driver"),
          "off boosts nothing");
}

/* which devices each value asks held, the choice an NVIDIA device's extensions are made by */
#if defined(__linux__)
static SnxGpuPstateDevice adreno(void) {
    static char dev[300];
    snprintf(dev, sizeof dev, "%s/dev", g.root);
    return (SnxGpuPstateDevice){"Adreno (TM) 610", SNX_GPU_VENDOR_QUALCOMM, INTEGRATED, 0, 0, 0, "/nowhere", dev, NULL};
}

static void case_kgsl(void) {
    kgsl_machine();
    SnxGpuPstateDevice d = adreno();
    CHECK(snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say), "auto holds an Adreno, integrated as it is");
    CHECK(g.creates == 1 && g.ctx_flags == 0x812, "a context with the constraint's, preamble and no-GMEM flags");
    CHECK(g.properties == 1 && g.property_size == 24 && g.constraint_type == 1 && g.constraint_ctx == 3 &&
              g.constraint_size == 4 && g.constraint_level == 1,
          "the top power level (PWR_MAX) as the constraint of the context made");
    CHECK(g.allocs_k == 1 && g.infos == 1 && !g.commands, "a page made, nothing submitted yet");
    char path[512];
    unsigned nop = 0;
    snprintf(path, sizeof path, "%s/dev/kgsl-3d0", g.root);
    FILE* f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 8192, SEEK_SET) || fread(&nop, 4, 1, f) != 1) nop = 0;
        fclose(f);
    }
    CHECK(nop == 0x70108000u, "a CP_NOP written at the page of object 2");
    CHECK(!g.warn &&
              has("gpu_pstate=auto: Adreno (TM) 610, an integrated GPU: its clocks held at kgsl's top power level") &&
              has("/dev/kgsl-3d0) while the game presents frames") && has("drops it 250 ms after the last frame") &&
              has("a crash too"),
          "the held line");
    int fd = g.fd;
    CHECK(is_open(fd) && (fcntl(fd, F_GETFD) & FD_CLOEXEC),
          "the device left open, closed in a program the game starts");
    uint64_t t = 1000; /* the keeper's own clock, which the thread's monotonic one never comes near */
    CHECK(snx_gpu_pstate_keep(t, 0) == 0 && !g.commands, "no frame yet: nothing renewed");
    CHECK(snx_gpu_pstate_keep(t + 40000000, t) == 1 && g.commands == 1, "a frame 40 ms before: renewed");
    CHECK(g.command_ctx == 3 && g.command_n == 1 && g.command_objsize == 32 && g.command_addr == 0x500030000ull &&
              g.command_size == 4 && g.command_ibflags == 1 && g.command_flags == 0x800,
          "one IB of the CP_NOP, of 4 bytes, on the context, flagged for its constraint");
    CHECK(snx_gpu_pstate_keep(t + 249000000, t) == 1 && g.commands == 2, "249 ms after the frame: renewed");
    CHECK(snx_gpu_pstate_keep(t + 250000000, t) == 0 && g.commands == 2, "250 ms after it: not renewed");
    CHECK(snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say) && g.creates == 1 && has("held already"),
          "a second hold makes nothing");
    int lines = g.lines;
    g.kgsl_err_at = 0x4a, g.kgsl_err = EDEADLK;
    CHECK(snx_gpu_pstate_keep(t + 80000000, t + 40000000) == -1, "a refused renewal gives the hold up");
    CHECK(g.lines == lines + 1 && g.warn && has("kgsl refused the command that renews its constraint") &&
              has("Resource deadlock avoided"),
          "said once, a warning, with the error");
    CHECK(!is_open(fd), "the device closed, its context with it");
    CHECK(snx_gpu_pstate_keep(t + 120000000, t + 80000000) == 0 && g.lines == lines + 1, "nothing after");
}

/* each step of the kgsl hold refused: said as a warning with the step and the error, the device closed */
static void case_kgsl_refused(void) {
    static const struct {
        int at, err;
        const char *words, *what;
    } steps[] = {
        {0x13, EINVAL, "a context of the runtime's own (IOCTL_KGSL_DRAWCTXT_CREATE)", "Invalid argument"},
        {0x32, EPERM, "the top power level as its constraint (KGSL_PROP_PWR_CONSTRAINT)", "Operation not permitted"},
        {0x45, ENOMEM, "a page for its commands (IOCTL_KGSL_GPUOBJ_ALLOC)", "Cannot allocate memory"},
        {0x47, EINVAL, "the page's GPU address (IOCTL_KGSL_GPUOBJ_INFO)", "Invalid argument"}};
    kgsl_machine();
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) {
        g.kgsl_err_at = steps[i].at, g.kgsl_err = steps[i].err, g.fd = -1;
        SnxGpuPstateDevice d = adreno();
        CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_ON, &d, say), steps[i].words);
        CHECK(g.warn && has("kgsl refused") && has(steps[i].words) && has(steps[i].what), steps[i].words);
        CHECK(g.fd < 0 || !is_open(g.fd), "the device closed again");
    }
    char path[512];
    snprintf(path, sizeof path, "%s/dev/kgsl-3d0", g.root);
    unlink(path);
    g.kgsl_err = 0;
    SnxGpuPstateDevice d = adreno();
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_AUTO, &d, say) && g.warn && has("kgsl-3d0 does not open (No such file"),
          "no kgsl device: said");
    CHECK(!snx_gpu_pstate_hold(SNX_GPU_PSTATE_OFF, &d, say) && !g.warn, "off asks nothing");
}

#endif

static void case_wanted(void) {
    static const struct {
        unsigned vendor;
        int mode, type, wanted;
    } cases[] = {{SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_AUTO, DISCRETE, 1},
                 {SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_AUTO, INTEGRATED, 0},
                 {SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_AUTO, OTHER, 0},
                 {SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_ON, DISCRETE, 1},
                 {SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_ON, INTEGRATED, 1},
                 {SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_ON, 4, 1},
                 {SNX_GPU_VENDOR_NVIDIA, SNX_GPU_PSTATE_OFF, DISCRETE, 0},
                 {SNX_GPU_VENDOR_AMD, SNX_GPU_PSTATE_AUTO, INTEGRATED, 0},
                 {SNX_GPU_VENDOR_QUALCOMM, SNX_GPU_PSTATE_AUTO, INTEGRATED, 1},
                 {SNX_GPU_VENDOR_QUALCOMM, SNX_GPU_PSTATE_OFF, INTEGRATED, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        SnxGpuPstateDevice d = {.vendor = cases[i].vendor, .type = cases[i].type};
        CHECK(snx_gpu_pstate_wanted(cases[i].mode, &d) == cases[i].wanted, "wanted");
    }
}

static const struct {
    const char* name;
    void (*run)(void);
} g_cases[] = {{"values", case_values},
               {"off", case_off},
               {"auto_integrated", case_auto_integrated},
               {"on_integrated", case_on_integrated},
               {"held", case_held},
#if !defined(__FreeBSD__)
               {"level_set", case_level_set},
#endif
               {"level_unread", case_level_unread},
               {"busy", case_busy},
               {"old_kernel", case_old_kernel},
               {"no_context", case_no_context},
               {"not_drm", case_not_drm},
               {"radeon", case_radeon},
               {"no_node", case_no_node},
               {"other_vendor", case_other_vendor},
               {"nvidia", case_nvidia},
#if defined(__linux__)
               {"kgsl", case_kgsl},
               {"kgsl_refused", case_kgsl_refused},
#endif
               {"wanted", case_wanted}};

int main(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof g_cases / sizeof *g_cases; i++) {
        fflush(stdout);
        pid_t p = fork();
        if (!p) {
            snprintf(g.root, sizeof g.root, "%s/snx-gpu-pstate-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
            if (!mkdtemp(g.root)) return perror("mkdtemp"), 3;
            g.fd = -1;
            g_cases[i].run();
            printf("%s: %u checks, %u failed\n", g_cases[i].name, g.checks, g.failures);
            const char* left = clean();
            if (left) printf("%s: %s was not removed: %s\n", g_cases[i].name, left, strerror(errno));
            fflush(stdout);
            _exit(g.failures ? 1 : 0);
        }
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) {
            printf("FAIL %s (status %d)\n", g_cases[i].name, st);
            failed++;
        }
    }
    printf("%s\n", failed ? "FAILED" : "ok");
    return failed ? 1 : 0;
}
