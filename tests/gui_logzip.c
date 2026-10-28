/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Share Log's zip (gui/logzip.h): each log under its name, deflated, with its last change; the homes as ~ in every
   one; summary.txt with the files left out; a zip that cannot be written says where and why */
#include "gui/logzip.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the zip's file name, as text, owned; NULL when it has none */
static char* entry(mz_zip_archive* zip, const char* name, mz_zip_archive_file_stat* stat) {
    int at = mz_zip_reader_locate_file(zip, name, NULL, 0);
    size_t n = 0;
    char *data, *text;
    if (at < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)at, stat)) return NULL;
    data = (char*)mz_zip_reader_extract_to_heap(zip, (mz_uint)at, &n, 0);
    if (!data) return NULL;
    text = snx_format("%.*s", (int)n, data);
    mz_free(data);
    return text;
}

int main(void) {
    char *dir = snx_temp_dir(NULL), *a, *b, *gone, *zip_path, *bad, *text, *big, error[1024];
    GuiZipFile files[3];
    mz_zip_archive zip;
    mz_zip_archive_file_stat stat;
    unsigned char* bytes;
    size_t size, i;
    static const char said[] = "a run at /home/someone/games/x crashed\nC:\\Users\\Someone\\AppData\\x opened\n"
                               "/data/user/0/org.scarletnx/files/state left\n/homework stays\n";
    assert(dir);
    a = snx_path_join(dir, "a.log");
    b = snx_path_join(dir, "b.log");
    gone = snx_path_join(dir, "gone.log");
    zip_path = snx_path_join(dir, "logs.zip");
    assert(!snx_write_file(a, said, sizeof said - 1));
    big = (char*)malloc(200 << 10);
    assert(big);
    for (i = 0; i < (200u << 10); i++) big[i] = "a line of a long run\n"[i % 21];
    assert(!snx_write_file(b, big, 200 << 10));
    assert(!snx_set_file_time(a, 1767225600)); /* 2026-01-01 00:00:00 UTC */
    files[0] = (GuiZipFile){a, "launcher/a.log"};
    files[1] = (GuiZipFile){b, "games/Some Game [0100ABCD12340000]/2026-10-28_14-05-33.log"};
    files[2] = (GuiZipFile){gone, "games/Gone/2026-10-28_14-05-34.log"};

    assert(!gui_logs_zip(zip_path, "Message: saved at /home/someone/x\n", files, 3, error, sizeof error));
    assert(!error[0]);
    bytes = snx_read_file(zip_path, &size);
    assert(bytes);
    mz_zip_zero_struct(&zip);
    assert(mz_zip_reader_init_mem(&zip, bytes, size, 0));
    assert(mz_zip_reader_get_num_files(&zip) == 3); /* the gone one left out */

    /* the homes as ~, every kind, and a word that only starts like one kept */
    text = entry(&zip, "launcher/a.log", &stat);
    assert(text && !strcmp(text, "a run at ~/games/x crashed\n~\\AppData\\x opened\n~/files/state left\n"
                                 "/homework stays\n"));
    assert(stat.m_time >= 1767225600 - 2 && stat.m_time <= 1767225600 + 2); /* a zip's times are to 2 s */
    free(text);
    /* deflated: a repeated run of lines comes out far smaller */
    text = entry(&zip, "games/Some Game [0100ABCD12340000]/2026-10-28_14-05-33.log", &stat);
    assert(text && strlen(text) == (200u << 10) && !memcmp(text, big, 200 << 10));
    assert(stat.m_method == MZ_DEFLATED && stat.m_comp_size * 20 < stat.m_uncomp_size);
    free(text);
    /* the summary: its homes as ~ too, and the file left out with its path and why */
    text = entry(&zip, "summary.txt", &stat);
    {
        static const char start[] = "Message: saved at ~/x\n\nNot in the zip: ";
        assert(text && !strncmp(text, start, sizeof start - 1) && strstr(text, "gone.log ("));
    }
    free(text);
    mz_zip_reader_end(&zip);
    free(bytes);

    /* a folder that is not there: nothing made, its path and why */
    bad = snx_path_join(gone, "logs.zip");
    assert(gui_logs_zip(bad, "x\n", files, 1, error, sizeof error));
    assert(!strncmp(error, bad, strlen(bad)) && strstr(error, " could not be written: ") && !snx_path_exists(bad));

    free(bad);
    free(big);
    free(zip_path);
    free(gone);
    free(b);
    free(a);
    assert(!snx_remove_tree(dir));
    free(dir);
    puts("gui_logzip passed");
    return 0;
}
