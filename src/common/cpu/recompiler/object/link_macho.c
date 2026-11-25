/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Mach-O: arm64 objects into an MH_DYLIB for macOS and iOS, without imports, initializers or unwind info. The objects
   are Mach-O, as LLVM writes them, or AArch64 ELF, as the native back ends and the runtime library's embedded object
   are: what the JIT loader takes on Darwin (code_loader.c), read and relocated by the same elf_object.c, their code
   Darwin's whatever the container. A module may have both.
   Segments: [__TEXT: headers, __text, __const (every read-only input section)] [__DATA: __data, __got, __bss]
   [__LINKEDIT: rebase opcodes, the export trie, the symbol table, the code signature]. Every symbol must be defined by
   one of the objects; the only fixups dyld applies are rebases, for absolute addresses in __DATA and the GOT's slots.
   Apple silicon runs no unsigned code, so the dylib carries the ad hoc signature ld64 and lld give it (linker-signed:
   a SHA-256 of each 4 KiB page of the file before it). __compact_unwind and __eh_frame are dropped, as the ELF module
   has no PT_GNU_EH_FRAME. Symbols are named in the Mach-O objects with C's leading underscore, so an export
   "snx_module" is "_snx_module" here, as dlsym looks it up. An ELF object's have none: its "snx_module" is spelled
   "_snx_module" too.
   Written from the formats <mach-o/loader.h>, <mach-o/nlist.h> and <mach-o/arm64/reloc.h> document, and the code
   signature's (<kern/cs_blobs.h>). The structures are read and written byte by byte, so this builds anywhere */
#include "common/cpu/recompiler/object/elf_object.h"
#include "common/cpu/recompiler/object/link_internal.h"
#include "common/utilities/bits.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MH_MAGIC_64      0xFEEDFACFu
#define MH_OBJECT        1
#define MH_DYLIB         6
#define CPU_TYPE_ARM64   0x0100000Cu
#define LC_SYMTAB        0x2
#define LC_DYSYMTAB      0xB
#define LC_LOAD_DYLIB    0xC
#define LC_ID_DYLIB      0xD
#define LC_SEGMENT_64    0x19
#define LC_UUID          0x1B
#define LC_CODE_SIGNATURE 0x1D
#define LC_VERSION_MIN_MACOSX 0x24
#define LC_VERSION_MIN_IPHONEOS 0x25
#define LC_BUILD_VERSION 0x32
#define LC_DYLD_INFO_ONLY 0x80000022u
#define PLATFORM_MACOS   1
#define PLATFORM_IOS     2
/* section types and attributes */
#define SECTION_TYPE     0xFFu
#define S_ZEROFILL       0x1
#define S_MOD_INIT_FUNC_POINTERS 0x9
#define S_MOD_TERM_FUNC_POINTERS 0xA
#define S_GB_ZEROFILL    0xC
#define S_THREAD_LOCAL_REGULAR 0x11
#define S_THREAD_LOCAL_INIT_FUNCTION_POINTERS 0x15
#define S_INIT_FUNC_OFFSETS 0x16
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_DEBUG     0x02000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
/* nlist */
#define N_STAB           0xE0
#define N_PEXT           0x10
#define N_TYPE           0x0E
#define N_EXT            0x01
#define N_UNDF           0x0
#define N_ABS            0x2
#define N_SECT           0xE
#define N_WEAK_REF       0x0040
#define N_WEAK_DEF       0x0080
/* ARM64 relocations */
enum {
    R_UNSIGNED,
    R_SUBTRACTOR,
    R_BRANCH26,
    R_PAGE21,
    R_PAGEOFF12,
    R_GOT_LOAD_PAGE21,
    R_GOT_LOAD_PAGEOFF12,
    R_POINTER_TO_GOT,
    R_TLVP_LOAD_PAGE21,
    R_TLVP_LOAD_PAGEOFF12,
    R_ADDEND
};
/* Apple silicon's pages: segments start and end on them. The code signature hashes 4 KiB ones */
#define PAGE           0x4000u
#define SIGNATURE_PAGE 0x1000u
/* a branch out of B's reach (128 MiB) goes through a veneer, ADRP x16 + ADD x16 + BR x16, as in link_elf.c */
#define VENEER      12
#define VENEER_TEXT (120ull << 20)

/* the input sections' kinds, and the output sections in their order (n_sect counts them from 1) */
enum { K_TEXT, K_CONST, K_DATA, K_BSS, K_COUNT };
enum { O_NONE, O_TEXT, O_CONST, O_DATA, O_GOT, O_BSS, O_COUNT };
static const int kind_section[K_COUNT] = {O_TEXT, O_CONST, O_DATA, O_BSS};

/* SHA-256 (FIPS 180-4), for the code signature's page hashes and the UUID */
typedef struct {
    uint32_t h[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} Sha256;
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }
static void sha256_block(Sha256* s, const unsigned char* p) {
    uint32_t w[64], v[8];
    unsigned i;
    for (i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; ++i)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(v, s->h, sizeof(v));
    for (i = 0; i < 64; ++i) {
        uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) +
            sha256_k[i] +
             w[i];
        uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof(*v));
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (i = 0; i < 8; ++i) s->h[i] += v[i];
}
static void sha256(const unsigned char* data, size_t size, unsigned char digest[32]) {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Sha256 s;
    memcpy(s.h, initial, sizeof(initial));
    s.length = (uint64_t)size * 8;
    for (; size >= 64; data += 64, size -= 64) sha256_block(&s, data);
    memset(s.block, 0, sizeof(s.block));
    memcpy(s.block, data, size);
    s.block[size] = 0x80;
    if (size >= 56) {
        sha256_block(&s, s.block);
        memset(s.block, 0, sizeof(s.block));
    }
    snx_st_be64(s.block + 56, s.length);
    sha256_block(&s, s.block);
    for (unsigned i = 0; i < 8; ++i) snx_st_be32(digest + 4 * i, s.h[i]);
}

typedef struct {
    char name[17], segment[17];
    uint64_t addr, size; /* addr: in the object's own address space, which its symbols' values are in */
    uint32_t offset, align, reloff, nreloc, flags;
    int kind;          /* K_*, or -1 when not loaded */
    uint64_t position; /* offset inside its kind */
    uint64_t address;  /* final */
} Sec;

typedef struct {
    const SnxLinkInput* input;
    const unsigned char* symbols; /* nlist_64, 16 bytes each */
    uint32_t nsymbols, nsections;
    const char* strings;
    uint32_t strings_size;
    Sec* sections;  /* from 1, as n_sect and a section relocation's r_symbolnum count them, and an ELF st_shndx */
    size_t veneers; /* its veneers, after its code at island (an offset in the code) */
    uint64_t island;
#if !defined(_WIN32)
    SnxElfObject elf; /* an ELF object (is_elf): its symbols and relocations are elf_object.c's to read */
#endif
    int is_elf;
} Obj;

typedef struct {
    Obj* objs;
    size_t nobjs;
    SnxLinkNames globals; /* name -> (object << 32 | symbol index) */
    SnxLinkNames got;     /* got_key -> GOT slot */
    SnxLinkNames veneer;  /* "caller:object:symbol" -> the caller's veneer to that definition */
    uint32_t platform, minos, sdk;
    uint64_t kind_size[K_COUNT], kind_align[K_COUNT], kind_address[K_COUNT];
    uint64_t got_address;
    size_t got_count;
    int far; /* the code may be out of a branch's reach */
    char* name; /* an ELF symbol's Mach-O spelling (spelled), the longest's room */
    size_t name_size;
    char* error;
    size_t error_size;
} Link;

/* a symbol of an object of either format, without its name (spelled) */
typedef struct {
    int external, undefined, absolute, defined; /* defined: in a section the module loads */
    int weak_def, weak_ref, unsupported;        /* unsupported: of a type the linker takes in no form */
    unsigned section;                           /* of a section's symbol, Obj.sections' */
    uint64_t value;                             /* its address in the object, or an absolute symbol's */
} Symbol;

static int fail(Link* l, const char* format, const char* a, const char* b) {
    return snx_link_fail(l->error, l->error_size, format, a, b);
}

static const unsigned char* symbol(const Obj* o, uint32_t index) { return o->symbols + 16 * (size_t)index; }
static const char* symbol_name(const Obj* o, const unsigned char* s) {
    uint32_t offset = snx_ld_le32(s);
    return offset < o->strings_size ? o->strings + offset : "";
}
/* the symbol's name as Mach-O has it: an ELF object's is C's own, spelled with an underscore in l->name, which the next
   ELF symbol's takes */
static const char* spelled(Link* l, const Obj* o, uint32_t index) {
#if !defined(_WIN32)
    if (o->is_elf) {
        const char* name = snx_elf_symbol_name(&o->elf, &o->elf.syms[index]);
        size_t n = strlen(name) + 1;
        l->name[0] = '_';
        memcpy(l->name + 1, name, n);
        return l->name;
    }
#endif
    (void)l;
    return symbol_name(o, symbol(o, index));
}
static Symbol symbol_at(const Obj* o, uint32_t index) {
    Symbol y = {0};
#if !defined(_WIN32)
    if (o->is_elf) {
        const Elf64_Sym* s = &o->elf.syms[index];
        y.external = ELF64_ST_BIND(s->st_info) == STB_GLOBAL || ELF64_ST_BIND(s->st_info) == STB_WEAK;
        y.undefined = s->st_shndx == SHN_UNDEF;
        y.absolute = s->st_shndx == SHN_ABS;
        y.unsupported = s->st_shndx == SHN_COMMON;
        y.weak_def = !y.undefined && ELF64_ST_BIND(s->st_info) == STB_WEAK;
        y.weak_ref = y.undefined && ELF64_ST_BIND(s->st_info) == STB_WEAK;
        y.section = y.undefined || y.absolute || y.unsupported ? 0 : s->st_shndx;
        y.value = s->st_value;
        y.defined = y.section && o->sections[y.section].kind >= 0;
        return y;
    }
#endif
    const unsigned char* s = symbol(o, index);
    unsigned type = s[4] & N_TYPE;
    y.external = s[4] & N_EXT;
    y.undefined = type == N_UNDF;
    y.absolute = type == N_ABS;
    y.unsupported = !y.undefined && !y.absolute && type != N_SECT;
    y.weak_def = snx_ld_le16(s + 6) & N_WEAK_DEF;
    y.weak_ref = snx_ld_le16(s + 6) & N_WEAK_REF;
    y.section = type == N_SECT ? s[5] : 0;
    y.value = snx_ld_le64(s + 8);
    y.defined = y.section && y.section <= o->nsections && o->sections[y.section].kind >= 0;
    return y;
}

/* the platform and oldest version the objects were compiled for (LLVM writes them from the triple) */
static int version(Link* l, const char* name, uint32_t platform, uint32_t minos, uint32_t sdk) {
    if (l->platform && l->platform != platform) return fail(l, "%s: an object for another Apple platform%s", name, "");
    l->platform = platform;
    if (minos > l->minos) l->minos = minos;
    if (sdk > l->sdk) l->sdk = sdk;
    return 0;
}

/* K_*, -1 for a section the module leaves out, or -2 with the error */
static int classify(Link* l, const Obj* o, const Sec* s) {
    uint32_t type = s->flags & SECTION_TYPE;
    if ((s->flags & S_ATTR_DEBUG) || !strcmp(s->segment, "__LD") || !strcmp(s->segment, "__DWARF") ||
        !strcmp(s->segment, "__LLVM") || !strcmp(s->name, "__llvm_addrsig") || !strcmp(s->name, "__eh_frame"))
        return -1;
    if (type >= S_THREAD_LOCAL_REGULAR && type <= S_THREAD_LOCAL_INIT_FUNCTION_POINTERS)
        return fail(l, "%s: thread-local data is not supported%s", o->input->name, ""), -2;
    if (type == S_MOD_INIT_FUNC_POINTERS || type == S_MOD_TERM_FUNC_POINTERS || type == S_INIT_FUNC_OFFSETS)
        return fail(l, "%s: constructors are not supported%s", o->input->name, ""), -2;
    if (type == S_ZEROFILL || type == S_GB_ZEROFILL) return K_BSS;
    if (s->flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) return K_TEXT;
    if (!strcmp(s->segment, "__TEXT")) return K_CONST;
    if (!strcmp(s->segment, "__DATA") || !strcmp(s->segment, "__DATA_CONST")) return K_DATA;
    return fail(l, "%s: unsupported section in segment %s", o->input->name, s->segment), -2;
}

#if !defined(_WIN32)
/* An AArch64 ELF object: its sections as Sec's, numbered as its st_shndx and sh_info number them, with elf_object.c's
   kinds in this module's segments. Its unwind tables go, as a Mach-O object's __eh_frame does */
static int parse_elf(Link* l, Obj* o, const SnxLinkInput* input) {
    static const int kinds[SNX_ELF_KINDS] = {
        [SNX_ELF_RODATA] = K_CONST, [SNX_ELF_TEXT] = K_TEXT, [SNX_ELF_DATA] = K_DATA, [SNX_ELF_BSS] = K_BSS};
    unsigned machine = EM_AARCH64, abi = 0;
    const Elf64_Ehdr* eh;
    if (input->size > 0xFFFFFFFFu) return fail(l, "%s: an object larger than Mach-O's 4 GiB%s", input->name, "");
    o->is_elf = 1;
    if (snx_elf_parse(&o->elf, input, &machine, &abi, l->error, l->error_size)) return -1;
    eh = o->elf.eh;
    o->sections = (Sec*)calloc(eh->e_shnum ? eh->e_shnum : 1, sizeof(Sec));
    if (!o->sections) return fail(l, "out of memory%s%s", "", "");
    o->sections[0].kind = -1;
    o->nsections = eh->e_shnum ? eh->e_shnum - 1u : 0;
    for (uint32_t j = 1; j <= o->nsections; ++j) {
        const Elf64_Shdr* h = &o->elf.sh[j];
        const char* name = snx_elf_section_name(&o->elf, j);
        Sec* s = &o->sections[j];
        uint64_t align = h->sh_addralign ? h->sh_addralign : 1;
        s->kind = o->elf.kind[j] < 0 || !strcmp(name, ".eh_frame") ? -1 : kinds[o->elf.kind[j]];
        if (s->kind < 0) continue;
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->size = h->sh_size;
        s->offset = (uint32_t)h->sh_offset;
        while (((uint64_t)1 << s->align) < align && s->align < 63) ++s->align;
        if (((uint64_t)1 << s->align) != align)
            return fail(l, "%s: section %s's alignment is not a power of two", input->name, s->name);
        if (s->align > 14) /* PAGE, which is all a slid image keeps */
            return fail(l, "%s: section %s aligned beyond a page", input->name, s->name);
    }
    for (size_t j = 0; j < o->elf.nsyms; ++j) {
        size_t n = strlen(snx_elf_symbol_name(&o->elf, &o->elf.syms[j])) + 2; /* the underscore, the NUL */
        if (n > l->name_size) {
            char* grown = (char*)realloc(l->name, n);
            if (!grown) return fail(l, "out of memory%s%s", "", "");
            l->name = grown;
            l->name_size = n;
        }
    }
    o->nsymbols = (uint32_t)o->elf.nsyms;
    return 0;
}
#endif

static int parse(Link* l, Obj* o, const SnxLinkInput* input) {
    const unsigned char* d = input->data;
    uint32_t at = 32;
    o->input = input;
    if (input->size >= 4 && !memcmp(d, "\177ELF", 4)) {
#if !defined(_WIN32)
        return parse_elf(l, o, input);
#else
        return fail(l, "%s: an ELF object, which this host's recompiler does not read%s", input->name, "");
#endif
    }
    if (input->size < 32 || snx_ld_le32(d) != MH_MAGIC_64 || snx_ld_le32(d + 12) != MH_OBJECT)
        return fail(l, "%s: not a 64-bit Mach-O object%s", input->name, "");
    if (snx_ld_le32(d + 4) != CPU_TYPE_ARM64)
        return fail(l, "%s: not an arm64 object (Mach-O modules are linked for Apple silicon)%s", input->name, "");
    uint32_t ncmds = snx_ld_le32(d + 16);
    if (32 + (uint64_t)snx_ld_le32(d + 20) > input->size)
        return fail(l, "%s: truncated Mach-O object%s", input->name, "");
    uint32_t end = 32 + snx_ld_le32(d + 20);
    for (uint32_t i = 0; i < ncmds; ++i) {
        const unsigned char* c = d + at;
        uint32_t size;
        if (at + 8 > end || (size = snx_ld_le32(c + 4)) < 8 || size > end - at)
            return fail(l, "%s: truncated load commands%s", input->name, "");
        uint32_t cmd = snx_ld_le32(c);
        if (cmd == LC_SEGMENT_64) {
            uint32_t n = size >= 72 ? snx_ld_le32(c + 64) : 0;
            if (size < 72 || 72 + 80 * (uint64_t)n > size)
                return fail(l, "%s: truncated segment command%s", input->name, "");
            Sec* grown = (Sec*)realloc(o->sections, (o->nsections + n + 1) * sizeof(Sec));
            if (!grown) return fail(l, "out of memory%s%s", "", "");
            o->sections = grown;
            if (!o->nsections) o->sections[0] = (Sec){0};
            for (uint32_t j = 0; j < n; ++j) {
                const unsigned char* h = c + 72 + 80 * (size_t)j;
                Sec* s = &o->sections[++o->nsections];
                *s = (Sec){0};
                memcpy(s->name, h, 16);
                memcpy(s->segment, h + 16, 16);
                s->addr = snx_ld_le64(h + 32);
                s->size = snx_ld_le64(h + 40);
                s->offset = snx_ld_le32(h + 48);
                s->align = snx_ld_le32(h + 52);
                s->reloff = snx_ld_le32(h + 56);
                s->nreloc = snx_ld_le32(h + 60);
                s->flags = snx_ld_le32(h + 64);
                if (s->align > 14) /* PAGE, which is all a slid image keeps */
                    return fail(l, "%s: section %s aligned beyond a page", input->name, s->name);
                int kind = classify(l, o, s);
                if (kind == -2) return -1;
                s->kind = kind;
                if (kind >= 0 && kind != K_BSS && s->offset + s->size > input->size)
                    return fail(l, "%s: truncated section %s", input->name, s->name);
                if (kind >= 0 && s->reloff + 8 * (uint64_t)s->nreloc > input->size)
                    return fail(l, "%s: truncated relocations in %s", input->name, s->name);
            }
        } else if (cmd == LC_SYMTAB && size >= 24) {
            uint32_t symoff = snx_ld_le32(c + 8), stroff = snx_ld_le32(c + 16);
            o->nsymbols = snx_ld_le32(c + 12);
            o->strings_size = snx_ld_le32(c + 20);
            if (symoff + 16 * (uint64_t)o->nsymbols > input->size || stroff + (uint64_t)o->strings_size > input->size)
                return fail(l, "%s: truncated symbol table%s", input->name, "");
            o->symbols = d + symoff;
            o->strings = (const char*)d + stroff;
        } else if (cmd == LC_BUILD_VERSION && size >= 24) {
            if (version(l, input->name, snx_ld_le32(c + 8), snx_ld_le32(c + 12), snx_ld_le32(c + 16))) return -1;
        } else if ((cmd == LC_VERSION_MIN_MACOSX || cmd == LC_VERSION_MIN_IPHONEOS) && size >= 16) {
            if (version(l, input->name, cmd == LC_VERSION_MIN_MACOSX ? PLATFORM_MACOS : PLATFORM_IOS,
                        snx_ld_le32(c + 8),
                        snx_ld_le32(c + 12)))
                return -1;
        }
        at += size;
    }
    if (!o->symbols && o->nsections) return fail(l, "%s: no symbol table%s", input->name, "");
    if (o->strings_size && o->strings[o->strings_size - 1])
        return fail(l, "%s: unterminated string table%s", input->name, "");
    return 0;
}

/* a symbol that says nothing of the code: a Mach-O debugger's, an ELF null symbol's */
static int no_symbol(const Obj* o, uint32_t index) { return o->is_elf ? !index : (symbol(o, index)[4] & N_STAB) != 0; }

/* a symbol the module's table leaves out: one without a name, a temporary's (L, l; .L in ELF), and in an ELF object
   what is neither a function nor an object, as link_elf.c: sections', files', AArch64's mapping symbols ($x, $d) */
static int unnamed(const Obj* o, uint32_t index) {
    const char* name;
#if !defined(_WIN32)
    if (o->is_elf) {
        const Elf64_Sym* s = &o->elf.syms[index];
        int type = ELF64_ST_TYPE(s->st_info);
        name = snx_elf_symbol_name(&o->elf, s);
        return (type != STT_FUNC && type != STT_OBJECT) || !*name || name[0] == '.';
    }
#endif
    name = symbol_name(o, symbol(o, index));
    return !*name || name[0] == 'L' || name[0] == 'l';
}

static int collect_globals(Link* l) {
    size_t found;
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; ++j) {
            const Symbol y = symbol_at(o, j);
            if (no_symbol(o, j) || !y.external) continue;
            if (y.undefined) {
                if (!o->is_elf && y.value) return fail(l, "%s: common symbol %s", o->input->name, spelled(l, o, j));
                continue;
            }
            if (y.unsupported)
                return fail(l, o->is_elf ? "%s: common symbol %s" : "%s: unsupported symbol type of %s", o->input->name,
                            spelled(l, o, j));
            if (!y.absolute && !y.defined) continue;
            const char* name = spelled(l, o, j);
            if (snx_link_find(&l->globals, name, &found)) {
                if (y.weak_def) continue;
                if (!symbol_at(&l->objs[found >> 32], (uint32_t)found).weak_def)
                    return fail(l, "duplicate symbol %s (%s)", name, o->input->name);
            }
            snx_link_insert(&l->globals, name, (i << 32) | j);
        }
    }
    return 0;
}

/* The defining object and symbol of object o's symbol index (an external one: the definition every object uses);
   *undefined_weak for an unresolved weak reference */
static int definition(Link* l, size_t object, uint32_t index, size_t* def_object, uint32_t* def_index,
                      int* undefined_weak) {
    const Obj* o = &l->objs[object];
    size_t found;
    *undefined_weak = 0;
    if (index >= o->nsymbols) return fail(l, "%s: bad symbol index%s", o->input->name, "");
    const Symbol y = symbol_at(o, index);
    if (y.external && snx_link_find(&l->globals, spelled(l, o, index), &found)) {
        *def_object = found >> 32;
        *def_index = (uint32_t)found;
        return 0;
    }
    if (!y.undefined) {
        if (y.unsupported) /* symbol_address reads a section's or the value */
            return fail(l, "%s: unsupported symbol type of %s", o->input->name, spelled(l, o, index));
        if (!y.absolute && !y.defined)
            return fail(l, "%s: reference to %s in a section the module leaves out", o->input->name,
                        spelled(l, o, index));
        *def_object = object;
        *def_index = index;
        return 0;
    }
    if (y.weak_ref) {
        *undefined_weak = 1;
        return 0;
    }
    return fail(l, "undefined symbol %s (referenced by %s)", spelled(l, o, index), o->input->name);
}

/* final address of a definition; the value of an absolute symbol */
static uint64_t symbol_address(const Link* l, size_t object, uint32_t index, int* absolute) {
    const Obj* o = &l->objs[object];
    const Symbol y = symbol_at(o, index);
    *absolute = y.absolute;
    if (y.absolute) return y.value;
    return o->sections[y.section].address + (y.value - o->sections[y.section].addr);
}

static int fits(int64_t value, unsigned bits) {
    return value >= -((int64_t)1 << (bits - 1)) && value < ((int64_t)1 << (bits - 1));
}
/* PAGEOFF12 into an ADD, or into a load or store, whose offset is scaled by the access's size (the address must be a
   multiple of it): its size field, or 16 bytes for a 128-bit SIMD access. 0, or -1 when misaligned */
static int pageoff12(unsigned char* p, uint64_t address) {
    uint32_t insn = snx_ld_le32(p);
    unsigned scale = 0;
    if ((insn & 0x3B000000u) == 0x39000000u) {
        scale = insn >> 30;
        if (!scale && (insn & 0x04800000u) == 0x04800000u) scale = 4;
    }
    if ((address & 0xFFF) & ((1u << scale) - 1)) return -1;
    snx_link_field(p, (address & 0xFFF) >> scale, 10, 12);
    return 0;
}

static uint64_t text_size(const Link* l) {
    uint64_t size = 0;
    for (size_t i = 0; i < l->nobjs; ++i)
        for (uint32_t j = 1; j <= l->objs[i].nsections; ++j)
            if (l->objs[i].sections[j].kind == K_TEXT)
                size += l->objs[i].sections[j].size + ((uint64_t)1 << l->objs[i].sections[j].align);
    return size;
}

static void layout_kinds(Link* l) {
    for (size_t k = 0; k < K_COUNT; ++k) l->kind_align[k] = k == K_TEXT ? 4 : 1;
    for (size_t k = 0; k < K_COUNT; ++k)
        for (size_t i = 0; i < l->nobjs; ++i) {
            Obj* o = &l->objs[i];
            for (uint32_t j = 1; j <= o->nsections; ++j) {
                Sec* s = &o->sections[j];
                uint64_t align = (uint64_t)1 << s->align;
                if (s->kind != (int)k) continue;
                l->kind_size[k] = snx_align_up(l->kind_size[k], align);
                s->position = l->kind_size[k];
                l->kind_size[k] += s->size;
                if (align > l->kind_align[k]) l->kind_align[k] = align;
            }
            if (k == K_TEXT && o->veneers) { /* right after the object's code, in reach of all of it */
                o->island = snx_align_up(l->kind_size[k], 4);
                l->kind_size[k] = o->island + VENEER * o->veneers;
            }
        }
}

typedef struct {
    uint64_t* addresses;
    size_t count, capacity;
} Rebases;
static int rebase(Link* l, Rebases* r, uint64_t address) {
    if (r->count == r->capacity) {
        r->capacity = r->capacity ? r->capacity * 2 : 1024;
        uint64_t* grown = (uint64_t*)realloc(r->addresses, r->capacity * sizeof(*grown));
        if (!grown) return fail(l, "out of memory%s%s", "", "");
        r->addresses = grown;
    }
    r->addresses[r->count++] = address;
    return 0;
}

/* a GOT slot for the definition, or for an undefined weak symbol by its name: the slot's address, or 0 when the second
   pass finds none */
static uint64_t got_slot(Link* l, const unsigned char* image, size_t def_object, size_t def_index, int weak,
                         const char* name) {
    char key[72];
    size_t slot;
    if (weak) /* a weak import nothing defines: a slot of its own, holding 0 */
        snprintf(key, sizeof(key), "~%.70s", name);
    else
        snx_link_got_key(key, def_object, def_index);
    if (!snx_link_find(&l->got, key, &slot)) {
        if (image) return 0;
        snx_link_insert(&l->got, key, slot = l->got_count++);
    }
    return l->got_address + 8 * (uint64_t)slot;
}

#if !defined(_WIN32)
/* An ELF object's relocations, in relocations' two passes, each written by elf_object.c as the AOT linker and the JIT
   loader write it: GOT slots and veneers are this module's, a 64-bit address in data a rebase */
static int elf_relocations(Link* l, size_t object, unsigned char* image, Rebases* rebases) {
    Obj* o = &l->objs[object];
    for (uint32_t j = 1; j <= o->nsections; ++j) {
        const Elf64_Shdr* h = &o->elf.sh[j];
        if ((h->sh_type != SHT_RELA && h->sh_type != SHT_REL) || h->sh_info > o->nsections) continue;
        const Sec* s = &o->sections[h->sh_info];
        if (s->kind < 0 || s->kind == K_BSS) continue;
        if (h->sh_type == SHT_REL) return fail(l, "%s: REL relocations in %s", o->input->name, s->name);
        const Elf64_Rela* r = (const Elf64_Rela*)(const void*)(o->input->data + h->sh_offset);
        for (size_t k = 0, n = h->sh_size / sizeof(*r); k < n; ++k) {
            unsigned type = snx_elf_type(EM_AARCH64, r[k].r_info);
            size_t index = snx_elf_symbol(EM_AARCH64, r[k].r_info), def_object = 0, slot;
            uint32_t def_index = 0;
            uint64_t S = 0, P = s->address + r[k].r_offset, got = 0;
            int weak = 0, absolute = 0, call = snx_elf_call_relocation(EM_AARCH64, type), written;
            char key[72];
            if (type == SNX_A64_NONE) continue;
            if (!index) return fail(l, "%s: a relocation of no symbol in %s", o->input->name, s->name);
            if (definition(l, object, (uint32_t)index, &def_object, &def_index, &weak)) return -1;
            if (!weak) S = symbol_address(l, def_object, def_index, &absolute);
            if (snx_elf_got_relocation(EM_AARCH64, type) &&
                !(got = got_slot(l, image, def_object, def_index, weak, spelled(l, o, (uint32_t)index))) && image)
                return fail(l, "%s: GOT slot missing%s", o->input->name, "");
            snx_link_veneer_key(key, object, def_object, def_index);
            if (!image) {
                if (call && weak) /* the module imports nothing: there is no address to call */
                    return fail(l, "%s: a call to the undefined weak symbol %s", o->input->name,
                                spelled(l, o, (uint32_t)index));
                if (call && l->far && def_object != object && !r[k].r_addend) {
                    if (!snx_link_find(&l->veneer, key, &slot)) snx_link_insert(&l->veneer, key, o->veneers++);
                } else if (snx_elf_absolute_relocation(EM_AARCH64, type) && !weak && !absolute && s->kind != K_DATA) {
                    return fail(l, "%s: absolute address in read-only section %s", o->input->name, s->name);
                }
                continue;
            }
            written = r[k].r_offset > s->size
                          ? SNX_LINK_OUTSIDE
                          : snx_elf_relocate(EM_AARCH64, image + P, (size_t)(s->size - r[k].r_offset), type, S,
                                             r[k].r_addend, P, 0, got);
            /* the caller's veneer, which it reaches, jumps there */
            if (written == SNX_LINK_RANGE && call && !r[k].r_addend && snx_link_find(&l->veneer, key, &slot)) {
                uint64_t veneer = l->kind_address[K_TEXT] + o->island + VENEER * (uint64_t)slot;
                written = snx_elf_branch_veneer(image + P, P, image + veneer, veneer, S, 0);
            }
            if (written == SNX_LINK_OUTSIDE)
                return fail(l, "%s: relocation outside section %s", o->input->name, s->name);
            if (written == SNX_LINK_MISALIGNED)
                return fail(l, "%s: relocation to a misaligned address in %s", o->input->name, s->name);
            if (written) {
                snprintf(l->error, l->error_size,
                         written == SNX_LINK_RANGE ? "%s: relocation %u out of range (the module is too large)"
                                                   : "%s: unsupported relocation type %u",
                         o->input->name, type);
                return -1;
            }
            if (snx_elf_absolute_relocation(EM_AARCH64, type) && !weak && !absolute && rebase(l, rebases, P)) return -1;
        }
    }
    return 0;
}
#endif

/* The relocations of every loaded section. The first pass (image NULL) finds the GOT slots and veneers and checks
   that absolute addresses are in data; the second applies them to the image (file offsets are addresses there) and
   lists the rebases. A relocation's target is a symbol (r_extern), or a section by its number, whose contents hold
   the target's address in the object; ARM64_RELOC_ADDEND gives the next one an addend, ARM64_RELOC_SUBTRACTOR the
   symbol the next one's is subtracted from */
static int relocations(Link* l, unsigned char* image, Rebases* rebases) {
    size_t def_object = 0, slot;
    uint32_t def_index = 0;
    char key[72];
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
#if !defined(_WIN32)
        if (o->is_elf) {
            if (elf_relocations(l, i, image, rebases)) return -1;
            continue;
        }
#endif
        for (uint32_t j = 1; j <= o->nsections; ++j) {
            const Sec* s = &o->sections[j];
            const unsigned char* table = o->input->data + s->reloff;
            int64_t addend = 0;
            int has_addend = 0;
            if (s->kind < 0 || s->kind == K_BSS) continue;
            for (uint32_t k = 0; k < s->nreloc; ++k) {
                uint32_t word = snx_ld_le32(table + 8 * (size_t)k + 4), at = snx_ld_le32(table + 8 * (size_t)k);
                unsigned type = word >> 28, length = (word >> 25) & 3, external = (word >> 27) & 1,
                         pcrel = (word >> 24) & 1, number = word & 0xFFFFFF;
                uint64_t P = s->address + at, S = 0, subtrahend = 0, value, got = 0;
                int64_t A = has_addend ? addend : 0, relative;
                int weak = 0, absolute = 0;
                unsigned char* p = image ? image + P : NULL;
                has_addend = 0;
                if (at & 0x80000000u) return fail(l, "%s: scattered relocation in %s", o->input->name, s->name);
                if (type == R_ADDEND) {
                    addend = (int64_t)(number ^ 0x800000) - 0x800000; /* 24 bits, signed */
                    has_addend = 1;
                    continue;
                }
                if (type == R_SUBTRACTOR) {
                    /* the subtrahend, here; the minuend and its size are the UNSIGNED that follows */
                    if (!external || k + 1 >= s->nreloc)
                        return fail(l, "%s: unpaired ARM64_RELOC_SUBTRACTOR in %s", o->input->name, s->name);
                    if (definition(l, i, number, &def_object, &def_index, &weak)) return -1;
                    if (weak)
                        return fail(l, "%s: subtraction of an undefined weak symbol in %s", o->input->name, s->name);
                    subtrahend = symbol_address(l, def_object, def_index, &absolute);
                    word = snx_ld_le32(table + 8 * (size_t)++k + 4);
                    if (snx_ld_le32(table + 8 * (size_t)k) != at || word >> 28 != R_UNSIGNED)
                        return fail(l, "%s: unpaired ARM64_RELOC_SUBTRACTOR in %s", o->input->name, s->name);
                    length = (word >> 25) & 3;
                    external = (word >> 27) & 1;
                    number = word & 0xFFFFFF;
                    pcrel = (word >> 24) & 1;
                    type = R_SUBTRACTOR;
                }
                if ((uint64_t)at + (length == 3 ? 8 : 4) > s->size)
                    return fail(l, "%s: relocation outside section %s", o->input->name, s->name);
                if (type == R_TLVP_LOAD_PAGE21 || type == R_TLVP_LOAD_PAGEOFF12)
                    return fail(l, "%s: thread-local data is not supported%s", o->input->name, "");
                if (external) {
                    if (definition(l, i, number, &def_object, &def_index, &weak)) return -1;
                    if (!weak) S = symbol_address(l, def_object, def_index, &absolute);
                } else {
                    /* a section: the contents hold the target's address in the object, which moved with it */
                    if (!number || number > o->nsections || o->sections[number].kind < 0 || pcrel ||
                        (type != R_UNSIGNED && type != R_SUBTRACTOR))
                        return fail(l, "%s: unsupported section relocation in %s", o->input->name, s->name);
                    S = o->sections[number].address - o->sections[number].addr;
                    def_object = (size_t)-1;
                    absolute = 0;
                }
                if (type == R_GOT_LOAD_PAGE21 || type == R_GOT_LOAD_PAGEOFF12 || type == R_POINTER_TO_GOT) {
                    if (!external) return fail(l, "%s: GOT reference to a section in %s", o->input->name, s->name);
                    got = got_slot(l, image, def_object, def_index, weak, symbol_name(o, symbol(o, number)));
                    if (!got && image) return fail(l, "%s: GOT slot missing%s", o->input->name, "");
                }
                if (!image) {
                    if (type == R_BRANCH26 && weak) /* the module imports nothing: there is no address to call */
                        return fail(l, "%s: a call to the undefined weak symbol %s", o->input->name,
                                    symbol_name(o, symbol(o, number)));
                    if (type == R_BRANCH26 && l->far && def_object != i && !weak) {
                        snx_link_veneer_key(key, i, def_object, def_index);
                        if (!snx_link_find(&l->veneer, key, &slot)) snx_link_insert(&l->veneer, key, o->veneers++);
                    } else if (type == R_UNSIGNED && length == 3 && !weak && !absolute && s->kind != K_DATA) {
                        return fail(l, "%s: absolute address in read-only section %s", o->input->name, s->name);
                    }
                    continue;
                }
                if (type == R_UNSIGNED || type == R_SUBTRACTOR) /* the addend is in place */
                    A += length == 3 ? (int64_t)snx_ld_le64(p) : (int64_t)(int32_t)snx_ld_le32(p);
                if (pcrel != (type == R_BRANCH26 || type == R_PAGE21 || type == R_GOT_LOAD_PAGE21 ||
                              type == R_POINTER_TO_GOT) ||
                    (length != 2 && type != R_UNSIGNED && type != R_SUBTRACTOR))
                    return fail(l, "%s: malformed relocation in %s", o->input->name, s->name);
                switch (type) {
                    case R_UNSIGNED:
                        value = S + (uint64_t)A;
                        if (length == 3) {
                            snx_st_le64(p, value);
                            if (!weak && !absolute && rebase(l, rebases, P)) return -1;
                        } else if (absolute || weak) {
                            snx_st_le32(p, (uint32_t)value);
                        } else {
                            return fail(l, "%s: 32-bit absolute address in %s", o->input->name, s->name);
                        }
                        break;
                    case R_SUBTRACTOR:
                        value = S + (uint64_t)A - subtrahend;
                        if (length == 3)
                            snx_st_le64(p, value);
                        else if ((int64_t)value == (int32_t)value)
                            snx_st_le32(p, (uint32_t)value);
                        else
                            goto range;
                        break;
                    case R_BRANCH26:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (!fits(relative, 28)) {
                            /* the caller's veneer, which it reaches, jumps there: ADRP x16, ADD x16, BR x16 */
                            uint64_t target = S + (uint64_t)A;
                            snx_link_veneer_key(key, i, def_object, def_index);
                            if (!snx_link_find(&l->veneer, key, &slot)) goto range;
                            uint64_t at_veneer = l->kind_address[K_TEXT] + o->island + VENEER * slot;
                            unsigned char* v = image + at_veneer;
                            snx_st_le32(v, 0x90000010u);
                            snx_link_adr_field(v, (uint64_t)((int64_t)(snx_link_page_of(target)
                                                                       - snx_link_page_of(at_veneer)) >> 12));
                            snx_st_le32(v + 4, 0x91000210u);
                            snx_link_field(v + 4, target & 0xFFF, 10, 12);
                            snx_st_le32(v + 8, 0xD61F0200u);
                            relative = (int64_t)(at_veneer - P);
                            if (!fits(relative, 28)) goto range;
                        }
                        snx_link_field(p, (uint64_t)relative >> 2, 0, 26);
                        break;
                    case R_PAGE21:
                    case R_GOT_LOAD_PAGE21:
                        value = type == R_PAGE21 ? S + (uint64_t)A : got + (uint64_t)A;
                        relative = (int64_t)(snx_link_page_of(value) - snx_link_page_of(P)) >> 12;
                        if (!fits(relative, 21)) goto range;
                        snx_link_adr_field(p, (uint64_t)relative);
                        break;
                    case R_PAGEOFF12:
                    case R_GOT_LOAD_PAGEOFF12:
                        if (pageoff12(p, type == R_PAGEOFF12 ? S + (uint64_t)A : got + (uint64_t)A))
                            return fail(l, "%s: relocation to a misaligned address in %s", o->input->name, s->name);
                        break;
                    case R_POINTER_TO_GOT:
                        relative = (int64_t)(got + (uint64_t)A - P);
                        if (relative != (int32_t)relative) goto range;
                        snx_st_le32(p, (uint32_t)relative);
                        break;
                    default:
                        snprintf(l->error, l->error_size, "%s: unsupported relocation type %u", o->input->name, type);
                        return -1;
                }
                continue;
            range:
                snprintf(l->error, l->error_size, "%s: relocation %u out of range (the module is too large)",
                         o->input->name, type);
                return -1;
            }
        }
    }
    return 0;
}

static void uleb(SnxLinkBuffer* b, uint64_t v) {
    do {
        unsigned char byte = (unsigned char)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        snx_link_append(b, &byte, 1);
    } while (v);
}
static size_t uleb_size(uint64_t v) {
    size_t n = 1;
    while (v >>= 7) ++n;
    return n;
}
static void byte(SnxLinkBuffer* b, unsigned v) {
    unsigned char c = (unsigned char)v;
    snx_link_append(b, &c, 1);
}

/* rebase opcodes: every pointer in __DATA (segment 1) gets the slide. Runs of consecutive pointers are one opcode */
static void rebase_info(SnxLinkBuffer* b, Rebases* r, uint64_t data_address) {
    size_t i = 0;
    qsort(r->addresses, r->count, sizeof(*r->addresses), snx_compare_u64);
    byte(b, 0x10 | 1); /* REBASE_OPCODE_SET_TYPE_IMM, REBASE_TYPE_POINTER */
    if (r->count) {
        byte(b, 0x20 | 1); /* REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB */
        uleb(b, r->addresses[0] - data_address);
    }
    for (uint64_t at = r->count ? r->addresses[0] : 0; i < r->count;) {
        size_t n;
        for (n = 1; i + n < r->count && r->addresses[i + n] == r->addresses[i] + 8 * n; ++n) {}
        byte(b, 0x60); /* REBASE_OPCODE_DO_REBASE_ULEB_TIMES: n pointers, the address past them */
        uleb(b, n);
        at = r->addresses[i] + 8 * n;
        i += n;
        if (i < r->count) {
            byte(b, 0x30); /* REBASE_OPCODE_ADD_ADDR_ULEB */
            uleb(b, r->addresses[i] - at);
        }
    }
    byte(b, 0); /* REBASE_OPCODE_DONE */
}

/* The export trie: a node per prefix the names branch at, each with its edges' strings, and a terminal's address.
   Nodes refer to their children by offset in ULEB128, whose sizes depend on the offsets: laid out until they stay */
typedef struct {
    size_t first, count, depth; /* the names under it, sorted, and the length of its prefix */
    int terminal;               /* names[first] is this prefix */
    size_t child, nchildren;    /* its children: nodes child .. child + nchildren - 1 */
    uint64_t offset;
} TrieNode;
typedef struct {
    const char* const* names;
    const uint64_t* addresses;
    TrieNode* nodes;
    size_t count;
} Trie;
static size_t common_prefix(const char* a, const char* b) {
    size_t n = 0;
    while (a[n] && a[n] == b[n]) ++n;
    return n;
}
/* the children of node index (its names at its depth), then theirs */
static void trie_children(Trie* t, size_t index) {
    size_t first = t->nodes[index].first, end = first + t->nodes[index].count, depth = t->nodes[index].depth, n;
    if (first < end && !t->names[first][depth]) {
        t->nodes[index].terminal = 1;
        ++first;
    }
    t->nodes[index].child = t->count;
    for (size_t g = first; g < end; g = n) {
        TrieNode* c = &t->nodes[t->count++];
        for (n = g + 1; n < end && t->names[n][depth] == t->names[g][depth]; ++n) {}
        *c = (TrieNode){0};
        c->first = g;
        c->count = n - g;
        c->depth = common_prefix(t->names[g], t->names[n - 1]);
        ++t->nodes[index].nchildren;
    }
    n = t->nodes[index].nchildren;
    for (size_t g = 0; g < n; ++g) trie_children(t, t->nodes[index].child + g);
}
static size_t trie_node_size(const Trie* t, const TrieNode* node) {
    size_t size = 1; /* the terminal's size, then the children's count */
    if (node->terminal) {
        size_t info = 1 + uleb_size(t->addresses[node->first]); /* flags (EXPORT_SYMBOL_FLAGS_KIND_REGULAR), address */
        size = uleb_size(info) + info;
    }
    size += 1;
    for (size_t i = 0; i < node->nchildren; ++i) {
        const TrieNode* c = &t->nodes[node->child + i];
        size += c->depth - node->depth + 1 + uleb_size(c->offset);
    }
    return size;
}
static void export_trie(SnxLinkBuffer* b, const char* const* names, const uint64_t* addresses, size_t count) {
    Trie t;
    int changed;
    t.names = names;
    t.addresses = addresses;
    t.nodes = (TrieNode*)calloc(2 * count + 1, sizeof(TrieNode));
    if (!t.nodes) {
        fputs("out of memory\n", stderr);
        abort();
    }
    t.count = 1;
    t.nodes[0].count = count;
    trie_children(&t, 0);
    do {
        uint64_t offset = 0;
        changed = 0;
        for (size_t i = 0; i < t.count; ++i) {
            if (t.nodes[i].offset != offset) changed = 1;
            t.nodes[i].offset = offset;
            offset += trie_node_size(&t, &t.nodes[i]);
        }
    } while (changed);
    for (size_t i = 0; i < t.count; ++i) {
        const TrieNode* node = &t.nodes[i];
        if (node->terminal) {
            uleb(b, 1 + uleb_size(addresses[node->first]));
            byte(b, 0);
            uleb(b, addresses[node->first]);
        } else {
            byte(b, 0);
        }
        byte(b, (unsigned)node->nchildren);
        for (size_t j = 0; j < node->nchildren; ++j) {
            const TrieNode* c = &t.nodes[node->child + j];
            snx_link_append(b, names[c->first] + node->depth, c->depth - node->depth);
            byte(b, 0);
            uleb(b, c->offset);
        }
    }
    free(t.nodes);
}

static void nlist(SnxLinkBuffer* symtab, uint32_t name, unsigned type, unsigned section, uint64_t value) {
    unsigned char s[16];
    snx_st_le32(s, name);
    s[4] = (unsigned char)type;
    s[5] = (unsigned char)section;
    snx_st_le16(s + 6, 0);
    snx_st_le64(s + 8, value);
    snx_link_append(symtab, s, sizeof(s));
}

/* dyld refuses an image that doesn't link libSystem, though nothing here calls into it */
#define LIBSYSTEM "/usr/lib/libSystem.B.dylib"
#define LIBSYSTEM_SIZE ((sizeof(LIBSYSTEM) + 24 + 7) & ~(size_t)7)

/* load commands */
static unsigned char* command(SnxLinkBuffer* b, uint32_t cmd, uint32_t size) {
    size_t at = b->size;
    snx_link_reserve(b, at + size);
    snx_st_le32(b->data + at, cmd);
    snx_st_le32(b->data + at + 4, size);
    return b->data + at;
}
static unsigned char* segment(SnxLinkBuffer* b, const char* name, uint64_t address, uint64_t vmsize, uint64_t offset,
                              uint64_t filesize, uint32_t protection, uint32_t nsections) {
    unsigned char* c = command(b, LC_SEGMENT_64, 72 + 80 * nsections);
    memcpy(c + 8, name, strlen(name));
    snx_st_le64(c + 24, address);
    snx_st_le64(c + 32, vmsize);
    snx_st_le64(c + 40, offset);
    snx_st_le64(c + 48, filesize);
    snx_st_le32(c + 56, protection); /* maxprot, as ld64 sets it on arm64 */
    snx_st_le32(c + 60, protection);
    snx_st_le32(c + 64, nsections);
    return c + 72;
}
static unsigned char* section(unsigned char* h, const char* segment_name, const char* name, uint64_t address,
                              uint64_t size, uint64_t align, uint32_t flags, int zerofill) {
    unsigned log2 = 0;
    while (((uint64_t)1 << log2) < align) ++log2;
    memcpy(h, name, strlen(name));
    memcpy(h + 16, segment_name, strlen(segment_name));
    snx_st_le64(h + 32, address);
    snx_st_le64(h + 40, size);
    snx_st_le32(h + 48, zerofill ? 0 : (uint32_t)address); /* file offsets are addresses in __TEXT and __DATA */
    snx_st_le32(h + 52, log2);
    snx_st_le32(h + 64, flags);
    return h + 80;
}

int snx_link_macho(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                   const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    Link l = {0};
    Rebases rebases = {0};
    SnxLinkBuffer out = {0}, commands = {0}, linkedit_rebase = {0}, trie = {0}, symtab = {0}, strtab = {0};
    char** names = NULL; /* the exports' Mach-O names, sorted */
    uint64_t* export_address = NULL;
    size_t nlocal = 0;
    uint32_t ncmds = 11;
    unsigned* export_section = NULL;
    int rc = -1;
    *output = (SnxLinkOutput){0};
    l.error = error;
    l.error_size = error_size;
    l.nobjs = count;
    l.objs = (Obj*)calloc(count, sizeof(Obj));
    names = (char**)calloc(export_count + 1, sizeof(*names));
    export_address = (uint64_t*)calloc(export_count + 1, sizeof(*export_address));
    export_section = (unsigned*)calloc(export_count + 1, sizeof(*export_section));
    if (!l.objs || !names || !export_address || !export_section) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    for (size_t i = 0; i < count; ++i)
        if (parse(&l, &l.objs[i], &inputs[i])) goto done;
    if (!l.platform) { /* objects without one: the oldest arm64 macOS */
        l.platform = PLATFORM_MACOS;
        l.minos = 11 << 16;
    }
    if (l.sdk < l.minos) l.sdk = l.minos;
    l.far = text_size(&l) >= VENEER_TEXT;
    if (collect_globals(&l) || relocations(&l, NULL, NULL)) goto done;
    layout_kinds(&l);
    /* the load commands' size, which the first section follows */
    size_t id_size = snx_align_up(24 + strlen(soname) + 1, 8);
    uint32_t sizeofcmds =
        (uint32_t)((72 + 80 * 2) + (72 + 80 * 3) + 72 + id_size + LIBSYSTEM_SIZE + 48 + 24 + 80 + 24 + 24 + 16);
    uint64_t headers = 32 + sizeofcmds;
    /* addresses: __TEXT from the headers, __DATA and __LINKEDIT on the pages after it */
    uint64_t text_address = snx_align_up(headers, l.kind_align[K_TEXT]);
    uint64_t const_address = snx_align_up(text_address + l.kind_size[K_TEXT], l.kind_align[K_CONST]);
    uint64_t data_address = snx_align_up(const_address + l.kind_size[K_CONST], PAGE);
    uint64_t got_address = snx_align_up(data_address + l.kind_size[K_DATA], 8);
    uint64_t bss_address = snx_align_up(got_address + 8 * l.got_count, l.kind_align[K_BSS]);
    uint64_t data_end = snx_align_up(bss_address + l.kind_size[K_BSS], PAGE);
    if (data_end == data_address) data_end += PAGE;
    uint64_t data_filesize = snx_align_up(got_address + 8 * l.got_count - data_address, PAGE);
    uint64_t linkedit_offset = data_address + data_filesize;
    uint64_t linkedit_address = data_end;
    l.kind_address[K_TEXT] = text_address;
    l.kind_address[K_CONST] = const_address;
    l.kind_address[K_DATA] = data_address;
    l.kind_address[K_BSS] = bss_address;
    l.got_address = got_address;
    for (size_t i = 0; i < count; ++i)
        for (uint32_t j = 1; j <= l.objs[i].nsections; ++j)
            if (l.objs[i].sections[j].kind >= 0)
                l.objs[i].sections[j].address =
                    l.kind_address[l.objs[i].sections[j].kind] + l.objs[i].sections[j].position;
    /* contents, relocated */
    snx_link_reserve(&out, linkedit_offset);
    for (size_t i = 0; i < count; ++i) {
        const Obj* o = &l.objs[i];
        for (uint32_t j = 1; j <= o->nsections; ++j) {
            const Sec* s = &o->sections[j];
            if (s->kind >= 0 && s->kind != K_BSS && s->size)
                memcpy(out.data + s->address, o->input->data + s->offset, s->size);
        }
    }
    if (relocations(&l, out.data, &rebases)) goto done;
    /* GOT slots: their symbol's address, rebased */
    for (size_t i = 0; i < l.got.capacity; ++i)
        if (l.got.keys[i]) {
            unsigned long object, index;
            int absolute;
            uint64_t slot = got_address + 8 * l.got.values[i];
            if (l.got.keys[i][0] == '~') continue; /* an undefined weak import's, 0 */
            sscanf(l.got.keys[i], "%lu:%lu", &object, &index);
            snx_st_le64(out.data + slot, symbol_address(&l, object, (uint32_t)index, &absolute));
            if (!absolute && rebase(&l, &rebases, slot)) goto done;
        }
    /* exports, by their Mach-O names */
    for (size_t i = 0; i < export_count; ++i) {
        names[i] = (char*)malloc(strlen(exports[i]) + 2);
        if (!names[i]) goto done;
        names[i][0] = '_';
        strcpy(names[i] + 1, exports[i]);
    }
    qsort(names, export_count, sizeof(*names), snx_compare_strings);
    for (size_t i = 0; i < export_count; ++i) {
        size_t found;
        int absolute;
        if (!snx_link_find(&l.globals, names[i], &found)) {
            snprintf(error, error_size, "exported symbol %s is not defined", names[i] + 1);
            goto done;
        }
        export_address[i] = symbol_address(&l, found >> 32, (uint32_t)found, &absolute);
        if (!absolute) {
            const Obj* o = &l.objs[found >> 32];
            export_section[i] = (unsigned)kind_section[o->sections[symbol_at(o, (uint32_t)found).section].kind];
        }
    }
    /* __LINKEDIT: rebases, exports, symbols (every named function and object, local, for debuggers and crash reports;
       the exports last, external), their names, then the signature */
    rebase_info(&linkedit_rebase, &rebases, data_address);
    export_trie(&trie, (const char* const*)names, export_address, export_count);
    snx_link_append(&strtab, " ", 2); /* index 1 is the empty name, as ld64 writes it */
    for (size_t i = 0; i < count; ++i) {
        const Obj* o = &l.objs[i];
        for (uint32_t j = 0; j < o->nsymbols; ++j) {
            const Symbol y = symbol_at(o, j);
            const char* name;
            size_t found;
            if (no_symbol(o, j) || !y.defined || unnamed(o, j)) continue;
            name = spelled(&l, o, j);
            if (y.external && snx_link_find(&l.globals, name, &found) && found != ((i << 32) | j))
                continue; /* an overridden weak definition */
            /* external, below */
            if (bsearch(&name, names, export_count, sizeof(*names), snx_compare_strings)) continue;
            {
                int absolute;
                uint32_t offset = (uint32_t)snx_link_append(&strtab, name, strlen(name) + 1);
                nlist(&symtab, offset, N_SECT, (unsigned)kind_section[o->sections[y.section].kind],
                      symbol_address(&l, i, j, &absolute));
                ++nlocal;
            }
        }
    }
    for (size_t i = 0; i < export_count; ++i)
        nlist(&symtab, (uint32_t)snx_link_append(&strtab, names[i], strlen(names[i]) + 1),
              export_section[i] ? N_SECT | N_EXT : N_ABS | N_EXT, export_section[i], export_address[i]);
    snx_link_align(&strtab, 8);
    uint64_t rebase_offset = linkedit_offset;
    uint64_t trie_offset = snx_align_up(rebase_offset + linkedit_rebase.size, 8);
    uint64_t symtab_offset = snx_align_up(trie_offset + trie.size, 8);
    uint64_t strtab_offset = symtab_offset + symtab.size;
    uint64_t signature_offset = snx_align_up(strtab_offset + strtab.size, 16);
    /* the signature: a super blob holding one code directory (version 0x20400, with the executable segment), its
       identifier, and a SHA-256 of each 4 KiB page before it */
    uint64_t nslots = (signature_offset + SIGNATURE_PAGE - 1) / SIGNATURE_PAGE;
    uint64_t hash_offset = snx_align_up(24 + 88 + strlen(soname) + 1, 16);
    uint64_t signature_size = hash_offset + 32 * nslots;
    if (signature_offset + signature_size > 0xFFFFFFFFu) {
        snprintf(error, error_size, "the module is larger than Mach-O's 4 GiB");
        goto done;
    }
    snx_link_reserve(&out, signature_offset + signature_size);
    memcpy(out.data + rebase_offset, linkedit_rebase.data, linkedit_rebase.size);
    memcpy(out.data + trie_offset, trie.data, trie.size);
    memcpy(out.data + symtab_offset, symtab.data, symtab.size);
    memcpy(out.data + strtab_offset, strtab.data, strtab.size);
    /* load commands */
    unsigned char* h = segment(&commands, "__TEXT", 0, data_address, 0, data_address, 5, 2);
    h = section(h, "__TEXT", "__text", text_address, l.kind_size[K_TEXT], l.kind_align[K_TEXT],
                S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, 0);
    section(h, "__TEXT", "__const", const_address, l.kind_size[K_CONST], l.kind_align[K_CONST], 0, 0);
    h = segment(&commands, "__DATA", data_address, data_end - data_address, data_address, data_filesize, 3, 3);
    h = section(h, "__DATA", "__data", data_address, l.kind_size[K_DATA], l.kind_align[K_DATA], 0, 0);
    h = section(h, "__DATA", "__got", got_address, 8 * l.got_count, 8, 0, 0);
    section(h, "__DATA", "__bss", bss_address, l.kind_size[K_BSS], l.kind_align[K_BSS], S_ZEROFILL, 1);
    segment(&commands, "__LINKEDIT", linkedit_address,
            snx_align_up(signature_offset + signature_size - linkedit_offset, PAGE), linkedit_offset,
            signature_offset + signature_size - linkedit_offset, 1, 0);
    unsigned char* c = command(&commands, LC_ID_DYLIB, (uint32_t)id_size);
    snx_st_le32(c + 8, 24);
    snx_st_le32(c + 12, 1);       /* timestamp */
    snx_st_le32(c + 16, 1 << 16); /* current and compatibility versions, 1.0.0 */
    snx_st_le32(c + 20, 1 << 16);
    memcpy(c + 24, soname, strlen(soname));
    c = command(&commands, LC_LOAD_DYLIB, (uint32_t)LIBSYSTEM_SIZE);
    snx_st_le32(c + 8, 24);
    snx_st_le32(c + 12, 2);
    snx_st_le32(c + 16, 1 << 16);
    snx_st_le32(c + 20, 1 << 16);
    memcpy(c + 24, LIBSYSTEM, sizeof(LIBSYSTEM) - 1);
    c = command(&commands, LC_DYLD_INFO_ONLY, 48);
    snx_st_le32(c + 8, (uint32_t)rebase_offset);
    snx_st_le32(c + 12, (uint32_t)linkedit_rebase.size);
    snx_st_le32(c + 40, (uint32_t)trie_offset);
    snx_st_le32(c + 44, (uint32_t)trie.size);
    c = command(&commands, LC_SYMTAB, 24);
    snx_st_le32(c + 8, (uint32_t)symtab_offset);
    snx_st_le32(c + 12, (uint32_t)(nlocal + export_count));
    snx_st_le32(c + 16, (uint32_t)strtab_offset);
    snx_st_le32(c + 20, (uint32_t)strtab.size);
    c = command(&commands, LC_DYSYMTAB, 80);
    snx_st_le32(c + 12, (uint32_t)nlocal); /* ilocalsym 0, nlocalsym */
    snx_st_le32(c + 16, (uint32_t)nlocal); /* iextdefsym, nextdefsym: the exports, sorted by name */
    snx_st_le32(c + 20, (uint32_t)export_count);
    snx_st_le32(c + 24, (uint32_t)(nlocal + export_count)); /* iundefsym, none */
    size_t uuid = 32 + commands.size + 8; /* in the output, after the header */
    command(&commands, LC_UUID, 24);
    c = command(&commands, LC_BUILD_VERSION, 24);
    snx_st_le32(c + 8, l.platform);
    snx_st_le32(c + 12, l.minos);
    snx_st_le32(c + 16, l.sdk);
    c = command(&commands, LC_CODE_SIGNATURE, 16);
    snx_st_le32(c + 8, (uint32_t)signature_offset);
    snx_st_le32(c + 12, (uint32_t)signature_size);
    if (commands.size != sizeofcmds) {
        snprintf(error, error_size, "Mach-O load commands of an unexpected size");
        goto done;
    }
    /* header */
    h = out.data;
    snx_st_le32(h, MH_MAGIC_64);
    snx_st_le32(h + 4, CPU_TYPE_ARM64);
    snx_st_le32(h + 8, 0); /* CPU_SUBTYPE_ARM64_ALL */
    snx_st_le32(h + 12, MH_DYLIB);
    snx_st_le32(h + 16, ncmds);
    snx_st_le32(h + 20, sizeofcmds);
    /* MH_NOUNDEFS, MH_DYLDLINK, MH_TWOLEVEL, MH_NO_REEXPORTED_DYLIBS */
    snx_st_le32(h + 24, 0x1 | 0x4 | 0x80 | 0x100000);
    memcpy(h + 32, commands.data, commands.size);
    /* the UUID: the image's hash with it zero, as a version 3 UUID (lld's) */
    {
        unsigned char digest[32];
        sha256(out.data, (size_t)signature_offset, digest);
        memcpy(out.data + uuid, digest, 16);
        out.data[uuid + 6] = (unsigned char)((out.data[uuid + 6] & 0x0F) | 0x30);
        out.data[uuid + 8] = (unsigned char)((out.data[uuid + 8] & 0x3F) | 0x80);
    }
    unsigned char* signature = out.data + signature_offset;
    snx_st_be32(signature, 0xFADE0CC0u); /* CSMAGIC_EMBEDDED_SIGNATURE */
    snx_st_be32(signature + 4, (uint32_t)signature_size);
    snx_st_be32(signature + 8, 1);
    snx_st_be32(signature + 12, 0); /* CSSLOT_CODEDIRECTORY */
    snx_st_be32(signature + 16, 24);
    c = signature + 24;
    snx_st_be32(c, 0xFADE0C02u); /* CSMAGIC_CODEDIRECTORY */
    snx_st_be32(c + 4, (uint32_t)(signature_size - 24));
    snx_st_be32(c + 8, 0x20400);    /* CS_SUPPORTSEXECSEG */
    snx_st_be32(c + 12, 0x20002);   /* CS_ADHOC | CS_LINKER_SIGNED */
    snx_st_be32(c + 16, (uint32_t)(hash_offset - 24));
    snx_st_be32(c + 20, 88);        /* the identifier, after the code directory */
    snx_st_be32(c + 28, (uint32_t)nslots);
    snx_st_be32(c + 32, (uint32_t)signature_offset);
    c[36] = 32;                /* hashSize */
    c[37] = 2;                 /* CS_HASHTYPE_SHA256 */
    c[39] = 12;                /* log2 of the page size */
    snx_st_be64(c + 64, 0);         /* execSegBase: __TEXT */
    snx_st_be64(c + 72, data_address);
    memcpy(c + 88, soname, strlen(soname) + 1);
    for (size_t i = 0; i < nslots; ++i) {
        uint64_t at = (uint64_t)i * SIGNATURE_PAGE;
        sha256(out.data + at, (size_t)(signature_offset - at < SIGNATURE_PAGE ? signature_offset - at : SIGNATURE_PAGE),
               signature + hash_offset + 32 * i);
    }
    output->data = out.data;
    output->size = (size_t)(signature_offset + signature_size);
    out.data = NULL;
    rc = 0;
done:
    for (size_t i = 0; l.objs && i < count; ++i) {
#if !defined(_WIN32)
        if (l.objs[i].is_elf) snx_elf_free(&l.objs[i].elf);
#endif
        free(l.objs[i].sections);
    }
    free(l.objs);
    free(l.name);
    for (size_t i = 0; names && i < export_count; ++i) free(names[i]);
    free(names);
    free(export_address);
    free(export_section);
    free(rebases.addresses);
    snx_link_names_free(&l.globals);
    snx_link_names_free(&l.got);
    snx_link_names_free(&l.veneer);
    free(out.data);
    free(commands.data);
    free(linkedit_rebase.data);
    free(trie.data);
    free(symtab.data);
    free(strtab.data);
    return rc;
}
