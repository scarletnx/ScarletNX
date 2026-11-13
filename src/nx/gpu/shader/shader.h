/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Maxwell shader programs: what translation learns about one (ShaderInfo), the descriptor layout the SPIR-V uses, and
   the translator's entry points. */
#ifndef SNX_SHADER_H
#define SNX_SHADER_H
#include "common/cpu/aarch64/cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
/* program stages; the values index tables and are stored in the corpus. ST_TS and ST_MS are the task and mesh
   programs of an Ounce game (sm86.h), which only the SM86 translator takes: NVN's control section gives these stages,
   as the SPH cannot */
enum { ST_VS = 0, ST_TCS = 1, ST_TES = 2, ST_GS = 3, ST_FS = 4, ST_CS = 5, ST_TS = 6, ST_MS = 7 };
#define SH_MAXTEX          64 /* texture slots per program */
#define SH_INDEXED_TABLES  10 /* indexed texture tables per program */
/* A 1D texture is bound as a 2D image, and a buffer texture longer than a row of one as rows of 2^SH_TEXEL_ROW_SHIFT
   texels (texture.c): a 1D texel fetch at x reads (x mod row, x / row) of it, which is (x, 0) for every texture that
   fits one row */
#define SH_TEXEL_ROW_SHIFT 14
/* a texture slot */
typedef struct {
    u32 cbuf_off; /* where its handle is in the texture constant buffer, in words */
    u8 type;      /* 0 1D, 1 2D, 2 3D, 3 cube */
    u8 array, shadow;
    u8 bindless;
    u8 bl_cbuf;  /* bindless: the constant buffer holding the handle */
    u8 bl2;      /* the handle is two reads ORed: the texture's and the sampler's */
    u8 bl2_cbuf; /* the second read's constant buffer, or SH_HANDLE_IMM: bl2_off is the second half itself */
    u8 flags;    /* bit 0 raw integer surface load, bit 1 multisample fetch, bit 2 SNX_TEX_INDEXED; bits 4-7 the
                    indexed table's stride shift */
    u32 bl2_off; /* the second read's offset, in words */
    /* an indexed table whose elements aren't a power of two apart (an Ounce's, by IMAD): the bytes between them, and
       how many there are. 0: the shift in flags, and SNX_TEX_ARRAY */
    u16 stride, count;
} ShTex;
#define SH_HANDLE_IMM 255 /* ShTex.bl2_cbuf of a handle's half that is a constant (the sampler's, from a MOV32I) */
/* An Ounce program's handle that it loads from memory (ShTex.flags): one game's material programs take each texture's
   handle from a table whose address is in cbuf 0 (LDG.64, then R2UR into the uniform register pair of a TEX whose
   handle is in a uniform register, or a TEX.B's handle register). The translation keeps the instructions that compute
   it (ShaderInfo.memh) where all they read is the draw's, and the slot names the value the handle is, a probe
   (SH_PROBE): cbuf_off the texture's word, and with bl2 set, bl2_off the sampler's index, a separate word (the uniform
   register after the texture's). The renderer runs them (sm86_eval) and binds the handle as a constant buffer's */
#define SNX_TEX_MEMORY   8
/* a value of ShaderInfo.memh: the register (uniform or not) as instruction insn of the list leaves it */
#define SH_PROBE(insn, uniform, reg) ((u32)(insn) << 9 | (u32)(uniform) << 8 | (u32)(reg))
#define SH_PROBE_INSN(p)    ((p) >> 9)
#define SH_PROBE_UNIFORM(p) ((p) >> 8 & 1u)
#define SH_PROBE_REG(p)     ((p) & 0xffu)
#define SH_MEMH_MAX         64 /* instructions of ShaderInfo.memh */
/* the probe of a handle whose instructions the renderer can't run before the draw (its address differs from invocation
   to invocation): the slot is bound as no texture, which samples zeros (ShaderInfo.memh_none counts them) */
#define SH_PROBE_NONE       0xFFFFFFFEu
/* whether two indexed slots read the same handle table the same way */
static inline int shader_same_indexed_table(const ShTex* a, const ShTex* b) {
    return a->bl_cbuf == b->bl_cbuf && a->cbuf_off == b->cbuf_off && a->flags == b->flags && a->type == b->type &&
           a->array == b->array && a->shadow == b->shadow && a->stride == b->stride && a->count == b->count;
}
/* Whether two indexed slots view their elements as the same kind of image, with the integer and signed integer
   texture masks. Each kind has its own SPIR-V array, all of them on SNX_VKB_TEXARRAY (and the shadow kinds' also on
   SNX_VKB_TEXDEPTH). An element is read only through the array of its own kind, so its view always matches the type
   of the array that reads it. */
static inline int shader_same_indexed_kind(const ShTex* tex, u64 int_mask, u64 sint_mask, int a, int b) {
    return tex[a].type == tex[b].type && tex[a].array == tex[b].array && tex[a].shadow == tex[b].shadow &&
           (tex[a].flags & 3) == (tex[b].flags & 3) && ((int_mask >> a) & 1) == ((int_mask >> b) & 1) && ((sint_mask >> a) & 1) == ((sint_mask >> b) & 1);
}
/* A global buffer (ShaderInfo.gbuf_cbuf_off) is named by where its 64-bit address is: the constant buffer in bits 16-20,
   the byte offset in it in bits 0-15. NVN's storage buffer descriptors, in cbuf 0 from 0x110 to 0x410 (compute) or
   0x830, follow the address with the buffer's size (shader_gbuf_sized). deko3d puts the vertex stage's 16 at 0xb0,
   after the draw's 16 bytes and that stage's 32 texture and 8 image handles (its driver_constbuf.h), so the vertex
   stage's start there; its other stages' lie within NVN's range. A graphics program may also keep a bare
   address in a constant buffer of its own, with nothing after it: one game's terrain vertex programs read their patch
   data through such an address in cbuf 6, as Eden tracks them when no NVN descriptor holds the base
   (global_memory_to_storage_buffer_pass.cpp's Track without the NVN bias). The renderer sizes those by the memory
   mapped from the address on.
   An Ounce program may also reach a buffer through a pointer in memory (SH_GBUF_INDIRECT, bit 31): the 64-bit address
   stored at byte SH_GBUF_CELL (bits 21-30, in 8-byte units) of the buffer whose address the constant buffer word of
   bits 0-20 holds (SH_GBUF_PARENT, itself one of the program's buffers). One game's material programs load pointers
   from a table whose address is in cbuf 0, then index the tables they point to (vk/render.c binds the memory mapped
   from the pointer, which it reads from guest memory, as a bare address's).
   A compute program may load a pointer from a constant buffer at an offset it computes (LDC.64 with a register): one
   of the bank's words, which one known only when it runs. Such a pointer may be in any of the bank's windows
   (SH_GBUF_TABLE, bit 30 with bit 31 clear): window SH_GBUF_TABLE_WINDOW (bits 21-24) of bank SH_GBUF_BANK, the
   offset 0, as many windows as the buffer slots the program's other buffers leave. The renderer reads the bank as it
   binds the dispatch, takes its words that map as pointers, and makes a window of the memory mapped from the lowest
   of them, the next from the lowest past that one's end, and so on (vk/compute.c). The program reads each window's
   base from the uniform buffer at SNX_VKB_MBASE, the 64-bit base at the window's buffer slot. One game's culling
   programs keep the lists they fill, their counts and their draws' records in such a table in cbuf 4: 20 pointers
   into 12 mappings */
#define SH_GBUF_TABLE(d)           (((d) >> 30) == 1u)
#define SH_GBUF_TABLE_KEY(bank, w) (1u << 30 | (u32)(w) << 21 | (u32)(bank) << 16)
#define SH_GBUF_TABLE_WINDOW(d)    ((d) >> 21 & 15u)
#define SH_GBUF_TABLE_MAX          16u
#define SH_GBUF_BANK(d)     ((d) >> 16 & 31u)
#define SH_GBUF_OFFSET(d)   ((d) & 0xFFFFu)
#define SH_GBUF_INDIRECT(d) ((d) >> 31)
#define SH_GBUF_CELL(d)     (((d) >> 21 & 1023u) * 8u)
#define SH_GBUF_PARENT(d)   ((d) & 0x1FFFFFu)
#define SH_GBUF_CELL_MAX    8184u
static inline int shader_gbuf_sized(int stage, u32 d) {
    return d >= (stage == ST_VS ? 0xb0u : 0x110u) && d < (stage == ST_CS ? 0x410u : 0x830u);
}
/* Transform feedback (key of the last program before rasterization): for each of the 4 buffers, the attribute word
   each 4-byte word of a vertex there holds (its ISA attribute address / 4: the position's x is 28, generic i's
   component k 32 + 4 * i + k), of count words, the vertices stride bytes apart. All counts 0: nothing is captured */
typedef struct {
    u8 layout[4][128];
    u8 count[4];
    u16 stride[4];
    u8 stream[4]; /* geometry output stream for each capture buffer, 0..3 */
} ShXfb;
/* The key of a task or mesh program (ShaderInfo.mesh): the fields of its NVN control section (nvn/nvn_section.h,
   NvnSectionMesh) and its per-primitive outputs' header, which a draw gives and the SPH does not (sm86.h) */
typedef struct {
    u16 threads;                      /* the invocations of a workgroup (section 0x58), 1 to 32 */
    u16 payload;                      /* the task payload's bytes, its 16-byte header included (0x5a) */
    u16 max_vertices, max_primitives; /* a mesh program's (0x60, 0x62) */
    u8 has_task, primitive_vertices;  /* a mesh program's: after a task program (0x5e), and 3 (0x5f) */
    u16 reserved;                     /* 0 */
    u32 prim_sph[20];                 /* the per-primitive SPH's first 80 bytes (0x64, 0x68), all 0 for none */
} ShMesh;
/* A program's key state (set by the renderer before translating) and what translation found. The corpus stores it
   whole, so a corpus file only loads into a build where this struct has the same size. */
typedef struct {
    int stage;
    u32 flow_base;           /* key: low32 GPU address represented by word0, for absolute Maxwell call/jump targets */
    u64 advanced_blend;      /* key: packed NVN advanced state for an ordered storage-image epilogue, zero when off */
    u32 advanced_blend_mask; /* key: components the epilogue writes */
    u32 coverage_to_color; /* key: 0 disables, otherwise the integer output location plus one */
    u32 clip_input_mask;
    int reads_point_size;
    u32 coverage_early_tests;
    u32 sph[20];             /* the shader program header, 80 bytes */
    u32 cbuf_mask;           /* constant buffers read */
    u32 cbuf_max[18];        /* bytes read of each, so only those are bound */
    u32 root_max;            /* bytes read of the root tables (SNX_VKB_ROOT), 0 for none */
    ShTex tex[SH_MAXTEX];
    int ntex;
    u32 in_mask;       /* generic attributes 0-31 read */
    u32 attr_int_mask; /* key: vertex attributes of integer formats, and of those the signed ones */
    u32 attr_sint_mask;
    u32 clip_enable; /* key: the clip distances the 3D engine enables (SetUserClipEnable), the only ones written,
                        SNX_SH_NOT_LAST, SNX_SH_WINDOW_XY and SNX_SH_SWIZZLE */
    int uses_position_in, uses_clip_mask, writes_layer, writes_point_size;
    int uses_sample_id; /* SampleId requires sample shading, which native mixed-sample coverage cannot combine */
    int has_discard; /* guest KIL can remove a fragment after its shader starts */
    /* FP32 arithmetic asking to flush denormals (FTZ/FMZ), and FP32 arithmetic that doesn't. Only flush in every
       FADD, FMUL and FFMA lets the module declare flushing, as Eden declares DenormFlushToZero */
    int fp32_flush, fp32_preserve;
    u32 gbuf_cbuf_off[16]; /* the global buffers found, by where a constant buffer holds their address (SH_GBUF_*) */
    int ngbuf;
    /* the cells (bit k: the 8 bytes at 8 * k) of the memory a pointer table's words point to (SH_GBUF_TABLE) that the
       program loads a pointer from and then reaches memory through: the renderer binds the memory those pointers
       point to as the table's own (vk/compute.c's table_windows) */
    u32 table_cells;
    int unsupported;            /* instructions or forms translation refused */
    char first_unsupported[96]; /* the longest: an untracked handle with its writer's constant (93 bytes) */
    u32 cs_local[3];
    int shared_size;
    u32 gbuf_wmask; /* global buffers the program writes */
    u32 gbuf_rmask; /* global buffers the program reads (a load or atomic: its input must be copied for them) */
    /* The program's write log (SNX_VKB_WPAGES, SH_WLOG_*): every store and atomic of a compute program to global
       buffer i records the bytes it writes in the log's part gbuf_mark[i] - 1 (0: none), nmarks of them, numbered in
       the order of the program's first store to each buffer (sm86_translate.c) or of the buffers (the Maxwell
       translator). gbuf_marks has bit i set when gbuf_mark[i] is (write_log.c) */
    u32 gbuf_marks;
    u8 gbuf_mark[16];
    u8 nmarks;
    struct {
        u32 cbuf_off;
        u8 type, bindless, bl_cbuf, kind; /* kind: 1 raw R32ui load/atomic; formatted: 0 float, 2 uint, 3 sint */
    } img[8];
    int nimg; /* storage images stored to or used atomically */
    u32 img_int_mask, img_sint_mask; /* key: formatted storage images with unsigned/signed integer components */
    /* FS key: textures of integer formats and their signed ones. rt_int_mask and rt_sint_mask are FS's integer
       render targets; for TES rt_int_mask holds the tessellation parameters (register 0xC8), for GS the input
       primitive's vertex count */
    u64 tex_int_mask, tex_sint_mask;
    u32 rt_int_mask, rt_sint_mask;
    u32 gp_skip_mask[8];           /* GS key: POST_VTG_SHADER_ATTRIB_SKIP_MASK, a bit per scalar attribute */
    /* FS key: color textures sampled with a depth compare (TEX.DC compares their first component, which a Vulkan
       compare sampler can't do on a color format): the slots, and their compare functions, 3 bits each in slot
       order */
    u64 tex_dc_color_mask;
    u32 tex_dc_color_funcs;
    /* FS and compute key: texture slots whose filtered samples take the least of their texels in the program (the
       greatest in tex_minmax_max), as their sampler asks and the device's sampler can't (vk/render.c
       vk_tex_minmax_in_program, shader_minmax_sample). A fragment program's key holds the first SH_MINMAX_SLOTS */
    u64 tex_minmax_mask, tex_minmax_max;
    /* FS: texture slots every read of which is a texel fetch at the fragment's own pixel (TLD or TLDS of a 2D texture
       at gl_FragCoord's integer x and y, level 0, no offset): the only ones a draw may sample in place while it
       renders to them (vk/render.c), each fragment reading what earlier draws left at its pixel */
    u64 tex_own_pixel_mask;
    /* FS: texture slots every read of which is a plain sample of a 2D texture at a perspective generic's (x / w, y / w)
       (shader_direct_screen_uv), or at its x / w and y / w mapped onto [0, 1] (screen_ndc), the generic screen_in for all
       of them, or reads the own pixel as above: the fragment's own pixel's centre too when the vertex stage writes that
       generic as its position mapped onto [0, 1] (screen_out_mask), or as the position itself for screen_ndc
       (pos_out_mask), and the draw's viewport covers the target (vk/render.c) */
    u64 tex_screen_mask;
    u32 screen_in;
    /* FS: how those samples take screen_in: 0 at its (x / w, y / w), the vertex stage mapping it (screen_out_mask); 1
       at (x / w / 2 + 1/2, y / w / 2 + 1/2) and 2 at (x / w / 2 + 1/2, -y / w / 2 + 1/2), the program mapping the
       position itself (pos_out_mask) from clip space (shader_direct_screen_ndc_uv) */
    u32 screen_ndc;
    /* FS: texture slots every read of which is a plain sample of a 2D texture at a generic's (x, y) as the vertex stage
       wrote them (shader_direct_plain_uv), the generic uv_in for all of them, or reads the own pixel as above: the
       fragment's own pixel's centre too when the vertex stage writes its position from that generic, mapping [0, 1]
       onto the viewport with a w of 1 (uv_out_mask), and the draw's viewport covers the target (vk/render.c) */
    u64 tex_uv_mask;
    u32 uv_in;
    /* VS: generic outputs whose (x, y) the program maps onto its position, pos.x = x * 2 - 1 and pos.y = y * 2 - 1,
       with a w of 1, and of them in uv_out_yneg_mask those with pos.y = y * -2 + 1 (shader_direct_uv_out) */
    u32 uv_out_mask, uv_out_yneg_mask;
    /* every stage: texture slots no instruction reads a texel of: TXQ and TMML alone name them, which ask a texture's
       size, its levels or a level of detail. A draw that renders to such a slot's texture binds its copy without the
       target's pixels brought into it (vk/render.c) */
    u64 tex_unread_mask;
    /* VS: generic outputs written as the position mapped onto [0, 1], ((pos.w + pos.x) / 2, (pos.w + pos.y) / 2, -,
       pos.w), and of them in screen_out_yneg_mask those whose y is (pos.w - pos.y) / 2 */
    u32 screen_out_mask, screen_out_yneg_mask;
    /* VS: generic outputs written as the position itself: their x, y and w end with the values the position's x, y and
       w end with, so a fragment's x / w and y / w of one is its own place in the viewport (SH_VARYING_) */
    u32 pos_out_mask;
    /* FS: the generic inputs something feeds that are read with perspective, every one of them by a plain IPA: the
       ones a varying pass may take. 0 for a program with an indexed IPA or legacy varyings */
    u32 smooth_in_mask;
    u32 varying;  /* FS key: the draw's varying pass (SH_VARYING_) */
    /* VS key: 1 + the generic that takes the position as the rasterizer gets it, for the fragment program of a
       per-vertex pass (SH_VARYING_BARYCENTRIC); 0 for none */
    u32 position_copy;
    u32 out_mask; /* generic outputs 0-31 written (all of them after an attribute store at a register's offset) */
    /* FS key: generic inputs the last stage before rasterization doesn't write. They read 0, 0, 0, 1 as yuzu reads
       them (GPL-2.0-or-later, shader_recompiler emit_spirv_context_get_set.cpp): Vulkan leaves an unfed input
       undefined */
    u32 in_unfed_mask;
    /* FS key: MANDATED_EARLY_Z (0x084) on, so the depth and stencil tests run before the program, as Eden's
       force_early_z has them: a program that stores to images or buffers then leaves no trace for fragments they
       reject */
    u32 early_fragment_tests;
    ShXfb xfb;           /* key: transform feedback's capture of this program's outputs */
    int writes_viewport; /* the last vertex stage selects one of the 16 viewport/scissor pairs */
    /* key (fragment, vertex and compute programs): texture slots whose header takes unnormalized coordinates (TIC
       word 5 bit 31 clear, NVN's rectangle textures, 2D): their sample coordinates are texels, divided here by the
       size, as Ryujinx's TexturePass and Eden's texture_pass do */
    u64 tex_rect_mask;
    /* key (fragment, vertex and compute programs): the dimension of the texture slots only a TXQ names (tex_query_mask, set by the
       translation: no TEX or TLD of the program reads the same handle, so TXQ, which doesn't say, is translated as a
       2D query; a bound query's texture may be read bindlessly in the same program). Each the header's: 1 + its
       ShTex type (0 1D, 1 2D, 2 3D, 3 cube), + 4 for an array; 0 when nothing is known yet */
    u8 tex_query_hint[SH_MAXTEX];
    u64 tex_query_mask;
    ShMesh mesh; /* key (task and mesh programs) */
    /* the generics that go to the fragment program per primitive: of a mesh program, the per-primitive outputs that
       translation found (in out_mask too); and key (fragment programs), the inputs that the draw's mesh program
       writes per primitive, which the module declares PerPrimitiveEXT */
    u32 prim_mask;
    int mixed_sample_invariant; /* proof: guest FS values and discard decisions do not depend on an invocation */
    u32 late_depth; /* key: ordered GPU depth/stencil shadow operations after guest fragment code */
    u32 capture_outputs; /* key: capture validity location2..7; zero retains the original location1 interface */
    int primitive_split_safe; /* proof: vertex values do not depend on subgroup/lane/clock invocation groups */
    /* An Ounce program's instructions that compute its texture handles from memory (SNX_TEX_MEMORY slots), in program
       order, each its lo and hi words: the integer ALU, constant buffer and global loads that sm86_eval runs */
    u64 memh[SH_MEMH_MAX][2];
    u32 nmemh;
    u32 memh_none;     /* texture instructions whose handle from memory is SH_PROBE_NONE, */
    char memh_why[64]; /* and why the first is */
    /* TEX.B instructions whose handle the writes don't agree on, which sample each constant buffer handle that reaches
       them and keep the sample of the one whose word the register holds (shader_direct_handle_candidates): a handle
       equal to none of them samples zeros, which the log says once a program, naming the first such TEX.B */
    u32 chosen_tex;
    char chosen_why[96]; /* "TEX.B at <byte offset>, R<handle>: c[<bank>][<byte>], ..." */
    u32 spec;          /* the specialization constants the module declares: bit SH_SPEC_ of each */
} ShaderInfo;
/* A fragment module's specialization constants, by SpecId: state a draw has before its program runs, which the
   pipeline gives the module (vk/pipeline.c, PipeKey.fs_spec) so that the driver compiles the program for it. As a
   push constant the same state is a value the program computes with for every fragment: the alpha test compared the
   function with each of its eight values. A program is translated once whatever the state; a pipeline is made for
   each value a program is drawn with, and only a module that declares the constant (ShaderInfo.spec) has one */
enum {
    /* the alpha test's function, in the 3D engine's order: 0 never, 1 less, 2 equal, 3 less or equal, 4 greater, 5 not
       equal, 6 greater or equal, 7 always. With the test off it is 7, the module's default */
    SH_SPEC_ALPHA = 0,
    /* 1 when the window's origin is the lower left: the fragment's y then counts up from the surface clip's bottom
       (shader_frag_y_origin, shader_scaled_fetch). 0, the upper left, is the default */
    SH_SPEC_LOWER_LEFT,
    /* 1 when a point sprite's T runs against PointCoord's (the sprite's origin and the window's, vk/render.c) */
    SH_SPEC_SPRITE_FLIP,
    SH_SPEC_COUNT
};
#define SH_SPEC_ALPHA_ALWAYS 7u
/* The graphics cache packs these explicit fragment-only fields above its eight image-format bits. */
enum { SH_FS_LATE_DEPTH = 1u, SH_FS_CAPTURE_SHIFT = 1, SH_FS_VARYING_SHIFT = 4, SH_FS_CONFIG_MASK = 0xFFFFFFu };
/* ShaderInfo.varying, a fragment program's key from SH_FS_VARYING_SHIFT up of those fields. A host GPU that clips a
   huge primitive (AMD, Intel, Arm) interpolates the inputs of the clipped vertices with less precision than the
   position, where the console interpolates the vertices' own values: a generic holding the position, divided by its
   w, misses the fragment's pixel by a pixel or more, and the depth a program fetches there is a neighbour's (Eden's
   position_varying_pass.cpp, GPL-3.0-or-later: John and Amicuchu's finding). Its low 2 bits name the pass that
   makes up for it.
   SH_VARYING_POSITION, the position pass: up to SH_VARYING_POSITIONS perspective inputs the vertex stage writes as
   its position (pos_out_mask), 1 + each one's generic in 6 bits from bit 2 up. Their x and y over w read as the
   fragment's own place in clip space, from gl_FragCoord and the draw's viewport (shader_frag_ndc), and their w over
   w as 1.
   SH_VARYING_BARYCENTRIC, the per-vertex pass, which costs GPU time on every draw it changes and is for the huge
   primitives alone: every generic of smooth_in_mask comes to the fragment program as its three vertices' values
   (PerVertexKHR, VK_KHR_fragment_shader_barycentric), and so does the position, in the free generic SH_VARYING_SLOT
   names (the vertex program's position_copy). The program interpolates them with the fragment's own weights among
   the three positions (shader_frag_weights), in 32-bit floats: a fraction of a pixel off on the largest primitives,
   so the position inputs, up to two of them from bit 7 up, still read the position pass's exact value.
   SH_VARYING_GEOMETRY, the per-vertex pass on a device without that extension: a geometry program made for the draw
   (vk/varying_geometry.c) hands the fragment program the same values as flat inputs. The three positions go in the
   generics from SH_VARYING_SLOT on; each generic of the pass keeps its first vertex's value, and its second and
   third vertices' go in the next two free generics, in the generics' order (shader_varying_copy) */
enum { SH_VARYING_POSITION = 0, SH_VARYING_BARYCENTRIC = 1, SH_VARYING_GEOMETRY = 2 };
#define SH_VARYING_SLOT(varying) ((varying) >> 2 & 31)
#define SH_VARYING_POSITIONS 3
static inline u32 shader_varying_positions(u32 generics) {
    u32 key = 0;
    for (u32 g = 0, k = 0; g < 32 && k < SH_VARYING_POSITIONS; g++)
        if (generics >> g & 1) key |= (g + 1) << (2 + 6 * k++);
    return key | SH_VARYING_POSITION;
}
static inline u32 shader_varying_per_vertex(u32 pass, u32 slot, u32 positions) {
    return pass | slot << 2 | (shader_varying_positions(positions) >> 2 & 0xFFF) << 7;
}
/* SH_VARYING_GEOMETRY: the generic that holds vertex 1 + k's value (k 0 or 1) of the pass's generic g, per_vertex
   the generics the pass takes and slot the first of the position's three; 32 and up when the generics run out */
static inline u32 shader_varying_copy(u32 per_vertex, u32 slot, u32 g, u32 k) {
    u32 before = 0;
    for (u32 i = 0; i < g; i++) before += per_vertex >> i & 1;
    return slot + 3 + 2 * before + k;
}
static inline u32 shader_varying_position_mask(u32 varying) {
    u32 generics = 0, first = (varying & 3) == SH_VARYING_POSITION ? 2 : 7;
    for (u32 at = first; at + 6 <= 2 + 6 * SH_VARYING_POSITIONS; at += 6)
        if (varying >> at & 63) generics |= 1u << ((varying >> at & 63) - 1);
    return generics;
}
/* ShaderInfo.tex_query_hint in a graphics key's words (vk_shader_get's mask words 14 to 19: 3 bits a slot, the 64
   slots in 6 words), and back. A 1D header's hint (1) goes as none: its query is the 2D slot's, a texel high, as the
   translation binds 1D textures */
#define SH_QUERY_WORDS 6
static inline void shader_query_hint_pack(const u8 hint[SH_MAXTEX], u32 words[SH_QUERY_WORDS]) {
    for (int i = 0; i < SH_QUERY_WORDS; i++) words[i] = 0;
    for (int k = 0; k < SH_MAXTEX; k++) {                       /* a slot's 3 bits may straddle two words */
        u64 v = hint[k] > 1 && hint[k] <= 8 ? hint[k] - 1u : 0; /* 1 to 7: hints 2 to 8 */
        int at = 3 * k;
        v <<= at % 32;
        words[at / 32] |= (u32)v;
        if (at / 32 + 1 < SH_QUERY_WORDS) words[at / 32 + 1] |= (u32)(v >> 32);
    }
}
static inline void shader_query_hint_unpack(const u32 words[SH_QUERY_WORDS], u8 hint[SH_MAXTEX]) {
    for (int k = 0; k < SH_MAXTEX; k++) {
        int at = 3 * k;
        u64 w = words[at / 32] | (at / 32 + 1 < SH_QUERY_WORDS ? (u64)words[at / 32 + 1] << 32 : 0);
        u32 v = (u32)(w >> (at % 32)) & 7;
        hint[k] = v ? (u8)(v + 1) : 0;
    }
}
#define SH_DC_COLOR_MAX  10 /* slots tex_dc_color_funcs has room for */
#define SH_MINMAX_SLOTS    15 /* slots a fragment program's key has room for in tex_minmax_mask (vk/pipeline.c) */
/* Each stage has a descriptor set (shader_vk_set), its bindings grouped by kind. */
#define SNX_VKB_CB       0                         /* 18 constant buffers */
#define SNX_VKB_SSBO     18                        /* 16 global memory buffers */
#define SNX_VKB_TEX      34                        /* SH_MAXTEX combined image samplers */
#define SNX_VKB_IMG      (SNX_VKB_TEX + SH_MAXTEX) /* 8 storage images (compute, vertex, fragment) */
#define SNX_VKB_RS       (SNX_VKB_IMG + 8)         /* fragment: 1 / the resolution scale (SNX_RS) */
#define SNX_VKB_FMT      (SNX_VKB_RS + 1)          /* compute: the images' format classes (SNX_FMT) */
#define SNX_VKB_SMAP     (SNX_VKB_FMT + 1)         /* graphics: storage buffer page indices for aliased bindings */
/* compute, the same binding, which no compute program reads otherwise: the write log the program's stores record their
   bytes in (gbuf_mark, SH_WLOG_*). With the GPU mirror, a pass after the dispatch puts the log's bits in the mirror's
   mask (vk/mirror.c vk_mirror_fold) */
#define SNX_VKB_WPAGES   SNX_VKB_SMAP
/* compute, the fragment program's binding, which no compute program has otherwise: the windows of its pointer tables
   (SH_GBUF_TABLE), a uniform buffer of 16 vec4 of words, one for each global buffer slot: the low and high words of
   the window's base and its length in bytes */
#define SNX_VKB_MBASE    SNX_VKB_RS
/* The write log: for each logged global buffer (ShaderInfo.gbuf_mark), a part of SH_WLOG_PART words. Its first
   SH_WPAGE_SLOT words are flags, word j set to 1 by any store to the 4 KiB window j of the binding; then a bit for each
   byte of each window, SH_WLOG_MASK words a window, bit b of word k of window j standing for byte j * 4096 + k * 32 +
   b, set by the store that writes that byte. A store past the windows sets no bit. The flags are an index, so that
   reading and clearing the log costs the windows written, not the windows bound */
#define SH_WPAGE_SLOT    4096u /* the windows of a global buffer: 16 MiB, the most vk_gbuf_desc binds */
#define SH_WLOG_MASK     128u  /* the mask words of a window: a bit for each of its 4096 bytes */
#define SH_WLOG_PART     (SH_WPAGE_SLOT * (1u + SH_WLOG_MASK))
#define SNX_VKB_ROOT     (SNX_VKB_SMAP + 1)      /* an Ounce program's root tables, banks 24 to 31 (SH_ROOT_BYTES) */
#define SNX_VKB_IMGBUF   (SNX_VKB_ROOT + 1)      /* 8 buffer images (compute, vertex, fragment) */
#define SNX_VKB_BLEND    SNX_VKB_IMG             /* fragment: ordered blend uses IMG+nimg, after the guest images */
#define SNX_VKB_CAPTURE  (SNX_VKB_IMGBUF + 8)    /* fragment: sparse capture tile flags and dispatch list */
#define SNX_VKB_TEXARRAY (SNX_VKB_CAPTURE + 1)   /* up to SNX_TEX_ARRAY distinct indexed handles */
#define SNX_VKB_TEXFUNCS (SNX_VKB_TEXARRAY + 1)  /* their depth compare functions */
#define SNX_VKB_TEXDEPTH (SNX_VKB_TEXFUNCS + 1)  /* their depth views */
#define SNX_VKB_TEXMAP   (SNX_VKB_TEXDEPTH + 1)  /* table and element to compact descriptor index */
#define SNX_VKB_END      (SNX_VKB_TEXMAP + 1)    /* one past the last binding */
/* Transform feedback by stores (g_shader_xfb_stores), in the global buffer slots after a program's own (at most
   SNX_XFB_GBUF_MAX): the parameters (first vertex, first instance, vertices an instance and a draw capture), then the
   4 buffers from where the capture starts */
#define SNX_XFB_GBUF_MAX   11
#define SNX_VKB_XFB_PARAMS (SNX_VKB_SSBO + SNX_XFB_GBUF_MAX)
#define SNX_VKB_XFB        (SNX_VKB_XFB_PARAMS + 1)
/* An indexed texture loads its handle with an LDC whose address has a register in it: one of SNX_TEX_ARRAY handles
   from cbuf_off on, each (1 << shift) bytes after the last, with the shift in bits 4-7 of ShTex.flags. */
#define SNX_TEX_ARRAY    256
/* An Ounce program reads its root tables (push constants) as banks 24 to 31: 8 tables of 256 bytes, which the
   renderer binds as one uniform buffer, table t at 256 t */
#define SH_ROOT_BANK     24
#define SH_ROOT_TABLES   8
#define SH_ROOT_TABLE    256
#define SH_ROOT_BYTES    (SH_ROOT_TABLES * SH_ROOT_TABLE)
#define SNX_TEX_INDEXED  4
/* Device capabilities the renderer sets before translating: */
/* sampled image arrays indexed per invocation (non-uniform indexing) */
extern int g_shader_texture_arrays;
/* the elements of each of those arrays: SNX_TEX_ARRAY, or fewer where the device binds fewer combined image samplers
   (vk_pipeline_init, vk/texture_arrays.h); a test may set fewer before the renderer starts */
extern u32 g_shader_tex_array;
/* constant buffers from this slot up are read-only storage buffers, on a device with fewer uniform buffers a stage
   than the 18 slots and the renderer's own need (18: none) */
extern u32 g_shader_uniform_cbufs;
/* tessellation and geometry programs may write the point size (shaderTessellationAndGeometryPointSize) */
extern int g_shader_stage_point_size;
/* vertex and tessellation programs may write the layer (shaderOutputLayer, the ShaderLayer capability); without it
   their layer writes go away (layer 0). 1 offline */
extern int g_shader_output_layer;
/* ViewportIndex output: bit 0 vertex/evaluation, bit 1 geometry. 3 offline. */
extern int g_shader_viewport_index;
/* the Geometry capability, for fragment programs reading Layer or PrimitiveId */
extern int g_shader_geometry;
extern int g_shader_geometry_streams;
extern int g_shader_geometry_streams_lines_triangles;
/* shaderFloat16, which fp16 texture results need. 1 offline */
extern int g_shader_float16;
/* shaderResourceMinLod: fragment TEX.LC uses the MinLod image operand. 1 offline. */
extern int g_shader_resource_min_lod;
/* maintenance8 permits a runtime Offset operand on texture samples. 0 for portable offline output. */
extern int g_shader_dynamic_texture_offsets;
/* Exact native half conversion: bit0 controls16, bit1 also controls64 for linked host modes. */
extern int g_shader_half_rounding;
/* bit 0 for OpConvertSToF, bit 1 for OpConvertUToF: the driver's conversion of a 32-bit integer to a 32-bit float
   truncates (extension_bans.txt), and I2F.F32 of a 32-bit integer converts its halves and adds them
   (spv_emit_int_to_f32_nearest), rounding to nearest as the console does. 0 offline */
extern int g_shader_i2f_nearest;
/* shaderDrawParameters: a vertex program's instance ID is InstanceIndex less BaseInstance, which an indirect draw's
   arguments set on the GPU. 1 offline */
extern int g_shader_draw_parameters;
/* no VK_EXT_transform_feedback but vertex stores: vertex programs store what is captured (SNX_VKB_XFB). 0 offline */
extern int g_shader_xfb_stores;
/* NVN's draw parameters: its driver writes each draw's base vertex and base instance into constant buffer 0 at these
   offsets, which vertex programs read (an instanced program adds the base instance to its instance ID). An indirect
   draw's are in its record, on the GPU: with draw parameters, a vertex program's reads of these words take
   BaseVertex and BaseInstance where the push constant's z has SH_PUSH_GPU_BASE_VERTEX and SH_PUSH_GPU_BASE_INSTANCE
   set (render.c) */
#define SH_CB0_BASE_VERTEX        0x640u
#define SH_CB0_BASE_INSTANCE      0x644u
#define SH_PUSH_GPU_BASE_VERTEX   1u
#define SH_PUSH_GPU_BASE_INSTANCE 2u
/* An Ounce's indirect mesh draw (DrawMeshTasksIndirect): its first workgroup is in its record, on the GPU. Where the
   push constant's z has this bit, a task program, or a mesh program without one, adds the word at SNX_VKB_RS (a
   uniform buffer the renderer copies the record's first word into, vk/render.c's mesh_indirect) to its workgroup ID in
   place of the push constant's x */
#define SH_PUSH_GPU_MESH_FIRST    4u
/* quad operations in fragment programs (the subgroup's QUAD bit): SHFL within a quad reads its helper lanes. 1 offline */
extern int g_shader_quad;
/* quad operations in compute programs (the QUAD bit, compute among the subgroup stages): an SM86 HMMA reads its A
   fragments from the lane's own quad with them, a DPP move on AMD where a shuffle goes through LDS. 1 offline */
extern int g_shader_quad_compute;
/* VK_VALVE_shader_mixed_float_dot_product's shaderMixedFloatDotProductFloat16AccFloat32, with shaderFloat16: an SM86
   HMMA sums each pair of f16 products into its f32 accumulator with one OpFDot2MixAcc32VALVE (v_dot2_f32_f16 on AMD)
   instead of unpacking both pairs to f32. 0 offline */
extern int g_shader_mixed_dot;
/* The hmma_f16 setting: an SM86 HMMA's products and their sum in half floats, two products to an OpFma where the f32
   sum takes two instructions, for a device whose driver offers no matrix unit (every MoltenVK so far). Its results
   are about 1e-3 off the console's f32 sum; 0, the default, is the f32 sum the console's tensor core keeps */
extern int g_shader_hmma_f16;
/* VK_KHR_cooperative_matrix's 16x16x16 product of f16 A and B into an f32 accumulator in subgroup scope, in compute,
   with the Vulkan memory model (vulkanMemoryModel, vulkanMemoryModelDeviceScope) and compute subgroups of 32 that a
   pipeline can require: the device's maxComputeSharedMemorySize, else 0. An SM86 compute program's HMMAs are then each
   one OpCooperativeMatrixMulAddKHR on fragments staged in shared memory after the program's (sm86_translate.c's
   hmma_coop), where the two fit in that size. 0 offline */
extern int g_shader_hmma_coop;
/* an SM86 HMMA off the cooperative matrices gathers its B fragment in pairs of lanes 4 apart, which multiply by the
   same 16 words of it: each shuffles in half of them and takes the other half from its pair with a xor of 4
   (sm86_translate.c's hmma). RADV with subgroups of 32 (GFX10 on): half of the B permutes through LDS become DPP
   moves. 0 offline */
extern int g_shader_hmma_pairs;
/* the way a compute program's HMMA takes with the flags above, in words for the log, into out (n bytes) */
void shader_hmma_way(char* out, size_t n);
/* an SM86 HMMA off the cooperative matrices takes A and B from the other lanes through shared memory, a slot a warp,
   where the driver compiles a shuffle whose index differs between lanes into a loop over the distinct indices (an
   "off" line on OpGroupNonUniformShuffle in vk/extension_bans.txt: Turnip, whose Adreno 610 ran the HMMA bench 35
   times faster so, 684.9 ms a dispatch to 19.6). Where the shuffles are the hardware's they stay: staged, an Adreno
   830 ran 5.4 and an RX 6700 XT 2.2 times slower. The device's maxComputeSharedMemorySize, else 0 (offline) */
extern int g_shader_hmma_stage;
/* g_shader_hmma_pairs and g_shader_hmma_stage from what vk/device.c reads of the device: subgroup operations in
   compute (subgroups), RADV with subgroups of 32 a pipeline can require (radv_warp32: its xor of 4 is a DPP move
   where a shuffle at any other index is a permute through LDS), an "off" line on OpGroupNonUniformShuffle that holds
   for its driver (shuffle_loops), and its maxComputeSharedMemorySize */
void shader_hmma_choose(int subgroups, int radv_warp32, int shuffle_loops, int shared_max);
/* The device may flush f16 subnormals (no shaderDenormPreserveFloat16), as Turnip on an Adreno 610 does in
   PackHalf2x16: an SM86 program packs its halves below 2^-14 exactly then (sm86_translate.c's pack_half). 0 offline */
extern int g_shader_half_flush;
/* The tile that device's cooperative matrices come in, when it has them: 16 for an M by N by K of 16x16x16, which
   RADV offers, 8 for Metal's simdgroup_matrix of 8x8x8, which a MoltenVK with VK_KHR_cooperative_matrix reports
   (hmma_coop). One SM86 HMMA is four 8x8x8 products there and one 16x16x16 otherwise */
extern int g_shader_hmma_coop_tile;
/* the most mesh workgroups one task workgroup can launch (maxMeshWorkGroupCount[0] and maxMeshWorkGroupTotalCount,
   the less of them): a task program's count, read from its payload's header, is clamped to it, as a count past it is
   undefined behavior that can hang the GPU. 0 offline: not clamped */
extern u32 g_shader_mesh_tasks_max;
/* VK_EXT_shader_atomic_float's shaderBufferFloat32AtomicAdd: an SM86 RED.ADD.F32 or ATOMG.ADD.F32 on global memory is
   one OpAtomicFAddEXT, else a compare and exchange loop on the word's bits (sm86_translate.c's atomic_fadd). 0 offline */
extern int g_shader_float_atomic_add;
/* the Tessellation capability, which also gives a fragment program PrimitiveId without Geometry */
extern int g_shader_tessellation;
/* VK_KHR_shader_clock's scopes: bit 0 subgroup, bit 1 device */
extern int g_shader_clock_scopes;
/* VK_KHR_shader_maximal_reconvergence: a structured program's lanes reconverge where its constructs merge, so one
   whose lanes must reconverge (subgroup operations, barriers, derivatives) takes structured control flow too */
extern int g_shader_maximal_reconvergence;
/* in clip_enable: a later stage before rasterization (tessellation or geometry) follows, and the last one applies
   the depth mode and the clip distances */
#define SNX_SH_NOT_LAST  (1u << 31)
/* in clip_enable, the last stage's: the viewport transform is off (3D register 0x64B), so the position's x and y are
   pixels of the surface clip, which the stage takes to clip space by its width and height (the push constant's w,
   vk/render.c) */
#define SNX_SH_WINDOW_XY (1u << 30)
/* in clip_enable, the last stage's: it gives its position the viewport coordinate swizzle in bits 8-23 before
   clipping, as the 3D engine does (DsViewports.swizzle's fields, shifted by SNX_SH_SWIZZLE_SHIFT): one the viewport
   can't apply (vk/render.c vk_viewport_swizzle_key) */
#define SNX_SH_SWIZZLE       (1u << 29)
#define SNX_SH_SWIZZLE_SHIFT 8
/* the descriptor set of a stage's bindings */
int shader_vk_set(int stage);
/* can_flush: how the module gets the console's FP32 flush to zero (FTZ on FADD, FMUL and FFMA). SH_FTZ_EMULATE: a
   test and a select on each such operand and result; SH_FTZ_MODE: the DenormFlushToZero execution mode, in a module
   whose every such instruction flushes; SH_FTZ_NATIVE: neither, on a device that flushes them by itself (vk/ftz_probe.c) */
#define SH_FTZ_EMULATE 0
#define SH_FTZ_MODE    1
#define SH_FTZ_NATIVE  2
/* Translates a program (code, readable up to max_bytes) to SPIR-V: malloc'd words, their count in word_count. NULL
   when the program uses something the translator can't express, with si->unsupported and the reason set. */
u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count, int can_flush,
                     int can_preserve, int can_preserve_f64, int can_subgroup);
u32* shader_compute_to_spirv(const u8* code, u32 max_bytes, ShaderInfo* si, size_t* word_count, int can_flush,
                             int can_preserve);
/* shared_reach.c: one past the highest byte of shared memory a compute program (code, readable up to max_bytes, of
   blocks local[0] x local[1] x local[2]) can touch with LDS, STS, ATOMS and ATOMS.CAS as its translation runs it, 0
   for none, and SH_SHARED_ANY when an address has no bound the walk proves: a value from memory or a constant buffer,
   a loop's counter, a 32-bit product of registers, a call, a jump table or more than 4096 blocks */
#define SH_SHARED_ANY 0xFFFFFFFFu
u32 shader_shared_reach(const u8* code, u32 max_bytes, const u32 local[3]);
/* the shared memory a compute program declares on a device that gives a block device_max bytes, for a launch asking
   for shm: 1 with shm in *size when it fits, or device_max when shader_shared_reach (in *reach) proves no access goes
   past that; 0 when one may, the dispatch then left out */
int shader_shared_fit(const u8* code, u32 max_bytes, const u32 local[3], u32 shm, u32 device_max, u32* size,
                      u32* reach);
/* reads word off (bytes) of constant buffer idx for jump tables, into *out as a host number: 1 when it could. Set by
   whoever translates, NULL offline */
extern int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
/* The programs are SM86 (sm86.h), not Maxwell: an Ounce game's. nvn_direct sets it once, from the game's NVN API
   version, before the game's first draw */
extern int g_shader_sm86;
/* a folder that sm86_debug_dump writes refused SM86 programs to (the sm86_dump setting), NULL for none */
extern const char* g_shader_sm86_dump;
/* the targets of the BRX at word index from its jump table, at most max_targets, or -1 */
int shader_brx_targets(const u64* words, u32 index, u32 count, u32* targets, int max_targets);
/* the program's hash, and its length in bytes in out_len */
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len);
int shader_absolute_flow(const u8* code, u32 bytes);
/* BRX reaching-definition proof inspects later incoming branches, requiring the complete code window. */
int shader_code_full_window(const u8* code, u32 bytes);
/* The shader corpus (shader_corpus=dir): each translated program's code and ShaderInfo, so tools/snx_shaderc.c
   can translate it again offline. A file only loads into a build with the same ShaderInfo. */
int shader_corpus_save(const char* dir, const u8* code, u32 len, u32 max_bytes, int stage, const ShaderInfo* si,
                       u64 key);
/* malloc'd code, the capture's max_bytes long with zeros past its recorded length, which goes in *max_bytes */
u8* shader_corpus_load(const char* path, u32* max_bytes, int* stage, ShaderInfo* si);
/* VertexA and VertexB as one program, A first; 0 when it doesn't fit */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes);
#ifdef __cplusplus
}
#endif
#endif
