/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's input in the Vita's window (src/vita/platform/input.c, gpu/present.c) under SDL's offscreen driver: the
   on-screen controls each press their key and push their stick under a finger and under the mouse, read once a vblank,
   drawn over framebuffers that go on screen; a finger off them on the picture touches the front panel, up to the
   console's six at once as SceTouch reports them, each with its id while it stays down, kept to the panel's edges, the
   mouse's left button too, by the touch setting; Auto hides the controls as a gamepad connects; F2 edits their layout,
   kept in the game's .ini, and Android's Back twice quits. Skipped (77) where no Vulkan window opens */
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/vpad_consoles.h"
#include "support/sdl_real_pads.h"
#include "support/vpad_window.h"
#include "vita/gpu/present.h"
#include "vita/gpu/vk.h"
#include "vita/hle/hle.h"
#include "vita/platform/input.h"
#include <stdlib.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vita_input:%d: %s\n", __LINE__, #x);                                                      \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static VitaProcess g_process; /* no framebuffer: its vblank reads the events only */
static VitaInput g_input;
/* a vblank, and the input after it: the buttons, 0xFFFFFFFF once the window closed */
/* the pause key pressed again, from a timer while the game's thread is held */
static Uint32 SDLCALL pause_again(void* window, SDL_TimerID timer, Uint32 interval) {
    (void)timer, (void)interval;
    (void)vpad_test_key((SDL_Window*)window, SDLK_PAUSE);
    return 0;
}
static uint32_t vblank(void) {
    bool open = vita_present_vblank(&g_process);
    vita_input_read(&g_input);
    return open ? g_input.buttons : 0xFFFFFFFFu;
}
/* the front panel's touch i of count is id at about x, y */
static bool front(unsigned count, unsigned i, unsigned id, int x, int y) {
    return g_input.fronts == count && g_input.front[i].id == id && abs((int)g_input.front[i].x - x) <= 3 &&
           abs((int)g_input.front[i].y - y) <= 3;
}
static bool at_rest(void) { return g_input.lx == 128 && g_input.ly == 128 && g_input.rx == 128 && g_input.ry == 128; }

/* a framebuffer of 960 by 544 shown: whether it went on screen */
static struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
} g_frame;
static bool show(void) {
    VitaVk* v = vita_vk();
    if (!g_frame.buffer) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = 960 * 544 * 4,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VkMemoryRequirements req;
        if (vkCreateBuffer(v->device, &bi, NULL, &g_frame.buffer) != VK_SUCCESS) return false;
        vkGetBufferMemoryRequirements(v->device, g_frame.buffer, &req);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0)};
        if (vkAllocateMemory(v->device, &ai, NULL, &g_frame.memory) != VK_SUCCESS ||
            vkBindBufferMemory(v->device, g_frame.buffer, g_frame.memory, 0) != VK_SUCCESS)
            return false;
    }
    for (int tries = 0; tries < 200; ++tries) { /* no image free for it yet: again */
        if (vita_vk_present(g_frame.buffer, 0, 960, VK_NULL_HANDLE, 960, 544)) return true;
        (void)vita_vk_poll();
        SDL_Delay(5);
    }
    return false;
}

static int controls(SDL_Window* window, const char* dir, const char* ini) {
    static const struct {
        int control;
        uint32_t button;
    } buttons[] = {{SNX_VPAD_A, VITA_CTRL_CIRCLE},   {SNX_VPAD_B, VITA_CTRL_CROSS},
                   {SNX_VPAD_X, VITA_CTRL_TRIANGLE}, {SNX_VPAD_Y, VITA_CTRL_SQUARE},
                   {SNX_VPAD_L, VITA_CTRL_LTRIGGER}, {SNX_VPAD_R, VITA_CTRL_RTRIGGER},
                   {SNX_VPAD_PLUS, VITA_CTRL_START}, {SNX_VPAD_MINUS, VITA_CTRL_SELECT}};
    static const struct {
        float dx, dy;
        uint32_t button;
    } arms[] = {
        {-0.7f, 0, VITA_CTRL_LEFT}, {0.7f, 0, VITA_CTRL_RIGHT}, {0, -0.7f, VITA_CTRL_UP}, {0, 0.7f, VITA_CTRL_DOWN}};
    float x, y, half;
    int w = 0, h = 0, rect[4];
    CHECK(SDL_GetWindowSizeInPixels(window, &w, &h));
    /* Auto on a device with no touch screen shows none; on one, until a gamepad connects */
    CHECK(!vblank() && !snx_vpad_window_rects(w, h));
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1"); /* SDL's own screen of fingers, SDL_MOUSE_TOUCHID */
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, SDL_MOUSE_TOUCHID, 2, 1, 1));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, SDL_MOUSE_TOUCHID, 2, 1, 1) && !vblank());
    CHECK(snx_vpad_window_rects(w, h) && snx_vpad_window_rects(w, h)->count > 100);
    SDL_JoystickID gamepad = vpad_test_gamepad();
    SDL_UpdateJoysticks();
    (void)vblank();
    CHECK(gamepad && !vblank() && !snx_vpad_window_rects(w, h));
    CHECK(SDL_DetachVirtualJoystick(gamepad));
    SDL_UpdateJoysticks();
    (void)vblank();
    CHECK(snx_vpad_window_rects(w, h));
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    g_settings.show_controls = 1;
    for (int frame = 0; frame < 3; ++frame) CHECK(show());
    CHECK(vita_vk_picture(rect) && rect[2] > 0 && rect[3] > 0 && rect[2] * 544 / 960 - rect[3] <= 1);
    for (size_t i = 0; i < sizeof buttons / sizeof *buttons; ++i) {
        vpad_test_middle(buttons[i].control, &x, &y);
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 10, x, y) && vblank() == buttons[i].button);
        CHECK(!g_input.fronts && at_rest()); /* the control's, never the panel's */
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 10, x, y) && !vblank());
        CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y) && vblank() == buttons[i].button);
        CHECK(!g_input.fronts);
        CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, x, y) && !vblank());
    }
    vpad_test_middle(SNX_VPAD_DPAD, &x, &y);
    half = vpad_test_side(0.25f) / 2;
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 13, x, y) && !vblank());
    for (size_t i = 0; i < sizeof arms / sizeof *arms; ++i) {
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 13, x + arms[i].dx * half, y + arms[i].dy * half));
        CHECK(vblank() == arms[i].button);
    }
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 13, x, y) && !vblank());
    /* a tap that came and went between two vblanks is one vblank's, whatever reads it */
    vpad_test_middle(SNX_VPAD_PLUS, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 14, x, y) &&
          vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 14, x, y));
    CHECK(vblank() == VITA_CTRL_START);
    vita_input_read(&g_input);
    CHECK(g_input.buttons == VITA_CTRL_START && !vblank());
    /* both sticks at once: pushed to their edge, down and right 255, up and left 0 */
    half = vpad_test_side(0.30f) / 2;
    vpad_test_middle(SNX_VPAD_STICK_L, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 15, x, y) && !vblank() && at_rest());
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 15, x + half * 2, y) && !vblank());
    CHECK(g_input.lx == 255 && g_input.ly == 128 && g_input.rx == 128 && g_input.ry == 128);
    vpad_test_middle(SNX_VPAD_STICK_R, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 16, x, y));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 16, x, y + half * 2) && !vblank());
    CHECK(g_input.lx == 255 && g_input.ly == 128 && g_input.rx == 128 && g_input.ry == 255 && !g_input.fronts);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 16, x, y - half * 2) && !vblank() && !g_input.ry);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 15, x, y) &&
          vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 16, x, y) && !vblank() && at_rest());

    /* the front panel: fingers on the picture, each its own touch, with a control held beside them */
    float px = (float)rect[0] + (float)rect[2] / 2, py = (float)rect[1] + (float)rect[3] / 2;
    vpad_test_middle(SNX_VPAD_B, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 20, px, py) && !vblank() && front(1, 0, 0, 960, 544));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 21, x, y) && vblank() == VITA_CTRL_CROSS);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 22, px, py + (float)rect[3] / 4));
    CHECK(vblank() == VITA_CTRL_CROSS && front(2, 0, 0, 960, 544) && front(2, 1, 1, 960, 815));
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 22, px, (float)(h + 400)));
    CHECK(vblank() == VITA_CTRL_CROSS && front(2, 1, 1, 960, 1087)); /* kept to the edge */
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 20, px, py) && vblank() == VITA_CTRL_CROSS);
    CHECK(front(1, 0, 1, 960, 1087)); /* the one still down, with its id */
    {
        /* as a game reads them: SceTouch's report of the panel */
        char error[256] = {0}, *root = snx_temp_dir(NULL), *folder = root ? snx_path_join(root, "game") : NULL;
        VitaImage image = {0};
        CHECK(folder && !snx_mkdirs(folder));
        VitaProcess* p = vita_process_new(&image, folder, error, sizeof error);
        CHECK(p);
        uint32_t mem = vita_alloc(p, &p->user, 0x1000, 0x1000);
        const VitaHle* peek = vita_hle_table;
        while (peek->name && strcmp(peek->name, "sceTouchPeek")) ++peek;
        CHECK(peek->name && mem);
        VitaThread* t = &p->threads[0];
        p->current = t;
        t->cpu.r[0] = 0, t->cpu.r[1] = mem, t->cpu.r[2] = 1;
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 24, px, py) && vblank() == VITA_CTRL_CROSS);
        CHECK(peek->function(p, t) == 1 && vita_read32(p, mem + 12) == 2);
        const unsigned char* b = vita_ptr(p, mem);
        CHECK(b[16] == 0 && b[17] == 128 && abs((int)snx_ld_le16(b + 18) - 960) <= 3);
        CHECK(b[32] == 1 && b[33] == 128 && snx_ld_le16(b + 36) == 1087);
        t->cpu.r[0] = 1, t->cpu.r[1] = mem, t->cpu.r[2] = 1; /* the rear one: none of them */
        CHECK(peek->function(p, t) == 1 && !vita_read32(p, mem + 12));
        vita_process_free(p);
        CHECK(!snx_remove_tree(root));
        free(folder);
        free(root);
    }
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 24, px, py) &&
          vpad_test_finger(window, SDL_EVENT_FINGER_CANCELED, 77, 22, px, py) &&
          vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 21, x, y) && !vblank() && !g_input.fronts);
    /* eight fingers: the panel's six, the others none */
    for (SDL_FingerID id = 40; id < 48; ++id)
        CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, id, px, py + (float)(id - 40)));
    CHECK(!vblank() && g_input.fronts == VITA_FRONT_TOUCHES && g_input.front[5].id == 5);
    for (SDL_FingerID id = 40; id < 48; ++id) CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, id, px, py));
    CHECK(!vblank() && !g_input.fronts);
    /* the touch setting: 2 the mouse too, 1 the fingers alone, 0 none */
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, px, py) && !vblank() && front(1, 0, 0, 960, 544));
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_MOTION, px, py + (float)rect[3] / 4) && !vblank());
    CHECK(front(1, 0, 0, 960, 815));
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, px, py) && !vblank() && !g_input.fronts);
    g_settings.touch_screen = 1;
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, px, py) && !vblank() && !g_input.fronts);
    CHECK(vpad_test_mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, px, py) && !vblank());
    g_settings.touch_screen = 0;
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 25, px, py) && !vblank() && !g_input.fronts);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 25, px, py) && !vblank());
    g_settings.touch_screen = 2;

    /* the layout edited: the game gets nothing, a control dragged kept in the .ini and read back */
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 30, px, py) && !vblank() && g_input.fronts == 1);
    CHECK(vpad_test_key(window, SDLK_F2) && !vblank() && vpad_editing() && !g_input.fronts);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 30, px, py) && !vblank());
    vpad_test_middle(SNX_VPAD_STICK_R, &x, &y);
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_DOWN, 77, 31, x, y) && !vblank() && at_rest());
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_MOTION, 77, 31, (float)w / 2, (float)h / 2) && !vblank());
    CHECK(vpad_test_finger(window, SDL_EVENT_FINGER_UP, 77, 31, (float)w / 2, (float)h / 2) && !vblank());
    CHECK(vpad_test_in_file(ini, "[On-screen controls]\ncontrols_stick_r=0.500 0.500 100\n") == 1);
    CHECK(vpad_test_key(window, SDLK_F2) && !vblank() && !vpad_editing());
    /* the pause key holds the game's thread in its vblank until it is pressed again */
    {
        Uint64 at = SDL_GetTicks();
        CHECK(vpad_test_key(window, SDLK_PAUSE) && SDL_AddTimer(200, pause_again, window));
        CHECK(!vblank() && !snx_vpad_window_paused() && SDL_GetTicks() - at >= 150);
    }
    /* the screenshot key: the frame the game shows, as SceDisplay has it, in captures/ beside the game's .ini */
    {
        enum { W = 16, H = 8, BASE = 64 };
        uint8_t* mem = calloc(1, BASE + W * H * 4);
        char *captures = snx_path_join(dir, "captures"), **names = NULL, *path = NULL;
        size_t count = 0, size = 0;
        unsigned char* got = NULL;
        CHECK(mem && captures);
        for (int i = 0; i < W * H * 4; ++i) mem[BASE + i] = (uint8_t)i;
        g_process.mem = mem;
        g_process.display.base = BASE, g_process.display.pitch = W;
        g_process.display.width = W, g_process.display.height = H;
        CHECK(vpad_test_key(window, SDLK_F3) && !vblank());
        CHECK(!snx_list_dir(captures, &names, &count) && count == 1 && !strncmp(names[0], "screenshot_", 11));
        CHECK((path = snx_path_join(captures, names[0])) && (got = snx_read_file(path, &size)));
        CHECK(size == 12 + W * H * 3 && !memcmp(got, "P6\n16 8\n255\n", 12) && got[12 + 3] == 4 && got[12 + 5] == 6);
        g_process.display.base = 0;
        g_process.mem = NULL;
        free(got);
        free(path);
        snx_free_strings(names, count);
        free(captures);
        free(mem);
    }
    /* the upscaler's key: the next value for the run, said over the game, another at the next press */
    {
        char first[64];
        CHECK(vpad_test_key_mod(window, SDLK_U, SDL_KMOD_LCTRL) && !vblank() &&
              !strncmp(vita_vk_notice_text(), "Upscaler: ", 10));
        snprintf(first, sizeof first, "%s", vita_vk_notice_text());
        CHECK(vpad_test_key_mod(window, SDLK_U, SDL_KMOD_LCTRL) && !vblank() && strcmp(vita_vk_notice_text(), first));
    }
    /* F5: the threads listed by the run between two slices, once */
    CHECK(!vita_present_dump_asked() && vpad_test_key(window, SDLK_F5) && !vblank());
    CHECK(vita_present_dump_asked() && !vita_present_dump_asked());
    /* F7 and F8: the game saved to slot 1 between two slices, in states/ of its folder, and loaded back: a word of its
       memory changed after the save is the saved one again */
    {
        char error[256] = {0}, *states = snx_path_join(dir, "states");
        char* slot = states ? snx_path_join(states, "a game.1.snxstate") : NULL;
        VitaImage image = {0};
        VitaProcess* p = vita_process_new(&image, dir, error, sizeof error);
        CHECK(p && slot);
        uint32_t word = vita_alloc(p, &p->user, 0x1000, 0x1000);
        CHECK(word);
        vita_write32(p, word, 0x1234);
        CHECK(vpad_test_key(window, SDLK_F7) && !vblank());
        vita_present_states(p, 0);
        CHECK(snx_is_file(slot));
        vita_write32(p, word, 0x5678);
        CHECK(vpad_test_key(window, SDLK_F8) && !vblank());
        vita_present_states(p, 0);
        CHECK(vita_read32(p, word) == 0x1234);
        vita_present_states(p, 0); /* asked once: nothing more */
        vita_process_free(p);
        CHECK(!snx_remove_tree(states));
        free(slot);
        free(states);
    }
    vpad_console(&snx_vpad_vita);
    vpad_settings(dir, "a game", 0, NULL);
    CHECK(vpad_config()->at[SNX_VPAD_STICK_R].x == 0.5f && vpad_config()->at[SNX_VPAD_STICK_R].y == 0.5f);
    CHECK(vpad_test_key(window, SDLK_AC_BACK) && !vblank() && vpad_editing() && show());
    CHECK(vpad_test_key(window, SDLK_AC_BACK) && vblank() == 0xFFFFFFFFu);
    return 0;
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    sdl_real_pads_hide();
    vpad_console(&snx_vpad_vita);
    g_settings.touch_screen = 2;
    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    char* ini = dir ? snx_path_join(dir, "a game.ini") : NULL;
    CHECK(ini && !snx_write_file(ini, "[Game]\nsystem=vita\n", strlen("[Game]\nsystem=vita\n")));
    vita_present_init("vita_input", ini, NULL);
    int failed = 0, opened = vita_vk() && vita_vk()->host.window;
    if (opened)
        failed = controls(vita_vk()->host.window, dir, ini);
    else
        puts("vita_input: no Vulkan window here: skipped");
    if (vita_vk()) {
        vkDeviceWaitIdle(vita_vk()->device);
        vkDestroyBuffer(vita_vk()->device, g_frame.buffer, NULL);
        vkFreeMemory(vita_vk()->device, g_frame.memory, NULL);
    }
    vita_present_free();
    if (snx_remove_tree(dir)) fprintf(stderr, "vita_input: %s could not be removed\n", dir);
    free(ini);
    free(dir);
    free(root);
    if (failed) return 1;
    if (!opened) return 77;
    puts("vita_input: passed");
    return 0;
}
