/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Fonts and shaping for android.graphics (text.h). Each font keeps the face's units per em as its scale, and each
   call scales to its size, so one hb_font_t serves every size. Text goes to HarfBuzz as UTF-16 in the host's byte
   order, which is a jchar's */
#include "android/hwui/text.h"
#include "android/runtime/jvm.h"
#include <hb-ot.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the fonts: a handle is an index + 1. Text.loadFont is a synchronized method, so one thread adds at a time, and a
   font never goes away */
enum { MAX_FONTS = 256 };
static struct {
    hb_font_t* fonts[MAX_FONTS];
    float units[MAX_FONTS]; /* units per em */
    int16_t y_min[MAX_FONTS], y_max[MAX_FONTS];
    jint count;
} text;

hb_font_t* android_text_font(jint handle) {
    return handle > 0 && handle <= text.count ? text.fonts[handle - 1] : NULL;
}

/* the big-endian int16 at off of a table, 0 past its end (OpenType tables are big-endian) */
static int16_t table_i16(hb_blob_t* blob, unsigned off) {
    unsigned length;
    const unsigned char* data = (const unsigned char*)hb_blob_get_data(blob, &length);
    if (!data || off + 2 > length) return 0;
    return (int16_t)(uint16_t)((unsigned)data[off] << 8 | data[off + 1]);
}

static jint JNICALL load_font(JNIEnv* env, jclass unused, jstring path, jint index) {
    (void)unused;
    if (text.count == MAX_FONTS) return 0;
    const char* name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return 0;
    hb_blob_t* blob = hb_blob_create_from_file_or_fail(name);
    (*env)->ReleaseStringUTFChars(env, path, name);
    if (!blob) return 0;
    hb_face_t* face = hb_face_create(blob, (unsigned)index);
    hb_blob_destroy(blob);
    if (hb_face_get_glyph_count(face) == 0) {
        hb_face_destroy(face);
        return 0;
    }
    hb_font_t* font = hb_font_create(face);
    unsigned upem = hb_face_get_upem(face);
    hb_font_set_scale(font, (int)upem, (int)upem);
    /* head's yMin and yMax (at 38 and 42): the font's top and bottom */
    hb_blob_t* head = hb_face_reference_table(face, HB_TAG('h', 'e', 'a', 'd'));
    jint i = text.count;
    text.y_min[i] = table_i16(head, 38);
    text.y_max[i] = table_i16(head, 42);
    hb_blob_destroy(head);
    hb_face_destroy(face);
    text.units[i] = (float)upem;
    text.fonts[i] = font;
    text.count = i + 1;
    return i + 1;
}

static float metric(hb_font_t* font, hb_ot_metrics_tag_t tag, float fallback) {
    hb_position_t v;
    return hb_ot_metrics_get_position(font, tag, &v) ? (float)v : fallback;
}

/* Android's font metrics, y down: ascent, descent, leading, top, bottom, underline position, underline thickness,
   x-height, cap height */
static void JNICALL metrics(JNIEnv* env, jclass unused, jint handle, jfloat size, jfloatArray out) {
    (void)unused;
    hb_font_t* font = android_text_font(handle);
    if (!font) return;
    jint i = handle - 1;
    float units = text.units[i], scale = size / units;
    float ascender = metric(font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, units * 0.8f);
    float descender = metric(font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, units * -0.2f);
    jfloat m[9] = {
        -ascender * scale,
        -descender * scale,
        metric(font, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, 0) * scale,
        -(float)text.y_max[i] * scale,
        -(float)text.y_min[i] * scale,
        -metric(font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, units * -0.1f) * scale,
        metric(font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, units * 0.05f) * scale,
        metric(font, HB_OT_METRICS_TAG_X_HEIGHT, units * 0.5f) * scale,
        metric(font, HB_OT_METRICS_TAG_CAP_HEIGHT, units * 0.7f) * scale,
    };
    (*env)->SetFloatArrayRegion(env, out, 0, 9, m);
}

static jint JNICALL shape(JNIEnv* env, jclass unused, jint handle, jfloat size, jfloat scale_x, jfloat letter_spacing,
                          jcharArray chars, jint start, jint count, jint context_start, jint context_end, jboolean rtl,
                          jintArray glyphs_out, jfloatArray positions_out, jfloatArray advances_out,
                          jfloatArray total_out) {
    (void)unused;
    hb_font_t* font = android_text_font(handle);
    if (!font || count <= 0 || context_start > start || context_end < start + count) {
        jfloat zero = 0;
        if (total_out) (*env)->SetFloatArrayRegion(env, total_out, 0, 1, &zero);
        return 0;
    }
    jint context = context_end - context_start;
    jchar* units = malloc((size_t)context * sizeof *units);
    hb_buffer_t* buffer = hb_buffer_create();
    if (!units || !hb_buffer_allocation_successful(buffer)) {
        free(units);
        hb_buffer_destroy(buffer);
        return 0;
    }
    (*env)->GetCharArrayRegion(env, chars, context_start, context, units);
    if ((*env)->ExceptionCheck(env)) {
        free(units);
        hb_buffer_destroy(buffer);
        return 0;
    }
    hb_buffer_add_utf16(buffer, units, context, (unsigned)(start - context_start), count);
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, NULL, 0);
    unsigned n;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &n);
    float scale = size / text.units[handle - 1], spacing = letter_spacing * size;

    jint* glyphs = malloc(((size_t)n + 1) * sizeof *glyphs);
    jfloat* positions = malloc(((size_t)n * 2 + 1) * sizeof *positions);
    jfloat* advances = calloc((size_t)count + 1, sizeof *advances);
    if (!glyphs || !positions || !advances) {
        free(glyphs);
        free(positions);
        free(advances);
        free(units);
        hb_buffer_destroy(buffer);
        return 0;
    }
    /* each cluster's advance on its first char, and letter spacing after each cluster, as minikin adds it */
    float x = 0;
    for (unsigned g = 0; g < n; ++g) {
        glyphs[g] = (jint)info[g].codepoint;
        positions[2 * g] = x + (float)pos[g].x_offset * scale * scale_x;
        positions[2 * g + 1] = -(float)pos[g].y_offset * scale;
        float advance = (float)pos[g].x_advance * scale * scale_x;
        bool last_of_cluster = g + 1 == n || info[g + 1].cluster != info[g].cluster;
        if (last_of_cluster) advance += spacing;
        x += advance;
        jint c = (jint)info[g].cluster - (start - context_start);
        if (c >= 0 && c < count) advances[c] += advance;
    }
    jfloat total = x;
    if (glyphs_out && (*env)->GetArrayLength(env, glyphs_out) >= (jsize)n)
        (*env)->SetIntArrayRegion(env, glyphs_out, 0, (jsize)n, glyphs);
    if (positions_out && (*env)->GetArrayLength(env, positions_out) >= (jsize)(2 * n))
        (*env)->SetFloatArrayRegion(env, positions_out, 0, (jsize)(2 * n), positions);
    if (advances_out && (*env)->GetArrayLength(env, advances_out) >= count)
        (*env)->SetFloatArrayRegion(env, advances_out, 0, count, advances);
    if (total_out) (*env)->SetFloatArrayRegion(env, total_out, 0, 1, &total);
    free(glyphs);
    free(positions);
    free(advances);
    free(units);
    hb_buffer_destroy(buffer);
    return (jint)n;
}

static jboolean JNICALL has_glyph(JNIEnv* env, jclass unused, jint handle, jint code_point) {
    (void)env;
    (void)unused;
    hb_font_t* font = android_text_font(handle);
    hb_codepoint_t glyph;
    return font && code_point >= 0 && hb_font_get_nominal_glyph(font, (hb_codepoint_t)code_point, &glyph) ? JNI_TRUE
                                                                                                       : JNI_FALSE;
}

/* the union of the glyphs' ink boxes, y down; all zero for none */
static void JNICALL bounds(JNIEnv* env, jclass unused, jint handle, jfloat size, jintArray glyphs_in,
                           jfloatArray positions_in, jint count, jfloatArray out) {
    (void)unused;
    hb_font_t* font = android_text_font(handle);
    jfloat box[4] = {0, 0, 0, 0};
    if (font && count > 0) {
        jint* glyphs = (*env)->GetIntArrayElements(env, glyphs_in, NULL);
        jfloat* positions = (*env)->GetFloatArrayElements(env, positions_in, NULL);
        float scale = size / text.units[handle - 1];
        bool any = false;
        for (jint g = 0; glyphs && positions && g < count; ++g) {
            hb_glyph_extents_t e;
            if (!hb_font_get_glyph_extents(font, (hb_codepoint_t)glyphs[g], &e) || !e.width || !e.height) continue;
            float left = positions[2 * g] + (float)e.x_bearing * scale;
            float top = positions[2 * g + 1] - (float)e.y_bearing * scale;
            float right = left + (float)e.width * scale, bottom = top - (float)e.height * scale;
            if (!any || left < box[0]) box[0] = left;
            if (!any || top < box[1]) box[1] = top;
            if (!any || right > box[2]) box[2] = right;
            if (!any || bottom > box[3]) box[3] = bottom;
            any = true;
        }
        if (glyphs) (*env)->ReleaseIntArrayElements(env, glyphs_in, glyphs, JNI_ABORT);
        if (positions) (*env)->ReleaseFloatArrayElements(env, positions_in, positions, JNI_ABORT);
    }
    (*env)->SetFloatArrayRegion(env, out, 0, 4, box);
}

bool android_text_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"loadFont", (char*)"(Ljava/lang/String;I)I", (void*)load_font},
        {(char*)"metrics", (char*)"(IF[F)V", (void*)metrics},
        {(char*)"shape", (char*)"(IFFF[CIIIIZ[I[F[F[F)I", (void*)shape},
        {(char*)"hasGlyph", (char*)"(II)Z", (void*)has_glyph},
        {(char*)"bounds", (char*)"(IF[I[FI[F)V", (void*)bounds},
    };
    return android_jvm_register(env, "snx/android/hwui/Text", methods, (int)(sizeof methods / sizeof *methods));
}
