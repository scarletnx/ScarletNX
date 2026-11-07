/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/vpad_skin.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/controls_skins.h"
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/common.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DESCS  256   /* a page's descriptors read; RetroArch's keyboards have about 100 */
#define MAX_IMAGES 32    /* the images a skin's controls take: 17 controls, a D-pad's four, two rings */
#define PAD        2     /* clear texels around each frame, so a linear filter never reads a neighbour */
#define NEAR       0.01f /* a ring's middle this close to its stick's, of the page */
/* the D-pad's directions as vpad.c keeps them (HidNpadButton bits 12 to 15) */
#define DIR_LEFT   (1u << 12)
#define DIR_UP     (1u << 13)
#define DIR_RIGHT  (1u << 14)
#define DIR_DOWN   (1u << 15)

/* ---- the .cfg: RetroArch's config file, key = value a line, a value in quotes or to the line's end */
typedef struct {
    char* text;
    unsigned (*pair)[2]; /* each line's key and value, as offsets into text */
    int n;
} Cfg;
/* the .cfg from file, its size bytes (taken, freed here) */
static int cfg_read(Cfg* c, char* file, size_t size) {
    size_t lines = 1, i;
    char *p, *end;
    void* block;
    if (!file) return 0;
    for (i = 0; i < size; ++i) lines += file[i] == '\n' || file[i] == '\r'; /* a line ends at either */
    if (lines > ((size_t)-1 - size - 1) / sizeof *c->pair) {
        free(file);
        return 0;
    }
    block = malloc(lines * sizeof *c->pair + size + 1); /* the pairs, then the text */
    if (!block) {
        free(file);
        return 0;
    }
    c->pair = (unsigned (*)[2])block;
    c->text = (char*)block + lines * sizeof *c->pair;
    memcpy(c->text, file, size + 1);
    free(file);
    c->n = 0;
    for (p = c->text; *p; p = end) {
        char *key, *eq, *v, *stop;
        end = p + strcspn(p, "\r\n");
        if (*end) *end++ = 0;
        key = p + strspn(p, " \t");
        eq = strchr(key, '=');
        if (*key == '#' || !eq) continue; /* a comment, #include among them, or no key */
        for (stop = eq; stop > key && (stop[-1] == ' ' || stop[-1] == '\t'); --stop) {}
        *stop = 0;
        v = eq + 1 + strspn(eq + 1, " \t");
        if (*v == '"') {
            stop = strchr(++v, '"');
            if (stop) *stop = 0;
        } else
            for (stop = v + strlen(v); stop > v && (stop[-1] == ' ' || stop[-1] == '\t'); *--stop = 0) {}
        c->pair[c->n][0] = (unsigned)(key - c->text);
        c->pair[c->n][1] = (unsigned)(v - c->text);
        ++c->n;
    }
    return 1;
}
/* a key's value, the last line's that sets it; NULL when none does */
static const char* get(const Cfg* c, const char* key) {
    int i;
    for (i = c->n - 1; i >= 0; --i)
        if (!strcmp(c->text + c->pair[i][0], key)) return c->text + c->pair[i][1];
    return NULL;
}
static const char* desc_key(const Cfg* c, unsigned page, int desc, const char* suffix) {
    char key[64];
    snprintf(key, sizeof key, "overlay%u_desc%d%s", page, desc, suffix);
    return get(c, key);
}
static const char* page_key(const Cfg* c, unsigned page, const char* suffix) {
    char key[64];
    snprintf(key, sizeof key, "overlay%u%s", page, suffix);
    return get(c, key);
}
static int is_true(const char* v) { return v && (!strcmp(v, "true") || !strcmp(v, "1")); }
static float number(const char* v, float otherwise) {
    char* end;
    double d;
    if (!v) return otherwise;
    d = strtod(v, &end);
    return end == v || d != d ? otherwise : (float)d;
}

/* ---- a descriptor: "name,x,y,radial|rect,range_x,range_y" and its own keys */
enum { D_CONTROL, D_DIRECTION, D_DECORATION, D_OTHER };
typedef struct {
    int kind, control; /* SNX_VPAD_ for a control */
    u32 dir;           /* a direction's DIR_ bit */
    float x, y, rx, ry, range_mod, alpha_mod, saturate, reach[4];
    int round, movable, image; /* image: an index of the images, -1 for none */
    char name[24];             /* its first word, for the log */
} Desc;
/* RetroArch's names (RetroPad's: its A, B, X and Y sit where the console's do) */
static const struct {
    const char* name;
    int control;
} NAMES[] = {{"a", SNX_VPAD_A},
             {"b", SNX_VPAD_B},
             {"x", SNX_VPAD_X},
             {"y", SNX_VPAD_Y},
             {"l", SNX_VPAD_L},
             {"r", SNX_VPAD_R},
             {"l2", SNX_VPAD_ZL},
             {"r2", SNX_VPAD_ZR},
             {"l3", SNX_VPAD_L3},
             {"r3", SNX_VPAD_R3},
             {"select", SNX_VPAD_MINUS},
             {"start", SNX_VPAD_PLUS},
             {"menu_toggle", SNX_VPAD_HOME},
             {"screenshot", SNX_VPAD_CAPTURE},
             {"analog_left", SNX_VPAD_STICK_L},
             {"analog_right", SNX_VPAD_STICK_R},
             {"dpad_area", SNX_VPAD_DPAD}};
static const struct {
    const char* name;
    u32 dir;
} DIRECTIONS[] = {{"left", DIR_LEFT}, {"up", DIR_UP}, {"right", DIR_RIGHT}, {"down", DIR_DOWN}};

typedef struct {
    const char* name; /* as the .cfg gives it, relative to its folder */
    SDL_Surface* frame[2];
    int frames, at[2][2]; /* each frame's place in the skin's rows */
} Image;
typedef struct {
    const char* path;  /* the .cfg's, or a shipped skin's name, for the log */
    const char* skin;  /* a shipped skin's name, NULL for a .cfg on disk */
    char cfg_name[80]; /* a shipped skin's .cfg: its name and .cfg */
    char* dir;         /* the .cfg's folder, its images' */
    Cfg c;
    unsigned page;
    float aspect, rect[4]; /* the page's: its width over its height, and where it sits on the clear part */
    Desc d[MAX_DESCS];
    int nd;
    Image img[MAX_IMAGES];
    int ni;
} Load;

/* a file of the skin, name relative to its .cfg's folder: malloc'd, its size into n; NULL when there is none */
static u8* skin_file(const Load* s, const char* name, size_t* n) {
    size_t i;
    char* path;
    u8* bytes;
    if (s->skin) {
        for (i = 0; i < snx_controls_skins_count; ++i)
            if (!strcmp(snx_controls_skins[i].skin, s->skin) && !strcmp(snx_controls_skins[i].file, name)) {
                bytes = (u8*)malloc(snx_controls_skins[i].size + 1);
                if (!bytes) return NULL;
                memcpy(bytes, snx_controls_skins[i].data, snx_controls_skins[i].size);
                bytes[snx_controls_skins[i].size] = 0; /* text ends where a file read ends it */
                *n = snx_controls_skins[i].size;
                return bytes;
            }
        return NULL;
    }
    path = snx_path_join(s->dir, name);
    bytes = path ? snx_read_file(path, n) : NULL;
    free(path);
    return bytes;
}
static int image_of(Load* s, const char* name) {
    int i;
    for (i = 0; i < s->ni; ++i)
        if (!strcmp(s->img[i].name, name)) return i;
    if (s->ni == MAX_IMAGES) return -1;
    s->img[s->ni].name = name;
    return s->ni++;
}
/* descriptor i of the page into s->d, or left out with why */
static void desc_read(Load* s, int i, int page_normal, float page_range, float page_alpha) {
    char tokens[256], *t[6], *p = tokens;
    const char *v = desc_key(&s->c, s->page, i, ""), *image;
    Desc* d = &s->d[s->nd];
    int n = 0, k;
    if (!v) {
        LOG("vpad skin: %s: overlay%u_desc%d is not there: left out\n", s->path, s->page, i);
        return;
    }
    snprintf(tokens, sizeof tokens, "%s", v);
    while (*p && n < 6) {
        p += strspn(p, ", ");
        if (!*p) break;
        t[n++] = p;
        p += strcspn(p, ", ");
        if (*p) *p++ = 0;
    }
    if (n < 6) {
        LOG("vpad skin: %s: overlay%u_desc%d = \"%s\" is not name,x,y,shape,range_x,range_y: left out\n", s->path,
            s->page, i, v);
        return;
    }
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", t[0]);
    d->kind = D_OTHER;
    d->control = -1;
    for (k = 0; k < (int)(sizeof NAMES / sizeof NAMES[0]); ++k)
        if (!strcmp(t[0], NAMES[k].name)) {
            d->kind = D_CONTROL;
            d->control = NAMES[k].control;
        }
    for (k = 0; k < 4; ++k)
        if (!strcmp(t[0], DIRECTIONS[k].name)) {
            d->kind = D_DIRECTION;
            d->control = SNX_VPAD_DPAD;
            d->dir = DIRECTIONS[k].dir;
        }
    if (!strcmp(t[0], "nul") || !strcmp(t[0], "null")) d->kind = D_DECORATION;
    if (d->kind == D_OTHER) {
        LOG("vpad skin: %s: overlay%u_desc%d %s is none of the controls: left out\n", s->path, s->page, i, t[0]);
        return;
    }
    if (!page_normal && !is_true(desc_key(&s->c, s->page, i, "_normalized"))) {
        LOG("vpad skin: %s: overlay%u_desc%d %s is placed in pixels of the page's image, not from 0 to 1: left out\n",
            s->path, s->page, i, t[0]);
        return;
    }
    if (strcmp(t[3], "radial") && strcmp(t[3], "rect")) {
        LOG("vpad skin: %s: overlay%u_desc%d %s has the hit area %s, not radial or rect: left out\n", s->path, s->page,
            i, t[0], t[3]);
        return;
    }
    d->x = number(t[1], NAN);
    d->y = number(t[2], NAN);
    d->rx = number(t[4], 0);
    d->ry = number(t[5], 0);
    if (!(d->x >= 0 && d->x <= 1 && d->y >= 0 && d->y <= 1)) {
        LOG("vpad skin: %s: overlay%u_desc%d %s is at %s %s, not from 0 to 1: left out\n", s->path, s->page, i, t[0],
            t[1], t[2]);
        return;
    }
    if (!(d->rx > 0 && d->ry > 0)) {
        LOG("vpad skin: %s: overlay%u_desc%d %s has the range %s by %s, not above 0: left out\n", s->path, s->page, i,
            t[0], t[4], t[5]);
        return;
    }
    d->round = !strcmp(t[3], "radial");
    d->range_mod = number(desc_key(&s->c, s->page, i, "_range_mod"), page_range);
    d->alpha_mod = number(desc_key(&s->c, s->page, i, "_alpha_mod"), page_alpha);
    d->saturate = number(desc_key(&s->c, s->page, i, "_saturate_pct"), 1);
    d->movable = is_true(desc_key(&s->c, s->page, i, "_movable"));
    d->reach[0] = d->reach[1] = number(desc_key(&s->c, s->page, i, "_reach_x"), 1);
    d->reach[2] = d->reach[3] = number(desc_key(&s->c, s->page, i, "_reach_y"), 1);
    d->reach[0] = number(desc_key(&s->c, s->page, i, "_reach_left"), d->reach[0]);
    d->reach[1] = number(desc_key(&s->c, s->page, i, "_reach_right"), d->reach[1]);
    d->reach[2] = number(desc_key(&s->c, s->page, i, "_reach_up"), d->reach[2]);
    d->reach[3] = number(desc_key(&s->c, s->page, i, "_reach_down"), d->reach[3]);
    image = desc_key(&s->c, s->page, i, "_overlay");
    d->image = image && *image ? image_of(s, image) : -1;
    if (image && *image && d->image < 0)
        LOG("vpad skin: %s: overlay%u_desc%d %s: more than %d images, %s is not drawn\n", s->path, s->page, i, t[0],
            MAX_IMAGES, image);
    if (d->kind == D_DECORATION && d->image < 0) return; /* nothing to draw nor to press */
    ++s->nd;
}

/* ---- the images: a PNG, or an APNG of two frames, the second its pressed look (RetroArch's pair) */
static u32 crc32_of(const u8* p, size_t n, u32 crc) {
    size_t i;
    int b;
    crc = ~crc;
    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        for (b = 0; b < 8; ++b) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static size_t chunk(u8* out, const char* type, const u8* data, size_t n) {
    put32(out, (u32)n);
    memcpy(out + 4, type, 4);
    if (n) memcpy(out + 8, data, n);
    put32(out + 8 + n, crc32_of(out + 4, n + 4, 0));
    return n + 12;
}
static SDL_Surface* decode(const u8* png, size_t n) {
    SDL_Surface *s = SDL_LoadPNG_IO(SDL_IOFromConstMem(png, n), true), *rgba;
    if (!s) return NULL;
    rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    return rgba;
}
/* frame f (0 or 1) of an APNG as a PNG of its own into out (n bytes or more), then decoded; its fcTL into fc */
static SDL_Surface* apng_frame(const u8* file, size_t n, int f, u8* out, u8 fc[26]) {
    static const u8 SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    size_t at = 8, o = 8, len;
    int frame = -1;
    memcpy(out, SIGNATURE, 8);
    while (at + 12 <= n && (len = be32(file + at)) <= n - at - 12) {
        const u8 *type = file + at + 4, *data = file + at + 8;
        if (!memcmp(type, "IHDR", 4) && len == 13) {
            o += chunk(out + o, "IHDR", data, 13); /* the frame's size goes in once its fcTL is read */
        } else if (!memcmp(type, "PLTE", 4) || !memcmp(type, "tRNS", 4)) {
            o += chunk(out + o, (const char*)type, data, len);
        } else if (!memcmp(type, "fcTL", 4) && len == 26) {
            if (++frame == f) {
                memcpy(fc, data, 26);
                memcpy(out + 16, data + 4, 8); /* IHDR's width and height: the frame's */
                put32(out + 29, crc32_of(out + 12, 17, 0));
            }
        } else if (frame == f && !memcmp(type, "IDAT", 4)) {
            o += chunk(out + o, "IDAT", data, len);
        } else if (frame == f && !memcmp(type, "fdAT", 4) && len > 4) {
            o += chunk(out + o, "IDAT", data + 4, len - 4);
        }
        at += len + 12;
    }
    o += chunk(out + o, "IEND", NULL, 0);
    return frame >= f ? decode(out, o) : NULL;
}
/* frame 1 drawn on frame 0 where its fcTL puts it, as APNG's blend op says: the pressed look whole */
static SDL_Surface* over(SDL_Surface* base, SDL_Surface* top, const u8 fc[26]) {
    u32 x0 = be32(fc + 12), y0 = be32(fc + 16), x, y;
    SDL_Surface* s;
    if (top->w > base->w || top->h > base->h || x0 > (u32)(base->w - top->w) || y0 > (u32)(base->h - top->h)) {
        SDL_DestroySurface(top);
        return NULL;
    }
    if (!x0 && !y0 && top->w == base->w && top->h == base->h && !fc[25]) return top; /* the whole image, replaced */
    s = SDL_DuplicateSurface(base);
    if (!s) {
        SDL_DestroySurface(top);
        return NULL;
    }
    for (y = 0; y < (u32)top->h; ++y)
        for (x = 0; x < (u32)top->w; ++x) {
            const u8* t = (const u8*)top->pixels + y * (u32)top->pitch + x * 4;
            u8* b = (u8*)s->pixels + (y + y0) * (u32)s->pitch + (x + x0) * 4;
            int c;
            if (!fc[25] || t[3] == 255) /* APNG_BLEND_OP_SOURCE, or opaque */
                memcpy(b, t, 4);
            else if (t[3]) { /* APNG_BLEND_OP_OVER, straight alpha */
                u32 a = t[3], ba = b[3] * (255 - a) / 255, out = a + ba;
                for (c = 0; c < 3; ++c) b[c] = (u8)((t[c] * a + b[c] * ba) / out);
                b[3] = (u8)out;
            }
        }
    SDL_DestroySurface(top);
    return s;
}
static int image_read(Load* s, Image* im) {
    size_t n = 0, at = 8;
    u8 *file = skin_file(s, im->name, &n), *out, fc[2][26];
    u32 frames = 1;
    int ok = 0;
    while (file && at + 12 <= n && be32(file + at) <= n - at - 12) { /* acTL: the frames */
        if (!memcmp(file + at + 4, "acTL", 4) && be32(file + at) == 8) frames = be32(file + at + 8);
        at += be32(file + at) + 12;
    }
    if (!file)
        LOG("vpad skin: %s: the image %s can't be read: the built-in look\n", s->path, im->name);
    else if (frames != 2) {
        if (frames > 2)
            LOG("vpad skin: %s: %s is an animation of %u frames: its image is drawn still\n", s->path, im->name,
                (unsigned)frames);
        im->frame[0] = decode(file, n);
        im->frames = 1;
        ok = im->frame[0] != NULL;
    } else if ((out = (u8*)malloc(n + 64)) != NULL) {
        im->frame[0] = apng_frame(file, n, 0, out, fc[0]);
        im->frame[1] = im->frame[0] ? apng_frame(file, n, 1, out, fc[1]) : NULL;
        if (im->frame[1]) im->frame[1] = over(im->frame[0], im->frame[1], fc[1]);
        im->frames = 2;
        ok = im->frame[0] && im->frame[1];
        free(out);
    }
    if (file && !ok)
        LOG("vpad skin: %s: the image %s is not a PNG SDL reads (%s): the built-in look\n", s->path, im->name,
            SDL_GetError());
    free(file);
    return ok;
}
/* every image's frames into rows of the atlas's width, tallest first along shelves; the rows, or 0 */
static u32* pack(Load* s, int* rows) {
    int order[2 * MAX_IMAGES], n = 0, i, j, x = PAD, y = PAD, shelf = 0, w = SNX_CONTROLS_ATLAS_WIDTH;
    u32* texels;
    for (i = 0; i < s->ni; ++i)
        for (j = 0; j < s->img[i].frames; ++j) order[n++] = i * 2 + j;
    for (i = 1; i < n; ++i) /* by height, the tallest first */
        for (j = i;
             j > 0 && s->img[order[j] / 2].frame[order[j] % 2]->h > s->img[order[j - 1] / 2].frame[order[j - 1] % 2]->h;
             --j) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (i = 0; i < n; ++i) {
        Image* im = &s->img[order[i] / 2];
        SDL_Surface* f = im->frame[order[i] % 2];
        if (f->w > w - 2 * PAD) {
            LOG("vpad skin: %s: %s is %d texels wide, the atlas %d: the built-in look\n", s->path, im->name, f->w, w);
            return NULL;
        }
        if (x + f->w + PAD > w) {
            x = PAD;
            y += shelf + PAD;
            shelf = 0;
        }
        im->at[order[i] % 2][0] = x;
        im->at[order[i] % 2][1] = y;
        x += f->w + PAD;
        if (f->h > shelf) shelf = f->h;
    }
    *rows = (y + shelf + PAD + 3) & ~3;
    if (*rows > ui_skin_room()) {
        LOG("vpad skin: %s: its images take %d rows of %d texels, the atlas has room for %d: the built-in look\n",
            s->path, *rows, w, ui_skin_room());
        return NULL;
    }
    texels = (u32*)calloc((size_t)w * (size_t)*rows, 4);
    if (!texels) return NULL;
    for (i = 0; i < s->ni; ++i)
        for (j = 0; j < s->img[i].frames; ++j) {
            SDL_Surface* f = s->img[i].frame[j];
            int r;
            for (r = 0; r < f->h; ++r)
                memcpy(texels + (size_t)(s->img[i].at[j][1] + r) * (size_t)w + (size_t)s->img[i].at[j][0],
                       (const u8*)f->pixels + (size_t)r * (size_t)f->pitch, (size_t)f->w * 4);
        }
    return texels;
}

/* ---- the controls from the descriptors */
/* a piece of desc d's image on the box (x, y, rx, ry: middle and half sizes, in the page's 0 to 1) */
static void piece(const Load* s, SnxSkinControl* k, const Desc* d, const float b[4], u32 lit, int skin_y) {
    SnxSkinPiece* p = &k->piece[k->pieces++];
    const Image* im = &s->img[d->image];
    p->u = (u16)im->at[0][0];
    p->v = (u16)(skin_y + im->at[0][1]);
    p->uw = (u16)im->frame[0]->w;
    p->vh = (u16)im->frame[0]->h;
    p->pu = (u16)(im->frames == 2 ? im->at[1][0] : im->at[0][0]);
    p->pv = (u16)(skin_y + (im->frames == 2 ? im->at[1][1] : im->at[0][1]));
    p->x = (d->x - d->rx - (b[0] - b[2])) / (2 * b[2]);
    p->y = (d->y - d->ry - (b[1] - b[3])) / (2 * b[3]);
    p->w = d->rx / b[2];
    p->h = d->ry / b[3];
    p->lit = lit;
}
/* control k's place and size from its box b on the page (middle and half sizes, 0 to 1 of the page) */
static void control_box(const Load* s, SnxSkinControl* k, const float b[4]) {
    /* the page's units to the clear part's: its rect, then its width in heights by its aspect (the clear part's
       shorter side is its height for a page not named portrait) */
    k->x = s->rect[0] + b[0] * s->rect[2];
    k->y = s->rect[1] + b[1] * s->rect[3];
    k->w = 2 * b[2] * s->rect[2] * s->aspect;
    k->h = 2 * b[3] * s->rect[3];
}
static void controls(Load* s, SnxSkinControl at[SNX_VPAD_COUNT], int skin_y) {
    int i, j, has_area = 0, dirs = 0;
    float dpad[4] = {0}, lo[2] = {2, 2}, hi[2] = {-1, -1}, dpad_range = 1;
    for (i = 0; i < s->nd; ++i) has_area |= s->d[i].kind == D_CONTROL && s->d[i].control == SNX_VPAD_DPAD;
    for (i = 0; i < s->nd; ++i) { /* each control from its first descriptor */
        const Desc* d = &s->d[i];
        SnxSkinControl* k;
        float b[4] = {d->x, d->y, d->rx, d->ry};
        if (d->kind == D_DIRECTION && !has_area) { /* a D-pad of four: the box around them all */
            if (d->x - d->rx < lo[0]) lo[0] = d->x - d->rx;
            if (d->y - d->ry < lo[1]) lo[1] = d->y - d->ry;
            if (d->x + d->rx > hi[0]) hi[0] = d->x + d->rx;
            if (d->y + d->ry > hi[1]) hi[1] = d->y + d->ry;
            if (d->range_mod > dpad_range) dpad_range = d->range_mod;
            ++dirs;
            continue;
        }
        if (d->kind == D_DIRECTION) {
            LOG("vpad skin: %s: overlay%u %s beside a dpad_area: left out, the D-pad is the area's\n", s->path, s->page,
                d->name);
            continue;
        }
        if (d->kind != D_CONTROL) continue;
        k = &at[d->control];
        if (k->pieces) {
            LOG("vpad skin: %s: overlay%u %s again: left out, its first one is the control\n", s->path, s->page,
                d->name);
            continue;
        }
        control_box(s, k, b);
        for (j = 0; j < 4; ++j) k->reach[j] = d->reach[j] * d->range_mod;
        k->round = d->round;
        k->alpha_mod = d->alpha_mod;
        k->saturate = d->saturate > 0 ? d->saturate : 1;
        k->knob = -1;
        if (d->control == SNX_VPAD_STICK_L || d->control == SNX_VPAD_STICK_R)
            for (j = 0; j < s->nd; ++j) { /* its ring: a decoration with an image where the stick is */
                const Desc* r = &s->d[j];
                if (r->kind == D_DECORATION && fabsf(r->x - d->x) < NEAR && fabsf(r->y - d->y) < NEAR &&
                    k->pieces < SNX_SKIN_PIECES - 1)
                    piece(s, k, r, b, 0, skin_y);
            }
        if (d->image >= 0) {
            if ((d->control == SNX_VPAD_STICK_L || d->control == SNX_VPAD_STICK_R) && d->movable) k->knob = k->pieces;
            piece(s, k, d, b,
                  d->control == SNX_VPAD_DPAD ? SNX_SKIN_LIT_HELD | DIR_LEFT | DIR_UP | DIR_RIGHT | DIR_DOWN
                                              : SNX_SKIN_LIT_HELD,
                  skin_y);
        }
        if (!k->pieces) {
            LOG("vpad skin: %s: overlay%u %s has no image: the built-in one\n", s->path, s->page, d->name);
            memset(k, 0, sizeof *k);
        }
    }
    if (dirs) { /* the four directions, each its own image lit by its own direction */
        SnxSkinControl* k = &at[SNX_VPAD_DPAD];
        dpad[0] = (lo[0] + hi[0]) / 2;
        dpad[1] = (lo[1] + hi[1]) / 2;
        dpad[2] = (hi[0] - lo[0]) / 2;
        dpad[3] = (hi[1] - lo[1]) / 2;
        control_box(s, k, dpad);
        for (j = 0; j < 4; ++j) k->reach[j] = dpad_range;
        k->alpha_mod = 1;
        k->saturate = 1;
        k->knob = -1;
        for (i = 0; i < s->nd; ++i)
            if (s->d[i].kind == D_DIRECTION && s->d[i].image >= 0 && k->pieces < SNX_SKIN_PIECES) {
                k->alpha_mod = s->d[i].alpha_mod;
                piece(s, k, &s->d[i], dpad, s->d[i].dir, skin_y);
            }
        if (!k->pieces) memset(k, 0, sizeof *k);
    }
    for (i = 0; i < s->nd; ++i) /* a decoration no stick took */
        if (s->d[i].kind == D_DECORATION) {
            int used = 0;
            for (j = 0; j < s->nd; ++j)
                used |= s->d[j].kind == D_CONTROL &&
                        (s->d[j].control == SNX_VPAD_STICK_L || s->d[j].control == SNX_VPAD_STICK_R) &&
                        fabsf(s->d[i].x - s->d[j].x) < NEAR && fabsf(s->d[i].y - s->d[j].y) < NEAR;
            if (!used)
                LOG("vpad skin: %s: overlay%u's nul at %.3f %.3f is no stick's ring: left out\n", s->path, s->page,
                    (double)s->d[i].x, (double)s->d[i].y);
        }
}

/* the skin at s->path (or shipped as s->skin) into at and its texels into *texels, *rows rows: 1, or 0 (logged) */
static int skin_load(Load* s, SnxSkinControl at[SNX_VPAD_COUNT], u32** texels, int* rows) {
    unsigned pages = 0;
    int i, page_normal;
    float page_range, page_alpha;
    size_t size = 0;
    const char *v, *name = "";
    if (!cfg_read(&s->c, (char*)skin_file(s, s->skin ? s->cfg_name : snx_path_leaf(s->path), &size), size)) {
        LOG("vpad skin: %s can't be read: the built-in look\n", s->path);
        return 0;
    }
    v = get(&s->c, "overlays");
    if (v) pages = (unsigned)strtoul(v, NULL, 10);
    for (s->page = 0; s->page < pages; ++s->page) { /* the first page that is no portrait one */
        name = page_key(&s->c, s->page, "_name");
        if (!name) name = "";
        if (!strstr(name, "portrait")) break;
        LOG("vpad skin: %s: overlay%u \"%s\" is a portrait page: the controls take the first other\n", s->path, s->page,
            name);
    }
    if (!pages || s->page == pages) {
        LOG("vpad skin: %s has %u pages (overlays), none of them a landscape one: the built-in look\n", s->path, pages);
        return 0;
    }
    page_normal = is_true(page_key(&s->c, s->page, "_normalized"));
    page_range = number(page_key(&s->c, s->page, "_range_mod"), 1);
    page_alpha = number(page_key(&s->c, s->page, "_alpha_mod"), 1);
    s->aspect = number(page_key(&s->c, s->page, "_aspect_ratio"), 0);
    if (s->aspect <= 0) s->aspect = 16.0f / 9; /* RetroArch's for a page with none */
    s->rect[0] = s->rect[1] = 0;
    s->rect[2] = s->rect[3] = 1;
    if ((v = page_key(&s->c, s->page, "_rect")) != NULL &&
        sscanf(v, "%f%*[, ]%f%*[, ]%f%*[, ]%f", &s->rect[0], &s->rect[1], &s->rect[2], &s->rect[3]) != 4) {
        LOG("vpad skin: %s: overlay%u_rect = \"%s\" is not x,y,w,h: the page is the clear part, 0,0,1,1\n", s->path,
            s->page, v);
        s->rect[0] = s->rect[1] = 0;
        s->rect[2] = s->rect[3] = 1;
    }
    if ((v = page_key(&s->c, s->page, "_overlay")) != NULL && *v)
        LOG("vpad skin: %s: overlay%u's image %s fills the screen, which the controls don't: not drawn\n", s->path,
            s->page, v);
    v = page_key(&s->c, s->page, "_descs");
    {
        unsigned descs = v ? (unsigned)strtoul(v, NULL, 10) : 0;
        if (descs > MAX_DESCS) {
            LOG("vpad skin: %s: overlay%u has %u descriptors: the first %d read\n", s->path, s->page, descs, MAX_DESCS);
            descs = MAX_DESCS;
        }
        for (i = 0; i < (int)descs; ++i) desc_read(s, i, page_normal, page_range, page_alpha);
    }
    for (i = 0; i < s->ni; ++i)
        if (!image_read(s, &s->img[i])) return 0;
    *texels = pack(s, rows);
    if (!*texels) return 0;
    controls(s, at, ui_skin_y());
    for (i = 0; i < SNX_VPAD_COUNT; ++i)
        if (!at[i].pieces) LOG("vpad skin: %s has no %s: the built-in one\n", s->path, vpad_name(i));
    LOG("vpad skin: %s, page %u \"%s\": %d images in %d rows of the atlas\n", s->path, s->page, name, s->ni, *rows);
    return 1;
}
/* a name of the shipped skins (cmake/controls_skins.cmake), NULL for another */
static const char* shipped(const char* name) {
    size_t i;
    for (i = 0; i < snx_controls_skins_count; ++i)
        if (!strcmp(snx_controls_skins[i].skin, name)) return snx_controls_skins[i].skin;
    return NULL;
}
int vpad_skin_read(const char* value, SnxSkinControl* at) {
    Load* s;
    u32* texels = NULL;
    int i, rows = 0, ok = 0;
    char names[256] = "", one[64];
    memset(at, 0, SNX_VPAD_COUNT * sizeof at[0]);
    ui_skin_set(NULL, 0);
    if (!*value) return 1; /* the built-in look */
    if (!strpbrk(value, "/\\") && !shipped(value)) {
        for (i = 0; vpad_skin_shipped(i, one, sizeof one); ++i)
            snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ", " : "", one);
        LOG("vpad skin: controls_skin=%s is neither a skin ScarletNX has (%s) nor the path of a RetroArch overlay's "
            ".cfg: the built-in look\n",
            value, names);
        return 0;
    }
    s = (Load*)calloc(1, sizeof(Load));
    if (!s) return 0;
    s->path = value;
    s->skin = shipped(value);
    if (s->skin) snprintf(s->cfg_name, sizeof s->cfg_name, "%s.cfg", s->skin);
    s->dir = s->skin ? NULL : snx_dirname(value);
    ok = skin_load(s, at, &texels, &rows);
    for (i = 0; i < s->ni; ++i) {
        if (s->img[i].frame[1] != s->img[i].frame[0]) SDL_DestroySurface(s->img[i].frame[1]);
        SDL_DestroySurface(s->img[i].frame[0]);
    }
    free(s->dir);
    free(s->c.pair);
    free(s);
    if (ok)
        ui_skin_set(texels, rows);
    else {
        free(texels);
        memset(at, 0, SNX_VPAD_COUNT * sizeof at[0]);
    }
    return ok;
}
int vpad_skin_apply(const u32* sprites, int w, int h) {
    SnxSkinControl at[SNX_VPAD_COUNT];
    char value[sizeof g_settings.controls_skin];
    u32* atlas;
    int rows = 0, ok;
    ui_lock(); /* the in-game menu writes it under the lock */
    snprintf(value, sizeof value, "%s", g_settings.controls_skin);
    ui_unlock();
    ok = vpad_skin_read(value, at);
    atlas = ui_atlas_make(sprites, w, h, &rows);
    if (!atlas) ui_skin_set(NULL, 0); /* the rows wait for no atlas */
    ui_lock(); /* the atlas and the controls drawn from it at once: the renderer sees both or neither */
    if (atlas) ui_atlas_put(atlas, rows);
    vpad_skin_set(atlas ? at : NULL); /* no atlas made: no rows for a skin's controls to draw from */
    ui_unlock();
    return ok && atlas;
}
