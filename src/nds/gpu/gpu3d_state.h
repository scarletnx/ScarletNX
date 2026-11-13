/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2026 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's state, which the geometry commands change (gpu3d.c) and the renderer draws a frame from
   (vk/render3d.c). The layout of the vertices and polygons is melonDS's (GPU3D.h, GPL-3.0-or-later). */
#ifndef SNX_NDS_GPU3D_STATE_H
#define SNX_NDS_GPU3D_STATE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t position[4]; /* clip coordinates */
    int32_t color[3];
    int16_t tex_coords[2];
    bool clipped;
    /* the vertex as it is drawn, which the polygons of a strip share */
    int32_t final_position[2];
    int32_t final_color[3];
    int32_t hires_position[2]; /* with four more bits */
} NdsVertex;

typedef struct {
    NdsVertex* vertices[10];
    uint32_t num_vertices;
    int32_t final_z[10];
    int32_t final_w[10];
    bool w_buffer;
    uint32_t attr, tex_param, tex_palette;
    bool degenerate, facing_view, translucent, is_shadow_mask, is_shadow;
    int type; /* 0 a polygon, 1 a line */
    uint32_t v_top, v_bottom;
    int32_t y_top, y_bottom, x_top, x_bottom;
    uint32_t sort_key;
} NdsPolygon;

/* a geometry command that waits for the buffers to swap */
typedef struct {
    uint32_t param;
    uint8_t command;
} NdsGxEntry;

typedef struct NdsGpu3D {
    uint32_t zero_dot_w_limit, gx_stat;
    union {
        uint32_t exec_params[32];
        int32_t exec_params_s[32];
    };
    uint32_t exec_param_count;
    uint32_t matrix_mode;
    int32_t proj_matrix[16], pos_matrix[16], vec_matrix[16], tex_matrix[16], clip_matrix[16];
    bool clip_matrix_dirty;
    uint32_t viewport[6];
    int32_t proj_matrix_stack[16], pos_matrix_stack[32][16], vec_matrix_stack[32][16], tex_matrix_stack[16];
    int32_t proj_matrix_stack_pointer, pos_matrix_stack_pointer, tex_matrix_stack_pointer;
    /* a word of packed commands under way at the FIFO's port */
    uint32_t num_commands, cur_command, param_count, total_params;
    uint32_t disp_cnt;
    uint8_t alpha_ref_val, alpha_ref;
    uint16_t toon_table[32], edge_table[8];
    uint32_t fog_color, fog_offset;
    uint8_t fog_density_table[32];
    uint32_t clear_attr1, clear_attr2;
    /* the registers as the frame being drawn latched them */
    uint32_t render_disp_cnt;
    uint8_t render_alpha_ref;
    uint16_t render_toon_table[32], render_edge_table[8];
    uint32_t render_fog_color, render_fog_offset, render_fog_shift;
    uint8_t render_fog_density_table[34];
    uint32_t render_clear_attr1, render_clear_attr2;
    bool render_frame_identical;
    uint32_t polygon_mode;
    int16_t cur_vertex[3];
    uint8_t vertex_color[3];
    int16_t tex_coords[2], raw_tex_coords[2], normal[3];
    int16_t light_direction[4][3];
    int32_t spec_recip[4];
    uint8_t light_color[4][3];
    uint8_t mat_diffuse[3], mat_ambient[3], mat_specular[3], mat_emission[3];
    bool use_shininess_table;
    uint8_t shininess_table[128];
    uint32_t polygon_attr, cur_polygon_attr, tex_param, tex_palette;
    int32_t pos_test_result[4];
    int16_t vec_test_result[3];
    NdsVertex temp_vertex_buffer[4];
    uint32_t vertex_num, vertex_num_in_poly, num_consecutive_polygons;
    NdsPolygon* last_strip_polygon;
    uint32_t num_opaque_polygons;
    NdsVertex vertex_ram[6144 * 2];
    NdsPolygon polygon_ram[2048 * 2];
    NdsVertex* cur_vertex_ram;
    NdsPolygon* cur_polygon_ram;
    uint32_t num_vertices, num_polygons, cur_ram_bank;
    NdsPolygon* render_polygon_ram[2048];
    uint32_t render_num_polygons;
    uint32_t flush_request, flush_attributes;
    /* the commands that came after SWAP_BUFFERS, which run once the buffers swapped at the vertical blank */
    NdsGxEntry* waiting;
    uint32_t waiting_count, waiting_capacity;
    uint64_t frames_swapped; /* the frames whose polygons were handed to the renderer */
} NdsGpu3D;
#endif
