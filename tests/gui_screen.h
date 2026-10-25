/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the screen tests of the menu (gui_sdl.c, gui_setup.c) read off the real renderer, after including gui/main.c:
   text in a colour, glyph by glyph in the menu's font, and the mouse or a finger at a pixel of the render.
   SNX_SCREEN_TEST names the test in what a check that fails says */
#ifndef SNX_TEST_GUI_SCREEN_H
#define SNX_TEST_GUI_SCREEN_H
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* the 16 rows of the menu font's glyph c: CP437's, then the Latin letters past it, then the wide letters' halves
   (gui_menu_glyph); an icon's two (GUI_ICON_GLYPH on) are none of the font's, but lunasvg's (icon_coverage) */
static inline const uint8_t* font_rows(int c) {
    return c < 256                     ? gui_vga_font + c * 16
           : c < 256 + gui_latin_count ? gui_latin_font + (c - 256) * 16
                                       : gui_wide_font + (c - 256 - gui_latin_count) * 16;
}
/* the icons as a row's text holds them, UTF-8 to write beside a label ("- " ICON_SETTINGS " Settings"): icons_are
   checks each is its icons.h code point */
#define ICON_GAMEPAD_2           "\uE000"
#define ICON_SQUARE_TERMINAL     "\uE001"
#define ICON_BOX                 "\uE002"
#define ICON_PACKAGE             "\uE003"
#define ICON_GAMEPAD             "\uE004"
#define ICON_KEY_ROUND           "\uE005"
#define ICON_SETTINGS            "\uE006"
#define ICON_SHARE_2             "\uE007"
#define ICON_FOLDER_OPEN         "\uE008"
#define ICON_LOG_OUT             "\uE009"
#define ICON_APP_WINDOW          "\uE00A"
#define ICON_ROCKET              "\uE00B"
#define ICON_JOYSTICK            "\uE00C"
#define ICON_MONITOR             "\uE00D"
#define ICON_IMAGE               "\uE00E"
#define ICON_GAUGE               "\uE00F"
#define ICON_POINTER             "\uE010"
#define ICON_WIFI                "\uE011"
#define ICON_BUG                 "\uE012"
#define ICON_USER                "\uE013"
#define ICON_PAINTBRUSH          "\uE014"
#define ICON_FOLDER              "\uE015"
#define ICON_ERASER              "\uE016"
#define ICON_FOLDER_INPUT        "\uE017"
#define ICON_ROTATE_CCW          "\uE018"
#define ICON_CPU                 "\uE019"
#define ICON_LAYOUT_GRID         "\uE01A"
#define ICON_GAMEPAD_DIRECTIONAL "\uE01B"
#define ICON_FOLDER_PLUS         "\uE01C"
#define ICON_PACKAGE_PLUS        "\uE01D"
#define ICON_FILE_PLUS           "\uE01E"
#define ICON_SAVE                "\uE01F"
#define ICON_PLAY                "\uE020"
#define ICON_HAMMER              "\uE021"
#define ICON_PALETTE             "\uE022"
#define ICON_HARD_DRIVE_DOWNLOAD "\uE023"
#define ICON_ACTIVITY            "\uE024"
#define ICON_INFO                "\uE025"
static inline void is_icon(const char* text, unsigned icon) {
    const char* at = text;
    assert(gui_utf8_step(&at) == icon && !*at);
}
static inline void icons_are(void) {
    is_icon(ICON_GAMEPAD_2, GUI_ICON_GAMEPAD_2);
    is_icon(ICON_SQUARE_TERMINAL, GUI_ICON_SQUARE_TERMINAL);
    is_icon(ICON_BOX, GUI_ICON_BOX);
    is_icon(ICON_PACKAGE, GUI_ICON_PACKAGE);
    is_icon(ICON_GAMEPAD, GUI_ICON_GAMEPAD);
    is_icon(ICON_KEY_ROUND, GUI_ICON_KEY_ROUND);
    is_icon(ICON_SETTINGS, GUI_ICON_SETTINGS);
    is_icon(ICON_SHARE_2, GUI_ICON_SHARE_2);
    is_icon(ICON_FOLDER_OPEN, GUI_ICON_FOLDER_OPEN);
    is_icon(ICON_LOG_OUT, GUI_ICON_LOG_OUT);
    is_icon(ICON_APP_WINDOW, GUI_ICON_APP_WINDOW);
    is_icon(ICON_ROCKET, GUI_ICON_ROCKET);
    is_icon(ICON_JOYSTICK, GUI_ICON_JOYSTICK);
    is_icon(ICON_MONITOR, GUI_ICON_MONITOR);
    is_icon(ICON_IMAGE, GUI_ICON_IMAGE);
    is_icon(ICON_GAUGE, GUI_ICON_GAUGE);
    is_icon(ICON_POINTER, GUI_ICON_POINTER);
    is_icon(ICON_WIFI, GUI_ICON_WIFI);
    is_icon(ICON_BUG, GUI_ICON_BUG);
    is_icon(ICON_USER, GUI_ICON_USER);
    is_icon(ICON_PAINTBRUSH, GUI_ICON_PAINTBRUSH);
    is_icon(ICON_FOLDER, GUI_ICON_FOLDER);
    is_icon(ICON_ERASER, GUI_ICON_ERASER);
    is_icon(ICON_FOLDER_INPUT, GUI_ICON_FOLDER_INPUT);
    is_icon(ICON_ROTATE_CCW, GUI_ICON_ROTATE_CCW);
    is_icon(ICON_CPU, GUI_ICON_CPU);
    is_icon(ICON_LAYOUT_GRID, GUI_ICON_LAYOUT_GRID);
    is_icon(ICON_GAMEPAD_DIRECTIONAL, GUI_ICON_GAMEPAD_DIRECTIONAL);
    is_icon(ICON_FOLDER_PLUS, GUI_ICON_FOLDER_PLUS);
    is_icon(ICON_PACKAGE_PLUS, GUI_ICON_PACKAGE_PLUS);
    is_icon(ICON_FILE_PLUS, GUI_ICON_FILE_PLUS);
    is_icon(ICON_SAVE, GUI_ICON_SAVE);
    is_icon(ICON_PLAY, GUI_ICON_PLAY);
    is_icon(ICON_HAMMER, GUI_ICON_HAMMER);
    is_icon(ICON_PALETTE, GUI_ICON_PALETTE);
    is_icon(ICON_HARD_DRIVE_DOWNLOAD, GUI_ICON_HARD_DRIVE_DOWNLOAD);
    is_icon(ICON_ACTIVITY, GUI_ICON_ACTIVITY);
    is_icon(ICON_INFO, GUI_ICON_INFO);
}
/* where a font's pixel k (from a glyph's corner) is read with letters half halves of a cell big: a whole scale s (2 * s
   of them) its block's corner, k * s, so a glyph is found at its corner alone; a half step's pixels, a pixel and two by
   turns, in their middle */
static inline int font_px(int k, int half) { return half % 2 ? (2 * k + 1) * half / 4 : k * (half / 2); }
/* n glyphs in color anywhere on the render: their ink pixels in color, the others not (a box's frame may cross the
   font's last row, which is empty); 1 and where they start, else 0. Their letters size times the text's, or at the
   half step app->half names while it is set. An icon, two of them, is lunasvg's drawing a cell square, as menu_icon
   draws it: the pixels it covers whole in color, those it does not touch not, the ones between either, and as a
   glyph's its last font row's left out */
static inline int find_glyphs(App* app, const int* text, int n, SDL_Color color, int size, int* at_x, int* at_y) {
    SDL_Surface *read = SDL_RenderReadPixels(app->renderer, NULL), *image;
    int h = app->half && size == 1 ? app->half : 2 * app->scale * size, first = -1, fx = 0, fy = 0, found = 0, x, y;
    int side = 8 * h, high = font_px(14, h) + 1, wide = font_px(n * 8 - 1, h) + 1;
    uint8_t* cover[256] = {0}; /* each icon's coverage at side, at its first glyph */
    assert(read && n <= 256);
    image = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(read);
    assert(image);
#define SNX_AT(px, py) ((const Uint8*)image->pixels + (py) * image->pitch + (px) * 4)
#define SNX_IS(c)      ((c)[0] == color.r && (c)[1] == color.g && (c)[2] == color.b)
    for (int i = 0; i < n; ++i)
        if (text[i] >= GUI_ICON_GLYPH && (text[i] - GUI_ICON_GLYPH) % 2 == 0) {
            cover[i] = (uint8_t*)malloc((size_t)side * (size_t)side);
            assert(cover[i] && !menu_icon_alpha((text[i] - GUI_ICON_GLYPH) / 2, side, cover[i]));
            high = SDL_max(high, side);
            wide = SDL_max(wide, i * 4 * h + side);
        }
    for (int i = 0; i < n && first < 0; ++i) /* the first ink, to look for */
        if (cover[i]) {
            for (int k = 0; 2 * (k / side) < 15 * h && first < 0; ++k)
                if (cover[i][k] == 255) {
                    first = i;
                    fx = i * 4 * h + k % side;
                    fy = k / side;
                }
        } else if (text[i] < GUI_ICON_GLYPH)
            for (int gy = 0; gy < 15 && first < 0; ++gy)
                for (int gx = 0; gx < 8 && first < 0; ++gx)
                    if (font_rows(text[i])[gy] & (0x80 >> gx)) {
                        first = i;
                        fx = font_px(i * 8 + gx, h);
                        fy = font_px(gy, h);
                    }
    assert(first >= 0);
    for (y = 0; !found && y + high <= image->h; ++y)
        for (x = 0; !found && x + wide <= image->w; ++x) {
            if (!SNX_IS(SNX_AT(x + fx, y + fy))) continue;
            found = 1;
            for (int i = 0; found && i < n; ++i)
                if (cover[i]) {
                    for (int k = 0; found && 2 * (k / side) < 15 * h; ++k)
                        if (cover[i][k] == 0 || cover[i][k] == 255)
                            found = (cover[i][k] == 255) == SNX_IS(SNX_AT(x + i * 4 * h + k % side, y + k / side));
                } else if (text[i] < GUI_ICON_GLYPH)
                    for (int gy = 0; found && gy < 15; ++gy)
                        for (int gx = 0; found && gx < 8; ++gx) {
                            int ink = (font_rows(text[i])[gy] & (0x80 >> gx)) != 0;
                            found = ink == SNX_IS(SNX_AT(x + font_px(i * 8 + gx, h), y + font_px(gy, h)));
                        }
            if (found) {
                *at_x = x;
                *at_y = y;
            }
        }
#undef SNX_IS
#undef SNX_AT
    for (int i = 0; i < n; ++i) free(cover[i]);
    SDL_DestroySurface(image);
    return found;
}
/* text in color, glyph by glyph in the font's own bytes (CP437) */
static inline int find_text(App* app, const char* text, SDL_Color color, int size, int* at_x, int* at_y) {
    int glyphs[256], n = 0;
    while (*text) { /* a byte each, CP437's; a character of two to four bytes as the menu draws it (an icon) */
        const char* was = text;
        uint32_t point = gui_utf8_step(&text);
        int c = (unsigned char)*was < 0x80 || text == was + 1 ? (unsigned char)*was : gui_menu_glyph(point);
        assert(n + 2 <= 256);
        glyphs[n++] = c;
        if (gui_glyph_cols(c) == 2) glyphs[n++] = c + 1;
    }
    return find_glyphs(app, glyphs, n, color, size, at_x, at_y);
}
/* UTF-8 text in color, a glyph each letter as the menu draws it (gui_menu_glyph; a wide one's two halves), size times
   the text's letters */
static inline int find_letters(App* app, const char* text, SDL_Color color, int size, int* at_x, int* at_y) {
    int glyphs[256], n = 0;
    while (*text) {
        int c = gui_menu_glyph(gui_utf8_step(&text));
        assert(n + 2 <= 256);
        glyphs[n++] = c;
        if (gui_glyph_cols(c) == 2) glyphs[n++] = c + 1;
    }
    return find_glyphs(app, glyphs, n, color, size, at_x, at_y);
}
/* the text drawn, or not, in color, its letters size times the text's: a check that fails names its line */
#define shows(app, text, color)           shows_at(__LINE__, app, text, color, 1, 1)
#define hides(app, text, color)           shows_at(__LINE__, app, text, color, 1, 0)
#define shows_big(app, text, color, size) shows_at(__LINE__, app, text, color, size, 1)
static inline void shows_at(int line, App* app, const char* text, SDL_Color color, int size, int drawn) {
    int x, y;
    if (find_text(app, text, color, size, &x, &y) == drawn) return;
    if (drawn)
        fprintf(stderr, "%s:%d: \"%s\" is not drawn in %02x%02x%02x\n", SNX_SCREEN_TEST, line, text, color.r, color.g,
                color.b);
    else
        fprintf(stderr, "%s:%d: \"%s\" is drawn in %02x%02x%02x at %d,%d\n", SNX_SCREEN_TEST, line, text, color.r,
                color.g, color.b, x, y);
    if (getenv("SNX_GUI_FAILED_SHOT")) { /* what was drawn, to look at */
        SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
        if (image) SDL_SaveBMP(image, getenv("SNX_GUI_FAILED_SHOT"));
        SDL_DestroySurface(image);
    }
    assert(!"the text drawn");
}
/* the same a half step under the text's letters (text_half: the tips under the head, a note, the Status box) */
#define shows_half(app, text, color) half_shows_at(__LINE__, app, text, color, 1)
#define hides_half(app, text, color) half_shows_at(__LINE__, app, text, color, 0)
static inline void half_shows_at(int line, App* app, const char* text, SDL_Color color, int drawn) {
    int half = text_half(app);
    shows_at(line, app, text, color, 1, drawn);
    app->half = half;
}
/* the same in the foot's letters, a size under the rest's (foot_scale) */
#define shows_foot(app, text, color) foot_shows_at(__LINE__, app, text, color, 1)
#define hides_foot(app, text, color) foot_shows_at(__LINE__, app, text, color, 0)
static inline void foot_shows_at(int line, App* app, const char* text, SDL_Color color, int drawn) {
    int scale = app->scale;
    app->scale = foot_scale(app);
    shows_at(line, app, text, color, 1, drawn);
    app->scale = scale;
}
/* the mouse at pixel x, y of the render, moved there or clicking */
static inline void mouse_px(App* app, int x, int y, bool click) {
    SDL_Event e = {0};
    float wx, wy;
    layout(app);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.type = click ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_MOTION;
    e.button.button = SDL_BUTTON_LEFT;
    if (click) {
        e.button.x = wx;
        e.button.y = wy;
    } else {
        e.motion.x = wx;
        e.motion.y = wy;
    }
    event(app, &e);
}
/* a finger tapping pixel x, y of the render */
static inline void tap_px(App* app, int x, int y) {
    SDL_Event e = {0};
    float wx, wy;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.tfinger.fingerID = 9;
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    e.type = SDL_EVENT_FINGER_DOWN;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    event(app, &e);
}
static inline void point(App* app, GuiRect b, bool click) { mouse_px(app, b.x + b.w / 2, b.y + b.h / 2, click); }
/* a row of a scene of ui.txt on a computer, as high as ui.txt's row_height of 17px: a cell and a pixel */
static inline int item_h(const App* app) { return cell_h(app) + app->scale; }
/* choice i's box in the dialog placed (ui.txt's scene Choices), as drawn: its row past the cursor's margin; none while it
   is scrolled out */
static inline GuiRect choice_box(const App* app, const GuiChoices* c, int i) {
    size_t first = c->lists[UI_CHOICES_ROWS].first;
    if (i < 0 || (size_t)i < first || (size_t)i >= first + ui_choices_shown(c->rects, app->scale, UI_CHOICES_ROWS))
        return gui_rect(0, 0, 0, 0);
    return ui_row_box(app, ui_choices_row(c->rects, app->scale, UI_CHOICES_ROWS, (size_t)i - first));
}
#endif
