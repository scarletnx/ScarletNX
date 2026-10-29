/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Initial FS session wire shapes referenced against Azahar service/fs/fs_user.cpp
   and libctru services/fs.c. The extracted title directory is the NX-style
   shared tree: romfs/exefs read-only and always open, sdmc read-write. */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "3ds/services/archives.h"
#include "3ds/services/fs.h"
#include "services/fs.swipc.inc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 3DS FS result codes: 0xc8804400 | the Azahar ErrCodes value. */
static uint32_t tree_result(FsTreeResult r) {
    switch (r) {
        case FS_TREE_OK: return 0;
        case FS_TREE_NOTFOUND: return 0xc8804478u;
        case FS_TREE_EXISTS: return 0xc88044beu;
        case FS_TREE_NOTEMPTY: return 0xc88044f0u;
        case FS_TREE_INVALID: return 0xc8804702u;
        case FS_TREE_TOOLONG: return 0xc88046beu;
        case FS_TREE_READONLY: return 0xd8e01beeu;
        default: return 0xc88044d2u;
    }
}
static bool initialized(SnxCtrIpc* c) {
    return c->process->objects[c->object].data.session.client_pid == CTR_PROCESS_ID;
}
static uint32_t snx_ctr_fs_wire_InitializeWithSdkVersion(SnxCtrIpc* c, uint32_t version, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    CtrObject* object = &c->process->objects[c->object];
    object->data.session.client_pid = process;
    object->data.session.sdk_version = version;
    return 0;
}
static uint32_t snx_ctr_fs_wire_Initialize(SnxCtrIpc* c, uint32_t process) {
    return snx_ctr_fs_wire_InitializeWithSdkVersion(c, 0, process);
}
#define CTR_NOT_FORMATTED 0xc8a04554u /* NotFormatted, FS, InvalidState, Status */
#define CTR_NO_ARCHIVE    0xc8804465u /* ArchiveNotMounted, FS, NotFound, Status: an archive handle not open */
/* NotFound, FS, InvalidState, Status: an ExtSaveData that was never created (Azahar ResultNotFoundInvalidState) */
#define CTR_NO_EXTDATA      0xc8a04478u
#define CTR_UNSUPPORTED_OPEN 0xe0c046f8u /* UnsupportedOpenFlags, FS, NotSupported, Usage */
static CtrSelfArchive* archive(SnxCtrProcess* p, uint64_t token) {
    if (token)
        for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i)
            if (p->self_archives[i].token == token) return &p->self_archives[i];
    return NULL;
}
/* the host area an archive's paths are in: the SD card's or the save data's; NULL for the read-only RomFS */
static FsNode* area(const SnxCtrProcess* p, const CtrSelfArchive* a) {
    return !a ? NULL : a->kind == 1 ? p->sd : a->kind == 2 ? p->save : a->kind == 3 ? a->root : NULL;
}
static uint32_t snx_ctr_fs_wire_CloseArchive(SnxCtrIpc* c, uint64_t token) {
    if (!initialized(c)) return SNX_CTR_IPC_INVALID_STATE;
    CtrSelfArchive* a = archive(c->process, token);
    if (!a) return CTR_NO_ARCHIVE;
    if (a->root) fs_tree_release(a->root);
    *a = (CtrSelfArchive){0};
    return 0;
}
bool snx_ctr_fs_file_object(SnxCtrIpc* c, uint32_t kind, uint32_t writable, uint32_t fixed, void* host, void* node) {
    SnxCtrProcess* p = c->process;
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) count += p->objects[i].type == CTR_OBJ_FILE;
    if (count >= 64) {
        snx_ctr_ipc_error_reply(c, 0xd0401834u);
        return false;
    }
    uint32_t handle = 0, result = snx_ctr_object_create(p, CTR_OBJ_FILE, &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return false;
    }
    CtrObject* object = &p->objects[snx_ctr_handle_object(p, handle)];
    object->data.file.kind = kind;
    object->data.file.writable = writable;
    object->data.file.fixed = fixed;
    object->data.file.host = host;
    object->data.file.node = node;
    if (node) fs_tree_retain((FsNode*)node);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x42;
    c->out[2] = 0x10;
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) {
        if (node) fs_tree_release((FsNode*)node);
        snx_ctr_ipc_drop_handle(c, handle);
        return false;
    }
    return true;
}
/* Bounded path decode shared by every path-carrying command. PATH_BINARY and
   PATH_UTF16 carry UTF-16LE; PATH_ASCII carries ASCII bytes. */
/* The path's descriptor, the word before its pointer: static buffer 0 of the path's size (libctru's FSUSER_*) */
static bool path_descriptor(SnxCtrIpc* c, uint32_t size_word, uint32_t ptr_word) {
    if (c->in[ptr_word - 1] == ((c->in[size_word] << 14) | 2)) return true;
    snx_ctr_ipc_bad(c);
    return false;
}
static bool read_path(SnxCtrIpc* c, uint32_t type_word, uint32_t size_word, uint32_t ptr_word, char out[768],
                      const char** relative) {
    uint32_t type = c->in[type_word], size = c->in[size_word];
    if (!size || size > 256) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return false;
    }
    unsigned char raw[256];
    if (snx_ctr_read_ram(c->process->space, c->in[ptr_word], raw, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return false;
    }
    if (type == 3) { /* PATH_ASCII */
        unsigned n = 0;
        bool terminated = false;
        for (; n < size; ++n) {
            if (!raw[n]) {
                terminated = true;
                break;
            }
            if (raw[n] > 0x7f) {
                c->state = SNX_CTR_IPC_UNSUPPORTED;
                return false;
            }
            out[n] = (char)raw[n];
        }
        if (!terminated) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return false;
        }
        out[n] = 0;
    } else if (type == 2 || type == 4) {
        if (size < 2 || (size & 1) || !snx_ctr_utf16_path(raw, size, out, 768)) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return false;
        }
    } else {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return false;
    }
    *relative = out;
    while (**relative == '/') ++*relative;
    return true;
}
/* The read-write sdmc area: resolve an indexed host file and open it. */
static void open_host(SnxCtrIpc* c, FsNode* root, uint32_t flags, uint32_t fixed, const char* relative) {
    SnxCtrProcess* p = c->process;
    FsNode* node = NULL;
    FILE* host = NULL;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_lookup(p->tree, root, relative, &node);
    if (rc == FS_TREE_NOTFOUND && (flags & 4)) rc = fs_tree_create(p->tree, root, relative, 0, 0, &node);
    if (!rc && (node->is_dir || node->is_virtual)) rc = FS_TREE_INVALID;
    if (!rc) {
        host = fs_tree_open(p->tree, node, (flags & 2) ? "r+b" : "rb");
        if (!host) rc = FS_TREE_IO;
    }
    pm_unlock(&p->tree->lock);
    if (rc) {
        if (host) fclose(host);
        snx_ctr_ipc_error_reply(c, tree_result(rc));
        return;
    }
    if (!snx_ctr_fs_file_object(c, CTR_FILE_HOST, !!(flags & 2), fixed, host, node)) fclose(host);
}
/* SelfNCCH's 12-byte file path: its type, then an ExeFS file's name (Azahar's SelfNCCHFilePath). 0 is the RomFS;
   2 an ExeFS file, of which Azahar serves the icon, logo and banner (exefs/ in the tree: the extraction's files, or
   the CIA's sections, process.c); the code (1) is not for this archive; 5, an update's RomFS, finds none, as no update
   is loaded */
static void open_self(SnxCtrIpc* c, const unsigned char path[12]) {
    SnxCtrProcess* p = c->process;
    uint32_t type = snx_ld_le32(path);
    if (type == 0 || type == 5) {
        if (type == 5 || !p->romfs) {
            snx_ctr_ipc_error_reply(c, 0xc8804464u); /* RomFSNotFound */
            return;
        }
        snx_ctr_fs_file_object(c, CTR_FILE_VIEW, 0, 0, NULL, NULL);
        return;
    }
    if (type != 2) {
        snx_ctr_ipc_error_reply(c, type == 1 ? 0xd9004676u : 0xe0e046beu); /* CommandNotAllowed, InvalidPath */
        return;
    }
    char name[9] = {0}, relative[16];
    memcpy(name, path + 4, 8);
    if (strcmp(name, "icon") && strcmp(name, "logo") && strcmp(name, "banner")) {
        snx_ctr_ipc_error_reply(c, 0xe0e046beu);
        return;
    }
    snprintf(relative, sizeof relative, "exefs/%s", name);
    FsNode* node = NULL;
    FILE* host = NULL;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_lookup(p->tree, NULL, relative, &node);
    if (!rc && node->is_dir) rc = FS_TREE_NOTFOUND;
    if (!rc && !node->is_virtual && !(host = fs_tree_open(p->tree, node, "rb"))) rc = FS_TREE_IO;
    pm_unlock(&p->tree->lock);
    if (rc) {
        snx_ctr_ipc_error_reply(c, rc == FS_TREE_NOTFOUND ? 0xc8804637u : tree_result(rc)); /* ExeFSSectionNotFound */
        return;
    }
    if (!snx_ctr_fs_file_object(c, CTR_FILE_HOST, 0, 0, host, node) && host) fclose(host);
}
/* The system titles whose RomFS a stand-in replaces, as Azahar's NCCHArchive::OpenFile picks them: the shared font
   for the Chinese, Korean and Taiwanese ones too (their text will not show right) */
static const unsigned char* system_archive(uint64_t title, size_t* size) {
    switch (title) {
        case 0x0004009b00010202ull: *size = snx_ctr_mii_data_size; return snx_ctr_mii_data;
        case 0x0004009b00010402ull: *size = snx_ctr_country_list_size; return snx_ctr_country_list;
        case 0x0004009b00014002ull:
        case 0x0004009b00014102ull:
        case 0x0004009b00014202ull:
        case 0x0004009b00014302ull: *size = snx_ctr_shared_font_size; return snx_ctr_shared_font;
        case 0x000400db00010302ull: *size = snx_ctr_bad_word_list_size; return snx_ctr_bad_word_list;
        default: return NULL;
    }
}
/* The NCCH archive (0x2345678A): a title's content by its ID, its archive path the title ID, a media type and a
   word, its file path the open type, the content index, the section (0 the RomFS, 1 the code, 2 an ExeFS file) and
   an ExeFS name (Azahar's NCCHArchivePath and NCCHFilePath). This title's own RomFS is its RomFS view. No system title
   is installed: the RomFS of one Azahar has an open stand-in for (the Mii data, the country list, the shared font,
   the bad word list) is that stand-in, read-only in the tree from the first open (system/), and another title is
   NotFound, as Azahar answers for one it has nothing of, a Status the SDK passes back to its caller */
static void open_ncch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (c->in[3] != 2 || c->in[4] != 16 || c->in[5] != 2 || c->in[6] != 0x14 || c->in[7] != 1 || c->in[8]) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[9] != ((16u << 14) | 0x802) || c->in[11] != ((0x14u << 14) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char archive_path[16], file_path[0x14];
    if (snx_ctr_read_ram(p->space, c->in[10], archive_path, sizeof archive_path) != SNX_CTR_MEM_OK ||
        snx_ctr_read_ram(p->space, c->in[12], file_path, sizeof file_path) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint64_t title = snx_ld_le64(archive_path);
    uint32_t words[3];
    for (unsigned i = 0; i < 3; ++i)
        words[i] = snx_ld_le32(file_path + i * 4);
    if (title == p->title_id) {
        if (words[0] || words[1] || words[2]) { /* its code or an ExeFS file, or another content */
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        if (!p->romfs) {
            snx_ctr_ipc_error_reply(c, 0xc8804464u);
            return;
        }
        snx_ctr_fs_file_object(c, CTR_FILE_VIEW, 0, 0, NULL, NULL);
        return;
    }
    size_t size = 0;
    const unsigned char* stand_in = system_archive(title, &size);
    if (stand_in && !words[0] && !words[1] && !words[2]) {
        char path[40];
        snprintf(path, sizeof path, "system/%016llx.romfs", (unsigned long long)title);
        FsNode* node = NULL;
        pm_lock(&p->tree->lock);
        FsTreeResult rc = fs_tree_lookup(p->tree, NULL, path, &node);
        if (rc == FS_TREE_NOTFOUND) rc = fs_tree_add_virtual(p->tree, path, stand_in, size, &node);
        if (!rc && !node->is_virtual) rc = FS_TREE_INVALID; /* a host file of that name is not the stand-in */
        pm_unlock(&p->tree->lock);
        if (rc) {
            snx_ctr_ipc_error_reply(c, tree_result(rc));
            return;
        }
        snx_ctr_fs_file_object(c, CTR_FILE_HOST, 0, 0, NULL, node);
        return;
    }
    fprintf(stderr,
            "3ds fs: the game reads title %016llx (a system archive), which is not installed: it is told the title is "
            "not found\n",
            (unsigned long long)title);
    snx_ctr_ipc_error_reply(c, 0xc8804478u); /* NotFound, FS, NotFound, Status */
}
/* FSUSER OpenFileDirectly: SelfNCCH with the 12-byte binary selector for the
   read-only RomFS view, or a direct read(-write) open on the sdmc area. */
static void open_direct(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 8, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1]) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[2] == 3) {
        if (c->in[3] != 1 || c->in[4] != 1 || c->in[5] != 2 || c->in[6] != 12 || c->in[7] != 1 || c->in[8]) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        if (c->in[9] != 0x4802 || c->in[11] != 0x30002) {
            snx_ctr_ipc_bad(c);
            return;
        }
        /* the archive's path is empty: its byte means nothing */
        unsigned char path[12];
        if (snx_ctr_read_ram(p->space, c->in[12], path, sizeof path) != SNX_CTR_MEM_OK) {
            c->transport_result = 0xe0e01bf5u;
            return;
        }
        open_self(c, path);
        return;
    }
    if (c->in[2] == 0x2345678au) {
        open_ncch(c);
        return;
    }
    /* SDMC direct open: empty archive path plus an indexed area path. */
    if (c->in[2] != 9 || c->in[3] != 1 || c->in[4] != 1 || c->in[9] != 0x4802 || c->in[11] != ((c->in[6] << 14) | 2)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint32_t flags = c->in[7];
    if (c->in[8] || (flags & ~7u) || !(flags & 3)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* the archive's path is empty: its byte means nothing (Azahar reads no byte of an empty path) */
    if (!p->sd) {
        snx_ctr_ipc_error_reply(c, 0xc8804478u);
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!read_path(c, 5, 6, 12, path, &relative)) return;
    open_host(c, p->sd, flags, 0, relative);
}
static void open_file(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 7, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1] || c->in[7] || c->in[8] != ((c->in[5] << 14) | 2)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint32_t size = c->in[5], flags = c->in[6];
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, CTR_NO_ARCHIVE);
        return;
    }
    unsigned char raw[256];
    if (size > sizeof raw) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (snx_ctr_read_ram(p->space, c->in[9], raw, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (a->kind == 0) {
        if (size != 12 || flags != 1) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        open_self(c, raw);
        return;
    }
    /* An ExtSaveData's files are made by CreateFile at their size, and opened read-write whatever the flags ask
       (Azahar's ExtSaveDataArchive::OpenFile): no flags, or the create flag, is UnsupportedOpenFlags */
    if (a->kind == 3) {
        if (!(flags & 7) || (flags & 4)) {
            snx_ctr_ipc_error_reply(c, CTR_UNSUPPORTED_OPEN);
            return;
        }
        flags |= 3;
    }
    /* SDMC: read(-write) opens of indexed files. FS_OPEN_READ/WRITE/CREATE. */
    if ((flags & ~7u) || !(flags & 3)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!read_path(c, 4, 5, 9, path, &relative)) return;
    open_host(c, area(p, a), flags, a->kind == 3, relative);
}
static void open_directory(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 4, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, CTR_NO_ARCHIVE);
        return;
    }
    if (c->in[3] != 2 && c->in[3] != 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!path_descriptor(c, 4, 6) || !read_path(c, 3, 4, 6, path, &relative)) return;
    if (!area(p, a)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    FsNode* node = NULL;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_lookup(p->tree, area(p, a), relative, &node);
    if (!rc && !node->is_dir) rc = FS_TREE_INVALID;
    pm_unlock(&p->tree->lock);
    if (rc) {
        snx_ctr_ipc_error_reply(c, tree_result(rc));
        return;
    }
    snx_ctr_fs_file_object(c, CTR_FILE_DIR, 0, 0, NULL, node);
}
static void create_entry(SnxCtrIpc* c, int is_dir) {
    if (!snx_ctr_ipc_expect(c, is_dir ? 6 : 8, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    if (!area(p, a)) {
        snx_ctr_ipc_error_reply(c, a ? 0xd8e01beeu : CTR_NO_ARCHIVE);
        return;
    }
    if (c->in[4] != 2 && c->in[4] != 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint64_t file_size = is_dir ? 0 : (uint64_t)c->in[7] | (uint64_t)c->in[8] << 32;
    char path[768];
    const char* relative = NULL;
    if (!path_descriptor(c, 5, is_dir ? 8 : 10) || !read_path(c, 4, 5, is_dir ? 8 : 10, path, &relative)) return;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_create(p->tree, area(p, a), relative, is_dir, file_size, NULL);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
static void remove_entry(SnxCtrIpc* c, int is_dir, int recursive) {
    if (!snx_ctr_ipc_expect(c, 5, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    if (!area(p, a)) {
        snx_ctr_ipc_error_reply(c, a ? 0xd8e01beeu : CTR_NO_ARCHIVE);
        return;
    }
    if (c->in[4] != 2 && c->in[4] != 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char path[768];
    const char* relative = NULL;
    if (!path_descriptor(c, 5, 7) || !read_path(c, 4, 5, 7, path, &relative)) return;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_remove(p->tree, area(p, a), relative, is_dir, recursive, 0);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
static void rename_entry(SnxCtrIpc* c, int is_dir) {
    if (!snx_ctr_ipc_expect(c, 9, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* src = archive(p, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32);
    CtrSelfArchive* dst = archive(p, (uint64_t)c->in[6] | (uint64_t)c->in[7] << 32);
    if (!area(p, src) || dst != src) {
        snx_ctr_ipc_error_reply(c, CTR_NO_ARCHIVE);
        return;
    }
    if ((c->in[4] != 2 && c->in[4] != 4) || (c->in[8] != 2 && c->in[8] != 4)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[10] != ((c->in[5] << 14) | 2) || c->in[12] != ((c->in[9] << 14) | (1u << 11) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    char from[768], to[768];
    const char *a_path = NULL, *b_path = NULL;
    if (!read_path(c, 4, 5, 11, from, &a_path)) return;
    if (!read_path(c, 8, 9, 13, to, &b_path)) return;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_rename(p->tree, area(p, src), a_path, b_path, is_dir);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
/* FSUSER ControlArchive: GET_TIMESTAMP returns the backing host file's real
   modification time; COMMIT_SAVE_DATA on the SD area is genuine: every write
   already landed on the host. Other actions stop. */
static void control_archive(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 5, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    CtrSelfArchive* a = archive(p, (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, CTR_NO_ARCHIVE);
        return;
    }
    if (!c->in[3]) { /* COMMIT_SAVE_DATA */
        if (area(p, a)) {
            c->out[0] = 0x080d0040;
            return;
        }
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[3] != 1 || c->in[5] != 8 || c->in[4] < 2 || c->in[4] > 256 || (c->in[4] & 1) ||
        c->in[6] != ((c->in[4] << 4) | 10) || c->in[8] != ((8u << 4) | 12)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (a->kind == 0 && !p->romfs) {
        snx_ctr_ipc_error_reply(c, 0xc8804464u);
        return;
    }
    unsigned char path[256], out[8];
    if (snx_ctr_read_ram(p->space, c->in[7], path, c->in[4]) != SNX_CTR_MEM_OK ||
        snx_ctr_check_ram(p->space, c->in[9], sizeof out, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint64_t datetime = 0;
    if (a->kind == 0) {
        SnxCtrTimeResult result = snx_ctr_romfs_time(p->romfs, path, c->in[4], &datetime);
        if (result == SNX_CTR_TIME_NOTFOUND) {
            snx_ctr_ipc_error_reply(c, 0xc8804478u);
            return;
        }
        if (result != SNX_CTR_TIME_OK) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
    } else {
        char decoded[768];
        const char* relative = NULL;
        if (!snx_ctr_utf16_path(path, c->in[4], decoded, sizeof decoded)) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        relative = decoded;
        while (*relative == '/') ++relative;
        FsNode* node = NULL;
        pm_lock(&p->tree->lock);
        FsTreeResult rc = area(p, a) ? fs_tree_lookup(p->tree, area(p, a), relative, &node) : FS_TREE_INVALID;
        uint64_t unix_ms = 0;
        if (!rc) rc = fs_tree_mtime(p->tree, node, &unix_ms);
        pm_unlock(&p->tree->lock);
        if (rc) {
            snx_ctr_ipc_error_reply(c, tree_result(rc));
            return;
        }
        datetime = unix_ms + 2208988800000ull;
    }
    snx_st_le64(out, datetime);
    if (snx_ctr_write_ram(p->space, c->in[9], out, sizeof out) != SNX_CTR_MEM_OK) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    c->out[0] = 0x080d0040;
}
/* An ExtSaveData's folder in the title's tree, as the console lays them out (Azahar's GetExtDataContainerPath and
   GetExtSaveDataPath): a game's own (archive 6) on the SD card, a shared one (archive 7, the system's) on the NAND,
   whose high word the FS module makes 0x48000 whatever the path says. The folder holds user/ (the archive), boss/,
   metadata (its ArchiveFormatInfo) and icon */
#define CTR_EXTDATA        "sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/extdata"
#define CTR_SHARED_EXTDATA "nand/data/00000000000000000000000000000000/extdata"
static void extdata_folder(char out[160], bool shared, uint32_t high, uint32_t low) {
    if (shared)
        snprintf(out, 160, CTR_SHARED_EXTDATA "/%08x/%08x", 0x48000u, low);
    else
        snprintf(out, 160, CTR_EXTDATA "/%08x/%08x", high, low);
}
/* An ExtSaveData archive's binary path: media type, save ID low, save ID high (Azahar ExtSaveDataArchivePath) */
static bool extdata_path(SnxCtrIpc* c, uint32_t address, uint32_t words[3]) {
    unsigned char raw[12];
    if (snx_ctr_read_ram(c->process->space, address, raw, sizeof raw) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return false;
    }
    for (unsigned i = 0; i < 3; ++i)
        words[i] = snx_ld_le32(raw + i * 4);
    return true;
}
static void open_archive(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    uint32_t id = c->in[1], type = c->in[2], size = c->in[3];
    bool extdata = (id == 6 || id == 7) && type == 2 && size == 12;
    if (!extdata && ((id != 9 && id != 3 && id != 4) || type != 1 || size != 1)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != ((size << 14) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    /* the archive's path is empty: its byte means nothing (Azahar reads no byte of an empty path) */
    if (id == 3 && !p->romfs) {
        snx_ctr_ipc_error_reply(c, 0xc8804464u);
        return;
    }
    if ((id == 9 || id == 6) && !p->sd) {
        snx_ctr_ipc_error_reply(c, 0xc8804478u);
        return;
    }
    if (id == 4 && !p->save) { /* the game formats its save data when told it has none */
        snx_ctr_ipc_error_reply(c, CTR_NOT_FORMATTED);
        return;
    }
    /* An ExtSaveData is where its archive keeps it whatever media its path names, as Azahar's archives are. A game
       creates its own when told it has none (NotFound); a shared one that is missing is NotFormatted, as Azahar
       answers, the system having made the ones the console has (services/ptm.c) */
    FsNode* root = NULL;
    if (extdata) {
        uint32_t path[3];
        if (!extdata_path(c, c->in[5], path)) return;
        char folder[160 + 8];
        extdata_folder(folder, id == 7, path[2], path[1]);
        strcat(folder, "/user");
        pm_lock(&p->tree->lock);
        FsTreeResult rc = fs_tree_lookup(p->tree, NULL, folder, &root);
        if (!rc && !root->is_dir) rc = FS_TREE_NOTFOUND;
        if (!rc) fs_tree_retain(root);
        pm_unlock(&p->tree->lock);
        if (rc) {
            snx_ctr_ipc_error_reply(c, rc != FS_TREE_NOTFOUND ? tree_result(rc) : id == 7 ? CTR_NOT_FORMATTED
                                                                                            : CTR_NO_EXTDATA);
            return;
        }
    }
    if (p->archive_sequence != UINT64_MAX)
        for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
            if (p->self_archives[i].token) continue;
            uint64_t token = ++p->archive_sequence;
            p->self_archives[i].token = token;
            p->self_archives[i].kind = id == 9 ? 1u : id == 4 ? 2u : extdata ? 3u : 0u;
            p->self_archives[i].root = root;
            c->out[0] = 0x080c00c0;
            c->out[2] = (uint32_t)token;
            c->out[3] = (uint32_t)(token >> 32);
            return;
        }
    if (root) {
        pm_lock(&p->tree->lock);
        fs_tree_release(root);
        pm_unlock(&p->tree->lock);
    }
    snx_ctr_ipc_error_reply(c, 0xd0401834u);
}
/* A host file of the tree written whole: made at its size or resized to it, then its bytes. Caller holds the lock */
static FsTreeResult write_whole(FsTree* tree, FsNode* scope, const char* path, const void* data, size_t size) {
    FsNode* node = NULL;
    FsTreeResult rc = fs_tree_lookup(tree, scope, path, &node);
    if (rc == FS_TREE_NOTFOUND)
        rc = fs_tree_create(tree, scope, path, 0, size, &node);
    else if (!rc)
        rc = node->is_dir ? FS_TREE_INVALID : fs_tree_resize(tree, node, size);
    if (rc) return rc;
    FILE* f = fs_tree_open(tree, node, "r+b");
    if (!f) return FS_TREE_IO;
    if (size && fwrite(data, 1, size, f) != size) rc = FS_TREE_IO;
    if (fclose(f)) rc = FS_TREE_IO;
    return rc;
}
/* Create an ExtSaveData, a game's own on the SD card (media 1) or a shared one on the NAND (media 0), as Azahar's
   ArchiveManager::CreateExtSaveData picks: user/ and boss/, the format it was given in metadata and its SMDH icon, as
   ArchiveFactory_ExtSaveData::FormatAsExtData writes them. One that exists is made again over itself, its files
   kept */
FsTreeResult snx_ctr_fs_make_extdata(SnxCtrProcess* p, bool shared, uint32_t high, uint32_t low,
                                     const unsigned char metadata[16], const void* icon, size_t icon_size) {
    char folder[160], path[160 + 16];
    extdata_folder(folder, shared, high, low);
    FsTreeResult rc = FS_TREE_OK;
    for (unsigned i = 0; i < 2 && !rc; ++i) {
        snprintf(path, sizeof path, "%s/%s", folder, i ? "boss" : "user");
        rc = fs_tree_mkdirs(p->tree, path, NULL);
    }
    if (!rc) {
        snprintf(path, sizeof path, "%s/metadata", folder);
        rc = write_whole(p->tree, NULL, path, metadata, 16);
    }
    if (!rc && icon_size) {
        snprintf(path, sizeof path, "%s/icon", folder);
        rc = write_whole(p->tree, NULL, path, icon, icon_size);
    }
    return rc;
}
static void create_extdata(SnxCtrIpc* c, uint32_t media, uint32_t low, uint32_t high, uint32_t directories,
                           uint32_t files, uint32_t total_size, uint32_t icon_size, uint32_t buffer_word) {
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (media > 1 || icon_size > 0x10000 || c->in[buffer_word] != ((icon_size << 4) | 10)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    unsigned char* icon = malloc(icon_size ? icon_size : 1);
    if (!icon) {
        snx_ctr_ipc_error_reply(c, 0xd86007f3u);
        return;
    }
    if (icon_size && snx_ctr_read_ram(p->space, c->in[buffer_word + 1], icon, icon_size) != SNX_CTR_MEM_OK) {
        free(icon);
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    unsigned char metadata[16] = {0}; /* ArchiveFormatInfo: total size, directories, files, duplicate data (none) */
    snx_st_le32(metadata, total_size);
    snx_st_le32(metadata + 4, directories);
    snx_st_le32(metadata + 8, files);
    pm_lock(&p->tree->lock);
    FsTreeResult rc = snx_ctr_fs_make_extdata(p, media == 0, high, low, metadata, icon, icon_size);
    pm_unlock(&p->tree->lock);
    free(icon);
    if (rc) {
        snx_ctr_ipc_error_reply(c, tree_result(rc));
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x42;
    c->out[2] = c->in[buffer_word];
    c->out[3] = c->in[buffer_word + 1];
}
/* Delete an ExtSaveData (a game's own, media 1, or a shared one, media 0) and everything in it; an archive still open
   on it keeps its folder until it is closed */
static void delete_extdata(SnxCtrIpc* c, uint32_t media, uint32_t low, uint32_t high) {
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (media > 1) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char folder[160];
    extdata_folder(folder, media == 0, high, low);
    pm_lock(&p->tree->lock);
    FsTreeResult rc = fs_tree_remove(p->tree, NULL, folder, 1, 1, 0);
    pm_unlock(&p->tree->lock);
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
/* The save data's format (FormatSaveData's numbers), kept beside it in the title's state folder as five little-endian
   words: total size, directories, files, duplicate data, and a version 1 */
#define CTR_FORMAT_FILE "savedata.format"
/* This title's save data (archive 4, an empty path) made anew and empty, as Azahar's
   ArchiveSource_SDSaveData::Format does: FormatSaveData and FormatThisUserSaveData */
static void format_save_data(SnxCtrIpc* c, uint32_t blocks, uint32_t directories, uint32_t files, uint32_t duplicate) {
    SnxCtrProcess* p = c->process;
    uint32_t info[5] = {blocks * 512, directories, files, duplicate & 0xff, 1};
    unsigned char bytes[sizeof info];
    for (unsigned i = 0; i < 5; ++i) snx_st_le32(bytes + i * 4, info[i]);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) /* an open save data archive goes with what it held */
        if (p->self_archives[i].kind == 2) p->self_archives[i].token = 0;
    pm_lock(&p->tree->lock);
    FsTreeResult rc = p->save ? fs_tree_remove(p->tree, NULL, "savedata", 1, 1, 0) : FS_TREE_OK;
    p->save = NULL;
    if (!rc) rc = fs_tree_create(p->tree, NULL, "savedata", 1, 0, &p->save);
    if (!rc) rc = write_whole(p->tree, NULL, CTR_FORMAT_FILE, bytes, sizeof bytes);
    pm_unlock(&p->tree->lock);
    if (rc) p->save = NULL;
    snx_ctr_ipc_error_reply(c, tree_result(rc));
}
/* FSUSER FormatSaveData: archive 4, this title's (an empty path) */
static void format_save(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 9, 2)) return;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1] != 4) {
        snx_ctr_ipc_error_reply(c, 0xd9001001u); /* InvalidPath */
        return;
    }
    if (c->in[2] != 1) { /* another title's save data */
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[10] != ((c->in[3] << 14) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    format_save_data(c, c->in[4], c->in[5], c->in[6], c->in[9]);
}
/* FSUSER GetFormatInfo of the save data or an ExtSaveData: the numbers it was formatted with */
static void format_info(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    bool extdata = (c->in[1] == 6 || c->in[1] == 7) && c->in[2] == 2 && c->in[3] == 12;
    if (!extdata && (c->in[1] != 4 || c->in[2] != 1)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != ((c->in[3] << 14) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    char path[160 + 16] = CTR_FORMAT_FILE;
    if (extdata) {
        uint32_t words[3];
        if (!extdata_path(c, c->in[5], words)) return;
        extdata_folder(path, c->in[1] == 7, words[2], words[1]);
        strcat(path, "/metadata");
    }
    unsigned char bytes[16] = {0};
    FsNode* format = NULL;
    pm_lock(&p->tree->lock);
    FILE* f = (extdata || p->save) && !fs_tree_lookup(p->tree, NULL, path, &format) && !format->is_dir
                  ? fs_tree_open(p->tree, format, "rb")
                  : NULL;
    bool read = f && fread(bytes, 1, sizeof bytes, f) == sizeof bytes;
    if (f) fclose(f);
    pm_unlock(&p->tree->lock);
    if (!read) {
        snx_ctr_ipc_error_reply(c, CTR_NOT_FORMATTED);
        return;
    }
    c->out[0] = 0x08450140;
    for (unsigned i = 0; i < 4; ++i)
        c->out[2 + i] = snx_ld_le32(bytes + i * 4);
    c->out[5] &= 0xff; /* duplicate data is one byte */
}
/* FSUSER GetFreeBytes: what Azahar's archives answer (the RomFS none, the SD card 1 GiB, save data 32 MiB) */
static void free_bytes(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 0)) return;
    CtrSelfArchive* a = archive(c->process, (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32);
    if (!a) {
        snx_ctr_ipc_error_reply(c, CTR_NO_ARCHIVE);
        return;
    }
    uint64_t bytes = a->kind == 0 ? 0 : a->kind == 1 ? 1ull << 30 : 32ull << 20;
    c->out[0] = 0x081200c0;
    c->out[2] = (uint32_t)bytes;
    c->out[3] = (uint32_t)(bytes >> 32);
}
/* An ArchiveResource of the SD card or the NAND: 512-byte sectors, 16 KiB clusters, 8 GiB of them all free (Azahar's
   ArchiveManager::GetArchiveResource) */
static void archive_resource(SnxCtrIpc* c, unsigned normal) {
    if (!snx_ctr_ipc_expect(c, normal, 0)) return;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x140;
    c->out[2] = 512;
    c->out[3] = 16384;
    c->out[4] = c->out[5] = 0x80000;
}
/* A command of no arguments answering one word */
static void one_word(SnxCtrIpc* c, unsigned normal, uint32_t value) {
    if (!snx_ctr_ipc_expect(c, normal, 0)) return;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x80;
    c->out[2] = value;
}
/* FSUSER GetProgramLaunchInfo: this process's title, installed on the SD card as a CIA is */
static void launch_info(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    if (c->in[1] != CTR_PROCESS_ID) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    c->out[0] = 0x082f0140;
    c->out[2] = (uint32_t)c->process->title_id;
    c->out[3] = (uint32_t)(c->process->title_id >> 32);
    c->out[4] = 1; /* MediaType SDMC */
    c->out[5] = 0;
}
/* The secure values, which tie a save to the console against rollback. No console keeps them here, as none does in
   Azahar's default backend: setting one changes nothing, and none exists */
static void secure_value(SnxCtrIpc* c, unsigned normal, unsigned reply_words) {
    if (!snx_ctr_ipc_expect(c, normal, 0)) return;
    c->out[0] = (c->in[0] & 0xffff0000u) | reply_words << 6;
    if (reply_words == 5) c->out[3] = 1; /* GetThisSaveDataSecureValue: the check is overridden, as for a game card */
}
void snx_ctr_fs_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    switch (c->in[0] >> 16) {
        case 0x802: open_file(c); break;
        case 0x803: open_direct(c); break;
        case 0x804: remove_entry(c, 0, 0); break;
        case 0x805: rename_entry(c, 0); break;
        case 0x806: remove_entry(c, 1, 0); break;
        case 0x807: remove_entry(c, 1, 1); break; /* DeleteDirectoryRecursively */
        case 0x808: create_entry(c, 0); break;
        case 0x809: create_entry(c, 1); break;
        case 0x80a: rename_entry(c, 1); break;
        case 0x80b: open_directory(c); break;
        case 0x80c: open_archive(c); break;
        case 0x80d: control_archive(c); break;
        case 0x80f: /* FormatThisUserSaveData: block size, directories, files, two bucket counts, duplicate data */
            if (!snx_ctr_ipc_expect(c, 6, 0)) return;
            format_save_data(c, c->in[1], c->in[2], c->in[3], c->in[6]);
            break;
        case 0x812: free_bytes(c); break;
        case 0x813: one_word(c, 0, 0); break; /* GetCardType: a CTR card */
        case 0x814:                           /* GetSdmcArchiveResource */
        case 0x815: archive_resource(c, 0); break; /* GetNandArchiveResource */
        case 0x817:                                /* IsSdmcDetected */
        case 0x818: one_word(c, 0, !!p->sd); break; /* IsSdmcWritable */
        case 0x821: one_word(c, 0, 0); break; /* CardSlotIsInserted: the title runs from the SD card, no card is in */
        case 0x82f: launch_info(c); break;
        case 0x830: /* Obsoleted_3_0_CreateExtSaveData: media, low, high, icon size, directories, files, the icon */
            if (!snx_ctr_ipc_expect(c, 6, 2)) return;
            create_extdata(c, c->in[1] & 0xff, c->in[2], c->in[3], c->in[5], c->in[6], UINT32_MAX, c->in[4], 7);
            break;
        case 0x835: /* Obsoleted_3_0_DeleteExtSaveData: media, low */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            delete_extdata(c, c->in[1] & 0xff, c->in[2], 0);
            break;
        case 0x845: format_info(c); break;
        case 0x849: archive_resource(c, 1); break; /* GetArchiveResource of a media type */
        case 0x84c: format_save(c); break;
        case 0x851: /* CreateExtSaveData: ExtSaveDataInfo (media, unknown, low, high), directories, files, size limit,
                       icon size, the icon */
            if (!snx_ctr_ipc_expect(c, 9, 2)) return;
            create_extdata(c, c->in[1] & 0xff, c->in[2], c->in[3], c->in[5], c->in[6], c->in[7], c->in[9], 10);
            break;
        case 0x852: /* DeleteExtSaveData: ExtSaveDataInfo */
            if (!snx_ctr_ipc_expect(c, 4, 0)) return;
            delete_extdata(c, c->in[1] & 0xff, c->in[2], c->in[3]);
            break;
        case 0x862: /* SetPriority */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            p->fs_priority = c->in[1];
            c->out[0] = 0x08620040;
            break;
        case 0x863: one_word(c, 0, p->fs_priority); break; /* GetPriority */
        case 0x865: secure_value(c, 5, 1); break;          /* Obsoleted SetSaveDataSecureValue */
        case 0x866: secure_value(c, 3, 4); break;          /* Obsoleted GetSaveDataSecureValue: none */
        case 0x86e: secure_value(c, 3, 1); break;          /* SetThisSaveDataSecureValue */
        case 0x86f: secure_value(c, 1, 5); break;          /* GetThisSaveDataSecureValue: none */
        default: snx_ctr_fs_wire_dispatch(c); break;
    }
}
