/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_LOADER_H
#define SNX_CTR_LOADER_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
/* The most content the runtime takes, in bytes: a CIA input, the extracted output and a RomFS view. A policy, not a
   format limit, since only the level-3 tables' entry offsets are 32-bit. A z3ds decompression and a decrypted CIA copy
   are not counted. */
#define SNX_CTR_MAX_CONTENT 0x100000000ull
typedef enum {
    SNX_CTR_EXTRACT_OK,
    SNX_CTR_EXTRACT_IO,
    SNX_CTR_EXTRACT_FORMAT,
    SNX_CTR_EXTRACT_UNSUPPORTED,
    SNX_CTR_EXTRACT_LIMIT,
    SNX_CTR_EXTRACT_EXISTS
} SnxCtrExtractResult;
/* An application CIA, a cartridge image (a CCI or a .3ds) or a bare NCCH (.cxi, an .app, homebrew's .hbb): what the
   file is comes from its own header, not its name. RSA/ECDSA trust and encrypted layers are not
   implemented. Hash checks are integrity checks, NOT signature authentication.
   Output is a NEW directory, published only after complete extraction. NULL
   destination selects <work>/<title-id>-<full-input-sha256>, work/3ds when work
   is NULL. Existing outputs are refused, never treated as a cache hit. Parent
   directory and input must be trusted and not concurrently modified. published
   is owned by caller on OK. */
SnxCtrExtractResult snx_ctr_extract(const char* input, const char* destination, const char* work, char** published,
                                    char* error, size_t error_size);
/* An application game read where it lies, nothing extracted: the file itself mapped read-only and
   shared for as long as it is open, after every check snx_ctr_extract makes (hashes, bounds, the RomFS metadata).
   Encrypted content is refused as the extraction refuses it. *source is the caller's on OK. The input must not be
   modified while it is open: the mapping shows what the file holds */
typedef struct SnxCtrSource SnxCtrSource;
SnxCtrExtractResult snx_ctr_source_open(const char* input, SnxCtrSource** source, char* error, size_t error_size);
void snx_ctr_source_close(SnxCtrSource* source);
uint64_t snx_ctr_source_title(const SnxCtrSource* source);
/* the game file's modification time, UTC Unix milliseconds, 0 when the host can't say */
uint64_t snx_ctr_source_mtime(const SnxCtrSource* source);
/* the game's ExHeader (0x800 bytes, as exheader.bin holds it), its .code expanded to its segments' pages (as
   exefs/.code holds it), its ExeFS files and its RomFS: the update NCCH's when a cartridge carries an update, as
   the console runs the update title's, while snx_ctr_source_title stays the base NCCH's, as the card is one game */
const unsigned char* snx_ctr_source_exheader(const SnxCtrSource* source);
const unsigned char* snx_ctr_source_code(const SnxCtrSource* source, size_t* size);
/* an ExeFS file as the game stores it, in the mapping; NULL with *size 0 for none by that name */
const unsigned char* snx_ctr_source_exefs(const SnxCtrSource* source, const char* name, uint64_t* size);
/* the RomFS's level 3, the image the guest reads, in the mapping; NULL with *size 0 without a RomFS */
const unsigned char* snx_ctr_source_romfs(const SnxCtrSource* source, uint64_t* size);
/* An encrypted game made into the decrypted one the loader reads: a CIA's contents under the ticket's title key and
   their NCCHs under the primary and secondary keys, or a cartridge image's NCCHs. keys is an aes_keys.txt in
   Azahar's format (the generator constant, the KeyX of slots 0x2C, 0x3D and the secondary key's, the common KeyYs).
   A game with nothing encrypted is left alone, *encrypted false and nothing written. Seed crypto (seeddb) is
   refused */
SnxCtrExtractResult snx_ctr_decrypt(const char* input, const char* keys, const char* output, bool* encrypted,
                                    char* error, size_t error_size);
/* A z3ds game (what Azahar's launcher keeps a game in, a .zcci, .zcxi or .zcia) unpacked into the plain container
   the loader reads, published only complete. The input must be a z3ds file; the output is its game's bytes, to be
   identified as any game is */
SnxCtrExtractResult snx_ctr_unzstd(const char* input, const char* output, char* error, size_t error_size);
/* ExeFS backward LZSS/BLZ, exact bounded expansion; output_size is authoritative.
   The input and output buffers must not overlap. */
bool snx_ctr_blz(const unsigned char* input, size_t input_size, unsigned char* output, size_t output_size);
#endif
