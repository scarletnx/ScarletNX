/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_link_macho, where no dyld can load what it writes: the dylib checked as macOS would. Segments on 16 KiB pages,
   in order, __LINKEDIT last; the rebases inside __DATA, onto addresses in the image; the export trie finding the
   entry in __text; the code's ADRP and its low 12 bits onto the symbols they name; the ad hoc signature last, with
   each 4 KiB page's SHA-256 as OpenSSL computes it. A dump of the output is written to argv[3] for llvm-objdump
   --macho. The same two parts as AArch64 ELF objects, and one of each, give dylibs that check the same: what the
   native back ends write, linked beside what LLVM does. Each dylib's code is run: as dyld loads it on Apple silicon,
   else under Unicorn, the image at an address of its own with its rebases applied. Then objects the linker refuses,
   each with words of the reason it must give.
   Usage: link_macho part1.o part2.o out.dylib elf1.o elf2.o elf_weak.o [refused.o reason]... */
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "common/cpu/recompiler/llvm/link.h"
#ifdef SNX_TEST_UNICORN
#include "support/host_unicorn.h"
#endif
#include <assert.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#endif

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const unsigned char* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static uint32_t be32(const unsigned char* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t uleb(const unsigned char** p) {
    uint64_t v = 0;
    unsigned shift = 0;
    do {
        v |= (uint64_t)(**p & 0x7F) << shift;
        shift += 7;
    } while (*(*p)++ & 0x80);
    return v;
}

static unsigned char* slurp(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    unsigned char* data;
    long n;
    assert(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char*)malloc((size_t)n);
    assert(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return data;
}

typedef struct {
    char name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
} Segment;

/* the trie's address of name, or 0 */
static uint64_t trie_find(const unsigned char* trie, size_t size, const char* name) {
    const unsigned char* node = trie;
    for (;;) {
        const unsigned char* p = node;
        uint64_t info = uleb(&p);
        unsigned children, i;
        int next = 0;
        if (!*name) {
            if (!info) return 0;
            assert(*p++ == 0); /* EXPORT_SYMBOL_FLAGS_KIND_REGULAR */
            return uleb(&p);
        }
        p += info;
        children = *p++;
        for (i = 0; i < children && !next; ++i) {
            size_t n = strlen((const char*)p);
            uint64_t offset;
            int match = !strncmp((const char*)p, name, n);
            p += n + 1;
            offset = uleb(&p);
            if (match) {
                assert(offset < size);
                name += n;
                node = trie + offset;
                next = 1;
            }
        }
        if (!next) return 0;
    }
}

/* 0 when the linker refuses inputs with an error that says reason; 1, saying why, otherwise */
static int refused(const SnxLinkInput* inputs, size_t count, const char* reason) {
    SnxLinkOutput out;
    const char* exports[] = {"test_entry"};
    char error[512];
    if (!snx_link_macho(inputs, count, exports, 1, "test.dylib", &out, error, sizeof(error))) {
        fprintf(stderr, "%s: linked, where it should fail with %s\n", inputs[0].name, reason);
        free(out.data);
        return 1;
    }
    if (!strstr(error, reason)) {
        fprintf(stderr, "%s: refused with \"%s\", not %s\n", inputs[0].name, error, reason);
        return 1;
    }
    return 0;
}

/* a copy of object, which the caller frees, with name's symbol made a local of type */
static unsigned char* retyped(const SnxLinkInput* object, const char* name, unsigned type) {
    unsigned char* copy = (unsigned char*)malloc(object->size);
    const unsigned char* c = object->data + 32;
    uint32_t i, j;
    assert(copy);
    memcpy(copy, object->data, object->size);
    for (i = 0; i < rd32(object->data + 16); ++i, c += rd32(c + 4))
        if (rd32(c) == 0x2) /* LC_SYMTAB */
            for (j = 0; j < rd32(c + 12); ++j) {
                unsigned char* s = copy + rd32(c + 8) + 16 * (size_t)j;
                if (!strcmp((const char*)copy + rd32(c + 16) + rd32(s), name)) {
                    s[4] = (unsigned char)type;
                    return copy;
                }
            }
    assert(!"no such symbol");
    return copy;
}

/* the linker's ELF side (elf_object.c) is not on Windows, whose objects are COFF */
#if !defined(_WIN32)
/* a copy of an ELF object, which the caller frees, with pad bytes of code before its own: its code's symbols moved, and
   no relocation (the part it is made of has none in its code) */
static unsigned char* stretched(const SnxLinkInput* object, uint64_t pad, size_t* size) {
    Elf64_Ehdr eh;
    unsigned char* copy;
    size_t at = (object->size + 15) & ~(size_t)15, k, j, text = 0, length = 0;
    Elf64_Shdr* sh;
    memcpy(&eh, object->data, sizeof(eh));
    for (k = 1; k < eh.e_shnum; ++k) {
        Elf64_Shdr h;
        memcpy(&h, object->data + eh.e_shoff + k * sizeof(h), sizeof(h));
        if (h.sh_type == SHT_PROGBITS && (h.sh_flags & SHF_EXECINSTR)) text = k, length = h.sh_size;
    }
    assert(text);
    *size = at + pad + length;
    copy = (unsigned char*)calloc(1, *size);
    assert(copy);
    memcpy(copy, object->data, object->size);
    sh = (Elf64_Shdr*)(void*)(copy + eh.e_shoff);
    memcpy(copy + at + pad, object->data + sh[text].sh_offset, length);
    sh[text].sh_offset = at;
    sh[text].sh_size = pad + length;
    for (k = 1; k < eh.e_shnum; ++k) {
        if (sh[k].sh_type == SHT_RELA) assert(sh[k].sh_info != text);
        if (sh[k].sh_type != SHT_SYMTAB) continue;
        Elf64_Sym* y = (Elf64_Sym*)(void*)(copy + sh[k].sh_offset);
        for (j = 1; j < sh[k].sh_size / sizeof(*y); ++j)
            if (y[j].st_shndx == text && ELF64_ST_TYPE(y[j].st_info) != STT_SECTION) y[j].st_value += pad;
    }
    return copy;
}

/* an ELF object's malformed copies the linker refuses: 1 for each it does not refuse so */
static int malformed(const SnxLinkInput* object, const SnxLinkInput* other, const char* name) {
    enum { COMMON, REL, NO_SYMBOL, ALIGNMENT, OUTSIDE, MISALIGNED, UNSUPPORTED, INDEX, CASES };
    static const char* const reasons[CASES] = {"common symbol _",
                                               "REL relocations in .",
                                               "a relocation of no symbol",
                                               "alignment is not a power of two",
                                               "relocation outside section",
                                               "relocation to a misaligned address",
                                               "unsupported relocation type 999",
                                               "bad symbol index"};
    int failed = 0, c;
    for (c = 0; c < CASES; ++c) {
        SnxLinkInput bad = *object;
        unsigned char* copy = (unsigned char*)malloc(object->size);
        Elf64_Ehdr eh;
        Elf64_Shdr* sh;
        size_t k, j, changed = 0;
        assert(copy);
        memcpy(copy, object->data, object->size);
        memcpy(&eh, copy, sizeof(eh));
        sh = (Elf64_Shdr*)(void*)(copy + eh.e_shoff);
        for (k = 1; k < eh.e_shnum && !changed; ++k) {
            if (c == COMMON && sh[k].sh_type == SHT_SYMTAB) {
                Elf64_Sym* y = (Elf64_Sym*)(void*)(copy + sh[k].sh_offset);
                for (j = 1; j < sh[k].sh_size / sizeof(*y) && !changed; ++j)
                    if (!strcmp((char*)copy + sh[sh[k].sh_link].sh_offset + y[j].st_name, name))
                        y[j].st_shndx = SHN_COMMON, changed = 1;
            } else if (c != COMMON && sh[k].sh_type == SHT_RELA && (sh[sh[k].sh_info].sh_flags & SHF_EXECINSTR)) {
                Elf64_Rela* r = (Elf64_Rela*)(void*)(copy + sh[k].sh_offset);
                if (c == REL) sh[k].sh_type = SHT_REL;
                if (c == NO_SYMBOL) r[0].r_info &= 0xFFFFFFFFu;
                if (c == ALIGNMENT) sh[sh[k].sh_info].sh_addralign = 3;
                if (c == OUTSIDE) r[0].r_offset = sh[sh[k].sh_info].sh_size;
                if (c == UNSUPPORTED) r[0].r_info = (r[0].r_info & ~(uint64_t)0xFFFFFFFFu) | 999;
                if (c == INDEX) r[0].r_info = (r[0].r_info & 0xFFFFFFFFu) | (uint64_t)0xFFFFFF << 32;
                for (j = 0; c == MISALIGNED && j < sh[k].sh_size / sizeof(*r); ++j)
                    if ((r[j].r_info & 0xFFFFFFFFu) == SNX_A64_LDST64_ABS_LO12_NC) r[j].r_addend += 1;
                changed = 1;
            }
        }
        assert(changed);
        bad.data = copy;
        const SnxLinkInput both[2] = {bad, *other};
        failed |= refused(both, 2, reasons[c]);
        free(copy);
    }
    return failed;
}
#endif

/* name's address in a symbol table, or 0 */
static uint64_t symbol_named(const unsigned char* symbols, uint32_t count, const char* strings, const char* name) {
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(strings + rd32(symbols + 16 * (size_t)i), name)) return rd64(symbols + 16 * (size_t)i + 8);
    return 0;
}

/* Each ARM64_RELOC_PAGE21 and PAGEOFF12 of object o's first section, its __text at address 0, read back from the dylib
   d, where that code starts at text_address: ADRP's page, and the low 12 bits an ADD adds or a load or store scales by
   its access's size (16 bytes for a 128-bit one), against the symbol's address there plus the ADDEND before it.
   Symbols the dylib does not name (temporary labels) are left out. Counts the 128-bit and the signed byte accesses */
static unsigned check_pages(const unsigned char* o, const unsigned char* d, uint64_t text_address,
                            const unsigned char* symbols, uint32_t count, const char* strings, unsigned* wide,
                            unsigned* signed_bytes) {
    const unsigned char *c = o + 32, *section = NULL, *object_symbols = NULL;
    const char* object_strings = NULL;
    uint32_t i, k;
    int64_t addend = 0;
    unsigned checked = 0;
    for (i = 0; i < rd32(o + 16); ++i, c += rd32(c + 4)) {
        if (rd32(c) == 0x19 && rd32(c + 64)) section = c + 72; /* LC_SEGMENT_64's first section */
        if (rd32(c) == 0x2) {                                  /* LC_SYMTAB */
            object_symbols = o + rd32(c + 8);
            object_strings = (const char*)o + rd32(c + 16);
        }
    }
    assert(section && object_symbols && !strcmp((const char*)section, "__text") && !rd64(section + 32));
    for (k = 0; k < rd32(section + 60); ++k) {
        const unsigned char* r = o + rd32(section + 56) + 8 * (size_t)k;
        uint32_t at = rd32(r), info = rd32(r + 4), type = info >> 28, insn = rd32(d + text_address + at);
        int64_t a = addend;
        uint64_t target;
        addend = 0;
        if (type == 10) { /* ARM64_RELOC_ADDEND, for the next one */
            addend = (int64_t)((info & 0xFFFFFF) ^ 0x800000) - 0x800000;
            continue;
        }
        if ((type != 3 && type != 4) || !(info >> 27 & 1)) continue; /* PAGE21 and PAGEOFF12, to a symbol */
        target = symbol_named(symbols, count, strings, object_strings + rd32(object_symbols + 16 * (info & 0xFFFFFF)));
        if (!target) continue;
        target += (uint64_t)a;
        if (type == 3) { /* ADRP: immlo at 29, immhi at 5, in pages */
            uint64_t pages = (insn >> 29 & 3) | (uint64_t)(insn >> 5 & 0x7FFFF) << 2;
            pages = (pages ^ 0x100000) - 0x100000;
            assert(((text_address + at) & ~(uint64_t)0xFFF) + (pages << 12) == (target & ~(uint64_t)0xFFF));
        } else {
            uint64_t offset = insn >> 10 & 0xFFF;
            if ((insn & 0x3B000000u) == 0x39000000u) { /* a load or store with an unsigned offset */
                unsigned scale = insn >> 30, vector = insn >> 26 & 1, opc1 = insn >> 23 & 1;
                if (!scale && opc1 && vector) {
                    scale = 4;
                    ++*wide;
                }
                if (!scale && opc1 && !vector) ++*signed_bytes;
                offset <<= scale;
            } else {
                assert((insn & 0x7FC00000u) == 0x11000000u); /* ADD (immediate), unshifted */
            }
            assert(offset == (target & 0xFFF));
        }
        ++checked;
    }
    return checked;
}

/* what test_entry gives: PART 1's sum, 'w' + 2 * 3 + 5 * 3 + 10 + 2 (missing is not there) + 3 + 2 */
#define EXPECTED 157u

/* 0 when the dylib's code gives what it should, run from entry: as dyld loads the file at path on Apple silicon,
   elsewhere under Unicorn where the test has it, the image of size bytes at pages of its own, the count pointers it
   rebases (addresses[]) moved with it; 1 with what it gave. 0, saying so, where neither is here */
static int run(const char* what, const char* path, const unsigned char* image, size_t size, uint64_t entry,
               const uint64_t* addresses, unsigned count) {
    uint64_t got = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    void* library = dlopen(path, RTLD_NOW);
    unsigned long long (*test_entry)(void);
    (void)image, (void)size, (void)entry, (void)addresses, (void)count;
    if (!library) {
        fprintf(stderr, "%s: dyld does not load it: %s\n", what, dlerror());
        return 1;
    }
    *(void**)&test_entry = dlsym(library, "test_entry");
    if (!test_entry) {
        fprintf(stderr, "%s: dlsym finds no test_entry: %s\n", what, dlerror());
        return 1;
    }
    got = test_entry();
    dlclose(library);
#elif defined(SNX_TEST_UNICORN)
    HostUc* u = hostuc_new(HOSTUC_ARM64, 1 << 16);
    unsigned char* pages;
    unsigned i;
    int rc;
    (void)path;
    assert(u);
    pages = (unsigned char*)hostuc_alloc(u, (size + 0xFFFF) & ~(size_t)0xFFFF);
    assert(pages);
    memcpy(pages, image, size);
    for (i = 0; i < count; ++i) {
        uint64_t value = rd64(pages + addresses[i]) + (uint64_t)(size_t)pages;
        memcpy(pages + addresses[i], &value, 8);
    }
    rc = hostuc_call(u, (uint64_t)(size_t)pages + entry, 0, 0, &got);
    hostuc_free(u);
    if (rc) {
        fprintf(stderr, "%s: Unicorn stopped\n", what);
        return 1;
    }
#else
    (void)path, (void)image, (void)size, (void)entry, (void)addresses, (void)count;
    printf("link_macho: %s: its code is not run here (no Unicorn, and no Apple silicon)\n", what);
    return 0;
#endif
    if (got == EXPECTED) return 0;
    fprintf(stderr, "%s: test_entry gives %llu, not %u\n", what, (unsigned long long)got, EXPECTED);
    return 1;
}

/* the dylib the two objects link into, checked as dyld would and run; written to path. page_object, where not NULL, is
   the first object, Mach-O, whose page relocations are read back (check_pages) */
static int check_dylib(const char* what, const SnxLinkInput* inputs, size_t count, const char* path,
                       const unsigned char* page_object, int by_first_input) {
    SnxLinkOutput out;
    Segment segments[4], *text = NULL, *data = NULL, *linkedit = NULL;
    const char* exports[] = {"test_entry"};
    char error[512];
    const unsigned char *d, *c, *trie = NULL, *rebase = NULL, *p, *end, *symbols = NULL;
    const char* strings = NULL;
    uint64_t text_address = 0, text_size = 0, entry, address = 0, symbol_value = 0, rebased[64];
    uint32_t ncmds, i, nsegments = 0, trie_size = 0, rebase_size = 0, signature_offset = 0, signature_size = 0,
                    nslots, k;
    uint32_t nsymbols = 0;
    unsigned rebases = 0, pages = 0, wide = 0, signed_bytes = 0;
    int failed;
    FILE* f;
    if (by_first_input ? snx_link(inputs, count, exports, 1, "test.dylib", &out, error, sizeof(error))
                       : snx_link_macho(inputs, count, exports, 1, "test.dylib", &out, error, sizeof(error))) {
        fprintf(stderr, "%s: link: %s\n", what, error);
        return 1;
    }
    /* a new file: macOS keeps a signed image by its inode, and kills a program that opens one written over */
    remove(path);
    f = fopen(path, "wb");
    assert(f && fwrite(out.data, 1, out.size, f) == out.size);
    fclose(f);
    d = out.data;
    assert(rd32(d) == 0xFEEDFACFu && rd32(d + 4) == 0x0100000Cu && rd32(d + 12) == 6); /* arm64 MH_DYLIB */
    ncmds = rd32(d + 16);
    c = d + 32;
    for (i = 0; i < ncmds; ++i, c += rd32(c + 4)) {
        uint32_t cmd = rd32(c);
        if (cmd == 0x19) { /* LC_SEGMENT_64 */
            Segment* s = &segments[nsegments++];
            assert(nsegments <= 4);
            memcpy(s->name, c + 8, 16);
            s->name[16] = 0;
            s->vmaddr = rd64(c + 24);
            s->vmsize = rd64(c + 32);
            s->fileoff = rd64(c + 40);
            s->filesize = rd64(c + 48);
            if (!strcmp(s->name, "__TEXT")) {
                text = s;
                assert(!strcmp((const char*)c + 72, "__text"));
                text_address = rd64(c + 72 + 32);
                text_size = rd64(c + 72 + 40);
            }
            if (!strcmp(s->name, "__DATA")) data = s;
            if (!strcmp(s->name, "__LINKEDIT")) linkedit = s;
        } else if (cmd == 0x80000022u) { /* LC_DYLD_INFO_ONLY */
            rebase = d + rd32(c + 8);
            rebase_size = rd32(c + 12);
            assert(!rd32(c + 20) && !rd32(c + 28) && !rd32(c + 36)); /* no binds */
            trie = d + rd32(c + 40);
            trie_size = rd32(c + 44);
        } else if (cmd == 0x2) { /* LC_SYMTAB: the export, last and external */
            const unsigned char* last = d + rd32(c + 8) + 16 * (size_t)(rd32(c + 12) - 1);
            assert(!strcmp((const char*)d + rd32(c + 16) + rd32(last), "_test_entry") && last[4] == 0xF);
            symbol_value = rd64(last + 8);
            symbols = d + rd32(c + 8);
            nsymbols = rd32(c + 12);
            strings = (const char*)d + rd32(c + 16);
        } else if (cmd == 0x1D) { /* LC_CODE_SIGNATURE */
            signature_offset = rd32(c + 8);
            signature_size = rd32(c + 12);
        }
    }
    /* segments: __TEXT from the start of the file, every one on pages, in order, __LINKEDIT to the end of the file */
    assert(nsegments == 3 && text == &segments[0] && data == &segments[1] && linkedit == &segments[2]);
    assert(!text->fileoff && !text->vmaddr && 32 + rd32(d + 20) <= text_address);
    for (i = 0; i < nsegments; ++i) {
        assert(!(segments[i].vmaddr % 0x4000) && !(segments[i].fileoff % 0x4000) && !(segments[i].vmsize % 0x4000));
        assert(segments[i].filesize <= segments[i].vmsize);
        if (i) assert(segments[i].vmaddr >= segments[i - 1].vmaddr + segments[i - 1].vmsize);
    }
    assert(linkedit->fileoff + linkedit->filesize == out.size);
    /* rebases: pointers in __DATA to addresses in the image (the GOT's, the tables') */
    assert(rebase && rebase_size);
    for (p = rebase, end = rebase + rebase_size; p < end && *p;) {
        unsigned op = *p & 0xF0, imm = *p++ & 0x0F;
        uint64_t n;
        switch (op) {
            case 0x10: assert(imm == 1); break; /* SET_TYPE_IMM, pointers */
            case 0x20: /* SET_SEGMENT_AND_OFFSET_ULEB, in __DATA */
                assert(imm == 1);
                address = data->vmaddr + uleb(&p);
                break;
            case 0x30: address += uleb(&p); break;
            case 0x60:
                for (n = uleb(&p); n--; address += 8, ++rebases) {
                    assert(rebases < sizeof(rebased) / sizeof(*rebased));
                    rebased[rebases] = address;
                    uint64_t target = rd64(d + data->fileoff + (address - data->vmaddr));
                    assert(address >= data->vmaddr && address + 8 <= data->vmaddr + data->filesize);
                    assert(target < data->vmaddr + data->vmsize);
                }
                break;
            default: assert(!"unexpected rebase opcode");
        }
    }
    assert(rebases >= 5); /* names, fns, and the GOT's counter */
    /* the export */
    entry = trie_find(trie, trie_size, "_test_entry");
    assert(entry && entry == symbol_value && entry >= text_address && entry < text_address + text_size);
    assert(!trie_find(trie, trie_size, "_helper") && !trie_find(trie, trie_size, "test_entry"));
    /* the code addresses what its relocations name, a 128-bit load and a signed byte one among them */
    assert(symbols);
    if (page_object) {
        pages = check_pages(page_object, d, text_address, symbols, nsymbols, strings, &wide, &signed_bytes);
        assert(pages >= 10 && wide && signed_bytes);
    }
    for (i = 0; i < nsymbols; ++i) {
        const char* name = strings + rd32(symbols + 16 * (size_t)i);
        if (name[0] != '_' || name[1] == '$' || name[1] == '.' || name[1] == '<' || !name[1]) {
            fprintf(stderr, "%s: a symbol named %s\n", what, name);
            free(out.data);
            return 1;
        }
    }
    assert(symbol_named(symbols, nsymbols, strings, "_second_vector"));
    /* the signature: the last thing in the file, hashing every page before it */
    assert(signature_offset && signature_offset + signature_size == out.size && !(signature_offset % 16));
    p = d + signature_offset;
    assert(be32(p) == 0xFADE0CC0u && be32(p + 4) == signature_size && be32(p + 8) == 1 && be32(p + 12) == 0);
    c = p + be32(p + 16);
    assert(be32(c) == 0xFADE0C02u && be32(c + 8) >= 0x20400 && (be32(c + 12) & 0x20002) == 0x20002);
    assert(be32(c + 32) == signature_offset && c[36] == 32 && c[37] == 2 && c[39] == 12);
    assert(!strcmp((const char*)c + be32(c + 20), "test.dylib"));
    nslots = be32(c + 28);
    assert(nslots == (signature_offset + 4095) / 4096);
    for (k = 0; k < nslots; ++k) {
        unsigned char digest[32];
        unsigned length = 0;
        size_t at = (size_t)k * 4096, n = signature_offset - at < 4096 ? signature_offset - at : 4096;
        assert(EVP_Digest(d + at, n, digest, &length, EVP_sha256(), NULL) && length == 32);
        assert(!memcmp(c + be32(c + 16) + 32 * (size_t)k, digest, 32));
    }
    failed = run(what, path, d, (size_t)(data->vmaddr + data->vmsize), entry, rebased, rebases);
    printf("link_macho: %s: %lu bytes, %u rebases, %u page relocations, entry at 0x%llx, %u pages signed\n", what,
           (unsigned long)out.size, rebases, pages, (unsigned long long)entry, nslots);
    free(out.data);
    return failed;
}

#if !defined(_WIN32)
/* the ELF objects argv[4] to argv[6] name, alone and beside the Mach-O ones: 1 where one of the cases fails */
static int check_elf(const SnxLinkInput* macho, char** argv) {
    SnxLinkInput elf[2];
    SnxLinkOutput out;
    const char* exports[] = {"helper"};
    char error[512];
    int failed = 0, i;
    for (i = 0; i < 2; ++i) {
        elf[i].data = slurp(argv[4 + i], &elf[i].size);
        elf[i].name = argv[4 + i];
    }
    /* another machine's object, and one definition of a name in an object of each format */
    {
        SnxLinkInput other = elf[0];
        unsigned char* copy = (unsigned char*)malloc(elf[0].size);
        const SnxLinkInput twice[2] = {elf[1], macho[1]};
        assert(copy);
        memcpy(copy, elf[0].data, elf[0].size);
        copy[18] = 62; /* e_machine: EM_X86_64 */
        other.data = copy;
        failed |= refused(&other, 1, "another machine's object");
        failed |= refused(twice, 2, "duplicate symbol _counter");
        free(copy);
        failed |= malformed(&elf[0], &elf[1], "test_entry");
    }
    /* the entry's own object alone leaves helper and counter undefined */
    failed |= refused(&elf[0], 1, "undefined symbol _");
    {
        const SnxLinkInput first_elf[2] = {elf[0], macho[1]}, first_macho[2] = {macho[0], elf[1]};
        failed |= check_dylib("ELF", elf, 2, argv[3], NULL, 0);
        failed |= check_dylib("ELF and Mach-O", first_elf, 2, argv[3], NULL, 0);
        failed |= check_dylib("Mach-O and ELF", first_macho, 2, argv[3], macho[0].data, 1);
    }
    /* a weak definition of helper beside PART 2's, whichever comes first */
    {
        SnxLinkInput weak;
        weak.data = slurp(argv[6], &weak.size);
        weak.name = argv[6];
        const SnxLinkInput before[3] = {elf[0], weak, elf[1]}, after[3] = {elf[0], elf[1], weak};
        failed |= check_dylib("ELF, the weak helper first", before, 3, argv[3], NULL, 0);
        failed |= check_dylib("ELF, the weak helper last", after, 3, argv[3], NULL, 0);
        free((void*)weak.data);
    }
    /* helper beyond a branch's 128 MiB: called through its caller's veneer */
    {
        SnxLinkInput far_elf[2] = {elf[0], elf[1]};
        far_elf[1].data = stretched(&elf[1], (uint64_t)130 << 20, &far_elf[1].size);
        failed |= check_dylib("ELF beyond a branch's reach", far_elf, 2, argv[3], NULL, 0);
        free((void*)far_elf[1].data);
    }
    /* which linker the first object's format picks: ELF's on Apple is this one's, elsewhere the ELF linker's */
    if (snx_link(&elf[1], 1, exports, 1, "test.dylib", &out, error, sizeof(error)))
        fprintf(stderr, "link of %s: %s\n", argv[5], error), failed = 1;
    else {
#ifdef __APPLE__
        const uint32_t magic = 0xFEEDFACFu;
#else
        const uint32_t magic = 0x464C457Fu; /* "\177ELF" */
#endif
        if (rd32(out.data) != magic) fprintf(stderr, "link of %s: not this host's module\n", argv[5]), failed = 1;
        free(out.data);
    }
    for (i = 0; i < 2; ++i) free((void*)elf[i].data);
    return failed;
}
#endif

int main(int argc, char** argv) {
    SnxLinkInput macho[2];
    int failed = 0, i;
    assert(argc >= 7 && argc % 2 == 1);
    for (i = 7; i < argc; i += 2) {
        SnxLinkInput input;
        input.data = slurp(argv[i], &input.size);
        input.name = argv[i];
        failed |= refused(&input, 1, argv[i + 1]);
        free((void*)input.data);
    }
    for (i = 0; i < 2; ++i) {
        macho[i].data = slurp(argv[1 + i], &macho[i].size);
        macho[i].name = argv[1 + i];
    }
    /* code referring to a symbol of a type the linker does not take: scale made a local N_PBUD */
    {
        SnxLinkInput retyped_inputs[2] = {macho[0], macho[1]};
        retyped_inputs[0].data = retyped(&macho[0], "_scale", 0xC);
        failed |= refused(retyped_inputs, 2, "unsupported symbol type of _scale");
        free((void*)retyped_inputs[0].data);
    }
    /* the entry's own object alone leaves helper and counter undefined */
    failed |= refused(&macho[0], 1, "undefined");
    failed |= check_dylib("Mach-O", macho, 2, argv[3], macho[0].data, 1);
#if !defined(_WIN32)
    failed |= check_elf(macho, argv);
#endif
    for (i = 0; i < 2; ++i) free((void*)macho[i].data);
    return failed;
}
