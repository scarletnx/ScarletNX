/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_pstate (gpu_pstate.h): the setting's values, and the hold on each system a way exists on */
#include "common/gpu/vk/gpu_pstate.h"
#include <errno.h>
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

static const char* const g_words[] = {"auto", "on", "off"};

#define SNX_GPU_TYPE_INTEGRATED 1 /* VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU */
#define SNX_GPU_TYPE_DISCRETE   2 /* VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU */

bool snx_gpu_pstate_wanted(int mode, const SnxGpuPstateDevice* d) {
    return mode == SNX_GPU_PSTATE_ON || (mode == SNX_GPU_PSTATE_AUTO && d->type == SNX_GPU_TYPE_DISCRETE);
}

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

static struct {
    /* the render node the hold's context is on, left open until the process ends, when the kernel closes it and puts
       the level back; -1 for none */
    int fd;
} g_gpu_pstate = {-1};

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

bool snx_gpu_pstate_hold(int mode, const SnxGpuPstateDevice* d, SnxGpuPstateSay say) {
    char head[512], line[1024];
    const char* word = mode >= 0 && mode < 3 ? g_words[mode] : "?";
    if (mode == SNX_GPU_PSTATE_OFF) {
        snprintf(line, sizeof line, "gpu_pstate=off: %s's driver chooses its clocks", d->name);
        say(line, 0);
        return false;
    }
    snprintf(head, sizeof head, "gpu_pstate=%s: %s, %s", word, d->name, type_name(d->type));
#if SNX_GPU_PSTATE_AMDGPU
    if (g_gpu_pstate.fd >= 0) {
        snprintf(line, sizeof line, "%s: its clocks are held already, by this process's first device", head);
        say(line, 0);
        return true;
    }
    int way = d->vendor == SNX_GPU_VENDOR_AMD || d->vendor == SNX_GPU_VENDOR_NVIDIA;
#else
    int way = d->vendor == SNX_GPU_VENDOR_NVIDIA;
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
