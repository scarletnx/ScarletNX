/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_LICENSE_H
#define SNX_VITA_LICENSE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A content's license, the console's .rif: a game's sce_sys/package/work.bin, an add-on's
   license/addcont/<title ID>/<its ID>/<name>.rif, or the same 512 bytes as a zRIF (NoPayStation's text form: base64 of the
   .rif deflated against a dictionary of the IDs' common parts). Its klicensee decrypts the content's files. A license
   NoNpDrm wrote holds it as it is, its account ID 0x0123456789abcdef; a console's own license keeps it encrypted to that
   console, which the import cannot undo, and its files then do not verify */
#define VITA_LICENSE_SIZE    512u
#define VITA_LICENSE_NONPDRM 0x0123456789abcdefull
typedef struct {
    char content_id[0x31]; /* UP0000-PCSE00000_00-0000000000000000 */
    unsigned char klicensee[16];
    uint64_t account;
} VitaLicense;

/* the license in bytes, from what (named in the messages): false with error for another size or a content ID that is
   not one */
bool vita_license_parse(const unsigned char* rif, size_t size, const char* what, VitaLicense* license, char* error,
                        size_t error_size);
/* the .rif at path */
bool vita_license_read(const char* path, VitaLicense* license, char* error, size_t error_size);
/* a zRIF: false with error for text that is not base64, a stream that does not inflate, uses another dictionary or
   fails its Adler-32, or a license of another size (a PSP game's) */
bool vita_license_zrif(const char* text, VitaLicense* license, char* error, size_t error_size);
#endif
