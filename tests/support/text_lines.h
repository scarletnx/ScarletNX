/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_TEXT_LINES_H
#define SNX_TEST_TEXT_LINES_H

/* text, as a stream of text mode wrote it, its lines ended in \n alone: Windows' end in \r\n */
static inline void snx_test_text_lines(char* text) {
    char* to = text;
    for (const char* from = text; *from; ++from)
        if (from[0] != '\r' || from[1] != '\n') *to++ = *from;
    *to = 0;
}
#endif
