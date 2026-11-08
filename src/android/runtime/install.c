/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Installing an app (install.h). The base APK is the one whose manifest has no split attribute; a split's manifest
   names it, as PackageParser reads them */
#include "android/runtime/install.h"
#include "android/res/axml.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the ABIs whose libraries the native bridge runs, in the order a 64-bit ARM phone takes them (its
   ro.product.cpu.abilist): an APK's best one is extracted, into the folder of the bridge that runs it */
static const struct {
    const char* dir;    /* in the APK */
    const char* folder; /* in the app's lib/ */
} ABIS[] = {{"lib/arm64-v8a/", "arm64"}, {"lib/armeabi-v7a/", "arm"}, {"lib/armeabi/", "arm"}};
enum { ABI_COUNT = sizeof ABIS / sizeof *ABIS };

static char* copy_text(const char* text) {
    size_t n = strlen(text) + 1;
    char* c = malloc(n);
    if (c) memcpy(c, text, n);
    return c;
}

/* a string attribute of the manifest tag, "" when it has none */
static void manifest_attribute(const AxmlParser* p, const AxmlDocument* doc, const char* name, char* out,
                               size_t out_size) {
    out[0] = 0;
    long i = axml_find(p, "", name);
    if (i < 0) return;
    uint32_t raw = axml_attribute_raw(p, (uint32_t)i);
    if (raw == AXML_NONE || res_string(&doc->strings, raw, out, out_size) < 0) out[0] = 0;
}

bool android_install_valid_name(const char* name, bool package) {
    unsigned segments = 0;
    for (const char* p = name;; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return false;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') ++p;
        ++segments;
        if (!*p) return !package || segments >= 2;
        if (*p != '.') return false;
    }
}

/* an APK's package and split name (empty for the base) */
static bool manifest_of(const char* apk, char* package, size_t package_size, char* split, size_t split_size,
                        char* error, size_t error_size) {
    size_t size;
    unsigned char* data = snx_zip_member(apk, "AndroidManifest.xml", &size, error, error_size);
    if (!data) return false;
    AxmlDocument doc;
    char why[128];
    bool ok = axml_open(&doc, data, size, why, sizeof why);
    if (!ok) snprintf(error, error_size, "%s: AndroidManifest.xml: %s", apk, why);
    AxmlParser p;
    if (ok) axml_parser(&p, &doc);
    int e = ok ? axml_next(&p) : AXML_END_DOCUMENT;
    while (e != AXML_END_DOCUMENT && e != AXML_START_TAG) e = axml_next(&p);
    if (ok && e == AXML_START_TAG) {
        manifest_attribute(&p, &doc, "package", package, package_size);
        manifest_attribute(&p, &doc, "split", split, split_size);
    }
    free(data);
    if (ok && !package[0]) {
        snprintf(error, error_size, "%s: the manifest names no package", apk);
        ok = false;
    }
    if (ok && !android_install_valid_name(package, true)) {
        snprintf(error, error_size, "%s: the manifest's package \"%s\" is not a package name", apk, package);
        ok = false;
    }
    if (ok && split[0] && !android_install_valid_name(split, false)) {
        snprintf(error, error_size, "%s: the manifest's split \"%s\" is not a split name", apk, split);
        ok = false;
    }
    return ok;
}

/* the best ABI (an index of ABIS) whose libraries an APK has, ABI_COUNT for none */
static unsigned best_abi(const char* apk) {
    char error[256];
    SnxZip* zip = snx_zip_open(apk, error, sizeof error);
    unsigned best = ABI_COUNT;
    for (uint32_t i = 0; zip && i < snx_zip_count(zip); ++i) {
        const char* name = snx_zip_name(zip, i);
        for (unsigned a = 0; a < best; ++a) {
            size_t n = strlen(ABIS[a].dir);
            if (!strncmp(name, ABIS[a].dir, n) && name[n] && !strchr(name + n, '/')) best = a;
        }
    }
    snx_zip_close(zip);
    return best;
}

/* the app's library folder, of the best ABI that its APKs have (arm64 when they have none) */
static bool library_folder(AndroidInstall* out, char* error, size_t error_size) {
    unsigned best = ABI_COUNT;
    for (unsigned i = 0; i <= out->split_count; ++i) {
        unsigned a = best_abi(i ? out->splits[i - 1] : out->base);
        if (a < best) best = a;
    }
    out->abi = best < ABI_COUNT ? best : 0;
    free(out->library_dir);
    out->library_dir = snx_format("%s/lib/%s", out->app_dir, ABIS[out->abi].folder);
    if (!out->library_dir || snx_mkdirs(out->library_dir)) {
        snprintf(error, error_size, "cannot make the app's library folder in the data folder");
        return false;
    }
    return true;
}

/* the native libraries of the app's ABI in an APK, extracted into its library folder */
static bool extract_libraries(const char* apk, const AndroidInstall* app, char* error, size_t error_size) {
    SnxZip* zip = snx_zip_open(apk, error, error_size);
    if (!zip) return false;
    bool ok = true;
    const char* abi = ABIS[app->abi].dir;
    const char* dir = app->library_dir;
    size_t prefix = strlen(abi);
    for (uint32_t i = 0; ok && i < snx_zip_count(zip); ++i) {
        const char* name = snx_zip_name(zip, i);
        if (strncmp(name, abi, prefix) || !name[prefix] || strchr(name + prefix, '/') || !snx_zip_safe_name(name))
            continue;
        char* path = snx_path_join(dir, name + prefix);
        ok = path && snx_zip_extract(zip, i, path, error, error_size);
        free(path);
    }
    snx_zip_close(zip);
    return ok;
}

void android_install_free(AndroidInstall* app) {
    free(app->base);
    for (unsigned i = 0; i < app->split_count; ++i) {
        free(app->splits[i]);
        free(app->split_names[i]);
    }
    free(app->app_dir);
    free(app->library_dir);
    free(app->data_dir);
    memset(app, 0, sizeof *app);
}

/* an APK of the app: the base, or a split by its name */
static bool add_apk(AndroidInstall* out, const char* apk, const char* split, char* error, size_t error_size) {
    if (!split[0]) {
        if (out->base) {
            snprintf(error, error_size, "two base APKs: %s and %s", out->base, apk);
            return false;
        }
        out->base = copy_text(apk);
        return out->base != NULL;
    }
    if (out->split_count == ANDROID_MAX_SPLITS) {
        snprintf(error, error_size, "more than %d splits", ANDROID_MAX_SPLITS);
        return false;
    }
    out->splits[out->split_count] = copy_text(apk);
    out->split_names[out->split_count] = copy_text(split);
    return out->splits[out->split_count++] && out->split_names[out->split_count - 1];
}

/* the folders of a package in the data folder */
static bool folders(AndroidInstall* out, char* error, size_t error_size) {
    char* data = snx_user_data_dir();
    out->app_dir = data ? snx_format("%s/android/app/%s", data, out->package) : NULL;
    out->data_dir = data ? snx_format("%s/android/data/%s", data, out->package) : NULL;
    free(data);
    if (!out->app_dir || !out->data_dir || snx_mkdirs(out->data_dir)) {
        snprintf(error, error_size, "cannot make the app's folders in the data folder");
        return false;
    }
    return true;
}

/* a bundle's APKs into the app's folder, named by their manifests */
static bool unpack(const char* bundle, SnxZip* zip, AndroidInstall* out, char* error, size_t error_size) {
    char* stage = NULL;
    bool ok = true;
    for (uint32_t i = 0; ok && i < snx_zip_count(zip); ++i) {
        const char* name = snx_zip_name(zip, i);
        size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 4, ".apk") || !snx_zip_safe_name(name)) continue;
        char package[256], split[256];
        if (!stage) {
            /* the package's folders come from the first APK's manifest: each APK of the bundle names it */
            char* tmp = snx_temp_dir(NULL);
            stage = tmp ? snx_path_join(tmp, "bundle.apk") : NULL;
            free(tmp);
            if (!stage) {
                snprintf(error, error_size, "no folder for temporary files");
                return false;
            }
        }
        ok = snx_zip_extract(zip, i, stage, error, error_size) &&
             manifest_of(stage, package, sizeof package, split, sizeof split, error, error_size);
        if (ok && !out->package[0]) {
            snprintf(out->package, sizeof out->package, "%s", package);
            ok = folders(out, error, error_size);
        }
        if (ok && strcmp(package, out->package)) {
            snprintf(error, error_size, "%s: %s is of %s, not %s", bundle, name, package, out->package);
            ok = false;
        }
        char* dest = NULL;
        if (ok) {
            char* file = split[0] ? snx_format("split_%s.apk", split) : snx_format("base.apk");
            dest = file ? snx_path_join(out->app_dir, file) : NULL;
            free(file);
            ok = dest && !snx_copy_file(stage, dest) && add_apk(out, dest, split, error, error_size);
            if (!dest || !ok) snprintf(error, error_size, "cannot install %s", name);
        }
        free(dest);
        remove(stage);
    }
    if (stage) {
        char* dir = snx_dirname(stage);
        if (dir) snx_remove_tree(dir);
        free(dir);
    }
    free(stage);
    if (ok && !out->base) {
        snprintf(error, error_size, "%s has no base APK", bundle);
        ok = false;
    }
    return ok;
}

/* The stamp of a bundle installed: a file in android/app named by a hash of the bundle's path. Its lines are the
   bundle's path, its size, the package, then each split's name */
static char* stamp_path(const char* bundle) {
    char* data = snx_user_data_dir();
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, bundle, strlen(bundle));
    char* path = data ? snx_format("%s/android/app/.bundle-%016llx", data, (unsigned long long)h) : NULL;
    free(data);
    return path;
}
static char* stamp_head(const char* bundle) {
    return snx_format("%s\n%llu\n", bundle, (unsigned long long)snx_file_size(bundle));
}

/* the APKs a stamp lists, when it is the bundle's as it is now */
static bool from_stamp(const char* bundle, AndroidInstall* out, char* error, size_t error_size) {
    char* path = stamp_path(bundle);
    char* head = stamp_head(bundle);
    size_t size = 0;
    unsigned char* text = path && head ? snx_read_file(path, &size) : NULL;
    bool ok = text && size >= strlen(head) && !memcmp(text, head, strlen(head));
    if (ok) {
        /* the copy is the stamp's text, zero-terminated, cut into its lines */
        char* lines = malloc(size + 1);
        ok = lines != NULL;
        if (ok) {
            memcpy(lines, text, size);
            lines[size] = 0;
            char* line = lines + strlen(head);
            char* end = strchr(line, '\n');
            ok = end && end != line && (size_t)(end - line) < sizeof out->package;
            if (ok) {
                *end = 0;
                snprintf(out->package, sizeof out->package, "%s", line);
                ok = folders(out, error, error_size);
            }
            char* base = ok ? snx_path_join(out->app_dir, "base.apk") : NULL;
            ok = base && snx_is_file(base) && add_apk(out, base, "", error, error_size);
            free(base);
            for (line = end ? end + 1 : NULL; ok && line && *line; line = end + 1) {
                end = strchr(line, '\n');
                if (!end) break;
                *end = 0;
                char* file = snx_format("%s/split_%s.apk", out->app_dir, line);
                ok = file && snx_is_file(file) && add_apk(out, file, line, error, error_size);
                free(file);
            }
        }
        free(lines);
    }
    if (ok) ok = library_folder(out, error, error_size);
    free(text);
    free(head);
    free(path);
    if (!ok) android_install_free(out);
    return ok;
}

static void write_stamp(const char* bundle, const AndroidInstall* app) {
    char* path = stamp_path(bundle);
    char* text = stamp_head(bundle);
    char* more = text ? snx_format("%s%s\n", text, app->package) : NULL;
    free(text);
    text = more;
    for (unsigned i = 0; text && i < app->split_count; ++i) {
        more = snx_format("%s%s\n", text, app->split_names[i]);
        free(text);
        text = more;
    }
    if (path && text) snx_write_file(path, text, strlen(text));
    free(path);
    free(text);
}

bool android_install(const char* path, AndroidInstall* out, char* error, size_t error_size) {
    memset(out, 0, sizeof *out);
    if (from_stamp(path, out, error, error_size)) return true;
    SnxZip* zip = snx_zip_open(path, error, error_size);
    if (!zip) return false;
    uint32_t index;
    bool ok = true;
    if (snx_zip_find(zip, "AndroidManifest.xml", &index)) {
        /* an APK runs where it is */
        char split[256];
        ok = manifest_of(path, out->package, sizeof out->package, split, sizeof split, error, error_size) &&
             folders(out, error, error_size) && add_apk(out, path, "", error, error_size) &&
             library_folder(out, error, error_size) && extract_libraries(path, out, error, error_size);
        snx_zip_close(zip);
        if (!ok) android_install_free(out);
        return ok;
    }
    fprintf(stderr, "scarletnx-android: installing %s\n", path);
    ok = unpack(path, zip, out, error, error_size);
    snx_zip_close(zip);
    if (ok) ok = library_folder(out, error, error_size);
    for (unsigned i = 0; ok && i <= out->split_count; ++i)
        ok = extract_libraries(i ? out->splits[i - 1] : out->base, out, error, error_size);
    if (ok) write_stamp(path, out);
    else android_install_free(out);
    return ok;
}
