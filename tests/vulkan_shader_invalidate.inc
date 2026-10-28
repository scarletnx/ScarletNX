/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */

static u64 invalidate_mov(u32 reg, u32 value)
{
	return 0x0100000000071000ull | ((u64)value << 20) | reg;
}

static int invalidate_write_code(u64 address, const u64 *code, size_t count)
{
	u8 bytes[512];
	size_t i;

	if (count > sizeof(bytes) / sizeof(u64))
		return 0;
	for (i = 0; i < count; i++)
		snx_st_le64(bytes + i * sizeof(u64), code[i]);
	return !gmem_write(address, bytes, count * sizeof(u64));
}

static VkSh *invalidate_fragment(GpuChannel *channel, u64 address)
{
	VkSh *shader;

	vk_lock();
	channel->r3d_dirty = ~0u; /* the registers were set directly */
	mx_draw_state_sync(channel); /* the state as the draw reads it */
	shader = vk_shader_get(&channel->ds, address, 0, ST_FS, 0, 0, 0, 0,
			       0, 0, 0, 0, NULL);
	vk_unlock();
	return shader;
}

/* a launch as the compute engine hands it to the renderer */
static void invalidate_dispatch(GpuChannel *channel, const u32 *qmd, u64 code)
{
	DispatchState d;

	mx_dispatch_state(qmd, channel->rcomp, code, &d);
	vk_dispatch(&d);
}

static void invalidate_draw(GpuChannel *channel)
{
	DrawCall call = {4, 0, 0, 3, 0, 0, 1}; /* a triangle list of 3 vertices */

	vk_lock();
	channel->r3d_dirty = ~0u; /* the registers were set directly */
	mx_draw_state_sync(channel);
	vk_render_draw(&channel->ds, &call, &channel->owner);
	vk_unlock();
}

/* The draw cache must not bypass the refreshed shader address lookup. */
static int check_kept_shader_invalidation(GpuChannel *channel, u64 address,
					u64 *fragment)
{
	u64 vertex[64] = {0};
	u32 *r = channel->r3d;
	const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
	u64 drawn, kept;
	u32 epoch;

	vertex[10] = 0x001f8000fc0007e0ull;
	vertex[11] = invalidate_mov(0, 0);
	vertex[12] = 0xeff0000000070000ull | 0x70ull << 20 | 255ull << 8;
	vertex[13] = 0xe30000000007000full;
	CHECK(invalidate_write_code(address + 0x10000, vertex, 64));
	channel->id = 127;
	r[0x582] = (u32)(address >> 32);
	r[0x583] = (u32)address;
	r[0x811] = 0x10000;
	r[0x850] = 1;
	r[0x851] = 0;
	r[0x64b] = 1;
	r[0x286] = 0x6420;
	r[0x3fd] = 4u << 16;
	r[0x3fe] = 4u << 16;
	r[0x680] = 0x1111;
	r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
	memcpy(r + 0x280, transform, sizeof(transform));
	channel->r3d_dirty = ~0u;
	vk_lock();
	rt_clear(channel, address + 0x30000, 4, 4, 0, 0);
	vk_unlock();

	drawn = g_vk_drawn;
	invalidate_draw(channel);
	CHECK(g_vk_drawn == drawn + 1);
	kept = g_vk_draw_kept;
	invalidate_draw(channel);
	CHECK(g_vk_drawn == drawn + 2 && g_vk_draw_kept == kept + 1);
	kept = g_vk_draw_kept;
	epoch = g_vk_epoch;

	fragment[11] = invalidate_mov(0, 0x3e800000);
	CHECK(invalidate_write_code(address, fragment, 64));
	maxwell3d_method(channel, 0x369, 1, 1);
	CHECK(g_vk_epoch == epoch);
	invalidate_draw(channel);
	CHECK(g_vk_drawn == drawn + 3 && g_vk_draw_kept == kept);
	invalidate_draw(channel);
	CHECK(g_vk_drawn == drawn + 4 && g_vk_draw_kept == kept + 1);
	puts("shader invalidation: held draw state expires and reuse resumes");
	return 0;
}

/* Replacing instructions during a submission must refresh the address lookup,
 * while an unchanged program still shares its translated module.
 */
static int check_shader_invalidation(void)
{
	const u64 address = 0x400000000ull;
	u64 code[64] = {0};
	u32 qmd[64] = {0};
	GpuChannel *channel = calloc(1, sizeof(*channel));
	VkSh *first, *second;
	u64 generation, finishes, dispatches;
	u32 epoch;

	CHECK(channel);
	gmmu_map(address, HEAP_BASE + 0x780000, 0x40000);
	code[9] = 15;
	code[10] = 0x001f8000fc0007e0ull;
	code[11] = invalidate_mov(0, 0x3f000000);
	code[12] = invalidate_mov(1, 0);
	code[13] = invalidate_mov(2, 0);
	code[14] = 0x001f8000fc0007e0ull;
	code[15] = invalidate_mov(3, 0x3f800000);
	code[16] = 0xe30000000007000full;
	CHECK(invalidate_write_code(address, code, 64));
	first = invalidate_fragment(channel, address);
	CHECK(first && !first->failed && first->mod);
	epoch = g_vk_epoch;
	generation = g_vk.shader_generation;
	finishes = g_vk_finishes;

	code[11] = invalidate_mov(0, 0x3f800000);
	CHECK(invalidate_write_code(address, code, 64));
	maxwell3d_method(channel, 0x369, 0, 1);
	maxwell3d_method(channel, 0x369, 0x1010, 1);
	CHECK(g_vk.shader_generation == generation);
	maxwell3d_method(channel, 0x369, 1, 1);
	CHECK(g_vk.shader_generation == generation + 1);
	CHECK(g_vk_epoch == epoch && g_vk_finishes == finishes);
	second = invalidate_fragment(channel, address);
	CHECK(second && !second->failed && second->mod);
	CHECK(second != first && second->mod != first->mod);
	CHECK(second->qh != first->qh);
	maxwell3d_method(channel, 0x369, 1, 1);
	CHECK(invalidate_fragment(channel, address) == second);
	CHECK(g_vk_epoch == epoch && g_vk_finishes == finishes);
	maxwell3d_method(channel, 0x87, 1, 1);
	CHECK(g_vk.shader_generation == generation + 3);
	CHECK(invalidate_fragment(channel, address) == second);
	CHECK(!check_kept_shader_invalidation(channel, address, code));

	/* A valid compute program first populates the address lookup. KIL is
	 * fragment-only, so replacing it must reject the next compute dispatch.
	 */
	memset(code, 0, sizeof(code));
	code[0] = 0x001f8000fc0007e0ull;
	code[1] = invalidate_mov(0, 42);
	code[2] = 0xe30000000007000full;
	CHECK(invalidate_write_code(address + 0x20000, code, 64));
	qmd[12] = 1;
	qmd[13] = 1 | 1u << 16;
	qmd[18] = 1u << 16;
	qmd[19] = 1 | 1u << 16;
	dispatches = g_vk_dispatches;
	vk_lock();
	invalidate_dispatch(channel, qmd, address + 0x20000);
	vk_unlock();
	CHECK(g_vk_dispatches == dispatches + 1);
	epoch = g_vk_epoch;
	generation = g_vk.shader_generation;
	finishes = g_vk_finishes;
	code[1] = 0xe33000000007000full;
	CHECK(invalidate_write_code(address + 0x20000, code, 64));
	compute_method(channel, 0x87, 0, 1);
	compute_method(channel, 0x87, 0x10, 1);
	CHECK(g_vk.shader_generation == generation);
	compute_method(channel, 0x87, 1, 1);
	CHECK(g_vk.shader_generation == generation + 1);
	CHECK(g_vk_epoch == epoch && g_vk_finishes == finishes);
	vk_lock();
	invalidate_dispatch(channel, qmd, address + 0x20000);
	vk_unlock();
	CHECK(g_vk_dispatches == dispatches + 1);
	CHECK(g_vk_epoch == epoch && g_vk_finishes == finishes);

	code[1] = invalidate_mov(0, 42);
	CHECK(invalidate_write_code(address + 0x20000, code, 64));
	compute_method(channel, 0x5a6, 1, 1);
	vk_lock();
	invalidate_dispatch(channel, qmd, address + 0x20000);
	vk_compute_land();
	vk_unlock();
	CHECK(g_vk_dispatches == dispatches + 2);
	/* the same launch with its grid in guest memory, which the GPU reads (DispatchState.indirect): on the graphics
	   queue, its grid copied into bank 0's dispatch size there */
	{
		DispatchState d;
		u8 grid[12];

		snx_st_le32(grid, 1);
		snx_st_le32(grid + 4, 1);
		snx_st_le32(grid + 8, 1);
		CHECK(!gmem_write(address + 0x1f000, grid, sizeof grid));
		mx_dispatch_state(qmd, channel->rcomp, address + 0x20000, &d);
		d.indirect = address + 0x1f000;
		vk_lock();
		vk_dispatch(&d);
		vk_finish();
		vk_unlock();
		CHECK(g_vk_dispatches == dispatches + 3);
	}
	puts("shader invalidation: graphics and compute replacement, module reuse and an indirect dispatch checked");
	free(channel);
	return 0;
}
