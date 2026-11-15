/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* dlss.h: NVIDIA DLSS Super Resolution through NVIDIA Streamline's manual hooking (Streamline's
   docs/ProgrammingGuideManualHooking.md and ProgrammingGuideDLSS.md). Streamline's headers are MIT (Copyright (c)
   2022-2023 NVIDIA CORPORATION, NVIDIA-RTX/Streamline v2.14.16, fetched by cmake/dependencies.cmake), and only a
   Windows build with them (SNX_STREAMLINE) has DLSS. Its runtime is NVIDIA's, the user's download, loaded from the
   folder they name: nothing of it is linked or shipped */
#include "nx/gpu/vk/upscaler/dlss.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(SNX_STREAMLINE)
#include <stddef.h>
#include <atomic>
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#pragma clang diagnostic pop
#pragma clang diagnostic ignored "-Wdeprecated-declarations" /* DLSSOptions' sharpness, which nothing here sets */

namespace {

/* a project of our own: Streamline wants one for a custom engine, and NVIDIA's application ids are for others */
const char* const PROJECT_ID = "4f9a6c2e-8d31-4b57-a0e2-7c5b1d93e6f4";
enum { NAMES = 32, NAME = 128 };

struct Api {
    PFun_slInit* init;
    PFun_slShutdown* shutdown;
    PFun_slIsFeatureSupported* supported;
    PFun_slGetFeatureRequirements* requirements;
    PFun_slGetFeatureVersion* version;
    PFun_slGetFeatureFunction* function;
    PFun_slGetNewFrameToken* token;
    PFun_slSetConstants* constants;
    PFun_slEvaluateFeature* evaluate;
    PFun_slFreeResources* free_resources;
    PFun_slSetVulkanInfo* vulkan_info;
    PFun_slDLSSSetOptions* set_options;
    PFun_slDLSSGetOptimalSettings* optimal;
    PFun_slDLSSGetState* state;
};

struct State {
    HMODULE module;
    Api api;
    int started, attached, preset;
    SnxDlssLog log;
    char instance_ext[NAMES][NAME], device_ext[NAMES][NAME];
    const char* instance_ptr[NAMES];
    uint32_t instance_n, device_n;
    char features12[NAMES][NAME], features13[NAMES][NAME];
    const char* features12_ptr[NAMES];
    const char* features13_ptr[NAMES];
    uint32_t features12_n, features13_n;
    char version[96];
    std::atomic<int> failed; /* an evaluation failed: DLSS is not available from then on */
    std::atomic<uint64_t> evaluated, failures;
    char why[256];
    char fail_reason[256]; /* written once, before failed is set */
};
State g;

void say(int level, const char* fmt, ...) {
    if (!g.log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g.log(level, line);
}

void from_streamline(sl::LogType type, const char* message) {
    char line[512];
    snprintf(line, sizeof line, "%s", message ? message : "");
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    say(type == sl::LogType::eError ? 2 : type == sl::LogType::eWarn ? 1 : 0, "Streamline: %s", line);
}

/* signed by NVIDIA Corporation (Authenticode, the chain checked without going online) */
int signed_by_nvidia(const wchar_t* path) {
    WINTRUST_FILE_INFO file = {};
    file.cbStruct = sizeof file;
    file.pcwszFilePath = path;
    WINTRUST_DATA data = {};
    data.cbStruct = sizeof data;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    int valid = WinVerifyTrust(NULL, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : NULL;
        CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : NULL;
        wchar_t name[128] = {0};
        valid = cert && CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 128) > 1 &&
                wcscmp(name, L"NVIDIA Corporation") == 0;
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &data);
    return valid;
}

template <typename T> int resolve(const char* name, T*& fn) {
    fn = reinterpret_cast<T*>(reinterpret_cast<void*>(GetProcAddress(g.module, name)));
    if (!fn) snprintf(g.why, sizeof g.why, "sl.interposer.dll has no %s", name);
    return fn != NULL;
}

template <typename T> int feature_function(const char* name, T*& fn) {
    void* p = NULL;
    fn = NULL;
    if (g.api.function(sl::kFeatureDLSS, name, p) == sl::Result::eOk && p) fn = reinterpret_cast<T*>(p);
    return fn != NULL;
}

int refuse(const char** why, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.why, sizeof g.why, fmt, ap);
    va_end(ap);
    *why = g.why;
    return 0;
}

void copy_names(uint32_t count, const char** from, char (*to)[NAME], const char** ptr, uint32_t* n) {
    *n = 0;
    for (uint32_t i = 0; i < count && *n < NAMES; i++) {
        snprintf(to[*n], NAME, "%s", from[i]);
        if (ptr) ptr[*n] = to[*n];
        (*n)++;
    }
}

/* every VkBool32 of a Vulkan 1.2 or 1.3 feature struct, from the first feature on: want |= asked where have has it.
   0 when have lacks one asked */
int merge(void* want, const void* have, const void* asked, size_t first, size_t size) {
    for (size_t o = first; o + sizeof(VkBool32) <= size; o += sizeof(VkBool32)) {
        VkBool32 a, h;
        memcpy(&a, (const char*)asked + o, sizeof a);
        memcpy(&h, (const char*)have + o, sizeof h);
        if (a && !h) return 0;
    }
    for (size_t o = first; o + sizeof(VkBool32) <= size; o += sizeof(VkBool32)) {
        VkBool32 a;
        memcpy(&a, (const char*)asked + o, sizeof a);
        if (a) memcpy((char*)want + o, &a, sizeof a);
    }
    return 1;
}

void list(char* out, size_t cap, uint32_t n, char (*names)[NAME]) {
    out[0] = 0;
    for (uint32_t i = 0; i < n; i++) snprintf(out + strlen(out), cap - strlen(out), "%s%s", i ? ", " : "", names[i]);
    if (!n) snprintf(out, cap, "none");
}

sl::float4x4 identity() {
    sl::float4x4 m;
    m.setRow(0, {1, 0, 0, 0});
    m.setRow(1, {0, 1, 0, 0});
    m.setRow(2, {0, 0, 1, 0});
    m.setRow(3, {0, 0, 0, 1});
    return m;
}

/* DLSS checks the render size against its mode's range: the mode of this scale */
sl::DLSSMode mode_for(uint32_t render, uint32_t output) {
    float ratio = (float)render / (float)output;
    if (ratio >= 0.99f) return sl::DLSSMode::eDLAA;
    if (ratio >= 0.65f) return sl::DLSSMode::eMaxQuality;
    if (ratio >= 0.57f) return sl::DLSSMode::eBalanced;
    if (ratio >= 0.49f) return sl::DLSSMode::eMaxPerformance;
    return sl::DLSSMode::eUltraPerformance;
}
const char* mode_name(sl::DLSSMode m) {
    switch (m) {
        case sl::DLSSMode::eDLAA: return "DLAA";
        case sl::DLSSMode::eMaxQuality: return "Quality";
        case sl::DLSSMode::eBalanced: return "Balanced";
        case sl::DLSSMode::eMaxPerformance: return "Performance";
        case sl::DLSSMode::eUltraPerformance: return "Ultra Performance";
        default: return "?";
    }
}

sl::Resource resource(const SnxUpscaleImage& i, VkImageLayout layout, VkImageUsageFlags usage) {
    sl::Resource r(sl::ResourceType::eTex2d, (void*)i.image, NULL, (void*)i.view, (uint32_t)layout);
    r.width = i.width;
    r.height = i.height;
    r.nativeFormat = (uint32_t)i.format;
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = usage;
    return r;
}

} // namespace

struct SnxDlss {
    SnxUpscalerInfo info;
    uint32_t frame;
    int options_set;
    sl::DLSSMode mode;
    uint32_t render[2];
    float pre_exposure;
    int auto_exposure;
};

extern "C" {

int snx_dlss_start(const char* folder, int preset, SnxDlssLog log, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (g.started) return 1;
    g.log = log;
    g.preset = preset;
    if (!folder || !*folder) return refuse(why, "no folder holds NVIDIA's DLSS runtime (the dlss_runtime setting)");
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32], data[MAX_PATH + 32];
    if (!MultiByteToWideChar(CP_UTF8, 0, folder, -1, dir, MAX_PATH))
        return refuse(why, "the folder's name is too long");
    size_t len = wcslen(dir);
    for (size_t i = 0; i < len; i++)
        if (dir[i] == L'/') dir[i] = L'\\'; /* LoadLibraryEx with an altered search path takes backslashes only */
    while (len && dir[len - 1] == L'\\') dir[--len] = 0;
    _snwprintf(path, MAX_PATH + 32, L"%ls\\sl.interposer.dll", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return refuse(why, "%s has no sl.interposer.dll (NVIDIA Streamline)", folder);
    if (!signed_by_nvidia(path))
        return refuse(why, "%s\\sl.interposer.dll is not signed by NVIDIA Corporation: not loaded", folder);
    if (!g.module) {
        g.module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g.module)
            return refuse(why, "%s\\sl.interposer.dll can't be loaded (error %lu)", folder,
                          (unsigned long)GetLastError());
        Api& a = g.api;
        if (!resolve("slInit", a.init) || !resolve("slShutdown", a.shutdown) ||
            !resolve("slIsFeatureSupported", a.supported) || !resolve("slGetFeatureRequirements", a.requirements) ||
            !resolve("slGetFeatureVersion", a.version) || !resolve("slGetFeatureFunction", a.function) ||
            !resolve("slGetNewFrameToken", a.token) || !resolve("slSetConstants", a.constants) ||
            !resolve("slEvaluateFeature", a.evaluate) || !resolve("slFreeResources", a.free_resources) ||
            !resolve("slSetVulkanInfo", a.vulkan_info)) {
            FreeLibrary(g.module);
            g.module = NULL;
            *why = g.why;
            return 0;
        }
    }
    /* its log and NGX's data in a folder of the runtime's: nothing written outside the user's folder. No downloads
       (no OTA): the runtime is the folder's */
    _snwprintf(data, MAX_PATH + 32, L"%ls\\data", dir);
    CreateDirectoryW(data, NULL);
    const wchar_t* plugins[] = {dir};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.logMessageCallback = from_streamline;
    pref.pathsToPlugins = plugins;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = data;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = PROJECT_ID;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = g.api.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) return refuse(why, "Streamline did not start (slInit result %d)", (int)r);
    g.started = 1;
    sl::FeatureRequirements req{};
    r = g.api.requirements(sl::kFeatureDLSS, req);
    if (r != sl::Result::eOk) {
        snx_dlss_stop();
        return refuse(why, "Streamline found no DLSS (sl.dlss.dll, result %d)", (int)r);
    }
    if (!((uint32_t)req.flags & (uint32_t)sl::FeatureRequirementFlags::eVulkanSupported)) {
        snx_dlss_stop();
        return refuse(why, "Streamline's DLSS does not run on Vulkan here");
    }
    if (req.vkNumComputeQueuesRequired || req.vkNumGraphicsQueuesRequired || req.vkNumOpticalFlowQueuesRequired) {
        snx_dlss_stop();
        return refuse(why, "this DLSS wants queues of its own (%u compute, %u graphics, %u optical flow)",
                      req.vkNumComputeQueuesRequired, req.vkNumGraphicsQueuesRequired,
                      req.vkNumOpticalFlowQueuesRequired);
    }
    copy_names(req.vkNumInstanceExtensions, req.vkInstanceExtensions, g.instance_ext, g.instance_ptr, &g.instance_n);
    copy_names(req.vkNumDeviceExtensions, req.vkDeviceExtensions, g.device_ext, NULL, &g.device_n);
    copy_names(req.vkNumFeatures12, req.vkFeatures12, g.features12, g.features12_ptr, &g.features12_n);
    copy_names(req.vkNumFeatures13, req.vkFeatures13, g.features13, g.features13_ptr, &g.features13_n);
    sl::FeatureVersion v{};
    g.api.version(sl::kFeatureDLSS, v);
    snprintf(g.version, sizeof g.version, "Streamline %u.%u.%u, DLSS %u.%u.%u", v.versionSL.major, v.versionSL.minor,
             v.versionSL.build, v.versionNGX.major, v.versionNGX.minor, v.versionNGX.build);
    char ie[512], de[1024], f12[1024], f13[512];
    list(ie, sizeof ie, g.instance_n, g.instance_ext);
    list(de, sizeof de, g.device_n, g.device_ext);
    list(f12, sizeof f12, g.features12_n, g.features12);
    list(f13, sizeof f13, g.features13_n, g.features13);
    say(0,
        "DLSS: %s started from %s. It asks for instance extensions %s, device extensions %s, Vulkan 1.2 "
        "features %s, 1.3 features %s",
        g.version, folder, ie, de, f12, f13);
    return 1;
}

uint32_t snx_dlss_instance_extensions(const char* const** names) {
    *names = g.instance_ptr;
    return g.started ? g.instance_n : 0;
}

int snx_dlss_device(VkPhysicalDevice physical, const char** ext, uint32_t* n, uint32_t cap,
                    VkPhysicalDeviceVulkan12Features* want12, const VkPhysicalDeviceVulkan12Features* have12,
                    VkPhysicalDeviceVulkan13Features* want13, const VkPhysicalDeviceVulkan13Features* have13,
                    const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (!g.started) return refuse(why, "Streamline is not running");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    if (props.vendorID != 0x10DE) {
        snx_dlss_stop();
        return refuse(why, "DLSS runs on NVIDIA GPUs, and this is %s", props.deviceName);
    }
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL);
    VkExtensionProperties* have = (VkExtensionProperties*)calloc(count ? count : 1, sizeof *have);
    vkEnumerateDeviceExtensionProperties(physical, NULL, &count, have);
    const char* add[NAMES];
    uint32_t nadd = 0;
    int ok = 1;
    for (uint32_t i = 0; i < g.device_n && ok; i++) {
        const char* name = g.device_ext[i];
        int found = 0;
        for (uint32_t j = 0; j < count && !found; j++) found = strcmp(have[j].extensionName, name) == 0;
        if (!found) {
            refuse(why, "%s has no %s, which DLSS needs", props.deviceName, name);
            ok = 0;
            break;
        }
        int there = 0;
        for (uint32_t j = 0; j < *n && !there; j++) there = strcmp(ext[j], name) == 0;
        /* the KHR buffer device address and the EXT one can't both be enabled, and Vulkan 1.2 has the KHR one */
        if (!there && strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0 &&
            (props.apiVersion >= VK_API_VERSION_1_2))
            there = 1;
        if (!there) add[nadd++] = name;
    }
    free(have);
    if (ok && *n + nadd > cap) {
        refuse(why, "the device's extension list is full (%u of %u, DLSS adds %u)", *n, cap, nadd);
        ok = 0;
    }
    VkPhysicalDeviceVulkan12Features asked12 =
        sl::getVkPhysicalDeviceVulkan12Features(g.features12_n, g.features12_ptr);
    VkPhysicalDeviceVulkan13Features asked13 =
        sl::getVkPhysicalDeviceVulkan13Features(g.features13_n, g.features13_ptr);
    VkPhysicalDeviceVulkan12Features w12 = *want12;
    VkPhysicalDeviceVulkan13Features w13 = *want13;
    if (ok && !merge(&w12, have12, &asked12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge),
                     sizeof w12)) {
        refuse(why, "%s lacks a Vulkan 1.2 feature DLSS needs (of those it asks for)", props.deviceName);
        ok = 0;
    }
    if (ok &&
        !merge(&w13, have13, &asked13, offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess), sizeof w13)) {
        refuse(why, "%s lacks a Vulkan 1.3 feature DLSS needs (of those it asks for)", props.deviceName);
        ok = 0;
    }
    if (!ok) {
        const char* reason = g.why;
        char keep[256];
        snprintf(keep, sizeof keep, "%s", reason);
        snx_dlss_stop();
        snprintf(g.why, sizeof g.why, "%s", keep);
        *why = g.why;
        return 0;
    }
    for (uint32_t i = 0; i < nadd; i++) ext[(*n)++] = add[i];
    /* the pNext chains stay the caller's */
    w12.pNext = want12->pNext;
    w13.pNext = want13->pNext;
    *want12 = w12;
    *want13 = w13;
    return 1;
}

int snx_dlss_attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t graphics_family,
                    uint32_t graphics_index, uint32_t compute_family, uint32_t compute_index, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (!g.started) return refuse(why, "Streamline is not running");
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    /* Streamline makes no queues of its own for DLSS (snx_dlss_start refuses a DLSS that asks for some) */
    info.computeQueueFamily = compute_family;
    info.computeQueueIndex = compute_index;
    info.graphicsQueueFamily = graphics_family;
    info.graphicsQueueIndex = graphics_index;
    sl::Result r = g.api.vulkan_info(info);
    if (r != sl::Result::eOk) {
        snx_dlss_stop();
        return refuse(why, "Streamline did not take the Vulkan device (result %d)", (int)r);
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    r = g.api.supported(sl::kFeatureDLSS, adapter);
    if (r != sl::Result::eOk) {
        snx_dlss_stop();
        return refuse(why, "DLSS is not supported on this GPU or driver (Streamline result %d)", (int)r);
    }
    if (!feature_function("slDLSSSetOptions", g.api.set_options)) {
        snx_dlss_stop();
        return refuse(why, "Streamline's DLSS has no slDLSSSetOptions");
    }
    feature_function("slDLSSGetOptimalSettings", g.api.optimal);
    feature_function("slDLSSGetState", g.api.state);
    g.attached = 1;
    say(0, "DLSS: %s runs on this device (preset %c)", g.version, g.preset ? 'A' + g.preset - 1 : '-');
    return 1;
}

PFN_vkVoidFunction snx_dlss_proxy(const char* name) {
    if (!g.started || !g.module) return NULL;
    return reinterpret_cast<PFN_vkVoidFunction>(reinterpret_cast<void*>(GetProcAddress(g.module, name)));
}

void snx_dlss_stop(void) {
    if (!g.started) return;
    sl::Result r = g.api.shutdown();
    g.started = 0;
    g.attached = 0;
    say(0, "DLSS: Streamline stopped (result %d) after %llu evaluations, %llu failed", (int)r,
        (unsigned long long)g.evaluated.load(), (unsigned long long)g.failures.load());
}

int snx_dlss_available(const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (g.failed.load()) {
        *why = g.fail_reason;
        return 0;
    }
    if (g.attached) return 1;
    *why =
        g.why[0] ? g.why : "DLSS is not running: name the folder of NVIDIA's DLSS runtime in the dlss_runtime setting";
    return 0;
}

SnxDlss* snx_dlss_create(const SnxUpscalerInfo* info, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (!snx_dlss_available(why)) return NULL;
    /* off for the rest of the run, as for a failed evaluation: the choice, which knows nothing of where the motion
       vectors are, would pick DLSS again for each of the game's evaluations, and the caller would run its own
       fallback every time instead of the next upscaler */
    if (info->flags & SNX_UP_FLAG_DISPLAY_MOTION) {
        if (!g.failed.load()) {
            snprintf(g.fail_reason, sizeof g.fail_reason,
                     "DLSS takes motion vectors at the render size here, and these are at the output's: DLSS is off "
                     "from now on");
            g.failed.store(1);
            say(1, "DLSS: %s", g.fail_reason);
        }
        *why = g.fail_reason;
        return NULL;
    }
    SnxDlss* d = (SnxDlss*)calloc(1, sizeof *d);
    if (!d) {
        *why = "no memory";
        return NULL;
    }
    d->info = *info;
    return d;
}

int snx_dlss_evaluate(SnxDlss* d, const SnxUpscaleEval* e, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    const Api& a = g.api;
    const SnxUpscalerInfo& in = d->info;
    if (!g.attached || g.failed.load()) {
        *why = "DLSS is not available";
        return 0;
    }
    if (!e->depth.image || !e->motion.image) {
        *why = "DLSS needs depth and motion vectors";
        return 0;
    }
    sl::ViewportHandle viewport(0u);
    sl::DLSSMode mode = mode_for(e->render[0], in.output[0]);
    int auto_exposure = (in.flags & SNX_UP_FLAG_AUTO_EXPOSURE) || !e->exposure.image;
    float pre = e->pre_exposure > 0 ? e->pre_exposure : 1.0f;
    sl::Result r = sl::Result::eOk;
    const char* stage = NULL;
    if (!d->options_set || mode != d->mode || pre != d->pre_exposure || auto_exposure != d->auto_exposure) {
        sl::DLSSOptions o{};
        o.mode = mode;
        o.outputWidth = in.output[0];
        o.outputHeight = in.output[1];
        o.preExposure = pre;
        o.exposureScale = 1.0f;
        o.colorBuffersHDR = in.flags & SNX_UP_FLAG_HDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        o.useAutoExposure = auto_exposure ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        sl::DLSSPreset p = g.preset ? (sl::DLSSPreset)g.preset : sl::DLSSPreset::eDefault;
        o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset =
            o.ultraQualityPreset = p;
        r = a.set_options(viewport, o);
        if (r != sl::Result::eOk)
            stage = "slDLSSSetOptions";
        else {
            int first = !d->options_set;
            d->options_set = 1;
            d->mode = mode;
            d->pre_exposure = pre;
            d->auto_exposure = auto_exposure;
            sl::DLSSOptimalSettings best{};
            if (a.optimal && a.optimal(o, best) == sl::Result::eOk)
                say(0,
                    "DLSS: %s mode for %ux%u to %ux%u (%s, %s exposure, preset %c, this mode renders %ux%u, "
                    "%ux%u to %ux%u)%s",
                    mode_name(mode), e->render[0], e->render[1], in.output[0], in.output[1],
                    in.flags & SNX_UP_FLAG_HDR ? "HDR" : "SDR", auto_exposure ? "its own" : "the game's",
                    g.preset ? 'A' + g.preset - 1 : '-', best.optimalRenderWidth, best.optimalRenderHeight,
                    best.renderWidthMin, best.renderHeightMin, best.renderWidthMax, best.renderHeightMax,
                    first ? "" : ", changed");
        }
    }
    sl::FrameToken* frame = NULL;
    uint32_t index = d->frame++;
    if (!stage && ((r = a.token(frame, &index)) != sl::Result::eOk || !frame)) stage = "slGetNewFrameToken";
    if (!stage) {
        float rw = (float)e->render[0], rh = (float)e->render[1];
        sl::Constants c{};
        c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = identity();
        /* the game's DLSS evaluation gives what DLSS takes: the jitter in render pixels, and motion vectors that a
           scale brings to render pixels, pointing to where a pixel was the frame before. Streamline wants that scale
           to its -1..1 screen units */
        c.jitterOffset = {e->jitter[0], e->jitter[1]};
        c.mvecScale = {e->motion_scale[0] / rw, e->motion_scale[1] / rh};
        c.cameraPinholeOffset = {0, 0};
        c.cameraPos = {0, 0, 0};
        c.cameraUp = {0, 1, 0};
        c.cameraRight = {1, 0, 0};
        c.cameraFwd = {0, 0, 1};
        c.cameraNear = e->near_plane > 0 ? e->near_plane : 0.1f;
        c.cameraFar = e->far_plane > 0 ? e->far_plane : 1000.0f;
        c.cameraFOV = e->fov_y > 0 ? e->fov_y : 1.0f;
        c.cameraAspectRatio = rw / rh;
        c.motionVectorsInvalidValue = 0;
        c.depthInverted = in.flags & SNX_UP_FLAG_INVERTED_DEPTH ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = e->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.motionVectorsJittered = in.flags & SNX_UP_FLAG_JITTERED_MOTION ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        if ((r = a.constants(c, *frame, viewport)) != sl::Result::eOk) stage = "slSetConstants";
    }
    if (!stage) {
        const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sl::Resource color = resource(e->color, ro, VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource depth = resource(e->depth, ro, VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource motion = resource(e->motion, ro, VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource exposure = resource(e->exposure, ro, VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource bias = resource(e->reactive, ro, VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource output = resource(e->output, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        sl::Extent render_area{0, 0, e->render[0], e->render[1]};
        sl::Extent output_area{0, 0, e->output.width, e->output.height};
        sl::Extent one{0, 0, 1, 1};
        const sl::ResourceLifecycle now = sl::ResourceLifecycle::eValidUntilEvaluate;
        sl::ResourceTag color_tag(&color, sl::kBufferTypeScalingInputColor, now, &render_area);
        sl::ResourceTag depth_tag(&depth, sl::kBufferTypeDepth, now, &render_area);
        sl::ResourceTag motion_tag(&motion, sl::kBufferTypeMotionVectors, now, &render_area);
        sl::ResourceTag output_tag(&output, sl::kBufferTypeScalingOutputColor, now, &output_area);
        sl::ResourceTag exposure_tag(&exposure, sl::kBufferTypeExposure, now, &one);
        sl::ResourceTag bias_tag(&bias, sl::kBufferTypeBiasCurrentColorHint, now, &render_area);
        const sl::BaseStructure* inputs[7] = {&viewport, &color_tag, &depth_tag, &motion_tag, &output_tag};
        uint32_t ni = 5;
        if (!auto_exposure && e->exposure.image) inputs[ni++] = &exposure_tag;
        if (e->reactive.image) inputs[ni++] = &bias_tag;
        r = a.evaluate(sl::kFeatureDLSS, *frame, inputs, ni, reinterpret_cast<sl::CommandBuffer*>(e->cb));
        /* out of the VRAM budget is a warning given after the evaluation was recorded */
        if (r != sl::Result::eOk && r != sl::Result::eWarnOutOfVRAM) stage = "slEvaluateFeature";
    }
    if (stage) {
        g.failures++;
        if (!g.failed.load()) {
            snprintf(g.fail_reason, sizeof g.fail_reason,
                     "DLSS failed at %s (Streamline result %d), and is off from now on", stage, (int)r);
            g.failed.store(1);
            say(2, "DLSS: %s", g.fail_reason);
        }
        *why = g.fail_reason;
        return 0;
    }
    uint64_t n = ++g.evaluated;
    /* proof it keeps running: the first ones, then every 600th */
    if (n <= 3 || n % 600 == 0) {
        sl::DLSSState st{};
        char mem[64] = "";
        if (a.state && a.state(viewport, st) == sl::Result::eOk)
            snprintf(mem, sizeof mem, ", %.1f MB of VRAM", (double)st.estimatedVRAMUsageInBytes / 1e6);
        say(0, "DLSS: %llu evaluations recorded, %llu failed (%ux%u to %ux%u, jitter %g %g, reset %d%s)",
            (unsigned long long)n, (unsigned long long)g.failures.load(), e->render[0], e->render[1], in.output[0],
            in.output[1], (double)e->jitter[0], (double)e->jitter[1], e->reset, mem);
    }
    return 1;
}

void snx_dlss_destroy(SnxDlss* d) {
    if (!d) return;
    if (g.attached && d->options_set) g.api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle(0u));
    free(d);
}

void snx_dlss_counts(uint64_t* evaluated, uint64_t* failed) {
    *evaluated = g.evaluated.load();
    *failed = g.failures.load();
}

} // extern "C"

#else /* no Streamline: a build for another system, or without its headers */

extern "C" {
static const char* const g_none = "DLSS is not built in here: NVIDIA's Streamline runs on Windows only";
int snx_dlss_start(const char* folder, int preset, SnxDlssLog log, const char** why) {
    (void)folder, (void)preset, (void)log;
    if (why) *why = g_none;
    return 0;
}
uint32_t snx_dlss_instance_extensions(const char* const** names) {
    *names = NULL;
    return 0;
}
int snx_dlss_device(VkPhysicalDevice physical, const char** ext, uint32_t* n, uint32_t cap,
                    VkPhysicalDeviceVulkan12Features* want12, const VkPhysicalDeviceVulkan12Features* have12,
                    VkPhysicalDeviceVulkan13Features* want13, const VkPhysicalDeviceVulkan13Features* have13,
                    const char** why) {
    (void)physical, (void)ext, (void)n, (void)cap, (void)want12, (void)have12, (void)want13, (void)have13;
    if (why) *why = g_none;
    return 0;
}
int snx_dlss_attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t graphics_family,
                    uint32_t graphics_index, uint32_t compute_family, uint32_t compute_index, const char** why) {
    (void)instance, (void)physical, (void)device, (void)graphics_family, (void)graphics_index, (void)compute_family,
        (void)compute_index;
    if (why) *why = g_none;
    return 0;
}
PFN_vkVoidFunction snx_dlss_proxy(const char* name) {
    (void)name;
    return NULL;
}
void snx_dlss_stop(void) {}
int snx_dlss_available(const char** why) {
    if (why) *why = g_none;
    return 0;
}
SnxDlss* snx_dlss_create(const SnxUpscalerInfo* info, const char** why) {
    (void)info;
    if (why) *why = g_none;
    return NULL;
}
int snx_dlss_evaluate(SnxDlss* d, const SnxUpscaleEval* e, const char** why) {
    (void)d, (void)e;
    if (why) *why = g_none;
    return 0;
}
void snx_dlss_destroy(SnxDlss* d) { (void)d; }
void snx_dlss_counts(uint64_t* evaluated, uint64_t* failed) {
    *evaluated = 0;
    *failed = 0;
}
}
#endif
