/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR FSFILE wire facts: libctru services/fs.c and Azahar service/fs/file.cpp.
   Two real file kinds: the immutable shared RomFS view and read-write host
   files in the sdmc tree. Directory objects share close state only. */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "3ds/services/file.h"
#include "3ds/services/fs.h"
#include "services/file.swipc.inc"
#include <stdlib.h>
#include <string.h>
/* the RomFS a view file reads: a DLC content's, else the game's own */
static SnxCtrRomfs* view_of(const SnxCtrProcess* p, const CtrObject* object) {
    return object->data.file.view ? (SnxCtrRomfs*)object->data.file.view : p->romfs;
}
static uint32_t snx_ctr_file_wire_GetSize(SnxCtrIpc* c, uint64_t* size) {
    CtrObject* object = &c->process->objects[c->object];
    if (object->data.file.closed) return SNX_CTR_IPC_INVALID_STATE;
    SnxCtrProcess* p = c->process;
    if (object->data.file.kind == CTR_FILE_VIEW) {
        *size = snx_ctr_romfs_size(view_of(p, object));
        return 0;
    }
    pm_lock(&p->tree->lock);
    FsNode* node = object->data.file.node;
    *size = node ? node->size : 0;
    pm_unlock(&p->tree->lock);
    return 0;
}
void snx_ctr_file_release(SnxCtrProcess* p, uint32_t id) {
    CtrObject* object = &p->objects[id];
    object->data.file.closed = 1;
    if (object->data.file.kind == CTR_FILE_HOST && object->data.file.host) {
        fclose((FILE*)object->data.file.host);
        object->data.file.host = NULL;
    }
    if (object->data.file.node) {
        fs_tree_release((FsNode*)object->data.file.node);
        object->data.file.node = NULL;
    }
}
static uint32_t snx_ctr_file_wire_Close(SnxCtrIpc* c) {
    CtrObject* object = &c->process->objects[c->object];
    if (object->data.file.closed) return SNX_CTR_IPC_INVALID_STATE;
    snx_ctr_file_release(c->process, c->object);
    return 0;
}
/* A directory entry's 8.3 name: FAT's eleven characters, space-filled and upper case (Azahar's SplitFilename83) */
static void short_name(const char* name, unsigned char out[0x0e]) {
    static const char forbidden[] = ".\"/\\[]:;=, ";
    memset(out, ' ', 8);
    out[8] = 0;
    memset(out + 0x0a, ' ', 3);
    out[0x0d] = 0;
    const char* point = strrchr(name, '.');
    size_t stem = point ? (size_t)(point - name) : strlen(name);
    unsigned j = 0;
    for (size_t i = 0; i < stem; ++i) {
        if (strchr(forbidden, name[i])) continue;
        if (j == 8) {
            out[6] = '~';
            out[7] = '1';
            break;
        }
        out[j++] = (unsigned char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    }
    if (point)
        for (unsigned i = 0; i < 3 && point[1 + i]; ++i)
            out[0x0a + i] = (unsigned char)(point[1 + i] >= 'a' && point[1 + i] <= 'z' ? point[1 + i] - 32 : point[1 + i]);
}
/* A host name (UTF-8) as the entry's UTF-16 name, at most 0x105 units and a terminator; false for bad UTF-8 */
static bool utf16_name(const char* name, unsigned char out[0x20c]) {
    const unsigned char* s = (const unsigned char*)name;
    unsigned n = 0;
    while (*s) {
        unsigned extra = *s < 0x80 ? 0 : (*s & 0xe0) == 0xc0 ? 1 : (*s & 0xf0) == 0xe0 ? 2 : (*s & 0xf8) == 0xf0 ? 3 : 4;
        if (extra > 3) return false;
        uint32_t cp = extra ? *s & (0x3f >> extra) : *s;
        for (unsigned i = 1; i <= extra; ++i) {
            if ((s[i] & 0xc0) != 0x80) return false;
            cp = cp << 6 | (s[i] & 0x3f);
        }
        s += extra + 1;
        unsigned units = cp > 0xffff ? 2 : 1;
        if (n + units > 0x105) break;
        if (units == 2) {
            cp -= 0x10000;
            uint32_t high = 0xd800 | cp >> 10, low = 0xdc00 | (cp & 0x3ff);
            out[n * 2] = (unsigned char)high, out[n * 2 + 1] = (unsigned char)(high >> 8), ++n;
            cp = low;
        }
        out[n * 2] = (unsigned char)cp, out[n * 2 + 1] = (unsigned char)(cp >> 8), ++n;
    }
    return true;
}
/* FSDIR Read: the next entries, 0x228 bytes each, into the mapped buffer (Azahar's DiskDirectory::Read and its Entry:
   the archive bit set on files, as on an SD card that never cleared it, a dot name hidden) */
static void read_directory(SnxCtrIpc* c, CtrObject* object) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t count = c->in[1], address = c->in[3];
    if (count > 0x10000 || c->in[2] != ((count * 0x228u) << 4 | 12)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (snx_ctr_check_ram(p->space, address, (size_t)count * 0x228, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    unsigned char entry[0x228];
    uint32_t read = 0;
    bool ok = true;
    pm_lock(&p->tree->lock);
    FsNode* child = ((FsNode*)object->data.file.node)->children;
    for (uint32_t skip = object->data.file.position; child && skip; --skip) child = child->next;
    for (; child && read < count && ok; child = child->next, ++read) {
        memset(entry, 0, sizeof entry);
        if (!utf16_name(child->name, entry)) memset(entry, 0, 0x20c);
        short_name(child->name, entry + 0x20c);
        entry[0x21a] = 1; /* unknown, always 1 */
        entry[0x21c] = (unsigned char)!!child->is_dir;
        entry[0x21d] = child->name[0] == '.';
        entry[0x21e] = !child->is_dir;
        uint64_t size = child->is_dir ? 0 : child->size;
        snx_st_le64(entry + 0x220, size);
        ok = snx_ctr_write_ram(p->space, address + read * 0x228, entry, sizeof entry) == SNX_CTR_MEM_OK;
    }
    pm_unlock(&p->tree->lock);
    if (!ok) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    object->data.file.position += read;
    c->out[0] = 0x08010082;
    c->out[2] = read;
    c->out[3] = c->in[2];
    c->out[4] = address;
}
static void directory_dispatch(SnxCtrIpc* c, CtrObject* object) {
    switch (c->in[0] >> 16) {
        case 0x801: read_directory(c, object); break;
        case 0x802: /* Close */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            snx_ctr_file_release(c->process, c->object);
            c->out[0] = 0x08020040;
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
/* FSFILE OpenLinkFile: another session on the same file, its own handle on the host file, read and written as the
   first is (Azahar's File::OpenLinkFile), moved to the caller */
static void open_link(SnxCtrIpc* c, CtrObject* object) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    FILE* host = NULL;
    if (object->data.file.kind == CTR_FILE_HOST && object->data.file.host) {
        pm_lock(&p->tree->lock);
        host = fs_tree_open(p->tree, object->data.file.node, object->data.file.writable ? "r+b" : "rb");
        pm_unlock(&p->tree->lock);
        if (!host) {
            snx_ctr_ipc_error_reply(c, 0xc88044d2u);
            return;
        }
    }
    uint32_t kind = object->data.file.kind, writable = object->data.file.writable, fixed = object->data.file.fixed;
    uint32_t priority = object->data.file.priority;
    if (!snx_ctr_fs_file_object(c, kind, writable, fixed, host, object->data.file.node)) {
        if (host) fclose(host);
        return;
    }
    p->objects[snx_ctr_handle_object(p, c->out[3])].data.file.priority = priority;
    p->objects[snx_ctr_handle_object(p, c->out[3])].data.file.view = object->data.file.view;
}
void snx_ctr_file_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    uint32_t command = c->in[0] >> 16;
    CtrObject* object = &p->objects[c->object];
    if (object->data.file.kind == CTR_FILE_DIR) {
        if (object->data.file.closed)
            snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        else
            directory_dispatch(c, object);
        return;
    }
    if (command != 0x802 && command != 0x803 && command != 0x805 && (command < 0x809 || command > 0x80c)) {
        snx_ctr_file_wire_dispatch(c);
        return;
    }
    if (object->data.file.closed) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    switch (command) {
        case 0x809: /* Flush: every write already reached the host file, unbuffered */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            c->out[0] = 0x08090040;
            return;
        case 0x80a: /* SetPriority */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            object->data.file.priority = c->in[1];
            c->out[0] = 0x080a0040;
            return;
        case 0x80b: /* GetPriority */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            c->out[0] = 0x080b0080;
            c->out[2] = object->data.file.priority;
            return;
        case 0x80c: open_link(c, object); return;
        default: break;
    }
    if (command == 0x805) { /* SetSize: read-write host files only; an ExtSaveData's keeps its size, as Azahar's */
        if (!snx_ctr_ipc_expect(c, 2, 0)) return;
        if (object->data.file.kind != CTR_FILE_HOST || !object->data.file.writable) {
            snx_ctr_ipc_error_reply(c, 0xd8e01beeu);
            return;
        }
        if (object->data.file.fixed) {
            c->out[0] = 0x08050040;
            return;
        }
        uint64_t size = (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32;
        pm_lock(&p->tree->lock);
        FsTreeResult rc = fs_tree_resize(p->tree, object->data.file.node, size);
        pm_unlock(&p->tree->lock);
        if (rc) {
            snx_ctr_ipc_error_reply(c, 0xe0e01bf4u);
            return;
        }
        c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
        return;
    }
    if (!snx_ctr_ipc_expect(c, command == 0x802 ? 3 : 4, 2)) return;
    uint32_t size = c->in[3], address = c->in[command == 0x802 ? 5 : 6];
    uint64_t offset = (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32;
    if (command == 0x803 && object->data.file.kind != CTR_FILE_HOST) {
        snx_ctr_ipc_error_reply(c, 0xd8e01beeu); /* the RomFS view is immutable */
        return;
    }
    if (command == 0x803 && !object->data.file.writable) { /* opened without the write flag (Azahar's DiskFile) */
        snx_ctr_ipc_error_reply(c, 0xc92044e6u);           /* InvalidOpenFlags */
        return;
    }
    if (size > 16u * 1024 * 1024) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[command == 0x802 ? 4 : 5] != (size << 4 | (command == 0x802 ? 12 : 10))) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (command == 0x802) {
        if (snx_ctr_check_ram(p->space, address, size, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
            c->transport_result = 0xe0e01bf5u;
            return;
        }
    } else if (snx_ctr_check_ram(p->space, address, size, SNX_CTR_READ) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint64_t total = object->data.file.kind == CTR_FILE_VIEW ? snx_ctr_romfs_size(view_of(p, object)) : 0;
    if (object->data.file.kind != CTR_FILE_VIEW) {
        pm_lock(&p->tree->lock);
        total = ((FsNode*)object->data.file.node)->size;
        pm_unlock(&p->tree->lock);
    }
    /* an ExtSaveData file does not grow: a write past its end fails, one reaching it stops there (Azahar's
       FixSizeDiskFile) */
    if (command == 0x803 && object->data.file.fixed && offset > total) {
        snx_ctr_ipc_error_reply(c, 0xe0c046c1u); /* WriteBeyondEnd */
        return;
    }
    uint32_t n = size;
    if ((command == 0x802 || object->data.file.fixed) && offset >= total)
        n = 0;
    else if ((command == 0x802 || object->data.file.fixed) && total - offset < size)
        n = (uint32_t)(total - offset);
    unsigned char* data = malloc(n ? n : 1);
    if (!data) {
        snx_ctr_ipc_error_reply(c, 0xd86007f3u);
        return;
    }
    if (command == 0x802) { /* Read: stage the complete result before guest writes */
        bool ok;
        if (object->data.file.kind == CTR_FILE_VIEW) {
            ok = !n || snx_ctr_romfs_read(view_of(p, object), offset, data, n);
        } else {
            FILE* host = object->data.file.host;
            FsNode* node = object->data.file.node;
            pm_lock(&p->tree->lock);
            if (!host) /* a section the tree holds in memory (a CIA's ExeFS file) */
                ok = !n || (node->virtual_data && (memcpy(data, node->virtual_data + offset, n), true));
            else
                ok = !n || (!fseek(host, (long)offset, SEEK_SET) && fread(data, 1, n, host) == n);
            pm_unlock(&p->tree->lock);
        }
        if (ok && n) ok = snx_ctr_write_ram(p->space, address, data, n) == SNX_CTR_MEM_OK;
        free(data);
        if (!ok) {
            c->state = SNX_CTR_IPC_FAULT;
            return;
        }
        c->out[0] = 0x08020082;
        c->out[2] = n;
        c->out[3] = c->in[4];
        c->out[4] = address;
        return;
    }
    /* Write: the guest bytes are staged first, then committed to the host. */
    if (n && snx_ctr_read_ram(p->space, address, data, n) != SNX_CTR_MEM_OK) {
        free(data);
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    FILE* host = object->data.file.host;
    pm_lock(&p->tree->lock);
    bool ok = !n || (!fseek(host, (long)offset, SEEK_SET) && fwrite(data, 1, n, host) == n && !fflush(host));
    if (ok) {
        FsNode* node = object->data.file.node;
        if (offset + n > node->size) node->size = offset + n;
    }
    pm_unlock(&p->tree->lock);
    free(data);
    if (!ok) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    c->out[0] = 0x08030082;
    c->out[2] = n;
    c->out[3] = c->in[5];
    c->out[4] = address;
}
