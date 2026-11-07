/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/steps.h"
#include "gui/i18n.h"
#include "common/utilities/run_theme.h" /* snx_run_seconds_left */
#include <stdio.h>
#include <string.h>

void gui_steps_reset(GuiSteps* steps) { memset(steps, 0, sizeof *steps); }

static GuiStep* current(GuiSteps* steps) { return steps->count ? &steps->step[steps->count - 1] : NULL; }
static void begin(GuiSteps* steps, const char* label, const char* detail, size_t detail_length, uint64_t now) {
    GuiStep* step;
    GuiStep* last = current(steps);
    if (last && last->state == GUI_STEP_ACTIVE) last->state = GUI_STEP_DONE;
    if (steps->count == GUI_STEPS_MAX)
        memmove(steps->step, steps->step + 1, sizeof steps->step - sizeof *steps->step);
    else
        steps->count++;
    step = current(steps);
    memset(step, 0, sizeof *step);
    snprintf(step->label, sizeof step->label, "%s", label);
    if (detail_length >= sizeof step->detail) detail_length = sizeof step->detail - 1;
    memcpy(step->detail, detail, detail_length);
    step->shown_ms = now;
    step->serial = ++steps->serial;
}
static void note(GuiSteps* steps, const char* s, uint64_t now) {
    GuiNote* n;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s || !steps->count) return;
    if (steps->notes == GUI_NOTES_MAX)
        memmove(steps->note, steps->note + 1, sizeof steps->note - sizeof *steps->note);
    else
        steps->notes++;
    n = &steps->note[steps->notes - 1];
    snprintf(n->text, sizeof n->text, "%s", s);
    n->shown_ms = now;
    n->step = steps->step[steps->count - 1].serial;
}
static int prefix(const char* line, const char* text) { return !strncmp(line, text, strlen(text)); }
static void line(GuiSteps* steps, const char* s, uint64_t now) {
    GuiStep* last = current(steps);
    uint32_t before = steps->serial;
    unsigned value, total;
    const char* block = strstr(s, "compiled block ");
    if (block && last && !strcmp(last->label, "RECOMPILE") &&
        sscanf(block, "compiled block %u/%u", &value, &total) == 2) {
        last->value = value;
        last->total = total;
    } else if (last && !strcmp(last->label, "RECOMPILE") &&
               sscanf(s, "Compiled %u/%u chunks of blocks", &value, &total) == 2) {
        /* Vita compiles its discovered AArch32 blocks in parallel chunks, rather than the Switch driver's
           per-function progress lines. The completed chunk count is its progress unit. */
        last->value = value;
        last->total = total;
    } else if (last && !strcmp(last->label, "EXTRACT") && sscanf(s, " extracted %u of %u MB", &value, &total) == 2) {
        /* the driver's row a second, over itself: the step's own line shows it, no note under it */
        last->value = value;
        last->total = total;
        last->megabytes = 1;
        return;
    } else if (prefix(s, "Extracting ")) /* the labels show translated */
        begin(steps, SNX_TR_NOOP("EXTRACT"), s + 11, strlen(s + 11), now);
    else if (prefix(s, "Recompiling ")) {
        const char* name = s + 12;
        const char* end = strstr(name, " (");
        begin(steps, SNX_TR_NOOP("RECOMPILE"), name, end ? (size_t)(end - name) : strlen(name), now);
    } else if (prefix(s, "Built ")) {
        begin(steps, SNX_TR_NOOP("BUILD"), s + 6, strlen(s + 6), now);
        current(steps)->state = GUI_STEP_DONE;
    } else if (prefix(s, "ScarletNX - "))
        begin(steps, SNX_TR_NOOP("LAUNCH"), s + 12, strlen(s + 12), now);
    else if (prefix(s, "Ready "))
        begin(steps, SNX_TR_NOOP("READY"), s + 6, strlen(s + 6), now);
    else if (prefix(s, "vulkan: ") && !steps->gpu && strstr(s, ", Vulkan ")) {
        /* the first device line: "vulkan: <device> [(<driver>)], Vulkan <version>, ..."; a later "(" is not the
           device's */
        const char* device = s + 8;
        const char* end = strstr(device, ", Vulkan ");
        const char* paren = strstr(device, " (");
        if (paren && paren < end) end = paren;
        begin(steps, SNX_TR_NOOP("GPU"), device, (size_t)(end - device), now);
        steps->gpu = 1;
    }
    if (steps->serial == before) note(steps, s, now); /* not a step of its own: a line of the current one */
}
void gui_steps_feed(GuiSteps* steps, const char* data, size_t size, uint64_t now_ms) {
    size_t i;
    for (i = 0; i < size; ++i) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            steps->line[steps->length] = 0;
            if (steps->length) line(steps, steps->line, now_ms);
            steps->length = 0;
        } else if (steps->length + 1 < sizeof steps->line)
            steps->line[steps->length++] = c;
    }
}
void gui_steps_finish(GuiSteps* steps, int ok) {
    GuiStep* last;
    if (steps->length) gui_steps_feed(steps, "\n", 1, current(steps) ? current(steps)->shown_ms : 0);
    last = current(steps);
    if (last && last->state == GUI_STEP_ACTIVE)
        last->state = ok ? GUI_STEP_DONE : GUI_STEP_FAILED;
    else if (last && !ok)
        last->state = GUI_STEP_FAILED;
}
int gui_steps_percent(const GuiSteps* steps) {
    int i;
    int percent = -1;
    for (i = 0; i < steps->count; ++i) {
        const GuiStep* s = &steps->step[i];
        if (!strcmp(s->label, "RECOMPILE"))
            percent = s->state == GUI_STEP_DONE ? 100 : s->total ? (int)(s->value * 100ull / s->total) : 0;
        else if (!strcmp(s->label, "EXTRACT"))
            percent = s->state == GUI_STEP_DONE ? 100 : s->total ? (int)(s->value * 100ull / s->total) : -1;
        else if (!strcmp(s->label, "BUILT") || !strcmp(s->label, "LAUNCH") || !strcmp(s->label, "READY"))
            percent = 100;
    }
    return percent;
}
void gui_step_progress(const GuiStep* s, int how, uint64_t now, char* out, size_t size) {
    uint64_t spent = now > s->shown_ms ? now - s->shown_ms : 0, done = s->value < s->total ? s->value : s->total, left;
    if (!s->total)
        snprintf(out, size, "%s", "");
    else if (how == GUI_PROGRESS_PERCENT)
        snprintf(out, size, "%u%%", (unsigned)(done * 100 / s->total));
    else if (how == GUI_PROGRESS_TIME && snx_run_seconds_left(spent, done, s->total) < 0)
        snprintf(out, size, "%s", snx_tr("--:-- left"));
    else if (how == GUI_PROGRESS_TIME) {
        left = (uint64_t)snx_run_seconds_left(spent, done, s->total);
        if (left >= 3600)
            snprintf(out, size, snx_tr("%u:%02u:%02u left"), (unsigned)(left / 3600), (unsigned)(left / 60 % 60),
                     (unsigned)(left % 60));
        else
            snprintf(out, size, snx_tr("%02u:%02u left"), (unsigned)(left / 60), (unsigned)(left % 60));
    } else if (s->megabytes && s->total >= 1024)
        snprintf(out, size, "%.1f / %.1f GB", s->value / 1024.0, s->total / 1024.0);
    else
        snprintf(out, size, s->megabytes ? "%u / %u MB" : "%u / %u", s->value, s->total);
}
const GuiStep* gui_steps_last(const GuiSteps* steps, const char* label) {
    int i;
    for (i = steps->count - 1; i >= 0; --i)
        if (!strcmp(steps->step[i].label, label)) return &steps->step[i];
    return NULL;
}
int gui_step_notes(const GuiSteps* steps, const GuiStep* step, const GuiNote** out, int max) {
    int i;
    int count = 0;
    for (i = steps->notes - 1; i >= 0 && count < max; --i)
        if (steps->note[i].step == step->serial) out[count++] = &steps->note[i];
    for (i = 0; i < count / 2; ++i) {
        const GuiNote* swap = out[i];
        out[i] = out[count - 1 - i];
        out[count - 1 - i] = swap;
    }
    return count;
}
