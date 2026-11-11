/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/utilities/elf_dynamic.c on libraries made here (elf_image.h): a file's library names written over where they
   are the ones asked for and nowhere else, each file that is not as it must be refused with its values; a loaded
   image's symbols found by its GNU hash table, with its addresses as bionic or as glibc leaves them; and this
   process's own dynamic linker, found as a run finds Android's, looked up against dlsym */
#include "common/utilities/elf_dynamic.h"
#include "elf_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* elf_dynamic_host.c: this process's own dynamic linker looked up, where the test knows one; what it found */
const char* elf_dynamic_host(void);

#define ROOM 4096
static const SnxElfRename names[] = {
    {"libgsl.so", "libgsL.so"}, {"libadreno_utils.so", "libadreno_utilS.so"}, {"vulkan.adreno.so", "vulkan.adrenO.so"}};
static const ElfImage driver = {.soname = "vulkan.adreno.so",
                                .needed = {"libc.so", "libgsl.so", "libadreno_utils.so", "libm.so"},
                                .versions = {"libc.so", "libgsl.so"},
                                .symbols = {"vkGetInstanceProcAddr", "HMI"}};

static int has(const char* text, const char* part) { return strstr(text, part) != NULL; }
/* how many bytes of two images differ */
static size_t differ(const unsigned char* a, const unsigned char* b, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; ++i) n += a[i] != b[i];
    return n;
}
/* a made image changed by edit, refused: why, 400 bytes of room */
static const char* refused(const ElfImage* e,
                           void (*edit)(unsigned char* image, const ElfImageMade* made, size_t* size), char* why) {
    unsigned char image[ROOM];
    ElfImageMade made = elf_image(e, image, sizeof image);
    size_t size = made.size;
    edit(image, &made, &size);
    why[0] = 0;
    assert(snx_elf_rename(image, size, names, 3, why, 400) == -1);
    return why;
}
static void edit_text(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)made;
    memcpy(image, "not a library", 13);
    *size = 13;
}
static void edit_short(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)image, (void)made;
    *size = 63;
}
static void edit_class(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)made, (void)size;
    image[EI_CLASS] = 1;
}
static void edit_order(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)made, (void)size;
    image[EI_DATA] = (unsigned char)(image[EI_DATA] == ELFDATA2LSB ? ELFDATA2MSB : ELFDATA2LSB);
}
static void edit_headers(unsigned char* image, const ElfImageMade* made, size_t* size) {
    Elf64_Ehdr eh;
    (void)made, (void)size;
    memcpy(&eh, image, sizeof eh);
    eh.e_phnum = 4000;
    memcpy(image, &eh, sizeof eh);
}
/* the dynamic segment's header made another kind's */
static void edit_no_dynamic(unsigned char* image, const ElfImageMade* made, size_t* size) {
    Elf64_Phdr ph;
    (void)made, (void)size;
    memcpy(&ph, image + sizeof(Elf64_Ehdr) + sizeof ph, sizeof ph);
    ph.p_type = PT_TLS;
    memcpy(image + sizeof(Elf64_Ehdr) + sizeof ph, &ph, sizeof ph);
}
/* the dynamic entry of tag given value */
static void set_dynamic(unsigned char* image, const ElfImageMade* made, int64_t tag, uint64_t value) {
    Elf64_Dyn d;
    for (size_t at = made->dynamic;; at += sizeof d) {
        memcpy(&d, image + at, sizeof d);
        assert(d.d_tag != DT_NULL);
        if (d.d_tag != tag) continue;
        d.d_un.d_val = value;
        memcpy(image + at, &d, sizeof d);
        return;
    }
}
static void edit_strtab(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)size;
    set_dynamic(image, made, DT_STRTAB, 0x900000);
}
static void edit_needed(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)size;
    set_dynamic(image, made, DT_NEEDED, 9999);
}
/* the string table ends inside the soname, its first name */
static void edit_open_name(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)size;
    set_dynamic(image, made, DT_STRSZ, 3);
}
static void edit_verneed(unsigned char* image, const ElfImageMade* made, size_t* size) {
    (void)size;
    set_dynamic(image, made, DT_VERNEED, 0x900000);
}
/* the first version need's next is past the file */
static void edit_verneed_next(unsigned char* image, const ElfImageMade* made, size_t* size) {
    Elf64_Verneed need;
    (void)size;
    memcpy(&need, image + made->verneed, sizeof need);
    need.vn_next = 100000;
    memcpy(image + made->verneed, &need, sizeof need);
}
static void edit_verneed_name(unsigned char* image, const ElfImageMade* made, size_t* size) {
    Elf64_Verneed need;
    (void)size;
    memcpy(&need, image + made->verneed, sizeof need);
    need.vn_file = 7777;
    memcpy(image + made->verneed, &need, sizeof need);
}

static void file_names(void) {
    unsigned char image[ROOM], before[ROOM];
    char why[400] = "";
    ElfImage e = driver;
    ElfImageMade made = elf_image(&e, image, sizeof image);
    memcpy(before, image, sizeof image);
    /* its soname and the two libraries it needs: three names, a letter each. A version need has its library's string,
       written once */
    assert(snx_elf_rename(image, made.size, names, 3, why, sizeof why) == 3 && !why[0]);
    assert(differ(image, before, sizeof image) == 3);
    assert(elf_image_count(image, made.size, "vulkan.adrenO.so") == 1 &&
           !elf_image_count(image, made.size, "vulkan.adreno.so"));
    assert(elf_image_count(image, made.size, "libgsL.so") == 1 && !elf_image_count(image, made.size, "libgsl.so"));
    assert(elf_image_count(image, made.size, "libadreno_utilS.so") == 1);
    assert(elf_image_count(image, made.size, "libc.so") == 1 && elf_image_count(image, made.size, "libm.so") == 1);
    /* written already: nothing left to write */
    memcpy(before, image, sizeof image);
    assert(snx_elf_rename(image, made.size, names, 3, why, sizeof why) == 0 && !differ(image, before, sizeof image));
    /* a version need with a string of its own is written too: the linker compares it with the library's new name */
    e.own_version_names = 1;
    made = elf_image(&e, image, sizeof image);
    assert(elf_image_count(image, made.size, "libgsl.so") == 2);
    assert(snx_elf_rename(image, made.size, names, 3, why, sizeof why) == 4);
    assert(elf_image_count(image, made.size, "libgsL.so") == 2 && !elf_image_count(image, made.size, "libgsl.so"));
    assert(elf_image_count(image, made.size, "libc.so") == 2);
    /* a name that only looks like one asked for stays: a symbol's, and a longer library's */
    e = (ElfImage){.soname = "libgsl.so.1", .needed = {"xlibgsl.so"}, .symbols = {"libgsl.so"}};
    made = elf_image(&e, image, sizeof image);
    memcpy(before, image, sizeof image);
    assert(snx_elf_rename(image, made.size, names, 3, why, sizeof why) == 0 && !differ(image, before, sizeof image));
    /* no names asked for */
    assert(snx_elf_rename(image, made.size, NULL, 0, why, sizeof why) == 0);
}

static void files_refused(void) {
    unsigned char image[ROOM];
    char why[400], expect[400];
    ElfImageMade made = elf_image(&driver, image, sizeof image);
    const SnxElfRename longer[] = {{"libgsl.so", "libgsl2.so"}},
                       chained[] = {{"liba.so", "libA.so"}, {"libA.so", "liba.so"}}, empty[] = {{"", ""}};
    const ElfImage alone = {.soname = "vulkan.adreno.so"};
    assert(!strcmp(refused(&driver, edit_text, why), "is not an ELF file: 13 bytes, the first four 6e 6f 74 20"));
    assert(!strcmp(refused(&driver, edit_short, why), "is not an ELF file: 63 bytes, the first four 7f 45 4c 46"));
    assert(!strncmp(refused(&driver, edit_class, why),
                    "is not a 64-bit ELF file in this machine's byte order: its class is 1 and its data ", 82));
    assert(has(refused(&driver, edit_order, why),
               "is not a 64-bit ELF file in this machine's byte order: its class is 2"));
    snprintf(expect, sizeof expect, "has 4000 program headers of 56 bytes at 64, outside its %zu bytes", made.size);
    assert(!strcmp(refused(&driver, edit_headers, why), expect));
    snprintf(expect, sizeof expect, "has no dynamic section in its %zu bytes", made.size);
    assert(!strcmp(refused(&driver, edit_no_dynamic, why), expect));
    snprintf(expect, sizeof expect,
             "has no dynamic string table in its %zu bytes: DT_STRTAB is 0x900000 and DT_STRSZ %zu", made.size,
             made.strsz);
    assert(!strcmp(refused(&driver, edit_strtab, why), expect));
    snprintf(expect, sizeof expect, "has a library name at 9999 of its dynamic string table of %zu bytes", made.strsz);
    assert(!strcmp(refused(&driver, edit_needed, why), expect));
    assert(!strcmp(refused(&alone, edit_open_name, why),
                   "has a library name at 1 of its dynamic string table of 3 bytes"));
    snprintf(expect, sizeof expect, "has its 2 version needs at 0x900000, outside its %zu bytes", made.size);
    assert(!strcmp(refused(&driver, edit_verneed, why), expect));
    snprintf(expect, sizeof expect, "has version need 1 of 2 at %zu, outside its %zu bytes", made.verneed + 100000,
             made.size);
    assert(!strcmp(refused(&driver, edit_verneed_next, why), expect));
    snprintf(expect, sizeof expect,
             "has version need 0 name its library at 7777 of its dynamic string table of %zu bytes", made.strsz);
    assert(!strcmp(refused(&driver, edit_verneed_name, why), expect));
    /* names that cannot be written at their length, or that would be written twice */
    assert(snx_elf_rename(image, made.size, longer, 1, why, sizeof why) == -1);
    assert(!strcmp(why, "cannot have \"libgsl.so\" renamed \"libgsl2.so\": a name of 10 letters for one of 9"));
    assert(snx_elf_rename(image, made.size, chained, 2, why, sizeof why) == -1);
    assert(!strcmp(why, "cannot have \"liba.so\" renamed \"libA.so\", a name that is renamed \"liba.so\" itself"));
    assert(snx_elf_rename(image, made.size, empty, 1, why, sizeof why) == -1);
}

static void loaded_symbols(void) {
    /* the buffer is the image as loaded: its first byte's address is the image's */
    unsigned char image[ROOM];
    ElfImage e = {.soname = "ld-android.so",
                  .symbols = {"__loader_android_create_namespace", "__loader_android_link_namespaces",
                              "__loader_android_link_namespaces_all_libs", "__loader_android_get_exported_namespace",
                              "__loader_dlopen", "a", "rtld_db_dlactivity", "snx_ab"},
                  .undefined = "__loader_missing"};
    char name[40], longest[200];
    int same_bucket = 0;
    for (int pass = 0; pass < 4; ++pass) {
        /* the filter as made and all set; the addresses as the file has them (bionic) and with the load address added
           (glibc) */
        e.full_filter = pass & 1;
        e.address_bias = pass & 2 ? (uint64_t)(uintptr_t)image - ELF_IMAGE_VADDR : 0;
        ElfImageMade made = elf_image(&e, image, sizeof image);
        for (int i = 0; e.symbols[i]; ++i)
            assert(snx_elf_loaded_symbol(image, e.symbols[i]) == (void*)(image + made.symbol[i]));
        /* one it asks for is not one it has; nor is a name no symbol has, whichever chain it lands in */
        assert(!snx_elf_loaded_symbol(image, "__loader_missing"));
        assert(!snx_elf_loaded_symbol(image, "__loader_android_create_namespac"));
        assert(!snx_elf_loaded_symbol(image, ""));
        /* nor is a name of the very hash of one it has: the names are compared */
        assert(snx_elf_gnu_hash("snx_bA") == snx_elf_gnu_hash("snx_ab") && !snx_elf_loaded_symbol(image, "snx_bA"));
        for (int i = 0; i < 200; ++i) {
            snprintf(name, sizeof name, "absent_%d", i);
            assert(!snx_elf_loaded_symbol(image, name));
        }
    }
    /* the eight share three buckets: some chain has more than one, so its end is read */
    for (int i = 0; e.symbols[i]; ++i)
        for (int k = i + 1; e.symbols[k]; ++k)
            same_bucket += snx_elf_gnu_hash(e.symbols[i]) % ELF_IMAGE_BUCKETS ==
                           snx_elf_gnu_hash(e.symbols[k]) % ELF_IMAGE_BUCKETS;
    assert(same_bucket);
    memset(longest, 'x', sizeof longest - 1);
    longest[sizeof longest - 1] = 0;
    assert(!snx_elf_loaded_symbol(image, longest));
    assert(!snx_elf_loaded_symbol(NULL, "a"));
    image[0] = 0; /* no ELF header there */
    assert(!snx_elf_loaded_symbol(image, "a"));
}

int main(void) {
    file_names();
    files_refused();
    loaded_symbols();
    printf("elf_dynamic: %s\n", elf_dynamic_host());
    printf("elf_dynamic: ok\n");
    return 0;
}
