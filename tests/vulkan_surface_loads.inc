/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */

static int check_layered_surface_loads(void)
{
	static const struct {
		u32 format, component_type, pixel;
		int raw;
	} cases[] = {
		{0x0f, 4, 0x12345678, 0}, {0x0f, 3, 0xffffffde, 0}, {0x0f, 7, 0xbf400000, 0},
		{0x08, 4, 0xaa996655, 0}, {0x08, 3, 0x44cd22ef, 0},
		{0x0f, 7, 0xbe800000, 1}, {0x08, 2, 0x87654321, 1},
	};
	const u64 address = 0x700000000ull;
	const u64 cbuf = address + 0x20000, descriptors = address + 0x21000, samplers = address + 0x22000;
	const u64 source = address + 0x30000, target = address + 0x32000;
	GpuChannel *channel = calloc(1, sizeof(*channel));
	u32 qmd[64] = {0}, count = 0;

	CHECK(channel);
	if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
		puts("surface load round-trips: storage writes unavailable, skipped");
		free(channel);
		return 0;
	}
	gmmu_map(address, HEAP_BASE + 0x6c0000, 0x40000);
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
	CHECK(!storage_write_words(samplers, (const u32[8]){0}, 8));
	CHECK(!storage_write_words(cbuf + 0x128, (const u32[2]){0, 1}, 2));
	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
		for (u32 dim = 4; dim <= 5; dim++)
			for (u32 bound = 0; bound < 2; bound++)
				for (u32 base = 0; base < 2; base++) {
					u32 type = cases[c].component_type;
					u32 word0 = cases[c].format | type << 7 | type << 10 | type << 13 | type << 16 |
						2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
					int rgba = !cases[c].raw && cases[c].format == 0x08;
					u32 mask = rgba ? 15 : 1;
					u32 id = (dim - 4) * 8 + (u32)cases[c].raw * 4 + bound * 2 + (u32)rgba;
					u64 program = address + 0x1000 + id * 0x200;
					u32 tic[8] = {word0, (u32)source, (u32)(source >> 32) | 3u << 21, 2u << 28,
						7u | (dim == 4 ? 5u : 2u) << 23, 3u | 3u << 16, 0, 2u << 4 | base};
					u32 output_tic[8];
					u64 load = 0xeb00000000070008ull | (u64)dim << 33 | 4ull << 39 | (u64)mask << 20;
					u64 store = 0xeb20000000070008ull | (u64)dim << 33 | 6ull << 39 | (u64)mask << 20;
					u64 instructions[] = {storage_mov(0, 1), storage_mov(1, 1), storage_mov(2, 1),
						0x4c98078804a70004ull, 0x4c98078804b70006ull, 0, storage_mov(0, 2), 0,
						0xe30000000007000full};
					u8 code[256] = {0}, input[8192], output[8192], expected[8192], result[8192];
					u64 dispatches = g_vk_dispatches;
					u32 at = 0;
					u64 level0 = 512u * (dim == 5 ? 4 : 1);
					u64 stride = level0 + 512u * (dim == 5 ? 2 : 1) + 512u;
					u64 start = (base ? level0 : 0) + (dim == 4 ? stride : 0);
					u64 input_offset = start + bl_offset(4, 1, dim == 5 ? 1 : 0, (8u >> base) * 4, 4u >> base, 0, 0);
					u64 output_offset = start + bl_offset(8, 1, dim == 5 ? 1 : 0, (8u >> base) * 4, 4u >> base, 0, 0);

					memcpy(output_tic, tic, sizeof(tic));
					output_tic[1] = (u32)target;
					if (cases[c].raw) {
						load = (load & ~(15ull << 20)) | 4ull << 20 | 1ull << 52;
						output_tic[0] = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16;
					}
					if (bound) {
						load = (load & ~(0x1fffull << 36)) | 74ull << 36 | 1ull << 51;
						store = (store & ~(0x1fffull << 36)) | 75ull << 36 | 1ull << 51;
					}
					instructions[5] = load;
					instructions[7] = store;
					for (u32 i = 0; i < sizeof(instructions) / sizeof(instructions[0]); i++) {
						if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
						snx_st_le64(code + 8 * at++, instructions[i]);
					}
					for (u32 i = 0; i < sizeof(input); i++) {
						input[i] = (u8)(i * 11 + (i >> 7));
						output[i] = (u8)(i * 37 + (i >> 8));
					}
					snx_st_le32(input + input_offset, cases[c].pixel);
					memcpy(expected, output, sizeof(expected));
					snx_st_le32(expected + output_offset, cases[c].pixel);
					CHECK(!gmem_write(program, code, sizeof(code)));
					CHECK(!gmem_write(source, input, sizeof(input)));
					CHECK(!gmem_write(target, output, sizeof(output)));
					CHECK(!storage_write_words(descriptors, tic, 8));
					CHECK(!storage_write_words(descriptors + 32, output_tic, 8));
					vk_lock();
					{
						DispatchState d;
						mx_dispatch_state(qmd, channel->rcomp, program, &d);
						vk_dispatch(&d);
					}
					vk_unlock();
					if (g_vk_dispatches != dispatches + 1)
						fprintf(stderr, "surface load skipped: case %u dim %u bound %u base %u\n", c, dim, bound, base);
					CHECK(g_vk_dispatches == dispatches + 1);
					CHECK(!gmem_read(target, result, sizeof(result)));
					if (memcmp(result, expected, sizeof(result)))
						fprintf(stderr, "surface load differs: case %u dim %u bound %u base %u got %08x expected %08x\n",
							c, dim, bound, base, snx_ld_le32(result + output_offset), cases[c].pixel);
					CHECK(!memcmp(result, expected, sizeof(result)));
					CHECK(!gmem_read(source, result, sizeof(result)));
					CHECK(!memcmp(result, input, sizeof(result)));
					count++;
				}
	printf("array and volume surface loads: %u formatted/raw round-trips checked\n", count);
	free(channel);
	return 0;
}
