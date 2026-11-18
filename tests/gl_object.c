/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared GL layer's buffers and vertex arrays (src/common/gpu/gl/gl_object.c), with no GPU and the platform's
   memory a few chunks of the test's: buffers made, bound and given bytes (kept where a smaller size fits, moved and
   the old range freed for the next when a larger one does not), written in part, read back, copied, mapped (a
   persistent map only of storage that allows it, an explicit flush telling exactly its range), immutable storage
   refusing what GL refuses, deleted with their bindings; what the platform hears (the GPU's writes asked for before
   the CPU touches the bytes, the bytes written, a buffer gone); free ranges merged and a second chunk asked when the
   first is full; vertex arrays and their attributes, the generic attributes, glGet's bindings; and a save state's
   copy loaded back whole and refused when a buffer, a free range or a vertex array in it is wrong */
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ARRAY = 0x8892,
    ELEMENTS = 0x8893,
    COPY_READ = 0x8f36,
    COPY_WRITE = 0x8f37,
    MAP_READ = 0x1,
    MAP_WRITE = 0x2,
    MAP_FLUSH_EXPLICIT = 0x10,
    MAP_PERSISTENT = 0x40,
    MAP_COHERENT = 0x80,
    DYNAMIC_STORAGE = 0x100,
    BUFFER_SIZE = 0x8764,
    BUFFER_USAGE = 0x8765,
    STREAM_DRAW = 0x88e0,
    CHUNK = 16u << 20,
};

/* the platform: its chunks, and what it heard */
static struct {
    unsigned chunks, gone, read_back;
    uint32_t written_name;
    uint64_t written_lo, written_hi;
} heard;
static uint64_t chunk(uint64_t size, uint64_t* got) {
    void* p = malloc((size_t)size);
    heard.chunks++;
    *got = p ? size : 0;
    return (uint64_t)(uintptr_t)p;
}
static uint8_t* bytes(uint64_t at) { return (uint8_t*)(uintptr_t)at; }
static void read_back(uint32_t name) {
    (void)name;
    heard.read_back++;
}
static void written(uint32_t name, uint64_t lo, uint64_t hi) {
    heard.written_name = name, heard.written_lo = lo, heard.written_hi = hi;
}
static void gone(uint32_t name) {
    (void)name;
    heard.gone++;
}
static void was_written(uint32_t name, uint64_t lo, uint64_t hi) {
    assert(heard.written_name == name && heard.written_lo == lo && heard.written_hi == hi);
    heard.written_name = 0;
}
static int64_t parameter(uint32_t ctx, uint32_t name, uint32_t pname) {
    int64_t v = -1;
    assert(snx_gl_buffer_parameter(ctx, name, pname, &v));
    return v;
}
static void refused(const void* copy, size_t size, const char* text) {
    char why[256];
    assert(snx_gl_state_check(copy, size, why, sizeof why) < 0);
    if (!strstr(why, text)) {
        fprintf(stderr, "gl_object: refused for \"%s\", not naming \"%s\"\n", why, text);
        abort();
    }
}

int main(void) {
    uint32_t ctx = snx_gl_context_new(SNX_GL_API_DESKTOP, 45, 0), names[4] = {0};
    unsigned char pattern[300];
    double v[4];
    assert(ctx);
    snx_gl_object_host(&(SnxGlObjectHost){chunk, bytes, read_back, written, gone});
    for (int i = 0; i < 300; i++) pattern[i] = (unsigned char)i;

    /* names, binds, and the errors of a target GL has not and of none bound */
    snx_gl_gen_buffers(ctx, 3, names);
    assert(names[0] == 1 && names[1] == 2 && names[2] == 3 && snx_gl_is_buffer(2) && !snx_gl_is_buffer(4));
    snx_gl_bind_buffer(ctx, ARRAY, 1);
    assert(snx_gl_bound_buffer(ctx, ARRAY) == 1 && snx_gl_get(ctx, 0x8894, v) == 1 && v[0] == 1);
    assert(!snx_gl_bound_buffer(ctx, 0x1234) && snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    assert(!snx_gl_bound_buffer(ctx, COPY_READ) && snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);

    /* bytes: given, written in part (the GPU's writes asked for first), outside refused */
    snx_gl_buffer_data(ctx, 1, 100, pattern, STREAM_DRAW);
    was_written(1, 0, 100);
    assert(heard.chunks == 1 && parameter(ctx, 1, BUFFER_SIZE) == 100 &&
           parameter(ctx, 1, BUFFER_USAGE) == STREAM_DRAW);
    uint64_t first = snx_gl_buffer(1)->data;
    assert(first && !memcmp(bytes(first), pattern, 100));
    unsigned asked = heard.read_back;
    snx_gl_buffer_sub_data(ctx, 1, 10, 5, pattern + 200);
    was_written(1, 10, 15);
    assert(heard.read_back == asked + 1 && !memcmp(bytes(first) + 10, pattern + 200, 5));
    snx_gl_buffer_sub_data(ctx, 1, 98, 5, pattern);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    {
        unsigned char back[5];
        snx_gl_get_buffer_sub_data(ctx, 1, 10, 5, back);
        assert(!memcmp(back, pattern + 200, 5));
    }
    /* a smaller size stays where it is; a larger one moves, the old range taken by the next buffer */
    snx_gl_buffer_data(ctx, 1, 50, NULL, STREAM_DRAW);
    assert(snx_gl_buffer(1)->data == first && !heard.gone && bytes(first)[0] == 0);
    snx_gl_buffer_data(ctx, 1, 1000, NULL, STREAM_DRAW);
    assert(snx_gl_buffer(1)->data != first && heard.gone == 1 && snx_gl_buffer(1)->capacity >= 2000);
    snx_gl_bind_buffer(ctx, COPY_READ, 2);
    snx_gl_buffer_data(ctx, 2, 100, pattern, STREAM_DRAW); /* 256 bytes, where buffer 1 was */
    assert(snx_gl_buffer(2)->data == first);

    /* copies: the CPU's bytes, and the write told */
    snx_gl_copy_buffer_sub_data(ctx, 2, 1, 4, 100, 8);
    was_written(1, 100, 108);
    assert(!memcmp(bytes(snx_gl_buffer(1)->data) + 100, pattern + 4, 8));

    /* maps: a range of the bytes; an explicit flush tells its range, and the unmap nothing more */
    {
        uint64_t at = snx_gl_map_buffer_range(ctx, 1, 16, 32, MAP_WRITE | MAP_FLUSH_EXPLICIT);
        assert(at == snx_gl_buffer(1)->data + 16 && snx_gl_buffer_pointer(ctx, 1) == at);
        assert(!snx_gl_map_buffer_range(ctx, 1, 0, 4, MAP_READ) && snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        snx_gl_flush_mapped_range(ctx, 1, 8, 4);
        was_written(1, 24, 28);
        assert(snx_gl_unmap_buffer(ctx, 1) && heard.written_name == 0);
        assert(!snx_gl_unmap_buffer(ctx, 1) && snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        assert(!snx_gl_map_buffer_range(ctx, 1, 0, 4, MAP_WRITE | MAP_PERSISTENT)); /* mutable: no persistent map */
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
    }
    /* immutable storage: persistent maps where its flags allow, glBufferData refused, glBufferSubData only dynamic */
    {
        uint32_t s = names[2];
        snx_gl_buffer_storage(ctx, s, 4096, NULL, MAP_WRITE | MAP_PERSISTENT | MAP_COHERENT);
        uint64_t at = snx_gl_map_buffer_range(ctx, s, 0, 4096, MAP_WRITE | MAP_PERSISTENT | MAP_COHERENT);
        assert(at && at == snx_gl_buffer(s)->data && snx_gl_buffer(s)->immutable);
        memcpy(bytes(at), "persistent", 11);
        assert(!strcmp((const char*)snx_gl_buffer_bytes(s), "persistent"));
        snx_gl_buffer_data(ctx, s, 16, NULL, STREAM_DRAW);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        snx_gl_buffer_sub_data(ctx, s, 0, 4, pattern);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        snx_gl_buffer_storage(ctx, s, 64, NULL, DYNAMIC_STORAGE);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        assert(snx_gl_unmap_buffer(ctx, s));
        was_written(s, 0, 4096);
        snx_gl_buffer_data(ctx, s, 16, NULL, STREAM_DRAW); /* unmapped, still immutable */
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
    }

    /* the memory: a buffer deleted frees its range for the next, neighbours merged; a full chunk asks for another */
    {
        uint32_t b[3];
        snx_gl_gen_buffers(ctx, 3, b);
        for (int i = 0; i < 3; i++) {
            snx_gl_bind_buffer(ctx, COPY_WRITE, b[i]);
            snx_gl_buffer_data(ctx, b[i], 100, NULL, STREAM_DRAW); /* 256 bytes each, side by side */
        }
        uint64_t a0 = snx_gl_buffer(b[0])->data;
        assert(snx_gl_buffer(b[1])->data == a0 + 256 && snx_gl_buffer(b[2])->data == a0 + 512);
        unsigned went = heard.gone;
        snx_gl_bind_buffer(ctx, ARRAY, b[1]);
        snx_gl_vertex_attrib_pointer(ctx, 5, 4, 0x1406, 0, 0, 0, 0); /* reading b[1] */
        snx_gl_delete_buffers(ctx, 2, b);
        assert(heard.gone == went + 2 && !snx_gl_is_buffer(b[0]) && snx_gl_get(ctx, 0x8f37, v) == 1 && v[0] == b[2]);
        assert(snx_gl_get(ctx, 0x8894, v) == 1 && v[0] == 0); /* its bindings gone with it */
        assert(snx_gl_get_vertex_attrib(ctx, 5, 0x889f, v) == 1 && v[0] == 0);
        snx_gl_gen_buffers(ctx, 1, b);
        snx_gl_bind_buffer(ctx, COPY_WRITE, b[0]);
        snx_gl_buffer_data(ctx, b[0], 256, NULL, STREAM_DRAW); /* 512 bytes: the two ranges merged */
        assert(snx_gl_buffer(b[0])->data == a0);
        snx_gl_buffer_data(ctx, b[0], CHUNK, NULL, STREAM_DRAW); /* past the first chunk's room */
        assert(heard.chunks == 2 && snx_gl_buffer(b[0])->data);
    }

    /* vertex arrays: an attribute reads the array buffer bound; a deleted array leaves the context's own bound */
    {
        uint32_t a[2];
        snx_gl_gen_vertex_arrays(ctx, 2, a);
        snx_gl_bind_vertex_array(ctx, a[1]);
        assert(snx_gl_get(ctx, 0x85b5, v) == 1 && v[0] == a[1]);
        snx_gl_bind_buffer(ctx, ARRAY, 1);
        snx_gl_vertex_attrib_pointer(ctx, 3, 2, 0x1406, 0, 0, 0, 64);
        snx_gl_enable_vertex_attrib(ctx, 3, 1);
        assert(snx_gl_get_vertex_attrib(ctx, 3, 0x8622, v) == 1 && v[0] == 1);
        assert(snx_gl_get_vertex_attrib(ctx, 3, 0x889f, v) == 1 && v[0] == 1);
        assert(snx_gl_vertex_array(ctx)->bindings[3].offset == 64 && snx_gl_vertex_array(ctx)->bindings[3].stride == 8);
        snx_gl_vertex_attrib_pointer(ctx, 3, 5, 0x1406, 0, 0, 0, 0);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
        snx_gl_bind_buffer(ctx, ELEMENTS, 2);
        assert(snx_gl_get(ctx, 0x8895, v) == 1 && v[0] == 2);
        snx_gl_bind_vertex_array(ctx, 99);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        {
            const float xy[2] = {0.5f, 0.25f};
            snx_gl_vertex_attrib(ctx, 7, xy, 2, 3);
            assert(snx_gl_get_vertex_attrib(ctx, 7, 0x8626, v) == 4);
            assert(v[0] == 0.5 && v[1] == 0.25 && v[2] == 0 && v[3] == 1);
        }
    }

    /* the platform's memory at an alignment: the bytes before it stay free for the next take; one not a power of 2 is
       refused */
    {
        uint64_t a = snx_gl_memory_take(300, 256), b = snx_gl_memory_take(1000, 4096), c = snx_gl_memory_take(100, 256);
        assert(a && b && !(b % 4096) && c && c < b && c > a);
        assert(!snx_gl_memory_take(100, 768));
        snx_gl_memory_give(a, 300), snx_gl_memory_give(b, 1000), snx_gl_memory_give(c, 100);
    }

    /* a save state's copy: loaded back whole, the platform told every buffer's copy is stale */
    size_t total = snx_gl_state_bytes();
    unsigned char *saved = malloc(total), *copy = malloc(total), *after = malloc(total);
    char why[256];
    assert(saved && copy && after);
    snx_gl_state_save(saved);
    assert(!snx_gl_state_check(saved, total, why, sizeof why));
    {
        unsigned went = heard.gone;
        uint32_t one = 1;
        snx_gl_delete_buffers(ctx, 1, &one);
        snx_gl_enable_vertex_attrib(ctx, 3, 0);
        assert(!snx_gl_state_load(saved, total));
        assert(heard.gone >= went + 1 + SNX_GL_BUFFERS);
        snx_gl_state_save(after);
        assert(!memcmp(saved, after, total));
        assert(snx_gl_is_buffer(1) && snx_gl_bound_buffer(ctx, ARRAY) == 1);
        assert(snx_gl_get_vertex_attrib(ctx, 3, 0x8622, v) == 1 && v[0] == 1);
    }
    /* refused copies, each naming what it found */
    {
        size_t objects = 16 + (size_t)SNX_GL_CONTEXTS * sizeof(SnxGlContext) + snx_gl_program_state_bytes() + 8;
        size_t arrays = objects + SNX_GL_BUFFERS * sizeof(SnxGlBuffer);
        size_t contexts = arrays + SNX_GL_VERTEX_ARRAYS * sizeof(SnxGlVertexArray);
        size_t chunks = contexts + SNX_GL_CONTEXTS * sizeof(SnxGlObjectContext);
        size_t frees = chunks + SNX_GL_CHUNKS * 16;
        SnxGlBuffer b;
        SnxGlVertexArray a;
        uint64_t range[2];
        memcpy(copy, saved, total);
        memcpy(&b, copy + objects, sizeof b); /* buffer 1 */
        b.data = 0x10;
        memcpy(copy + objects, &b, sizeof b);
        refused(copy, total, "buffer 1 has");
        memcpy(copy, saved, total);
        memcpy(&b, copy + objects, sizeof b);
        b.size = b.capacity + 1;
        memcpy(copy + objects, &b, sizeof b);
        refused(copy, total, "outside its memory");
        memcpy(copy, saved, total);
        memcpy(range, copy + frees, sizeof range);
        range[0] = 0x10; /* in no chunk */
        memcpy(copy + frees, range, sizeof range);
        refused(copy, total, "free range 0 at 10+");
        memcpy(copy, saved, total);
        memcpy(range, copy + frees, sizeof range);
        range[0] -= 256; /* over the buffer below it */
        memcpy(copy + frees, range, sizeof range);
        refused(copy, total, "overlaps free range 0");
        memcpy(copy, saved, total);
        memcpy(&a, copy + arrays + 1 * sizeof a, sizeof a); /* array 2, the one bound */
        a.attribs[3].binding = 99;
        memcpy(copy + arrays + 1 * sizeof a, &a, sizeof a);
        refused(copy, total, "has attribute 3 at binding 99");
        refused(saved, total - 8, "this build reads");
    }
    free(saved);
    free(copy);
    free(after);
    puts("gl_object: ok");
    return 0;
}
