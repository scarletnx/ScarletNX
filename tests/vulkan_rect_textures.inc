/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */

/* A rectangle texture (TIC word 5 bit 31 clear, as NVN writes its RECTANGLE target) takes texel coordinates: a
 * compute program's TEX.LZ at (3.5, 0.5) reads texel (3, 0) of a 4x4 R32F texture, which it stores. The same
 * texture with normalized coordinates reads that texel at (0.875, 0.125). Sampled as normalized, (3.5, 0.5) repeats
 * to the middle of the texture, texel (2, 2).
 */
static int check_rect_textures(void)
{
	static const struct {
		u32 x, y; /* the coordinates, as floats */
		int rect;
	} cases[] = {{0x40600000, 0x3f000000, 1}, {0x3f600000, 0x3e000000, 0}};
	const u64 address = 0x740000000ull;
	const u64 cbuf = address + 0x20000, descriptors = address + 0x21000, samplers = address + 0x22000;
	const u64 source = address + 0x30000, target = address + 0x32000;
	GpuChannel *channel = calloc(1, sizeof(*channel));
	u32 qmd[64] = {0}, count = 0;

	CHECK(channel);
	if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
		puts("rectangle textures: storage writes unavailable, skipped");
		free(channel);
		return 0;
	}
	gmmu_map(address, HEAP_BASE + 0x6c0000, 0x40000); /* the surface loads' region, which that check is done with */
	qmd[QMD_GRID_DIM_X] = 1;
	qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
	qmd[QMD_BLOCK_DIM_X] = 1u << 16;
	qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
	qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2;
	qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)cbuf;
	qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(cbuf >> 32) | 0x200u << 15;
	channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
	channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
	channel->rcomp[KEPLER_COMPUTE_TSC] = (u32)(samplers >> 32);
	channel->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)samplers;
	channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
	CHECK(!storage_write_words(samplers, (const u32[8]){0}, 8)); /* nearest, repeat */
	CHECK(!storage_write_words(cbuf + 0x128, (const u32[2]){0, 1}, 2));
	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		/* R32F, 4x4 texels, one level, block linear of one GOB */
		u32 word0 = 0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
		u32 tic[8] = {word0, (u32)source, (u32)(source >> 32) | 3u << 21, 0, 3u | 1u << 23,
			3u | (cases[c].rect ? 0u : 1u << 31), 0, 0};
		u32 output_tic[8];
		u64 sample = 0xc000000000000000ull | 1ull << 55 | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 |
			0xffull << 20 | 7ull << 16 | 8; /* TEX.LZ R8, R0, c[0x128], 2D, R */
		u64 store = (0xeb20000000070008ull | 3ull << 33 | 1ull << 20 | 1ull << 51) | 75ull << 36; /* SUST.P [R0], R8 */
		u64 instructions[] = {storage_mov(0, cases[c].x), storage_mov(1, cases[c].y), sample, storage_mov(0, 0),
			storage_mov(1, 0), store, 0xe30000000007000full};
		u8 code[256] = {0}, input[512], output[512], result[512];
		u64 dispatches = g_vk_dispatches;
		u32 at = 0;

		memcpy(output_tic, tic, sizeof(tic));
		output_tic[1] = (u32)target;
		for (u32 i = 0; i < sizeof(instructions) / sizeof(instructions[0]); i++) {
			if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
			snx_st_le64(code + 8 * at++, instructions[i]);
		}
		memset(input, 0, sizeof(input));
		for (u32 y = 0; y < 4; y++)
			for (u32 x = 0; x < 4; x++) {
				float value = 10.0f * (float)y + (float)x + 0.25f;
				u32 bits;
				memcpy(&bits, &value, 4);
				snx_st_le32(input + bl_offset(x * 4, y, 0, 16, 4, 0, 0), bits);
			}
		memset(output, 0, sizeof(output));
		CHECK(!gmem_write(address + 0x1000 + c * 0x200, code, sizeof(code)));
		CHECK(!gmem_write(source, input, sizeof(input)));
		CHECK(!gmem_write(target, output, sizeof(output)));
		CHECK(!storage_write_words(descriptors, tic, 8));
		CHECK(!storage_write_words(descriptors + 32, output_tic, 8));
		vk_lock();
		{
			DispatchState d;
			mx_dispatch_state(qmd, channel->rcomp, address + 0x1000 + c * 0x200, &d);
			vk_dispatch(&d);
		}
		vk_unlock();
		CHECK(g_vk_dispatches == dispatches + 1);
		CHECK(!gmem_read(target, result, sizeof(result)));
		float got;
		u32 bits = snx_ld_le32(result);
		memcpy(&got, &bits, 4);
		if (got != 3.25f)
			fprintf(stderr, "rectangle texture case %u (rect %d): read %g at (%08x, %08x), want 3.25 (texel 3, 0)\n",
				c, cases[c].rect, (double)got, cases[c].x, cases[c].y);
		CHECK(got == 3.25f);
		count++;
	}
	printf("rectangle textures: %u texel and normalized samples checked\n", count);
	free(channel);
	return 0;
}
