/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/error_sdl.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/fatal_screen.h"
#include "common/utilities/notice.h"
#include "common/utilities/ui_rects.h"
#include "common/utilities/vpad_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUADS 4096 /* a panel's: its letters and fills, a long message's rows on a wide window */
#define PADS  8

static void say(const SnxErrorSdlHooks* hooks, const char* line) {
    if (hooks && hooks->say)
        hooks->say(line);
    else
        fprintf(stderr, "%s\n", line);
}

/* said, as the panel shows it: each home folder as ~ (fatal_shown), since a photo of the screen would name the player;
   the whole path stays in the file's name and on the clipboard */
static void said_set(SnxErrorPanel* p, const SnxErrorSdlHooks* hooks, const char* said) {
    char* shown = fatal_shown(said);
    say(hooks, said);
    snprintf(p->said, sizeof p->said, "%s", shown ? shown : said);
    free(shown);
}

/* the actions that keep the panel: done here, said on it */
static void act(SnxErrorPanel* p, const SnxError* e, int action, const SnxErrorSdlHooks* hooks) {
    char said[1400], url[3608], made[1300];
    if (action == SNX_ACTION_SHARE_LOG) {
        char* folder = snx_dirname(e->log);
        int ok = folder && !fatal_share_log(folder, snx_build_log_line(), e->message, e->log, made, sizeof made);
        if (!ok)
            snprintf(said, sizeof said, "The log could not be saved: %s", folder ? made : e->log);
        else if (hooks && hooks->share && !hooks->share(made))
            snprintf(said, sizeof said, "Log saved to %s, but nothing took it to share.", made);
        else
            snprintf(said, sizeof said, "Log saved to %s.", made);
        free(folder);
    } else if (action == SNX_ACTION_COPY_TEXT) {
        char* text = snx_format("%s\n%s\nLog: %s", e->message[0] ? e->message : snx_error_line(e), snx_build_log_line(),
                                e->log[0] ? e->log : "none");
        if (text && SDL_SetClipboardText(text))
            snprintf(said, sizeof said, "Copied: the error, the build and the log's place.");
        else
            snprintf(said, sizeof said, "The text could not be copied: %s", text ? SDL_GetError() : "out of memory");
        free(text);
    } else if (action == SNX_ACTION_OPEN_LOG || action == SNX_ACTION_OPEN_FOLDER) {
        const char* path = action == SNX_ACTION_OPEN_LOG ? e->log : e->folder;
        if (!fatal_log_url(path, url, sizeof url))
            snprintf(said, sizeof said, "%s cannot be opened: it is no whole path", path);
        else if (!SDL_OpenURL(url))
            snprintf(said, sizeof said, "%s could not be opened: %s", path, SDL_GetError());
        else
            snprintf(said, sizeof said, "Opened %s.", path);
    } else
        return;
    said_set(p, hooks, said);
}

/* the renderer: software where the video driver gives a window pixels of its own, else SDL's choice */
static SDL_Renderer* renderer_make(SDL_Window* window) {
    const char* driver = SDL_GetCurrentVideoDriver();
    SDL_Renderer* r = NULL;
    if (driver && (!strcmp(driver, "x11") || !strcmp(driver, "windows"))) {
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
        r = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    }
    return r ? r : SDL_CreateRenderer(window, NULL);
}

/* a pad's button as the panel takes it, -1 for none */
static int pad_key(Uint8 button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return SNX_ERROR_KEY_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return SNX_ERROR_KEY_DOWN;
        case SDL_GAMEPAD_BUTTON_SOUTH: return SNX_ERROR_KEY_DO;
        case SDL_GAMEPAD_BUTTON_EAST: return SNX_ERROR_KEY_BACK;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return SNX_ERROR_KEY_PAGE_UP;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return SNX_ERROR_KEY_PAGE_DOWN;
        default: return -1;
    }
}
static int key_key(const SDL_KeyboardEvent* k) {
    switch (k->key) {
        case SDLK_UP: return SNX_ERROR_KEY_UP;
        case SDLK_DOWN: return SNX_ERROR_KEY_DOWN;
        case SDLK_TAB: return k->mod & SDL_KMOD_SHIFT ? SNX_ERROR_KEY_UP : SNX_ERROR_KEY_DOWN;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: return SNX_ERROR_KEY_DO;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK: return SNX_ERROR_KEY_BACK;
        case SDLK_PAGEUP: return SNX_ERROR_KEY_PAGE_UP;
        case SDLK_PAGEDOWN: return SNX_ERROR_KEY_PAGE_DOWN;
        default: return -1;
    }
}
/* a pad's labels for do and back, as its face shows them */
static void pad_names(SnxErrorPanel* p, SDL_Gamepad* pad) {
    static const char* const labels[] = {"?", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    SDL_GamepadButtonLabel south = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    SDL_GamepadButtonLabel east = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST);
    if (south > 0 && (size_t)south < sizeof labels / sizeof *labels)
        snprintf(p->pad_do, sizeof p->pad_do, "%s", labels[south]);
    if (east > 0 && (size_t)east < sizeof labels / sizeof *labels)
        snprintf(p->pad_back, sizeof p->pad_back, "%s", labels[east]);
}

int snx_error_sdl_run(SDL_Window* window, SnxError* e, const SnxErrorSdlHooks* hooks, char* why, size_t why_size) {
    SnxUiQuad* quads = malloc(QUADS * sizeof *quads);
    SnxUiRects rects = {0};
    SnxErrorPanel panel = {.back_key = hooks && hooks->back_key};
    SDL_Gamepad* pads[PADS] = {0};
    SDL_Renderer* r = quads ? renderer_make(window) : NULL;
    SDL_FingerID finger = 0;
    int action = SNX_ACTION_NONE, button = -1, fingered = 0, stick = 0;
    float finger_y = 0;
    char line[160];
    if (!r) {
        snprintf(why, why_size, "no renderer for the error's panel: %s", quads ? SDL_GetError() : "out of memory");
        free(quads);
        return -1;
    }
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    panel.fingers = 1;
#endif
    panel.percent = snx_display_percent(SDL_GetWindowDisplayScale(window));
    panel.open_log = !panel.fingers; /* a phone has no viewer that takes a file's path: Share Log sends it */
    snx_error_panel_start(&panel, SDL_GetTicks());
    SDL_SetWindowRelativeMouseMode(window, false);
    snprintf(line, sizeof line, "error: the panel shows on SDL's %s renderer", SDL_GetRendererName(r));
    say(hooks, line);
    while (action == SNX_ACTION_NONE) {
        SDL_Event ev;
        SnxUiList l = {quads, 0, QUADS};
        int w = 0, h = 0, k;
        float x, y;
        SDL_GetCurrentRenderOutputSize(r, &w, &h);
        while (action == SNX_ACTION_NONE && SDL_PollEvent(&ev)) {
            Uint64 now = SDL_GetTicks();
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: action = SNX_ACTION_CLOSE; break;
                case SDL_EVENT_KEY_DOWN:
                    if (!ev.key.repeat && (k = key_key(&ev.key)) >= 0)
                        action = snx_error_panel_key(&panel, e, k, SNX_ERROR_INPUT_KEYS, now);
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    for (int i = 0; i < PADS; ++i)
                        if (!pads[i]) {
                            pads[i] = SDL_OpenGamepad(ev.gdevice.which);
                            if (pads[i]) pad_names(&panel, pads[i]);
                            break;
                        }
                    break;
                /* a button counts as it comes up, after it went down on the panel: one held from the game does not */
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: button = ev.gbutton.button; break;
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (ev.gbutton.button == button && (k = pad_key(ev.gbutton.button)) >= 0)
                        action = snx_error_panel_key(&panel, e, k, SNX_ERROR_INPUT_PAD, now);
                    button = -1;
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION: /* the left stick as the D-pad, a step each time it leaves the middle */
                    if (ev.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) break;
                    if (stick == 0 && (ev.gaxis.value < -16000 || ev.gaxis.value > 16000)) {
                        stick = ev.gaxis.value < 0 ? -1 : 1;
                        action = snx_error_panel_key(&panel, e, stick < 0 ? SNX_ERROR_KEY_UP : SNX_ERROR_KEY_DOWN,
                                                     SNX_ERROR_INPUT_PAD, now);
                    } else if (ev.gaxis.value > -8000 && ev.gaxis.value < 8000)
                        stick = 0;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.which == SDL_TOUCH_MOUSEID || ev.button.button != SDL_BUTTON_LEFT ||
                        !SDL_RenderCoordinatesFromWindow(r, ev.button.x, ev.button.y, &x, &y))
                        break;
                    action = snx_error_panel_press(&panel, e, x, y, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, 0, now);
                    break;
                case SDL_EVENT_MOUSE_WHEEL: snx_error_panel_scroll(&panel, ev.wheel.y > 0 ? -1 : 1); break;
                case SDL_EVENT_FINGER_DOWN:
                    if (fingered) break;
                    fingered = 1;
                    finger = ev.tfinger.fingerID;
                    finger_y = ev.tfinger.y * (float)h;
                    action = snx_error_panel_press(&panel, e, ev.tfinger.x * (float)w, finger_y, 1, 1, now);
                    break;
                case SDL_EVENT_FINGER_MOTION: /* a drag on Details scrolls the message a row each row's height */
                    if (fingered && ev.tfinger.fingerID == finger && panel.details) {
                        float dy = ev.tfinger.y * (float)h - finger_y,
                              row = (float)(16 * snx_menu_scale(w, h, panel.percent));
                        if (dy <= -row || dy >= row) {
                            snx_error_panel_scroll(&panel, dy < 0 ? 1 : -1);
                            finger_y = ev.tfinger.y * (float)h;
                            panel.pressed = -1; /* a drag is no tap */
                        }
                    }
                    break;
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_CANCELED:
                    if (!fingered || ev.tfinger.fingerID != finger) break;
                    fingered = 0;
                    if (ev.type == SDL_EVENT_FINGER_UP)
                        action = snx_error_panel_press(&panel, e, ev.tfinger.x * (float)w, ev.tfinger.y * (float)h, 0,
                                                       1, now);
                    else
                        panel.pressed = -1;
                    break;
                case SDL_EVENT_RENDER_DEVICE_LOST: /* a renderer of the GPU's whose device went: another one */
                    SDL_DestroyRenderer(r);
                    r = renderer_make(window);
                    if (!r) {
                        snprintf(why, why_size, "the error panel's renderer was lost: %s", SDL_GetError());
                        action = -1;
                    }
                    break;
                default: break;
            }
            if (action == SNX_ACTION_SHARE_LOG || action == SNX_ACTION_COPY_TEXT || action == SNX_ACTION_OPEN_LOG ||
                action == SNX_ACTION_OPEN_FOLDER) {
                act(&panel, e, action, hooks);
                action = SNX_ACTION_NONE;
            }
        }
        if (action != SNX_ACTION_NONE) break;
        snx_error_panel_draw(&panel, e, &l, w, h);
        if (!snx_ui_rects(&rects, &l, w, h)) {
            snprintf(why, why_size, "out of memory for the error panel's rectangles");
            action = -1;
            break;
        }
        SDL_SetRenderDrawColor(r, 0x14, 0x14, 0x16, 255); /* where the game's picture was */
        SDL_RenderClear(r);
        snx_ui_rects_render(r, &rects);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    for (int i = 0; i < PADS; ++i)
        if (pads[i]) SDL_CloseGamepad(pads[i]);
    snx_ui_rects_free(&rects);
    if (r) SDL_DestroyRenderer(r);
    free(quads);
    return action;
}

int snx_error_sdl_window(SnxError* e, const SnxErrorSdlHooks* hooks, char* why, size_t why_size) {
    char title[400];
    SDL_Window* window;
    int action, started = !SDL_WasInit(SDL_INIT_VIDEO);
    if (started && !SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        snprintf(why, why_size, "SDL's video did not start for the error's panel: %s", SDL_GetError());
        return -1;
    }
    /* the line in the title too, which a screen reader reads and a task bar shows */
    snprintf(title, sizeof title, "%s%s%s", e->game[0] ? e->game : "ScarletNX", " - ", snx_error_line(e));
    window = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        snprintf(why, why_size, "SDL could not make the error panel's window: %s", SDL_GetError());
        if (started) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return -1;
    }
    SDL_RaiseWindow(window);
    action = snx_error_sdl_run(window, e, hooks, why, why_size);
    SDL_DestroyWindow(window);
    if (started) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    return action;
}

int snx_error_screen_main(int argc, char** argv) {
    SnxError e;
    char why[1400];
    const char* path = argc > 1 && !strncmp(argv[1], SNX_ERROR_SCREEN, strlen(SNX_ERROR_SCREEN))
                           ? argv[1] + strlen(SNX_ERROR_SCREEN)
                           : NULL;
    int action;
    if (!path || argc > 2) {
        fprintf(stderr, "scarletnx: %s takes one record and nothing more: %s\n", SNX_ERROR_SCREEN,
                argc > 2 ? argv[2] : "none given");
        return 2;
    }
    if (snx_error_read(&e, path, why, sizeof why)) {
        fprintf(stderr, "scarletnx: %s%s: %s\n", SNX_ERROR_SCREEN, path, why);
        return 2;
    }
    action = snx_error_sdl_window(&e, NULL, why, sizeof why);
    if (action < 0) {
        fprintf(stderr, "scarletnx: %s\n", why);
        return 2;
    }
    return SNX_ERROR_EXIT_BASE + action;
}
