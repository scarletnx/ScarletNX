/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   ISBE layout and mesh epilogue based on Eglantine's emit_spirv_mesh.cpp:
   Copyright 2026 Eden Emulator Project, GPL-3.0-or-later.
   SPHv4 field reference: NVIDIA clcb97sph.h, Copyright (c) 2003-2021 NVIDIA CORPORATION & AFFILIATES, MIT. */

/* Included by spirv_translate.c. The native program runs in a called function. Its early EXITs return to one
   uniform epilogue, so invocations that cull themselves still reach the workgroup barrier and mesh output count.
   The ISBE attribute region has 32-word scalar lines, grouped by 32 vertices, then by 32 primitives. Map bytes
   have separate words: neighboring invocations can write their U8 indices without a read/modify/write race. */
static u32 mesh_const(DirectCtx* c, u32 value) {
    return spv_emit_constant_u32(&c->emit, c->u32_type, value);
}

/* The task's output region and the mesh's input region when a task runs: Vulkan's payload. Its size is a translation
   constant; the guest writes and reads are clamped to it, as the reference emulator clamps to the header's size. */
#define SH_MESH_PAYLOAD_WORDS 1024u
static int task_layout_valid(const ShaderInfo* si, const u32 sph[20]) {
    u32 threads = si->mesh[1] >> 20 & 0x3f;
    return (sph[0] >> 5 & 31) == 4 && (si->mesh[0] & 15) == 4 && threads && threads <= 63 &&
           !(si->mesh[1] & ~0x03f001ffu);
}
static void task_declare(DirectCtx* c, const ShaderInfo* si, u32 vec3_u32, u32* interfaces, u32* count) {
    c->mesh.threads = si->mesh[1] >> 20 & 0x3f;
    c->mesh.task = 1;
    c->mesh.local = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 27);
    c->mesh.group = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 26);
    interfaces[(*count)++] = c->mesh.local;
    interfaces[(*count)++] = c->mesh.group;
    u32 payload_type = spv_emit_type_array_u32(&c->emit, c->u32_type, SH_MESH_PAYLOAD_WORDS);
    c->mesh.payload = spv_emit_variable(&c->emit, 5402, payload_type); /* TaskPayloadWorkgroupEXT */
    c->mesh.payload_pointer = spv_emit_type_pointer(&c->emit, 5402, c->u32_type);
    interfaces[(*count)++] = c->mesh.payload;
}
static void task_epilogue(DirectCtx* c) {
    /* every invocation reaches this: the barrier orders their payload writes, then the workgroup's count launches
       the mesh workgroups (payload word 1, which the task programs write at byte 4) */
    spv_emit_control_barrier(&c->emit, c->u32_type);
    u32 ptr = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, c->mesh.payload_pointer, c->mesh.payload, mesh_const(c, 1));
    u32 count = spv_emit_load(&c->emit, c->u32_type, ptr);
    SPV_VOID(&c->emit, 5294, count, mesh_const(c, 1), mesh_const(c, 1), c->mesh.payload); /* OpEmitMeshTasksEXT */
}
static u32 mesh_pointer(DirectCtx* c, u32 array, u32 index) {
    return SPV(&c->emit, SPV_OP_ACCESS_CHAIN, c->mesh.shared_pointer, array, index);
}
static u32 mesh_output(DirectCtx* c, u32 type, u32 length, u32 builtin, u32 location, int primitive) {
    u32 array = spv_emit_type_array(&c->emit, type, length);
    u32 variable = builtin == SPV_NOT_BUILTIN ? spv_emit_location(&c->emit, 3, array, location)
                                             : spv_emit_builtin(&c->emit, 3, array, builtin);
    if (primitive) spv_emit_decorate(&c->emit, variable, 5271, NULL, 0); /* PerPrimitiveEXT */
    return variable;
}
static int mesh_layout_valid(const ShaderInfo* si, const u32 sph[20]) {
    u32 vertices = si->mesh[0] >> 4 & 0x1ff, primitives = si->mesh[0] >> 13 & 0x3ff;
    u32 threads = si->mesh[1] >> 20 & 0x3f;
    if ((si->mesh[0] & 15) != 4 || !vertices || vertices > 256 || !primitives || primitives > 256 ||
        !threads || threads > 32 || (si->mesh[0] & ~0x007fffffu) || (si->mesh[1] & ~0x03f001ffu)) return 0;
    /* No legacy color, clip, fixed-function or viewport-mask outputs in this path. Keep their bits out of the
       line count rather than silently assigning later generic lines to the wrong attribute. */
    if ((sph[13] & 0x7fu) || (sph[17] & 0xffff0000u) || sph[18] || sph[19] ||
        (si->mesh[3] & 0x7fu) || (si->mesh[7] & 0xffff0000u)) return 0;
    /* Position is per vertex. PrimitiveId, Layer and ViewportIndex are per primitive. */
    if ((sph[13] & 0x700u) || (si->mesh[3] & 0xf800u) || (sph[13] & 0xf000u) != 0xf000u) return 0;
    for (u32 a = 32; a < 160; ++a)
        if (shader_mesh_omap_bit(sph + 12, a) && shader_mesh_omap_bit(si->mesh + 2, a)) return 0;
    return 1;
}
static int mesh_declare(DirectCtx* c, const ShaderInfo* si, const u32 sph[20], u32 vec4, u32 vec3_u32,
                        u32 i32_type, u32* interfaces, u32* count) {
    c->mesh.vertices = si->mesh[0] >> 4 & 0x1ff;
    c->mesh.primitives = si->mesh[0] >> 13 & 0x3ff;
    c->mesh.threads = si->mesh[1] >> 20 & 0x3f;
    c->mesh.local = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 27);
    c->mesh.group = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 26);
    interfaces[(*count)++] = c->mesh.local;
    interfaces[(*count)++] = c->mesh.group;
    c->mesh.indices = mesh_output(c, vec3_u32, c->mesh.primitives, 5296, 0, 0); /* PrimitiveTriangleIndicesEXT */
    interfaces[(*count)++] = c->mesh.indices;
    for (u32 p = 0; p < 2; ++p) {
        const u32* map = p ? si->mesh + 2 : sph + 12;
        u32 generics[32] = {0}, position = 0;
        for (u32 a = 23; a < 160; ++a) {
            if (!shader_mesh_omap_bit(map, a)) continue;
            u32 line = c->mesh.lines[p]++;
            c->mesh.attribute[p][line] = (u8)a;
            u32 variable = 0, length = p ? c->mesh.primitives : c->mesh.vertices;
            if (a >= 32) {
                u32 location = (a - 32) / 4;
                if (!generics[location]) {
                    generics[location] = mesh_output(c, vec4, length, SPV_NOT_BUILTIN, location, p);
                    interfaces[(*count)++] = generics[location];
                }
                variable = generics[location];
            } else if (a >= 28) {
                if (!position) {
                    position = mesh_output(c, vec4, length, 0, 0, 0);
                    interfaces[(*count)++] = position;
                    c->mesh.position = position;
                }
                variable = position;
            } else if (a >= 24 && a <= 27) {
                static const u32 builtin[4] = {7, 9, 10, 1}; /* PrimitiveId, Layer, ViewportIndex, PointSize */
                variable = mesh_output(c, a == 27 ? c->f32_type : i32_type, length, builtin[a - 24], 0, p);
                interfaces[(*count)++] = variable;
            }
            /* VPRS (a[0x5c]) still occupies a line, even though full-rate rasterization does not consume it. */
            c->mesh.output[p][line] = variable;
        }
    }
    c->mesh.primitive_base = ((c->mesh.vertices + 31) / 32) * c->mesh.lines[0] * 32;
    c->mesh.attribute_words = c->mesh.primitive_base +
        ((c->mesh.primitives + 31) / 32) * c->mesh.lines[1] * 32 + (si->mesh[1] & 0x1ff) * 32;
    c->mesh.map_bytes = 4 + 3 * c->mesh.primitives;
    /* Vulkan guarantees these minima for mesh shaders. The renderer also checks the host's actual output limits. */
    if (c->mesh.lines[0] + c->mesh.lines[1] > 128 ||
        (c->mesh.attribute_words + c->mesh.map_bytes + 1) * 4 > 28672) return 0;
    if (si->mesh_task) {
        u32 payload_type = spv_emit_type_array_u32(&c->emit, c->u32_type, SH_MESH_PAYLOAD_WORDS);
        c->mesh.payload = spv_emit_variable(&c->emit, 5402, payload_type); /* TaskPayloadWorkgroupEXT */
        c->mesh.payload_pointer = spv_emit_type_pointer(&c->emit, 5402, c->u32_type);
        c->mesh.mesh_task = 1;
        interfaces[(*count)++] = c->mesh.payload;
    }
    c->mesh.shared_pointer = spv_emit_type_workgroup_pointer(&c->emit, c->u32_type);
    c->mesh.attributes = spv_emit_variable(&c->emit, 4,
        spv_emit_type_array_u32(&c->emit, c->u32_type, c->mesh.attribute_words));
    c->mesh.map = spv_emit_variable(&c->emit, 4,
        spv_emit_type_array_u32(&c->emit, c->u32_type, c->mesh.map_bytes));
    c->mesh.count = spv_emit_variable(&c->emit, 4, c->u32_type);
    interfaces[(*count)++] = c->mesh.attributes;
    interfaces[(*count)++] = c->mesh.map;
    interfaces[(*count)++] = c->mesh.count;
    return 1;
}
static void mesh_prologue(DirectCtx* c) {
    /* Every invocation zeroes the primitive count; the barrier orders those zeroes before any invocation's increments.
       Without it one invocation's zero can land after another's increment. */
    spv_emit_store(&c->emit, c->mesh.count, c->zero);
    spv_emit_control_barrier(&c->emit, c->u32_type);
}
static void mesh_access(DirectCtx* c, u64 w) {
    u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, B(w, 8, 8)),
                                    mesh_const(c, B(w, 20, 24)));
    if (!B(w, 48, 1)) {
        /* the input region holds this mesh workgroup's index at byte 0; with a task, the other words are its
           payload */
        u32 group = spv_emit_load(&c->emit, spv_emit_type_vec3(&c->emit, c->u32_type), c->mesh.group);
        u32 index = spv_emit_vec4_extract(&c->emit, c->u32_type, group, 0);
        if (!c->mesh.mesh_task) {
            reg_write(c, B(w, 0, 8), index);
            return;
        }
        u32 word = shared_index(c, spv_emit_shift_right_u32(&c->emit, c->u32_type, address, mesh_const(c, 2)),
                                SH_MESH_PAYLOAD_WORDS);
        u32 at = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, c->mesh.payload_pointer, c->mesh.payload, word);
        u32 payload = spv_emit_load(&c->emit, c->u32_type, at);
        u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, address, c->zero);
        reg_write(c, B(w, 0, 8), spv_emit_select(&c->emit, c->u32_type, zero, index, payload));
        return;
    }
    u32 value = reg_read(c, B(w, 0, 8));
    if (c->mesh.task) {
        /* a task's output region is the payload: its writes land there, clamped to its size */
        u32 word = shared_index(c, spv_emit_shift_right_u32(&c->emit, c->u32_type, address, mesh_const(c, 2)),
                                SH_MESH_PAYLOAD_WORDS);
        u32 at = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, c->mesh.payload_pointer, c->mesh.payload, word);
        spv_emit_store(&c->emit, at, value);
        return;
    }
    if (!B(w, 45, 1) && B(w, 44, 1)) {
        /* The preamble can zero the count from every lane. Atomic exchange avoids racing same-value stores. */
        SPV(&c->emit, SPV_OP_ATOMIC_EXCHANGE, c->u32_type, c->mesh.count, mesh_const(c, 2), c->zero, value);
        return;
    }
    if (B(w, 45, 1)) {
        u32 word = spv_emit_shift_right_u32(&c->emit, c->u32_type, address, mesh_const(c, 2));
        spv_emit_store(&c->emit, mesh_pointer(c, c->mesh.attributes, shared_index(c, word, c->mesh.attribute_words)), value);
    } else {
        value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value, mesh_const(c, 255));
        spv_emit_store(&c->emit, mesh_pointer(c, c->mesh.map, shared_index(c, address, c->mesh.map_bytes)), value);
    }
}
static void mesh_copy_lines(DirectCtx* c, u32 primitive, u32 index) {
    u32 group = spv_emit_shift_right_u32(&c->emit, c->u32_type, index, mesh_const(c, 5));
    u32 slot = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index, mesh_const(c, 31));
    u32 first = spv_emit_iadd_u32(&c->emit, c->u32_type, slot,
        spv_emit_imul_u32(&c->emit, c->u32_type, group, mesh_const(c, c->mesh.lines[primitive] * 32)));
    for (u32 line = 0; line < c->mesh.lines[primitive]; ++line) {
        if (!c->mesh.output[primitive][line]) continue;
        u32 a = c->mesh.attribute[primitive][line], scalar = a >= 27 ? c->f32_type : spv_emit_type_i32(&c->emit);
        u32 word = spv_emit_iadd_u32(&c->emit, c->u32_type, first,
            mesh_const(c, line * 32 + (primitive ? c->mesh.primitive_base : 0)));
        u32 raw = spv_emit_load(&c->emit, c->u32_type, mesh_pointer(c, c->mesh.attributes, word));
        u32 value = spv_emit_bitcast(&c->emit, scalar, raw);
        u32 pointer = spv_emit_type_pointer(&c->emit, 3, scalar), output = c->mesh.output[primitive][line];
        u32 ptr = a >= 28 ? SPV(&c->emit, SPV_OP_ACCESS_CHAIN, pointer, output, index, mesh_const(c, a & 3))
                          : SPV(&c->emit, SPV_OP_ACCESS_CHAIN, pointer, output, index);
        spv_emit_store(&c->emit, ptr, value);
    }
}
/* OpenGL's clip space: z = (z + w) / 2, computed from the vertex's shared position lines. A mesh shader can't read
   its Output variables, and these are read before the output is written. */
static void mesh_depth_fix(DirectCtx* c, u32 index, u32 push, u32 push_pointer) {
    u32 zline = ~0u, wline = ~0u;
    for (u32 line = 0; line < c->mesh.lines[0]; ++line) {
        if (c->mesh.attribute[0][line] == 30) zline = line;
        else if (c->mesh.attribute[0][line] == 31) wline = line;
    }
    if (zline == ~0u || wline == ~0u || !c->mesh.position) return;
    u32 group = spv_emit_shift_right_u32(&c->emit, c->u32_type, index, mesh_const(c, 5));
    u32 slot = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index, mesh_const(c, 31));
    u32 first = spv_emit_iadd_u32(&c->emit, c->u32_type, slot,
        spv_emit_imul_u32(&c->emit, c->u32_type, group, mesh_const(c, c->mesh.lines[0] * 32)));
    u32 zword = spv_emit_iadd_u32(&c->emit, c->u32_type, first, mesh_const(c, zline * 32));
    u32 wword = spv_emit_iadd_u32(&c->emit, c->u32_type, first, mesh_const(c, wline * 32));
    u32 z = spv_emit_bitcast(&c->emit, c->f32_type,
                             spv_emit_load(&c->emit, c->u32_type, mesh_pointer(c, c->mesh.attributes, zword)));
    u32 w = spv_emit_bitcast(&c->emit, c->f32_type,
                             spv_emit_load(&c->emit, c->u32_type, mesh_pointer(c, c->mesh.attributes, wword)));
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, c->zero, mesh_const(c, 1), c->f32_type);
    u32 convert = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled,
                                     spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
    u32 fixed = spv_emit_fmul(&c->emit, c->f32_type, spv_emit_fadd(&c->emit, c->f32_type, z, w),
                              spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f));
    u32 ptr = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, spv_emit_type_pointer(&c->emit, 3, c->f32_type), c->mesh.position,
                  index, mesh_const(c, 2));
    spv_emit_store(&c->emit, ptr, spv_emit_select(&c->emit, c->f32_type, convert, fixed, z));
}
static void mesh_epilogue(DirectCtx* c, u32 push, u32 push_pointer) {
    spv_emit_control_barrier(&c->emit, c->u32_type);
    u32 count = shared_index(c, spv_emit_load(&c->emit, c->u32_type, c->mesh.count), c->mesh.primitives + 1);
    SPV_VOID(&c->emit, 5295, mesh_const(c, c->mesh.vertices), count); /* OpSetMeshOutputsEXT */
    u32 local = spv_emit_load(&c->emit, spv_emit_type_vec3(&c->emit, c->u32_type), c->mesh.local);
    u32 lane = spv_emit_vec4_extract(&c->emit, c->u32_type, local, 0);
    for (u32 primitive = 0; primitive < 2; ++primitive) {
        u32 maximum = primitive ? c->mesh.primitives : c->mesh.vertices;
        for (u32 start = 0; start < maximum; start += c->mesh.threads) {
            u32 index = spv_emit_iadd_u32(&c->emit, c->u32_type, lane, mesh_const(c, start));
            u32 condition = spv_emit_u_less(&c->emit, c->bool_type, index,
                                            primitive ? count : mesh_const(c, maximum));
            u32 body = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
            spv_emit_selection_merge(&c->emit, merge);
            spv_emit_branch_conditional(&c->emit, condition, body, merge);
            spv_emit_label(&c->emit, body);
            mesh_copy_lines(c, primitive, index);
            if (primitive) {
                u32 base = spv_emit_iadd_u32(&c->emit, c->u32_type, mesh_const(c, 4),
                                            spv_emit_imul_u32(&c->emit, c->u32_type, index, mesh_const(c, 3)));
                u32 vertices[3];
                for (u32 k = 0; k < 3; ++k) {
                    u32 at = spv_emit_iadd_u32(&c->emit, c->u32_type, base, mesh_const(c, k));
                    vertices[k] = spv_emit_load(&c->emit, c->u32_type, mesh_pointer(c, c->mesh.map, at));
                }
                u32 vec3 = spv_emit_type_vec3(&c->emit, c->u32_type);
                u32 ptr = SPV(&c->emit, SPV_OP_ACCESS_CHAIN, spv_emit_type_pointer(&c->emit, 3, vec3), c->mesh.indices, index);
                spv_emit_store(&c->emit, ptr, spv_emit_vec3_construct(&c->emit, vec3, vertices[0], vertices[1], vertices[2]));
            } else if (push) {
                mesh_depth_fix(c, index, push, push_pointer);
            }
            spv_emit_branch(&c->emit, merge);
            spv_emit_label(&c->emit, merge);
        }
    }
}
