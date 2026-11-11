/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's frame as a list. What a frame draws (the renderer's draw calls in their order, with the state they set) is
   listed in place of drawn, and its list compared with that of the frame the window shows: the same list draws nothing
   and presents nothing, so a menu at rest costs the GPU nothing; another list is drawn whole and presented. It is the
   game's on-screen layer's way, whose renderer sends only the quads that changed (nx/gpu/vk/ui.c).

   main.c calls the renderer by SDL's names, which the #defines at this file's end hand to the functions here: between
   gui_drawn_begin and the frame's SDL_RenderPresent the draws are listed; outside a frame they go to the renderer as
   they are. The viewport, the clip and the target are set as they come too, a frame's layout reading them back. A frame
   is drawn whatever its list says after anything that changes what the window shows with no change in the list
   (gui_drawn_stale): a draw outside a frame, a texture made, written or freed, the window uncovered or resized. A
   frame that changes a texture it has already drawn with is drawn as it comes (gui_drawn_flush) */
#ifndef SNX_GUI_DRAWN_H
#define SNX_GUI_DRAWN_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <string.h>
#include "gui/log.h"

typedef struct {
    SDL_Renderer* renderer;  /* the one a frame is being listed for, NULL outside a frame */
    SDL_Renderer* shown_for; /* the one whose window shows `shown` */
    Uint8 *now, *shown;      /* this frame's list, and the list of what the window shows */
    size_t n, room, shown_n, shown_room;
    bool stale;           /* the window may show something else than `shown`: the next frame is drawn */
    unsigned drawn, kept; /* the frames drawn and presented, and those left as the window had them */
} GuiDrawn;
static GuiDrawn g_drawn = {.stale = true};

/* a draw of the list: its code in a byte, then what it takes, as the structs below, unaligned */
enum {
    GUI_DRAWN_CLEAR,
    GUI_DRAWN_COLOR,
    GUI_DRAWN_BLEND,
    GUI_DRAWN_VIEWPORT,
    GUI_DRAWN_CLIP,
    GUI_DRAWN_TARGET,
    GUI_DRAWN_FILL,
    GUI_DRAWN_RECT,
    GUI_DRAWN_LINE,
    GUI_DRAWN_LINES,
    GUI_DRAWN_GEOMETRY,
    GUI_DRAWN_TEXTURE,
    GUI_DRAWN_TEXTURE_COLOR,
    GUI_DRAWN_TEXTURE_ALPHA
};
/* the structs listed are set to zero whole before their fields (memset, not = {0}): two frames' lists are compared
   byte by byte, the bytes between the fields too */
typedef struct {
    Uint8 has; /* 0: the whole target (a NULL rectangle) */
    SDL_Rect rect;
} GuiDrawnArea;
typedef struct {
    SDL_Texture* texture;
    Uint8 has_source, has_place;
    SDL_FRect source, place;
} GuiDrawnTexture;
typedef struct {
    SDL_Texture* texture;
    Uint8 r, g, b, a;
} GuiDrawnTint;
typedef struct {
    SDL_Texture* texture;
    int vertices, indices; /* the SDL_Vertex and the int that follow */
} GuiDrawnGeometry;

/* what the window shows may not be the list kept: the next frame is drawn and presented whatever its list says */
static inline void gui_drawn_stale(void) { g_drawn.stale = true; }
/* the frames drawn and presented, and those whose list was the one shown, which drew nothing */
static inline unsigned gui_drawn_drawn(void) { return g_drawn.drawn; }
static inline unsigned gui_drawn_kept(void) { return g_drawn.kept; }
static inline bool gui_drawn_listing(const SDL_Renderer* r) { return r && g_drawn.renderer == r; }
static void gui_drawn_replay(SDL_Renderer* r, const Uint8* at, size_t n);
/* The frame stops being a list: what it listed is drawn now, and the rest of it goes to the renderer as it comes. For a
   frame that cannot be listed to its end as it is: one that writes, sets or frees a texture after draws that took it
   as it was (a wide letter put in the font's slot of another, the theme editor's preview, whose pictures are freed
   before the frame ends), or one with no memory for its list. The frame after it is drawn */
static void gui_drawn_flush(void) {
    SDL_Renderer* r = g_drawn.renderer;
    g_drawn.stale = true;
    if (!r) return;
    g_drawn.renderer = NULL;
    gui_drawn_replay(r, g_drawn.now, g_drawn.n);
    g_drawn.n = 0;
}
/* size bytes more in this frame's list after its code: where they go. Without memory for them the frame stops being a
   list: what it listed is drawn now, the rest goes to the renderer as it comes, and NULL */
static Uint8* gui_drawn_put(Uint8 code, size_t size) {
    if (g_drawn.n + 1 + size > g_drawn.room) {
        size_t room = g_drawn.room ? g_drawn.room * 2 : 1u << 16;
        Uint8* more;
        while (room < g_drawn.n + 1 + size) room *= 2;
        more = (Uint8*)SDL_realloc(g_drawn.now, room);
        if (!more) {
            gui_log(
                "the menu's frame could not be listed in %zu bytes: it is drawn as it comes, and every frame is drawn "
                "until one can be",
                room);
            gui_drawn_flush();
            return NULL;
        }
        g_drawn.now = more;
        g_drawn.room = room;
    }
    g_drawn.now[g_drawn.n] = code;
    g_drawn.n += 1 + size;
    return g_drawn.now + g_drawn.n - size;
}
/* code and the size bytes at what, listed; false when the list could not take them */
static bool gui_drawn_list(Uint8 code, const void* what, size_t size) {
    Uint8* at = gui_drawn_put(code, size);
    if (at && size) memcpy(at, what, size);
    return at != NULL;
}
/* the draws of a list, n bytes at at, to the renderer in their order */
static void gui_drawn_replay(SDL_Renderer* r, const Uint8* at, size_t n) {
    const Uint8* end = at + n;
    while (at < end) {
        Uint8 code = *at++;
        switch (code) {
            case GUI_DRAWN_CLEAR: SDL_RenderClear(r); break;
            case GUI_DRAWN_COLOR:
                SDL_SetRenderDrawColor(r, at[0], at[1], at[2], at[3]);
                at += 4;
                break;
            case GUI_DRAWN_BLEND: {
                SDL_BlendMode mode;
                memcpy(&mode, at, sizeof mode);
                at += sizeof mode;
                SDL_SetRenderDrawBlendMode(r, mode);
                break;
            }
            case GUI_DRAWN_VIEWPORT:
            case GUI_DRAWN_CLIP: {
                GuiDrawnArea a;
                memcpy(&a, at, sizeof a);
                at += sizeof a;
                if (code == GUI_DRAWN_VIEWPORT)
                    SDL_SetRenderViewport(r, a.has ? &a.rect : NULL);
                else
                    SDL_SetRenderClipRect(r, a.has ? &a.rect : NULL);
                break;
            }
            case GUI_DRAWN_TARGET: {
                SDL_Texture* target;
                memcpy(&target, at, sizeof target);
                at += sizeof target;
                SDL_SetRenderTarget(r, target);
                break;
            }
            case GUI_DRAWN_FILL:
            case GUI_DRAWN_RECT: {
                SDL_FRect rect;
                memcpy(&rect, at, sizeof rect);
                at += sizeof rect;
                if (code == GUI_DRAWN_FILL)
                    SDL_RenderFillRect(r, &rect);
                else
                    SDL_RenderRect(r, &rect);
                break;
            }
            case GUI_DRAWN_LINE: {
                float p[4];
                memcpy(p, at, sizeof p);
                at += sizeof p;
                SDL_RenderLine(r, p[0], p[1], p[2], p[3]);
                break;
            }
            case GUI_DRAWN_LINES: {
                int count;
                memcpy(&count, at, sizeof count);
                at += sizeof count;
                {
                    /* a copy, aligned as SDL reads the points, which the list's bytes may not be */
                    SDL_FPoint few[64], *p = count <= 64 ? few : (SDL_FPoint*)SDL_malloc((size_t)count * sizeof *p);
                    if (p) {
                        memcpy(p, at, (size_t)count * sizeof *p);
                        SDL_RenderLines(r, p, count);
                        if (p != few) SDL_free(p);
                    } else
                        gui_log("a line of the menu's frame was not drawn: no memory for its %d points", count);
                }
                at += (size_t)count * sizeof(SDL_FPoint);
                break;
            }
            case GUI_DRAWN_GEOMETRY: {
                GuiDrawnGeometry g;
                size_t vertices, indices;
                void* copy;
                memcpy(&g, at, sizeof g);
                at += sizeof g;
                vertices = (size_t)g.vertices * sizeof(SDL_Vertex);
                indices = (size_t)g.indices * sizeof(int);
                copy = SDL_malloc(vertices + indices + 1); /* aligned, as SDL reads them */
                if (copy) {
                    memcpy(copy, at, vertices + indices);
                    SDL_RenderGeometry(r, g.texture, (const SDL_Vertex*)copy, g.vertices,
                                       g.indices ? (const int*)((const Uint8*)copy + vertices) : NULL, g.indices);
                    SDL_free(copy);
                } else
                    gui_log("a shape of the menu's frame was not drawn: no memory for its %d corners", g.vertices);
                at += vertices + indices;
                break;
            }
            case GUI_DRAWN_TEXTURE: {
                GuiDrawnTexture t;
                memcpy(&t, at, sizeof t);
                at += sizeof t;
                SDL_RenderTexture(r, t.texture, t.has_source ? &t.source : NULL, t.has_place ? &t.place : NULL);
                break;
            }
            case GUI_DRAWN_TEXTURE_COLOR:
            case GUI_DRAWN_TEXTURE_ALPHA: {
                GuiDrawnTint t;
                memcpy(&t, at, sizeof t);
                at += sizeof t;
                if (code == GUI_DRAWN_TEXTURE_COLOR)
                    SDL_SetTextureColorMod(t.texture, t.r, t.g, t.b);
                else
                    SDL_SetTextureAlphaMod(t.texture, t.a);
                break;
            }
            default:
                gui_log("the menu's frame list holds an unknown draw, %u: the rest of the frame is not drawn", code);
                return;
        }
    }
}
/* a frame starts: its draws on r are listed until its present */
static void gui_drawn_begin(SDL_Renderer* r) {
    if (g_drawn.renderer || g_drawn.shown_for != r) g_drawn.stale = true; /* a frame never presented, another window */
    g_drawn.renderer = r;
    g_drawn.n = 0;
}
/* the frame's end. Its list the one the window shows: nothing drawn, nothing presented. Else its draws to the
   renderer, and the present. Outside a frame, the present as it is */
static bool gui_drawn_present(SDL_Renderer* r) {
    Uint8* list;
    size_t room;
    if (!gui_drawn_listing(r)) {
        g_drawn.stale = true;
        return SDL_RenderPresent(r);
    }
    g_drawn.renderer = NULL; /* from here the calls go to the renderer */
    if (!g_drawn.stale && g_drawn.n == g_drawn.shown_n && !memcmp(g_drawn.now, g_drawn.shown, g_drawn.n)) {
        ++g_drawn.kept;
        return true;
    }
    gui_drawn_replay(r, g_drawn.now, g_drawn.n);
    list = g_drawn.shown;
    room = g_drawn.shown_room;
    g_drawn.shown = g_drawn.now;
    g_drawn.shown_n = g_drawn.n;
    g_drawn.shown_room = g_drawn.room;
    g_drawn.now = list;
    g_drawn.room = room;
    g_drawn.n = 0;
    g_drawn.shown_for = r;
    g_drawn.stale = false;
    ++g_drawn.drawn;
    return SDL_RenderPresent(r);
}
/* the lists freed, as the menu ends or its renderer goes */
static void gui_drawn_free(void) {
    SDL_free(g_drawn.now);
    SDL_free(g_drawn.shown);
    g_drawn = (GuiDrawn){.stale = true};
}

static bool gui_drawn_clear(SDL_Renderer* r) {
    if (gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_CLEAR, NULL, 0)) return true;
    g_drawn.stale = true;
    return SDL_RenderClear(r);
}
static bool gui_drawn_color(SDL_Renderer* r, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
    const Uint8 c[4] = {red, green, blue, alpha};
    if (gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_COLOR, c, sizeof c)) return true;
    return SDL_SetRenderDrawColor(r, red, green, blue, alpha);
}
static bool gui_drawn_blend(SDL_Renderer* r, SDL_BlendMode mode) {
    if (gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_BLEND, &mode, sizeof mode)) return true;
    return SDL_SetRenderDrawBlendMode(r, mode);
}
/* set now, a layout reading it back, and listed, for the draws after it when the list is drawn */
static bool gui_drawn_viewport(SDL_Renderer* r, const SDL_Rect* rect) {
    GuiDrawnArea a;
    memset(&a, 0, sizeof a);
    a.has = rect != NULL;
    if (rect) a.rect = *rect;
    if (gui_drawn_listing(r)) gui_drawn_list(GUI_DRAWN_VIEWPORT, &a, sizeof a);
    return SDL_SetRenderViewport(r, rect);
}
static bool gui_drawn_clip(SDL_Renderer* r, const SDL_Rect* rect) {
    GuiDrawnArea a;
    memset(&a, 0, sizeof a);
    a.has = rect != NULL;
    if (rect) a.rect = *rect;
    if (gui_drawn_listing(r)) gui_drawn_list(GUI_DRAWN_CLIP, &a, sizeof a);
    return SDL_SetRenderClipRect(r, rect);
}
static bool gui_drawn_target(SDL_Renderer* r, SDL_Texture* target) {
    if (gui_drawn_listing(r)) gui_drawn_list(GUI_DRAWN_TARGET, &target, sizeof target);
    return SDL_SetRenderTarget(r, target);
}
static bool gui_drawn_fill(SDL_Renderer* r, const SDL_FRect* rect) {
    if (rect && gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_FILL, rect, sizeof *rect)) return true;
    g_drawn.stale = true;
    return SDL_RenderFillRect(r, rect);
}
static bool gui_drawn_rect(SDL_Renderer* r, const SDL_FRect* rect) {
    if (rect && gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_RECT, rect, sizeof *rect)) return true;
    g_drawn.stale = true;
    return SDL_RenderRect(r, rect);
}
static bool gui_drawn_line(SDL_Renderer* r, float x1, float y1, float x2, float y2) {
    const float p[4] = {x1, y1, x2, y2};
    if (gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_LINE, p, sizeof p)) return true;
    g_drawn.stale = true;
    return SDL_RenderLine(r, x1, y1, x2, y2);
}
static bool gui_drawn_lines(SDL_Renderer* r, const SDL_FPoint* points, int count) {
    if (count > 0 && gui_drawn_listing(r)) {
        Uint8* at = gui_drawn_put(GUI_DRAWN_LINES, sizeof count + (size_t)count * sizeof *points);
        if (at) {
            memcpy(at, &count, sizeof count);
            memcpy(at + sizeof count, points, (size_t)count * sizeof *points);
            return true;
        }
    }
    g_drawn.stale = true;
    return SDL_RenderLines(r, points, count);
}
static bool gui_drawn_geometry(SDL_Renderer* r, SDL_Texture* texture, const SDL_Vertex* vertices, int count,
                               const int* indices, int index_count) {
    if (count > 0 && index_count >= 0 && gui_drawn_listing(r)) {
        GuiDrawnGeometry g;
        size_t v = (size_t)count * sizeof *vertices, i = indices ? (size_t)index_count * sizeof *indices : 0;
        Uint8* at;
        memset(&g, 0, sizeof g);
        g.texture = texture;
        g.vertices = count;
        g.indices = indices ? index_count : 0;
        at = gui_drawn_put(GUI_DRAWN_GEOMETRY, sizeof g + v + i);
        if (at) {
            memcpy(at, &g, sizeof g);
            memcpy(at + sizeof g, vertices, v);
            if (i) memcpy(at + sizeof g + v, indices, i);
            return true;
        }
    }
    g_drawn.stale = true;
    return SDL_RenderGeometry(r, texture, vertices, count, indices, index_count);
}
static bool gui_drawn_texture(SDL_Renderer* r, SDL_Texture* texture, const SDL_FRect* source, const SDL_FRect* place) {
    GuiDrawnTexture t;
    memset(&t, 0, sizeof t);
    t.texture = texture;
    t.has_source = source != NULL;
    t.has_place = place != NULL;
    if (source) t.source = *source;
    if (place) t.place = *place;
    if (gui_drawn_listing(r) && gui_drawn_list(GUI_DRAWN_TEXTURE, &t, sizeof t)) return true;
    g_drawn.stale = true;
    return SDL_RenderTexture(r, texture, source, place);
}
/* a texture's tint: listed in a frame, its texture the frame's renderer's */
static bool gui_drawn_texture_color(SDL_Texture* texture, Uint8 red, Uint8 green, Uint8 blue) {
    GuiDrawnTint t;
    memset(&t, 0, sizeof t);
    t.texture = texture;
    t.r = red;
    t.g = green;
    t.b = blue;
    if (g_drawn.renderer && gui_drawn_list(GUI_DRAWN_TEXTURE_COLOR, &t, sizeof t)) return true;
    return SDL_SetTextureColorMod(texture, red, green, blue);
}
static bool gui_drawn_texture_alpha(SDL_Texture* texture, Uint8 alpha) {
    GuiDrawnTint t;
    memset(&t, 0, sizeof t);
    t.texture = texture;
    t.a = alpha;
    if (g_drawn.renderer && gui_drawn_list(GUI_DRAWN_TEXTURE_ALPHA, &t, sizeof t)) return true;
    return SDL_SetTextureAlphaMod(texture, alpha);
}
/* a texture made: what a list draws with it is another picture, with no change in the list. One written, set or
   freed: so are the draws this frame already listed with it, which are drawn first (gui_drawn_flush) */
static SDL_Texture* gui_drawn_texture_make(SDL_Renderer* r, SDL_PixelFormat format, SDL_TextureAccess access, int w,
                                           int h) {
    g_drawn.stale = true;
    return SDL_CreateTexture(r, format, access, w, h);
}
static SDL_Texture* gui_drawn_texture_from(SDL_Renderer* r, SDL_Surface* surface) {
    g_drawn.stale = true;
    return SDL_CreateTextureFromSurface(r, surface);
}
static bool gui_drawn_texture_write(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch) {
    gui_drawn_flush();
    return SDL_UpdateTexture(texture, rect, pixels, pitch);
}
static bool gui_drawn_texture_blend(SDL_Texture* texture, SDL_BlendMode mode) {
    gui_drawn_flush();
    return SDL_SetTextureBlendMode(texture, mode);
}
static bool gui_drawn_texture_scale(SDL_Texture* texture, SDL_ScaleMode mode) {
    gui_drawn_flush();
    return SDL_SetTextureScaleMode(texture, mode);
}
static void gui_drawn_texture_free(SDL_Texture* texture) {
    gui_drawn_flush();
    SDL_DestroyTexture(texture);
}
static void gui_drawn_renderer_free(SDL_Renderer* r) {
    if (g_drawn.renderer == r) g_drawn.renderer = NULL;
    if (g_drawn.shown_for == r) g_drawn.shown_for = NULL;
    g_drawn.stale = true;
    SDL_DestroyRenderer(r);
}

/* from here on, the renderer by SDL's names is the frame's */
#define SDL_RenderClear              gui_drawn_clear
#define SDL_SetRenderDrawColor       gui_drawn_color
#define SDL_SetRenderDrawBlendMode   gui_drawn_blend
#define SDL_SetRenderViewport        gui_drawn_viewport
#define SDL_SetRenderClipRect        gui_drawn_clip
#define SDL_SetRenderTarget          gui_drawn_target
#define SDL_RenderFillRect           gui_drawn_fill
#define SDL_RenderRect               gui_drawn_rect
#define SDL_RenderLine               gui_drawn_line
#define SDL_RenderLines              gui_drawn_lines
#define SDL_RenderGeometry           gui_drawn_geometry
#define SDL_RenderTexture            gui_drawn_texture
#define SDL_SetTextureColorMod       gui_drawn_texture_color
#define SDL_SetTextureAlphaMod       gui_drawn_texture_alpha
#define SDL_CreateTexture            gui_drawn_texture_make
#define SDL_CreateTextureFromSurface gui_drawn_texture_from
#define SDL_UpdateTexture            gui_drawn_texture_write
#define SDL_SetTextureBlendMode      gui_drawn_texture_blend
#define SDL_SetTextureScaleMode      gui_drawn_texture_scale
#define SDL_DestroyTexture           gui_drawn_texture_free
#define SDL_DestroyRenderer          gui_drawn_renderer_free
#define SDL_RenderPresent            gui_drawn_present
#endif
