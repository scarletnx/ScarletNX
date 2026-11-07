/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_GLES_VK_H
#define SNX_ANDROID_GLES_VK_H
/* What GLES 1.1 (hle_gles.c) and GLES 2 and 3 (gles2*.c) share over Vulkan (gles_vk.c): one instance, device and
   queue, the window surfaces with their swapchains, and the frames that the calls record (docs/android/NATIVE.md,
   "EGL and GLES").

   A surface records one frame at a time into the command buffer of a slot. Two slots are in flight. The image of a
   window surface is acquired at the first draw of the frame. Between passes it is in PRESENT_SRC_KHR, so GLES 1.1's
   render passes and GLES 2's dynamic rendering go into one frame. A pbuffer surface has an image of its own, which
   rests in COLOR_ATTACHMENT_OPTIMAL.

   Each submission has a serial, and an object that commands use is destroyed only after its serial completed. The
   queue signals in submission order, so the serial of the last fence that signaled is the completed one */
#include "android/bridge/hle_gles.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

/* the Vulkan commands of the library's instance and device */
#define GLES_INSTANCE_COMMANDS(X)                                                                                      \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkGetPhysicalDeviceFormatProperties)                                                                             \
    X(vkGetPhysicalDeviceImageFormatProperties)                                                                        \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetDeviceProcAddr)
#define GLES_DEVICE_COMMANDS(X)                                                                                        \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkFreeCommandBuffers)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkResetCommandBuffer)                                                                                            \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkGetFenceStatus)                                                                                                \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkQueueSubmit)                                                                                                   \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkCreateBufferView)                                                                                              \
    X(vkDestroyBufferView)                                                                                             \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkBindBufferMemory)                                                                                              \
    X(vkMapMemory)                                                                                                     \
    X(vkInvalidateMappedMemoryRanges)                                                                                  \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkResetDescriptorPool)                                                                                           \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreateRenderPass)                                                                                              \
    X(vkDestroyRenderPass)                                                                                             \
    X(vkCreateFramebuffer)                                                                                             \
    X(vkDestroyFramebuffer)                                                                                            \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkCreateComputePipelines)                                                                                        \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreateQueryPool)                                                                                               \
    X(vkDestroyQueryPool)                                                                                              \
    X(vkGetQueryPoolResults)                                                                                           \
    X(vkCmdBeginRenderPass)                                                                                            \
    X(vkCmdEndRenderPass)                                                                                              \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdSetViewport)                                                                                                \
    X(vkCmdSetScissor)                                                                                                 \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindIndexBuffer)                                                                                            \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdDrawIndexed)                                                                                                \
    X(vkCmdDispatch)                                                                                                   \
    X(vkCmdDispatchIndirect)                                                                                           \
    X(vkCmdDrawIndirect)                                                                                               \
    X(vkCmdDrawIndexedIndirect)                                                                                        \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyBuffer)                                                                                                 \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkCmdCopyImage)                                                                                                  \
    X(vkCmdBlitImage)                                                                                                  \
    X(vkCmdResolveImage)                                                                                               \
    X(vkCmdFillBuffer)                                                                                                 \
    X(vkCmdClearColorImage)                                                                                            \
    X(vkCmdClearDepthStencilImage)                                                                                     \
    X(vkCmdClearAttachments)                                                                                           \
    X(vkCmdResetQueryPool)                                                                                             \
    X(vkCmdBeginQuery)                                                                                                 \
    X(vkCmdEndQuery)                                                                                                   \
    X(vkCmdCopyQueryPoolResults)
/* Vulkan 1.3's, which GLES 2 and 3 take: dynamic rendering and the dynamic state of extended_dynamic_state 1 and 2 */
#define GLES_DEVICE13_COMMANDS(X)                                                                                      \
    X(vkCmdBeginRendering)                                                                                             \
    X(vkCmdEndRendering)                                                                                               \
    X(vkCmdSetCullMode)                                                                                                \
    X(vkCmdSetFrontFace)                                                                                               \
    X(vkCmdSetPrimitiveTopology)                                                                                       \
    X(vkCmdSetDepthTestEnable)                                                                                         \
    X(vkCmdSetDepthWriteEnable)                                                                                        \
    X(vkCmdSetDepthCompareOp)                                                                                          \
    X(vkCmdSetStencilTestEnable)                                                                                       \
    X(vkCmdSetStencilOp)                                                                                               \
    X(vkCmdSetStencilCompareMask)                                                                                      \
    X(vkCmdSetStencilWriteMask)                                                                                        \
    X(vkCmdSetStencilReference)                                                                                        \
    X(vkCmdSetDepthBiasEnable)                                                                                         \
    X(vkCmdSetDepthBias)                                                                                               \
    X(vkCmdSetLineWidth)                                                                                               \
    X(vkCmdSetBlendConstants)                                                                                          \
    X(vkCmdSetPrimitiveRestartEnable)                                                                                  \
    X(vkCmdSetRasterizerDiscardEnable)                                                                                 \
    X(vkCmdBindVertexBuffers2)

enum { GLES_SLOTS = 2, GLES_MAX_IMAGES = 8, GLES_MAX_SETS = 8192 };

/* a host-visible buffer of a frame, used from its start */
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* map;
    VkDeviceSize size, used;
} GlesArena;

/* what a frame used, destroyed once its fence is signaled */
typedef struct {
    VkImage image;
    VkImageView view;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBufferView buffer_view;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
} GlesGarbage;

typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkSemaphore acquire;
    VkDescriptorPool pool;
    /* GLES 2's descriptor pools: pools[0, pool_used) gave sets in this frame. They grow when one is full */
    VkDescriptorPool* pools;
    uint32_t pool_count, pool_used;
    GlesArena vertices, staging;
    GlesGarbage* garbage;
    size_t garbage_count, garbage_capacity;
    uint64_t serial; /* the submission of the frame's commands */
} GlesSlot;

/* what is open in the frame's command buffer */
enum { GLES_OPEN_NONE = 0, GLES_OPEN_PASS = 1, GLES_OPEN_RENDERING = 2 };

struct GlesSurface {
    int width, height; /* the size EGL gave, for a surface that leaves the size to the swapchain */
    bool pbuffer;      /* EGL's pbuffer: images[0] is the surface's own, in own_memory, and nothing presents */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkImage images[GLES_MAX_IMAGES];
    VkImageView views[GLES_MAX_IMAGES];
    VkFramebuffer framebuffers[GLES_MAX_IMAGES];
    VkSemaphore rendered[GLES_MAX_IMAGES];
    VkDeviceMemory own_memory;
    uint32_t image_count;
    uint32_t generation; /* counts the swapchains made: GLES 2's targets made for an older one are stale */
    bool stale;          /* the swapchain is made again before the next frame */
    /* the depth and stencil buffer that GLES 2 draws with, made at its first use: the bits the config asks for */
    int depth_bits, stencil_bits;
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFormat depth_format;
    bool depth_ready; /* in DEPTH_STENCIL_ATTACHMENT_OPTIMAL; else UNDEFINED */
    bool own_ready;   /* a pbuffer's image is in COLOR_ATTACHMENT_OPTIMAL; else UNDEFINED */
    GlesSlot slots[GLES_SLOTS];
    unsigned slot;
    /* the frame being recorded */
    bool recording, acquired, drew;
    bool waited;   /* a submission of this frame waited for the acquire semaphore already */
    bool touched;  /* the acquired image was written: it is in PRESENT_SRC_KHR between passes, else UNDEFINED */
    uint8_t open;  /* GLES_OPEN_* */
    uint32_t image;
};

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} GlesImage;

#define GLES_DECLARE(name) PFN_##name name;
typedef struct {
    pthread_mutex_t lock; /* every call of GLES 1.1 and 2 that reaches Vulkan, and the surfaces */
    bool tried, up;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features; /* the ones enabled */
    uint32_t max_texture;
    /* GLES 2 and 3: a Vulkan 1.3 device; and the extensions they use when the device has them */
    bool v13, depth_clip_control, index_uint8;
    struct {
        GLES_INSTANCE_COMMANDS(GLES_DECLARE)
        GLES_DEVICE_COMMANDS(GLES_DECLARE)
        GLES_DEVICE13_COMMANDS(GLES_DECLARE)
    } vk;
    VkCommandPool commands;
    /* the render passes of GLES 1.1 on the surfaces' format: one that clears the image first, one that keeps it */
    VkFormat format;
    VkRenderPass pass_clear, pass_load;
    /* the serials: of the last submission, and of the last one known to be complete */
    uint64_t submitted, completed;
    /* what an object no slot owns left: destroyed once its serial completed */
    struct {
        GlesGarbage item;
        uint64_t serial;
    }* retired;
    size_t retired_count, retired_capacity;
    VkFence flush_fence; /* a frame's submission before its end, which the call waits for */
} GlesVk;
#undef GLES_DECLARE
extern GlesVk g_gles_vk;

/* the instance, a surface of the host window (*surface), the device and queue that present to it: once, at the first
   window surface. Then only a new surface. False when there is no Vulkan device */
bool gles_vk_device(VkSurfaceKHR* surface);
/* the device and instance gone, and every object made on them that the library still keeps: after the last GLES 1.1 and
   2 surface and context (gles2_shutdown). The next surface brings them up again */
void gles_vk_shutdown(void);
uint32_t gles_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);
VkShaderModule gles_vk_shader(const uint32_t* words, size_t bytes);
/* an image and its memory (device-local where the device has it), and a view of the whole when view_type is not
   VK_IMAGE_VIEW_TYPE_MAX_ENUM. False, and nothing made, when Vulkan refuses */
bool gles_vk_image(const VkImageCreateInfo* info, VkImageViewType view_type, VkImageAspectFlags aspect,
                   GlesImage* out);
void gles_vk_destroy(GlesGarbage item);
/* item for the slot's frame to destroy once it is done */
void gles_vk_garbage(GlesSlot* slot, GlesGarbage item);
/* item that commands recorded so far may use: destroyed once they completed */
void gles_vk_retire(GlesGarbage item);
/* size bytes of the slot's arena at an offset aligned to align (a power of 2): their offset, or VK_WHOLE_SIZE. A full
   arena goes to the slot's garbage (this frame's commands read it) and a larger one replaces it */
VkDeviceSize gles_vk_arena_take(GlesSlot* slot, GlesArena* a, VkDeviceSize size, VkDeviceSize align,
                                VkDeviceSize least, VkBufferUsageFlags usage);
/* a descriptor set of layout from the slot's pools, which grow; VK_NULL_HANDLE when Vulkan gives none */
VkDescriptorSet gles_vk_set(GlesSlot* slot, VkDescriptorSetLayout layout);
/* waits until the submission of serial completed */
void gles_vk_wait(uint64_t serial);

/* gles_pbuffer_surface with the lock held */
GlesSurface* gles_pbuffer_make(int width, int height);
/* the frame's command buffer open: its slot's last use finished, and what that use left destroyed */
bool gles_frame_begin(GlesSurface* s);
/* the image of the frame acquired (a window surface's), with s->image its index */
bool gles_acquire(GlesSurface* s);
/* ends what is open in the frame's command buffer: GLES 1.1's render pass or GLES 2's rendering */
void gles_pass_end(GlesSurface* s);
/* GLES 1.1's render pass open on the frame's image: the image acquired at the first, and cleared by it */
bool gles_pass_begin(GlesSurface* s);
/* the commands recorded so far submitted, and waited for; the frame goes on in the same command buffer */
bool gles_flush(GlesSurface* s);
/* a pbuffer's frame ended: its commands submitted, the next frame in the next slot */
void gles_frame_end(GlesSurface* s);
/* the frame of serial pending: the serial the next submission has */
static inline uint64_t gles_vk_pending(void) {
    return g_gles_vk.submitted + 1;
}

void gles_image_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                        VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
                        VkPipelineStageFlags src, VkPipelineStageFlags dst);
#endif
