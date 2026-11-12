/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: shared state and the interfaces between its parts. Functions are loaded through volk from the
   loader the window backend gives (SDL's), or from libvulkan itself in headless runs (vk_init), so nothing links
   against a Vulkan library.

   device.c    instance, device, queues, swapchain, memory allocator (VMA)
   memory.c    the cap on one allocation, and images bigger than it bound in pieces (sparse binding); textures
               that guest memory maps in part, with memory only where it maps them (sparse residency); the device
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
   renderdoc.c RenderDoc captures of chosen frames (renderdoc) and of the capture key's
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
#include "nx/gpu/vk/coverage.h"
#include "nx/gpu/vk/sample_locations.h"
#include "nx/gpu/vk/advanced_blend.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "volk.h"
#include "common/gpu/vk/vk_common.h"
#include "vk_mem_alloc.h"
#include "nx/gpu/shader/shader.h"

#define VK_FRAMES          2  /* frames in flight */
#define VK_FRAME_RING_CAPS 4  /* a frame ends once it has written this many allocations to its rings (vk_frame_check) */
#define VK_POOL_FRAMES     8  /* the rings' pool keeps, per slot, the most chunks a frame of the last this many took */
#define VK_SC_MAX          16 /* swapchain images */
/* a transfer bigger than one allocation (g_vk.alloc_cap) goes in parts of at most this share of it: the rows a band
   or a readback part needs on both sides and in scratch fit the cap together (vk_band, surface.c) */
#define VK_PART_SHARE      4

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    /* the Vulkan version the renderer runs at: 1.3, or 1.2 with the extensions 1.3 took in (device.c g_vk12), their
       commands under the core names */
    u32 api;
    u32 api_cap;           /* the most it takes, 0 for 1.3: vk_api=1.2 runs the 1.2 way on a newer device */
    u32 driver_id;         /* VkDriverId: whose driver runs, which keeps a shader cache of its own (pipeline.c) */
    char driver_name[256]; /* its own name for itself (VkPhysicalDeviceDriverProperties) */
    u64 shader_generation; /* shader instruction invalidations, separate from memory epochs */
    int has_descriptor_buffers;
    int has_descriptor_heaps;
    int has_descriptor_heap_support; /* enabled device feature, retained during a draw's buffer fallback */
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_props;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptor_heap_props;
    VkPipelineLayout cs_playout;
    VkDescriptorSetLayout cs_dslayout;
    VkDevice dev;
    VmaAllocator vma;
    /* compute is a second queue of the graphics family, or of a compute-only family, when the device has one; else
       it is the graphics queue itself */
    u32 gfx_family, compute_family;
    VkQueue gfx, compute;
    u32 gfx_queues; /* the queues made of the graphics family (2 when compute is the second) */
    /* Vulkan Video's decode queue, of a family of its own (~0u without one), made when the vulkan_video setting asks
       for it (experimental), and the codecs it decodes of H.264 and AV1 (VK_VIDEO_CODEC_OPERATION_DECODE_*), each with
       its extension enabled: FFmpeg's Vulkan decoder decodes on it (vk/nvdec.c), told of the device by the extensions
       and features enabled */
    u32 video_family;
    VkVideoCodecOperationFlagsKHR video_codecs;
    const char* dev_ext[67]; /* create_device enables 67 at most, were every one it can enable there */
    u32 dev_ext_n;
    VkPhysicalDeviceFeatures2 feat2; /* feat, with the 1.1 to 1.3 features enabled chained (to 1.2 on a 1.2 device) */
    VkPhysicalDeviceVulkan11Features feat11;
    VkPhysicalDeviceVulkan12Features feat12;
    VkPhysicalDeviceVulkan13Features feat13; /* on a 1.2 device, what its extensions enabled of these */
    /* swapchain */
    VkSwapchainKHR swapchain;
    VkExtent2D sc_extent;
    int sc_win_w, sc_win_h; /* the window size it was made for (sc_extent: that as the surface takes it) */
    int sc_srgb;            /* its format is sRGB: what presented frames in sRGB formats are blitted to */
    u32 sc_space;           /* what its images take (gpu/hdr_output.h): SNX_HDR_SPACE_SRGB unless it was made for a
                               game's HDR frames, whose values the present's pass then writes as they are */
    u32 sc_made;            /* the swapchains made so far: what one was given (its HDR metadata) is the next one's
                               to be given again */
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
    int has_divisor;    /* VK_EXT_vertex_attribute_divisor: instance divisors above 1 */
    int astc_bc7_could; /* the GPU samples BCn but not ASTC: astc_mode=2 holds its ASTC textures as BC7 */
    /* the block formats the device samples, whether turned on or not (vk_blocks_want): VK_BLOCKS_* bits */
    u32 blocks_have;
    /* astc_mode's Auto holds the ASTC textures it decodes as BC7: the device started with a budget under
       SNX_ASTC_EXACT_MB (gpu/texture/astc_bc.h). vk_astc_bc7 says what the mode in effect holds them as */
    int astc_auto_bc7;
    /* ASTC held as BC7 is encoded as each texture loads, with no RGBA8 image before it: astc_mode=3, or its Auto for an
       Ounce program, as feat/ounce holds them (texture.c's tex_create) */
    int astc_at_load;
    /* VK_EXT_vertex_input_dynamic_state in effect (vertex_input.h's choice): each draw sets its vertex layout, which
       pipelines leave out of their keys */
    int vertex_input_dynamic;
    int has_dynamic_stride; /* a binding's stride is dynamic state, which MoltenVK refuses: a pipeline holds it then */
    /* a test's: linked pipelines are never optimized, so every draw binds the link (pipeline.c) */
    int gpl_hold;
    int has_vertex_robustness2; /* exact zero/default components for out-of-range mesh fetches */
    int has_calibrated_host;    /* VK_EXT_calibrated_timestamps with CLOCK_MONOTONIC_RAW: the GPU's clock goes next to
                                the client's, one driver call (gpuprof.c's calibration) */
    int has_portability;        /* a portability driver (MoltenVK): the device lists VK_KHR_portability_subset */
    int has_xfb;                /* VK_EXT_transform_feedback (without it, g_shader_xfb_stores) */
    int has_mixed_samples;      /* VK_NV_framebuffer_mixed_samples: coverage at more samples than color storage */
    /* VK_NV_device_diagnostic_checkpoints, enabled with vk_checkpoints=1 where the device has it: the draw or dispatch a
       GPU fault hangs on (record.c's vk_checkpoint_dump). has_checkpoints below is checkpoints.c's markers, on it too */
    int has_nv_checkpoints;
    int has_fault; /* VK_EXT_device_fault: the GPU's own report of a fault, at a lost device */
    int has_coverage_to_color, has_post_depth_coverage;
    VkSampleLocationCaps sample_locations;
    VkAdvancedBlendCaps advanced_blend_caps;
    int has_fragment_interlock; /* ordered pixel storage-image read/modify/write */
    int has_sample_interlock;
    int has_geometry_streams;
    int geometry_streams_lines_triangles;
    u32 image_slots[5], buffer_image_slots[5]; /* descriptor capacity by shader stage, after the pipeline-wide limit */
    int has_border_color;       /* VK_EXT_custom_border_color */
    u32 border_color_max;       /* maxCustomBorderColorSamplers: past it, samplers take the nearest built-in color */
    int has_mirror_clamp;       /* samplerMirrorClampToEdge: VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE */
    /* filterMinmaxImageComponentMapping: a min/max sampler reads a view of any swizzle. Without it the view's r must
       be the identity, and the other components are poison */
    int has_minmax_swizzle;
    int has_shader_draw_parameters; /* shaders take an instance's ID from BaseInstance (spirv_translate.c) */
    int has_draw_indirect_count;    /* drawIndirectCount: an indirect draw's count from the GPU's memory */
    u64 alloc_cap;                  /* the most one device memory allocation may be (memory.c) */
    u64 max_buffer_size;            /* the biggest buffer the device makes (maintenance4's maxBufferSize) */
    int sparse; /* sparseBinding on the graphics queue: images bigger than alloc_cap are bound in pieces */
    /* sparse residency of 2D or 3D images too, what isn't bound reading as zeros: a texture with holes in its mapping
       has memory only where it is mapped (memory.c vk_resident_shape says for which) */
    int resident;
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
    /* VK_EXT_mesh_shader's meshShader (and taskShader for has_task), with a required subgroup size of 32 in full
       subgroups for both stages: an Ounce's mesh draws (render.c). mesh_stages: the stage bits that the sets and the
       push constants of the graphics pipelines' layouts add for them (VK_PUSH_STAGES), 0 without */
    int has_mesh, has_task;
    /* compute pipelines can ask for subgroups of 32 (subgroupSizeControl and computeFullSubgroups, 32 within the
       sizes and compute among requiredSubgroupSizeStages): those of the programs that use other lanes' values do
       (compute.c's cs_make) */
    int has_cs_warp32;
    VkShaderStageFlags mesh_stages;
    u32 mesh_groups_max; /* the most workgroups a mesh draw launches: maxTaskWorkGroupCount[0], with maxMesh...'s */
    /* maxMeshOutputMemorySize and the granules that sm86_mesh_output_bytes counts vertices and primitives in */
    u32 mesh_output_bytes_max, mesh_vertex_granule, mesh_primitive_granule;
    /* VK_KHR_fragment_shader_barycentric's fragmentShaderBarycentric: a fragment program reads its inputs' values at
       the triangle's three vertices (render.c's draw_varying, the per-vertex pass) */
    int has_barycentric;
    /* VK_EXT_primitive_topology_list_restart: the guest's restart index restarts lists as it does strips (render.c),
       and patch lists with its patch list feature */
    int has_list_restart, has_patch_list_restart;
    /* VK_EXT_conservative_rasterization: conservative rasterization (CONSERVATIVE_RASTER, 0x452), dilated by at most
       conservative_extra pixels (maxExtraPrimitiveOverestimationSize) */
    int has_conservative;
    int has_depth_range_unrestricted; /* raw D32F copies retain every imported bit */
    float conservative_extra;
    /* VK_EXT_graphics_pipeline_library with fast linking (pipeline_parts.h's vk_gpl_choose): a new pipeline links
       parts of its shaders that compile once, and an optimized one is made after it (pipeline.c) */
    int has_gpl;
    u32 dyn_pr; /* VK_DYN_PR_*: the pre-rasterization state those pipelines take as dynamic state */
    /* VK_KHR_index_type_uint8 (or the EXT before it): 8-bit index buffers bound as they are */
    int has_index8;
    /* VK_KHR_present_id and VK_KHR_present_wait: presents carry ids, and display.c learns when each is displayed */
    int has_present_wait;
    /* VK_KHR_present_id2, VK_KHR_present_wait2 and VK_EXT_present_timing's presentAtAbsoluteTime (with
       VK_KHR_get_surface_capabilities2): a present can be held until a time, on a surface that says so (sc_hold) */
    int has_present_timing;
    int has_calibrated_mono; /* CLOCK_MONOTONIC among the calibrateable time domains */
    /* QueryPerformanceCounter among them (Windows), VK_EXT_calibrated_timestamps on for it: plat_ns's clock, which the
       GPU profile's idle times are put on there (gpuprof.c) */
    int has_calibrated_qpc;
    int has_surface_caps2;   /* the instance's VK_KHR_get_surface_capabilities2, which present timing is asked by */
    /* VK_EXT_swapchain_colorspace (the instance's) and VK_EXT_hdr_metadata (the device's): an HDR10 swapchain, and
       the light level its metadata names */
    int has_swapchain_colorspace;
    int has_hdr_metadata;
    /* A game is told the display is HDR, and its HDR frames get a swapchain of their own space: the hdr_game setting
       with both extensions on a surface that lists both HDR spaces (hdr_swapchain.h vk_hdr_display), as the last
       vk_swapchain_create found it. Atomic: the display session's thread reads it */
    int hdr_display;
    u32 gfx_timestamp_bits;  /* the graphics queue family's timestampValidBits: 0 where it writes no timestamps */
    /* VK_EXT_memory_budget: the driver tells each heap's use and budget (VMA reads them, and guesses without it) */
    int has_budget;
    int has_checkpoints; /* vk_checkpoints=1 on a driver that has them (checkpoints.c) */
    /* VK_GOOGLE_display_timing, enabled without present waits (Android): presents carry ids and times to be shown at,
       and the swapchain tells when each was */
    int has_display_timing;
    /* the device's memory (uma.c): VK_MEM_*, and how guest buffers reach it, VK_MEM_PATH_* (the vk_memory setting) */
    int mem_model, mem_path;
    char start_failure[600]; /* why vk_init found no device it runs on, with each device's Vulkan version */
} VkState;
enum { VK_MEM_SMALL_BAR, VK_MEM_REBAR, VK_MEM_UMA };
enum { VK_MEM_PATH_COPY, VK_MEM_PATH_UMA };
extern VkState g_vk;

/* the swapchain's display clock (sc_clock): none; present ids and waits; their second versions, which a swapchain made
   for VK_EXT_present_timing takes; VK_GOOGLE_display_timing's past presentation timings */
typedef enum { VK_CLOCK_NONE, VK_CLOCK_WAIT, VK_CLOCK_WAIT2, VK_CLOCK_GOOGLE } VkClock;

/* device.c */
int vk_init(void);
/* why vk_init found no device it runs on, with each device's Vulkan version; "" when it did not say */
const char* vk_start_failure(void);
/* how many of the extensions a Vulkan 1.2 device runs the renderer with (1.3's, device.c) e lacks, their names in out
   (size bytes, may be 0) comma separated */
u32 vk_1_2_missing(const SnxVkExtensions* e, char* out, size_t size);
/* the vk_api setting's text as the most Vulkan version the renderer takes, *cap (0 for auto, the device's up to 1.3):
   1, or 0 with why (n bytes) naming the value and the ones it takes */
int vk_api_setting(const char* text, u32* cap, char* why, size_t n);
/* (re)creates the swapchain at the window's size, of a kind: 8 bits a channel in a UNORM format, or in an sRGB one
   where the surface has one, or for a game's HDR10 or scRGB frames where hdr_display holds (else the UNORM one, said
   in the log), with the present mode the vsync setting asks for, recorded in sc_vsync (show() makes a new one when the
   setting moves). 0 while the window has no area (minimized) or its surface can't be queried, the old swapchain
   staying, or when the create failed: then there is none. A lost surface sets sc_lost; the next present tries again */
enum { VK_SC_UNORM, VK_SC_SRGB, VK_SC_PQ, VK_SC_SCRGB };
int vk_swapchain_create(int kind);
int vk_surface_recreate(void);
/* Renderer.release_window, for a fatal error's screen (sdl3.c): 1 when the swapchain and the surface are gone, 0 when a
   thread holding the renderer, a queue or the swapchain did not let go in time, and the window stays as it is */
int vk_window_release(void);
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
int vk_trylock(void);    /* vk_lock unless another thread holds it: 1 when taken */
u32 vk_lock_owner(void); /* the thread (plat_tid) holding vk_lock, 0 when none */
int vk_lock_held(void);  /* this thread holds vk_lock */

/* memory.c. vk_mem_init once the device exists, before the allocator (VMA's blocks are kept within the cap):
   sparse_queue is whether the graphics queue's family can bind sparse memory */
void vk_mem_init(int sparse_queue);
/* an image with its memory: one VMA allocation in mem, or, past g_vk.alloc_cap, pieces (mem stays VK_NULL_HANDLE).
   0 on failure. Destroyed with vk_image_destroy (vk_defer_image does that), never vmaDestroyImage */
int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem);
/* the same, with the result of the call that failed in res (VK_ERROR_OUT_OF_DEVICE_MEMORY: no memory for it) */
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, VkResult* res);
void vk_image_destroy(VkImage img, VmaAllocation mem);
/* An image with memory only where it is bound, what isn't reading as zeros (g_vk.resident). vk_resident_shape: how the
   device would bind an image made as ii that way (the texels of its blocks, its first level in the mip tail, its mip
   tails), 0 when it can't. vk_image_create_resident: such an image, of that shape (which it is refused without), with
   none bound; destroyed with vk_image_destroy. Its blocks, each a page, go layer by layer, in a layer level by level
   up to the mip tail, in a level x first, then y, then z; after them its mip tails, each a page bound whole.
   vk_image_resident binds memory to its pages whose want[] is set and releases it from the others (n: all of them),
   after the work recorded before, which it submits; the commands recorded after it see the new bindings, and the
   memory released goes once the GPU is done with it. A page that got memory has its want[] made 2: its texels are
   undefined until they are loaded. *bytes gets the memory bound. 0 when memory ran out: the pages that got memory
   are bound, the others as they were */
typedef struct {
    VkExtent3D block; /* the texels of a block */
    u32 tail_first;   /* the first level in the mip tail, the image's levels when there is none */
    u32 tails;        /* mip tails: 0, 1 for all the layers, or one a layer */
} VkResidentShape;
int vk_resident_shape(const VkImageCreateInfo* ii, VkResidentShape* s);
int vk_image_create_resident(const VkImageCreateInfo* ii, const VkResidentShape* s, VkImage* img);
int vk_image_resident(VkImage img, u8* want, u32 n, u64* bytes);
/* the sparse binds made since the last submission, to the graphics queue in one batch before it (cmd.c's submit). 1
   when the submission has to wait for one, which *wait then holds */
int vk_sparse_flush(VkSemaphoreSubmitInfo* wait);
/* img has binds in a batch that hasn't gone to the queue yet, which goes ahead of the next submission */
int vk_sparse_unissued(VkImage img);
/* Sparse residency (memory.c), for a texture that guest memory maps only in part. vk_sparse_supported: the device
   makes an image of ii's format, type and usage with sparse residency. vk_image_create_sparse: such an image, with no
   memory bound yet (destroyed by vk_image_destroy with no allocation). 0 when the device can't, or when the image is
   too small to save memory.
   Its sparse blocks are in the order of VkSparseShape: layer by layer, in a layer level by level up to the mip tail,
   in a level slice by slice, row by row, left to right. vk_image_sparse_bind binds memory to the blocks whose bit is
   set in bits (block b: bit b & 7 of bits[b >> 3]) and to the mip tail, which is bound always. It releases the memory
   of the other blocks. It gives -1 when memory ran out (nothing changed), 0 when nothing had to change, 1 when the
   binds changed, with the bytes bound now in *bytes. vk_image_sparse_need: the bytes that a bind of bits adds.
   vk_image_sparse_bound: a block of the level's texel rows y0 to y1 and slices z0 to z1 has memory (all: each block
   has), or the level is in the mip tail */
#define VK_SPARSE_LEVELS 16
typedef struct {
    VkExtent3D gran;                 /* a sparse block, in texels */
    u32 layers, levels, tail_first;  /* tail_first: the mip tail's first level, levels when there is none */
    u32 per_layer;                   /* the blocks of a layer */
    u32 first[VK_SPARSE_LEVELS + 1]; /* level l's first block in its layer */
    u32 bx[VK_SPARSE_LEVELS], by[VK_SPARSE_LEVELS], bz[VK_SPARSE_LEVELS]; /* level l's blocks across, down, deep */
} VkSparseShape;
int vk_sparse_supported(const VkImageCreateInfo* ii);
int vk_image_create_sparse(const VkImageCreateInfo* ii, VkImage* img);
const VkSparseShape* vk_image_sparse_shape(VkImage img); /* NULL for an image without sparse residency */
int vk_image_sparse_bind(VkImage img, const u8* bits, u64* bytes);
u64 vk_image_sparse_need(VkImage img, const u8* bits);
int vk_image_sparse_bound(VkImage img, u32 layer, u32 level, u32 y0, u32 y1, u32 z0, u32 z1, int all);
void vk_sparse_stats(u64* binds, u64* bind_ns); /* statistics: sparse binds, and the host's wait for them */
/* Tracy memory events for VMA allocations. */
#if defined(SNX_TRACY)
void vk_mem_track(VmaAllocation a);
void vk_mem_untrack(VmaAllocation a);
#else
static inline void vk_mem_track(VmaAllocation a) { (void)a; }
static inline void vk_mem_untrack(VmaAllocation a) { (void)a; }
#endif
/* uma.c: the device's memory model, and guest memory that the GPU reads in place (vk_memory) */
/* before the device is made: the extension to enable, VK_UMA_WITH_* (VK_EXT_external_memory_host, or Android's
   VK_ANDROID_external_memory_android_hardware_buffer) */
enum { VK_UMA_WITH_NONE, VK_UMA_WITH_HOST, VK_UMA_WITH_AHB };
int vk_uma_choose(int has_import, int has_ahb, int has_placed);
/* what vk_uma_choose takes for vk_memory want (0 auto, 1 copy, 2 UMA) and uma_memory how (0 auto, 1 imported only, 2
   moved only) on a device of model VK_MEM_*: VK_UMA_WITH_* */
int vk_uma_way(int want, int how, int model, int has_import, int has_ahb);
void vk_uma_init(void); /* after it */
int vk_uma(void);       /* guest ranges are read in place where they can be */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
u64 vk_uma_bound(void); /* the submit that last read guest memory in place */
void vk_uma_frame(void);
void vk_uma_forget_all(void);
void vk_uma_owning(u64 cpu, u64 size);        /* that guest memory becomes GPU-owned next, trapped (vk_own_publish) */
void vk_uma_mapping_begin(u64 cpu, u64 size); /* mem.c changes that guest memory next (size ~0: all) */
void vk_uma_mapping_end(void);                /* they changed: before anything else is submitted */
void vk_uma_before_write(u64 gva, u64 len);   /* the runtime writes GPU range [gva, gva + len) next (vk_lock held) */
/* Storage and transform feedback written in place: [gva, gva + len) bound where it is for a draw or dispatch to write,
   the write pending once the draw is recorded (vk_uma_writes_commit) until its submit is done. 0 when it is copied out
   as a pending result instead (buffer.c). vk_lock held */
int vk_uma_write(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
void vk_uma_writes_commit(void);                         /* the draw or dispatch was recorded */
void vk_uma_writes_abort(void);                          /* it wasn't */
void vk_uma_frame_done(u64 frame);                       /* that submit is done: its in-place writes are in memory */
void vk_uma_host_barrier(VkCommandBuffer cb, u64 frame); /* at that submit's end: its writes visible to the host */
int vk_uma_pending(u64 gva, u64 len);                    /* an in-place write is pending over GPU range [gva, +len) */
int vk_uma_pending_cpu(u64 cpu, u64 len);                /* the same by guest address */
u64 vk_uma_out_adds(void);                               /* in-place writes added so far, as vk_buf_adds counts */
/* the waits for a submit with an in-place write since the last statistics line (every 600 presents): a test tells by a
   change in it that a read waited, where a write the GPU finished meanwhile is no longer pending either */
u64 vk_uma_out_waits(void);
/* the runtime's CPU is about to read [gva, gva + len): the in-place writes pending there are done first (gmem_need's
   hook), unless the calling thread copies them on the GPU (vk_uma_gpu_reads) */
void vk_uma_mem_need(u64 gva, u64 len);
void vk_uma_mem_need_cpu(u64 cpu, u64 len); /* the same by guest address, whichever thread */
void vk_uma_gpu_reads(int on);
/* the import that guest memory [cpu, cpu + len) is written in place in, while a write there is pending: a GPU copy from
   it reads the write, after it (vk_lock held) */
int vk_uma_copy_pending(u64 cpu, u64 len, VkBuffer* buf, u64* off);
/* An engine's result (src at off) over guest memory [cpu, cpu + len) that an import holds already: copied there on the
   GPU in stream order, a write in place pending until its submit is done, with no trap and the import kept
   (vk_own_publish). 0 elsewhere: then it is published GPU-owned, as without UMA. vk_lock held */
int vk_uma_publish(u64 cpu, u64 len, VkBuffer src, u64 off);
int vk_uma_held(u64 cpu, u64 len); /* one import holds guest memory [cpu, cpu + len) now (vk_lock held) */
/* the first part of guest memory [cpu, cpu + len): up to where the import holding cpu ends (*held 1), or up to where
   the next import begins when none holds cpu (*held 0). 0 where it can't tell (no unified memory, placed memory).
   vk_lock held */
u64 vk_uma_import_part(u64 cpu, u64 len, int* held);
int vk_uma_gpu_write(u64 cpu, u64 len, VkBuffer* buf, u64* off);
void vk_uma_gpu_wrote(u64 cpu, u64 len);
/* placed maps (uma.c): whether the device can map the heap's windows as GPU memory where they are; turning it on
   (returning what it was), which takes the windows the heap and physical memory commit whole from then on; and
   whether one chunk of it holds [cpu, cpu + len) (vk_lock held) */
int vk_uma_can_place(void);
int vk_uma_placed(int on);
int vk_uma_placed_at(u64 cpu, u64 len);
/* whether the device exports a chunk as a dma-buf (VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf), told
   before it is made: a view of the heap (MapMemory's) then maps the chunk's pages again, else it is a copy */
void vk_uma_dmabuf(int has);
int vk_uma_can_view(void);
void vk_uma_settle(u64 ended);         /* a draw or dispatch begins: the ones before it recorded up to submit ended */
u32 vk_uma_imports(u64 cpu, u64 size); /* the imports the driver holds over guest memory [cpu, cpu + size) (vk_lock) */
/* the first import the driver holds over guest memory [cpu, end), on any thread: where it starts there, with where it
   ends in *hi (end at most); end for none. Write tracking leaves those pages alone */
u64 vk_uma_import_in(u64 cpu, u64 end, u64* hi);
/* guest memory moves into regions of buffers of their own (the migrate way, Android's) from now on, or with on 0 is
   imported where it is, the other way's imports let go first: 0 without guest memory in place. Tests run the migrate
   way off Android with memfd regions the driver imports as host memory (vk_lock) */
int vk_uma_migrate(int on);

/* The caches of textures, render targets and buffers are kept within the driver's budget for the heap images go to
   (VK_EXT_memory_budget through VMA; vk_budget_mb lowers it, for testing): past it the driver pages memory out,
   which took 160 ms frames and then the device in one game. Ages are kept in presents, and measured in time as well
   as presents, the frame rate being anything from 5 to 144 a second.
   vk_mem_use: that heap's use as the caches count it, and its budget. vk_mem_present: at each present, after
   g_vk_present moved. vk_present_before: the present before which an entry last used then has gone unused for at
   least ago_ns and at least `presents` presents (0: none has) */
void vk_mem_use(u64* usage, u64* budget);
int vk_mem_heap(void); /* that heap's index */
u64 vk_mem_budget_cap(u64 bytes); /* vk_budget_mb's cap set to bytes (0: none), the one it had back */
/* that heap's use by this process and its budget as the driver tells them (VK_EXT_memory_budget), for the stats box;
   0 and 0 without the extension */
void vk_mem_driver_use(u64* usage, u64* budget);
void vk_mem_present(void);
u32 vk_present_before(u64 ago_ns, u32 presents);

/* cmd.c */
void vk_cmd_init(void);
VkCommandBuffer vk_cmd(void); /* the current frame's command buffer, begun on first use */
/* The current submission's upload command buffer, begun on first use: the submission runs it before the frame's
   (vk_cmd), and its copies before any command recorded there, the rendering pass open now and the draws before it
   too, so they need not end that pass. For copies whose sources were written before the submission and whose
   destinations nothing in it has used: the CPU's bytes in staging (flushed at the submit) into a new buffer, say. gpu:
   their sources are also what earlier submissions wrote on the GPU, which they wait for. The submission's commands
   wait for every copy recorded here */
VkCommandBuffer vk_cmd_upload(int gpu);
VkCommandBuffer vk_cmd_upload_recording(void); /* that command buffer while it is begun, else VK_NULL_HANDLE */
/* size bytes of the current frame's mapped staging memory (flushed when the frame is submitted), valid until
   that frame's work is done. NULL past g_vk.alloc_cap: a big upload stages in parts */
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off);
/* the most a request to a frame ring takes out of a pooled chunk: a bigger one gets a pooled big buffer of its size
   class */
u64 vk_stage_chunk(void);
/* Streamed buffer snapshots in host RAM, with the same frame lifetime as vk_stage. */
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off);
/* One-use guest input: host staging for CPU-only bytes, output staging when GPU-owned or in-place writes cover it. */
u8* vk_stage_guest_input(u64 gva, u64 source_size, u64 bytes, VkBuffer* buf, u64* off);
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
/* the commands recorded since the last submission submitted, when a command buffer is begun: what a change of an
   image's sparse binding has to follow (memory.c) */
void vk_submit_recorded(void);
void vk_submit_upload_wait(void); /* the same, and waits for the GPU, landing nothing: an upload's own pace */
/* vk_cmd() returns without waiting: the frame slot is recording, or the GPU is done with its last use */
int vk_cmd_ready(void);
/* vk_checkpoints=1: what the GPU reached when the device was lost (checkpoints.c) */
enum {
    VK_CK_ASTC,
    VK_CK_BC7,
    VK_CK_COPY,
    VK_CK_STRIP_DONE,
    VK_CK_DESWIZZLE,
    VK_CK_CONVERT,
    VK_CK_DISPATCH,
    VK_CK_PASS,
    VK_CK_SUBMIT,
    VK_CK_KINDS
};
void vk_ck_init(void);
void vk_ck_(VkCommandBuffer cb, u32 kind, u32 a, u32 b, u32 c, u32 d);
void vk_ck_report(void); /* logs them, once: before the fatal error of a lost device */
#define vk_ck(cb, kind, a, b, c, d)                                                                                    \
    do {                                                                                                               \
        if (g_vk.has_checkpoints) vk_ck_((cb), (kind), (a), (b), (c), (d));                                            \
    } while (0)
/* the next submission waits for a timeline semaphore of someone else's (FFmpeg's picture's) at one value and signals
   it at another: whichever submit comes next takes them, so commands recorded from now on never run before the wait */
void vk_submit_after(VkSemaphore sem, u64 wait, u64 signal);
/* submits and waits until the GPU has done everything, and lands every result. The caller's file and line count the
   finishes it made, which the stats every 600 presents name (vk_finishes_log), by their number and their wait */
void vk_finish_at(const char* file, int line);
#define vk_finish() vk_finish_at(__FILE__, __LINE__)
void vk_finishes_log(void);
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
void vk_frame_check(void); /* at the start of a draw or dispatch, between a copy's bands: ends a frame grown too big */
u64 vk_ring_bytes(void);                  /* the rings' memory: the slots' chunks and big buffers, the pool's */
u64 vk_ring_made(void);                   /* the rings' chunks and big buffers made since the start */
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
u64 vk_last_submit(void);          /* the last submission's number, which its completion signals */
VkSemaphore vk_gfx_timeline(void); /* the timeline those numbers are values of: another queue can wait for one */
void vk_submit_progress(u64* submitted, u64* done); /* the last submission and the last done (any thread) */
void vk_wait_submit(u64 value); /* until the GPU has done that submission (any thread, without vk_lock) */
/* until sem reaches value, in slices: a GPU whose semaphore stops moving for 15 s ends the run with its hang logged
   (cmd.c), where a wait for good froze the game */
void vk_wait_semaphore(VkSemaphore sem, u64 value);
/* until fence is signaled, as vk_wait_semaphore waits: 0 when the device was lost */
int vk_wait_fence(VkFence fence);
/* Flush recorded work without frame recycling or guest result callbacks, under
   vk_lock, before an ownership readback uses the graphics queue directly. */
void vk_submit_for_readback(void);
/* the same without waiting for the recording thread: the commands being recorded, if any, handed to it (vk_lock) */
void vk_submit_open(void);
/* the commands being recorded, if any, handed to the GPU as a submission of their own between two pieces of one long
   piece of work (astc.c's strips), so that no submission runs past the driver's job timeout. Nothing lands and the
   frame keeps its slot, so a draw's or a dispatch's setup can call it (they set themselves up again after a submit).
   The caller records the rest in the next vk_cmd, a new command buffer */
void vk_submit_split(void);
/* the same for a piece of a background encode, after a present's submission (astc.c), counted apart */
void vk_submit_background(void);
/* the renderer's ready (renderer.h): the GPU's work on the slot the next frame records in, waited for without vk_lock
   (none when this thread holds it) */
void vk_frame_ready(void);
void vk_land_through(u64 value); /* the results of the submissions up to value, done, into guest memory (vk_lock) */
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to);
/* a descriptor set from the current frame's pools, freed when the frame is reused */
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout);
/* a pool for 1024 sets of the stage or compute layouts, array_sets of them with the texture arrays (vk_pool_sizes). A
   set a frame's pool can't hold goes to the frame's next pool, made for VK_POOL_ARRAY_SETS of them, or for
   VK_POOL_ARRAY_SETS_MORE when a set with the arrays is what didn't fit */
VkDescriptorPool vk_desc_pool_new(u32 array_sets);
#define VK_POOL_ARRAY_SETS             64
#define VK_POOL_ARRAY_SETS_MORE        256
#define VK_POOL_ARRAY_SETS_FOR(layout) (vk_dslayout_arrays(layout) ? VK_POOL_ARRAY_SETS_MORE : VK_POOL_ARRAY_SETS)
u32 vk_desc_pools(void);                 /* the pools that exist, made on any thread */
void vk_desc_taken(VkDescriptorSetLayout layout, u32 n); /* n sets of layout taken from a frame's pools */
u64 vk_desc_array_sets(void);                            /* the sets of the arrays layouts taken so far */
int vk_dslayout_arrays(VkDescriptorSetLayout layout);    /* one of the stage layouts with the texture arrays */
/* each heap's use and budget and the descriptor pools, as text for a line: any thread, also one that never calls
   Vulkan */
void vk_memory_text(char* out, size_t size);
u64 vk_memory_blocks(void);              /* the bytes of VMA's blocks in all heaps, any thread */
u32 vk_frame_slot(void);                 /* the frame slot being recorded */
/* A descriptor pool's sizes (VK_POOL_TYPES entries) for `sets` stage or auxiliary sets, array_sets of them with the
   texture arrays (gpu/vk/pipeline.c). */
#define VK_POOL_TYPES 6 /* uniform/storage buffers, combined textures, storage images/texels, sampled images */
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets, u32 array_sets);
extern u32 g_vk_buffer_image_slots; /* storage texel buffer bindings in a set, from SNX_VKB_IMGBUF */
VkDescriptorType vk_cbuf_type(u32 slot); /* the descriptor type of a constant buffer slot (g_shader_uniform_cbufs) */
/* A buffer image: the texels a buffer header describes, bound as a storage texel buffer over their copy. Raw loads
   and atomics view it as R32_UINT, as the console reads it for them whatever its format. Formatted loads and stores
   see the format itself, of the number type the program was keyed on (kind 0 float, 2 unsigned, 3 signed); unkeyed,
   a 32-bit float one is viewed as the integer format of the same bits, which register bits match. 0 for a header
   that isn't one, or a format this GPU can't load and store so */
typedef struct {
    u64 gva;
    u32 bytes;
    VkFormat fmt;
} VkBufImg;
int vk_buffer_image(const u32* tic, int kind, VkBufImg* out);
/* the staging alignment that suits every use: uniform, storage, vertex and index buffers */
u64 vk_stage_align(void);
u64 vk_hash(const void* p, u64 size);
void vk_hash128(const void* p, u64 size, u64 hash[2]);
u64 vk_hash_copy(void* dst, const void* src, u64 size); /* vk_hash(src, size), with src copied into dst on the way */
/* a hash of guest memory: all of it when small, else its ends and 62 samples between them */
u64 vk_sample_hash(u64 gva, u64 len);
/* frames presented: what the caches check memory against once a frame, beside the write epoch (vk_ww_epoch) */
extern u32 g_vk_present;

/* record.c: the frame's command buffer is recorded on a thread of its own. rec_Cmd* take vkCmd*'s arguments and
   record into that command buffer from the ring, or into any other one at once */
void vk_rec_init(void);
int vk_rec_threaded(void); /* the frame's commands go through the recording thread */
u64 vk_rec_behind(void);   /* bytes of commands handed to the recording thread and not recorded yet (any thread) */
u64 vk_rec_gfx_queued(void); /* the graphics timeline value of the last submission on the queue (any thread) */
void vk_rec_begin(VkCommandBuffer cb); /* begins cb, the frame's command buffer from now on */
void vk_rec_begin_upload(VkCommandBuffer cb); /* begins cb, the submission's upload command buffer (vk_cmd_upload) */
/* ends and submits up (the upload command buffer, VK_NULL_HANDLE for none) and then cb with those semaphores (s[0]
   the graphics timeline's) */
void vk_rec_submit(VkCommandBuffer up, VkCommandBuffer cb, const VkSemaphoreSubmitInfo* w, u32 nw,
                   const VkSemaphoreSubmitInfo* s, u32 ns);
void vk_rec_sync(void);       /* until everything queued is recorded and submitted */
/* vk_rec_sync's wait, given up at until (plat_ns; VK_REC_FOREVER: never): 1 when the ring drained, 0 when it had not by
   then. The window's release for a fatal error waits this way (vk_window_release): an error or a crash on the
   recording thread stops it inside a packet for good, and the ring never drains */
#define VK_REC_FOREVER (~0ull)
int vk_rec_sync_until(u64 until);
/* Runs fn(arg) on the recording thread, after what the ring already holds, and returns when it has run. This is how a
   check puts a call where the fatal path calls from (submit_run): vk_rec_sync there must not wait for the thread it
   runs on (tests/vulkan_headless.c's check_recorder_self_sync). A call from the recording thread itself returns at
   once (vk_rec_sync), so fn would run after it: the caller is another thread */
void vk_rec_run(void (*fn)(void*), void* arg);
void vk_rec_queue(VkQueue q); /* before submitting to or presenting on q outside the ring (vk_lock) */
/* fn with a copy of size bytes of arg (a quarter of the ring at most), on the recording thread in order with the
   commands, submissions and presents queued around it (vk_lock); at once without that thread */
void vk_rec_call(void (*fn)(const void* arg), const void* arg, u64 size);
/* Around every call on the graphics or the compute queue: a submit, a sparse bind, a present, a device wait. Vulkan
   requires a queue's calls to be externally synchronized, and they come from more than one thread: the recording
   thread submits and presents the frames and binds sparse memory (record.c, memory.c) without vk_lock, which the GPU
   thread holds while it submits compute. A leaf lock: taken with vk_lock held or not, never the other way */
void vk_queue_lock(VkQueue q);
int vk_queue_trylock(VkQueue q); /* vk_queue_lock unless another thread holds it: 1 when taken */
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
void rec_CmdDrawIndexed(VkCommandBuffer cb, u32 count, u32 instances, u32 first, i32 vertex_offset, u32 first_instance);
void rec_CmdDrawMeshTasks(VkCommandBuffer cb, u32 groups); /* vkCmdDrawMeshTasksEXT(groups, 1, 1) */
/* vkCmdDrawMeshTasksIndirectEXT of the one record at off */
void rec_CmdDrawMeshTasksIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off);
/* the programs of the draws recorded next (the vertex or mesh program's hash, the fragment program's), which their
   checkpoints name in vk_checkpoint_dump */
void rec_draw_tag(u64 first, u64 second);
/* max draws of the arguments at buf + off, stride bytes apart (vkCmdDraw[Indexed]Indirect), or with count_buf, as
   many as the 32-bit count at count_buf + count_off says (their Count forms) */
void rec_CmdDrawIndirect(VkCommandBuffer cb, int indexed, VkBuffer buf, VkDeviceSize off, VkBuffer count_buf,
                         VkDeviceSize count_off, u32 max, u32 stride);
void rec_CmdDispatch(VkCommandBuffer cb, u32 x, u32 y, u32 z);
void rec_CmdSetCheckpointNV(VkCommandBuffer cb, const void* marker);
void rec_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off);
void rec_CmdDispatchKey(VkCommandBuffer cb, u32 x, u32 y, u32 z, u64 key, u64 addr); /* the game's: names the shader in a fault */
void rec_CmdDispatchIndirectKey(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, u64 key, u64 addr);
void rec_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, u32 data);
/* NV diagnostic checkpoints: a hang or device-lost names the last draw or dispatch the GPU reached (record.c sets one
   before each). Reads and logs both queues' checkpoint markers */
void vk_checkpoint_dump(const char* why);
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
/* the pre-rasterization state pipelines of libraries take as dynamic state (g_vk.dyn_pr) */
void rec_CmdSetPolygonModeEXT(VkCommandBuffer cb, VkPolygonMode mode);
void rec_CmdSetDepthClampEnableEXT(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetProvokingVertexModeEXT(VkCommandBuffer cb, VkProvokingVertexModeEXT mode);
void rec_CmdSetConservativeRasterizationModeEXT(VkCommandBuffer cb, VkConservativeRasterizationModeEXT mode);
void rec_CmdSetExtraPrimitiveOverestimationSizeEXT(VkCommandBuffer cb, float size);
void rec_CmdSetPatchControlPointsEXT(VkCommandBuffer cb, u32 points);
void rec_CmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer cb, VkBool32 on);
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
/* descriptor sets bound so far (rec_CmdBindDescriptorSets): one bound for any pipeline unbinds the descriptor buffers
   the draws' sets are in (descriptors.c), so a draw after it binds them again */
u64 vk_rec_set_binds(void);
void rec_CmdBindVertexBuffers2(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs,
                               const VkDeviceSize* sizes, const VkDeviceSize* strides);
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs);
void rec_CmdSetVertexInputEXT(VkCommandBuffer cb, u32 nb, const VkVertexInputBindingDescription2EXT* b, u32 na,
                              const VkVertexInputAttributeDescription2EXT* a);
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
/* g_vk.blocks_have's bits */
enum { VK_BLOCKS_ASTC = 1, VK_BLOCKS_ETC2 = 2, VK_BLOCKS_BC = 4 };
/* the block formats' features to turn on of those the device has (have): BCn wherever it has them, ASTC LDR and ETC2
   where native_astc and native_etc2 ask as well */
void vk_blocks_want(const VkPhysicalDeviceFeatures* have, VkPhysicalDeviceFeatures* want);
/* the log's line for each family of block textures (ASTC, ETC2 and EAC, BCn): sampled as the games' blocks, encoded
   again on the GPU, or decoded, and into what; a warning where a setting asks for what the GPU lacks. Once the device
   is made and astc_mode chosen */
void vk_blocks_say(void);
/* the ASTC textures the GPU decodes are held as BC7: astc_mode's BC7, or its Auto on a device that started with little
   memory (gpu/texture/astc_bc.h) */
int vk_astc_bc7(void);
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
/* the most levels a texture has: a texture header's count of them is 4 bits */
#define VK_TEX_MAX_LEVELS 16
typedef struct Surf {
    u64 gva;
    u32 w, h, layers, levels;
    u32 sw, sh;  /* the image's size: render targets are w x h times the resolution scale */
    u32 depth3d; /* 3D textures: depth (layers is 1) */
    u32 fkey;    /* render targets: guest format | 0x10000 for depth. Textures: TIC word 0's format bits (0x7FFFF), sRGB
                    in bit 31, raw (vk_tex_from_tic_raw) in bit 30 */
    VkFmt f;
    u32 bl, bh, bd, pitch; /* guest layout: block linear with block height 2^bh GOBs, or pitch linear */
    u32 tile_w;            /* a sparse texture's tile width (log2 GOBs): its levels at least a tile in size have rows
                              of whole tiles (the header's TILE_WIDTH_IN_GOBS) */
    u64 layer_size;        /* bytes from one layer to the next in guest memory */
    u64 rt_size;           /* a render target's guest bytes, all layers (its shape is fixed when it is made) */
    int is_rt;
    int storage_capable;  /* image was created with VK_IMAGE_USAGE_STORAGE_BIT */
    int sample_locations_compatible; /* depth image carries sample-location layout metadata */
    VkSampleLocations sample_locations;
    int volume;           /* render target layers use 3D block-linear memory rather than array strides */
    u8 msaa;              /* DS_MSAA_: a multisampled image's sample layout. w and h count its pixels, the guest's
                             memory its samples (w << ds_msaa_log2_x(msaa) by h << ds_msaa_log2_y(msaa) texels) */
    u8 ms_raw;            /* multisampled: its samples move to and from guest memory (gpu/vk/msaa.c) */
    VkImageViewType type; /* what it was created as */
    VkImage img;
    VmaAllocation mem;
    VkImageView att;        /* level 0: the attachment */
    VkImageView att_linear; /* same storage without sRGB conversion */
    VkImageView present;    /* level 0's layer 0 as its bytes (its sRGB twin's UNORM), sampled: the window's present
                               (present_quad.c), made at its first */
    u32 present_serial;     /* that view's number among all made, which a set holding it is known by: unlike its
                               handle, never another view's */
    VkImageLayout layout;   /* the whole image is kept in one layout */
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 last_used;
    u64 touched; /* when it last went to the recent end of its recency list: ties of last_used in the lists' order */
    u32 wgen;    /* render targets: bumped when drawn to */
    u64 wseq;    /* when its image last changed, counted across surfaces: of two over the same bytes, the higher
                       holds them (vk_surf_wrote) */
    u64 discard_wseq; /* a render target discarded (vk_render_discard) at this wseq: its next pass loads it as
                         DONT_CARE while nothing wrote it since */
    u8 discarded;
    u32 drawn_at;  /* render targets: g_vk_present when last drawn to */
    u8 compared;   /* depth targets: a draw sampled it with a compare while shadows=0 (render.c shadow_map_sampled) */
    u32 compared_at; /* g_vk_present at the last such sample */
    u64 draw_wseq; /* render targets: wseq at its last draw or clear (vk_rt_drawn), 0 before one */
    int in_drawn;  /* in surface.c's list of drawn targets, entered at drawn_from */
    int take_seen; /* render targets: vk_rt_drawn_in gave it to a texture's search since its last write */
    int in_linear; /* a pitch-linear target in surface.c's list of those drawn since the last release */
    int in_bl;     /* a block linear target in surface.c's other list of those */
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
    int piecewise;    /* textures: holes in its mapping when made, loaded a piece at a time (texture.c tex_patch) */
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
    /* storage images: the serial of the owner the last publish put over its texels (vk_surf_publish), 0 before one.
       Bytes that owner still holds are the image's own (texture.c pieces_published) */
    u64 pub_serial;
    /* textures: vk_rt_take_gen() and wgen when the search for drawn targets over it (texture.c tex_take_drawn) last left
       nothing it could take: it searches again only once either moved */
    u64 take_gen;
    u32 take_wgen;
    /* textures whose image has memory only where they are mapped (vk_image_create_resident): in each layer, bricks of
       grain texels, each brick_bytes of the guest's layout in one range, level by level up to tail_first, x first,
       then y, then z; bricks of them a layer, 0 for every other image. The levels from tail_first on are in mip tails,
       tails of them: 0, 1 for all the layers, or one a layer */
    u32 bricks, brick_bytes, tail_first, tails;
    VkExtent3D grain;
    /* textures: an image with sparse residency, memory bound only for the levels that guest memory maps
       (gpu/vk/texture.c tex_bind). mem is VK_NULL_HANDLE, and bytes counts the memory bound now */
    u8 sparse;
    /* depth targets: undefined but for draws and clears predicated on the GPU, which may not have run, so they count
       as no draw (render.c zt_written) */
    int pred_undefined;
    /* textures: an ASTC one held as BC7 whose image is RGBA8 until the BC7 encoder has made its BC7 one between frames
       (texture.c vk_tex_later_frame), and again each time the game writes it whole */
    u8 bc7_later;
    /* held Exact for its writes (texture.c later_wrote): the game wrote it again before the encoder could have made its
       BC7 image, so it stays RGBA8 until it goes unwritten a while (later_cool); and the present it was last written
       in, plus one (0: never) */
    u8 bc7_hot;
    u32 bc7_wrote;
    /* that BC7 image while it is made (VK_NULL_HANDLE before the encoder starts on it), its memory, and how far the
       encoder got: level, then slice (a layer, or a 3D level's depth slice) and row of ASTC blocks, from the blocks
       kept of write generation wgen. queued: in the encoder's queue */
    struct {
        VkImage img;
        VmaAllocation mem;
        u64 bytes;
        u32 level, slice, row, wgen;
        int queued;
        /* the ASTC blocks the RGBA8 image was decoded from, kept on the GPU while the texture is RGBA8 (astc.c's
           vk_astc_keep): what the BC7 image is made from, as an upload straight to BC7 makes it */
        VkBuffer kept;
        VmaAllocation kept_mem;
        u64 kept_bytes;
        /* the BC7 cache's key of each level (keyed: a bit a level), from the guest bytes the RGBA8 image was loaded
           from: the encoder's levels are read back into the cache, and a level the cache has is copied from it */
        u32 keyed;
        u64 key[VK_TEX_MAX_LEVELS][2];
    } bc7;
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
int vk_rt_same_layout(const Surf* s, u32 w, u32 h, int bl, u32 bh, u32 pitch); /* a header with s's pixels */
/* a copy of a target, for a draw that samples it while writing it. pixels: the target's pixels are brought into the
   copy; 0 for a program that only asks the texture's size or levels, which takes the copy as it is */
Surf* vk_rt_snapshot(Surf* rt, int pixels);
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
/* A draw's storage image kept on the GPU (vk_surf_keep_begin): held, the owner of its span from the bind on, with a
   reference, its bytes from before the draw from off on; made: the bind made it, the CPU staging the span */
typedef struct {
    struct VkOwnRes* held;
    u64 off;
    int made;
} VkImageKeep;
/* a draw's writes to s's storage image, size bytes, to write back over original, s's bytes from before the draw
   (the queue frees it), or over the bytes the write-back after leaves; or kept (keep, its held reference the
   queue's), published at the release */
void vk_gfx_image_record(Surf* s, u8* original, u32 size, u64 after, const VkImageKeep* keep);
/* statistics: guest bytes copied on the CPU for draws' storage images to merge with (the bytes from before a draw),
   their bytes copied back from the GPU to land, and the images published over their bytes GPU-owned instead, since the
   start; the report LOGs them since the last one */
void vk_gfx_image_stats(u64* copied, u64* read_back, u64* published);
void vk_gfx_image_report(void);
/* the write-back a draw's writes to s go on from (its landed bytes become their original, NULL until then), 0: none */
u64 vk_gfx_image_after(Surf* s, u32 size);
int vk_gfx_image_unrecorded(const Surf* s); /* s has a write-back whose copy isn't recorded: a draw's writes join it */
void vk_gfx_image_mem_need(u64 gva, u64 size);
void vk_gfx_image_guest_need(u64 cpu, u64 size); /* by guest memory (size ~0: all of it) */
int vk_gfx_image_pending(u64 gva, u64 size);
void vk_gfx_image_land(void);
/* at a release: the storage images' write-backs recorded, to land once their submit is done (vk_lock) */
void vk_gfx_image_put_aside(void);
/* a recorded write-back waits to land (unsubmitted: in the submit being recorded) */
int vk_gfx_image_in_flight(int unsubmitted);
/* the submissions up to sub are done: the write-backs recorded in them land (cmd.c's landing, vk_lock) */
void vk_gfx_image_frame_done(u64 sub);
int vk_gfx_image_holds(const Surf* s); /* s has a storage result waiting to land in memory */
/* depth_pack.c: a zeta format's packing of depth and stencil, -1 for one that doesn't pack; layer of the packed target
   s (unscaled, single-sampled, in TRANSFER_SRC_OPTIMAL) into buf at at in the guest's layout, rows pitch bytes apart:
   1, or 0 when it can't */
int vk_depth_pack_mode(u32 zeta);
int vk_depth_pack(VkCommandBuffer cb, const Surf* s, u32 layer, VkBuffer buf, u64 at, u32 pitch);
u64 vk_surf_guest_size(const Surf* s);
int vk_surf_can_writeback(const Surf* s);
void vk_surf_flush(Surf* s, const u8* original); /* original preserves newer guest writes for storage images */
/* s's pixels published over its guest bytes, GPU-owned and not waited for: 1, or 0 when only vk_surf_flush can */
int vk_surf_publish(Surf* s);
/* a draw's storage image s kept on the GPU until the CPU touches its bytes, its span GPU-owned from the bind on: 1
   with k filled, 0 when it takes the write-back (surface.c says when). vk_surf_keep_end, at the release or when
   something needs its bytes sooner, takes k's reference: 1 when s is published, its span untouched since the bind;
   0 when the CPU touched it, *original then the bytes from before the draw for the write-back's merge (malloc'd, or
   NULL when they couldn't be read back) */
int vk_surf_keep_begin(Surf* s, VkImageKeep* k);
int vk_surf_keep_end(Surf* s, const VkImageKeep* k, u8** original);
/* a release: the pitch-linear color targets drawn since the last one published, for the CPU to read. The block linear
   color targets drawn since then, which the CPU reads only now and then, are watched (own_watch): the CPU's first
   access publishes them (vk_rt_watch_hit). One the CPU read lately is published at once, as the pitch-linear ones */
void vk_rt_publish_drawn(void);
/* own.c's resolve: the CPU touched watched guest memory [cpu, cpu + size): the targets pending there are published
   over it (vk_lock held, by a thread that did not hold it before) */
void vk_rt_watch_hit(u64 cpu, u64 size);
/* the watches that releases armed over targets, the CPU accesses that published them, and the targets that releases
   published at once because the CPU read them lately: the statistics, and the tests */
typedef struct {
    u64 arms, hits, publishes;
} VkRtWatchStats;
extern VkRtWatchStats g_vk_rt_watch;
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
/* keep: when non-NULL (and original is), the image's bytes as the landing leaves them, malloc'd, keep_size of them */
void vk_surf_flush_land(Surf* s, const u8* p, const u8* original, u64 wgen, u8** keep, u64* keep_size);
void vk_tex_note_writeback(Surf* s);
/* A DMA copy's rows of block linear destination gva (wb bytes a row, blocks 2^bh GOBs high), line_bytes of each from
   byte ox of row oy on, as they are at rows_at in buf (rows_pitch bytes apart): copied into each texture whose level 0
   is that surface, so its image keeps them without loading the whole texture again (the console's copy changes
   those bytes only). A texture qualifies when no engine wrote its memory since its last check (what it lacks would
   be lost), and it is 2D, of one sample, and loaded as memory holds it. Into hit (max of them), for vk_tex_dma_seen
   once the copy's write is published; how many */
int vk_tex_dma_rows(u64 gva, u32 wb, u32 bh, u32 ox, u32 oy, u32 line_bytes, u32 rows, VkBuffer buf, u64 rows_at,
                    u32 rows_pitch, Surf** hit, int max);
void vk_tex_dma_seen(Surf** hit, int n); /* their memory as of now is the copy's, which they hold */
void vk_rt_drawn(Surf* s);               /* a render target was drawn to (gpu/vk/render.c) */
void vk_rt_drawn_full(Surf* s);          /* a full clear or blit replaced every pixel */
int vk_rt_drawn_fresh(Surf* s);          /* no guest-memory write has replaced the target since its last draw */
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max); /* count drawn in [lo, hi), write up to max */
/* a count that moves whenever what vk_rt_drawn_in returns, or the pixels of a target it returned, can have changed */
u64 vk_rt_take_gen(void);
void vk_rt_reload(u64 gva);
/* textures share the surface table. g_vk_surf_epoch changes whenever a surface enters it, leaves it or moves (a
   target parked or taken back), so what was found there can be kept until it changes */
extern u32 g_vk_surf_epoch;
u32 vk_surf_epoch_at(u64 gva); /* the same, for the surfaces of gva's bucket of the table only */
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
/* the device memory a surface of the caches takes now: an image whose memory follows its mapping grows and shrinks */
void vk_surf_bytes(Surf* s, u64 bytes);
int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy);
int vk_blit2d_filter(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                     double sdy, int linear);

/* blocklinear.c: a block linear surface's rows moved between its layout and linear rows on the GPU
   (gpu/texture/blocklinear.comp). The surface is row_bytes wide (texels or compressed blocks times their bytes),
   height rows high, in blocks of 2^bh GOBs high and 2^bd deep, already shrunk for small surfaces as the hardware
   does; the move takes rows y0 to y0 + rows of slices z0 to z0 + slices. Its linear rows are lin_pitch bytes apart
   (a multiple of 16, at least row_bytes rounded up to 16: whole 16 byte pieces move, so a row's last one carries
   padding along), a slice's rows after another's. gobs_x: the GOBs across a row of blocks when there are more than
   row_bytes takes (a sparse texture's rows of whole tiles, or some of a row's GOB columns, the move's bl_at then
   at the first one's block), else 0 */
typedef struct {
    u32 row_bytes, height, bh, bd;
    u32 y0, rows, z0, slices;
    u32 lin_pitch;
    u32 gobs_x;
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
   g_vk.alloc_cap or when memory ran out. vk_bl_scratch_asked: the most bytes asked of it since the last call, which
   counts again from 0: what a test sees of a request the buffer already held */
VkBuffer vk_bl_scratch(u64 size, u64* cap);
u64 vk_bl_scratch_asked(void);

/* owned.c: guest memory the GPU holds (memory/own.c, docs/layers/gpu/ownership.md). An engine's result (VIC's planes, a
   DMA copy's destination, a 2D blit's, inline-to-memory) is a resource: a device-local buffer in the guest's own
   layout, published over the guest bytes it holds. The CPU sees it only when it touches those bytes: the trap lands
   it then. vk_own_init after the device and the frames exist: 0 when the host traps guest memory */
typedef struct VkOwnRes {
    VkBuffer buf;
    VmaAllocation mem;
    u64 size;
    u64 written; /* the graphics submission its last publish's writes were recorded in: a landing waits for that one */
    u64 serial;  /* new with each write: what its bytes hash as (vk_guest_peek), never its address, a freed one's */
    u64 read;    /* buffer.c's count of reads where it is when a draw or dispatch last read it there (buf_get_owned) */
    u32 refs;    /* the creator's, and one an owned range */
    u8 borrowed; /* buf is another's (the GPU mirror's): letting go of the last reference frees only this record */
} VkOwnRes;
/* a resource whose bytes are in buf, at the guest address of each range it owns (the GPU mirror's): one reference */
VkOwnRes* vk_own_res_borrow(VkBuffer buf);
/* r's bytes over [cpu, cpu + size) from off on, published as vk_own_publish_owned does, written by the submission
   r->written names (submitted already: a landing waits for it and nothing recorded since) */
void vk_own_publish_submitted(u64 cpu, u64 size, VkOwnRes* r, u64 off);
/* mirror.c: the pages the mirror took in once their programs were done, published GPU-owned. Where nothing is being
   recorded (a draw being set up, a submission): at the end of a GPU job or a wait, and before the CPU reads them */
void vk_mirror_publish(void);
/* cmd.c: 1 inside a landing, a frame's recycling or a submission, where nothing may be recorded or submitted */
int vk_rec_unsafe(void);
/* mirror.c: guest memory [cpu, cpu + size) landed from an owner: the mirror's shadow of its pages goes */
void vk_mirror_landed(u64 cpu, u64 size);
int vk_own_init(void);
/* guest memory [cpu, cpu + size) watched for the CPU (own_watch), as vk_own_publish traps an owner's: 0, or -1 where it
   can't be (no traps, memory an import holds, memory not in the pool). vk_lock held */
int vk_own_watch(u64 cpu, u64 size);
/* whether the CPU made GPU-owned bytes of guest memory [cpu, cpu + size) land within the last presents presents, or
   touched them under a draw's storage image kept on the GPU (vk_own_cpu_touched) */
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents);
void vk_own_cpu_touched(u64 cpu, u64 size);
VkOwnRes* vk_own_res_new(u64 size); /* with the caller's reference; NULL when out of memory */
void vk_own_res_ref(VkOwnRes* r);
void vk_own_res_unref(VkOwnRes* r);
/* r's bytes are written by work recorded in the submission being recorded: a landing of them waits for that
   submission (owned.c own_land). vk_own_publish does it. A writer that keeps an owned range in place, with no new
   publish, calls it itself. Every writer of r records its writes outside rendering passes, after a barrier from what
   read r before (a new r has no readers), and a barrier to all later work before it calls this: what reads r later,
   inside a pass too, reads it where it is with no barrier of its own (buffer.c buf_get_owned, compute.c res_bind).
   The one exception, a draw writing r in place (buffer.c keep_new), stays a pending result until its submission is
   done, and what reads r meanwhile reads that result, with the barriers it takes (gfx_gpu_src). Under vk_lock */
void vk_own_res_wrote(VkOwnRes* r);
/* the guest CPU bytes [cpu, cpu + size) hold r's bytes from off on once the work recorded so far is done: owned, or,
   where the host can't trap them, landed at once (the GPU waited for). With unified memory, copied there on the GPU
   (vk_uma_publish) where one import holds them. Under vk_lock */
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off);
/* the same, owned whatever the memory path: what vk_own_publish does where unified memory can't publish in place, which
   the owned path's own checks take with unified memory too */
void vk_own_publish_owned(u64 cpu, u64 size, VkOwnRes* r, u64 off);
/* guest bytes [gva, gva + size) (by GPU address) for the GPU to read, without landing what it holds: the CPU's
   copied into host (staging memory at `at` in buf), the GPU's copied into buf there by the GPU, recorded in the
   frame's command buffer with the barriers that order them. vk_cpu_read is the same by guest CPU address. Under
   vk_lock */
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at);
void vk_cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at);
/* the same into device memory (buf, which the CPU doesn't map): 0 when staging has no room */
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at);
/* the same in the submission's upload command buffer (vk_cmd_upload), ahead of its rendering passes, when every byte
   was written before this submission; 0 with nothing recorded otherwise, or when staging has no room */
int vk_guest_read_ahead(u64 gva, u64 size, VkBuffer buf, u64 at);
/* guest bytes [gva, gva + size) into device memory as the GPU sees them: vk_guest_read_device, with the render targets
   drawn over them (vk_rt_overlay). 0 when there is no room */
int vk_guest_read_seen(u64 gva, u64 size, VkBuffer buf, u64 at);
/* guest bytes [gva, gva + size) into dst for change detection, as gmem_read reads them, except that bytes the GPU
   holds come back as a pattern of their owner (which a new owner changes, and the CPU's own bytes replace once it
   touches them): nothing lands */
void vk_guest_peek(u64 gva, void* dst, u64 size);
/* 1 when one GPU-owned range holds all of guest bytes [gva, gva + size), one run of the GPU's page table, with *tag its
   identity at gva as vk_guest_peek writes it there (its owner's serial, the offset in it); else 0 */
int vk_guest_owned_whole(u64 gva, u64 size, u64* tag);
/* 1 when one GPU-owned range holds all of guest bytes [gva, gva + size), one run of the GPU's page table, and its
   owner's serial is serial (that owner, not written since it had that serial); else 0. Under vk_lock */
int vk_guest_owned_by(u64 gva, u64 size, u64 serial);
/* everything recorded so far done before what follows reads or writes what it did (the engines' steps) */
void vk_all_barrier(VkCommandBuffer cb);
/* rows rows of row_bytes from src (rows src_pitch apart) over dst's rows (dst_pitch apart), a region a row */
void vk_copy_rows(VkCommandBuffer cb, VkBuffer src, u64 src_at, u32 src_pitch, VkBuffer dst, u64 dst_at, u32 dst_pitch,
                  u32 row_bytes, u32 rows);

/* texconv.c: a format the host can't take as it is (f->conv but ASTC, and packed depth and stencil), converted on the
   GPU (gpu/texture/texconv.comp) from linear rows of its guest texels or blocks into rows a copy into the image
   takes */
int vk_tc_converts(const VkFmt* f);
/* the conversion's shape for a row w texels wide: units (invocations) a row, the output's row in texels (padded to
   whole units: a copy's bufferRowLength) and its bytes a texel (a depth aspect's for depth), output rows a row of
   units (4 for a row of blocks), whether a stencil aspect follows the depth one, what the input's pitch must be
   a multiple of (the input rows are read in whole units), and the units of it one submission may hold (0: any; an
   encoder's, whose rows go in pieces then, vk_submit_split between them) */
typedef struct {
    u32 units, out_texels, out_bpp, out_rows, stencil, in_align, submit_units;
} VkTcShape;
VkTcShape vk_tc_shape(const VkFmt* f, u32 w);
/* the output's bytes for rows rows of units of slices slices: depth rows, then as many of stencil bytes */
u64 vk_tc_out_size(const VkTcShape* s, u32 rows, u32 slices);
/* records the conversion of rows rows of units (of texels, or blocks) of slices slices, w texels wide, from rows
   in_pitch bytes apart at in_at in in to out_at in out (offsets and pitch multiples of 4). Between vk_bl_barrier
   calls as vk_bl_move is. 0 when the device's limits don't allow it (said once) */
int vk_tc_convert(VkCommandBuffer cb, const VkFmt* f, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at, u32 in_pitch,
                  VkBuffer out, u64 out_at);
/* the same for a frame vi presents from memory, of vi's pixel format fmt (Android's: 2 RGBX8888, 4 RGB565, 5
   BGRA8888, 6 RGBA5551, 7 RGBA4444) to RGBA8 rows of the shape's out_texels; vk_tc_vi_converts is 0 for RGBA8888,
   which needs no conversion, and formats vi doesn't have */
/* NV12's chroma rows from FFmpeg's U and V planes (rows in_pitch bytes apart at u_at and v_at in in): pairs pairs a
   row, rows rows, into out at out_at, rows out_pitch bytes apart (offsets and pitches multiples of 4; the rows are
   written in whole words: pairs rounded up to 4). 0 past the device's limits */
int vk_tc_nv12_uv(VkCommandBuffer cb, u32 pairs, u32 rows, VkBuffer in, u64 u_at, u64 v_at, u32 in_pitch, VkBuffer out,
                  u64 out_at, u32 out_pitch);
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

/* astc.c: decode a level of one layer of t, or a region of it, w x h x depth texels at (x, y) of it, into it: x and y
   on its blocks, and on 4x4 ones for a BC7 image, whose region ends on them too or at the level's edge (astc_bc7.comp
   encodes a block from its own texels, the last one repeated past a strip's edge). The blocks come from rows(ctx, cb,
   z, nz, y, n, &buf, &off): block rows y to y + n (of the region) of slices z to z + nz, tightly packed, 16 bytes a
   block, a slice's after another's, in buf at off (aligned for a storage buffer) once what rows recorded into cb is
   done. They are asked for
   a strip at a time, so a level is never all in memory at once. 0 when staging ran out (the level is left out, and
   the caller tries again later); rows returns 0 for that too. The caller ends rendering and moves t to
   TRANSFER_DST_OPTIMAL first. A guest read while staging lands GPU results, and landing can submit the frame's
   command buffer, which ends the one cb names: rows then stores the buffer recording now through cb, and the caller
   records the rest of the strip there. content: for a whole level into a BC7 image that vk_astc_cached takes, the
   part of its BC7 key its guest bytes make (texture.c's astc_content), else NULL. With it the level's blocks come from
   the BC7 cache when they are there, with no decode and no encode, and an encode's blocks go into the cache once its
   submission is done (vk_astc_frame_done) */
typedef int (*VkAstcRows)(void* ctx, VkCommandBuffer* cb, u32 z, u32 nz, u32 y, u32 n, VkBuffer* buf, u64* off);
int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 x, u32 y, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                   VkAstcRows rows, void* ctx, const u64* content);
/* the same for a whole level of layers layers of a 2D array from layer, as a 3D level's slices: rows's z and nz count
   layers from layer, and a strip takes as many as the scratch holds. A layer at a time, a small level is too little
   work to fill the GPU and each layer's encoder dispatch waits for the one before (docs/layers/gpu/astc.md) */
int vk_astc_upload_layers(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 level, u32 layer, u32 layers, VkAstcRows rows,
                          void* ctx, const u64* content);
/* The ASTC blocks a texture held as BC7 keeps while its image is RGBA8, for its BC7 image made between frames
   (texture.c's vk_tex_later_frame): vk_astc_keep gives t the buffer for them (1 when it has one), and from then on
   vk_astc_upload and vk_astc_upload_layers copy each strip they decode into its place there, a region's too.
   vk_astc_unkeep lets it go, once what the GPU still does with it is done. vk_astc_kept_bytes: the bytes kept now.
   vk_bc7_encode_kept: slices z to z + nz of level `level` of t (its layers, or a 3D level's depth slices), ASTC block
   rows y to y + n of them, from what t keeps straight into BC7 blocks (astc_bc7.comp), copied into the same place of
   dst, a BC7 image of t's shape in TRANSFER_DST_OPTIMAL: the blocks an upload straight to BC7 makes. y is a multiple
   of vk_bc7_kept_unit(t), the block rows after which a 4x4 block starts on a block row again (1, 2 or 4), and n too
   unless the strip ends the level; more than one slice are whole ones; the strip's BC7 blocks are vk_bc7_strip_blocks
   at most. 0 when no descriptor set is left */
int vk_astc_keep(Surf* t);
void vk_astc_unkeep(Surf* t);
u64 vk_astc_kept_bytes(void);
u32 vk_bc7_kept_unit(const Surf* t);
int vk_bc7_encode_kept(VkCommandBuffer cb, Surf* t, VkImage dst, u32 level, u32 z, u32 nz, u32 y, u32 n);
/* A level of ASTC encoded again as BC7 with VK_BC7_FAST_TEXELS texels or more takes the encoder's fast build, which
   keeps mode 6 where it is close and tries every mode on the other blocks, and one of VK_BC7_MODE6_TEXELS or more its
   build of mode 6 alone (bc7_encode.glsl's SNX_BC7_FAST). vk_bc7_build: the build for a level of w x h texels and d
   slices of a volume (1 else): 0 the full one, 1 the fast one, 2 mode 6 alone (3, the quick one, is astc.c's for a very
   large level: vk_astc_quick). VK_BC7_FAST_TEXELS 0: every level takes
   the fast build, which a batch of small levels encoded with the full one held a frame for seconds without. The fast
   build is about 15 times faster than the full one, for the loss that docs/layers/gpu/astc.md measures (50.7 vs 53.8
   dB) */
#define VK_BC7_FAST_TEXELS  0u
#define VK_BC7_MODE6_TEXELS (8192u * 8192u)
u32 vk_bc7_build(u32 w, u32 h, u32 d);

/* bc7_cache.c: the BC7 blocks of ASTC levels encoded before, kept across runs in one file, which only an index of
   points into (a hit reads its record from the file). VK_BC7_ENCODER_VERSION goes into every key: bump it when what
   the encoder writes changes other than through its SPIR-V (which the file's version holds), as a level's choice of
   build does. VK_BC7_CACHE_MAX: the file's bound, after which it starts over: one Ounce game's demo alone holds
   3.3 GB of BC7, so 4 GB went over as more of it was played */
#define VK_BC7_ENCODER_VERSION 1
#define VK_BC7_CACHE_MAX       (8ull << 30)
/* content (two words, zero to start) with n bytes at p mixed in: an ASTC level's guest bytes and its layout's words */
void vk_bc7_content(u64 content[2], const void* p, u64 n);
/* what else changes a level's blocks: its size, its slices (depth) or layers, the footprint, the sRGB decode mode and
   the encoder's build (vk_bc7_build) */
typedef struct {
    u32 w, h, depth, layers, bw, bh, srgb, build;
} VkBc7Level;
void vk_bc7_key(const u64 content[2], const VkBc7Level* lv, u64 key[2]);
/* the file at path as the cache, or a new one where it holds another version, bounded to max bytes: 0 when there is
   no cache (another process holds the file, or it cannot be written) */
int vk_bc7_cache_open(const char* path, const u64 version[2], u64 max);
void vk_bc7_cache_close(void);
u64 vk_bc7_cache_find(const u64 key[2]); /* the bytes of key's blocks, 0 when they aren't there */
/* key's blocks, size bytes of them, into dst: 0 when they aren't there or don't check out (then dropped) */
int vk_bc7_cache_read(const u64 key[2], u8* dst, u64 size);
void vk_bc7_cache_put(const u64 key[2], const u8* p, u64 size);
/* hits, records stored, times it started over at its bound, and the file's bytes */
void vk_bc7_cache_stats(u64* hits, u64* stored, u64* restarts, u64* bytes);
u64 vk_bc7_strip_blocks(void); /* the BC7 blocks vk_bc7_encode_kept makes at once at most */
/* the image levels (or regions of them) uploaded as BC7 so far, the levels the BC7 cache gave (here or between frames),
   their bytes, and the bytes they would have taken as RGBA8 */
void vk_astc_stats(u64* levels, u64* hits, u64* bytes, u64* rgba8_bytes);
/* The BC7 cache of ASTC levels (astc.c over bc7_cache.c). vk_astc_cache_open: the cache of this game and device in the
   file at path, its records' version made of the encoder's (and the decoder's) SPIR-V, VK_BC7_ENCODER_VERSION and the
   GPU and its driver (pipeline.c, where the GPU samples BCn but not ASTC). vk_astc_cached: a level of w x h texels and
   depth slices or layers goes through it (the cache open, its blocks VK_BC7_CAPTURE_BYTES or less).
   vk_bc7_level_key: the key of a level of that shape, footprint bw x bh, sRGB decode mode and encoder build (from
   content, its guest bytes' part). vk_astc_frame_done: the submission sub is done, and the blocks encodes in it read
   back go into the cache (cmd.c's landing). vk_bc7_cache_level: level `level` of img (TRANSFER_DST_OPTIMAL), w x h
   texels and slices layers or 3D slices, copied from the cache: 0 when its blocks aren't there.
   vk_bc7_capture_level: that level's blocks, which the encoder wrote whole (texture.c, between frames), read back for
   the cache: 0 when there is no room for them now. VK_BC7_CAPTURE_BYTES bounds the blocks read back at once, which
   bounds the readback ring's growth and is the most a level's blocks are to be cached */
#define VK_BC7_CAPTURE_BYTES (32ull << 20)
void vk_astc_cache_open(const char* path);
int vk_astc_cached(u32 w, u32 h, u32 depth);
void vk_bc7_level_key(const u64 content[2], u32 w, u32 h, u32 depth, u32 layers, u32 bw, u32 bh, int srgb, u32 build,
                      u64 key[2]);
void vk_astc_frame_done(u64 sub);
int vk_bc7_cache_level(VkCommandBuffer cb, VkImage img, const u64 key[2], u32 level, u32 w, u32 h, u32 slices,
                       int is3d);
int vk_bc7_capture_level(VkCommandBuffer cb, VkImage img, const u64 key[2], u32 level, u32 w, u32 h, u32 slices,
                         int is3d);
u64 vk_astc_dispatches(void); /* the BC7 encoder's dispatches so far */
/* A very large level (astc_quick_mtexels or more megatexels, every layer of a 2D array and slice of a 3D level
   counted) held as BC7 at load takes the encoder's quick build in the frame that uploads it, and its fast build is made
   in the background (astc.c). vk_astc_quick: whether a level of w x h texels and depth slices or layers is one, with
   the BC7 cache open, whose fast build goes into the cache in pieces whatever its size (texture.c keys it as it keys a
   level vk_astc_cached takes). vk_astc_background: after a present's submission (vk_lock), the levels whose pieces are
   all stored ended (their snapshots freed), and with astc_background on a piece encoded on the GPU in a submission of
   its own once the last one is done. vk_astc_background_drain: until every
   background encode is done (tests: there are no presents). vk_astc_forget: t's image no longer holds what the upload
   of a level wrote there (another upload of it), or of any (~0u: t is freed), so the background encodes of those
   levels go on into the cache alone. g_vk_astc_hit_ns: the time a very large level's hit may take reading its pieces,
   ns (astc.c's BG_HIT_NS; tests set it) */
int vk_astc_quick(u32 w, u32 h, u32 depth);
void vk_astc_background(void);
void vk_astc_background_drain(void);
void vk_astc_forget(const Surf* t, u32 level);
/* very large levels encoded quickly, those given to the background and those it finished, those still in it, and
   those whose fast build came from the cache; the pieces stored, the pieces the GPU encoded, and those loaded from the
   cache after a hit ran out of time */
typedef struct {
    u64 quick, made, finished, pending, hits, stored, gpu_pieces, loaded;
} VkAstcBackground;
void vk_astc_background_stats(VkAstcBackground* s);
extern u64 g_vk_astc_hit_ns;
/* Internal ASTC, BC7, block-linear, conversion, multisample and depth-packing pipelines, apart from guest shaders.
   Both functions run under vk_lock. Cumulative time is in nanoseconds, including time inside the driver. */
void vk_texture_pipeline_made(u64 ns);
void vk_texture_pipeline_stats(u64* count, u64* ns);
/* the bytes of the ASTC textures decoded to RGBA8 so far (astc_mode=1, or a GPU without BCn) */
u64 vk_astc_rgba8_bytes(void);

/* vic.c: the VIC composition as a compute shader, recorded in the frame's command buffer, its output planes
   published (vk_own_publish): 1 when composed, 0 when rows of it could not be (said, with the bytes they lacked). A
   job bigger than one buffer goes in bands of rows. Takes vk_lock */
int vk_vic_compose(const VicJob* j);
/* nvdec.c: an NVDEC picture into its surfaces on the GPU (gpu.h's NvdecPic): staged rows (chroma interleaved there)
   or a Vulkan Video picture's planes copied out of its image, both planes swizzled there, the surfaces published
   (vk_own_publish). 1 when written; takes vk_lock. vk_nvdec_hw_device: the renderer's nvdec_hw_device */
int vk_nvdec_write(const NvdecPic* p);
void* vk_nvdec_hw_device(u32 codec);
/* dma.c: a launch of the DMA copy engine on the GPU (gpu.h's GpuDmaOp), its destination published: 1 when done, 0
   when it could not be (said, with the bytes it lacked). A launch bigger than one allocation, or whose scratch is
   bigger than VK_PART_SHARE of one, goes in bands of lines. Takes vk_lock. vk_dma_stats: the launches so far, the
   bands the big ones took, and the guest bytes they read and wrote: a pitch linear side's lines, a block linear one's
   tiles (the blocks of its lines' GOB columns) */
int vk_dma_copy(const GpuDmaOp* o);
void vk_dma_stats(u64* copies, u64* bands, u64* bytes);
/* A transfer that may be more than one buffer can hold (cap: g_vk.alloc_cap, or less), taken in bands of lines: the
   DMA's copies, the 2D engine's blits (blit.c), VIC's jobs (vic.c). need(user, y, n): the most bytes one of the
   buffers of lines [y, y + n) takes. A band ends where its end, counted from base, is a multiple of unit (a block
   linear side's rows of blocks). split: in bands even when all the lines fit the cap, for a transfer that would grow a
   buffer which stays (the scratch of the DMA's copies and of the blits) */
typedef struct {
    u64 cap;
    u32 unit, base;
    u64 (*need)(const void* user, u32 y, u32 n);
    const void* user;
    int split;
} VkBands;
/* the lines from y on (left of them) the next band takes: all of them when they are the first, fit the cap and b
   isn't split, else as many as fit VK_PART_SHARE of it, at least a unit. *most: the bytes the band needs, more than
   the cap when even a unit is */
u32 vk_band(const VkBands* b, u32 y, u32 left, u64* most);
/* memory for the DMA engine's copies of sources at submission, which the copy reads in place (Renderer.snap_alloc and
   snap_free, from any thread), and the bytes it holds */
u8* vk_snap_alloc(u64 len, u64* id);
void vk_snap_free(u64 id);
u64 vk_snap_bytes(void);
/* blit.c: a 2D blit from memory on the GPU (gpu.h's GpuBlit), its destination published: 1 when done, 0 when rows of
   it could not be (said, with the bytes they lacked). A blit bigger than one allocation goes in bands of rows. Takes
   vk_lock */
int vk_blit_mem(const GpuBlit* b);
/* msaa.c: a multisampled surface's samples moved between its image and the guest's layout of them (sample i of pixel
   (x, y) at texel (x X + i % X, y Y + i / X) of a surface X by Y times its size, DS_MSAA_), through a single-sampled
   image of that size (the grid), by draws. Under vk_lock, outside a rendering pass (they end an open one) */
int vk_ms_moves(const Surf* s); /* s's samples can move: multisampled, unscaled, with a raw view (Surf.ms_raw) */
/* rows of layer's samples from the guest's row y0 drawn into a new grid of s's move format, rows high (its row 0 is
   row y0), left in TRANSFER_SRC_OPTIMAL for copies recorded in this frame (it is freed with it): VK_NULL_HANDLE when
   they can't be */
VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer, u32 y0, u32 rows);
u64 vk_ms_unpacked(void); /* the grid texels vk_ms_unpack has drawn so far (tests) */
/* layer's samples drawn from a grid loaded with rows of pitch bytes from off in buf: 0 when they can't be (a depth
   surface's never are) */
int vk_ms_pack(VkCommandBuffer cb, Surf* s, u32 layer, VkBuffer buf, u64 off, u32 pitch);
/* surface.c: a multisampled surface's samples loaded from guest memory (a texture's upload): 0 when they can't be */
int vk_ms_load(Surf* s);

/* n little-endian words of a texture or sampler header in guest memory at gva as host numbers, as gmem_read reads them.
   No program writes headers, so a program's pending write in place that covers them isn't waited for: one game's
   dispatches found one over their headers in each frame, and the wait held the GPU thread for about 20 ms of each
   frame with the GPU idle behind it */
static inline void vk_gmem_words(u64 gva, u32* w, u32 n) {
    vk_uma_gpu_reads(1);
    gmem_read(gva, w, (u64)n * 4);
    vk_uma_gpu_reads(0);
    snx_le32_array(w, n);
}

/* texture.c */
void vk_render_forget_state(void);     /* the next draw sets all its state again (another pipeline or state was set) */
Surf* vk_tex_from_tic(const u32* tic); /* tic: the header's words as host numbers */
/* Writable resolution bypasses read-only render-target crop snapshots. */
Surf* vk_tex_from_tic_storage(const u32* tic);
/* a texture vk_tex_from_tic returned for an unchanged header, checked again as it would be: memory written since, what
   targets drew into it. NULL when it can't be drawn now */
Surf* vk_tex_recheck(Surf* t);
Surf* vk_tex_from_tic_raw(const u32* tic); /* UINT view of raw RGBA8 texel bytes */
/* vk_tex_from_tic, or with raw vk_tex_from_tic_raw, for a compute program or a stage before the fragment's, whose
   fetches, size queries and storage images count the guest's texels: a scaled render target (resolution= other than
   1) comes as a copy of the guest's size, and as the scaled image itself only where no such copy can be made */
Surf* vk_tex_from_tic_guest(const u32* tic, int raw);
/* a view of s as the shader declares it, sRGB or linear (color formats that have both), with a component mapping;
   VK_NULL_HANDLE when the image can't be viewed that way */
/* stencil: a depth/stencil surface's stencil aspect instead of its depth */
VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil);
/* why s has no view of type, NULL when it can have one: what vk_tex_view checks before it asks the driver */
const char* vk_tex_view_refusal(const Surf* s, VkImageViewType type);
/* a view type's name in a message: "2D", "cube array"... */
const char* vk_view_type_name(VkImageViewType type);
/* the format of s's views, its sRGB form when srgb (a depth format stays) */
VkFormat vk_tex_view_format(const Surf* s, int srgb);
/* One selected mip for storage; the sampled view retains the complete chain. */
VkImageView vk_tex_storage_view(Surf* s, VkImageViewType type);
/* an empty texture of a view type: num 0 float, 1 unsigned, 2 signed integer, 3 depth at 1.0 (shadow samplers);
   ms: a multisampled one (2D and 2D array views, not depth) */
VkImageView vk_null_view(VkImageViewType type, int num, int ms);
/* An image slot a program declares whose header is empty (word 0 zero: no format, nothing bound there): a 1x1 storage
   image of zeros, its format by the slot's kind (ShaderInfo.img[].kind), viewed as type (2D, 2D array or 3D) in
   *view. The caller makes it ready for its stage (vk_surf_use, layout GENERAL) and writes nothing of it back. NULL
   when it can't be made */
Surf* vk_null_storage(VkImageViewType type, int kind, VkImageView* view);
/* a storage image header with nothing bound: no format and no component types in word 0 */
static inline int vk_tic_empty(const u32* tic) { return tic[0] == 0; }
/* the sampler for a TSC: raise lets anisotropy= raise its filtering (a 2D texture with mipmaps), integer gives its
   border color as integers (an integer texture) */
VkSampler vk_sampler(const u32* tsc, int raise, int integer);
extern int g_vk_anisotropy; /* anisotropy=, read once at start: the sampler table keeps the samplers it made */
void vk_tex_frame(void);    /* a frame was presented: evicts what wasn't used for a while */
/* After a frame's present, its own submission: the BC7 encoder makes the BC7 images of ASTC textures held as BC7 from
   their RGBA8 ones, VK_BC7_LATER_BLOCKS blocks at most, a texture at a time in the order they were loaded, and puts
   each in its texture's place once whole. Nothing when vk_cmd would wait for the GPU first */
void vk_tex_later_frame(void);
/* the encoder's blocks a frame: 3.3 ms of an RX 6700 XT, which encodes 10 million a second (docs/layers/gpu/astc.md), a tenth of
   a frame at 30 fps */
#define VK_BC7_LATER_BLOCKS (1u << 15)
/* s goes: its place in the queue and the BC7 image being made for it with it (surface.c surf_free) */
void vk_tex_later_forget(Surf* s);
/* statistics since the start: textures made BC7, the RGBA8 bytes that freed, encodes started over for a write or
   given up, blocks encoded, textures held Exact for their writes and made BC7 again after; and now: textures waiting,
   the bytes their RGBA8 images take, textures held Exact for their writes */
typedef struct {
    u64 made, freed, restarted, blocks, waiting_bytes, went_hot, cooled;
    u32 waiting, hot;
} VkLaterStats;
void vk_tex_later_stats(VkLaterStats* s);

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
int vk_buf_get_uniform(u64 gva, u64 len, VkBuffer* buf, u64* off); /* a constant buffer: in place where it can be */
/* a dispatch's read-only storage buffer: no delayed content checks, and in place where it can be, as a constant buffer */
int vk_buf_get_strict(u64 gva, u64 len, VkBuffer* buf, u64* off);
/* a copy engine's source (DMA, 2D): as strict, and on pool pages compared by contents rather than trusted to the
   write tracking, since what a copy lands in keeps a stale byte for good. vk_buf_copy_src_read: the bytes such
   sources were read on the CPU so far, to be compared or copied */
int vk_buf_get_copy_src(u64 gva, u64 len, VkBuffer* buf, u64* off);
u64 vk_buf_copy_src_read(void);
/* a copy of len bytes made now (data that changes within a frame), or of zeros when gva is 0 */
int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off);
/* ordered=0 only for a front end that supplies execution and shader-data barriers explicitly. */
/* A compute program's write log (writelog.c, shader.h's SH_WLOG_*): host memory the GPU writes, from a pool. get
   gives a clear one of parts parts with one reference, the caller's; put gives back one no recorded work used; unref
   drops a reference, and the last, once the work that wrote it is done, clears what was written and gives it back.
   get_gpu gives one in the GPU's memory, for work whose log only the GPU reads (the mirror's folds): its readers are
   recorded, then vk_wlog_clear clears it on the GPU, in the same command buffer; the CPU never reads it
   (vk_wlog_runs refuses it), and unref only gives it back */
typedef struct VkWriteLog VkWriteLog;
VkWriteLog* vk_wlog_get(u32 parts);
VkWriteLog* vk_wlog_get_gpu(u32 parts);
int vk_wlog_on_gpu(const VkWriteLog* l);
void vk_wlog_clear(VkCommandBuffer cb, VkWriteLog* l);
VkBuffer vk_wlog_buffer(const VkWriteLog* l);
u64 vk_wlog_size(const VkWriteLog* l);
void vk_wlog_ref(VkWriteLog* l);
void vk_wlog_unref(VkWriteLog* l);
void vk_wlog_put(VkWriteLog* l);
/* a run of bytes [from, to) of a range, from its start */
typedef struct {
    u32 from, to;
} VkRun;
typedef void (*VkRunFn)(void* user, u32 from, u32 to);
/* the bytes part of l logs for a binding of size bytes, once its work is done: fn gets each run, in order, runs that
   meet joined. The windows that were written */
u32 vk_wlog_runs(VkWriteLog* l, u32 part, u32 size, VkRunFn fn, void* user);
/* the same bytes of [from, to) of the binding written on the GPU, in the command buffer being recorded: from src, where
   the work wrote the binding from src_off on, into dst, which holds the binding's byte from at dst_off
   (wlog_merge.comp), every other byte of dst kept. from, to, src_off and dst_off are multiples of 4; the caller orders
   it after that work and before what uses dst. 0 when it can't be (no descriptor set left), with nothing recorded */
int vk_wlog_merge(VkWriteLog* l, u32 part, u32 from, u32 to, VkBuffer src, u64 src_off, VkBuffer dst, u64 dst_off);
void vk_wlog_stats(u64* logs, u64* bytes, u64* in_use, u64* windows);
void vk_wlog_trim(void); /* the logs the pool hasn't given out for a while go */
/* The GPU mirror of guest memory (mirror.c): whether it is on, its buffer and its mask (a bit a byte) */
int vk_mirror_on(void);
VkBuffer vk_mirror_buffer(void);
VkBuffer vk_mirror_mask(void);
/* [gva, gva + len), one run of guest memory, up to date in the mirror for the work recorded next: the mirror in *buf,
   at *off, which is the guest address of gva. 0 when it can't be (not one run, the mirror off) */
int vk_mirror_slice(u64 gva, u64 len, VkBuffer* buf, u64* off);
/* a program recorded now writes [cpu, cpu + len) of the mirror: pending until the submit's harvest lands it */
void vk_mirror_wrote(u64 cpu, u64 len);
/* a buffer in the mirror that a dispatch writes: its guest bytes [cpu, cpu + len) and its part of the write log */
typedef struct {
    u64 cpu;
    u32 len, part;
} VkMirFold;
/* recorded into cb after a dispatch: the bytes its write log (log, log_size bytes) gives for each of its n buffers in
   the mirror, set in the mirror's mask (mirror_fold.comp), and their ranges pending (vk_mirror_wrote). The submit's
   harvest lands those bytes and no others */
void vk_mirror_fold(VkCommandBuffer cb, VkBuffer log, u64 log_size, const VkMirFold* f, u32 n);
/* a count of the programs' writes of the mirror recorded so far (each vk_mirror_fold): a read of the mirror in place
   needs a barrier only after it moved */
u64 vk_mirror_write_gen(void);
/* whether pending writes of the mirror are behind [gva, gva + len); a read of guest memory there lands them first */
int vk_mirror_pending(u64 gva, u64 len);
/* whether the mirror owns a part of [gva, gva + len), one run of guest memory: written by its programs and landed
   only as far as the CPU touched it (vk_mirror_publish). The mirror holds the range's bytes then, guest memory not */
int vk_mirror_holds(u64 gva, u64 len);
void vk_mirror_mem_need(u64 gva, u64 len);
/* at a submit, recorded into cb: the pages its programs wrote, copied for landing; and once submissions up to done are,
   their landing in guest memory */
void vk_mirror_harvest(VkCommandBuffer cb, u64 submit);
void vk_mirror_done(u64 done);
void vk_mirror_stats(u64* granules, u64* harvests, u64* pages, u64* landed, u64* overflows, u32* room);
/* the bytes of a result at res a writer logged (n sorted runs that don't meet, from addr), into guest memory: as
   vk_write_changed lands what it finds changed. The runs whose guest pages the CPU wrote since the write tracking
   clock since, which the GPU's bytes then win over: their count */
u32 vk_write_runs(u64 addr, const u8* res, const VkRun* runs, u32 n, u32 since);
/* the writer vk_buf_copy_writable bound last logs its writes in part of l: the result it writes lands those of all of
   its writers and no other bytes, once its commit gives it a reference to l. A writer that logs nothing (a draw, a
   buffer image) has its result compared with its original bytes as before */
int vk_buf_copy_writable_logged(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered, VkWriteLog* l, u32 part,
                                int no_read); /* no_read: the program writes but never reads it, so its input is not
                                                 copied */
/* the dispatch's own reference to l, dropped once the submit being recorded is done */
void vk_buf_log_retire(VkWriteLog* l);
int vk_buf_copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered);
/* Shader-data invalidation needs writes only. Execution order also covers reads before an in-place writer. */
int vk_buf_order_pending(int writes_only);
void vk_buf_order_request(int writes_only);
void vk_buf_ordered(void);
typedef struct {
    u64 addr;
    u32 size;
    int writable, aliased;
    int bare; /* its address has no size beside it (SH_GBUF_*): bound in a shared window */
    VkBuffer buf;
    u64 off, range;
} VkDrawGbuf;
/* a draw's global memory, by the bindings of the stage that reads it (DsBindings'): vertex, the two tessellation
   stages, geometry, fragment (ST_VS to ST_FS), then an Ounce's task and mesh stages (DS_TASK, DS_MESH) */
#define VK_DRAW_STAGES DS_GFX_STAGES
typedef struct {
    VkDrawGbuf b[VK_DRAW_STAGES][16];
    int n[VK_DRAW_STAGES];
    VkBuffer map_buf[VK_DRAW_STAGES];
    u64 map_off[VK_DRAW_STAGES], map_size[VK_DRAW_STAGES];
} VkDrawStorage;
/* whether any stage of the draw reads or writes global memory */
static inline int vk_draw_storage_any(const VkDrawStorage* draw) {
    for (int s = 0; s < VK_DRAW_STAGES; s++)
        if (draw->n[s]) return 1;
    return 0;
}
int vk_buf_draw_has_alias(const VkDrawStorage* draw);
int vk_buf_draw_prepare(VkDrawStorage* draw);
void vk_buf_writable_abort(void);
void vk_buf_writable_commit(void);
void vk_buf_mem_need(u64 gva, u64 len);
/* own buffers last used before the present `before`, the least recently used first, dropped until `want` bytes went
   (their ranges are copied again when next read). Returns the bytes */
u64 vk_buf_reclaim(u32 before, u64 want);
void vk_buf_stats(u32* count, u64* bytes);
/* writable storage bindings since the start: guest bytes copied for them before their draws (their original bytes and
   their output's fill), their results' bytes copied back from the GPU to land, and the results landed by comparing
   them with their original bytes. buffer_copy_stats LOGs them every 600 presents */
void vk_buf_writable_stats(u64* copied, u64* read_back, u64* merged);
/* temporary writeback diagnosis: bytes compared (memcmp) and bytes actually changed and written back */
void vk_writeback_diag(u64* cmp, u64* chg);
/* guest memory checked by its contents so far: compared with a pending result's original bytes, hashed whole, and
   sampled (tests count them: write tracking's answers stand in for them) */
void vk_buf_checks(u64* compares, u64* hashes, u64* samples);
u64 vk_buf_ahead(void); /* buffers filled ahead of their submission's rendering passes (vk_cmd_upload) so far */
int vk_buf_pending(u64 gva, u64 len); /* a pending graphics result overlaps the range */
/* GPU-owned bytes are on the range's pages (memory/own.c): reading them on the CPU lands them, a wait for the GPU */
int vk_buf_owned(u64 gva, u64 len);
u64 vk_buf_adds(void);                /* graphics results committed so far: as vk_compute_adds */
int vk_buf_pending_any(void);         /* any graphics result is pending */
int vk_buf_pending_unsubmitted(void); /* one is in the commands recorded since the last submit */
/* graphics results that searches for the ones over a range gave their callers to compare so far: each search looks at
   the results on the range's pages alone (tests count them) */
u64 vk_buf_pending_inspected(void);
/* renderdoc.c: RenderDoc loaded before the instance exists, with renderdoc set, or taken where RenderDoc's program
   started the game (load); the frames renderdoc_frames names and the capture key asks for captured (present, at each
   present under vk_lock, where RenderDoc writes each, the game stopped, a thread of its own saying how long). capture:
   whether a capture can be taken (RenderDoc is in the process), and with ask, the frame after the next present
   captured, said in the log and over the game; the renderer's capture. Any thread */
/* RenderDoc's library, by the name its own program puts in a game it starts: the one load looks for and opens */
#if defined(_WIN32)
#define SNX_VK_RENDERDOC_LIBRARY "renderdoc.dll"
#elif defined(__ANDROID__)
#define SNX_VK_RENDERDOC_LIBRARY "libVkLayer_GLES_RenderDoc.so"
#else
#define SNX_VK_RENDERDOC_LIBRARY "librenderdoc.so"
#endif
void vk_renderdoc_load(void);
int vk_renderdoc_capture(int ask);
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
int vk_release_pending(u64 gva, u64 len); /* a queued release's payload covers GPU range [gva, gva + len) (vk_lock) */
/* a samples-passed report a draw reads before it is performed: its count, len bytes (4 or 8) at byte at of what was
   read, is counter's segments [first, end), written there on the GPU (vk_query_counts) */
typedef struct {
    u32 counter, at, len;
    u64 first, end;
} SamplesCount;
/* vk_release_read's, with the samples-passed reports queued over the range that the GPU can write there left out of dst
   (zeros) and listed in counts, at most max of them, *n set: no wait for those */
int vk_release_read_counts(u64 gva, void* dst, u64 len, SamplesCount* counts, u32 max, u32* n);
extern u64 g_vk_release_forwards;           /* statistics: reads vk_release_read answered */
extern u64 g_vk_dset_new, g_vk_dset_reused; /* statistics: descriptor sets written, reused for the same contents */
extern u64 g_vk_itable_uploads, g_vk_itable_reused, g_vk_itable_bytes; /* indexed uniform staging statistics */
void vk_render_upload_cache_reset(void); /* renderer start: invalidate staging addresses from an earlier device */
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
   (render.c), vk_query_flush outside a pass before a submit (cmd.c), where the reports' copies are recorded */
void vk_query_draw(VkCommandBuffer cb, u32 owner); /* counted as the targets' samples, the guest's */
void vk_query_pass_end(VkCommandBuffer cb);
void vk_query_flush(VkCommandBuffer cb);
void vk_query_reset(u32 owner);
void vk_query_enable(u32 owner, int on);
int vk_query_enabled(u32 owner);
int vk_query_report(u32 owner, u64 gva, u32 len, u64 ticks);
/* a report performed (release.c): the count of counter's segments [first, end) */
u64 vk_query_value(u32 counter, u64 first, u64 end);
u64 vk_query_peek(u32 counter, u64 first, u64 end); /* the same, out of order, changing nothing */
/* writes the n counts of reports not performed yet into buf, whose bytes from off on were read at their at: 1, or 0
   when they can't be counted on the GPU (the caller then reads the reports performed) */
int vk_query_counts(VkBuffer buf, u64 off, const SamplesCount* counts, u32 n);
extern u64 g_vk_counts_on_gpu; /* statistics: the counts vk_query_counts wrote */
#define VK_QUERY_COUNTERS 8    /* query.c's samples-passed counters, a draw owner's each */
/* render enable predicates (release.c's vk_render_predicate): a queued report of counter's segments [first, end) gets
   a predicate slot, held once, that the next vk_query_pred_flush writes (2, *slot set), or its count is known now: 1
   not zero, 0 zero. -1 when there can be no predicate: the device can't make them, or there's no memory for the
   batch. The reports of a counter come in their queue order, each with what the one before it in its epoch, still
   queued, is known to be (prev, ending at prev_end): the count up to there, which it goes on from. first may be the
   end of an earlier report of the epoch: the predicate is then whether the two reports' counts differ */
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
/* [host, host + len)'s hot pages (write_tracking_hot, plat_ww_cool), and those at its pages' other addresses, protected
   again and taken in as written, so the next vk_ww_now is a baseline after which "written" stands for a write: for a
   render target, whose write-back keeps memory's bytes on the pages written since its baseline. A hot page reads as
   written at every scan. Called after the range's writes were taken in (vk_written_since), whose scan may leave a page
   hot itself. Under vk_lock */
void vk_ww_cool(const u8* host, u64 len);
void vk_ww_new_epoch(void); /* the CPU may have written what a GPU stream reads next: write tracking scans again */
/* The epoch of the CPU's writes the calling thread's checks see: a cache that trusts a check for the rest of its epoch
   stamps this one, read before the check (or the read of memory) it stands for. A job's checks see the epoch it began
   in, not a later submission's: a check stamped with that one would vouch for writes it never looked for */
u32 vk_ww_epoch(void);
/* the calling thread runs a job of a stream (1: its checks see the CPU's writes from before the newest epoch now, and
   from before the waits it ends) or stops (0: they see every epoch's) */
void vk_ww_stream(int on);
void vk_ww_seen(void);                 /* renderer.h mem_seen: the running job's checks take in the newest epoch */
void vk_mem_changed(u64 cpu, u64 len); /* gpu.c's g_gpu_mem_changed: the range counts as written */
/* own.c's landed hook: host pages [cpu, cpu + size) a landing wrote, with their bytes then, whose next report as
   written counts only if those changed */
void vk_ww_landed(u64 cpu, u64 size, const u8* bytes);
void vk_ww_stats(u32 presents); /* the stats: the write tracking's epochs and scans since the last, a line */
/* tests: the landed pages' list sorted as a scan's take sorts it, its length, or ~0u when it isn't one entry a page in
   the pages' order; and the hash kept for the host page at cpu (0 for none) */
u32 vk_ww_landed_check(void);
u64 vk_ww_landed_hash(u64 cpu);

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
/* the window of a program's bytes that vk_shader_get reads from its SPH (pipeline.c says why it is this wide) */
#define SH_CODE 0x80000u
/* Read/hash a program into code, SH_CODE bytes zero-padded, without cohering unrelated trailing data. Holds
   vk_lock. */
void vk_shader_read_code(u64 gva, u8* code, u32* len, VkCbReader read);
u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads);
/* A compute pipeline kept across runs in the pipeline file (pipeline_file.h's PF_CSPIPE). vk_pf_cspipe (pipeline.c):
   one made this run, of key, launch (the block's threads x y z, its shared memory bytes, then the program's address
   less 80, which a program of absolute branches is translated at: CsPipe's flow_base), the reads cbr its
   translation made, nkey bytes of compute.c's variant key and ncode bytes of its program window. vk_cs_pipe_load
   (compute.c), on the file's reading thread: one of an earlier run translated again with its reads and made, then put
   where the dispatches find it; with record, kept in the file again too (a file of another build being rebuilt). 1
   when it was made */
void vk_pf_cspipe(u64 key, const u32 launch[5], const VkCbReads* cbr, const void* vkey, u32 nkey, const u8* code,
                  u32 ncode);
int vk_cs_pipe_load(const u32 launch[5], const u32* cbr, u32 ncbr, const u8* vkey, u32 nkey, const u8* code, u32 ncode,
                    int record);
/* why the device can't take the module: a capability it declares (Float64, Int64) whose feature is off, or NULL */
const char* vk_spirv_unavailable(const u32* spv, size_t words);
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read); /* BRX tables through read */
/* where each new shader's snx-shaderc corpus entry goes: shader_corpus, else with diag=1 captures/shaders beside
   the games; NULL for neither */
const char* vk_corpus_dir(void);
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read);
typedef struct VkSh {
    u64 hash;
    u32 mask[26]; /* vk_shader_get's masks, including the geometry passthrough skip mask */
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
/* Generated graphics modules share the persistent shader/pipeline ownership and cache. Caller holds vk_lock. */
VkSh* vk_shader_generated_find(u64 hash, int stage, const u32 mask[26]);
void vk_shader_generated_record(VkSh* shader, const u32* spv, size_t words, const u8* source, u32 bytes);

/* the pipeline layout whose sets have no texture array bindings: compute's, and the one push constants go through
   (every layout has the same push constant range) */
extern VkPipelineLayout g_vk_playout;
VkPipelineLayout vk_playout(u32 arrays); /* arrays: a bit per set (shader_vk_set) that has them */
VkShaderStageFlags vk_set_stages(int set); /* the stages that see descriptor set set (pipeline.c) */
int vk_graphics_playout_is(VkPipelineLayout layout);
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
   in pc[7]. The task and mesh stages, where the device has them (g_vk.mesh_stages), read the first workgroup in
   pc[0] and the depth fix in pc[1] */
#define VK_PUSH_STAGES                                                                                                 \
    (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |                                           \
     VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |       \
     g_vk.mesh_stages)
void vk_pipeline_init(void);
/* the shader at gva (after a VertexA program at gva_a, or 0). The masks key integer inputs: attributes for the
   vertex stage, textures and render target outputs for the fragment stage. dc_funcs and unfed, the fragment stage's,
   key the vertex stage's unsigned and signed textures (its first 32). rt_int keys the primitives of the
   tessellation evaluation and geometry stages (ShaderInfo.rt_int_mask). clip: the clip distances the 3D engine
   enables, and SNX_SH_NOT_LAST (the stages before rasterization). xfb: what transform feedback captures of the last stage before
   rasterization's outputs (ShaderInfo.xfb), NULL for none. ds: the draw state whose programs key a fragment program
   (its early fragment tests) and a geometry program (its passthrough mask), and whose bound constant buffers the
   translator reads to resolve indirect branches. NULL for none. rect: the textures a fragment or vertex program
   samples whose headers take unnormalized coordinates (ShaderInfo.tex_rect_mask) */
VkSh* vk_shader_get(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb,
                    const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint);
VkSh* vk_shader_get_ex(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                       u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb,
                       const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint,
                       u32 fs_config);
/* An Ounce's task or mesh program at gva (stage ST_TS or ST_MS), with the fields of its key k: vk_shader_get with the
   fields in the key's words, and the per-primitive SPH that k names read from the program's code area. A fragment
   program drawn after a mesh program takes the generics that it writes per primitive (ShaderInfo.prim_mask) in the
   clip word of its key */
VkSh* vk_shader_mesh(const DsState* ds, u64 gva, int stage, const DsMeshKey* k);
/* whether the constant buffer words s's translation read still hold in ds's bound buffers (vk_shader_get's check on
   a hit, for a caller that kept s) */
int vk_shader_cb_hold(const DsState* ds, const VkSh* s);
/* a draw's vertex layout: attributes by location and the bindings they read, as a pipeline's vertex input state
   holds them or vkCmdSetVertexInputEXT sets them */
typedef struct {
    u32 nattr, nbind;
    struct {
        u32 loc, binding, format, offset;
    } attr[32];
    struct {
        u32 divisor; /* 0: per vertex */
        u32 stride;  /* only without has_dynamic_stride, else 0 */
    } bind[33];      /* bindings 0..nbind-1: a binding per attribute at most, and one for constant attributes */
} VertexLayout;
typedef struct {
    VkShaderModule vs, fs;
    VkShaderModule tcs, tes, gs; /* tessellation control and evaluation, geometry: VK_NULL_HANDLE when off */
    u32 patch;                   /* control points of a patch, with tessellation */
    /* bit 0: an Ounce's mesh pipeline. vs is its mesh shader and tcs its task shader (VK_NULL_HANDLE for none), and
       it has no vertex input, input assembly or tessellation state (topo_class and vi stay 0). Bits 1 and 2 (PIPE_
       MESH_WHOLE): the mesh and the task workgroup are whole subgroups, which the pipeline then requires */
    u32 mesh;
    /* POINT_LIST, LINE_LIST, TRIANGLE_LIST, their adjacency forms or PATCH_LIST: the topology is dynamic within its
       class */
    u32 topo_class;
    u32 polygon;
    u32 depth_clamp;
    /* all zero with dynamic vertex input (g_vk.vertex_input_dynamic): each draw sets its own (render.c), and one
       pipeline serves every layout its programs are drawn with */
    VertexLayout vi;
    u32 ncolor;
    u32 color[8];
    u32 depth, stencil;
    struct {
        u32 en, sc, dc, cop, sa, da, aop, mask;
    } blend[8];
    VkAdvancedBlend advanced;
    u32 feedback; /* VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT, without the dynamic state for it */
    u32 provoking_last;  /* the last vertex provokes (has_provoking_last) */
    u32 logic_op;        /* 0, or the VkLogicOp the color targets take in place of blending, plus 1 */
    u32 arrays;          /* the sets with texture arrays: its layout, vk_playout(arrays) */
    u32 heaps;           /* graphics descriptor heap mappings instead of descriptor-set layouts */
    u32 samples;         /* the targets' VkSampleCountFlagBits (0 taken as 1) */
    VkCoverage coverage; /* color-only target-independent rasterization, all zero for ordinary multisampling */
    VkSampleLocations sample_locations;
    u32 coverage_to_color; /* zero off, else the integer color attachment index+1 */
    /* with several samples, DsRasterKey's: the samples the sample mask leaves out, alpha to coverage and to one */
    u32 sample_off, alpha_to_coverage, alpha_to_one;
    u32 sample_shading; /* every sample is shaded (sampleRateShading), with several samples */
    u32 conservative;   /* DsRasterKey's (has_conservative): 0 off, else 1 plus the dilation in quarter pixels */
    /* what the pipeline gives the fragment module's specialization constants (shader.h's SH_SPEC_), of those the
       module declares; 0 is every constant at its default, a pipeline with none given. PIPE_SPEC_ALPHA's bits: the
       alpha test's function, as its difference from always (SH_SPEC_ALPHA_ALWAYS ^ the function). Then a bit each for
       the lower-left window origin and the point sprite's flipped T */
    u32 fs_spec;
} PipeKey;
#define PIPE_SPEC_ALPHA       7u
#define PIPE_SPEC_LOWER_LEFT  8u
#define PIPE_SPEC_SPRITE_FLIP 16u
/* the overestimation of conservative rasterization of a PipeKey.conservative, within the device's most (pipeline.c) */
float vk_conservative_extra(u32 conservative);
#define PIPE_MESH_WHOLE(stage) (2u << ((stage) == ST_TS)) /* PipeKey.mesh's bit of a whole stage's workgroup */
/* the pipeline for k, made when new and waited for while a worker makes it: NULL only when it can't be made */
VkPipeline vk_pipeline_get(const PipeKey* k);
/* the key a file of earlier runs' shaders and pipelines must have to be read: the device, the build, and what changes
   what a pipeline key means (dynamic vertex input among them), into out; the length of its guest part, which the
   pipelines depend on, in *guest */
void vk_pipeline_file_key(char* out, size_t n, size_t* guest);
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
/* with pipeline libraries, so far: the parts made by part (vertex input, pre-rasterization, fragment shader,
   fragment output), how many of them the pipeline cache had and ms making them; the fast links and their ms; the
   optimized pipelines made in the background and their ms */
void vk_pipeline_lib_stats(u64 made[4], u64 hits[4], u64 make_ms[4], u64* links, u64* link_ms, u64* opts,
                           u64* opt_ms);
/* so far: the draws' new pipelines that waited in the workers' queue, the ms they waited, the ms of those queued while
   every worker was busy with one or more on an optimized pipeline, and the longest wait; the most workers that made
   optimized pipelines at once */
void vk_pipeline_lib_waits(u64* waits, u64* wait_ms, u64* behind_opt_ms, u64* max_ms, int* opt_workers);
/* the shipped programs' parts warmed into the pipeline cache ahead of their draws: made, those the cache had, their ms */
void vk_pipeline_lib_warm_stats(u64* made, u64* hit, u64* ms);
void vk_tex_stats(u64* uploads, u64* bytes); /* texture uploads from guest memory so far, and their bytes */
u64 vk_tex_hashes(void); /* texture memory hashed so far, whole or sampled, to learn whether it changed */
u64 vk_rt_made(void);                        /* render targets made so far */
VkPipelineCache vk_pipeline_cache(void);     /* for pipelines made elsewhere (compute) */
/* of vk_tex_stats' uploads, the ones of some blocks alone (texture.c tex_patch): the blocks of the pages that changed
   since the last check, or that a texture with holes in its mapping has mapped */
void vk_tex_partial_stats(u64* loads, u64* bytes);
void vk_pipeline_save(int at_exit);
VkShaderModule vk_module_spv(const u32* words, size_t count);
VkShaderModule vk_module_spv_location(const u32* words, size_t count, u32 location);
/* pipelines of earlier runs made in the background: made so far, how many the pipeline cache had, and still to make */
void vk_pipeline_prewarm_stats(u64* made, u64* hits, u64* left);
/* until the workers have made every queued pipeline and wait for more: none is inside the driver then. A process that
   returns from main sooner can run the driver's exit handlers under a worker's vkCreateGraphicsPipelines (any thread,
   without vk_lock) */
void vk_pipeline_quiesce(void);
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
void vk_compute_wait(u64 value);                     /* until the compute batch is done (any thread, without vk_lock) */
void vk_compute_progress(u64* submitted, u64* done); /* the last batch and the last done (any thread) */
void vk_compute_land_through(u64 value);             /* the batches up to it, done, land (vk_lock) */
void vk_compute_before_write(u64 gva, u64 size);     /* gpu.c's g_gpu_before_write */
void vk_compute_mem_need(u64 a, u64 n);              /* a read of [a, a + n): land results there first */
void vk_compute_mem_need_map(u64 a, u64 n);          /* the GMMU changes [a, a + n): land results there first */
int vk_compute_pending(u64 a, u64 n);                /* a pending result covers a guest page of [a, a + n) */
/* compute results recorded so far (pending_pages adds): a check without a hit holds while this is unchanged */
u64 vk_compute_adds(void);
/* a pending compute result covering [gva, gva + len): the arena buffer and offset to read it from on the GPU */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off);
extern VkSemaphore g_vk_csem;  /* timeline semaphore the compute queue signals */
extern u64 g_vk_cs_wait_value; /* the value graphics work waits for before reading compute results */
extern u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_wait_ns; /* statistics */
extern u64 g_vk_cs_cpu_reads; /* dispatches kept on the compute queue because the CPU reads what they write */
/* uma.c: the device's memory model, and guest memory that the GPU reads in place (vk_memory) */
int vk_uma_choose(int has_import, int has_ahb, int has_placed);
void vk_uma_init(void); /* after it */
int vk_uma(void);       /* guest ranges are read in place where they can be */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
u64 vk_uma_bound(void); /* the submit that last read guest memory in place */
void vk_uma_frame(void);
void vk_uma_forget_all(void);

/* the dispatches skipped so far; why (size bytes) gets their reasons with each one's count, " (12 not translated, 3
   a texture not bound)", or "" when none was */
u64 vk_cs_skipped(char* why, size_t size);

/* render.c */
/* ends the dynamic rendering pass, if one is open. The caller's file and line count the passes it ended, which the
   stats every 600 presents name (vk_pass_ends_log): what splits a frame into many passes */
void vk_render_end_at(const char* file, int line);
#define vk_render_end() vk_render_end_at(__FILE__, __LINE__)
/* the guest's fragment barrier (NVN's ORDER_FRAGMENTS or INVALIDATE_TEXTURE, the 3D engine's): the open pass's later
   draws in a feedback loop see what its earlier draws wrote (feedback_order) */
void vk_render_fragment_barrier(void);
void vk_pass_ends_log(void);
/* the on-screen controls and the game's menu over a swapchain image in TRANSFER_DST_OPTIMAL, which it stays in;
   safe is the picture's part clear of the system's bars (gpu/vk/ui.c) */
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]);
/* The window's one pass over sc, which the acquire waits for at the colour output: cleared to black, src's crop drawn
   at to (transform's flips, then its quarter turn), and the on-screen layer over it; sc ends ready to present. src is
   sampled through its bytes' view into a UNORM swapchain, so nothing converts sRGB; NULL for black alone */
void vk_ui_present(VkCommandBuffer cb, VkImage sc, Surf* src, VkRect2D crop, VkRect2D to, u32 transform,
                   const int safe[4]);
void vk_ui_swapchain_gone(void); /* its images' views go before the swapchain does */
/* Whether the on-screen layer is another than the one the last pass drew: its parts are made as a frame makes them
   (what they write is kept for the next pass, which sends it), and 1 when a quad was written, their number or what the
   shaders take beside them changed, the atlas or the picture is a newer one, or no pass drew it on this swapchain yet;
   1 from then until a pass draws it. For the refresh of a game that shows nothing new (renderer.c), which draws the
   window again only for a layer that changed. Under the Vulkan lock */
int vk_ui_changed(void);
void vk_render_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o);
/* viewport index of a draw's state with the resolution scale, and the draw's facing */
VkViewport vk_viewport_from(const DsDynamic* d, const DsViewports* v, u32 index);
/* the viewport coordinate swizzle the last stage before rasterization applies, as its key's SNX_SH_SWIZZLE and
   fields: 0 where the viewport applies it (render.c). And the swizzle (DS_SWIZZLE_) viewport index draws with */
u32 vk_viewport_swizzle_key(const DsViewports* v);
u32 vk_viewport_swizzle_drawn(const DsViewports* v, u32 index);
VkFrontFace vk_render_front_face(const DsDynamic* d);
void vk_render_draw_texture(const DsState* ds, const DrawTexture* t);
void vk_render_discard(const DsState* ds, u32 targets);
void vk_render_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o);
int vk_render_xfb_control(u32 owner, u64 control, u32 what, u32 stride, DrawOwner* o);
int vk_xfb_control_buffer(VkBuffer counters, u64 counter_offset, u64 control, u32 what, u32 stride);
int vk_xfb_indirect(u64 control, VkBuffer* arguments, u64* offset);
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
   size} at off (SH_GBUF_*), where shader_gbuf_tag (shader/forms.c) found it for stage (ST_VS, ST_FS or ST_CS).
   No size takes 16 bytes, and sizes are capped at 16 MiB and rounded up to words. A bare address in a program's own
   constant buffer (shader_gbuf_tag_any, v[2] unread) takes what is mapped from it on, to the cap. */
int vk_tex_num(const u32* tic, const Surf* t);
/* the header's dimension as ShaderInfo.tex_query_hint takes it (1 + ShTex type, + 4 for an array) */
u8 vk_tic_query_hint(const u32* tic);
/* whether a texture header takes unnormalized coordinates (word 5 bit 31 clear: NVN's rectangle textures): 1 for a 2D
   one, which the translator normalizes (ShaderInfo.tex_rect_mask), 2 for another type, which it can't (texture type
   in word 4 bits 23-26; buffers' word 5 holds no such bit), 0 for none */
int vk_tic_unnormalized(const u32* tic);
int vk_image_keyable(const ShaderInfo* si, int i);
int vk_image_key(const ShaderInfo* si, int i, const u32* tic);
VkImageView vk_storage_image_view(Surf* t, const ShaderInfo* si, int i, u64* bytes);
/* the view type of storage image slot i of si: 3D for a volume, 2D array for an array (1D too), else 2D */
VkImageViewType vk_storage_image_type(const ShaderInfo* si, int i);
u32 vk_gbuf_desc(int stage, u32 off, const u32* v, u64* addr);
/* The handles of si's texture slots that the program loads from memory (SNX_TEX_MEMORY), into out[i] for slot i (0 for
   the others): ShaderInfo.memh run by sm86_eval with io (sm86.h). Returns how many. vk_memh_mem is io's guest memory
   reader, a pending write landed first */
struct Sm86EvalIo;
u32 vk_memory_handles(const ShaderInfo* si, const struct Sm86EvalIo* io, u32* out);
int vk_memh_mem(void* user, u64 address, u32* words, u32 count);
/* the access a storage image is moved to GENERAL with: image atomics read it as well as write it, and only the
   accesses a barrier names see the writes before it */
#define VK_STORAGE_IMAGE_ACCESS (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
extern u64 g_vk_drawn, g_vk_skipped; /* draws recorded, and draws skipped (unsupported topology, failed shader) */
extern u64 g_vk_mesh_draws, g_vk_mesh_drawn; /* of them, an Ounce's mesh draws, and those recorded */
/* The ordered capture fallback (render.c capture_draw) records a clear, a draw and a resolve for every primitive: past
   this many primitives a draw is skipped instead. Statistics: draws captured, their primitives, the draws skipped */
#define VK_CAPTURE_PRIMITIVES_MAX 256u
extern u64 g_vk_capture_draws, g_vk_capture_primitives, g_vk_capture_capped;
/* draws that kept the last draw's shaders and pipeline key, and those of them draw_state_check made again to compare */
extern u64 g_vk_draw_kept, g_vk_draw_checked;
/* draws, clears and the like that kept the last one's render targets, found again through the surface table before */
extern u64 g_vk_targets_kept;
/* draws that sampled a color target they render to in place, the barriers ordering them within a pass, the passes
   begun again for them (a target's layout, or ordering without those barriers), and draws that sampled a target
   through a copy instead (gpu/vk/render.c); the times a target's pixels were copied into a copy of it, for those draws
   and for textures that read a target as another size or format (gpu/vk/texture.c) */
extern u64 g_vk_fb_draws, g_vk_fb_barriers, g_vk_fb_restarts, g_vk_rt_copies, g_vk_rt_copy_writes;
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
   (vk_frame_check), a readback of GPU-owned memory, the end of a frame's slot, a long piece of work cut into
   submissions (vk_submit_split), a background encode's piece (vk_submit_background), or anything else */
enum {
    VK_SUB_OTHER,
    VK_SUB_PRESENT,
    VK_SUB_FINISH,
    VK_SUB_PERIODIC,
    VK_SUB_READBACK,
    VK_SUB_FRAME,
    VK_SUB_SPLIT,
    VK_SUB_BACKGROUND,
    VK_SUB_N
};
/* what the GPU thread waits for the graphics timeline for: a vk_finish, a frame slot's previous use, or one submission
   (a release's gate, a result's landing: vk_wait_submit) */
enum { VK_WAIT_FINISH, VK_WAIT_RECYCLE, VK_WAIT_SUBMIT, VK_WAIT_N };
extern u64 g_vk_wait_ns[VK_WAIT_N], g_vk_waits[VK_WAIT_N]; /* cmd.c: the GPU thread's waits, by kind */
/* Each dispatch recorded into the frame's command buffer, timed (record.c's rec_CmdDispatch): begin before it, 1 when
   it is timed, end after it. tag names the next one: a compute program's key, or the VK_PROF_* of one of the
   renderer's own pipelines */
int vk_gpuprof_dispatch_begin(VkCommandBuffer cb);
void vk_gpuprof_dispatch_end(VkCommandBuffer cb);
void vk_gpuprof_tag(u64 key);
/* any operation the same way: kind VK_OP_*, key what it is (a copy's: where it was recorded, from vk_gpuprof_base) */
enum { VK_OP_DISPATCH, VK_OP_COPY, VK_OP_TO_IMAGE, VK_OP_TO_BUFFER, VK_OP_IMAGE, VK_OP_DRAW, VK_OP_N };
typedef struct {
    u32 qi;
    u32 kind; /* VK_OP_* */
    u64 key, bytes;
    u16 q0, q1;
    u64 ep;
} GpOp;
/* a batch lands after the frame slot that would hold it may be reused: it has a pool of its own */
typedef struct {
    GpOp* op;
    u32 cap, nop;
    VkQueryPool pool;
    u32 queries, used;
    int reset;
    u16 span0, span1;
    u64 span_ep;
} GpOps;
#define VK_GP_OPS 256
#define VK_GP_QUERIES (2 + 2 * VK_GP_OPS)
void vk_gpuprof_compute_begin(GpOps* o, VkCommandBuffer cb);
void vk_gpuprof_compute_submit(GpOps* o, VkCommandBuffer cb);
void vk_gpuprof_compute_land(GpOps* o);
int vk_gpuprof_op_begin(VkCommandBuffer cb, u32 kind, u64 key, u64 bytes);
void vk_gpuprof_op_end(VkCommandBuffer cb);
const void* vk_gpuprof_base(void);
int vk_gpuprof_profiled(VkCommandBuffer cb); /* cb is one of the command buffers the profile times */
/* the renderer's own compute pipelines, a tag each, which gpuprof.c names: small numbers, which no program's key is in
   practice. A dispatch recorded with no tag counts as VK_PROF_NONE */
enum {
    VK_PROF_NONE,
    VK_PROF_ASTC,            /* the ASTC decode (astc.comp) */
    VK_PROF_BC7,             /* ASTC blocks made BC7 blocks (astc_bc7.comp) */
    VK_PROF_BC7_BACKGROUND,  /* a very large level's decode and fast build in the background (astc.c) */
    VK_PROF_MIRROR_FOLD,     /* a dispatch's write log into the GPU mirror's mask (mirror_fold.comp) */
    VK_PROF_MIRROR_HARVEST,  /* the mirror's pages written in a submit, out for landing (mirror_harvest.comp) */
    VK_PROF_WLOG_MERGE,      /* a logged result written back on the GPU (wlog_merge.comp) */
    VK_PROF_BL_MOVE,         /* a block linear move (blocklinear.c) */
    VK_PROF_DEPTH_PACK,      /* a depth target packed into guest layout (depth_pack.comp) */
    VK_PROF_TEXCONV,         /* a texture format converted (texconv.comp) */
    VK_PROF_VIC,             /* a VIC composition (vic.c) */
    VK_PROF_XFB_INDIRECT,    /* a transform feedback counter's draw arguments (xfb_indirect.comp) */
    VK_PROF_QUERY_PREDICATE, /* the queries' predicates (query_predicate.comp) */
    VK_PROF_QUERY_COMPARE,   /* a query compare (query_compare.comp) */
    VK_PROF_QUERY_COUNT,     /* the queries' counts (query_count.comp) */
    VK_PROF_N
};
void vk_gpuprof_cb_begin(VkCommandBuffer cb);
void vk_gpuprof_cb_end(VkCommandBuffer cb, int why, u64 value);
void vk_gpuprof_waited(u64 value, int kind);
void vk_gpuprof_frame_done(u32 slot);
/* The stats box's GPU line (runtime/platform/stats.c): while stats is on, each command buffer of the frame gets a
   timestamp at its begin and its end, and no pass does unless the profile asks. vk_gpuprof_timed: whether the graphics
   queue can be timed. vk_gpuprof_busy_ns: the time it was busy with the frames timed, summed, read when each frame
   slot is reused (its work is done then: no wait). g_vk_timestamps: the timestamps written, none while stats and the
   profile are off */
int vk_gpuprof_timed(void);
u64 vk_gpuprof_busy_ns(void);
extern u64 g_vk_timestamps;
/* the device's timestamps next to the client's clock (VK_EXT_calibrated_timestamps with a host time domain) */
int vk_gpuprof_calibrated(void);
/* a fresh pair of the GPU's clock and the client's, for the track's calibration (0 without the extension) */
int vk_gpuprof_clock(i64* cpu, i64* gpu);
extern u64 g_rec_xfers, g_rec_barriers;         /* record.c: copies and blits, and barriers, put in the frame's ring */
extern u64 g_rec_barriers_synced;               /* record.c: barriers with a pNext chain, after the ring drained */
extern u64 g_rec_xfer_kind[3], g_rec_buf_bytes; /* buffer, buffer-image, image copies; buffer copies' bytes */

/* uma.c: the device's memory model, and guest memory that the GPU reads in place (vk_memory) */
int vk_uma_choose(int has_import, int has_ahb, int has_placed);
void vk_uma_init(void); /* after it */
int vk_uma(void);       /* guest ranges are read in place where they can be */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
u64 vk_uma_bound(void); /* the submit that last read guest memory in place */
void vk_uma_frame(void);
void vk_uma_forget_all(void);

#endif
