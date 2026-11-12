/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/queue.h"
#include "gui/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool live(const GuiJob* job) { return job->state == GUI_JOB_WAITING || job->state == GUI_JOB_RUNNING; }
/* a compile and a recompile of one file are one job: both make its game; an AOT pack is another */
static bool same_kind(GuiJobKind a, GuiJobKind b) { return (a == GUI_JOB_AOT) == (b == GUI_JOB_AOT); }
/* text at at in the job's block, where it goes past it */
static size_t put(GuiJob* job, size_t at, const char* text) {
    memcpy(job->text + at, text, strlen(text) + 1);
    return at + strlen(text) + 1;
}
int gui_queue_add(GuiQueue* queue, const char* const* argv, const char* file, const char* listed, const char* name,
                  GuiJobKind kind, bool native, bool* added) {
    GuiJob* job;
    size_t size = strlen(file) + strlen(listed) + strlen(name) + 3, at;
    int argc = 0, i;
    if (added) *added = false;
    for (i = 0; i < queue->count; ++i)
        if (live(&queue->job[i]) && same_kind(queue->job[i].kind, kind) && !strcmp(gui_queue_file(queue, i), file))
            return i;
    while (argv[argc] && argc < GUI_QUEUE_ARGS) size += strlen(argv[argc++]) + 1 + sizeof(uint32_t);
    if (queue->count == GUI_QUEUE_MAX || argc >= GUI_QUEUE_ARGS || size > UINT32_MAX) return -1;
    job = &queue->job[queue->count];
    memset(job, 0, sizeof *job);
    if (!(job->text = (char*)malloc(size))) return -1;
    at = (size_t)argc * sizeof(uint32_t); /* the strings past their offsets */
    for (i = 0; i < argc; ++i) {
        uint32_t offset = (uint32_t)at;
        memcpy(job->text + (size_t)i * sizeof offset, &offset, sizeof offset);
        at = put(job, at, argv[i]);
    }
    job->argc = argc;
    job->file = (uint32_t)at;
    at = put(job, at, file);
    job->listed = (uint32_t)at;
    at = put(job, at, listed);
    job->name = (uint32_t)at;
    put(job, at, name);
    job->kind = kind;
    job->state = GUI_JOB_WAITING;
    job->native = native;
    job->percent = -1;
    if (gui_queue_running(queue) < 0 && gui_queue_next(queue) < 0) queue->first = queue->serial + 1; /* compiles anew */
    job->serial = ++queue->serial;
    if (added) *added = true;
    return queue->count++;
}
int gui_queue_find(const GuiQueue* queue, const char* path) {
    for (int i = 0; i < queue->count; ++i)
        if (live(&queue->job[i]) &&
            (!strcmp(gui_queue_file(queue, i), path) || !strcmp(gui_queue_listed(queue, i), path)))
            return i;
    return -1;
}
int gui_queue_running(const GuiQueue* queue) {
    for (int i = 0; i < queue->count; ++i)
        if (queue->job[i].state == GUI_JOB_RUNNING) return i;
    return -1;
}
int gui_queue_next(const GuiQueue* queue) {
    for (int i = 0; i < queue->count; ++i)
        if (queue->job[i].state == GUI_JOB_WAITING) return i;
    return -1;
}
int gui_queue_waiting(const GuiQueue* queue) {
    int n = 0;
    for (int i = 0; i < queue->count; ++i) n += queue->job[i].state == GUI_JOB_WAITING;
    return n;
}
int gui_queue_percent(const GuiQueue* queue) {
    int sum = 0, n = 0;
    bool said = false;
    for (int i = 0; i < queue->count; ++i) {
        const GuiJob* job = &queue->job[i];
        if (job->serial < queue->first) continue;
        ++n;
        if (job->state == GUI_JOB_OK || job->state == GUI_JOB_FAILED) {
            sum += 100;
            said = true;
        } else if (job->state == GUI_JOB_RUNNING && job->percent >= 0) {
            sum += job->percent;
            said = true;
        }
    }
    return said ? sum / n : -1;
}
int gui_queue_place(const GuiQueue* queue, int i) {
    int place = 0;
    if (i < 0 || i >= queue->count || !live(&queue->job[i])) return 0;
    /* the running one first wherever it is, then the waiting ones in their order */
    if (queue->job[i].state == GUI_JOB_RUNNING) return 1;
    place = gui_queue_running(queue) >= 0 ? 1 : 0;
    for (int k = 0; k <= i; ++k) place += queue->job[k].state == GUI_JOB_WAITING;
    return place;
}
int gui_queue_argv(const GuiQueue* queue, int i, const char** out) {
    const GuiJob* job = &queue->job[i];
    for (int k = 0; k < job->argc; ++k) {
        uint32_t offset;
        memcpy(&offset, job->text + (size_t)k * sizeof offset, sizeof offset);
        out[k] = job->text + offset;
    }
    out[job->argc] = NULL;
    return job->argc;
}
const char* gui_queue_file(const GuiQueue* queue, int i) { return queue->job[i].text + queue->job[i].file; }
const char* gui_queue_listed(const GuiQueue* queue, int i) { return queue->job[i].text + queue->job[i].listed; }
const char* gui_queue_name(const GuiQueue* queue, int i) { return queue->job[i].text + queue->job[i].name; }
void gui_queue_remove(GuiQueue* queue, int i) {
    if (i < 0 || i >= queue->count) return;
    free(queue->job[i].text);
    memmove(queue->job + i, queue->job + i + 1, (size_t)(queue->count - i - 1) * sizeof *queue->job);
    memset(&queue->job[--queue->count], 0, sizeof *queue->job);
}
void gui_queue_free(GuiQueue* queue) {
    while (queue->count) gui_queue_remove(queue, queue->count - 1);
}
void gui_queue_place_text(int place, char* out, size_t size) {
    /* English's ordinals by their last digits, 11th to 13th apart; a language has each form its own way */
    const char* form = place % 100 / 10 == 1 ? SNX_TR_NOOP("%dth")
                       : place % 10 == 1     ? SNX_TR_NOOP("%dst")
                       : place % 10 == 2     ? SNX_TR_NOOP("%dnd")
                       : place % 10 == 3     ? SNX_TR_NOOP("%drd")
                                             : SNX_TR_NOOP("%dth");
    if (place == 2)
        snprintf(out, size, "%s", snx_tr("next"));
    else
        snprintf(out, size, snx_tr(form), place);
}
