/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the actual renderer and SDL event handlers without production test flags. */
#define SNX_GUI_NO_MAIN
#include "gui/main.c"
#define SNX_SCREEN_TEST "gui_sdl.c"
#include "gui_screen.h"
#include "common/cpu/aarch64/cpu.h" /* SNX_ABI, which a save state names */
#include <assert.h>
#ifdef _WIN32
#include "win32_crash.h"
#include <windows.h> /* the queue's recompile's priority class */
#else
#include <errno.h>
#include <sys/resource.h> /* the queue's recompile's priority */
#include <sys/stat.h>     /* a mod's file it may not read */
#include <sys/statvfs.h>  /* Folders on a disk mounted noexec */
#include <unistd.h>       /* a cheats folder it may not write */
#endif
/* app.ini as the menu writes it: its head, and the Games options' lines that end [app], at their defaults */
#define APP_INI_HEAD "; Settings > App and Games' View in the launcher, as they were left\n[app]\n"
#define GAMES_DEFAULTS                                                                                                 \
    "recent_games = on\ngames_dumps = on\ngames_sort = title\ngames_view = list\ngames_icons = medium\n"
/* the default theme's colours, as the checks below name them */
static const SDL_Color normal = {220, 220, 220, 255}, yellow = {255, 255, 0, 255}, red = {255, 0, 0, 255},
                       pink = {255, 85, 255, 255}, black = {0, 0, 0, 255};

/* the question's box and its choices, as the theme places them (ask_place) */
static void ask_boxes(App* app, GuiRect* r, GuiChoices* choices) {
    ask_place(app, choices);
    *r = choices->box;
}
static void key(App* app, SDL_Keycode code) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
/* a finger down at pixel x, y of the render and lifted at to_x, to_y, as SDL gives it: 0 to 1 across the window */
static void swipe_px(App* app, int x, int y, int to_x, int to_y) {
    SDL_Event e = {0};
    float wx, wy;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    e.tfinger.fingerID = 7;
    e.type = SDL_EVENT_FINGER_DOWN;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)to_x + 0.5f, (float)to_y + 0.5f, &wx, &wy));
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    event(app, &e);
}
/* a key with a modifier held, and text as a keyboard types it */
static void key_with(App* app, SDL_Keycode code, SDL_Keymod mod) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.mod = mod;
    event(app, &e);
}
static void typed(App* app, const char* text) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    event(app, &e);
}
/* what the system's picker answers, from its own thread */
static void pick(App* app, const char* uri) {
    SDL_Event e = {0};
    e.type = app->picked;
    e.user.data1 = uri ? SDL_strdup(uri) : NULL;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void colored_text_on(App* app, int col, int row, const char* prompt, SDL_Color color, SDL_Color back) {
    /* Verify the exact text, its glyph color and what is behind it in the real renderer. */
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; prompt[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                assert(SDL_ReadSurfacePixel(image, ((i + col) * 8 + x) * app->scale, (row * 16 + y) * app->scale, &r,
                                            &g, &b, &a));
                bool ink = (gui_vga_font[(unsigned char)prompt[i] * 16 + y] & (0x80 >> x)) != 0;
                SDL_Color want = ink ? color : back;
                assert(r == want.r && g == want.g && b == want.b);
            }
    SDL_DestroySurface(image);
}
static SDL_Color pixel(App* app, float x, float y) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    Uint8 r = 0, g = 0, b = 0, a = 0; /* mingw's assert returns, as far as the compiler knows */
    SDL_Color out;
    assert(image && SDL_ReadSurfacePixel(image, (int)x, (int)y, &r, &g, &b, &a));
    SDL_DestroySurface(image);
    out.r = r;
    out.g = g;
    out.b = b;
    out.a = 255;
    return out;
}
static int same_colour(SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
/* the menu's item i's box, as drawn (its row past the cursor); none while it is scrolled out */
static GuiRect menu_entry_box(const App* app, const GuiMenuBoxes* m, int item) {
    size_t first = m->lists[UI_MENU_ENTRIES].first;
    if ((size_t)item < first || (size_t)item >= first + ui_menu_shown(m->rects, app->scale, UI_MENU_ENTRIES))
        return gui_rect(0, 0, 0, 0);
    return ui_row_box(app, ui_menu_row(m->rects, app->scale, UI_MENU_ENTRIES, (size_t)item - first));
}
/* Install Keys' box in the failure colour, at the clock given (a blink's half): inside it, past its label */
static int keys_red(App* app, Uint64 clock) {
    GuiMenuBoxes m;
    GuiRect b;
    app->clock = clock;
    render(app);
    menu_boxes(app, &m);
    b = menu_entry_box(app, &m, GUI_KEYS);
    return same_colour(pixel(app, (float)(b.x + b.w - 3 * app->scale - 1), (float)(b.y + b.h / 2)),
                       rgb(app->theme.failure));
}
static int near_colour(SDL_Color a, SDL_Color b, int by) {
    return abs(a.r - b.r) <= by && abs(a.g - b.g) <= by && abs(a.b - b.b) <= by;
}
/* the splash on the screen: page all around the square of side pixels in the middle, and in it svg's drawing at that
   size on the page as snx_splash_draw makes it (premultiplied, so over black its own colours) */
static void splash_drawn(App* app, const SDL_Rect* view, const SnxSplashSvg* svg, int side, SDL_Color page) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    Uint32* drawing = malloc((size_t)side * (size_t)side * 4);
    int left = (view->w - side) / 2, top = (view->h - side) / 2, x, y, drawn = 0;
    SnxSplashDrawing* read = snx_splash_open(svg);
    assert(image && drawing && read && !snx_splash_draw(read, side, drawing, side * 4));
    snx_splash_close(read);
    for (y = 0; y < view->h; ++y)
        for (x = 0; x < view->w; ++x) {
            Uint8 r = 0, g = 0, b = 0, a = 0;
            int in = x >= left && x < left + side && y >= top && y < top + side;
            assert(SDL_ReadSurfacePixel(image, x, y, &r, &g, &b, &a));
            if (!in) {
                assert(r == page.r && g == page.g && b == page.b);
            } else {
                Uint32 p = drawing[(y - top) * side + (x - left)];
                int alpha = (int)(p >> 24), want[3] = {(int)(p >> 16 & 255), (int)(p >> 8 & 255), (int)(p & 255)};
                int c, got[3] = {r, g, b}, on[3] = {page.r, page.g, page.b};
                for (c = 0; c < 3; ++c) assert(abs(got[c] - (want[c] + on[c] * (255 - alpha) / 255)) <= 2);
                drawn += alpha > 0;
            }
        }
    assert(drawn > side * side / 4);
    free(drawing);
    SDL_DestroySurface(image);
}
/* the boxes a pointer or a finger goes to: a menu's item, Games' rows (0 Add Folder, then the page's games; with
   recent a recent row), a form's row on the left or the right; a pointer at a box's middle */
static GuiRect entry_box(App* app, int item) {
    GuiMenuBoxes m;
    layout(app);
    menu_boxes(app, &m);
    return menu_entry_box(app, &m, item);
}
/* Games' Rows' row (the label and the recent rows under the lead, then the page's), as drawn */
static GuiRect games_row_box(const App* app, const GuiGamesBoxes* g, int row) {
    return ui_row_box(app, ui_games_row(g->rects, app->scale, UI_GAMES_ROWS, (size_t)row));
}
static GuiRect game_box(App* app, int i, bool recent) {
    GuiGamesBoxes g;
    layout(app);
    games_boxes(app, &g);
    assert(app->menu.view == GUI_LOAD_GAMES && (!recent || i < g.lead - 1));
    return games_row_box(app, &g, recent ? i + 1 : g.lead + i);
}
/* a form's list, the left (ui.txt's FormList) or the right (FormSide): the rows it has room for, the row it shows
   first, and row's box as drawn (its row past the cursor), none while it is scrolled out */
static size_t form_shown(const App* app, const GuiFormBoxes* f, bool right) {
    return right ? ui_form_side_shown(f->right, app->scale, UI_FORM_SIDE_ROWS)
                 : ui_form_list_shown(f->left, app->scale, UI_FORM_LIST_ROWS);
}
static size_t form_first(const GuiFormBoxes* f, bool right) {
    return right ? f->right_lists[UI_FORM_SIDE_ROWS].first : f->left_lists[UI_FORM_LIST_ROWS].first;
}
static GuiRect form_row_box(const App* app, const GuiFormBoxes* f, bool right, int row) {
    size_t first = form_first(f, right);
    if (row < 0 || (size_t)row < first || (size_t)row >= first + form_shown(app, f, right)) return gui_rect(0, 0, 0, 0);
    return ui_row_box(app, right ? ui_form_side_row(f->right, app->scale, UI_FORM_SIDE_ROWS, (size_t)row - first)
                                 : ui_form_list_row(f->left, app->scale, UI_FORM_LIST_ROWS, (size_t)row - first));
}
static GuiRect row_box(App* app, int row, bool right) {
    GuiFormBoxes f;
    layout(app);
    form_boxes(app, &f);
    return form_row_box(app, &f, right, row);
}
/* text drawn from pixel x, y in the font's own bytes: its glyphs in color, around them back but on the font's last row,
   empty, which a box's frame may cover; an icon lunasvg's drawing, the pixels it covers whole in color, those it does
   not touch back. A pixel that differs is named with the check's line */
#define text_px_is(app, x0, y0, text, color, back) text_px_is_at(__LINE__, app, x0, y0, text, color, back)
static void text_px_is_at(int line, App* app, int x0, int y0, const char* text, SDL_Color color, SDL_Color back) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    int half = app->half ? app->half : 2 * app->scale; /* the letters' size, in halves of a cell */
    int side = 8 * half;                               /* an icon's, two columns a cell high */
    uint8_t* cover = (uint8_t*)malloc((size_t)side * (size_t)side);
    const char *at = text, *was = text;
    uint32_t point;
    assert(image && cover);
    /* i: the column, a wide letter's or an icon's halves two; a byte no UTF-8 character starts with is CP437's own
       ("\xfe", which the menu draws for U+25A0) */
    for (int i = 0; (point = gui_utf8_step(&at)) != 0; was = at) {
        int glyph = point == 0xFFFD && at == was + 1 ? (unsigned char)*was : gui_menu_glyph(point);
        if (glyph >= GUI_ICON_GLYPH) {
            assert(!menu_icon_alpha((glyph - GUI_ICON_GLYPH) / 2, side, cover));
            for (int k = 0; 2 * (k / side) < 15 * half; ++k) {
                Uint8 r, g, b, a;
                int px = x0 + i * 4 * half + k % side, py = y0 + k / side;
                SDL_Color want = cover[k] == 255 ? color : back;
                assert(SDL_ReadSurfacePixel(image, px, py, &r, &g, &b, &a));
                if ((cover[k] && cover[k] != 255) || (r == want.r && g == want.g && b == want.b)) continue;
                fprintf(stderr, "gui_sdl.c:%d: \"%s\" from %d,%d: U+%04X at %d,%d is %02x%02x%02x, not %02x%02x%02x\n",
                        line, text, x0, y0, (unsigned)point, px, py, r, g, b, want.r, want.g, want.b);
                assert(!"the icon drawn");
            }
            i += 2;
            continue;
        }
        for (int c = glyph, n = gui_glyph_cols(c), half_of = 0; half_of < n; ++half_of, ++i)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 8; ++x) {
                    Uint8 r, g, b, a;
                    int px = x0 + font_px(i * 8 + x, half), py = y0 + font_px(y, half);
                    assert(SDL_ReadSurfacePixel(image, px, py, &r, &g, &b, &a));
                    bool ink = (font_rows(c + half_of)[y] & (0x80 >> x)) != 0;
                    SDL_Color want = ink ? color : back;
                    if ((y == 15 && !ink) || (r == want.r && g == want.g && b == want.b)) continue;
                    fprintf(stderr,
                            "gui_sdl.c:%d: \"%s\" from %d,%d: U+%04X at %d,%d is %02x%02x%02x, not %02x%02x%02x\n",
                            line, text, x0, y0, (unsigned)point, px, py, r, g, b, want.r, want.g, want.b);
                    assert(!"the text drawn");
                }
    }
    free(cover);
    SDL_DestroySurface(image);
}
/* the same in the foot's letters */
#define foot_px_is(app, x0, y0, text, color, back) foot_px_is_at(__LINE__, app, x0, y0, text, color, back)
static void foot_px_is_at(int line, App* app, int x0, int y0, const char* text, SDL_Color color, SDL_Color back) {
    int scale = app->scale;
    app->scale = foot_scale(app);
    text_px_is_at(line, app, x0, y0, text, color, back);
    app->scale = scale;
}
/* the menu, then its foot again with the tips beside the keys, as the setup's are (GUI_SHOWS_FOOT_TIPS): m the places
   of that foot */
static void render_foot_tips(App* app, GuiMenuBoxes* m) {
    char keys[256];
    render(app);
    menu_boxes(app, m);
    place_screen(app, &m->place, shown_keys(app, keys, sizeof keys),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS | GUI_SHOWS_SIDE | GUI_SHOWS_FOOT_TIPS);
    screen_foot(app, &m->place, item_about(app, (int)m->lists[UI_MENU_ENTRIES].focus), keys);
}
/* where the text of a box of the foot goes, in its letters (foot_text) */
static GuiRect foot_room(App* app, GuiRect r, int box) {
    int scale = app->scale;
    GuiRect out;
    app->scale = foot_scale(app);
    out = foot_text(app, r, box);
    app->scale = scale;
    return out;
}
/* text drawn at the start of the tips on their row under the head, past their box's bar, a half step under the text's
   letters */
#define tips_px_is(app, tips, text, color, back) tips_px_is_at(__LINE__, app, tips, text, color, back)
static void tips_px_is_at(int line, App* app, GuiRect tips, const char* text, SDL_Color color, SDL_Color back) {
    GuiRect r = foot_text(app, tips, app->theme.tips_box);
    int half = text_half(app);
    text_px_is_at(line, app, r.x, r.y, text, color, back);
    app->half = half;
}
/* a click, or a tap, on column col of the key the foot draws in color, in its letters */
#define press_key(app, key, col, tap, color) press_key_at(__LINE__, app, key, col, tap, color)
static void press_key_at(int line, App* app, const char* key, int col, bool tap, SDL_Color color) {
    int x, y, fs = foot_scale(app), scale = app->scale;
    render(app);
    app->scale = fs;
    if (!find_text(app, key, color, 1, &x, &y)) shows_at(line, app, key, color, 1, 1); /* says which */
    app->scale = scale;
    x += col * 8 * fs + 4 * fs;
    y += 8 * fs;
    if (tap)
        tap_px(app, x, y);
    else
        mouse_px(app, x, y, true);
}
static void touch_box(App* app, GuiRect b) { tap_px(app, b.x + b.w / 2, b.y + b.h / 2); }
/* a Library row's start as it shows path, whole or cut in its middle (gui_path_fit): the marker, the root and the first
   folder, which it always keeps */
static void row_start(const char* path, char* out, size_t size) {
    size_t n = strspn(path, "/\\");
    n += strcspn(path + n, "/\\");
    snprintf(out, size, "- %.*s", (int)n + (path[n] != 0), path);
}
/* Folders' row of kind at at (GuiFolderRow, a place, a folder's index or a library) */
#define FOLDER_ROW(kind, at) folder_row_of(&app, kind, at)
/* the folder picker's answer where it could not open (none on the test's display): the box that says so whole, which
   OK closes; none when it answered no folder */
static void picker_box_closed(App* app) {
    if (!app->confirm.shown) return;
    assert(app->confirm.ask == GUI_ASK_ERROR && strstr(app->error_line, "The folder picker could not open: "));
    key(app, SDLK_ESCAPE); /* Back closes it as OK does */
    assert(!app->confirm.shown);
}
/* a path the GUI keeps against one the test made: on Windows the system hands paths back with backslashes, where
   snx_path_join put a '/' */
static bool same_path(const char* a, const char* b) {
#ifdef _WIN32
    for (; *a && *b; ++a, ++b)
        if (*a != *b && !(strchr("/\\", *a) && strchr("/\\", *b))) return false;
    return *a == *b;
#else
    return !strcmp(a, b);
#endif
}
/* the Settings' pick for key is value; NULL: the .ini's */
static bool setting_is(App* app, const char* key, const char* value) {
    const char* v = gui_settings_value(&app->settings, key);
    return value ? v && !strcmp(v, value) : !v;
}
/* Select on a row of Settings > App, then back to the menu unless it opened a screen of its own */
static void app_select(App* app, int row) {
    open_app(app, row);
    key(app, SDLK_RETURN);
    if (app->editing && app->form == GUI_FORM_APP) leave_editor(app);
}
/* the theme editor's preview as last drawn, its pixels */
static SDL_Surface* preview_pixels(App* app) {
    SDL_Surface* read;
    assert(app->preview && SDL_SetRenderTarget(app->renderer, app->preview));
    read = SDL_RenderReadPixels(app->renderer, NULL);
    assert(SDL_SetRenderTarget(app->renderer, NULL) && read);
    return read;
}
static bool same_pixels(const SDL_Surface* a, const SDL_Surface* b) {
    int y;
    if (a->w != b->w || a->h != b->h || a->format != b->format) return false;
    for (y = 0; y < a->h; ++y)
        if (memcmp((const Uint8*)a->pixels + y * a->pitch, (const Uint8*)b->pixels + y * b->pitch,
                   (size_t)a->w * SDL_BYTESPERPIXEL(a->format)))
            return false;
    return true;
}
/* the launcher's gdbstub on or off */
static void launcher(App* app, bool on) { assert(gui_settings_set(&app->settings, "gdbstub", on ? "on" : "off")); }
/* the focus on the game of Games named name, its page shown */
static void focus_game(App* app, const char* name) {
    size_t i = 0;
    list_games(app);
    layout(app);
    while (i < app->games.count && strcmp(app->games.names[i], name)) ++i;
    assert(i < app->games.count && app->browser.page_size);
    app->browser.page = i / app->browser.page_size;
    app->browser.selected = app->browser.head + i % app->browser.page_size;
}
static void finish(App* app, int status) {
    /* the fake driver's game run waits for this, so the menu has shown it running before it ends: without it the
       process could exit before the test's first pump, which made compile_first fail now and then */
    const char* argv_file = getenv("SNX_TEST_ARGV");
    char* go = argv_file ? snx_format("%s.go", argv_file) : NULL;
    if (go) { assert(!snx_write_file(go, "", 0)); }
    assert(app->menu.running && !app->finished && !app->aborting && !app->abnormal_exit);
    int selected = app->menu.selected;
    Uint64 until = SDL_GetTicks() + 10000;
    while (app->menu.running && SDL_GetTicks() < until) {
        pump(app);
        render(app);
        SDL_Delay(5);
    }
    assert(!app->menu.running && app->finished && app->menu.view == GUI_LOAD_NONE);
    char expected[80];
    snprintf(expected, sizeof(expected),
             status ? "Program ended abnormally (status %d)." : "The run ended with status %d.", status);
    assert(!strcmp(app->message, expected) && app->abnormal_exit == (status != 0) && app->has_output);
    assert(app->log_due == (status != 0)); /* Share Log blinks after a crash, and stops after a run that ends well */
    shows_foot(app, "Press [ESC] to return to menu", red);   /* where the keys go, in their letters */
    if (status) shows(app, "Program ended abnormally", red); /* beside the output, how it ended */
    /* a theme can make that line a prompt: framed and placed, the done and the failed one apart */
    GuiPromptLine line = status ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE;
    app->theme.prompt.lines[line] = true;
    app->theme.prompt.blink = false;
    render(app);
    shows_foot(app, "[ Press [ESC] to return to menu ]", red);
    app->theme.prompt.lines[line] = false;
    app->theme.prompt.lines[status ? GUI_PROMPT_DONE : GUI_PROMPT_FAILED] = true;
    render(app);
    hides_foot(app, "[ Press [ESC] to return to menu ]", red);
    shows_foot(app, "Press [ESC] to return to menu", red);
    app->theme.prompt = gui_theme_builtin(app->theme.id)->prompt;
    for (int i = 0; i < 3; ++i) pump(app);
    mouse_px(app, 4, 4, true); /* A completed process still requires ESC acknowledgement. */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    key(app, SDLK_SPACE);
    assert(!app->menu.running && app->finished && app->menu.selected == selected);
    key(app, SDLK_ESCAPE);
    assert(!app->menu.running && !app->finished && app->menu.view == GUI_LOAD_NONE);
    assert(!app->menu.quit && !app->message[0] && app->has_output);
    /* the item the run was started from has the focus again */
    assert(app->menu.selected == selected && setting_is(app, "fullscreen", "1"));
    render(app);
    if (go) {
        remove(go);
        free(go);
    }
}
/* the queue's recompile running, through to its end as pump reads it: how it ended (OK for status 0, else FAILED) on its
   row, its output kept; then ESC on the Recompile tab, back to Games, the line seen and gone with the tab, and the menu
   again, as a run's end leaves it */
static void job_done(App* app, int status) {
    int i = gui_queue_running(&app->queue);
    Uint64 until = SDL_GetTicks() + 10000;
    assert(i >= 0 && !app->menu.running);
    while (gui_queue_running(&app->queue) == i && SDL_GetTicks() < until) {
        pump(app);
        render(app);
        SDL_Delay(5);
    }
    assert(gui_queue_running(&app->queue) < 0 && app->queue.job[i].state == (status ? GUI_JOB_FAILED : GUI_JOB_OK));
    assert(app->queue.job[i].status == status && app->ended_serial == app->queue.job[i].serial);
    go_tab(app, GUI_TAB_RECOMPILE);
    app->queue_focus = i;
    key(app, SDLK_ESCAPE);
    assert(app->menu.view == GUI_LOAD_GAMES && !app->queue.count && tab_count(app) == GUI_TAB_RECOMPILE);
    go_tab(app, GUI_TAB_MENU);
}
static void set_env(const char* name, const char* value);
/* what launch() started for the game it opened: the menu's run, or the queue's job (a first compile, R's or B's) */
static bool launched(const App* app) { return app->menu.running || gui_queue_running(&app->queue) >= 0; }
/* the fake driver's next runs as the test asks them: each keeps its argv in argv_file, a string a line, prints say and
   ends with status (main's recorder); argv_file NULL: its own checks again */
static void driver_will(const char* argv_file, int status, const char* say) {
    char text[16];
    snprintf(text, sizeof text, "%d", status);
    set_env("SNX_TEST_ARGV", argv_file); /* NULL: the fake driver's own checks again */
    set_env("SNX_TEST_STATUS", argv_file ? text : NULL);
    set_env("SNX_TEST_SAY", argv_file ? say : NULL);
}
/* what launch() started, run to its end with status: the menu's run to its end screen, or the queue's job to its end,
   its line taken out and the Menu shown. The argv the fake driver got, a string a line (driver_will), which the
   caller frees */
static char* ran(App* app, int status) {
    size_t size;
    char* argv;
    if (app->menu.running)
        finish(app, status);
    else
        job_done(app, status);
    argv = (char*)snx_read_file(getenv("SNX_TEST_ARGV"), &size);
    assert(argv && !remove(getenv("SNX_TEST_ARGV")));
    return argv;
}
/* whether argv (a string a line, as ran gives it) holds arg, and the string after it in after (NULL: not asked) */
static bool has_arg(const char* argv, const char* arg, const char* after) {
    size_t n = strlen(arg);
    for (const char* line = argv; *line; line = strchr(line, '\n') + 1) {
        if (!strncmp(line, arg, n) && line[n] == '\n')
            return !after || (!strncmp(line + n + 1, after, strlen(after)) && line[n + 1 + strlen(after)] == '\n');
        if (!strchr(line, '\n')) break;
    }
    return false;
}
/* a console's game not made yet, opened: its first compile through driver, its own (no Switch flag), -r before file
   and its data folder, run to its end with status (driver_will) */
static void compiled_by(App* app, const char* driver, const char* file, int status) {
    char* argv = ran(app, status);
    assert(strstr(argv, driver) && strstr(argv, driver) < strchr(argv, '\n') && has_arg(argv, "-r", file) &&
           has_arg(argv, "--data", app->data_dir) && !has_arg(argv, "-S", NULL) &&
           !has_arg(argv, "--fullscreen=1", NULL));
    free(argv);
}
/* the stamp this build's driver leaves in the game name's folder (<folder>/<name>.abi), as a game it made with no mod has:
   the launcher plays that game as it is (made_here) */
static void stamp_here(const char* folder, const char* name) {
    char *base = snx_path_join(folder, name), *stamp = snx_format("%s.abi", base),
         *version = snx_game_stamp_for(SNX_ABI, "");
    assert(base && stamp && version && !snx_write_file(stamp, version, strlen(version)));
    free(version);
    free(stamp);
    free(base);
}
/* a game of the library made from dump already, as a console's driver leaves one: its program, its data folder,
   source= in its folder's <name>.ini and this build's stamp. Opening dump then plays it, no compile first. Its folder,
   which the caller removes */
static char* made_game(const App* app, const char* dump, const char* name) {
    char *folder = snx_path_join(app->games_dir, name), *data_name = snx_format("%s_data", name),
         *ini_name = snx_format("%s.ini", name), *text = snx_format("[Game]\nsource=%s\n", dump);
#ifdef _WIN32
    char* file = snx_format("%s.exe", name);
#else
    char* file = snx_strdup(name);
#endif
    char *data = snx_path_join(folder, data_name), *ini = snx_path_join(folder, ini_name),
         *program = snx_path_join(folder, file);
    assert(!snx_mkdirs(data) && !snx_write_file(program, "x", 1) && !snx_make_executable(program) &&
           !snx_write_file(ini, text, strlen(text)));
    stamp_here(folder, name);
    free(program);
    free(ini);
    free(data);
    free(file);
    free(text);
    free(ini_name);
    free(data_name);
    return folder;
}
/* the question a dump whose game is made asks (GUI_ASK_MADE), naming the game and where it is: Play, in focus, taken */
static void play_made(App* app) {
    assert(app->confirm.shown && app->confirm.ask == GUI_ASK_MADE && !app->confirm.selected && app->made_file);
    assert(strstr(app->ask_line, " is compiled already, in "));
    key(app, SDLK_RETURN);
    assert(!(app->confirm.shown && app->confirm.ask == GUI_ASK_MADE) && !app->made_file); /* a failure may say why */
}
/* made_game's game gone, and with it the games folder when nothing else is left in it (made_game made it), as the tests
   after it expect the folder; folder freed */
static void made_gone(const App* app, char* folder) {
    char** names = NULL;
    size_t count = 0;
    assert(!snx_remove_tree(folder));
    free(folder);
    if (!snx_list_dir(app->games_dir, &names, &count) && !count) assert(!snx_remove_tree(app->games_dir));
    snx_free_strings(names, count);
}
/* what the button map page last drew: a text holding s, or the shape of id of a kind and style (-1: any) */
static bool drawn_text(const App* app, const char* s) {
    for (int i = 0; i < app->pad_drawing.n; ++i)
        if (app->pad_drawing.shape[i].kind == SNX_PAD_SHAPE_TEXT && strstr(app->pad_drawing.shape[i].text, s))
            return true;
    return false;
}
static const SnxPadShape* drawn(const App* app, int id, int kind, int style) {
    for (int i = 0; i < app->pad_drawing.n; ++i) {
        const SnxPadShape* p = &app->pad_drawing.shape[i];
        if (p->id == id && p->kind == kind && (style < 0 || p->style == style)) return p;
    }
    return NULL;
}
/* the page's events as the loop gets them, after the virtual pad's changes */
static void pads(App* app) {
    SDL_Event e;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void pad_press(App* app, SDL_Joystick* j, SDL_GamepadButton button) {
    SDL_SetJoystickVirtualButton(j, button, true);
    pads(app);
    SDL_SetJoystickVirtualButton(j, button, false);
    pads(app);
}
static bool picks_have(App* app, const char* line) {
    size_t size;
    char* kept = (char*)snx_read_file(app->settings_file, &size);
    bool has = kept && strstr(kept, line);
    free(kept);
    return has;
}
/* Settings > Controllers by player, as Eden's tabs: its Player row goes round All (every player's rows) and 1 to 8
   (the player's own and those no player has its own of); a player's Button map is the button map page on its keys,
   its A and B as its own swap says, else every player's; ESC comes back to the player's rows */
static void players_view(App* app) {
    static const char list[] =
        "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\tSwap A/B\tSwap A/B\t0=Off|1=On\n"
        "Controllers\tgamepad_deadzone\t10\tint\t0\t90\tDead zone\tStick dead zone\t0=0%|10=10%\n"
        "Controllers\tgamepad_rumble\t100\tint\t0\t100\tVibration\tVibration\t0=Off|100=100%\n"
        "Controllers/Button map\tgamepad_map\t\ttext\t\t\tEach control's button\tButton map\t\n"
        "Controllers/Player\tgamepad_p1_device\t\ttext\t\t\tIts controller\tController\t\n"
        "Controllers/Player\tgamepad_p1_swap_ab\t-1\tint\t-1\t1\tIts A and B\tSwap A/B\t-1=As All|0=Off|1=On\n"
        "Controllers/Player\tgamepad_p1_map\t\ttext\t\t\tIts map\tButton map\t\n"
        "Controllers/Player\tgamepad_p2_device\t\ttext\t\t\t\tController\t\n"
        "Controllers/Player\tgamepad_p2_swap_ab\t-1\tint\t-1\t1\t\tSwap A/B\t-1=As All|0=Off|1=On\n"
        "Controllers/Player\tgamepad_p2_map\t\ttext\t\t\t\tButton map\t\n";
    int r;
    settings_forget(app);
    assert(!gui_settings_read(&app->settings, list));
    gui_settings_mark(&app->settings);
    app->player = 0;
    open_settings(app);
    key(app, SDLK_DOWN); /* past Launch, the launcher's own */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    /* All: the Player row, then every player's */
    assert(app->form == GUI_FORM_GROUP && app->view_count == 5 && app->editor.row == 0);
    assert(!strcmp(app->settings.values[setting_of(app, 0)].text, "< All >"));
    assert(row_of(app, setting_row(app, "gamepad_swap_ab")) == 1 &&
           row_of(app, setting_row(app, "gamepad_p1_map")) < 0);
    render(app);
    shows(app, "-    Player", yellow); /* focused */
    /* Right: player 1, its own rows and the dead zone; no every player's Swap A/B, Vibration or Button map */
    key(app, SDLK_RIGHT);
    assert(app->player == 1 && app->view_count == 5 && !strcmp(app->settings.values[setting_of(app, 0)].text, "< 1 >"));
    assert(row_of(app, setting_row(app, "gamepad_p1_swap_ab")) > 0 &&
           row_of(app, setting_row(app, "gamepad_swap_ab")) < 0);
    assert(row_of(app, setting_row(app, "gamepad_deadzone")) > 0 &&
           row_of(app, setting_row(app, "gamepad_p2_map")) < 0);
    render(app);
    shows(app, "As All [default]", normal);
    shows(app, "As All >", normal); /* its Button map, every player's until it has its own */
    /* its Controller: Any while it names none; Enter waits for a press, which gives it the controller pressed by its
       GUID ("/2" for the second of two alike); ESC leaves it as it was */
    {
        SDL_VirtualJoystickDesc d;
        SDL_JoystickID one, two;
        SDL_Joystick *j1, *j2;
        char guid[33], line[40];
        shows(app, "Any [default]", normal);
        app->editor.row = row_of(app, setting_row(app, "gamepad_p1_device"));
        key(app, SDLK_RETURN);
        assert(app->device_listen && strstr(app->editor.note, "Press a button on player 1's controller"));
        key(app, SDLK_ESCAPE);
        assert(!app->device_listen && !gui_settings_value(&app->settings, "gamepad_p1_device") &&
               app->form == GUI_FORM_GROUP && !SDL_WasInit(SDL_INIT_GAMEPAD));
        key(app, SDLK_RETURN);
        assert(SDL_InitSubSystem(SDL_INIT_JOYSTICK)); /* the pads stay between the waits, as real ones do */
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.vendor_id = 0x045e;
        d.product_id = 0x02ea;
        d.name = "One Pad";
        one = SDL_AttachVirtualJoystick(&d);
        two = SDL_AttachVirtualJoystick(&d);
        j1 = SDL_OpenJoystick(one);
        j2 = SDL_OpenJoystick(two);
        assert(j1 && j2);
        pads(app);
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(one), guid, sizeof guid);
        pad_press(app, j2, SDL_GAMEPAD_BUTTON_SOUTH);
        snprintf(line, sizeof line, "%s/2", guid);
        assert(!app->device_listen && !strcmp(gui_settings_value(&app->settings, "gamepad_p1_device"), line));
        assert(!strcmp(app->editor.note, "Player 1: One Pad") && picks_have(app, "gamepad_p1_device="));
        key(app, SDLK_RETURN);
        pad_press(app, j1, SDL_GAMEPAD_BUTTON_SOUTH);
        assert(!strcmp(gui_settings_value(&app->settings, "gamepad_p1_device"), guid));
        key(app, SDLK_DELETE); /* the next to connect again */
        assert(!gui_settings_value(&app->settings, "gamepad_p1_device"));
        SDL_CloseJoystick(j1);
        SDL_CloseJoystick(j2);
        assert(SDL_DetachVirtualJoystick(one) && SDL_DetachVirtualJoystick(two));
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }
    /* round: Left twice is 8 (no rows of its own here), Right twice back to 1 */
    app->editor.row = 0;
    key(app, SDLK_LEFT);
    key(app, SDLK_LEFT);
    assert(app->player == 8 && app->view_count == 2);
    key(app, SDLK_RIGHT);
    key(app, SDLK_RIGHT);
    assert(app->player == 1);
    /* its Swap A/B and every player's: its own wins, As All takes every player's */
    assert(gui_settings_set(&app->settings, "gamepad_swap_ab", "1"));
    app->editor.row = row_of(app, setting_row(app, "gamepad_p1_map"));
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && app->pad_player == 1 && app->pad_map == setting_row(app, "gamepad_p1_map"));
    assert(app->pad.swap_ab); /* As All: on */
    render(app);
    { /* the page's place in the Settings, as much of it as fits beside the tabs */
        char title[160];
        int letters;
        head_title(app, true, title, sizeof title, &letters);
        assert(strlen(title) >= 23 && !strcmp(title + strlen(title) - 23, "PLAYER 1  /  BUTTON MAP"));
        shows(app, title, normal);
    }
    /* a change keeps the player's map, every player's untouched */
    app->pad.focus = SNX_PAD_LT;
    key(app, SDLK_RIGHT);
    assert(gui_settings_value(&app->settings, "gamepad_p1_map") && !gui_settings_value(&app->settings, "gamepad_map"));
    assert(strstr(gui_settings_value(&app->settings, "gamepad_p1_map"), "lt:ZR"));
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_GROUP && app->player == 1 &&
           setting_of(app, app->editor.row) == setting_row(app, "gamepad_p1_map"));
    assert(gui_settings_set(&app->settings, "gamepad_p1_swap_ab", "0"));
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && !app->pad.swap_ab); /* its own: off */
    key(app, SDLK_ESCAPE);
    /* its map its own now; Del on a player's row: back to As All */
    render(app);
    shows(app, "Custom >", yellow); /* focused, as ESC came back to it */
    app->editor.row = row_of(app, setting_row(app, "gamepad_p1_swap_ab"));
    key(app, SDLK_DELETE);
    assert(!gui_settings_value(&app->settings, "gamepad_p1_swap_ab"));
    /* every row the view shows is a row of the settings, the Player row first */
    for (r = 0; r < app->view_count; ++r) assert(setting_of(app, r) >= 0 && setting_of(app, r) < app->settings.count);
    /* the list freed with the view open (a new start, a list read again): the view, the groups and the form on the
       view's rows go with it, and the groups' names were never the list's */
    assert(app->view_count > 0 && app->groups > 1 && app->editor.keys == app->view_keys);
    assert(app->group_keys[1].key == app->group_name[1]);
    settings_forget(app);
    assert(!app->view_count && !app->groups && !app->editor.count && !app->settings.keys);
    assert(!gui_settings_read(&app->settings, list));
    gui_settings_mark(&app->settings);
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_SETTINGS);
    app->player = 0;
    leave_editor(app);
}
/* Settings > Controllers > Button map: the pad drawn (pad_screen.h) with what each control sends, on SDL's virtual
   Xbox pad. No presets: each control the button in its place, gamepad_map over that, A and B as gamepad_swap_ab says;
   the keys, the pad's presses, its triggers and stick, a click and the wheel act on it, each change kept in the picks
   at once, Del puts the map back, and ESC goes back to the page's row with the pads closed */
static void button_map(App* app) {
    static const char list[] =
        "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\tSwap A/B\tSwap A/B\t0=Off|1=On\n"
        "Controllers\tgamepad_deadzone\t10\tint\t0\t90\tDead zone\tStick dead zone\t0=0%|10=10%\n"
        "Controllers\tgamepad_joycon_pair\t1\tint\t0\t1\tJoy-Cons\tPair Joy-Cons\t0=Separate|1=Together\n"
        "Controllers/Button map\tgamepad_map\t\ttext\t\t\tEach control's button\tButton map\t\n";
    SDL_VirtualJoystickDesc d;
    SDL_JoystickID id;
    SDL_Joystick* j;
    SDL_Event e = {0};
    const SnxPadShape* box;
    float x = 0, y = 0; /* mingw's assert returns, as far as the compiler knows */
    int page = -1, focus;
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x045e/0x02ea"); /* a real pad plugged in stays out */
    settings_forget(app);
    assert(!gui_settings_read(&app->settings, list));
    gui_settings_mark(&app->settings);
    for (int i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].kind == GUI_SETTING_PAGE) page = i;
    open_settings(app);
    key(app, SDLK_DOWN); /* past Launch, the launcher's own */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_GROUP && !strcmp(app->group_keys[app->group].key, "Controllers"));
    app->editor.row = page - app->first;
    key(app, SDLK_RETURN);
    /* drawn before SDL looks for pads, which takes it most of a second, saying so */
    shows_half(app, "Looking for controllers...", rgb(app->theme.soft)); /* in the tips */
    /* no pad: no presets, the first control focused, A and B where the console's are */
    assert(app->form == GUI_FORM_PAD && app->pads_read && !app->gamepad && app->pad.focus == SNX_PAD_LT &&
           !app->pad.swap_ab);
    /* two Joy-Cons one pad, as gamepad_joycon_pair has it when not picked */
    assert(!strcmp(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS), "1"));
    render(app);
    { /* the page's place in the Settings, as much of it as fits beside the tabs */
        char title[160];
        int letters;
        head_title(app, true, title, sizeof title, &letters);
        assert(strlen(title) >= 26 && !strcmp(title + strlen(title) - 26, "CONTROLLERS  /  BUTTON MAP"));
        shows(app, title, normal);
    }
    assert(drawn_text(app, "No controller") && !drawn_text(app, "By position") && !drawn_text(app, "By label"));
    assert(drawn(app, SNX_PAD_LT, SNX_PAD_SHAPE_BOX, SNX_PAD_STYLE_FOCUS) && drawn_text(app, "LT sends ZL"));
    /* its texts in the font's own glyphs, a byte each */
    {
        SnxPadShape shape = {SNX_PAD_SHAPE_TEXT,
                             SNX_PAD_STYLE_PLAIN,
                             -1,
                             16.0f * (float)app->scale,
                             16.0f * 20 * (float)app->scale,
                             0,
                             0,
                             "\xFE \x82"};
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
        SDL_RenderClear(app->renderer);
        pad_text(app, &shape, app->theme.ink);
        colored_text_on(app, 2, 20, "\xFE \x82", rgb(app->theme.ink), black);
    }
    key(app, SDLK_RETURN); /* with no pad, nothing to wait for */
    assert(!app->pad.listening && !gui_settings_value(&app->settings, "gamepad_map"));
    /* a pad plugged in is opened, named, and what it holds lit */
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = 0x045e;
    d.product_id = 0x02ea;
    d.name = "Man\xC3\xA9 Pad"; /* UTF-8, shown in the font's glyphs */
    id = SDL_AttachVirtualJoystick(&d);
    j = SDL_OpenJoystick(id);
    assert(id && j);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) /* at rest: a trigger at the bottom of its range */
        SDL_SetJoystickVirtualAxis(j, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SDL_JOYSTICK_AXIS_MIN : 0);
    pads(app);
    assert(app->gamepad && SDL_GetGamepadID(app->gamepad) == id);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, true); /* X: no key of the page's */
    pads(app);
    render(app);
    assert(!strcmp(app->pad.pad, "Man\x82 Pad"));
    assert(!drawn_text(app, "No controller"));
    assert(drawn_text(app, "Man\x82 Pad"));
    assert(drawn(app, SNX_PAD_NORTH, SNX_PAD_SHAPE_DISC, SNX_PAD_STYLE_LIT) && app->form == GUI_FORM_PAD);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, false);
    pads(app);
    /* its presses as the game's menu takes them, by the button each sends: the D-pad moves; A, the right one as on
       the console, waits for a press, which takes the focused control's button and gives its own */
    app->pad.focus = SNX_PAD_SOUTH;
    pad_press(app, j, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    assert(app->pad.focus == SNX_PAD_RS);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_DPAD_UP);
    assert(app->pad.focus == SNX_PAD_SOUTH && !app->pad.listening);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_EAST);
    assert(app->pad.listening);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_EAST);
    assert(!app->pad.listening && app->pad.focus == SNX_PAD_EAST && app->form == GUI_FORM_PAD);
    /* the whole map is the player's from then on, whatever pad comes next */
    assert(setting_is(app, "gamepad_map",
                      "south:A east:B west:Y north:X lb:L rb:R lt:ZL rt:ZR ls:L3 rs:R3 back:Minus "
                      "start:Plus up:Up down:Down left:Left right:Right"));
    assert(picks_have(app, "gamepad_map=south:A east:B west:Y"));
    /* a trigger is pressed once it is past SNX_PAD_TRIGGER */
    app->pad.focus = SNX_PAD_LT;
    key(app, SDLK_RETURN);
    assert(app->pad.listening);
    /* a trigger's 0 to 32767 from its axis' whole range */
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)(2 * (SNX_PAD_TRIGGER - 100) - 32768));
    pads(app);
    assert(app->pad.listening);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    pads(app);
    assert(!app->pad.listening && strstr(gui_settings_value(&app->settings, "gamepad_map"), " lt:ZR rt:ZL "));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    pads(app);
    /* the left stick past half way is the D-pad, a step each time it crosses */
    focus = app->pad.focus;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    pads(app);
    assert(app->pad.focus != focus);
    focus = app->pad.focus;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 32000);
    pads(app);
    assert(app->pad.focus == focus);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pads(app);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    pads(app);
    assert(app->pad.focus != focus);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pads(app);
    /* the wheel moves as Up and Down; a click on a control's box focuses it, a second one waits for a press */
    focus = app->pad.focus;
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.integer_y = 1;
    event(app, &e);
    assert(app->pad.focus != focus);
    render(app);
    box = drawn(app, SNX_PAD_NORTH, SNX_PAD_SHAPE_BOX, -1);
    assert(box && SDL_RenderCoordinatesToWindow(app->renderer, box->x + box->w / 2, box->y + box->h / 2, &x, &y));
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = x;
    e.button.y = y;
    event(app, &e);
    assert(app->pad.focus == SNX_PAD_NORTH && !app->pad.listening);
    event(app, &e);
    assert(app->pad.listening);
    key(app, SDLK_ESCAPE); /* stops waiting, stays */
    assert(!app->pad.listening && app->form == GUI_FORM_PAD);
    /* Del: the map the game's .ini's again; Swap A/B, its row's, over it when the page opens */
    key(app, SDLK_DELETE);
    assert(!gui_settings_value(&app->settings, "gamepad_map") && !picks_have(app, "gamepad_"));
    {
        SnxPadMap shown;
        assert(gui_settings_set(&app->settings, "gamepad_swap_ab", "1"));
        pad_start(app);
        snx_pad_screen_map(&app->pad, &shown);
        assert(app->pad.swap_ab && shown.bit[SNX_PAD_SOUTH] == 0 && shown.bit[SNX_PAD_EAST] == 1); /* A, B */
        assert(gui_settings_set(&app->settings, "gamepad_swap_ab", ".ini"));
        pad_start(app);
        assert(!app->pad.swap_ab);
    }
    /* the pad unplugged: none shown, nothing waited for */
    app->pad.focus = SNX_PAD_NORTH;
    key(app, SDLK_RETURN);
    assert(app->pad.listening);
    SDL_CloseJoystick(j);
    assert(SDL_DetachVirtualJoystick(id));
    pads(app);
    assert(!app->gamepad && !app->pad.listening);
    render(app);
    assert(drawn_text(app, "No controller"));
    /* a joystick SDL has no gamepad mapping for, plugged in: named in the middle, with its GUID and the file its
       mapping goes in, in the failure colour; gone once it is unplugged */
    {
        char *folder = snx_dirname(app->settings_file), *file = snx_path_join(folder, "gamecontrollerdb.txt");
        SDL_VirtualJoystickDesc odd;
        SDL_JoystickID stick;
        assert(!app->editor.note[0] && !app->pad.note[0]);
        SDL_INIT_INTERFACE(&odd);
        odd.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        odd.naxes = 2;
        odd.nbuttons = 4;
        odd.vendor_id = 0x1234;
        odd.product_id = 0x5678;
        odd.name = "Odd Stick";
        stick = SDL_AttachVirtualJoystick(&odd);
        assert(stick);
        pads(app);
        assert(!app->gamepad && !strncmp(app->pad.note, "Odd Stick has no SDL mapping: add a line for GUID ",
                                         sizeof "Odd Stick has no SDL mapping: add a line for GUID " - 1));
        assert(strstr(app->pad.note, file) && !app->editor.note[0]);
        render(app);
        assert(drawn_text(app, "Odd Stick has no SDL"));
        {
            int i, noted = 0;
            for (i = 0; i < app->pad_drawing.n; ++i) noted |= app->pad_drawing.shape[i].style == SNX_PAD_STYLE_NOTE;
            assert(noted);
        }
        assert(SDL_DetachVirtualJoystick(stick));
        pads(app);
        assert(!app->pad.note[0]);
        free(file);
        free(folder);
    }
    /* ESC: back to the page's row, the pads closed */
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_GROUP && app->page < 0 && app->editor.row == page - app->first);
    assert(!app->pads_read && !SDL_WasInit(SDL_INIT_GAMEPAD));
    /* a map it cannot read is named, in the failure colour, and the page shows the labels' */
    assert(gui_settings_set(&app->settings, "gamepad_map", "south:Q"));
    assert(gui_settings_set(&app->settings, "gamepad_joycon_pair", "0")); /* and the Joy-Cons apart */
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && strstr(app->editor.note, "\"south:Q\" is not a control"));
    assert(!strcmp(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS), "0"));
    assert(gui_settings_set(&app->settings, "gamepad_joycon_pair", ".ini"));
    assert(app->pad.own.bit[SNX_PAD_SOUTH] == SNX_PAD_UNSET);
    render(app);
    {
        char start[21]; /* in the tips, the foot's letters */
        snprintf(start, sizeof start, "%s", app->editor.note);
        shows_half(app, start, rgb(app->theme.failure));
    }
    key(app, SDLK_TAB); /* the next screen, as the foot says: the Menu, the pads closed */
    assert(!app->editing && app->menu.view == GUI_LOAD_NONE && !app->pads_read && !SDL_WasInit(SDL_INIT_GAMEPAD));
    /* the games it starts read the mappings file the page names, once it is there; one the player named stays */
    {
        char *folder = snx_dirname(app->settings_file), *db = snx_path_join(folder, "gamecontrollerdb.txt");
        const char* named;
        SDL_unsetenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE");
        pad_mappings_share(app);
        assert(!SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE"));
        assert(!snx_write_file(db, "", 0));
        pad_mappings_share(app);
        named = SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE");
        assert(named && !strcmp(named, db));
        SDL_setenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE", "/elsewhere/mine.txt", 1);
        pad_mappings_share(app);
        assert(!strcmp(SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE"), "/elsewhere/mine.txt"));
        SDL_unsetenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE");
        assert(!snx_remove_tree(db));
        free(db);
        free(folder);
    }
}
/* every section of settings.txt has its group's icon (group_icon, by group_title's name) and every page its row's
   (page_icon), but Controllers' Player, a row that steps the player and opens none: one without is named */
static void icons_named(void) {
    size_t size;
    char* text = (char*)snx_read_file(SNX_GUI_SETTINGS_TXT, &size);
    const char* line;
    int sections = 0, pages = 0;
    assert(text && group_icon("App") != GUI_ICON_NONE);
    icons_are();
    for (line = text; line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        char name[64], rest[256];
        const char* page;
        if (sscanf(line, "section %63[^:]: %255[^\n]", name, rest) != 2) continue;
        if (group_icon(group_title(name)) == GUI_ICON_NONE) {
            fprintf(stderr, "settings.txt's section %s has no icon in main.c's group_icon\n", name);
            assert(!"a group's icon");
        }
        ++sections;
        for (page = strchr(rest, '/'); page; page = strchr(page + 1, '/')) {
            char p[64];
            assert(sscanf(page + 1, "%63[^,\t\n]", p) == 1);
            if (strstr(p, "  ")) *strstr(p, "  ") = 0; /* the section's words after its tables: backend, launcher */
            while (p[0] && p[strlen(p) - 1] == ' ') p[strlen(p) - 1] = 0;
            if (!strcmp(p, "Player")) continue;
            if (page_icon(p) == GUI_ICON_NONE) {
                fprintf(stderr, "settings.txt's page %s of %s has no icon in main.c's page_icon\n", p, name);
                assert(!"a page's icon");
            }
            ++pages;
        }
    }
    assert(sections >= 10 && pages >= 3);
    free(text);
}
/* a running step's line ends in its progress as the theme says, a done one in OK */
static void step_line(void) {
    GuiStep s;
    char runs[3][112];
    size_t lengths[3];
    memset(&s, 0, sizeof s);
    snprintf(s.label, sizeof s.label, "EXTRACT");
    snprintf(s.detail, sizeof s.detail, "Game.xci");
    s.value = 1024;
    s.total = 4096;
    s.megabytes = 1;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!strcmp(runs[0], "EXTRACT:") && !strcmp(runs[1], " GAME.XCI") && !strcmp(runs[2], " 25%"));
    step_runs(&s, GUI_PROGRESS_AMOUNT, 0, runs, lengths);
    assert(!strcmp(runs[2], " 1.0 / 4.0 GB") && lengths[2] == 13);
    s.state = GUI_STEP_DONE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!strcmp(runs[2], "...OK"));
    s.total = 0;
    s.state = GUI_STEP_ACTIVE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!runs[2][0] && !lengths[2]);
    /* a detail of wide letters (tests/po/xx.po's Menu twice) in columns, cut to those left: none crossing them, its
       length the columns it keeps */
    snprintf(s.detail, sizeof s.detail, "\xe8\x8f\x9c\xe5\x8d\x95\xe8\x8f\x9c\xe5\x8d\x95");
    s.state = GUI_STEP_DONE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(lengths[0] == 8 && lengths[1] == 9 && lengths[2] == 5);
    clip_row(runs, lengths, 0, 17); /* 4 columns left for the detail: the second letter would end on the 5th */
    assert(!strcmp(runs[1], " \xe8\x8f\x9c") && lengths[1] == 3 && lengths[0] == 8 && lengths[2] == 5);
}
/* the runtime's list as the menu built into scarletnx reads it, with no program to ask (snx_menu_settings) */
static char* built_in_settings(void) {
    return snx_strdup("Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\n"
                      "Network\tbuilt_in\t0\tint\t0\t1\tOne the program's own list has\t\t0=Off|1=On\n");
}
/* a runtime's list with a Debug row, for the games run with one off its default */
static char* debug_settings(void) {
    return snx_strdup("Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\n"
                      "Debug\tlog\t1\tint\t0\t2\tThe log's detail\tLog level\t0=0|1=1|2=2\n");
}
static int SDLCALL hang(void* unused) {
    (void)unused;
    for (;;) SDL_Delay(100);
}
/* the keys screen's outcome: installed or not, said on it, then the menu once it has shown long enough, not before */
static void keys_outcome(App* app, bool ok) {
    Uint64 at = app->keys_done, shown = ok ? GUI_KEYS_OK_MS : GUI_KEYS_FAILED_MS;
    assert(app->menu.view == GUI_LOAD_KEYS && at && app->keys_ok == ok);
    app->clock = at + shown - 1;
    pump(app);
    render(app);
    shows(app, ok ? "Installed" : "Not installed", rgb(ok ? app->theme.focus : app->theme.failure));
    shows(app, "Back to the menu in 1 s", rgb(app->theme.soft));
    assert(app->menu.view == GUI_LOAD_KEYS);
    app->clock = at + shown;
    pump(app);
    assert(app->menu.view == GUI_LOAD_NONE && !app->keys_done);
    app->clock = 0;
}
/* name set (NULL: unset) where getenv reads it as well as SDL: on Windows the C runtime keeps a copy of its own, which
   SDL_setenv_unsafe leaves naming the real user's folders */
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    assert(!_putenv_s(name, value ? value : ""));
#endif
    assert(!(value ? SDL_setenv_unsafe(name, value, 1) : SDL_unsetenv_unsafe(name)));
}
/* Settings > App's UI scale, its row in focus, made size through its box, then kept or not */
static void pick_size(App* app, int size, bool keep) {
    key(app, SDLK_RETURN);
    assert(app->sizes_shown);
    app->sizes.focus = size < 0 ? -size - 1 : size + 3;
    key(app, SDLK_RETURN);
    assert(app->menu.text_size == size && app->confirm.shown && app->confirm.ask == GUI_ASK_TEXT);
    if (keep) key(app, SDLK_RIGHT);
    key(app, SDLK_RETURN);
    assert(!app->confirm.shown && !app->size_until && app->menu.text_size == (keep ? size : app->size_was));
}
/* what the folders Games lists hold, as the watcher hashes them: a look of its own, here */
static uint64_t stamp_now(App* app) {
    uint64_t h = 0;
    watch_set(app);
    watch_pass(&app->watch);
    assert(watch_get(app, &h));
    return h;
}
/* a JPEG's bytes as snx_splash_jpeg hands them out: a game's icon */
typedef struct {
    unsigned char data[16384];
    int size;
} IconJpeg;
static void icon_jpeg_write(void* closure, void* data, int size) {
    IconJpeg* j = closure;
    assert(j->size + size <= (int)sizeof j->data);
    memcpy(j->data + j->size, data, (size_t)size);
    j->size += size;
}
/* a game of folder made in a folder of its own beside its data, as the recompile lays it out (gui_game_folder): a file
   that runs nothing, which Games lists by its folder's name; its program's path, owned */
static char* folder_game_in(const char* folder, const char* name) {
    char *own = snx_path_join(folder, name), *data = snx_format("%s/%s_data", own, name);
#ifdef _WIN32
    char* program = snx_format("%s/%s.exe", own, name);
#else
    char* program = snx_path_join(own, name);
#endif
    assert(!snx_mkdirs(data) && !snx_write_file(program, "game", 4) && !snx_make_executable(program));
    free(data);
    free(own);
    return program;
}
/* whether Games lists the game shown so, and the heading text so */
static bool games_lists(const App* app, const char* shown) {
    for (size_t i = 0; i < app->games.count; ++i)
        if (!strcmp(app->games.shown[i], shown)) return true;
    return false;
}
/* Games' View box (V): Recent Games and Dumps, on by default, and Sort, by title; each changed by Enter, Left, Right
   or a click, kept in app.ini at once and Games listed again by it; an app.ini without them (an older version's) has
   the defaults, iOS's off; a word it does not know named with the one it keeps */
static void view_box(App* app, const char* temporary) {
    char *was_games = app->games_dir, *was_data = app->data_dir, *was_app = app->app_file;
    char* was_recent_file = app->recent_file;
    GuiRecent was_recent = app->recent;
    int was_folders = app->folder_count;
    char *root = snx_path_join(temporary, "view box"), *dumps = snx_path_join(root, "dumps"), *dump, *game, *kept, *cia;
    char* was_folder = app->folders[0];
    int was_library = app->folders_library[0];
    bool was_deep = app->folders_deep[0];
    char expected[1024], said[1024];
    size_t size = 0;
    app->data_dir = root;
    app->games_dir = snx_path_join(root, "recompiled");
    app->app_file = snx_path_join(root, "app.ini");
    app->recent_file = NULL; /* the list in the menu alone */
    app->folder_count = 0;
    game = folder_game_in(app->games_dir, "alpha game");
    { /* two more, one played after alpha game, one never: Sort's Last opened by their logs */
        char *delta = folder_game_in(app->games_dir, "delta game"),
             *epsilon = folder_game_in(app->games_dir, "epsilon game");
        char* log = snx_format("%s/alpha game/alpha game.log", app->games_dir);
        assert(!snx_write_file(log, "log", 3) && !snx_set_file_time(log, 1000000));
        free(log);
        log = snx_format("%s/delta game/delta game.log", app->games_dir);
        assert(!snx_write_file(log, "log", 3) && !snx_set_file_time(log, 2000000));
        free(log);
        free(epsilon);
        free(delta);
    }
    dump = snx_path_join(dumps, "beta dump.nsp");
    cia = snx_path_join(app->games_dir, "gamma.cia"); /* in the dumps' group, with them */
    assert(!snx_mkdirs(dumps) && !snx_write_file(dump, "dump", 4) && !snx_write_file(cia, "cia", 3));
    app->recent = (GuiRecent){0};
    assert(!gui_recent_add(&app->recent, game, GUI_SWITCH, 0));
    go_tab(app, GUI_TAB_GAMES);
    assert(recent_on(app) && dumps_on(app) && app->browser.head == 1 && games_lists(app, "Dumps") &&
           games_lists(app, "beta dump.nsp") && games_lists(app, "gamma.cia") && games_lists(app, "alpha game"));
    render(app);
    shows(app, "Recent Games", rgb(app->theme.soft));
    shows(app, "dumps", rgb(app->theme.soft)); /* the side box's folders, dumps/ with them, wrapped at a slash */
    shows_foot(app, "V", yellow);              /* its key, then what it does; its word presses V as the key does */
    shows_foot(app, "view", normal);
    assert(hint_keys[hint_pick("V", 0, false)].key == SDLK_V && !hint_keys[hint_pick("V", 0, false)].mod);
    assert(hint_keys[hint_pick("View", 1, false)].key == SDLK_V); /* a phone's: the word alone */
    assert(strstr(screen_said(app, true, said, sizeof said), "V: view."));
    /* V opens the box, its rows the options, a value each between the arrows */
    key(app, SDLK_V);
    assert(app->options_shown && app->options_focus == 0 && app->menu.view == GUI_LOAD_GAMES);
    render(app);
    shows(app, "- Recent Games  < On >", yellow);
    shows(app, "- Dumps         < On >", normal);
    shows(app, "- Sort          < Title >", normal);
    /* Enter: Recent Games off, kept at once, Games without its rows */
    key(app, SDLK_RETURN);
    assert(app->options_shown && !recent_on(app) && app->browser.head == 0 && games_lead(app) == 0);
    kept = (char*)snx_read_file(app->app_file, &size);
    assert(kept && strstr(kept, "\nrecent_games = off\ngames_dumps = on\n"));
    free(kept);
    render(app);
    shows(app, "- Recent Games  < Off >", yellow);
    hides(app, "Recent Games", rgb(app->theme.soft));
    /* Down, then Right: Dumps off, the dumps out of the list and its side box, the groups' headings with them */
    key(app, SDLK_DOWN);
    assert(app->options_focus == 1);
    key(app, SDLK_RIGHT);
    assert(!dumps_on(app) && app->games.count == 3 && games_lists(app, "alpha game") && !games_lists(app, "gamma.cia"));
    /* Down, then Left: Sort by the last opened, the game whose log a run wrote last first, one never run after them */
    assert(!strcmp(app->games.names[0], "alpha game") && !strcmp(app->games.names[1], "delta game") &&
           !strcmp(app->games.names[2], "epsilon game"));
    key(app, SDLK_DOWN);
    key(app, SDLK_LEFT);
    assert(by_last_opened(app) && app->games.count == 3 && !strcmp(app->games.names[0], "delta game") &&
           !strcmp(app->games.names[1], "alpha game") && !strcmp(app->games.names[2], "epsilon game"));
    render(app);
    shows(app, "- Sort          < Last opened >", yellow);
    key(app, SDLK_UP);
    kept = (char*)snx_read_file(app->app_file, &size);
    assert(kept && strstr(kept, "\nrecent_games = off\ngames_dumps = off\ngames_sort = last_opened\n"));
    free(kept);
    key(app, SDLK_ESCAPE); /* the box closed, Games still there */
    assert(!app->options_shown && app->menu.view == GUI_LOAD_GAMES);
    render(app);
    hides(app, "Dumps", rgb(app->theme.soft)); /* a group's heading */
    hides(app, "dumps", rgb(app->theme.soft));
    shows(app, "recompiled", rgb(app->theme.soft));
    /* read back as the menu opens; an app.ini without them, an older version's, has the defaults: on, iOS's off */
    memset(app->options, 0, sizeof app->options);
    app->message[0] = 0;
    app_load(app);
    assert(!recent_on(app) && !dumps_on(app) && by_last_opened(app) && !app->message[0]);
    snprintf(expected, sizeof expected, "[app]\ntheme = %s\n", app->theme.id);
    assert(!snx_write_file(app->app_file, expected, strlen(expected)));
    games_defaults(app, false);
    app_load(app);
    assert(recent_on(app) && dumps_on(app) && !by_last_opened(app) && !app->message[0]);
    assert(app->options[GUI_OPTION_ICONS] == 1); /* Medium */
    games_defaults(app, true);
    app_load(app);
    assert(!recent_on(app) && !dumps_on(app) && !by_last_opened(app) && !app->message[0]);
    /* a word it does not know: named with the word it keeps, the file's last it knows or the default's */
    games_defaults(app, false);
    {
        const char* broken =
            "[app]\nrecent_games = maybe\ngames_dumps = off\ngames_dumps = none\ngames_sort = newest\n";
        assert(!snx_write_file(app->app_file, broken, strlen(broken)));
        app_load(app);
        snprintf(expected, sizeof expected,
                 "Not used from %s: recent_games = maybe (it keeps on), games_dumps = none (it keeps off), "
                 "games_sort = newest (it keeps title)",
                 app->app_file);
        assert(recent_on(app) && !dumps_on(app) && !by_last_opened(app) && !strcmp(app->message, expected));
        broken = "[app]\nrecent_games = maybe\nrecent_games = off\n"; /* the file's own after it, in use after it */
        assert(!snx_write_file(app->app_file, broken, strlen(broken)));
        games_defaults(app, false);
        app->message[0] = 0;
        app_load(app);
        snprintf(expected, sizeof expected, "Not used from %s: recent_games = maybe (it keeps off)", app->app_file);
        assert(!recent_on(app) && dumps_on(app) && !strcmp(app->message, expected));
        broken = "[app]\ngames_icons = large\ngames_icons = huge\n"; /* Icon size's three words */
        assert(!snx_write_file(app->app_file, broken, strlen(broken)));
        games_defaults(app, false);
        app->message[0] = 0;
        app_load(app);
        snprintf(expected, sizeof expected, "Not used from %s: games_icons = huge (it keeps large)", app->app_file);
        assert(app->options[GUI_OPTION_ICONS] == 2 && !strcmp(app->message, expected));
        games_defaults(app, false);
        app->message[0] = 0;
    }
    /* the pointer: over a row its focus, a click changes it; a click off the box closes it */
    list_games(app);
    key(app, SDLK_V);
    {
        GuiViewBoxes d;
        GuiRect dumps_row;
        view_boxes(app, &d);
        dumps_row = ui_row_box(app, ui_view_row(d.rects, app->scale, UI_VIEW_ROWS, 1));
        point(app, dumps_row, false);
        assert(app->options_focus == 1 && dumps_on(app));
        point(app, dumps_row, true);
        assert(app->options_shown && !dumps_on(app) && !games_lists(app, "beta dump.nsp"));
        mouse_px(app, d.box.x + d.box.w + 2, d.box.y + d.box.h + 2, true);
        assert(!app->options_shown);
    }
    /* Dumps off reads no dumps' folder: one that cannot be read is named with them on alone */
    {
        char* gone = snx_path_join(root, "gone dumps");
        app->folders[0] = gone;
        app->folders_library[0] = GUI_LIBRARY_DUMPS;
        app->folders_deep[0] = false;
        app->folder_count = 1;
        games_defaults(app, false);
        list_games(app);
        assert(strstr(app->message, "gone dumps"));
        app->options[GUI_OPTION_DUMPS] = 1;
        list_games(app);
        assert(!strstr(app->message, "gone dumps") && !games_lists(app, "beta dump.nsp"));
        app->folder_count = 0;
        free(gone);
#ifndef _WIN32
        assert(!chmod(dumps, 0)); /* dumps/ itself, where it cannot be read (not as root) */
        {
            char** names = NULL;
            size_t count = 0;
            bool unreadable = snx_list_dir(dumps, &names, &count) != 0;
            if (!unreadable) snx_free_strings(names, count);
            list_games(app);
            assert(!app->message[0]);
            games_defaults(app, false);
            list_games(app);
            assert(!unreadable || strstr(app->message, dumps));
        }
        assert(!chmod(dumps, 0755));
#endif
        app->message[0] = 0;
    }
    games_defaults(app, false);
    app->recent = was_recent;
    app->recent_file = was_recent_file;
    app->folder_count = was_folders;
    app->folders[0] = was_folder;
    app->folders_library[0] = was_library;
    app->folders_deep[0] = was_deep;
    free(app->games_dir);
    free(app->app_file);
    app->games_dir = was_games;
    app->data_dir = was_data;
    app->app_file = was_app;
    go_tab(app, GUI_TAB_MENU);
    app->games_stamp = stamp_now(app);
    app->message[0] = 0;
    assert(!snx_remove_tree(root));
    free(game);
    free(dump);
    free(cia);
    free(dumps);
    free(root);
}
/* the grid's row in focus, as games_boxes has it */
static size_t grid_focused(App* app) {
    GuiGamesBoxes g;
    games_boxes(app, &g);
    return g.grid_lists[UI_GAMES_GRID_TILES].focus;
}
/* where the grid puts row r of a list whose labels are those of label, by the rule ui.txt says, apart from the code
   that places them: a label on a line of its own, the list's width; tiles cols a line, gap apart both ways */
static GuiRect grid_model(const UiFlow* f, const bool* label, size_t r, int* line, int* col) {
    int y = 0, h = 0, l = -1, c = 0;
    for (size_t i = 0; i <= r; ++i) {
        if (label[i] || c >= f->cols || l < 0) { /* a line of its own, a full line or the first */
            if (l >= 0) y += h + f->gap;
            ++l;
            c = 0;
            h = label[i] ? f->row_h : f->tile_h;
        }
        *line = l;
        *col = c;
        if (i == r)
            return label[i] ? gui_rect(f->area.x, f->area.y + y, f->area.w, f->row_h)
                            : gui_rect(f->area.x + c * (f->tile_w + f->gap), f->area.y + y, f->tile_w, f->tile_h);
        c = label[i] ? f->cols : c + 1;
    }
    return gui_rect(0, 0, 0, 0);
}
/* View's Grid: Games' rows as tiles, as many a line as fit, "Recent Games" and the groups' headings each on a line of
   its own; the arrows go to the tile beside, above or under the focus, PgUp and PgDn by the lines that show, the
   pointer takes a tile and a click opens it; the lines scroll to the focus. Kept as games_view = grid */
static void grid_view(App* app, const char* temporary) {
    char *was_games = app->games_dir, *was_data = app->data_dir, *was_app = app->app_file,
         *was_recent_file = app->recent_file;
    GuiRecent was_recent = app->recent;
    int was_folders = app->folder_count, was_size = app->menu.text_size;
    char *root = snx_path_join(temporary, "grid view"), *dumps = snx_path_join(root, "dumps"), *kept;
    char* games[7];
    bool label[16];
    GuiGamesBoxes g;
    UiFlow f;
    GuiRect at;
    size_t size = 0, rows, r;
    int line, col;
    app->data_dir = root;
    app->games_dir = snx_path_join(root, "recompiled");
    app->app_file = snx_path_join(root, "app.ini");
    app->recent_file = NULL;
    app->folder_count = 0;
    app->recent = (GuiRecent){0};
    for (int i = 0; i < 7; ++i) { /* game a to game g; the last five recent, game c the newest */
        char name[16];
        snprintf(name, sizeof name, "game %c", 'a' + i);
        games[i] = folder_game_in(app->games_dir, name);
    }
    { /* game b's icon, which its tile draws over its name */
        IconJpeg jpeg = {{0}, 0};
        char* icon = snx_format("%s/game b/game b_data/icon.jpg", app->games_dir);
        assert(!snx_splash_jpeg("wimsy-fuddles", 32, 90, icon_jpeg_write, &jpeg));
        assert(!snx_write_file(icon, jpeg.data, (size_t)jpeg.size));
        free(icon);
    }
    for (int i = 6; i >= 2; --i) assert(!gui_recent_add(&app->recent, games[i], GUI_SWITCH, 0));
    assert(!snx_mkdirs(dumps));
    for (int i = 0; i < 2; ++i) {
        char* dump = snx_format("%s/dump %c.nsp", dumps, 'x' + i);
        assert(!snx_write_file(dump, "dump", 4));
        free(dump);
    }
    app->menu.text_size = 1; /* a line holds 4 tiles at least */
    layout(app);
    go_tab(app, GUI_TAB_GAMES);
    /* the View box's last row: View, List, then Grid, kept at once */
    key(app, SDLK_V);
    render(app);
    shows(app, "- View          < List >", normal);
    hides(app, "Icon size", normal); /* the grid's alone */
    {
        int shown_rows[GUI_VIEW_BOX_ROWS];
        assert(view_rows(app, shown_rows) == GUI_VIEW_BOX_ROWS - 1 && shown_rows[4] == GUI_VIEW_SIDE);
    }
    for (int i = 0; i < 3; ++i) key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(games_grid(app) && app->options_focus == GUI_OPTION_VIEW);
    kept = (char*)snx_read_file(app->app_file, &size);
    assert(kept && strstr(kept, "\ngames_view = grid\n"));
    free(kept);
    render(app);
    shows(app, "- View          < Grid >", yellow);
    { /* under it, the grid's own, Icon size: Medium, ui.txt's tiles; Right Large, 16 cells by 6 rows and 8px, which the
         grid places, draws and hits; Left back, and again Small, 8 by 3 and 8px; each kept at once */
        GuiGamesBoxes sized;
        UiFlow tiles;
        GuiRect at[3];
        int item;
        size_t hit_row;
        key(app, SDLK_DOWN);
        assert(app->options_focus == GUI_OPTION_ICONS);
        render(app);
        shows(app, "- Icon size     < Medium >", yellow);
        key(app, SDLK_RIGHT);
        render(app);
        shows(app, "- Icon size     < Large >", yellow);
        kept = (char*)snx_read_file(app->app_file, &size);
        assert(kept && strstr(kept, "\ngames_view = grid\ngames_icons = large\n"));
        free(kept);
        games_boxes(app, &sized);
        grid_flow(app, &sized, &tiles);
        assert(tiles.tile_w == 16 * cell_w(app) && tiles.tile_h == 6 * cell_h(app) + 8 * app->scale);
        for (int i = 0; i < 3; ++i) assert(ui_flow_next(&tiles, games_row_label(app, (size_t)i), &at[i]));
        assert(!games_row_label(app, 2) && at[2].w == 16 * cell_w(app));
        assert(ui_games_grid_hit(app, sized.grid_rects, app->scale, sized.grid_lists, at[2].x + at[2].w - 1,
                                 at[2].y + at[2].h / 2, 0, &item, &hit_row) &&
               item == UI_GAMES_GRID_TILES && hit_row == 2);
        key(app, SDLK_LEFT);
        key(app, SDLK_LEFT);
        render(app);
        shows(app, "- Icon size     < Small >", yellow);
        kept = (char*)snx_read_file(app->app_file, &size);
        assert(kept && strstr(kept, "\ngames_icons = small\n"));
        free(kept);
        games_boxes(app, &sized);
        grid_flow(app, &sized, &tiles);
        assert(tiles.tile_w == 8 * cell_w(app) && tiles.tile_h == 3 * cell_h(app) + 8 * app->scale);
        key(app, SDLK_RIGHT); /* Medium again, for what follows */
        assert(app->options[GUI_OPTION_ICONS] == 1);
    }
    { /* under it the Side panel, Settings > App's and F2's: Right the next of Auto, On and Off, kept at once, Left
         back, Down round to the first row */
        int was = app->side_box;
        char row[64], line[64];
        key(app, SDLK_DOWN);
        assert(app->options_focus == GUI_VIEW_SIDE);
        render(app);
        snprintf(row, sizeof row, "- Side panel    < %s >", side_names[was]);
        shows(app, row, yellow);
        key(app, SDLK_RIGHT);
        assert(app->side_box == (was + 1) % 3);
        kept = (char*)snx_read_file(app->app_file, &size);
        snprintf(line, sizeof line, "\nside_box = %s\n", side_words[(was + 1) % 3]);
        assert(kept && strstr(kept, line));
        free(kept);
        key(app, SDLK_LEFT);
        assert(app->side_box == was);
        key(app, SDLK_DOWN);
        assert(app->options_focus == 0);
        key(app, SDLK_UP);
    }
    key(app, SDLK_ESCAPE);
    /* one page, all of it; the rows: Recent Games, three recent, Show More, Recompiled, seven games, Dumps, two */
    rows = games_rows(app);
    assert(rows == 16 && app->browser.page_size == app->games.count && gui_browser_pages(&app->browser) == 1);
    for (r = 0; r < rows; ++r) label[r] = games_row_label(app, r);
    assert(label[0] && !label[1] && !label[4] && label[5] && !label[6] && label[13] && !label[15]);
    games_boxes(app, &g);
    ui_games_grid_flow(g.grid_rects, app->scale, UI_GAMES_GRID_TILES, &f);
    assert(f.cols >= 4 && f.tile_w == 12 * cell_w(app) && f.tile_h == 4 * cell_h(app) + 8 * app->scale &&
           f.gap == 8 * app->scale && f.row_h == item_h(app));
    { /* the grid in the wide list's place: three fifths of the screen, the side box the rest */
        GuiPlace p;
        place_screen(app, &p, "", GUI_SHOWS_STATUS | GUI_SHOWS_TABS | GUI_SHOWS_SIDE | GUI_SHOWS_WIDE);
        assert(!memcmp(&g.grid_rects[UI_GAMES_GRID_TILES], &p.list, sizeof p.list));
    }
    /* each row where the rule puts it: the pointer over its middle finds it, and none between two tiles */
    for (r = 0; r < rows; ++r) {
        int item;
        size_t hit_row;
        at = grid_model(&f, label, r, &line, &col);
        assert(ui_games_grid_hit(app, g.grid_rects, app->scale, g.grid_lists, at.x + at.w / 2, at.y + at.h / 2, 0,
                                 &item, &hit_row) &&
               item == UI_GAMES_GRID_TILES && hit_row == r);
    }
    at = grid_model(&f, label, 1, &line, &col);
    {
        int item;
        size_t hit_row;
        assert(!ui_games_grid_hit(app, g.grid_rects, app->scale, g.grid_lists, at.x + at.w + f.gap / 2, at.y + at.h / 2,
                                  0, &item, &hit_row));
    }
    /* drawn there: the label as a row's, a tile's name under its icon, in the middle */
    render(app);
    {
        int x, y;
        GuiRect label_at = grid_model(&f, label, 0, &line, &col), tile = grid_model(&f, label, 7, &line, &col);
        assert(find_text(app, "Recent Games", rgb(app->theme.soft), 1, &x, &y) && y >= label_at.y &&
               y < label_at.y + label_at.h);
        assert(find_text(app, "game b", normal, 1, &x, &y) && x == tile.x + (tile.w - 6 * cell_w(app)) / 2 &&
               y > tile.y + tile.h / 2 && y + cell_h(app) <= tile.y + tile.h);
        assert(find_text(app, "game c", yellow, 1, &x, &y)); /* the focus: the newest recent game */
        shows(app, "9 game(s)", rgb(app->theme.soft));       /* one page, no page's number */
        hides(app, "Page 1/1", rgb(app->theme.soft));
        { /* its icon a square in the middle of its top, over the tile's own colour */
            int side = SDL_min(tile.w - 4 * app->scale, tile.h - cell_h(app) - 6 * app->scale);
            SDL_Color back = pixel(app, (float)(tile.x + 2), (float)(tile.y + tile.h / 2));
            SDL_Color in = pixel(app, (float)(tile.x + tile.w / 2), (float)(tile.y + 2 * app->scale + side / 2));
            SDL_Color beside =
                pixel(app, (float)(tile.x + (tile.w - side) / 2 - 2), (float)(tile.y + 2 * app->scale + side / 2));
            assert(!same_colour(in, back) && same_colour(beside, back));
        }
    }
    { /* F1's box says the grid's keys: the arrows go over the tiles, PgUp and PgDn by the lines */
        char said[1024];
        assert(strstr(screen_said(app, true, said, sizeof said), "Arrows: select.") &&
               strstr(said, "PgUp/PgDown: page.") && !strstr(said, "Left/Right/PgUp/PgDown: page.") &&
               strstr(said, "V: view."));
    }
    /* Right and Left: the row after and before, past a label; nothing past the first */
    assert(grid_focused(app) == 1);
    key(app, SDLK_LEFT);
    assert(grid_focused(app) == 1);
    for (int i = 0; i < 4; ++i) key(app, SDLK_RIGHT);
    assert(grid_focused(app) == 6); /* past Show More, Recompiled */
    key(app, SDLK_LEFT);
    assert(grid_focused(app) == 4);
    /* Down and Up: the tile under and over, its column or its line's last; nothing past the first line */
    key(app, SDLK_LEFT);
    key(app, SDLK_LEFT);
    assert(grid_focused(app) == 2);
    key(app, SDLK_UP);
    assert(grid_focused(app) == 2);
    { /* the next tiles' line, its tile at the column or the line's last */
        int from_line, from_col, l, c, next = -1;
        size_t want = rows;
        grid_model(&f, label, 2, &from_line, &from_col);
        for (r = 0; r < rows && next < 0; ++r) {
            grid_model(&f, label, r, &l, &c);
            if (!label[r] && l > from_line) next = l;
        }
        for (r = 0; r < rows; ++r) {
            grid_model(&f, label, r, &l, &c);
            if (!label[r] && l == next && (want == rows || c <= from_col)) want = r;
        }
        key(app, SDLK_DOWN);
        assert(grid_focused(app) == want && want == 7);
        key(app, SDLK_UP);
        assert(grid_focused(app) == 2);
    }
    /* PgDn: as many lines as show, here more than there are: the last line's, at its column or the last */
    key(app, SDLK_PAGEDOWN);
    assert(grid_focused(app) == 15);
    key(app, SDLK_PAGEDOWN);
    assert(grid_focused(app) == 15);
    key(app, SDLK_PAGEUP);
    assert(grid_focused(app) == 2);
    /* Enter on a recent game runs it as the list's does; the pointer over a tile takes the focus, a click opens it:
       Show More, every recent game */
    {
        GuiRect more = grid_model(&f, label, 4, &line, &col), game = grid_model(&f, label, 9, &line, &col);
        point(app, game, false);
        assert(grid_focused(app) == 9 && !app->menu.running);
        point(app, more, true);
        assert(app->recent_all && grid_focused(app) == 4 && games_rows(app) == 17);
        app->recent_all = false;
        list_games(app);
    }
    /* the lines from the first shown: a list exactly as tall as those from Recompiled's to the last game's shows them
       all, from Recompiled's, with the focus on the last */
    {
        GuiRect from = grid_model(&f, label, 5, &line, &col), last = grid_model(&f, label, rows - 1, &line, &col);
        games_boxes(app, &g);
        g.grid_rects[UI_GAMES_GRID_TILES].h = last.y + last.h - from.y;
        assert(grid_first(app, &g, rows - 1) == 5);
        g.grid_rects[UI_GAMES_GRID_TILES].h -= 1; /* a pixel less: from the line after it */
        assert(grid_first(app, &g, rows - 1) == 6);
    }
    /* a label first or last of the rows shown has the arrows of those hidden, as a row has: the grid's lines end on a
       heading as often as on a tile */
    {
        GuiRect b = gui_rect(0, 0, 30 * cell_w(app), item_h(app)), box;
        SDL_Color page = rgb(app->theme.page), soft = rgb(app->theme.soft);
        SDL_SetRenderDrawColor(app->renderer, page.r, page.g, page.b, 255);
        SDL_RenderClear(app->renderer);
        ui_draw_row(app, b, "Dumps", 0, NULL, 0, UI_ATTR_LABEL | UI_ATTR_MORE_DOWN, false);
        box = ui_row_box(app, b);
        text_px_is(app, box.x + box.w - cell_w(app), ui_text_y(app, box), "\x1f", soft, page);
        SDL_RenderClear(app->renderer);
        ui_draw_row(app, b, "Dumps", 0, NULL, 0, UI_ATTR_LABEL | UI_ATTR_MORE_UP | UI_ATTR_MORE_DOWN, false);
        text_px_is(app, box.x + box.w - 2 * cell_w(app), ui_text_y(app, box), "\x1e\x1f", soft, page);
    }
    /* big letters: the lines that show fewer than there are; the focus on the last game, the lines from a line's
       first so it shows, the first lines scrolled out */
    app->menu.text_size = 4;
    layout(app);
    app->browser.selected = app->browser.head + app->games.count - 1;
    games_boxes(app, &g);
    {
        size_t first = g.grid_lists[UI_GAMES_GRID_TILES].first, focus = g.grid_lists[UI_GAMES_GRID_TILES].focus;
        bool placed = false;
        ui_games_grid_flow(g.grid_rects, app->scale, UI_GAMES_GRID_TILES, &f);
        assert(focus == rows - 1 && first > 0);
        for (r = first; r <= focus && ui_flow_next(&f, games_row_label(app, r), &at); ++r) placed = r == focus;
        assert(placed); /* the focus fits */
        ui_games_grid_flow(g.grid_rects, app->scale, UI_GAMES_GRID_TILES, &f);
        f.area.h = 1 << 30; /* the rows before it, placed as though they showed */
        f.max = rows;
        for (r = 0; r < first; ++r) assert(ui_flow_next(&f, games_row_label(app, r), &at));
        assert(f.col >= f.cols || games_row_label(app, first)); /* a line's first: the one before ends its line */
    }
    /* the pointer on a tile of a line that shows moves no line, where a key puts the focus's line in the middle: at a
       size five lines show at, the focus on a game by the keys, the last tile shown hovered, then Left and Right back
       to it */
    app->menu.text_size = 2;
    layout(app);
    app->browser.selected = app->browser.head + 4;
    key(app, SDLK_RIGHT);
    key(app, SDLK_LEFT);
    games_boxes(app, &g);
    {
        size_t first = g.grid_lists[UI_GAMES_GRID_TILES].first, focus = g.grid_lists[UI_GAMES_GRID_TILES].focus,
               tile = rows;
        int focus_line = -1, tile_line = -1;
        GuiRect last = {0};
        ui_games_grid_flow(g.grid_rects, app->scale, UI_GAMES_GRID_TILES, &f);
        for (r = first; r < rows && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {
            if (r == focus) focus_line = f.line;
            if (!games_row_label(app, r)) {
                tile = r;
                tile_line = f.line;
                last = at;
            }
        }
        assert(first > 0 && focus_line >= 0 && tile < rows && tile_line > focus_line);
        point(app, last, false);
        games_boxes(app, &g);
        assert(grid_focused(app) == tile && g.grid_lists[UI_GAMES_GRID_TILES].first == first);
        key(app, SDLK_LEFT);
        key(app, SDLK_RIGHT);
        games_boxes(app, &g);
        assert(grid_focused(app) == tile && g.grid_lists[UI_GAMES_GRID_TILES].first > first);
    }
    app->menu.text_size = was_size;
    games_defaults(app, false);
    app->recent = was_recent;
    app->recent_file = was_recent_file;
    app->folder_count = was_folders;
    free(app->games_dir);
    free(app->app_file);
    app->games_dir = was_games;
    app->data_dir = was_data;
    app->app_file = was_app;
    layout(app);
    go_tab(app, GUI_TAB_MENU);
    app->games_stamp = stamp_now(app);
    app->message[0] = 0;
    assert(!snx_remove_tree(root));
    for (int i = 0; i < 7; ++i) free(games[i]);
    free(dumps);
    free(root);
}
/* the system's picker's own answer to the box's Add row, which with no picker here is that it could not open: waited
   for (on Windows it comes from the dialog's thread), then dropped, or taken as the menu takes it */
static void addon_own_answer(App* app, bool take) {
    SDL_Event own;
    Uint64 until = SDL_GetTicks() + 2000;
    int got = 0;
    assert(app->addon_picked && app->addon_adding);
    while (!got && SDL_GetTicks() < until) {
        got = SDL_PeepEvents(&own, 1, SDL_GETEVENT, app->addon_picked, app->addon_picked) == 1;
        if (!got) SDL_Delay(1);
    }
    assert(got && own.user.code && !own.user.data1);
    if (take)
        event(app, &own);
    else {
        SDL_free(own.user.data1);
        SDL_free(own.user.data2);
    }
}
/* what the picker answers the Add row pressed with, its own answer dropped first (addon_answer_is): path, NULL for
   none picked */
static void addon_answer(App* app, const char* path) {
    SDL_Event e = {0};
    e.type = app->addon_picked;
    e.user.data1 = path ? SDL_strdup(path) : NULL;
    event(app, &e);
}
static void addon_answer_is(App* app, const char* path) {
    addon_own_answer(app, false);
    addon_answer(app, path);
}
/* the folder at path, made with a file at its relative file holding text */
static void fixture_file(const char* path, const char* file, const char* text) {
    char *at = snx_path_join(path, file), *folder = snx_dirname(at);
    assert(!snx_mkdirs(folder) && !snx_write_file(at, text, strlen(text)));
    free(folder);
    free(at);
}
/* file at path has text */
static bool file_is(const char* path, const char* file, const char* text) {
    char* at = snx_path_join(path, file);
    size_t size = 0;
    char* data = (char*)snx_read_file(at, &size);
    bool same = data && size == strlen(text) && !memcmp(data, text, size);
    free(data);
    free(at);
    return same;
}
/* the box M opened on a game with nothing beside it: Add Mod first, in focus, then its folder's Open, Add Cheats and its
   folder's Open, Save and Play. Add
   Mod opens the folder picker; the folder it gives is copied whole into the game's mods folder by its name, checked
   in the box and counted on Games, the original left as it is. Each folder it refuses is said with its path or name,
   and nothing is copied: no mods folder is even made until a mod goes in. ESC on the box while the picker is open
   leaves its answer nowhere to go, which says so. The box is closed at the end */
static void add_mod(App* app, const char* temporary) {
    char *root = snx_path_join(temporary, "add mod"), *mods = snx_strdup(app->mods_folder), *data = snx_dirname(mods);
    char *fonts = snx_path_join(root, "Better Fonts"), *first = snx_path_join(root, "A first");
    char *bare = snx_path_join(root, "Bare"), *hidden = snx_path_join(root, ".hidden"),
         *file = snx_path_join(root, "x");
    char *to = snx_path_join(mods, "Better Fonts"), *to_first = snx_path_join(mods, "A first"), want[2048];
    const char* cloud = "content://com.example.cloud.documents/tree/abc%3Amods";
    static const int tail[] = {ROW_ADD_MOD, ROW_MODS_FOLDER, ROW_ADD_CHEATS, ROW_CHEATS_FOLDER, ROW_SAVE, ROW_PLAY};
    size_t index = 0;
    int i, nx, ny;
    while (index < app->games.count &&
           !(app->games.paths[index] && snx_same_file(app->games.paths[index], app->addons_game)))
        ++index;
    assert(index < app->games.count && !app->games.addons[index]);
    fixture_file(fonts, "romfs/font/a.bin", "font bytes");
    fixture_file(fonts, "romfs/readme.txt", "read me");
    assert(!snx_mkdirs(first) && !snx_path_exists(mods));
    {
        char* exefs = snx_path_join(first, "exefs"); /* an empty exefs/ is a mod's all the same */
        assert(!snx_mkdirs(exefs));
        free(exefs);
    }
    fixture_file(bare, "notes.txt", "no exefs, no romfs");
    fixture_file(hidden, "romfs/a", "hidden");
    assert(!snx_write_file(file, "a file", 6));
    assert(addons_rows(app) == ADDONS_TAIL && ADDONS_TAIL == (int)SDL_arraysize(tail) && !app->addons_focus);
    for (i = 0; i < ADDONS_TAIL; ++i) assert(addons_row(app, i) == tail[i]);
    render(app);
    assert(find_text(app, "Add Mod", yellow, 1, &nx, &ny));
    assert(find_text(app, "Open Mods Folder", rgb(app->theme.ink), 1, &nx, &ny));
    /* the picker that cannot open says why, in the failure's box over the box */
    key(app, SDLK_RETURN);
    assert(app->addon_adding == ROW_ADD_MOD && !strcmp(app->adding_game, app->addons_game));
    addon_own_answer(app, true);
    assert(!app->addon_adding && !app->adding_game && app->addons_shown);
    assert(!strncmp(app->message, "Add Mod: the folder picker could not open: ", 43) && app->message[43]);
    assert(app->confirm.shown && app->confirm.ask == GUI_ASK_ERROR && !strcmp(app->error_line, app->message));
    key(app, SDLK_ESCAPE);
    assert(!app->confirm.shown && app->addons_shown);
    /* none picked */
    key(app, SDLK_RETURN);
    addon_answer_is(app, NULL);
    assert(!strcmp(app->message, "Add Mod: no folder was picked.") && !app->addon_adding);
    /* a phone's folder of an app or a cloud, which no path reaches */
    key(app, SDLK_RETURN);
    addon_answer_is(app, cloud);
    snprintf(want, sizeof want, "Not added: %s is not a folder of this device's storage", cloud);
    assert(!strcmp(app->message, want));
    /* a file */
    key(app, SDLK_RETURN);
    addon_answer_is(app, file);
    snprintf(want, sizeof want, "Not added: %s is not a folder", file);
    assert(!strcmp(app->message, want));
    /* a folder with neither exefs/ nor romfs/ */
    key(app, SDLK_RETURN);
    addon_answer_is(app, bare);
    snprintf(want, sizeof want, "Not added: %s has no exefs/ or romfs/ folder, which a mod is made of", bare);
    assert(!strcmp(app->message, want));
    /* one whose name the mods list hides */
    key(app, SDLK_RETURN);
    addon_answer_is(app, hidden);
    assert(!strcmp(app->message,
                   "Not added: the mod would be named .hidden, and the mods list hides a name that starts with a dot"));
    /* the game's data folder, which holds the mods folder */
    key(app, SDLK_RETURN);
    addon_answer_is(app, data);
    snprintf(want, sizeof want, "Not added: %s holds the mods folder %s, which it would copy into itself", data, mods);
    assert(!strcmp(app->message, want));
    assert(!snx_path_exists(mods) && !app->mods_count && app->addons_shown && !app->confirm.shown);
    /* a mod: copied whole, the original as it was; checked, the focus still on Add Mod, a row down; Games counts it */
    key(app, SDLK_RETURN);
    addon_answer_is(app, fonts);
    snprintf(want, sizeof want, "Mod Better Fonts added: %s", to);
    assert(!strcmp(app->message, want) && !app->confirm.shown);
    assert(app->mods_count == 1 && !strcmp(app->mods[0], "Better Fonts") && app->mods_on[0]);
    assert(addons_rows(app) == ADDONS_TAIL + 1 && app->addons_focus == 1 && addons_row(app, 1) == ROW_ADD_MOD);
    assert(file_is(to, "romfs/font/a.bin", "font bytes") && file_is(to, "romfs/readme.txt", "read me"));
    assert(file_is(fonts, "romfs/font/a.bin", "font bytes") && file_is(fonts, "romfs/readme.txt", "read me"));
    {
        char* staged = snx_path_join(mods, ".Better Fonts.adding");
        assert(!snx_path_exists(staged));
        free(staged);
    }
    assert(app->games.addons[index] == 1);
    render(app);
    assert(find_text(app, "[x] Mod Better Fonts", rgb(app->theme.ink), 1, &nx, &ny));
    assert(find_text(app, "Add Mod", yellow, 1, &nx, &ny));
    /* the same name again, and a mod of the mods folder itself */
    key(app, SDLK_RETURN);
    addon_answer_is(app, fonts);
    snprintf(want, sizeof want, "Not added: the mods folder has Better Fonts already: %s", to);
    assert(!strcmp(app->message, want));
    key(app, SDLK_RETURN);
    addon_answer_is(app, to);
    snprintf(want, sizeof want, "Not added: %s is in the mods folder already, %s", to, mods);
    assert(!strcmp(app->message, want) && app->mods_count == 1);
    /* another, named before it, over what a copy that stopped left of it: the list read again keeps Better Fonts as the
       box has it (off), the new one checked, and nothing of the stopped copy in it */
    {
        char* stopped = snx_path_join(mods, ".A first.adding");
        fixture_file(stopped, "romfs/half.bin", "half");
        free(stopped);
    }
    key(app, SDLK_UP);
    key(app, SDLK_RETURN);
    assert(!app->mods_on[0] && app->addons_focus == 0);
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    addon_answer_is(app, first);
    snprintf(want, sizeof want, "Mod A first added: %s", to_first);
    assert(!strcmp(app->message, want) && app->mods_count == 2 && !strcmp(app->mods[0], "A first") &&
           !strcmp(app->mods[1], "Better Fonts") && app->mods_on[0] && !app->mods_on[1]);
    assert(app->addons_focus == 2 && addons_row(app, 2) == ROW_ADD_MOD && app->games.addons[index] == 2);
    {
        char *exefs = snx_path_join(to_first, "exefs"), *half = snx_path_join(to_first, "romfs"),
             *staged = snx_path_join(mods, ".A first.adding");
        assert(snx_is_dir(exefs) && !snx_path_exists(half) && !snx_path_exists(staged));
        free(staged);
        free(half);
        free(exefs);
    }
#ifndef _WIN32
    { /* a link in the folder: refused, said in the failure's box, nothing of it left in the mods folder */
        char *linked = snx_path_join(root, "Linked"), *link = snx_path_join(linked, "romfs/link");
        char *gone = snx_path_join(mods, "Linked"), *staged = snx_path_join(mods, ".Linked.adding");
        fixture_file(linked, "romfs/a.bin", "a");
        assert(!snx_symlink(fonts, link));
        key(app, SDLK_RETURN);
        addon_answer_is(app, linked);
        snprintf(want, sizeof want, "Not added: %s is a link, which a mod cannot hold", link);
        assert(!strcmp(app->message, want) && app->confirm.shown && !strcmp(app->error_line, want));
        key(app, SDLK_ESCAPE);
        assert(!snx_path_exists(gone) && !snx_path_exists(staged) && app->mods_count == 2 && app->addons_shown);
        free(staged);
        free(gone);
        free(link);
        free(linked);
    }
    { /* a file the system does not let it read: its path and the system's reason, nothing left */
        char *locked = snx_path_join(root, "Locked"), *secret = snx_path_join(locked, "romfs/secret.bin");
        char *staged = snx_path_join(mods, ".Locked.adding"), *gone = snx_path_join(mods, "Locked");
        FILE* f;
        fixture_file(locked, "romfs/secret.bin", "secret");
        assert(!chmod(secret, 0));
        f = fopen(secret, "rb");
        if (f) /* root reads it all the same: a container's */
            fclose(f);
        else {
            char* into = snx_path_join(staged, "romfs/secret.bin");
            snprintf(want, sizeof want, "Not added: cannot copy %s to %s: %s", secret, into, strerror(EACCES));
            key(app, SDLK_RETURN);
            addon_answer_is(app, locked);
            assert(!strcmp(app->message, want) && app->confirm.shown);
            key(app, SDLK_ESCAPE);
            assert(!snx_path_exists(gone) && !snx_path_exists(staged) && app->mods_count == 2);
            free(into);
        }
        assert(!chmod(secret, 0600));
        free(gone);
        free(staged);
        free(secret);
        free(locked);
    }
#endif
    /* the box closed while the picker is open: its answer is not added */
    key(app, SDLK_RETURN);
    addon_own_answer(app, false);
    key(app, SDLK_ESCAPE);
    assert(!app->addons_shown && app->addon_adding == ROW_ADD_MOD);
    addon_answer(app, fonts);
    snprintf(want, sizeof want, "Not added: %s came back after the box it was picked for closed", fonts);
    assert(!strcmp(app->message, want) && !app->addon_adding && !app->adding_game);
    /* the mods the test added taken away, Games' count with them */
    assert(!snx_remove_tree(mods));
    app->games.addons[index] = 0;
    app->message[0] = 0;
    assert(!snx_remove_tree(root));
    free(to_first);
    free(to);
    free(file);
    free(hidden);
    free(bare);
    free(first);
    free(fonts);
    free(data);
    free(mods);
    free(root);
}
/* a module's head at path, as far as the build ID the runtime names a cheats file by: an NSO's (NSO0 at 0), or an
   NRO's (NRO0 at 0x10), the ID's first 8 bytes at 0x40 those of id, 16 hex digits */
static void fixture_module(const char* path, const char* id, bool nro) {
    unsigned char head[0x60] = {0};
    char* folder = snx_dirname(path);
    for (int i = 0; i < 8; ++i) {
        char byte[3] = {id[2 * i], id[2 * i + 1], 0};
        head[0x40 + i] = (unsigned char)strtoul(byte, NULL, 16);
    }
    memcpy(head + (nro ? 0x10 : 0), nro ? "NRO0" : "NSO0", 4);
    assert(!snx_mkdirs(folder) && !snx_write_file(path, head, sizeof head));
    free(folder);
}
/* the build ID Add Cheats names a file by, as the box of the game whose data folders dirs has (its own, Update 1.2.0's,
   Update 1.0.0's, a DLC's) is set: the game's own main; the picked update's from its extraction, none for one read from
   its dump (no exefs/ of its own), never the game's then; a played mod's exefs/main over both, one turned off not. The
   files made here taken away after */
static void build_ids(App* app, char** dirs) {
    char *own = snx_path_join(dirs[0], "exefs/main"), *newer = snx_path_join(dirs[1], "exefs/main");
    char *older = snx_path_join(dirs[2], "exefs/main"), *mod = snx_path_join(app->mods_folder, "B mod/exefs/main");
    char id[17], *file;
    char *exefs_own = snx_dirname(own), *exefs_newer = snx_dirname(newer);
    assert(!snx_path_exists(own) && !snx_path_exists(newer) && !strcmp(app->mods[1], "B mod") && !app->mods_on[1]);
    assert(!app->addons_checked[0] && !app->addons_checked[1]);       /* No update */
    assert(!addons_build_id(app, id, &file) && same_path(file, own)); /* none made yet: the first looked for */
    free(file);
    fixture_module(own, "0100000000000001", false);
    fixture_module(newer, "0100000000000002", false);
    fixture_module(mod, "0100000000000003", false);
    assert(addons_build_id(app, id, &file) && !strcmp(id, "0100000000000001") && same_path(file, own));
    free(file);
    app->addons_checked[0] = true;
    assert(addons_build_id(app, id, &file) && !strcmp(id, "0100000000000002") && same_path(file, newer));
    free(file);
    app->addons_checked[0] = false;
    app->addons_checked[1] = true;
    assert(!addons_build_id(app, id, &file) && same_path(file, older));
    free(file);
    app->mods_on[1] = true;
    assert(addons_build_id(app, id, &file) && !strcmp(id, "0100000000000003") && same_path(file, mod));
    free(file);
    app->mods_on[1] = false;
    app->addons_checked[1] = false;
    { /* an NRO's main.nro, the game's own; a file that is no module, none */
        char* nro = snx_path_join(dirs[0], "exefs/main.nro");
        assert(!snx_remove_tree(own));
        fixture_module(nro, "0500000000000ABC", true);
        assert(addons_build_id(app, id, &file) && !strcmp(id, "0500000000000ABC") && same_path(file, nro));
        free(file);
        assert(
            !snx_write_file(nro, "no module at all, past 0x48 bytes of it......................................", 76));
        assert(!addons_build_id(app, id, &file) && same_path(file, own));
        free(file);
        free(nro);
    }
    assert(!snx_remove_tree(exefs_own) && !snx_remove_tree(exefs_newer) && !snx_remove_tree(mod));
    free(exefs_newer);
    free(exefs_own);
    free(mod);
    free(older);
    free(newer);
    free(own);
}
/* Add Cheats on the box M opened on a game with nothing beside it, the row after Open Mods Folder: the file picker; the
   .txt it gives, a cheat in it at least, copied into the cheats folder as <build ID>.txt, the name the runtime reads,
   its cheats checked in the box and counted on Games, the original left as it is. A file already named so keeps its
   name, in capitals; another takes the build ID of the main module the game plays with, and without one is refused
   naming the module looked for. Each file it refuses is said with its path or name, and nothing is written. The box
   is closed at the end, the files the test made taken away */
static void add_cheats(App* app, const char* temporary) {
    char *root = snx_path_join(temporary, "add cheats"), *cheats = snx_strdup(app->cheats_folder);
    char *data = snx_dirname(cheats), *main_file = snx_path_join(data, "exefs/main"), *exefs = snx_dirname(main_file);
    char *named = snx_path_join(root, "0123456789abcdef.txt"), *mine = snx_path_join(root, "my cheats.txt");
    char *other = snx_path_join(root, "1111111111111111.TXT"), *empty = snx_path_join(root, "empty.txt");
    char *cht = snx_path_join(root, "cheats.cht"), *big = snx_path_join(root, "big.txt");
    char *to = snx_path_join(cheats, "0123456789ABCDEF.txt"), *to_mine = snx_path_join(cheats, "FEDCBA9876543210.txt");
    char *to_other = snx_path_join(cheats, "1111111111111111.txt"), want[2048];
    const char* cloud = "content://com.example.cloud.documents/document/abc%3Acheats.txt";
    const char* named_text = "[60 FPS]\n04000000 0 1\n[Moon Jump]\n04000000 4 2\n";
    const char* mine_text = "; my own\n[Infinite HP]\n04000000 8 3\n[60 FPS]\n04000000 0 1\n";
    size_t index = 0;
    int nx, ny;
    while (index < app->games.count &&
           !(app->games.paths[index] && snx_same_file(app->games.paths[index], app->addons_game)))
        ++index;
    assert(index < app->games.count && !app->games.addons[index] && !snx_path_exists(cheats));
    assert(!snx_mkdirs(root) && !snx_write_file(named, named_text, strlen(named_text)));
    assert(!snx_write_file(mine, mine_text, strlen(mine_text)));
    assert(!snx_write_file(other, "[Speed]\n04000000 c 4\n", 21));
    assert(!snx_write_file(empty, "; no cheat here\n04000000 0 1\n", 29));
    assert(!snx_write_file(cht, "[60 FPS]\n", 9));
    {
        char* bytes = (char*)calloc(SNX_GUI_CHEATS_MAX + 1, 1);
        assert(bytes);
        memcpy(bytes, "[Big]\n", 6);
        assert(!snx_write_file(big, bytes, SNX_GUI_CHEATS_MAX + 1));
        free(bytes);
    }
    assert(addons_row(app, 2) == ROW_ADD_CHEATS && !app->addons_focus);
    key(app, SDLK_DOWN);
    key(app, SDLK_DOWN);
    render(app);
    assert(find_text(app, "Add Cheats", yellow, 1, &nx, &ny));
    /* the picker that cannot open says why, in the failure's box over the box */
    key(app, SDLK_RETURN);
    assert(app->addon_adding == ROW_ADD_CHEATS && !strcmp(app->adding_game, app->addons_game));
    addon_own_answer(app, true);
    assert(!app->addon_adding && app->addons_shown);
    assert(!strncmp(app->message, "Add Cheats: the file picker could not open: ", 44) && app->message[44]);
    assert(app->confirm.shown && app->confirm.ask == GUI_ASK_ERROR && !strcmp(app->error_line, app->message));
    key(app, SDLK_ESCAPE);
    assert(!app->confirm.shown && app->addons_shown && app->addons_focus == 2);
    /* none picked; a phone's document of an app or a cloud; a folder; not a .txt; past 1 MiB; no cheat in it */
    key(app, SDLK_RETURN);
    addon_answer_is(app, NULL);
    assert(!strcmp(app->message, "Add Cheats: no file was picked."));
    key(app, SDLK_RETURN);
    addon_answer_is(app, cloud);
    snprintf(want, sizeof want, "Not added: %s is not a file of this device's storage", cloud);
    assert(!strcmp(app->message, want));
    key(app, SDLK_RETURN);
    addon_answer_is(app, root);
    snprintf(want, sizeof want, "Not added: %s is not a file", root);
    assert(!strcmp(app->message, want));
    key(app, SDLK_RETURN);
    addon_answer_is(app, cht);
    snprintf(want, sizeof want, "Not added: %s is not a .txt file, which cheats are", cht);
    assert(!strcmp(app->message, want));
    key(app, SDLK_RETURN);
    addon_answer_is(app, big);
    snprintf(want, sizeof want,
             "Not added: %s is 1048577 bytes, and a cheats file is text of 1048576 bytes (1 MiB) at most", big);
    assert(!strcmp(app->message, want));
    key(app, SDLK_RETURN);
    addon_answer_is(app, empty);
    snprintf(want, sizeof want, "Not added: %s has no cheat, no line that starts with [name]", empty);
    assert(!strcmp(app->message, want));
    /* a name that is no build ID, and none to read: the game's data has no main module */
    key(app, SDLK_RETURN);
    addon_answer_is(app, mine);
    snprintf(want, sizeof want,
             "Not added: my cheats.txt is not named <build ID>.txt, and the build ID of the main module the game plays "
             "with cannot be read from %s",
             main_file);
    assert(!strcmp(app->message, want) && !snx_path_exists(cheats) && !app->cheats_count);
    /* named by its build ID: copied by that name in capitals, its cheats checked, the focus still on Add Cheats */
    key(app, SDLK_RETURN);
    addon_answer_is(app, named);
    snprintf(want, sizeof want, "Added %s: 2 cheats", to);
    assert(!strcmp(app->message, want) && !app->confirm.shown);
    assert(app->cheats_count == 2 && !strcmp(app->cheats[0], "60 FPS") && !strcmp(app->cheats[1], "Moon Jump") &&
           app->cheats_on[0] && app->cheats_on[1]);
    assert(addons_rows(app) == ADDONS_TAIL + 2 && app->addons_focus == 4 && addons_row(app, 4) == ROW_ADD_CHEATS);
    assert(file_is(cheats, "0123456789ABCDEF.txt", named_text) && file_is(root, "0123456789abcdef.txt", named_text));
    {
        char* staged = snx_path_join(cheats, ".0123456789ABCDEF.txt.adding");
        assert(!snx_path_exists(staged));
        free(staged);
    }
    assert(app->games.addons[index] == 2);
    render(app);
    assert(find_text(app, "[x] Cheat Moon Jump", rgb(app->theme.ink), 1, &nx, &ny));
    assert(find_text(app, "Add Cheats", yellow, 1, &nx, &ny));
    /* that name again: never added to */
    key(app, SDLK_RETURN);
    addon_answer_is(app, named);
    snprintf(want, sizeof want, "Not added: the cheats folder has 0123456789ABCDEF.txt already: %s", to);
    assert(!strcmp(app->message, want) && file_is(cheats, "0123456789ABCDEF.txt", named_text));
    /* with the game's main module there, a file of another name takes its build ID; the list read again keeps 60 FPS
       as the box has it (off), Infinite HP checked */
    fixture_module(main_file, "FEDCBA9876543210", false);
    for (int i = 0; i < 4; ++i) key(app, SDLK_UP);
    key(app, SDLK_RETURN);
    assert(!app->cheats_on[0] && app->addons_focus == 0);
    for (int i = 0; i < 4; ++i) key(app, SDLK_DOWN);
    assert(addons_row(app, app->addons_focus) == ROW_ADD_CHEATS);
    key(app, SDLK_RETURN);
    addon_answer_is(app, mine);
    snprintf(want, sizeof want,
             "Added %s: 2 cheats from my cheats.txt, named by the build ID of the main module the game plays with",
             to_mine);
    assert(!strcmp(app->message, want) && file_is(cheats, "FEDCBA9876543210.txt", mine_text));
    assert(app->cheats_count == 3 && !strcmp(app->cheats[2], "Infinite HP") && !app->cheats_on[0] &&
           app->cheats_on[1] && app->cheats_on[2] && app->addons_focus == 5 && app->games.addons[index] == 3);
    /* named by another build ID than the one the game plays with: refused, the message naming both, nothing written */
    key(app, SDLK_RETURN);
    addon_answer_is(app, other);
    assert(!strcmp(app->message, "Not added: 1111111111111111.TXT is for build ID 1111111111111111, and the game "
                                 "plays with build ID FEDCBA9876543210: its cheats would never run"));
    assert(!snx_path_exists(to_other) && app->cheats_count == 3 && !app->confirm.shown);
    /* the same file once the game plays with that build ID: added by its name */
    fixture_module(main_file, "1111111111111111", false);
    key(app, SDLK_RETURN);
    addon_answer_is(app, other);
    snprintf(want, sizeof want, "Added %s: 1 cheats", to_other);
    assert(!strcmp(app->message, want) && app->cheats_count == 4 &&
           file_is(cheats, "1111111111111111.txt", "[Speed]\n04000000 c 4\n"));
    { /* a name in small letters is the same build ID as the main's, which its head gives in capitals */
        char *small = snx_path_join(root, "abcdef0123456789.txt"),
             *to_small = snx_path_join(cheats, "ABCDEF0123456789.txt");
        assert(!snx_write_file(small, "[Speed]\n04000000 c 4\n", 21)); /* a cheat listed already: the count stays */
        fixture_module(main_file, "ABCDEF0123456789", false);
        key(app, SDLK_RETURN);
        addon_answer_is(app, small);
        snprintf(want, sizeof want, "Added %s: 1 cheats", to_small);
        assert(!strcmp(app->message, want) && app->cheats_count == 4 && snx_is_file(to_small));
        free(to_small);
        free(small);
    }
    fixture_module(main_file, "FEDCBA9876543210", false);
    /* 16 letters that are not hex digits are no build ID, nor 16 hex digits and more before .txt: each takes the
       game's, which the folder has already */
    for (int i = 0; i < 2; ++i) {
        char* named_so = snx_path_join(root, i ? "0123456789abcdef-v1.txt" : "GHIJKLMNOPQRSTUV.txt");
        assert(!snx_write_file(named_so, "[Letters]\n", 10));
        key(app, SDLK_RETURN);
        addon_answer_is(app, named_so);
        snprintf(want, sizeof want, "Not added: the cheats folder has FEDCBA9876543210.txt already: %s", to_mine);
        assert(!strcmp(app->message, want) && app->cheats_count == 4);
        free(named_so);
    }
#ifdef __linux__
    if (snx_path_exists("/dev/full")) { /* a disk that fills up as the file is written: what it wrote taken away */
        char *full = snx_path_join(root, "3333333333333333.txt"), *gone = snx_path_join(cheats, "3333333333333333.txt");
        char* staged = snx_path_join(cheats, ".3333333333333333.txt.adding");
        fixture_module(main_file, "3333333333333333", false); /* the file's own: nothing refuses it before */
        assert(!snx_write_file(full, "[Full]\n", 7) && !snx_symlink("/dev/full", staged));
        snprintf(want, sizeof want, "Not added: cannot write %s: %s", gone, strerror(ENOSPC));
        key(app, SDLK_RETURN);
        addon_answer_is(app, full);
        assert(!strcmp(app->message, want) && app->confirm.shown && !strcmp(app->error_line, want));
        key(app, SDLK_ESCAPE);
        assert(!snx_path_exists(gone) && !snx_path_exists(staged) && app->cheats_count == 4);
        free(staged);
        free(gone);
        free(full);
    }
#endif
#ifndef _WIN32
    { /* a cheats folder the system does not let it write: the file and the system's reason, nothing left */
        char *locked = snx_path_join(root, "2222222222222222.txt"),
             *gone = snx_path_join(cheats, "2222222222222222.txt");
        char* staged = snx_path_join(cheats, ".2222222222222222.txt.adding");
        fixture_module(main_file, "2222222222222222", false);
        assert(!snx_write_file(locked, "[Lock]\n", 7) && !chmod(cheats, 0500));
        if (access(cheats, W_OK)) { /* root writes there all the same: a container's */
            snprintf(want, sizeof want, "Not added: cannot write %s: %s", gone, strerror(EACCES));
            key(app, SDLK_RETURN);
            addon_answer_is(app, locked);
            assert(!strcmp(app->message, want) && app->confirm.shown && !strcmp(app->error_line, want));
            key(app, SDLK_ESCAPE);
            assert(!snx_path_exists(gone) && !snx_path_exists(staged) && app->cheats_count == 4);
        }
        assert(!chmod(cheats, 0700));
        free(staged);
        free(gone);
        free(locked);
    }
#endif
    key(app, SDLK_ESCAPE);
    assert(!app->addons_shown);
    /* the files the test added taken away, Games' count with them */
    assert(!snx_remove_tree(cheats) && !snx_remove_tree(exefs) && !snx_remove_tree(root));
    app->games.addons[index] = 0;
    app->message[0] = 0;
    free(to_other);
    free(to_mine);
    free(to);
    free(big);
    free(cht);
    free(empty);
    free(other);
    free(mine);
    free(named);
    free(exefs);
    free(main_file);
    free(data);
    free(cheats);
    free(root);
}
#ifdef SNX_XDG
/* the message an answer from a picker's own thread leaves, waited for up to 5 seconds */
static void message_waited(App* app) {
    SDL_Event e;
    Uint64 until = SDL_GetTicks() + 5000;
    while (!app->message[0] && SDL_GetTicks() < until) {
        while (SDL_PollEvent(&e)) event(app, &e);
        SDL_Delay(5);
    }
}
/* the system's pickers the box's Add rows open, as SDL runs zenity (one of the test's own on PATH, which says what it
   was asked and answers a folder or a file): Add Mod the folder picker, Add Cheats the file picker for a .txt, each
   answer from SDL's thread added to the box. Off, the menu's own pickers go back to none */
static void pickers_asked(App* app, const char* temporary, const char* self) {
    char *root = snx_path_join(temporary, "pickers"), *bin = snx_path_join(root, "bin");
    char *zenity = snx_path_join(bin, "zenity"), *asked = snx_path_join(root, "asked.txt");
    char *mod = snx_path_join(root, "Zen"), *cheat = snx_path_join(root, "0123456789ABCDEF.txt");
    char *game = snx_path_join(app->games_dir, "picker game"), *program = snx_path_join(game, "picker game");
    char *data = snx_path_join(game, "picker game_data"), *was = getenv("PATH") ? snx_strdup(getenv("PATH")) : NULL;
    char *script, *path, *text, want[2048];
    size_t size = 0;
    fixture_file(mod, "romfs/zen.bin", "zen");
    assert(!snx_write_file(cheat, "[Zen]\n", 6));
    assert(!snx_mkdirs(data) && !snx_copy_file(self, program) && !snx_make_executable(program));
    script = snx_format("#!/bin/sh\n"
                        "[ \"$1\" = --version ] && { echo 3.44.0; exit 0; }\n"
                        "printf '%%s\\n' \"$@\" > '%s'\n"
                        "for a in \"$@\"; do [ \"$a\" = --directory ] && { echo '%s'; exit 0; }; done\n"
                        "echo '%s'\n",
                        asked, mod, cheat);
    assert(!snx_mkdirs(bin) && !snx_write_file(zenity, script, strlen(script)) && !snx_make_executable(zenity));
    path = snx_format("%s:%s", bin, was ? was : "/usr/bin:/bin");
    assert(!SDL_setenv_unsafe("PATH", path, 1) &&
           SDL_SetHintWithPriority(SDL_HINT_FILE_DIALOG_DRIVER, "zenity", SDL_HINT_OVERRIDE));
    go_tab(app, GUI_TAB_GAMES);
    focus_game(app, "picker game");
    key(app, SDLK_M);
    assert(app->addons_shown && addons_row(app, 0) == ROW_ADD_MOD && !app->addons_focus);
    app->message[0] = 0;
    key(app, SDLK_RETURN);
    message_waited(app);
    snprintf(want, sizeof want, "Mod Zen added: %s/Zen", app->mods_folder);
    assert(!strcmp(app->message, want) && app->mods_count == 1 && app->mods_on[0]);
    text = (char*)snx_read_file(asked, &size);
    assert(text && strstr(text, "--file-selection\n") && strstr(text, "--directory\n") &&
           !strstr(text, "--file-filter"));
    free(text);
    assert(!snx_remove_tree(asked));
    app->addons_focus = addons_row_of(app, ROW_ADD_CHEATS);
    app->message[0] = 0;
    key(app, SDLK_RETURN);
    message_waited(app);
    snprintf(want, sizeof want, "Added %s/0123456789ABCDEF.txt: 1 cheats", app->cheats_folder);
    assert(!strcmp(app->message, want) && app->cheats_count == 1 && app->cheats_on[0]);
    text = (char*)snx_read_file(asked, &size);
    assert(text && strstr(text, "--file-selection\n") && !strstr(text, "--directory") &&
           strstr(text, "--file-filter=Cheats | *.[tT][xX][tT]\n"));
    free(text);
    key(app, SDLK_ESCAPE);
    assert(!app->addons_shown);
    /* the menu's pickers as they were: none, the PATH the test had */
    SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER); /* the environment's, ctest's none */
    assert(was ? !SDL_setenv_unsafe("PATH", was, 1) : !SDL_unsetenv_unsafe("PATH"));
    assert(!snx_remove_tree(game) && !snx_remove_tree(root));
    list_games(app);
    app->message[0] = 0;
    free(path);
    free(script);
    free(was);
    free(data);
    free(program);
    free(game);
    free(cheat);
    free(mod);
    free(asked);
    free(zenity);
    free(bin);
    free(root);
}
#endif
/* this process's priority: its nice value, on Windows its class */
static int own_priority(void) {
#ifdef _WIN32
    return (int)GetPriorityClass(GetCurrentProcess());
#else
    return getpriority(PRIO_PROCESS, 0);
#endif
}
/* a low one as the queue's recompile gets it (gui_process_low): ten steps of nice under this program's, on Windows
   the class under its own */
static int low_priority(void) {
#ifdef _WIN32
    int own = own_priority(); /* a runner may start this program below normal */
    return own == BELOW_NORMAL_PRIORITY_CLASS || own == IDLE_PRIORITY_CLASS ? IDLE_PRIORITY_CLASS
                                                                            : BELOW_NORMAL_PRIORITY_CLASS;
#else
    return SDL_min(own_priority() + 10, PRIO_MAX - 1);
#endif
}
/* process pid's priority as own_priority reads its own, -100 when it cannot be read */
static int priority_of(long pid) {
#ifdef _WIN32
    HANDLE child = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    int class = child ? (int)GetPriorityClass(child) : 0;
    if (child) CloseHandle(child);
    return class ? class : -100;
#else
    int n;
    errno = 0;
    n = getpriority(PRIO_PROCESS, (id_t)pid);
    return errno ? -100 : n;
#endif
}
/* the queue's recompile running gone a block further: <file>.step made, pump until its percent is percent */
static void job_step(App* app, int percent) {
    int i = gui_queue_running(&app->queue);
    char* step;
    Uint64 until = SDL_GetTicks() + 10000;
    assert(i >= 0);
    step = snx_format("%s.step", gui_queue_file(&app->queue, i));
    assert(!snx_write_file(step, "", 0));
    while (gui_queue_running(&app->queue) == i && app->queue.job[i].percent != percent && SDL_GetTicks() < until) {
        pump(app);
        SDL_Delay(2);
    }
    assert(app->queue.job[i].percent == percent);
    free(step);
}
/* pump until the queue's recompile running has said block 1, its percent 25 */
static void job_started(App* app) {
    int i = gui_queue_running(&app->queue);
    Uint64 until = SDL_GetTicks() + 10000;
    assert(i >= 0);
    while (app->queue.job[i].percent != 25 && SDL_GetTicks() < until) {
        pump(app);
        SDL_Delay(2);
    }
    assert(app->queue.job[i].percent == 25);
}
/* the last of the queue's recompile running's blocks, then pump until it ended: the next one, if any, running */
static void job_end(App* app) {
    int i = gui_queue_running(&app->queue);
    uint32_t serial = app->queue.job[i].serial;
    char* step = snx_format("%s.step", gui_queue_file(&app->queue, i));
    Uint64 until = SDL_GetTicks() + 10000;
    assert(!snx_write_file(step, "", 0));
    while (app->ended_serial != serial && SDL_GetTicks() < until) {
        pump(app);
        SDL_Delay(2);
    }
    assert(app->ended_serial == serial);
    free(step);
}
/* a note of the steps: a line the run printed that is no step of its own */
static bool noted(const GuiSteps* steps, const char* text) {
    for (int i = 0; i < steps->notes; ++i)
        if (strstr(steps->note[i].text, text)) return true;
    return false;
}
/* the pid the queue's recompile running wrote beside its file */
static long job_pid(App* app) {
    int i = gui_queue_running(&app->queue);
    char* path = snx_format("%s.pid", gui_queue_file(&app->queue, i));
    size_t size;
    char* text = (char*)snx_read_file(path, &size);
    long pid;
    assert(text);
    pid = strtol(text, NULL, 10);
    free(text);
    free(path);
    return pid;
}
/* Games' row of the game named name on the page shown, as its label reads (the whole of it, which a long one slides
   to show) */
static bool games_row_reads(App* app, const char* name, const char* want) {
    GuiGamesBoxes g;
    games_boxes(app, &g);
    for (size_t row = 0; row < games_rows(app); ++row) {
        UiRowInfo info = {0};
        if (ui_games_rows_get_row_info(app, row, 0, &info) && info.label && !strncmp(info.label, name, strlen(name)))
            return !strcmp(info.label, want);
    }
    return false;
}
/* the Recompile tab's label as it reads now */
static bool tab_reads(App* app, const char* want) {
    char label[64];
    tab_label(app, GUI_TAB_RECOMPILE, label, sizeof label);
    return !strcmp(label, want);
}
/* R's and B's recompiles: a queue behind the screens and its Recompile tab (queue.h; DraVee's picks, 2026-10-23) */
static void recompile_queue(App* app) {
    static const char* const names[] = {"queue game one", "queue game two", "queue game fails"};
    char *folders[3], *programs[3], line[512], said[1024];
    int own = own_priority(), low = low_priority();
    GuiSettings* settings = settings_of(app);
    bool raised;
    for (int i = 0; i < 3; ++i) { /* games of folders of their own, which R recompiles where they are */
        folders[i] = snx_path_join(app->games_dir, names[i]);
#ifdef _WIN32
        char* file = snx_format("%s.exe", names[i]);
#else
        char* file = snx_strdup(names[i]);
#endif
        char* data = snx_format("%s_data", names[i]); /* a game's: its program and its data beside it */
        char* where = snx_path_join(folders[i], data);
        programs[i] = snx_path_join(folders[i], file);
        assert(!snx_mkdirs(where) && !snx_write_file(programs[i], "x", 1) && !snx_make_executable(programs[i]));
        stamp_here(folders[i], names[i]); /* made by this build: Enter plays it */
        free(where);
        free(data);
        free(file);
    }
    app->theme = *gui_theme_builtin("default");
    app->side_box = GUI_SIDE_AUTO;
    app->background_gentle = false;
    free(app->job_log); /* beside the games: Folders' Logs, which may have set it, are gone */
    app->job_log = snx_format("%s-last-recompile.log", app->games_dir);
    recent_open(app, snx_format("%s-recent.txt", app->games_dir)); /* R puts its game first in the recent games */
    assert(gui_settings_set(settings, "aot_threads", "8"));        /* Full: the AOT threads picked; Gentle: half */
    assert(!app->queue.count && tab_count(app) == GUI_TAB_RECOMPILE);
    assert(SDL_GetWindowProgressState(app->window) == SDL_PROGRESS_STATE_NONE);
    /* R on a game: the Recompile tab, a fourth, shows it, its output at the letters a half step under the text's,
       the queue and the Status box beside it; the menu runs nothing, the queue's process does */
    go_tab(app, GUI_TAB_GAMES);
    focus_game(app, names[0]);
    key(app, SDLK_R);
    assert(!app->menu.running && gui_queue_running(&app->queue) == 0 && app->queue.count == 1);
    assert(app->menu.view == GUI_LOAD_RECOMPILE && tab_shown(app) == GUI_TAB_RECOMPILE && tab_count(app) == GUI_TABS);
    job_started(app);
    assert(noted(&app->job_steps, "threads 8, priority") && !app->job_low && !app->job_threads); /* Full */
    snprintf(line, sizeof line, "threads 8, priority %d", own);
    assert(noted(&app->job_steps, line));
    assert(tab_reads(app, "Compile 25%"));
    render(app);
    shows(app, "Compile 25%", yellow); /* the tab, in focus */
    shows(app, "COMPILE", normal);     /* the head's title, its first part left out beside four tabs */
    hides(app, "RECOMPILE", normal);   /* the tab's name, not the action's */
    shows_half(app, "Recompiling queue game one (1 modules)", normal); /* its output, in the Output box */
    shows(app, "Output", normal);
    shows(app, "Queue", normal);
    shows(app, "- queue game one  25%", yellow);
    shows_half(app, "Background recompile: Full", rgb(app->theme.soft));
    shows_half(app, "Compiling queue game one: 25%", rgb(app->theme.ink)); /* the tips under the head */
    shows_foot(app, "abort", normal);
    shows_foot(app, "games", normal);    /* Tab's, which every other foot leaves to F1 */
    hides_foot(app, "take out", normal); /* the one running stays: ESC aborts it */
    assert(SDL_GetWindowProgressState(app->window) == SDL_PROGRESS_STATE_NORMAL &&
           SDL_fabsf(SDL_GetWindowProgressValue(app->window) - 0.25f) < 0.001f);
    /* YoRHa's: its loading screen as the panel, in its own colours and the letters a half step under the text's, the
       usage and how it goes on the footer's band at its foot */
    app->theme = *gui_theme_builtin("yorha");
    app->clock = SDL_GetTicks() + 60000; /* every line typed */
    render(app);
    shows_half(app, "RECOMPILE: QUEUE GAME ONE 25%", rgb(app->theme.run_colors.ink));
    shows_half(app, "threads 8, priority", rgb(app->theme.run_colors.soft)); /* its lines under its step */
    shows_half(app, "Full", rgb(app->theme.run_colors.page));
    hides(app, "RECOMPILE: QUEUE GAME ONE 25%", rgb(app->theme.run_colors.ink)); /* not the text's own letters */
    app->clock = 0;
    app->theme = *gui_theme_builtin("default");
    /* Tab back to Games, the recompile going on: its row says how far it is, and the tab too */
    key(app, SDLK_TAB);
    assert(app->menu.view == GUI_LOAD_GAMES && gui_queue_running(&app->queue) == 0);
    job_step(app, 50);
    render(app);
    shows(app, "Compile 50%", normal);
    shows(app, "- queue game one  50%", yellow);
    shows(app, "Compiling: RECOMPILE: QUEUE GAME ONE 2 / 4", normal); /* the side box of its row: the theme's amount */
    assert(SDL_fabsf(SDL_GetWindowProgressValue(app->window) - 0.5f) < 0.001f);
    /* R on another game while one runs: it joins the queue, the tips saying where; one runs at a time */
    focus_game(app, names[1]);
    key(app, SDLK_R);
    assert(app->menu.view == GUI_LOAD_GAMES && app->queue.count == 2 && gui_queue_running(&app->queue) == 0);
    assert(!strcmp(app->message, "queue game two joins the queue: next. queue game one compiles now (50%)"));
    focus_game(app, names[2]);
    key(app, SDLK_R);
    assert(app->queue.count == 3 && app->queue.job[2].state == GUI_JOB_WAITING);
    assert(!strcmp(app->message, "queue game fails joins the queue: 3rd. queue game one compiles now (50%)"));
    { /* the focus on the game R was pressed on, though the recent games over the list grew */
        bool recent;
        assert(!strcmp(games_focused_path(app, &recent), app->queue.job[2].text + app->queue.job[2].listed) && !recent);
    }
    assert(games_row_reads(app, names[1], "queue game two  next"));
    assert(games_row_reads(app, names[2], "queue game fails  3rd in queue"));
    assert(games_row_reads(app, names[0], "queue game one  50%"));
    /* R on one in the queue again adds nothing: its line shows */
    key(app, SDLK_R);
    assert(app->queue.count == 3 && app->menu.view == GUI_LOAD_RECOMPILE && app->queue_focus == 2);
    assert(!strcmp(app->message, "queue game fails is in the queue already"));
    /* Enter on a game of the queue plays nothing yet: its recompile shows */
    key(app, SDLK_TAB);
    focus_game(app, names[0]);
    key(app, SDLK_RETURN);
    assert(!app->menu.running && app->menu.view == GUI_LOAD_RECOMPILE && app->queue_focus == 0);
    { /* its recent row too, which R put first; and one with updates, DLCs or mods: its recompile, not their box */
        int r = 0;
        size_t g;
        char* mod = snx_format("%s/%s_data/mods/A mod", folders[0], names[0]);
        key(app, SDLK_TAB);
        while (r < recent_shown(app) && !snx_same_file(app->recent.game[r].path, programs[0])) ++r;
        assert(r < recent_shown(app));
        app->browser.page = 0;
        app->browser.selected = (size_t)r;
        key(app, SDLK_RETURN);
        assert(!app->menu.running && app->menu.view == GUI_LOAD_RECOMPILE && app->queue_focus == 0);
        assert(!snx_mkdirs(mod)); /* a mod of its own, which Games counts */
        key(app, SDLK_TAB);
        focus_game(app, names[0]);
        g = app->browser.page * app->browser.page_size + app->browser.selected - app->browser.head;
        assert(app->games.addons[g] == 1);
        key(app, SDLK_RETURN);
        assert(!app->addons_shown && !app->menu.running && app->menu.view == GUI_LOAD_RECOMPILE);
        assert(!snx_remove_tree(mod));
        free(mod);
    }
    /* the queue's rows: one waiting says where it waits, Del takes it out; Del on the one running says why not */
    key(app, SDLK_DOWN);
    render(app);
    shows(app, "Waits in the queue: next", normal);
    shows_foot(app, "take out", normal);
    key(app, SDLK_DOWN);
    key(app, SDLK_DELETE);
    assert(app->queue.count == 2 && !strcmp(app->message, "queue game fails taken out of the queue"));
    assert(app->queue_focus == 1); /* the row it was on, the last */
    key(app, SDLK_UP);
    assert(app->queue_focus == 0);
    key(app, SDLK_DELETE);
    assert(app->queue.count == 2 &&
           !strcmp(app->message, "queue game one compiles now: ESC aborts it, Del takes out one waiting"));
    /* F1 there: its keys, Up/Down and Tab among them */
    key(app, SDLK_F1);
    render(app);
    shows(app, "Up/Down", yellow);
    shows(app, "queue", normal);
    key(app, SDLK_ESCAPE);
    assert(!app->help_shown && app->menu.view == GUI_LOAD_RECOMPILE && gui_queue_running(&app->queue) == 0);
    /* leaving asks, naming it: ESC on the menu, the Exit item, the window closed */
    go_tab(app, GUI_TAB_MENU);
    key(app, SDLK_ESCAPE);
    assert(app->confirm.shown && app->confirm.ask == GUI_ASK_EXIT);
    assert(!strcmp(ask_question(app),
                   "queue game one is compiling (50%), 1 more in the queue: exiting stops them. Exit?"));
    key(app, SDLK_ESCAPE);
    app->menu.selected = GUI_EXIT;
    key(app, SDLK_RETURN);
    assert(!app->menu.quit && app->confirm.shown && app->confirm.ask == GUI_ASK_EXIT);
    key(app, SDLK_ESCAPE);
    {
        SDL_Event e = {0};
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event(app, &e);
        assert(!app->closing_ms && app->confirm.shown && app->confirm.ask == GUI_ASK_CLOSE);
        assert(!strcmp(ask_question(app),
                       "queue game one is compiling (50%), 1 more in the queue: exiting stops them. Exit?"));
        key(app, SDLK_ESCAPE);
    }
    app->menu.selected = GUI_SWITCH;
    /* Move Old Files and Clean Old Settings wait until the queue ends, the note naming the one running */
    open_app(app, GUI_APP_MOVE);
    app_action(app, GUI_APP_MOVE, 0);
    assert(!app->confirm.shown && !app->cleaned_shown &&
           !strcmp(app->editor.note, "queue game one is compiling (50%): Move Old Files waits until the queue ends"));
    app_action(app, GUI_APP_CLEAN, 0);
    assert(
        !app->confirm.shown &&
        !strcmp(app->editor.note, "queue game one is compiling (50%): Clean Old Settings waits until the queue ends"));
    leave_editor(app);
    /* the Settings read on a process of their own, the recompile going on, and a run of the menu's too */
    if (!snx_menu_settings) {
        settings_forget(app);
        settings = settings_of(app);
        assert(settings->count > settings->own && gui_queue_running(&app->queue) == 0);
        assert(gui_settings_set(settings, "aot_threads", "8"));
    }
    if (!snx_menu_settings) {
        char* step = snx_format("%s.step", programs[2]);
        go_tab(app, GUI_TAB_GAMES);
        focus_game(app, names[2]); /* taken out of the queue: it plays */
        key(app, SDLK_RETURN);
        assert(app->menu.running && gui_queue_running(&app->queue) == 0);
        settings_forget(app);
        settings = settings_of(app);
        assert(settings->count > settings->own && app->menu.running);
        assert(gui_settings_set(settings, "aot_threads", "8"));
        assert(!snx_write_file(step, "", 0));
        for (Uint64 until = SDL_GetTicks() + 10000; app->menu.running && SDL_GetTicks() < until;) {
            pump(app);
            SDL_Delay(2);
        }
        assert(!app->menu.running && app->finished);
        key(app, SDLK_ESCAPE);
        free(step);
    }
    /* the end: its line OK, the tab OK a while, then the queue's percent; the next runs by itself */
    go_tab(app, GUI_TAB_RECOMPILE);
    job_step(app, 75);
    job_step(app, 100);
    job_end(app);
    assert(app->queue.job[0].state == GUI_JOB_OK && gui_queue_running(&app->queue) == 1 && app->queue_focus == 0);
    assert(!strncmp(app->message, "queue game one: Recompiled in ", 30) && !app->log_due);
    assert(tab_reads(app, "Compile OK"));
    queue_tips(app, line, sizeof line); /* how it ended, then the one that runs now, a period between */
    assert(!strncmp(line, "queue game one: Recompiled in ", 30) && strstr(line, " s. queue game two compiles now (") &&
           !strstr(line, ".."));
    job_started(app);
    app->clock = app->queue.job[0].ended_ms + GUI_JOB_SAID_MS;
    assert(tab_reads(app, "Compile 62%")); /* the queue's: game one whole, game two at 25% */
    render(app);
    shows(app, "- queue game one  OK", yellow);
    shows(app, "- queue game two  25%", normal); /* each row its own */
    assert(SDL_fabsf(SDL_GetWindowProgressValue(app->window) - 0.62f) < 0.001f);
    shows_half(app, "Built queue game one", normal); /* its output kept, beside its row */
    shows_foot(app, "play", normal);
    app->clock = 0;
    { /* a click on a row takes the focus, a click on the one that ended well and in focus plays it, as Enter does */
        GuiQueueBoxes q;
        queue_boxes(app, &q);
        mouse_px(app, ui_queue_row(q.rects, app->scale, UI_QUEUE_ROWS, 1).x + 4,
                 ui_queue_row(q.rects, app->scale, UI_QUEUE_ROWS, 1).y + 4, true);
        assert(app->queue_focus == 1 && !app->menu.running);
        render(app);
        shows_foot(app, "abort", normal); /* the keys follow the focus */
        mouse_px(app, ui_queue_row(q.rects, app->scale, UI_QUEUE_ROWS, 0).x + 4,
                 ui_queue_row(q.rects, app->scale, UI_QUEUE_ROWS, 0).y + 4, true);
        assert(app->queue_focus == 0 && !app->menu.running && app->queue.count == 2);
    }
    /* Enter on it plays its game, its line seen and gone; while a game plays the one running goes gently, its
       priority low; once it ended, back where the system lets a program raise it, else low until it ends, said */
    {
        long pid = job_pid(app);
        char* step = snx_format("%s.step", programs[0]);
        assert(priority_of(pid) == own);
        key(app, SDLK_RETURN);
        assert(app->menu.running && app->queue.count == 1 && gui_queue_running(&app->queue) == 0);
        pump(app);
        assert(app->job_low && priority_of(pid) == low);
        job_speed(app, said, sizeof said);
        assert(!strcmp(said, "Gentle, a game plays"));
        assert(!snx_write_file(step, "", 0));
        for (Uint64 until = SDL_GetTicks() + 10000; app->menu.running && SDL_GetTicks() < until;) {
            pump(app);
            SDL_Delay(2);
        }
        assert(!app->menu.running && app->finished);
        key(app, SDLK_ESCAPE);
        pump(app);
        raised = !app->job_kept_low;
        job_speed(app, said, sizeof said);
        if (raised)
            assert(!app->job_low && priority_of(pid) == own && !strcmp(said, "Full"));
        else /* Linux and macOS without the privilege */
            assert(app->job_low && priority_of(pid) == low && !strncmp(said, "Gentle until it ends: ", 22));
        printf("gui_sdl: the queue's recompile after the game: %s\n", said);
        free(step);
    }
    /* a failure: FAILED on its row and its tab, blinking as Share Log does, which blinks too; the taskbar's error
       once none runs */
    go_tab(app, GUI_TAB_GAMES);
    focus_game(app, names[2]);
    key(app, SDLK_R);
    assert(app->queue.count == 2 && app->queue.job[1].state == GUI_JOB_WAITING);
    job_step(app, 50);
    job_step(app, 75);
    job_step(app, 100);
    job_end(app);
    assert(app->queue.job[0].state == GUI_JOB_OK && gui_queue_running(&app->queue) == 1);
    job_started(app);
    assert(SDL_GetWindowProgressState(app->window) == SDL_PROGRESS_STATE_NORMAL);
    job_step(app, 50);
    job_step(app, 75);
    job_step(app, 100);
    job_end(app);
    assert(app->queue.job[1].state == GUI_JOB_FAILED && app->queue.job[1].status == 3 && app->log_due);
    assert(!strcmp(app->message, "queue game fails: Recompile failed (status 3).") && tab_reads(app, "Compile FAILED"));
    assert(SDL_GetWindowProgressState(app->window) == SDL_PROGRESS_STATE_ERROR);
    {
        GuiPlace p;
        GuiRect tab;
        go_tab(app, GUI_TAB_GAMES);
        games_place(app, &p);
        tab = tab_rect(app, p.tabs, GUI_TAB_RECOMPILE);
        app->clock = 2 * app->theme.blink_ms * 50;
        render(app);
        assert(
            same_colour(pixel(app, (float)(tab.x + app->scale), (float)(tab.y + tab.h / 2)), rgb(app->theme.failure)));
        app->clock += app->theme.blink_ms;
        render(app);
        assert(
            !same_colour(pixel(app, (float)(tab.x + app->scale), (float)(tab.y + tab.h / 2)), rgb(app->theme.failure)));
        app->clock = 0;
    }
    /* the log holds the queue's recompiles since one started with no line of an ended one left, the failure's among
       them; Share Log holds the log */
    {
        size_t size;
        char *log = (char*)snx_read_file(app->job_log, &size), *shared;
        const char *at, *end;
        assert(log && strstr(log, "queue game one") && strstr(log, "queue game two") &&
               strstr(log, "queue game fails") && strstr(log, "[ended, status 3]"));
        free(log);
        share_log(app);
        assert((at = strstr(app->message, "Log saved to ")) != NULL);
        at += 13;
        end = strstr(at, " (path copied)");
        shared = snx_format("%.*s", (int)(end ? (size_t)(end - at) : strlen(at)), at);
        log = (char*)snx_read_file(shared, &size);
        snprintf(line, sizeof line, "== the last recompile of the queue: %s ==", app->job_log);
        assert(log && strstr(log, line) && strstr(log, "[ended, status 3]") && !app->log_due);
        app->log_due = true; /* for the checks below */
        free(log);
        free(shared);
    }
    /* ESC on an ended one: back to Games, its line gone; the taskbar's error with the failure's */
    go_tab(app, GUI_TAB_RECOMPILE);
    app->queue_focus = 1;
    key(app, SDLK_ESCAPE);
    assert(app->menu.view == GUI_LOAD_GAMES && app->queue.count == 1 && tab_count(app) == GUI_TABS);
    pump(app);
    assert(SDL_GetWindowProgressState(app->window) == SDL_PROGRESS_STATE_NONE && tab_reads(app, "Compile OK"));
    go_tab(app, GUI_TAB_RECOMPILE);
    key(app, SDLK_ESCAPE);
    assert(app->menu.view == GUI_LOAD_GAMES && !app->queue.count && tab_count(app) == GUI_TAB_RECOMPILE);
    app->log_due = false;
    /* ESC on the one running aborts it, its line gone with it, the next one starting, the one after it waiting */
    focus_game(app, names[0]);
    key(app, SDLK_R);
    go_tab(app, GUI_TAB_GAMES);
    focus_game(app, names[1]);
    key(app, SDLK_R);
    go_tab(app, GUI_TAB_GAMES);
    focus_game(app, names[2]);
    key(app, SDLK_R);
    assert(app->queue.count == 3);
    go_tab(app, GUI_TAB_RECOMPILE);
    app->queue_focus = 0;
    job_started(app);
    key(app, SDLK_ESCAPE);
    for (Uint64 until = SDL_GetTicks() + 10000; app->queue.count == 3 && SDL_GetTicks() < until;) {
        pump(app);
        SDL_Delay(2);
    }
    assert(app->queue.count == 2 && gui_queue_running(&app->queue) == 0 && !app->log_due);
    assert(app->queue.job[1].state == GUI_JOB_WAITING && app->queue_focus == 0);
    assert(!strcmp(gui_queue_name(&app->queue, 0), names[1]) &&
           !strcmp(app->message, "queue game one: aborted with ESC"));
    for (int left = 1; left >= 0; --left) { /* the others aborted too, one after the other; the tab goes with them */
        job_started(app);
        key(app, SDLK_ESCAPE);
        for (Uint64 until = SDL_GetTicks() + 10000; app->queue.count > left && SDL_GetTicks() < until;) {
            pump(app);
            SDL_Delay(2);
        }
        assert(app->queue.count == left);
    }
    assert(!app->queue.count && app->menu.view == GUI_LOAD_GAMES && tab_count(app) == GUI_TAB_RECOMPILE);
    /* Background recompile Gentle: half the AOT threads and a low priority from its start */
    app->background_gentle = true;
    go_tab(app, GUI_TAB_GAMES);
    focus_game(app, names[0]);
    key(app, SDLK_R);
    job_started(app);
    snprintf(line, sizeof line, "threads 4, priority %d", low);
    assert(noted(&app->job_steps, line) && app->job_low && app->job_threads == 4 && priority_of(job_pid(app)) == low);
    job_speed(app, said, sizeof said);
    assert(!strcmp(said, "Gentle, 4 threads"));
    job_step(app, 50);
    job_step(app, 75);
    job_step(app, 100);
    job_end(app);
    go_tab(app, GUI_TAB_RECOMPILE);
    key(app, SDLK_ESCAPE);
    assert(!app->queue.count);
    app->background_gentle = false;
    /* B's AOT pack joins the same queue: its row says so, its end what the cache gave */
    {
        UiRowInfo info = {0};
        app->aot_pack = app->recompile = true;
        app->aot_cache = snx_strdup("queue game two.jitcache");
        app->aot_opt[0] = '2';
        app->menu.selected = GUI_SWITCH;
        launch(app, snx_strdup(programs[1]), true);
        app->aot_pack = app->recompile = false;
        free(app->aot_cache);
        app->aot_cache = NULL;
        assert(app->queue.count == 1 && app->queue.job[0].kind == GUI_JOB_AOT && app->menu.view == GUI_LOAD_RECOMPILE);
        job_started(app);
        assert(ui_queue_rows_get_row_info(app, 0, 0, &info) && !strcmp(info.label, "queue game two, AOT pack  25%"));
        job_step(app, 50);
        job_step(app, 75);
        job_step(app, 100);
        job_end(app);
        assert(!strcmp(app->queue.job[0].said, "AOT pack built from 3 cached functions."));
        key(app, SDLK_ESCAPE);
        assert(!app->queue.count);
    }
    app->message[0] = 0;
    for (int i = 0; i < 3; ++i) {
        assert(!snx_remove_tree(folders[i]));
        free(folders[i]);
        free(programs[i]);
    }
    go_tab(app, GUI_TAB_MENU);
}
/* the fake driver's wait for the test: until path is there (taken), ten seconds at most */
static void wait_taken(const char* path) {
    for (int n = 0; n < 2000 && !snx_is_file(path); ++n) SDL_Delay(5);
    remove(path);
}
int main(int argc, char** argv) {
#ifdef _WIN32
    snx_test_crash_said();
#endif
    if (argc > 2 && !strcmp(argv[1], "-r") && strstr(argv[2], "queue game")) {
        /* a recompile of the queue: its blocks one at a time, each once the test makes <input>.step (which it takes),
           then how it ended; the threads and the priority it got on its second line, its pid in <input>.pid */
        const char *threads = "default", *name = argv[2] + strlen(argv[2]);
        char *step = snx_format("%s.step", argv[2]), *pid = snx_format("%s.pid", argv[2]), *at;
        for (int i = 3; i < argc; ++i)
            if (!strncmp(argv[i], "--aot_threads=", 14)) threads = argv[i] + 14; /* the last one given */
        while (name > argv[2] && name[-1] != '/' && name[-1] != '\\') --name;
#ifdef _WIN32
        at = snx_format("%lu", (unsigned long)GetCurrentProcessId());
#else
        at = snx_format("%ld", (long)getpid());
#endif
        assert(!snx_write_file(pid, at, strlen(at)));
        printf("Recompiling %.*s (1 modules)\nthreads %s, priority %d\n",
               (int)(strstr(name, ".exe") ? (size_t)(strstr(name, ".exe") - name) : strlen(name)), name, threads,
               own_priority());
        for (int b = 1; b <= 4; ++b) {
            printf("  compiled block %d/4: code_00%d, 10 functions (0.1 s)\n", b, b - 1);
            fflush(stdout);
            wait_taken(step);
        }
        for (int i = 3; i < argc; ++i) /* B's: what the cache gave, as the driver says it */
            if (!strcmp(argv[i], "--jit-cache")) puts("AOT pack: 3 functions from the cache");
        printf("Built %s\n", name);
        free(at);
        free(pid);
        free(step);
        return strstr(argv[2], "fails") ? 3 : 0;
    }
    if (argc > 1 && strstr(argv[1], "queue game")) { /* one of them played: until <input>.step, the queue going on */
        char* step = snx_format("%s.step", argv[1]);
        puts("ScarletNX - queue game");
        fflush(stdout);
        wait_taken(step);
        free(step);
        return 0;
    }
    if (argc == 9 && !strcmp(argv[3], "-x") && !strcmp(argv[4], "--icon")) { /* a dump's icon, as -x --icon writes it */
        IconJpeg jpeg = {{0}, 0};
        char* to = snx_path_join(argv[6], "icon.jpg");
        int rc = 1;
        if (strstr(argv[8], "no icon"))
            fprintf(stderr, "scarletnx: Games shows no icon for %s: it has no Control NCA\n",
                    argv[8]); /* as snx_error */
        else
            rc = snx_splash_jpeg("wimsy-fuddles", 32, 90, icon_jpeg_write, &jpeg) ||
                 snx_write_file(to, jpeg.data, (size_t)jpeg.size);
        free(to);
        return rc;
    }
    if (argc == 2 && !strcmp(argv[1], "--settings")) { /* as the runtime lists its settings */
        fputs("Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\n"
              "Graphics\tresolution\t1\tfloat\t0.25\t4\tInternal 3D resolution\t\t\n"
              "On-screen controls\tcontrols\t0\tint\t0\t1\tThe on-screen controls\tShow controls\t0=Off|1=On\n"
              "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\tA control drawn\tA\t0=Hidden|1=Shown\n"
              "On-screen controls/Buttons\tcontrols_b_shown\t\tint\t0\t1\tA control drawn\tB\t0=Hidden|1=Shown\n"
              "User\tuser\t1\tint\t1\t8\tThe console's user the game plays as\t\t\n",
              stdout);
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "hang in join")) {
        SDL_Thread* thread = SDL_CreateThread(hang, "hung worker", NULL);
        assert(thread && !snx_write_file(argv[1], "ready", 5));
        SDL_WaitThread(thread, NULL); /* Never returns. ESC must kill, not request a graceful exit. */
        return 1;
    }
    if (argc > 1 && getenv("SNX_TEST_ARGV")) {
        /* a driver doing what the test asked (driver_will): its argv kept, a string a line, for the test to check */
        FILE* kept = fopen(getenv("SNX_TEST_ARGV"), "wb"); /* "\n" alone, as has_arg reads, on Windows too */
        int compiling = 0, status;
        for (int i = 1; i < argc; ++i) compiling |= !strcmp(argv[i], "-r");
        for (int i = 0; kept && i < argc; ++i) fprintf(kept, "%s\n", argv[i]);
        assert(kept && !fclose(kept));
        puts(getenv("SNX_TEST_SAY") ? getenv("SNX_TEST_SAY") : "the fake driver ran");
        status = getenv("SNX_TEST_STATUS") ? atoi(getenv("SNX_TEST_STATUS")) : 0;
        if (!compiling) { /* a game: let the menu show it running, then the test's finish lets it end */
            char* go = snx_format("%s.go", getenv("SNX_TEST_ARGV"));
            for (int n = 0; n < 100 && !snx_is_file(go); ++n) SDL_Delay(5);
            remove(go);
            free(go);
        }
        return status;
    }
    if (argc == 3 && !strcmp(argv[1], "--fullscreen=1") && !strcmp(argv[2], "--controls=0")) {
        puts("prepared game stdout");
        fputs("prepared game stderr\n", stderr);
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "option flags")) {
        int i = 2;
        assert(argc == 8 && !strcmp(argv[i++], "--fullscreen=1") && !strcmp(argv[i++], "--controls=0"));
        assert(!strcmp(argv[i++], "-S") && !strcmp(argv[i++], "-s"));
        assert(!strcmp(argv[i++], "--data") && argv[i++][0] && i == argc);
        puts("Recompiling option flags\nScarletNX - option flags"); /* the runner's banner: now loading */
        return 0;
    }
    if (argc > 2 && strstr(argv[argv[1][0] == '-' ? 2 : 1], "0100ABCD12340000")) { /* a game or its update */
        int r = !strcmp(argv[1], "-r");
        assert(strstr(argv[0], "runtime") && snx_is_file(argv[1 + r]));
        assert(!strcmp(argv[argc - 2], "--data") && argv[argc - 1][0]);
        puts("ScarletNX - an update");
        return 0;
    }
    if (strstr(argv[0], "0100ABCD12350000")) { /* a game played from its save state, slot 1: its own program */
        int i, found = 0;
        for (i = 1; i < argc; i++) found |= !strncmp(argv[i], "--state_load=0:", 15) && strstr(argv[i], ".1.snxstate");
        assert(found);
        puts("ScarletNX - a save state");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-r") && strstr(argv[2], "folder game")) { /* R on it: recompiled, not played */
        int i = 3;
        assert(strstr(argv[0], "runtime") && snx_is_file(argv[2]));
        assert(i + 2 == argc && !strcmp(argv[i], "--data") && argv[i + 1][0]);
        puts("Recompiling folder game (1 modules)\nBuilt folder game");
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "folder game")) { /* a game in a folder of its own, through the menu's driver */
        assert(strstr(argv[0], "runtime") && argc == 6 && snx_is_file(argv[1]) && !strcmp(argv[2], "--fullscreen=1") &&
               !strcmp(argv[3], "--controls=0") && !strcmp(argv[4], "--data") && argv[5][0]);
        puts("Recompiling folder game (1 modules)\nBuilt folder game\nScarletNX - folder game");
        return 0;
    }
    if (argc > 1) {
        assert(argc == 2 || argc == 6);
        assert(snx_is_file(argv[1]));
        if (strstr(argv[1], "Game00")) {
            assert(argc == 2 && strstr(argv[0], "gnuabi"));
            puts("Linux file selected from games/");
            return 0;
        }
        assert(strstr(argv[1], "game with spaces"));
        bool nx = strstr(argv[0], "runtime") != NULL; /* the program the menu is part of */
        if (nx)
            assert(argc == 6 && !strcmp(argv[2], "--fullscreen=1") && !strcmp(argv[3], "--controls=0") &&
                   !strcmp(argv[4], "--data") && argv[5][0]);
        else
            assert(argc == 2);
        size_t size;
        unsigned char* data = snx_read_file(argv[1], &size);
        assert(data && size == 1);
        ++data[0];
        assert(!snx_write_file(argv[1], data, 1));
        free(data);
        puts("driver stdout");
        fputs("driver stderr\n", stderr);
        return nx ? 0 : 7; /* Both successful and failed drivers need ESC acknowledgement. */
    }
    App app;
    /* the menu's data folder from the test's own environment, never the user's: XDG's here, and a home of the test's
       own where it comes from HOME, APPDATA or USERPROFILE */
    char* own_home = snx_temp_dir(NULL);
    assert(own_home);
    set_env("HOME", own_home);
    set_env("APPDATA", own_home);
    set_env("USERPROFILE", own_home);
    assert(getenv("APPDATA") && !strcmp(getenv("APPDATA"), own_home));
    set_env("XDG_DATA_HOME", "/nonexistent/snx-gui-test");
    set_env("XDG_CONFIG_HOME", "/nonexistent/snx-gui-test/config");
    set_env("XDG_STATE_HOME", "/nonexistent/snx-gui-test/state");
    set_env("XDG_CACHE_HOME", "/nonexistent/snx-gui-test/cache");
    /* the menu runs Switch inputs with the runtime it is part of, the other drivers from beside it */
    assert(initialize(&app, "/nonexistent/snx-gui-test/bin/scarletnx"));
    assert(!strcmp(app.self, "/nonexistent/snx-gui-test/bin/scarletnx"));
    assert(!strcmp(app.drivers, "/nonexistent/snx-gui-test/bin"));
    assert(!strcmp(app.theme.id, "yorha"));    /* the menu opens in YoRHa everywhere */
    app.theme = *gui_theme_builtin("default"); /* the checks below read Default's colours */
    assert(!app.animations_off);               /* on by default; the checks read each screen as it settles */
    app.animations_off = true;
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(SDL_PLATFORM_ANDROID)
    /* no checkout and no games/ beside the test: recompiled/ and themes/ come from the user's data folder */
    assert(!strcmp(app.data_dir, "/nonexistent/snx-gui-test/scarletnx"));
    assert(!strcmp(app.games_dir, "/nonexistent/snx-gui-test/scarletnx/recompiled"));
    assert(!strcmp(app.themes_dirs[0], "/nonexistent/snx-gui-test/scarletnx/themes"));
    /* and the keys go in it, where the driver reads them first, not in ~/.switch */
    assert(!strcmp(app.keys_path, "/nonexistent/snx-gui-test/scarletnx/prod.keys"));
#endif
    /* the themes that come with the GUI: beside it in a build tree, else in share/scarletnx beside its bin/ */
    {
        char *root = snx_temp_dir(NULL), *bin = snx_path_join(root, "bin"), *beside = snx_path_join(bin, "themes");
        char *shared = snx_path_join(bin, "../share/scarletnx/themes"), *found;
        assert(root && !snx_mkdirs(bin));
        found = shipped_themes(bin);
        assert(!strcmp(found, shared));
        free(found);
        assert(!snx_mkdirs(beside));
        found = shipped_themes(bin);
        assert(!strcmp(found, beside));
        free(found);
        assert(!snx_remove_tree(root));
        free(shared);
        free(beside);
        free(bin);
        free(root);
    }
    /* the window says which build it is: commit, compiler, platform */
    assert(!strcmp(SDL_GetWindowTitle(app.window), snx_build_info()) && !strncmp(snx_build_info(), "ScarletNX ", 10));
    /* the boot's splash: a drawing of Wimsy's in the middle of the default theme's black page, a third of the window's
       shorter side, until its time, drawn again when the window changes; a key ends it and does nothing else. Every
       drawing built in shows. A theme's own page shows in its chance of boots, the default's in the others */
    {
        SDL_Rect view;
        SDL_Color black = {0, 0, 0, 255}, wimsy = {17, 206, 161, 255}; /* wimsy.svg's middle */
        SnxSplashDrawing* read;
        int i, w, h;
        assert(SDL_GetWindowSize(app.window, &w, &h));
        for (i = 0; i < snx_splash_svg_count; ++i) {
            splash_with(&app, gui_theme_builtin("default"), &snx_splash_svgs[i]);
            app.clock = app.splash_until - 1;
            render(&app);
            assert(app.splash && app.splash_svg == &snx_splash_svgs[i]);
            SDL_GetRenderViewport(app.renderer, &view);
            assert(app.splash_side == SDL_min(view.w, view.h) / 3);
            splash_drawn(&app, &view, &snx_splash_svgs[i], app.splash_side, black);
            if (!i) assert(near_colour(pixel(&app, view.w / 2.0f, view.h / 2.0f), wimsy, 8));
            if (i + 1 < snx_splash_svg_count) {
                SDL_DestroyTexture(app.splash);
                app.splash = NULL;
                snx_splash_close(app.splash_drawing);
                app.splash_drawing = NULL;
            }
        }
        read = app.splash_drawing; /* read once: a new size only draws it again */
        assert(read && SDL_SetWindowSize(app.window, 900, 600));
        SDL_SyncWindow(app.window);
        render(&app);
        SDL_GetRenderViewport(app.renderer, &view);
        assert(view.w == 900 && view.h == 600 && app.splash && app.splash_side == 200 && app.splash_drawing == read);
        splash_drawn(&app, &view, app.splash_svg, 200, black);
        key(&app, SDLK_RETURN);
        assert(!app.splash_until && app.menu.selected == GUI_SWITCH && !app.menu.activated);
        render(&app);
        assert(!app.splash && !app.splash_drawing);
        app.clock = 0;
        assert(SDL_SetWindowSize(app.window, w, h));
        SDL_SyncWindow(app.window);
        render(&app);
        assert(gui_splash_theme(gui_theme_builtin("yorha"), 24) == gui_theme_builtin("yorha"));
        assert(gui_splash_theme(gui_theme_builtin("yorha"), 25) == gui_theme_builtin("default"));
    }
    char* temporary = snx_temp_dir(NULL);
    char* self = snx_executable(argv[0]);
    char* input = snx_path_join(temporary, "game with spaces;literal.nro");
    assert(!snx_write_file(input, "0", 1));
    const char* names[] = {"runtime", "scarletnx-gnuabi", "scarletnx-3ds", "scarletnx-vita", "scarletnx-agb"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
#ifdef _WIN32
        char* filename = snx_format("%s.exe", names[i]);
#else
        char* filename = snx_strdup(names[i]);
#endif
        char* path = snx_path_join(temporary, filename);
        assert(!snx_copy_file(self, path) && !snx_make_executable(path));
        free(filename);
        free(path);
    }
    free(app.drivers);
    app.drivers = snx_strdup(temporary);
    free(app.self);
#ifdef _WIN32
    app.self = snx_path_join(temporary, "runtime.exe");
#else
    app.self = snx_path_join(temporary, "runtime");
#endif
    free(app.games_dir);
    app.games_dir = snx_path_join(temporary, "games");
    app.settings_file = snx_path_join(temporary, "config/settings.ini"); /* the Settings' picks, kept */
    app.folders_file = snx_path_join(temporary, "config/folders.txt");   /* the player's game folders */
    app.app_file = snx_path_join(temporary, "config/app.ini");           /* Settings > App's picks */
    /* a start's failure is said in the box over the menu: /nonexistent's dumps/ cannot be made here, but a root that
       can make it (a CI container) has none */
    while (app.confirm.shown) {
        assert(app.confirm.ask == GUI_ASK_ERROR);
        key(&app, SDLK_RETURN);
    }
    app.message[0] = 0;
    /* a data folder whose dumps/ cannot be made, under a file whoever runs this: said in the box, whole, until OK; the
       tips keep it */
    {
        char *blocker = snx_path_join(temporary, "a file"), *data = app.data_dir, *games = app.games_dir;
        char *themes = app.themes_dirs[0], *work = app.work_dir, *keys = app.keys_path, *home = app.home_dir, *said;
        char* whole;
        assert(!snx_write_file(blocker, "", 0));
        app.home_dir = snx_path_join(blocker, "data"); /* the data folder places_apply takes, none picked */
        app.data_dir = NULL;
        app.games_dir = app.themes_dirs[0] = app.work_dir = app.keys_path = NULL;
        places_apply(&app);
        said = snx_path_join(app.data_dir, "dumps"); /* in the system's separators, as the box says it */
        whole = snx_format("Could not make %s", said);
        free(said);
        said = whole;
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ERROR && !strcmp(app.error_line, said));
        render(&app);
        shows(&app, said, rgb(app.theme.warning));
        free(app.data_dir), free(app.games_dir), free(app.themes_dirs[0]), free(app.work_dir), free(app.keys_path);
        free(app.home_dir);
        app.data_dir = data, app.games_dir = games, app.themes_dirs[0] = themes, app.work_dir = work;
        app.keys_path = keys, app.home_dir = home;
        assert(!remove(blocker));
        free(blocker);
        free(said);
    }
    shows(&app, "- OK", yellow);
    {
        GuiChoices c;
        assert(ask_place(&app, &c) == 1 && c.count == 1); /* OK alone */
    }
    key(&app, SDLK_DOWN); /* its one choice: nothing to move to, nothing closes */
    assert(app.confirm.shown && !app.confirm.selected);
    key(&app, SDLK_RETURN);
    assert(!app.confirm.shown && !strcmp(app.message, app.error_line));
    { /* the same failure again: said whole once in the session, the tips go on saying it; another, said */
        char said[sizeof app.error_line];
        snprintf(said, sizeof said, "%s", app.error_line);
        say_failure(&app, said);
        assert(!app.confirm.shown);
        say_failure(&app, "Not saved: x could not be written");
        assert(app.confirm.shown && !strcmp(app.error_line, "Not saved: x could not be written"));
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown);
    }
    /* the menu's pads stay open through a run until its game loads (LAUNCH, the runner's banner), so a pad can stop a
       recompile; open again once it ends. SDL failing to start them is said with its reason, once */
    {
        app.menu.running = true;
        gui_steps_reset(&app.steps);
        pads_follow(&app);
        assert(app.pads.started);
        gui_steps_feed(&app.steps, "Recompiling pad game (1 modules)\n", 33, 0);
        pads_follow(&app);
        assert(app.pads.started);
        gui_steps_feed(&app.steps, "ScarletNX - pad game\n", 21, 0);
        pads_follow(&app);
        assert(!app.pads.started);
        app.menu.running = false;
        pads_follow(&app);
        assert(app.pads.started && !app.confirm.shown);
        gui_pads_stop(&app.pads);
        gui_steps_reset(&app.steps);
        SDL_SetError("no pad driver here");
        pads_fail(&app);
        app.menu.running = true; /* not over a run: said once it ends */
        pads_follow(&app);
        assert(!app.confirm.shown);
        app.menu.running = false;
        pads_follow(&app);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ERROR &&
               !strcmp(app.error_line, "Controllers cannot be read: no pad driver here"));
        key(&app, SDLK_RETURN);
        pads_follow(&app);
        assert(!app.confirm.shown && !app.pads.started); /* not asked again, nor said */
        app.pads_failed = false;
        app.message[0] = 0;
    }
    render(&app);
    shows(&app, "- " ICON_GAMEPAD_2 " Load Switch NSP/XCI/NRO", yellow);
    assert(!app.menu.activated);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_3DS);
    render(&app);
    shows(&app, "- " ICON_BOX " Load 3DS game", yellow);
    shows(&app, "- " ICON_GAMEPAD_2 " Load Switch NSP/XCI/NRO", normal);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_VITA);
    render(&app);
    shows(&app, "- " ICON_PACKAGE " Load Vita VPK", yellow);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_GBA);
    render(&app);
    shows(&app, "- " ICON_GAMEPAD " Load GBA ROM", yellow);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_KEYS);
    /* Never touch the real user's keys: exercise installation in a private home. */
    free(app.keys_path);
    char* switch_dir = snx_path_join(temporary, ".switch");
    app.keys_path = snx_path_join(switch_dir, "prod.keys");
    char* source_keys = snx_path_join(temporary, "prod.keys");
    assert(!snx_write_file(source_keys, "first keys", 10));
    /* no prod.keys in use: Install Keys blinks red, focused or not, its label on the page colour; steady with
       blink_ms 0. A home without keys, as an installed build also takes ~/.switch's */
    {
        Uint64 on = 1000 * app.theme.blink_ms * 2, off = on + app.theme.blink_ms;
        char *bare = snx_path_join(temporary, "bare-home"),
             *saved_home = getenv("HOME") ? SDL_strdup(getenv("HOME")) : NULL;
        assert(!snx_mkdirs(bare));
        set_env("HOME", bare);
        assert(!keys_red(&app, off) && keys_red(&app, on));
        shows(&app, "- " ICON_KEY_ROUND " Install Keys", rgb(app.theme.page));
        key(&app, SDLK_DOWN);
        assert(app.menu.selected != GUI_KEYS && keys_red(&app, on) && !keys_red(&app, off));
        key(&app, SDLK_UP);
        app.theme.blink_ms = 0;
        assert(keys_red(&app, on) && keys_red(&app, off));
        app.theme.blink_ms = gui_theme_builtin(app.theme.id)->blink_ms;
        app.clock = 0;
        if (saved_home)
            set_env("HOME", saved_home);
        else
            set_env("HOME", NULL);
        SDL_free(saved_home);
        free(bare);
    }
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    render(&app);
    /* the drop line waits on the user: framed, in the middle of its box, blinking at blink_ms; steady, at the left
       or the right of the box as the theme says, or plain */
    {
        const char* waiting = "[ Pick or drop prod.keys ]";
        int width = (int)strlen(waiting) * 8 * app.scale;
        GuiPickBoxes k;
        GuiRect r;
        app.clock = 1000 * app.theme.blink_ms * 2;
        render(&app);
        pick_boxes(&app, &k);
        r = inner(&app, k.place.list);
        text_px_is(&app, r.x + (r.w - width) / 2, k.prompt, waiting, yellow, black);
        app.clock += app.theme.blink_ms;
        render(&app);
        hides(&app, "Pick or drop prod.keys", yellow);
        app.theme.prompt.blink = false; /* steady */
        render(&app);
        text_px_is(&app, r.x + (r.w - width) / 2, k.prompt, waiting, yellow, black);
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        text_px_is(&app, r.x + r.w - width, k.prompt, waiting, yellow, black);
        app.theme.prompt.align = GUI_ALIGN_LEFT;
        render(&app);
        text_px_is(&app, r.x, k.prompt, waiting, yellow, black);
        app.theme.prompt.lines[GUI_PROMPT_KEYS] = false; /* plain */
        render(&app);
        text_px_is(&app, r.x, k.prompt, "Pick or drop prod.keys    ", yellow, black);
        app.theme.prompt = gui_theme_builtin(app.theme.id)->prompt;
        app.clock = 0;
    }
    drop(&app, input, false); /* A game is not a key file and must not launch. */
    assert(app.menu.view == GUI_LOAD_KEYS && !app.menu.running && !snx_is_file(app.keys_path));
    drop(&app, "file://remote/prod.keys", true);
    assert(app.menu.view == GUI_LOAD_KEYS && !snx_is_file(app.keys_path));
    SDL_Event keys_event = {0};
    keys_event.type = SDL_EVENT_DROP_FILE;
    keys_event.drop.data = source_keys;
    event(&app, &keys_event);
    assert(!app.menu.running && strstr(app.message, "installed"));
    keys_outcome(&app, true);
    size_t keys_size;
    unsigned char* keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 10 && !memcmp(keys_data, "first keys", 10));
    free(keys_data);
    assert(!keys_red(&app, 1000 * app.theme.blink_ms * 2)); /* the keys in: as the other entries */
    app.clock = 0;
#ifndef _WIN32
    struct stat keys_stat;
    assert(!stat(app.keys_path, &keys_stat) && (keys_stat.st_mode & 0777) == 0600);
#endif
    key(&app, SDLK_RETURN);
    render(&app);
    shows(&app, "Keys installed here: installing again", red);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    key(&app, SDLK_RETURN);
    assert(!snx_write_file(source_keys, "replacement", 11));
    char* quoted_keys = snx_format("\"%s\"", source_keys);
    keys_event.type = SDL_EVENT_DROP_TEXT;
    keys_event.drop.data = quoted_keys;
    event(&app, &keys_event);
    free(quoted_keys);
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    keys_outcome(&app, true);
    key(&app, SDLK_RETURN);
    drop(&app, app.keys_path, false); /* Dropping the destination itself is safe. */
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    keys_outcome(&app, true);
    key(&app, SDLK_RETURN);
    assert(!snx_remove_tree(source_keys));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_file_size(app.keys_path) == 11);
    /* Failed replacement must leave the destination untouched and permit retry. */
    assert(!snx_remove_tree(app.keys_path) && !snx_mkdirs(app.keys_path));
    assert(!snx_write_file(source_keys, "replacement", 11));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_is_dir(app.keys_path) && app.message[0]);
    assert(app.keys_done && !app.keys_ok); /* not installed: said, then the menu after a while */
    assert(!snx_remove_tree(app.keys_path));
    {
        char* keys_path = app.keys_path; /* NULL when snx_keys_dir finds no home: the variable it reads is named */
        app.keys_path = NULL;
        drop(&app, source_keys, false);
#ifdef _WIN32
        assert(!strcmp(app.message, "Cannot install prod.keys: neither USERPROFILE nor HOME is set"));
#else
        assert(!strcmp(app.message, "Cannot install prod.keys: HOME is not set"));
#endif
        app.keys_path = keys_path;
    }
    drop(&app, source_keys, false); /* a try that works says so in place of it */
    assert(snx_is_file(app.keys_path));
    keys_outcome(&app, true);
    /* a try that fails, left alone, goes back to the menu too, its line saying why */
    app.menu.view = GUI_LOAD_KEYS;
    drop(&app, "file://remote/prod.keys", true);
    keys_outcome(&app, false);
    assert(strstr(app.message, "local file path"));
    /* a document of an app's (Android's picker): read through it, as a path to it may not be readable or there is none
       (Downloads by number, #280), and installed when its text is a prod.keys', whatever its name; refused when it is
       not one or is too big. The picker's message for a document with no path stays on the other screens */
    {
        static const char keys_text[] = "; dumped\r\nheader_key = 00112233aabbccdd\r\n";
        char *document = snx_path_join(temporary, "msf 12"), *big = (char*)calloc(1, SNX_GUI_KEYS_MAX + 2);
        assert(big && !snx_write_file(document, keys_text, sizeof keys_text - 1));
        app.menu.view = GUI_LOAD_KEYS;
        install_keys_document(&app, document);
        keys_data = snx_read_file(app.keys_path, &keys_size);
        assert(app.menu.view == GUI_LOAD_KEYS && app.keys_ok && keys_data && keys_size == sizeof keys_text - 1 &&
               !memcmp(keys_data, keys_text, keys_size));
        free(keys_data);
        assert(!snx_write_file(document, "a game, not keys", 16));
        app.menu.view = GUI_LOAD_KEYS;
        install_keys_document(&app, document);
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "is not a prod.keys"));
        memset(big, 'a', SNX_GUI_KEYS_MAX + 1);
        memcpy(big, "header_key = 00\n", 16);
        assert(!snx_write_file(document, big, SNX_GUI_KEYS_MAX + 1));
        install_keys_document(&app, document);
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "could not be read as prod.keys"));
        assert(snx_file_size(app.keys_path) == sizeof keys_text - 1);
        picked(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "could not be read as prod.keys"));
        /* one of the storage's too, as its path may not be this app's to read */
        app.message[0] = 0;
        picked(&app, "content://com.android.externalstorage.documents/document/primary%3ADownload%2Fprod.keys");
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "could not be read as prod.keys"));
        app.menu.view = GUI_LOAD_DROP;
        picked(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
        assert(app.menu.view == GUI_LOAD_DROP && strstr(app.message, "from this computer's storage"));
        app.menu.view = GUI_LOAD_NONE;
        app.message[0] = 0;
        free(big);
        free(document);
    }
    free(switch_dir);
    free(source_keys);
#ifdef SNX_XDG
    /* keys another tool left in ~/.switch, which the driver reads after the data folder's, are the installed build's
       keys in use until some are installed here */
    {
        char *home = snx_path_join(temporary, "home"), *saved_home = getenv("HOME") ? SDL_strdup(getenv("HOME")) : NULL;
        char *dir = snx_path_join(home, ".switch"), *found = snx_path_join(dir, "prod.keys"), *in_use;
        assert(!snx_mkdirs(dir) && !snx_write_file(found, "home keys", 9));
        set_env("HOME", home);
        in_use = keys_in_use(&app);
        assert(in_use && !strcmp(in_use, app.keys_path)); /* those installed above come first */
        free(in_use);
        assert(!snx_remove_tree(app.keys_path));
        in_use = keys_in_use(&app);
        assert(in_use && !strcmp(in_use, found));
        free(in_use);
        key(&app, SDLK_RETURN);
        assert(app.menu.view == GUI_LOAD_KEYS);
        render(&app);
        key(&app, SDLK_ESCAPE);
        if (saved_home)
            set_env("HOME", saved_home);
        else
            set_env("HOME", NULL);
        SDL_free(saved_home);
        free(found);
        free(dir);
        free(home);
    }
#endif
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_SETTINGS);
    render(&app);
    shows_half(&app, "The game's .ini settings,",
               normal);         /* what it is about, in the tips, a half step under the text's */
    mouse_px(&app, 2, 2, true); /* Blank space must not activate the current selection. */
    assert(!app.editing);
    /* Settings: the launcher's own, then each setting the runtime lists, a row each with its help above; what the
       menu's items used to set is in them */
    key(&app, SDLK_SPACE);
    assert(app.editing && app.editor.object && app.editor.count == 6); /* User the last */
    /* and the disk: what ScarletNX's folders take, counted on a thread, and what is left */
    {
        char line[256], used[32], left[32];
        uint64_t total = 0;
        Uint64 until = SDL_GetTicks() + 5000;
        char* big = snx_path_join(app.games_dir, "big.bin");
        char* bytes = (char*)calloc(1, 3 << 20);
        /* the test's own folders alone: the user's data folder (%APPDATA%\scarletnx on Windows) may be another
           test's at the same time */
        char *data_dir = app.data_dir, *themes = app.themes_dirs[0];
        app.data_dir = snx_path_join(temporary, "data");
        app.themes_dirs[0] = snx_path_join(app.data_dir, "themes");
        assert(bytes && !snx_mkdirs(app.games_dir) && !snx_write_file(big, bytes, 3 << 20));
        key(&app, SDLK_ESCAPE); /* counted again as the Settings open */
        key(&app, SDLK_SPACE);
        assert(app.space && app.free_bytes);
        render(&app);
        while (!__atomic_load_n(&app.space->done, __ATOMIC_ACQUIRE) && SDL_GetTicks() < until) SDL_Delay(5);
        for (int i = 0; i < GUI_SPACE_DIRS; ++i) total += gui_tree_size(app.space->dirs[i], &app.space->stop);
        assert(app.space->done && app.space->used == total && same_path(app.space->dirs[0], app.games_dir));
        gui_size_text(used, sizeof used, total);
        gui_size_text(left, sizeof left, app.free_bytes);
        /* in the Status box, after the RAM and the CPU */
        snprintf(line, sizeof line, "Disk: %s, %s free", used, left);
        key(&app, SDLK_ESCAPE);
        render(&app);
        shows_half(&app, line, rgb(app.theme.soft));
        shows_half(&app, "RAM ", rgb(app.theme.soft));
        assert(total == 3 << 20 && !strncmp(line, "Disk: 3 MB, ", 12));
        key(&app, SDLK_SPACE);
        assert(!snx_remove_tree(app.games_dir)); /* the browser's checks below start with no games/ */
        free(app.themes_dirs[0]);
        free(app.data_dir);
        app.themes_dirs[0] = themes;
        app.data_dir = data_dir;
        free(bytes);
        free(big);
    }
    /* the groups first: App, then a group per section the runtime lists (their reset is App's); beside them the
       focused group's rows, in the tips what it holds */
    render(&app);
    assert(app.form == GUI_FORM_SETTINGS && app.groups == 6 && gui_editor_rows(&app.editor) == 6);
    shows(&app, "SETTINGS", normal);
    shows_half(&app, "The theme, the menu's", normal); /* in the tips, a half step under the text's */
    shows(&app, "- " ICON_APP_WINDOW " App", yellow);
    shows(&app, "- " ICON_ROCKET " Launch", normal);
    shows(&app, "- " ICON_MONITOR " Display", normal);
    shows(&app, "- " ICON_IMAGE " Graphics", normal);
    shows(&app, "- " ICON_POINTER " Overlay", normal); /* the runtime's On-screen controls */
    hides(&app, "On-screen controls", normal);
    shows(&app, "- " ICON_USER " User", normal);
    shows(&app, "Reset to Defaults", normal); /* App's last row, beside: none among the groups */
    hides(&app, "\x1e", rgb(app.theme.soft)); /* every row shows: no arrows */
    hides(&app, "\x1f", rgb(app.theme.soft));
    key(&app, SDLK_DOWN);
    render(&app);
    shows_half(&app, "GDB stub, Extract to tmpfs", normal); /* a group's help names its settings */
    key(&app, SDLK_RIGHT);                                  /* a group opens with Enter only */
    assert(app.form == GUI_FORM_SETTINGS);
    key(&app, SDLK_RETURN);
    /* a group's settings on the right, a row each with its help in the tips; the group named over them */
    /* gdbstub, Extract to tmpfs, Extract dump, Optimization level, AOT threads, Unit size, Unit cache limit, the
       LLVM Internals page and Tracy capture, whose row a launcher without SNX_TRACY does not list */
    int tracy = 0;
    for (int i = 0; i < app.settings.own; i++)
        if (!strcmp(app.settings.values[i].key, "tracy_capture")) tracy = 1;
    assert(app.form == GUI_FORM_GROUP && app.group == 1 && app.editor.count == 8 + tracy);
    render(&app);
    shows(&app, "Launch", normal);
    shows_half(&app, "Enables gdbstub", normal); /* in the tips */
    shows(&app, "-    GDB stub ", yellow); /* its value in the column after the longest name, Optimization level's */
    shows(&app, "-    Extract to tmpfs ", normal);
    {
        char row[64];
        snprintf(row, sizeof row, "-    %-*s off", (int)strlen("Optimization level"), "Extract dump");
        shows(&app, row, normal);
    }
    if (tracy) shows(&app, "-    Tracy capture", normal);
    { /* Del is said where it does something: on a setting away from its default */
        char said[512];
        assert(!strstr(screen_said(&app, true, said, sizeof said), "Del: default."));
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "gdbstub", "on") &&
               strstr(screen_said(&app, true, said, sizeof said), "Del: default."));
    }
    key(&app, SDLK_LEFT);
    assert(!gui_settings_value(&app.settings, "gdbstub"));
    key(&app, SDLK_DOWN);
    render(&app);
    shows_half(&app, "Extract to t", normal);  /* the tips follow the focus, the row itself yellow */
    point(&app, row_box(&app, 0, true), true); /* a click focuses a row, one on the focused row changes it */
    assert(app.editor.row == 0 && !gui_settings_value(&app.settings, "gdbstub"));
    point(&app, row_box(&app, 0, true), true);
    assert(setting_is(&app, "gdbstub", "on"));
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "gdbstub"));
    key(&app, SDLK_RETURN);
    assert(setting_is(&app, "gdbstub", "on"));
    /* ESC goes back to the groups, on the one it came from */
    key(&app, SDLK_ESCAPE);
    assert(app.editing && app.form == GUI_FORM_SETTINGS && app.editor.row == 1);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_GROUP && app.group == 2);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "fullscreen", "1"));
    render(&app);
    shows(&app, "Display", normal);
    shows(&app, "- fullscreen 1", yellow); /* the launcher's choice was 0 */
    /* Del puts a setting back as it started */
    key(&app, SDLK_DELETE);
    assert(setting_is(&app, "fullscreen", "0"));
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    /* a number typed, and nothing typed for the .ini's again */
    render(&app);
    shows(&app, "- resolution 1 [default]", yellow); /* not picked: the default */
    key(&app, SDLK_RETURN);
    typed(&app, "0.5");
    key(&app, SDLK_RETURN);
    assert(setting_is(&app, "resolution", "0.5"));
    key(&app, SDLK_RETURN);
    for (int i = 0; i < 3; ++i) key(&app, SDLK_BACKSPACE);
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "resolution"));
    key(&app, SDLK_ESCAPE);
    /* the game's menu's settings, named and worded as it shows them; a page's row opens the page, All first */
    {
        char line[64];
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        render(&app);
        shows(&app, "Overlay", normal);
        /* the rows' text past the gutter of Buttons' icon */
        snprintf(line, sizeof line, "-    %-13s %s", "Show controls", "Off"); /* the launcher's choice, 0 */
        shows(&app, line, yellow);
        snprintf(line, sizeof line, "- " ICON_LAYOUT_GRID " %-13s %s", "Buttons", ">");
        shows(&app, line, normal);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RIGHT); /* a page opens with Enter only */
        assert(app.page < 0);
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_GROUP && app.page >= 0 && app.editor.count == 3);
        render(&app);
        shows(&app, "Buttons", normal); /* the page open, over its rows */
        snprintf(line, sizeof line, "- %-8s %s", "All", "[default]");
        shows(&app, line, yellow);
        snprintf(line, sizeof line, "- %-8s %s", "B", "[default]"); /* an empty default */
        shows(&app, line, normal);
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "controls_a_shown", "0") && setting_is(&app, "controls_b_shown", "0"));
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "controls_a_shown", "1") && setting_is(&app, "controls_b_shown", "0"));
        render(&app);
        snprintf(line, sizeof line, "- %-8s %s", "All", "Mixed");
        shows(&app, line, normal);
        snprintf(line, sizeof line, "- %-8s %s", "A", "Shown");
        shows(&app, line, yellow);
        /* ESC comes back to the page's row, Del on it puts its rows back */
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_GROUP && app.page < 0 && app.editor.row == 1);
        key(&app, SDLK_DELETE);
        assert(!gui_settings_value(&app.settings, "controls_a_shown") &&
               !gui_settings_value(&app.settings, "controls_b_shown"));
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 4);
    }
    /* App's Reset to Defaults, its last row, asks first, with how many settings change; Back changes none, Reset puts every one
       back as it started, written to settings.ini, and says how many; none left to change, no box */
    {
        int changed = gui_settings_changed(&app.settings);
        char* asked = snx_format("Reset to Defaults? %d settings change", changed);
        char* done = snx_format("%d settings back to their defaults", changed);
        size_t size;
        char* kept;
        assert(changed >= 2); /* gdbstub and fullscreen */
        app.editor.row = 0;
        key(&app, SDLK_RETURN);
        app.editor.row = GUI_APP_RESET;
        assert(app.form == GUI_FORM_APP && GUI_APP_RESET == GUI_APP_ROWS - 1);
        render(&app);
        shows(&app, "- " ICON_ROTATE_CCW " Reset to Defaults", yellow);
        key(&app, SDLK_RETURN);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_RESET && !strcmp(app.ask_line, asked));
        render(&app);
        shows(&app, asked, rgb(app.theme.warning));
        shows(&app, "- Cancel", yellow);
        shows(&app, "- Reset", normal);
        key(&app, SDLK_RETURN); /* Back, in focus */
        assert(!app.confirm.shown && setting_is(&app, "gdbstub", "on") && app.form == GUI_FORM_APP);
        key(&app, SDLK_RETURN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !gui_settings_value(&app.settings, "gdbstub") &&
               setting_is(&app, "fullscreen", "0") && !gui_settings_changed(&app.settings));
        assert(!strcmp(app.editor.note, done) && app.form == GUI_FORM_APP);
        kept = (char*)snx_read_file(app.settings_file, &size);
        assert(kept && !strstr(kept, "gdbstub"));
        free(kept);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !strcmp(app.editor.note, "Every setting is at its default"));
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 0);
        free(asked);
        free(done);
    }
    app.editor.row = 2; /* fullscreen 1 again, for the runs below */
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "fullscreen", "1"));
    key(&app, SDLK_ESCAPE);
    /* App: the theme and the letters, either way round, and the theme editor, which comes back here */
    app.editor.row = 0;
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME && GUI_APP_ROWS == 11);
    {
        char first[64], line[1024];
        snprintf(first, sizeof first, "%s", app.theme.name);
        render(&app);
        shows(&app, "- " ICON_APP_WINDOW " App", yellow); /* the group open, as focused while its rows have the focus */
        /* past Background recompile, the longest; after the gutter of the icons of the rows that open or do
           something */
        snprintf(line, sizeof line, "-    %-20s %s", "Theme", first);
        shows(&app, line, yellow);
        shows(&app, "-    UI scale             Auto", normal);
        shows(&app, "-    Language             Auto: English", normal); /* SDL_PREFERRED_LOCALES, the test's */
        shows(&app, "- " ICON_PAINTBRUSH " Theme Editor", normal);
        /* under the rows, the three folders Folders moves, whole: the extractions too */
        shows(&app, "- " ICON_FOLDER " Folders              Defaults", normal);
        shows_half(&app, "Extractions:", rgb(app.theme.soft)); /* in a half step under the text's */
        { /* out of the data folder, as XDG's cache has them: without XDG (Windows) they are in it and read work/ */
            char *games = app.games_dir, *work = app.work_dir;
            app.work_dir = snx_path_join(temporary, "extractions");
            app_values(&app);
            snprintf(line, sizeof line, "Data: %s  Recompiled: %s  Extractions: %s", app.data_dir, app.games_dir,
                     app.work_dir);
            assert(!strcmp(app.places_note, line));
            /* one in the data folder, from it: the note stays short on a phone, whose data folder is a long path */
            app.games_dir = snx_recompiled_dir(app.data_dir);
            app_values(&app);
            snprintf(line, sizeof line, "Data: %s  Recompiled: recompiled%c  Extractions: %s", app.data_dir,
                     app.games_dir[strlen(app.data_dir)], app.work_dir);
            assert(!strcmp(app.places_note, line));
            free(app.games_dir);
            free(app.work_dir);
            app.games_dir = games;
            app.work_dir = work;
            app_values(&app);
        }
        shows(&app, "- " ICON_ERASER " Clean Old Settings", normal);
        hides(&app, "- Layout", normal);
        key(&app, SDLK_RIGHT);
        assert(strcmp(app.theme.name, first) && !strcmp(app.app_form.theme, app.theme.name));
        key(&app, SDLK_LEFT);
        assert(!strcmp(app.theme.name, first));
    }
    key(&app, SDLK_DOWN);
    /* UI scale: its sizes in a box, Auto's parts first; the one picked is used at once and asked about: Keep keeps it,
       Back, ESC or no answer in 15 s bring the one before it back */
    key(&app, SDLK_RIGHT);
    assert(app.sizes_shown && app.sizes.count == GUI_TEXT_MAX + 4 && app.sizes.focus == 3 && !app.menu.text_size);
    render(&app);
    { /* the pointer over a size: its focus; a click off the box closes it, nothing taken */
        GuiChoices c;
        sizes_place(&app, &c);
        point(&app, choice_box(&app, &c, 5), false);
        assert(app.sizes_shown && app.sizes.focus == 5);
        mouse_px(&app, c.box.x + c.box.w / 2, c.box.y + 2, true); /* on the box, off its rows: nothing */
        assert(app.sizes_shown && app.sizes.focus == 5 && !app.menu.text_size);
        mouse_px(&app, c.box.x - 4, c.box.y + c.box.h / 2, true);
        assert(!app.sizes_shown && !app.menu.text_size && !app.confirm.shown);
        key(&app, SDLK_RIGHT);
        assert(app.sizes_shown && app.sizes.focus == 3);
    }
    { /* in an area too short for them: the rows that fit, from the focus's; the arrow of those hidden above in the first
         row's box, at its right; YoRHa's rows from the area's top down to the question */
        GuiChoices c;
        GuiTheme was = app.theme;
        GuiRect first, short_area = gui_rect(0, 0, 60 * cell_w(&app), 6 * item_h(&app));
        size_t rows;
        int cw = cell_w(&app);
        choices_place(&app, GUI_CHOICES_SIZES, short_area, "UI scale", NULL, GUI_TEXT_MAX + 3, &c); /* 4x, the last */
        rows = ui_choices_shown(c.rects, app.scale, UI_CHOICES_ROWS);
        assert(c.count == GUI_TEXT_MAX + 4 && rows > 0 && rows < (size_t)c.count);
        assert(c.lists[UI_CHOICES_ROWS].first == (size_t)c.count - rows && c.box.y + c.box.h <= short_area.h);
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer); /* the dialog alone */
        choices_draw(&app, &c, "UI scale", NULL);
        first = choice_box(&app, &c, c.count - (int)rows);
        {
            int x, y;
            assert(find_text(&app, "\x1e", rgb(app.theme.soft), 1, &x, &y));
            assert(x == first.x + first.w - cw && y == item_text_y(&app, first));
            hides(&app, "\x1f", rgb(app.theme.soft)); /* the last in focus: none hidden below */
        }
        app.theme = *gui_theme_builtin("yorha");
        choices_place(&app, GUI_CHOICES_SIZES, short_area, "UI scale", NULL, 0, &c);
        rows = ui_choices_shown(c.rects, app.scale, UI_CHOICES_ROWS);
        assert(c.at.y == short_area.y && c.at.y + c.at.h <= c.box.y && rows > 0 && rows < (size_t)c.count);
        assert(!c.lists[UI_CHOICES_ROWS].first);
        app.theme = was;
        render(&app);
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(!app.sizes_shown && app.menu.text_size == 1 && app.scale == 1 && !strcmp(app.app_form.text, "1x"));
    assert(app.confirm.shown && app.confirm.ask == GUI_ASK_TEXT && strstr(app.size_line, "Back to Auto in 15 s"));
    render(&app);
    key(&app, SDLK_ESCAPE);
    assert(!app.confirm.shown && !app.menu.text_size && !strcmp(app.app_form.text, "Auto"));
    app.clock = 1000;
    key(&app, SDLK_RETURN);
    {
        GuiChoices c;
        render(&app);
        sizes_place(&app, &c);
        point(&app, choice_box(&app, &c, 1), true); /* 0.5 of Auto clicked */
    }
    assert(app.menu.text_size == -2 && app.scale == SDL_max(1, (app.auto_scale * 2 + 2) / 4) &&
           strstr(app.app_form.text, "0.5 of Auto: "));
    app.clock = 1000 + GUI_TEXT_KEEP_MS - 1;
    pump(&app);
    assert(app.confirm.shown && strstr(app.size_line, "in 1 s"));
    app.clock = 1000 + GUI_TEXT_KEEP_MS;
    pump(&app);
    assert(!app.confirm.shown && !app.menu.text_size && !app.size_until);
    app.clock = 0;
    /* a part of Auto is Auto's scale times it, rounded, never under 1x */
    {
        int was = app.auto_scale;
        app.auto_scale = 4;
        for (int q = 1; q <= 3; ++q) {
            app.menu.text_size = -q;
            layout(&app);
            assert(app.scale == q);
        }
        app.auto_scale = 2;
        app.menu.text_size = -1;
        layout(&app);
        assert(app.scale == 1);
        app.menu.text_size = -3;
        layout(&app);
        assert(app.scale == 2);
        app.auto_scale = was;
        app.menu.text_size = 0;
        layout(&app);
    }
    assert(!app.menu.text_size && app.editor.row == GUI_APP_TEXT);
    /* the App's picks kept at once in app.ini, which the next start reads back */
    {
        size_t size = 0;
        char *kept, expected[512], id[64], first_id[64];
        const char* broken = "[app]\ntheme = gone\ntext_size = 9\nlayout = boxes\ncolour = red\ntext_size = 2x\n"
                             "[other]\nlayout = panels\n";
        char* blocker = snx_path_join(temporary, "not a folder");
        char* kept_file = app.app_file;
        snprintf(first_id, sizeof first_id, "%s", app.theme.id);
        pick_size(&app, 1, true);
        key(&app, SDLK_UP);
        key(&app, SDLK_RIGHT); /* the next theme */
        snprintf(id, sizeof id, "%s", app.theme.id);
        assert(strcmp(id, first_id));
        snprintf(expected, sizeof expected,
                 APP_INI_HEAD "theme = %s\nui_scale = 1\nlanguage = auto\n"
                              "animations = 0\nside_box = auto\nrecompile_background = full\n" GAMES_DEFAULTS,
                 id);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && size == strlen(expected) && !memcmp(kept, expected, size));
        free(kept);
        /* a start with them: the theme and the letters as they were left, and nothing to say */
        gui_theme_load(&app.theme, (const char* const*)app.themes_dirs, 2, first_id, NULL, 0);
        app.menu.text_size = 0;
        app.message[0] = 0;
        app_load(&app);
        assert(!strcmp(app.theme.id, id) && app.menu.text_size == 1 && app.scale == 1 && !app.message[0]);
        /* back as they were, kept so */
        key(&app, SDLK_LEFT);
        key(&app, SDLK_DOWN);
        pick_size(&app, 0, true);
        assert(!strcmp(app.theme.id, first_id) && !app.menu.text_size && !app.editor.note[0]);
        /* a part of Auto kept as app.ini names it, and read back */
        pick_size(&app, -3, true);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && strstr(kept, "\nui_scale = 0.75\n"));
        free(kept);
        app.menu.text_size = 0;
        app_load(&app);
        assert(app.menu.text_size == -3);
        pick_size(&app, 0, true);
        snprintf(expected, sizeof expected,
                 APP_INI_HEAD
                 "theme = %s\nui_scale = auto\n"
                 "language = auto\nanimations = 0\nside_box = auto\nrecompile_background = full\n" GAMES_DEFAULTS,
                 first_id);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && size == strlen(expected) && !memcmp(kept, expected, size));
        free(kept);
        /* text_size =, as an older version named ui_scale =: read, the file written again with ui_scale = in its
           place, said once with the value; the next start says nothing */
        {
            char* old = snx_format("[app]\ntheme = %s\ntext_size = 2\n", first_id);
            assert(old && !snx_write_file(app.app_file, old, strlen(old)));
            free(old);
            app.menu.text_size = 0;
            app.message[0] = 0;
            app_load(&app);
            snprintf(expected, sizeof expected, "%s: text_size = 2 is now ui_scale = 2", app.app_file);
            assert(app.menu.text_size == 2 && !strcmp(app.message, expected));
            kept = (char*)snx_read_file(app.app_file, &size);
            assert(kept && strstr(kept, "\nui_scale = 2\n") && !strstr(kept, "text_size"));
            free(kept);
            app.message[0] = 0;
            app_load(&app);
            assert(app.menu.text_size == 2 && !app.message[0]);
            pick_size(&app, 0, true);
        }
        /* a file a hand broke: what it does not know is left, named, and the menu keeps what it has */
        assert(!snx_write_file(app.app_file, broken, strlen(broken)));
        app_load(&app);
        assert(!strcmp(app.theme.id, first_id) && !app.menu.text_size);
        snprintf(expected, sizeof expected,
                 "Not used from %s: theme = gone, text_size = 9, layout = boxes, colour = red, text_size = 2x, "
                 "[other] layout = panels",
                 app.app_file);
        assert(!strcmp(app.message, expected));
        app.message[0] = 0;
        /* a file that cannot be written: the pick holds for the session, and the note says it is not kept */
        assert(!snx_write_file(blocker, "file", 4));
        app.app_file = snx_path_join(blocker, "app.ini");
        snprintf(expected, sizeof expected, "Not kept for the next start: %s could not be written", app.app_file);
        key(&app, SDLK_RETURN);
        app.sizes.focus = 1 + 3;
        key(&app, SDLK_RETURN);
        key(&app, SDLK_RIGHT);
        key(&app, SDLK_RETURN); /* kept: a box says it is not, whole, until OK; the note after */
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ERROR && !strcmp(app.error_line, expected));
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && app.menu.text_size == 1 && !snx_is_file(app.app_file));
        assert(!strcmp(app.editor.note, expected));
        free(app.app_file);
        app.app_file = kept_file;
        pick_size(&app, 0, true);
        assert(!app.menu.text_size);
        remove(blocker);
        free(blocker);
        app.editor.note[0] = 0;
    }
    /* Language: Auto (the system's: English here, SDL_PREFERRED_LOCALES), English, then the test's xx and zh-Hant-TW
       (tests/po), round either way; kept in app.ini and read back, an older form of a tag written as the tag; its words
       on the screens, a key of the foot found by its own */
    {
        size_t size = 0;
        char *kept, expected[512], line[128];
        const char* other = "[app]\nlanguage = xx\n";
        const char* unknown = "[app]\nlanguage = tlh\n";
        open_app(&app, GUI_APP_LANGUAGE);
        assert(!app.language && snx_tr_language() == 0 && !strcmp(app.app_form.language, "Auto: English"));
        key(&app, SDLK_RIGHT);
        assert(app.language == 1 && snx_tr_language() == 0 && !strcmp(app.app_form.language, "English"));
        key(&app, SDLK_RETURN); /* Enter goes round too */
        assert(app.language == 2 && snx_tr_language() == 1 && !strcmp(app.app_form.language, "Test\303\252s"));
        snprintf(expected, sizeof expected,
                 APP_INI_HEAD
                 "theme = %s\nui_scale = auto\n"
                 "language = xx\nanimations = 0\nside_box = auto\nrecompile_background = full\n" GAMES_DEFAULTS,
                 app.theme.id);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && size == strlen(expected) && !memcmp(kept, expected, size));
        free(kept);
        render(&app);
        /* the labels' columns as they read: xx's Language, its i accented, is 6 of them in 7 bytes (the font's one) */
        snprintf(line, sizeof line, "-    L\241ngua%14s Test", "");
        shows(&app, line, yellow);
        shows(&app, "-    UI scale", normal); /* not translated in xx: English */
        shows(&app, "Ajustes", yellow);       /* the tab */
        shows_foot(&app, "volta", normal);    /* the foot's own key, ESC alone at its left, translated */
        hides_foot(&app, "linha", normal);    /* the keys every screen has: in F1's box alone */
        hides_foot(&app, "tela", normal);
        hides_foot(&app, "change or open", normal);
        {
            int form = app.form;
            key(&app, SDLK_F1);
            render(&app);
            assert(app.help_shown);
            shows(&app, "linha", normal); /* F1's box: every key of the screen, translated */
            shows(&app, "tela", normal);
            key(&app, SDLK_ESCAPE); /* any key closes it, and does nothing else */
            assert(!app.help_shown && app.editing && app.form == form);
            render(&app);
        }
        /* xx's wide letters, two columns each: the Menu tab (U+83DC U+5355) as wide as they are, the Theme row's value
           (U+4E3B U+9898) lined up with the others, a foot's keys (Up/Down as U+4E0A/U+4E0B) found by their columns */
        {
            GuiRect none = {0, 0, 0, 0};
            int x, y, up = hint_word("Up", 2, false), down = hint_word("Down", 4, false);
            assert(tab_rect(&app, none, GUI_TAB_MENU).w == 6 * cell_w(&app));
            assert(tab_rect(&app, none, GUI_TAB_GAMES).x == 7 * cell_w(&app));
            assert(find_letters(&app, "\xe8\x8f\x9c\xe5\x8d\x95", normal, 1, &x, &y));
            snprintf(line, sizeof line, "-    \xe4\xb8\xbb\xe9\xa2\x98%16s %.4s", "", app.app_form.theme);
            assert(find_letters(&app, line, normal, 1, &x, &y));
            assert(hint_width("\xe4\xb8\x8a/\xe4\xb8\x8b", "linha") == 11 && up >= 0 && down >= 0 && up != down);
            assert(hint_pick("\xe4\xb8\x8a/\xe4\xb8\x8b", 1, false) == up &&
                   hint_pick("\xe4\xb8\x8a/\xe4\xb8\x8b", 2, false) == up);
            assert(hint_pick("\xe4\xb8\x8a/\xe4\xb8\x8b", 3, false) == down &&
                   hint_pick("\xe4\xb8\x8a/\xe4\xb8\x8b", 4, false) == down);
            assert(hint_pick("\xe4\xb8\x8a/\xe4\xb8\x8b", 5, false) == -1); /* past them: no one key for both */
        }
        key(&app, SDLK_RIGHT);
        assert(app.language == 3 && snx_tr_language() == 2 &&
               !strcmp(app.app_form.language, "\xe4\xb8\xad\xe6\x96\x87 (TW)"));
        key(&app, SDLK_RIGHT);
        assert(!app.language && snx_tr_language() == 0); /* Auto again: English */
        key(&app, SDLK_LEFT);
        assert(app.language == 3 && snx_tr_language() == 2);
        key(&app, SDLK_LEFT);
        assert(app.language == 2 && snx_tr_language() == 1);
        /* a start with them: xx as it was left, a language the menu does not have named and left out */
        app.language = 0;
        language_apply(&app);
        assert(!snx_write_file(app.app_file, other, strlen(other)));
        app.message[0] = 0;
        app_load(&app);
        assert(app.language == 2 && snx_tr_language() == 1 && !app.message[0]);
        assert(!snx_write_file(app.app_file, unknown, strlen(unknown)));
        app_load(&app);
        assert(app.language == 2 && snx_tr_language() == 1);
        snprintf(expected, sizeof expected, "Fora de uso em %s: language = tlh", app.app_file); /* in xx: its words */
        assert(!strcmp(app.message, expected));
        app.message[0] = 0;
        /* zh_TW, as a build before the tags wrote zh-Hant-TW: that language, its tag written in place of it, the file's
           other lines as they were, and the message saying so once */
        {
            const char* older = "; kept\n[app]\ntheme = no-such-theme\nlanguage = zh_TW\n";
            const char* written = "; kept\n[app]\ntheme = no-such-theme\nlanguage = zh-Hant-TW\n";
            assert(!snx_write_file(app.app_file, older, strlen(older)));
            app_load(&app);
            assert(app.language == 3 && snx_tr_language() == 2);
            snprintf(expected, sizeof expected, "%s: language = zh_TW is now zh-Hant-TW", app.app_file);
            assert(strstr(app.message, expected));
            kept = (char*)snx_read_file(app.app_file, &size);
            assert(kept && size == strlen(written) && !memcmp(kept, written, size));
            free(kept);
            app.message[0] = 0;
            app_load(&app);
            assert(app.language == 3 && !strstr(app.message, "is now"));
            assert(!snx_write_file(app.app_file, other, strlen(other)));
            app.message[0] = 0;
            app_load(&app);
            assert(app.language == 2 && snx_tr_language() == 1 && !app.message[0]);
        }
        /* the foot's Back, as xx says it, presses Back: the App's rows left for the groups */
        assert(hint_press(&app, "Volta", 0) && app.editing && app.form == GUI_FORM_SETTINGS);
        assert(hint_press(&app, "Back", 0) && !app.editing); /* and as English does */
        /* Auto: the first of the system's languages the menu has, for its country or another; English for none. Over
           the test's SDL_PREFERRED_LOCALES, which a hint of normal priority does not override */
        SDL_SetHintWithPriority(SDL_HINT_PREFERRED_LOCALES, "ja_JP,xx_YY,en_US", SDL_HINT_OVERRIDE);
        assert(language_auto() == 1);
        SDL_SetHintWithPriority(SDL_HINT_PREFERRED_LOCALES, "ja_JP,en_US,xx", SDL_HINT_OVERRIDE);
        assert(language_auto() == 0);
        SDL_SetHintWithPriority(SDL_HINT_PREFERRED_LOCALES, "ja_JP", SDL_HINT_OVERRIDE);
        assert(language_auto() == 0);
        SDL_ResetHint(SDL_HINT_PREFERRED_LOCALES); /* the test's own again, en_US */
        assert(language_auto() == 0);
        /* English for what follows, kept so, on App as the next part opens it */
        open_app(&app, GUI_APP_LANGUAGE);
        key(&app, SDLK_RIGHT); /* xx, zh-Hant-TW, then Auto */
        key(&app, SDLK_RIGHT);
        assert(!app.language && snx_tr_language() == 0);
    }
    /* the Latin letters past CP437 drawn from Unifont's glyphs (latin.h), the a and the o with a tilde beside CP437's
       c with a cedilla, by each way the menu draws a line: at a cell, in a box, and in the title's bigger letters */
    {
        static const char words[] = "n\303\243o a\303\247\303\265es";
        SDL_Color white = {255, 255, 255, 255};
        int x, y, w = cell_w(&app), h = cell_h(&app);
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        text(&app, 2, 1, words, white);
        assert(find_letters(&app, words, white, 1, &x, &y) && x == 2 * w && y == h);
        assert(!find_text(&app, "n?o a\207?es", white, 1, &x, &y)); /* not CP437's question marks */
        SDL_RenderClear(app.renderer);
        text_in(&app, gui_rect(0, 0, 40 * w, 4 * h), 3 * w, 2 * h, words, white);
        assert(find_letters(&app, words, white, 1, &x, &y) && x == 3 * w && y == 2 * h);
        SDL_RenderClear(app.renderer);
        big_text(&app, gui_rect(0, 0, 60 * w, 6 * h), w, h, words, 2, white);
        assert(find_letters(&app, words, white, 2, &x, &y) && x == w && y == h);
    }
    /* the wide letters (tests/po/xx.po's, Unifont's 16x16) two columns each, by each way the menu draws a line: at a
       cell, in a box, in the title's bigger letters and a column count at a time; none drawn across an edge, a row
       wrapped after one */
    {
        static const char menu[] = "\xe8\x8f\x9c\xe5\x8d\x95" /* U+83DC U+5355, then A: 5 columns */ "A";
        static const char cai[] = "\xe8\x8f\x9c", dan[] = "\xe5\x8d\x95";
        SDL_Color white = {255, 255, 255, 255};
        int x, y, w = cell_w(&app), h = cell_h(&app);
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        text(&app, 2, 1, menu, white);
        assert(find_letters(&app, menu, white, 1, &x, &y) && x == 2 * w && y == h);
        { /* over its row by 2 columns, slid by 1: past the first letter whole, the second at the row's start */
            Uint32 scroll = app.theme.scroll_ms;
            Uint64 clock = app.clock;
            app.theme.scroll_ms = 100;
            for (app.clock = 1; gui_slide(2, app.clock, app.theme.scroll_ms) != 1;) app.clock += 100;
            SDL_RenderClear(app.renderer);
            text(&app, app.cols - 4, 1, menu, white);
            assert(find_letters(&app, menu + 3, white, 1, &x, &y) && x == (app.cols - 4) * w && y == h);
            app.theme.scroll_ms = scroll;
            app.clock = clock;
        }
        SDL_RenderClear(app.renderer);
        text_in(&app, gui_rect(0, 0, 6 * w, 4 * h), 3 * w, 2 * h, menu, white); /* 3 columns: the first alone */
        assert(find_letters(&app, cai, white, 1, &x, &y) && x == 3 * w && y == 2 * h);
        assert(!find_letters(&app, dan, white, 1, &x, &y));
        SDL_RenderClear(app.renderer);
        big_text(&app, gui_rect(0, 0, 60 * w, 6 * h), w, h, menu, 2, white);
        assert(find_letters(&app, menu, white, 2, &x, &y) && x == w && y == h);
        SDL_RenderClear(app.renderer);
        big_text(&app, gui_rect(0, 0, 7 * w, 6 * h), w, h, menu, 2, white); /* 6 columns at twice the size: one */
        assert(find_letters(&app, cai, white, 2, &x, &y) && x == w && !find_letters(&app, dan, white, 2, &x, &y));
        SDL_RenderClear(app.renderer);
        assert(text_px(&app, (float)w, (float)h, menu, 3, white, 1) == (float)(3 * w));
        assert(find_letters(&app, cai, white, 1, &x, &y) && x == w && !find_letters(&app, dan, white, 1, &x, &y));
        assert(text_px(&app, (float)w, (float)h, menu, 5, white, 1) == (float)(6 * w));
        /* widths in columns: the cursor's margin, the user's box, a question's box */
        {
            char cursor[sizeof app.theme.cursor], name[sizeof app.user_name], room[sizeof app.room_line];
            int user = app.user, side = 3 * h;
            GuiAsk ask = app.confirm.ask;
            GuiPicture picture = app.theme.picture;
            GuiChoices choices;
            GuiRect asked, ascii;
            memcpy(cursor, app.theme.cursor, sizeof cursor);
            memcpy(name, app.user_name, sizeof name);
            memcpy(room, app.room_line, sizeof room);
            snprintf(app.theme.cursor, sizeof app.theme.cursor, "%s", cai);
            assert(ui_row_box(&app, gui_rect(0, 0, 40 * w, h)).x == 2 * w); /* a row's box past the cursor's margin */
            app.user = 1;
            app.theme.picture = GUI_PICTURE_SQUARE;
            snprintf(app.user_name, sizeof app.user_name, "%s", menu);
            assert(user_width(&app, side) == 7 * w + user_side(&app, side) + 2 * app.scale);
            app.confirm.ask = GUI_ASK_ROOM;
            snprintf(app.room_line, sizeof app.room_line, "%s%s%s%s%s", menu, menu, menu, menu, menu); /* 25 columns */
            ask_boxes(&app, &asked, &choices);
            snprintf(app.room_line, sizeof app.room_line, "%25s", "");
            ask_boxes(&app, &ascii, &choices);
            assert(asked.w == ascii.w && asked.w == 27 * w && asked.h == ascii.h); /* on one row, wrapped at 25 */
            memcpy(app.theme.cursor, cursor, sizeof cursor);
            memcpy(app.user_name, name, sizeof name);
            memcpy(app.room_line, room, sizeof room);
            app.user = user;
            app.confirm.ask = ask;
            app.theme.picture = picture;
        }
        /* a line wraps after a wide letter as at a space: "U+83DC U+5355 U+83DC" in 7 columns */
        assert(wrap_at("\xe8\x8f\x9c\xe5\x8d\x95 \xe8\x8f\x9c\xe5\x8d\x95\xe8\x8f\x9c\xe5\x8d\x95", 7, 0) == 10);
        assert(wrap_at("\xe8\x8f\x9c\xe5\x8d\x95\xe8\x8f\x9c", 5, 0) == 6 && wrap_at("ab cd", 4, 0) == 2);
    }
    /* Clean Old Settings: a box says what it would do, Clean does it and shows what it did over the screen, until a
       key closes it; then there is nothing left. The tips say a box asks */
    {
        char *old_ini = snx_path_join(app.games_dir, "Old game.ini"), *kept, unread[64], expected[512];
        const char* game_ini =
            "[Other]\nlanguage=en\n\n[Deprecated]\n; renderer: Vulkan is the only one\nrenderer=gl\n";
        const char* gone = "[app]\ntheme = gone\n"; /* a theme taken away */
        SDL_Color title = rgb(app.theme.panels == GUI_PANELS_FILL ? app.theme.page : app.theme.ink);
        size_t size = 0;
        int row = app.editor.row, had_games = snx_is_dir(app.games_dir); /* the tests after this count on its absence */
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(old_ini, game_ini, strlen(game_ini)));
        kept = (char*)snx_read_file(app.settings_file, &size);
        {
            char* more = snx_format("%s[Display]\nrenderer=opengl\n", kept ? kept : "");
            assert(!snx_write_file(app.settings_file, more, strlen(more)));
            free(more);
        }
        free(kept);
        assert(settings_unread(&app, unread, sizeof unread) == 1 && !strcmp(unread, "renderer=opengl"));
        assert(!snx_write_file(app.app_file, gone, strlen(gone)));
        app.editor.row = GUI_APP_CLEAN;
        render(&app);
        shows_half(&app, "this version no longer reads. Asks first", normal);
        key(&app, SDLK_RETURN); /* a box says what it would take out, Back in focus */
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_CLEAN && !app.cleaned_shown && snx_is_file(old_ini));
        assert(!strcmp(app.ask_line, "Clean Old Settings? [Deprecated] in 1 .ini, 2 old keys"));
        render(&app);
        shows(&app, "Clean Old Settings? [Deprecated] in 1 .ini, 2 old keys", rgb(app.theme.warning));
        shows(&app, "- Cancel", yellow);
        shows(&app, "- Clean", normal);
        key(&app, SDLK_ESCAPE); /* Back: nothing taken out */
        assert(!app.confirm.shown && !app.cleaned_shown && app.editor.row == GUI_APP_CLEAN);
        kept = (char*)snx_read_file(old_ini, &size);
        assert(kept && !strcmp(kept, game_ini));
        free(kept);
        key(&app, SDLK_RETURN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && app.cleaned_shown && app.cleaned.count == 3 && app.cleaned.done == 3);
        assert(!strcmp(app.editor.note, "Cleaned: 3 done, 0 not"));
        kept = (char*)snx_read_file(old_ini, &size);
        assert(kept && !strcmp(kept, "[Other]\nlanguage=en\n"));
        free(kept);
        assert(!settings_unread(&app, unread, sizeof unread) && setting_is(&app, "fullscreen", "1"));
        snprintf(expected, sizeof expected,
                 "+\t%s\t[Deprecated] removed\n+\t%s\tRemoved renderer=opengl\n+\t%s\tRemoved theme = gone\n", old_ini,
                 app.settings_file, app.app_file);
        assert(!strcmp(app.cleaned.text, expected));
        kept = (char*)snx_read_file(app.app_file, &size); /* the picks as the menu has them */
        snprintf(expected, sizeof expected, "\ntheme = %s\n", app.theme.id);
        assert(kept && strstr(kept, expected) && !strstr(kept, "gone"));
        free(kept);
        render(&app);
        shows(&app, "Clean Old Settings: 3 done", title);
        shows(&app, "[Deprecated] removed", rgb(app.theme.soft));
        shows(&app, "- OK", yellow);
        key(&app, SDLK_DOWN); /* the three show: nothing to go to */
        assert(app.cleaned_shown && !app.files_top && app.editor.row == GUI_APP_CLEAN);
        key(&app, SDLK_LEFT); /* closes it, and does nothing else */
        assert(!app.cleaned_shown && app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_CLEAN);
        render(&app);
        hides(&app, "Clean Old Settings: 3 done", title);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !strncmp(app.editor.note, "Nothing to clean", 16));
        /* a runtime that listed no settings: every key of settings.ini would look old, and none is taken out */
        {
            GuiSettings listed = app.settings;
            app.settings = (GuiSettings){0};
            assert(!gui_settings_read(&app.settings, "") && app.settings.count == app.settings.own);
            assert(!settings_unread(&app, unread, sizeof unread) && !unread[0]);
            gui_settings_free(&app.settings);
            app.settings = listed;
            assert(setting_is(&app, "fullscreen", "1"));
        }
        remove(old_ini);
        if (!had_games) assert(!snx_remove_tree(app.games_dir));
        free(old_ini);
        app.editor.row = row;
        app.editor.note[0] = 0;
    }
    /* Move Old Files: what an older version left, moved where this one keeps it once asked, entry by entry; a game laid
       out beside others is not, the report says how it goes; then nothing that can move, said; nothing left, said */
    {
        char *data_was = app.data_dir, *older = snx_path_join(temporary, "move data"), *old_games, *old_game;
        char *laid = snx_path_join(app.games_dir, "Laid"), *laid_data = snx_path_join(app.games_dir, "Laid_data");
        char *moved = snx_format("%s/Old/Old", app.games_dir), *dir;
        int row = app.editor.row, had_games = snx_is_dir(app.games_dir);
        SDL_Color title = rgb(app.theme.panels == GUI_PANELS_FILL ? app.theme.page : app.theme.ink);
        app.data_dir = older;
        old_games = snx_path_join(older, "games");
        old_game = snx_format("%s/Old/Old", old_games);
        dir = snx_dirname(old_game);
        assert(!snx_mkdirs(dir) && !snx_write_file(old_game, "fixture", 7) && !snx_mkdirs(laid_data));
        assert(!snx_write_file(laid, "fixture", 7));
        free(dir);
        app.editor.row = GUI_APP_MOVE;
        key(&app, SDLK_RETURN);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_MOVE &&
               !strcmp(app.ask_line, "Move Old Files? 1 move, 1 stay where they are"));
        assert(!strcmp(app.app_form.moves, "2 left"));
        render(&app);
        shows(&app, "- Move", normal);
        key(&app, SDLK_ESCAPE); /* Back: nothing moved */
        assert(!app.confirm.shown && snx_is_file(old_game) && !snx_path_exists(moved));
        key(&app, SDLK_RETURN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && app.cleaned_shown && app.report_move && snx_is_file(moved));
        assert(!snx_path_exists(old_games) && snx_is_file(laid)); /* games/ went once empty; Laid stays */
        assert(!strcmp(app.editor.note, "1 moved, 1 not") && app.cleaned.count == 3 && app.cleaned.done == 2);
        assert(strstr(app.cleaned.text, "Not moved: run it from its dump once, which makes "));
        render(&app);
        shows(&app, "Move Old Files: 2 done, 1 not", title);
        key(&app, SDLK_LEFT); /* the report closes */
        assert(!app.cleaned_shown && app.lefts == 1 && !strcmp(app.app_form.moves, "1 left"));
        key(&app, SDLK_RETURN); /* only Laid: none can move, the report says why at once */
        assert(!app.confirm.shown && app.cleaned_shown && app.cleaned.count == 1 && !app.cleaned.done &&
               !strcmp(app.editor.note, "None of the 1 can move: the box says why"));
        key(&app, SDLK_LEFT);
        assert(!snx_remove_tree(laid_data) && !remove(laid));
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !app.cleaned_shown &&
               !strcmp(app.editor.note, "Nothing left by an older version"));
        dir = snx_dirname(moved);
        assert(!snx_remove_tree(dir) && !snx_remove_tree(older));
        if (!had_games) assert(!snx_remove_tree(app.games_dir));
        free(dir);
        free(old_game);
        free(old_games);
        free(older);
        app.data_dir = data_was;
        left_behind(&app);
        gui_cleaned_free(&app.cleaned);
        app.report_move = false;
        app.editor.row = row;
        app.editor.note[0] = 0;
        free(moved);
        free(laid_data);
        free(laid);
#ifdef SNX_XDG
        /* scarletnx --migrate, the same without the menu, in the XDG folders (Linux's): each entry a line and where it
           goes, nothing touched; =yes moves them; any other value refused, naming it */
        {
            char *xdg = snx_path_join(temporary, "migrate"), *was[4], *data_home = snx_path_join(xdg, "data");
            char *game = snx_format("%s/scarletnx/games/G/G", data_home),
                 *now = snx_format("%s/scarletnx/recompiled/G/G", data_home);
            static const char* const vars[4] = {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
            char said[8192];
            size_t n;
            FILE* out;
            for (int v = 0; v < 4; ++v) {
                char* to = snx_format("%s/%d", xdg, v);
                was[v] = getenv(vars[v]) ? snx_strdup(getenv(vars[v])) : NULL;
                set_env(vars[v], v ? to : data_home);
                free(to);
            }
            dir = snx_dirname(game);
            assert(!snx_mkdirs(dir) && !snx_write_file(game, "fixture", 7));
            free(dir);
            { /* the menu's gui/ of an older version: its files split between the config and state folders */
                char *gui = snx_format("%s/scarletnx/gui", data_home), *recent = snx_path_join(gui, "recent.txt"),
                     *picks = snx_path_join(gui, "app.ini");
                assert(!snx_mkdirs(gui) && !snx_write_file(recent, "", 0) && !snx_write_file(picks, "", 0));
                free(picks);
                free(recent);
                free(gui);
            }
            for (int apply = 0; apply < 2; ++apply) {
                char* line = snx_format("%s/scarletnx/games/G: %s to %s/scarletnx/recompiled/G\n%s", data_home,
                                        apply ? "Moved" : "Moves", data_home,
                                        apply ? "" : "3 to move, 0 staying: --migrate=yes moves them\n");
                assert((out = tmpfile()) != NULL);
                assert(!migrate(app.self, apply, out));
                rewind(out);
                n = fread(said, 1, sizeof said - 1, out);
                said[n] = 0;
                fclose(out);
                if (!apply) { /* nothing touched, no folder made */
                    char *config = snx_path_join(xdg, "1"), *state = snx_path_join(xdg, "2");
                    char* dumps = snx_format("%s/scarletnx/dumps", data_home);
                    assert(strstr(said, line) && snx_is_file(game) && !snx_path_exists(now));
                    assert(!snx_path_exists(config) && !snx_path_exists(state) && !snx_path_exists(dumps));
                    free(dumps);
                    free(state);
                    free(config);
                } else {
                    char *recent = snx_format("%s/2/scarletnx/recent.txt", xdg),
                         *picks = snx_format("%s/1/scarletnx/app.ini", xdg);
                    assert(strstr(said, line) && strstr(said, "3 moved, 0 not\n") && snx_is_file(now) &&
                           !snx_path_exists(game));
                    assert(snx_is_file(recent) && snx_is_file(picks));
                    free(picks);
                    free(recent);
                }
                free(line);
            }
            assert(snx_gui_migrate(app.self, "--migrate=maybe") == 2);
            for (int v = 0; v < 4; ++v) {
                set_env(vars[v], was[v]);
                free(was[v]);
            }
            assert(!snx_remove_tree(xdg));
            free(now);
            free(game);
            free(data_home);
            free(xdg);
        }
#endif
    }
    key(&app, SDLK_DOWN);
    /* Animations: On or Off, either way, kept in app.ini as animations = 1 or 0 */
    assert(app.editor.row == GUI_APP_ANIMATIONS && !strcmp(app.app_form.animations, "Off"));
    key(&app, SDLK_RIGHT);
    assert(!app.animations_off && !strcmp(app.app_form.animations, "On"));
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && strstr(kept, "\nanimations = 1\n"));
        free(kept);
    }
    key(&app, SDLK_RETURN);
    assert(app.animations_off && !strcmp(app.app_form.animations, "Off")); /* the checks below read settled screens */
    key(&app, SDLK_DOWN);
    /* Side panel: Auto, On, Off either way round, kept in app.ini as side_box = auto, on or off */
    assert(app.editor.row == GUI_APP_SIDE && !strcmp(app.app_form.side, "Auto"));
    key(&app, SDLK_RIGHT);
    assert(app.side_box == GUI_SIDE_ON && !strcmp(app.app_form.side, "On"));
    key(&app, SDLK_RIGHT);
    assert(app.side_box == GUI_SIDE_OFF && !strcmp(app.app_form.side, "Off"));
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && strstr(kept, "\nside_box = off\n"));
        free(kept);
    }
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    assert(app.side_box == GUI_SIDE_AUTO && !strcmp(app.app_form.side, "Auto"));
    /* app.ini's side_box read back: on taken; a word it does not know named, the side box as the line before left it */
    {
        size_t size;
        char *kept = (char*)snx_read_file(app.app_file, &size), *text, said[512];
        assert(kept);
        text = snx_format("%sside_box = on\nside_box = sideways\n", kept);
        assert(!snx_write_file(app.app_file, text, strlen(text)));
        app.message[0] = 0;
        app_load(&app);
        snprintf(said, sizeof said, "Not used from %s: side_box = sideways", app.app_file);
        assert(app.side_box == GUI_SIDE_ON && strstr(app.message, said));
        app.message[0] = 0;
        app.side_box = GUI_SIDE_AUTO;
        layout(&app);
        assert(!app_save(&app));
        app_values(&app);
        free(text);
        free(kept);
    }
    key(&app, SDLK_DOWN);
    /* Background recompile: Full or Gentle, either way, kept in app.ini as recompile_background = full or gentle */
    assert(app.editor.row == GUI_APP_BACKGROUND && !app.background_gentle && !strcmp(app.app_form.background, "Full"));
    key(&app, SDLK_RIGHT);
    assert(app.background_gentle && !strcmp(app.app_form.background, "Gentle"));
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && strstr(kept, "\nrecompile_background = gentle\n"));
        free(kept);
    }
    key(&app, SDLK_LEFT);
    assert(!app.background_gentle && !strcmp(app.app_form.background, "Full"));
    key(&app, SDLK_RETURN);
    assert(app.background_gentle);
    key(&app, SDLK_RETURN);
    assert(!app.background_gentle);
    /* read back: gentle taken; a word it does not know named, with the one the file leaves in use; an older version's
       app.ini, without the key, Full as the menu opens, the key written with the next save */
    {
        size_t size;
        char *kept = (char*)snx_read_file(app.app_file, &size), *text, said[512];
        assert(kept);
        text = snx_format("%srecompile_background = gentle\nrecompile_background = slow\n", kept);
        assert(!snx_write_file(app.app_file, text, strlen(text)));
        app.message[0] = 0;
        app_load(&app);
        snprintf(said, sizeof said, "Not used from %s: recompile_background = slow (it keeps gentle)", app.app_file);
        assert(app.background_gentle && strstr(app.message, said));
        free(text);
        text = snx_format(APP_INI_HEAD "theme = %s\nside_box = auto\n", app.theme.id);
        assert(!snx_write_file(app.app_file, text, strlen(text)));
        app.background_gentle = false; /* as the menu opens */
        app.message[0] = 0;
        app_load(&app);
        assert(!app.background_gentle && !strstr(app.message, "recompile_background"));
        assert(!app_save(&app));
        free(kept);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && strstr(kept, "\nside_box = auto\nrecompile_background = full\n"));
        app.message[0] = 0;
        app_values(&app);
        free(text);
        free(kept);
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT); /* the editor opens with Enter only */
    assert(app.form == GUI_FORM_APP);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_THEME && app.editing);
    key(&app, SDLK_ESCAPE);
    assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_EDITOR);
    /* Folders, every folder on one screen, a box a group: the app's (Data, Settings read-only, Logs, Caches,
       Extractions), the recompiled library (its
       Main, recompiled/, its Extras, Add Folder), the dumps one (its Main, dumps/, its Extras, Add Folder); one
       dropped on a library's row is added to it and kept, once; the picker answers none here
       (SDL_FILE_DIALOG_DRIVER=none); Del or Enter asks before an Extra is taken off, the folder staying */
    key(&app, SDLK_DOWN);
    render(&app);
    shows(&app, "- " ICON_FOLDER " Folders              Defaults", yellow);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_FOLDERS && gui_editor_rows(&app.editor) == 10 && app.editor.row == 0);
    assert(FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_DATA) == 0 && FOLDER_ROW(GUI_ROW_CONFIG, -1) == 1 &&
           FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_STATE) == 2 && FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_CACHE) == 3 &&
           FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_WORK) == 4 && FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES) == 5 &&
           FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED) == 6 && FOLDER_ROW(GUI_ROW_DUMPS, -1) == 7 &&
           FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_DUMPS) == 8);
    /* no Back row over the rows: the foot's ESC, clicked, does what the key does, back on the App's row */
    {
        GuiFormBoxes f;
        render(&app);
        hides(&app, "Back [ESC]", normal);
        form_boxes(&app, &f);
        /* the whole screen, the app's title first: none beside it; a row each folder row and group title, and the
           last */
        assert(form_row_box(&app, &f, true, 0).y == f.place.side.y + 2 * app.scale && !form_shown(&app, &f, false) &&
               !f.place.status.h && f.place.side.w == f.place.head.w);
        {
            UiRowInfo info = {0};
            /* Reset to Defaults in a group of its own, Defaults, after a blank entry */
            assert(ui_form_side_rows_get_row_info(&app, 9 + GUI_FOLDER_GROUPS, 100, &info) &&
                   info.attr == UI_ATTR_LABEL && !info.label[0]);
            info = (UiRowInfo){0};
            assert(ui_form_side_rows_get_row_info(&app, 9 + GUI_FOLDER_GROUPS + 1, 100, &info) &&
                   info.attr == UI_ATTR_TITLE && !strcmp(info.label, "Defaults"));
            info = (UiRowInfo){0};
            assert(ui_form_side_rows_get_row_info(&app, 9 + GUI_FOLDER_GROUPS + 2, 100, &info) && !info.attr &&
                   !strcmp(info.label, "Reset to Defaults"));
            assert(!ui_form_side_rows_get_row_info(&app, 9 + GUI_FOLDER_GROUPS + 3, 100, &info));
            assert(ui_form_side_rows_get_row_info(&app, 0, 100, &info) && info.attr == UI_ATTR_TITLE &&
                   !strcmp(info.label, "App Folders"));
        }
        render(&app);
        { /* each group's title: in the page's colour on a band of ink in a theme of filled boxes, else in ink */
            SDL_Color title = rgb(app.theme.panels == GUI_PANELS_FILL ? app.theme.page : app.theme.ink);
            shows(&app, "App Folders", title);
            shows(&app, "Recompiled Library", title);
            shows(&app, "Dumps Library", title);
        }
        { /* each group in a box as far as it shows, half a cell wider than its rows, in the dim colour; Reset to
             Defaults, the last row, in one of its own */
            GuiRect r = ui_form_side_row(f.right, app.scale, UI_FORM_SIDE_ROWS, 1); /* Data, under the app's title */
            SDL_Color dim = rgb(app.theme.dim);
            assert(!app.theme.grid && !same_colour(dim, rgb(app.theme.page)));
            assert(same_colour(pixel(&app, (float)(r.x - 4 * app.scale), (float)(r.y + r.h / 2)), dim));
            r = ui_form_side_row(f.right, app.scale, UI_FORM_SIDE_ROWS, 9 + GUI_FOLDER_GROUPS + 2);
            assert(same_colour(pixel(&app, (float)(r.x - 4 * app.scale), (float)(r.y + r.h / 2)), dim));
        }
        { /* a title on a band of ink in a theme of filled boxes, its text in the page's colour; over a rule in dim in
             one of outlined boxes, in ink */
            GuiRect t = ui_form_side_row(f.right, app.scale, UI_FORM_SIDE_ROWS, 0);
            GuiPanels kept = app.theme.panels;
            for (int filled = 0; filled < 2; ++filled) {
                app.theme.panels = filled ? GUI_PANELS_FILL : GUI_PANELS_OUTLINE;
                render(&app);
                shows(&app, "Dumps Library", rgb(filled ? app.theme.page : app.theme.ink));
                assert(same_colour(pixel(&app, (float)(t.x + 1), (float)(t.y + t.h / 2)),
                                   rgb(filled ? app.theme.ink : app.theme.page)));
                assert(same_colour(pixel(&app, (float)(t.x + 1), (float)(t.y + t.h - 1)),
                                   rgb(filled ? app.theme.ink : app.theme.dim)));
            }
            app.theme.panels = kept;
            render(&app);
        }
        { /* the names' column as wide as the longest of the rows with a folder (Extractions), not Add Folder's or the
             last's; the folder after it */
            char start[64];
            snprintf(start, sizeof start, "-    %-11s %c", "Data", app.data_dir[0]);
            shows(&app, start, yellow);
        }
        { /* what a row is at its end; with no room for it beside the folder whole, it goes and the folder stays */
            char line[1100];
            int labels = folder_labels(&app), r = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_DATA);
            int whole = gui_text_cols(app.theme.marker) + labels + 1 + gui_text_cols(app.data_dir);
            size_t n;
            assert(!strcmp(app.app_form.folder[r], "default"));
            for (int extra = 9; extra <= 40; extra += 31) {
                form_line(&app, &app.editor, r, labels, whole + extra, line, sizeof line);
                n = strlen(line);
                assert(gui_text_cols(line) == whole + extra && n > 7 && !strcmp(line + n - 7, "default") &&
                       strstr(line, app.data_dir));
            }
            form_line(&app, &app.editor, r, labels, whole + 8, line, sizeof line);
            assert(strstr(line, app.data_dir) && !strstr(line, "default"));
        }
        { /* a click on a row takes it; one on a group's title, nothing */
            GuiRect b;
            b = form_row_box(&app, &f, true,
                             folder_entry(&app, FOLDER_ROW(GUI_ROW_DUMPS, -1)) - 1); /* the dumps' title */
            mouse_px(&app, b.x + b.w / 2, b.y + b.h / 2, true);
            assert(app.editor.row == 0 && !app.editor.note[0]);
            b = form_row_box(&app, &f, true, folder_entry(&app, FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED)));
            mouse_px(&app, b.x + b.w / 2, b.y + b.h / 2, true);
            assert(app.editor.row == FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED));
            app.editor.row = 0;
            /* the pointer over a row takes the focus to that row, not to the row its place in the list numbers
               (the groups' titles are in the list): over a title, nowhere */
            mouse_px(&app, b.x + b.w / 2, b.y + b.h / 2, false);
            assert(app.editor.row == FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED));
            b = form_row_box(&app, &f, true, folder_entry(&app, FOLDER_ROW(GUI_ROW_DUMPS, -1)) - 1);
            mouse_px(&app, b.x + b.w / 2, b.y + b.h / 2, false);
            assert(app.editor.row == FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED));
            app.editor.row = 0;
        }
        /* a row with no value has no padding to the widest label: short, it stays whole while a long one slides */
        {
            Uint32 kept = app.theme.scroll_ms;
            app.theme.scroll_ms = 100; /* the default theme slides nothing */
            for (app.clock = 1; gui_slide(1000, app.clock, app.theme.scroll_ms) != 3;) app.clock += 100;
            render(&app);
            shows(&app, "- " ICON_FOLDER_PLUS " Add Folder", normal);
            app.theme.scroll_ms = kept;
            app.clock = 0;
        }
        press_key(&app, "ESC", 0, false, yellow);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_PLACES);
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_SETTINGS);
        key(&app, SDLK_RETURN);
        app.editor.row = GUI_APP_PLACES;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_FOLDERS && app.editor.row == 0);
    }
    {
        char* more = snx_path_join(temporary, "more games");
        char* file = snx_path_join(more, "Zed");
        char* kept_as;
        char *kept, line[160];
        size_t size;
        SDL_Event e = {0};
        assert(!snx_mkdirs(more) && !snx_write_file(file, "fixture", 7));
        kept_as = snx_absolute(more); /* as the system writes the folder once it is there: backslashes on Windows */
        /* the recompiled library's Main, where recompiled games go: its folder in its row, its tips what it holds */
        app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES);
        render(&app);
        shows_half(&app, "The recompiled games, ", normal);
        shows(&app, "-    Main", yellow);
        shows(&app, "- " ICON_FOLDER_PLUS " Add Folder", normal);
        assert(!strcmp(app.app_form.folder[app.editor.row], "default") && !strcmp(form_note(&app), app.games_dir));
        /* at its default: Del says so, no box */
        key(&app, SDLK_DELETE);
        snprintf(line, sizeof line, "Recompiled: %s, the default", app.games_dir);
        assert(!app.confirm.shown && !strcmp(app.editor.note, line) && !app.folder_count &&
               !app.place[GUI_PLACE_GAMES]);
        /* Enter on it picks where games go: the picker's own answer first (none here), then a folder; that folder is
           the row's, and Del asks before it puts back the default, only the setting changing */
        key(&app, SDLK_RETURN);
        assert(app.place_row == GUI_PLACE_GAMES);
        {
            SDL_Event answer;
            Uint64 until = SDL_GetTicks() + 2000;
            while (app.place_row >= 0 && SDL_GetTicks() < until)
                while (SDL_PollEvent(&answer)) event(&app, &answer);
            assert(app.place_row == -1 && !app.place[GUI_PLACE_GAMES]);
        }
        picker_box_closed(&app);
        { /* the folders as the tests before set them, put back after: Folders' default is not theirs */
            char *data = app.data_dir, *games = app.games_dir, *themes = app.themes_dirs[0], *work = app.work_dir;
            char *keys_path = app.keys_path, *elsewhere = snx_path_join(temporary, "games elsewhere"), *fallback;
            char keys[512];
            app.data_dir = snx_strdup(data);
            app.games_dir = snx_strdup(games);
            app.themes_dirs[0] = snx_strdup(themes);
            app.work_dir = snx_strdup(work);
            app.keys_path = keys_path ? snx_strdup(keys_path) : NULL;
            SDL_Event answer;
            assert(!snx_mkdirs(elsewhere));
            app.place_row = GUI_PLACE_GAMES;
            SDL_zero(answer);
            answer.type = app.picked_folder;
            answer.user.data1 = SDL_strdup(elsewhere);
            event(&app, &answer);
            assert(app.place[GUI_PLACE_GAMES] && same_path(app.games_dir, elsewhere) &&
                   !strncmp(app.editor.note, "Recompiled: ", 12));
            assert(app.form == GUI_FORM_FOLDERS && app.editor.row == FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES) &&
                   same_path(folder_row_path(&app, app.editor.row), elsewhere) &&
                   !strcmp(app.app_form.folder[app.editor.row], "custom"));
            {
                char note[sizeof app.editor.note];
                snprintf(note, sizeof note, "%s", app.editor.note);
                app.editor.note[0] = 0; /* the tips say the row while no note covers them */
                render(&app);
                shows_half(&app, "The recompiled games, ", normal); /* what it holds, its keys in the foot */
                snprintf(app.editor.note, sizeof app.editor.note, "%s", note);
            }
            assert(strstr(shown_keys(&app, keys, sizeof keys), "Del: default."));
            render(&app);
            hides_foot(&app, "delete", rgb(app.theme.soft)); /* an Extra's, not this row's */
            fallback = snx_recompiled_dir(app.data_dir);
            {
                char* asked = snx_format("Recompiled back to its default, %s? %s keeps what it has.", fallback,
                                         app.place[GUI_PLACE_GAMES]);
                key(&app, SDLK_DELETE);
                assert(app.confirm.shown && app.confirm.ask == GUI_ASK_PLACE && !strcmp(app.ask_line, asked));
                render(&app);
                shows(&app, "- Reset", normal); /* the question names the default: the choice says no word for it */
                free(asked);
            }
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && app.place[GUI_PLACE_GAMES] && same_path(app.games_dir, elsewhere));
            key(&app, SDLK_DELETE);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.place[GUI_PLACE_GAMES] && same_path(app.games_dir, fallback) &&
                   strstr(app.editor.note, ", the default"));
            assert(snx_is_dir(elsewhere) && same_path(folder_row_path(&app, app.editor.row), fallback));
            free(elsewhere);
            free(fallback);
            free(app.data_dir), free(app.games_dir), free(app.themes_dirs[0]), free(app.work_dir), free(app.keys_path);
            app.data_dir = data, app.games_dir = games, app.themes_dirs[0] = themes, app.work_dir = work;
            app.keys_path = keys_path;
            open_folders(&app, FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_DUMPS));
        }
        /* dropped on a row of the dumps library: a file is no folder; a folder is added to it, kept with its library */
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = file;
        event(&app, &e);
        assert(app.folder_count == 0 && !strncmp(app.editor.note, "Not a folder: ", 14));
        e.drop.data = more;
        event(&app, &e);
        if (app.folder_count != 1 || !same_path(app.folders[0], kept_as) ||
            strcmp(app.editor.note, "Added to the dumps library"))
            fprintf(stderr, "folders: %d, first \"%s\", expected \"%s\", note \"%s\"\n", app.folder_count,
                    app.folder_count ? app.folders[0] : "", kept_as, app.editor.note);
        assert(app.folder_count == 1 && same_path(app.folders[0], kept_as) &&
               app.folders_library[0] == GUI_LIBRARY_DUMPS && !strcmp(app.editor.note, "Added to the dumps library"));
        assert(app.form == GUI_FORM_FOLDERS && app.editor.row == FOLDER_ROW(GUI_ROW_FOLDER, 0) &&
               app.editor.row == FOLDER_ROW(GUI_ROW_DUMPS, -1) + 1 && gui_editor_rows(&app.editor) == 11);
        kept = (char*)snx_read_file(app.folders_file, &size);
        snprintf(line, sizeof line, "dumps %s\n", kept_as);
        assert(kept && !strcmp(kept, line));
        free(kept);
        event(&app, &e);
        assert(app.folder_count == 1 && !strncmp(app.editor.note, "Already in the dumps library: ", 30));
        render(&app);
        shows(&app, "-    Extra", yellow);
        row_start(kept_as, line, sizeof line);
        shows(&app, line + 2, yellow); /* its folder, from its root */
        /* F5 looks at the folders again: one gone from the disk says so on its row, and the note counts them */
        {
            char *moved = snx_format("%s moved", more), *dumps = snx_path_join(app.data_dir, "dumps");
            char* said = snx_format("; dumps/ and 1 Extra(s), %d not found", !snx_is_dir(dumps) + 1);
            int row = app.editor.row;
            assert(!rename(more, moved));
            key(&app, SDLK_F5);
            assert(app.editor.row == row && !strcmp(app.app_form.folder[row], "not found") &&
                   !strncmp(app.editor.note, "Refreshed: ", 11) && strstr(app.editor.note, said));
            assert(!rename(moved, more));
            press_key(&app, "F5", 0, false, yellow);
            assert(strcmp(app.app_form.folder[row], "not found") && app.folder_count == 1);
            free(said);
            free(dumps);
            free(moved);
        }
        /* a folder too long for its row: cut in its middle, its end kept, never sliding; whole under the rows. Its
           middle folder is longer than the row, so that whatever root it has (C:\ is shorter than /tmp/), the end
           kept is games alone */
        {
            char* deep = snx_format(
                "%s/a folder name long enough to be cut in its row whatever the root before it/games", temporary);
            assert(!snx_mkdirs(deep));
            folder_add(&app, deep, GUI_LIBRARY_DUMPS, false);
            assert(app.folder_count == 2 && app.editor.row == FOLDER_ROW(GUI_ROW_FOLDER, 1) &&
                   !strcmp(form_note(&app), app.folders[1]));
            render(&app);
            { /* its end as the system wrote the path: a backslash before games on Windows */
                char tail[16];
                snprintf(tail, sizeof tail, "...%s", app.folders[1] + strlen(app.folders[1]) - 6);
                shows(&app, tail, yellow);
            }
            hides(&app, "long enough to be cut", yellow);
            folder_remove(&app, 1);
            assert(app.folder_count == 1);
            free(deep);
        }
        app.editor.row = FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_DUMPS); /* the dumps library's Add Folder */
        key(&app, SDLK_RETURN);
        {
            SDL_Event answer;
            Uint64 until = SDL_GetTicks() + 2000;
            while (!app.editor.note[0] && SDL_GetTicks() < until) /* its answer comes on the picker's thread */
                while (SDL_PollEvent(&answer)) event(&app, &answer);
        }
        picker_box_closed(&app);
        assert(app.folder_count == 1 && (!strcmp(app.editor.note, "No folder was picked.") ||
                                         !strncmp(app.editor.note, "The folder picker could not open: ", 34)));
        /* Enter or Del on an Extra asks before it is taken off, as the tips say; Back keeps it */
        app.editor.row = FOLDER_ROW(GUI_ROW_FOLDER, 0);
        app.editor.note[0] = 0; /* the picker's answer above, over the tips */
        render(&app);
        shows_half(&app, "Removes it from the library; its games stay in the folder", normal);
        {
            char *asked = snx_format("Remove %s from the Dumps Library? Its games stay in the folder.", app.folders[0]),
                 *done = snx_format("%s removed from the library. Its games stay in the folder", app.folders[0]);
            key(&app, SDLK_RETURN);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_FOLDER && !strcmp(app.ask_line, asked));
            render(&app);
            shows(&app, "- Remove", normal);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && app.folder_count == 1);
            key(&app, SDLK_DELETE);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_FOLDER && !strcmp(app.ask_line, asked));
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && !app.folder_count && !strcmp(app.editor.note, done));
            free(asked);
            free(done);
        }
        assert(snx_is_dir(more) && (kept = (char*)snx_read_file(app.folders_file, &size)) && !size);
        free(kept);
        /* a folder with folders in it asks whether their games are listed too: Cancel adds nothing, Only This Folder
           its own games, With Subfolders theirs as well, down GUI_FOLDER_DEPTH levels, a hidden one left alone; kept
           with GUI_FOLDER_DEEP after its path, and its row says so */
        {
#ifdef _WIN32
#define GAME(name) name ".exe" /* a game there is an .exe, listed by its file's name */
#else
#define GAME(name) name
#endif
            static const char* const files[] = {GAME("Top"), "a/" GAME("One"), "a/b/c/d/" GAME("Four"),
                                                "a/b/c/d/e/" GAME("Five"), ".hid/" GAME("Hidden")};
            char *deep = snx_path_join(temporary, "deep games"), *deep_as;
            GuiGames listed = {0};
            char said[sizeof app.message];
            size_t f, g;
            int found = 0;
            for (f = 0; f < sizeof files / sizeof files[0]; ++f) {
                char *path = snx_path_join(deep, files[f]), *dir = snx_dirname(path);
                assert(!snx_mkdirs(dir) && !snx_write_file(path, "fixture", 7) && !snx_make_executable(path));
                free(dir);
                free(path);
            }
            deep_as = snx_absolute(deep);
            app.editor.row = FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED); /* the recompiled library's */
            e.drop.data = deep;
            event(&app, &e);
            assert(app.folder_asked && same_path(app.folder_asked, deep_as) && app.confirm.shown && !app.folder_count);
            render(&app);
            shows(&app, "List the games in its subfolders too?", rgb(app.theme.warning));
            key(&app, SDLK_ESCAPE);
            assert(!app.folder_asked && !app.confirm.shown && !app.folder_count &&
                   !strcmp(app.editor.note, "Not added"));
            event(&app, &e);
            key(&app, SDLK_RETURN); /* Only This Folder: games ready to run, so the recompiled library */
            assert(app.folder_count == 1 && !app.folders_deep[0] && app.folders_library[0] == GUI_LIBRARY_RECOMPILED &&
                   !strcmp(app.editor.note, "Added to the recompiled library"));
            games_scan(&app, &listed, said, sizeof said);
            for (g = 0; g < listed.count; ++g)
                found |= !strcmp(listed.names[g], GAME("Top")) | !strcmp(listed.names[g], GAME("One")) << 1;
            assert(found == 1);
            gui_games_free(&listed);
            folder_remove(&app, 0);
            app.editor.row = FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED);
            event(&app, &e);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN); /* With Subfolders */
            assert(app.folder_count == 1 && app.folders_deep[0] &&
                   !strcmp(app.editor.note, "Added to the recompiled library, with its subfolders"));
            kept = (char*)snx_read_file(app.folders_file, &size);
            snprintf(line, sizeof line, "recompiled %s" GUI_FOLDER_DEEP "\n", deep_as);
            assert(kept && !strcmp(kept, line));
            free(kept);
            assert(!strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_FOLDER, 0)], "+ subfolders"));
            games_scan(&app, &listed, said, sizeof said);
            found = 0;
            for (g = 0; g < listed.count; ++g) {
                found |= !strcmp(listed.names[g], GAME("Top")) | !strcmp(listed.names[g], GAME("One")) << 1;
                found |= !strcmp(listed.names[g], GAME("Four")) << 2 | !strcmp(listed.names[g], GAME("Five")) << 3;
                found |= !strcmp(listed.names[g], GAME("Hidden")) << 4;
            }
            assert(found == 7); /* Top, One and Four; not Five, past four levels, nor Hidden */
            gui_games_free(&listed);
            /* read back as it was kept; a change below it changes the stamp */
            {
                uint64_t stamp = stamp_now(&app);
                char* more_deep = snx_path_join(deep, "a/b/Two");
                assert(!snx_write_file(more_deep, "fixture", 7));
                assert(stamp_now(&app) != stamp);
                free(more_deep);
            }
            /* the watcher's own thread: a hash soon, another once a game comes, and an end when told */
            {
                uint64_t first = 0, now = 0;
                Uint64 until;
                char* more = snx_path_join(deep, "a/Three");
                watch_start(&app);
                for (until = SDL_GetTicks() + 20000; !watch_get(&app, &first) && SDL_GetTicks() < until;) SDL_Delay(20);
                assert(watch_get(&app, &first) && app.watch.thread);
                assert(!snx_write_file(more, "fixture", 7));
                for (until = SDL_GetTicks() + 20000;
                     (!watch_get(&app, &now) || now == first) && SDL_GetTicks() < until;)
                    SDL_Delay(20);
                assert(now != first);
                watch_stop(&app);
                assert(!app.watch.thread && !watch_get(&app, &now));
                free(more);
            }
            free(app.folders[0]);
            app.folder_count = 0;
            folders_load(&app);
            assert(app.folder_count == 1 && app.folders_deep[0] && same_path(app.folders[0], deep_as) &&
                   app.folders_library[0] == GUI_LIBRARY_RECOMPILED);
            folder_remove(&app, 0);
            free(deep_as);
            free(deep);
            /* folders.txt as an older version wrote it, a path a line: each folder in the library of what it holds,
               both for both, none or gone the dumps'; written again with them, said once naming each */
            {
                char *ready = snx_path_join(temporary, "old ready"), *dumped = snx_path_join(temporary, "old dumps"),
                     *both = snx_path_join(temporary, "old both"), *empty = snx_path_join(temporary, "old empty");
                char *game = snx_path_join(ready, GAME("Ready")), *nsp = snx_path_join(dumped, "a.nsp"),
                     *both_game = snx_path_join(both, GAME("Ready")), *both_nsp = snx_path_join(both, "b.xci");
                char *old_text, *want, *read;
                assert(!snx_mkdirs(ready) && !snx_mkdirs(dumped) && !snx_mkdirs(both) && !snx_mkdirs(empty));
                assert(!snx_write_file(game, "fixture", 7) && !snx_make_executable(game));
                assert(!snx_write_file(both_game, "fixture", 7) && !snx_make_executable(both_game));
                assert(!snx_write_file(nsp, "fixture", 7) && !snx_write_file(both_nsp, "fixture", 7));
                old_text = snx_format("%s\n%s" GUI_FOLDER_DEEP "\n%s\n%s\n", ready, dumped, both, empty);
                assert(!snx_write_file(app.folders_file, old_text, strlen(old_text)));
                assert(!app.folder_count);
                app.message[0] = 0;
                folders_load(&app);
                assert(app.folder_count == 5);
                assert(!strcmp(app.folders[0], ready) && app.folders_library[0] == GUI_LIBRARY_RECOMPILED);
                assert(!strcmp(app.folders[1], dumped) && app.folders_library[1] == GUI_LIBRARY_DUMPS &&
                       app.folders_deep[1]);
                assert(!strcmp(app.folders[2], both) && app.folders_library[2] == GUI_LIBRARY_RECOMPILED);
                assert(!strcmp(app.folders[3], both) && app.folders_library[3] == GUI_LIBRARY_DUMPS);
                assert(!strcmp(app.folders[4], empty) && app.folders_library[4] == GUI_LIBRARY_DUMPS);
                want =
                    snx_format("%s: each folder of an older version's lines is in a library now: %s: recompiled, %s: "
                               "dumps, %s: recompiled and dumps, %s: dumps",
                               app.folders_file, ready, dumped, both, empty);
                assert(!strcmp(app.message, want));
                free(want);
                read = (char*)snx_read_file(app.folders_file, &size);
                want = snx_format("recompiled %s\ndumps %s" GUI_FOLDER_DEEP "\nrecompiled %s\ndumps %s\ndumps %s\n",
                                  ready, dumped, both, both, empty);
                assert(read && !strcmp(read, want));
                free(want);
                free(read);
                while (app.folder_count) free(app.folders[--app.folder_count]);
                app.message[0] = 0;
                folders_load(&app); /* once: its lines name their libraries now */
                assert(app.folder_count == 5 && !app.message[0]);
                while (app.folder_count) free(app.folders[--app.folder_count]);
                assert(!snx_write_file(app.folders_file, "", 0));
                free(old_text);
                free(game), free(nsp), free(both_game), free(both_nsp);
                free(ready), free(dumped), free(both), free(empty);
            }
            /* the data folder's games/, where an older version made its games: still listed, and said as left behind
               with where they go now, until the player moves them */
            {
                char *data_was = app.data_dir, *old_games, *old_game;
                app.data_dir = snx_path_join(temporary, "older data");
                old_games = snx_path_join(app.data_dir, "games");
                old_game = snx_format("%s/Old/%s", old_games, GAME("Old"));
                {
                    char* dir = snx_dirname(old_game);
                    assert(!snx_mkdirs(dir) && !snx_write_file(old_game, "fixture", 7) &&
                           !snx_make_executable(old_game));
                    free(dir);
                }
                games_scan(&app, &listed, said, sizeof said);
                found = 0;
                for (g = 0; g < listed.count; ++g)
                    if (!strcmp(listed.names[g], "Old")) { /* its row marked, as no other game's */
                        char row[256];
                        found = 1;
                        game_label(&app, &listed, g, row, sizeof row);
                        assert(strstr(row, "Old  (games/)") && game_was_old(&app, listed.paths[g]));
                        game_kind(&app, &listed, g, row, sizeof row); /* and the tips say why */
                        assert(!strcmp(row, "Ready to run, in the games/ an older version made: new games go to "
                                            "recompiled/"));
                    } else if (!gui_games_heading(&listed, g)) {
                        char row[256];
                        game_label(&app, &listed, g, row, sizeof row);
                        assert(!strstr(row, "(games/)"));
                    }
                assert(found && !strstr(said, old_games)); /* the test's own games/ is gone: said, not this */
                gui_games_free(&listed);
                left_behind(&app);
                snprintf(line, sizeof line, "%s" SAY_LEFT_BY "%s\n", old_games, app.games_dir);
                assert(app.left && strstr(app.left, line) && snx_is_file(old_game)); /* nothing moved */
                assert(!snx_remove_tree(app.data_dir));
                free(app.data_dir);
                app.data_dir = data_was;
                left_behind(&app);
                free(old_game);
                free(old_games);
            }
#undef GAME
        }
        app.editor.row = FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_DUMPS);
        e.drop.data = more;
        event(&app, &e); /* back for the games below */
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_PLACES);
        assert(!strcmp(app.app_form.places, "Defaults, 1 Extra(s)"));
        free(file);
        free(kept_as);
        free(more);
    }
    /* Folders' places: the data folder, recompiled/ (the recompiled library's Main) and the extractions, each its
       default or a folder picked, kept in app.ini's [folders]. A folder that cannot take what goes there is refused,
       never swapped for the default */
    {
        char *data = snx_strdup(app.data_dir), *games = snx_strdup(app.games_dir), *work = snx_strdup(app.work_dir);
        char* games_default = snx_recompiled_dir(data); /* the test's own games/ is elsewhere, set by hand */
        char *keys = snx_strdup(app.keys_path), *themes = snx_strdup(app.themes_dirs[0]); /* so are these */
        char *picked = snx_path_join(temporary, "picked games"), *file = snx_path_join(temporary, "not a folder.txt");
        char *kept, line[512];
        size_t size;
        assert(app.editor.row == GUI_APP_PLACES && !strcmp(app.app_form.places, "Defaults, 1 Extra(s)"));
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_FOLDERS && app.editor.row == 0 && gui_editor_rows(&app.editor) == 11);
        render(&app);
        shows(&app, "FOLDERS", normal);
        shows(&app, "-    Data", yellow);
        shows(&app, "default", yellow);
        /* under the rows, the folder in use: its start, as a long one wraps (Windows' %APPDATA% does) */
        snprintf(line, sizeof line, "%.24s", data);
        shows_half(&app, line, rgb(app.theme.soft));
        shows(&app, "-    Extractions", normal);
        shows(&app, "- " ICON_ROTATE_CCW " Reset to Defaults", normal);
        assert(!strcmp(app.place_line[GUI_PLACE_GAMES], app.games_dir) &&
               !strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES)], "default"));
        assert(strstr(app.place_about[GUI_PLACE_GAMES], "The recompiled games, ") &&
               strlen(app.place_about[GUI_PLACE_GAMES]) > 5 &&
               !strcmp(app.place_about[GUI_PLACE_GAMES] + strlen(app.place_about[GUI_PLACE_GAMES]) - 5, " free"));
        /* refused: a folder not there, a file; nothing changes, nothing is kept */
        app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES);
        place_set(&app, GUI_PLACE_GAMES, picked);
        snprintf(line, sizeof line, "Not changed: %s is not there", picked);
        assert(!app.place[GUI_PLACE_GAMES] && !strcmp(app.editor.note, line) && !strcmp(app.games_dir, games));
        assert(!snx_write_file(file, "x", 1));
        place_set(&app, GUI_PLACE_GAMES, file);
        assert(!app.place[GUI_PLACE_GAMES] && strstr(app.editor.note, " is not a folder"));
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(!kept || !strstr(kept, "[folders]"));
        free(kept);
        /* Enter opens the folder picker for the focused row; its answer is that row's folder at once, kept, and the
           folder before keeps what it has */
        assert(!snx_mkdirs(picked));
        key(&app, SDLK_RETURN);
        assert(app.place_row == GUI_PLACE_GAMES);
        {
            SDL_Event answer;
            SDL_zero(answer);
            answer.type = app.picked_folder;
            answer.user.data1 = SDL_strdup(picked);
            event(&app, &answer);
        }
        assert(app.place_row == -1 && app.place[GUI_PLACE_GAMES] && !strcmp(app.games_dir, app.place[GUI_PLACE_GAMES]));
        assert(snx_path_within(app.games_dir, picked) && !strcmp(app.data_dir, data) && !strcmp(app.work_dir, work));
        snprintf(line, sizeof line, "Recompiled: %s. The folder before keeps what it has", app.games_dir);
        assert(!strcmp(app.editor.note, line) && app.editor.row == FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES));
        kept = (char*)snx_read_file(app.app_file, &size);
        snprintf(line, sizeof line, "[folders]\ndata = \nrecompiled = %s\nwork = \n", app.games_dir);
        assert(kept && strstr(kept, "[app]\ntheme = ") && strstr(kept, line));
        free(kept);
        /* F5 looks at the five again: all there, said so; a default not there yet is made when a run needs it */
        { /* the test's data folder is not there: a temporary one in its place, and the extractions', logs' and
             caches' */
            char *data_was = app.data_dir, *work_was = app.work_dir, *state_was = app.state_dir,
                 *cache_was = app.cache_dir;
            app.data_dir = snx_strdup(temporary);
            app.work_dir = snx_strdup(temporary);
            app.state_dir = snx_strdup(temporary);
            app.cache_dir = snx_strdup(temporary);
            char *dumps = snx_path_join(app.data_dir, "dumps"), *libraries;
            libraries = snx_format("; dumps/ and 1 Extra(s), %d not found", !snx_is_dir(dumps));
            key(&app, SDLK_F5);
            snprintf(line, sizeof line, "Refreshed: Data, Recompiled, Extractions, Logs and Caches are there%s",
                     libraries);
            assert(app.form == GUI_FORM_FOLDERS && app.editor.row == FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES) &&
                   !strcmp(app.editor.note, line));
            free(app.work_dir);
            app.work_dir = snx_path_join(temporary, "no work yet");
            assert(!app.place[GUI_PLACE_WORK]);
            key(&app, SDLK_F5);
            snprintf(line, sizeof line, "Refreshed: Extractions %s, made when needed%s", app.work_dir, libraries);
            assert(!strcmp(app.editor.note, line));
            { /* a picked one gone is not made again: it is not there */
                char* moved = snx_format("%s moved", app.games_dir);
                assert(app.place[GUI_PLACE_GAMES] && !rename(app.games_dir, moved));
                key(&app, SDLK_F5);
                snprintf(line, sizeof line, "Refreshed: Recompiled %s is not there, Extractions %s, made when needed%s",
                         app.games_dir, app.work_dir, libraries);
                assert(!strcmp(app.editor.note, line));
                assert(!rename(moved, app.games_dir));
                free(moved);
            }
            { /* Logs and Caches too, each with its folder */
                free(app.cache_dir);
                app.cache_dir = snx_path_join(temporary, "no caches yet");
                key(&app, SDLK_F5);
                snprintf(line, sizeof line,
                         "Refreshed: Extractions %s, made when needed, Caches %s, made when needed%s", app.work_dir,
                         app.cache_dir, libraries);
                assert(!strcmp(app.editor.note, line));
            }
            free(libraries);
            free(dumps);
            free(app.work_dir);
            free(app.data_dir);
            free(app.state_dir);
            free(app.cache_dir);
            app.work_dir = work_was;
            app.data_dir = data_was;
            app.state_dir = state_was;
            app.cache_dir = cache_was;
            key(&app, SDLK_F5); /* the rows as the folders are again */
        }
        /* Logs and Caches, each a row: its default the menu's own folder; one picked kept in app.ini first, where the
           drivers and the games find it (snx_menu_dir), the next run's log in Logs; app.ini that cannot be written
           leaves them as they were. Settings above them, read-only: app.ini in it names them */
        {
            const char* home = app.home_dir ? app.home_dir : app.data_dir;
            char *logs = snx_path_join(temporary, "picked logs"), *caches = snx_path_join(temporary, "picked caches");
            char *fallback = snx_menu_kind_default(home, SNX_USER_STATE), *in_use, *ini_was = app.app_file;
            assert(!snx_mkdirs(logs) && !snx_mkdirs(caches) && fallback && !strcmp(app.state_dir, fallback));
            assert(!strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_STATE)], "default") &&
                   !strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_CACHE)], "default"));
            app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_STATE);
            { /* a key the focused row cannot do now is not said: Del on a default; F1's box the same */
                char said[512];
                assert(!strstr(screen_said(&app, true, said, sizeof said), "Del:") && strstr(said, "Enter: change.") &&
                       strstr(said, "F5: refresh."));
            }
            place_set(&app, GUI_PLACE_STATE, logs);
            {
                char said[512];
                assert(strstr(screen_said(&app, true, said, sizeof said), "Del: default."));
                app.editor.row = folder_rows(&app); /* Reset to Defaults, with a folder picked */
                assert(strstr(screen_said(&app, true, said, sizeof said), "Enter: reset."));
                app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_STATE);
            }
            assert(app.place[GUI_PLACE_STATE] && same_path(app.state_dir, logs));
            snprintf(line, sizeof line, "Logs: %s. The folder before keeps what it has", app.state_dir);
            assert(!strcmp(app.editor.note, line) && snx_path_within(app.run_log, logs));
            render(&app);
            shows(&app, "-    Logs", yellow);
            /* custom, at the row's end where the folder leaves room (a long one, as Windows' temporary folder, none) */
            assert(!strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_STATE)], "custom"));
            kept = (char*)snx_read_file(app.app_file, &size);
            snprintf(line, sizeof line, "\nstate = %s\ncache = \n", app.state_dir);
            assert(kept && strstr(kept, line));
            free(kept);
            /* the test's app.ini is <temporary>/config/app.ini: a data folder's config/, as a portable copy's is */
            in_use = snx_app_kind_dir(temporary, SNX_USER_STATE);
            assert(in_use && same_path(in_use, logs));
            free(in_use);
            /* app.ini not written: Caches as it was, said with the file */
            app.app_file = snx_path_join(file, "app.ini"); /* under a file: no folder can be made there */
            place_set(&app, GUI_PLACE_CACHE, caches);
            snprintf(line, sizeof line,
                     "Logs and Caches not changed: %s, which the runs read them from, could not be written",
                     app.app_file);
            assert(!app.place[GUI_PLACE_CACHE] && !strcmp(app.editor.note, line));
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ERROR && !strcmp(app.error_line, line)); /* whole */
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && !snx_is_file(app.app_file));
            free(app.app_file);
            app.app_file = ini_was;
            place_set(&app, GUI_PLACE_CACHE, caches);
            assert(app.place[GUI_PLACE_CACHE] && same_path(app.cache_dir, caches));
            in_use = snx_app_kind_dir(temporary, SNX_USER_CACHE);
            assert(in_use && same_path(in_use, caches));
            free(in_use);
            render(&app);
            shows(&app, "-    Caches", yellow);
            /* back to the defaults, app.ini's keys empty */
            place_use(&app, GUI_PLACE_STATE, NULL);
            place_use(&app, GUI_PLACE_CACHE, NULL);
            snprintf(line, sizeof line, "Caches: %s, the default", app.cache_dir);
            assert(!app.place[GUI_PLACE_STATE] && !strcmp(app.state_dir, fallback) && !strcmp(app.editor.note, line));
            in_use = snx_app_kind_dir(temporary, SNX_USER_STATE);
            assert(in_use && snx_path_within(in_use, temporary)); /* its own state/ again */
            free(in_use);
            /* Settings: its folder, read-only; Enter says why */
            if (!app.config_dir) app.config_dir = snx_dirname(app.app_file); /* as open_files keeps it */
            app.editor.row = FOLDER_ROW(GUI_ROW_CONFIG, -1);
            assert(app.config_dir && !strcmp(folder_row_path(&app, app.editor.row), app.config_dir) &&
                   !strcmp(app.app_form.folder[app.editor.row], "read-only") &&
                   !strcmp(form_note(&app), app.config_dir));
            key(&app, SDLK_RETURN);
            assert(!strcmp(app.editor.note, config_row) && app.place_row == -1);
            { /* its foot: refresh and back, neither a pick nor Del */
                char said[512];
                screen_said(&app, false, said, sizeof said);
                assert(strstr(said, "F5: refresh.") && !strstr(said, "Del:") && !strstr(said, "Add Folder"));
            }
            render(&app);
            shows(&app, "-    Settings", yellow); /* a row without an icon, where Add Folder has one */
            app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES); /* where the checks below look */
            free(fallback);
            free(logs);
            free(caches);
        }
        render(&app);
        shows(&app, "-    Main", yellow);
        assert(!strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES)], "custom"));
        assert(!strcmp(app.place_line[GUI_PLACE_GAMES], app.games_dir));
        app_values(&app); /* App's note, the folder now */
        assert(!strcmp(app.app_form.places, "1 custom, 1 Extra(s)") && strstr(app.places_note, app.games_dir));
        { /* a folder dropped on a place's row takes its place there, as one picked; Del puts back the default */
            char* dropped = snx_path_join(temporary, "dropped work");
            SDL_Event drop_event = {0};
            assert(!snx_mkdirs(dropped));
            app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_WORK);
            drop_event.type = SDL_EVENT_DROP_FILE;
            drop_event.drop.data = dropped;
            event(&app, &drop_event);
            assert(app.place[GUI_PLACE_WORK] && same_path(app.work_dir, dropped) && app.folder_count == 1);
            key(&app, SDLK_DELETE);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.place[GUI_PLACE_WORK] && !strcmp(app.work_dir, work));
            app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES);
            free(dropped);
        }
        /* read back as the menu opens, a path not from the root left out and named */
        free(app.place[GUI_PLACE_GAMES]);
        app.place[GUI_PLACE_GAMES] = NULL;
        places_apply(&app);
        assert(!strcmp(app.games_dir, games_default));
        kept = (char*)snx_read_file(app.app_file, &size);
        {
            char* more_text = snx_format("%s[folders]\nwork = relative/work\nshaders = /x\n", kept);
            assert(!snx_write_file(app.app_file, more_text, strlen(more_text)));
            free(more_text);
        }
        free(kept);
        app.message[0] = 0;
        app_load(&app);
        /* what app.ini has that it does not take, named; and first, where /nonexistent cannot be made (a root CI container
           can), that the data folder's dumps/ could not be */
        {
            char* dumps = snx_path_join(app.data_dir, "dumps");
            char* unmade = snx_is_dir(dumps) ? snx_strdup("") : snx_format("Could not make %s ", dumps);
            snprintf(line, sizeof line, "%sNot used from %s: [folders] work = relative/work, [folders] shaders = /x",
                     unmade, app.app_file);
            free(unmade);
            free(dumps);
        }
        assert(!strcmp(app.message, line) && app.place[GUI_PLACE_GAMES] && !strcmp(app.games_dir, app.place[1]));
        assert(!app.place[GUI_PLACE_WORK] && !strcmp(app.work_dir, work));
        app.message[0] = 0;
        /* app.ini as an older version wrote it, [folders] games =: read as recompiled =, written again so, said once */
        {
            char *was = snx_strdup(app.games_dir), *old_text = snx_format("[folders]\ngames = %s\n", was);
            assert(!snx_write_file(app.app_file, old_text, strlen(old_text)));
            free(app.place[GUI_PLACE_GAMES]);
            app.place[GUI_PLACE_GAMES] = NULL;
            app_load(&app);
            assert(app.place[GUI_PLACE_GAMES] && !strcmp(app.place[GUI_PLACE_GAMES], was));
            snprintf(line, sizeof line, "%s: [folders] games = %s is now recompiled = %s", app.app_file, was, was);
            assert(strstr(app.message, line));
            kept = (char*)snx_read_file(app.app_file, &size);
            snprintf(line, sizeof line, "\nrecompiled = %s\n", was);
            assert(kept && strstr(kept, line) && !strstr(kept, "\ngames = "));
            free(kept);
            app.message[0] = 0;
            app_load(&app); /* once: the file says recompiled = now */
            assert(!strstr(app.message, "is now recompiled") && !strcmp(app.place[GUI_PLACE_GAMES], was));
            app.message[0] = 0;
            free(old_text);
            free(was);
        }
        /* a folder picked once and gone since: its row says so, as a run would refuse it */
        assert(!snx_remove_tree(picked));
        open_folders(&app, FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES));
        snprintf(line, sizeof line, "%s is not there", app.games_dir);
        assert(!strcmp(app.app_form.folder[app.editor.row], "is not there") &&
               !strcmp(app.place_line[GUI_PLACE_GAMES], line));
        assert(!places_fit(&app, line, sizeof line) && strstr(line, "Recompiled folder ") &&
               strstr(line, " is not there. "));
        /* Del: the default again once asked, kept */
        key(&app, SDLK_DELETE);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_PLACE && app.place[GUI_PLACE_GAMES]);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.place[GUI_PLACE_GAMES] && !strcmp(app.games_dir, games_default) &&
               strstr(app.editor.note, ", the default"));
        key(&app, SDLK_DELETE); /* at its default: said, no box */
        snprintf(line, sizeof line, "Recompiled: %s, the default", games_default);
        assert(!app.confirm.shown && !strcmp(app.editor.note, line));
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && !strstr(kept, "[folders]"));
        free(kept);
        /* a disk that runs no programs takes the extractions, not the games */
#if defined(ST_NOEXEC) && !defined(SDL_PLATFORM_ANDROID)
        {
            struct statvfs st;
            if (!statvfs("/dev/shm", &st) && (st.f_flag & ST_NOEXEC)) {
                char* shm = snx_temp_dir("/dev/shm");
                assert(shm);
                place_set(&app, GUI_PLACE_GAMES, shm);
                assert(!app.place[GUI_PLACE_GAMES] && strstr(app.editor.note, "runs no programs (noexec)"));
                place_set(&app, GUI_PLACE_WORK, shm);
                assert(app.place[GUI_PLACE_WORK] && !strcmp(app.work_dir, app.place[GUI_PLACE_WORK]));
                assert(!snx_remove_tree(shm));
                free(shm);
            }
        }
#endif
        /* Android: a folder outside the app's own storage is asked about first, its Use it pressed twice; Back leaves
           the one in use. One inside is taken at once */
        {
            char *own = snx_path_join(temporary, "own"), *inside = snx_path_join(own, "games");
            int w, h;
            GuiRect r;
            GuiChoices l;
            assert(!snx_mkdirs(inside));
            app.own_dir = own;
            app.editor.row = FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_WORK);
            place_set(&app, GUI_PLACE_WORK, temporary);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_OUTSIDE && !app.place[GUI_PLACE_WORK]);
            /* the question wraps where it is wider than the screen, its choices under it */
            SDL_GetWindowSize(app.window, &w, &h);
            assert(SDL_SetWindowSize(app.window, 420, h));
            render(&app);
            shows(&app, "Outside the app,", rgb(app.theme.warning));
            shows(&app, "games may run slower.", rgb(app.theme.warning));
            ask_boxes(&app, &r, &l);
            assert(choice_box(&app, &l, 0).y >= r.y + 2 * cell_h(&app));
            assert(SDL_SetWindowSize(app.window, w, h));
            render(&app);
            shows(&app, "- Cancel", yellow);
            shows(&app, "- Use Anyway", normal);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && !app.place[GUI_PLACE_WORK] && !strcmp(app.editor.note, "Not changed"));
            place_set(&app, GUI_PLACE_WORK, temporary);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(app.confirm.shown && app.confirm.armed && !app.place[GUI_PLACE_WORK]);
            render(&app);
            shows(&app, "- Use Anyway: press again", yellow);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && app.place[GUI_PLACE_WORK] && same_path(app.work_dir, temporary));
            assert(!strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_WORK)], "outside the app"));
            /* a finger: the first tap on Use it arms it, the second takes it */
            key(&app, SDLK_DELETE);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.place[GUI_PLACE_WORK]);
            place_set(&app, GUI_PLACE_WORK, temporary);
            ask_boxes(&app, &r, &l);
            touch_box(&app, choice_box(&app, &l, 1));
            assert(app.confirm.shown && app.confirm.armed && !app.place[GUI_PLACE_WORK]);
            touch_box(&app, choice_box(&app, &l, 1));
            assert(!app.confirm.shown && app.place[GUI_PLACE_WORK]);
            key(&app, SDLK_DELETE);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && !app.place[GUI_PLACE_WORK]);
            place_set(&app, GUI_PLACE_GAMES, inside);
            assert(!app.confirm.shown && app.place[GUI_PLACE_GAMES] &&
                   !strcmp(app.app_form.folder[FOLDER_ROW(GUI_ROW_PLACE, GUI_PLACE_GAMES)], "custom"));
            app.own_dir = NULL;
            free(inside);
            free(own);
        }
        /* the last row: every one back to its default */
        place_set(&app, GUI_PLACE_DATA, temporary);
        assert(app.place[GUI_PLACE_DATA] && snx_path_within(app.data_dir, temporary) &&
               snx_path_within(app.games_dir, app.data_dir));
        app.editor.row = gui_editor_rows(&app.editor) - 1; /* asked first, the folders picked named: data, and
                                                              recompiled/ inside it above */
        snprintf(line, sizeof line, "Reset to Defaults? Data: %s, Recompiled: %s. Nothing is moved.",
                 app.place[GUI_PLACE_DATA], app.place[GUI_PLACE_GAMES]);
        key(&app, SDLK_RETURN);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_PLACES && !strcmp(app.ask_line, line));
        render(&app);
        shows(&app, "- Reset", normal);
        key(&app, SDLK_ESCAPE);
        assert(!app.confirm.shown && app.place[GUI_PLACE_DATA]);
        key(&app, SDLK_RETURN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.place[0] && !app.place[1] && !app.place[2] && !app.place[3] && !app.place[4] &&
               !strcmp(app.data_dir, data));
        assert(!strcmp(app.editor.note, "Reset to Defaults: nothing was moved"));
        key(&app, SDLK_RETURN); /* none picked: the defaults named, no box */
        snprintf(line, sizeof line,
                 "They are the defaults: Data: %s, Recompiled: %s, Extractions: %s, Logs: %s, Caches: %s", app.data_dir,
                 app.games_dir, app.work_dir, app.state_dir, app.cache_dir);
        assert(!app.confirm.shown && !strcmp(app.editor.note, line));
        {
            char said[512];
            assert(!strstr(screen_said(&app, true, said, sizeof said), "reset")); /* nothing to reset: no key for it */
        }
        place_set(&app, GUI_PLACE_DATA, temporary); /* the data folder alone asked about too */
        snprintf(line, sizeof line, "Reset to Defaults? Data: %s. Nothing is moved.", app.place[GUI_PLACE_DATA]);
        app.editor.row = gui_editor_rows(&app.editor) - 1;
        key(&app, SDLK_RETURN);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_PLACES && !strcmp(app.ask_line, line));
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.place[GUI_PLACE_DATA] && !strcmp(app.data_dir, data));
        free(app.games_dir); /* the test's own games/ and the rest, as before */
        app.games_dir = games;
        free(app.keys_path);
        app.keys_path = keys;
        free(app.themes_dirs[0]);
        app.themes_dirs[0] = themes;
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_PLACES);
        free(file);
        free(picked);
        free(work);
        free(data);
        free(games_default);
    }
    key(&app, SDLK_ESCAPE);
    assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 0);
    key(&app, SDLK_ESCAPE);
    assert(!app.editing && app.menu.selected == GUI_SETTINGS);
    /* kept in a file, the changed ones alone, they come back as the menu opens again; the runs below get them */
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.settings_file, &size);
        assert(kept && strstr(kept, "[Display]\nfullscreen=1\n") && !strstr(kept, "controls=") &&
               !strstr(kept, "gdbstub"));
        free(kept);
        assert(app.groups > 0); /* the Settings' groups were made, each naming a section of the list */
        settings_forget(&app);  /* as a new start has none read */
        assert(setting_is(&app, "fullscreen", NULL) && settings_of(&app) && setting_is(&app, "fullscreen", "1"));
        /* the groups went with the list: the screens drawn later (the theme editor's width) read none of its names */
        assert(app.groups == 0);
        /* built into scarletnx, the menu reads the runtime's list in its own process: no program asked, none found */
        {
            char* drivers = app.drivers;
            app.drivers = snx_strdup("/nonexistent/snx-gui-test/drivers");
            snx_menu_settings = built_in_settings;
            settings_forget(&app);
            assert(settings_of(&app) && setting_row(&app, "built_in") >= 0 && setting_is(&app, "fullscreen", "1"));
            snx_menu_settings = NULL;
            settings_forget(&app);
            free(app.drivers);
            app.drivers = drivers;
            assert(settings_of(&app) && setting_row(&app, "built_in") < 0 && setting_row(&app, "user") >= 0);
            app.groups = 0;
        }
    }
    key(&app, SDLK_UP);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    /* a load opens the screen its file is picked on at once: games/ is Games' now, not an option under it */
    assert(app.menu.selected == GUI_SWITCH);
    pump(&app);
    render(&app);
    shows(&app, "- " ICON_GAMEPAD_2 " Load Switch NSP/XCI/NRO", yellow);
    const char* screenshot = getenv("SNX_GUI_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_SWITCH);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_SWITCH);
    /* the activated item stays pink while the mouse hovers another in yellow; a click selects */
    point(&app, entry_box(&app, GUI_LINUX), false);
    assert(app.menu.selected == GUI_LINUX && app.menu.active == GUI_SWITCH && app.menu.view == GUI_LOAD_NONE);
    render(&app);
    shows(&app, "- " ICON_GAMEPAD_2 " Load Switch NSP/XCI/NRO", pink);
    shows(&app, "- " ICON_SQUARE_TERMINAL " Load Linux ARM ELF", yellow);
    point(&app, entry_box(&app, GUI_SWITCH), true);
    assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_SWITCH);
    {
        GuiPickBoxes k;
        app.clock = 1000 * app.theme.blink_ms * 2; /* the prompt shown, not blinked away */
        render(&app);
        shows_half(&app, "Load Switch NSP/XCI/NRO:", normal);
        shows(&app, "[ Pick or drop a file ]", yellow);
        app.clock = 0;
        /* the arrows have nothing to move to there; a click on the prompt, or Enter, opens the system's picker, which
           answers no file here (SDL_FILE_DIALOG_DRIVER=none) */
        key(&app, SDLK_DOWN);
        assert(app.menu.view == GUI_LOAD_DROP && !app.picked);
        pick_boxes(&app, &k);
        mouse_px(&app, k.place.list.x + 16, k.prompt + 4, true);
        assert(app.picked && app.menu.view == GUI_LOAD_DROP);
        key(&app, SDLK_RETURN);
        {
            SDL_Event answer; /* its answer comes in the next poll, as a frame's */
            while (SDL_PollEvent(&answer)) event(&app, &answer);
        }
        assert(app.menu.view == GUI_LOAD_DROP && !strcmp(app.message, "No file was picked."));
    }
    /* the NRO made already: opening it plays it, as below (a game not made compiles first) */
    char* input_made = made_game(&app, input, "game with spaces");
    app.menu.selected = GUI_LINUX; /* a drop runs the item that opened the screen */
    SDL_Event e = {0};
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = "one\ntwo";
    event(&app, &e);
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = input;
    free(app.run_log);                                      /* Folders' Logs may have set one */
    app.run_log = snx_path_join(temporary, "last-run.log"); /* the run's output, kept as it comes */
    event(&app, &e);
    play_made(&app);
    assert(app.menu.running && app.menu.selected == GUI_SWITCH);
    finish(&app, 0);
    assert(!app.native && !gui_steps_last(&app.steps, "LAUNCH")); /* a driver that started no game stayed compiling */
    /* a menu that is no runtime's (a build without one) looks for scarletnx beside it, then on PATH */
    {
        char* own = app.self;
        char* path = SDL_strdup(getenv("PATH"));
        app.self = NULL;
        set_env("PATH", "/nonexistent");
        launch(&app, snx_strdup(input), false);
        play_made(&app);
        assert(!app.menu.running && !strcmp(app.message, "Cannot find scarletnx beside ScarletNX or on PATH."));
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ERROR && !strcmp(app.error_line, app.message));
        key(&app, SDLK_RETURN); /* in the box too, whole, until OK */
        assert(!app.confirm.shown);
        set_env("PATH", path);
        SDL_free(path);
        app.self = own;
        app.message[0] = 0;
    }
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.run_log, &size);
        char* line = snx_format("game with spaces;literal.nro --fullscreen=1 --controls=0 --data %s\n", app.data_dir);
        assert(kept && line && !app.run_out && !strncmp(kept, "$ ", 2) && strstr(kept, line));
        free(line);
        assert(strstr(kept, "driver stdout") && strstr(kept, "driver stderr") && strstr(kept, "\n[ended, status 0]\n"));
        free(kept);
    }
    made_gone(&app, input_made);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_SPACE);
    assert(app.menu.selected == GUI_LINUX && app.menu.view == GUI_LOAD_DROP);
    char* quoted = snx_format("\"%s\"", input);
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = quoted;
    event(&app, &e);
    free(quoted);
    assert(app.menu.running);
    finish(&app, 7);
    size_t size;
    unsigned char* runs = snx_read_file(input, &size);
    assert(runs && size == 1 && runs[0] == '2');
    free(runs);
    char* flags_input = snx_path_join(temporary, "option flags.nro");
    assert(!snx_write_file(flags_input, "fixture", 7));
    input_made = made_game(&app, flags_input, "option flags");
    app.menu.selected = GUI_SWITCH;
    launcher(&app, true);
    launch(&app, snx_strdup(flags_input), false);
    play_made(&app);
    finish(&app, 0);
    assert(!app.native && gui_steps_last(&app.steps, "LAUNCH"));
    launcher(&app, false);
    made_gone(&app, input_made);
    app.menu.selected = GUI_LINUX;
    free(flags_input);
    /* the folders picked in Settings > App > Folders, passed to the driver of a first compile: games/ for every input,
       the extraction for a dump, under the picked folder as the driver names it; one gone since stops it, naming it */
    {
        char *games = snx_path_join(temporary, "picked games"), *work = snx_path_join(temporary, "picked work");
        char *dump = snx_path_join(temporary, "picked folders.nsp"),
             *nro = snx_path_join(temporary, "picked folders.nro"), *got = snx_path_join(temporary, "driver argv");
        char *games_was = snx_strdup(app.games_dir), *keys_was = snx_strdup(app.keys_path), expected[512], *argv_got;
        char* out = snx_path_join(work, "picked folders");
        assert(!snx_mkdirs(games) && !snx_mkdirs(work) && !snx_write_file(dump, "fixture", 7) &&
               !snx_write_file(nro, "fixture", 7));
        place_set(&app, GUI_PLACE_GAMES, games);
        place_set(&app, GUI_PLACE_WORK, work);
        app.menu.selected = GUI_SWITCH;
        driver_will(got, 0, "Recompiling picked folders");
        launch(&app, snx_strdup(dump), false); /* read in place: no extraction to put in the folder picked */
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "-r", dump) && has_arg(argv_got, "--outdir", games) &&
               !has_arg(argv_got, "--out", NULL));
        free(argv_got);
        assert(gui_settings_set(&app.settings, "extract", "on")); /* Extract dump: extracted there */
        launch(&app, snx_strdup(dump), false);
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "--extract-container", NULL) && has_arg(argv_got, "--outdir", games) &&
               has_arg(argv_got, "--out", out));
        free(argv_got);
        assert(gui_settings_set(&app.settings, "extract", "off"));
        launch(&app, snx_strdup(nro), false); /* an NRO: no extraction */
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "-r", nro) && has_arg(argv_got, "--outdir", games) &&
               !has_arg(argv_got, "--out", NULL));
        free(argv_got);
        driver_will(NULL, 0, NULL);
        assert(!snx_remove_tree(work));
        launch(&app, snx_strdup(dump), false);
        assert(!launched(&app) && strstr(app.message, "Extractions folder ") &&
               strstr(app.message, " is not there. Settings > App > Folders picks another."));
        app.menu.selected = GUI_LINUX;
        places_reset(&app);
        snprintf(expected, sizeof expected, "Folders: Reset to Defaults: nothing was moved");
        assert(!app.place[GUI_PLACE_GAMES] && !app.place[GUI_PLACE_WORK] && !strcmp(app.message, expected));
        app.message[0] = 0;
        free(app.games_dir);
        app.games_dir = games_was;
        free(app.keys_path);
        app.keys_path = keys_was;
        free(out);
        free(got);
        free(nro);
        free(dump);
        free(work);
        free(games);
    }

    /* a dump with more than one folder of the recompiled library that can take its game: asked which, its Main first
       with what it is and the room each has; the one taken is its first compile's --outdir; Back starts nothing; with
       one, no box */
    {
        char *extra = snx_path_join(temporary, "recompiled extra"),
             *dump = snx_path_join(temporary, "elsewhere dump.nsp"), *got = snx_path_join(temporary, "driver argv");
        char question[512], *argv_got;
        bool still = app.animations_off;
        app.animations_off = true; /* the run's foot read at once, no box's shade fading over it */
        assert(!snx_mkdirs(extra) && !snx_write_file(dump, "fixture", 7));
        folder_keep(&app, extra, GUI_LIBRARY_RECOMPILED, false);
        folder_keep(&app, "/nonexistent/snx-recompiled", GUI_LIBRARY_RECOMPILED, false); /* not there: not offered */
        app.menu.selected = GUI_SWITCH;
        driver_will(got, 0, "Recompiling elsewhere dump");
        launch(&app, snx_strdup(dump), false);
        assert(app.dests_shown && app.dests.count == 2 && !launched(&app) && app.dest_file);
        render(&app);
#ifndef _WIN32
        { /* off Windows a \ is a letter of the file's name, which the question keeps whole */
            char* was = app.dest_file;
            char odd[] = "/somewhere/odd\\name.nsp";
            app.dest_file = odd;
            dests_question(&app, question, sizeof question);
            assert(!strcmp(question, "odd\\name.nsp: into which folder of the recompiled library?"));
            app.dest_file = was;
        }
#endif
        snprintf(question, sizeof question, "%s", "elsewhere dump.nsp: into which folder of the recompiled library?");
        shows(&app, question, rgb(app.theme.warning));
        shows(&app, "default, ", yellow); /* the Main first, in focus */
        { /* YoRHa's choices as wide as the longest of them with the marker and two columns, past the cursor's margin */
            GuiChoices c;
            GuiTheme was = app.theme;
            char text[700];
            int cols;
            app.theme = *gui_theme_builtin("yorha");
            dests_place(&app, &c, question, sizeof question);
            cols = gui_text_cols(choice_text(&app, GUI_CHOICES_DESTS, 0, text, sizeof text)) +
                   gui_text_cols(app.theme.marker) + 2;
            assert(cols > 24 && choice_box(&app, &c, 0).w >= cols * cell_w(&app));
            app.theme = was;
        }
        key(&app, SDLK_ESCAPE);
        assert(!app.dests_shown && !launched(&app) && !app.dest_file && !app.dest_out);
        { /* the pointer over a folder: its focus; a click off the box is Back */
            GuiChoices c;
            launch(&app, snx_strdup(dump), false);
            render(&app);
            dests_place(&app, &c, question, sizeof question);
            point(&app, choice_box(&app, &c, 1), false);
            assert(app.dests_shown && app.dests.focus == 1);
            mouse_px(&app, c.box.x - 4, c.box.y + c.box.h / 2, true);
            assert(!app.dests_shown && !launched(&app) && !app.dest_file && !app.dest_out);
        }
        /* the second taken, by its keys, then by a click: each compile's --outdir that folder */
        for (int pass = 0; pass < 2; ++pass) {
            launch(&app, snx_strdup(dump), false);
            if (!pass) {
                key(&app, SDLK_DOWN);
                key(&app, SDLK_RETURN);
            } else {
                GuiChoices c;
                render(&app);
                dests_place(&app, &c, question, sizeof question);
                point(&app, choice_box(&app, &c, 1), true);
            }
            assert(!app.dests_shown && launched(&app) && !app.dest_out && app.menu.selected == GUI_SWITCH);
            argv_got = ran(&app, 0);
            assert(has_arg(argv_got, "-r", dump) && has_arg(argv_got, "--outdir", extra));
            free(argv_got);
        }
        free(app.folders[--app.folder_count]); /* off the list, no screen of Folders opened for them */
        free(app.folders[--app.folder_count]);
        launch(&app, snx_strdup(dump), false); /* the Main alone: no box, its default */
        assert(!app.dests_shown && launched(&app));
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "-r", dump) && !has_arg(argv_got, "--outdir", NULL));
        free(argv_got);
        driver_will(NULL, 0, NULL);
        app.animations_off = still;
        free(got);
        free(dump);
        free(extra);
    }
    /* a dump not extracted yet, with less room than its extraction takes (straight into its game's folder: once):
       asked first, Run Anyway pressed twice; Back starts nothing. With room, made already or an NRO, it starts at once */
    {
        char *dump = snx_path_join(temporary, "room dump.nsp"), *nro = snx_path_join(temporary, "room dump.nro");
        char *work_was = app.work_dir, *games_was = app.games_dir, *out, *argv_got;
        char *got = snx_path_join(temporary, "driver argv"), *dump_folder = snx_dirname(dump);
        GuiRect r;
        GuiChoices l;
        void* zeros = calloc(1, 3u << 20);
        assert(zeros && !snx_write_file(dump, zeros, 3u << 20) && !snx_write_file(nro, zeros, 3u << 20));
        free(zeros);
        app.work_dir = snx_path_join(temporary, "room work"); /* one disk, as the defaults are */
        app.games_dir = snx_path_join(temporary, "room games");
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        driver_will(got, 0, "Recompiling room dump");
        app.room_free = 2u << 20;              /* 3 MB to take */
        launch(&app, snx_strdup(dump), false); /* Extract dump off: read in place, it takes no room */
        assert(!app.confirm.shown && launched(&app));
        argv_got = ran(&app, 0);
        /* a dump from a folder of its own, the folders their defaults: the data folder alone, no --out or --outdir, so
           the extraction goes to the data folder's work/ (on Android in the app's own storage), never beside it */
        assert(has_arg(argv_got, "-r", dump) && has_arg(argv_got, "--data", app.data_dir) &&
               !has_arg(argv_got, "--out", NULL) && !has_arg(argv_got, "--outdir", NULL) &&
               !snx_path_within(app.data_dir, dump_folder) && !snx_path_within(dump_folder, app.data_dir));
        free(argv_got);
        assert(gui_settings_set(&app.settings, "extract", "on"));
        launch(&app, snx_strdup(dump), false);
        assert(!launched(&app) && app.confirm.shown && app.confirm.ask == GUI_ASK_ROOM && app.room_file);
        render(&app);
        shows(&app, "Needs about 3 MB, 2 MB free.", rgb(app.theme.warning));
        shows(&app, "- Cancel", yellow);
        shows(&app, "- Run Anyway", normal);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !launched(&app) && !app.room_file);
        launch(&app, snx_strdup(dump), false);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(app.confirm.shown && app.confirm.armed && !launched(&app));
        render(&app);
        shows(&app, "- Run Anyway: press again", yellow);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && launched(&app) && !app.room_ok && app.menu.selected == GUI_SWITCH);
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "--extract-container", NULL)); /* asked about its room only when it is extracted */
        free(argv_got);
        /* R's recompile of a dump is asked about too, and Run Anyway prepares it again, playing nothing: in the queue,
           its Compile tab shown */
        app.recompile = true;
        launch(&app, snx_strdup(dump), false);
        app.recompile = false;
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ROOM && !launched(&app));
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !app.menu.running && !app.recompile && gui_queue_running(&app.queue) == 0);
        assert(app.menu.view == GUI_LOAD_RECOMPILE && tab_shown(&app) == GUI_TAB_RECOMPILE);
        assert(app.queue.job[0].kind == GUI_JOB_RECOMPILE);
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "-r", dump) && has_arg(argv_got, "--extract-container", NULL));
        free(argv_got);
        assert(gui_steps_last(&app.ended_steps, "RECOMPILE") && !gui_steps_last(&app.ended_steps, "LAUNCH"));
        /* a finger: the first tap arms Run Anyway, the second starts it */
        launch(&app, snx_strdup(dump), false);
        ask_boxes(&app, &r, &l);
        touch_box(&app, choice_box(&app, &l, 1));
        assert(app.confirm.shown && app.confirm.armed && !launched(&app));
        touch_box(&app, choice_box(&app, &l, 1));
        assert(!app.confirm.shown && launched(&app));
        free(ran(&app, 0));
        app.room_free = 3u << 20;
        launch(&app, snx_strdup(dump), false);
        assert(!app.confirm.shown && launched(&app));
        free(ran(&app, 0));
        app.room_free = 1;
        launch(&app, snx_strdup(nro), false);
        assert(!app.confirm.shown && launched(&app));
        free(ran(&app, 0));
        /* made already: a game of its own whose .ini names the dump (source=), or whose folder links to it as an older
           version made it. Its recent row runs the game, and Games lists the game alone */
        out = snx_path_join(app.games_dir, "0100ABCD12345000");
        {
            char *data = snx_path_join(out, "0100ABCD12345000_data"),
                 *link = snx_path_join(out, "0100ABCD12345000.source");
            char *data_was = app.data_dir, *ini, *dumps, *listed;
#ifdef _WIN32
            char* program = snx_path_join(out, "0100ABCD12345000.exe");
#else
            char* program = snx_path_join(out, "0100ABCD12345000");
#endif
            GuiGames games = {0};
            GuiRecent was = app.recent;
            char message[sizeof app.message];
            size_t i, shown = 0;
            app.data_dir = snx_temp_dir(NULL); /* its dumps/, which Games lists; --repo, never in the dump's folder */
            dumps = snx_path_join(app.data_dir, "dumps");
            listed = snx_path_join(dumps, "made dump.nsp");
            assert(!snx_mkdirs(data) && !snx_mkdirs(dumps) && !snx_write_file(listed, "x", 1));
            assert(!snx_record_source(out, "0100ABCD12345000", listed));
            games_scan(&app, &games, message, sizeof message); /* named, but no program: nothing made yet */
            for (i = 0; i < games.count; ++i) shown += games.paths[i] && snx_same_file(games.paths[i], listed);
            assert(shown == 1);
            assert(!snx_record_source(out, "0100ABCD12345000", dump));
            launch(&app, snx_strdup(dump), false);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ROOM && !app.menu.running);
            key(&app, SDLK_RETURN); /* Back */
            assert(!app.confirm.shown && !snx_copy_file(self, program) && !snx_make_executable(program));
            stamp_here(out, "0100ABCD12345000");   /* this build's: Play plays it */
            launch(&app, snx_strdup(dump), false); /* made: played or recompiled, as asked */
            play_made(&app);
            assert(!app.confirm.shown && app.menu.running);
            finish(&app, 0);
            app.recent = (GuiRecent){0};
            assert(!gui_recent_add(&app.recent, dump, GUI_SWITCH, 0));
            assert(gui_recent_rename(&app.recent, dump, "0100ABCD12345000")); /* as its run's line named it */
            recent_as_games(&app);
            assert(app.recent.count == 1 && app.recent.game[0].prepared &&
                   snx_same_file(app.recent.game[0].path, program));
            assert(!strcmp(app.recent.game[0].name, "0100ABCD12345000")); /* no control.nacp: its folder's name */
            {
                static unsigned char nacp[0x300];
                char* file = snx_path_join(data, "control.nacp");
                memcpy(nacp, "Made Quest", 10);
                assert(!snx_write_file(file, nacp, sizeof nacp));
                free(file);
            }
            recent_as_games(&app); /* by the name Games shows it by, not its ID */
            assert(app.recent.count == 1 && !strcmp(app.recent.game[0].name, "Made Quest"));
            { /* its recent row says what Games' row of it says; a recent game Games doesn't list, what its file is */
                static const struct {
                    const char* file;
                    int driver;
                    const char* kind;
                } unlisted[] = {{"far away.nsp", GUI_SWITCH, "A dump, prepared when it runs"},
                                {"tool.elf", GUI_LINUX, "Linux ARM ELF, run with scarletnx-gnuabi"},
                                {"homebrew.cia", GUI_3DS, "3DS game, opened with scarletnx-3ds"},
                                {"card.3ds", GUI_3DS, "3DS game, opened with scarletnx-3ds"},
                                {"homebrew.vpk", GUI_VITA, "Vita VPK, opened with scarletnx-vita"},
                                {"homebrew.gba", GUI_GBA, "GBA ROM, played with scarletnx-agb"}};
                GuiRecent was = app.recent;
                GuiGamesBoxes none = {0};
                char kind[192], listed[192];
                const char* about;
                size_t k;
                list_games(&app);
                for (k = 0; k < app.games.count &&
                            !(app.games.paths[k] && !strcmp(app.games.paths[k], app.recent.game[0].path));
                     ++k) {}
                assert(k < app.games.count);
                game_kind(&app, &app.games, k, listed, sizeof listed);
                recent_changed(&app);
                app.browser.selected = 0;
                assert(games_focus(&app, &none, &about, kind, sizeof kind) == app.recent.game[0].name &&
                       about == app.recent.game[0].path && !strcmp(kind, listed));
                for (k = 0; k < sizeof unlisted / sizeof unlisted[0]; ++k) {
                    char* path = snx_path_join(temporary, unlisted[k].file);
                    assert(!gui_recent_add(&app.recent, path, unlisted[k].driver, 0));
                    recent_changed(&app);
                    app.browser.selected = 0;
                    games_focus(&app, &none, &about, kind, sizeof kind);
                    assert(!strcmp(kind, unlisted[k].kind));
                    free(path);
                }
                app.recent = was;
                recent_changed(&app);
                app.browser.selected = 0;
            }
            { /* its icon, icon.jpg beside its control.nacp: none, then drawn once and kept, and one that does not draw
                 said with its file */
                IconJpeg jpeg = {{0}, 0};
                char* icon = snx_path_join(data, "icon.jpg");
                SDL_Texture* drawn;
                icons_free(&app);
                assert(!game_icon(&app, program));
                assert(!snx_splash_jpeg("wimsy-fuddles", 32, 90, icon_jpeg_write, &jpeg));
                assert(!snx_write_file(icon, jpeg.data, (size_t)jpeg.size));
                assert(!game_icon(&app, program)); /* as it was drawn, until the list is read again */
                icons_free(&app);
                drawn = game_icon(&app, program);
                assert(drawn && game_icon(&app, program) == drawn && game_icon(&app, dump) == app.icon_loading);
                list_games(&app); /* Games' row of it: its icon before its name */
                {
                    UiRowInfo info = {0};
                    size_t k, lead = (size_t)games_lead(&app);
                    for (k = 0;
                         k < app.games.count && !(app.games.paths[k] && snx_same_file(app.games.paths[k], program));
                         ++k) {}
                    assert(k < app.games.count && k < app.browser.page_size);
                    assert(ui_games_rows_get_row_info(&app, lead + k, 300, &info) && info.image &&
                           info.image == game_icon(&app, program));
                }
                assert(!snx_write_file(icon, "not a JPEG", 10));
                icons_free(&app);
                app.message[0] = 0;
                assert(!game_icon(&app, program) && strstr(app.message, icon) &&
                       strstr(app.message, "could not be drawn"));
                icons_free(&app);
                assert(!remove(icon));
                free(icon);
            }
            { /* a dump's icon, asked of the driver (-x --icon) into the menu's cache: none until it answers, once, then
                 its row's; one it has none for keeps what the driver said and is not asked again */
                char* cache_was = getenv("XDG_CACHE_HOME") ? snx_strdup(getenv("XDG_CACHE_HOME")) : NULL;
                char *cache = snx_path_join(temporary, "icon cache"), *with = snx_path_join(temporary, "with icon.nsp");
                char *without = snx_path_join(temporary, "no icon.nsp"), *dir, *why;
                char* said;
                size_t said_size = 0;
                unsigned answered;
                set_env("XDG_CACHE_HOME", cache);
                assert(!snx_write_file(with, "x", 1) && !snx_write_file(without, "x", 1));
                icons_free(&app);
                dump_icons_pass(&app.dump_icons); /* the dumps Games' rows asked for before this */
                pump(&app);
                icons_free(&app);
                answered = app.dump_icons.answered;
                /* the loading mark while it is asked, in the place of the icon to come */
                assert(game_icon(&app, with) == icon_loading(&app) && app.icon_loading && app.dump_icons.count == 1 &&
                       !app.dump_icons.thread);
                icons_free(&app);
                assert(game_icon(&app, with) == app.icon_loading && app.dump_icons.count == 1); /* asked once */
                dump_icons_pass(&app.dump_icons);
                assert(!app.dump_icons.count && app.dump_icons.answered == answered + 1 &&
                       game_icon(&app, with) == app.icon_loading);
                pump(&app); /* an answer: the rows without one look again */
                assert(app.dump_icons_seen == answered + 1 && game_icon(&app, with) &&
                       game_icon(&app, with) != app.icon_loading);
                assert(game_icon(&app, without) == app.icon_loading);
                dump_icons_pass(&app.dump_icons);
                pump(&app);
                assert(!game_icon(&app, without)); /* none, and no loading mark */
                dir = dump_icon_dir(&app, without);
                why = snx_path_join(dir, "why.txt");
                said = (char*)snx_read_file(why, &said_size);
                assert(said && strstr(said, "no icon.nsp: it has no Control NCA"));
                icons_free(&app);
                assert(!game_icon(&app, without) && !app.dump_icons.count); /* said once: not asked again */
                { /* keys put in: a dump the driver could not read is asked again */
                    char* keys_was = app.keys_path;
                    app.keys_path = snx_path_join(temporary, "icon cache keys");
                    assert(!snx_write_file(app.keys_path, "keys", 4));
                    icons_free(&app);
                    assert(game_icon(&app, without) == app.icon_loading && app.dump_icons.count == 1);
                    dump_icons_pass(&app.dump_icons);
                    assert(!remove(app.keys_path));
                    free(app.keys_path);
                    app.keys_path = keys_was;
                }
                dump_icons_stop(&app);
                set_env("XDG_CACHE_HOME", cache_was);
                icons_free(&app);
                free(said);
                free(why);
                free(dir);
                free(without);
                free(with);
                free(cache);
                free(cache_was);
            }
            app.recent = was;
            recent_changed(&app);
            assert(!snx_record_source(out, "0100ABCD12345000", listed));
            games_scan(&app, &games, message, sizeof message);
            for (i = 0, shown = 0; i < games.count; ++i)
                shown += games.paths[i] && snx_same_file(games.paths[i], listed);
            assert(!shown);
            gui_games_free(&games);
            ini = snx_game_ini(out, "0100ABCD12345000"); /* the link alone, no source= */
            assert(!remove(ini));
            if (!snx_symlink(dump, link)) {            /* Windows without the right to make one: none to test */
                launch(&app, snx_strdup(dump), false); /* made, an older version's way: asked as well */
                play_made(&app);
                assert(!app.confirm.shown && app.menu.running);
                finish(&app, 0);
            }
            assert(!snx_remove_tree(app.data_dir));
            free(app.data_dir);
            app.data_dir = data_was;
            free(ini);
            free(program);
            free(listed);
            free(dumps);
            free(link);
            free(data);
        }
        assert(!snx_remove_tree(out));
        free(out);
        app.room_free = 0;
        assert(gui_settings_set(&app.settings, "extract", "off"));
        free(app.work_dir);
        app.work_dir = work_was;
        free(app.games_dir);
        app.games_dir = games_was;
        launcher(&app, false);
        driver_will(NULL, 0, NULL);
        remove(got); /* the made game's run's */
        app.menu.selected = GUI_LINUX;
        free(dump_folder);
        free(got);
        free(nro);
        free(dump);
    }

    /* recent games: what the GUI ran, newest first at the top of Games and kept in a file, named as its first compile
       named it; once made, it plays again with the options the menu has now, which the fake driver checks */
    {
        GuiRecent kept;
        char* recent_file = snx_path_join(temporary, "prefs/recent.txt");
        char *again = snx_path_join(temporary, "option flags again.nro"),
             *got = snx_path_join(temporary, "driver argv");
        char *again_made, *argv_got;
        SDL_Color soft = rgb(app.theme.soft);
        assert(!snx_write_file(again, "fixture", 7));
        recent_open(&app, snx_strdup(recent_file));
        assert(!app.recent.count && !app.browser.head);
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        driver_will(got, 0, "Recompiling option flags");
        launch(&app, snx_strdup(again), false);
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "-r", again));
        free(argv_got);
        driver_will(NULL, 0, NULL);
        free(got);
        assert(app.recent.count == 1 && app.browser.head == 1 && !strcmp(app.recent.game[0].name, "option flags"));
        assert(app.recent.game[0].driver == GUI_SWITCH && !app.recent.game[0].prepared);
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && !strcmp(kept.game[0].name, "option flags"));
        assert(!strcmp(kept.game[0].path, app.recent.game[0].path));
        /* in Games, not the menu: a title and the game, then the games; the focus on it, first */
        render(&app);
        hides(&app, "Recent Games", soft); /* the game itself shows in the last output, beside the items */
        go_tab(&app, GUI_TAB_GAMES);
        render(&app);
        hides(&app, "- Add Folder", normal);
        shows(&app, "Recent Games", soft);
        assert(app.browser.selected == 0);
        shows(&app, "option flags", yellow); /* a dump's: the loading mark between the marker and its name */
        {
            char start[21]; /* the tips: where it is */
            snprintf(start, sizeof start, "%s", app.recent.game[0].path);
            shows(&app, start, normal);
        }
        /* made since, it plays from there, whatever item the menu last had: its own driver, the options of now; the
           menu keeps its item */
        again_made = made_game(&app, again, "option flags");
        app.menu.selected = GUI_LINUX;
        key(&app, SDLK_RETURN);
        play_made(&app);
        assert(app.menu.running && app.menu.selected == GUI_LINUX && app.recent.game[0].driver == GUI_SWITCH);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.menu.view == GUI_LOAD_NONE);
        /* the mouse over the game: the focus on it */
        go_tab(&app, GUI_TAB_GAMES);
        point(&app, game_box(&app, 0, true), false);
        assert(app.browser.selected == 0);
        /* YoRHa: the focused game on the band, the title in its soft colour */
        app.theme = *gui_theme_builtin("yorha");
        render(&app);
        shows(&app, "Recent Games", rgb(app.theme.soft));
        shows(&app, "option flags", rgb(app.theme.page));
        app.theme = *gui_theme_builtin("default");
        /* a game whose file went away leaves the list when chosen, and the file says so: the game made, its row's */
        assert(!remove(again));
        made_gone(&app, again_made);
        key(&app, SDLK_RETURN);
        assert(!app.menu.running && !app.recent.count && !app.browser.head && !app.browser.selected);
        assert(strstr(app.message, "is gone"));
        gui_recent_load(&kept, recent_file);
        assert(!kept.count);
        render(&app);
        hides(&app, "Recent Games", soft);
        /* five: three of them, and a row that shows the rest */
        {
            const char* names[] = {"e", "d", "c", "b", "a"}; /* added oldest first: a is the newest */
            for (int i = 0; i < 5; ++i) {
                char* path = snx_format("%s/%s.nro", temporary, names[i]);
                assert(!gui_recent_add(&app.recent, path, GUI_SWITCH, 0));
                free(path);
            }
            recent_changed(&app);
            assert(app.recent.count == 5 && app.browser.head == 4 && !app.recent_all);
            render(&app);
            shows(&app, "Recent Games", soft);
            shows(&app, "- a", yellow); /* the focus, the first row */
            shows(&app, "- c", normal);
            shows(&app, "- Show More", normal);
            hides(&app, "- d", normal);
            app.browser.selected = 3; /* the row that shows the rest */
            app.message[0] = 0;       /* the gone game's, over the tips */
            render(&app);
            shows(&app, "- Show More", yellow);
            { /* in the tips under the head, not again beside the list */
                GuiGamesBoxes g;
                int tx, ty;
                games_boxes(&app, &g);
                int half = text_half(&app), found; /* the tips' letters */
                found = find_text(&app, "The recent games not shown", normal, 1, &tx, &ty);
                app.half = half;
                assert(found && ty >= g.place.tips.y && ty < g.place.tips.y + g.place.tips.h);
            }
            key(&app, SDLK_RETURN); /* all five, the focus on the first that was hidden */
            assert(!app.menu.running && app.recent_all && app.browser.head == 5 && app.browser.selected == 3);
            render(&app);
            shows(&app, "- d", yellow);
            shows(&app, "- e", normal);
            app.recent = (GuiRecent){0};
            app.recent_all = false;
            recent_changed(&app);
            go_tab(&app, GUI_TAB_MENU);
        }
        /* a list that cannot be written: the run's end says so, and the menu after it, until the list is kept */
        {
            char* blocker = snx_path_join(temporary, "not a folder");
            char* unsaved = snx_path_join(blocker, "recent.txt");
            char* game = snx_path_join(temporary, "option flags unsaved.nro");
            char note[512], expected[600];
            char* game_made;
            assert(!snx_write_file(blocker, "file", 4) && !snx_write_file(game, "fixture", 7));
            game_made = made_game(&app, game, "option flags unsaved"); /* made: it plays */
            recent_open(&app, snx_strdup(unsaved));
            app.menu.selected = GUI_SWITCH;
            launcher(&app, true);
            launch(&app, game, false);
            play_made(&app);
            for (Uint64 until = SDL_GetTicks() + 10000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5))
                pump(&app);
            assert(app.finished && !app.abnormal_exit);
            snprintf(note, sizeof note, "Recent Games not kept: %s could not be written.", unsaved);
            snprintf(expected, sizeof expected, "The run ended with status 0. %s", note);
            assert(app.recent_unsaved && app.recent.count == 1 && !strcmp(app.message, expected));
            render(&app);
            shows(&app, "The run ended with status 0.", yellow);
            leave_run(&app);
            assert(!strcmp(app.message, note));
            /* written at last: the note goes, what the message said before it stays */
            assert(!remove(blocker));
            snprintf(app.message + strlen(app.message), sizeof app.message - strlen(app.message), " After.");
            recent_changed(&app);
            assert(!app.recent_unsaved && !strcmp(app.message, "After.") && snx_is_file(unsaved));
            snprintf(app.message, sizeof app.message, "Before. %s", note);
            recent_changed(&app);
            assert(!strcmp(app.message, "Before."));
            assert(!snx_remove_tree(blocker));
            made_gone(&app, game_made);
            free(unsaved);
            free(blocker);
            app.recent = (GuiRecent){0};
            recent_changed(&app);
        }
        /* the rest runs with no list */
        free(app.recent_file);
        app.recent_file = NULL;
        app.message[0] = 0;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(again);
        free(recent_file);
    }

    view_box(&app, temporary);
    grid_view(&app, temporary);
    /* 3DS: its own driver for drops, CIA browser and recents, with no Switch
       flags or direct execution, even when another item is hovered. */
    {
        char* original_games = app.games_dir;
        char* recent_file = snx_path_join(temporary, "prefs/3ds-recent.txt");
        app.games_dir = snx_path_join(temporary, "cia-games");
        char* cia = snx_path_join(app.games_dir, "3ds input.CIA");
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(cia, "fixture", 7));
        char* got = snx_path_join(temporary, "driver argv");
        recent_open(&app, snx_strdup(recent_file));
        launcher(&app, true);
        driver_will(got, 3, "the CIA is no game");
        point(&app, entry_box(&app, GUI_3DS), true);
        assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_3DS);
        point(&app, entry_box(&app, GUI_LINUX), false); /* where Linux's was: nothing there now */
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = cia;
        event(&app, &e);
        assert(launched(&app) && app.menu.selected == GUI_3DS && !app.native);
        compiled_by(&app, "scarletnx-3ds", cia, 3);
        assert(app.recent.count == 1 && app.recent.game[0].driver == GUI_3DS && !app.recent.game[0].prepared);
        GuiRecent kept;
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && kept.game[0].driver == GUI_3DS && !kept.game[0].prepared);
        open_recent(&app, 0);
        compiled_by(&app, "scarletnx-3ds", cia, 3);
        /* a CIA in games/ is one of Games', which compiles it with the 3DS driver */
        go_tab(&app, GUI_TAB_GAMES);
        assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 1);
        app.browser.selected = app.browser.head; /* the game, under the recent one */
        key(&app, SDLK_RETURN);
        assert(launched(&app) && !app.native && app.menu.selected == GUI_3DS);
        compiled_by(&app, "scarletnx-3ds", cia, 3);
        driver_will(NULL, 0, NULL);
        free(got);
        app.recent = (GuiRecent){0};
        recent_changed(&app);
        free(app.recent_file);
        app.recent_file = NULL;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(app.games_dir);
        app.games_dir = original_games;
        app.games_stamp = stamp_now(&app);
        free(cia);
        free(recent_file);
    }
    /* Vita: the same, a VPK with scarletnx-vita */
    {
        char* original_games = app.games_dir;
        char* recent_file = snx_path_join(temporary, "prefs/vita-recent.txt");
        app.games_dir = snx_path_join(temporary, "vpk-games");
        char* vpk = snx_path_join(app.games_dir, "vita input.VPK");
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(vpk, "fixture", 7));
        char* got = snx_path_join(temporary, "driver argv");
        recent_open(&app, snx_strdup(recent_file));
        launcher(&app, true);
        driver_will(got, 4, "the VPK has no eboot.bin");
        point(&app, entry_box(&app, GUI_VITA), true);
        assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_VITA);
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = vpk;
        event(&app, &e);
        assert(launched(&app) && app.menu.selected == GUI_VITA && !app.native);
        compiled_by(&app, "scarletnx-vita", vpk, 4);
        assert(app.recent.count == 1 && app.recent.game[0].driver == GUI_VITA && !app.recent.game[0].prepared);
        GuiRecent kept;
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && kept.game[0].driver == GUI_VITA && !kept.game[0].prepared);
        go_tab(&app, GUI_TAB_GAMES);
        assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 1);
        app.browser.selected = app.browser.head;
        key(&app, SDLK_RETURN);
        assert(launched(&app) && !app.native && app.menu.selected == GUI_VITA);
        compiled_by(&app, "scarletnx-vita", vpk, 4);
        driver_will(NULL, 0, NULL);
        free(got);
        app.recent = (GuiRecent){0};
        recent_changed(&app);
        free(app.recent_file);
        app.recent_file = NULL;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(app.games_dir);
        app.games_dir = original_games;
        app.games_stamp = stamp_now(&app);
        free(vpk);
        free(recent_file);
    }
    /* GBA: a ROM through scarletnx-agb as a VPK through its driver; then a game a driver made, its folder's .ini saying
       gba: Games shows it ready, Enter plays it in its folder through that driver, and Recent keeps it made */
    {
        char* original_games = app.games_dir;
        char* recent_file = snx_path_join(temporary, "prefs/gba-recent.txt");
        app.games_dir = snx_path_join(temporary, "gba-games");
        char* rom = snx_path_join(app.games_dir, "agb input.gba");
        char* folder = snx_path_join(app.games_dir, "made");
#ifdef _WIN32
        char* program = snx_path_join(folder, "made.exe"); /* a game there is an .exe */
#else
        char* program = snx_path_join(folder, "made");
#endif
        char kind[160];
        size_t i;
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(rom, "fixture", 7));
        assert(gui_settings_set(&app.settings, "fullscreen", "1"));
        char *got = snx_path_join(temporary, "driver argv"), *argv_got, *game_arg = snx_format("--game=%s", folder);
        recent_open(&app, snx_strdup(recent_file));
        launcher(&app, true);
        driver_will(got, 4, "the ROM is no game");
        point(&app, entry_box(&app, GUI_GBA), true);
        assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_GBA);
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = rom;
        event(&app, &e);
        assert(launched(&app) && app.menu.selected == GUI_GBA && !app.native);
        compiled_by(&app, "scarletnx-agb", rom, 4);
        assert(app.recent.count == 1 && app.recent.game[0].driver == GUI_GBA && !app.recent.game[0].prepared);
        assert(!snx_mkdirs(folder) && !snx_write_file(program, "#!/bin/sh\n", 10) && !snx_make_executable(program));
        assert(!snx_record_system(folder, "made", SNX_SYSTEM_GBA));
        app.games_stamp = stamp_now(&app);
        go_tab(&app, GUI_TAB_GAMES);
        for (i = 0; i < app.games.count && (!app.games.names[i] || strcmp(app.games.names[i], "made")); ++i);
        assert(i < app.games.count);
        game_kind(&app, &app.games, i, kind, sizeof kind);
        assert(!strcmp(kind, "GBA game, ready to run"));
        app.browser.selected = app.browser.head + i;
        driver_will(got, 5, "ScarletNX - made"); /* a game made: played in its folder, fullscreen as Settings say */
        key(&app, SDLK_RETURN);
        assert(app.menu.running && !app.native && app.menu.selected == GUI_GBA);
        argv_got = ran(&app, 5);
        assert(strstr(argv_got, "scarletnx-agb") < strchr(argv_got, '\n') && has_arg(argv_got, game_arg, NULL) &&
               has_arg(argv_got, "--fullscreen=1", NULL) && !has_arg(argv_got, "--data", NULL));
        free(argv_got);
        assert(app.recent.game[0].driver == GUI_GBA && app.recent.game[0].prepared &&
               !strcmp(app.recent.game[0].path, program));
        /* the ROM in the games folder is one of Games', which Enter plays through scarletnx-agb as a CIA's */
        leave_run(&app);
        go_tab(&app, GUI_TAB_GAMES);
        focus_game(&app, "agb input.gba");
        game_kind(&app, &app.games, app.browser.page * app.browser.page_size + app.browser.selected - app.browser.head,
                  kind, sizeof kind);
        assert(!strcmp(kind, "GBA ROM, played with scarletnx-agb"));
        driver_will(got, 4, "the ROM is no game");
        key(&app, SDLK_RETURN);
        assert(launched(&app) && !app.native && app.menu.selected == GUI_GBA);
        compiled_by(&app, "scarletnx-agb", rom, 4);
        driver_will(NULL, 0, NULL);
        free(game_arg);
        free(got);
        app.recent = (GuiRecent){0};
        recent_changed(&app);
        free(app.recent_file);
        app.recent_file = NULL;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(app.games_dir);
        app.games_dir = original_games;
        app.games_stamp = stamp_now(&app);
        free(program);
        free(folder);
        free(rom);
        free(recent_file);
    }

    char* hung = snx_path_join(temporary, "hang in join.elf");
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    Uint64 until = SDL_GetTicks() + 5000;
    bool ready = false;
    while (SDL_GetTicks() < until) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
        if (ready) break;
        SDL_Delay(5);
    }
    assert(ready && app.menu.running);
    render(&app);
    shows_foot(&app, SAY_ABORT, red);
    int selected = app.menu.selected;
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.running && app.menu.selected == selected);
    { /* the window closed while it runs: asked first; Back keeps it running */
        SDL_Event close = {0};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event(&app, &close);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_CLOSE && !app.closing_ms && !app.menu.quit);
        assert(strstr(ask_question(&app), ": exiting ScarletNX "));
        render(&app);
        shows(&app, "- Exit", normal);
        key(&app, SDLK_ESCAPE);
        assert(!app.confirm.shown && app.menu.running && !app.closing_ms && !app.aborting);
    }
    Uint64 started = SDL_GetTicks();
    key(&app, SDLK_ESCAPE);
    assert(app.aborting && SDL_GetTicks() - started < 1000);
    render(&app);
    shows(&app, "Aborting...", red);
    app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
    app.theme.prompt.blink = false;
    render(&app);
    shows_foot(&app, "[ Aborting... ]", red); /* where the keys go */
    app.theme.prompt = gui_theme_builtin(app.theme.id)->prompt;
    key(&app, SDLK_ESCAPE); /* Repeated ESC while reaping is harmless. */
    while (app.menu.running && SDL_GetTicks() - started < 2000) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && !app.finished && !app.aborting && !app.menu.quit);
    assert(app.menu.view == GUI_LOAD_NONE && app.abnormal_exit);
    assert(!strcmp(app.message, "Aborted with ESC."));
    render(&app);
    shows_half(&app, "Aborted with ESC.", red); /* in the tips */
    /* a finger on a running run's ESC line aborts it, anywhere else it does nothing */
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
    }
    assert(ready && app.menu.running);
    {
        GuiPlace p;
        int cw = 8 * app.scale, ch = 16 * app.scale;
        run_place(&app, &p);
        /* ui.txt's Wide: the output three fifths, the box beside it the rest, 24 columns at least */
        assert(!p.stacked && p.side.x == p.list.x + p.list.w + cw && p.side.w >= 24 * cw &&
               SDL_abs(p.list.w * 2 - p.side.w * 3) <= 5);
        touch_box(&app, p.list); /* the output: nothing */
        assert(app.menu.running && !app.aborting);
        app.theme = *gui_theme_builtin("yorha"); /* a loading screen's ESC line is its last row */
        tap_px(&app, 20 * cw, 3 * ch + ch / 2);
        assert(!app.aborting);
        tap_px(&app, 20 * cw, (app.rows - 1) * ch + ch / 2);
        assert(app.aborting);
        app.theme = *gui_theme_builtin("default");
    }
    while (app.menu.running && SDL_GetTicks() < until) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && app.abnormal_exit);
    /* closed while it runs, Exit: the run ends, then the screen of what the menu keeps for half a second, then out */
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
    }
    assert(ready && app.menu.running);
    {
        SDL_Event close = {0};
        Uint64 closing;
        close.type = SDL_EVENT_QUIT;
        event(&app, &close);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_CLOSE);
        int folders = app.folder_count;
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && app.aborting && app.closing_ms && !app.menu.quit);
        assert(app.folder_count == folders); /* its answer is the run's alone: no other box's (a folder taken off) */
        closing = app.closing_ms;
        render(&app);
        shows(&app, "Closing ScarletNX", rgb(app.theme.panels == GUI_PANELS_FILL ? app.theme.page : app.theme.ink));
        shows(&app, "Kept for the next start:", rgb(app.theme.soft));
        key(&app, SDLK_RETURN); /* nothing else is taken meanwhile */
        assert(app.closing_ms == closing && !app.menu.quit);
        app.clock = closing + GUI_CLOSING_MS - 1;
        pump(&app);
        assert(!app.menu.quit);
        app.clock = closing + GUI_CLOSING_MS;
        pump(&app);
        assert(app.menu.quit);
        for (until = SDL_GetTicks() + 2000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
        assert(!app.menu.running);
        app.menu.quit = false;
        app.closing_ms = 0;
        app.clock = 0;
        app.message[0] = 0;
    }
    /* where ESC is a back gesture made by mistake (ask_abort, Android's) it asks first: ESC again, a click off the box
       or Continue leave the run going; Abort aborts it. The ESC line's tap asks too, and a held key answers nothing */
    {
        int pass;
        GuiRect r;
        GuiChoices l;
        GuiPlace p;
        SDL_Event held;
        app.ask_abort = true;
        for (pass = 0; pass < 2; ++pass) {
            assert(!snx_write_file(hung, "", 0));
            launch(&app, snx_strdup(hung), false);
            for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
                unsigned char* data = snx_read_file(hung, &size);
                ready = data && size == 5 && !memcmp(data, "ready", 5);
                free(data);
                pump(&app);
            }
            assert(ready && app.menu.running);
            key(&app, SDLK_ESCAPE);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_KEEP && !app.aborting && app.menu.running);
            if (pass) break;
            render(&app);
            ask_boxes(&app, &r, &l);
            run_place(&app, &p);
            shows(&app, "Abort the compile? Its work is lost.", rgb(app.theme.warning));
            shows(&app, "- Continue", yellow);
            shows(&app, "- Abort", normal);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && app.menu.running && !app.aborting);
            key(&app, SDLK_AC_BACK);
            key(&app, SDLK_DOWN);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            render(&app);
            shows(&app, "- Continue", normal);
            shows(&app, "- Abort", yellow);
            key(&app, SDLK_ESCAPE); /* on Abort too */
            assert(!app.confirm.shown && !app.aborting);
            touch_box(&app, p.keys); /* the ESC line */
            assert(app.confirm.shown && !app.aborting);
            tap_px(&app, r.x + 16 * app.scale, r.y + 8 * app.scale); /* the question itself: nothing */
            assert(app.confirm.shown);
            tap_px(&app, r.x - 8, choice_box(&app, &l, 1).y + 4); /* beside Abort, off the box */
            assert(!app.confirm.shown && !app.aborting && app.menu.running);
            key(&app, SDLK_ESCAPE);
            point(&app, choice_box(&app, &l, 1), false); /* the pointer moves the focus */
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            point(&app, choice_box(&app, &l, 0), true);
            assert(!app.confirm.shown && !app.aborting && app.menu.running);
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && !app.aborting);
            SDL_zero(held);
            held.type = SDL_EVENT_KEY_DOWN;
            held.key.key = SDLK_ESCAPE;
            held.key.repeat = true;
            event(&app, &held);
            assert(!app.confirm.shown);
            /* over YoRHa's loading screen: the question in a box of ink at its foot, in the page colour, the choices
               above it at its right, the focused one on a band of its focus colour (options-dialog = yorha) */
            app.theme = *gui_theme_builtin("yorha");
            app.fade_ms = 0;
            key(&app, SDLK_ESCAPE);
            render(&app);
            shows(&app, "Abort the compile?", rgb(app.theme.page));
            shows(&app, "\xfe Continue", rgb(app.theme.page));
            shows(&app, "\xfe Abort", rgb(app.theme.ink));
            ask_boxes(&app, &r, &l);
            {
                int w, h;
                area(&app, &w, &h);
                assert(r.y + r.h >= h - cell_h(&app) && r.w > w * 3 / 4); /* the foot's whole width */
                assert(l.at.y + l.at.h <= r.y && l.at.x > w / 2);         /* above it, at its right */
                assert(same_colour(pixel(&app, (float)(r.x + r.w / 2), (float)(r.y + r.h - 2)), rgb(app.theme.ink)));
            }
            { /* a row hit whole, the cursor's margin before its box too; a click between two rows is on the dialog,
                 above its box: it keeps on */
                GuiRect keep = ui_choices_row(l.rects, app.scale, UI_CHOICES_ROWS, 0), box = choice_box(&app, &l, 0);
                GuiRect abort_row = ui_choices_row(l.rects, app.scale, UI_CHOICES_ROWS, 1);
                assert(box.x > keep.x); /* YoRHa's cursor, in the row */
                mouse_px(&app, abort_row.x + 1, abort_row.y + abort_row.h / 2, false);
                assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
                mouse_px(&app, keep.x + 1, keep.y + keep.h / 2, false);
                assert(app.confirm.selected == GUI_CONFIRM_KEEP);
                assert(abort_row.y > keep.y + keep.h && keep.y + keep.h + 1 < r.y);
                mouse_px(&app, keep.x + keep.w / 2, keep.y + keep.h, true);
                assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_KEEP && !app.aborting);
            }
            /* its options-dialog = default: in the middle, the question in the warning colour */
            app.theme.dialog = GUI_DIALOG_DEFAULT;
            render(&app);
            shows(&app, "Abort the compile?", rgb(app.theme.warning));
            app.theme = *gui_theme_builtin("default");
            ask_boxes(&app, &r, &l);
            touch_box(&app, choice_box(&app, &l, 1));
            assert(app.aborting && !app.confirm.shown);
            while (app.menu.running && SDL_GetTicks() < until) {
                pump(&app);
                SDL_Delay(5);
            }
            assert(!app.menu.running && app.abnormal_exit && !app.finished);
            assert(!strcmp(app.message, "Aborted with ESC."));
        }
        /* a run that ends on its own while asking takes the question with it */
        gui_process_terminate(app.process);
        while (app.menu.running && SDL_GetTicks() < until) {
            pump(&app);
            SDL_Delay(5);
        }
        assert(!app.menu.running && app.finished && app.abnormal_exit && !app.confirm.shown);
        key(&app, SDLK_ESCAPE);
        assert(!app.finished && !app.confirm.shown);
        app.ask_abort = false;
    }
    free(hung);

    /* Games: a tab of its own, not an item of the menu; its browser filters, pages and launches both ways. Reopening
       refreshes the folders rather than caching them */
    assert(!snx_mkdirs(app.games_dir));
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES && !app.games.count && !app.browser.head);
    render(&app);
    shows(&app, "No games", normal); /* the side box: no row, and how to add a folder */
    shows(&app, "Add a folder of games or dumps", normal);
    key(&app, SDLK_RETURN); /* no row: nothing */
    assert(!app.editing && app.menu.view == GUI_LOAD_GAMES);
    /* P, or the foot's: Settings > App > Folders at the recompiled library's Add Folder; ESC is Games again, its
       list read again; a tab away goes where it goes from the Settings, Games forgotten */
    press_key(&app, "P", 0, false, yellow);
    assert(app.editing && app.form == GUI_FORM_FOLDERS && app.folders_from_games &&
           app.editor.row == folder_row_of(&app, GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED));
    key(&app, SDLK_ESCAPE);
    assert(!app.editing && !app.folders_from_games && app.menu.view == GUI_LOAD_GAMES && !app.browser.selected);
    key(&app, SDLK_P);
    assert(app.form == GUI_FORM_FOLDERS && app.folders_from_games);
    key(&app, SDLK_TAB);
    assert(!app.editing && !app.folders_from_games && app.menu.view == GUI_LOAD_NONE);
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES);
    for (int i = 0; i < 35; ++i) {
        char name[40];
#ifdef _WIN32
        snprintf(name, sizeof(name), "Game%02d.exe", i);
#else
        snprintf(name, sizeof(name), "Game%02d", i);
#endif
        char* path = snx_path_join(app.games_dir, name);
        assert(!(i ? snx_write_file(path, "fixture", 7) : snx_copy_file(self, path)));
        assert(!snx_make_executable(path));
        free(path);
    }
    const char* ignored[] = {"Game_code.so", "Game_code.dll", ".hidden.exe", "notes.ini"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        char* path = snx_path_join(app.games_dir, ignored[i]);
        assert(!snx_write_file(path, "fixture", 7));
        if (i < 3) assert(!snx_make_executable(path));
        free(path);
    }
    char* directory = snx_path_join(app.games_dir, "directory.exe");
    assert(!snx_mkdirs(directory));
    free(directory);
    /* Games finds them itself as the watcher's thread looks (a look here: watch_pass); F5, or the foot's, at once */
    pump(&app);
    assert(!app.games.count);
    key(&app, SDLK_F5);
    assert(app.games.count == 35 && app.browser.count == 35 && !strcmp(app.message, "Refreshed: 35 games"));
    app.message[0] = 0;
    press_key(&app, "F5", 0, false, yellow);
    assert(app.games.count == 35 && !strcmp(app.message, "Refreshed: 35 games"));
    app.message[0] = 0;
    app.browser.activated = true; /* which a new listing would reset */
    watch_pass(&app.watch);       /* its first look after a listing: what the list holds, not listed again */
    pump(&app);
    assert(app.games.count == 35 && app.browser.count == 35 && !app.games_fresh && app.browser.activated);
    app.browser.activated = false;
    /* the menu: the loads, the keys, Settings, Share Log and Exit, a box each with none between; no games in it. F5
       samples the status again */
    go_tab(&app, GUI_TAB_MENU);
    app.sampled = 1;
    key(&app, SDLK_F5);
    assert(app.sampled > 1 && app.menu.view == GUI_LOAD_NONE);
    render(&app);
    /* animations on: another screen comes in from the page's colour, a seventh of a second; a box's shade comes as it
       opens and goes once it closed; off, each at once */
    {
        GuiGamesBoxes g;
        GuiRect b;
        SDL_Color page = rgb(app.theme.page), was, mid;
        int x, y;
        app.animations_off = false;
        app.clock = 50000;
        render(&app); /* the menu, as it settled */
        go_tab(&app, GUI_TAB_GAMES);
        render(&app);
        games_boxes(&app, &g);
        b = g.place.side;
        x = b.x;
        y = b.y + b.h / 2; /* the side box's frame */
        assert(same_colour(pixel(&app, (float)x, (float)y), page));
        app.clock += GUI_FADE_MS / 2;
        render(&app);
        mid = pixel(&app, (float)x, (float)y);
        app.clock += GUI_FADE_MS / 2;
        render(&app);
        was = pixel(&app, (float)x, (float)y);
        assert(!same_colour(was, page) && !same_colour(mid, page) && !same_colour(mid, was));
        key(&app, SDLK_TAB); /* off: the next screen at once */
        app.animations_off = true;
        render(&app);
        app.animations_off = false;
        app.clock += GUI_FADE_MS;
        render(&app);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        key(&app, SDLK_ESCAPE);
        go_tab(&app, GUI_TAB_MENU);
        render(&app); /* the menu comes in */
        app.clock += GUI_FADE_MS;
        render(&app); /* and has settled */
        {
            GuiMenuBoxes m;
            menu_boxes(&app, &m);
            x = m.place.tabs.x + 1; /* the Menu tab's frame, which the box in the middle leaves */
            y = m.place.tabs.y + m.place.tabs.h / 2;
        }
        was = pixel(&app, (float)x, (float)y);
        key(&app, SDLK_ESCAPE); /* Exit? over the menu: its shade comes in */
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
        render(&app);
        assert(same_colour(pixel(&app, (float)x, (float)y), was));
        app.clock += GUI_FADE_MS;
        render(&app);
        mid = pixel(&app, (float)x, (float)y);
        assert(!same_colour(mid, was));
        key(&app, SDLK_ESCAPE); /* Stay: the shade goes */
        assert(!app.confirm.shown);
        render(&app);
        assert(same_colour(pixel(&app, (float)x, (float)y), mid));
        app.clock += GUI_FADE_MS;
        render(&app);
        assert(same_colour(pixel(&app, (float)x, (float)y), was));
        app.animations_off = true;
        app.clock = 0;
    }
    /* F2, or the foot's: the side box off where it shows, the list the whole width and the status gone with it; on
       again; a group's rows the whole column with it off, the groups out; kept in app.ini */
    {
        GuiMenuBoxes m;
        GuiFormBoxes f;
        size_t size;
        char* kept;
        assert(app.side_box == GUI_SIDE_AUTO);
        key(&app, SDLK_F2);
        menu_boxes(&app, &m);
        assert(app.side_box == GUI_SIDE_OFF && !m.place.side.w && !m.place.status.h &&
               m.place.list.w == m.place.head.w);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && strstr(kept, "\nside_box = off\n"));
        free(kept);
        render(&app);
        hides(&app, "Status", normal);
        open_settings(&app);
        form_boxes(&app, &f);
        assert(app.form == GUI_FORM_SETTINGS && form_shown(&app, &f, false) >= (size_t)app.groups &&
               !form_first(&f, false) && !form_shown(&app, &f, true));
        render(&app);
        hides(&app, "\x1f", rgb(app.theme.soft)); /* the rows' list, gone with the side box, marks nothing */
        open_group(&app, 2);
        form_boxes(&app, &f);
        assert(app.form == GUI_FORM_GROUP && !form_shown(&app, &f, false) && f.place.side.w == f.place.head.w &&
               form_shown(&app, &f, true));
        render(&app);
        shows(&app, "Display", normal);
        /* Display's rows all fit: an arrow would be the groups' */
        assert(form_shown(&app, &f, true) >= (size_t)gui_editor_rows(&app.editor) && !form_first(&f, true));
        hides(&app, "\x1f", rgb(app.theme.soft)); /* nor the groups', gone */
        key(&app, SDLK_F2);                       /* F1's box says it, the foot no longer */
        form_boxes(&app, &f);
        assert(app.side_box == GUI_SIDE_ON && form_shown(&app, &f, false) &&
               f.place.side.x > f.place.list.x + f.place.list.w);
        key(&app, SDLK_ESCAPE);
        key(&app, SDLK_ESCAPE);
        assert(!app.editing);
        app.side_box = GUI_SIDE_AUTO;
        app_save(&app);
        render(&app);
        shows(&app, "Status", normal);
    }
    hides(&app, "- Games", normal);
    hides(&app, "Recent Games", rgb(app.theme.soft));
    {
        GuiMenuBoxes m;
        menu_boxes(&app, &m);
        assert(ui_menu_shown(m.rects, app.scale, UI_MENU_ENTRIES) >= GUI_ITEM_COUNT && GUI_SWITCH == 0);
        /* room past a list's default 8 rows: a ninth entry (Android's Exit) shows where it fits, no scroll */
        assert(ui_menu_shown(m.rects, app.scale, UI_MENU_ENTRIES) > 8);
    }
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    { /* the list's column as wide on the menu as on Games, however long their entries: ui.txt's Screen lays it out */
        GuiMenuBoxes m;
        GuiGamesBoxes g;
        app.menu.view = GUI_LOAD_NONE;
        menu_boxes(&app, &m);
        app.menu.view = GUI_LOAD_GAMES;
        games_boxes(&app, &g);
        assert(g.place.list.w == m.place.list.w);
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 1 && app.browser.selected == 0);
    /* the wheel goes over the pages as Up and Down do, a page's last game on to the next one's first and back, and
       stops at the list's ends, where the keys go round */
    {
        SDL_Event e = {0};
        size_t pages = gui_browser_pages(&app.browser), last;
        assert(pages >= 2);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        app.browser.page = 0;
        last = app.browser.head + gui_browser_items(&app.browser) - 1;
        app.browser.selected = last;
        e.wheel.integer_y = -1;
        event(&app, &e);
        assert(app.browser.page == 1 && app.browser.selected == app.browser.head);
        e.wheel.integer_y = 1;
        event(&app, &e);
        assert(app.browser.page == 0 && app.browser.selected == last);
        app.browser.page = pages - 1;
        last = app.browser.head + gui_browser_items(&app.browser) - 1;
        app.browser.selected = last;
        e.wheel.integer_y = -1;
        event(&app, &e);
        assert(app.browser.page == pages - 1 && app.browser.selected == last);
        app.browser.page = 1;
        app.browser.selected = 0;
    }
    /* a group's heading has no path: the focus on it, as the pointer over it puts it, offers no AOT pack, and M on it
       says what it opens, no box (it took the launcher down); so does M on a dump, which has no data folder */
    {
        GuiGames was = app.games, two = {0};
        GuiBrowser browser_was = app.browser;
        char *names[2] = {"Dumps", "a dump.nsp"}, *paths[2] = {NULL, "/nonexistent/a dump.nsp"};
        size_t addons[2] = {0, 0};
        AotFocus f;
        two.names = two.shown = names;
        two.paths = paths;
        two.addons = addons;
        two.count = 2;
        app.games = two;
        app.browser.count = 2;
        app.browser.page = 0;
        app.browser.selected = app.browser.head; /* the heading */
        assert(!aot_focus_game(&app, &f) && !f.path && !f.folder);
        app.message[0] = 0;
        mods_focus(&app);
        assert(!strcmp(app.message, "M opens the mods and cheats of a recompiled Switch game in focus.") &&
               !app.addons_shown);
        app.message[0] = 0;
        app.browser.selected = app.browser.head + 1; /* a dump: no game folder either */
        assert(!aot_focus_game(&app, &f) && !f.path);
        mods_focus(&app);
        assert(!strcmp(app.message, "M opens the mods and cheats of a recompiled Switch game in focus.") &&
               !app.addons_shown);
        app.message[0] = 0;
        app.games = was;
        app.browser = browser_was;
    }
    /* no row to add a folder, which P, the foot's, does; no Back: ESC is the menu's */
    render(&app);
    hides(&app, "- Add Folder", yellow);
    hides(&app, "Back [ESC]", normal);
    shows_foot(&app, "P", yellow);
    shows_foot(&app, "folders", normal);
    {
        GuiGamesBoxes g;
        games_boxes(&app, &g);
        GuiRect first = games_row_box(&app, &g, 0), second = games_row_box(&app, &g, 1);
        assert(first.y == g.place.list.y && second.y - (first.y + first.h) == 2 * app.scale);
    }
    key(&app, SDLK_P); /* P: Settings > App > Folders, ESC back */
    assert(app.editing && app.form == GUI_FORM_FOLDERS && app.folders_from_games);
    key(&app, SDLK_ESCAPE);
    assert(!app.editing && app.menu.view == GUI_LOAD_GAMES);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    assert(!strncmp(app.games.names[0], "Game00", 6));
    /* the player's folders' games after games/'s, each run from where it is, each library's own: a game ready to run
       in a dumps folder is not listed; a folder gone is named */
    {
#ifdef _WIN32
        char* zed = snx_path_join(app.folders[0], "Zed.exe");
#else
        char* zed = snx_path_join(app.folders[0], "Zed");
#endif
        assert(app.folder_count == 1 && !snx_write_file(zed, "fixture", 7) && !snx_make_executable(zed));
        assert(app.folders_library[0] == GUI_LIBRARY_DUMPS);
        list_games(&app);
        assert(app.games.count == 35 && !app.message[0]);
        app.folders_library[0] = GUI_LIBRARY_RECOMPILED;
        list_games(&app);
        assert(app.games.count == 36 && !strcmp(app.games.paths[35], zed) && !app.message[0]);
        app.folders[1] = snx_strdup("/nonexistent/snx-games");
        app.folders_library[1] = GUI_LIBRARY_DUMPS;
        app.folder_count = 2;
        list_games(&app);
        assert(app.games.count == 36 && strstr(app.message, "/nonexistent/snx-games") &&
               strstr(app.message, "Settings > App > Folders"));
        free(app.folders[1]);
        app.folder_count = 1;
        assert(!snx_remove_tree(zed));
        app.folders_library[0] = GUI_LIBRARY_DUMPS; /* as dropped: one recompiled library folder for the runs below */
        list_games(&app);
        assert(app.games.count == 35 && !app.message[0]);
        free(zed);
    }
    /* listed again, the folders as they were: the first hash after it is the list's all the same */
    watch_pass(&app.watch);
    pump(&app);
    list_games(&app);
    watch_pass(&app.watch);
    pump(&app);
    assert(!app.games_fresh);
    assert(gui_browser_pages(&app.browser) > 1);
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_DOWN);
    assert(app.browser.page == 1 && app.browser.selected == 1);
    /* a game put in while the list shows joins it, the list staying on its page and file; taken out, it leaves */
    {
#ifdef _WIN32
        char* late = snx_path_join(app.games_dir, "Game35.exe");
#else
        char* late = snx_path_join(app.games_dir, "Game35");
#endif
        watch_pass(&app.watch); /* the list as the watcher first sees it */
        pump(&app);
        assert(!snx_write_file(late, "fixture", 7) && !snx_make_executable(late));
        pump(&app); /* nothing yet: the watcher has not looked */
        assert(app.games.count == 35);
        watch_pass(&app.watch);
        pump(&app);
        assert(app.games.count == 36 && app.browser.page == 1 && app.browser.selected == 1);
        assert(!snx_remove_tree(late));
        watch_pass(&app.watch);
        pump(&app);
        assert(app.games.count == 35 && app.browser.page == 1 && app.browser.selected == 1);
        free(late);
    }
    render(&app);
    shows(&app, "\x1e", rgb(app.theme.soft)); /* games on the pages before and after */
    shows(&app, "\x1f", rgb(app.theme.soft));
    screenshot = getenv("SNX_GUI_BROWSER_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 2 && gui_browser_items(&app.browser) == 35 - 2 * app.browser.page_size);
    render(&app);
    shows(&app, "\x1e", rgb(app.theme.soft));
    hides(&app, "\x1f", rgb(app.theme.soft));
    key(&app, SDLK_PAGEUP);
    key(&app, SDLK_LEFT);
    assert(app.browser.page == 0 && app.browser.selected == 1); /* the focus kept from page to page */
    render(&app);
    hides(&app, "\x1e", rgb(app.theme.soft));
    shows(&app, "\x1f", rgb(app.theme.soft));
    point(&app, game_box(&app, 1, false), false);
    assert(app.browser.selected == 1 && !app.menu.running && !app.browser.activated);
    render(&app);
    shows(&app, app.games.names[1], yellow);
    point(&app, game_box(&app, 0, false), true); /* native prepared game, not another extraction */
    assert(app.menu.running && app.native && screen(&app) == GUI_SCREEN_LOADING);
    finish(&app, 0);
    /* a game in a folder of its own beside its data that this build made goes through the menu's driver with the
       settings and the data folder (Game00 above, an older version's beside the others, runs as it is): the driver plays
       it, recompiled first when it finds another build made it, which the run shows as compiling until the game loads
       (one the launcher sees another build made compiles in the queue first, below) */
    {
        char *folder = snx_path_join(app.games_dir, "folder game"), *data = snx_path_join(folder, "folder game_data");
#ifdef _WIN32
        char* program = snx_path_join(folder, "folder game.exe");
#else
        char* program = snx_path_join(folder, "folder game");
#endif
        int item = app.menu.selected;
        assert(!snx_mkdirs(data) && !snx_copy_file(self, program) && !snx_make_executable(program));
        stamp_here(folder, "folder game");
        leave_run(&app);
        app.menu.selected = GUI_SWITCH;
        launch(&app, snx_strdup(program), true);
        assert(app.menu.running && app.native && screen(&app) == GUI_SCREEN_LOADING);
        {
            char name[64];
            run_name_of(&app.steps, app.native, app.file, name, sizeof name);
            assert(!strcmp(name, "FOLDER GAME")); /* its file's, until its recompile names it */
            gui_steps_feed(&app.steps, "Recompiling Folder Quest 2.0 (1 modules)\n", 41, 0);
            run_name_of(&app.steps, app.native, app.file, name, sizeof name);
            assert(!strcmp(name, "FOLDER QUEST 2.0") && screen(&app) == GUI_SCREEN_COMPILING);
        }
        gui_steps_feed(&app.steps, "ScarletNX - folder game\n", 24, 0);
        assert(screen(&app) == GUI_SCREEN_LOADING);
        gui_steps_reset(&app.steps);
        finish(&app, 0);
        assert(gui_steps_last(&app.steps, "RECOMPILE") && gui_steps_last(&app.steps, "LAUNCH"));
        app.menu.selected = item;
        /* R on it recompiles it through the driver with the backend picked and no option of a run (-S -s, the
           settings), playing nothing; R on Add Folder or a CIA says why it does nothing. The foot says so, and its
           word presses R as the key does */
        {
            char* cia = snx_path_join(app.games_dir, "r test.cia");
            assert(!snx_write_file(cia, "fixture", 7));
            go_tab(&app, GUI_TAB_GAMES);
            render(&app);
            shows_foot(&app, "R", yellow); /* its key, then what it does */
            shows_foot(&app, "recompile", normal);
            assert(hint_keys[hint_pick("R", 0, false)].key == SDLK_R &&
                   hint_keys[hint_pick("R", 5, false)].key == SDLK_R);
            assert(hint_keys[hint_pick("Recompile", 3, false)].key == SDLK_R); /* Android's: no R to name */
            assert(hint_keys[hint_pick("Mods", 2, false)].key == SDLK_M);      /* and no M: a tap on it opens the box */
            { /* a key's word alone is that key (a finger's "Refresh."); a sentence of other words, text */
                const char* at = "Refresh. Back.";
                char k[64], w[64];
                assert(hint_next(&at, k, w, sizeof k) && !strcmp(k, "Refresh") && !w[0]);
                assert(hint_next(&at, k, w, sizeof k) && !strcmp(k, "Back") && !w[0] &&
                       !hint_next(&at, k, w, sizeof k));
                at = "Type the nickname.";
                assert(hint_next(&at, k, w, sizeof k) && !k[0] && !strcmp(w, "Type the nickname"));
            }
            /* B: AOT pack from the keyboard, back from a pad */
            assert(hint_keys[hint_pick("B", 0, false)].key == SDLK_B &&
                   hint_keys[hint_pick("B", 0, true)].key == SDLK_ESCAPE);
            assert(hint_keys[hint_pick("LB", 0, true)].key == SDLK_TAB && hint_keys[hint_pick("LB", 0, true)].mod);
            assert(hint_pick("LB", 0, false) < 0);
            assert(hint_keys[hint_pick("L3", 0, true)].key == SDLK_M &&
                   hint_keys[hint_pick("R3", 0, true)].key == SDLK_B);
            app.browser.selected = 0;
            key(&app, SDLK_R);
            assert(!app.menu.running && !strcmp(app.message, "No game in focus to recompile."));
            focus_game(&app, "r test.cia");
            { /* R is said on any console's game, M on a Switch one alone: R on a CIA, not M */
                char said[512];
                assert(strstr(screen_said(&app, true, said, sizeof said), "R: recompile.") &&
                       !strstr(said, "M: mods."));
            }
            {
                GuiRecent was = app.recent;
                char* got = snx_path_join(temporary, "driver argv");
                driver_will(got, 0, "Built r test");
                key(&app, SDLK_R); /* through the 3DS driver's -r, as a Switch game's through scarletnx -r: queued */
                assert(launched(&app) && !app.menu.running); /* the menu keeps its item: the job has its console */
                assert(app.queue.job[0].kind == GUI_JOB_RECOMPILE && app.queue.job[0].console == GUI_3DS &&
                       !app.queue.job[0].native);
                compiled_by(&app, "scarletnx-3ds", cia, 0);
                driver_will(NULL, 0, NULL);
                free(got);
                assert(!app.recompile);
                app.recent = was;
                recent_changed(&app);
                go_tab(&app, GUI_TAB_GAMES);
            }
            focus_game(&app, "folder game");
            {
                char said[512];
                int state = app.aot_state, match = app.aot_match;
                assert(strstr(screen_said(&app, true, said, sizeof said), "R: recompile.") && strstr(said, "M: mods."));
                /* B where it builds a pack: a cache read whole, at the build's level */
                app.aot_state = GUI_AOT_BAD;
                assert(!strstr(screen_said(&app, true, said, sizeof said), "B: AOT pack."));
                app.aot_state = GUI_AOT_CACHE;
                app.aot_match = 0;
                assert(!strstr(screen_said(&app, true, said, sizeof said), "B: AOT pack."));
                app.aot_match = 1;
                assert(strstr(screen_said(&app, true, said, sizeof said), "B: AOT pack."));
                /* Games' every key at once, the most parts a foot puts together: each in F1's box, none left out */
                assert(strstr(said, "R: recompile.") && strstr(said, "M: mods.") && strstr(said, "P: folders.") &&
                       strstr(said, "V: view.") && strstr(said, "ESC: back.") && strstr(said, "Tab: screen."));
#ifndef SDL_PLATFORM_ANDROID
                assert(strstr(said, "S: save states."));
#endif
                assert(strstr(shown_keys(&app, said, sizeof said), "R: recompile.") && strstr(said, "B: AOT pack.") &&
                       strstr(said, "P: folders.") && strstr(said, "ESC: back."));
                app.aot_state = state;
                app.aot_match = match;
            }
            /* a game whose .ini has a Debug setting off its default: its row says so, as no other game's, its side
               panel lists it in the failure colour and its Play says so too; the GDB stub on puts it on every game */
            {
                char *ini = snx_game_ini(folder, "folder game"), *dir = ini ? snx_dirname(ini) : NULL, row[256];
                size_t index, other, size = 0;
                char *was = ini ? (char*)snx_read_file(ini, &size) : NULL, *debug;
                int nx, ny, r;
                bool play = false;
                debug = snx_format("%s\n[Debug]\nlog=2\n", was ? was : ""); /* beside what the folder's .ini has */
                assert(dir && debug && !snx_mkdirs(dir) && !snx_write_file(ini, debug, strlen(debug)));
                snx_menu_settings = debug_settings;
                settings_forget(&app);
                focus_game(&app, "folder game");
                index = app.browser.page * app.browser.page_size + app.browser.selected - app.browser.head;
                for (other = 0; other < app.games.count && (other == index || !app.games.paths[other]); ++other) {}
                assert(other < app.games.count);
                render(&app);
                game_label(&app, &app.games, index, row, sizeof row);
                assert(strstr(row, "  (Debug on)"));
                game_label(&app, &app.games, other, row, sizeof row);
                assert(!strstr(row, "(Debug on)"));
                assert(!strcmp(app.debug_focus, "log=2"));
                assert(find_text(&app, "Debug on: log=2", rgb(app.theme.failure), 1, &nx, &ny));
                key(&app, SDLK_M);
                assert(app.addons_shown && app.addons_debug);
                for (r = 0; r < addons_rows(&app); ++r) {
                    addons_label(&app, r, row, sizeof row);
                    play = play || strstr(row, "Play  (Debug on)");
                }
                assert(play);
                key(&app, SDLK_ESCAPE);
                assert(!app.addons_shown);
                launcher(&app, true);
                render(&app);
                game_label(&app, &app.games, other, row, sizeof row);
                assert(strstr(row, "(Debug on)") && app.debug_picks);
                launcher(&app, false);
                assert(was ? !snx_write_file(ini, was, size) : !remove(ini));
                settings_forget(&app);
                snx_menu_settings = NULL;
                list_games(&app);
                render(&app);
                game_label(&app, &app.games, other, row, sizeof row);
                assert(!strstr(row, "(Debug on)") && !app.debug_picks);
                focus_game(&app, "folder game");
                free(debug);
                free(was);
                free(dir);
                free(ini);
            }
            /* M on it, a game with no update, DLC, mod or cheat: its box all the same, for its Add rows, its mods and
               cheats folders those of its data (add_mod adds to it); ESC closes it, nothing run */
            {
                size_t index = app.browser.page * app.browser.page_size + app.browser.selected - app.browser.head;
                char *mods = snx_path_join(data, "mods"), *cheats = snx_path_join(data, "cheats");
                int nx, ny;
                assert(!app.games.addons[index]);
                key(&app, SDLK_M);
                assert(app.addons_shown && !app.menu.running && !app.addons.count && !app.mods_count &&
                       !app.cheats_count && !strcmp(app.addons_title, app.games.shown[index]) &&
                       !strcmp(app.addons_game, app.games.paths[index]));
                assert(same_path(app.mods_folder, mods) && same_path(app.cheats_folder, cheats));
                render(&app);
                assert(find_text(&app, app.games.shown[index], rgb(app.theme.ink), 1, &nx, &ny) ||
                       find_text(&app, app.games.shown[index], rgb(app.theme.page), 1, &nx, &ny));
                assert(find_text(&app, "Add Mod", yellow, 1, &nx, &ny)); /* the first row, in focus */
                add_mod(&app, temporary);
                assert(!app.addons_shown && !app.menu.running && app.menu.view == GUI_LOAD_GAMES);
                key(&app, SDLK_M);
                assert(app.addons_shown && !app.mods_count);
                add_cheats(&app, temporary);
                key(&app, SDLK_M);
                assert(app.addons_shown && !app.mods_count && !app.cheats_count);
                key(&app, SDLK_ESCAPE);
                assert(!app.addons_shown && !app.menu.running && app.menu.view == GUI_LOAD_GAMES);
                { /* and on its row of the recent games: the same box, named as the run named it */
                    size_t selected = app.browser.selected, page = app.browser.page;
                    GuiRecent was = app.recent;
                    char* absolute = snx_absolute(program);
                    assert(!gui_recent_add(&app.recent, absolute, GUI_SWITCH, 1));
                    snprintf(app.recent.game[0].name, sizeof app.recent.game[0].name, "%s", "Folder Quest 2.0");
                    recent_changed(&app);
                    assert(recent_shown(&app) && app.browser.head);
                    app.browser.selected = 0; /* the newest */
                    key(&app, SDLK_M);
                    assert(app.addons_shown && !app.addons.count && !app.mods_count && !app.cheats_count &&
                           !strcmp(app.addons_title, "Folder Quest 2.0") && !strcmp(app.addons_game, absolute));
                    key(&app, SDLK_ESCAPE);
                    assert(!app.addons_shown && !app.menu.running);
                    app.recent = was;
                    recent_changed(&app);
                    app.browser.selected = selected;
                    app.browser.page = page;
                    free(absolute);
                }
                free(mods);
                free(cheats);
            }
            launcher(&app, true);
            key(&app, SDLK_R);
            assert(!app.menu.running && !app.recompile && gui_queue_running(&app.queue) == 0 &&
                   app.queue.job[0].native);
            job_done(&app, 0);
            assert(gui_steps_last(&app.ended_steps, "BUILD") && !gui_steps_last(&app.ended_steps, "LAUNCH"));
            launcher(&app, false);
            app.menu.selected = item;
            assert(!snx_remove_tree(cia));
            free(cia);
        }
        /* a game's save states (S on Games): a row each, the newest first, the focused one's picture beside the box;
           one this build can't load in the failure colour, Enter saying why; Del deletes one, asked first; Enter
           plays the game's program with --state_load= of the state */
        {
            char *own = snx_path_join(app.games_dir, "0100ABCD12350000"), *program, *states, *file[3], *kept, *name;
            static uint8_t thumb[320 * 180 * 4];
            static const uint64_t created[3] = {1000, 2000, 500};
            static const char* const slots[3] = {"1", "auto", "2"};
            GuiStatesBoxes d;
            SDL_Color c;
            size_t size;
            int i, k, nx, ny;
#ifdef _WIN32
            program = snx_path_join(own, "0100ABCD12350000.exe");
#else
            program = snx_path_join(own, "0100ABCD12350000");
#endif
            states = snx_path_join(own, "states");
            assert(!snx_mkdirs(states));
            assert(!snx_copy_file(self, program) && !snx_make_executable(program));
            for (k = 0; k < 320 * 180; k++) { /* a red picture */
                thumb[k * 4] = 255;
                thumb[k * 4 + 3] = 255;
            }
            for (i = 0; i < 3; i++) {
                SnxStateHeader h;
                uint8_t* bytes = (uint8_t*)calloc(1, SNX_STATE_HEADER + sizeof thumb);
                memset(&h, 0, sizeof h);
                h.format = SNX_STATE_FORMAT;
                h.abi = i == 2 ? SNX_ABI + 1 : SNX_ABI; /* slot 2's from another recompiled code */
                h.title_id = 0x0100ABCD12350000ull;
                h.created = created[i];
                h.guest_ns = 3723000000000ull; /* 1:02:03 */
                h.thumb_w = 320;
                h.thumb_h = 180;
                assert(bytes);
                snx_state_header_write(bytes, &h, 0, 0);
                memcpy(bytes + SNX_STATE_HEADER, thumb, sizeof thumb);
                name = snx_format("0100ABCD12350000.%s.snxstate", slots[i]);
                file[i] = name ? snx_path_join(states, name) : NULL; /* the system's separator, as the box joins */
                free(name);
                assert(file[i]);
                assert(!snx_write_file(file[i], bytes, SNX_STATE_HEADER + sizeof thumb));
                free(bytes);
            }
            app.menu.selected = GUI_SWITCH;
            go_tab(&app, GUI_TAB_GAMES);
            focus_game(&app, "0100ABCD12350000");
            render(&app);
            {
                char said[512];
                assert(strstr(shown_keys(&app, said, sizeof said), "S: save states.")); /* Games' foot says S */
            }
            key(&app, SDLK_S);
            assert(app.states_shown && app.states_count == 3 && !app.menu.running);
            assert(strstr(app.states[0].label, "Auto ") && strstr(app.states[1].label, "Slot 1 ") &&
                   strstr(app.states[2].label, "Slot 2 ") && strstr(app.states[0].label, "1:02:03"));
            assert(!app.states[0].why[0] && !app.states[1].why[0] && strstr(app.states[2].why, "SNX_ABI"));
            { /* a pad's X opens it too, and the pad's foot says so */
                char said[512];
                key(&app, SDLK_ESCAPE);
                assert(!app.states_shown);
                app.pad_driven = true;
                assert(strstr(shown_keys(&app, said, sizeof said), "X: save states."));
                { /* X as pad_menu.c gives it: Del from the pads' own keyboard */
                    SDL_Event e = {0};
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.key = SDLK_DELETE;
                    e.key.which = GUI_PAD_KEYBOARD;
                    event(&app, &e);
                }
                assert(app.states_shown && app.states_count == 3 && !app.confirm.shown);
                app.pad_driven = false;
            }
            render(&app);
            assert(find_text(&app, "Slot 1", rgb(app.theme.ink), 1, &nx, &ny));
            states_boxes(&app, &d);
            assert(d.picture.w > 0);
            c = pixel(&app, (float)(d.picture.x + d.picture.w / 2), (float)(d.picture.y + d.picture.h / 2));
            assert(c.r > 200 && c.g < 40 && c.b < 40); /* the focused state's picture */
            /* the one this build can't load: Enter says why, nothing runs */
            key(&app, SDLK_DOWN);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(app.states_shown && !app.menu.running && strstr(app.message, "SNX_ABI"));
            /* Del asks; Back keeps it, Delete takes it */
            key(&app, SDLK_DELETE);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_STATE && strstr(app.ask_line, "Slot 2"));
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && app.states_shown && snx_is_file(file[2]));
            key(&app, SDLK_DELETE);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && app.states_shown && app.states_count == 2 && !snx_is_file(file[2]));
            assert(app.states_focus == 1);
            /* Enter on slot 1: the game's program, loading it */
            launcher(&app, true);
            key(&app, SDLK_RETURN);
            assert(!app.states_shown && app.menu.running && !app.state_load);
            {
                char* ran = snx_absolute(program);
                assert(ran && !strcmp(app.file, ran));
                free(ran);
            }
            finish(&app, 0);
            launcher(&app, false);
            kept = (char*)snx_read_file(app.run_log, &size);
            {
                char* option = snx_format("--state_load=0:%s", file[0]);
                assert(kept && option && strstr(kept, option));
                free(option);
            }
            free(kept);
            /* a game without states: S says so, no box */
            for (i = 0; i < 2; i++) assert(!remove(file[i]));
            go_tab(&app, GUI_TAB_GAMES);
            focus_game(&app, "0100ABCD12350000");
            key(&app, SDLK_S);
            assert(!app.states_shown && strstr(app.message, "no save states"));
            for (i = 0; i < 3; i++) free(file[i]);
            assert(!snx_remove_tree(own));
            free(own);
            free(program);
            free(states);
        }
        /* a game with two versions of its update and a DLC beside it: opening it shows its updates as a choice of one,
           the newest first, and No update, its DLC as a box to check, as the game's addons= has them; then Save and
           Play. Back keeps nothing. Save keeps the picks without running anything; Play keeps them and plays the
           picked update's program through the driver. R opens the box too, its last row Recompile */
        {
            static unsigned char nacp[0x4000];
            char *own = snx_path_join(app.games_dir, "0100ABCD12340000"), *base, *newer, *older, *file, *ini, *checked;
            char* dirs[4];
            int nx, ny, i;
#ifdef _WIN32
            base = snx_path_join(own, "0100ABCD12340000.exe");
            newer = snx_path_join(own, "0100ABCD12340000_0100ABCD12340800v65536.exe");
            older = snx_path_join(own, "0100ABCD12340000_0100ABCD12340800v0.exe");
#else
            base = snx_path_join(own, "0100ABCD12340000");
            newer = snx_path_join(own, "0100ABCD12340000_0100ABCD12340800v65536");
            older = snx_path_join(own, "0100ABCD12340000_0100ABCD12340800v0");
#endif
            dirs[0] = snx_path_join(own, "0100ABCD12340000_data");
            dirs[1] = snx_path_join(own, "0100ABCD12340000_0100ABCD12340800v65536_data");
            dirs[2] = snx_path_join(own, "0100ABCD12340000_0100ABCD12340800v0_data");
            dirs[3] = snx_path_join(own, "0100ABCD12340000_0100ABCD12341001_data");
            for (i = 0; i < 4; i++) assert(!snx_mkdirs(dirs[i]));
            for (i = 1; i < 3; i++) { /* each version's own control.nacp, its version shown */
                memcpy(nacp + 0x3060, i == 1 ? "1.2.0" : "1.0.0", 5);
                file = snx_path_join(dirs[i], "control.nacp");
                assert(!snx_write_file(file, nacp, sizeof nacp));
                free(file);
            }
            assert(!snx_copy_file(self, base) && !snx_make_executable(base));
            assert(!snx_copy_file(self, newer) && !snx_make_executable(newer));
            assert(!snx_copy_file(self, older) && !snx_make_executable(older));
            stamp_here(own, "0100ABCD12340000"); /* the game and both updates as this build made them */
            stamp_here(own, "0100ABCD12340000_0100ABCD12340800v65536");
            stamp_here(own, "0100ABCD12340000_0100ABCD12340800v0");
            ini = snx_game_ini(own, "0100ABCD12340000");
            assert(!snx_ini_record(ini, "addons", "0100ABCD12341001")); /* the DLC checked, no update */
            /* two mods in its data folder's mods/, the second left out; a file there is no mod */
            for (i = 0; i < 2; i++) {
                file = snx_format("%s/mods/%s", dirs[0], i ? "B mod/exefs" : "A mod/romfs");
                assert(!snx_mkdirs(file));
                free(file);
            }
            file = snx_format("%s/mods/readme.txt", dirs[0]);
            assert(!snx_write_file(file, "no mod", 6));
            free(file);
            assert(!snx_ini_record(ini, "mods_off", "B mod"));
            /* three cheats in its data folder's cheats/, the second left out; the third has a | in its name, which
               cheats_off= cannot name: it always runs */
            file = snx_format("%s/cheats", dirs[0]);
            assert(!snx_mkdirs(file));
            free(file);
            file = snx_format("%s/cheats/0123456789ABCDEF.txt", dirs[0]);
            assert(!snx_write_file(file, "[60 FPS]\n04000000 0 1\n[Infinite HP]\n04000000 4 2\n[Speed x2|x4]\n",
                                   strlen("[60 FPS]\n04000000 0 1\n[Infinite HP]\n04000000 4 2\n[Speed x2|x4]\n")));
            free(file);
            assert(!snx_ini_record(ini, "cheats_off", "Infinite HP"));
            app.menu.selected = GUI_SWITCH;
            go_tab(&app, GUI_TAB_GAMES);
            focus_game(&app, "0100ABCD12340000");
            assert(
                app.games.addons[app.browser.page * app.browser.page_size + app.browser.selected - app.browser.head] ==
                8);
            key(&app, SDLK_RETURN);
            /* rows: Update 1.2.0, Update 1.0.0, No update, DLC 1, A mod, B mod, 60 FPS, Infinite HP, Speed x2|x4, Add
               Mod, Open Mods Folder, Add Cheats, Open Cheats Folder, Save, Play */
            assert(app.addons_shown && !app.menu.running && app.addons.count == 3 && app.mods_count == 2 &&
                   app.cheats_count == 3 && addons_rows(&app) == 15);
            assert(addons_row(&app, 9) == ROW_ADD_MOD && addons_row(&app, 10) == ROW_MODS_FOLDER &&
                   addons_row(&app, 11) == ROW_ADD_CHEATS && addons_row(&app, 12) == ROW_CHEATS_FOLDER &&
                   addons_row(&app, 13) == ROW_SAVE && addons_row(&app, 14) == ROW_PLAY);
            build_ids(&app, dirs);
            assert(!app.addons_checked[0] && !app.addons_checked[1] && app.addons_checked[2]);
            assert(app.mods_on[0] && !app.mods_on[1] && !strcmp(app.mods[0], "A mod"));
            assert(app.cheats_on[0] && !app.cheats_on[1] && app.cheats_on[2] && !strcmp(app.cheats[1], "Infinite HP"));
            render(&app);
            assert(find_text(&app, "[x] Mod A mod", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "[ ] Mod B mod", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "Add Mod", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "Open Mods Folder", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "Add Cheats", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "[x] Cheat 60 FPS", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "[ ] Cheat Infinite HP", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "Open Cheats Folder", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "( ) Update 1.2.0", rgb(app.theme.ink), 1, &nx, &ny) ||
                   find_text(&app, "( ) Update 1.2.0", yellow, 1, &nx, &ny));
            assert(find_text(&app, "(*) No update", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "[x] DLC 1", rgb(app.theme.ink), 1, &nx, &ny));
            { /* the rows as the menu's (ui.txt's scene Addons), 2 pixels apart; the pointer over one its focus, a click
                 presses it; a click off the box closes it, nothing kept */
                GuiAddonsBoxes d;
                GuiRect dlc, mod, first;
                size_t top;
                int s = app.scale;
                addons_boxes(&app, &d);
                top = d.lists[UI_ADDONS_ROWS].first;
                first = ui_row_box(&app, ui_addons_row(d.rects, s, UI_ADDONS_ROWS, 0));
                dlc = ui_row_box(&app, ui_addons_row(d.rects, s, UI_ADDONS_ROWS, 3 - top));
                mod = ui_row_box(&app, ui_addons_row(d.rects, s, UI_ADDONS_ROWS, 4 - top));
                assert(!top && mod.y - dlc.y == item_h(&app) + 2 * s && dlc.h == item_h(&app));
                { /* its text past the marker, as the menu's rows */
                    int x, y;
                    assert(find_text(&app, "[x] DLC 1", rgb(app.theme.ink), 1, &x, &y));
                    assert(x == dlc.x + cell_w(&app) / 2 + gui_text_cols(app.theme.marker) * cell_w(&app));
                }
                { /* rows past the screen's height: the box kept on it, the rows in it */
                    GuiRect box, at = over_rows(&app, 20, 1 << 20, &box);
                    int w, h;
                    area(&app, &w, &h);
                    assert(box.y >= 0 && box.y + box.h <= h && at.y + at.h <= box.y + box.h && at.h > 0);
                }
                point(&app, dlc, false);
                assert(app.addons_focus == 3 && app.addons_checked[2]);
                render(&app);
                assert(find_text(&app, "[x] DLC 1", yellow, 1, &nx, &ny)); /* drawn in focus */
                point(&app, dlc, true);
                assert(app.addons_shown && !app.addons_checked[2]);
                point(&app, dlc, true);
                assert(app.addons_checked[2]);
                point(&app, first, false);
                assert(app.addons_focus == 0);
                mouse_px(&app, d.box.x + d.box.w / 2, d.box.y + 2, true); /* its head: nothing */
                assert(app.addons_shown && !app.addons_focus);
                mouse_px(&app, d.box.x - 4, d.box.y + d.box.h / 2, true);
                assert(!app.addons_shown && !app.menu.running);
                key(&app, SDLK_RETURN);
                assert(app.addons_shown && !app.addons_focus && app.addons_checked[2] && !app.addons_checked[0]);
            }
            key(&app, SDLK_RETURN); /* Update 1.2.0 picked */
            assert(app.addons_checked[0] && !app.addons_checked[1]);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN); /* Update 1.0.0 picked, 1.2.0 not: one at a time */
            assert(!app.addons_checked[0] && app.addons_checked[1] && app.addons_checked[2]);
            key(&app, SDLK_ESCAPE); /* nothing kept */
            assert(!app.addons_shown && !app.menu.running);
            checked = snx_ini_value(ini, "addons");
            assert(checked && !strcmp(checked, "0100ABCD12341001"));
            free(checked);
            /* Save: the older update picked and kept, the first mod left out and the second played, nothing run */
            key(&app, SDLK_RETURN);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            for (i = 0; i < 3; i++) key(&app, SDLK_DOWN);
            assert(app.addons_focus == 4);
            key(&app, SDLK_RETURN);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(!app.mods_on[0] && app.mods_on[1]);
            /* and 60 FPS left out, Infinite HP run; Speed x2|x4 says why it stays */
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            key(&app, SDLK_DOWN);
            app.message[0] = 0;
            key(&app, SDLK_RETURN);
            assert(!app.cheats_on[0] && app.cheats_on[1] && app.addons_focus == 8 &&
                   !strcmp(app.message,
                           "\"Speed x2|x4\" always runs: a | in its name, which cheats_off= puts between two"));
            for (i = 0; i < 5; i++) key(&app, SDLK_DOWN);
            assert(app.addons_focus == 13);
            key(&app, SDLK_RETURN);
            assert(!app.addons_shown && !app.menu.running && strstr(app.message, "kept"));
            checked = snx_ini_value(ini, "addons");
            assert(checked && !strcmp(checked, "0100ABCD12340800v0,0100ABCD12341001"));
            free(checked);
            checked = snx_ini_value(ini, "mods_off");
            assert(checked && !strcmp(checked, "A mod"));
            free(checked);
            checked = snx_ini_value(ini, "cheats_off");
            assert(checked && !strcmp(checked, "60 FPS"));
            free(checked);
            /* Play: the older update's program run, as kept */
            key(&app, SDLK_RETURN);
            assert(app.addons_shown && app.addons_checked[1] && app.addons_checked[2]);
            key(&app, SDLK_UP); /* Play, the last row */
            launcher(&app, true);
            key(&app, SDLK_RETURN);
            assert(!app.addons_shown && app.menu.running && app.native && !app.recompile);
            {
                char* ran = snx_absolute(older);
                assert(ran && !strcmp(app.file, ran));
                free(ran);
            }
            finish(&app, 0);
            leave_run(&app);
            /* R: No update picked, the game's own program recompiled, the DLC kept */
            go_tab(&app, GUI_TAB_GAMES);
            focus_game(&app, "0100ABCD12340000");
            key(&app, SDLK_R);
            assert(app.addons_shown && app.addons_recompile && !app.recompile);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            for (i = 0; i < 12; i++) key(&app, SDLK_DOWN);
            assert(app.addons_focus == 14);
            key(&app, SDLK_RETURN);
            assert(!app.addons_shown && !app.menu.running && !app.recompile && gui_queue_running(&app.queue) == 0);
            {
                char* ran = snx_absolute(base);
                assert(ran && !strcmp(gui_queue_file(&app.queue, 0), ran));
                free(ran);
            }
            checked = snx_ini_value(ini, "addons");
            assert(checked && !strcmp(checked, "0100ABCD12341001"));
            free(checked);
            job_done(&app, 0);
            launcher(&app, false);
            assert(!snx_remove_tree(own));
            free(own);
            free(base);
            free(newer);
            free(older);
            for (i = 0; i < 4; i++) free(dirs[i]);
            free(ini);
        }
        assert(!snx_remove_tree(folder));
        free(program);
        free(data);
        free(folder);
    }
    go_tab(&app, GUI_TAB_GAMES); /* back from the run on the menu, the focus on the first game */
    char* removed = snx_path_join(app.games_dir, app.games.names[0]);
    assert(!snx_remove_tree(removed));
    free(removed);
    key(&app, SDLK_RETURN);
    assert(!app.menu.running && app.menu.view == GUI_LOAD_GAMES && app.message[0]);
    point(&app, game_box(&app, 1, false), false);
    render(&app);
    shows(&app, app.games.names[0], pink);
    shows(&app, app.games.names[1], yellow);
    key(&app, SDLK_ESCAPE);
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.games.count == 34);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE && !app.confirm.shown);
    /* Compact and spacious layouts share the renderer's mouse hit coordinates. UI scale Auto keeps the letters of
       the window it opened in (1280x800: 2x), however the window is resized, and the window keeps 16 rows of them */
    const int sizes[][4] = {{2560, 1440, 45, 160}, {1280, 960, 30, 80}, {800, 480, 16, 50}, {1280, 800, 25, 80}};
    assert(app.auto_scale == 2);
    /* Auto is twice the display's scale, as far as 16 rows fit on a computer: 100%, 125%, 150% and 200%; a phone keeps
       20, so the Poco F8 (408 dpi, 1156 rows of pixels) and the Pixel 9 (420 dpi, 1080) get 3x, with 24 and 22 rows */
    assert(pick_auto_scale(1.0f, 800, 16) == 2 && pick_auto_scale(1.25f, 1000, 16) == 2);
    assert(pick_auto_scale(1.5f, 1200, 16) == 3 && pick_auto_scale(2.0f, 1600, 16) == 4);
    assert(pick_auto_scale(408.0f / 160, 1156, 20) == 3 && pick_auto_scale(420.0f / 160, 1080, 20) == 3);
    assert(pick_auto_scale(408.0f / 160, 1156, 16) == 4);
    assert(pick_auto_scale(1.0f, 480, 16) == 1 && pick_auto_scale(0.5f, 800, 16) == 1 &&
           pick_auto_scale(3.0f, 2400, 16) == 6);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert(SDL_SetWindowSize(app.window, sizes[i][0], sizes[i][1]));
        SDL_SyncWindow(app.window);
        render(&app);
        assert(app.scale == 2 && app.rows == sizes[i][2] && app.cols == sizes[i][3]);
        app.menu.selected = app.rows == 16 ? GUI_LOG : GUI_SWITCH; /* 16 rows: the list scrolls to its focus */
        render(&app);
        point(&app, entry_box(&app, GUI_LOG), false);
        assert(app.menu.selected == GUI_LOG && !app.menu.text_size);
        app.clock = 1; /* a description too long for its box rests before it slides */
        render(&app);
        shows(&app, "- " ICON_SHARE_2 " Share Log", yellow);
        shows_half(&app, "Saves the last run", normal);
        app.clock = 0;
        {
            GuiMenuBoxes m;
            menu_boxes(&app, &m);
            const UiList* l = &m.lists[UI_MENU_ENTRIES];
            assert(l->focus >= l->first && l->focus < l->first + ui_menu_shown(m.rects, app.scale, UI_MENU_ENTRIES));
        }
    }
    /* a text size too big for the window (800x1024 at 4x: the foot's keys over more rows): the list scrolls as the focus goes down, then back, an arrow where boxes
       are hidden, and a size that would leave fewer than 16 rows falls back */
    {
        GuiMenuBoxes m;
        SDL_Color soft = rgb(app.theme.soft);
        int shown;
        /* Install Keys blinks, no keys being in use: the clock on a half where it has its usual look, so an
           arrow in its row is drawn soft wherever the row lands */
        assert(app.theme.blink_ms);
        app.clock = app.theme.blink_ms;
        assert(SDL_SetWindowSize(app.window, 800, 1024));
        SDL_SyncWindow(app.window);
        app.menu.text_size = GUI_TEXT_MAX;
        app.menu.selected = GUI_SWITCH;
        /* too narrow for the side box at that size: the list alone, its whole height (ui.txt's Screen hides the side
           box below 45 columns) */
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.place.side.h && !m.place.status.h && !m.place.stacked && m.place.list.w == m.place.head.w);
        /* the side box under the list (side_box = on), the list too short for the menu: it scrolls */
        app.side_box = GUI_SIDE_ON;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.place.stacked);
        shown = (int)ui_menu_shown(m.rects, app.scale, UI_MENU_ENTRIES);
        assert(app.scale == GUI_TEXT_MAX && app.rows == 16 && shown < GUI_ITEM_COUNT &&
               !m.lists[UI_MENU_ENTRIES].first);
        hides(&app, "\x1e", soft);
        shows(&app, "\x1f", soft);           /* items below the last row */
        assert(shown + 2 <= GUI_ITEM_COUNT); /* room to hide rows above and below the ones shown */
        app.clock = 2 * app.theme.blink_ms;  /* the arrow as the last row's text: page on a failing one, else soft */
        render(&app);
        shows(&app, "\x1f",
              rgb(ui_menu_entries_attr_cb(&app, shown - 1) & UI_ATTR_FAILURE ? app.theme.page : app.theme.soft));
        app.clock = app.theme.blink_ms;
        /* ui.txt's scroll, center: each focus the keys put in the middle of the rows shown, the ends where the list
           has no more to move */
        app.pointed = false;
        for (int at = 0; at < GUI_ITEM_COUNT; ++at) {
            int want = SDL_clamp(at - (shown - 1) / 2, 0, GUI_ITEM_COUNT - shown);
            app.menu.selected = at;
            menu_boxes(&app, &m);
            assert((int)m.lists[UI_MENU_ENTRIES].first == want);
        }
        app.menu.selected = (shown - 1) / 2 + 1;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.lists[UI_MENU_ENTRIES].first == 1 && 1 + shown < GUI_ITEM_COUNT); /* hidden above and below */
        shows(&app, "\x1e", soft);
        shows(&app, "\x1f", soft);
        app.menu.selected = GUI_EXIT;
        render(&app);
        menu_boxes(&app, &m);
        assert((int)m.lists[UI_MENU_ENTRIES].first + shown == GUI_ITEM_COUNT && m.lists[UI_MENU_ENTRIES].first > 0);
        shows(&app, "\x1e", soft);
        hides(&app, "\x1f", soft);
        app.menu.selected = GUI_SWITCH;
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.lists[UI_MENU_ENTRIES].first);
        app.clock = 0;
        /* the pointer on a row that shows moves nothing, where a key puts the focus in the middle: the last row shown
           hovered, then Up and Down back to it */
        point(&app, menu_entry_box(&app, &m, shown - 1), false);
        menu_boxes(&app, &m);
        assert(app.menu.selected == shown - 1 && !m.lists[UI_MENU_ENTRIES].first);
        key(&app, SDLK_UP);
        key(&app, SDLK_DOWN);
        menu_boxes(&app, &m);
        assert(app.menu.selected == shown - 1 && (int)m.lists[UI_MENU_ENTRIES].first ==
                                                     SDL_clamp(shown - 1 - (shown - 1) / 2, 1, GUI_ITEM_COUNT - shown));
        { /* scrolled, the top row hovered: the list stays where it was last shown */
            size_t top = m.lists[UI_MENU_ENTRIES].first;
            point(&app, menu_entry_box(&app, &m, (int)top), false);
            menu_boxes(&app, &m);
            assert(app.menu.selected == (int)top && m.lists[UI_MENU_ENTRIES].first == top);
        }
        app.menu.selected = GUI_SWITCH;
        render(&app);
        /* a drag down moves the focus down row by row while the finger goes, not all at once when it lifts */
        {
            SDL_Event e = {0};
            float row = drag_row(&app); /* a row of the menu's */
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            e.type = SDL_EVENT_FINGER_DOWN;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 1.5f * row;
            event(&app, &e);
            assert(app.menu.selected == GUI_LINUX);
            e.tfinger.y = 0.2f + 3.2f * row;
            event(&app, &e);
            assert(app.menu.selected == GUI_VITA);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            assert(app.menu.selected == GUI_VITA && !app.finger);
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        app.side_box = GUI_SIDE_AUTO;
        assert(SDL_SetWindowSize(app.window, 1280, 1024)); /* back to the window the checks below were made for */
        SDL_SyncWindow(app.window);
        render(&app);
        /* its rows all fitting, the focus follows a drag too, a row of the menu's, not of text, each time */
        {
            SDL_Event e = {0};
            GuiMenuBoxes all;
            int text_size = app.menu.text_size;
            float row;
            app.menu.text_size = 2; /* a row past the 5% of the window a drag starts at */
            render(&app);
            menu_boxes(&app, &all);
            assert(ui_menu_shown(all.rects, app.scale, UI_MENU_ENTRIES) >= GUI_ITEM_COUNT);
            row = drag_row(&app);
            assert(row > 1.0f / (float)app.rows);
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            e.type = SDL_EVENT_FINGER_DOWN;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 2.9f * row; /* 2 of the menu's rows, which 3 of text would make 3 */
            event(&app, &e);
            assert(app.menu.selected == GUI_3DS);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            assert(app.menu.selected == GUI_3DS && !app.finger);
            /* on the Settings a row of the list the keys move in: the groups', then an open group's */
            open_settings(&app);
            for (int open = 0; open < 2; ++open) {
                GuiFormBoxes f;
                GuiRect first, second;
                int width, height;
                render(&app);
                form_boxes(&app, &f);
                area(&app, &width, &height);
                first = form_row_box(&app, &f, open, 0);
                second = form_row_box(&app, &f, open, 1);
                row = drag_row(&app);
                assert(row == (float)(second.y - first.y) / (float)height && row > 1.0f / (float)app.rows);
                assert(app.editor.row == 0 && gui_editor_rows(&app.editor) > 3);
                e.type = SDL_EVENT_FINGER_DOWN;
                e.tfinger.y = 0.2f;
                event(&app, &e);
                e.type = SDL_EVENT_FINGER_MOTION;
                e.tfinger.y = 0.2f + 2.9f * row; /* 2 of its rows, which 3 of text would make 3 */
                event(&app, &e);
                assert(app.editor.row == 2);
                e.type = SDL_EVENT_FINGER_UP;
                event(&app, &e);
                assert(app.editor.row == 2 && !app.finger);
                if (!open) {
                    for (app.editor.row = 1; app.editor.row < app.groups && app.group_rows[app.editor.row] < 4;
                         ++app.editor.row) {}
                    assert(app.editor.row < app.groups);
                    key(&app, SDLK_RETURN); /* a group of 4 rows or more */
                    assert(app.form == GUI_FORM_GROUP);
                }
            }
            /* a group open, wide enough for the side box (3x): the groups beside its rows, the open one in the
               focus's look. Below 45 columns its rows take the whole column (ui.txt's Screen); a list too long for
               its column follows its focus in tests/gui_ui.c */
            app.menu.text_size = GUI_TEXT_MAX - 1;
            key(&app, SDLK_ESCAPE);
            app.editor.row = app.groups - 1;
            key(&app, SDLK_RETURN);
            assert(app.editing && app.form != GUI_FORM_SETTINGS && app.group == app.groups - 1);
            render(&app);
            {
                GuiFormBoxes f;
                GuiRect b;
                form_boxes(&app, &f);
                assert(f.place.side.w && form_shown(&app, &f, false) >= (size_t)app.groups && !form_first(&f, false));
                b = form_row_box(&app, &f, false, app.groups - 1);
                assert(b.h && same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), rgb(app.theme.focus)));
            }
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_ESCAPE);
            assert(!app.editing);
            app.menu.text_size = text_size;
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        /* the wheel too: a notch is a row, down three and back up */
        {
            SDL_Event e = {0};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.integer_y = -3;
            event(&app, &e);
            assert(app.menu.selected == GUI_VITA);
            e.wheel.integer_y = 3;
            event(&app, &e);
            assert(app.menu.selected == GUI_SWITCH);
            /* both stop at the menu's ends, where the keys go round */
            app.menu.selected = GUI_EXIT;
            e.wheel.integer_y = -2;
            event(&app, &e);
            assert(app.menu.selected == GUI_EXIT);
            app.menu.selected = 0;
            e.wheel.integer_y = 2;
            event(&app, &e);
            assert(app.menu.selected == 0);
            app.menu.selected = GUI_EXIT;
            e.type = SDL_EVENT_FINGER_DOWN;
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 2.5f / (float)app.rows;
            event(&app, &e);
            assert(app.menu.selected == GUI_EXIT);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        assert(SDL_SetWindowSize(app.window, 800, 480)); /* 800x512 at least: 4x would leave 8 rows */
        SDL_SyncWindow(app.window);
        render(&app);
        assert(app.scale == 2 && app.rows == 16);
        app.menu.text_size = 0;
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        render(&app);
    }
    {
        SDL_Color half;
        char row_text[160];
        int last_row;
        float x, y;
        SDL_Color page, ink, lp, li, ls, dimmed, lf;
        const GuiPalette* run;
        char* steps_input;
        Uint64 until, shown, faded, later, entered, launch_shown, first_shown, second_shown;
        float cell;
        GuiSteps kept;
        const char* detail;
        const char* live;
        const char* wide;
        const char* wide_name;
        int i;
        /* Themes: the menu item goes through the built-ins, then each themes/<id>/theme.ini */
        char* themes = snx_path_join(temporary, "themes");
        char* custom = snx_path_join(themes, "custom");
        char* custom_ini = snx_path_join(custom, "theme.ini");
        const char* ini = "[theme]\nbase = yorha\nname = Custom test\n[colors]\npage = #102030\nink = not a colour\n"
                          "[head]\nmenu = TEST  /  MENU\n[body]\nmarker = \"> \"\n";
        assert(!snx_mkdirs(custom) && !snx_write_file(custom_ini, ini, strlen(ini)));
        free(app.themes_dirs[0]);
        app.themes_dirs[0] = snx_strdup(themes);
        assert(!strcmp(app.theme.id, "default"));
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "yorha") && !app.editing && app.theme.panels == GUI_PANELS_FILL);
        app.menu.selected = GUI_SETTINGS;
        render(&app);
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        shows_big(&app, "SCARLETNX  /  MENU", ink, 2);        /* its head plain, as NieR's titles */
        shows(&app, "\xfe " ICON_SETTINGS " Settings", page); /* the focused box on a band */
        shows(&app, "\xfe " ICON_LOG_OUT " Exit", ink);
        shows_half(&app, "Working folder: ", ink);
        {
            GuiMenuBoxes
                m; /* the foot on a band of ink, the keys' box's bar in the page's colour, in the foot's size */
            menu_boxes(&app, &m);
            assert(same_colour(pixel(&app, (float)(m.place.keys.x + m.place.keys.w - 2), (float)(m.place.keys.y + 1)),
                               ink));
            assert(same_colour(pixel(&app, (float)(m.place.keys.x + 3 * foot_scale(&app)),
                                     (float)(m.place.keys.y + m.place.keys.h / 2)),
                               page));
        }
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "custom") && !strcmp(app.theme.name, "Custom test"));
        assert(app.theme.panels == GUI_PANELS_FILL); /* its base's boxes */
        { /* its ink, no colour, said with the base's it keeps and the file */
            const GuiTheme* y = gui_theme_builtin("yorha");
            char want[600];
            snprintf(want, sizeof want,
                     "%s: [colors] ink = not a colour is not a color as #RRGGBB: it keeps #%02X%02X%02X", custom_ini,
                     y->ink.r, y->ink.g, y->ink.b);
            assert(!strcmp(app.editor.note, want) && !app.editing);
            app.message[0] = 0; /* as the menu opens with app.ini's theme = custom: said in its message */
            app_load(&app);
            assert(strstr(app.message, want) && !strcmp(app.theme.id, "custom"));
            app.message[0] = 0;
        }
        render(&app);
        page.r = 0x10; /* its own page; its ink, unreadable, is YoRHa's */
        page.g = 0x20;
        page.b = 0x30;
        shows_big(&app, "TEST  /  MENU", ink, 2);
        shows(&app, "> " ICON_SETTINGS " Settings", page);
        shows(&app, "> " ICON_LOG_OUT " Exit", ink);
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "default") && !strcmp(app.app_form.theme, "Default"));
        free(custom_ini);
        free(custom);
        free(themes);

        /* The theme editor: its screen in the menu's theme, a preview the menu's own drawing makes in the theme being
           edited, a typed value, the preview's switch, and Save making it the menu's theme */
        {
            int r;
            GuiFormBoxes f;
            GuiRect preview;
            SDL_Color night = {0x10, 0x20, 0x30, 255}, soft = rgb(app.theme.soft);
            char* saved = snx_format("%s/themes/mytheme/theme.ini", temporary);
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && app.form == GUI_FORM_THEME && !strcmp(app.editor.id, "mytheme") &&
                   !strcmp(app.editor.theme.id, "default"));
            render(&app);
            shows(&app, "THEME EDITOR  /  mytheme", normal);
            shows(&app, "- Save", yellow); /* its first row */
            /* more rows than the box holds: an arrow down at first, both in the middle, up at the end */
            form_boxes(&app, &f);
            assert(app.editor.row == 0 && form_shown(&app, &f, false) < (size_t)gui_editor_rows(&app.editor));
            hides(&app, "\x1e", soft);
            shows(&app, "\x1f", soft);
            app.editor.row = gui_editor_rows(&app.editor) / 2;
            render(&app);
            form_boxes(&app, &f);
            assert(form_first(&f, false) > 0);
            shows(&app, "\x1e", soft);
            shows(&app, "\x1f", soft);
            app.editor.row = gui_editor_rows(&app.editor) - 1;
            render(&app);
            shows(&app, "\x1e", soft);
            hides(&app, "\x1f", soft);
            app.editor.row = 0;
            render(&app);
            form_boxes(&app, &f);
            preview = inner(&app, f.place.side);
            preview.y += head_h(&app) + 2 * app.scale; /* under the pane's head */
            /* page = #102030, picked: Enter opens the picker over the keys, Left/Right and the pages step a channel,
               a click or a drag on a bar sets it, its last row types the colour; the preview's page takes it, the
               editor's own stays the menu's */
            for (r = 0; !gui_editor_key_of(&app.editor, r) || strcmp(gui_editor_key_of(&app.editor, r)->key, "page");
                 ++r) {}
            app.editor.row = r;
            app.editor.theme.page = (GuiRgb){0x80, 0x40, 0x20}; /* the bars' marks off their ends */
            key(&app, SDLK_RETURN);
            assert(app.editor.picking && !app.editor.typing && app.editor.channel == GUI_PICK_RED);
            {
                GuiPicker pk;
                GuiRgb was = app.editor.theme.page;
                int s = app.scale, w;
                SDL_Color c;
                render(&app);
                shows_foot(&app, "cancel", normal); /* the picker's keys */
                picker_place(&app, &pk);
                assert(pk.box.x + pk.box.w <= f.place.list.x + f.place.list.w); /* over the keys: the preview shows */
                /* each bar the colours its channel goes through, the red one's ends black and red */
                w = pk.bars[0].w - 2 * s;
                c = pixel(&app, (float)(pk.bars[0].x + s), (float)(pk.bars[0].y + pk.bars[0].h / 2));
                assert(c.r == 0 && c.g == was.g && c.b == was.b);
                c = pixel(&app, (float)(pk.bars[0].x + s + w - 1), (float)(pk.bars[0].y + pk.bars[0].h / 2));
                assert(c.r == 255 && c.g == was.g && c.b == was.b);
                key(&app, SDLK_RIGHT);
                assert(app.editor.theme.page.r == was.r + 1 && app.editor.dirty);
                key(&app, SDLK_PAGEUP);
                assert(app.editor.theme.page.r == was.r + 17);
                key(&app, SDLK_PAGEDOWN);
                key(&app, SDLK_LEFT);
                assert(app.editor.theme.page.r == was.r);
                /* the mouse: a press at the green bar's start, a drag to its end, and the lift lets it go */
                mouse_px(&app, pk.bars[1].x + s, pk.bars[1].y + 2, true);
                assert(app.editor.channel == GUI_PICK_GREEN && app.editor.theme.page.g == 0 && app.pick_press == 2);
                mouse_px(&app, pk.bars[1].x + pk.bars[1].w + 40, pk.bars[0].y, false); /* past it, on another row */
                assert(app.editor.theme.page.g == 255 && app.editor.theme.page.r == was.r);
                mouse_px(&app, pk.bars[1].x + s + (w - 1) / 2, pk.bars[1].y + 2, false); /* the middle's strip */
                assert(app.editor.theme.page.g >= 126 && app.editor.theme.page.g <= 129);
                {
                    SDL_Event up = {0};
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    event(&app, &up);
                }
                mouse_px(&app, pk.bars[1].x + s, pk.bars[1].y + 2, false);
                assert(!app.pick_press && app.editor.theme.page.g >= 126 && app.editor.theme.page.g <= 129);
                /* a finger on the blue bar's end: set, and its lift taps nothing under it */
                tap_px(&app, pk.bars[2].x + pk.bars[2].w - s - 1, pk.bars[2].y + 2);
                assert(app.editor.picking && app.editor.channel == GUI_PICK_BLUE && app.editor.theme.page.b == 255);
                assert(!app.pick_press && !app.editor.typing);
                /* ESC puts it back; the wheel does not move the rows under it */
                {
                    SDL_Event wheel = {0};
                    wheel.type = SDL_EVENT_MOUSE_WHEEL;
                    wheel.wheel.integer_y = -1;
                    event(&app, &wheel);
                    assert(app.editor.row == r && app.editor.channel == GUI_PICK_BLUE);
                }
                key(&app, SDLK_ESCAPE);
                assert(!app.editor.picking && !memcmp(&app.editor.theme.page, &was, sizeof was) && !app.editor.dirty);
                assert(app.editing && app.form == GUI_FORM_THEME && app.editor.row == r);
                /* a click on the focused row opens it again, a click on its last row types */
                key(&app, SDLK_RETURN);
                mouse_px(&app, pk.rows[GUI_PICK_HEX].x + 4, pk.rows[GUI_PICK_HEX].y + 4, true);
                assert(app.editor.typing && app.editor.channel == GUI_PICK_HEX && SDL_TextInputActive(app.window));
                for (int i = 0; i < 7; ++i) key(&app, SDLK_BACKSPACE);
                typed(&app, "#102030");
                key(&app, SDLK_RETURN);
                assert(app.editor.picking && !app.editor.typing && !SDL_TextInputActive(app.window) &&
                       app.editor.dirty);
                assert(!strcmp(app.editor.text, "#102030") && app.editor.theme.page.r == 0x10);
                render(&app);
                c = pixel(&app, (float)(pk.rows[GUI_PICK_HEX].x + 8 * s / 2 + 10 * 8 * s + 4 * s),
                          (float)(pk.rows[GUI_PICK_HEX].y + 3 * s + 8 * s));
                assert(same_colour(c, night)); /* the colour now, beside the one it had */
                /* a click off the box keeps it, and does nothing else: the preview stays the menu */
                mouse_px(&app, preview.x + preview.w / 2, preview.y + 8 * s, true);
                assert(!app.editor.picking && app.editor.preview == GUI_SCREEN_MENU && app.editor.theme.page.g == 0x20);
                { /* its row: the value, then a cell on, the colour framed in soft */
                    int x, y;
                    render(&app);
                    assert(find_text(&app, "#102030", yellow, 1, &x, &y));
                    x += 8 * 8 * s;
                    assert(same_colour(pixel(&app, (float)x, (float)(y + 8 * s)), soft));
                    assert(same_colour(pixel(&app, (float)(x + 8 * s), (float)(y + 8 * s)), night));
                    assert(!same_colour(pixel(&app, (float)(x + 16 * s), (float)(y + 8 * s)), night));
                }
                {
                    SDL_Event up = {0};
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    event(&app, &up);
                    assert(!app.pick_press);
                }
                /* a mouse's press on a bar whose lift never came: a finger after it still taps */
                key(&app, SDLK_RETURN);
                mouse_px(&app, pk.bars[0].x + pk.bars[0].w / 2, pk.bars[0].y + 2, true);
                key(&app, SDLK_ESCAPE);
                assert(!app.editor.picking && app.pick_press == 1);
                touch_box(&app, row_box(&app, r + 1, false));
                assert(app.editor.row == r + 1 && !app.pick_press);
                app.editor.row = r;
            }
            render(&app);
            { /* the icons of the menu the preview draws at 1x, 16 pixels, the editor's own rows none: the app keeps
                 them once drawn, a preview on a copy of it put back after it, and draws them from there next time */
                SDL_Texture* sheet = NULL;
                int k, kept = 0;
                for (k = 0; k < GUI_ICON_SHEETS; ++k) {
                    SDL_DestroyTexture(app.icon_sheets[k]);
                    app.icon_sheets[k] = NULL;
                    app.icon_sides[k] = 0;
                }
                render(&app);
                for (k = 0; k < GUI_ICON_SHEETS; ++k)
                    if (app.icon_sides[k] == 16 && app.icon_sheets[k]) sheet = app.icon_sheets[k], ++kept;
                assert(kept == 1);
                render(&app);
                for (k = 0; k < GUI_ICON_SHEETS; ++k) kept -= app.icon_sides[k] == 16 && app.icon_sheets[k] == sheet;
                assert(!kept);
            }
            { /* the preview's page, beside the boxes of the menu it draws */
                int w = SDL_min(preview.w, (f.info.y - f.place.side.y - 2 * 16 * app.scale) * 16 / 10);
                assert(same_colour(
                    pixel(&app, (float)(preview.x + (preview.w - w) / 2 + w / 160), (float)(preview.y + w * 5 / 16)),
                    night));
            }
            /* Ctrl+P, or a tap on the preview: a run in the theme, then the menu again; Tab, which goes to the next
               screen, keeps a theme not saved, saying so */
            key(&app, SDLK_TAB);
            assert(app.editing && app.form == GUI_FORM_THEME && !strncmp(app.editor.note, "Not saved: ", 11));
            key_with(&app, SDLK_P, SDL_KMOD_LCTRL);
            assert(app.editor.preview == GUI_SCREEN_COMPILING);
            render(&app);
            point(&app, f.place.side, true);
            assert(app.editor.preview == GUI_SCREEN_MENU);
            /* a space typed arrives as text: the key itself, Select elsewhere, does not take a text value */
            app.editor.row = 2; /* [theme] name, after Save and the folder */
            key(&app, SDLK_RETURN);
            typed(&app, "x");
            key(&app, SDLK_SPACE);
            assert(app.editor.typing);
            point(&app, row_box(&app, 3, false), false); /* the pointer moves no focus off a value being typed */
            assert(app.editor.row == 2 && app.editor.typing);
            key(&app, SDLK_ESCAPE);
            assert(!app.editor.typing && strcmp(app.editor.theme.name, "Defaultx"));
            /* a click focuses a row, a click on the focused one changes it: [theme] author, typed */
            point(&app, row_box(&app, 3, false), true);
            assert(app.editor.row == 3 && !app.editor.typing);
            point(&app, row_box(&app, 3, false), true);
            assert(app.editor.row == 3 && app.editor.typing);
            key(&app, SDLK_ESCAPE);
            assert(!app.editor.typing);
            /* ESC over changes asks first; Ctrl+S saves in themes/mytheme, and the menu's theme is it */
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.editor.leaving);
            key_with(&app, SDLK_S, SDL_KMOD_LCTRL);
            assert(snx_is_file(saved) && !app.editor.dirty && !strcmp(app.theme.id, "mytheme"));
            assert(same_colour(rgb(app.theme.page), night));
            {
                size_t size = 0;
                char* kept = (char*)snx_read_file(app.app_file, &size);
                assert(kept && strstr(kept, "\ntheme = mytheme\n")); /* the next start opens on it too */
                free(kept);
            }
            key(&app, SDLK_ESCAPE); /* nothing left unsaved: it leaves at once, back to App */
            assert(app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_EDITOR);
            leave_editor(&app);
            render(&app);
            shows(&app, "- " ICON_SHARE_2 " Share Log", normal); /* on its own page */
            assert(same_colour(pixel(&app, 2, 2), night));
            /* opened on a theme of themes/, it saves there under its own name */
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && !strcmp(app.editor.id, "mytheme") && !strcmp(app.editor.file.base, "default"));
            key(&app, SDLK_ESCAPE);
            assert(app.form == GUI_FORM_APP);
            leave_editor(&app);
            free(saved);
        }

        /* YoRHa loading: a run as steps, typed in, with the ring, the dots and the bar; the run fades in, and out */
        steps_input = snx_path_join(temporary, "option flags steps.nro");
        assert(!snx_write_file(steps_input, "fixture", 7));
        char* steps_made = made_game(&app, steps_input, "option flags steps"); /* made: it plays */
        gui_theme_load(&app.theme, (const char* const*)app.themes_dirs, 2, "yorha", NULL, 0);
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        launch(&app, snx_strdup(steps_input), false);
        play_made(&app);
        assert(app.menu.running && app.fade_ms);
        for (until = SDL_GetTicks() + 10000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) {
            pump(&app);
            render(&app);
        }
        assert(app.finished && !app.abnormal_exit && app.steps.count == 2 && screen(&app) == GUI_SCREEN_LOADING);
        run = &app.theme.run_colors;
        lp = rgb(run->page);
        li = rgb(run->ink);
        ls = rgb(run->soft);
        shown = app.steps.step[1].shown_ms;
        faded = app.fade_ms;
        later = (shown + 60000) / 1600 * 1600; /* where turning dots would show none: the "..." is the end's */
        /* entering: the menu stays and fades out over fade_out_ms, then the run fades in */
        entered = app.entering_ms;
        assert(entered && faded == entered + app.theme.fade_out_ms);
        app.clock = entered;
        render(&app);
        shows_big(&app, "SCARLETNX  /  MENU", rgb(app.theme.ink), 2);
        hides(&app, "Last output", rgb(app.theme.soft)); /* the run's output waits for the run */
        app.clock = entered + app.theme.fade_out_ms / 2;
        render(&app);
        dimmed = pixel(&app, 4, 4); /* the page, half way to black */
        assert(!same_colour(dimmed, rgb(app.theme.ink)) && !same_colour(dimmed, black));
        app.clock = entered + app.theme.fade_out_ms;
        render(&app);
        assert(same_colour(pixel(&app, 4, 4), black));
        app.theme.texture = false; /* exact backgrounds first */
        app.clock = later;
        render(&app);
        colored_text_on(&app, 1, 1, "BOOTING", li, lp);
        colored_text_on(&app, 9, 1, "OPTION FLAGS...", ls, lp);
        colored_text_on(&app, 1, 3, "RECOMPILE: OPTION FLAGS", ls, lp);
        colored_text_on(&app, 24, 3, "...OK", li, lp);
        colored_text_on(&app, 1, 4, "LAUNCH: OPTION FLAGS...OK", li, lp);
        colored_text_on(&app, 1, app.rows - 1, "Press [ESC] to return to menu", ls, lp);
        /* the run's last line as a prompt, when the theme says so: done, failed and aborting each on its own switch */
        app.theme.prompt.blink = false;
        app.theme.prompt.lines[GUI_PROMPT_DONE] = true;
        render(&app);
        shows(&app, "[ Press [ESC] to return to menu ]", ls); /* in the middle of its row */
        app.abnormal_exit = true;
        render(&app);
        colored_text_on(&app, 1, app.rows - 1, "Press [ESC] to return to menu    ", ls, lp);
        app.theme.prompt.lines[GUI_PROMPT_FAILED] = true;
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        colored_text_on(&app, app.cols - 34, app.rows - 1, "[ Press [ESC] to return to menu ]", ls, lp);
        app.abnormal_exit = app.finished = false;
        app.menu.running = app.aborting = true;
        app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
        render(&app);
        colored_text_on(&app, app.cols - 1 - (int)strlen("[ Aborting... ]"), app.rows - 1, "[ Aborting... ]",
                        rgb(run->warning), lp);
        app.menu.running = app.aborting = false;
        app.finished = true;
        app.theme.prompt = gui_theme_builtin("yorha")->prompt;
        cell = 16.0f * (float)app.scale;
        /* the menu's footer over the ESC line, in the run's colours: YoRHa's bar style, a band of its ink */
        colored_text_on(&app, 1, app.rows - 3, "RAM ", lp, li);
        colored_text_on(&app, 1, app.rows - 2, "Working folder: ", lp, li);
        app.theme.footer = GUI_STYLE_PLAIN;
        render(&app);
        colored_text_on(&app, 1, app.rows - 2, "Working folder:", ls, lp);
        app.theme.footer = GUI_STYLE_NONE;
        render(&app);
        colored_text_on(&app, 1, app.rows - 2, "    ", ls, lp);
        app.theme.footer = GUI_STYLE_BAR;
        colored_text_on(&app, app.cols - 6, app.rows - 5, "    ", li, lp); /* YoRHa has no bar unless asked */
        colored_text_on(&app, 1, app.rows - 4, "            ", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 5) + cell / 2), lp));
        app.theme.bar = true;
        render(&app);
        colored_text_on(&app, app.cols - 6, app.rows - 5, "100%", li, lp); /* above the footer */
        colored_text_on(&app, 1, app.rows - 4, "OPTION FLAGS", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 5) + cell / 2), li));
        colored_text_on(&app, 1, app.rows - 2, "Working folder: ", lp, li);
        /* typing: five characters in, the cursor after them; async, each line from when it came. The run is over, so
           a row types whole in typing_ms: 25 characters in 10 ms */
        app.theme.typing_sync = false;
        app.theme.typing_ms = 10;
        app.theme.fade_in_ms = app.theme.fade_out_ms = 0;
        app.theme.blink_ms = 0;
        launch_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = entered + 1; /* it came while the menu still showed: it types once the run is in */
        app.clock = faded + 2;
        render(&app);
        colored_text_on(&app, 1, 4, "LAUNC_", li, lp);
        app.steps.step[1].shown_ms = launch_shown;
        app.fade_ms = 0; /* in view from the start, for the lines below */
        app.clock = shown + 2;
        render(&app);
        colored_text_on(&app, 1, 4, "LAUNC_", li, lp);
        /* sync: one line at a time; the second waits for the first */
        app.theme.typing_sync = true;
        first_shown = app.steps.step[0].shown_ms;
        second_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = first_shown; /* both lines came at once */
        app.clock = first_shown + 2;
        render(&app);
        colored_text_on(&app, 1, 3, "RECOM", ls, lp); /* not the last line: soft, with the cursor typing it */
        colored_text_on(&app, 6, 3, "_", li, lp);
        colored_text_on(&app, 1, 4, "    ", li, lp);
        app.theme.typing_sync = false;
        app.steps.step[1].shown_ms = second_shown;
        /* the newest row of a step still running types a character every typing_ms; a done step's, or one a newer
           row of its step passed, whole in typing_ms; one above the screen at once, the visible ones from when they
           came: a fast log keeps the cursor on its newest lines */
        {
            static size_t total[200], typed[200];
            static Uint64 came[200];
            static int slow[200];
            GuiTheme t = *gui_theme_builtin("yorha");
            int k;
            t.typing_ms = 10;
            for (k = 0; k < 200; ++k) {
                total[k] = 50;
                came[k] = 1000;
                slow[k] = 0;
            }
            slow[199] = 1; /* 200 rows at once, the last its running step's newest; rows 180 to 199 on the screen */
            assert(t.typing_sync && t.line_in == GUI_LINE_TYPING);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 5, typed) == 180);
            assert(typed[0] == 50 && typed[179] == 50 && typed[180] == 25 && typed[181] == 0);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 19 * 10 - 1, typed) == 198);
            assert(typed[180] == 50 && typed[198] == 45 && typed[199] == 0);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 19 * 10 + 30, typed) == 199 && typed[199] == 3);
            slow[199] = 0; /* the step is done: its 20 rows on the screen take 200 ms */
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 20 * 10 - 1, typed) == 199 && typed[199] == 45);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 20 * 10, typed) == -1 && typed[199] == 50);
            slow[199] = 1;
            t.typing_sync = 0; /* async: each row from when it came, at its own speed */
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 30, typed) == 199);
            assert(typed[180] == 50 && typed[198] == 50 && typed[199] == 3);
            t.line_in = GUI_LINE_NONE;
            assert(type_rows(&t, 200, 180, total, came, slow, 1000, typed) == -1 && typed[180] == 50 &&
                   typed[199] == 50);
        }
        /* detailed: a step's lines under it, indented; collapse folds a done step's lines, never a failed one's */
        kept = app.steps;
        lf = rgb(run->failure);
        gui_steps_reset(&app.steps);
        detail = "Recompiling Game (1 modules)\n  compiled block 1/2: code_000 (1.0 s)\n"
                 "  compiled block 2/2: code_001 (1.0 s)\nBuilt games/Game\n"
                 "ScarletNX - Game\nfs: 3 files mapped\n";
        gui_steps_feed(&app.steps, detail, strlen(detail), shown);
        gui_steps_finish(&app.steps, false);
        app.clock = later;
        render(&app);
        colored_text_on(&app, 1, 3, "RECOMPILE: GAME", ls, lp);
        colored_text_on(&app, 16, 3, "...OK", li, lp);
        colored_text_on(&app, 3, 4, "compiled block 1/2: code_000 (1.0 s)", ls, lp);
        colored_text_on(&app, 3, 5, "compiled block 2/2: code_001 (1.0 s)", ls, lp);
        colored_text_on(&app, 1, 6, "BUILD: GAMES/GAME", ls, lp);
        colored_text_on(&app, 1, 7, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 8, "fs: 3 files mapped", ls, lp);
        app.theme.collapse = true;
        render(&app);
        colored_text_on(&app, 1, 4, "BUILD: GAMES/GAME", ls, lp);
        colored_text_on(&app, 1, 5, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 6, "fs: 3 files mapped", ls, lp);
        app.theme.detailed = false;
        render(&app);
        colored_text_on(&app, 1, 5, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 6, "                  ", ls, lp);
        /* a step still running keeps its lines, folded or not; the dots and the cursor stay on the step's own row */
        app.theme.detailed = true;
        gui_steps_reset(&app.steps);
        live = "Recompiling Live (1 modules)\n";
        gui_steps_feed(&app.steps, live, strlen(live), shown);
        gui_steps_feed(&app.steps, "Functions: 2\n", 13, shown + 1000);
        app.clock = shown + 1000 + 3 * app.theme.typing_ms + 1; /* a line types from when it came */
        render(&app);
        colored_text_on(&app, 3, 4, "Fun", ls, lp);
        colored_text_on(&app, 6, 4, "_         ", li, lp);
        app.fade_ms = shown + 2000; /* or once the run is in, when it came before */
        app.clock = shown + 2000 + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 3, 4, "Fun", ls, lp);
        colored_text_on(&app, 6, 4, "_         ", li, lp);
        app.fade_ms = 0;
        app.clock = later + 1200;
        render(&app);
        colored_text_on(&app, 16, 3, "..._", li, lp);
        colored_text_on(&app, 3, 4, "Functions: 2    ", ls, lp);
        /* two lines at once: the one the newer passed types whole, the newer a character every typing_ms */
        gui_steps_reset(&app.steps);
        gui_steps_feed(&app.steps, live, strlen(live), shown);
        gui_steps_feed(&app.steps, "Functions: 2\nBlocks: 3\n", 23, shown + 1000);
        app.clock = shown + 1000 + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 3, 4, "Functions: 2", ls, lp);
        colored_text_on(&app, 3, 5, "Blo", ls, lp);
        /* every line a step kept, down the screen; a row too wide for it ends at the edge */
        app.theme.collapse = app.theme.bar = false;
        gui_steps_reset(&app.steps);
        wide_name = "Recompiling A Game Whose Name Runs Far Past The Right Edge Of This Small Window (1 modules)\n";
        gui_steps_feed(&app.steps, wide_name, strlen(wide_name), shown);
        for (i = 0; i < 9; ++i) {
            char line[32];
            snprintf(line, sizeof line, "line %d\n", i);
            gui_steps_feed(&app.steps, line, strlen(line), shown);
        }
        wide = "fs: a line long enough to pass the right edge of a window eighty columns wide, and on, and on\n";
        gui_steps_feed(&app.steps, wide, strlen(wide), shown);
        gui_steps_finish(&app.steps, true);
        app.clock = later;
        render(&app);
        /* of the row's cols - 6, "RECOMPILE: ", the detail's space and "...OK" take 16 */
        snprintf(row_text, sizeof row_text, "RECOMPILE: %.*s", app.cols - 6 - 16,
                 "A GAME WHOSE NAME RUNS FAR PAST THE RIGHT EDGE OF THIS SMALL WINDOW");
        colored_text_on(&app, 1, 3, row_text, li, lp); /* the last step, in ink */
        colored_text_on(&app, app.cols - 10, 3, "...OK ", li, lp);
        colored_text_on(&app, 3, 4, "line 0", ls, lp);
        colored_text_on(&app, 3, 12, "line 8", ls, lp);
        snprintf(row_text, sizeof row_text, "%.*s...", app.cols - 8, wide);
        colored_text_on(&app, 3, 13, row_text, ls, lp);
        colored_text_on(&app, app.cols - 2, 13, " ", ls, lp);
        for (i = 9; i < 20; ++i) { /* past the bottom, the latest rows stay */
            char line[32];
            snprintf(line, sizeof line, "line %d\n", i);
            gui_steps_feed(&app.steps, line, strlen(line), shown);
        }
        render(&app);
        last_row = app.rows - 5; /* a blank row above the footer */
        colored_text_on(&app, 3, last_row, "line 19", ls, lp);
        colored_text_on(&app, 3, last_row - 1, "line 18", ls, lp);
        app.steps = kept;
        /* the page's faint lines, one pixel row in four */
        app.theme = *gui_theme_builtin("yorha");
        app.fade_ms = faded;
        app.clock = later;
        render(&app);
        x = 8.0f * (float)app.scale * (float)(app.cols - 10);
        y = cell * 10;
        assert(!same_colour(pixel(&app, x, y + 3), lp) && same_colour(pixel(&app, x, y + 2), lp));
        /* the fade in from black, over fade_in_ms */
        app.clock = faded;
        render(&app);
        assert(same_colour(pixel(&app, x, y + 2), black));
        app.clock = faded + app.theme.fade_in_ms / 2;
        render(&app);
        half = pixel(&app, x, y + 2);
        assert(!same_colour(half, black) && !same_colour(half, lp));
        /* ESC: the run fades out to black over fade_out_ms, then the menu fades in */
        app.clock = later;
        key(&app, SDLK_ESCAPE);
        assert(app.finished && app.leaving_ms == app.clock);
        app.clock += app.theme.fade_out_ms / 2;
        pump(&app);
        render(&app);
        assert(app.finished && !same_colour(pixel(&app, x, y + 2), lp));
        app.clock += app.theme.fade_out_ms / 2;
        pump(&app);
        assert(!app.finished && app.menu.view == GUI_LOAD_NONE && app.fade_ms == app.clock);
        render(&app);
        assert(same_colour(pixel(&app, x, y + 2), black));
        app.clock += app.theme.fade_in_ms;
        render(&app);
        shows_big(&app, "SCARLETNX  /  MENU", rgb(app.theme.ink), 2); /* the menus' own */
        app.clock = 0;
        app.theme = *gui_theme_builtin("default");
        launcher(&app, false);
        made_gone(&app, steps_made);
        free(steps_input);
    }
    /* The menu: each entry a box of its own, the focused one framed in the focus colour (outline) or inverted with the
       cursor in the margin (fill); beside the list what the focus is about, the status under it; at the foot the tips,
       the same or the screen's message, beside the keys. A box under the pointer takes the focus and a click selects
       it; many entries scroll, and a narrow window puts the side box under the list */
    {
        GuiMenuBoxes m;
        GuiRect b, r;
        int s, cw, focus, exit_entry;
        SDL_Color dim, page, ink;
        char* keys_folder = snx_dirname(app.keys_path);
        app.theme = *gui_theme_builtin("default");
        assert(app.theme.panels == GUI_PANELS_OUTLINE && !app.theme.cursor[0] && !app.theme.tips_box);
        /* keys in use, so Install Keys has its usual look, not the red of a start without them */
        assert(keys_folder && !snx_mkdirs(keys_folder) && !snx_write_file(app.keys_path, "header_key = 00\n", 16));
        free(keys_folder);
        app.menu.selected = GUI_KEYS;
        app.message[0] = 0;
        app.abnormal_exit = false;
        app.fade_ms = 0; /* the last run's fade in, on the clock it had */
        render(&app);
        menu_boxes(&app, &m);
        s = app.scale;
        cw = 8 * s;
        dim = rgb(app.theme.dim);
        focus = GUI_KEYS;
        exit_entry = GUI_EXIT;
        assert(focus >= 0 && (int)m.lists[UI_MENU_ENTRIES].focus == focus && exit_entry > focus && !m.place.stacked);
        b = menu_entry_box(&app, &m, focus);
        assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), yellow)); /* framed in the focus colour */
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- " ICON_KEY_ROUND " Install Keys", yellow, black);
        b = menu_entry_box(&app, &m, exit_entry);
        assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), dim)); /* the others in the dim one */
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- " ICON_LOG_OUT " Exit", normal, black);
        /* the load activated last in the active colour, the focus kept where it is */
        app.menu.activated = true;
        app.menu.active = GUI_3DS;
        render(&app);
        b = menu_entry_box(&app, &m, GUI_3DS);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- " ICON_BOX " Load 3DS game", rgb(app.theme.active),
                   black);
        app.menu.activated = false;
        render(&app);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- " ICON_BOX " Load 3DS game", normal, black);
        /* a row taller than a cell and a pixel, as Android's 40px: its text in the middle, not at its top */
        b = gui_rect(b.x, b.y, b.w, 40 * s);
        fill_rect(&app, b, app.theme.page); /* the rows it covers, gone */
        ui_draw_row(&app, b, "Exit", 0, NULL, UI_COLOR_NONE, 0, false);
        text_px_is(&app, b.x + cw / 2, b.y + (40 * s - 16 * s + s) / 2, "- Exit", normal, black); /* not b.y + s */
        { /* a row with an icon: the marker, the icon's two columns of the font, a column, then the text */
            char glyph[4], line[32];
            fill_rect(&app, b, app.theme.page);
            ui_draw_row(&app, b, "Exit", GUI_ICON_LOG_OUT, NULL, UI_COLOR_NONE, 0, false);
            snprintf(line, sizeof line, "- %s Exit", gui_icon_utf8(GUI_ICON_LOG_OUT, glyph));
            text_px_is(&app, b.x + cw / 2, b.y + (40 * s - 16 * s + s) / 2, line, normal, black);
            assert(gui_text_cols(line) == 9);
        }
        { /* a row with an image: the marker, the image as big as the head's user picture, then the text */
            SDL_Texture* red =
                SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 4, 4);
            Uint32 pixels[16];
            int side = user_side(&app, ui_row_box(&app, b).h), x = b.x + cw / 2 + gui_text_cols(app.theme.marker) * cw;
            for (int k = 0; k < 16; ++k) pixels[k] = 0xffff0000u;
            assert(red && SDL_UpdateTexture(red, NULL, pixels, 16));
            fill_rect(&app, b, app.theme.page);
            ui_draw_row(&app, b, "Exit", 0, red, UI_COLOR_NONE, 0, false);
            text_px_is(&app, b.x + cw / 2, b.y + (40 * s - 16 * s + s) / 2, "- ", normal, black);
            assert(near_colour(pixel(&app, x + side / 2.0f, ui_row_box(&app, b).y + 2 * s + side / 2.0f),
                               (SDL_Color){255, 0, 0, 255}, 8));
            text_px_is(&app, x + side + cw / 2, b.y + (40 * s - 16 * s + s) / 2, "Exit", normal, black);
            SDL_DestroyTexture(red);
        }
        /* the side box: the focus's name over a rule and the keys in use, what it does left to the tips; the status
           box under it */
        r = m.place.side;
        assert(r.x >= m.place.list.x + m.place.list.w && r.h > 0 && m.place.status.y >= r.y + r.h);
        text_px_is(&app, r.x + cw, r.y + 2 * s, "Install Keys", normal, black);
        text_px_is(&app, r.x + cw, r.y + head_h(&app) + 2 * s, "In use: ", rgb(app.theme.soft), black);
        text_px_is(&app, m.place.status.x + cw, m.place.status.y + 2 * s, "Status", normal, black);
        { /* a status too narrow for the usage: the memory and the CPU a row each, the CPU's from its row's start */
            char usage[256];
            int first, cx, cy;
            GuiRect box;
            app.usage.available = true;
            app.usage.memory_bytes = 90u << 20;
            app.usage.cpu_percent = 50;
            gui_usage_text(usage, sizeof usage, true, app.usage.memory_bytes, 50, SDL_GetNumLogicalCPUCores());
            first = (int)(strstr(usage, "  ") - usage);
            box = gui_rect(cw, cw, (first + 6) * cw, 10 * 16 * s); /* room for "  CPU" on the first row, not more */
            char *cwd_was = app.cwd, short_cwd[] = "/"; /* the usage's rows alone: a working folder of one row */
            app.cwd = short_cwd;
            SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
            SDL_RenderClear(app.renderer);
            assert(status_rows(&app, first + 6, box, box.y, 1) == status_rows(&app, first + 6, box, box.y, 0));
            assert(find_text(&app, "CPU ", rgb(app.theme.soft), 1, &cx, &cy) && cx == box.x && cy > box.y);
            assert(find_text(&app, "RAM 90 MB", rgb(app.theme.soft), 1, &cx, &cy) && cx == box.x);
            app.cwd = cwd_was;
            render(&app);
        }
        /* the tips on a row of their own under the head, what the focus is about or the screen's message, a failure's
           in its colour; the keys alone at the foot, the whole width */
        r = m.place.tips;
        assert(r.x == m.place.head.x && r.w == m.place.head.w);
        assert(r.y > m.place.tabs.y + m.place.tabs.h && r.y + r.h < m.place.list.y);
        assert(m.place.keys.x == m.place.head.x && m.place.keys.w == m.place.head.w && m.place.keys.y > r.y);
        assert(r.h == 8 * (s + foot_scale(&app)) + 4 * s); /* a row of letters a half step under the text's */
        tips_px_is(&app, r, "Installs prod.keys", normal, black);
        snprintf(app.message, sizeof app.message, "Program ended abnormally (status 7).");
        app.abnormal_exit = true;
        render(&app);
        tips_px_is(&app, r, "Program ended abnormally", red, black);
        app.message[0] = 0;
        app.abnormal_exit = false;
        /* the keys under the list and the side box, half a row of text over the window's bottom edge, each key in the
           focus colour; the default's on the page, no box */
        r = m.place.keys;
        assert(r.y > m.place.list.y + m.place.list.h && r.y + r.h == 800 - 8 * s);
        { /* the keys in the foot's letters, a size under the rest's: ESC alone against the left edge of the first row,
             F1's the last key, against the right edge of the keys' last row */
            int fs = foot_scale(&app), rows;
            assert(fs == s - 1);
            rows = (r.h - 4 * fs) / (16 * fs);
            assert(rows >= 1 && r.h == rows * 16 * fs + 4 * fs);
            foot_px_is(&app, r.x, r.y + 2 * fs, "ESC", yellow, black);
            shows_foot(&app, "exit", normal);
            hides(&app, "ESC", yellow);
            foot_px_is(&app, r.x + r.w - 7 * 8 * fs, r.y + 2 * fs + (rows - 1) * 16 * fs, "F1", yellow, black);
            app.menu.text_size = 1; /* 1x: the foot's too, never under it */
            render(&app);
            assert(app.scale == 1 && foot_scale(&app) == 1);
            shows(&app, "ESC", yellow);
            app.menu.text_size = 0;
            render(&app);
            menu_boxes(&app, &m);
        }
        /* the screens as tabs over it all, the one shown framed in the focus colour */
        r = m.place.tabs;
        assert(r.h && r.y + r.h <= m.place.list.y);
        text_px_is(&app, tab_rect(&app, r, GUI_TAB_MENU).x + cw, r.y + 2 * s, "Menu", yellow, black);
        text_px_is(&app, tab_rect(&app, r, GUI_TAB_GAMES).x + cw, r.y + 2 * s, "Games", normal, black);
        assert(same_colour(pixel(&app, (float)tab_rect(&app, r, GUI_TAB_MENU).x, (float)(r.y + r.h / 2)), yellow));
        /* Tab goes round them, Shift+Tab back; a click on one opens it, as a pad's shoulders do (screen_tab) */
        key(&app, SDLK_TAB);
        assert(app.menu.view == GUI_LOAD_GAMES && !app.editing);
        key(&app, SDLK_TAB);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        key(&app, SDLK_TAB);
        assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
        key_with(&app, SDLK_TAB, SDL_KMOD_LSHIFT);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        screen_tab(&app, -1);
        assert(!app.editing && app.menu.view == GUI_LOAD_GAMES);
        render(&app);
        r = tab_rect(&app, m.place.tabs, GUI_TAB_MENU);
        mouse_px(&app, r.x + 4, r.y + 4, true);
        assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
        app.menu.selected = GUI_KEYS;
        render(&app);
        r = m.place.tips;
        /* the screen's message takes the tips, a failure's in its colour */
        snprintf(app.message, sizeof app.message, "Program ended abnormally (status 7).");
        app.abnormal_exit = true;
        render(&app);
        tips_px_is(&app, r, "Program ended abnormally", red, black);
        app.message[0] = 0;
        app.abnormal_exit = false;
        /* the pointer: over Exit's box it takes the focus, a click on Install Keys opens its screen */
        b = menu_entry_box(&app, &m, exit_entry);
        mouse_px(&app, b.x + 4, b.y + 4, false);
        assert(app.menu.selected == GUI_EXIT);
        b = menu_entry_box(&app, &m, focus);
        mouse_px(&app, b.x + 4, b.y + 4, true);
        assert(app.menu.view == GUI_LOAD_KEYS);
        key(&app, SDLK_ESCAPE);
        assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_KEYS);
        /* Games: Add Folder, then as many of a page's games as the list's box holds, the focused one's path beside
           them and in the bar; Right turns the page, ESC goes back */
        {
            GuiGamesBoxes g;
            size_t page;
            go_tab(&app, GUI_TAB_GAMES);
            assert(app.menu.view == GUI_LOAD_GAMES && !app.browser.selected && app.games.count > 12);
            render(&app);
            games_boxes(&app, &g);
            page = games_page(&app);
            assert(app.browser.page_size == page &&
                   ui_games_shown(g.rects, app.scale, UI_GAMES_ROWS) >= (size_t)g.lead + page);
            b = games_row_box(&app, &g, g.lead); /* the focus, first: the page's first game */
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- ", yellow, black);
            text_px_is(&app, b.x + cw / 2 + 2 * cw, item_text_y(&app, b), app.games.names[0], yellow, black);
            text_px_is(&app, g.place.side.x + cw, g.place.side.y + 2 * s, app.games.names[0], normal, black);
            { /* its kind in the tips, its path in the box under its name, from a drive's letter on Windows */
                char start[9], kind[96];
                const char* about;
                games_focus(&app, &g, &about, kind, sizeof kind);
                assert(kind[0] && about == app.games.paths[0]);
                snprintf(start, sizeof start, "%s", kind);
                tips_px_is(&app, g.place.tips, start, normal, black);
                snprintf(start, sizeof start, "%s", app.games.paths[0]);
                text_px_is(&app, g.place.side.x + cw, g.place.side.y + head_h(&app) + 2 * s, start, normal, black);
            }
            key(&app, SDLK_RIGHT);
            assert(app.browser.page == 1);
            render(&app);
            games_boxes(&app, &g);
            assert(g.start == page);
            key(&app, SDLK_ESCAPE);
            assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_KEYS);
        }
        /* the keys' screen: one box with where they go, the line that waits in its middle and Back at its foot; a click
           in the box opens the picker, whose answer (none here) the bar says; a click on Back goes back */
        {
            GuiPickBoxes k;
            SDL_Event answer;
            Uint64 until;
            const char* waiting = "[ Pick or drop prod.keys ]";
            app.menu.selected = GUI_KEYS;
            key(&app, SDLK_RETURN);
            assert(app.menu.view == GUI_LOAD_KEYS);
            app.clock = 1000 * app.theme.blink_ms * 2; /* the line shown, not blinked away */
            render(&app);
            pick_boxes(&app, &k);
            r = inner(&app, k.place.list);
            text_px_is(&app, r.x, k.place.list.y + 2 * s, "Install Keys", normal, black);
            text_px_is(&app, r.x + (r.w - (int)strlen(waiting) * cw) / 2, k.prompt, waiting, yellow, black);
            tips_px_is(&app, k.place.tips, "Install Keys:", normal, black);
            app.clock = 0;
            mouse_px(&app, r.x + 10, k.prompt + 4, true);
            for (until = SDL_GetTicks() + 2000; !app.message[0] && SDL_GetTicks() < until;)
                while (SDL_PollEvent(&answer)) event(&app, &answer);
            assert(app.menu.view == GUI_LOAD_KEYS && !strcmp(app.message, "No file was picked."));
            render(&app);
            tips_px_is(&app, k.place.tips, "No file was picked.", yellow, black);
            press_key(&app, "ESC", 0, false, yellow); /* the foot's: no Back in the box */
            assert(app.menu.view == GUI_LOAD_NONE);
            app.message[0] = 0;
        }
        /* the Settings: the groups on the left, beside them the focused group's rows; Enter opens it, its rows taking
           the focus and the group marked chosen; a click on another group opens that one, one on a row focuses it and
           a second changes it; the bar says what the focused row is */
        {
            GuiFormBoxes f;
            int launch, row;
            char last[80], glyph[4];
            open_settings(&app);
            assert(app.editing && app.form == GUI_FORM_SETTINGS && !app.editor.row && app.groups > 3);
            render(&app);
            form_boxes(&app, &f);
            assert(f.place.side.x > f.place.list.x + f.place.list.w);
            /* every group shown, ui.txt's 2 pixels apart in a column that would spread them further; nothing past the
               last group */
            assert(form_shown(&app, &f, false) >= (size_t)app.groups && !form_first(&f, false) &&
                   app.groups * (item_h(&app) + 14 * s) <= f.left[UI_FORM_LIST_ROWS].h);
            {
                UiRowInfo info = {0};
                assert(!ui_form_list_rows_get_row_info(&app, (size_t)app.groups, 100, &info));
            }
            form_boxes(&app, &f);
            b = form_row_box(&app, &f, false, 1);
            assert(b.y - form_row_box(&app, &f, false, 0).y == item_h(&app) + 2 * s && b.h == item_h(&app));
            b = form_row_box(&app, &f, false, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- " ICON_APP_WINDOW " App", yellow, black);
            b = form_row_box(&app, &f, false, app.groups - 1); /* the last group, whole */
            snprintf(last, sizeof last, "- %s %s", gui_icon_utf8(group_icon(app.group_keys[app.groups - 1].key), glyph),
                     app.group_keys[app.groups - 1].key);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), last, normal, black);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "App", normal, black);
            /* the note at the foot of the rows whole, wrapped to the box, a half step under the text's letters: at 2x
               (1.5x) and at 1x (1x) */
            for (int size = 2; size >= 1; --size) {
                const char* note = form_note(&app);
                int y, room, half, line, x;
                app.menu.text_size = size == 1 ? 1 : 0; /* Auto is 2x here */
                render(&app);
                form_boxes(&app, &f);
                assert(app.scale == size && note);
                x = f.info.x + 8 * app.scale;
                half = text_half(&app);
                assert(app.half == (size == 2 ? 3 : 2));
                room = inner(&app, f.place.side).w / cell_w(&app);
                line = cell_h(&app);
                assert(f.info.h == wrap_rows(&app, note, room, 0) * line + 2 * app.scale);
                for (y = f.info.y; *note; y += line) {
                    char row[96];
                    size_t n = wrap_at(note, room, 0);
                    snprintf(row, sizeof row, "%.*s", (int)n, note);
                    text_px_is(&app, x, y, row, rgb(app.theme.soft), black);
                    for (note += n; *note == ' ';) ++note;
                }
                app.half = half;
                { /* under the last row drawn */
                    GuiEditor peek;
                    size_t drawn = SDL_min(form_shown(&app, &f, true), (size_t)gui_editor_rows(form_rows(&app, &peek)));
                    GuiRect last = form_row_box(&app, &f, true, (int)drawn - 1);
                    assert(drawn && y <= f.info.y + f.info.h && f.info.y >= last.y + last.h);
                }
            }
            app.menu.text_size = 0;
            render(&app);
            form_boxes(&app, &f);
            b = form_row_box(&app, &f, true, GUI_APP_THEME);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "-    Theme", normal,
                       black); /* no focus on the right yet */
            for (launch = 0; launch < app.groups && strcmp(app.group_keys[launch].key, "Launch"); ++launch) {}
            assert(launch < app.groups);
            /* the pointer over a group focuses it, its rows beside it; over those rows it waits for a click */
            b = form_row_box(&app, &f, false, 2);
            mouse_px(&app, b.x + 4, b.y + 4, false);
            assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 2);
            mouse_px(&app, b.x + 4, b.y + 4, true); /* and a click on the focused group opens it */
            assert(app.form != GUI_FORM_SETTINGS && app.group == 2);
            key(&app, SDLK_ESCAPE);
            assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 2);
            b = form_row_box(&app, &f, true, 0);
            mouse_px(&app, b.x + 4, b.y + 4, false);
            assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 2);
            app.editor.row = launch;
            render(&app);
            form_boxes(&app, &f);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "Launch", normal, black);
            b = form_row_box(&app, &f, true, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "-    GDB stub", normal, black);
            /* a click on one of those rows opens the group on it */
            b = form_row_box(&app, &f, true, 1);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.form == GUI_FORM_GROUP && app.group == launch && app.editor.row == 1);
            key(&app, SDLK_ESCAPE);
            assert(app.form == GUI_FORM_SETTINGS && app.editor.row == launch);
            key(&app, SDLK_RETURN);
            assert(app.form == GUI_FORM_GROUP && app.group == launch && !app.editor.row);
            render(&app);
            form_boxes(&app, &f);
            b = form_row_box(&app, &f, false, launch);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- " ICON_ROCKET " Launch", yellow,
                       black); /* the group open */
            b = form_row_box(&app, &f, true, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "-    GDB stub", yellow, black);
            {
                char start[16]; /* in the tips, its first row, under the head */
                snprintf(start, sizeof start, "%s", app.settings.keys[0].about);
                shows_half(&app, start, normal);
            }
            /* a click on the second row focuses it, a second click changes it */
            launcher(&app, false);
            b = form_row_box(&app, &f, true, 1);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.form == GUI_FORM_GROUP && app.editor.row == 1 && setting_is(&app, "tmpfs", NULL));
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(setting_is(&app, "tmpfs", "on"));
            key(&app, SDLK_LEFT); /* back as it was */
            assert(setting_is(&app, "tmpfs", NULL));
            /* the pointer over a row, not pressed, focuses it, as on the menu's screens (#415), and a click then
               changes it; over the groups' list it waits for a click */
            b = form_row_box(&app, &f, true, 0);
            mouse_px(&app, b.x + 4, b.y + 4, false);
            assert(app.form == GUI_FORM_GROUP && app.editor.row == 0);
            b = form_row_box(&app, &f, true, 1);
            mouse_px(&app, b.x + 4, b.y + 4, false);
            assert(app.editor.row == 1 && setting_is(&app, "tmpfs", NULL));
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(setting_is(&app, "tmpfs", "on"));
            key(&app, SDLK_LEFT);
            b = form_row_box(&app, &f, false, 0);
            mouse_px(&app, b.x + 4, b.y + 4, false);
            assert(app.form == GUI_FORM_GROUP && app.editor.row == 1);
            /* a click on App in the list opens it, from a group's rows; one on another group, that one */
            b = form_row_box(&app, &f, false, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME);
            b = form_row_box(&app, &f, false, launch);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.form == GUI_FORM_GROUP && app.group == launch && !app.editor.row);
            b = form_row_box(&app, &f, false, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME);
            /* the theme editor: its keys on the left, the theme's menu in the preview on the right, which a click, or
               Ctrl+P, turns to a run; a long value being typed shows its end */
            app.editor.row = GUI_APP_EDITOR;
            key(&app, SDLK_RETURN);
            assert(app.form == GUI_FORM_THEME);
            render(&app);
            form_boxes(&app, &f);
            assert(form_shown(&app, &f, false) < (size_t)gui_editor_rows(&app.editor));
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "Preview", normal, black);
            /* the pointer over the preview, where a group's rows would be, focuses no key */
            app.editor.row = 3;
            mouse_px(&app, f.place.side.x + 2 * cw, f.place.side.y + f.place.side.h / 4, false);
            mouse_px(&app, f.place.side.x + 2 * cw, f.place.side.y + 24 * s + 4, false);
            assert(app.editor.row == 3 && app.editor.preview == GUI_SCREEN_MENU);
            app.editor.row = 0;
            mouse_px(&app, f.place.side.x + f.place.side.w / 2, f.place.side.y + f.place.side.h / 2, true);
            assert(app.editor.preview == GUI_SCREEN_COMPILING);
            key_with(&app, SDLK_P, SDL_KMOD_LCTRL);
            assert(app.editor.preview == GUI_SCREEN_MENU);
            /* the colour picker over the keys' box, under a head naming the key; a drag on a bar sets it, ESC puts
               it back */
            {
                GuiPicker pk;
                GuiRgb was;
                for (row = 0;
                     !gui_editor_key_of(&app.editor, row) || strcmp(gui_editor_key_of(&app.editor, row)->key, "ink");
                     ++row) {}
                app.editor.row = row;
                was = app.editor.theme.ink;
                key(&app, SDLK_RETURN);
                assert(app.editor.picking);
                render(&app);
                picker_place(&app, &pk);
                assert(pk.box.x >= f.place.list.x && pk.box.x + pk.box.w <= f.place.list.x + f.place.list.w);
                text_px_is(&app, pk.box.x + cw, pk.box.y + 2 * s, "[colors] ink", normal, black);
                text_px_is(&app, pk.rows[0].x + cw / 2, pk.rows[0].y + 3 * s, "R", yellow, black);
                text_px_is(&app, pk.rows[1].x + cw / 2, pk.rows[1].y + 3 * s, "G", normal, black);
                mouse_px(&app, pk.bars[2].x + s, pk.bars[2].y + 2, true);
                assert(app.editor.channel == GUI_PICK_BLUE && !app.editor.theme.ink.b);
                {
                    SDL_Event up = {0};
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    event(&app, &up);
                }
                render(&app);
                text_px_is(&app, pk.rows[2].x + cw / 2, pk.rows[2].y + 3 * s, "B", yellow, black);
                key(&app, SDLK_ESCAPE);
                assert(!app.editor.picking && !memcmp(&app.editor.theme.ink, &was, sizeof was));
                assert(app.editing && app.form == GUI_FORM_THEME);
            }
            for (row = 0;
                 !gui_editor_key_of(&app.editor, row) || strcmp(gui_editor_key_of(&app.editor, row)->key, "name");
                 ++row) {}
            app.editor.row = row;
            key(&app, SDLK_RETURN);
            assert(app.editor.typing);
            typed(&app, "a theme name long enough to run past its box's right edge");
            render(&app);
            form_boxes(&app, &f);
            b = form_row_box(&app, &f, false, row);
            {
                int room = (b.w - cw * 3 / 2) / cw, end = b.x + cw / 2 + (room - 2) * cw, x, y;
                text_px_is(&app, end, item_text_y(&app, b), "e_", yellow, black); /* its end, and the cursor */
                /* wide letters typed in its place (tests/po/xx.po's Menu, 52 columns in 78 bytes): the end still in
                   view, the last letter's two columns and the cursor's within the box, a column short of it at most */
                for (x = 0; x < 80; ++x) key(&app, SDLK_BACKSPACE);
                assert(!app.editor.text[0]);
                for (x = 0; x < 13; ++x) typed(&app, "\xe8\x8f\x9c\xe5\x8d\x95");
                assert(strlen(app.editor.text) == 78);
                render(&app);
                assert(find_letters(&app, "\xe5\x8d\x95_", yellow, 1, &x, &y) && y == item_text_y(&app, b));
                assert(x <= b.x + cw / 2 + (room - 3) * cw && x >= b.x + cw / 2 + (room - 4) * cw);
            }
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.form == GUI_FORM_APP);
            leave_editor(&app);
        }
        /* a box over the screen is a box on the screen darkened: the exit question, its choices as boxes, a click on
           one taking it, a click off the box keeping on; the files an older version left, OK focused, a key closing it */
        {
            GuiChoices choices;
            app.menu.selected = GUI_EXIT;
            render(&app);
            menu_boxes(&app, &m);
            b = menu_entry_box(&app, &m, (int)m.lists[UI_MENU_ENTRIES].focus);
            key(&app, SDLK_ESCAPE);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            render(&app);
            ask_boxes(&app, &r, &choices);
            text_px_is(&app, r.x + cw, r.y + 4 * s, "Exit ScarletNX?", rgb(app.theme.warning), black);
            text_px_is(&app, choice_box(&app, &choices, 0).x + cw / 2, item_text_y(&app, choice_box(&app, &choices, 0)),
                       "- Cancel", yellow, black);
            { /* a row each, 2 pixels apart and as high as the menu's rows */
                GuiRect stay = choice_box(&app, &choices, 0), quit = choice_box(&app, &choices, 1);
                assert(stay.h == item_h(&app) && quit.y - stay.y == item_h(&app) + 2 * s);
            }
            assert(b.x < r.x && /* its left edge, off the question's box, darkened */
                   same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), (SDL_Color){0x7F, 0x7F, 0, 255}));
            mouse_px(&app, choice_box(&app, &choices, 1).x + 4, choice_box(&app, &choices, 1).y + 4, false);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            mouse_px(&app, r.x + r.w + 8, r.y + r.h + 8, true);
            assert(!app.confirm.shown && !app.menu.quit);
            key(&app, SDLK_ESCAPE);
            render(&app);
            mouse_px(&app, choice_box(&app, &choices, 1).x + 4, choice_box(&app, &choices, 1).y + 4, true);
            assert(!app.confirm.shown && app.closing_ms && !app.menu.quit); /* the screen of what is kept, first */
            app.closing_ms = 0;
            {
                char* elsewhere[] = {"/elsewhere"}; /* a count for another list is not this one's */
                space_count(&app.left_space, &app.left_counter, elsewhere, 1, true);
                while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
            }
            app.left = snx_format("/old/recent.txt" SAY_LEFT_BY "/new/recent.txt\n");
            app.lefts = 1;
            app.left_shown = true;
            app.left_at = SDL_GetTicks(); /* not looked for again, as there is no such file */
            render(&app);
            {
                int width, height;
                area(&app, &width, &height);
                r = over_rect(&app, width / cw - 6, head_h(&app) + 2 * 16 * s + item_h(&app) + 16 * s);
                text_px_is(&app, r.x + cw, r.y + 2 * s, "Left by older versions", normal, black);
                text_px_is(&app, r.x + cw, r.y + head_h(&app) + 4 * s, "- /old/recent.txt", red, black);
                /* its space, not counted: the menu counts what it looked for itself */
                text_px_is(&app, r.x + cw + 2 * cw, r.y + head_h(&app) + 4 * s + 16 * s, "counting",
                           rgb(app.theme.soft), black);
                /* OK, ui.txt's scene Files: a row as the menu's at the box's foot, in focus */
                text_px_is(&app, r.x + cw + cw / 2, r.y + r.h - item_h(&app) - 6 * s + s, "- OK", yellow, black);
            }
            key(&app, SDLK_DOWN); /* one file: nothing to go to */
            assert(app.left_shown && app.menu.selected == GUI_EXIT);
            key(&app, SDLK_RETURN);
            assert(!app.left_shown && app.menu.selected == GUI_EXIT && !app.confirm.shown); /* closed, nothing else */
            free(app.left);
            app.left = NULL;
            app.lefts = 0;
            app.menu.selected = GUI_KEYS;
        }
        /* a run of the console layout: the driver's output in its box, sized to it, beside it what runs and how it
           ended, the ESC line in the bar, where a finger's tap aborts a run */
        {
            GuiPlace p;
            Uint64 until;
            int cols, rows;
            char* hung = snx_path_join(temporary, "hang in join boxed.elf");
            bool ready = false;
            assert(!snx_write_file(hung, "", 0));
            app.menu.selected = GUI_LINUX;
            launch(&app, snx_strdup(hung), false);
            for (until = SDL_GetTicks() + 5000; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
                size_t got = 0;
                unsigned char* data = snx_read_file(hung, &got);
                ready = data && got == 5 && !memcmp(data, "ready", 5);
                free(data);
                pump(&app);
            }
            assert(ready && app.menu.running);
            render(&app);
            run_place(&app, &p);
            run_console(&app, &p, &cols, &rows);
            assert(app.console.cols == cols && app.console.rows == rows && !p.tabs.h);
            text_px_is(&app, p.list.x + cw, p.list.y + 2 * s, "Output", normal, black);
            text_px_is(&app, p.side.x + cw, p.side.y + 2 * s, "Load Linux ARM ELF", normal, black);
            r = foot_room(&app, p.keys, app.theme.footer_box);
            foot_px_is(&app, r.x + r.w - (int)strlen(SAY_ABORT) * 8 * foot_scale(&app), r.y, SAY_ABORT, red, black);
            touch_box(&app, p.keys);
            assert(app.aborting);
            for (until = SDL_GetTicks() + 3000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
            assert(!app.menu.running && !app.finished);
            free(hung);
            /* one that ends: its message beside the output, the ESC line to go back. The NRO made already plays */
            char* input_made = made_game(&app, input, "game with spaces");
            app.menu.selected = GUI_SWITCH;
            launch(&app, snx_strdup(input), false);
            play_made(&app);
            made_gone(&app, input_made);
            for (until = SDL_GetTicks() + 5000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
            assert(app.finished && !app.abnormal_exit);
            render(&app);
            run_place(&app, &p);
            text_px_is(&app, p.list.x + cw, p.list.y + head_h(&app) + 2 * s, "driver std", normal, black);
            text_px_is(&app, p.side.x + cw,
                       p.side.y + head_h(&app) + 2 * s +
                           wrap_rows(&app, app.file, (p.side.w - 2 * cw) / cw, 1) * 16 * s + 8 * s,
                       "The run ended with status 0.", yellow, black);
            r = foot_room(&app, p.keys, app.theme.footer_box);
            foot_px_is(&app, r.x, r.y, SAY_DONE, red, black);
            key(&app, SDLK_ESCAPE);
            assert(!app.finished && app.menu.view == GUI_LOAD_NONE);
            pump(&app);
            app.menu.selected = GUI_KEYS;
        }
        /* each key the foot shows is a button: a click or a tap on its word presses it as the keyboard does, each word
           between slashes its own; its text, when its words all press one key, that key. A word of every screen's keys
           presses one, or is one that names none */
        {
            static const char* const says[] = {
                SAY_MENU_KEYS, SAY_GAMES, SAY_KEYS_HOW, SAY_EDITOR, SAY_PICKER, SAY_SETTINGS, SAY_GROUPS, SAY_APP,
                SAY_FOLDERS, SAY_CHANGE, SAY_DEFAULT, SAY_DELETE, SAY_ADD, SAY_RESET, SAY_PAD, SAY_TAB, SAY_BACK,
                SAY_HELP,
                /* and while a pad drives the menu, its words found first from here */
                SAY_PAD_MENU, SAY_PAD_GAMES, SAY_PAD_KEYS_HOW, SAY_PAD_EDITOR, SAY_PAD_PICKER, SAY_PAD_SETTINGS,
                SAY_PAD_GROUPS, SAY_PAD_APP, SAY_PAD_FOLDERS, SAY_PAD_CHANGE, SAY_PAD_DEFAULT, SAY_PAD_DELETE,
                SAY_PAD_ADD, SAY_PAD_RESET, SAY_PAD_USERS, SAY_PAD_TAB, SAY_PAD_GAMES_AOT, SAY_PAD_HELP};
            const size_t pads = 18; /* the first of the pad's */
            static const char* const none[] = {"Arrows", "mouse", "pad", "D-pad"};
            char key_of[128], what[128];
            Uint8 was;
            int launch;
            for (size_t i = 0; i < SDL_arraysize(says); ++i)
                for (const char* at = says[i]; hint_next(&at, key_of, what, sizeof key_of);)
                    for (const char *word = key_of, *end; *word; word = *end ? end + 1 : end) {
                        size_t n, k;
                        end = strchr(word, '/');
                        if (!end) end = word + strlen(word);
                        n = (size_t)(end - word);
                        for (k = 0; k < SDL_arraysize(none) && (strlen(none[k]) != n || strncmp(none[k], word, n));
                             ++k) {}
                        if (hint_word(word, n, i >= pads) < 0 && k == SDL_arraysize(none))
                            fprintf(stderr, "gui_sdl.c: the foot's key \"%.*s\" presses nothing\n", (int)n, word);
                        assert(hint_word(word, n, i >= pads) >= 0 || k < SDL_arraysize(none));
                    }
            app.theme = *gui_theme_builtin("default");
            app.menu.view = GUI_LOAD_NONE;
            app.menu.selected = GUI_SETTINGS;
            app.message[0] = 0;
            /* the menu's foot: ESC alone at its left, F5 and F1 at its right, the keys every screen has (moving,
               picking, the tabs, the side box) left to F1's box */
            {
                GuiMenuBoxes mb;
                GuiRect room;
                int x, y, scale = app.scale;
                render(&app);
                menu_boxes(&app, &mb);
                room = foot_room(&app, mb.place.keys, app.theme.footer_box);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "ESC", yellow, 1, &x, &y) && x == room.x);
                assert(find_text(&app, "F1", yellow, 1, &x, &y) &&
                       x + (int)(strlen("F1 help") * 8) * app.scale == room.x + room.w);
                app.scale = scale;
                hides_foot(&app, "Arrows/mouse", yellow);
                hides_foot(&app, "Tab", yellow);
                hides_foot(&app, "F2", yellow);
                shows_foot(&app, "F5", yellow);
            }
            /* ESC asks before closing, the pointer over it presses nothing */
            press_key(&app, "ESC", 0, false, yellow);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && !app.menu.quit);
            {
                int x, y, scale = app.scale;
                render(&app);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "ESC", yellow, 1, &x, &y));
                app.scale = scale;
                mouse_px(&app, x + 4, y + 4, false);
                assert(!app.confirm.shown);
                app.hovering = false; /* the pointer gone: no key lit */
            }
            /* a sentence's words press as the keyboard does, each its own: Arrows/mouse name no one key; select, the
               text of three words that all select, opens the Settings, where ESC comes back */
            assert(!hint_press(&app, "Arrows/mouse", 1) && app.menu.selected == GUI_SETTINGS && !app.editing);
            assert(hint_press(&app, "Space/Enter/click", 20) && app.editing && app.form == GUI_FORM_SETTINGS);
            press_key(&app, "ESC", 0, false, yellow);
            assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
            { /* the keys' box's whole height is their rows': a click on its top edge, over F1, opens its box, which a
                 click closes */
                GuiMenuBoxes mb;
                int x, y, scale = app.scale;
                render(&app);
                menu_boxes(&app, &mb);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "F1", yellow, 1, &x, &y));
                app.scale = scale;
                assert(y > mb.place.keys.y);
                mouse_px(&app, x + 4, mb.place.keys.y, true);
                assert(app.help_shown);
                render(&app);
                shows(&app, "Keys", normal);
                shows(&app, "Arrows/mouse", yellow); /* every key, the foot's and the others */
                shows(&app, "side box", normal);
                mouse_px(&app, x + 4, y + 4, true);
                assert(!app.help_shown && app.menu.view == GUI_LOAD_NONE);
            }
            { /* the mouse over a key of the foot lights its sentence, inverted, when a click there presses something;
                 not once it leaves the window */
                SDL_Event left = {0};
                int x, y, scale = app.scale;
                SDL_Color page = rgb(app.theme.page);
                render(&app);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "F5", yellow, 1, &x, &y));
                app.scale = scale;
                shows_foot(&app, "refresh", normal);
                mouse_px(&app, x + 4, y + 4, false);
                render(&app);
                shows_foot(&app, "F5", page);
                shows_foot(&app, "refresh", page);
                hides_foot(&app, "F5", yellow);
                left.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
                app.hovering = true;
                event(&app, &left);
                assert(!app.hovering);
            }
            /* Games, where each word of Left/Right/PgUp/PgDown turns the page its way, and its text, of words two ways,
               none; Up/Down's each move the focus */
            key(&app, SDLK_TAB);
            assert(app.menu.view == GUI_LOAD_GAMES && app.browser.page == 0 && gui_browser_pages(&app.browser) > 1);
            assert(hint_press(&app, "Left/Right/PgUp/PgDown", 18) && app.browser.page == 1);  /* PgDown */
            assert(!hint_press(&app, "Left/Right/PgUp/PgDown", 24) && app.browser.page == 1); /* page */
            assert(hint_press(&app, "Left/Right/PgUp/PgDown", 12) && app.browser.page == 0);  /* PgUp */
            assert(hint_press(&app, "Left/Right/PgUp/PgDown", 10) && app.browser.page == 1);  /* Right's slash */
            assert(hint_press(&app, "Left/Right/PgUp/PgDown", -1) &&
                   app.browser.page == 0); /* the margin before Left */
            app.browser.selected = 0;
            assert(hint_press(&app, "Up/Down", 3) && app.browser.selected == 1);
            assert(hint_press(&app, "Up/Down", 0) && app.browser.selected == 0);
            press_key(&app, "ESC", 0, false, yellow); /* Games' own ESC, to the menu */
            assert(app.menu.view == GUI_LOAD_NONE);
            key(&app, SDLK_TAB);
            /* Android's words: a swipe turns the page, Back goes back as ESC, a tap selects; Tap outside, the
               picker's, presses nothing (a tap off the picker keeps the colour) */
            assert(hint_press(&app, "Swipe", 0) && app.browser.page == 1);
            assert(!hint_press(&app, "Tap outside", 0));
            go_tab(&app, GUI_TAB_MENU);
            assert(hint_press(&app, "Back", 0) && app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            key(&app, SDLK_ESCAPE);
            app.menu.selected = GUI_KEYS;
            assert(hint_press(&app, "Tap", 0) && app.menu.view == GUI_LOAD_KEYS);
            /* the keys' screen: ESC goes back */
            press_key(&app, "ESC", 0, false, yellow);
            assert(app.menu.view == GUI_LOAD_NONE && !app.confirm.shown);
            /* a group's settings: Up/Down, Left/Right change the row, Del puts it back, ESC leaves the rows, then the
               Settings */
            open_settings(&app);
            for (launch = 0; launch < app.groups && strcmp(app.group_keys[launch].key, "Launch"); ++launch) {}
            assert(launch < app.groups);
            app.editor.row = launch;
            assert(hint_press(&app, "Enter", 0));
            assert(app.form == GUI_FORM_GROUP && app.group == launch && !app.editor.row);
            assert(hint_press(&app, "Up/Down", 5));
            assert(app.editor.row == 1 && !strcmp(app.settings.keys[app.first + 1].key, "Extract to tmpfs"));
            assert(gui_settings_set(&app.settings, "tmpfs", "off"));
            assert(hint_press(&app, "Left/Right", 6));
            assert(setting_is(&app, "tmpfs", "on"));
            press_key(&app, "Del", 0, false, yellow);
            assert(setting_is(&app, "tmpfs", NULL));
            press_key(&app, "ESC", 0, false, yellow);
            assert(app.editing && app.form == GUI_FORM_SETTINGS);
            press_key(&app, "ESC", 0, false, yellow);
            assert(!app.editing);
            /* the colour picker, its keys over the foot as ever: Right steps the channel, ESC puts the colour back; a
               finger on Enter keeps it as it goes down, its lift no tap that would open the picker again */
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && app.form == GUI_FORM_THEME);
            for (app.editor.row = 0; !gui_editor_key_of(&app.editor, app.editor.row) ||
                                     strcmp(gui_editor_key_of(&app.editor, app.editor.row)->key, "page");
                 ++app.editor.row) {}
            key(&app, SDLK_RETURN);
            assert(app.editor.picking && app.editor.channel == GUI_PICK_RED && app.editor.theme.page.r < 255);
            was = app.editor.theme.page.r;
            assert(hint_press(&app, "Left/Right", 6));
            assert(app.editor.picking && app.editor.theme.page.r == was + 1);
            press_key(&app, "ESC", 0, false, yellow);
            assert(!app.editor.picking && app.editor.theme.page.r == was && app.form == GUI_FORM_THEME);
            key(&app, SDLK_RETURN);
            assert(app.editor.picking);
            press_key(&app, "ESC", 0, true, yellow); /* a finger: put back as it goes down, its lift no tap */
            assert(!app.editor.picking && app.editing && app.form == GUI_FORM_THEME && !app.pick_press);
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.form == GUI_FORM_APP);
            leave_editor(&app);
            /* Folders: a library's Add Folder opens the folder picker for that library, which answers none here; the
               foot has only what the focused row does: Del on an Extra, nothing on dumps/ */
            app_select(&app, GUI_APP_PLACES);
            assert(app.editing && app.form == GUI_FORM_FOLDERS && !app.editor.note[0]);
            app.editor.row = FOLDER_ROW(GUI_ROW_DUMPS, -1);
            render(&app);
            hides_foot(&app, "Del", yellow);
            app.editor.row = FOLDER_ROW(GUI_ROW_ADD, GUI_LIBRARY_DUMPS);
            key(&app, SDLK_RETURN);
            assert(app.adding == GUI_LIBRARY_DUMPS);
            {
                SDL_Event answer;
                Uint64 until = SDL_GetTicks() + 2000;
                while (!app.editor.note[0] && SDL_GetTicks() < until) /* its answer comes on the picker's thread */
                    while (SDL_PollEvent(&answer)) event(&app, &answer);
            }
            picker_box_closed(&app);
            assert(!strcmp(app.editor.note, "No folder was picked.") ||
                   !strncmp(app.editor.note, "The folder picker could not open: ", 34));
            leave_editor(&app);
            /* and in YoRHa's box on the band, in its page colour */
            press_key(&app, "F1", 0, false, yellow);
            assert(app.help_shown);
            key(&app, SDLK_F1);
            assert(!app.help_shown);
            app.theme = *gui_theme_builtin("yorha");
            app.theme.grid = false;
            press_key(&app, "ESC", 0, false, rgb(app.theme.page));
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            key(&app, SDLK_ESCAPE);
            app.theme = *gui_theme_builtin("default");
            app.menu.selected = GUI_KEYS;
        }
        /* YoRHa: the boxes on its dim colour, the focused one on its focus colour with the cursor in the margin, the
           side box's name on a band of its ink */
        app.theme = *gui_theme_builtin("yorha");
        assert(app.theme.panels == GUI_PANELS_FILL && !strcmp(app.theme.cursor, "\xe2\x96\xba "));
        /* the page behind the boxes: its grid in the dim colour under them */
        {
            SDL_Color plain = rgb(app.theme.page), c;
            int width, height;
            render(&app);
            area(&app, &width, &height);
            c = pixel(&app, 0, (float)(height / 2 + 1)); /* the grid's first column, beside the boxes */
            assert(!same_colour(c, plain) && c.r < plain.r && c.r > app.theme.dim.r);
            app.theme.grid = false; /* exact colours below */
        }
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        app.clock = 1; /* the tips at their start, before they slide */
        render(&app);
        menu_boxes(&app, &m);
        b = menu_entry_box(&app, &m, (int)m.lists[UI_MENU_ENTRIES].focus);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe " ICON_KEY_ROUND " Install Keys", page,
                   rgb(app.theme.focus));
        text_px_is(&app, m.rects[UI_MENU_ENTRIES].x, item_text_y(&app, b), "\x10", rgb(app.theme.focus), page);
        b = menu_entry_box(&app, &m, GUI_EXIT);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe " ICON_LOG_OUT " Exit", ink,
                   dim = rgb(app.theme.dim));
        r = m.place.side;
        text_px_is(&app, r.x + cw, r.y + 2 * s, "Install Keys", page, ink);
        assert(m.place.tips.y < m.place.list.y); /* the tips in their box under the head, off the band */
        tips_px_is(&app, m.place.tips, "Installs prod.keys", ink, page);
        render_foot_tips(&app, &m); /* in the foot, as the setup's: on its band, in the page's colour */
        assert(m.place.tips.y == m.place.keys.y);
        r = foot_room(&app, m.place.tips, app.theme.tips_box);
        foot_px_is(&app, r.x, r.y, "Installs prod.keys", page, ink);
        /* on one row of the foot, the rest of a long one out of it until it slides there; [tips] wrap puts the rest on
           the foot's next rows */
        snprintf(app.message, sizeof app.message,
                 "The tips take a row of the foot: a long line slides, or wraps over its rows.");
        assert((int)strlen(app.message) > r.w / (8 * foot_scale(&app)));
        render_foot_tips(&app, &m);
        foot_px_is(&app, r.x, r.y, "The tips take", page, ink);
        hides_foot(&app, "rows.", page);
        for (app.clock = 1; gui_slide(1000, app.clock, app.theme.scroll_ms) != 2;) app.clock += app.theme.scroll_ms;
        render_foot_tips(&app, &m);
        foot_px_is(&app, r.x, r.y, "e tips take", page, ink);
        app.clock = 1;
        app.theme.tips_wrap = true;
        render_foot_tips(&app, &m);
        shows_foot(&app, "rows.", page);
        /* longer than the foot's rows: its last row slides what is left, which nothing else would show */
        {
            int fs = foot_scale(&app), cols = r.w / (8 * fs), n = 0, x, y, found, scale = app.scale, over;
            const char* rest = app.message;
            while (n < 6 * cols) n += snprintf(app.message + n, sizeof app.message - (size_t)n, "a row of words ");
            snprintf(app.message + n, sizeof app.message - (size_t)n, "and its end.");
            render_foot_tips(&app, &m);
            r = foot_room(&app, m.place.tips, app.theme.tips_box);
            hides_foot(&app, "its end.", page);
            for (int row = 1; row < r.h / (16 * fs); ++row) /* the rows over the last, as they wrap */
                for (rest += wrap_at(rest, cols, 0); *rest == ' ';) ++rest;
            over = (int)SDL_utf8strlen(rest) - cols;
            assert(r.h >= 2 * 16 * fs && over > 0);
            for (app.clock = 1; gui_slide(over, app.clock, app.theme.scroll_ms) != over;)
                app.clock += app.theme.scroll_ms;
            render_foot_tips(&app, &m);
            app.scale = fs;
            found = find_text(&app, "and its end.", page, 1, &x, &y);
            app.scale = scale;
            assert(found && x + 12 * 8 * fs == r.x + cols * 8 * fs && y == r.y + r.h / (16 * fs) * 16 * fs - 16 * fs);
            app.clock = 1;
        }
        app.theme.tips_wrap = false;
        app.message[0] = 0;
        app.clock = 0;
        /* [body] text: a text too long for its box over its rows, or on one row that slides */
        {
            const char* many = "one two three four five six seven eight";
            GuiRect box = gui_rect(0, 0, 10 * cw, 4 * 16 * s);
            int rows = wrap_rows(&app, many, 10, 0);
            assert(app.theme.text == GUI_TEXT_WRAP && rows > 1 &&
                   wrap_in(&app, box, 0, 0, many, 0, ink) == SDL_min(rows, 4));
            app.theme.text = GUI_TEXT_SLIDE;
            assert(wrap_rows(&app, many, 10, 0) == 1 && wrap_in(&app, box, 0, 0, many, 0, ink) == 1);
            assert(!wrap_in(&app, box, 0, 0, "", 0, ink) &&
                   !wrap_in(&app, gui_rect(0, 0, 10 * cw, 8), 0, 0, many, 0, ink));
            app.theme.text = GUI_TEXT_WRAP;
        }
        /* the users of the app's users/: Aogo and Xuji. The head shows Settings' user, 1, in a box at its right */
        {
            char* users = snx_path_join(temporary, "users");
            char* ini = snx_path_join(users, "users.ini");
            static const char text[] =
                "[user1]\nnickname=Aogo\n[user2]\nuid=5f3c0d9e8a7b41c2963e0f1a2b3c4d5e\nnickname=Xuji\n";
            assert(!snx_mkdirs(users) && !snx_write_file(ini, text, sizeof text - 1));
            free(app.users_dir);
            app.users_dir = users;
            users_read(&app);
            /* Settings > User: who the games play as, Left/Right round the users, then each user's nickname, which Enter
               types and Enter writes to users.ini, the head's too; one the rule refuses is said, the nickname kept */
            {
                int g, user = app.user;
                char* kept;
                size_t size;
                open_settings(&app);
                for (g = 1; g < app.groups && strcmp(app.group_keys[g].key, "User"); ++g) {}
                assert(g < app.groups);
                app.editor.row = g;
                render(&app);
                shows(&app, "User 2", rgb(app.theme.ink)); /* the focused group's rows beside it */
                key(&app, SDLK_RETURN);
                assert(app.form == GUI_FORM_USERS && gui_editor_rows(&app.editor) == 3 &&
                       !strcmp(app.users_form.plays, "User 1: Aogo"));
                render(&app);
                shows(&app, "Current user User 1: Aogo", rgb(app.theme.page)); /* focused: on YoRHa's bar */
                shows(&app, "Xuji", rgb(app.theme.ink));
                key(&app, SDLK_RIGHT);
                assert(setting_is(&app, "user", "2") && !strcmp(app.users_form.plays, "User 2: Xuji") && app.user == 2);
                key(&app, SDLK_LEFT);
                assert(setting_is(&app, "user", "1") && app.user == 1);
                key(&app, SDLK_DOWN);
                key(&app, SDLK_RETURN);
                assert(app.editor.typing);
                for (int i = 0; i < 8; ++i) key(&app, SDLK_BACKSPACE);
                typed(&app, "Aogo 55");
                point(&app, row_box(&app, 2, true), false); /* the pointer moves no focus off a nickname typed */
                assert(app.editor.row == 1 && app.editor.typing);
                key(&app, SDLK_RETURN);
                assert(!app.editor.typing && !strcmp(app.editor.note, "User 1 is Aogo 55 now") &&
                       !strcmp(app.user_name, "Aogo 55"));
                kept = (char*)snx_read_file(ini, &size);
                assert(kept && strstr(kept, "[user1]\nnickname=Aogo 55\n[user2]"));
                free(kept);
                key(&app, SDLK_RETURN);
                for (int i = 0; i < 8; ++i) key(&app, SDLK_BACKSPACE);
                typed(&app, "Ab");
                key(&app, SDLK_RETURN);
                assert(!strcmp(app.editor.note, "Ab is not a nickname: nickname 4 to 20 of A-Z a-z 0-9 space . _ -") &&
                       !strcmp(app.users_form.nick[0], "Aogo 55"));
                key(&app, SDLK_RETURN); /* back as the tests below know it */
                for (int i = 0; i < 8; ++i) key(&app, SDLK_BACKSPACE);
                typed(&app, "Aogo");
                key(&app, SDLK_RETURN);
                assert(!strcmp(app.user_name, "Aogo"));
                key(&app, SDLK_ESCAPE);
                assert(app.form == GUI_FORM_SETTINGS && app.editor.row == g);
                key(&app, SDLK_ESCAPE);
                assert(!app.editing && app.user == user);
            }
            free(ini);
            render(&app);
            menu_boxes(&app, &m);
        }
        /* its title whole at 1280x800, at the text's size (twice it 2400 wide, below), in the row of tabs against its
           right, two columns before the user there, its foot on the tabs'; with its shadow, in the dim colour a big
           pixel down and right */
        r = m.place.title;
        assert(app.user == 1 && !strcmp(app.user_name, "Aogo") && m.place.badge.h == SDL_max(r.h, m.place.tabs.h));
        assert(m.place.badge.x + m.place.badge.w == m.place.head.x + m.place.head.w);
        assert(r.h == 16 * s && r.x + r.w == m.place.badge.x - 2 * cw && r.y + r.h == m.place.tabs.y + m.place.tabs.h);
        /* the head the same on the menu, Games and the Settings: the title on the tabs' row, in letters of one size,
           each whole a size smaller before the same first parts are left out for the widest of their titles to fit;
           2400 wide, each whole and big; at 800 wide their last parts; with a nickname that leaves too little room,
           none */
        {
            int w0, h0;
            char name[sizeof app.user_name];
            SDL_GetWindowSize(app.window, &w0, &h0);
            snprintf(name, sizeof name, "%s", app.user_name);
            for (int wide = 0; wide < 4; ++wide) {
                char seen[3][160];
                int sizes[3];
                if (wide) {
                    assert(SDL_SetWindowSize(app.window, wide == 1 ? 2400 : 800, h0));
                    SDL_SyncWindow(app.window);
                }
                if (wide == 3) { /* a nickname that leaves the title some room, too little for the shortest last part */
                    int n = 0;
                    do {
                        memset(app.user_name, 'x', (size_t)++n);
                        app.user_name[n] = 0;
                    } while (n < SNX_USERS_NICKNAME &&
                             !(head_room(&app, 1) > 0 && head_room(&app, 1) < title_px(&app, "MENU", 1)));
                    assert(head_room(&app, 1) > 0 && head_room(&app, 1) < title_px(&app, "MENU", 1));
                }
                for (int tab = 0; tab < 3; ++tab) {
                    GuiPlace p;
                    go_tab(&app, tab);
                    layout(&app);
                    place_screen(&app, &p, "", GUI_SHOWS_TABS);
                    head_title(&app, true, seen[tab], sizeof seen[tab], &sizes[tab]);
                    if (wide == 3) {
                        assert(!seen[tab][0] && !p.title.h);
                        continue;
                    }
                    assert(p.title.h && p.title.y + p.title.h == p.tabs.y + p.tabs.h && sizes[tab] == sizes[0]);
                    assert(p.title.w == gui_text_cols(seen[tab]) * sizes[tab] * cw);
                }
                if (wide == 2)
                    assert(!strcmp(seen[0], "MENU") && !strcmp(seen[1], "GAMES") && !strcmp(seen[2], "SETTINGS") &&
                           sizes[0] == 1);
                else if (wide == 1) {
                    /* a translation too long for the size the English titles take: that size still, the same first
                       parts left out of every screen's */
                    const char* xx_title = "SCARLETNX  /  CONFIGURATIONS OF EVERY GROUP AND ITS SETTINGS";
                    char seen_xx[3][160];
                    int sizes_xx[3];
                    assert(!strcmp(seen[0], "SCARLETNX  /  MENU") && !strcmp(seen[1], "SCARLETNX  /  GAMES") &&
                           !strcmp(seen[2], "SCARLETNX  /  SETTINGS") && sizes[0] == 2);
                    assert(title_px(&app, xx_title, 2) > head_room(&app, 2) &&
                           title_px(&app, title_tail(xx_title, 1), 2) <= head_room(&app, 2));
                    snx_tr_use(1); /* xx */
                    for (int tab = 0; tab < 3; ++tab) {
                        go_tab(&app, tab);
                        layout(&app);
                        head_title(&app, true, seen_xx[tab], sizeof seen_xx[tab], &sizes_xx[tab]);
                        assert(sizes_xx[tab] == 2);
                    }
                    assert(!strcmp(seen_xx[0], "MENU") && !strcmp(seen_xx[1], "GAMES") &&
                           !strcmp(seen_xx[2], title_tail(xx_title, 1)));
                    snx_tr_use(0);
                } else if (
                    !wide) /* too narrow for SCARLETNX  /  SETTINGS big beside the tabs: every one whole, smaller */
                    assert(!strcmp(seen[0], "SCARLETNX  /  MENU") && !strcmp(seen[1], "SCARLETNX  /  GAMES") &&
                           !strcmp(seen[2], "SCARLETNX  /  SETTINGS") && sizes[0] == 1);
            }
            snprintf(app.user_name, sizeof app.user_name, "%s", name);
            assert(SDL_SetWindowSize(app.window, w0, h0));
            SDL_SyncWindow(app.window);
            go_tab(&app, GUI_TAB_MENU);
            layout(&app);
            render(&app);
            menu_boxes(&app, &m);
        }
        { /* the box framed in the dim colour and filled with the page's, as YoRHa's boxes; the nickname a column in, in
             ink; Wimsy (no users/<uid>.jpg) against its right, two pixels in */
            GuiRect b = m.place.badge;
            int nx, ny, side = b.h - 4 * s;
            render(&app);
            assert(find_text(&app, "Aogo", rgb(app.theme.ink), 1, &nx, &ny) && nx == b.x + cw && ny > b.y &&
                   ny + 16 * s < b.y + b.h);
            assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), rgb(app.theme.dim)));
            assert(same_colour(pixel(&app, (float)(b.x + b.w - 1), (float)(b.y + b.h / 2)), rgb(app.theme.dim)));
            assert(same_colour(pixel(&app, (float)(b.x + b.w - 1 - s), (float)(b.y + b.h / 2)), rgb(app.theme.page)));
            assert(!same_colour(pixel(&app, (float)(b.x + b.w - 2 * s - side / 2), (float)(b.y + 2 * s + side / 2)),
                                rgb(app.theme.page)));
            /* a click on it opens the box of users, the one shown in focus; Down and Return pick Xuji, kept as
               Settings' user in settings.ini, and the head shows them */
            swipe_px(&app, b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2);
            assert(app.users_shown && app.users_focus == 0);
            render(&app);
            assert(find_text(&app, "Xuji", rgb(app.theme.ink), 1, &nx, &ny));
            assert(find_text(&app, "Who plays the games", rgb(app.theme.page), 1, &nx, &ny)); /* whole: none cut */
            key(&app, SDLK_DOWN);
            assert(app.users_shown && app.users_focus == 1);
            key(&app, SDLK_RETURN);
            assert(!app.users_shown && app.user == 2 && !strcmp(app.user_name, "Xuji"));
            {
                size_t size;
                char* kept = (char*)snx_read_file(app.settings_file, &size);
                assert(kept && strstr(kept, "[User]\nuser=2\n"));
                free(kept);
            }
            /* ESC closes it, as does a click off it, the user kept; Up and Return pick Aogo again, kept as picked */
            render(&app);
            menu_boxes(&app, &m);
            b = m.place.badge;
            swipe_px(&app, b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2);
            assert(app.users_shown && app.users_focus == 1);
            key(&app, SDLK_ESCAPE);
            assert(!app.users_shown && app.user == 2);
            swipe_px(&app, b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2);
            assert(app.users_shown);
            swipe_px(&app, 1, 1, 1, 1);
            assert(!app.users_shown && app.user == 2);
            swipe_px(&app, b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2);
            key(&app, SDLK_UP);
            key(&app, SDLK_RETURN);
            assert(!app.users_shown && app.user == 1 && !strcmp(app.user_name, "Aogo"));
            { /* the rows as the menu's (ui.txt's scene Users), 2 pixels apart, each picture inside its row's box at its
                 right; the pointer over a row its focus, a click picks its user */
                GuiUsersBoxes u;
                GuiRect aogo, xuji;
                int side;
                swipe_px(&app, b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2);
                render(&app);
                users_boxes(&app, &u);
                aogo = ui_row_box(&app, ui_users_row(u.rects, s, UI_USERS_ROWS, 0));
                xuji = ui_row_box(&app, ui_users_row(u.rects, s, UI_USERS_ROWS, 1));
                side = xuji.h - 4 * s;
                assert(xuji.y - aogo.y == item_h(&app) + 2 * s && xuji.h == item_h(&app) && app.users_side == side);
                assert(same_colour(pixel(&app, (float)(xuji.x + xuji.w / 2), (float)(xuji.y + xuji.h / 2)),
                                   rgb(app.theme.dim)) &&
                       !same_colour(
                           pixel(&app, (float)(xuji.x + xuji.w - 2 * s - side / 2), (float)(xuji.y + 2 * s + side / 2)),
                           rgb(app.theme.dim)));
                point(&app, xuji, false);
                assert(app.users_shown && app.users_focus == 1 && app.user == 1);
                render(&app);
                {
                    int x, y; /* the one the games play as, in the active colour */
                    assert(find_text(&app, "Aogo", rgb(app.theme.active), 1, &x, &y) && y > aogo.y &&
                           y < aogo.y + aogo.h);
                }
                mouse_px(&app, u.box.x + u.box.w / 2, u.box.y + 2, true); /* its head: nothing */
                assert(app.users_shown && app.users_focus == 1 && app.user == 1);
                point(&app, xuji, true);
                assert(!app.users_shown && app.user == 2);
                swipe_px(&app, b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2);
                point(&app, aogo, true);
                assert(!app.users_shown && app.user == 1);
            }
            {
                size_t size;
                char* kept = (char*)snx_read_file(app.settings_file, &size);
                assert(kept && strstr(kept, "[User]\nuser=1\n"));
                free(kept);
                /* back to no pick, as the runs below were written for: theirs has no --user */
                gui_settings_reset(&app.settings, setting_row(&app, "user"));
                gui_settings_sync(&app.settings);
                assert(!gui_settings_save(&app.settings, app.settings_file));
                kept = (char*)snx_read_file(app.settings_file, &size);
                assert(!kept || !strstr(kept, "user="));
                free(kept);
            }
            /* the theme editor's preview is the menu in the theme, drawn on a copy of the app put back after it: the
               box of users opened from the editor's head is not on it, as the pictures the copy made for it would go
               with the copy, and the app's own be destroyed under it */
            {
                int text_size = app.menu.text_size;
                Uint64 clock = app.clock;
                GuiPlace head;
                SDL_Surface *plain, *boxed;
                app.clock = 1;
                app.menu.text_size = 2; /* the preview's own scale is 1: its pictures are of another size */
                app_select(&app, GUI_APP_EDITOR);
                assert(app.editing && app.form == GUI_FORM_THEME && app.scale == 2);
                render(&app);
                plain = preview_pixels(&app);
                place_screen(&app, &head, "", GUI_SHOWS_TABS);
                swipe_px(&app, head.badge.x + head.badge.w / 2, head.badge.y + head.badge.h / 2,
                         head.badge.x + head.badge.w / 2, head.badge.y + head.badge.h / 2);
                assert(app.users_shown && app.editing && app.form == GUI_FORM_THEME);
                render(&app);
                boxed = preview_pixels(&app);
                assert(same_pixels(plain, boxed));
                /* the box shows over the editor itself, its pictures the app's at the size it draws them, render
                   after render */
                assert(find_text(&app, "Who plays the games", rgb(app.theme.page), 1, &nx, &ny));
                render(&app);
                for (int i = 0; i < g_user_count; ++i) {
                    float w = 0, h = 0;
                    assert(app.users_pictures[i] && SDL_GetTextureSize(app.users_pictures[i], &w, &h) &&
                           (int)w == app.users_side);
                }
                SDL_DestroySurface(plain);
                SDL_DestroySurface(boxed);
                key(&app, SDLK_ESCAPE);
                assert(!app.users_shown && app.editing && app.form == GUI_FORM_THEME);
                key(&app, SDLK_ESCAPE);
                assert(app.editing && app.form == GUI_FORM_APP);
                leave_editor(&app);
                app.menu.text_size = text_size;
                app.clock = clock;
                render(&app);
            }
            /* none: neither the box nor the nickname, and the title against the right again */
            app.theme.picture = GUI_PICTURE_NONE;
            render(&app);
            menu_boxes(&app, &m);
            assert(!m.place.badge.h && m.place.title.x + m.place.title.w == m.place.head.x + m.place.head.w);
            assert(!find_text(&app, "Aogo", rgb(app.theme.ink), 1, &nx, &ny));
            app.theme.picture = GUI_PICTURE_SQUARE;
            render(&app);
            menu_boxes(&app, &m);
            r = m.place.title;
        }
        assert(m.place.tabs.y + m.place.tabs.h == r.y + r.h && r.y + r.h <= m.place.list.y);
        for (int pass = 0; pass < 2; ++pass) {
            char shown[160];
            const unsigned char* g;
            int big, letters, shadowed = 0;
            app.theme.title_shadow = pass;
            render(&app);
            menu_boxes(&app, &m);
            r = m.place.title;
            head_title(&app, true, shown, sizeof shown, &letters); /* its first letter, at its size */
            g = &gui_vga_font[(unsigned char)shown[0] * 16];
            big = letters * s;
            for (int fy = 0; fy < 15; ++fy)
                for (int fx = 0; fx < 7; ++fx) {
                    int here = g[fy] & (0x80 >> fx), under = g[fy + 1] & (0x80 >> (fx + 1));
                    if (here) assert(same_colour(pixel(&app, (float)(r.x + fx * big), (float)(r.y + fy * big)), ink));
                    if (here && !under)
                        shadowed +=
                            same_colour(pixel(&app, (float)(r.x + (fx + 1) * big), (float)(r.y + (fy + 1) * big)), dim);
                }
            assert(pass ? shadowed > 4 : !shadowed);
        }
        app.theme.title_shadow = false;
        /* the head on a band of ink with its bar style, the title and the tabs in the page's colour; a rule under it */
        app.theme.head = GUI_STYLE_BAR;
        render(&app);
        menu_boxes(&app, &m);
        assert(same_colour(pixel(&app, (float)(m.place.head.x + m.place.head.w / 2), (float)(m.place.head.y)), ink));
        shows(&app, "Games", page);
        shows_big(&app, "SCARLETNX  /  MENU", page, 1); /* too wide big beside the user: whole, at the text's size */
        app.theme.head = GUI_STYLE_PLAIN;
        render(&app);
        assert(same_colour(pixel(&app, (float)(m.place.head.x + m.place.head.w / 2),
                                 (float)(m.place.head.y + m.place.head.h + 16 * s / 4 - s / 2)),
                           dim));
        /* the tips on a row of their own under the head, in their box with its bar; the keys alone at the foot, a rule
           over it; the footer's none leaves no Status box */
        app.theme.footer_rule = true;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.place.tips.y > m.place.title.y + m.place.title.h && m.place.list.y > m.place.tips.y + m.place.tips.h);
        assert(m.place.tips.w == m.place.head.w && m.place.keys.x == m.place.head.x &&
               m.place.keys.w == m.place.head.w);
        tips_px_is(&app, m.place.tips, "Installs prod.keys", ink, page);
        assert(same_colour(pixel(&app, (float)(m.place.tips.x + 3 * s), (float)(m.place.tips.y + m.place.tips.h / 2)),
                           ink));
        assert(same_colour(
            pixel(&app, (float)(m.place.keys.x + m.place.keys.w / 2), (float)(m.place.keys.y - 16 * s / 4 - s / 2)),
            rgb(app.theme.soft)));
        assert(m.place.status.h);
        app.theme.footer = GUI_STYLE_NONE;
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.place.status.h);
        app.theme = *gui_theme_builtin("yorha");
        app.theme.grid = false;
        render(&app);
        menu_boxes(&app, &m);
        /* with its tabs and big title, a 1280x800 window still holds the whole menu, and Games Add Folder, three
           recent games, the row for the rest and a page of games */
        {
            GuiRecent kept = app.recent;
            int n;
            app.recent.count = 4;
            for (n = 0; n < 4; ++n) {
                snprintf(app.recent.game[n].name, sizeof app.recent.game[n].name, "Game %d", n);
                snprintf(app.recent.game[n].path, sizeof app.recent.game[n].path, "/games/Game %d", n);
                app.recent.game[n].driver = GUI_SWITCH;
            }
            app.recent_all = false;
            recent_changed(&app);
            render(&app);
            menu_boxes(&app, &m);
            assert(ui_menu_shown(m.rects, app.scale, UI_MENU_ENTRIES) >= GUI_ITEM_COUNT);
            go_tab(&app, GUI_TAB_GAMES);
            {
                GuiGamesBoxes g;
                render(&app);
                games_boxes(&app, &g);
                assert(g.lead == 5 && ui_games_shown(g.rects, app.scale, UI_GAMES_ROWS) > (size_t)g.lead);
                shows(&app, "Recent Games", rgb(app.theme.soft));
                shows(&app, "\xfe Show More", rgb(app.theme.ink));
            }
            go_tab(&app, GUI_TAB_MENU);
            app.recent = kept;
            recent_changed(&app);
        }
        /* letters too big for the screen: the list scrolls, the focus in view, an arrow where entries are hidden. At
           768 high the menu takes 3x, its foot's letters 2x; 960 wide is under 45 columns (ui.txt's narrow_below),
           where side_box = on (F2) puts the side box under the list, three fifths of the height left to the list */
        assert(SDL_SetWindowSize(app.window, 960, 768));
        SDL_SyncWindow(app.window);
        app.menu.text_size = GUI_TEXT_MAX;
        app.menu.selected = GUI_EXIT;
        app.side_box = GUI_SIDE_ON;
        render(&app);
        menu_boxes(&app, &m);
        assert(app.scale == 3 && foot_scale(&app) == 2 && (960 - 2 * cell_w(&app)) / cell_w(&app) < 45);
        assert(m.place.stacked && m.place.side.y > m.place.list.y + m.place.list.h && m.place.side.w == m.place.list.w);
        assert(m.place.status.h && m.place.status.y > m.place.side.y + m.place.side.h);
        assert(SDL_abs(m.place.list.h * 2 - (m.place.side.h + 8 * app.scale + m.place.status.h) * 3) <= 5);
        {
            const UiList* l = &m.lists[UI_MENU_ENTRIES];
            size_t shown = ui_menu_shown(m.rects, app.scale, UI_MENU_ENTRIES);
            assert(shown < GUI_ITEM_COUNT && l->first > 0 && l->focus < l->first + shown);
            b = menu_entry_box(&app, &m, (int)l->first);
        }
        text_px_is(&app, b.x + b.w - 8 * app.scale, item_text_y(&app, b), "\x1e", rgb(app.theme.soft),
                   rgb(app.theme.panels == GUI_PANELS_FILL ? app.theme.dim : app.theme.page)); /* in its box */
        app.side_box = GUI_SIDE_AUTO;
        /* and with side_box = auto, the list alone: no side box, no status */
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.place.stacked && !m.place.side.h && !m.place.status.h && m.place.list.w == m.place.head.w);
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        app.menu.text_size = 0;
        app.theme = *gui_theme_builtin("default");
        render(&app);
    }
    /* ESC on the menu's first screen asks before the app closes: ESC again stays, Exit closes */
    assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished && !app.confirm.shown);
    key(&app, SDLK_ESCAPE);
    assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT && app.confirm.selected == GUI_CONFIRM_KEEP &&
           !app.menu.quit);
    render(&app);
    key(&app, SDLK_ESCAPE);
    assert(!app.confirm.shown && !app.menu.quit);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN); /* Exit: the screen of what is kept first, a second close at once */
    assert(!app.confirm.shown && app.closing_ms && !app.menu.quit);
    key(&app, SDLK_ESCAPE); /* taken by nothing meanwhile: no Exit? again */
    assert(app.closing_ms && !app.menu.quit && !app.confirm.shown);
    {
        SDL_Event close = {0};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event(&app, &close);
    }
    assert(app.menu.quit);
    app.menu.quit = false;
    app.closing_ms = 0;
    /* fingers: a tap takes an entry anywhere across its row, the cursor's margin too, as a click does (the cursor is
       drawn inside the row); a drag is no tap, nor a second finger; a swipe across turns the games' pages; back is
       ESC */
    {
        GuiMenuBoxes m;
        GuiRect b;
        SDL_Event second = {0};
        int margin, y, ch = 16 * app.scale;
        app.theme = *gui_theme_builtin("yorha"); /* a cursor's margin before the boxes */
        app.theme.grid = false;
        assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished && !app.editing);
        app.menu.selected = GUI_KEYS;
        render(&app);
        menu_boxes(&app, &m);
        b = entry_box(&app, GUI_SETTINGS);
        margin = m.rects[UI_MENU_ENTRIES].x + 2;
        y = b.y + b.h / 2;
        assert(margin < b.x);
        mouse_px(&app, margin, y, true);
        assert(app.editing && app.menu.selected == GUI_SETTINGS);
        key(&app, SDLK_ESCAPE);
        app.menu.selected = GUI_KEYS;
        tap_px(&app, margin, y);
        assert(app.editing && app.menu.selected == GUI_SETTINGS);
        key(&app, SDLK_ESCAPE);
        swipe_px(&app, b.x + 10, y, b.x + 10, y + 4 * ch);
        assert(!app.editing);
        second.type = SDL_EVENT_FINGER_DOWN;
        second.tfinger.fingerID = 8;
        event(&app, &second); /* the first finger is still down */
        assert(app.finger == 8);
        tap_px(&app, b.x + 10, y); /* finger 9: no finger of the screen's */
        assert(!app.editing);
        second.type = SDL_EVENT_FINGER_UP;
        second.tfinger.x = app.finger_x;
        second.tfinger.y = app.finger_y;
        event(&app, &second); /* finger 8 lifts where it went down, at the corner: nothing there */
        assert(!app.finger && !app.editing);
        tap_px(&app, b.x + 10, y);
        assert(app.editing);
        key(&app, SDLK_ESCAPE);
        touch_box(&app, entry_box(&app, GUI_SWITCH));
        assert(app.menu.view == GUI_LOAD_DROP);
        key(&app, SDLK_AC_BACK);
        assert(app.menu.view == GUI_LOAD_NONE);
        app.menu.view = GUI_LOAD_GAMES;
        app.browser = (GuiBrowser){.count = 1000};
        layout(&app);
        swipe_px(&app, 60 * 8 * app.scale, 10 * ch, 10 * 8 * app.scale, 11 * ch);
        assert(app.browser.page == 1);
        swipe_px(&app, 10 * 8 * app.scale, 10 * ch, 60 * 8 * app.scale, 10 * ch);
        assert(app.browser.page == 0);
        { /* up or down too, as a phone's lists scroll: up to the next page, down back; a short one turns none */
            int h, w;
            area(&app, &w, &h); /* the render's pixels, as swipe_px takes them */
            swipe_px(&app, 30 * 8 * app.scale, 3 * h / 4, 31 * 8 * app.scale, h / 4);
            assert(app.browser.page == 1);
            swipe_px(&app, 30 * 8 * app.scale, h / 4, 30 * 8 * app.scale, 3 * h / 4);
            assert(app.browser.page == 0);
            swipe_px(&app, 30 * 8 * app.scale, h / 2, 30 * 8 * app.scale, h / 2 - h / 20);
            assert(app.browser.page == 0);
            (void)w;
        }
        app.menu.view = GUI_LOAD_NONE;
        app.browser = (GuiBrowser){0};
        app.theme = *gui_theme_builtin("default");
    }
    /* Share Log off Android: scarletnx.log with the message and the last run's output, in the state folder of a data
       folder that keeps its kinds apart, else in the working folder */
    {
        char *saved = snx_path_join(temporary, "scarletnx.log"), *installed_dir = app.data_dir;
        char *portable = snx_path_join(temporary, ".scarletnx"), *kept = snx_path_join(portable, "state/scarletnx.log");
        size_t size = 0;
        char* text;
        SDL_free(app.cwd);
        app.cwd = SDL_strdup(temporary);
        snprintf(app.message, sizeof(app.message), "before the log");
        app.menu.selected = GUI_LOG;
#ifdef SNX_XDG
        /* an installed build's in its state folder, as the working folder is the home folder from the desktop */
        {
            char *state = snx_path_join(temporary, "state"), *logged = snx_path_join(state, "scarletnx/scarletnx.log");
            set_env("XDG_STATE_HOME", state);
            key(&app, SDLK_RETURN);
            assert(strstr(app.message, logged) && !snx_path_exists(saved));
            text = (char*)snx_read_file(logged, &size);
            assert(text && strstr(text, "Message: before the log"));
            free(text);
            free(logged);
            free(state);
        }
#endif
        /* a portable copy's .scarletnx/: its state/, on every system */
        app.data_dir = snx_strdup(portable);
        snprintf(app.message, sizeof(app.message), "before the log");
        key(&app, SDLK_RETURN);
        assert(strstr(app.message, "scarletnx.log") && snx_is_file(kept) && !snx_path_exists(saved));
        text = (char*)snx_read_file(kept, &size);
        assert(text && strstr(text, "Message: before the log"));
        free(text);
        /* any other build's (a checkout) in the working folder, with the newest game log's summary at its top: how the
           run ended, demangled, before the run's output and the log itself */
        free(app.data_dir);
        app.data_dir = snx_strdup(temporary);
        {
            char *game = snx_path_join(app.games_dir, "Some Game"), *log = snx_path_join(game, "Some Game.log");
            /* with CRLF lines, as a game log is on Windows (plat.c writes it in text mode) */
            static const char crashed[] =
                "[   0.00] ScarletNX - Some Game\r\n"
                "[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Windows x86_64)\r\n"
                "[   2.25] \r\n*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***\r\n"
                "[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>\r\n"
                "[   2.25]    #1 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>\r\n"
                "[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.\r\n";
            assert(!snx_mkdirs(game) && !snx_write_file(log, crashed, sizeof crashed - 1));
            snprintf(app.message, sizeof(app.message), "before the log");
            key(&app, SDLK_RETURN);
            assert(!strncmp(app.message, "Log saved to ", 13) && strstr(app.message, "scarletnx.log"));
            text = (char*)snx_read_file(saved, &size);
            assert(text && strstr(text, "Message: before the log") && strstr(text, "== the last run: "));
            assert(strstr(text, "== summary ==\n### `Some Game`") &&
                   strstr(text, "**Build:** `ScarletNX main 1234abc (clang 23.1.3, Windows x86_64)`") &&
                   strstr(text, "**Signature:** `svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv`") &&
                   strstr(text, "**In:** `nn::hid::GetNpadJoyHoldType()+38`"));
            assert(strstr(text, "== summary ==") < strstr(text, "== the last run: ") &&
                   !strstr(text, "The log's last lines"));
            /* the log's copy, CR gone: no line ends in the '?' a CR would have become */
            assert(strstr(text, "ScarletNX - Some Game\n") && !strstr(text, "?\n"));
            free(text);
            assert(!snx_remove_tree(game));
            free(log);
            free(game);
        }
        /* a working folder that is gone: the file it was to be and the system's reason */
        {
            char *cwd = app.cwd, *gone = snx_path_join(temporary, "gone"), *log = snx_path_join(gone, "scarletnx.log");
            char* said = snx_format("The log could not be saved to %s: ", log);
            app.cwd = gone;
            key(&app, SDLK_RETURN);
            assert(!strncmp(app.message, said, strlen(said)) && app.message[strlen(said)] && !snx_path_exists(log));
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ERROR && !strcmp(app.error_line, app.message));
            key(&app, SDLK_RETURN); /* in the box too, whole, until OK */
            app.cwd = cwd;
            free(said);
            free(log);
            free(gone);
        }
        free(app.data_dir);
        app.data_dir = installed_dir;
        assert(!snx_remove_tree(portable));
        free(kept);
        free(portable);
        free(saved);
    }
    /* Android's menu: its own files in its data folder's config/ and state/ (.scarletnx/ in the app's own files, which
       the Files app shows). An older version's, in the app's own files, are neither moved nor read: the menu starts
       from its defaults, and the box of what older versions left names each file with its size, then how they go while
       the app is in testing; Share Log too */
    {
        static const char* const olds[6][2] = {{"settings.ini", "[Display]\nfullscreen = 0\n"},
                                               {"app.ini", "[app]\ntheme = yorha\ntext_size = 2\n"},
                                               {"folders.txt", "/nonexistent/older folder\n"},
                                               {"gamecontrollerdb.txt", "older mappings\n"},
                                               {"recent.txt", ""},
                                               {"last-run.log", "$ an older run\n"}};
        char *old = snx_path_join(temporary, "own files"), *data = snx_path_join(old, SNX_PORTABLE_DIR);
        char *home = app.home_dir, *data_dir = app.data_dir, *full, *config, *state, *path, *text, *expected;
        char *settings_file = app.settings_file, *folders_file = app.folders_file, *app_file = app.app_file;
        char *recent_file = app.recent_file, *run_log = app.run_log;
        GuiRecent recent = app.recent;
        size_t head = app.browser.head, size;
        int i, j, text_size = app.menu.text_size, folder_count = app.folder_count;
        assert(!snx_mkdirs(data) && (full = snx_absolute(data)) != NULL);
        for (i = 0; i < 6; ++i) {
            path = snx_path_join(old, olds[i][0]);
            if (i == 4) /* a recent game whose file is there, which a read list would keep */
                text = snx_format("switch\tfile\t%s/app.ini\tolder game\n", old);
            else
                text = snx_strdup(olds[i][1]);
            assert(!snx_write_file(path, text, strlen(text)));
            free(text);
            free(path);
        }
        path = snx_path_join(old, "other.txt"); /* not the menu's */
        assert(!snx_write_file(path, "other", 5));
        free(path);
        /* the menu opens in its own folders alone: nothing read from the older ones, nothing written */
        app.home_dir = data;
        app.data_dir = full;
        app.menu_old = old;
        free(app.users_dir); /* the users test's: open_files names the new data folder's */
        app.users_dir = NULL;
        open_files(&app);
        config = menu_folder(&app, SNX_USER_CONFIG, false);
        state = menu_folder(&app, SNX_USER_STATE, false);
        expected = snx_path_join(full, "config");
        assert(config && !strcmp(config, expected) && snx_is_dir(config));
        free(expected);
        expected = snx_path_join(config, "app.ini");
        assert(!strcmp(app.app_file, expected) && !snx_path_exists(expected));
        free(expected);
        expected = snx_path_join(full, "state");
        assert(state && !strcmp(state, expected) && snx_is_dir(state));
        free(expected);
        expected = snx_path_join(state, "recent.txt");
        assert(!strcmp(app.recent_file, expected) && !snx_path_exists(expected));
        free(expected);
        assert(!strcmp(app.theme.id, "default") && app.menu.text_size == text_size && !app.recent.count &&
               app.folder_count == folder_count);
        /* the box: each file with its size, where this one keeps it for Share Log, then how they go */
        left_behind(&app);
        assert(app.left_clear);
        for (i = 0; i < 6; ++i) {
            path = snx_path_join(old, olds[i][0]);
            expected = snx_format("%s" SAY_LEFT_BY "%s\n", path, i < 4 ? config : state);
            assert(strstr(app.left, expected));
            free(expected);
            free(path);
        }
        assert(!strstr(app.left, "other.txt"));
        while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
        for (i = 0; i < 6; ++i) {
            path = snx_path_join(old, olds[i][0]);
            for (j = 0; j < app.left_space->count && strcmp(app.left_space->dirs[j], path); ++j) {}
            assert(j < app.left_space->count && app.left_space->bytes[j] == snx_file_size(path));
            free(path);
        }
        app.left_shown = true;
        render(&app);
        shows(&app, SAY_LEFT_TITLE, rgb(app.theme.ink));
        shows(&app, "While the app is in testing,", rgb(app.theme.ink));
        app.left_shown = false;
        app.log_due = true; /* a crash's log: Share Log blinks in the failure colour until it is saved */
        assert((ui_menu_entries_attr_cb(&app, GUI_LOG) & (UI_ATTR_FAILURE | UI_ATTR_BLINK)) ==
               (UI_ATTR_FAILURE | UI_ATTR_BLINK));
        share_log(&app);
        assert(!app.log_due && !(ui_menu_entries_attr_cb(&app, GUI_LOG) & UI_ATTR_FAILURE));
        path = snx_path_join(state, "scarletnx.log");
        text = (char*)snx_read_file(path, &size);
        assert(text && strstr(text, SAY_LEFT_CLEAR "\n"));
        free(text);
        free(path);
        /* every older file where it was, as it was */
        for (i = 0; i < 6; ++i) {
            path = snx_path_join(old, olds[i][0]);
            text = (char*)snx_read_file(path, &size);
            assert(text && (i == 4 ? strstr(text, "older game") != NULL : !strcmp(text, olds[i][1])));
            free(text);
            free(path);
        }
        /* elsewhere none are looked for */
        app.menu_old = NULL;
        left_behind(&app);
        assert(!app.left_clear && (!app.left || !strstr(app.left, old)));
        free(app.settings_file);
        free(app.folders_file);
        free(app.app_file);
        free(app.recent_file);
        free(app.run_log);
        app.settings_file = settings_file;
        app.folders_file = folders_file;
        app.app_file = app_file;
        app.recent_file = recent_file;
        app.run_log = run_log;
        app.recent = recent;
        app.browser.head = head;
        app.message[0] = 0;
        app.home_dir = home;
        app.data_dir = data_dir;
        left_behind(&app);
        assert(!snx_remove_tree(old));
        free(state);
        free(config);
        free(full);
        free(data);
        free(old);
    }
#ifdef SNX_XDG
    /* the menu's recent.txt and last-run.log in the state folder; an older build's in the data folder's gui/ stay */
    {
        char *data = snx_path_join(temporary, "data"), *state = snx_path_join(temporary, "state");
        char *old = snx_path_join(data, "scarletnx/gui"), *recent = snx_path_join(old, "recent.txt");
        char *run = snx_path_join(old, "last-run.log"), *folder, *expected = snx_path_join(state, "scarletnx"), *now;
        assert(!snx_mkdirs(old) && !snx_write_file(recent, "recent", 6) && !snx_write_file(run, "run", 3));
        set_env("XDG_DATA_HOME", data);
        folder = menu_folder(&app, SNX_USER_STATE, true);
        assert(folder && !strcmp(folder, expected) && snx_file_size(recent) == 6 && snx_file_size(run) == 3);
        now = snx_path_join(folder, "recent.txt");
        assert(!snx_path_exists(now) && snx_left_behind(recent, now));
        free(now);
        free(folder);
        /* the menu says, a folder at a time, what an older build left where this one no longer looks: its own files'
           gui/, work/, each game laid out beside the others (its folder now by its title ID, else by its name), until
           they are gone */
#define LONG "Foo, a game whose name is longer than a row of the box has room for"
        {
            char *installed_dir = app.data_dir, *games = app.games_dir, *loose, *loose_data, *named, *npdm, *work,
                 *line;
            char* config = snx_path_join(temporary, "config");
            char* cache = snx_path_join(temporary, "cache");
            char* lines[5][2];
            unsigned char meta[0xa0];
            const char* at;
            int i;
            set_env("XDG_CONFIG_HOME", config);
            set_env("XDG_CACHE_HOME", cache);
            app.data_dir = snx_path_join(data, "scarletnx");
            app.games_dir = snx_recompiled_dir(app.data_dir);
            app.recent_file = snx_path_join(expected, "recent.txt");
            loose = snx_path_join(app.games_dir, LONG);
            loose_data = snx_format("%s_data", loose);
            named = snx_path_join(app.games_dir, "MyGame");
            npdm = snx_path_join(app.games_dir, "MyGame_data/exefs/main.npdm");
            work = snx_path_join(app.data_dir, "work");
            memset(meta, 0, sizeof meta); /* an NPDM whose ACI0 at 0x80 gives the game its title ID */
            memcpy(meta, "META", 4);
            meta[0x70] = 0x80;
            meta[0x74] = 0x20;
            memcpy(meta + 0x80, "ACI0", 4);
            for (i = 0; i < 8; ++i) meta[0x90 + i] = (unsigned char)(0x0100ABCD12345000ull >> (8 * i));
            line = snx_dirname(npdm);
            assert(!snx_mkdirs(work) && !snx_mkdirs(loose_data) && !snx_mkdirs(line) && !snx_write_file(loose, "", 0));
            {
                char* big = snx_path_join(work, "big");
                void* zeros = calloc(1, 3u << 20);
                assert(zeros && !snx_write_file(big, zeros, 3u << 20));
                free(zeros);
                free(big);
            }
            assert(!snx_write_file(named, "", 0) && !snx_write_file(npdm, meta, sizeof meta));
            free(line);
            lines[0][0] = snx_strdup(old); /* the menu's gui/, its recent games and last run in the state folder */
            lines[0][1] = snx_strdup(expected);
            lines[1][0] = snx_strdup(work);
            lines[1][1] = snx_format("%s/scarletnx/work", cache);
            lines[2][0] = snx_strdup(loose);
            lines[2][1] = snx_format("%s/" LONG "/", app.games_dir);
            lines[3][0] = snx_strdup(named);
            lines[3][1] = snx_format("%s/0100ABCD12345000/", app.games_dir);
            lines[4][0] = snx_path_join(app.drivers, "games"); /* a portable copy's games/ beside the program */
            lines[4][1] = snx_format("%s/.scarletnx/recompiled", app.drivers);
            left_behind(&app);
            assert(app.lefts == 5 && snx_is_dir(lines[4][0]));
            for (i = 0, at = app.left; i < 5; ++i, at = strchr(at, '\n') + 1) {
                line =
                    snx_format("%s was left by an older version: this one keeps it in %s\n", lines[i][0], lines[i][1]);
                assert(!strncmp(at, line, strlen(line)));
                free(line);
                free(lines[i][0]);
                free(lines[i][1]);
            }
            /* gui/'s gamecontrollerdb.txt, where #276 read it, goes to the config folder as settings.ini does */
            {
                char *db = snx_path_join(old, "gamecontrollerdb.txt"),
                     *said = snx_format("%s" SAY_LEFT_BY "%s/scarletnx\n", old, config);
                assert(!snx_write_file(db, "", 0));
                left_behind(&app);
                assert(app.lefts == 6 && strstr(app.left, said));
                assert(!snx_remove_tree(db));
                left_behind(&app);
                assert(app.lefts == 5);
                free(said);
                free(db);
            }
            /* a build tree's menu: its checkout's games/, work/ and keys, where an older version kept them, each where
               this one does */
            {
                char *checkout = snx_path_join(temporary, "checkout"), *old_games = snx_path_join(checkout, "games");
                char *old_work = snx_path_join(checkout, "work"), *keys = snx_path_join(checkout, "prod.keys"),
                     *said[3];
                assert(!snx_mkdirs(old_games) && !snx_mkdirs(old_work) && !snx_write_file(keys, "", 0));
                said[0] = snx_format("%s" SAY_LEFT_BY "%s\n", old_games, app.games_dir);
                said[1] = snx_format("%s" SAY_LEFT_BY "%s/scarletnx/work\n", old_work, cache);
                said[2] = snx_format("%s" SAY_LEFT_BY "%s/prod.keys\n", keys, app.data_dir);
                app.checkout = checkout;
                left_behind(&app);
                assert(app.lefts == 8);
                /* a list that changed is counted again */
                while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
                assert(app.left_space->count == 8 && !strcmp(app.left_space->dirs[7], keys));
                for (i = 0; i < 3; ++i) {
                    assert(strstr(app.left, said[i]));
                    free(said[i]);
                }
                app.checkout = NULL;
                left_behind(&app);
                assert(app.lefts == 5);
                assert(!snx_remove_tree(checkout));
                free(keys);
                free(old_work);
                free(old_games);
                free(checkout);
            }
            /* in a box over the menu as it opens: each where it is (-), a path too long for a row going on in the next,
               and under it the space it takes, counted on a thread (a game laid out beside the others with its files
               beside it), as many as fit */
            {
                int width, wraps = 0, selected = app.menu.selected, more, n;
                SDL_Color ink = rgb(app.theme.ink), added = rgb(app.theme.added), failure = rgb(app.theme.failure);
                SDL_Color soft = rgb(app.theme.soft);
                static char paths[2][4200]; /* where each file is, and where it goes */
                while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
                assert(app.left_space->count == 5 && app.left_space->bytes[0] == 9 &&
                       app.left_space->bytes[1] == 3u << 20 && app.left_space->bytes[2] == 0 &&
                       app.left_space->bytes[3] == sizeof meta);
                int window_w, window_h, text_size = app.menu.text_size;
                app.left_shown = true;
                app.clock = 1;
                /* tall enough for all five, whatever the temporary folder's path, at the letters' size of now */
                assert(SDL_GetWindowSize(app.window, &window_w, &window_h));
                app.menu.text_size = app.scale;
                assert(SDL_SetWindowSize(app.window, window_w, 30 * 16 * app.scale));
                SDL_SyncWindow(app.window);
                render(&app);
                width = app.cols - 6; /* the box's columns, as render_left takes them */
                assert(app.rows >= 28 && width >= 57 && !same_colour(added, ink));
                shows(&app, SAY_LEFT_TITLE, ink);
                for (at = app.left; *at; at = strchr(at, '\n') + 1) {
                    left_paths(at, paths[0], paths[1], sizeof paths[0]);
                    for (const char* rest = paths[0]; *rest;) { /* the whole path, a piece a row */
                        size_t cut = gui_wrap(rest, width - 2);
                        line = snx_format("%.*s", (int)cut, rest);
                        shows(&app, line, failure);
                        wraps += rest != paths[0];
                        free(line);
                        rest += cut;
                    }
                    hides(&app, paths[1], ink); /* where it goes is Share Log's */
                }
                assert(wraps >= 1); /* the long game's path wraps */
                shows(&app, "1 KB", soft);
                shows(&app, "3 MB", soft);
                shows(&app, "0 MB", soft);
                shows(&app, "- OK", yellow);
                hides(&app, "which Share Log lists", rgb(app.theme.soft));
                /* a window too short for them all: as many as fit, then how many more */
                assert(SDL_SetWindowSize(app.window, window_w, 14 * 16 * app.scale));
                SDL_SyncWindow(app.window);
                render(&app);
                for (more = 0, n = 1; n < 5; ++n) {
                    char said[64];
                    int x, y;
                    snprintf(said, sizeof said, "and %d more, which Share Log lists", n);
                    if (find_text(&app, said, rgb(app.theme.soft), 1, &x, &y)) more = n;
                }
                assert(more > 0);
                shows(&app, "- OK", yellow);
                hides(&app, "\x1e", rgb(app.theme.soft));
                /* Up/Down go over them, a file at a time, an arrow beside the first saying some are above it; down
                   only while some are under; PgUp/PgDn as many as show, the wheel a file a notch */
                {
                    int top;
                    const char* last = strrchr(app.left, '\n');
                    while (last > app.left && last[-1] != '\n') --last; /* the last file's line */
                    left_paths(last, paths[0], paths[1], sizeof paths[0]);
                    line = snx_format("%.*s", (int)gui_wrap(paths[0], width - 2), paths[0]);
                    assert(app.files_top == 0 && app.files_shown == app.lefts - more);
                    key(&app, SDLK_UP);
                    assert(app.left_shown && app.files_top == 0);
                    key(&app, SDLK_DOWN);
                    assert(app.left_shown && app.files_top == 1 && app.menu.selected == selected);
                    render(&app);
                    shows(&app, "\x1e", rgb(app.theme.soft));
                    for (n = 0; n < 10 && app.files_top + app.files_shown < app.lefts; ++n) {
                        key(&app, SDLK_DOWN);
                        render(&app);
                    }
                    assert(app.files_top + app.files_shown == app.lefts && app.files_top > 0);
                    hides(&app, "which Share Log lists", rgb(app.theme.soft));
                    shows(&app, line, failure);
                    free(line);
                    top = app.files_top;
                    key(&app, SDLK_DOWN);
                    render(&app);
                    assert(app.files_top == top);
                    key(&app, SDLK_PAGEUP);
                    assert(app.files_top == SDL_max(0, top - app.files_shown));
                    key(&app, SDLK_PAGEDOWN);
                    assert(app.files_top == SDL_max(0, top - app.files_shown) + app.files_shown);
                    key(&app, SDLK_HOME); /* another key closes it, and does nothing else */
                    assert(!app.left_shown && app.menu.selected == selected);
                    app.left_shown = true;
                    app.files_top = 0;
                    render(&app);
                    {
                        SDL_Event wheel = {0};
                        wheel.type = SDL_EVENT_MOUSE_WHEEL;
                        wheel.wheel.integer_y = -1;
                        event(&app, &wheel);
                        assert(app.left_shown && app.files_top == 1 && app.menu.selected == selected);
                        wheel.wheel.integer_y = 3;
                        event(&app, &wheel);
                        assert(app.files_top == 0);
                    }
                }
                app.menu.text_size = text_size;
                assert(SDL_SetWindowSize(app.window, window_w, window_h));
                SDL_SyncWindow(app.window);
                layout(&app);
                app.clock = 0;
                /* the wheel moves nothing under it, and leaves it open */
                {
                    SDL_Event wheel = {0};
                    wheel.type = SDL_EVENT_MOUSE_WHEEL;
                    wheel.wheel.integer_y = -2;
                    event(&app, &wheel);
                    assert(app.left_shown && app.menu.selected == selected);
                }
                /* another key closes it and does nothing else; nothing was moved */
                key(&app, SDLK_RIGHT);
                assert(!app.left_shown && app.menu.selected == selected && snx_is_file(loose));
                /* Clean Old Settings cleans settings only: what an older version left stays where it is */
                clean_old(&app);
                assert(snx_is_file(loose) && snx_is_dir(loose_data) && snx_is_file(npdm) && snx_is_dir(work));
                left_behind(&app);
                assert(app.lefts == 5);
                gui_cleaned_free(&app.cleaned);
                app.cleaned_shown = false;
                /* gone while it shows, it closes: the data folder's, and the program in a folder with no games/ */
                {
                    char* drivers = app.drivers;
                    app.drivers = snx_path_join(temporary, "program alone");
                    app.left_shown = true;
                    assert(!snx_remove_tree(app.data_dir)); /* its gui/ too */
                    app.left_at = SDL_GetTicks();           /* pump looks again in 5 s; F5 now */
                    key(&app, SDLK_F5);
                    assert(!app.left_shown && !app.lefts && !app.left);
                    app.left_shown = true;
                    app.left_at = 0;
                    pump(&app);
                    assert(!app.left_shown && !app.lefts && !app.left);
                    free(app.drivers);
                    app.drivers = drivers;
                }
            }
            set_env("XDG_CONFIG_HOME", "/nonexistent/snx-gui-test/config");
            set_env("XDG_CACHE_HOME", "/nonexistent/snx-gui-test/cache");
            free(app.recent_file);
            app.recent_file = NULL;
            free(app.games_dir);
            app.games_dir = games;
            free(app.data_dir);
            app.data_dir = installed_dir;
            free(work);
            free(npdm);
            free(named);
            free(loose_data);
            free(loose);
            free(cache);
            free(config);
        }
#undef LONG
        /* and an empty state folder is made for the first run */
        assert(!snx_remove_tree(state));
        folder = menu_folder(&app, SNX_USER_STATE, true);
        assert(folder && snx_is_dir(folder));
        free(folder);
        set_env("XDG_DATA_HOME", "/nonexistent/snx-gui-test");
        free(expected);
        free(run);
        free(recent);
        free(old);
        free(state);
        free(data);
    }
#endif
    /* the system's picker, as the Android menu opens it for a file to load or for the keys: a document with no path
       stays on the screen, and so does none, saying so, for the screen's messages to be read; a path loads */
    app.picked = SDL_RegisterEvents(1);
    assert(app.picked);
    app.menu.selected = GUI_KEYS;
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    pick(&app, NULL);
    assert(app.menu.view == GUI_LOAD_KEYS && !strcmp(app.message, "No file was picked."));
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    app.menu.selected = GUI_SWITCH;
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_DROP);
    pick(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running && app.message[0]);
    pick(&app, NULL);
    assert(app.menu.view == GUI_LOAD_DROP && !strcmp(app.message, "No file was picked."));
    {
        char *got = snx_path_join(temporary, "driver argv"), *argv_got;
        driver_will(got, 0, "Recompiling game with spaces");
        pick(&app, input); /* not made: its first compile */
        assert(launched(&app) && app.queue.job[0].console == GUI_SWITCH);
        argv_got = ran(&app, 0);
        assert(has_arg(argv_got, "-r", input));
        free(argv_got);
        driver_will(NULL, 0, NULL);
        free(got);
    }
    button_map(&app);
    /* the button map in the theme's boxes: the tabs, the title big, the pad drawn in the box between it and the bar,
       which says the note a map it cannot read leaves */
    {
        GuiPlace p;
        GuiRect r;
        char keys[256];
        const SnxPadShape* first;
        int s = app.scale, cw = 8 * s, page = -1;
        app.theme = *gui_theme_builtin("default");
        for (int i = 0; i < app.settings.count; ++i)
            if (app.settings.values[i].kind == GUI_SETTING_PAGE) page = i;
        open_settings(&app);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        app.editor.row = page - app.first;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_PAD && app.editor.note[0]); /* the map it cannot read, still set */
        render(&app);
        place_screen(&app, &p, shown_keys(&app, keys, sizeof keys), GUI_SHOWS_TABS); /* the button map's */
        r = inner(&app, p.list);
        first = drawn(&app, 0, SNX_PAD_SHAPE_BOX, -1);
        assert(first && first->x >= (float)r.x && first->y >= (float)r.y && first->y < (float)(r.y + r.h));
        text_px_is(&app, tab_rect(&app, p.tabs, GUI_TAB_SETTINGS).x + cw, p.tabs.y + 2 * s, "Settings", yellow, black);
        {
            char start[24];
            snprintf(start, sizeof start, "%s", app.editor.note); /* as far as the tips' first row shows it */
            tips_px_is(&app, p.tips, start, red, black);
        }
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_GROUP);
        leave_editor(&app);
    }
    players_view(&app);
    icons_named();
    step_line();
    /* YoRHa's Settings: its groups' column wide enough for the last row with its cursor; a page of a group open, its
       name over its rows */
    {
        static const char list[] = "Display\tfullscreen\t0\tint\t0\t1\tStart in fullscreen\tFullscreen\t0=Off|1=On\n"
                                   "Display/Window\tscale\t1\tint\t1\t4\tThe window's scale\tScale\t\n";
        GuiFormBoxes f;
        GuiRect b;
        int s = app.scale, cw = 8 * s, row;
        settings_forget(&app);
        assert(!gui_settings_read(&app.settings, list));
        gui_settings_mark(&app.settings);
        app.theme = *gui_theme_builtin("yorha");
        app.theme.grid = false;
        open_settings(&app);
        app.editor.row = app.groups - 1; /* the last group */
        render(&app);
        form_boxes(&app, &f);
        b = form_row_box(&app, &f, false, app.groups - 1);
        {
            char last[80], glyph[4];
            snprintf(last, sizeof last, "\xfe %s %s",
                     gui_icon_utf8(group_icon(app.group_keys[app.groups - 1].key), glyph),
                     app.group_keys[app.groups - 1].key);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), last, rgb(app.theme.page), rgb(app.theme.focus));
        }
        for (row = 0; row < app.groups && strcmp(app.group_keys[row].key, "Display"); ++row) {}
        app.editor.row = row;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_GROUP && app.editor.row == 0);
        /* a setting not picked: its default and [default], as the lines show it */
        {
            char shown[96], line[200];
            render(&app);
            form_boxes(&app, &f);
            gui_settings_shown(&app.settings, app.first, shown, sizeof shown);
            assert(!strcmp(shown, "Off [default]"));
            snprintf(line, sizeof line, "\xfe %-*s %s", form_labels(&app.editor), "Fullscreen", shown);
            b = form_row_box(&app, &f, true, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), line, rgb(app.theme.page), rgb(app.theme.focus));
            /* the cursor (YoRHa's "► ", CP437's 0x10) beside it; the group open on the focus's band, without the
               cursor: the keys are not there */
            assert(!strcmp(app.theme.cursor, "\xe2\x96\xba ") && app.theme.focus_bar);
            text_px_is(&app, b.x - 2 * cw, item_text_y(&app, b), "\x10 ", rgb(app.theme.focus), rgb(app.theme.page));
            b = form_row_box(&app, &f, false, row);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe " ICON_MONITOR " Display", rgb(app.theme.page),
                       rgb(app.theme.focus));
            text_px_is(&app, b.x - 2 * cw, item_text_y(&app, b), "  ", rgb(app.theme.focus), rgb(app.theme.page));
        }
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(app.page >= 0);
        render(&app);
        form_boxes(&app, &f);
        text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, app.settings.keys[app.page].key,
                   rgb(app.theme.page), rgb(app.theme.ink));
        leave_editor(&app);
        app.theme = *gui_theme_builtin("default");
    }
    recompile_queue(&app);
#ifdef SNX_XDG /* the last of the test: SDL keeps the picker it found from then on */
    pickers_asked(&app, temporary, self);
#endif
    e.type = SDL_EVENT_QUIT;
    event(&app, &e);
    assert(app.closing_ms && !app.menu.quit);
    event(&app, &e);
    assert(app.menu.quit);
    cleanup(&app);
    assert(!snx_remove_tree(temporary));
    free(temporary);
    assert(!snx_remove_tree(own_home));
    free(own_home);
    free(self);
    free(input);
    puts("SDL3 GUI: keyboard, font rendering, drops, dispatch and return-to-menu passed");
    return 0;
}
