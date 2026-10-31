/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver.h. A package checked before anything is written, as Eden's GpuDriverHelper.kt does it (meta.json and
   its minApi; no entry outside the folder, FileUtil.kt's zip-slip check), and Eden's Freedreno presets
   (NativeFreedrenoConfig.kt's FreedrenoPresets) */
#include "gui/gpu_driver.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "gui/i18n.h"
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const GuiFreedrenoPreset gui_freedreno_presets[] = {
    {SNX_TR_NOOP("Debug - CPU Memory"), SNX_TR_NOOP("Use CPU memory (slower but more stable)"), "TU_DEBUG=sysmem"},
    {SNX_TR_NOOP("Debug - No UBWC"), SNX_TR_NOOP("Disable UBWC compression for debugging"), "TU_DEBUG=noubwc"},
    {SNX_TR_NOOP("Debug - No Binning"), SNX_TR_NOOP("Disable binning optimization"), "TU_DEBUG=nobin"},
    {SNX_TR_NOOP("Capture - Renderpass"), SNX_TR_NOOP("Capture command stream data for debugging"),
     "FD_RD_DUMP=enable"},
    {SNX_TR_NOOP("Capture - First 100 Frames"), SNX_TR_NOOP("Capture command stream for first 100 frames only"),
     "FD_RD_DUMP=enable;FD_RD_DUMP_FRAMES=0-100"},
    {SNX_TR_NOOP("Shader Debug"), SNX_TR_NOOP("Enable IR3 shader compiler debugging"),
     "IR3_SHADER_DEBUG=nouboopt,spillall"},
    {SNX_TR_NOOP("GPU Hang Trace"), SNX_TR_NOOP("Trace GPU progress for debugging hangs"), "TU_BREADCRUMBS=1"},
    {SNX_TR_NOOP("Dev - UBWC Flag Hint"), SNX_TR_NOOP("Enable TP UBWC flag hint for development"),
     "FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1"},
    {SNX_TR_NOOP("Performance - Default"), SNX_TR_NOOP("Clear all debug options for performance"), ""},
    {NULL, NULL, NULL}};

/* a zip entry's name that is a file at the package's top: no folder, nothing hidden, no way out of it */
static int top_file(const char* name) {
    return name[0] && name[0] != '.' && !strchr(name, '/') && !strchr(name, '\\') && !strchr(name, ':');
}

int gui_gpu_driver_add(const void* zip, size_t size, const char* file, const char* dir, int api, char* id,
                       size_t id_size, char* said, size_t said_size) {
    mz_zip_archive z;
    mz_uint i, count, meta_at = (mz_uint)-1, library_at = (mz_uint)-1;
    mz_zip_archive_file_stat st;
    SnxGpuDriverMeta meta, there_meta;
    char why[600], *meta_text = NULL, *folder = NULL, *staging = NULL, *path = NULL, *there = NULL, *stage_name;
    const char* leaf = snx_path_leaf(file);
    size_t meta_size = 0, there_size = 0;
    unsigned long long total = 0;
    int result = GUI_DRIVER_REFUSED;
    memset(&z, 0, sizeof z);
    id[0] = 0;
    if (!mz_zip_reader_init_mem(&z, zip, size, 0)) {
        snprintf(said, said_size, snx_tr("Not added: %s is not a zip (%s)"), leaf,
                 mz_zip_get_error_string(mz_zip_get_last_error(&z)));
        return GUI_DRIVER_REFUSED;
    }
    count = mz_zip_reader_get_num_files(&z);
    for (i = 0; i < count; ++i) {
        if (!mz_zip_reader_file_stat(&z, i, &st)) {
            snprintf(said, said_size, snx_tr("Not added: %s's entry %u does not read (%s)"), leaf, (unsigned)i,
                     mz_zip_get_error_string(mz_zip_get_last_error(&z)));
            goto done;
        }
        if (!strcmp(st.m_filename, "meta.json")) meta_at = i;
        if (st.m_is_directory || !top_file(st.m_filename)) {
            snprintf(said, said_size, snx_tr("Not added: %s's %s is %s; a driver's files are at the zip's top"), leaf,
                     st.m_filename, st.m_is_directory ? snx_tr("a folder") : snx_tr("not a file at the zip's top"));
            goto done;
        }
        total += st.m_uncomp_size;
    }
    if (total > GUI_GPU_DRIVER_MAX) {
        snprintf(said, said_size, snx_tr("Not added: %s unpacks to %llu bytes, over the %llu a driver takes"), leaf,
                 total, GUI_GPU_DRIVER_MAX);
        goto done;
    }
    if (meta_at == (mz_uint)-1) {
        snprintf(said, said_size, snx_tr("Not added: %s has no meta.json at its top"), leaf);
        goto done;
    }
    mz_zip_reader_file_stat(&z, meta_at, &st);
    if (st.m_uncomp_size > SNX_GPU_DRIVER_META_MAX) {
        snprintf(said, said_size, snx_tr("Not added: %s's meta.json is %llu bytes, over the %d a driver's takes"), leaf,
                 (unsigned long long)st.m_uncomp_size, SNX_GPU_DRIVER_META_MAX);
        goto done;
    }
    meta_text = (char*)mz_zip_reader_extract_to_heap(&z, meta_at, &meta_size, 0);
    if (!meta_text) {
        snprintf(said, said_size, snx_tr("Not added: %s's meta.json does not unpack (%s)"), leaf,
                 mz_zip_get_error_string(mz_zip_get_last_error(&z)));
        goto done;
    }
    if (!snx_gpu_driver_meta(meta_text, meta_size, &meta, why, sizeof why)) {
        snprintf(said, said_size, snx_tr("Not added: %s: %s"), leaf, why);
        goto done;
    }
    for (i = 0; i < count; ++i)
        if (mz_zip_reader_file_stat(&z, i, &st) && !strcmp(st.m_filename, meta.library)) library_at = i;
    if (library_at == (mz_uint)-1) {
        snprintf(said, said_size, snx_tr("Not added: %s has no %s, the library its meta.json names"), leaf,
                 meta.library);
        goto done;
    }
    if (api > 0 && meta.min_api > api) {
        snprintf(said, said_size, snx_tr("Not added: %s needs Android API %d; this device has API %d"), meta.name,
                 meta.min_api, api);
        goto done;
    }
    if (!snx_gpu_driver_id(file, id, id_size, why, sizeof why)) {
        snprintf(said, said_size, snx_tr("Not added: %s"), why);
        goto done;
    }
    folder = snx_path_join(dir, id);
    path = folder ? snx_path_join(folder, "meta.json") : NULL;
    there = path ? (char*)snx_read_file(path, &there_size) : NULL;
    if (there && there_size == meta_size && !memcmp(there, meta_text, meta_size)) {
        snprintf(said, said_size, snx_tr("Already added: %s (%s/%s)"), meta.name, SNX_GPU_DRIVERS_DIR, id);
        result = GUI_DRIVER_THERE;
        goto done;
    }
    if (folder && snx_path_exists(folder)) {
        int read = there && snx_gpu_driver_meta(there, there_size, &there_meta, why, sizeof why);
        snprintf(said, said_size, snx_tr("Not added: %s/%s holds %s %s already; remove it first"), SNX_GPU_DRIVERS_DIR,
                 id, read ? there_meta.name : snx_tr("a package whose meta.json does not read,"),
                 read ? there_meta.driver_version : "");
        goto done;
    }
    /* unpacked beside its folder and put in place whole: a failure leaves no half package */
    stage_name = snx_format(".%s.tmp", id);
    staging = stage_name ? snx_path_join(dir, stage_name) : NULL;
    free(stage_name);
    if (!folder || !staging) {
        snprintf(said, said_size, snx_tr("Not added: no memory for %s's paths"), leaf);
        goto done;
    }
    snx_remove_tree(staging);
    if (snx_mkdirs(staging)) {
        snprintf(said, said_size, snx_tr("Not added: %s could not be made (%s)"), staging,
                 snx_os_error(why, sizeof why));
        goto done;
    }
    for (i = 0; i < count; ++i) {
        size_t n = 0;
        void* data;
        char* to;
        int failed;
        mz_zip_reader_file_stat(&z, i, &st);
        data = mz_zip_reader_extract_to_heap(&z, i, &n, 0);
        if (!data) {
            snprintf(said, said_size, snx_tr("Not added: %s's %s does not unpack (%s)"), leaf, st.m_filename,
                     mz_zip_get_error_string(mz_zip_get_last_error(&z)));
            goto unstage;
        }
        if (i == library_at && !snx_gpu_driver_elf((const unsigned char*)data, n, why, sizeof why)) {
            snprintf(said, said_size, snx_tr("Not added: %s's %s %s"), leaf, st.m_filename, why);
            mz_free(data);
            goto unstage;
        }
        to = snx_path_join(staging, st.m_filename);
        failed = !to || snx_write_file(to, data, n);
        mz_free(data);
        if (failed) {
            snprintf(said, said_size, snx_tr("Not added: %s could not be written (%s)"), to ? to : st.m_filename,
                     snx_os_error(why, sizeof why));
            free(to);
            goto unstage;
        }
        free(to);
    }
    if (rename(staging, folder)) {
        snprintf(said, said_size, snx_tr("Not added: %s could not become %s (%s)"), staging, folder,
                 snx_os_error(why, sizeof why));
        goto unstage;
    }
    snprintf(said, said_size, snx_tr("Added %s (%s), in %s/%s"), meta.name,
             meta.driver_version[0] ? meta.driver_version : snx_tr("no driverVersion"), SNX_GPU_DRIVERS_DIR, id);
    result = GUI_DRIVER_ADDED;
    goto done;
unstage:
    snx_remove_tree(staging);
done:
    if (result == GUI_DRIVER_REFUSED) id[0] = 0;
    mz_zip_reader_end(&z);
    mz_free(meta_text);
    free(there);
    free(path);
    free(folder);
    free(staging);
    return result;
}

/* by name in any case, then by folder */
static int by_name(const void* a, const void* b) {
    const GuiGpuDriver *x = (const GuiGpuDriver*)a, *y = (const GuiGpuDriver*)b;
    const char *p = x->meta.name[0] ? x->meta.name : x->id, *q = y->meta.name[0] ? y->meta.name : y->id;
    while (*p && snx_to_lower((unsigned char)*p) == snx_to_lower((unsigned char)*q)) {
        ++p;
        ++q;
    }
    if (snx_to_lower((unsigned char)*p) != snx_to_lower((unsigned char)*q))
        return snx_to_lower((unsigned char)*p) - snx_to_lower((unsigned char)*q);
    return strcmp(x->id, y->id);
}

GuiGpuDriver* gui_gpu_drivers_list(const char* dir, size_t* count) {
    char** names = NULL;
    size_t n = 0, i, kept = 0;
    GuiGpuDriver* list;
    *count = 0;
    if (!dir || snx_list_dir(dir, &names, &n) || !n) {
        snx_free_strings(names, n);
        return NULL;
    }
    list = (GuiGpuDriver*)calloc(n, sizeof *list);
    for (i = 0; list && i < n; ++i) {
        char *folder = snx_path_join(dir, names[i]), *path = folder ? snx_path_join(folder, "meta.json") : NULL;
        unsigned char* text;
        size_t size = 0;
        GuiGpuDriver* d = &list[kept];
        if (names[i][0] == '.' || !folder || !snx_is_dir(folder) || !snx_gpu_driver_id_valid(names[i])) {
            free(path);
            free(folder);
            continue;
        }
        snprintf(d->id, sizeof d->id, "%s", names[i]);
        text = path ? snx_read_file(path, &size) : NULL;
        if (!text)
            snprintf(d->error, sizeof d->error, "%s/%s has no meta.json", SNX_GPU_DRIVERS_DIR, names[i]);
        else if (!snx_gpu_driver_meta((const char*)text, size, &d->meta, d->error, sizeof d->error))
            d->meta.name[0] = 0;
        free(text);
        free(path);
        free(folder);
        ++kept;
    }
    snx_free_strings(names, n);
    if (!kept) {
        free(list);
        return NULL;
    }
    qsort(list, kept, sizeof *list, by_name);
    *count = kept;
    return list;
}

int gui_gpu_driver_remove(const char* dir, const char* id, char* said, size_t size) {
    char* folder = snx_gpu_driver_id_valid(id) ? snx_path_join(dir, id) : NULL;
    char why[300];
    int ok = folder && snx_is_dir(folder) && !snx_remove_tree(folder);
    if (ok)
        snprintf(said, size, snx_tr("Removed %s/%s"), SNX_GPU_DRIVERS_DIR, id);
    else if (!folder)
        snprintf(said, size, snx_tr("Not removed: %s is no driver's folder name"), id);
    else if (!snx_is_dir(folder))
        snprintf(said, size, snx_tr("Not removed: %s is not there"), folder);
    else
        snprintf(said, size, snx_tr("Not removed: %s could not be deleted (%s)"), folder,
                 snx_os_error(why, sizeof why));
    free(folder);
    return ok;
}

char* gui_run_option(const char* key, const char* game_ini, const char* global) {
    if ((game_ini && snx_ini_has(game_ini, key)) || !global || !global[0]) return NULL;
    return snx_format("--%s=%s", key, global);
}
