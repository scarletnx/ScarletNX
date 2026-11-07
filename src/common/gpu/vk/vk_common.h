/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COMMON_VK_COMMON_H
#define SNX_COMMON_VK_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <volk.h>
/* What the Vulkan frontends (the Switch's src/nx/gpu/vk, the 3DS's src/3ds/gpu/vk, the Vita's src/vita/gpu) share of
   setting up a device: the instance, the extension and layer queries, the choice of a physical device and queue
   family, memory types, pNext chains and the names of results. Each frontend keeps its own feature lists, queues and
   resources. The calls go through volk, whose loader the frontend initialized. */

/* VkResult's name ("VkResult" for one it doesn't name) */
const char* snx_vk_result_str(VkResult r);

/* the instance has the layer, or the extension (of the loader or its implicit layers) */
bool snx_vk_has_layer(const char* name);
bool snx_vk_has_instance_extension(const char* name);

/* An instance of Vulkan 1.3, or of api_version when it is given, volk loaded for it. extensions are the window's
   (SDL's or a backend's). validation adds VK_LAYER_KHRONOS_validation with VK_EXT_debug_utils when it is installed,
   and a messenger of warnings and errors to debug when that is given, and gpu_assisted asks it for GPU-assisted
   validation too; portability enumerates portability drivers (MoltenVK) when the loader can */
typedef struct {
    const char* app_name;
    const char* const* extensions;
    uint32_t extension_count;
    uint32_t api_version; /* the most Vulkan version the application uses, 0 for 1.3 */
    bool validation, gpu_assisted, portability;
    PFN_vkDebugUtilsMessengerCallbackEXT debug;
} SnxVkInstanceInfo;
typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    bool validation;  /* the layer is on: false when it was asked for and is not installed */
    bool portability; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
} SnxVkInstance;
VkResult snx_vk_instance_create(const SnxVkInstanceInfo* info, SnxVkInstance* out);
void snx_vk_instance_free(SnxVkInstance* instance);

/* a physical device's extensions, queried once and asked for by name */
typedef struct {
    VkExtensionProperties* list;
    uint32_t count;
} SnxVkExtensions;
bool snx_vk_device_extensions(VkPhysicalDevice phys, SnxVkExtensions* out);
bool snx_vk_extensions_has(const SnxVkExtensions* extensions, const char* name);
void snx_vk_extensions_free(SnxVkExtensions* extensions);

/* a physical device's queue families, *count of them (calloc'd, NULL when it has none or memory ran out) */
VkQueueFamilyProperties* snx_vk_queue_families(VkPhysicalDevice phys, uint32_t* count);
/* the first queue family from first on with every flag of flags that presents to surface (any, when surface is
   VK_NULL_HANDLE); UINT32_MAX for none */
uint32_t snx_vk_queue_family(VkPhysicalDevice phys, VkQueueFlags flags, VkSurfaceKHR surface, uint32_t first);

/* The first device, in the loader's order, of api_version or newer whose queue family of flags (presenting to
   surface, as snx_vk_queue_family) make accepts: make creates the device and says whether it did. False when none */
typedef bool (*SnxVkMake)(VkPhysicalDevice phys, uint32_t family, void* user);
bool snx_vk_first_device(VkInstance instance, uint32_t api_version, VkQueueFlags flags, VkSurfaceKHR surface,
                         SnxVkMake make, void* user);
/* The device of api_version or newer that score rates highest, the first of equal ones; a negative score rejects it.
   VK_NULL_HANDLE when none is left */
typedef int (*SnxVkScore)(VkPhysicalDevice phys, const VkPhysicalDeviceProperties* properties, void* user);
VkPhysicalDevice snx_vk_best_device(VkInstance instance, uint32_t api_version, SnxVkScore score, void* user);

/* A device with one queue of family: features is the head of its VkPhysicalDeviceFeatures2 chain */
VkResult snx_vk_device_create(VkPhysicalDevice phys, uint32_t family, const void* features,
                              const char* const* extensions, uint32_t extension_count, VkDevice* out);

/* Links the structures of links that are not NULL (each a Vulkan structure with sType and pNext) into one pNext chain
   in their order, the last one ending it. The first, NULL when there is none */
void* snx_vk_chain(void* const* links, size_t count);

/* a memory type of the bits with the properties, UINT32_MAX for none */
uint32_t snx_vk_memory_type(const VkPhysicalDeviceMemoryProperties* memory, uint32_t bits,
                            VkMemoryPropertyFlags properties);
/* Where a device's memory is, which decides whether the GPU reads and writes guest memory in place or copies it:
   - UMA, the system's memory: an integrated GPU, a CPU's, or a GPU neither discrete nor integrated whose biggest
     device-local heap the CPU maps and holds most of the memory;
   - REBAR, a discrete GPU's own, which the CPU maps all of (resizable BAR): device-local, host-visible memory is at
     least half of its biggest device-local heap;
   - SMALL_BAR, a discrete GPU's own, which the CPU maps a small window of (256 MiB without resizable BAR) */
typedef enum { SNX_VK_SMALL_BAR, SNX_VK_REBAR, SNX_VK_UMA } SnxVkMemoryModel;
SnxVkMemoryModel snx_vk_memory_model(VkPhysicalDeviceType type, const VkPhysicalDeviceMemoryProperties* memory);

/* The threads that make new pipelines (pipeline_threads): setting when it is one or more, else all of cpus but two,
   two to twelve */
int snx_vk_pipeline_workers(int setting, int cpus);
/* Of that many pipeline workers, the ones past the first two, which run at the system's low priority
   (snx_thread_priority_low) */
int snx_vk_pipeline_low_workers(int workers);

/* a shader module of size bytes of SPIR-V */
VkResult snx_vk_shader_module(VkDevice dev, const uint32_t* words, size_t size, VkShaderModule* out);
/* a compute pipeline of module's "main" in layout, spec (NULL for none) giving its specialization constants */
VkResult snx_vk_module_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, VkShaderModule module,
                                const VkSpecializationInfo* spec, VkPipeline* out);
/* a compute pipeline of size bytes of SPIR-V's "main" in layout, through a module that it frees again */
VkResult snx_vk_compute_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, const uint32_t* words,
                                 size_t size, VkPipeline* out);
/* the same with subgroups of subgroup lanes (0: the driver's choice), and every one of them whole when full: a program
   whose lanes are a warp's needs 32 of them in one subgroup. The device must have subgroupSizeControl, subgroup within
   its sizes for compute, and computeFullSubgroups for full */
VkResult snx_vk_compute_pipeline_subgroups(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout,
                                           const uint32_t* words, size_t size, uint32_t subgroup, bool full,
                                           VkPipeline* out);
#endif
