/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/splash.h"
#include <lunasvg.h>
#include <plutovg.h>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

struct SnxSplashDrawing {
    std::unique_ptr<lunasvg::Document> document;
};

SnxSplashDrawing* snx_splash_open(const SnxSplashSvg* svg) {
    if (!svg) return nullptr;
    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(svg->text, svg->size);
    if (!document || document->width() <= 0 || document->height() <= 0) return nullptr;
    return new (std::nothrow) SnxSplashDrawing{std::move(document)};
}

int snx_splash_draw(const SnxSplashDrawing* drawing, int side, void* pixels, int stride) {
    if (!drawing || side <= 0 || !pixels || stride < side * 4) return 1;
    const lunasvg::Document& document = *drawing->document;
    lunasvg::Bitmap bitmap(static_cast<uint8_t*>(pixels), side, side, stride);
    bitmap.clear(0);
    /* as renderToBitmap scales it, into our own pixels */
    document.render(bitmap, lunasvg::Matrix(side / document.width(), 0, 0, side / document.height(), 0, 0));
    return 0;
}

void snx_splash_close(SnxSplashDrawing* drawing) { delete drawing; }

/* the drawing named name filling a side x side surface over white, as a picture with none is shown; NULL for none */
static plutovg_surface_t* over_white(const char* name, int side) {
    const SnxSplashSvg* svg = nullptr;
    for (int i = 0; name && i < snx_splash_svg_count; i++)
        if (!std::strcmp(snx_splash_svgs[i].name, name)) svg = &snx_splash_svgs[i];
    std::unique_ptr<SnxSplashDrawing> drawing(snx_splash_open(svg));
    if (!drawing || side <= 0) return nullptr;
    plutovg_surface_t* surface = plutovg_surface_create(side, side);
    if (!surface) return nullptr;
    const plutovg_color_t white = {1, 1, 1, 1};
    plutovg_surface_clear(surface, &white);
    const lunasvg::Document& document = *drawing->document;
    lunasvg::Bitmap bitmap(plutovg_surface_get_data(surface), side, side, plutovg_surface_get_stride(surface));
    document.render(bitmap, lunasvg::Matrix(side / document.width(), 0, 0, side / document.height(), 0, 0));
    return surface;
}

int snx_splash_jpeg(const char* name, int side, int quality, SnxSplashWrite write, void* closure) {
    if (!write) return 1;
    /* a JPEG has no alpha: over white */
    plutovg_surface_t* surface = over_white(name, side);
    if (!surface) return 1;
    const bool written = plutovg_surface_write_to_jpg_stream(surface, write, closure, quality);
    plutovg_surface_destroy(surface);
    return written ? 0 : 1;
}

int snx_splash_jpeg_rgba(const void* rgba, int width, int height, int quality, SnxSplashWrite write, void* closure) {
    if (!rgba || !write || width <= 0 || height <= 0) return 1;
    plutovg_surface_t* surface = plutovg_surface_create(width, height);
    if (!surface) return 1;
    /* plutovg's pixels are 32-bit words 0xAARRGGBB: opaque, the alpha a JPEG has no place for */
    const uint8_t* in = static_cast<const uint8_t*>(rgba);
    uint8_t* data = plutovg_surface_get_data(surface);
    const int stride = plutovg_surface_get_stride(surface);
    for (int y = 0; y < height; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(data + static_cast<size_t>(y) * stride);
        for (int x = 0; x < width; x++, in += 4)
            row[x] = 0xFF000000u | static_cast<uint32_t>(in[0]) << 16 | static_cast<uint32_t>(in[1]) << 8 | in[2];
    }
    const bool written = plutovg_surface_write_to_jpg_stream(surface, write, closure, quality);
    plutovg_surface_destroy(surface);
    return written ? 0 : 1;
}

int snx_splash_picture(const void* jpeg, size_t size, int side, int round, void* pixels, int stride) {
    if (side <= 0 || !pixels || stride < side * 4 || size > INT_MAX) return 1;
    plutovg_surface_t* source =
        jpeg ? plutovg_surface_load_from_image_data(jpeg, static_cast<int>(size)) : over_white("wimsy-fuddles", side);
    if (!source) return 1;
    for (int y = 0; y < side; y++)
        std::memset(static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * stride, 0, side * 4);
    plutovg_surface_t* target = plutovg_surface_create_for_data(static_cast<uint8_t*>(pixels), side, side, stride);
    plutovg_canvas_t* canvas = target ? plutovg_canvas_create(target) : nullptr;
    if (!canvas) {
        plutovg_surface_destroy(target);
        plutovg_surface_destroy(source);
        return 1;
    }
    /* its middle square, filling the side: a wide picture loses its sides, a tall one its top and bottom */
    const int w = plutovg_surface_get_width(source), h = plutovg_surface_get_height(source);
    const float scale = static_cast<float>(side) / static_cast<float>(w < h ? w : h), half = side / 2.0f;
    plutovg_matrix_t matrix;
    plutovg_matrix_init_translate(&matrix, (side - w * scale) / 2, (side - h * scale) / 2);
    plutovg_matrix_scale(&matrix, scale, scale);
    plutovg_canvas_set_texture(canvas, source, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &matrix);
    if (round)
        plutovg_canvas_circle(canvas, half, half, half);
    else
        plutovg_canvas_rect(canvas, 0, 0, static_cast<float>(side), static_cast<float>(side));
    plutovg_canvas_fill(canvas);
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(target);
    plutovg_surface_destroy(source);
    return 0;
}
