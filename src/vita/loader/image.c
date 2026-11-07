/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SELF container, the SCE ELF module format (module info, import and export tables, the SCE_RELA relocations)
   as the Vita's homebrew toolchain writes them (vita-toolchain's vita-elf-create, and its headers, MIT), and the PS
   Vita developer wiki's SELF pages */
#include "vita/loader/image.h"
#include "vita/loader/inflate.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ET_SCE_EXEC    0xfe00u
#define ET_SCE_RELEXEC 0xfe04u
#define PT_LOAD        1u
#define PT_SCE_RELA    0x60000000u
#define NID_MODULE_START  0x935cd196u
#define NID_MODULE_STOP   0x79f8e492u
#define NID_PROCESS_PARAM 0x70fba1e7u
#define PAGE 0x1000u

typedef struct {
    const unsigned char* data;
    size_t size;
    uint32_t type, vaddr, filesz, memsz, flags, align;
    unsigned char* owned; /* a decompressed segment's bytes */
} Program;
typedef struct {
    VitaImage* image;
    uint32_t base[16]; /* each program header's load address, by its index (a relocation's segment), 0 for none */
    int loaded[16];    /* the image segment of each program header, -1 for none */
    size_t seed_capacity;
    uint32_t origin, variable_base;
    char* error;
    size_t error_size;
} Loader;


const VitaSegment* vita_image_segment(const VitaImage* image, uint32_t address, uint32_t size) {
    for (unsigned i = 0; i < image->segment_count; ++i) {
        const VitaSegment* s = &image->segment[i];
        if (address >= s->address && (uint64_t)address + size <= (uint64_t)s->address + s->size) return s;
    }
    return NULL;
}
static unsigned char* at(VitaImage* image, uint32_t address, uint32_t size) {
    const VitaSegment* s = vita_image_segment(image, address, size);
    return s ? s->bytes + (address - s->address) : NULL;
}
static bool word(VitaImage* image, uint32_t address, uint32_t* value) {
    const unsigned char* p = at(image, address, 4);
    if (p) *value = snx_ld_le32(p);
    return p != NULL;
}
static bool fail(Loader* l, const char* format, uint32_t a, uint32_t b) {
    snprintf(l->error, l->error_size, format, a, b);
    return false;
}
/* key, an address in executable memory with bit 0 for Thumb, where discovery starts */
static void seed_code(Loader* l, uint32_t key) {
    VitaImage* image = l->image;
    const VitaSegment* s = vita_image_segment(image, key & ~1u, key & 1 ? 2 : 4);
    if (!s || !(s->flags & 1)) return;
    if (image->seed_count == l->seed_capacity) {
        size_t capacity = l->seed_capacity ? l->seed_capacity * 2 : 1024;
        uint32_t* grown = realloc(image->seeds, capacity * sizeof *grown);
        if (!grown) return;
        image->seeds = grown, l->seed_capacity = capacity;
    }
    image->seeds[image->seed_count++] = key;
}
/* a pointer the image holds, when a Thumb function's address */
static void seed(Loader* l, uint32_t pointer) {
    if (pointer & 1) seed_code(l, pointer);
}
static uint32_t thumb_imm16(uint32_t hw1, uint32_t hw2) {
    return (hw1 & 15) << 12 | ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xff);
}
/* A fixed-address module has no relocations to say where its code pointers are: each word of the image and each
   Thumb MOVW and MOVT of one register a few instructions apart (how the SDK's compiler calls far) that makes a Thumb
   function's address is taken as one. One that isn't code is refused by discovery */
static void seed_pointers(Loader* l) {
    const VitaImage* image = l->image;
    for (unsigned i = 0; i < image->segment_count; ++i) {
        const VitaSegment* s = &image->segment[i];
        for (uint32_t at = 0; at + 4 <= s->size; at += 4) seed(l, snx_ld_le32(s->bytes + at));
        if (!(s->flags & 1)) continue;
        for (uint32_t at = 0; at + 4 <= s->size; at += 2) {
            uint32_t hw1 = snx_ld_le16(s->bytes + at), hw2 = snx_ld_le16(s->bytes + at + 2);
            if ((hw1 & 0xfbf0) != 0xf240 || (hw2 & 0x8000)) continue; /* MOVW (T3) */
            uint32_t rd = (hw2 >> 8) & 15, low = thumb_imm16(hw1, hw2);
            for (uint32_t next = at + 4; next + 4 <= s->size && next <= at + 16; next += 2) {
                uint32_t h1 = snx_ld_le16(s->bytes + next), h2 = snx_ld_le16(s->bytes + next + 2);
                if ((h1 & 0xfbf0) == 0xf2c0 && !(h2 & 0x8000) && ((h2 >> 8) & 15) == rd) { /* MOVT (T1) */
                    seed(l, thumb_imm16(h1, h2) << 16 | low);
                    break;
                }
            }
        }
    }
}

/* One relocation: code (ARM ELF's relocation types) at P in segment datseg, the symbol's segment's base S (0 for
   segment 15, an absolute value) plus A. A PC-relative one inside one segment changes nothing, as the segment moves
   whole; across segments it is computed again */
static bool relocate(Loader* l, unsigned code, unsigned symseg, unsigned datseg, uint32_t offset, uint32_t addend) {
    if (datseg >= 16 || l->loaded[datseg] < 0 || (symseg != 15 && (symseg >= 16 || l->loaded[symseg] < 0)))
        return fail(l, "relocation between segments %u and %u, which aren't loaded", symseg, datseg);
    uint32_t S = symseg == 15 ? 0 : l->base[symseg], P = l->base[datseg] + offset, value = S + addend;
    bool same = symseg == datseg;
    unsigned char* p = at(l->image, P, code == 0 || code == 40 ? 0 : 4);
    if (!p) return fail(l, "relocation at 0x%08x (type %u) outside its segment", P, code);
    switch (code) {
        case 0:  /* R_ARM_NONE */
        case 40: /* R_ARM_V4BX: BX is the architecture's on ARMv7 */
            return true;
        case 2:  /* R_ARM_ABS32 */
        case 38: /* R_ARM_TARGET1, ABS32 on the Vita */
            snx_st_le32(p, value);
            seed(l, value);
            return true;
        case 3:  /* R_ARM_REL32 */
        case 41: /* R_ARM_TARGET2, REL32 on the Vita */
            if (!same) snx_st_le32(p, value - P);
            return true;
        case 42: /* R_ARM_PREL31 */
            if (!same) snx_st_le32(p, (snx_ld_le32(p) & 0x80000000u) | ((value - P) & 0x7fffffffu));
            return true;
        case 10: /* R_ARM_THM_CALL */
        case 30: /* R_ARM_THM_JUMP24 */
        case 28: /* R_ARM_CALL */
        case 29: /* R_ARM_JUMP24 */
            if (!same) return fail(l, "a branch between segments (relocation type %u at 0x%08x)", code, P);
            return true;
        case 43: /* R_ARM_MOVW_ABS_NC */
        case 44: { /* R_ARM_MOVT_ABS */
            uint32_t v = code == 44 ? value >> 16 : value & 0xffff, w = snx_ld_le32(p);
            snx_st_le32(p, (w & 0xfff0f000u) | ((v & 0xf000) << 4) | (v & 0x0fff));
            return true;
        }
        case 47: /* R_ARM_THM_MOVW_ABS_NC */
        case 48: { /* R_ARM_THM_MOVT_ABS: imm16 = imm4:i:imm3:imm8 over the two halfwords */
            uint32_t v = code == 48 ? value >> 16 : value & 0xffff, upper = snx_ld_le16(p), lower = snx_ld_le16(p + 2);
            upper = (upper & 0xfbf0) | ((v >> 12) & 15) | (((v >> 11) & 1) << 10);
            lower = (lower & 0x8f00) | (((v >> 8) & 7) << 12) | (v & 0xff);
            snx_st_le16(p, upper), snx_st_le16(p + 2, lower);
            if (code == 47) seed(l, value);
            return true;
        }
        default: return fail(l, "unknown relocation type %u at 0x%08x", code, P);
    }
}
static bool relocate_all(Loader* l, const unsigned char* r, size_t size) {
    for (size_t pos = 0; pos < size;) {
        if (size - pos < 8) return fail(l, "truncated relocation at %u of %u bytes", (uint32_t)pos, (uint32_t)size);
        uint32_t type = snx_ld_le32(r + pos), offset, addend, code2 = 0, dist2 = 0;
        if (type & 15) { /* the short form: the offset split over the two words, a 12-bit addend */
            uint32_t second = snx_ld_le32(r + pos + 4);
            offset = (type >> 20) | (second & 0xfffff) << 12;
            addend = second >> 20;
            pos += 8;
        } else {
            if (size - pos < 12) return fail(l, "truncated relocation at %u of %u bytes", (uint32_t)pos, (uint32_t)size);
            addend = snx_ld_le32(r + pos + 4);
            offset = snx_ld_le32(r + pos + 8);
            code2 = (type >> 20) & 0xff;
            dist2 = (type >> 28) & 15;
            pos += 12;
        }
        unsigned code = (type >> 8) & 0xff, symseg = (type >> 4) & 15, datseg = (type >> 16) & 15;
        if (!relocate(l, code, symseg, datseg, offset, addend)) return false;
        /* a second relocation of the same symbol dist2 halfwords on (a MOVW's MOVT) */
        if (code2 && !relocate(l, code2, symseg, datseg, offset + dist2 * 2, addend)) return false;
    }
    return true;
}
/* An imported variable's references: a header word (0x40, then the count from bit 7), then two words for each, the
   relocation's type in the first's second byte and its offset from the module's first segment in the second. Each is
   relocated to the variable's slot */
static bool variable_references(Loader* l, uint32_t references, uint32_t slot) {
    uint32_t header, count, first = 16;
    for (unsigned i = 0; i < 16 && first == 16; ++i)
        if (l->loaded[i] >= 0) first = i;
    if (!word(l->image, references, &header) || (header & 0x7f) != 0x40 || first == 16)
        return fail(l, "bad variable import references at 0x%08x", references, 0);
    count = (header >> 7) & 0x1ffff;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t kind, offset;
        if (!word(l->image, references + 4 + i * 8, &kind) || !word(l->image, references + 8 + i * 8, &offset))
            return fail(l, "bad variable import references at 0x%08x", references, 0);
        if ((kind & 0xf) != 1) return fail(l, "variable import reference of form %u at 0x%08x", kind & 0xf, references);
        /* The entry (Vita3K's EntryFormat1Alt): format in bits 0 to 3, the patched segment in 4 to 7, the relocation's
           code in 8 to 15 and its addend in 16 to 31. The value written is the variable's address plus that addend:
           a C++ class's type_info has its vtable pointer as `&_ZTV... + 8`, and a vtable's RTTI pointer is read at
           vptr[-1], so the addend must stay or the program reads the neighbouring slot's last word instead */
        unsigned patch = (kind >> 4) & 0xf;
        if (!relocate(l, (kind >> 8) & 0xff, 15, l->loaded[patch] >= 0 ? patch : first, offset, slot + (kind >> 16)))
            return false;
    }
    return true;
}
/* Fills each imported function's stub: "svc #index; bx lr", in the stub's instruction set; gives each imported
   variable its slot */
static bool imports(Loader* l, uint32_t top, uint32_t end) {
    VitaImage* image = l->image;
    uint32_t count = 0, variable_capacity = 0;
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t index = 0;
        for (uint32_t p = top; p < end;) {
            const unsigned char* e = at(image, p, 0x24);
            if (!e || (snx_ld_le16(e) != 0x24 && snx_ld_le16(e) != 0x34) || !at(image, p, snx_ld_le16(e)))
                return fail(l, "bad import table entry at 0x%08x", p, 0);
            uint32_t functions = snx_ld_le16(e + 6), variables, library_nid, name, nids, entries, variable_nids,
                     variable_entries;
            if (snx_ld_le16(e) == 0x34) { /* the older layout, with TLS variables */
                if (snx_ld_le16(e + 10)) return fail(l, "the module imports %u TLS variables (at 0x%08x), which isn't "
                                                 "supported yet", snx_ld_le16(e + 10), p);
                variables = snx_ld_le16(e + 8);
                library_nid = snx_ld_le32(e + 16), name = snx_ld_le32(e + 20), nids = snx_ld_le32(e + 28), entries = snx_ld_le32(e + 32);
                variable_nids = snx_ld_le32(e + 36), variable_entries = snx_ld_le32(e + 40);
            } else {
                variables = snx_ld_le32(e + 8);
                library_nid = snx_ld_le32(e + 12), name = snx_ld_le32(e + 16), nids = snx_ld_le32(e + 20), entries = snx_ld_le32(e + 24);
                variable_nids = snx_ld_le32(e + 28), variable_entries = snx_ld_le32(e + 32);
            }
            VitaImport named = {.library_nid = library_nid};
            for (unsigned c = 0; c + 1 < sizeof named.library; ++c) {
                const unsigned char* ch = at(image, name + c, 1);
                if (!ch || !*ch) break;
                named.library[c] = (char)*ch;
            }
            if (!pass) variable_capacity += variables;
            for (uint32_t i = 0; pass && i < variables; ++i) {
                uint32_t nid, references, v = 0;
                if (!word(image, variable_nids + i * 4, &nid) || !word(image, variable_entries + i * 4, &references))
                    return fail(l, "bad import table at 0x%08x", p, 0);
                /* v walks this module's own table, which starts empty even for a library: a match reuses that slot,
                   a miss appends one. Slots are numbered after those of the modules loaded before it, which
                   variable_base counts, or its first would land on the game's */
                while (v < image->variable_count && image->variables[v].nid != nid) ++v;
                if (v == image->variable_count) {
                    if (l->variable_base + v >= VITA_VARIABLE_COUNT)
                        return fail(l, "more than %u imported variables", l->variable_base + v, 0);
                    image->variables[v] = named;
                    image->variables[v].nid = nid;
                    image->variables[v].stub = VITA_VARIABLES + (l->variable_base + v) * VITA_VARIABLE_SIZE;
                    ++image->variable_count;
                }
                if (!variable_references(l, references, image->variables[v].stub)) return false;
            }
            for (uint32_t i = 0; i < functions; ++i, ++index) {
                uint32_t nid, stub;
                if (!word(image, nids + i * 4, &nid) || !word(image, entries + i * 4, &stub))
                    return fail(l, "bad import table at 0x%08x", p, 0);
                if (!pass) continue;
                VitaImport* import = &image->imports[index];
                *import = named;
                import->nid = nid, import->stub = stub, import->svc = index;
                unsigned char* code = at(image, stub & ~1u, 4);
                if (!code) return fail(l, "import stub 0x%08x outside the module", stub, 0);
                seed_code(l, stub);
                if (stub & 1) {
                    if (index > 255) return fail(l, "Thumb import stub 0x%08x for import %u", stub, index);
                    snx_st_le16(code, 0xdf00 | index); /* svc #index */
                    snx_st_le16(code + 2, 0x4770);     /* bx lr */
                } else {
                    if (!at(image, stub, 8)) return fail(l, "import stub 0x%08x outside the module", stub, 0);
                    snx_st_le32(code, 0xef000000u | index); /* svc #index */
                    snx_st_le32(code + 4, 0xe12fff1eu);     /* bx lr */
                }
            }
            p += snx_ld_le16(e);
        }
        if (!pass) {
            count = index;
            if (count >= VITA_SVC_RUNTIME) return fail(l, "%u imports", count, 0);
            image->imports = calloc(count ? count : 1, sizeof *image->imports);
            image->variables = calloc(variable_capacity ? variable_capacity : 1, sizeof *image->variables);
            if (!image->imports || !image->variables) return fail(l, "out of memory", 0, 0);
            image->import_count = count;
        }
    }
    return true;
}
static bool exports(Loader* l, uint32_t top, uint32_t end) {
    VitaImage* image = l->image;
    uint32_t total = 0;
    for (uint32_t p = top; p < end;) {
        const unsigned char* e = at(image, p, 0x20);
        if (!e || snx_ld_le16(e) < 0x20) return fail(l, "bad export table entry at 0x%08x", p, 0);
        total += snx_ld_le16(e + 6) + snx_ld_le32(e + 8) + snx_ld_le32(e + 12);
        p += snx_ld_le16(e);
    }
    image->exports = calloc(total ? total : 1, sizeof *image->exports);
    if (!image->exports) return fail(l, "out of memory", 0, 0);
    for (uint32_t p = top; p < end;) {
        const unsigned char* e = at(image, p, 0x20);
        uint32_t functions = snx_ld_le16(e + 6), variables = snx_ld_le32(e + 8), tls = snx_ld_le32(e + 12);
        uint32_t count = functions + variables + tls, library_nid = snx_ld_le32(e + 16);
        uint32_t nids = snx_ld_le32(e + 24), entries = snx_ld_le32(e + 28);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t nid, value;
            if (!word(image, nids + i * 4, &nid) || !word(image, entries + i * 4, &value))
                return fail(l, "bad export table at 0x%08x", p, 0);
            image->exports[image->export_count++] = (VitaExport){nid, library_nid, value, i >= functions};
            if (nid == NID_MODULE_START) image->module_start = value;
            if (nid == NID_MODULE_STOP) image->module_stop = value;
            if (nid == NID_PROCESS_PARAM) image->process_param = value;
            if (i < functions) seed(l, value);
        }
        p += snx_ld_le16(e);
    }
    return true;
}
static bool identity(VitaImage* image) {
    static const char domain[] = "ScarletNX Vita image identity v1";
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    unsigned length = 0;
    bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
              EVP_DigestUpdate(hash, domain, sizeof domain) == 1;
    for (unsigned i = 0; ok && i < image->segment_count; ++i) {
        unsigned char header[12];
        snx_st_le32(header, image->segment[i].address), snx_st_le32(header + 4, image->segment[i].size);
        snx_st_le32(header + 8, image->segment[i].flags);
        ok = EVP_DigestUpdate(hash, header, sizeof header) == 1 &&
             EVP_DigestUpdate(hash, image->segment[i].bytes, image->segment[i].size) == 1;
    }
    ok = ok && EVP_DigestFinal_ex(hash, image->identity, &length) == 1 && length == sizeof image->identity;
    EVP_MD_CTX_free(hash);
    return ok;
}
/* The program headers and their bytes, from a SELF's segment table (each maybe deflated) or the ELF's own */
static bool programs(Loader* l, const unsigned char* file, size_t size, Program* out, unsigned* count,
                     const unsigned char** elf) {
    bool self = size >= 0x78 && snx_ld_le32(file) == 0x00454353u; /* "SCE\0" */
    uint64_t elf_offset = self ? snx_ld_le64(file + 0x40) : 0, phdr_offset, info = self ? snx_ld_le64(file + 0x58) : 0;
    if (elf_offset > size || size - elf_offset < 52 || memcmp(file + elf_offset, "\x7f" "ELF\x01\x01", 6))
        return fail(l, "eboot.bin is neither a SELF nor an ELF", 0, 0);
    *elf = file + elf_offset;
    if (snx_ld_le16(*elf + 18) != 40) return fail(l, "eboot.bin is not an ARM module (machine %u)", snx_ld_le16(*elf + 18), 0);
    *count = snx_ld_le16(*elf + 44);
    phdr_offset = self ? snx_ld_le64(file + 0x48) : snx_ld_le32(*elf + 28);
    if (*count > 16 || phdr_offset > size || (size - phdr_offset) / 32 < *count ||
        (self && (info > size || (size - info) / 32 < *count)))
        return fail(l, "bad program headers (%u)", *count, 0);
    for (unsigned i = 0; i < *count; ++i) {
        const unsigned char* ph = file + phdr_offset + i * 32;
        Program* p = &out[i];
        p->type = snx_ld_le32(ph), p->vaddr = snx_ld_le32(ph + 8), p->filesz = snx_ld_le32(ph + 16), p->memsz = snx_ld_le32(ph + 20);
        p->flags = snx_ld_le32(ph + 24), p->align = snx_ld_le32(ph + 28);
        uint64_t offset = snx_ld_le32(ph + 4), length = p->filesz;
        unsigned compression = 1;
        if (self) {
            const unsigned char* s = file + info + i * 32;
            offset = snx_ld_le64(s), length = snx_ld_le64(s + 8), compression = (unsigned)snx_ld_le64(s + 16);
            if (snx_ld_le64(s + 24) != 2 && length)
                return fail(l, "eboot.bin is encrypted (segment %u): a retail game's needs decrypting first", i, 0);
        }
        if (offset > size || length > size - offset) return fail(l, "segment %u is outside the file", i, 0);
        if (compression == 2) {
            p->owned = malloc(p->filesz ? p->filesz : 1);
            if (!p->owned || !vita_zlib_inflate(file + offset, (size_t)length, p->owned, p->filesz))
                return fail(l, "cannot decompress segment %u", i, 0);
            p->data = p->owned, p->size = p->filesz;
        } else {
            if (length < p->filesz && p->type == PT_LOAD) return fail(l, "segment %u is truncated", i, 0);
            p->data = file + offset, p->size = (size_t)length;
        }
    }
    return true;
}
bool vita_image_load_at(const char* path, VitaImage* image, uint32_t origin, uint32_t variable_base, char* error, size_t error_size) {
    memset(image, 0, sizeof *image);
    Loader l = {.image = image, .error = error, .error_size = error_size, .origin = origin, .variable_base = variable_base};
    for (unsigned i = 0; i < 16; ++i) l.loaded[i] = -1;
    size_t size = 0;
    unsigned char* file = snx_read_file(path, &size);
    Program ph[16] = {{0}};
    unsigned count = 0;
    const unsigned char* elf = NULL;
    bool ok = file != NULL;
    if (!ok) {
        char why[256];
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
    }
    ok = ok && programs(&l, file, size, ph, &count, &elf);
    uint32_t type = ok ? snx_ld_le16(elf + 16) : 0, entry = ok ? snx_ld_le32(elf + 24) : 0;
    if (ok && type != ET_SCE_RELEXEC && type != ET_SCE_EXEC)
        ok = fail(&l, "eboot.bin is not a Vita module (ELF type 0x%x)", type, 0);
    /* the loaded segments keep their places relative to each other, the first at VITA_LOAD_BASE, or where a
       fixed-address module's are: within the image's range either way */
    uint32_t first = UINT32_MAX;
    for (unsigned i = 0; ok && i < count; ++i)
        if (ph[i].type == PT_LOAD && ph[i].vaddr < first) first = ph[i].vaddr & ~(PAGE - 1);
    if (type == ET_SCE_EXEC) l.origin = first;
    for (unsigned i = 0; ok && i < count; ++i) {
        if (ph[i].type != PT_LOAD) continue;
        uint64_t address = (uint64_t)l.origin + (ph[i].vaddr - first);
        if (image->segment_count == VITA_SEGMENTS || ph[i].memsz < ph[i].filesz || !ph[i].memsz ||
            address < (l.origin < VITA_LOAD_BASE ? 0x70000000u : VITA_LOAD_BASE) ||
            address + ph[i].memsz > (l.origin < VITA_LOAD_BASE ? 0x80000000u : VITA_IMAGE_END)) {
            ok = fail(&l, "bad loadable segment %u (at 0x%08x)", i, ph[i].vaddr);
            break;
        }
        VitaSegment* s = &image->segment[image->segment_count];
        s->address = (uint32_t)address;
        s->size = ph[i].memsz;
        s->flags = ph[i].flags;
        s->bytes = calloc(1, s->size);
        if (!s->bytes) {
            ok = fail(&l, "out of memory", 0, 0);
            break;
        }
        memcpy(s->bytes, ph[i].data, ph[i].filesz);
        for (unsigned j = 0; j < image->segment_count; ++j)
            if (s->address < image->segment[j].address + image->segment[j].size &&
                image->segment[j].address < s->address + s->size)
                ok = fail(&l, "segments %u and %u overlap", j, image->segment_count);
        l.base[i] = s->address;
        l.loaded[i] = (int)image->segment_count++;
    }
    for (unsigned i = 0; ok && i < count; ++i)
        if (ph[i].type == PT_SCE_RELA) ok = relocate_all(&l, ph[i].data, ph[i].size);
    /* the module info, in the segment e_entry's top bits name, at the offset its others give */
    unsigned info_segment = entry >> 30;
    const unsigned char* info = NULL;
    if (ok && (info_segment >= count || l.loaded[info_segment] < 0 ||
               !(info = at(image, l.base[info_segment] + (entry & 0x3fffffffu), 0x5c))))
        ok = fail(&l, "no module info (e_entry 0x%08x)", entry, 0);
    if (ok) {
        uint32_t base = l.base[info_segment];
        memcpy(image->name, info + 4, 27);
        ok = exports(&l, base + snx_ld_le32(info + 36), base + snx_ld_le32(info + 40)) &&
             imports(&l, base + snx_ld_le32(info + 44), base + snx_ld_le32(info + 48));
        if (ok && !image->module_start && snx_ld_le32(info + 68)) image->module_start = base + snx_ld_le32(info + 68);
        if (ok && !image->module_start) ok = fail(&l, "the module has no module_start", 0, 0);
        /* the thread-local data: its image, the bytes it starts with and its size */
        image->tls_image = snx_ld_le32(info + 64) ? base + snx_ld_le32(info + 56) : 0;
        image->tls_filesz = snx_ld_le32(info + 60), image->tls_memsz = snx_ld_le32(info + 64);
        if (ok && image->tls_memsz && (image->tls_filesz > image->tls_memsz || image->tls_memsz > 0x100000u ||
                                       !at(image, image->tls_image, image->tls_filesz)))
            ok = fail(&l, "bad thread-local data (%u bytes at 0x%08x)", image->tls_memsz, image->tls_image);
    }
    if (ok && type == ET_SCE_EXEC) seed_pointers(&l);
    if (ok) {
        seed(&l, image->module_start);
        if (image->module_stop) seed(&l, image->module_stop);
        ok = identity(image) || fail(&l, "cannot hash the image", 0, 0);
    }
    for (unsigned i = 0; i < count; ++i) free(ph[i].owned);
    free(file);
    if (!ok) vita_image_free(image);
    return ok;
}
bool vita_image_load(const char* path, VitaImage* image, char* error, size_t error_size) {
    return vita_image_load_at(path, image, VITA_LOAD_BASE, 0, error, error_size);
}
bool vita_image_merge(VitaImage* image, VitaImage* library, char* error, size_t error_size) {
    if (image->segment_count + library->segment_count > VITA_SEGMENTS) {
        snprintf(error, error_size, "too many loaded Vita segments");
        return false;
    }
    for (unsigned i = 0; i < library->segment_count; ++i) {
        for (unsigned j = 0; j < image->segment_count; ++j) {
            VitaSegment* a = &image->segment[j], *b = &library->segment[i];
            if (a->address < (uint64_t)b->address + b->size && b->address < (uint64_t)a->address + a->size) {
                snprintf(error, error_size, "Vita library segments overlap at 0x%08x", b->address);
                return false;
            }
        }
        image->segment[image->segment_count++] = library->segment[i];
        library->segment[i].bytes = NULL;
    }
    VitaImport* imports = realloc(image->imports, (image->import_count + library->import_count) * sizeof *imports);
    if (!imports && library->import_count) { snprintf(error, error_size, "out of memory"); return false; }
    image->imports = imports;
    memcpy(image->imports + image->import_count, library->imports, library->import_count * sizeof *imports);
    image->import_count += library->import_count;
    VitaImport* variables = realloc(image->variables, (image->variable_count + library->variable_count) * sizeof *variables);
    if (!variables && library->variable_count) { snprintf(error, error_size, "out of memory"); return false; }
    image->variables = variables;
    if (library->variable_count)
        memcpy(image->variables + image->variable_count, library->variables, library->variable_count * sizeof *variables);
    image->variable_count += library->variable_count;
    VitaExport* exports = realloc(image->exports, (image->export_count + library->export_count) * sizeof *exports);
    if (!exports && library->export_count) { snprintf(error, error_size, "out of memory"); return false; }
    image->exports = exports;
    memcpy(image->exports + image->export_count, library->exports, library->export_count * sizeof *exports);
    image->export_count += library->export_count;
    uint32_t* seeds = realloc(image->seeds, (image->seed_count + library->seed_count) * sizeof *seeds);
    if (!seeds && library->seed_count) { snprintf(error, error_size, "out of memory"); return false; }
    image->seeds = seeds;
    memcpy(image->seeds + image->seed_count, library->seeds, library->seed_count * sizeof *seeds);
    image->seed_count += library->seed_count;
    if (library->module_start && image->library_start_count < 16)
        image->library_starts[image->library_start_count++] = library->module_start;
    /* Include every loaded segment in the identity used by the AOT cache. */
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    unsigned length = 0;
    static const char domain[] = "ScarletNX Vita image identity v1";
    bool hashed = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
                  EVP_DigestUpdate(hash, domain, sizeof domain) == 1;
    for (unsigned i = 0; hashed && i < image->segment_count; ++i) {
        unsigned char header[12];
        snx_st_le32(header, image->segment[i].address);
        snx_st_le32(header + 4, image->segment[i].size);
        snx_st_le32(header + 8, image->segment[i].flags);
        hashed = EVP_DigestUpdate(hash, header, sizeof header) == 1 &&
                 EVP_DigestUpdate(hash, image->segment[i].bytes, image->segment[i].size) == 1;
    }
    hashed = hashed && EVP_DigestFinal_ex(hash, image->identity, &length) == 1 && length == sizeof image->identity;
    EVP_MD_CTX_free(hash);
    if (!hashed) { snprintf(error, error_size, "cannot hash loaded Vita modules"); return false; }
    return true;
}
void vita_image_free(VitaImage* image) {
    for (unsigned i = 0; i < image->segment_count; ++i) free(image->segment[i].bytes);
    free(image->imports);
    free(image->variables);
    free(image->seeds);
    free(image->exports);
    memset(image, 0, sizeof *image);
}