/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Generated (parallel draw batches, docs/dev/parallel-draw-batches.md): the functions of the other files that
   render.c calls, each under the share lock (vk_share_lock), so that the renderer thread and a worker never run
   two of them at once. render.c includes this after its other headers */
#ifndef SNX_VK_SHARE_WRAP_H
#define SNX_VK_SHARE_WRAP_H
static inline void sh_vk_all_barrier_by(int site, VkCommandBuffer cb) {
    if (!vk_share_on()) {
        vk_all_barrier_by(site, cb);
        return;
    }
    vk_share_lock_at(0);
    vk_all_barrier_by(site, cb);
    vk_share_unlock();
}
static inline int sh_vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_copy(gva, len, buf, off);
    vk_share_lock_at(2);
    int const r = vk_buf_copy(gva, len, buf, off);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered) {
    if (!vk_share_on()) return vk_buf_copy_writable(gva, len, buf, off, ordered);
    vk_share_lock_at(3);
    int const r = vk_buf_copy_writable(gva, len, buf, off, ordered);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_copy_writable_word(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered) {
    if (!vk_share_on()) return vk_buf_copy_writable_word(gva, len, buf, off, ordered);
    vk_share_lock_at(4);
    int const r = vk_buf_copy_writable_word(gva, len, buf, off, ordered);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_draw_has_alias(const VkDrawStorage* draw) {
    if (!vk_share_on()) return vk_buf_draw_has_alias(draw);
    vk_share_lock_at(5);
    int const r = vk_buf_draw_has_alias(draw);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_draw_prepare(VkDrawStorage* draw) {
    if (!vk_share_on()) return vk_buf_draw_prepare(draw);
    vk_share_lock_at(6);
    int const r = vk_buf_draw_prepare(draw);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_get_at(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_get_at(gva, len, align, buf, off);
    vk_share_lock_at(7);
    int const r = vk_buf_get_at(gva, len, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_get_input(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_get_input(gva, len, align, buf, off);
    vk_share_lock_at(8);
    int const r = vk_buf_get_input(gva, len, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_get_storage(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_get_storage(gva, len, align, buf, off);
    vk_share_lock_at(9);
    int const r = vk_buf_get_storage(gva, len, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_get_uniform_at(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_get_uniform_at(gva, len, align, buf, off);
    vk_share_lock_at(10);
    int const r = vk_buf_get_uniform_at(gva, len, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_get_window(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_get_window(gva, len, align, buf, off);
    vk_share_lock_at(11);
    int const r = vk_buf_get_window(gva, len, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_get_window_input(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_buf_get_window_input(gva, len, align, buf, off);
    vk_share_lock_at(12);
    int const r = vk_buf_get_window_input(gva, len, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_buf_mem_need(u64 gva, u64 len) {
    if (!vk_share_on()) {
        vk_buf_mem_need(gva, len);
        return;
    }
    vk_share_lock_at(13);
    vk_buf_mem_need(gva, len);
    vk_share_unlock();
}
static inline u32 sh_vk_buf_own_got(u64* id) {
    if (!vk_share_on()) return vk_buf_own_got(id);
    vk_share_lock_at(14);
    u32 const r = vk_buf_own_got(id);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_own_holds(u32 i, u64 id) {
    if (!vk_share_on()) return vk_buf_own_holds(i, id);
    vk_share_lock_at(15);
    int const r = vk_buf_own_holds(i, id);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_owned(u64 gva, u64 len) {
    if (!vk_share_on()) return vk_buf_owned(gva, len);
    vk_share_lock_at(16);
    int const r = vk_buf_owned(gva, len);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buf_pending(u64 gva, u64 len) {
    if (!vk_share_on()) return vk_buf_pending(gva, len);
    vk_share_lock_at(17);
    int const r = vk_buf_pending(gva, len);
    vk_share_unlock();
    return r;
}
static inline u64 sh_vk_buffer_address(VkBuffer b) {
    if (!vk_share_on()) return vk_buffer_address(b);
    vk_share_lock_at(18);
    u64 const r = vk_buffer_address(b);
    vk_share_unlock();
    return r;
}
static inline VkResult sh_vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer, DevMem* memory, DevMemInfo* details) {
    if (!vk_share_on()) return vk_buffer_create(info, allocation, buffer, memory, details);
    vk_share_lock_at(19);
    VkResult const r = vk_buffer_create(info, allocation, buffer, memory, details);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_buffer_image(const u32* tic, int kind, VkBufImg* out) {
    if (!vk_share_on()) return vk_buffer_image(tic, kind, out);
    vk_share_lock_at(20);
    int const r = vk_buffer_image(tic, kind, out);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_capture_clear(VkCapture* capture) {
    if (!vk_share_on()) return vk_capture_clear(capture);
    vk_share_lock_at(21);
    int const r = vk_capture_clear(capture);
    vk_share_unlock();
    return r;
}
static inline VkCapture* sh_vk_capture_get(Surf* target) {
    if (!vk_share_on()) return vk_capture_get(target);
    vk_share_lock_at(22);
    VkCapture* const r = vk_capture_get(target);
    vk_share_unlock();
    return r;
}
static inline VkCapture* sh_vk_capture_get_mixed_targets(Surf* const* targets, u32 count, u32 raster_samples) {
    if (!vk_share_on()) return vk_capture_get_mixed_targets(targets, count, raster_samples);
    vk_share_lock_at(23);
    VkCapture* const r = vk_capture_get_mixed_targets(targets, count, raster_samples);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_capture_mixed_resolve(VkCapture* capture, const VkCoverage* coverage, const DsState* state, u32 outputs) {
    if (!vk_share_on()) return vk_capture_mixed_resolve(capture, coverage, state, outputs);
    vk_share_lock_at(24);
    int const r = vk_capture_mixed_resolve(capture, coverage, state, outputs);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_capture_mixed_source(VkCapture* capture, u32 index) {
    if (!vk_share_on()) return vk_capture_mixed_source(capture, index);
    vk_share_lock_at(25);
    Surf* const r = vk_capture_mixed_source(capture, index);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_capture_resolve(VkCapture* capture, u64 blend, u32 flags, int coverage) {
    if (!vk_share_on()) return vk_capture_resolve(capture, blend, flags, coverage);
    vk_share_lock_at(26);
    int const r = vk_capture_resolve(capture, blend, flags, coverage);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_compute_gpu_src(gva, len, buf, off);
    vk_share_lock_at(27);
    int const r = vk_compute_gpu_src(gva, len, buf, off);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_compute_mem_need(u64 a, u64 n) {
    if (!vk_share_on()) {
        vk_compute_mem_need(a, n);
        return;
    }
    vk_share_lock_at(28);
    vk_compute_mem_need(a, n);
    vk_share_unlock();
}
static inline int sh_vk_compute_pending(u64 a, u64 n) {
    if (!vk_share_on()) return vk_compute_pending(a, n);
    vk_share_lock_at(29);
    int const r = vk_compute_pending(a, n);
    vk_share_unlock();
    return r;
}
static inline float sh_vk_conservative_dilation(u32 conservative, float most, char* line, size_t n) {
    if (!vk_share_on()) return vk_conservative_dilation(conservative, most, line, n);
    vk_share_lock_at(30);
    float const r = vk_conservative_dilation(conservative, most, line, n);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_defer_buffer(VkBuffer b, DevMem a) {
    if (!vk_share_on()) {
        vk_defer_buffer(b, a);
        return;
    }
    vk_share_lock_at(31);
    vk_defer_buffer(b, a);
    vk_share_unlock();
}
static inline void sh_vk_defer_buffer_view(VkBufferView v) {
    if (!vk_share_on()) {
        vk_defer_buffer_view(v);
        return;
    }
    vk_share_lock_at(32);
    vk_defer_buffer_view(v);
    vk_share_unlock();
}
static inline int sh_vk_depth_shadow_aliases(const Surf* depth, const Surf* other) {
    if (!vk_share_on()) return vk_depth_shadow_aliases(depth, other);
    vk_share_lock_at(33);
    int const r = vk_depth_shadow_aliases(depth, other);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_depth_shadow_cancel(void) {
    if (!vk_share_on()) {
        vk_depth_shadow_cancel();
        return;
    }
    vk_share_lock_at(34);
    vk_depth_shadow_cancel();
    vk_share_unlock();
}
static inline int sh_vk_depth_shadow_finish(Surf* surface) {
    if (!vk_share_on()) return vk_depth_shadow_finish(surface);
    vk_share_lock_at(35);
    int const r = vk_depth_shadow_finish(surface);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_depth_shadow_prepare(Surf* surface, const DsDynamic* dynamic, const DrawCall* call, u32 fixed_mask, VkDescriptorBufferInfo* descriptor) {
    if (!vk_share_on()) return vk_depth_shadow_prepare(surface, dynamic, call, fixed_mask, descriptor);
    vk_share_lock_at(36);
    int const r = vk_depth_shadow_prepare(surface, dynamic, call, fixed_mask, descriptor);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_depth_shadow_range_aliases(const Surf* depth, u64 gva, u64 bytes) {
    if (!vk_share_on()) return vk_depth_shadow_range_aliases(depth, gva, bytes);
    vk_share_lock_at(37);
    int const r = vk_depth_shadow_range_aliases(depth, gva, bytes);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_depth_shadow_reserve(Surf* surface, VkDescriptorBufferInfo* descriptor) {
    if (!vk_share_on()) return vk_depth_shadow_reserve(surface, descriptor);
    vk_share_lock_at(38);
    int const r = vk_depth_shadow_reserve(surface, descriptor);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_descriptor_heaps_get_stats(VkDescriptorHeapStats* stats) {
    if (!vk_share_on()) {
        vk_descriptor_heaps_get_stats(stats);
        return;
    }
    vk_share_lock_at(39);
    vk_descriptor_heaps_get_stats(stats);
    vk_share_unlock();
}
static inline void sh_vk_descriptor_heaps_invalidate(void) {
    if (!vk_share_on()) {
        vk_descriptor_heaps_invalidate();
        return;
    }
    vk_share_lock_at(40);
    vk_descriptor_heaps_invalidate();
    vk_share_unlock();
}
static inline void sh_vk_descriptor_heaps_pipeline_state(int active) {
    if (!vk_share_on()) {
        vk_descriptor_heaps_pipeline_state(active);
        return;
    }
    vk_share_lock_at(41);
    vk_descriptor_heaps_pipeline_state(active);
    vk_share_unlock();
}
static inline void sh_vk_descriptors_bind(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count, const VkDescriptorSet* sets, u32 ndyn, const u32* dyn) {
    if (!vk_share_on()) {
        vk_descriptors_bind(cb, layout, first, count, sets, ndyn, dyn);
        return;
    }
    vk_share_lock_at(42);
    vk_descriptors_bind(cb, layout, first, count, sets, ndyn, dyn);
    vk_share_unlock();
}
static inline void sh_vk_descriptors_bind_compute(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, VkDescriptorSet set) {
    if (!vk_share_on()) {
        vk_descriptors_bind_compute(cb, layout, first, set);
        return;
    }
    vk_share_lock_at(43);
    vk_descriptors_bind_compute(cb, layout, first, set);
    vk_share_unlock();
}
static inline VkDescriptorSet sh_vk_descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes, u32 cb_addr) {
    if (!vk_share_on()) return vk_descriptors_write(layout, count, writes, cb_addr);
    vk_share_lock_at(44);
    VkDescriptorSet const r = vk_descriptors_write(layout, count, writes, cb_addr);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_draw_texture_fragment(u32 texture_int, u32 texture_signed, u32 target_int, u32 target_signed, u64 advanced_blend, u32 advanced_mask) {
    if (!vk_share_on()) return vk_draw_texture_fragment(texture_int, texture_signed, target_int, target_signed, advanced_blend, advanced_mask);
    vk_share_lock_at(45);
    VkSh* const r = vk_draw_texture_fragment(texture_int, texture_signed, target_int, target_signed, advanced_blend, advanced_mask);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_draw_texture_vertex(void) {
    if (!vk_share_on()) return vk_draw_texture_vertex();
    vk_share_lock_at(46);
    VkSh* const r = vk_draw_texture_vertex();
    vk_share_unlock();
    return r;
}
static inline int sh_vk_format_minmax_filter(VkFormat f) {
    if (!vk_share_on()) return vk_format_minmax_filter(f);
    vk_share_lock_at(47);
    int const r = vk_format_minmax_filter(f);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_frame_check(void) {
    if (!vk_share_on()) {
        vk_frame_check();
        return;
    }
    vk_share_lock_at(48);
    vk_frame_check();
    vk_share_unlock();
}
static inline u64 sh_vk_gfx_image_after(Surf* s, u32 size) {
    if (!vk_share_on()) return vk_gfx_image_after(s, size);
    vk_share_lock_at(49);
    u64 const r = vk_gfx_image_after(s, size);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_gfx_image_pending(u64 gva, u64 size) {
    if (!vk_share_on()) return vk_gfx_image_pending(gva, size);
    vk_share_lock_at(50);
    int const r = vk_gfx_image_pending(gva, size);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_gfx_image_pending_besides(u64 gva, u64 size, const Surf* self) {
    if (!vk_share_on()) return vk_gfx_image_pending_besides(gva, size, self);
    vk_share_lock_at(51);
    int const r = vk_gfx_image_pending_besides(gva, size, self);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_gfx_image_record(Surf* s, u8* original, u32 size, u64 after, const VkImageKeep* keep) {
    if (!vk_share_on()) {
        vk_gfx_image_record(s, original, size, after, keep);
        return;
    }
    vk_share_lock_at(52);
    vk_gfx_image_record(s, original, size, after, keep);
    vk_share_unlock();
}
static inline int sh_vk_gfx_image_unrecorded(const Surf* s) {
    if (!vk_share_on()) return vk_gfx_image_unrecorded(s);
    vk_share_lock_at(53);
    int const r = vk_gfx_image_unrecorded(s);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_gpuprof_op_begin(VkCommandBuffer cb, u32 kind, u64 key, u64 bytes) {
    if (!vk_share_on()) return vk_gpuprof_op_begin(cb, kind, key, bytes);
    vk_share_lock_at(54);
    int const r = vk_gpuprof_op_begin(cb, kind, key, bytes);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_gpuprof_op_end(VkCommandBuffer cb) {
    if (!vk_share_on()) {
        vk_gpuprof_op_end(cb);
        return;
    }
    vk_share_lock_at(55);
    vk_gpuprof_op_end(cb);
    vk_share_unlock();
}
static inline void sh_vk_gpuprof_pass_begin(VkCommandBuffer cb, const Surf* rt, const Surf* zt) {
    if (!vk_share_on()) {
        vk_gpuprof_pass_begin(cb, rt, zt);
        return;
    }
    vk_share_lock_at(56);
    vk_gpuprof_pass_begin(cb, rt, zt);
    vk_share_unlock();
}
static inline void sh_vk_gpuprof_pass_end(VkCommandBuffer cb) {
    if (!vk_share_on()) {
        vk_gpuprof_pass_end(cb);
        return;
    }
    vk_share_lock_at(57);
    vk_gpuprof_pass_end(cb);
    vk_share_unlock();
}
static inline VkSh* sh_vk_host_shader(u64 code, int stage) {
    if (!vk_share_on()) return vk_host_shader(code, stage);
    vk_share_lock_at(58);
    VkSh* const r = vk_host_shader(code, stage);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_indirect_quads(VkBuffer record, u64 record_at, u32 stride, u32 records, u32 size, VkBuffer indices, u64 indices_at, u32 limit, int last, VkBuffer* arguments, u64* arguments_at, u64* made_at) {
    if (!vk_share_on()) return vk_indirect_quads(record, record_at, stride, records, size, indices, indices_at, limit, last, arguments, arguments_at, made_at);
    vk_share_lock_at(59);
    int const r = vk_indirect_quads(record, record_at, stride, records, size, indices, indices_at, limit, last, arguments, arguments_at, made_at);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_mem_track(DevMem a, u32 owner) {
    if (!vk_share_on()) {
        vk_mem_track(a, owner);
        return;
    }
    vk_share_lock_at(60);
    vk_mem_track(a, owner);
    vk_share_unlock();
}
static inline int sh_vk_mirror_holds(u64 gva, u64 len) {
    if (!vk_share_on()) return vk_mirror_holds(gva, len);
    vk_share_lock_at(61);
    int const r = vk_mirror_holds(gva, len);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_mirror_pending(u64 gva, u64 len) {
    if (!vk_share_on()) return vk_mirror_pending(gva, len);
    vk_share_lock_at(62);
    int const r = vk_mirror_pending(gva, len);
    vk_share_unlock();
    return r;
}
static inline VkShaderModule sh_vk_module_spv(const u32* words, size_t count) {
    if (!vk_share_on()) return vk_module_spv(words, count);
    vk_share_lock_at(63);
    VkShaderModule const r = vk_module_spv(words, count);
    vk_share_unlock();
    return r;
}
static inline VkShaderModule sh_vk_module_spv_location(const u32* words, size_t count, u32 location) {
    if (!vk_share_on()) return vk_module_spv_location(words, count, location);
    vk_share_lock_at(64);
    VkShaderModule const r = vk_module_spv_location(words, count, location);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_null_buffer_image(int kind, VkBufImg* out) {
    if (!vk_share_on()) {
        vk_null_buffer_image(kind, out);
        return;
    }
    vk_share_lock_at(65);
    vk_null_buffer_image(kind, out);
    vk_share_unlock();
}
static inline int sh_vk_null_header_first(u64 tic_at) {
    if (!vk_share_on()) return vk_null_header_first(tic_at);
    vk_share_lock_at(66);
    int const r = vk_null_header_first(tic_at);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_null_storage(VkImageViewType type, int kind, VkImageView* view) {
    if (!vk_share_on()) return vk_null_storage(type, kind, view);
    vk_share_lock_at(67);
    Surf* const r = vk_null_storage(type, kind, view);
    vk_share_unlock();
    return r;
}
static inline VkImageView sh_vk_null_view(VkImageViewType type, int num, int ms) {
    if (!vk_share_on()) return vk_null_view(type, num, ms);
    vk_share_lock_at(68);
    VkImageView const r = vk_null_view(type, num, ms);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_own_res_unref(VkOwnRes* r) {
    if (!vk_share_on()) {
        vk_own_res_unref(r);
        return;
    }
    vk_share_lock_at(69);
    vk_own_res_unref(r);
    vk_share_unlock();
}
static inline void sh_vk_pages_read(u64 gva, u64 len) {
    if (!vk_share_on()) {
        vk_pages_read(gva, len);
        return;
    }
    vk_share_lock_at(70);
    vk_pages_read(gva, len);
    vk_share_unlock();
}
static inline void sh_vk_pipe_unwritten_targets(PipeKey* k) {
    if (!vk_share_on()) {
        vk_pipe_unwritten_targets(k);
        return;
    }
    vk_share_lock_at(71);
    vk_pipe_unwritten_targets(k);
    vk_share_unlock();
}
static inline VkPipeline sh_vk_pipeline_get(const PipeKey* k) {
    if (!vk_share_on()) return vk_pipeline_get(k);
    vk_share_lock_at(72);
    VkPipeline const r = vk_pipeline_get(k);
    vk_share_unlock();
    return r;
}
static inline VkPipeline sh_vk_pipeline_get_deferred(const PipeKey* k) {
    if (!vk_share_on()) return vk_pipeline_get_deferred(k);
    vk_share_lock_at(73);
    VkPipeline const r = vk_pipeline_get_deferred(k);
    vk_share_unlock();
    return r;
}
static inline VkPipelineLayout sh_vk_pipeline_layout(VkPipeline h, u32 arrays) {
    if (!vk_share_on()) return vk_pipeline_layout(h, arrays);
    vk_share_lock_at(74);
    VkPipelineLayout const r = vk_pipeline_layout(h, arrays);
    vk_share_unlock();
    return r;
}
static inline VkPipeline sh_vk_pipeline_now(VkPipeline h, int* pending) {
    if (!vk_share_on()) return vk_pipeline_now(h, pending);
    vk_share_lock_at(75);
    VkPipeline const r = vk_pipeline_now(h, pending);
    vk_share_unlock();
    return r;
}
static inline VkPipeline sh_vk_pipeline_ready(VkPipeline h) {
    if (!vk_share_on()) return vk_pipeline_ready(h);
    vk_share_lock_at(76);
    VkPipeline const r = vk_pipeline_ready(h);
    vk_share_unlock();
    return r;
}
static inline VkPipelineLayout sh_vk_playout_compute(u32 arrays) {
    if (!vk_share_on()) return vk_playout_compute(arrays);
    vk_share_lock_at(77);
    VkPipelineLayout const r = vk_playout_compute(arrays);
    vk_share_unlock();
    return r;
}
static inline u32 sh_vk_pointer_windows(u64* v, u32 n, u32 windows, u32 reach, int whole, u64* base, u32* len) {
    if (!vk_share_on()) return vk_pointer_windows(v, n, windows, reach, whole, base, len);
    vk_share_lock_at(78);
    u32 const r = vk_pointer_windows(v, n, windows, reach, whole, base, len);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_predraw_drawn(void) {
    if (!vk_share_on()) {
        vk_predraw_drawn();
        return;
    }
    vk_share_lock_at(79);
    vk_predraw_drawn();
    vk_share_unlock();
}
static inline VkSh* sh_vk_predraw_fragment(u32 count, u32 slots, u32 ints, u32 sints) {
    if (!vk_share_on()) return vk_predraw_fragment(count, slots, ints, sints);
    vk_share_lock_at(80);
    VkSh* const r = vk_predraw_fragment(count, slots, ints, sints);
    vk_share_unlock();
    return r;
}
static inline u32 sh_vk_predraw_next(const Surf* const* targets, const u32* views, u32 n) {
    if (!vk_share_on()) return vk_predraw_next(targets, views, n);
    vk_share_lock_at(81);
    u32 const r = vk_predraw_next(targets, views, n);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_predraw_pass_begin(VkCommandBuffer cb) {
    if (!vk_share_on()) {
        vk_predraw_pass_begin(cb);
        return;
    }
    vk_share_lock_at(82);
    vk_predraw_pass_begin(cb);
    vk_share_unlock();
}
static inline int sh_vk_predraw_reserve(u32 sw, u32 sh, u32 sides) {
    if (!vk_share_on()) return vk_predraw_reserve(sw, sh, sides);
    vk_share_lock_at(83);
    int const r = vk_predraw_reserve(sw, sh, sides);
    vk_share_unlock();
    return r;
}
static inline VkImageView sh_vk_predraw_view(u32 k) {
    if (!vk_share_on()) return vk_predraw_view(k);
    vk_share_lock_at(84);
    VkImageView const r = vk_predraw_view(k);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_query_pass_end(VkCommandBuffer cb) {
    if (!vk_share_on()) {
        vk_query_pass_end(cb);
        return;
    }
    vk_share_lock_at(85);
    vk_query_pass_end(cb);
    vk_share_unlock();
}
static inline VkDescriptorSet sh_vk_rec_new_set(VkDescriptorSetLayout layout, u32 n, const VkWriteDescriptorSet* w) {
    if (!vk_share_on()) return vk_rec_new_set(layout, n, w);
    vk_share_lock_at(86);
    VkDescriptorSet const r = vk_rec_new_set(layout, n, w);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_rt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 pitch, u32 layers, u64 layer_size, int volume, u32 msaa) {
    if (!vk_share_on()) return vk_rt_array(gva, w, h, fmt, tile, pitch, layers, layer_size, volume, msaa);
    vk_share_lock_at(87);
    Surf* const r = vk_rt_array(gva, w, h, fmt, tile, pitch, layers, layer_size, volume, msaa);
    vk_share_unlock();
    return r;
}
static inline VkImageView sh_vk_rt_attachment(Surf* s, int srgb_write) {
    if (!vk_share_on()) return vk_rt_attachment(s, srgb_write);
    vk_share_lock_at(88);
    VkImageView const r = vk_rt_attachment(s, srgb_write);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_rt_copy_drawn(Surf* rt) {
    if (!vk_share_on()) {
        vk_rt_copy_drawn(rt);
        return;
    }
    vk_share_lock_at(89);
    vk_rt_copy_drawn(rt);
    vk_share_unlock();
}
static inline void sh_vk_rt_drawn(Surf* s) {
    if (!vk_share_on()) {
        vk_rt_drawn(s);
        return;
    }
    vk_share_lock_at(90);
    vk_rt_drawn(s);
    vk_share_unlock();
}
static inline void sh_vk_rt_drawn_full(Surf* s) {
    if (!vk_share_on()) {
        vk_rt_drawn_full(s);
        return;
    }
    vk_share_lock_at(91);
    vk_rt_drawn_full(s);
    vk_share_unlock();
}
static inline Surf* sh_vk_rt_find(u64 gva) {
    if (!vk_share_on()) return vk_rt_find(gva);
    vk_share_lock_at(92);
    Surf* const r = vk_rt_find(gva);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_rt_note_read(Surf* s) {
    if (!vk_share_on()) {
        vk_rt_note_read(s);
        return;
    }
    vk_share_lock_at(93);
    vk_rt_note_read(s);
    vk_share_unlock();
}
static inline Surf* sh_vk_rt_save(Surf* rt, u32 set) {
    if (!vk_share_on()) return vk_rt_save(rt, set);
    vk_share_lock_at(94);
    Surf* const r = vk_rt_save(rt, set);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_rt_snapshot(Surf* rt, int pixels) {
    if (!vk_share_on()) return vk_rt_snapshot(rt, pixels);
    vk_share_lock_at(95);
    Surf* const r = vk_rt_snapshot(rt, pixels);
    vk_share_unlock();
    return r;
}
static inline VkSampler sh_vk_sampler(const u32* tsc, int raise, int integer) {
    if (!vk_share_on()) return vk_sampler(tsc, raise, integer);
    vk_share_lock_at(96);
    VkSampler const r = vk_sampler(tsc, raise, integer);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_shader_cb_hold(const DsState* ds, const VkSh* s) {
    if (!vk_share_on()) return vk_shader_cb_hold(ds, s);
    vk_share_lock_at(97);
    int const r = vk_shader_cb_hold(ds, s);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_shader_chosen(const DsState* ds, const VkSh* s) {
    if (!vk_share_on()) return vk_shader_chosen(ds, s);
    vk_share_lock_at(98);
    int const r = vk_shader_chosen(ds, s);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_shader_fold_top(const VkSh* s) {
    if (!vk_share_on()) return vk_shader_fold_top(s);
    vk_share_lock_at(99);
    int const r = vk_shader_fold_top(s);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_shader_get(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int, u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb, const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint) {
    if (!vk_share_on()) return vk_shader_get(ds, gva, gva_a, stage, int_mask, sint_mask, rt_int, rt_sint, clip, dc_color, dc_funcs, unfed, rect, xfb, query_hint, advanced_blend, advanced_mask, img_int, img_sint);
    vk_share_lock_at(100);
    VkSh* const r = vk_shader_get(ds, gva, gva_a, stage, int_mask, sint_mask, rt_int, rt_sint, clip, dc_color, dc_funcs, unfed, rect, xfb, query_hint, advanced_blend, advanced_mask, img_int, img_sint);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_shader_get_ex(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int, u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb, const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint, u32 fs_config, u32 minmax, u64 predraw, int early) {
    if (!vk_share_on()) return vk_shader_get_ex(ds, gva, gva_a, stage, int_mask, sint_mask, rt_int, rt_sint, clip, dc_color, dc_funcs, unfed, rect, xfb, query_hint, advanced_blend, advanced_mask, img_int, img_sint, fs_config, minmax, predraw, early);
    vk_share_lock_at(101);
    VkSh* const r = vk_shader_get_ex(ds, gva, gva_a, stage, int_mask, sint_mask, rt_int, rt_sint, clip, dc_color, dc_funcs, unfed, rect, xfb, query_hint, advanced_blend, advanced_mask, img_int, img_sint, fs_config, minmax, predraw, early);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_shader_mesh(const DsState* ds, u64 gva, int stage, const DsMeshKey* k, u32 payload_stride, int task_compute) {
    if (!vk_share_on()) return vk_shader_mesh(ds, gva, stage, k, payload_stride, task_compute);
    vk_share_lock_at(102);
    VkSh* const r = vk_shader_mesh(ds, gva, stage, k, payload_stride, task_compute);
    vk_share_unlock();
    return r;
}
static inline u8* sh_vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_stage(size, align, buf, off);
    vk_share_lock_at(103);
    u8* const r = vk_stage(size, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline u8* sh_vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, DevMem* mem) {
    if (!vk_share_on()) return vk_stage_output(size, align, buf, off, mem);
    vk_share_lock_at(104);
    u8* const r = vk_stage_output(size, align, buf, off, mem);
    vk_share_unlock();
    return r;
}
static inline u8* sh_vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off) {
    if (!vk_share_on()) return vk_stage_upload(size, align, buf, off);
    vk_share_lock_at(105);
    u8* const r = vk_stage_upload(size, align, buf, off);
    vk_share_unlock();
    return r;
}
static inline u64 sh_vk_submits(int why) {
    if (!vk_share_on()) return vk_submits(why);
    vk_share_lock_at(106);
    u64 const r = vk_submits(why);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_surf_batch_begin(void) {
    if (!vk_share_on()) {
        vk_surf_batch_begin();
        return;
    }
    vk_share_lock_at(107);
    vk_surf_batch_begin();
    vk_share_unlock();
}
static inline void sh_vk_surf_batch_end(void) {
    if (!vk_share_on()) {
        vk_surf_batch_end();
        return;
    }
    vk_share_lock_at(108);
    vk_surf_batch_end();
    vk_share_unlock();
}
static inline int sh_vk_surf_can_writeback(const Surf* s) {
    if (!vk_share_on()) return vk_surf_can_writeback(s);
    vk_share_lock_at(109);
    int const r = vk_surf_can_writeback(s);
    vk_share_unlock();
    return r;
}
static inline u32 sh_vk_surf_epoch_at(u64 gva) {
    if (!vk_share_on()) return vk_surf_epoch_at(gva);
    vk_share_lock_at(110);
    u32 const r = vk_surf_epoch_at(gva);
    vk_share_unlock();
    return r;
}
static inline u64 sh_vk_surf_guest_size(const Surf* s) {
    if (!vk_share_on()) return vk_surf_guest_size(s);
    vk_share_lock_at(111);
    u64 const r = vk_surf_guest_size(s);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_surf_keep_begin(Surf* s, VkImageKeep* k) {
    if (!vk_share_on()) return vk_surf_keep_begin(s, k);
    vk_share_lock_at(112);
    int const r = vk_surf_keep_begin(s, k);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_surf_view_reset(void) {
    if (!vk_share_on()) {
        vk_surf_view_reset();
        return;
    }
    vk_share_lock_at(113);
    vk_surf_view_reset();
    vk_share_unlock();
}
static inline void sh_vk_surf_wrote(Surf* s) {
    if (!vk_share_on()) {
        vk_surf_wrote(s);
        return;
    }
    vk_share_lock_at(114);
    vk_surf_wrote(s);
    vk_share_unlock();
}
static inline VkDescriptorSet sh_vk_table_set(void) {
    if (!vk_share_on()) return vk_table_set();
    vk_share_lock_at(115);
    VkDescriptorSet const r = vk_table_set();
    vk_share_unlock();
    return r;
}
static inline int sh_vk_table_words(const VkImageView* views, const VkImageLayout* layouts, const VkSampler* samplers, u32 n, u32* words) {
    if (!vk_share_on()) return vk_table_words(views, layouts, samplers, n, words);
    vk_share_lock_at(116);
    int const r = vk_table_words(views, layouts, samplers, n, words);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_tc_index(VkCommandBuffer cb, u32 mode, u32 remap, u32 restart, u32 n, VkBuffer in, u64 in_at, u64 in_size, VkBuffer out, u64 out_at, u64 out_size) {
    if (!vk_share_on()) return vk_tc_index(cb, mode, remap, restart, n, in, in_at, in_size, out, out_at, out_size);
    vk_share_lock_at(117);
    int const r = vk_tc_index(cb, mode, remap, restart, n, in, in_at, in_size, out, out_at, out_size);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_tc_restride(VkCommandBuffer cb, u32 in_stride, u32 out_stride, u32 nv, VkBuffer in, u64 in_at, u64 len, VkBuffer out, u64 out_at) {
    if (!vk_share_on()) return vk_tc_restride(cb, in_stride, out_stride, nv, in, in_at, len, out, out_at);
    vk_share_lock_at(118);
    int const r = vk_tc_restride(cb, in_stride, out_stride, nv, in, in_at, len, out, out_at);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_tess_passthrough(u32 vertices, const VkSh* vertex, const VkSh* evaluation) {
    if (!vk_share_on()) return vk_tess_passthrough(vertices, vertex, evaluation);
    vk_share_lock_at(119);
    VkSh* const r = vk_tess_passthrough(vertices, vertex, evaluation);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_tex_from_tic(const u32* tic) {
    if (!vk_share_on()) return vk_tex_from_tic(tic);
    vk_share_lock_at(120);
    Surf* const r = vk_tex_from_tic(tic);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_tex_from_tic_guest(const u32* tic, int raw) {
    if (!vk_share_on()) return vk_tex_from_tic_guest(tic, raw);
    vk_share_lock_at(121);
    Surf* const r = vk_tex_from_tic_guest(tic, raw);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_tex_from_tic_raw(const u32* tic) {
    if (!vk_share_on()) return vk_tex_from_tic_raw(tic);
    vk_share_lock_at(122);
    Surf* const r = vk_tex_from_tic_raw(tic);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_tex_from_tic_storage(const u32* tic) {
    if (!vk_share_on()) return vk_tex_from_tic_storage(tic);
    vk_share_lock_at(123);
    Surf* const r = vk_tex_from_tic_storage(tic);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_tex_is_copy_of(const Surf* t, const Surf* rt) {
    if (!vk_share_on()) return vk_tex_is_copy_of(t, rt);
    vk_share_lock_at(124);
    int const r = vk_tex_is_copy_of(t, rt);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_tex_recheck(Surf* t) {
    if (!vk_share_on()) return vk_tex_recheck(t);
    vk_share_lock_at(125);
    Surf* const r = vk_tex_recheck(t);
    vk_share_unlock();
    return r;
}
static inline VkImageView sh_vk_tex_storage_view(Surf* s, VkImageViewType type) {
    if (!vk_share_on()) return vk_tex_storage_view(s, type);
    vk_share_lock_at(126);
    VkImageView const r = vk_tex_storage_view(s, type);
    vk_share_unlock();
    return r;
}
static inline VkImageView sh_vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil) {
    if (!vk_share_on()) return vk_tex_view(s, type, srgb, map, stencil);
    vk_share_lock_at(127);
    VkImageView const r = vk_tex_view(s, type, srgb, map, stencil);
    vk_share_unlock();
    return r;
}
static inline VkFormat sh_vk_tex_view_format(const Surf* s, int srgb) {
    if (!vk_share_on()) return vk_tex_view_format(s, srgb);
    vk_share_lock_at(128);
    VkFormat const r = vk_tex_view_format(s, srgb);
    vk_share_unlock();
    return r;
}
static inline const char* sh_vk_tex_view_refusal(const Surf* s, VkImageViewType type) {
    if (!vk_share_on()) return vk_tex_view_refusal(s, type);
    vk_share_lock_at(129);
    const char* const r = vk_tex_view_refusal(s, type);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_tic_names_none(const u32* tic) {
    if (!vk_share_on()) return vk_tic_names_none(tic);
    vk_share_lock_at(130);
    int const r = vk_tic_names_none(tic);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_uma_pending(u64 gva, u64 len) {
    if (!vk_share_on()) return vk_uma_pending(gva, len);
    vk_share_lock_at(131);
    int const r = vk_uma_pending(gva, len);
    vk_share_unlock();
    return r;
}
static inline VkSh* sh_vk_varying_geometry(const VkSh* vertex, u32 per_vertex, u32 slot) {
    if (!vk_share_on()) return vk_varying_geometry(vertex, per_vertex, slot);
    vk_share_lock_at(132);
    VkSh* const r = vk_varying_geometry(vertex, per_vertex, slot);
    vk_share_unlock();
    return r;
}
static inline const char* sh_vk_view_type_name(VkImageViewType type) {
    if (!vk_share_on()) return vk_view_type_name(type);
    vk_share_lock_at(133);
    const char* const r = vk_view_type_name(type);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_vrs_draw(VkCommandBuffer cb, u32 id) {
    if (!vk_share_on()) {
        vk_vrs_draw(cb, id);
        return;
    }
    vk_share_lock_at(134);
    vk_vrs_draw(cb, id);
    vk_share_unlock();
}
static inline void sh_vk_vrs_keep(VkCommandBuffer cb) {
    if (!vk_share_on()) {
        vk_vrs_keep(cb);
        return;
    }
    vk_share_lock_at(135);
    vk_vrs_keep(cb);
    vk_share_unlock();
}
static inline int sh_vk_written_since(const u8* host, u64 len, u32 since) {
    if (!vk_share_on()) return vk_written_since(host, len, since);
    vk_share_lock_at(136);
    int const r = vk_written_since(host, len, since);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_xfb_control_buffer(VkBuffer counters, u64 counter_offset, u64 control, u32 what, u32 stride) {
    if (!vk_share_on()) return vk_xfb_control_buffer(counters, counter_offset, control, what, stride);
    vk_share_lock_at(137);
    int const r = vk_xfb_control_buffer(counters, counter_offset, control, what, stride);
    vk_share_unlock();
    return r;
}
static inline int sh_vk_xfb_indirect(u64 control, VkBuffer* arguments, u64* offset) {
    if (!vk_share_on()) return vk_xfb_indirect(control, arguments, offset);
    vk_share_lock_at(138);
    int const r = vk_xfb_indirect(control, arguments, offset);
    vk_share_unlock();
    return r;
}
static inline Surf* sh_vk_zt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 layers, u64 layer_size, u32 msaa) {
    if (!vk_share_on()) return vk_zt_array(gva, w, h, fmt, tile, layers, layer_size, msaa);
    vk_share_lock_at(139);
    Surf* const r = vk_zt_array(gva, w, h, fmt, tile, layers, layer_size, msaa);
    vk_share_unlock();
    return r;
}
static inline void sh_vk_shader_fold_draw(const DsState* ds, VkSh* s, u64 gva, const PipeKey* k) {
    if (!vk_share_on()) {
        vk_shader_fold_draw(ds, s, gva, k);
        return;
    }
    vk_share_lock_at(140);
    vk_shader_fold_draw(ds, s, gva, k);
    vk_share_unlock();
}
/* the wrapped functions by their number in vk_share_lock_at, for the stats line */
static const char* const sh_names[] = {"vk_all_barrier_by", "vk_astc_bcn", "vk_buf_copy", "vk_buf_copy_writable", "vk_buf_copy_writable_word", "vk_buf_draw_has_alias", "vk_buf_draw_prepare", "vk_buf_get_at", "vk_buf_get_input", "vk_buf_get_storage", "vk_buf_get_uniform_at", "vk_buf_get_window", "vk_buf_get_window_input", "vk_buf_mem_need", "vk_buf_own_got", "vk_buf_own_holds", "vk_buf_owned", "vk_buf_pending", "vk_buffer_address", "vk_buffer_create", "vk_buffer_image", "vk_capture_clear", "vk_capture_get", "vk_capture_get_mixed_targets", "vk_capture_mixed_resolve", "vk_capture_mixed_source", "vk_capture_resolve", "vk_compute_gpu_src", "vk_compute_mem_need", "vk_compute_pending", "vk_conservative_dilation", "vk_defer_buffer", "vk_defer_buffer_view", "vk_depth_shadow_aliases", "vk_depth_shadow_cancel", "vk_depth_shadow_finish", "vk_depth_shadow_prepare", "vk_depth_shadow_range_aliases", "vk_depth_shadow_reserve", "vk_descriptor_heaps_get_stats", "vk_descriptor_heaps_invalidate", "vk_descriptor_heaps_pipeline_state", "vk_descriptors_bind", "vk_descriptors_bind_compute", "vk_descriptors_write", "vk_draw_texture_fragment", "vk_draw_texture_vertex", "vk_format_minmax_filter", "vk_frame_check", "vk_gfx_image_after", "vk_gfx_image_pending", "vk_gfx_image_pending_besides", "vk_gfx_image_record", "vk_gfx_image_unrecorded", "vk_gpuprof_op_begin", "vk_gpuprof_op_end", "vk_gpuprof_pass_begin", "vk_gpuprof_pass_end", "vk_host_shader", "vk_indirect_quads", "vk_mem_track", "vk_mirror_holds", "vk_mirror_pending", "vk_module_spv", "vk_module_spv_location", "vk_null_buffer_image", "vk_null_header_first", "vk_null_storage", "vk_null_view", "vk_own_res_unref", "vk_pages_read", "vk_pipe_unwritten_targets", "vk_pipeline_get", "vk_pipeline_get_deferred", "vk_pipeline_layout", "vk_pipeline_now", "vk_pipeline_ready", "vk_playout_compute", "vk_pointer_windows", "vk_predraw_drawn", "vk_predraw_fragment", "vk_predraw_next", "vk_predraw_pass_begin", "vk_predraw_reserve", "vk_predraw_view", "vk_query_pass_end", "vk_rec_new_set", "vk_rt_array", "vk_rt_attachment", "vk_rt_copy_drawn", "vk_rt_drawn", "vk_rt_drawn_full", "vk_rt_find", "vk_rt_note_read", "vk_rt_save", "vk_rt_snapshot", "vk_sampler", "vk_shader_cb_hold", "vk_shader_chosen", "vk_shader_fold_top", "vk_shader_get", "vk_shader_get_ex", "vk_shader_mesh", "vk_stage", "vk_stage_output", "vk_stage_upload", "vk_submits", "vk_surf_batch_begin", "vk_surf_batch_end", "vk_surf_can_writeback", "vk_surf_epoch_at", "vk_surf_guest_size", "vk_surf_keep_begin", "vk_surf_view_reset", "vk_surf_wrote", "vk_table_set", "vk_table_words", "vk_tc_index", "vk_tc_restride", "vk_tess_passthrough", "vk_tex_from_tic", "vk_tex_from_tic_guest", "vk_tex_from_tic_raw", "vk_tex_from_tic_storage", "vk_tex_is_copy_of", "vk_tex_recheck", "vk_tex_storage_view", "vk_tex_view", "vk_tex_view_format", "vk_tex_view_refusal", "vk_tic_names_none", "vk_uma_pending", "vk_varying_geometry", "vk_view_type_name", "vk_vrs_draw", "vk_vrs_keep", "vk_written_since", "vk_xfb_control_buffer", "vk_xfb_indirect", "vk_zt_array", "vk_shader_fold_draw"};
#define vk_all_barrier_by sh_vk_all_barrier_by
/* vk_astc_bcn (sh_names' 1) reads its argument alone: no share lock. It was the lock's most taken function, 7060
   takes a present in one game's race */
#define vk_buf_copy sh_vk_buf_copy
#define vk_buf_copy_writable sh_vk_buf_copy_writable
#define vk_buf_copy_writable_word sh_vk_buf_copy_writable_word
#define vk_buf_draw_has_alias sh_vk_buf_draw_has_alias
#define vk_buf_draw_prepare sh_vk_buf_draw_prepare
#define vk_buf_get_at sh_vk_buf_get_at
#define vk_buf_get_input sh_vk_buf_get_input
#define vk_buf_get_storage sh_vk_buf_get_storage
#define vk_buf_get_uniform_at sh_vk_buf_get_uniform_at
#define vk_buf_get_window sh_vk_buf_get_window
#define vk_buf_get_window_input sh_vk_buf_get_window_input
#define vk_buf_mem_need sh_vk_buf_mem_need
#define vk_buf_own_got sh_vk_buf_own_got
#define vk_buf_own_holds sh_vk_buf_own_holds
#define vk_buf_owned sh_vk_buf_owned
#define vk_buf_pending sh_vk_buf_pending
#define vk_buffer_address sh_vk_buffer_address
#define vk_buffer_create sh_vk_buffer_create
#define vk_buffer_image sh_vk_buffer_image
#define vk_capture_clear sh_vk_capture_clear
#define vk_capture_get sh_vk_capture_get
#define vk_capture_get_mixed_targets sh_vk_capture_get_mixed_targets
#define vk_capture_mixed_resolve sh_vk_capture_mixed_resolve
#define vk_capture_mixed_source sh_vk_capture_mixed_source
#define vk_capture_resolve sh_vk_capture_resolve
#define vk_compute_gpu_src sh_vk_compute_gpu_src
#define vk_compute_mem_need sh_vk_compute_mem_need
#define vk_compute_pending sh_vk_compute_pending
#define vk_conservative_dilation sh_vk_conservative_dilation
#define vk_defer_buffer sh_vk_defer_buffer
#define vk_defer_buffer_view sh_vk_defer_buffer_view
#define vk_depth_shadow_aliases sh_vk_depth_shadow_aliases
#define vk_depth_shadow_cancel sh_vk_depth_shadow_cancel
#define vk_depth_shadow_finish sh_vk_depth_shadow_finish
#define vk_depth_shadow_prepare sh_vk_depth_shadow_prepare
#define vk_depth_shadow_range_aliases sh_vk_depth_shadow_range_aliases
#define vk_depth_shadow_reserve sh_vk_depth_shadow_reserve
#define vk_descriptor_heaps_get_stats sh_vk_descriptor_heaps_get_stats
#define vk_descriptor_heaps_invalidate sh_vk_descriptor_heaps_invalidate
#define vk_descriptor_heaps_pipeline_state sh_vk_descriptor_heaps_pipeline_state
#define vk_descriptors_bind sh_vk_descriptors_bind
#define vk_descriptors_bind_compute sh_vk_descriptors_bind_compute
#define vk_descriptors_write sh_vk_descriptors_write
#define vk_draw_texture_fragment sh_vk_draw_texture_fragment
#define vk_draw_texture_vertex sh_vk_draw_texture_vertex
#define vk_format_minmax_filter sh_vk_format_minmax_filter
#define vk_frame_check sh_vk_frame_check
#define vk_gfx_image_after sh_vk_gfx_image_after
#define vk_gfx_image_pending sh_vk_gfx_image_pending
#define vk_gfx_image_pending_besides sh_vk_gfx_image_pending_besides
#define vk_gfx_image_record sh_vk_gfx_image_record
#define vk_gfx_image_unrecorded sh_vk_gfx_image_unrecorded
#define vk_gpuprof_op_begin sh_vk_gpuprof_op_begin
#define vk_gpuprof_op_end sh_vk_gpuprof_op_end
#define vk_gpuprof_pass_begin sh_vk_gpuprof_pass_begin
#define vk_gpuprof_pass_end sh_vk_gpuprof_pass_end
#define vk_host_shader sh_vk_host_shader
#define vk_indirect_quads sh_vk_indirect_quads
#define vk_mem_track sh_vk_mem_track
#define vk_mirror_holds sh_vk_mirror_holds
#define vk_mirror_pending sh_vk_mirror_pending
#define vk_module_spv sh_vk_module_spv
#define vk_module_spv_location sh_vk_module_spv_location
#define vk_null_buffer_image sh_vk_null_buffer_image
#define vk_null_header_first sh_vk_null_header_first
#define vk_null_storage sh_vk_null_storage
#define vk_null_view sh_vk_null_view
#define vk_own_res_unref sh_vk_own_res_unref
#define vk_pages_read sh_vk_pages_read
#define vk_pipe_unwritten_targets sh_vk_pipe_unwritten_targets
#define vk_pipeline_get sh_vk_pipeline_get
#define vk_pipeline_get_deferred sh_vk_pipeline_get_deferred
#define vk_pipeline_layout sh_vk_pipeline_layout
#define vk_pipeline_now sh_vk_pipeline_now
#define vk_pipeline_ready sh_vk_pipeline_ready
#define vk_playout_compute sh_vk_playout_compute
#define vk_pointer_windows sh_vk_pointer_windows
#define vk_predraw_drawn sh_vk_predraw_drawn
#define vk_predraw_fragment sh_vk_predraw_fragment
#define vk_predraw_next sh_vk_predraw_next
#define vk_predraw_pass_begin sh_vk_predraw_pass_begin
#define vk_predraw_reserve sh_vk_predraw_reserve
#define vk_predraw_view sh_vk_predraw_view
#define vk_query_pass_end sh_vk_query_pass_end
#define vk_rec_new_set sh_vk_rec_new_set
#define vk_rt_array sh_vk_rt_array
#define vk_rt_attachment sh_vk_rt_attachment
#define vk_rt_copy_drawn sh_vk_rt_copy_drawn
#define vk_rt_drawn sh_vk_rt_drawn
#define vk_rt_drawn_full sh_vk_rt_drawn_full
#define vk_rt_find sh_vk_rt_find
#define vk_rt_note_read sh_vk_rt_note_read
#define vk_rt_save sh_vk_rt_save
#define vk_rt_snapshot sh_vk_rt_snapshot
#define vk_sampler sh_vk_sampler
#define vk_shader_cb_hold sh_vk_shader_cb_hold
#define vk_shader_chosen sh_vk_shader_chosen
#define vk_shader_fold_top sh_vk_shader_fold_top
#define vk_shader_get sh_vk_shader_get
#define vk_shader_get_ex sh_vk_shader_get_ex
#define vk_shader_mesh sh_vk_shader_mesh
#define vk_stage sh_vk_stage
#define vk_stage_output sh_vk_stage_output
#define vk_stage_upload sh_vk_stage_upload
#define vk_submits sh_vk_submits
#define vk_surf_batch_begin sh_vk_surf_batch_begin
#define vk_surf_batch_end sh_vk_surf_batch_end
#define vk_surf_can_writeback sh_vk_surf_can_writeback
#define vk_surf_epoch_at sh_vk_surf_epoch_at
#define vk_surf_guest_size sh_vk_surf_guest_size
#define vk_surf_keep_begin sh_vk_surf_keep_begin
#define vk_surf_view_reset sh_vk_surf_view_reset
#define vk_surf_wrote sh_vk_surf_wrote
#define vk_table_set sh_vk_table_set
#define vk_table_words sh_vk_table_words
#define vk_tc_index sh_vk_tc_index
#define vk_tc_restride sh_vk_tc_restride
#define vk_tess_passthrough sh_vk_tess_passthrough
#define vk_tex_from_tic sh_vk_tex_from_tic
#define vk_tex_from_tic_guest sh_vk_tex_from_tic_guest
#define vk_tex_from_tic_raw sh_vk_tex_from_tic_raw
#define vk_tex_from_tic_storage sh_vk_tex_from_tic_storage
#define vk_tex_is_copy_of sh_vk_tex_is_copy_of
#define vk_tex_recheck sh_vk_tex_recheck
#define vk_tex_storage_view sh_vk_tex_storage_view
#define vk_tex_view sh_vk_tex_view
#define vk_tex_view_format sh_vk_tex_view_format
#define vk_tex_view_refusal sh_vk_tex_view_refusal
#define vk_tic_names_none sh_vk_tic_names_none
#define vk_uma_pending sh_vk_uma_pending
#define vk_varying_geometry sh_vk_varying_geometry
#define vk_view_type_name sh_vk_view_type_name
#define vk_vrs_draw sh_vk_vrs_draw
#define vk_vrs_keep sh_vk_vrs_keep
#define vk_written_since sh_vk_written_since
#define vk_xfb_control_buffer sh_vk_xfb_control_buffer
#define vk_xfb_indirect sh_vk_xfb_indirect
#define vk_zt_array sh_vk_zt_array
#define vk_shader_fold_draw sh_vk_shader_fold_draw
#endif
