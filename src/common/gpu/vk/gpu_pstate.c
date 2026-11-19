/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_pstate (gpu_pstate.h): the setting's values, and the hold on each system a way exists on */
#include "common/gpu/vk/gpu_pstate.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <stdlib.h>
#include <sys/stat.h>
#endif
#define SNX_GPU_PSTATE_AMDGPU 1
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#define SNX_GPU_PSTATE_KGSL 1
#endif

static const char* const g_words[] = {"auto", "on", "off"};

#define SNX_GPU_TYPE_INTEGRATED 1 /* VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU */
#define SNX_GPU_TYPE_DISCRETE   2 /* VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU */

bool snx_gpu_pstate_wanted(int mode, const SnxGpuPstateDevice* d) {
    return mode == SNX_GPU_PSTATE_ON ||
           (mode == SNX_GPU_PSTATE_AUTO && (d->type == SNX_GPU_TYPE_DISCRETE || d->vendor == SNX_GPU_VENDOR_QUALCOMM));
}

static struct {
    /* amdgpu: the render node the hold's context is on, left open until the process ends, when the kernel closes it
       and puts the level back; -1 for none */
    int fd;
    /* kgsl: the device the hold's context is on (-1 for none; the keeper's thread reads it), the context, the
       CP_NOP's GPU address and object, the last frame presented (ns of the monotonic clock; the renderer writes it),
       and where the keeper says a renewal refused */
    _Atomic int kgsl;
    unsigned kgsl_ctx, kgsl_obj;
    uint64_t kgsl_nop;
    _Atomic uint64_t last_present;
    SnxGpuPstateSay say;
} g_gpu_pstate = {.fd = -1, .kgsl = -1};

int snx_gpu_pstate_mode(const char* text) {
    for (int i = 0; i < 3; i++)
        if (!strcmp(text, g_words[i])) return i;
    return -1;
}

/* the system's name, for a line that says no way exists on it */
static const char* system_name(void) {
#if defined(__ANDROID__)
    return "Android";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "this system";
#endif
}

/* a VkPhysicalDeviceType's words */
static const char* type_name(int type) {
    static const char* const names[] = {"a GPU of another kind", "an integrated GPU", "a discrete GPU", "a virtual GPU",
                                        "a CPU"};
    return type >= 0 && type < 5 ? names[type] : names[0];
}
#if SNX_GPU_PSTATE_AMDGPU
/* The kernel's interface, as its uapi headers declare it (drm.h's struct drm_version and DRM_IOCTL_VERSION,
   amdgpu_drm.h's union drm_amdgpu_ctx and DRM_IOCTL_AMDGPU_CTX), so that no header of the system's is needed */
struct snx_drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len;
    char* name;
    size_t date_len;
    char* date;
    size_t desc_len;
    char* desc;
};
union snx_amdgpu_ctx {
    struct {
        uint32_t op, flags, ctx_id;
        int32_t priority;
    } in;
    struct {
        uint32_t ctx_id, pad;
    } alloc;
    uint64_t out[2];
};
typedef char snx_amdgpu_ctx_size[sizeof(union snx_amdgpu_ctx) == 16 ? 1 : -1];
#define SNX_DRM_IOCTL_VERSION        _IOWR('d', 0x00, struct snx_drm_version)
#define SNX_DRM_IOCTL_AMDGPU_CTX     _IOWR('d', 0x40 + 0x02, union snx_amdgpu_ctx)
#define SNX_AMDGPU_CTX_OP_ALLOC_CTX  1u
#define SNX_AMDGPU_CTX_OP_SET_PSTATE 6u /* AMDGPU_CTX_OP_SET_STABLE_PSTATE */
#define SNX_AMDGPU_PSTATE_PEAK       4u /* AMDGPU_CTX_STABLE_PSTATE_PEAK */

/* path's first line, its newline cut, into out: 0, or the errno that kept it from opening */
static int read_line(const char* path, char* out, size_t n) {
    FILE* f = fopen(path, "r");
    out[0] = 0;
    if (!f) return errno;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* The render node's path in d->dev from its device number: the name the kernel gives it (Linux: DEVNAME in sysfs's
   uevent; FreeBSD: devname(3)). 0, or the errno, what it read named in where */
static int node_path(const SnxGpuPstateDevice* d, char* out, size_t n, char* where, size_t where_n) {
#if defined(__FreeBSD__)
    const char* name = devname(makedev((unsigned)d->render_major, (unsigned)d->render_minor), S_IFCHR);
    snprintf(where, where_n, "devname(3)");
    if (!name || name[0] == '#') return ENOENT;
    snprintf(out, n, "%s/%s", d->dev, name);
    return 0;
#else
    char line[256];
    snprintf(where, where_n, "%s/dev/char/%lld:%lld/uevent", d->sys, (long long)d->render_major,
             (long long)d->render_minor);
    FILE* f = fopen(where, "r");
    if (!f) return errno;
    int r = ENOENT;
    while (r && fgets(line, sizeof line, f))
        if (!strncmp(line, "DEVNAME=", 8)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, n, "%s/%s", d->dev, line + 8);
            r = 0;
        }
    fclose(f);
    return r;
#endif
}

/* amdgpu's performance level of the node (power_dpm_force_performance_level): 0, or the errno that kept it from
   reading; its path in where. FreeBSD has no sysfs to read it from */
static int dpm_level(const SnxGpuPstateDevice* d, char* out, size_t n, char* where, size_t where_n) {
#if defined(__FreeBSD__)
    (void)d, (void)n;
    snprintf(where, where_n, "of FreeBSD's amdgpu, which has no sysfs,");
    out[0] = 0;
    return ENOTSUP;
#else
    snprintf(where, where_n, "%s/dev/char/%lld:%lld/device/power_dpm_force_performance_level", d->sys,
             (long long)d->render_major, (long long)d->render_minor);
    return read_line(where, out, n);
#endif
}

/* The hold on an amdgpu node, said in a line after head. True where held */
static bool amdgpu_hold(int mode, const SnxGpuPstateDevice* d, const char* head, SnxGpuPstateSay say) {
    char node[512], where[512], level[64], level_at[512], driver[32] = {0}, line[2048];
    const char* chooses = "its driver chooses the clocks";
    int err = node_path(d, node, sizeof node, where, sizeof where);
    if (err) {
        snprintf(line, sizeof line, "%s: no render node %lld:%lld in %s (%s): %s", head, (long long)d->render_major,
                 (long long)d->render_minor, where, strerror(err), chooses);
        say(line, 1);
        return false;
    }
    int level_err = dpm_level(d, level, sizeof level, level_at, sizeof level_at);
    if (!level_err && strcmp(level, "auto")) {
        /* the kernel counts a level set on this PC as no stable pstate, so the hold's end would put auto over it */
        snprintf(line, sizeof line,
                 "%s: the clocks stay at the level set on this PC, \"%s\" (%s): a stable pstate held from it would "
                 "leave auto behind when the game ends",
                 head, level, level_at);
        say(line, 0);
        return false;
    }
    if (level_err && mode == SNX_GPU_PSTATE_AUTO) {
        snprintf(line, sizeof line,
                 "%s: the level %s does not read (%s), and a stable pstate held from a level set on this PC would "
                 "leave auto behind when the game ends: %s (gpu_pstate=on holds them anyway)",
                 head, level_at, strerror(level_err), chooses);
        say(line, 0);
        return false;
    }
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        snprintf(line, sizeof line, "%s: %s does not open (%s): %s", head, node, strerror(err), chooses);
        say(line, 1);
        return false;
    }
    struct snx_drm_version version = {0};
    version.name = driver;
    version.name_len = sizeof driver - 1;
    const char* what = "its driver's name (DRM_IOCTL_VERSION)";
    err = ioctl(fd, SNX_DRM_IOCTL_VERSION, &version) ? errno : 0;
    if (!err && strcmp(driver, "amdgpu")) {
        snprintf(line, sizeof line, "%s: %s's kernel driver is %s, not amdgpu: %s", head, node, driver, chooses);
        say(line, 1);
        close(fd);
        return false;
    }
    union snx_amdgpu_ctx ctx = {0};
    if (!err) {
        what = "a context of the runtime's own (AMDGPU_CTX_OP_ALLOC_CTX)";
        ctx.in.op = SNX_AMDGPU_CTX_OP_ALLOC_CTX;
        err = ioctl(fd, SNX_DRM_IOCTL_AMDGPU_CTX, &ctx) ? errno : 0;
    }
    if (!err) {
        uint32_t id = ctx.alloc.ctx_id;
        what = "the peak stable pstate (AMDGPU_CTX_OP_SET_STABLE_PSTATE)";
        ctx = (union snx_amdgpu_ctx){0};
        ctx.in.op = SNX_AMDGPU_CTX_OP_SET_PSTATE;
        ctx.in.ctx_id = id;
        ctx.in.flags = SNX_AMDGPU_PSTATE_PEAK;
        err = ioctl(fd, SNX_DRM_IOCTL_AMDGPU_CTX, &ctx) ? errno : 0;
    }
    if (err) {
        /* amdgpu_ctx.c's reasons: another context holds one; an op it does not know (before Linux 5.19), or the GPU at
           its thermal limit, or its power code refusing the level */
        const char* why = err == EBUSY    ? ": another process holds one on this GPU, a profiler or another game"
                          : err == EINVAL ? ": a kernel before Linux 5.19 has none, or the GPU is at its thermal limit"
                                          : "";
        snprintf(line, sizeof line, "%s: the kernel refused %s on %s (%s%s): %s", head, what, node, strerror(err), why,
                 chooses);
        say(line, 1);
        close(fd); /* its context goes with it */
        return false;
    }
    g_gpu_pstate.fd = fd;
    char now[64] = "";
    if (level_err)
        snprintf(line, sizeof line,
                 "%s: its clocks held at amdgpu's peak stable pstate (%s) until the game ends, from a level that does "
                 "not read (%s %s): the kernel puts auto back when the game ends, a crash too",
                 head, node, level_at, strerror(level_err));
    else {
        read_line(level_at, now, sizeof now);
        snprintf(line, sizeof line,
                 "%s: its clocks held at amdgpu's peak stable pstate (%s, the level auto, now \"%s\") until the game "
                 "ends, when the kernel puts auto back, a crash too",
                 head, node, now);
    }
    say(line, 0);
    return true;
}
#endif

#if SNX_GPU_PSTATE_KGSL
/* kgsl's interface, as its uapi header msm_kgsl.h declares it */
struct snx_kgsl_property {
    unsigned int type;
    void* value;
    size_t sizebytes;
};
struct snx_kgsl_ctx_create {
    unsigned int flags, drawctxt_id;
};
struct snx_kgsl_constraint {
    unsigned int type, context_id;
    void* data;
    size_t size;
};
struct snx_kgsl_alloc {
    uint64_t size, flags, va_len, mmapsize;
    unsigned int id, metadata_len;
    uint64_t metadata;
};
struct snx_kgsl_info {
    uint64_t gpuaddr, flags, size, va_len, va_addr;
    unsigned int id;
};
struct snx_kgsl_object {
    uint64_t offset, gpuaddr, size;
    unsigned int flags, id;
};
struct snx_kgsl_command {
    uint64_t flags, cmdlist;
    unsigned int cmdsize, numcmds;
    uint64_t objlist;
    unsigned int objsize, numobjs;
    uint64_t synclist;
    unsigned int syncsize, numsyncs, context_id, timestamp;
};
#define SNX_KGSL_SETPROPERTY     _IOW(0x09, 0x32, struct snx_kgsl_property)
#define SNX_KGSL_DRAWCTXT_CREATE _IOWR(0x09, 0x13, struct snx_kgsl_ctx_create)
#define SNX_KGSL_GPUOBJ_ALLOC    _IOWR(0x09, 0x45, struct snx_kgsl_alloc)
#define SNX_KGSL_GPUOBJ_INFO     _IOWR(0x09, 0x47, struct snx_kgsl_info)
#define SNX_KGSL_GPU_COMMAND     _IOWR(0x09, 0x4A, struct snx_kgsl_command)
#define SNX_KGSL_PROP_CONSTRAINT 0x12u  /* KGSL_PROP_PWR_CONSTRAINT */
#define SNX_KGSL_CONTEXT_FLAGS   0x812u /* KGSL_CONTEXT_PWR_CONSTRAINT, PREAMBLE, NO_GMEM_ALLOC: an a6xx's needs both */
#define SNX_KGSL_CONSTRAINT_LEVEL 1u    /* KGSL_CONSTRAINT_PWRLEVEL */
#define SNX_KGSL_CONSTRAINT_MAX   1u    /* KGSL_CONSTRAINT_PWR_MAX */
#define SNX_KGSL_CMDLIST_IB       1u
#define SNX_KGSL_CP_NOP           0x70108000u  /* a type 7 CP_NOP with no payload, its parity bits set */
#define SNX_KGSL_WINDOW_NS        250000000ull /* a frame presented this recently keeps the hold */

static uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* the keeper: a renewal every 40 ms while frames are presented, until one is refused */
static void* kgsl_keeper(void* unused) {
    (void)unused;
    struct timespec period = {0, 40000000L};
    while (snx_gpu_pstate_keep(monotonic_ns(),
                               atomic_load_explicit(&g_gpu_pstate.last_present, memory_order_relaxed)) >= 0)
        nanosleep(&period, NULL);
    return NULL;
}

/* The hold on kgsl's device, said in a line after head. True where held */
static bool kgsl_hold(const SnxGpuPstateDevice* d, const char* head, SnxGpuPstateSay say) {
    char node[512], line[1536];
    const char* chooses = "its driver chooses the clocks";
    snprintf(node, sizeof node, "%s/kgsl-3d0", d->dev);
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        snprintf(line, sizeof line, "%s: %s does not open (%s): %s", head, node, strerror(err), chooses);
        say(line, 1);
        return false;
    }
    struct snx_kgsl_ctx_create ctx = {SNX_KGSL_CONTEXT_FLAGS, 0};
    unsigned level = SNX_KGSL_CONSTRAINT_MAX;
    struct snx_kgsl_constraint constraint = {SNX_KGSL_CONSTRAINT_LEVEL, 0, &level, sizeof level};
    struct snx_kgsl_property property = {SNX_KGSL_PROP_CONSTRAINT, &constraint, sizeof constraint};
    struct snx_kgsl_alloc alloc = {.size = 4096};
    struct snx_kgsl_info info = {0};
    const char* what = "a context of the runtime's own (IOCTL_KGSL_DRAWCTXT_CREATE)";
    int err = ioctl(fd, SNX_KGSL_DRAWCTXT_CREATE, &ctx) ? errno : 0;
    if (!err) {
        what = "the top power level as its constraint (KGSL_PROP_PWR_CONSTRAINT)";
        constraint.context_id = ctx.drawctxt_id;
        err = ioctl(fd, SNX_KGSL_SETPROPERTY, &property) ? errno : 0;
    }
    if (!err) {
        what = "a page for its commands (IOCTL_KGSL_GPUOBJ_ALLOC)";
        err = ioctl(fd, SNX_KGSL_GPUOBJ_ALLOC, &alloc) ? errno : 0;
    }
    if (!err) {
        what = "the page's GPU address (IOCTL_KGSL_GPUOBJ_INFO)";
        info.id = alloc.id;
        err = ioctl(fd, SNX_KGSL_GPUOBJ_INFO, &info) ? errno : 0;
    }
    void* page = MAP_FAILED;
    if (!err) {
        what = "the page mapped (mmap)";
        page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)alloc.id << 12);
        err = page == MAP_FAILED ? errno : 0;
    }
    if (err) {
        snprintf(line, sizeof line, "%s: kgsl refused %s on %s (%s): %s", head, what, node, strerror(err), chooses);
        say(line, 1);
        close(fd); /* the context and the page go with it */
        return false;
    }
    *(volatile uint32_t*)page = SNX_KGSL_CP_NOP;
    munmap(page, 4096);
    g_gpu_pstate.kgsl_ctx = ctx.drawctxt_id;
    g_gpu_pstate.kgsl_obj = alloc.id;
    g_gpu_pstate.kgsl_nop = info.gpuaddr;
    g_gpu_pstate.say = say;
    atomic_store_explicit(&g_gpu_pstate.kgsl, fd, memory_order_release);
    pthread_t keeper;
    pthread_attr_t attr;
    err = pthread_attr_init(&attr);
    if (!err) err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!err) err = pthread_create(&keeper, &attr, kgsl_keeper, NULL);
    if (err) {
        atomic_store_explicit(&g_gpu_pstate.kgsl, -1, memory_order_release);
        close(fd);
        snprintf(line, sizeof line, "%s: the thread that renews kgsl's constraint did not start (%s): %s", head,
                 strerror(err), chooses);
        say(line, 1);
        return false;
    }
    snprintf(line, sizeof line,
             "%s: its clocks held at kgsl's top power level (KGSL_PROP_PWR_CONSTRAINT of a context of the runtime's "
             "own on %s) while the game presents frames, renewed every 40 ms; kgsl drops it %llu ms after the last "
             "frame, and with the context when the game ends, a crash too",
             head, node, (unsigned long long)(SNX_KGSL_WINDOW_NS / 1000000));
    say(line, 0);
    return true;
}
#endif

void snx_gpu_pstate_present(void) {
#if SNX_GPU_PSTATE_KGSL
    if (atomic_load_explicit(&g_gpu_pstate.kgsl, memory_order_relaxed) >= 0)
        atomic_store_explicit(&g_gpu_pstate.last_present, monotonic_ns(), memory_order_relaxed);
#endif
}

int snx_gpu_pstate_keep(uint64_t now_ns, uint64_t last_present_ns) {
#if SNX_GPU_PSTATE_KGSL
    int fd = atomic_load_explicit(&g_gpu_pstate.kgsl, memory_order_acquire);
    if (fd < 0 || !last_present_ns || now_ns - last_present_ns >= SNX_KGSL_WINDOW_NS) return 0;
    struct snx_kgsl_object nop = {
        .gpuaddr = g_gpu_pstate.kgsl_nop, .size = 4, .flags = SNX_KGSL_CMDLIST_IB, .id = g_gpu_pstate.kgsl_obj};
    struct snx_kgsl_command command = {.flags = SNX_KGSL_CONTEXT_FLAGS & 0x800u, /* KGSL_CMDBATCH_PWR_CONSTRAINT */
                                       .cmdlist = (uintptr_t)&nop,
                                       .cmdsize = sizeof nop,
                                       .numcmds = 1,
                                       .context_id = g_gpu_pstate.kgsl_ctx};
    if (!ioctl(fd, SNX_KGSL_GPU_COMMAND, &command)) return 1;
    int err = errno;
    char line[512];
    snprintf(line, sizeof line,
             "gpu_pstate: kgsl refused the command that renews its constraint (IOCTL_KGSL_GPU_COMMAND, %s): the "
             "clocks are the driver's from now on",
             strerror(err));
    atomic_store_explicit(&g_gpu_pstate.kgsl, -1, memory_order_release);
    close(fd);
    g_gpu_pstate.say(line, 1);
    return -1;
#else
    (void)now_ns, (void)last_present_ns;
    return 0;
#endif
}

bool snx_gpu_pstate_hold(int mode, const SnxGpuPstateDevice* d, SnxGpuPstateSay say) {
    char head[512], line[1024];
    const char* word = mode >= 0 && mode < 3 ? g_words[mode] : "?";
    if (mode == SNX_GPU_PSTATE_OFF) {
        snprintf(line, sizeof line, "gpu_pstate=off: %s's driver chooses its clocks", d->name);
        say(line, 0);
        return false;
    }
    snprintf(head, sizeof head, "gpu_pstate=%s: %s, %s", word, d->name, type_name(d->type));
    if (g_gpu_pstate.fd >= 0 || atomic_load_explicit(&g_gpu_pstate.kgsl, memory_order_relaxed) >= 0) {
        snprintf(line, sizeof line, "%s: its clocks are held already, by this process's first device", head);
        say(line, 0);
        return true;
    }
    int way = d->vendor == SNX_GPU_VENDOR_NVIDIA;
#if SNX_GPU_PSTATE_AMDGPU
    way = way || d->vendor == SNX_GPU_VENDOR_AMD;
#endif
#if SNX_GPU_PSTATE_KGSL
    way = way || d->vendor == SNX_GPU_VENDOR_QUALCOMM;
#endif
    if (!way) {
        snprintf(line, sizeof line, "%s: no way to hold this GPU's clocks on %s: its driver chooses them", head,
                 system_name());
        say(line, mode == SNX_GPU_PSTATE_ON);
        return false;
    }
    if (!snx_gpu_pstate_wanted(mode, d)) {
        snprintf(line, sizeof line, "%s: its driver chooses its clocks (gpu_pstate=on holds them)%s", head,
                 d->type == SNX_GPU_TYPE_INTEGRATED
                     ? ": an integrated GPU shares its power with the CPU, whose threads it would take"
                     : "");
        say(line, 0);
        return false;
    }
    if (d->vendor == SNX_GPU_VENDOR_NVIDIA) {
        if (d->boost_lacks) {
            snprintf(line, sizeof line, "%s: %s: its driver chooses its clocks", head, d->boost_lacks);
            say(line, mode == SNX_GPU_PSTATE_ON);
            return false;
        }
        snprintf(line, sizeof line,
                 "%s: its clocks boosted while the game runs, through VK_NV_low_latency2's lowLatencyBoost on each "
                 "swapchain (its low latency mode off), which ends with the swapchain, a crash too",
                 head);
        say(line, 0);
        return true;
    }
#if SNX_GPU_PSTATE_KGSL
    if (d->vendor == SNX_GPU_VENDOR_QUALCOMM) return kgsl_hold(d, head, say);
#endif
#if SNX_GPU_PSTATE_AMDGPU
    if (!d->has_render) {
        snprintf(line, sizeof line,
                 "%s: its Vulkan device names no render node (VK_EXT_physical_device_drm, whose line says why): its "
                 "driver chooses the clocks",
                 head);
        say(line, mode == SNX_GPU_PSTATE_ON);
        return false;
    }
    return amdgpu_hold(mode, d, head, say);
#else
    return false;
#endif
}
