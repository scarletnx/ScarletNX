/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: shared state and the interfaces between its parts. Functions are loaded through volk from the
   loader the window backend gives (SDL's), or from libvulkan itself in headless runs (vk_init), so nothing links
   against a Vulkan library.

   device.c    instance, device, queues, swapchain, memory allocator (VMA)
   memory.c    the cap on one allocation, and images bigger than it bound in pieces (sparse binding); the device
               memory budget the caches keep within (surface.c vk_mem_trim), and the present clock
   cmd.c       frames in flight: the command buffer being recorded, staging and readback rings, deferred destruction
   record.c    recording the frame's command buffer on a thread of its own
   format.c    guest render target and texture formats -> Vulkan formats
   surface.c   render targets by guest address (a table textures share), loads from and write-backs to guest memory
   writeback.c landing compute and graphics storage results in guest memory
   texture.c   textures from the guest's texture headers (TIC), views and samplers (TSC)
   astc.c      compute decoding of ASTC texture blocks during upload
   buffer.c    vertex, index, constant and storage buffers from guest memory
   pipeline.c  translated shaders, the pipeline layout and pipelines by state
   pipeline_file.c  the shaders and pipelines kept across runs, made again at startup (pipeline_file.h), and the
                    shaders of the programs the game ships (<game>.vkshipped, translated by the pass over its files
                    in gpu/shipped_pass.c), read as the draws ask for them
   render.c    the render target set (dynamic rendering), clears and draws
   compute.c   compute dispatches (on a queue of their own when there is one), landing their results in guest memory
   renderer.c  the renderer interface the GPU front end calls (gpu/renderer.h) and presenting
   renderdoc.c RenderDoc captures of chosen frames (renderdoc)
   ui.c        the on-screen controls and the game's menu, drawn over the swapchain image
   vma.cpp     VulkanMemoryAllocator's implementation

   Everything that records commands runs under vk_lock(), which is reentrant: submits and presents come from the GPU
   thread (gpu/gpu.c, hle/services/vi.c's present_queued), the menu's refresh from the main thread, and compute results
   land on whichever thread reads their memory first. */
#ifndef SNX_VK_H
#define SNX_VK_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/draw_state.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "volk.h"
#include "vk_mem_alloc.h"
#include "nx/gpu/shader/shader.h"

#define VK_FRAMES 2  /* frames in flight */
#define VK_SC_MAX 16 /* swapchain images */

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
	u64 shader_generation; /* shader instruction invalidations, separate from memory epochs */
    VkDevice dev;
    VmaAllocator vma;
    /* compute is a second queue of the graphics family, or of a compute-only family, when the device has one; else
       it is the graphics queue itself */
    u32 gfx_family, compute_family;
    VkQueue gfx, compute;
    u32 gfx_queues; /* the queues made of the graphics family (2 when compute is the second) */
    /* Vulkan Video's H.264 decode queue, of a family of its own (~0u without one), made when the vulkan_video setting
       asks for it (experimental): FFmpeg's Vulkan decoder decodes on it (vk/nvdec.c), told of the device by the
       extensions and features enabled */
    u32 video_family;
    const char* dev_ext[40];
    u32 dev_ext_n;
    VkPhysicalDeviceFeatures2 feat2; /* feat, with the 1.1 to 1.3 features enabled chained */
    VkPhysicalDeviceVulkan11Features feat11;
    VkPhysicalDeviceVulkan12Features feat12;
    VkPhysicalDeviceVulkan13Features feat13;
    /* swapchain */
    VkSwapchainKHR swapchain;
    VkExtent2D sc_extent;
    int sc_win_w, sc_win_h; /* the window size it was made for (sc_extent: that as the surface takes it) */
    int sc_srgb;            /* its format is sRGB: what presented frames in sRGB formats are blitted to */
    VkFormat sc_format;     /* its images' format, which the on-screen controls draw to (gpu/vk/ui.c) */
    int sc_vsync;  /* the vsync setting it was made for (1: FIFO; 0 is FIFO too on a surface with nothing else) */
    int sc_turned; /* upright on a display held another way: Android calls each present suboptimal for it */
    int sc_lost;   /* the last create found the surface lost: show() has it made anew */
    int sc_clock;  /* how display.c learns when its presents are displayed (VkClock) */
    /* its presents can be held until a time (display.c's targets, CLOCK_MONOTONIC): with VK_CLOCK_WAIT2 in the time
       domain sc_time_domain, the present stage sc_time_stage's own clock (0 for CLOCK_MONOTONIC itself) */
    int sc_hold;
    u64 sc_time_domain;
    u32 sc_time_stage;
    u32 sc_count;
    /* taken around the swapchain's externally synchronized calls, on the GPU thread (its acquire, create and destroy,
       under vk_lock), the recording thread (its presents, record.c) and display.c's (its present waits) */
    PMutex sc_lock;
    VkImage sc_images[VK_SC_MAX];
    VkSemaphore sc_done[VK_SC_MAX]; /* per swapchain image: rendering into it finished, it can be presented */
    /* optional features that were enabled */
    VkPhysicalDeviceFeatures feat;
    int has_divisor;      /* VK_EXT_vertex_attribute_divisor: instance divisors above 1 */
    int has_dynamic_stride; /* a binding's stride is dynamic state, which MoltenVK refuses: a pipeline holds it then */
    int has_vertex_robustness2; /* exact zero/default components for out-of-range mesh fetches */
    int has_calibrated_host; /* VK_EXT_calibrated_timestamps with CLOCK_MONOTONIC_RAW: the GPU's clock goes next to
                                the client's, one driver call (gpuprof.c's calibration) */
    int has_xfb;          /* VK_EXT_transform_feedback: draws capture their outputs into guest memory */
    int has_border_color; /* VK_EXT_custom_border_color */
    u32 border_color_max; /* maxCustomBorderColorSamplers: past it, samplers take the nearest built-in color */
    int has_mirror_clamp; /* samplerMirrorClampToEdge: VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE */
    /* filterMinmaxImageComponentMapping: a min/max sampler reads a view of any swizzle. Without it the view's r must
       be the identity, and the other components are poison */
    int has_minmax_swizzle;
    int has_shader_draw_parameters; /* shaders take an instance's ID from BaseInstance (spirv_translate.c) */
    int has_draw_indirect_count;    /* drawIndirectCount: an indirect draw's count from the GPU's memory */
    u64 alloc_cap; /* the most one device memory allocation may be (memory.c) */
    int sparse;    /* sparseBinding on the graphics queue: images bigger than alloc_cap are bound in pieces */
    /* VK_EXT_attachment_feedback_loop_layout: a draw samples a color target it renders to in place (gpu/vk/render.c),
       with the loop enabled per draw by dynamic state (VK_EXT_attachment_feedback_loop_dynamic_state) or else by a
       pipeline flag, and ordered within the pass by a barrier there (dynamicRenderingLocalRead) or else by beginning
       the pass again */
    int has_feedback, has_feedback_dynamic, has_local_read;
    /* VK_EXT_conditional_rendering: a render enable comparing a samples-passed count not known yet with zero
       predicates its draws on the GPU (gpu/vk/query.c) instead of waiting for the count */
    int has_cond_render;
    /* VK_EXT_provoking_vertex's provokingVertexLast: flat inputs take a primitive's last vertex, as the 3D engine's
       PROVOKING_VERTEX (0x5A1) can ask */
    int has_provoking_last;
    /* VK_EXT_conservative_rasterization: conservative rasterization (CONSERVATIVE_RASTER, 0x452), dilated by at most
       conservative_extra pixels (maxExtraPrimitiveOverestimationSize) */
    int has_conservative;
    float conservative_extra;
    /* VK_KHR_index_type_uint8 (or the EXT before it): 8-bit index buffers bound as they are */
    int has_index8;
    /* VK_KHR_present_id and VK_KHR_present_wait: presents carry ids, and display.c learns when each is displayed */
    int has_present_wait;
    /* VK_KHR_present_id2, VK_KHR_present_wait2 and VK_EXT_present_timing's presentAtAbsoluteTime (with
       VK_KHR_get_surface_capabilities2): a present can be held until a time, on a surface that says so (sc_hold) */
    int has_present_timing;
    int has_calibrated_mono; /* CLOCK_MONOTONIC among the calibrateable time domains */
    int has_surface_caps2;   /* the instance's VK_KHR_get_surface_capabilities2, which present timing is asked by */
    /* VK_GOOGLE_display_timing, enabled without present waits (Android): presents carry ids and times to be shown at,
       and the swapchain tells when each was */
    int has_display_timing;
} VkState;
extern VkState g_vk;

/* the swapchain's display clock (sc_clock): none; present ids and waits; their second versions, which a swapchain made
   for VK_EXT_present_timing takes; VK_GOOGLE_display_timing's past presentation timings */
typedef enum { VK_CLOCK_NONE, VK_CLOCK_WAIT, VK_CLOCK_WAIT2, VK_CLOCK_GOOGLE } VkClock;

/* device.c */
int vk_init(void);
/* (re)creates the swapchain at the window's size, in an sRGB format when srgb is set and the surface has one, with
   the present mode the vsync setting asks for, recorded in sc_vsync (show() makes a new one when the setting moves).
   0 while the window has no area (minimized) or its surface can't be queried, the old swapchain staying, or when the
   create failed: then there is none. A lost surface sets sc_lost; the next present tries again */
int vk_swapchain_create(int srgb);
int vk_surface_recreate(void);
int vk_window_release(void); /* Renderer.release_window */
/* logs a failed call with where it happened; evaluates to 1 on success */
#define VK_OK(call) vk_ok_((call), #call, __FILE__, __LINE__)
int vk_ok_(VkResult r, const char* what, const char* file, int line);
/* messages the validation layer's debug messenger logged (0 without vk_validation): a test that enables the layer
   holds a path to zero more of them */
extern u32 g_vk_validation_messages;
/* The renderer's lock, reentrant on the thread that holds it. The outermost vk_unlock, once the lock is let go,
   signals the event slots this thread's syncpoint releases under it reached (vk_release_events): that takes nvdrv's
   nvev_lk and the kernel's lock, so a hold that performs releases is never let go under either */
void vk_lock(void);
void vk_unlock(void);
int vk_trylock(void); /* vk_lock unless another thread holds it: 1 when taken */
u32 vk_lock_owner(void); /* the thread (plat_tid) holding vk_lock, 0 when none */

/* memory.c. vk_mem_init once the device exists, before the allocator (VMA's blocks are kept within the cap):
   sparse_queue is whether the graphics queue's family can bind sparse memory */
void vk_mem_init(int sparse_queue);
/* an image with its memory: one VMA allocation in mem, or, past g_vk.alloc_cap, pieces (mem stays VK_NULL_HANDLE).
   0 on failure. Destroyed with vk_image_destroy (vk_defer_image does that), never vmaDestroyImage */
int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem);
/* the same, with the result of the call that failed in res (VK_ERROR_OUT_OF_DEVICE_MEMORY: no memory for it) */
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, VkResult* res);
void vk_image_destroy(VkImage img, VmaAllocation mem);
/* Tracy memory events for VMA allocations. */
#if defined(SNX_TRACY)
void vk_mem_track(VmaAllocation a);
void vk_mem_untrack(VmaAllocation a);
#else
static inline void vk_mem_track(VmaAllocation a) { (void)a; }
static inline void vk_mem_untrack(VmaAllocation a) { (void)a; }
#endif
/* The caches of textures, render targets and buffers are kept within the driver's budget for the heap images go to
   (VK_EXT_memory_budget through VMA; vk_budget_mb lowers it, for testing): past it the driver pages memory out,
   which took 160 ms frames and then the device in one game. Ages are kept in presents, and measured in time as well
   as presents, the frame rate being anything from 5 to 144 a second.
   vk_mem_use: that heap's use as the caches count it, and its budget. vk_mem_present: at each present, after
   g_vk_present moved. vk_present_before: the present before which an entry last used then has gone unused for at
   least ago_ns and at least `presents` presents (0: none has) */
void vk_mem_use(u64* usage, u64* budget);
int vk_mem_heap(void); /* that heap's index */
void vk_mem_present(void);
u32 vk_present_before(u64 ago_ns, u32 presents);

/* cmd.c */
void vk_cmd_init(void);
VkCommandBuffer vk_cmd(void); /* the current frame's command buffer, begun on first use */
/* size bytes of the current frame's mapped staging memory (flushed when the frame is submitted), valid until
   that frame's work is done. NULL past g_vk.alloc_cap: a big upload stages in parts */
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off);
/* Streamed buffer snapshots in host RAM, with the same frame lifetime as vk_stage. */
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off);
/* GPU-written storage, device-local when host-visible memory and its budget allow;
   includes the allocation for invalidation after its frame fence. */
u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, VmaAllocation* mem);
/* Host-cached memory for the current frame's GPU -> host copies, read after its fence once invalidated. */
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, VmaAllocation* mem);
/* Device memory for the current frame's data that the GPU writes and reads itself, with the same lifetime as vk_stage:
   0 when none could be made */
int vk_stage_device(u64 size, u64 align, VkBuffer* buf, u64* off);
/* logs what each ring handed out since the last report, by where its memory is (vk_buffer_stats) */
void vk_stage_report(void);
/* ends and submits the commands recorded since the last submit, waiting for wait (at wait_stage) and signaling signal.
   The frame goes on: its staging, pools and deferred resources live until vk_frame_next and its slot's next use.
   Either semaphore may be VK_NULL_HANDLE */
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal);
void vk_submit_for_readback(void);
/* the next submission waits for a timeline semaphore of someone else's (FFmpeg's picture's) at one value and signals
   it at another: whichever submit comes next takes them, so commands recorded from now on never run before the wait */
void vk_submit_after(VkSemaphore sem, u64 wait, u64 signal);
void vk_finish(void); /* submits and waits until the GPU has done everything, and lands every result */
/* a save state's load (Renderer.reset): every surface, buffer and sample counter kept of the old guest memory goes */
void vk_surf_forget_all(void);
void vk_buf_forget_all(void);
void vk_query_forget_all(void);
u32 vk_rt_flush_all(void); /* a save: every render target's pixels written back where it can be, the count */
/* a save state's depth targets (surface.c): every live one's pixels into io, read back (0, or -1); a load's, kept for
   the targets the loaded game makes of the same place, format and size; and those dropped */
struct StateIo;
int vk_zt_save(struct StateIo* io);
int vk_zt_load(struct StateIo* io);
void vk_zt_forget(void);
extern u64 g_vk_finishes, g_vk_finish_ns; /* statistics: vk_finish calls and their wait (the GPU thread idles) */
void vk_frame_next(void);                 /* ends the frame (a present): the next records in the other slot */
void vk_frame_check(void);                /* at the start of a draw or dispatch: ends a frame that has grown too big */
/* a buffer for GPU -> host copies: record the copy into it, vk_finish(), vk_readback_ready(), then read */
u8* vk_readback_buffer(u64 size, VkBuffer* buf);
void vk_readback_ready(void);
/* destroyed once the frame being recorded now is done (the GPU may still use them until then) */
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2);
void vk_defer_buffer(VkBuffer b, VmaAllocation a);
void vk_defer_buffer_view(VkBufferView v);
/* bytes of device memory the resources just deferred release, and all deferred releases still to come: an evicted
   entry's memory counts as gone from when it is deferred (vk_mem_trim) */
void vk_defer_bytes(u64 bytes);
u64 vk_deferred_bytes(void);
void vk_defer_call(void (*fn)(void*), void* arg); /* fn(arg) then */
/* the submission being recorded (a number that grows at every submit: what a submit may have changed, results landed,
   is checked against it), and the frame (grows at vk_frame_next: frame f's work is done once f + VK_FRAMES started) */
u64 vk_submit_number(void);
/* iOS takes the GPU from an app in the background: a submit then loses the device ("Insufficient Permission (to submit
   GPU work from background)"). Holds the caller until the app is shown again, or the game is leaving. Returns at once
   everywhere else */
void vk_wait_foreground(void);
u32 vk_cb_begun(void); /* command buffers begun so far: changes when a new one starts recording */
u64 vk_frame_id(void);
/* the submission being recorded reads buffers in frame frame_id's rings: that frame's slot isn't reused until it is
   done. Only the frame before the current one can be held: older slots were reused already */
void vk_frame_hold(u64 frame_id);
u64 vk_last_submit(void);        /* the last submission's number, which its completion signals */
void vk_submit_progress(u64* submitted, u64* done); /* the last submission and the last done (any thread) */
void vk_wait_submit(u64 value);  /* until the GPU has done that submission (any thread, without vk_lock) */
/* Flush recorded work without frame recycling or guest result callbacks, under
   vk_lock, before an ownership readback uses the graphics queue directly. */
void vk_submit_for_readback(void);
void vk_land_through(u64 value); /* the results of the submissions up to value, done, into guest memory (vk_lock) */
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to);
/* a descriptor set from the current frame's pools, freed when the frame is reused */
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout);
VkDescriptorPool vk_desc_pool_new(void); /* a pool for 1024 sets of the stage layouts */
u32 vk_frame_slot(void);                 /* the frame slot being recorded */
/* a descriptor pool's sizes (VK_POOL_TYPES entries) for `sets` sets of the stage layouts (gpu/vk/pipeline.c) */
#define VK_POOL_TYPES 5 /* descriptor types in a set: uniform, storage buffers, textures, images, texel buffers */
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets);
extern u32 g_vk_buffer_image_slots; /* storage texel buffer bindings in the compute set, from SNX_VKB_IMGBUF */
/* the staging alignment that suits every use: uniform, storage, vertex and index buffers */
u64 vk_stage_align(void);
u64 vk_hash(const void* p, u64 size);
u64 vk_hash_copy(void* dst, const void* src, u64 size); /* vk_hash(src, size), with src copied into dst on the way */
/* a hash of guest memory: all of it when small, else its ends and 62 samples between them */
u64 vk_sample_hash(u64 gva, u64 len);
/* guest submits processed so far, and frames presented: what the caches check memory against */
extern u32 g_vk_epoch, g_vk_present;

/* record.c: the frame's command buffer is recorded on a thread of its own. rec_Cmd* take vkCmd*'s arguments and
   record into that command buffer from the ring, or into any other one at once */
void vk_rec_init(void);
int vk_rec_threaded(void); /* the frame's commands go through the recording thread */
u64 vk_rec_behind(void); /* bytes of commands handed to the recording thread and not recorded yet (any thread) */
void vk_rec_begin(VkCommandBuffer cb); /* begins cb, the frame's command buffer from now on */
/* ends and submits cb with those semaphores (s[0] the graphics timeline's) */
void vk_rec_submit(VkCommandBuffer cb, const VkSemaphoreSubmitInfo* w, u32 nw, const VkSemaphoreSubmitInfo* s,
                   u32 ns);
void vk_rec_sync(void);        /* until everything queued is recorded and submitted */
void vk_rec_queue(VkQueue q);  /* before submitting to or presenting on q outside the ring (vk_lock) */
/* Around every call on the graphics or the compute queue: a submit, a sparse bind, a present, a device wait. Vulkan
   requires a queue's calls to be externally synchronized, and they come from more than one thread: the recording
   thread submits and presents the frames (record.c) without vk_lock, which the GPU thread holds while it binds an
   image's pieces (memory.c) or submits compute. A leaf lock: taken with vk_lock held or not, never the other way */
void vk_queue_lock(VkQueue q);
void vk_queue_unlock(VkQueue q);
/* presents image idx of sc once wait is signaled, in order after the submissions queued before it (vk_lock). The
   result is this present's without the recording thread, otherwise that of an earlier one which wasn't VK_SUCCESS,
   or VK_SUCCESS: a swapchain out of date shows a frame later */
/* id: the present's id (display.c's), 0 none; with sc_hold, target: not shown before then (CLOCK_MONOTONIC ns), 0 at
   once */
VkResult vk_rec_present(VkSwapchainKHR sc, u32 idx, VkSemaphore wait, u64 id, u64 target);
/* takes the queued presents' result that wasn't VK_SUCCESS, if any (after a drain: a new swapchain drops the old's) */
VkResult vk_rec_present_result(void);
/* a new descriptor set of layout with those writes, for commands of the frame's command buffer only: allocated and
   written on the recording thread, a stand-in handle until then (vk_lock) */
VkDescriptorSet vk_rec_new_set(VkDescriptorSetLayout layout, u32 n, const VkWriteDescriptorSet* w);
void vk_rec_frame_reset(u32 slot); /* the slot's new sets are done with: its pools are reset in order */
u32 vk_rec_frame_sets(u32 slot);   /* new sets made in the slot since its reset */
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe);
void rec_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type);
void rec_CmdDraw(VkCommandBuffer cb, u32 count, u32 instances, u32 first, u32 first_instance);
void rec_CmdDrawIndexed(VkCommandBuffer cb, u32 count, u32 instances, u32 first, i32 vertex_offset,
                        u32 first_instance);
/* max draws of the arguments at buf + off, stride bytes apart (vkCmdDraw[Indexed]Indirect), or with count_buf, as
   many as the 32-bit count at count_buf + count_off says (their Count forms) */
void rec_CmdDrawIndirect(VkCommandBuffer cb, int indexed, VkBuffer buf, VkDeviceSize off, VkBuffer count_buf,
                         VkDeviceSize count_off, u32 max, u32 stride);
void rec_CmdDispatch(VkCommandBuffer cb, u32 x, u32 y, u32 z);
void rec_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off);
void rec_CmdEndRendering(VkCommandBuffer cb);
void rec_CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags mode);
void rec_CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace face);
void rec_CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology topo);
void rec_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetRasterizerDiscardEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetLineWidth(VkCommandBuffer cb, float width);
void rec_CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope);
void rec_CmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp op);
void rec_CmdSetDepthBoundsTestEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthBounds(VkCommandBuffer cb, float lo, float hi);
void rec_CmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass,
                         VkStencilOp depth_fail, VkCompareOp op);
void rec_CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags face, u32 v);
void rec_CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags face, u32 v);
void rec_CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags face, u32 v);
void rec_CmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer cb, VkImageAspectFlags aspect);
void rec_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, u32 first, u32 count);
void rec_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, u32 query);
void rec_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, u32 query, VkQueryControlFlags flags);
void rec_CmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, u32 query);
/* vkCmdBeginConditionalRenderingEXT on the word at buf + off, flags its VkConditionalRenderingBeginInfoEXT's */
void rec_CmdBeginConditionalRenderingEXT(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                         VkConditionalRenderingFlagsEXT flags);
void rec_CmdEndConditionalRenderingEXT(VkCommandBuffer cb);
void rec_CmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool pool, u32 first, u32 count, VkBuffer dst,
                                 VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags flags);
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]);
/* data is host-order 32-bit words (every block the runtime pushes is), stored little-endian for the device */
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset, u32 size,
                          const void* data);
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first, u32 n,
                               const VkDescriptorSet* sets, u32 ndyn, const u32* dyn);
void rec_CmdBindVertexBuffers2(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs,
                               const VkDeviceSize* sizes, const VkDeviceSize* strides);
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs);
void rec_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs,
                                            const VkDeviceSize* offs, const VkDeviceSize* sizes);
void rec_CmdBeginTransformFeedbackEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* counters,
                                      const VkDeviceSize* offs);
void rec_CmdEndTransformFeedbackEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* counters,
                                    const VkDeviceSize* offs);
void rec_CmdSetViewportWithCount(VkCommandBuffer cb, u32 n, const VkViewport* v);
void rec_CmdSetViewport(VkCommandBuffer cb, u32 first, u32 n, const VkViewport* v);
void rec_CmdSetScissorWithCount(VkCommandBuffer cb, u32 n, const VkRect2D* r);
void rec_CmdSetScissor(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D* r);
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r);
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r);
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter);
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                         const VkImageResolve* r);
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r);
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, u32 n,
                              const VkBufferImageCopy* r);
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c, u32 n,
                            const VkImageSubresourceRange* r);
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* c,
                                   u32 n, const VkImageSubresourceRange* r);
void rec_CmdClearAttachments(VkCommandBuffer cb, u32 na, const VkClearAttachment* a, u32 nr, const VkClearRect* r);
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d);
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri);

/* format.c */
typedef struct {
    VkFormat vk;
    u8 bpp; /* bytes per pixel (per block for compressed formats) */
    u8 bw, bh;
    u8 depth, stencil, is_int;
    u8 raw;  /* guest memory holds exactly the Vulkan format's bytes: it can be copied both ways */
    u8 sint; /* signed integer (is_int) */
    u8 conv; /* textures: converted on upload (VKC_*), the Vulkan format is what it converts to */
    u8 bgra; /* stored B, G, R, A in memory: a texture header's swizzle swaps R and B */
} VkFmt;
enum {
    VKC_NONE,
    VKC_ASTC,
    VKC_RGBA4,
    VKC_RGB5A1,
    VKC_RGB32F,
    VKC_A5B5G5R1,
    VKC_D24S8,
    VKC_S8D24,
    VKC_D24X8,
    VKC_D32S8,
    /* ETC2 and EAC decoded where the GPU lacks textureCompressionETC2, in texconv's TC_ETC2_RGB to TC_EAC_RG_SNORM
       order */
    VKC_ETC2,
    VKC_ETC2_PTA,
    VKC_ETC2_RGBA,
    VKC_EAC_R,
    VKC_EAC_R_SNORM,
    VKC_EAC_RG,
    VKC_EAC_RG_SNORM,
    /* BC1 to BC7 decoded where the GPU lacks textureCompressionBC, in texconv's TC_BC1 to TC_BC7 order */
    VKC_BC1,
    VKC_BC2,
    VKC_BC3,
    VKC_BC4,
    VKC_BC4_SNORM,
    VKC_BC5,
    VKC_BC5_SNORM,
    VKC_BC6H_UF,
    VKC_BC6H_SF,
    VKC_BC7
};
VkFmt vk_rt_format(u32 fmt);
VkFmt vk_zt_format(u32 fmt);
VkFmt vk_tic_format(u32 w0, int srgb); /* bpp 0: not supported */
/* vk_tic_format for a 3D image: ASTC and ETC2 blocks decoded where the device can't make 3D images of them */
VkFmt vk_tic_format_3d(u32 w0, int srgb);
/* The device and optimal-tiling format both support writes without a shader format. */
int vk_format_storage_write(VkFormat f);
/* whether a sampler may take the least or greatest of the texels its linear filter reads of f (min/max reduction) */
int vk_format_minmax_filter(VkFormat f);
int vk_tic_reads_stencil(const u32* tic);
/* sRGB and linear views of the same bytes: the twin of a color format, or the format itself when it has none */
VkFormat vk_srgb_as(VkFormat f, int srgb);
typedef struct {
    VkImageFormatListCreateInfo list;
    VkFormat f[3];
} VkViewFormats;
/* an image of a color format that has an sRGB twin is viewed as both, and as raw (not VK_FORMAT_UNDEFINED, the
   unsigned format of its texels' bytes, vk_raw_format): sets MUTABLE_FORMAT and chains the list of them to ii (vf has
   to live until the image is created) */
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf, VkFormat raw);
/* the unsigned integer format whose texels are bpp bytes (1, 2, 4, 8 or 16): the same bytes as any color format of
   that size, which a view of it moves without converting them. VK_FORMAT_UNDEFINED for other sizes */
VkFormat vk_raw_format(u32 bpp);

/* gpu/vk/surface.c: a render target or a texture */
typedef struct {
    u64 key; /* view type | swizzle << 4 | format << 20 | stencil << 60 | single mip << 61 (vk_tex_view) */
    VkImageView v;
} SurfView;
typedef struct Surf {
    u64 gva;
    u32 w, h, layers, levels;
    u32 sw, sh;  /* the image's size: render targets are w x h times the resolution scale */
    u32 depth3d; /* 3D textures: depth (layers is 1) */
    u32 fkey;    /* render targets: guest format | 0x10000 for depth. Textures: TIC word 0's format bits (0x7FFFF), sRGB
                    in bit 31, raw (vk_tex_from_tic_raw) in bit 30 */
    VkFmt f;
    u32 bl, bh, bd, pitch; /* guest layout: block linear with block height 2^bh GOBs, or pitch linear */
    u64 layer_size;        /* bytes from one layer to the next in guest memory */
    u64 rt_size;           /* a render target's guest bytes, all layers (its shape is fixed when it is made) */
    int is_rt;
    int storage_capable;  /* image was created with VK_IMAGE_USAGE_STORAGE_BIT */
    int volume;           /* render target layers use 3D block-linear memory rather than array strides */
    u8 msaa;              /* DS_MSAA_: a multisampled image's sample layout. w and h count its pixels, the guest's
                             memory its samples (w << ds_msaa_log2_x(msaa) by h << ds_msaa_log2_y(msaa) texels) */
    u8 ms_raw;            /* multisampled: its samples move to and from guest memory (gpu/vk/msaa.c) */
    VkImageViewType type; /* what it was created as */
    VkImage img;
    VmaAllocation mem;
    VkImageView att;        /* level 0: the attachment */
    VkImageView att_linear; /* same storage without sRGB conversion */
    VkImageLayout layout;   /* the whole image is kept in one layout */
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 last_used;
    u64 touched; /* when it last went to the recent end of its recency list: ties of last_used in the lists' order */
    u32 wgen;       /* render targets: bumped when drawn to */
    u64 wseq;       /* when its image last changed, counted across surfaces: of two over the same bytes, the higher
                       holds them (vk_surf_wrote) */
    u64 discard_wseq; /* a render target discarded (vk_render_discard) at this wseq: its next pass loads it as
                         DONT_CARE while nothing wrote it since */
    u8 discarded;
    u32 drawn_at;   /* render targets: g_vk_present when last drawn to */
    u64 draw_wseq;  /* render targets: wseq at its last draw or clear (vk_rt_drawn), 0 before one */
    int in_drawn;   /* in surface.c's list of drawn targets, entered at drawn_from */
    int in_linear;  /* a pitch-linear color target in surface.c's list of those drawn since the last release */
    u64 drawn_from;
    u64 draw_gw;    /* guest-memory generation at the first draw this present or since a full replacement */
    u64 draw_hash;  /* fallback guest-memory hash at that baseline */
    u32 draw_wseen; /* CPU write-tracking clock at that baseline, zero after a reload */
    /* the clock when guest memory last held the image (a load, a write-back) or the image last replaced all of it (a
       full clear or draw): zero when unknown. A write-back keeps the CPU's pages written since (surface.c cpu_newer) */
    u32 mem_wseen;
    int draw_has_hash;
    int feeds_tex; /* a texture asked for its pixels (vk_rt_drawn_fresh): without write tracking, its baselines are hashed */
    /* render targets: the two textures last copied from it (gpu/vk/texture.c), with its wgen and the texture's upload
       generation then, so two textures over one target don't copy it again on each other's binds */
    struct {
        struct Surf* t;
        u32 wgen, tgen;
    } copied[2];
    u32 mem_wgen; /* render targets: wgen when guest memory last held the same (written back or loaded) */
    u64 gw;       /* gpu_mem_generation() when it last matched guest memory or its unchanged range was checked */
    /* textures: views by type and swizzle, and the change check (gpu/vk/texture.c) */
    SurfView sv[6];
    int nsv;
    u64 size, hash, shash;
    int upload_valid; /* textures: every subresource was uploaded (a failed upload is retried) */
    u32 checked, next_check, stable, tfull;
    u32 wseen;    /* write-tracking clock when it last matched memory */
    u32 ww_epoch; /* the write epoch (vk_ww_epoch) of its last check for CPU writes */
    /* copies of regions of this render target for texture headers or feedback draws, one per size, linked through next
       (gpu/vk/texture.c rt_crop) */
    struct Surf* crop;
    u32 crop_gen;
    struct Surf* next;
    /* the recency list it is in (surface.c): textures, render targets (parked ones too), or none (copies, the CPU
       frame), with the device memory its image takes. A list runs from the least recently used to the most */
    int lru;
    struct Surf *lru_prev, *lru_next;
    u64 bytes;
    int took_drawn; /* textures: pixels copied from render targets since the last upload (memory may not have them) */
    /* depth targets: undefined but for draws and clears predicated on the GPU, which may not have run, so they count
       as no draw (render.c zt_written) */
    int pred_undefined;
} Surf;
enum { VK_LRU_NONE, VK_LRU_TEX, VK_LRU_RT };
/* resolution= in the .ini: render targets are made this much bigger (read once, at startup) */
extern float g_vk_scale;
u32 vk_sc(u32 v);      /* a size or coordinate scaled, at least 1 */
float vk_scf(float v); /* the same for viewport values */
/* w in pixels; pitch: bytes per row of a pitch-linear target (the register holds it in place of the width) */
Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch);
/* the array and depth targets: w and h in pixels, of the sample layout msaa (DS_MSAA_) */
Surf* vk_rt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 pitch, u32 layers, u64 layer_size, int volume,
                  u32 msaa);
Surf* vk_zt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 layers, u64 layer_size, u32 msaa);
/* a surface's sample count */
static inline VkSampleCountFlagBits vk_surf_samples(const Surf* s) {
    return (VkSampleCountFlagBits)ds_msaa_samples(s->msaa);
}
/* whether the device makes images of format f, usage and flags with samples of the sample layout msaa */
int vk_ms_supported(VkFormat f, VkImageUsageFlags usage, VkImageCreateFlags flags, u32 msaa);
Surf* vk_rt_find(u64 gva);
Surf* vk_rt_snapshot(Surf* rt);                  /* copy a target so a draw can sample its contents while writing it */
Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h); /* the render target the guest presents from that memory */
/* a render target read where its guest memory is (a texture, a present): reload it if the GPU wrote its memory */
void vk_rt_note_read(Surf* s);
void vk_rt_before_write(u64 gva, u64 size);
void vk_rt_before_write_cpu(u64 cpu, u64 size);
/* an image of a scaled target's format at the guest's size with that many layers, a transfer's source and
   destination: to move it to and from guest memory, or into a texture a blit can't reach. VK_NULL_HANDLE on failure */
VkImage vk_unscaled_image(const Surf* s, u32 layers, VmaAllocation* mem);
/* moves a surface into a layout for a use, with the barrier that needs (which ends an open rendering pass) */
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
/* the stages a texture moved to SHADER_READ_ONLY_OPTIMAL is made visible to: all that sample, since a later read in
   the same layout gets no barrier of its own */
#define VK_SAMPLED_STAGES (VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
VkImageView vk_rt_attachment(Surf* s, int srgb_write);
void vk_rt_flush(u64 gva);
void vk_gfx_image_mem_need(u64 gva, u64 size);
int vk_gfx_image_pending(u64 gva, u64 size);
void vk_gfx_image_land(void);
/* at a release: the storage images' write-backs recorded, to land once their submit is done (vk_lock) */
void vk_gfx_image_put_aside(void);
/* a recorded write-back waits to land (unsubmitted: in the submit being recorded) */
int vk_gfx_image_in_flight(int unsubmitted);
/* the submissions up to sub are done: the write-backs recorded in them land (cmd.c's landing, vk_lock) */
void vk_gfx_image_frame_done(u64 sub);
int vk_gfx_image_holds(const Surf* s); /* s has a storage result waiting to land in memory */
u64 vk_surf_guest_size(const Surf* s);
int vk_surf_can_writeback(const Surf* s);
void vk_surf_flush(Surf* s, const u8* original); /* original preserves newer guest writes for storage images */
/* s's pixels published over its guest bytes, GPU-owned and not waited for: 1, or 0 when only vk_surf_flush can */
int vk_surf_publish(Surf* s);
/* a release: the pitch-linear color targets drawn since the last one published, for the CPU to read */
void vk_rt_publish_linear(void);
void vk_surf_wrote(Surf* s); /* its image changed: its copies are out of date (wgen), and it is the newest (wseq) */
u32 vk_surf_gen(void);       /* a write generation no image has held yet */
/* vk_surf_flush in two halves, the GPU not waited for: the bytes a write-back of s copies (0 for none), its copy
   recorded into buf at at (host-readable once the submit being recorded is done), and its landing from those bytes p,
   as of the write generation the copy was recorded at */
/* the render targets over [gva, gva + size) that hold what guest memory doesn't yet (drawn since their last
   writeback): their bytes recorded over buf's copy of the range (at `at`), on the GPU, swizzled as guest memory lays
   them out, the oldest image first so the newest is on top; a target reaching past the range is written back to guest
   memory instead (g_vk_rt_readbacks counts those). How many were recorded: the caller orders their writes (a barrier)
   before what reads buf */
int vk_rt_overlay(u64 gva, u64 size, VkBuffer buf, u64 at);
/* whether vk_rt_overlay has a render target to record over [gva, gva + size) */
int vk_rt_over(u64 gva, u64 size);
extern u64 g_vk_rt_readbacks;
u64 vk_surf_flush_size(const Surf* s);
void vk_surf_flush_record(Surf* s, VkBuffer buf, u64 at);
void vk_surf_flush_land(Surf* s, const u8* p, const u8* original, u64 wgen);
void vk_tex_note_writeback(Surf* s);
void vk_rt_drawn(Surf* s);      /* a render target was drawn to (gpu/vk/render.c) */
void vk_rt_drawn_full(Surf* s); /* a full clear or blit replaced every pixel */
int vk_rt_drawn_fresh(Surf* s); /* no guest-memory write has replaced the target since its last draw */
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max); /* count drawn in [lo, hi), write up to max */
void vk_rt_reload(u64 gva);
/* textures share the surface table. g_vk_surf_epoch changes whenever a surface enters it, leaves it or moves (a
   target parked or taken back), so what was found there can be kept until it changes */
extern u32 g_vk_surf_epoch;
Surf* vk_surf_first(u64 gva);
void vk_surf_add(Surf* s); /* a texture, into the table and the textures' recency list */
/* s used now: last_used is this present, and it moves to the recent end of its list */
void vk_surf_touch(Surf* s);
/* s's pixels can be had again now: off the list of old ones whose pixels would be lost, if it is on it */
void vk_surf_relist(Surf* s);
VkImageAspectFlags vk_aspect(const VkFmt* f);
/* frees textures (VK_LRU_TEX) or render targets (VK_LRU_RT), the least recently used first, last used before the
   present `before`, until `want` bytes went. lossy says which: VK_RECLAIM_LOSSLESS only those whose pixels can be had
   again (textures loaded from memory, render targets memory holds), VK_RECLAIM_IDLE the others too but for a target
   holding a draw memory lacks, which a texture made at its address takes, VK_RECLAIM_ANY everything. Returns the
   bytes freed */
#define VK_RECLAIM_LOSSLESS 0
#define VK_RECLAIM_IDLE     1
#define VK_RECLAIM_ANY      2
u64 vk_surf_reclaim(int kind, u32 before, u64 want, int lossy);
/* the render targets later ones drew all over (no pixels to lose), last used before `before`, up to `want` bytes */
u64 vk_rt_reclaim_superseded(u32 before, u64 want);
void vk_surf_stats(int kind, u32* count, u64* bytes, u32* parked);
/* frees cache entries (surfaces and own buffers), the least recently used first, when the heap's use with `extra`
   bytes more would be past 90% of its budget: idle lossless ones down to 80%, the lossless working set down to 95%,
   then lossy ones (vk_surf_reclaim). Only entries unused for the last two presents go, so the caller may hold any
   surface it used in them. With force (an allocation found no memory), `extra` bytes go even under the marks, less
   what is being released already, once a present */
void vk_mem_trim(u64 extra, int force);
u64 vk_mem_trimmed(void); /* statistics: the bytes vk_mem_trim freed */
/* vk_image_create for a surface of the caches, with room made for it first (vk_mem_trim). When no memory is found, a
   forced trim makes room for later, and images as big fail at once for the rest of the present. bytes: the device
   memory it takes */
int vk_cache_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, u64* bytes);
int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy);

/* blocklinear.c: a block linear surface's rows moved between its layout and linear rows on the GPU
   (gpu/texture/blocklinear.comp). The surface is row_bytes wide (texels or compressed blocks times their bytes),
   height rows high, in blocks of 2^bh GOBs high and 2^bd deep, already shrunk for small surfaces as the hardware
   does; the move takes rows y0 to y0 + rows of slices z0 to z0 + slices. Its linear rows are lin_pitch bytes apart
   (a multiple of 16, at least row_bytes rounded up to 16: whole 16 byte pieces move, so a row's last one carries
   padding along), a slice's rows after another's */
typedef struct {
    u32 row_bytes, height, bh, bd;
    u32 y0, rows, z0, slices;
    u32 lin_pitch;
} VkBlMove;
/* the layout's bytes [*lo, *hi) the move reads or writes: whole rows of blocks */
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi);
/* records the move, into block linear when to_bl, else out of it: the layout's byte lo (vk_bl_range) at bl_at in bl,
   the first linear row at lin_at in lin, both multiples of 16. 0 when the device's limits don't allow it (said once).
   Moves go between vk_bl_barrier(cb, 1) and vk_bl_barrier(cb, 0), several of them between one pair when they write
   different bytes */
int vk_bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at);
/* before moves (1): after the transfers and dispatches that used either side's buffers; after them (0): before the
   transfers, dispatches and host reads of what they wrote */
void vk_bl_barrier(VkCommandBuffer cb, int before);
/* a device-local buffer of size bytes or more for the linear side, *cap of them, the same one until a bigger one is
   asked for (vk_bl_barrier orders its uses; the old one lives until the frame's work is done); VK_NULL_HANDLE past
   g_vk.alloc_cap or when memory ran out */
VkBuffer vk_bl_scratch(u64 size, u64* cap);

/* owned.c: guest memory the GPU holds (memory/own.c, docs/GPU_OWNERSHIP.md). An engine's result (VIC's planes, a
   DMA copy's destination, a 2D blit's, inline-to-memory) is a resource: a device-local buffer in the guest's own
   layout, published over the guest bytes it holds. The CPU sees it only when it touches those bytes: the trap lands
   it then. vk_own_init after the device and the frames exist: 0 when the host traps guest memory */
typedef struct VkOwnRes {
    VkBuffer buf;
    VmaAllocation mem;
    u64 size;
    u32 refs; /* the creator's, and one an owned range */
} VkOwnRes;
int vk_own_init(void);
/* whether the CPU made GPU-owned bytes of guest memory [cpu, cpu + size) land within the last presents presents */
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents);
VkOwnRes* vk_own_res_new(u64 size); /* with the caller's reference; NULL when out of memory */
void vk_own_res_unref(VkOwnRes* r);
/* the guest CPU bytes [cpu, cpu + size) hold r's bytes from off on once the work recorded so far is done: owned, or,
   where the host can't trap them, landed at once (the GPU waited for). Under vk_lock */
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off);
/* guest bytes [gva, gva + size) (by GPU address) for the GPU to read, without landing what it holds: the CPU's
   copied into host (staging memory at `at` in buf), the GPU's copied into buf there by the GPU, recorded in the
   frame's command buffer with the barriers that order them. vk_cpu_read is the same by guest CPU address. Under
   vk_lock */
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at);
void vk_cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at);
/* the same into device memory (buf, which the CPU doesn't map): 0 when staging has no room */
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at);
/* guest bytes [gva, gva + size) into device memory as the GPU sees them: vk_guest_read_device, with the render targets
   drawn over them (vk_rt_overlay). 0 when there is no room */
int vk_guest_read_seen(u64 gva, u64 size, VkBuffer buf, u64 at);
/* guest bytes [gva, gva + size) into dst for change detection, as gmem_read reads them, except that bytes the GPU
   holds come back as a pattern of their owner (which a new owner changes, and the CPU's own bytes replace once it
   touches them): nothing lands */
void vk_guest_peek(u64 gva, void* dst, u64 size);
/* everything recorded so far done before what follows reads or writes what it did (the engines' steps) */
void vk_all_barrier(VkCommandBuffer cb);
/* rows rows of row_bytes from src (rows src_pitch apart) over dst's rows (dst_pitch apart), a region a row */
void vk_copy_rows(VkCommandBuffer cb, VkBuffer src, u64 src_at, u32 src_pitch, VkBuffer dst, u64 dst_at,
                  u32 dst_pitch, u32 row_bytes, u32 rows);

/* texconv.c: a format the host can't take as it is (f->conv but ASTC, and packed depth and stencil), converted on the
   GPU (gpu/texture/texconv.comp) from linear rows of its guest texels or blocks into rows a copy into the image
   takes */
int vk_tc_converts(const VkFmt* f);
/* the conversion's shape for a row w texels wide: units (invocations) a row, the output's row in texels (padded to
   whole units: a copy's bufferRowLength) and its bytes a texel (a depth aspect's for depth), output rows a row of
   units (4 for a row of blocks), whether a stencil aspect follows the depth one, and what the input's pitch must be
   a multiple of (the input rows are read in whole units) */
typedef struct {
    u32 units, out_texels, out_bpp, out_rows, stencil, in_align;
} VkTcShape;
VkTcShape vk_tc_shape(const VkFmt* f, u32 w);
/* the output's bytes for rows rows of units of slices slices: depth rows, then as many of stencil bytes */
u64 vk_tc_out_size(const VkTcShape* s, u32 rows, u32 slices);
/* records the conversion of rows rows of units (of texels, or blocks) of slices slices, w texels wide, from rows
   in_pitch bytes apart at in_at in in to out_at in out (offsets and pitch multiples of 4). Between vk_bl_barrier
   calls as vk_bl_move is. 0 when the device's limits don't allow it (said once) */
int vk_tc_convert(VkCommandBuffer cb, const VkFmt* f, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at,
                  u32 in_pitch, VkBuffer out, u64 out_at);
/* the same for a frame vi presents from memory, of vi's pixel format fmt (Android's: 2 RGBX8888, 4 RGB565, 5
   BGRA8888, 6 RGBA5551, 7 RGBA4444) to RGBA8 rows of the shape's out_texels; vk_tc_vi_converts is 0 for RGBA8888,
   which needs no conversion, and formats vi doesn't have */
/* NV12's chroma rows from FFmpeg's U and V planes (rows in_pitch bytes apart at u_at and v_at in in): pairs pairs a
   row, rows rows, into out at out_at, rows out_pitch bytes apart (offsets and pitches multiples of 4; the rows are
   written in whole words: pairs rounded up to 4). 0 past the device's limits */
int vk_tc_nv12_uv(VkCommandBuffer cb, u32 pairs, u32 rows, VkBuffer in, u64 u_at, u64 v_at, u32 in_pitch,
                  VkBuffer out, u64 out_at, u32 out_pitch);
/* the DMA copy engine's remap (gpu.h's GpuDmaOp) of lines lines: each line in at in_at + y * in_pitch (bytes, any
   alignment), out at out_at + y * out_pitch (words), which holds the destination's own bytes already for the
   components that keep them. 0 past the device's limits */
int vk_tc_dma_remap(VkCommandBuffer cb, const GpuDmaOp* o, u32 lines, VkBuffer in, u64 in_at, u32 in_pitch,
                    VkBuffer out, u64 out_at, u32 out_pitch);
/* the 2D blit's gather (gpu.h's GpuBlit): rows rows of nb bytes into out at out_at (rows out_pitch bytes apart, a
   multiple of 4), pixels of bpp bytes from source rows in_pitch bytes apart at src_at in in, through the column and
   row tables at col_at and row_at in in (words, multiples of 4), converted between float formats when fmt is
   GpuBlit's (GPU_BLIT_FLOAT). in is bound from 0. 0 past the device's limits */
int vk_tc_blit(VkCommandBuffer cb, u32 bpp, u32 fmt, u32 nb, u32 rows, VkBuffer in, u64 in_size, u64 src_at,
               u32 in_pitch, u64 col_at, u64 row_at, VkBuffer out, u64 out_at, u32 out_pitch);
/* index buffers Vulkan can't read (texconv_modes.glsl's TC_INDEX_*): n elements (index pairs, indices or quads) from
   in_size bytes at in_at in in into out_size bytes at out_at in out, with the guest's restart index restart and
   remap as the mode says (offsets multiples of 4; in_size 0 binds a word). 0 past the device's limits */
int vk_tc_index(VkCommandBuffer cb, u32 mode, u32 remap, u32 restart, u32 n, VkBuffer in, u64 in_at, u64 in_size,
                VkBuffer out, u64 out_at, u64 out_size);
/* a vertex stream of len bytes at in_at in in, its vertices in_stride bytes apart, written to out at out_at with them
   out_stride apart (a multiple of 4: TC_RESTRIDE), nv vertices. in must hold every word the stream touches; out_at is
   a multiple of 4. 0 past the device's limits */
int vk_tc_restride(VkCommandBuffer cb, u32 in_stride, u32 out_stride, u32 nv, VkBuffer in, u64 in_at, u64 len,
                   VkBuffer out, u64 out_at);
int vk_tc_vi_converts(u32 fmt);
VkTcShape vk_tc_vi_shape(u32 fmt, u32 w);
int vk_tc_convert_vi(VkCommandBuffer cb, u32 fmt, u32 w, u32 rows, VkBuffer in, u64 in_at, u32 in_pitch, VkBuffer out,
                     u64 out_at);

/* astc.c: decode a level of one layer of t, w x h x depth texels, into it. The blocks come from rows(ctx, cb, z, nz,
   y, n, &buf, &off): block rows y to y + n of slices z to z + nz, tightly packed, 16 bytes a block, a slice's after
   another's, in buf at off (aligned for a storage buffer) once what rows recorded into cb is done. They are asked for
   a strip at a time, so a level is never all in memory at once. 0 when staging ran out (the level is left out, and
   the caller tries again later); rows returns 0 for that too. The caller ends rendering and moves t to
   TRANSFER_DST_OPTIMAL first. A guest read while staging lands GPU results, and landing can submit the frame's
   command buffer, which ends the one cb names: rows then stores the buffer recording now through cb, and the caller
   records the rest of the strip there */
typedef int (*VkAstcRows)(void* ctx, VkCommandBuffer* cb, u32 z, u32 nz, u32 y, u32 n, VkBuffer* buf, u64* off);
int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 depth, u32 level, u32 layer, VkAstcRows rows,
                   void* ctx);
/* the image levels uploaded as BC7 so far, their bytes, and the bytes they would have taken as RGBA8 */
void vk_astc_stats(u64* levels, u64* bytes, u64* rgba8_bytes);

/* vic.c: the VIC composition as a compute shader, recorded in the frame's command buffer, its output planes
   published (vk_own_publish): 1 when composed, 0 when the job exceeds the device (it is skipped). Takes vk_lock */
int vk_vic_compose(const VicJob* j);
/* nvdec.c: an NVDEC picture into its surfaces on the GPU (gpu.h's NvdecPic): staged rows (chroma interleaved there)
   or a Vulkan Video picture's planes copied out of its image, both planes swizzled there, the surfaces published
   (vk_own_publish). 1 when written; takes vk_lock. vk_nvdec_hw_device: the renderer's nvdec_hw_device */
int vk_nvdec_write(const NvdecPic* p);
void* vk_nvdec_hw_device(void);
/* dma.c: a launch of the DMA copy engine on the GPU (gpu.h's GpuDmaOp), its destination published: 1 when done, 0
   past the device's limits (skipped). Takes vk_lock */
int vk_dma_copy(const GpuDmaOp* o);
/* memory for the DMA engine's copies of sources at submission, which the copy reads in place (Renderer.snap_alloc and
   snap_free, from any thread), and the bytes it holds */
u8* vk_snap_alloc(u64 len, u64* id);
void vk_snap_free(u64 id);
u64 vk_snap_bytes(void);
/* blit.c: a 2D blit from memory on the GPU (gpu.h's GpuBlit), its destination published: 1 when done. Takes vk_lock */
int vk_blit_mem(const GpuBlit* b);
/* msaa.c: a multisampled surface's samples moved between its image and the guest's layout of them (sample i of pixel
   (x, y) at texel (x X + i % X, y Y + i / X) of a surface X by Y times its size, DS_MSAA_), through a single-sampled
   image of that size (the grid), by draws. Under vk_lock, outside a rendering pass (they end an open one) */
int vk_ms_moves(const Surf* s); /* s's samples can move: multisampled, unscaled, with a raw view (Surf.ms_raw) */
/* layer's samples drawn into a new grid of s's move format, left in TRANSFER_SRC_OPTIMAL for copies recorded in this
   frame (it is freed with it): VK_NULL_HANDLE when they can't be */
VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer);
/* layer's samples drawn from a grid loaded with rows of pitch bytes from off in buf: 0 when they can't be (a depth
   surface's never are) */
int vk_ms_pack(VkCommandBuffer cb, Surf* s, u32 layer, VkBuffer buf, u64 off, u32 pitch);
/* surface.c: a multisampled surface's samples loaded from guest memory (a texture's upload): 0 when they can't be */
int vk_ms_load(Surf* s);

/* n little-endian words of guest memory at gva as host numbers (texture and sampler headers, constant buffer words),
   as gmem_read reads them */
static inline void vk_gmem_words(u64 gva, u32* w, u32 n) {
    gmem_read(gva, w, (u64)n * 4);
    snx_le32_array(w, n);
}

/* texture.c */
void vk_render_forget_state(void); /* the next draw sets all its state again (another pipeline or state was set) */
Surf* vk_tex_from_tic(const u32* tic); /* tic: the header's words as host numbers */
/* Writable resolution bypasses read-only render-target crop snapshots. */
Surf* vk_tex_from_tic_storage(const u32* tic);
/* a texture vk_tex_from_tic returned for an unchanged header, checked again as it would be: memory written since, what
   targets drew into it. NULL when it can't be drawn now */
Surf* vk_tex_recheck(Surf* t);
Surf* vk_tex_from_tic_raw(const u32* tic); /* UINT view of raw RGBA8 texel bytes */
/* a view of s as the shader declares it, sRGB or linear (color formats that have both), with a component mapping;
   VK_NULL_HANDLE when the image can't be viewed that way */
/* stencil: a depth/stencil surface's stencil aspect instead of its depth */
VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil);
/* the format of s's views, its sRGB form when srgb (a depth format stays) */
VkFormat vk_tex_view_format(const Surf* s, int srgb);
/* One selected mip for storage; the sampled view retains the complete chain. */
VkImageView vk_tex_storage_view(Surf* s, VkImageViewType type);
/* an empty texture of a view type: num 0 float, 1 unsigned, 2 signed integer, 3 depth at 1.0 (shadow samplers);
   ms: a multisampled one (2D and 2D array views, not depth) */
VkImageView vk_null_view(VkImageViewType type, int num, int ms);
/* the sampler for a TSC: raise lets anisotropy= raise its filtering (a 2D texture with mipmaps), integer gives its
   border color as integers (an integer texture) */
VkSampler vk_sampler(const u32* tsc, int raise, int integer);
extern int g_vk_anisotropy; /* anisotropy=, read once at start: the sampler table keeps the samplers it made */
void vk_tex_frame(void);    /* a frame was presented: evicts what wasn't used for a while */

/* gpu/vk/buffer.c: guest memory [gva, gva + len) as a buffer the GPU reads, valid until the next submit */
int vk_buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off);
/* the same for a vertex, index or storage buffer, whose offset needs only to be a multiple of align (a power of two) */
int vk_buf_get_input(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
/* the same inside a window shared with the ranges it overlaps (a storage buffer with no size of its own): *off is the
   range's offset in it, a multiple of minStorageBufferOffsetAlignment */
int vk_buf_get_window(u64 gva, u64 len, VkBuffer* buf, u64* off);
int vk_buf_get_window_input(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
/* the cache lines vk_buf_get(gva, len) will read, fetched ahead: step 0 its index slot, step 1 (after step 0 for every
   range) its entry. For a caller with several ranges to look up, so their misses overlap */
void vk_buf_prefetch(u64 gva, u64 len, int step);
void vk_buf_mapping_changed(void); /* the GPU mappings or the guest memory under them changed: translations go */
int vk_buf_get_strict(u64 gva, u64 len, VkBuffer* buf, u64* off); /* no delayed content checks */
/* a copy of len bytes made now (data that changes within a frame), or of zeros when gva is 0 */
int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off);
int vk_buf_copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off);
typedef struct {
    u64 addr;
    u32 size;
    int writable, aliased;
    int bare; /* its address has no size beside it (SH_GBUF_*): bound in a shared window */
    VkBuffer buf;
    u64 off, range;
} VkDrawGbuf;
typedef struct {
    VkDrawGbuf b[2][16];
    int n[2];
    VkBuffer map_buf[2];
    u64 map_off[2], map_size[2];
} VkDrawStorage;
int vk_buf_draw_has_alias(const VkDrawStorage* draw);
int vk_buf_draw_prepare(VkDrawStorage* draw);
void vk_buf_writable_abort(void);
void vk_buf_writable_commit(void);
void vk_buf_mem_need(u64 gva, u64 len);
/* own buffers last used before the present `before`, the least recently used first, dropped until `want` bytes went
   (their ranges are copied again when next read). Returns the bytes */
u64 vk_buf_reclaim(u32 before, u64 want);
void vk_buf_stats(u32* count, u64* bytes);
int vk_buf_pending(u64 gva, u64 len); /* a pending graphics result overlaps the range */
u64 vk_buf_adds(void);                /* graphics results committed so far: as vk_compute_adds */
int vk_buf_pending_any(void);         /* any graphics result is pending */
int vk_buf_pending_unsubmitted(void); /* one is in the commands recorded since the last submit */
/* renderdoc.c: with renderdoc set, RenderDoc loaded before the instance exists (load) and the frames
   renderdoc_frames names captured (present, at each present under vk_lock) */
void vk_renderdoc_load(void);
void vk_renderdoc_present(u64 present);

/* release.c: releases the game can observe, once the results before them land (renderer.h release_write) */
void vk_release_write(u64 gva, const void* data, u32 len);
void vk_release_incr(u32 id);
/* a samples-passed report: len bytes at gva (4, or 16 with ticks after the count), the count of query.c's counter's
   segments [first, end), once the GPU has done the submit being recorded */
void vk_release_samples(u64 gva, u32 len, u32 counter, u64 first, u64 end, u64 ticks);
void vk_release_queued(u64* n, u64* gate, u64* cgate); /* the queue's length and its oldest's gates (any thread) */
void vk_release_submit(void); /* the GPFIFO submission ended or waits: a release gated on the recording submits */
void vk_release_need(u64 gva, u64 len);
int vk_release_read(u64 gva, void* dst, u64 len);
extern u64 g_vk_release_forwards;           /* statistics: reads vk_release_read answered */
extern u64 g_vk_dset_new, g_vk_dset_reused; /* statistics: descriptor sets written, reused for the same contents */
u32 vk_release_value(u32 id);
/* the event slots the syncpoint increments this thread performed under vk_lock reached, signaled (nvdrv.c's
   syncpt_events): the outermost vk_unlock's, after the lock goes */
void vk_release_events(void);
/* renderer.h render_predicate and render_predicate_drop. A predicate is its slot + 1 (query.c), with VK_PRED_EQUAL for
   a draw when the count is zero */
#define VK_PRED_EQUAL 0x80000000u
int vk_render_predicate(u64 a, int equal, u32* pred);
void vk_render_predicate_drop(u32 pred);
void vk_release_retire(u32 id);
void vk_release_drain(void);
/* query.c: the samples-passed counter with occlusion queries (renderer.h samples_*), under vk_lock. vk_query_draw before
   a game's draw in a rendering pass, vk_query_pass_end before the pass ends or a draw of the renderer's own in it
   (render.c), vk_query_flush outside a pass before a submit (cmd.c) */
void vk_query_draw(VkCommandBuffer cb, u32 owner); /* counted as the targets' samples, the guest's */
void vk_query_pass_end(VkCommandBuffer cb);
void vk_query_flush(VkCommandBuffer cb);
void vk_query_reset(u32 owner);
void vk_query_enable(u32 owner, int on);
int vk_query_report(u32 owner, u64 gva, u32 len, u64 ticks);
/* a report performed (release.c): the count of counter's segments [first, end) */
u64 vk_query_value(u32 counter, u64 first, u64 end);
#define VK_QUERY_COUNTERS 8 /* query.c's samples-passed counters, a draw owner's each */
/* render enable predicates (release.c's vk_render_predicate): a queued report of counter's segments [first, end) gets
   a predicate slot, held once, that the next vk_query_pred_flush writes (2, *slot set), or its count is known now: 1
   not zero, 0 zero. -1 when there can be no predicate: the device can't make them, or there's no memory for the
   batch. The reports of a counter come in their queue order, each with what the one before it in its epoch, still
   queued, is known to be (prev, ending at prev_end): the count up to there, which it goes on from */
enum { VK_PRED_PREV_NONE, VK_PRED_PREV_ZERO, VK_PRED_PREV_NONZERO, VK_PRED_PREV_SLOT };
int vk_query_pred_ok(void); /* the device makes predicates (VK_EXT_conditional_rendering); makes the shader */
int vk_query_pred_job(u32 counter, u64 first, u64 end, u64 prev_end, int prev, u32 prev_slot, u32* slot);
int vk_query_pred_flush(void); /* records the batch's shader; 0 without memory, its slots let go */
void vk_query_pred_hold(u32 slot);
void vk_query_pred_drop(u32 slot);
VkBuffer vk_query_pred_buffer(void); /* a predicate's word is at slot * 4 */
/* a predicate slot, held once, that the GPU writes now with whether the 64-bit values at a and a + 16 differ: 1, or 0
   when it can't be made */
int vk_query_pred_compare(u64 a, u32* slot);
void vk_buf_mem_need_map(u64 gva, u64 len);
void vk_buf_before_write(u64 gva, u64 len);
void vk_buf_land(void);
void vk_buf_land_syncwait(void);
void vk_buf_land_map(void);
void vk_buf_frame_done(u64 frame);
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame);
void vk_buf_frame(void);
/* Land GPU-written bytes only where guest memory still matches the shader's input snapshot. */
void vk_result_begin(void);
void vk_result_end(void);
int vk_result_landing(void);
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size);
/* the same, telling run each run of changed granules: the 8-byte pieces of the result from its start on (the last may be
   shorter) where res and org differ, merged when they touch, in order */
typedef void (*VkChangedRun)(void* user, u32 from, u32 to);
void vk_write_changed_runs(u64 addr, const u8* res, const u8* org, u32 size, VkChangedRun run, void* user);
/* 1 if [host, host+len) was written since the write-tracking clock read `since`, 0 if not, -1 when write tracking
   can't tell (no host pointer, or not available): then check the contents */
int vk_written_since(const u8* host, u64 len, u32 since);
/* fn(user, at, n) for each run of [host, host + len) on pages written after since, at the range's byte offset at: for
   a range vk_written_since(host, len, since) has just found written, so its pages are marked */
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user);
u32 vk_ww_now(void);
void vk_ww_new_epoch(void); /* the CPU may have written what a GPU stream reads next: write tracking scans again */
u32 vk_ww_epoch(void);
/* the calling thread runs a job of a stream (1: its checks see the CPU's writes from before the newest epoch now, and
   from before the waits it ends) or stops (0: they see every epoch's) */
void vk_ww_stream(int on);
void vk_mem_changed(u64 cpu, u64 len); /* gpu.c's g_gpu_mem_changed: the range counts as written */
/* own.c's landed hook: host pages [cpu, cpu + size) a landing wrote, with their bytes then, whose next report as
   written counts only if those changed */
void vk_ww_landed(u64 cpu, u64 size, const u8* bytes);

/* pipeline.c */
/* the constant buffer words a translation read (the translator resolves BRX jump tables through them): what it made
   is only reused while they still hold the same */
typedef struct {
    u32 idx, off, ok, v;
} VkCbRead;
typedef struct {
    VkCbRead* r;
    u32 n;
} VkCbReads;
typedef int (*VkCbReader)(u32 idx, u32 off, u32* out);
u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads);
/* why the device can't take the module: a capability it declares (Float64, Int64) whose feature is off, or NULL */
const char* vk_spirv_unavailable(const u32* spv, size_t words);
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read); /* BRX tables through read */
/* where each new shader's snx-shaderc corpus entry goes: shader_corpus, else with diag=1 captures/shaders beside
   the games; NULL for neither */
const char* vk_corpus_dir(void);
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read);
typedef struct VkSh {
    u64 hash;
    u32 mask[20]; /* vk_shader_get's masks, including the geometry passthrough skip mask */
    int stage;
    VkCbReads cbr;
    ShaderInfo si;
    VkShaderModule mod;
    int indexed; /* it reads indexed textures: its set needs the texture array bindings (vk_playout) */
    int failed;
    int shipped; /* a program the game ships, translated before a draw asked for it: 2 once one found it */
    u32 clen, clen_a;
    u64 qh, qh_a;
    struct VkSh* next;
} VkSh;
/* the pipeline layout whose sets have no texture array bindings: compute's, and the one push constants go through
   (every layout has the same push constant range) */
extern VkPipelineLayout g_vk_playout;
VkPipelineLayout vk_playout(u32 arrays); /* arrays: a bit per set (shader_vk_set) that has them */
extern int g_vk_preserve_infnan;
extern int g_vk_preserve_infnan64;
extern int g_vk_flush_denorm;
/* SH_FTZ_*: how translations get the console's FP32 flush to zero, from g_vk_flush_denorm and vk_ftz_native */
extern int g_vk_ftz;
/* ftz_probe.c: 1 when the device flushes FP32 subnormals in FADD, FMUL and FFMA as the console's FTZ does, in a module
   that declares no denormal mode (it runs a compute shader and waits for it: at the renderer's start only) */
int vk_ftz_native(void);
extern VkShaderStageFlags g_vk_subgroup_stages;
/* by set, shader_vk_set(stage): [0] without the indexed texture arrays, [1] with them. A set holding 512 array
   descriptors cost the driver twice the time to bind (one game's 6,700 draws a frame), so only an indexed shader's has
   them */
extern VkDescriptorSetLayout g_vk_dslayout[2][5];
extern u32 g_vk_stage_sets; /* sets in the pipeline layout: 5, or 2 without the tessellation and geometry stages */
/* the stages that read the push constants: every graphics stage reads the viewport/depth parameters in pc[0..3]
   (the last one before rasterization applies them); fragment reads the alpha test in pc[4..6] and sprite T flip
   in pc[7] */
#define VK_PUSH_STAGES                                                                                                 \
    (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |                                           \
     VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
void vk_pipeline_init(void);
/* the shader at gva (after a VertexA program at gva_a, or 0). The masks key integer inputs: attributes for the
   vertex stage, textures and render target outputs for the fragment stage. rt_int keys the primitives of the
   tessellation evaluation and geometry stages (ShaderInfo.rt_int_mask). clip: the clip distances the 3D engine
   enables, and SNX_SH_NOT_LAST (the stages before rasterization). xfb: what transform feedback captures of the last stage before
   rasterization's outputs (ShaderInfo.xfb), NULL for none. ds: the draw state whose programs key a fragment program
   (its early fragment tests) and a geometry program (its passthrough mask), and whose bound constant buffers the
   translator reads to resolve indirect branches. NULL for none */
VkSh* vk_shader_get(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, const ShXfb* xfb);
/* whether the constant buffer words s's translation read still hold in ds's bound buffers (vk_shader_get's check on
   a hit, for a caller that kept s) */
int vk_shader_cb_hold(const DsState* ds, const VkSh* s);
typedef struct {
    VkShaderModule vs, fs;
    VkShaderModule tcs, tes, gs; /* tessellation control and evaluation, geometry: VK_NULL_HANDLE when off */
    u32 patch;                   /* control points of a patch, with tessellation */
    /* POINT_LIST, LINE_LIST, TRIANGLE_LIST, their adjacency forms or PATCH_LIST: the topology is dynamic within its
       class */
    u32 topo_class;
    u32 polygon;
    u32 depth_clamp;
    u32 nattr, nbind;
    struct {
        u32 loc, binding, format, offset;
    } attr[32];
    struct {
        u32 divisor; /* 0: per vertex */
        u32 stride;  /* only without has_dynamic_stride, else 0 */
    } bind[33];      /* bindings 0..nbind-1: a binding per attribute at most, and one for constant attributes */
    u32 ncolor;
    u32 color[8];
    u32 depth, stencil;
    struct {
        u32 en, sc, dc, cop, sa, da, aop, mask;
    } blend[8];
    u32 feedback; /* VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT, without the dynamic state for it */
    u32 provoking_last; /* the last vertex provokes (has_provoking_last) */
    u32 logic_op;       /* 0, or the VkLogicOp the color targets take in place of blending, plus 1 */
    u32 arrays;   /* the sets with texture arrays: its layout, vk_playout(arrays) */
    u32 samples;  /* the targets' VkSampleCountFlagBits (0 taken as 1) */
    /* with several samples, DsRasterKey's: the samples the sample mask leaves out, alpha to coverage and to one */
    u32 sample_off, alpha_to_coverage, alpha_to_one;
    u32 sample_shading; /* every sample is shaded (sampleRateShading), with several samples */
    u32 conservative;   /* DsRasterKey's (has_conservative): 0 off, else 1 plus the dilation in quarter pixels */
} PipeKey;
/* the pipeline for k, made when new and waited for while a worker makes it: NULL only when it can't be made */
VkPipeline vk_pipeline_get(const PipeKey* k);
/* the threads making new pipelines: setting (pipeline_threads) when it is one or more, else a quarter of cpus, two
   to six */
int vk_pipeline_worker_count(int setting, int cpus);
/* a draw's pipeline: the pipeline, or a stand-in the recording thread waits for at its bind (vk_pipeline_resolve, the
   pipeline a stand-in names, or h itself) */
VkPipeline vk_pipeline_get_deferred(const PipeKey* k);
VkPipeline vk_pipeline_resolve(VkPipeline h);
/* the pipeline h names when it is ready, else h itself, without waiting: a stand-in and its pipeline are one */
VkPipeline vk_pipeline_ready(VkPipeline h);
/* h's pipeline if it is ready, else VK_NULL_HANDLE, *pending saying whether a worker still makes it */
VkPipeline vk_pipeline_now(VkPipeline h, int* pending);
/* so far: pipelines made, how many of them the pipeline cache had, ms the workers took making them, ms the GPU thread
   waited for new ones, shaders translated, ms translating them */
void vk_pipeline_stats(u64* pipes, u64* hits, u64* make_ms, u64* wait_ms, u64* shaders, u64* shader_ms);
void vk_tex_stats(u64* uploads, u64* bytes); /* texture uploads from guest memory so far, and their bytes */
u64 vk_rt_made(void);                         /* render targets made so far */
VkPipelineCache vk_pipeline_cache(void); /* for pipelines made elsewhere (compute) */
void vk_pipeline_save(int at_exit);
VkShaderModule vk_module_spv(const u32* words, size_t count);
VkShaderModule vk_module_spv_location(const u32* words, size_t count, u32 location);
/* pipelines of earlier runs made in the background: made so far, how many the pipeline cache had, and still to make */
void vk_pipeline_prewarm_stats(u64* made, u64* hits, u64* left);
/* what is made before the game draws, so far (the renderer's prepare) */
struct RendererPrepare;
void vk_pipeline_prepare(struct RendererPrepare* p);

/* display.c: the display's clock (sc_clock). vk_display_id gives the next present's id (0 without a clock),
   vk_display_presented hands a present queued with it to the thread that learns when
   it is displayed (game: a frame of the game's, released by it at released_ns), on the GPU thread under vk_lock.
   syncpt (0 none) is incremented once it is displayed or let go: a frame with no id (not presented, or no clock) is
   handed over too, and its syncpoint goes up once the presents before it are done with. vk_display_fifo
   says whether the swapchain shows each present for a refresh at least (FIFO), which pacing on the display needs.
   vk_display_wait is renderer.h's display_wait */
u64 vk_display_id(void);
void vk_display_dropped(void); /* a game frame not shown: no swapchain image was free (GPU thread) */
/* with sc_hold, a game frame's display target for a frame period of period_ns (0 for none): the refresh it goes
   to, held there, in CLOCK_MONOTONIC ns (vk_rec_present's target), 0 to show it at once. GPU thread */
u64 vk_display_target(u64 period_ns);
void vk_display_presented(VkSwapchainKHR sc, u64 id, int game, u64 released_ns, u32 syncpt);
/* the present with that id (vk_display_presented before) was made, with that result: display.c waits for it only
   then (a present wait 2 for an id not presented yet is invalid). Any thread */
void vk_display_made(u64 id, VkResult r);
void vk_display_fifo(int fifo);
int vk_display_wait(u64 deadline_ns, u64* refresh_ns);

/* compute.c */
void vk_dispatch(const DispatchState* d);
void vk_compute_land(void); /* pending compute results into guest memory, now */
/* the compute results so far, submitted and put aside to land later: the batch to wait for (0: none pending) */
u64 vk_compute_put_aside(void);
void vk_compute_wait(u64 value);                 /* until the compute batch is done (any thread, without vk_lock) */
void vk_compute_progress(u64* submitted, u64* done); /* the last batch and the last done (any thread) */
void vk_compute_land_through(u64 value);         /* the batches up to it, done, land (vk_lock) */
void vk_compute_before_write(u64 gva, u64 size); /* gpu.c's g_gpu_before_write */
void vk_compute_mem_need(u64 a, u64 n);          /* a read of [a, a + n): land results there first */
void vk_compute_mem_need_map(u64 a, u64 n);      /* the GMMU changes [a, a + n): land results there first */
int vk_compute_pending(u64 a, u64 n);            /* a pending result covers a guest page of [a, a + n) */
/* compute results recorded so far (pending_pages adds): a check without a hit holds while this is unchanged */
u64 vk_compute_adds(void);
/* a pending compute result covering [gva, gva + len): the arena buffer and offset to read it from on the GPU */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off);
extern VkSemaphore g_vk_csem;  /* timeline semaphore the compute queue signals */
extern u64 g_vk_cs_wait_value; /* the value graphics work waits for before reading compute results */
extern u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_wait_ns; /* statistics */
#if defined(SNX_TRACY)
extern u64 g_vk_cs_cpu_reads; /* dispatches kept on the compute queue because the CPU reads what they write */
/* uma.c: the device's memory model, and guest memory that the GPU reads in place (vk_memory) */
int vk_uma_choose(int has_import, int has_placed); /* before the device is made: 1 to enable external_memory_host */
void vk_uma_init(void);                            /* after it */
int vk_uma(void);                                  /* guest ranges are read in place where they can be */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
u64 vk_uma_bound(void); /* the submit that last read guest memory in place */
void vk_uma_frame(void);
void vk_uma_forget_all(void);

#endif
/* the dispatches skipped so far; why (size bytes) gets their reasons with each one's count, " (12 not translated, 3
   a texture not bound)", or "" when none was */
u64 vk_cs_skipped(char* why, size_t size);

/* render.c */
void vk_render_end(void); /* ends the dynamic rendering pass, if one is open */
/* the on-screen controls and the game's menu over a swapchain image in TRANSFER_DST_OPTIMAL, which it stays in;
   safe is the picture's part clear of the system's bars (gpu/vk/ui.c) */
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]);
void vk_ui_swapchain_gone(void); /* its images' views go before the swapchain does */
void vk_render_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o);
/* viewport index of a draw's state with the resolution scale, and the facing its first viewport gives */
VkViewport vk_viewport_from(const DsDynamic* d, const DsViewports* v, u32 index);
VkFrontFace vk_render_front_face(const DsDynamic* d, const DsViewports* v);
void vk_render_draw_texture(const DsState* ds, const DrawTexture* t);
void vk_render_discard(const DsState* ds, u32 targets);
void vk_render_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o);
/* a texture binding, for draws and compute: the view type a declaration takes, the view of t with the header's
   swizzle (VK_NULL_HANDLE when t is NULL or not of the number type num: 0 float, 1 unsigned, 2 signed integer), and
   the header's sampler with the declaration's comparison (integer textures unfiltered). The sampler's t and want,
   the texture whose own view is bound (NULL for none) and its view type, say whether anisotropy= may raise it, and
   with tic (unread when t is NULL) whether its view's format and swizzle take the TSC's min/max reduction */
VkImageViewType vk_tex_view_type(const ShTex* st);
VkImageView vk_tex_bind_view(Surf* t, VkImageViewType want, const u32* tic, const u32* tsc, int num);
VkSampler vk_tex_bind_sampler(const u32* tsc, const u32* tic, const ShTex* st, int num, const Surf* t,
                              VkImageViewType want);
/* Draws and compute key, read and bind their programs' resources alike, here, so that the two don't drift apart.
   vk_tex_num: the number type num a texture is declared with (ShaderInfo.tex_int_mask and tex_sint_mask, 0 float,
   1 unsigned, 2 signed) for its header tic, matching what vk_tex_bind_view binds. t is the surface the header resolves
   to, which can be a depth target of another type than the header's; NULL takes the type a surface made from the
   header has (tex_from_tic copies a target of another color format into the header's).
   Storage image stores are translated unsigned when bindless and float when bound (spirv_translate.c's image kinds).
   vk_image_keyable: whether slot i of an unkeyed program is one whose declaration follows its image, a 2D, 2D array or 3D
   store. Atomics need an unsigned image whatever the header says, and buffer images (bound by compute only) take a
   view in the format the unkeyed kind needs instead (cs_buffer_image). vk_image_key: whether such a slot's header
   names an image of the other class, so that the program is translated again with bit 8 + i of rt_int_mask set.
   Signed image stores also set bit 8 + i of rt_sint_mask. The header has the class of the surface
   vk_storage_image_view checks (tex_from_tic keeps it).
   vk_storage_image_view: the view storage image slot i of the keyed program si binds t through, or VK_NULL_HANDLE when
   it can't: the first selected level of a mip chain (a 2D view, every layer for a 2D array slot, or a 3D volume)
   that lands back in guest memory as it is: R32_UINT for atomics, an unsigned integer format for unsigned stores,
   a signed integer format for signed stores and a float format for float stores. Stores declare no shader format
   and require the device and view format to support that. *bytes covers the selected level of every layer,
   including the intervening mip data that writeback preserves.
   A draw also refuses its own attachments, which a dispatch, outside any rendering pass, can write.
   vk_gbuf_desc: a storage buffer's address (*addr) and size from its descriptor's words {address low, address high,
   size} at off (SH_GBUF_*), where gbuf_tag (spirv_translate.c) found it for stage (ST_VS, ST_FS or ST_CS).
   No size takes 16 bytes, and sizes are capped at 16 MiB and rounded up to words. The vertex stage's 0xb0 holds a
   bare 64-bit pointer with no size after it, 16 bytes (gbuf_tag tags it for the vertex stage alone). A bare address
   in a program's own constant buffer (gbuf_tag_any, v[2] unread) takes what is mapped from it on, to the cap. */
int vk_tex_num(const u32* tic, const Surf* t);
int vk_image_keyable(const ShaderInfo* si, int i);
int vk_image_key(const ShaderInfo* si, int i, const u32* tic);
VkImageView vk_storage_image_view(Surf* t, const ShaderInfo* si, int i, u64* bytes);
u32 vk_gbuf_desc(int stage, u32 off, const u32* v, u64* addr);
/* the access a storage image is moved to GENERAL with: image atomics read it as well as write it, and only the
   accesses a barrier names see the writes before it */
#define VK_STORAGE_IMAGE_ACCESS (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
extern u64 g_vk_drawn, g_vk_skipped; /* draws recorded, and draws skipped (unsupported topology, failed shader) */
/* draws that kept the last draw's shaders and pipeline key, and those of them SNX_DIRTY_CHECK made again to compare */
extern u64 g_vk_draw_kept, g_vk_draw_checked;
/* draws that sampled a color target they render to in place, the barriers ordering them within a pass, the passes
   begun again for them (a target's layout, or ordering without those barriers), and draws that sampled a target
   through a copy instead (gpu/vk/render.c) */
extern u64 g_vk_fb_draws, g_vk_fb_barriers, g_vk_fb_restarts, g_vk_rt_copies;
/* rendering passes begun; clears recorded as vkCmdClearAttachments; attachments a pass began by clearing (its load
   op) and by loading as DONT_CARE */
extern u64 g_vk_passes, g_vk_clear_cmds, g_vk_clear_loads, g_vk_dontcare_loads;

/* gpuprof.c, vk_gpu_profile=1: GPU time per rendering pass, summed per target and logged every 10 s */
/* the Tracy GPU track's contexts, one per queue: the graphics queue's render passes, the compute queue's batches */
#define VK_GPU_CTX_GRAPHICS 1
#define VK_GPU_CTX_COMPUTE  2
void vk_gpuprof_pass_begin(VkCommandBuffer cb, const Surf* rt, const Surf* zt);
void vk_gpuprof_pass_end(VkCommandBuffer cb);
/* why cmd.c submitted the frame's commands: a present, a vk_finish, the 1 ms of recording a submission holds at most
   (vk_frame_check), a readback of GPU-owned memory, the end of a frame's slot, or anything else */
enum { VK_SUB_OTHER, VK_SUB_PRESENT, VK_SUB_FINISH, VK_SUB_PERIODIC, VK_SUB_READBACK, VK_SUB_FRAME, VK_SUB_N };
/* what the GPU thread waits for the graphics timeline for: a vk_finish, a frame slot's previous use, or one submission
   (a release's gate, a result's landing: vk_wait_submit) */
enum { VK_WAIT_FINISH, VK_WAIT_RECYCLE, VK_WAIT_SUBMIT, VK_WAIT_N };
extern u64 g_vk_wait_ns[VK_WAIT_N], g_vk_waits[VK_WAIT_N]; /* cmd.c: the GPU thread's waits, by kind */
void vk_gpuprof_cb_begin(VkCommandBuffer cb);
void vk_gpuprof_cb_end(VkCommandBuffer cb, int why, u64 value);
void vk_gpuprof_waited(u64 value, int kind);
void vk_gpuprof_frame_done(u32 slot);
/* the device's timestamps next to the client's clock (VK_EXT_calibrated_timestamps with a host time domain) */
int vk_gpuprof_calibrated(void);
/* a fresh pair of the GPU's clock and the client's, for the track's calibration (0 without the extension) */
int vk_gpuprof_clock(i64* cpu, i64* gpu);
extern u64 g_rec_xfers, g_rec_barriers; /* record.c: copies and blits, and barriers, put in the frame's ring */
extern u64 g_rec_xfer_kind[3], g_rec_buf_bytes; /* buffer, buffer-image, image copies; buffer copies' bytes */

/* uma.c: the device's memory model, and guest memory that the GPU reads in place (vk_memory) */
int vk_uma_choose(int has_import, int has_placed); /* before the device is made: 1 to enable external_memory_host */
void vk_uma_init(void);                            /* after it */
int vk_uma(void);                                  /* guest ranges are read in place where they can be */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
u64 vk_uma_bound(void); /* the submit that last read guest memory in place */
void vk_uma_frame(void);
void vk_uma_forget_all(void);

#endif
