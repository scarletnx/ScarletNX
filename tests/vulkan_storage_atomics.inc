/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */

static u32 volume_atomic_result(u32 op, u32 before, u32 operand, int signed_compare, u32 invocations)
{
	u32 a = before ^ (signed_compare ? 0x80000000u : 0);
	u32 b = operand ^ (signed_compare ? 0x80000000u : 0);

	switch (op) {
	case 0: return before + invocations * operand;
	case 1: return a < b ? before : operand;
	case 2: return a > b ? before : operand;
	case 5: return before & operand;
	case 6: return before | operand;
	case 7: return before ^ (invocations & 1 ? operand : 0);
	case 8: return operand;
	default: return before;
	}
}

/* Many invocations contend on one voxel. A separate single-invocation pass
 * records SUATOM's returned value into a neighboring voxel through SUST.
 */
static int check_volume_surface_atomics(void)
{
	static const struct {
		u32 op, initial, operand;
	} cases[] = {
		{0, 5, 3}, {1, 0x7ffffff0, 0x80000010}, {2, 0x80000010, 0x7ffffff0},
		{5, 0xf0f0a5a5, 0x0ff00ff0}, {6, 0xf000aa00, 0x00ff0055},
		{7, 0xabcd1234, 0x55aa55aa}, {8, 1, 0xa5f01234},
	};
	const u64 address = 0x600000000ull;
	const u64 cbuf = address + 0x20000, descriptors = address + 0x21000;
	const u64 target = address + 0x30000;
	GpuChannel *channel = calloc(1, sizeof(*channel));
	u32 qmd[64] = {0};
	u32 count = 0;

	CHECK(channel);
	gmmu_map(address, HEAP_BASE + 0x7c0000, 0x40000);
	qmd[QMD_GRID_DIM_X] = 1;
	qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
	qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
	qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2;
	qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)cbuf;
	qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(cbuf >> 32) | 0x200u << 15;
	channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
	channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
	channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
	for (u32 operation = 0; operation < sizeof(cases) / sizeof(cases[0]); operation++)
		for (u32 signed_compare = 0; signed_compare < 2; signed_compare++)
			for (u32 bound = 0; bound < 2; bound++)
				for (u32 base = 0; base < 2; base++)
					for (u32 family = 0; family < 3; family++) {
						int reduction = family == 0, returned = family == 2;
						u32 op = cases[operation].op, at = 0;
						u32 invocations = returned ? 1 : 63;
						u32 program_id = operation * 12 + signed_compare * 6 + bound * 3 + family;
						u64 program = address + 0x1000 + program_id * 0x200;
						u32 tic[8] = {0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16,
							(u32)target, (u32)(target >> 32) | 3u << 21, 2u << 28,
							7u | 2u << 23, 3u | 3u << 16, 0, 2u << 4 | base};
						u64 atomic = reduction ? 0xeb50020a00070008ull : 0xea40020a00870009ull;
						u64 store = 0xeb20020a00170009ull;
						u64 instructions[10] = {storage_mov(0, 1), storage_mov(1, 1), storage_mov(2, 1),
							0x4c98078804a70004ull, 0x4c98078804c70008ull};
						u8 code[256] = {0}, initial[4096], expected[4096], result[4096];
						u64 dispatches = g_vk_dispatches;
						u64 offset = (base ? 2048 : 0) + bl_offset(4, 1, 1, (8u >> base) * 4, 4u >> base, 0, 0);
						u64 prior_offset = (base ? 2048 : 0) + bl_offset(8, 1, 1, (8u >> base) * 4, 4u >> base, 0, 0);
						u32 n = 5;

						if (reduction && op > 2) continue;
						if (returned && !g_vk.feat.shaderStorageImageWriteWithoutFormat) continue;
						if (reduction)
							atomic = (atomic & ~(7ull << 20 | 7ull << 24)) | (u64)signed_compare << 20 | (u64)op << 24;
						else
							atomic = (atomic & ~(7ull << 51 | 15ull << 29)) | (u64)signed_compare << 51 | (u64)op << 29;
						if (bound) {
							atomic = (atomic & ~(0x1fffull << 36)) | 74ull << 36;
							if (reduction) atomic |= 1ull << 51;
							else atomic &= ~(1ull << 54);
						}
						instructions[n++] = atomic;
						if (returned) {
							instructions[n++] = storage_mov(0, 2);
							instructions[n++] = store;
						}
						instructions[n++] = 0xe30000000007000full;
						for (u32 i = 0; i < n; i++) {
							if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
							snx_st_le64(code + 8 * at++, instructions[i]);
						}
						for (u32 i = 0; i < sizeof(initial); i++) initial[i] = (u8)(i * 71 + (i >> 8));
						snx_st_le32(initial + offset, cases[operation].initial);
						memcpy(expected, initial, sizeof(expected));
						snx_st_le32(expected + offset, volume_atomic_result(op, cases[operation].initial,
							cases[operation].operand, signed_compare, invocations));
						if (returned) snx_st_le32(expected + prior_offset, cases[operation].initial);
						CHECK(!gmem_write(program, code, sizeof(code)));
						CHECK(!storage_write_words(cbuf + 0x128, (const u32[1]){0}, 1));
						CHECK(!storage_write_words(cbuf + 0x130, &cases[operation].operand, 1));
						CHECK(!storage_write_words(descriptors, tic, 8));
						CHECK(!gmem_write(target, initial, sizeof(initial)));
						qmd[QMD_BLOCK_DIM_X] = invocations << 16;
						vk_lock();
						{
							DispatchState d;
							mx_dispatch_state(qmd, channel->rcomp, program, &d);
							vk_dispatch(&d);
						}
						vk_unlock();
						if (g_vk_dispatches != dispatches + 1)
							fprintf(stderr, "3D atomic skipped: op %u signed %u bound %u base %u family %u\n",
								op, signed_compare, bound, base, family);
						CHECK(g_vk_dispatches == dispatches + 1);
						CHECK(!gmem_read(target, result, sizeof(result)));
						if (memcmp(result, expected, sizeof(result))) {
							u32 first = 0;
							while (first < sizeof(result) && result[first] == expected[first]) first++;
							fprintf(stderr, "3D atomic differs: op %u signed %u bound %u base %u family %u: voxel %08x, "
								"expected %08x (initial %08x, operand %08x); first byte apart at %u of %u\n",
								op, signed_compare, bound, base, family, snx_ld_le32(result + offset),
								snx_ld_le32(expected + offset), cases[operation].initial, cases[operation].operand,
								first, (u32)offset);
						}
						CHECK(!memcmp(result, expected, sizeof(result)));
						count++;
					}
	printf("3D storage atomics: %u contention, return-value and mip cases checked\n", count);
	free(channel);
	return 0;
}
