/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The frame a screen's present dumped (present.c's write_ppm): its file, frame-<screen>-<NN>.ppm, read whole */
#ifndef SNX_TEST_FRAME_DUMP_H
#define SNX_TEST_FRAME_DUMP_H
#include <stdio.h>
#include <stdlib.h>

/* <dir>/frame-<screen>-<index>.ppm's pixels, three bytes each, row after row, with its width and height: NULL when
   the file is missing or is not an 8-bit P6. The caller frees the pixels */
static inline unsigned char* test_ctr_frame_dump(const char* dir, unsigned screen, unsigned index, unsigned* width,
                                                 unsigned* height) {
    char path[1024];
    snprintf(path, sizeof path, "%s/frame-%u-%02u.ppm", dir, screen, index);
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    unsigned w = 0, h = 0;
    unsigned char* pixels = NULL;
    if (fscanf(file, "P6 %u %u 255", &w, &h) == 2 && fgetc(file) != EOF && w && h) {
        size_t size = (size_t)w * h * 3;
        pixels = (unsigned char*)malloc(size);
        if (pixels && fread(pixels, 1, size, file) != size) {
            free(pixels);
            pixels = NULL;
        }
    }
    fclose(file);
    if (!pixels) return NULL;
    *width = w;
    *height = h;
    return pixels;
}
#endif
