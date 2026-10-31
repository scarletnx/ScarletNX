/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */

static u64 storage_mov(u32 reg, u32 value)
{
	return 0x0100000000071000ull | (u64)value << 20 | reg;
}

/* These levels have at most four rows, BH=BD=0: one GOB row per slice. */
static u64 storage_level_bytes(u32 row_bytes, u32 depth)
{
	return (u64)((row_bytes + MAXWELL_GOB_SIZE_X - 1) / MAXWELL_GOB_SIZE_X) * MAXWELL_GOB_SIZE * depth;
}

static int storage_write_words(u64 address, const u32 *words, u32 count)
{
	for (u32 i = 0; i < count; i++) {
		u8 word[4];

		snx_st_le32(word, words[i]);
		CHECK(!gmem_write(address + i * 4, word, sizeof(word)));
	}
	return 0;
}

static int storage_pixel(Surf *surface, u32 level, u32 layer, u32 z,
			const u8 *expected)
{
	VkBuffer buffer;
	u8 *pixel = vk_readback_buffer(surface->f.bpp, &buffer);
	VkBufferImageCopy copy = {
		.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
		.imageOffset = {1, 1, (i32)z},
		.imageExtent = {1, 1, 1},
	};

	CHECK(pixel);
	vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			     buffer, 1, &copy);
	vk_finish();
	vk_readback_ready();
	CHECK(!memcmp(pixel, expected, surface->f.bpp));
	return 0;
}

/* Every store must preserve the rest of its mip chain and array layers, plus
 * surrounding memory that can contain vertex data or uniform buffers.
 */
static int storage_image_case(GpuChannel *channel, const u32 *qmd, u64 address,
			     u32 format_word, u32 components, u32 component_bytes,
			     u32 id, u32 dim, int numeric, int bound, u32 mip_case)
{
	const VkComponentMapping identity = {0};
	u32 base = mip_case == 2, levels = mip_case && mip_case < 3 ? 3 : 1;
	u64 target = address + 0x30000 + (u64)id * 0x2000;
	u64 program = address + (u64)((dim - 3) + 3 * bound + 1) * 0x1000;
	u32 tic[8] = {format_word, (u32)target, (u32)(target >> 32) | 3u << 21,
		     (levels - 1) << 28,
		     7u | (dim == 3 ? 1u : dim == 4 ? 5u : 2u) << 23,
		     3u | (dim == 3 ? 0u : 3u) << 16, 0, (levels - 1) << 4 | base};
	u64 instructions[] = {
		storage_mov(0, 1), storage_mov(1, 1), storage_mov(2, 1),
		0x4c98078804a70004ull, /* MOV R4, c[2][0x128] */
		0x4c98078804c70008ull, 0x4c98078804d70009ull,
		0x4c98078804e7000aull, 0x4c98078804f7000bull,
		0, /* SUST.P.RGBA [R0], R8, R4 (or the bound handle at word 74) */
		0xe30000000007000full,
	};
	u8 code[384] = {0}, initial[8192], expected[8192], result[8192];
	u64 dispatches = g_vk_dispatches, bytes, offset;
	u64 program_ops[24];
	u32 program_count = 0, at = 0, values[4];
	const u32 floats[4] = {0xbe800000, 0x3f000000, 0xbf400000, 0x3f800000};
	const u32 halves[4] = {0xb400, 0x3800, 0xba00, 0x3c00};
	VkFmt format = vk_tic_format(format_word, 0);

	instructions[8] = (0xeb203a0200f77614ull &
		~(0xffull | 0xffull << 8 | 0xfull << 16 | 0xffull << 39 | 7ull << 33)) |
		8ull | 7ull << 16 | 4ull << 39 | (u64)dim << 33;
	if (bound)
		instructions[8] = (instructions[8] & ~(0x1fffull << 36)) | 74ull << 36 | 1ull << 51;
	for (u32 i = 0; i < sizeof(instructions) / sizeof(instructions[0]); i++) {
		u64 word = instructions[i];
		if ((mip_case == 4 || mip_case == 5) && i == 3) {
			/* A dynamic thread ID keeps both branches in the translator's
			 * control-flow graph, including for a single invocation. */
			program_ops[program_count++] = 0xf0c8000002170005ull;
			program_ops[program_count++] = mip_case == 4 ? 0x5b6b03800ff70507ull : 0x5b6503800ff70507ull;
			program_ops[program_count++] = 0xe24000000000000full | 24ull << 20;
			program_ops[program_count++] = 0x4c98078804b70004ull; /* input handle */
			program_ops[program_count++] = 0xe24000000007000full | 16ull << 20;
			program_ops[program_count++] = 0x4c98078804b70004ull;
			continue;
		}
		if ((mip_case == 4 || mip_case == 5) && i == 8) {
			program_ops[program_count++] = 0x4c98078804a70006ull; /* output handle */
			program_ops[program_count++] = 0xdd38000080070008ull | 2ull << 28 | 4ull << 20;
			word = (word & ~(255ull << 39)) | 6ull << 39;
		}
		if (mip_case == 3 && i == 3)
			program_ops[program_count++] = 0x5b6b03800ff70007ull; /* P0 = R0 != RZ */
		if (mip_case == 3 && i == 8) {
			program_ops[program_count++] = 0x50880380e007000cull; /* R12 = P0 */
			program_ops[program_count++] = 0x5b6503800ff70007ull; /* P0 = R0 == RZ */
			program_ops[program_count++] = 0x5b6b03800ff70c07ull; /* P0 = R12 != RZ */
		}
		if (mip_case == 3 && (i == 3 || i == 8)) word &= ~(15ull << 16);
		program_ops[program_count++] = word;
	}
	for (u32 i = 0; i < program_count; i++) {
		if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
		snx_st_le64(code + 8 * at++, program_ops[i]);
	}
	CHECK(!gmem_write(program, code, sizeof(code)));
	g_renderer->method_barrier(5); /* the fixture replaces code at a reused address */
	CHECK(!storage_write_words(address + 0x20000 + 0x128, (const u32[1]){0}, 1));
	if (mip_case == 4 || mip_case == 5) {
		u32 source_tic[8];
		memcpy(source_tic, tic, sizeof source_tic);
		source_tic[0] |= 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
		source_tic[1] += 4096;
		CHECK(!storage_write_words(address + 0x21020, source_tic, 8));
		CHECK(!storage_write_words(address + 0x20000 + 0x12c, (const u32[1]){1}, 1));
	}
	for (u32 c = 0; c < 4; c++) {
		values[c] = numeric == 2 ? floats[c] : 17 * (c + 1);
		if (format.vk == VK_FORMAT_R8G8B8A8_UNORM && c == 1) values[c] = 0x3f800000;
		if (numeric == 1 && !(c & 1)) values[c] = 0u - values[c];
	}
	CHECK(!storage_write_words(address + 0x20000 + 0x130, values, 4));
	CHECK(!storage_write_words(address + 0x21000, tic, 8));
	for (u32 i = 0; i < sizeof(initial); i++) initial[i] = (u8)(i * 37 + (i >> 7));
	if (mip_case == 4 || mip_case == 5) snx_st_le32(initial + 4096 + 20, 0x87654321);
	CHECK(!gmem_write(target, initial, sizeof(initial)));
	if (mip_case == 3) {
		vk_lock();
		rt_clear_format(channel, target, 16, 4, 0, 1,
				format.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 0xE0 : 0xD5);
		vk_surf_flush(vk_rt_find(target), NULL);
		vk_unlock();
		CHECK(!gmem_read(target, initial, sizeof(initial)));
	}
	memcpy(expected, initial, sizeof(expected));
	u64 level0 = storage_level_bytes(8 * format.bpp, dim == 5 ? 4 : 1);
	u64 stride = level0;

	if (levels > 1)
		stride += storage_level_bytes(4 * format.bpp, dim == 5 ? 2 : 1) +
			  storage_level_bytes(2 * format.bpp, 1);
	offset = (base ? level0 : 0) + (dim == 4 ? stride : 0) +
		 bl_offset(format.bpp, 1, dim == 5 ? 1 : 0, (8u >> base) * format.bpp, 4u >> base, 0, 0);
	CHECK(offset + format.bpp <= sizeof(expected));
	for (u32 c = 0; c < components; c++) {
		u32 value = numeric == 2 && component_bytes == 2 ? halves[c] : values[c];
		for (u32 b = 0; b < component_bytes; b++)
			expected[offset + c * component_bytes + b] = (u8)(value >> (8 * b));
	}
	if (format.vk == VK_FORMAT_R8G8B8A8_UNORM)
		memcpy(expected + offset, (const u8[4]){0, 255, 0, 255}, 4);
	if (format.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
		snx_st_le32(expected + offset, 0x380u << 11); /* (0, 0.5, 0), unsigned mini-floats */
	if (mip_case == 4 || mip_case == 5) snx_st_le32(expected + offset, 0x87654321);
	vk_lock();
	Surf *surface = vk_tex_from_tic_storage(tic);
	ShaderInfo info = {0};
	VkImageViewType view_type = dim == 5 ? VK_IMAGE_VIEW_TYPE_3D :
		dim == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

	CHECK(surface && surface->f.vk == format.vk && surface->levels == levels - base);
	info.nimg = 1;
	info.img[0].type = (u8)dim;
	info.img[0].kind = numeric == 2 ? 0 : numeric == 1 ? 3 : 2;
	VkImageView sampled = vk_tex_view(surface, view_type, 0, &identity, 0);
	VkImageView storage = vk_storage_image_view(surface, &info, 0, &bytes);

	CHECK(sampled && storage);
	CHECK(surface->levels == levels - base);
	CHECK(vk_tex_view(surface, view_type, 0, &identity, 0) == sampled);
	CHECK(vk_storage_image_view(surface, &info, 0, &bytes) == storage);
	if (surface->levels > 1) CHECK(sampled != storage);
	g_vk.feat.shaderStorageImageWriteWithoutFormat = 0;
	CHECK(!vk_storage_image_view(surface, &info, 0, &bytes));
	info.img[0].kind = 1;
	CHECK(!!vk_storage_image_view(surface, &info, 0, &bytes) == (format.vk == VK_FORMAT_R32_UINT));
	g_vk.feat.shaderStorageImageWriteWithoutFormat = 1;
	info.img[0].kind = numeric == 2 ? 0 : numeric == 1 ? 3 : 2;
	Surf wrong = *surface;

	if (numeric != 2) {
		wrong.f.sint = !surface->f.sint;
		CHECK(!vk_storage_image_view(&wrong, &info, 0, &bytes));
	}
	wrong.f.is_int = !surface->f.is_int;
	CHECK(!vk_storage_image_view(&wrong, &info, 0, &bytes));
	info.img[0].kind = numeric == 2 ? 2 : 0;
	CHECK(!vk_storage_image_view(surface, &info, 0, &bytes));
	{
		DispatchState d;
		mx_dispatch_state(qmd, channel->rcomp, program, &d);
		vk_dispatch(&d);
	}
	vk_unlock();
	if (g_vk_dispatches != dispatches + 1)
		fprintf(stderr, "storage image format %u dimension %u bound %d base %u skipped\n",
			format.vk, dim, bound, base);
	CHECK(g_vk_dispatches == dispatches + 1);
	CHECK(!gmem_read(target, result, sizeof(result)));
	if (memcmp(result, expected, sizeof(result))) {
		fprintf(stderr, "storage image format %u dimension %u bound %d base %u differs\n",
			format.vk, dim, bound, base);
		fprintf(stderr, "expected write at %llu, guest span %llu, layer stride %llu\n",
			(unsigned long long)offset, (unsigned long long)bytes, (unsigned long long)surface->layer_size);
		for (u32 i = 0, n = 0; i < sizeof(result) && n < 8; i++)
			if (result[i] != expected[i]) {
				fprintf(stderr, "byte %u: got %02x expected %02x\n", i, result[i], expected[i]);
				n++;
			}
	}
	CHECK(!memcmp(result, expected, sizeof(result)));
	/* Read through the complete sampled texture after a selected-mip write.
	 * The original mip chain must see that update, including cached chains.
	 */
	if (base) {
		tic[7] &= ~15u;
		vk_lock();
		Surf *chain = vk_tex_from_tic(tic);

		CHECK(chain && chain->levels == levels);
		CHECK(!storage_pixel(chain, base, dim == 4 ? 1 : 0, dim == 5 ? 1 : 0, expected + offset));
		vk_unlock();
	}
	return 0;
}

static int check_storage_image_formats(void)
{
	static const struct {
		u32 format, components, component_bytes;
	} formats[] = {
		{0x08, 4, 1}, {0x1b, 1, 2}, {0x0f, 1, 4}, {0x04, 2, 4},
		{0x03, 4, 2}, {0x01, 4, 4}, {0x1d, 1, 1},
	};
	const u64 address = 0x500000000ull;
	const u64 cbuf = address + 0x20000, descriptors = address + 0x21000;
	GpuChannel *channel = calloc(1, sizeof(*channel));
	u32 qmd[64] = {0};
	u32 completed = 0;

	CHECK(channel);
	if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
		puts("storage image formats: unformatted writes unavailable, skipped");
		free(channel);
		return 0;
	}
	gmmu_map(address, HEAP_BASE + 0x700000, 0x60000);
	qmd[QMD_GRID_DIM_X] = 1;
	qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
	qmd[QMD_BLOCK_DIM_X] = 1u << 16;
	qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
	qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2;
	qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)cbuf;
	qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(cbuf >> 32) | 0x200u << 15;
	channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
	channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
	channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
	for (u32 f = 0; f < sizeof(formats) / sizeof(formats[0]); f++)
		for (int numeric = 0; numeric < 3; numeric++) {
			if (numeric == 2 && (formats[f].format == 0x08 || formats[f].format == 0x1d)) continue;
			u32 type = numeric == 2 ? 7 : numeric == 1 ? 3 : 4;
			u32 word0 = formats[f].format | type << 7 | type << 10 | type << 13 | type << 16;
			VkFmt format = vk_tic_format(word0, 0);

			CHECK(!!format.is_int == (numeric != 2) && !!format.sint == (numeric == 1));
			if (!vk_format_storage_write(format.vk)) continue;
			for (u32 dim = 3; dim <= 5; dim++)
				for (int bound = 0; bound < 2; bound++)
					for (u32 mip = 0; mip < 3; mip++) {
						CHECK(!storage_image_case(channel, qmd, address, word0, formats[f].components,
							formats[f].component_bytes, f * 3 + dim - 3, dim, numeric, bound, mip));
						completed++;
					}
		}
	/* A smaller writable descriptor over a wider target must preserve the
	 * surrounding target bytes and publish the store through guest memory. */
	const u32 unorm = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
	CHECK(!storage_image_case(channel, qmd, address, unorm, 4, 1, 21, 3, 2, 0, 3));
	completed++;
	if (vk_format_storage_write(VK_FORMAT_B10G11R11_UFLOAT_PACK32)) {
		const u32 packed = 0x21 | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16;
		for (u32 crop = 0; crop < 2; crop++)
			for (int bound = 0; bound < 2; bound++) {
				CHECK(!storage_image_case(channel, qmd, address, packed, 0, 0,
						 22 + (u32)bound, 3, 2, bound, crop ? 3 : 0));
				completed++;
			}
	}
	const u32 uint_word = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16;
	for (u32 path = 4; path <= 5; path++) {
		CHECK(!storage_image_case(channel, qmd, address, uint_word, 1, 4, 0, 3, 0, 0, path));
		completed++;
	}
	CHECK(completed >= 36);
	printf("storage image formats: %u signed/unsigned/float, bound/bindless and mip cases checked\n", completed);
	free(channel);
	return 0;
}
