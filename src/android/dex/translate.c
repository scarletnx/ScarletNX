/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/dex/translate.h"
#include "android/dex/annotations.h"
#include "android/dex/classfile.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The views of a register: each is a JVM local of one type */
enum { V_I, V_F, V_A, V_J, V_D, VIEWS };
#define VIEW_BIT(v) (1u << (v))

/* What the dataflow knows of a register. A kind is its low four bits; a reference's payload is its static type (an
   index in Method.types, 0 for one the code does not tell), an uninitialized object's is the address of its
   new-instance */
enum {
    K_TOP,         /* nothing usable: not set, or set differently on two paths */
    K_INT,         /* a 32-bit integer (boolean, byte, char, short, int) */
    K_FLOAT,
    K_CONST,       /* a 32-bit constant that is not zero: an int or a float */
    K_ZERO,        /* the constant zero: an int, a float or null */
    K_REF,
    K_UNINIT,      /* an object new-instance made, before its <init> */
    K_UNINIT_THIS, /* a constructor's this before the call to its super or this <init> */
    K_LONG,
    K_DOUBLE,
    K_CONSTW,      /* a 64-bit constant: a long or a double */
    K_HI           /* the second register of a long or a double */
};
#define KIND(k)          ((k) & 15u)
#define PAYLOAD(k)       ((k) >> 4)
#define MAKE(kind, data) ((uint32_t)(kind) | (uint32_t)(data) << 4)

/* JVM opcodes */
enum {
    J_NOP = 0x00, J_ACONST_NULL = 0x01, J_ICONST_0 = 0x03, J_LCONST_0 = 0x09, J_FCONST_0 = 0x0b, J_DCONST_0 = 0x0e,
    J_BIPUSH = 0x10, J_SIPUSH = 0x11, J_LDC = 0x12, J_LDC_W = 0x13, J_LDC2_W = 0x14, J_ILOAD = 0x15, J_ALOAD = 0x19,
    J_ALOAD_0 = 0x2a, J_IALOAD = 0x2e, J_ISTORE = 0x36, J_IASTORE = 0x4f, J_AASTORE = 0x53, J_POP = 0x57,
    J_POP2 = 0x58, J_DUP = 0x59, J_IXOR = 0x82, J_I2L = 0x85, J_LXOR = 0x83, J_IFEQ = 0x99, J_IF_ICMPEQ = 0x9f,
    J_IF_ACMPEQ = 0xa5, J_GOTO = 0xa7, J_TABLESWITCH = 0xaa, J_LOOKUPSWITCH = 0xab, J_IRETURN = 0xac,
    J_RETURN = 0xb1, J_GETSTATIC = 0xb2, J_PUTSTATIC = 0xb3, J_GETFIELD = 0xb4, J_PUTFIELD = 0xb5,
    J_INVOKEVIRTUAL = 0xb6, J_INVOKESPECIAL = 0xb7, J_INVOKESTATIC = 0xb8, J_INVOKEINTERFACE = 0xb9, J_NEW = 0xbb,
    J_NEWARRAY = 0xbc, J_ANEWARRAY = 0xbd, J_ARRAYLENGTH = 0xbe, J_ATHROW = 0xbf, J_CHECKCAST = 0xc0,
    J_INSTANCEOF = 0xc1, J_MONITORENTER = 0xc2, J_MONITOREXIT = 0xc3, J_WIDE = 0xc4, J_IFNULL = 0xc6,
    J_GOTO_W = 0xc8
};
/* the load and store of each view, and its slots */
static const unsigned char view_load[VIEWS] = {0x15, 0x17, 0x19, 0x16, 0x18};
static const unsigned char view_store[VIEWS] = {0x36, 0x38, 0x3a, 0x37, 0x39};
static const unsigned view_width[VIEWS] = {1, 1, 1, 2, 2};

/* DEX instruction formats */
enum {
    F10x, F12x, F11n, F11x, F10t, F20t, F22x, F21t, F21s, F21h, F21c, F23x, F22b, F22t, F22s, F22c, F32x, F30t, F31t,
    F31i, F31c, F35c, F3rc, F51l, F45cc, F4rcc
};
static unsigned char format_of(unsigned op) {
    static const struct {
        unsigned char first, last, format;
    } ranges[] = {
        {0x00, 0x00, F10x}, {0x01, 0x01, F12x}, {0x02, 0x02, F22x}, {0x03, 0x03, F32x}, {0x04, 0x04, F12x},
        {0x05, 0x05, F22x}, {0x06, 0x06, F32x}, {0x07, 0x07, F12x}, {0x08, 0x08, F22x}, {0x09, 0x09, F32x},
        {0x0a, 0x0d, F11x}, {0x0e, 0x0e, F10x}, {0x0f, 0x11, F11x}, {0x12, 0x12, F11n}, {0x13, 0x13, F21s},
        {0x14, 0x14, F31i}, {0x15, 0x15, F21h}, {0x16, 0x16, F21s}, {0x17, 0x17, F31i}, {0x18, 0x18, F51l},
        {0x19, 0x19, F21h}, {0x1a, 0x1a, F21c}, {0x1b, 0x1b, F31c}, {0x1c, 0x1c, F21c}, {0x1d, 0x1e, F11x},
        {0x1f, 0x1f, F21c}, {0x20, 0x20, F22c}, {0x21, 0x21, F12x}, {0x22, 0x22, F21c}, {0x23, 0x23, F22c},
        {0x24, 0x24, F35c}, {0x25, 0x25, F3rc}, {0x26, 0x26, F31t}, {0x27, 0x27, F11x}, {0x28, 0x28, F10t},
        {0x29, 0x29, F20t}, {0x2a, 0x2a, F30t}, {0x2b, 0x2c, F31t}, {0x2d, 0x31, F23x}, {0x32, 0x37, F22t},
        {0x38, 0x3d, F21t}, {0x3e, 0x43, F10x}, {0x44, 0x51, F23x}, {0x52, 0x5f, F22c}, {0x60, 0x6d, F21c},
        {0x6e, 0x72, F35c}, {0x73, 0x73, F10x}, {0x74, 0x78, F3rc}, {0x79, 0x7a, F10x}, {0x7b, 0x8f, F12x},
        {0x90, 0xaf, F23x}, {0xb0, 0xcf, F12x}, {0xd0, 0xd7, F22s}, {0xd8, 0xe2, F22b}, {0xe3, 0xf9, F10x},
        {0xfa, 0xfa, F45cc}, {0xfb, 0xfb, F4rcc}, {0xfc, 0xfc, F35c}, {0xfd, 0xfd, F3rc}, {0xfe, 0xff, F21c}};
    for (size_t i = 0; i < sizeof ranges / sizeof *ranges; ++i)
        if (op >= ranges[i].first && op <= ranges[i].last) return ranges[i].format;
    return F10x;
}
static const unsigned char format_units[] = {1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 5,
                                             4, 4};

typedef struct {
    unsigned op, size; /* size in code units */
    uint32_t a, b, c, idx;
    int64_t lit;
    uint32_t target; /* a branch's address, or a payload's */
    unsigned argc;
    uint32_t args[5]; /* a /range form's first register in args[0] */
    bool range;
} Insn;
static uint32_t arg_reg(const Insn* in, unsigned i) {
    return in->range ? in->args[0] + i : in->args[i];
}

/* the units of the method, bounded: past its end reads as 0 */
static unsigned unit(const DexCode* code, uint64_t pc) {
    return pc < code->insns_size ? (unsigned)code->insns[2 * pc] | (unsigned)code->insns[2 * pc + 1] << 8 : 0;
}
/* the size of the payload at pc (packed-switch, sparse-switch, fill-array-data), 0 when pc holds none */
static uint32_t payload_units(const DexCode* code, uint32_t pc) {
    unsigned w = unit(code, pc);
    uint64_t size;
    switch (w) {
    case 0x0100: size = 4 + 2ull * unit(code, pc + 1); break;
    case 0x0200: size = 2 + 4ull * unit(code, pc + 1); break;
    case 0x0300: {
        uint64_t width = unit(code, pc + 1), count = unit(code, pc + 2) | (uint64_t)unit(code, pc + 3) << 16;
        size = 4 + (width * count + 1) / 2;
        break;
    }
    default: return 0;
    }
    return size > code->insns_size ? code->insns_size : (uint32_t)size;
}
static Insn decode(const DexCode* code, uint32_t pc) {
    Insn in = {0};
    unsigned w0 = unit(code, pc), w1 = unit(code, pc + 1), w2 = unit(code, pc + 2);
    in.op = w0 & 0xff;
    unsigned format = format_of(in.op);
    in.size = format_units[format];
    unsigned aa = w0 >> 8, a4 = (w0 >> 8) & 15, b4 = w0 >> 12;
    switch (format) {
    case F10x: break;
    case F12x: in.a = a4; in.b = b4; break;
    case F11n: in.a = a4; in.lit = (int64_t)((int32_t)(b4 << 28) >> 28); break;
    case F11x: in.a = aa; break;
    case F10t: in.target = pc + (uint32_t)(int32_t)(int8_t)aa; break;
    case F20t: in.target = pc + (uint32_t)(int32_t)(int16_t)w1; break;
    case F22x: in.a = aa; in.b = w1; break;
    case F21t: in.a = aa; in.target = pc + (uint32_t)(int32_t)(int16_t)w1; break;
    case F21s: in.a = aa; in.lit = (int16_t)w1; break;
    case F21h:
        in.a = aa;
        in.lit = in.op == 0x15 ? (int64_t)(int32_t)((uint32_t)w1 << 16) : (int64_t)((uint64_t)w1 << 48);
        break;
    case F21c: in.a = aa; in.idx = w1; break;
    case F23x: in.a = aa; in.b = w1 & 0xff; in.c = w1 >> 8; break;
    case F22b: in.a = aa; in.b = w1 & 0xff; in.lit = (int8_t)(w1 >> 8); break;
    case F22t: in.a = a4; in.b = b4; in.target = pc + (uint32_t)(int32_t)(int16_t)w1; break;
    case F22s: in.a = a4; in.b = b4; in.lit = (int16_t)w1; break;
    case F22c: in.a = a4; in.b = b4; in.idx = w1; break;
    case F32x: in.a = w1; in.b = w2; break;
    case F30t: in.target = pc + (w1 | (uint32_t)w2 << 16); break;
    case F31t: in.a = aa; in.target = pc + (w1 | (uint32_t)w2 << 16); break;
    case F31i: in.a = aa; in.lit = (int32_t)(w1 | (uint32_t)w2 << 16); break;
    case F31c: in.a = aa; in.idx = w1 | (uint32_t)w2 << 16; break;
    case F35c:
    case F45cc:
        in.argc = b4;
        in.idx = w1;
        in.args[0] = w2 & 15;
        in.args[1] = (w2 >> 4) & 15;
        in.args[2] = (w2 >> 8) & 15;
        in.args[3] = w2 >> 12;
        in.args[4] = a4;
        if (in.argc > 5) in.argc = 5;
        break;
    case F3rc:
    case F4rcc:
        in.argc = aa;
        in.idx = w1;
        in.args[0] = w2;
        in.range = true;
        break;
    case F51l: {
        uint64_t v = 0;
        for (unsigned i = 0; i < 4; ++i) v |= (uint64_t)unit(code, pc + 1 + i) << (16 * i);
        in.a = aa;
        in.lit = (int64_t)v;
        break;
    }
    default: break;
    }
    return in;
}

/* the instructions that can throw: the ones whose exceptions a try covers */
static bool can_throw(unsigned op) {
    /* the constants that load a class or string, the monitors, casts, allocations and throw; array, field and
       invoke instructions; the integer and long divisions and remainders, in each form */
    return (op >= 0x1a && op <= 0x27) || (op >= 0x44 && op <= 0x78) || op == 0x93 || op == 0x94 || op == 0x9e ||
           op == 0x9f || op == 0xb3 || op == 0xb4 || op == 0xbe || op == 0xbf || op == 0xd3 || op == 0xd4 ||
           op == 0xdb || op == 0xdc || op >= 0xfa;
}
static bool is_invoke(unsigned op) {
    return (op >= 0x6e && op <= 0x72) || (op >= 0x74 && op <= 0x78) || (op >= 0xfa && op <= 0xfd);
}

typedef struct {
    uint32_t pc, at, base; /* the branch's address, where its offset goes in the code and the opcode's offset */
    unsigned width;
    uint32_t target; /* an address */
} Fixup;
typedef struct {
    uint32_t offset;
    bool init;      /* whether this is initialized (in a constructor) */
    unsigned stack; /* a handler's exception class (its constant), 0 for an empty stack */
} Frame;
typedef struct {
    uint32_t pc;       /* the handler's address */
    unsigned type;     /* the class it catches, a constant (0 when it catches all) */
    const char* desc;  /* the same as a descriptor, the frame's */
    uint32_t offset;   /* where its stub is in the code */
} Handler;

/* a constructor the JVM needs that the DEX code does not have: it calls its direct superclass's <init>, which is the
   ancestor a Dalvik call names or one more synthetic constructor on the way to it */
typedef struct {
    char* cls;    /* the class that gets it */
    char* desc;   /* its descriptor */
    char* target; /* the descriptor of the <init> it calls, its superclass's */
} Synthetic;
struct DexProgram {
    const Dex* dexes;
    uint32_t count;
    DexTranslateOptions options;
    Synthetic* synthetics;
    size_t synthetic_count, synthetic_capacity;
};

/* one method's translation */
typedef struct {
    const Dex* dex;
    const DexProgram* program;
    DexProgram* collecting; /* the program whose synthetic constructors this method's calls add to, when it reads the
                               program's methods before translating any */
    CfPool* pool;
    const char* class_desc;
    const char* super_desc; /* NULL for a class without one */
    uint32_t interfaces_off; /* the class's direct superinterfaces */
    uint32_t method_idx, proto_idx;
    bool is_static, is_ctor;
    DexCode code;
    uint32_t n, regs, width; /* insns, registers, a state's words (the registers, the flags, the result) */
    /* per code unit */
    unsigned char* insn_start; /* 1 at each instruction */
    int32_t* block;            /* the block that starts at the unit, -1 */
    uint32_t* label;           /* the code offset of each instruction, and of the end */
    unsigned char* consumed;   /* a move-result its invoke stores */
    uint32_t block_count;
    uint32_t* states;   /* each block's state at its entry */
    unsigned char* reached;
    uint32_t* worklist;
    /* a reference's static types */
    const char** types;
    uint32_t type_count, type_capacity;
    /* the views each register is read as, and their locals */
    unsigned char* reads;
    int32_t* slot;     /* regs * VIEWS */
    uint32_t locals, param_locals;
    /* the handlers */
    Handler* handlers;
    uint32_t handler_count;
    /* emission */
    bool scan, long_branches, changed;
    CfBuffer out;
    int sp, max_sp;
    Fixup* fixups;
    size_t fixup_count, fixup_capacity;
    Frame* frames;
    size_t frame_count, frame_capacity;
    CfBuffer text; /* a descriptor being built */
    bool failed;
    char error[256];
} Method;

static void fail(Method* m, const char* format, ...) {
    if (m->failed) return;
    m->failed = true;
    va_list args;
    va_start(args, format);
    vsnprintf(m->error, sizeof m->error, format, args);
    va_end(args);
}

/* ---- types ---- */

static uint32_t intern(Method* m, const char* desc) {
    if (!desc || !desc[0]) return 0;
    for (uint32_t i = 0; i < m->type_count; ++i)
        if (m->types[i] == desc || !strcmp(m->types[i], desc)) return i + 1;
    if (m->type_count == m->type_capacity) {
        uint32_t capacity = m->type_capacity ? m->type_capacity * 2 : 16;
        const char** types = realloc(m->types, capacity * sizeof *types);
        if (!types) {
            fail(m, "out of memory");
            return 0;
        }
        m->types = types;
        m->type_capacity = capacity;
    }
    m->types[m->type_count++] = desc;
    return m->type_count;
}
static const char* type_of(const Method* m, uint32_t kind) {
    uint32_t t = PAYLOAD(kind);
    return KIND(kind) == K_REF && t && t <= m->type_count ? m->types[t - 1] : NULL;
}
/* the kind a value of descriptor desc has */
static uint32_t kind_of_desc(Method* m, const char* desc) {
    switch (desc[0]) {
    case 'Z': case 'B': case 'S': case 'C': case 'I': return K_INT;
    case 'F': return K_FLOAT;
    case 'J': return K_LONG;
    case 'D': return K_DOUBLE;
    case 'L': case '[': return MAKE(K_REF, intern(m, desc));
    default: return K_TOP;
    }
}
static bool wide_desc(const char* desc) {
    return desc[0] == 'J' || desc[0] == 'D';
}
static unsigned view_of_desc(const char* desc) {
    switch (desc[0]) {
    case 'F': return V_F;
    case 'J': return V_J;
    case 'D': return V_D;
    case 'L': case '[': return V_A;
    default: return V_I;
    }
}

static uint32_t merge_kind(uint32_t a, uint32_t b) {
    if (a == b) return a;
    unsigned ka = KIND(a), kb = KIND(b);
    if (ka == K_TOP || kb == K_TOP) return K_TOP;
    if (kb == K_ZERO || (kb == K_CONST && ka != K_ZERO)) { /* the constant second */
        uint32_t t = a;
        a = b;
        b = t;
        ka = KIND(a);
        kb = KIND(b);
    }
    if (ka == K_ZERO) {
        if (kb == K_CONST || kb == K_INT || kb == K_FLOAT || kb == K_REF) return b;
        return K_TOP;
    }
    if (ka == K_CONST) return kb == K_INT || kb == K_FLOAT ? b : K_TOP;
    if (ka == K_REF && kb == K_REF) return MAKE(K_REF, 0);
    if (ka == K_CONSTW || kb == K_CONSTW) {
        unsigned other = ka == K_CONSTW ? kb : ka;
        return other == K_LONG || other == K_DOUBLE ? MAKE(other, 0) : K_TOP;
    }
    return K_TOP;
}

/* ---- the method's shape: instructions, blocks, handlers ---- */

static bool find_instructions(Method* m) {
    for (uint32_t pc = 0; pc < m->n;) {
        uint32_t payload = payload_units(&m->code, pc);
        if (payload) {
            pc += payload;
            continue;
        }
        Insn in = decode(&m->code, pc);
        m->insn_start[pc] = 1;
        if (pc + in.size > m->n) {
            fail(m, "an instruction runs past the method's end at %u", (unsigned)pc);
            return false;
        }
        pc += in.size;
    }
    return true;
}
static void mark_block(Method* m, uint32_t pc) {
    if (pc < m->n && m->insn_start[pc] && m->block[pc] < 0) m->block[pc] = (int32_t)m->block_count++;
}
static bool branch_target(Method* m, uint32_t from, uint32_t target) {
    if (target >= m->n || !m->insn_start[target]) {
        fail(m, "the branch at %u goes to %u, not an instruction", (unsigned)from, (unsigned)target);
        return false;
    }
    mark_block(m, target);
    return true;
}
/* the targets of a switch at pc: count of them, each into targets (NULL: only count) and its key into keys */
static uint32_t switch_targets(const Method* m, uint32_t pc, const Insn* in, uint32_t* targets, int32_t* keys) {
    uint32_t p = in->target;
    unsigned ident = unit(&m->code, p);
    uint32_t count = unit(&m->code, p + 1);
    if (in->op == 0x2b && ident == 0x0100) {
        int32_t first = (int32_t)(unit(&m->code, p + 2) | (uint32_t)unit(&m->code, p + 3) << 16);
        for (uint32_t i = 0; targets && i < count; ++i) {
            uint32_t at = p + 4 + 2 * i;
            targets[i] = pc + (unit(&m->code, at) | (uint32_t)unit(&m->code, at + 1) << 16);
            keys[i] = (int32_t)((uint32_t)first + i);
        }
        return count;
    }
    if (in->op == 0x2c && ident == 0x0200) {
        for (uint32_t i = 0; targets && i < count; ++i) {
            uint32_t k = p + 2 + 2 * i, t = p + 2 + 2 * count + 2 * i;
            keys[i] = (int32_t)(unit(&m->code, k) | (uint32_t)unit(&m->code, k + 1) << 16);
            targets[i] = pc + (unit(&m->code, t) | (uint32_t)unit(&m->code, t + 1) << 16);
        }
        return count;
    }
    return 0;
}
/* the catch handlers of the try covering pc, through visit; false when the list is bad */
typedef void (*HandlerVisit)(Method* m, uint32_t type_idx, uint32_t handler, void* context);
static bool visit_handlers(Method* m, uint32_t handler_off, HandlerVisit visit, void* context) {
    const unsigned char* end = m->dex->data + m->dex->size;
    const unsigned char* p = m->code.handlers + handler_off;
    if (p >= end) return false;
    int32_t size = dex_sleb(&p, end);
    uint32_t count = size < 0 ? (uint32_t)-(int64_t)size : (uint32_t)size;
    for (uint32_t i = 0; i < count && p < end; ++i) {
        uint32_t type = dex_uleb(&p, end);
        visit(m, type, dex_uleb(&p, end), context);
    }
    if (size <= 0) visit(m, DEX_NO_INDEX, dex_uleb(&p, end), context);
    return p <= end;
}
static void add_handler(Method* m, uint32_t type_idx, uint32_t pc, void* unused) {
    (void)unused;
    if (pc >= m->n || !m->insn_start[pc]) {
        fail(m, "a catch handler at %u is not an instruction", (unsigned)pc);
        return;
    }
    mark_block(m, pc);
    /* after a move-exception, the stub goes on at the next instruction */
    Insn in = decode(&m->code, pc);
    if (in.op == 0x0d) mark_block(m, pc + in.size);
    const char* desc = type_idx == DEX_NO_INDEX ? "Ljava/lang/Throwable;" : dex_type(m->dex, type_idx);
    for (uint32_t i = 0; i < m->handler_count; ++i)
        if (m->handlers[i].pc == pc) {
            if (strcmp(m->handlers[i].desc, desc)) m->handlers[i].desc = "Ljava/lang/Throwable;";
            return;
        }
    Handler* handlers = realloc(m->handlers, (m->handler_count + 1) * sizeof *handlers);
    if (!handlers) {
        fail(m, "out of memory");
        return;
    }
    m->handlers = handlers;
    m->handlers[m->handler_count++] = (Handler){pc, 0, desc, 0};
}
static uint32_t try_start(const Method* m, uint32_t i) {
    const unsigned char* t = m->code.try_items + 8 * i;
    return (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
}
static uint32_t try_count(const Method* m, uint32_t i) {
    const unsigned char* t = m->code.try_items + 8 * i;
    return (uint32_t)t[4] | (uint32_t)t[5] << 8;
}
static uint32_t try_handler(const Method* m, uint32_t i) {
    const unsigned char* t = m->code.try_items + 8 * i;
    return (uint32_t)t[6] | (uint32_t)t[7] << 8;
}
/* the try covering pc, -1 for none */
static int32_t try_of(const Method* m, uint32_t pc) {
    for (uint32_t i = 0; i < m->code.tries; ++i)
        if (pc >= try_start(m, i) && pc - try_start(m, i) < try_count(m, i)) return (int32_t)i;
    return -1;
}
static bool find_blocks(Method* m) {
    mark_block(m, 0);
    for (uint32_t pc = 0; pc < m->n; ++pc) {
        if (!m->insn_start[pc]) continue;
        Insn in = decode(&m->code, pc);
        uint32_t next = pc + in.size;
        unsigned op = in.op;
        if (op >= 0x28 && op <= 0x2a) {
            if (!branch_target(m, pc, in.target)) return false;
            mark_block(m, next);
        } else if (op >= 0x32 && op <= 0x3d) {
            if (!branch_target(m, pc, in.target)) return false;
            mark_block(m, next);
        } else if (op == 0x2b || op == 0x2c) {
            uint32_t count = switch_targets(m, pc, &in, NULL, NULL);
            uint32_t* targets = malloc((count + 1) * sizeof *targets);
            int32_t* keys = malloc((count + 1) * sizeof *keys);
            if (!targets || !keys) {
                free(targets);
                free(keys);
                fail(m, "out of memory");
                return false;
            }
            switch_targets(m, pc, &in, targets, keys);
            bool ok = true;
            for (uint32_t i = 0; ok && i < count; ++i) ok = branch_target(m, pc, targets[i]);
            free(targets);
            free(keys);
            if (!ok) return false;
            mark_block(m, next);
        } else if ((op >= 0x0e && op <= 0x11) || op == 0x27) {
            mark_block(m, next);
        }
    }
    for (uint32_t i = 0; i < m->code.tries && !m->failed; ++i) {
        mark_block(m, try_start(m, i));
        mark_block(m, try_start(m, i) + try_count(m, i));
        if (!visit_handlers(m, try_handler(m, i), add_handler, NULL)) fail(m, "a bad catch handler list");
    }
    return !m->failed;
}

/* ---- dataflow ---- */

static uint32_t* state_of(const Method* m, uint32_t block) {
    return m->states + (size_t)block * m->width;
}
#define FLAGS(m)  ((m)->regs)
#define RESULT(m) ((m)->regs + 1)
#define F_INIT    1u /* this is initialized */

static void merge_into(Method* m, uint32_t block, const uint32_t* state, uint32_t* stack, uint32_t* depth) {
    uint32_t* s = state_of(m, block);
    bool changed = false;
    if (!m->reached[block]) {
        memcpy(s, state, m->width * sizeof *s);
        m->reached[block] = 1;
        changed = true;
    } else {
        for (uint32_t r = 0; r < m->regs; ++r) {
            uint32_t k = merge_kind(s[r], state[r]);
            if (k != s[r]) {
                s[r] = k;
                changed = true;
            }
        }
        uint32_t flags = s[FLAGS(m)] & state[FLAGS(m)];
        uint32_t result = merge_kind(s[RESULT(m)], state[RESULT(m)]);
        if (flags != s[FLAGS(m)] || result != s[RESULT(m)]) changed = true;
        s[FLAGS(m)] = flags;
        s[RESULT(m)] = result;
    }
    if (changed) {
        for (uint32_t i = 0; i < *depth; ++i)
            if (stack[i] == block) return;
        stack[(*depth)++] = block;
    }
}
static void set_reg(Method* m, uint32_t* s, uint32_t r, uint32_t kind) {
    if (r >= m->regs) return;
    /* a write to the second half of a pair breaks the pair */
    if (r > 0 && KIND(s[r - 1]) >= K_LONG && KIND(s[r - 1]) <= K_CONSTW) s[r - 1] = K_TOP;
    s[r] = kind;
}
static void set_wide(Method* m, uint32_t* s, uint32_t r, uint32_t kind) {
    set_reg(m, s, r, kind);
    if (r + 1 < m->regs) s[r + 1] = K_HI;
}
static const char* field_type(const Method* m, uint32_t idx, const char** owner, const char** name) {
    uint32_t class_idx, type_idx;
    const char* n;
    dex_field(m->dex, idx, &class_idx, &type_idx, &n);
    if (owner) *owner = dex_type(m->dex, class_idx);
    if (name) *name = n;
    return dex_type(m->dex, type_idx);
}
static const char* method_return(const Method* m, uint32_t idx) {
    uint32_t class_idx, proto_idx;
    const char* name;
    dex_method(m->dex, idx, &class_idx, &proto_idx, &name);
    return dex_type(m->dex, dex_proto_return(m->dex, proto_idx));
}
/* the kind of an element of the array of kind array_kind, for an aget of op */
static uint32_t element_kind(Method* m, uint32_t array_kind, unsigned op) {
    const char* t = type_of(m, array_kind);
    switch (op) {
    case 0x44: return t && t[0] == '[' && t[1] == 'F' ? K_FLOAT : K_INT;
    case 0x45: return t && t[0] == '[' && t[1] == 'D' ? K_DOUBLE : K_LONG;
    case 0x46: return t && t[0] == '[' ? kind_of_desc(m, t + 1) : MAKE(K_REF, 0);
    default: return K_INT;
    }
}
static uint32_t unary_kind(unsigned op) {
    static const unsigned char kinds[] = {K_INT,    K_INT,    K_LONG,   K_LONG,  K_FLOAT,  K_DOUBLE, K_LONG,
                                          K_FLOAT,  K_DOUBLE, K_INT,    K_FLOAT, K_DOUBLE, K_INT,    K_LONG,
                                          K_DOUBLE, K_INT,    K_LONG,   K_FLOAT, K_INT,    K_INT,    K_INT};
    return kinds[op - 0x7b];
}
static uint32_t binary_kind(unsigned op) {
    unsigned o = op >= 0xb0 ? op - 0x20 : op; /* the 2addr forms in the same order */
    return o <= 0x9a ? K_INT : o <= 0xa5 ? K_LONG : o <= 0xaa ? K_FLOAT : K_DOUBLE;
}
static void transfer(Method* m, uint32_t pc, const Insn* in, uint32_t* s) {
    unsigned op = in->op;
    uint32_t result = K_TOP;
    switch (op) {
    case 0x01: case 0x02: case 0x03: case 0x07: case 0x08: case 0x09:
        set_reg(m, s, in->a, in->b < m->regs ? s[in->b] : K_TOP);
        break;
    case 0x04: case 0x05: case 0x06: {
        uint32_t lo = in->b < m->regs ? s[in->b] : K_TOP;
        set_wide(m, s, in->a, lo);
        break;
    }
    case 0x0a: case 0x0c: set_reg(m, s, in->a, s[RESULT(m)]); break;
    case 0x0b: set_wide(m, s, in->a, s[RESULT(m)]); break;
    case 0x0d: set_reg(m, s, in->a, s[RESULT(m)]); break;
    case 0x12: case 0x13: case 0x14: case 0x15: set_reg(m, s, in->a, in->lit ? K_CONST : K_ZERO); break;
    case 0x16: case 0x17: case 0x18: case 0x19: set_wide(m, s, in->a, K_CONSTW); break;
    case 0x1a: case 0x1b: set_reg(m, s, in->a, MAKE(K_REF, intern(m, "Ljava/lang/String;"))); break;
    case 0x1c: set_reg(m, s, in->a, MAKE(K_REF, intern(m, "Ljava/lang/Class;"))); break;
    case 0x1f: set_reg(m, s, in->a, MAKE(K_REF, intern(m, dex_type(m->dex, in->idx)))); break;
    case 0x20: case 0x21: set_reg(m, s, in->a, K_INT); break;
    case 0x22: set_reg(m, s, in->a, MAKE(K_UNINIT, pc)); break;
    case 0x23: set_reg(m, s, in->a, MAKE(K_REF, intern(m, dex_type(m->dex, in->idx)))); break;
    case 0x24: case 0x25: result = MAKE(K_REF, intern(m, dex_type(m->dex, in->idx))); break;
    case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: set_reg(m, s, in->a, K_INT); break;
    case 0x44: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:
        set_reg(m, s, in->a, element_kind(m, in->b < m->regs ? s[in->b] : K_TOP, op));
        break;
    case 0x45: set_wide(m, s, in->a, element_kind(m, in->b < m->regs ? s[in->b] : K_TOP, op)); break;
    default:
        if ((op >= 0x52 && op <= 0x58) || (op >= 0x60 && op <= 0x66)) {
            const char* t = field_type(m, in->idx, NULL, NULL);
            if (wide_desc(t))
                set_wide(m, s, in->a, kind_of_desc(m, t));
            else
                set_reg(m, s, in->a, kind_of_desc(m, t));
        } else if (is_invoke(op) && op < 0xfa) {
            uint32_t class_idx, proto_idx;
            const char* name;
            dex_method(m->dex, in->idx, &class_idx, &proto_idx, &name);
            result = kind_of_desc(m, dex_type(m->dex, dex_proto_return(m->dex, proto_idx)));
            if ((op == 0x70 || op == 0x76) && !strcmp(name, "<init>") && in->argc) {
                uint32_t self = arg_reg(in, 0), k = self < m->regs ? s[self] : K_TOP;
                if (KIND(k) == K_UNINIT) {
                    Insn site = decode(&m->code, PAYLOAD(k));
                    uint32_t made = MAKE(K_REF, intern(m, dex_type(m->dex, site.idx)));
                    for (uint32_t r = 0; r < m->regs; ++r)
                        if (s[r] == k) s[r] = made;
                } else if (KIND(k) == K_UNINIT_THIS) {
                    uint32_t made = MAKE(K_REF, intern(m, m->class_desc));
                    for (uint32_t r = 0; r < m->regs; ++r)
                        if (KIND(s[r]) == K_UNINIT_THIS) s[r] = made;
                    s[FLAGS(m)] |= F_INIT;
                }
            }
        } else if (op >= 0x7b && op <= 0x8f) {
            uint32_t k = unary_kind(op);
            if (k == K_LONG || k == K_DOUBLE)
                set_wide(m, s, in->a, k);
            else
                set_reg(m, s, in->a, k);
        } else if (op >= 0x90 && op <= 0xcf) {
            uint32_t k = binary_kind(op);
            if (k == K_LONG || k == K_DOUBLE)
                set_wide(m, s, in->a, k);
            else
                set_reg(m, s, in->a, k);
        } else if (op >= 0xd0 && op <= 0xe2) {
            set_reg(m, s, in->a, K_INT);
        } else if (op >= 0xfa) {
            if (op == 0xfe || op == 0xff) set_reg(m, s, in->a, MAKE(K_REF, 0));
        }
        break;
    }
    s[RESULT(m)] = result;
}
typedef struct {
    uint32_t* state;
    uint32_t* stack;
    uint32_t* depth;
} ExceptionEdge;
static void exception_edge(Method* m, uint32_t type_idx, uint32_t handler, void* context) {
    ExceptionEdge* e = context;
    if (handler >= m->n || m->block[handler] < 0) return;
    uint32_t saved = e->state[RESULT(m)];
    const char* desc = type_idx == DEX_NO_INDEX ? "Ljava/lang/Throwable;" : dex_type(m->dex, type_idx);
    e->state[RESULT(m)] = MAKE(K_REF, intern(m, desc));
    merge_into(m, (uint32_t)m->block[handler], e->state, e->stack, e->depth);
    e->state[RESULT(m)] = saved;
}
static bool dataflow(Method* m, const uint32_t* entry) {
    uint32_t depth = 0;
    uint32_t* s = malloc(m->width * sizeof *s);
    uint32_t* starts = malloc((m->block_count + 1) * sizeof *starts);
    if (!s || !starts) {
        free(s);
        free(starts);
        fail(m, "out of memory");
        return false;
    }
    for (uint32_t pc = 0; pc < m->n; ++pc)
        if (m->block[pc] >= 0) starts[m->block[pc]] = pc;
    merge_into(m, 0, entry, m->worklist, &depth);
    while (depth && !m->failed) {
        uint32_t block = m->worklist[--depth];
        memcpy(s, state_of(m, block), m->width * sizeof *s);
        uint32_t pc = starts[block];
        for (;;) {
            Insn in = decode(&m->code, pc);
            int32_t t = try_of(m, pc);
            if (t >= 0 && can_throw(in.op)) {
                ExceptionEdge e = {s, m->worklist, &depth};
                visit_handlers(m, try_handler(m, (uint32_t)t), exception_edge, &e);
            }
            transfer(m, pc, &in, s);
            unsigned op = in.op;
            uint32_t next = pc + in.size;
            bool falls = true;
            if (op >= 0x28 && op <= 0x2a) {
                merge_into(m, (uint32_t)m->block[in.target], s, m->worklist, &depth);
                falls = false;
            } else if (op >= 0x32 && op <= 0x3d) {
                merge_into(m, (uint32_t)m->block[in.target], s, m->worklist, &depth);
            } else if (op == 0x2b || op == 0x2c) {
                uint32_t count = switch_targets(m, pc, &in, NULL, NULL);
                uint32_t* targets = malloc((count + 1) * sizeof *targets);
                int32_t* keys = malloc((count + 1) * sizeof *keys);
                if (targets && keys) {
                    switch_targets(m, pc, &in, targets, keys);
                    for (uint32_t i = 0; i < count; ++i)
                        merge_into(m, (uint32_t)m->block[targets[i]], s, m->worklist, &depth);
                } else {
                    fail(m, "out of memory");
                }
                free(targets);
                free(keys);
            } else if ((op >= 0x0e && op <= 0x11) || op == 0x27) {
                falls = false;
            }
            if (!falls) break;
            if (next >= m->n) {
                fail(m, "the code runs past the method's end after %u", (unsigned)pc);
                break;
            }
            if (m->block[next] >= 0) {
                merge_into(m, (uint32_t)m->block[next], s, m->worklist, &depth);
                break;
            }
            pc = next;
        }
    }
    free(s);
    free(starts);
    return !m->failed;
}

/* ---- emission ---- */

static void grow_stack(Method* m, int delta) {
    m->sp += delta;
    if (m->sp > m->max_sp) m->max_sp = m->sp;
}
static void op1(Method* m, unsigned op, int delta) {
    if (!m->scan) cf_u1(&m->out, op);
    grow_stack(m, delta);
}
static void op_u1(Method* m, unsigned op, unsigned operand, int delta) {
    if (!m->scan) {
        cf_u1(&m->out, op);
        cf_u1(&m->out, operand);
    }
    grow_stack(m, delta);
}
static void op_u2(Method* m, unsigned op, unsigned operand, int delta) {
    if (!m->scan) {
        cf_u1(&m->out, op);
        cf_u2(&m->out, operand);
    }
    grow_stack(m, delta);
}
static void local_op(Method* m, unsigned op, uint32_t local, int delta) {
    if (local > 255) {
        if (!m->scan) {
            cf_u1(&m->out, J_WIDE);
            cf_u1(&m->out, op);
            cf_u2(&m->out, local);
        }
        grow_stack(m, delta);
    } else {
        op_u1(m, op, local, delta);
    }
}
static unsigned pool_check(Method* m, unsigned index) {
    if (!index && !m->scan) fail(m, "the constant pool is full");
    return index;
}
static void push_int(Method* m, int32_t v) {
    if (v >= -1 && v <= 5)
        op1(m, (unsigned)(J_ICONST_0 + v), 1);
    else if (v >= -128 && v <= 127)
        op_u1(m, J_BIPUSH, (unsigned)v & 0xff, 1);
    else if (v >= -32768 && v <= 32767)
        op_u2(m, J_SIPUSH, (unsigned)v & 0xffff, 1);
    else if (!m->scan) {
        unsigned c = pool_check(m, cf_integer(m->pool, (uint32_t)v));
        if (c < 256) op_u1(m, J_LDC, c, 1); else op_u2(m, J_LDC_W, c, 1);
    } else {
        grow_stack(m, 1);
    }
}
static void push_float(Method* m, uint32_t bits) {
    if (bits == 0)
        op1(m, J_FCONST_0, 1);
    else if (bits == 0x3f800000u)
        op1(m, J_FCONST_0 + 1, 1);
    else if (bits == 0x40000000u)
        op1(m, J_FCONST_0 + 2, 1);
    else if (!m->scan) {
        unsigned c = pool_check(m, cf_float(m->pool, bits));
        if (c < 256) op_u1(m, J_LDC, c, 1); else op_u2(m, J_LDC_W, c, 1);
    } else {
        grow_stack(m, 1);
    }
}
static void push_long(Method* m, uint64_t bits) {
    if (bits <= 1)
        op1(m, J_LCONST_0 + (unsigned)bits, 2);
    else
        op_u2(m, J_LDC2_W, m->scan ? 0 : pool_check(m, cf_long(m->pool, bits)), 2);
}
static void push_double(Method* m, uint64_t bits) {
    if (bits == 0)
        op1(m, J_DCONST_0, 2);
    else if (bits == 0x3ff0000000000000ull)
        op1(m, J_DCONST_0 + 1, 2);
    else
        op_u2(m, J_LDC2_W, m->scan ? 0 : pool_check(m, cf_double(m->pool, bits)), 2);
}
static void push_string(Method* m, const char* text) {
    unsigned c = m->scan ? 0 : pool_check(m, cf_string(m->pool, text));
    if (c && c < 256) op_u1(m, J_LDC, c, 1); else op_u2(m, J_LDC_W, c, 1);
}
static unsigned class_ref(Method* m, const char* desc) {
    return m->scan ? 0 : pool_check(m, cf_class_descriptor(m->pool, desc));
}
static bool is_interface(const Method* m, const char* desc);
/* A cast that only the JVM's verifier needs, to a value's narrower class where it is used (DEX's own check-cast is
   not this one). The verifier takes any reference where an interface type is wanted, as ART's does, so a value of
   an interface type gets no cast: R8 leaves classes that the code stores in a field of an interface type they no
   longer implement, and a cast to it throws where Android's code runs on */
static void checkcast(Method* m, const char* desc) {
    if (!desc || !strcmp(desc, "Ljava/lang/Object;") || (desc[0] == 'L' && is_interface(m, desc))) return;
    op_u2(m, J_CHECKCAST, class_ref(m, desc), 0);
}

/* the current state of the registers, during the walk */
static uint32_t kind_at(const Method* m, const uint32_t* s, uint32_t r) {
    return r < m->regs ? s[r] : K_TOP;
}
static int32_t slot_of(Method* m, uint32_t r, unsigned view) {
    return r < m->regs ? m->slot[r * VIEWS + view] : -1;
}
static void load(Method* m, const uint32_t* s, unsigned view, uint32_t r) {
    if (view == V_A && KIND(kind_at(m, s, r)) == K_UNINIT_THIS) {
        op1(m, J_ALOAD_0, 1);
        return;
    }
    if (r >= m->regs) {
        fail(m, "register v%u past the method's %u", (unsigned)r, (unsigned)m->regs);
        return;
    }
    if (m->scan) {
        if (!(m->reads[r] & VIEW_BIT(view))) {
            m->reads[r] |= (unsigned char)VIEW_BIT(view);
            m->changed = true;
        }
        grow_stack(m, (int)view_width[view]);
        return;
    }
    local_op(m, view_load[view], (uint32_t)slot_of(m, r, view), (int)view_width[view]);
}
static void store(Method* m, unsigned view, uint32_t r) {
    int width = (int)view_width[view];
    if (m->scan || r >= m->regs || slot_of(m, r, view) < 0) {
        op1(m, width == 2 ? J_POP2 : J_POP, -width);
        return;
    }
    local_op(m, view_store[view], (uint32_t)slot_of(m, r, view), -width);
}
/* the views a value of kind has, which a move copies */
static unsigned views_of_kind(uint32_t kind) {
    switch (KIND(kind)) {
    case K_INT: return VIEW_BIT(V_I);
    case K_FLOAT: return VIEW_BIT(V_F);
    case K_CONST: return VIEW_BIT(V_I) | VIEW_BIT(V_F);
    case K_ZERO: return VIEW_BIT(V_I) | VIEW_BIT(V_F) | VIEW_BIT(V_A);
    case K_REF: return VIEW_BIT(V_A);
    case K_LONG: return VIEW_BIT(V_J);
    case K_DOUBLE: return VIEW_BIT(V_D);
    case K_CONSTW: return VIEW_BIT(V_J) | VIEW_BIT(V_D);
    default: return 0;
    }
}
/* a move of the views dst is read as from src's */
static void move(Method* m, const uint32_t* s, uint32_t dst, uint32_t src, unsigned views) {
    if (dst >= m->regs || src >= m->regs) {
        fail(m, "a move between registers past the method's");
        return;
    }
    unsigned wanted = m->reads[dst] & views & views_of_kind(s[src]);
    for (unsigned v = 0; v < VIEWS; ++v)
        if (wanted & VIEW_BIT(v)) {
            load(m, s, v, src);
            store(m, v, dst);
        }
}
static void branch(Method* m, unsigned op, uint32_t pc, uint32_t target) {
    bool conditional = op != J_GOTO;
    if (m->long_branches) {
        if (conditional) {
            /* the opposite test over a goto_w */
            unsigned inverse = op >= J_IFNULL ? op ^ 1u : (((op - J_IFEQ) ^ 1u) + J_IFEQ);
            op_u2(m, inverse, 8, 0);
        }
        if (!m->scan) {
            size_t base = m->out.size;
            cf_u1(&m->out, J_GOTO_W);
            cf_u4(&m->out, 0);
            if (m->fixup_count == m->fixup_capacity) {
                size_t capacity = m->fixup_capacity ? m->fixup_capacity * 2 : 64;
                Fixup* fixups = realloc(m->fixups, capacity * sizeof *fixups);
                if (!fixups) {
                    fail(m, "out of memory");
                    return;
                }
                m->fixups = fixups;
                m->fixup_capacity = capacity;
            }
            m->fixups[m->fixup_count++] = (Fixup){pc, (uint32_t)base + 1, (uint32_t)base, 4, target};
        }
        return;
    }
    if (m->scan) return;
    size_t base = m->out.size;
    cf_u1(&m->out, op);
    cf_u2(&m->out, 0);
    if (m->fixup_count == m->fixup_capacity) {
        size_t capacity = m->fixup_capacity ? m->fixup_capacity * 2 : 64;
        Fixup* fixups = realloc(m->fixups, capacity * sizeof *fixups);
        if (!fixups) {
            fail(m, "out of memory");
            return;
        }
        m->fixups = fixups;
        m->fixup_capacity = capacity;
    }
    m->fixups[m->fixup_count++] = (Fixup){pc, (uint32_t)base + 1, (uint32_t)base, 2, target};
}
static void add_fixup(Method* m, uint32_t pc, size_t at, size_t base, uint32_t target) {
    if (m->scan) return;
    if (m->fixup_count == m->fixup_capacity) {
        size_t capacity = m->fixup_capacity ? m->fixup_capacity * 2 : 64;
        Fixup* fixups = realloc(m->fixups, capacity * sizeof *fixups);
        if (!fixups) {
            fail(m, "out of memory");
            return;
        }
        m->fixups = fixups;
        m->fixup_capacity = capacity;
    }
    m->fixups[m->fixup_count++] = (Fixup){pc, (uint32_t)at, (uint32_t)base, 4, target};
}
static void add_frame(Method* m, uint32_t offset, bool init, unsigned stack) {
    if (m->scan) return;
    if (m->frame_count && m->frames[m->frame_count - 1].offset == offset) return;
    if (m->frame_count == m->frame_capacity) {
        size_t capacity = m->frame_capacity ? m->frame_capacity * 2 : 64;
        Frame* frames = realloc(m->frames, capacity * sizeof *frames);
        if (!frames) {
            fail(m, "out of memory");
            return;
        }
        m->frames = frames;
        m->frame_capacity = capacity;
    }
    m->frames[m->frame_count++] = (Frame){offset, init, stack};
}

/* the JVM descriptor of a method, in m->text: the DEX prototype's types in the JVM's syntax */
static const char* method_descriptor(Method* m, uint32_t proto_idx) {
    m->text.size = 0;
    cf_u1(&m->text, '(');
    uint32_t count = dex_proto_param_count(m->dex, proto_idx);
    for (uint32_t i = 0; i < count; ++i) {
        const char* t = dex_type(m->dex, dex_proto_param(m->dex, proto_idx, i));
        cf_bytes(&m->text, t, strlen(t));
    }
    cf_u1(&m->text, ')');
    const char* r = dex_type(m->dex, dex_proto_return(m->dex, proto_idx));
    cf_bytes(&m->text, r, strlen(r) + 1);
    if (m->text.failed) {
        fail(m, "out of memory");
        return "()V";
    }
    return (const char*)m->text.data;
}
static bool is_interface(const Method* m, const char* desc) {
    uint32_t dex, index;
    if (dex_program_find(m->program, desc, &dex, &index))
        return (dex_class_def(&m->program->dexes[dex], index).access & 0x200) != 0;
    const DexTranslateOptions* o = &m->program->options;
    return o->is_interface && o->is_interface(o->context, desc);
}
/* the superclass of a class of the program, NULL for one outside it or without one */
static const char* program_super(const DexProgram* program, const char* desc) {
    uint32_t dex, index;
    if (!dex_program_find(program, desc, &dex, &index)) return NULL;
    DexClassDef def = dex_class_def(&program->dexes[dex], index);
    return def.super_idx == DEX_NO_INDEX ? NULL : dex_type(&program->dexes[dex], def.super_idx);
}
static char* copy_text(const char* text) {
    size_t n = strlen(text) + 1;
    char* copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}
/* one synthetic constructor, once */
static bool add_synthetic(DexProgram* p, const char* cls, const char* desc, const char* target) {
    for (size_t i = 0; i < p->synthetic_count; ++i)
        if (!strcmp(p->synthetics[i].cls, cls) && !strcmp(p->synthetics[i].desc, desc)) return true;
    if (p->synthetic_count == p->synthetic_capacity) {
        size_t capacity = p->synthetic_capacity ? p->synthetic_capacity * 2 : 32;
        Synthetic* list = realloc(p->synthetics, capacity * sizeof *list);
        if (!list) return false;
        p->synthetics = list;
        p->synthetic_capacity = capacity;
    }
    Synthetic* y = &p->synthetics[p->synthetic_count];
    y->cls = copy_text(cls);
    y->desc = copy_text(desc);
    y->target = copy_text(target);
    if (!y->cls || !y->desc || !y->target) {
        free(y->cls);
        free(y->desc);
        free(y->target);
        return false;
    }
    ++p->synthetic_count;
    return true;
}
/* whether class desc is a class of the program with ancestor among its superclasses (or ancestor itself) */
static bool program_subclass(const DexProgram* program, const char* desc, const char* ancestor) {
    for (unsigned depth = 0; desc && depth < 256; ++depth) {
        if (!strcmp(desc, ancestor)) return true;
        desc = program_super(program, desc);
    }
    return false;
}
/* whether iface is one of the method's class's direct superinterfaces */
static bool direct_interface(const Method* m, const char* iface) {
    uint32_t count = dex_type_list_count(m->dex, m->interfaces_off);
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(dex_type(m->dex, dex_type_list_item(m->dex, m->interfaces_off, i)), iface)) return true;
    return false;
}
/* the class a receiver is cast to for a member of owner. R8 rebinds a reference to the class that declares the
   member, and the JVM allows a protected member of a class of another package only on the current class or its
   subclasses: the receiver's own static type, when the dataflow knows it and it is a class of the program. ART
   verified that the type is assignable to owner, also when the chain between them leaves the program (an app's
   ViewGroup, a View member), so a class of the program below owner or below a class outside it is safe */
static const char* receiver_type(const Method* m, uint32_t kind, const char* owner) {
    const char* t = type_of(m, kind);
    if (!t || t[0] != 'L' || !strcmp(t, owner)) return owner;
    uint32_t dex, index;
    if (program_subclass(m->program, t, owner)) return t;
    return dex_program_find(m->program, t, &dex, &index) && !is_interface(m, t) && !dex_program_find(m->program, owner, &dex, &index)
               ? t : owner;
}
/* A call of ancestor's <init> of descriptor desc on an instance of start, which the JVM only allows of start's own
   constructor (a new instance's) or its superclass's (a constructor's this): the descriptor of start's synthetic
   constructor that leads to it, the caller's to free. It has two more parameters, a marker class's and the
   ancestor's, so that it is never one of the class's own. Reading the program adds the synthetic constructors of
   start and of each class between it and ancestor */
static char* synthetic_target(Method* m, const char* start, const char* ancestor, const char* desc) {
    size_t n = strlen(desc);
    const char* close = strrchr(desc, ')');
    size_t params = close ? (size_t)(close - desc) : n;
    static const char marker[] = "Lsnx/dalvik/Init;";
    size_t size = n + sizeof marker + strlen(ancestor) + 1;
    char* out = malloc(size);
    if (!out) {
        fail(m, "out of memory");
        return NULL;
    }
    snprintf(out, size, "%.*s%s%s%s", (int)params, desc, marker, ancestor, desc + params);
    if (m->collecting) {
        for (const char* cls = start; cls;) {
            const char* parent = program_super(m->program, cls);
            if (!parent) {
                fail(m, "%s's <init> is called on a %s, and a class between them is not the app's", ancestor, start);
                break;
            }
            bool last = !strcmp(parent, ancestor);
            if (!add_synthetic(m->collecting, cls, out, last ? desc : out)) {
                fail(m, "out of memory");
                break;
            }
            cls = last ? NULL : parent;
        }
    }
    return out;
}
static int desc_slots(const char* desc) {
    return desc[0] == 'V' ? 0 : wide_desc(desc) ? 2 : 1;
}

/* the store of an invoke's result, into the move-result after it */
static void store_result(Method* m, uint32_t pc, const Insn* in, const char* ret) {
    uint32_t next = pc + in->size;
    Insn after = next < m->n && m->insn_start[next] ? decode(&m->code, next) : (Insn){0};
    int slots = desc_slots(ret);
    if (next < m->n && m->insn_start[next] && after.op >= 0x0a && after.op <= 0x0c) {
        m->consumed[next] = 1;
        if (slots) store(m, view_of_desc(ret), after.a);
    } else if (slots) {
        op1(m, slots == 2 ? J_POP2 : J_POP, -slots);
    }
}

/* Calls of the JDK that behave otherwise on Android, sent to the runtime's own. An instance method becomes a static
   one with the receiver as its first parameter. System.loadLibrary must load the app's native libraries through the
   native bridge (docs/android/NATIVE.md), not HotSpot's loader. ByteBuffer.allocateDirect gives a buffer that native
   code reaches, which for 32-bit libraries must be in their window */
typedef struct {
    const char *owner, *name, *desc;
    const char *to_owner, *to_name;
} Remap;
static const Remap remaps[] = {
    {"Ljava/lang/System;", "loadLibrary", "(Ljava/lang/String;)V", "Lsnx/dalvik/Runtime;", "loadLibrary"},
    {"Ljava/lang/System;", "load", "(Ljava/lang/String;)V", "Lsnx/dalvik/Runtime;", "load"},
    {"Ljava/lang/Runtime;", "loadLibrary", "(Ljava/lang/String;)V", "Lsnx/dalvik/Runtime;", "runtimeLoadLibrary"},
    {"Ljava/lang/Runtime;", "load", "(Ljava/lang/String;)V", "Lsnx/dalvik/Runtime;", "runtimeLoad"},
    {"Ljava/nio/ByteBuffer;", "allocateDirect", "(I)Ljava/nio/ByteBuffer;", "Lsnx/dalvik/Runtime;", "allocateDirect"},
};
static const Remap* remap_of(const char* owner, const char* name, const char* desc) {
    for (size_t i = 0; i < sizeof remaps / sizeof *remaps; ++i)
        if (!strcmp(remaps[i].owner, owner) && !strcmp(remaps[i].name, name) && !strcmp(remaps[i].desc, desc))
            return &remaps[i];
    return NULL;
}

static void emit_invoke(Method* m, uint32_t pc, const Insn* in, const uint32_t* s) {
    unsigned op = in->op >= 0x74 ? in->op - 6 : in->op;
    uint32_t class_idx, proto_idx;
    const char* name;
    dex_method(m->dex, in->idx, &class_idx, &proto_idx, &name);
    const char* owner = dex_type(m->dex, class_idx);
    const char* built = method_descriptor(m, proto_idx);
    size_t built_size = strlen(built) + 1;
    char* desc = malloc(built_size);
    if (desc) memcpy(desc, built, built_size);
    if (!desc) {
        fail(m, "out of memory");
        return;
    }
    bool is_static = op == 0x71;
    unsigned jop = op == 0x6e ? J_INVOKEVIRTUAL : op == 0x71 ? J_INVOKESTATIC : op == 0x72 ? J_INVOKEINTERFACE
                                                                                           : J_INVOKESPECIAL;
    unsigned tag = op == 0x72 ? CF_INTERFACE_METHODREF
                   : op != 0x6e && is_interface(m, owner) ? CF_INTERFACE_METHODREF : CF_METHODREF;
    if (op == 0x6f && m->super_desc) {
        /* invoke-super: a direct superinterface's default method, or what resolution from the superclass finds,
           which is where ART looks for a method of another interface or an ancestor. The JVM allows an
           invokespecial only of the superclass's and a direct superinterface's */
        bool direct = direct_interface(m, owner);
        if (!direct) owner = m->super_desc;
        tag = direct ? CF_INTERFACE_METHODREF : CF_METHODREF;
    }
    unsigned reg = 0, arg_slots = 0;
    uint32_t uninit = K_TOP;
    char* redirect = NULL; /* the synthetic constructor a call goes to instead */
    bool clone_helper = false; /* Object.clone of a receiver the JVM's verifier would refuse it on */
    if (!is_static) {
        if (!in->argc) {
            fail(m, "an invoke with no receiver at %u", (unsigned)pc);
            free(desc);
            return;
        }
        uint32_t self = arg_reg(in, 0), k = kind_at(m, s, self);
        if (op == 0x70 && !strcmp(name, "<init>") && KIND(k) == K_UNINIT) {
            Insn site = decode(&m->code, PAYLOAD(k));
            const char* made = dex_type(m->dex, site.idx);
            if (strcmp(made, owner)) {
                redirect = synthetic_target(m, made, owner, desc);
                owner = made;
            }
            op_u2(m, J_NEW, class_ref(m, made), 1);
            op1(m, J_DUP, 1);
            uninit = k;
        } else if (op == 0x70 && !strcmp(name, "<init>") && KIND(k) == K_UNINIT_THIS) {
            if (strcmp(owner, m->class_desc) && (!m->super_desc || strcmp(owner, m->super_desc))) {
                if (m->super_desc) redirect = synthetic_target(m, m->super_desc, owner, desc);
                if (!m->super_desc) fail(m, "a constructor of a class without a superclass calls %s's", owner);
                owner = m->super_desc ? m->super_desc : owner;
            }
            op1(m, J_ALOAD_0, 1);
            uninit = k;
        } else if (op == 0x6e && !strcmp(owner, "Ljava/lang/Object;") && !strcmp(name, "clone") &&
                   !strcmp(desc, "()Ljava/lang/Object;")) {
            /* Object.clone, protected: the JVM allows it of another class's object only on an array, whose clone is
               public, and ART allows it on any array (R8 rebinds an array's clone to Object's). An array's clone
               is called on its array type; a receiver whose type is not known to be the caller's class goes to
               Runtime.cloneObject */
            load(m, s, V_A, self);
            const char* t = type_of(m, k);
            if (t && t[0] == '[') {
                checkcast(m, t);
                owner = t;
            } else if (!t || t[0] != 'L' || !program_subclass(m->program, t, m->class_desc)) {
                clone_helper = true;
            } else {
                checkcast(m, t);
            }
        } else {
            load(m, s, V_A, self);
            checkcast(m, op == 0x6f ? m->class_desc : op == 0x72 ? owner : receiver_type(m, k, owner));
        }
        reg = 1;
        arg_slots = 1;
    }
    uint32_t count = dex_proto_param_count(m->dex, proto_idx);
    for (uint32_t i = 0; i < count && !m->failed; ++i) {
        const char* t = dex_type(m->dex, dex_proto_param(m->dex, proto_idx, i));
        if (reg >= in->argc) {
            fail(m, "an invoke with too few registers at %u", (unsigned)pc);
            break;
        }
        uint32_t r = arg_reg(in, reg);
        unsigned view = view_of_desc(t);
        load(m, s, view, r);
        if (view == V_A) checkcast(m, t);
        reg += wide_desc(t) ? 2 : 1;
        arg_slots += wide_desc(t) ? 2 : 1;
    }
    if (redirect) { /* the synthetic constructor's two markers */
        op1(m, J_ACONST_NULL, 1);
        op1(m, J_ACONST_NULL, 1);
        arg_slots += 2;
        free(desc);
        desc = redirect;
    }
    const Remap* remap = remap_of(dex_type(m->dex, class_idx), name, desc);
    if (remap && !redirect) {
        /* the receiver, loaded already, becomes the first parameter */
        size_t n = strlen(desc) + strlen(remap->owner) + 1;
        char* to = malloc(n);
        if (!to) {
            fail(m, "out of memory");
            free(desc);
            return;
        }
        snprintf(to, n, "(%s%s", is_static ? "" : remap->owner, desc + 1);
        free(desc);
        desc = to;
        owner = remap->to_owner;
        name = remap->to_name;
        jop = J_INVOKESTATIC;
        tag = CF_METHODREF;
    }
    if (clone_helper) {
        static const char helper[] = "(Ljava/lang/Object;)Ljava/lang/Object;";
        char* to = malloc(sizeof helper);
        if (!to) {
            fail(m, "out of memory");
            free(desc);
            return;
        }
        memcpy(to, helper, sizeof helper);
        free(desc);
        desc = to;
        owner = "Lsnx/dalvik/Runtime;";
        name = "cloneObject";
        jop = J_INVOKESTATIC;
        tag = CF_METHODREF;
    }
    const char* ret = desc + strlen(desc) - 1;
    while (ret > desc && ret[-1] != ')') --ret;
    unsigned ref = m->scan ? 0 : pool_check(m, cf_member(m->pool, tag, owner, name, desc));
    int delta = desc_slots(ret) - (int)arg_slots;
    if (jop == J_INVOKEINTERFACE) {
        if (!m->scan) {
            cf_u1(&m->out, jop);
            cf_u2(&m->out, ref);
            cf_u1(&m->out, arg_slots);
            cf_u1(&m->out, 0);
        }
        grow_stack(m, delta);
    } else {
        op_u2(m, jop, ref, delta);
    }
    if (KIND(uninit) == K_UNINIT) {
        /* the new object into each register that holds it */
        uint32_t holders = 0;
        for (uint32_t r = 0; r < m->regs; ++r)
            if (s[r] == uninit && slot_of(m, r, V_A) >= 0) ++holders;
        if (m->scan) holders = 0;
        if (!holders) op1(m, J_POP, -1);
        for (uint32_t r = 0; r < m->regs && holders; ++r)
            if (s[r] == uninit && slot_of(m, r, V_A) >= 0) {
                if (--holders) op1(m, J_DUP, 1);
                store(m, V_A, r);
            }
    } else if (KIND(uninit) == K_UNINIT_THIS) {
        for (uint32_t r = 0; r < m->regs; ++r)
            if (KIND(s[r]) == K_UNINIT_THIS && slot_of(m, r, V_A) >= 0) {
                op1(m, J_ALOAD_0, 1);
                store(m, V_A, r);
            }
    }
    store_result(m, pc, in, ret);
    free(desc);
}

static void emit_field(Method* m, const Insn* in, const uint32_t* s) {
    unsigned op = in->op;
    const char *owner, *name;
    const char* type = field_type(m, in->idx, &owner, &name);
    bool is_static = op >= 0x60, is_put = is_static ? op >= 0x67 : op >= 0x59;
    unsigned view = view_of_desc(type);
    int width = (int)view_width[view];
    unsigned ref = m->scan ? 0 : pool_check(m, cf_member(m->pool, CF_FIELDREF, owner, name, type));
    if (!is_static) {
        load(m, s, V_A, in->b);
        if (KIND(kind_at(m, s, in->b)) != K_UNINIT_THIS) checkcast(m, receiver_type(m, kind_at(m, s, in->b), owner));
    }
    if (is_put) {
        load(m, s, view, in->a);
        if (view == V_A) checkcast(m, type);
        op_u2(m, is_static ? J_PUTSTATIC : J_PUTFIELD, ref, -width - (is_static ? 0 : 1));
    } else {
        op_u2(m, is_static ? J_GETSTATIC : J_GETFIELD, ref, width - (is_static ? 0 : 1));
        store(m, view, in->a);
    }
}

/* an array of the array register's type, or of the opcode's when the code does not tell */
static const char* array_type(Method* m, const uint32_t* s, uint32_t r, unsigned op) {
    const char* t = type_of(m, kind_at(m, s, r));
    unsigned base = op >= 0x4b ? op - 7 : op;
    static const char* const defaults[] = {"[I", "[J", "[Ljava/lang/Object;", "[Z", "[B", "[C", "[S"};
    if (t && t[0] == '[') {
        /* aget-boolean and aget-byte share the JVM's baload, which takes either */
        if (base == 0x47 || base == 0x48) return t[1] == 'Z' || t[1] == 'B' ? t : defaults[base - 0x44];
        return t;
    }
    return defaults[base - 0x44];
}
static void emit_array(Method* m, const Insn* in, const uint32_t* s) {
    unsigned op = in->op, base = op >= 0x4b ? op - 7 : op;
    bool put = op >= 0x4b;
    const char* t = array_type(m, s, in->b, op);
    load(m, s, V_A, in->b);
    checkcast(m, t);
    load(m, s, V_I, in->c);
    unsigned view, jop;
    switch (t[1]) {
    case 'I': view = V_I; jop = 0x2e; break;
    case 'J': view = V_J; jop = 0x2f; break;
    case 'F': view = V_F; jop = 0x30; break;
    case 'D': view = V_D; jop = 0x31; break;
    case 'Z': case 'B': view = V_I; jop = 0x33; break;
    case 'C': view = V_I; jop = 0x34; break;
    case 'S': view = V_I; jop = 0x35; break;
    default: view = V_A; jop = 0x32; break;
    }
    (void)base;
    int width = (int)view_width[view];
    if (put) {
        load(m, s, view, in->a);
        op1(m, jop + (J_IASTORE - J_IALOAD), -2 - width);
    } else {
        op1(m, jop, width - 2);
        store(m, view, in->a);
    }
}

static void emit_compare_branch(Method* m, uint32_t pc, const Insn* in, const uint32_t* s) {
    unsigned op = in->op;
    if (op <= 0x37) { /* if-test vA, vB */
        unsigned ka = KIND(kind_at(m, s, in->a)), kb = KIND(kind_at(m, s, in->b));
        bool refs = ka == K_REF || kb == K_REF || ka == K_UNINIT_THIS || kb == K_UNINIT_THIS || ka == K_UNINIT ||
                    kb == K_UNINIT;
        if (refs) {
            load(m, s, V_A, in->a);
            load(m, s, V_A, in->b);
            m->sp -= 2;
            branch(m, J_IF_ACMPEQ + (op - 0x32), pc, in->target);
        } else {
            load(m, s, V_I, in->a);
            load(m, s, V_I, in->b);
            m->sp -= 2;
            branch(m, J_IF_ICMPEQ + (op - 0x32), pc, in->target);
        }
        return;
    }
    unsigned k = KIND(kind_at(m, s, in->a));
    if (k == K_REF || k == K_UNINIT_THIS || k == K_UNINIT) {
        load(m, s, V_A, in->a);
        m->sp -= 1;
        branch(m, op == 0x38 ? J_IFNULL : J_IFNULL + 1, pc, in->target);
        return;
    }
    if (k == K_FLOAT) {
        load(m, s, V_F, in->a);
        op_u2(m, J_INVOKESTATIC,
              m->scan ? 0 : pool_check(m, cf_member(m->pool, CF_METHODREF, "Ljava/lang/Float;", "floatToRawIntBits",
                                                    "(F)I")),
              0);
    } else {
        load(m, s, V_I, in->a);
    }
    m->sp -= 1;
    branch(m, J_IFEQ + (op - 0x38), pc, in->target);
}

static void emit_switch(Method* m, uint32_t pc, const Insn* in, const uint32_t* s) {
    load(m, s, V_I, in->a);
    m->sp -= 1;
    uint32_t count = switch_targets(m, pc, in, NULL, NULL);
    if (m->scan) return;
    uint32_t* targets = malloc((count + 1) * sizeof *targets);
    int32_t* keys = malloc((count + 1) * sizeof *keys);
    if (!targets || !keys) {
        free(targets);
        free(keys);
        fail(m, "out of memory");
        return;
    }
    switch_targets(m, pc, in, targets, keys);
    size_t base = m->out.size;
    uint32_t next = pc + in->size;
    if (in->op == 0x2b) {
        cf_u1(&m->out, J_TABLESWITCH);
        while (m->out.size % 4) cf_u1(&m->out, 0); /* the code starts at a multiple of four (see the prologue) */
        add_fixup(m, pc, m->out.size, base, next);
        cf_u4(&m->out, 0);
        cf_u4(&m->out, (uint32_t)(count ? keys[0] : 0));
        cf_u4(&m->out, (uint32_t)(count ? keys[0] + (int32_t)(count - 1) : -1));
        for (uint32_t i = 0; i < count; ++i) {
            add_fixup(m, pc, m->out.size, base, targets[i]);
            cf_u4(&m->out, 0);
        }
    } else {
        cf_u1(&m->out, J_LOOKUPSWITCH);
        while (m->out.size % 4) cf_u1(&m->out, 0);
        add_fixup(m, pc, m->out.size, base, next);
        cf_u4(&m->out, 0);
        cf_u4(&m->out, count);
        for (uint32_t i = 0; i < count; ++i) {
            cf_u4(&m->out, (uint32_t)keys[i]);
            add_fixup(m, pc, m->out.size, base, targets[i]);
            cf_u4(&m->out, 0);
        }
    }
    free(targets);
    free(keys);
}

/* fill-array-data: a call to the runtime with the payload's bytes in strings, one char for each byte */
static void emit_fill(Method* m, uint32_t pc, const Insn* in, const uint32_t* s) {
    uint32_t p = in->target;
    if (p >= m->n || unit(&m->code, p) != 0x0300) {
        fail(m, "fill-array-data at %u has no payload", (unsigned)pc);
        return;
    }
    uint32_t width = unit(&m->code, p + 1), count = unit(&m->code, p + 2) | (uint32_t)unit(&m->code, p + 3) << 16;
    uint64_t bytes = (uint64_t)width * count;
    if ((uint64_t)p + 4 + (bytes + 1) / 2 > m->n) {
        fail(m, "fill-array-data at %u: its payload runs past the method", (unsigned)pc);
        return;
    }
    const unsigned char* data = m->code.insns + 2 * ((size_t)p + 4);
    const uint64_t chunk = 16000; /* bytes in each string: two bytes each in modified UTF-8 at most */
    for (uint64_t at = 0; at < bytes || at == 0; at += chunk) {
        uint64_t n = bytes - at < chunk ? bytes - at : chunk;
        load(m, s, V_A, in->a);
        push_int(m, (int32_t)at);
        if (!m->scan) {
            CfBuffer text = {0};
            for (uint64_t i = 0; i < n; ++i) {
                unsigned b = data[at + i];
                if (b && b < 0x80) {
                    cf_u1(&text, b);
                } else { /* two bytes: 110xxxxx 10xxxxxx */
                    cf_u1(&text, 0xc0 | (b >> 6));
                    cf_u1(&text, 0x80 | (b & 0x3f));
                }
            }
            cf_u1(&text, 0);
            push_string(m, text.failed ? "" : (const char*)text.data);
            cf_free(&text);
        } else {
            grow_stack(m, 1);
        }
        push_int(m, (int32_t)width);
        op_u2(m, J_INVOKESTATIC,
              m->scan ? 0
                      : pool_check(m, cf_member(m->pool, CF_METHODREF, "Lsnx/dalvik/Runtime;", "fillArrayData",
                                                "(Ljava/lang/Object;ILjava/lang/String;I)V")),
              -4);
        if (!bytes) break;
    }
}

static void emit_new_array(Method* m, const Insn* in, const char* type) {
    static const char prims[] = "ZCFDBSIJ";
    static const unsigned char codes[] = {4, 5, 6, 7, 8, 9, 10, 11};
    const char* hit = type[0] == '[' && type[1] ? strchr(prims, type[1]) : NULL;
    if (hit && type[2] == 0)
        op_u1(m, J_NEWARRAY, codes[hit - prims], 0);
    else
        op_u2(m, J_ANEWARRAY, class_ref(m, type + 1), 0);
    (void)in;
}

static void emit_instruction(Method* m, uint32_t pc, const Insn* in, const uint32_t* s) {
    unsigned op = in->op;
    switch (op) {
    case 0x00: return;
    case 0x01: case 0x02: case 0x03:
        move(m, s, in->a, in->b, VIEW_BIT(V_I) | VIEW_BIT(V_F));
        return;
    case 0x04: case 0x05: case 0x06:
        move(m, s, in->a, in->b, VIEW_BIT(V_J) | VIEW_BIT(V_D));
        return;
    case 0x07: case 0x08: case 0x09:
        if (KIND(kind_at(m, s, in->b)) == K_UNINIT || KIND(kind_at(m, s, in->b)) == K_UNINIT_THIS) return;
        move(m, s, in->a, in->b, VIEW_BIT(V_A));
        return;
    case 0x0a: case 0x0b: case 0x0c:
        /* the invoke before it stores its result; one alone (after filled-new-array, done there too) is bad */
        if (!m->consumed[pc]) fail(m, "move-result at %u follows no invoke", (unsigned)pc);
        return;
    case 0x0d: return; /* its handler's stub stores the exception */
    case 0x0e: op1(m, J_RETURN, 0); return;
    case 0x0f: case 0x10: case 0x11: {
        const char* ret = method_return(m, m->method_idx);
        unsigned view = view_of_desc(ret);
        load(m, s, view, in->a);
        if (view == V_A) checkcast(m, ret);
        static const unsigned char returns[VIEWS] = {0xac, 0xae, 0xb0, 0xad, 0xaf};
        op1(m, returns[view], -(int)view_width[view]);
        return;
    }
    case 0x12: case 0x13: case 0x14: case 0x15: {
        uint32_t r = in->a, v = (uint32_t)in->lit;
        if (r >= m->regs) break;
        if (m->reads[r] & VIEW_BIT(V_I)) {
            push_int(m, (int32_t)v);
            store(m, V_I, r);
        }
        if (m->reads[r] & VIEW_BIT(V_F)) {
            push_float(m, v);
            store(m, V_F, r);
        }
        if (!v && (m->reads[r] & VIEW_BIT(V_A))) {
            op1(m, J_ACONST_NULL, 1);
            store(m, V_A, r);
        }
        return;
    }
    case 0x16: case 0x17: case 0x18: case 0x19: {
        uint32_t r = in->a;
        if (r >= m->regs) break;
        if (m->reads[r] & VIEW_BIT(V_J)) {
            push_long(m, (uint64_t)in->lit);
            store(m, V_J, r);
        }
        if (m->reads[r] & VIEW_BIT(V_D)) {
            push_double(m, (uint64_t)in->lit);
            store(m, V_D, r);
        }
        return;
    }
    case 0x1a: case 0x1b:
        push_string(m, dex_string(m->dex, in->idx));
        store(m, V_A, in->a);
        return;
    case 0x1c: {
        unsigned c = class_ref(m, dex_type(m->dex, in->idx));
        if (c && c < 256) op_u1(m, J_LDC, c, 1); else op_u2(m, J_LDC_W, c, 1);
        store(m, V_A, in->a);
        return;
    }
    case 0x1d: case 0x1e:
        load(m, s, V_A, in->a);
        op1(m, op == 0x1d ? J_MONITORENTER : J_MONITOREXIT, -1);
        return;
    case 0x1f: {
        const char* t = dex_type(m->dex, in->idx);
        load(m, s, V_A, in->a);
        op_u2(m, J_CHECKCAST, class_ref(m, t), 0);
        store(m, V_A, in->a);
        return;
    }
    case 0x20:
        load(m, s, V_A, in->b);
        op_u2(m, J_INSTANCEOF, class_ref(m, dex_type(m->dex, in->idx)), 0);
        store(m, V_I, in->a);
        return;
    case 0x21: {
        const char* t = type_of(m, kind_at(m, s, in->b));
        load(m, s, V_A, in->b);
        if (t && t[0] == '[') {
            checkcast(m, t);
            op1(m, J_ARRAYLENGTH, 0);
        } else {
            op_u2(m, J_INVOKESTATIC,
                  m->scan ? 0
                          : pool_check(m, cf_member(m->pool, CF_METHODREF, "Ljava/lang/reflect/Array;", "getLength",
                                                    "(Ljava/lang/Object;)I")),
                  0);
        }
        store(m, V_I, in->a);
        return;
    }
    case 0x22: return; /* the new happens at the <init> call */
    case 0x23: {
        const char* t = dex_type(m->dex, in->idx);
        load(m, s, V_I, in->b);
        emit_new_array(m, in, t);
        store(m, V_A, in->a);
        return;
    }
    case 0x24: case 0x25: {
        const char* t = dex_type(m->dex, in->idx);
        bool ints = t[1] == 'I';
        push_int(m, (int32_t)in->argc);
        emit_new_array(m, in, t);
        for (unsigned i = 0; i < in->argc; ++i) {
            op1(m, J_DUP, 1);
            push_int(m, (int32_t)i);
            load(m, s, ints ? V_I : V_A, arg_reg(in, i));
            op1(m, ints ? J_IASTORE : J_AASTORE, -3);
        }
        store_result(m, pc, in, t);
        return;
    }
    case 0x26: emit_fill(m, pc, in, s); return;
    case 0x27:
        load(m, s, V_A, in->a);
        checkcast(m, "Ljava/lang/Throwable;");
        op1(m, J_ATHROW, -1);
        return;
    case 0x28: case 0x29: case 0x2a: branch(m, J_GOTO, pc, in->target); return;
    case 0x2b: case 0x2c: emit_switch(m, pc, in, s); return;
    case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: {
        static const unsigned char views[] = {V_F, V_F, V_D, V_D, V_J};
        static const unsigned char ops[] = {0x95, 0x96, 0x97, 0x98, 0x94};
        unsigned v = views[op - 0x2d];
        load(m, s, v, in->b);
        load(m, s, v, in->c);
        op1(m, ops[op - 0x2d], 1 - 2 * (int)view_width[v]);
        store(m, V_I, in->a);
        return;
    }
    default: break;
    }
    if (op >= 0x32 && op <= 0x3d) {
        emit_compare_branch(m, pc, in, s);
        return;
    }
    if (op >= 0x44 && op <= 0x51) {
        emit_array(m, in, s);
        return;
    }
    if (op >= 0x52 && op <= 0x6d) {
        emit_field(m, in, s);
        return;
    }
    if ((op >= 0x6e && op <= 0x72) || (op >= 0x74 && op <= 0x78)) {
        emit_invoke(m, pc, in, s);
        return;
    }
    if (op >= 0x7b && op <= 0x8f) {
        /* the source view, the result's, and the JVM operation */
        static const struct {
            unsigned char from, to, op;
        } unary[] = {
            {V_I, V_I, 0x74}, {V_I, V_I, 0},    {V_J, V_J, 0x75}, {V_J, V_J, 0},    {V_F, V_F, 0x76},
            {V_D, V_D, 0x77}, {V_I, V_J, 0x85}, {V_I, V_F, 0x86}, {V_I, V_D, 0x87}, {V_J, V_I, 0x88},
            {V_J, V_F, 0x89}, {V_J, V_D, 0x8a}, {V_F, V_I, 0x8b}, {V_F, V_J, 0x8c}, {V_F, V_D, 0x8d},
            {V_D, V_I, 0x8e}, {V_D, V_J, 0x8f}, {V_D, V_F, 0x90}, {V_I, V_I, 0x91}, {V_I, V_I, 0x92},
            {V_I, V_I, 0x93}};
        unsigned i = op - 0x7b;
        load(m, s, unary[i].from, in->b);
        if (op == 0x7c) { /* not-int */
            push_int(m, -1);
            op1(m, J_IXOR, -1);
        } else if (op == 0x7e) { /* not-long */
            push_long(m, ~(uint64_t)0);
            op1(m, J_LXOR, -2);
        } else {
            op1(m, unary[i].op, (int)view_width[unary[i].to] - (int)view_width[unary[i].from]);
        }
        store(m, unary[i].to, in->a);
        return;
    }
    if (op >= 0x90 && op <= 0xcf) {
        bool two_addr = op >= 0xb0;
        unsigned o = two_addr ? op - 0x20 : op;
        uint32_t dst = in->a, x = two_addr ? in->a : in->b, y = two_addr ? in->b : in->c;
        static const unsigned char int_ops[] = {0x60, 0x64, 0x68, 0x6c, 0x70, 0x7e, 0x80, 0x82, 0x78, 0x7a, 0x7c};
        unsigned view, jop;
        bool shift = false;
        if (o <= 0x9a) {
            view = V_I;
            jop = int_ops[o - 0x90];
        } else if (o <= 0xa5) {
            view = V_J;
            jop = int_ops[o - 0x9b] + 1u;
            shift = o >= 0xa3;
        } else if (o <= 0xaa) {
            view = V_F;
            jop = int_ops[o - 0xa6] + 2u;
        } else {
            view = V_D;
            jop = int_ops[o - 0xab] + 3u;
        }
        int width = (int)view_width[view];
        load(m, s, view, x);
        load(m, s, shift ? V_I : view, y);
        op1(m, jop, shift ? -1 : -width);
        store(m, view, dst);
        return;
    }
    if (op >= 0xd0 && op <= 0xe2) {
        bool lit8 = op >= 0xd8;
        unsigned o = lit8 ? op - 0xd8 : op - 0xd0; /* add, rsub, mul, div, rem, and, or, xor, shl, shr, ushr */
        static const unsigned char ops[] = {0x60, 0x64, 0x68, 0x6c, 0x70, 0x7e, 0x80, 0x82, 0x78, 0x7a, 0x7c};
        if (o == 1) { /* rsub: the literal minus the register */
            push_int(m, (int32_t)in->lit);
            load(m, s, V_I, in->b);
        } else {
            load(m, s, V_I, in->b);
            push_int(m, (int32_t)in->lit);
        }
        op1(m, ops[o], -1);
        store(m, V_I, in->a);
        return;
    }
    /* invoke-polymorphic, invoke-custom, const-method-handle, const-method-type: not in the DEX versions apps
       ship for the API levels this runs */
    fail(m, "instruction 0x%02x at %u is not translated", op, (unsigned)pc);
}

/* the code, from the walk of the reachable instructions in address order */
static void walk(Method* m) {
    uint32_t* s = malloc(m->width * sizeof *s);
    if (!s) {
        fail(m, "out of memory");
        return;
    }
    bool live = false;
    m->sp = 0;
    memset(m->consumed, 0, m->n);
    for (uint32_t pc = 0; pc < m->n && !m->failed; ++pc) {
        if (!m->insn_start[pc]) continue;
        if (!m->scan) m->label[pc] = (uint32_t)m->out.size;
        int32_t block = m->block[pc];
        if (block >= 0) {
            live = m->reached[block] != 0;
            if (live) {
                memcpy(s, state_of(m, (uint32_t)block), m->width * sizeof *s);
                add_frame(m, (uint32_t)m->out.size, (s[FLAGS(m)] & F_INIT) != 0, 0);
            }
        }
        if (!live) continue;
        Insn in = decode(&m->code, pc);
        if (m->sp) fail(m, "the stack is not empty at %u", (unsigned)pc);
        if (!m->consumed[pc]) emit_instruction(m, pc, &in, s);
        transfer(m, pc, &in, s);
        unsigned op = in.op;
        if ((op >= 0x0e && op <= 0x11) || op == 0x27 || (op >= 0x28 && op <= 0x2a)) live = false;
    }
    if (!m->scan) m->label[m->n] = (uint32_t)m->out.size;
    /* the handlers' stubs: the exception into the move-exception's register, then on to the handler */
    for (uint32_t i = 0; i < m->handler_count && !m->failed; ++i) {
        Handler* h = &m->handlers[i];
        int32_t block = m->block[h->pc];
        if (block < 0 || !m->reached[block]) continue;
        const uint32_t* entry = state_of(m, (uint32_t)block);
        h->offset = (uint32_t)m->out.size;
        h->type = m->scan ? 0 : pool_check(m, cf_class_descriptor(m->pool, h->desc));
        add_frame(m, h->offset, (entry[FLAGS(m)] & F_INIT) != 0, h->type);
        m->sp = 1;
        Insn in = decode(&m->code, h->pc);
        if (in.op == 0x0d) {
            store(m, V_A, in.a);
            branch(m, J_GOTO, h->pc, h->pc + in.size);
        } else {
            op1(m, J_POP, -1);
            branch(m, J_GOTO, h->pc, h->pc);
        }
    }
    free(s);
}

/* ---- the method's code attribute ---- */

/* a verification_type_info for a descriptor's type */
static void verification_type(Method* m, CfBuffer* b, const char* desc) {
    switch (desc[0]) {
    case 'Z': case 'B': case 'S': case 'C': case 'I': cf_u1(b, 1); break;
    case 'F': cf_u1(b, 2); break;
    case 'D': cf_u1(b, 3); break;
    case 'J': cf_u1(b, 4); break;
    default:
        cf_u1(b, 7);
        cf_u2(b, pool_check(m, cf_class_descriptor(m->pool, desc)));
        break;
    }
}
/* the locals of every frame: the parameters, then each view's local in order */
static unsigned frame_locals(Method* m, CfBuffer* b, bool init) {
    unsigned count = 0;
    if (!m->is_static) {
        if (m->is_ctor && !init)
            cf_u1(b, 6); /* UninitializedThis */
        else
            verification_type(m, b, m->class_desc);
        ++count;
    }
    uint32_t params = dex_proto_param_count(m->dex, m->proto_idx);
    for (uint32_t i = 0; i < params; ++i) {
        verification_type(m, b, dex_type(m->dex, dex_proto_param(m->dex, m->proto_idx, i)));
        ++count;
    }
    /* the views' locals, in local order: the slots were given out in register and view order */
    static const char* const types[VIEWS] = {"I", "F", "Ljava/lang/Object;", "J", "D"};
    for (uint32_t r = 0; r < m->regs; ++r)
        for (unsigned v = 0; v < VIEWS; ++v)
            if (m->slot[r * VIEWS + v] >= 0) {
                verification_type(m, b, types[v]);
                ++count;
            }
    return count;
}
static void write_code(Method* m, CfBuffer* method_out) {
    /* the prologue: every view's local set, then the parameters copied into their registers' views */
    CfBuffer body = m->out;
    m->out = (CfBuffer){0};
    m->scan = false;
    m->sp = 0;
    for (uint32_t r = 0; r < m->regs; ++r)
        for (unsigned v = 0; v < VIEWS; ++v) {
            int32_t local = m->slot[r * VIEWS + v];
            if (local < 0) continue;
            static const unsigned char zeros[VIEWS] = {J_ICONST_0, J_FCONST_0, J_ACONST_NULL, J_LCONST_0,
                                                       J_DCONST_0};
            op1(m, zeros[v], (int)view_width[v]);
            local_op(m, view_store[v], (uint32_t)local, -(int)view_width[v]);
        }
    uint32_t reg = m->regs - m->code.ins, local = 0;
    if (!m->is_static) {
        if (!m->is_ctor && m->slot[reg * VIEWS + V_A] >= 0) {
            op1(m, J_ALOAD_0, 1);
            local_op(m, view_store[V_A], (uint32_t)m->slot[reg * VIEWS + V_A], -1);
        }
        ++reg;
        ++local;
    }
    uint32_t params = dex_proto_param_count(m->dex, m->proto_idx);
    for (uint32_t i = 0; i < params; ++i) {
        const char* t = dex_type(m->dex, dex_proto_param(m->dex, m->proto_idx, i));
        unsigned v = view_of_desc(t);
        if (reg < m->regs && m->slot[reg * VIEWS + v] >= 0) {
            local_op(m, view_load[v], local, (int)view_width[v]);
            local_op(m, view_store[v], (uint32_t)m->slot[reg * VIEWS + v], -(int)view_width[v]);
        }
        reg += wide_desc(t) ? 2 : 1;
        local += wide_desc(t) ? 2 : 1;
    }
    while (m->out.size % 4) cf_u1(&m->out, J_NOP); /* the switches' padding counts from the code's start */
    CfBuffer prologue = m->out;
    m->out = body;
    uint32_t shift = (uint32_t)prologue.size;
    uint64_t code_size = (uint64_t)prologue.size + body.size;
    if (code_size > 65535) {
        fail(m, "the translated method is %llu bytes, past the JVM's 65535", (unsigned long long)code_size);
        cf_free(&prologue);
        return;
    }
    /* the locals of a frame, before and after a constructor's this is initialized */
    CfBuffer locals[2] = {{0}, {0}};
    unsigned local_counts[2] = {frame_locals(m, &locals[0], false), frame_locals(m, &locals[1], true)};
    CfBuffer frames = {0};
    unsigned frame_count = 0;
    uint32_t last = 0;
    for (size_t i = 0; i < m->frame_count; ++i) {
        uint32_t offset = m->frames[i].offset + shift;
        cf_u1(&frames, 255); /* full_frame */
        cf_u2(&frames, frame_count ? offset - last - 1 : offset);
        unsigned which = m->frames[i].init ? 1 : 0;
        cf_u2(&frames, local_counts[which]);
        cf_bytes(&frames, locals[which].data, locals[which].size);
        if (m->frames[i].stack) {
            cf_u2(&frames, 1);
            cf_u1(&frames, 7);
            cf_u2(&frames, m->frames[i].stack);
        } else {
            cf_u2(&frames, 0);
        }
        last = offset;
        ++frame_count;
    }
    CfBuffer table = {0};
    unsigned entries = 0;
    for (uint32_t t = 0; t < m->code.tries; ++t) {
        uint32_t start = try_start(m, t), end = start + try_count(m, t);
        if (start > m->n || end > m->n) continue;
        uint32_t from = m->label[start] + shift, to = m->label[end] + shift;
        if (from >= to) continue;
        const unsigned char* dend = m->dex->data + m->dex->size;
        const unsigned char* p = m->code.handlers + try_handler(m, t);
        int32_t size = dex_sleb(&p, dend);
        uint32_t count = size < 0 ? (uint32_t)-(int64_t)size : (uint32_t)size;
        for (uint32_t i = 0; i <= count; ++i) {
            if (i == count && size > 0) break;
            uint32_t type_idx = i < count ? dex_uleb(&p, dend) : DEX_NO_INDEX, handler = dex_uleb(&p, dend);
            for (uint32_t h = 0; h < m->handler_count; ++h)
                if (m->handlers[h].pc == handler && m->block[handler] >= 0 && m->reached[m->block[handler]]) {
                    cf_u2(&table, from);
                    cf_u2(&table, to);
                    cf_u2(&table, m->handlers[h].offset + shift);
                    cf_u2(&table, type_idx == DEX_NO_INDEX ? 0
                                                           : pool_check(m, cf_class_descriptor(
                                                                               m->pool, dex_type(m->dex, type_idx))));
                    ++entries;
                    break;
                }
        }
    }
    unsigned max_stack = (unsigned)(m->max_sp > 0 ? m->max_sp : 0) + 2;
    unsigned code_name = pool_check(m, cf_utf8(m->pool, "Code"));
    unsigned map_name = frame_count ? pool_check(m, cf_utf8(m->pool, "StackMapTable")) : 0;
    uint32_t length = 2 + 2 + 4 + (uint32_t)code_size + 2 + (uint32_t)table.size + 2 +
                      (frame_count ? 6 + 2 + (uint32_t)frames.size : 0);
    cf_u2(method_out, 1); /* attributes: Code */
    cf_u2(method_out, code_name);
    cf_u4(method_out, length);
    cf_u2(method_out, max_stack);
    cf_u2(method_out, m->locals);
    cf_u4(method_out, (uint32_t)code_size);
    cf_bytes(method_out, prologue.data, prologue.size);
    size_t body_at = method_out->size;
    cf_bytes(method_out, m->out.data, m->out.size);
    /* the branches, now that every label is known */
    for (size_t i = 0; i < m->fixup_count; ++i) {
        const Fixup* f = &m->fixups[i];
        int64_t delta = (int64_t)m->label[f->target] - (int64_t)f->base;
        if (f->width == 2)
            cf_put_u2(method_out, body_at + f->at, (unsigned)(delta & 0xffff));
        else
            cf_put_u4(method_out, body_at + f->at, (uint32_t)delta);
    }
    cf_u2(method_out, entries);
    cf_bytes(method_out, table.data, table.size);
    if (frame_count) {
        cf_u2(method_out, 1);
        cf_u2(method_out, map_name);
        cf_u4(method_out, 2 + (uint32_t)frames.size);
        cf_u2(method_out, frame_count);
        cf_bytes(method_out, frames.data, frames.size);
    } else {
        cf_u2(method_out, 0);
    }
    if (frames.failed || table.failed || prologue.failed || locals[0].failed || locals[1].failed)
        fail(m, "out of memory");
    cf_free(&locals[0]);
    cf_free(&locals[1]);
    cf_free(&frames);
    cf_free(&table);
    cf_free(&prologue);
}
/* whether some 16-bit branch is out of its range */
static bool branches_overflow(const Method* m) {
    for (size_t i = 0; i < m->fixup_count; ++i) {
        const Fixup* f = &m->fixups[i];
        int64_t delta = (int64_t)m->label[f->target] - (int64_t)f->base;
        if (f->width == 2 && (delta < -32768 || delta > 32767)) return true;
    }
    return false;
}
/* the fixups name addresses; a stub's branch names one too. Each target has a label */
static void method_free(Method* m) {
    free(m->insn_start);
    free(m->block);
    free(m->label);
    free(m->consumed);
    free(m->states);
    free(m->reached);
    free(m->worklist);
    free(m->types);
    free(m->reads);
    free(m->slot);
    free(m->handlers);
    free(m->fixups);
    free(m->frames);
    cf_free(&m->out);
    cf_free(&m->text);
}
/* a method's Code attribute into out (with its attributes count); false with m->error filled */
/* the method's instructions, blocks and handlers, and the dataflow over them */
static bool analyze(Method* m) {
    uint32_t n = m->n = m->code.insns_size;
    m->regs = m->code.registers;
    m->width = m->regs + 2;
    m->insn_start = calloc(n + 1, 1);
    m->block = malloc((n + 1) * sizeof *m->block);
    m->label = calloc(n + 1, sizeof *m->label);
    m->consumed = calloc(n + 1, 1);
    m->reads = calloc(m->regs + 1, 1);
    m->slot = malloc(((size_t)m->regs * VIEWS + 1) * sizeof *m->slot);
    if (!m->insn_start || !m->block || !m->label || !m->consumed || !m->reads || !m->slot) {
        fail(m, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i <= n; ++i) m->block[i] = -1;
    for (size_t i = 0; i < (size_t)m->regs * VIEWS; ++i) m->slot[i] = -1;
    if (!n) {
        fail(m, "a method with no code");
        return false;
    }
    if (!find_instructions(m) || !find_blocks(m)) return false;
    m->states = malloc(((size_t)m->block_count * m->width + 1) * sizeof *m->states);
    m->reached = calloc(m->block_count + 1, 1);
    m->worklist = malloc((m->block_count + 1) * sizeof *m->worklist);
    uint32_t* entry = malloc(m->width * sizeof *entry);
    if (!m->states || !m->reached || !m->worklist || !entry) {
        free(entry);
        fail(m, "out of memory");
        return false;
    }
    /* the entry state: the parameters in the last registers */
    for (uint32_t r = 0; r < m->width; ++r) entry[r] = K_TOP;
    entry[FLAGS(m)] = m->is_ctor ? 0 : F_INIT;
    uint32_t reg = m->regs - m->code.ins;
    if (!m->is_static) {
        entry[reg++] = m->is_ctor ? K_UNINIT_THIS : MAKE(K_REF, intern(m, m->class_desc));
    }
    uint32_t params = dex_proto_param_count(m->dex, m->proto_idx);
    for (uint32_t i = 0; i < params && reg < m->regs; ++i) {
        const char* t = dex_type(m->dex, dex_proto_param(m->dex, m->proto_idx, i));
        entry[reg] = kind_of_desc(m, t);
        if (wide_desc(t) && reg + 1 < m->regs) entry[reg + 1] = K_HI;
        reg += wide_desc(t) ? 2 : 1;
    }
    bool ok = dataflow(m, entry);
    free(entry);
    return ok;
}
static bool translate_code(Method* m, CfBuffer* out) {
    if (!analyze(m)) return false;
    uint32_t params = dex_proto_param_count(m->dex, m->proto_idx);
    /* the views each register is read as: a move reads its source as its destination is read, so until no more
       change */
    m->scan = true;
    do {
        m->changed = false;
        m->max_sp = 0;
        walk(m);
    } while (m->changed && !m->failed);
    if (m->failed) return false;
    /* the locals: the parameters', then one for each view read */
    m->param_locals = m->is_static ? 0 : 1;
    for (uint32_t i = 0; i < params; ++i)
        m->param_locals += wide_desc(dex_type(m->dex, dex_proto_param(m->dex, m->proto_idx, i))) ? 2 : 1;
    m->locals = m->param_locals;
    for (uint32_t r = 0; r < m->regs; ++r)
        for (unsigned v = 0; v < VIEWS; ++v) {
            m->slot[r * VIEWS + v] = -1;
            if (m->reads[r] & VIEW_BIT(v)) {
                m->slot[r * VIEWS + v] = (int32_t)m->locals;
                m->locals += view_width[v];
            }
        }
    if (m->locals > 65535) {
        fail(m, "the method needs %u locals", (unsigned)m->locals);
        return false;
    }
    /* the code, with 32-bit branches when 16 bits do not reach */
    for (int attempt = 0; attempt < 2; ++attempt) {
        m->scan = false;
        m->out.size = 0;
        m->fixup_count = m->frame_count = 0;
        m->max_sp = 0;
        walk(m);
        if (m->failed) return false;
        if (!branches_overflow(m)) break;
        m->long_branches = true;
    }
    write_code(m, out);
    return !m->failed && !out->failed;
}

/* a body that throws VerifyError with message, for a method the translator can't translate */
static void write_failing_code(CfPool* pool, CfBuffer* out, const char* message, unsigned locals) {
    CfBuffer code = {0};
    cf_u1(&code, J_NEW);
    cf_u2(&code, cf_class(pool, "java/lang/VerifyError"));
    cf_u1(&code, J_DUP);
    cf_u1(&code, J_LDC_W);
    cf_u2(&code, cf_string(pool, message));
    cf_u1(&code, J_INVOKESPECIAL);
    cf_u2(&code, cf_member(pool, CF_METHODREF, "Ljava/lang/VerifyError;", "<init>", "(Ljava/lang/String;)V"));
    cf_u1(&code, J_ATHROW);
    cf_u2(out, 1);
    cf_u2(out, cf_utf8(pool, "Code"));
    cf_u4(out, 12 + (uint32_t)code.size);
    cf_u2(out, 3);
    cf_u2(out, locals);
    cf_u4(out, (uint32_t)code.size);
    cf_bytes(out, code.data, code.size);
    cf_u2(out, 0);
    cf_u2(out, 0);
    cf_free(&code);
}

static size_t next_param(const char** p);

/* A native method's body: a call of the native bridge (snx.android.nativebridge.NativeBridge.invoke*), which runs
   the app's AArch64 code for it. The key names the method ("Lpkg/Class;.name(signature)"), then the object (or the
   class of a static method), the primitive parameters as their bits in a long[], and the references in an
   Object[]. The result comes back as its bits, or as the reference (docs/android/NATIVE.md, "JNI") */
static void write_native_stub(CfPool* pool, CfBuffer* out, const char* class_desc, const char* name, const char* desc,
                              bool is_static) {
    enum { LLOAD = 0x16, FLOAD = 0x17, DLOAD = 0x18, LASTORE = 0x50, L2I = 0x88, LRETURN = 0xad, FRETURN = 0xae,
           DRETURN = 0xaf, ARETURN = 0xb0, T_LONG = 11 };
    static const char bridge[] = "Lsnx/android/nativebridge/NativeBridge;";
    CfBuffer code = {0};
    size_t n = strlen(class_desc) + strlen(name) + strlen(desc) + 2;
    char* key = malloc(n);
    if (!key) return;
    snprintf(key, n, "%s.%s%s", class_desc, name, desc);
    cf_u1(&code, J_LDC_W);
    cf_u2(&code, cf_string(pool, key));
    free(key);
    if (is_static) {
        cf_u1(&code, J_LDC_W);
        cf_u2(&code, cf_class_descriptor(pool, class_desc));
    } else {
        cf_u1(&code, J_ALOAD_0);
    }
    /* the parameters' kinds and slots */
    unsigned slot = is_static ? 0 : 1, prims = 0, refs = 0;
    const char* p = desc + 1;
    size_t length;
    while ((length = next_param(&p))) {
        char t = p[-(long)length];
        if (t == 'L' || t == '[') ++refs;
        else ++prims;
    }
    cf_u1(&code, J_SIPUSH);
    cf_u2(&code, prims);
    cf_u1(&code, J_NEWARRAY);
    cf_u1(&code, T_LONG);
    unsigned k = 0;
    p = desc + 1;
    while ((length = next_param(&p))) {
        char t = p[-(long)length];
        bool wide = t == 'J' || t == 'D';
        if (t != 'L' && t != '[') {
            cf_u1(&code, J_DUP);
            cf_u1(&code, J_SIPUSH);
            cf_u2(&code, k++);
            if (t == 'J') {
                cf_u1(&code, LLOAD);
                cf_u1(&code, slot);
            } else if (t == 'D') {
                cf_u1(&code, DLOAD);
                cf_u1(&code, slot);
                cf_u1(&code, J_INVOKESTATIC);
                cf_u2(&code, cf_member(pool, CF_METHODREF, "Ljava/lang/Double;", "doubleToRawLongBits", "(D)J"));
            } else if (t == 'F') {
                cf_u1(&code, FLOAD);
                cf_u1(&code, slot);
                cf_u1(&code, J_INVOKESTATIC);
                cf_u2(&code, cf_member(pool, CF_METHODREF, "Ljava/lang/Float;", "floatToRawIntBits", "(F)I"));
                cf_u1(&code, J_I2L);
            } else {
                cf_u1(&code, J_ILOAD);
                cf_u1(&code, slot);
                cf_u1(&code, J_I2L);
            }
            cf_u1(&code, LASTORE);
        }
        slot += wide ? 2 : 1;
    }
    cf_u1(&code, J_SIPUSH);
    cf_u2(&code, refs);
    cf_u1(&code, J_ANEWARRAY);
    cf_u2(&code, cf_class(pool, "java/lang/Object"));
    k = 0;
    slot = is_static ? 0 : 1;
    p = desc + 1;
    while ((length = next_param(&p))) {
        char t = p[-(long)length];
        if (t == 'L' || t == '[') {
            cf_u1(&code, J_DUP);
            cf_u1(&code, J_SIPUSH);
            cf_u2(&code, k++);
            cf_u1(&code, J_ALOAD);
            cf_u1(&code, slot);
            cf_u1(&code, J_AASTORE);
        }
        slot += t == 'J' || t == 'D' ? 2 : 1;
    }
    const char* ret = strchr(desc, ')') + 1;
    const char* invoke = *ret == 'V' ? "invokeV" : *ret == 'L' || *ret == '[' ? "invokeL" : "invokeJ";
    const char* invoke_desc = *ret == 'V'   ? "(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)V"
                              : *ret == 'L' || *ret == '['
                                  ? "(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)Ljava/lang/Object;"
                                  : "(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)J";
    cf_u1(&code, J_INVOKESTATIC);
    cf_u2(&code, cf_member(pool, CF_METHODREF, bridge, invoke, invoke_desc));
    switch (*ret) {
    case 'V': cf_u1(&code, J_RETURN); break;
    case 'L':
    case '[':
        cf_u1(&code, J_CHECKCAST);
        cf_u2(&code, cf_class_descriptor(pool, ret));
        cf_u1(&code, ARETURN);
        break;
    case 'J': cf_u1(&code, LRETURN); break;
    case 'D':
        cf_u1(&code, J_INVOKESTATIC);
        cf_u2(&code, cf_member(pool, CF_METHODREF, "Ljava/lang/Double;", "longBitsToDouble", "(J)D"));
        cf_u1(&code, DRETURN);
        break;
    case 'F':
        cf_u1(&code, L2I);
        cf_u1(&code, J_INVOKESTATIC);
        cf_u2(&code, cf_member(pool, CF_METHODREF, "Ljava/lang/Float;", "intBitsToFloat", "(I)F"));
        cf_u1(&code, FRETURN);
        break;
    default:
        cf_u1(&code, L2I);
        cf_u1(&code, J_IRETURN);
        break;
    }
    cf_u2(out, 1);
    cf_u2(out, cf_utf8(pool, "Code"));
    cf_u4(out, 12 + (uint32_t)code.size);
    cf_u2(out, 10); /* the stack: key, self, the array, its copy, an index and a wide value */
    cf_u2(out, slot ? slot : 1);
    cf_u4(out, (uint32_t)code.size);
    cf_bytes(out, code.data, code.size);
    cf_u2(out, 0);
    cf_u2(out, 0);
    cf_free(&code);
}

/* ---- the class ---- */

bool dex_class_name(const Dex* dex, uint32_t index, char* out, size_t out_size) {
    const char* d = dex_type(dex, dex_class_def(dex, index).class_idx);
    size_t n = strlen(d);
    if (n < 3 || d[0] != 'L' || d[n - 1] != ';' || n - 2 >= out_size) return false;
    memcpy(out, d + 1, n - 2);
    out[n - 2] = 0;
    return true;
}

static void write_constant_value(CfPool* pool, CfBuffer* out, const char* type, DexValue v) {
    unsigned c = 0;
    switch (v.type) {
    case DEX_VALUE_BYTE: case DEX_VALUE_SHORT: case DEX_VALUE_CHAR: case DEX_VALUE_INT: case DEX_VALUE_BOOLEAN:
        if (strchr("ZBSCI", type[0]) && type[1] == 0) c = cf_integer(pool, (uint32_t)v.bits);
        break;
    case DEX_VALUE_LONG: if (!strcmp(type, "J")) c = cf_long(pool, v.bits); break;
    case DEX_VALUE_FLOAT: if (!strcmp(type, "F")) c = cf_float(pool, (uint32_t)v.bits); break;
    case DEX_VALUE_DOUBLE: if (!strcmp(type, "D")) c = cf_double(pool, v.bits); break;
    default: break;
    }
    if (!c) {
        cf_u2(out, 0);
        return;
    }
    cf_u2(out, 1);
    cf_u2(out, cf_utf8(pool, "ConstantValue"));
    cf_u4(out, 2);
    cf_u2(out, c);
}
/* a String constant: its index in the DEX file's strings */
static void write_string_value(CfPool* pool, CfBuffer* out, const Dex* dex, const char* type, DexValue v) {
    if (v.type != DEX_VALUE_STRING || strcmp(type, "Ljava/lang/String;")) {
        write_constant_value(pool, out, type, v);
        return;
    }
    cf_u2(out, 1);
    cf_u2(out, cf_utf8(pool, "ConstantValue"));
    cf_u4(out, 2);
    cf_u2(out, cf_string(pool, dex_string(dex, (uint32_t)v.bits)));
}

/* more attributes after a member's or the class's: its attributes count at at, raised by count */
static void add_attributes(CfBuffer* body, size_t at, unsigned count) {
    if (!count || body->failed) return;
    cf_put_u2(body, at, ((unsigned)body->data[at] << 8 | body->data[at + 1]) + count);
}
/* a method's annotations after its other attributes: its own, its parameters', and its default as an annotation
   class's element */
static void method_attributes(CfPool* pool, CfBuffer* body, size_t at, const DexAnnotations* a, uint32_t method_idx,
                              uint32_t proto_idx, const char* name) {
    unsigned params = dex_proto_param_count(a->dex, proto_idx);
    unsigned count = cf_annotation_attributes(pool, body, a->dex, dex_method_annotations(a, method_idx));
    count += cf_parameter_attributes(pool, body, a->dex, dex_parameter_annotations(a, method_idx), params);
    count += cf_annotation_default(pool, body, a->dex, a->class_set, name);
    add_attributes(body, at, count);
}

/* the types of a descriptor's parameters, through *p: the next one's length, 0 at its end */
static size_t next_param(const char** p) {
    const char* t = *p;
    if (*t == '(') t = ++*p;
    if (!*t || *t == ')') return 0;
    const char* e = t;
    while (*e == '[') ++e;
    if (*e == 'L') {
        while (*e && *e != ';') ++e;
    }
    if (*e) ++e;
    *p = e;
    return (size_t)(e - t);
}
/* a synthetic constructor: its parameters on to its superclass's <init> of descriptor y->target, which takes the
   same ones or all but the two markers */
static void write_synthetic(CfPool* pool, CfBuffer* out, const char* super_desc, const Synthetic* y) {
    CfBuffer code = {0};
    unsigned local = 1, params = 0;
    const char* p = y->desc;
    size_t length;
    while ((length = next_param(&p))) ++params;
    unsigned pass = !strcmp(y->target, y->desc) ? params : params - 2;
    cf_u1(&code, J_ALOAD_0);
    p = y->desc;
    for (unsigned i = 0; (length = next_param(&p)); ++i) {
        const char* t = p - length;
        unsigned view = view_of_desc(t);
        if (i < pass) {
            cf_u1(&code, view_load[view]);
            cf_u1(&code, local);
        }
        local += view_width[view];
    }
    cf_u1(&code, J_INVOKESPECIAL);
    cf_u2(&code, cf_member(pool, CF_METHODREF, super_desc, "<init>", y->target));
    cf_u1(&code, J_RETURN);
    cf_u2(out, 0x1001); /* public, synthetic */
    cf_u2(out, cf_utf8(pool, "<init>"));
    cf_u2(out, cf_utf8(pool, y->desc));
    cf_u2(out, 1);
    cf_u2(out, cf_utf8(pool, "Code"));
    cf_u4(out, 12 + (uint32_t)code.size);
    cf_u2(out, local);
    cf_u2(out, local);
    cf_u4(out, (uint32_t)code.size);
    cf_bytes(out, code.data, code.size);
    cf_u2(out, 0);
    cf_u2(out, 0);
    if (code.failed) out->failed = true;
    cf_free(&code);
}

bool dex_program_find(const DexProgram* program, const char* descriptor, uint32_t* dex, uint32_t* index) {
    for (uint32_t d = 0; d < program->count; ++d)
        if (dex_find_class(&program->dexes[d], descriptor, index)) {
            *dex = d;
            return true;
        }
    return false;
}
void dex_program_close(DexProgram* program) {
    if (!program) return;
    for (size_t i = 0; i < program->synthetic_count; ++i) {
        free(program->synthetics[i].cls);
        free(program->synthetics[i].desc);
        free(program->synthetics[i].target);
    }
    free(program->synthetics);
    free(program);
}
DexProgram* dex_program_open(const Dex* dexes, uint32_t count, const DexTranslateOptions* options, char* error,
                             size_t error_size) {
    DexProgram* program = calloc(1, sizeof *program);
    if (!program) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    program->dexes = dexes;
    program->count = count;
    if (options) program->options = *options;
    /* every method's calls of an ancestor's <init>: a scan of each, as its translation's first pass makes it. A
       method that fails here fails again when its class is translated, which warns of it */
    CfPool pool = {0};
    for (uint32_t d = 0; d < count; ++d) {
        const Dex* dex = &dexes[d];
        for (uint32_t c = 0; c < dex->class_count; ++c) {
            DexClassDef def = dex_class_def(dex, c);
            DexClassData data;
            if (!dex_class_data(dex, &def, &data)) continue;
            for (uint32_t i = 0; i < data.direct_count + data.virtual_count; ++i) {
                const DexMethod* dm = &data.methods[i];
                if (!dm->code_off) continue;
                uint32_t class_idx, proto_idx;
                const char* name;
                dex_method(dex, dm->method_idx, &class_idx, &proto_idx, &name);
                Method m = {0};
                m.dex = dex;
                m.program = program;
                m.collecting = program;
                m.pool = &pool;
                m.class_desc = dex_type(dex, def.class_idx);
                m.super_desc = def.super_idx == DEX_NO_INDEX ? NULL : dex_type(dex, def.super_idx);
                m.interfaces_off = def.interfaces_off;
                m.method_idx = dm->method_idx;
                m.proto_idx = proto_idx;
                m.is_static = (dm->access & 0x8) != 0;
                m.is_ctor = !strcmp(name, "<init>");
                if (dex_code(dex, dm->code_off, &m.code) && analyze(&m)) {
                    m.scan = true;
                    walk(&m);
                }
                method_free(&m);
            }
            dex_class_data_free(&data);
        }
    }
    cf_pool_free(&pool);
    return program;
}

unsigned char* dex_translate_class(const DexProgram* program, uint32_t dex_index, uint32_t index, size_t* size,
                                   char* error, size_t error_size) {
    if (dex_index >= program->count) {
        snprintf(error, error_size, "no DEX file %u", (unsigned)dex_index);
        return NULL;
    }
    const Dex* dex = &program->dexes[dex_index];
    const DexTranslateOptions* options = &program->options;
    if (index >= dex->class_count) {
        snprintf(error, error_size, "no class def %u", (unsigned)index);
        return NULL;
    }
    DexClassDef def = dex_class_def(dex, index);
    const char* class_desc = dex_type(dex, def.class_idx);
    DexClassData data;
    if (!dex_class_data(dex, &def, &data)) {
        snprintf(error, error_size, "%s: a bad class_data_item", class_desc);
        return NULL;
    }
    CfPool pool = {0};
    CfBuffer body = {0};
    bool is_interface_class = (def.access & 0x200) != 0;
    unsigned access = (def.access & 0x7611u) | (is_interface_class ? 0 : 0x20u);
    cf_u2(&body, access);
    cf_u2(&body, cf_class_descriptor(&pool, class_desc));
    cf_u2(&body, def.super_idx == DEX_NO_INDEX ? 0 : cf_class_descriptor(&pool, dex_type(dex, def.super_idx)));
    uint32_t interfaces = dex_type_list_count(dex, def.interfaces_off);
    cf_u2(&body, interfaces);
    for (uint32_t i = 0; i < interfaces; ++i)
        cf_u2(&body, cf_class_descriptor(&pool, dex_type(dex, dex_type_list_item(dex, def.interfaces_off, i))));

    /* fields, the static ones' initial values from the encoded_array */
    uint32_t field_count = data.static_count + data.instance_count;
    cf_u2(&body, field_count);
    const unsigned char *values = NULL, *end = dex->data + dex->size;
    uint32_t value_count = 0;
    if (def.static_values_off && def.static_values_off < dex->size) {
        values = dex->data + def.static_values_off;
        value_count = dex_uleb(&values, end);
    }
    DexAnnotations annotations;
    dex_annotations(dex, def.annotations_off, &annotations);
    for (uint32_t i = 0; i < field_count; ++i) {
        uint32_t class_idx, type_idx;
        const char* name;
        dex_field(dex, data.fields[i].field_idx, &class_idx, &type_idx, &name);
        const char* type = dex_type(dex, type_idx);
        cf_u2(&body, data.fields[i].access & 0x50dfu);
        cf_u2(&body, cf_utf8(&pool, name));
        cf_u2(&body, cf_utf8(&pool, type));
        size_t attributes_at = body.size;
        if (i < data.static_count && i < value_count) {
            const unsigned char* at = values;
            DexValue v = dex_read_value(&values, end);
            if (v.type == DEX_VALUE_ARRAY || v.type == DEX_VALUE_ANNOTATION) {
                /* not a value a field's ConstantValue holds: past it */
                values = at;
                dex_skip_value(&values, end);
                cf_u2(&body, 0);
            } else {
                write_string_value(&pool, &body, dex, type, v);
            }
        } else {
            cf_u2(&body, 0);
        }
        add_attributes(&body, attributes_at,
                       cf_annotation_attributes(&pool, &body, dex, dex_field_annotations(&annotations, data.fields[i].field_idx)));
    }

    const char* super_desc = def.super_idx == DEX_NO_INDEX ? NULL : dex_type(dex, def.super_idx);
    uint32_t method_count = data.direct_count + data.virtual_count, synthetics = 0;
    for (size_t i = 0; i < program->synthetic_count; ++i)
        if (!strcmp(program->synthetics[i].cls, class_desc)) ++synthetics;
    cf_u2(&body, method_count + (super_desc ? synthetics : 0));
    for (size_t i = 0; i < program->synthetic_count && super_desc; ++i)
        if (!strcmp(program->synthetics[i].cls, class_desc))
            write_synthetic(&pool, &body, super_desc, &program->synthetics[i]);
    char failure[512] = "";
    for (uint32_t i = 0; i < method_count; ++i) {
        const DexMethod* dm = &data.methods[i];
        uint32_t class_idx, proto_idx;
        const char* name;
        dex_method(dex, dm->method_idx, &class_idx, &proto_idx, &name);
        Method m = {0};
        m.dex = dex;
        m.program = program;
        m.pool = &pool;
        m.class_desc = class_desc;
        m.super_desc = super_desc;
        m.interfaces_off = def.interfaces_off;
        m.method_idx = dm->method_idx;
        m.proto_idx = proto_idx;
        m.is_static = (dm->access & 0x8) != 0;
        m.is_ctor = !strcmp(name, "<init>");
        unsigned method_access = dm->access & 0x1ddfu;
        if (dm->access & 0x100) method_access |= dm->access & 0x20u; /* synchronized: only a native method's */
        /* a native method gets a body that calls the native bridge */
        bool native = (dm->access & 0x100) != 0;
        if (native) method_access &= ~0x100u;
        cf_u2(&body, method_access);
        cf_u2(&body, cf_utf8(&pool, name));
        cf_u2(&body, cf_utf8(&pool, method_descriptor(&m, proto_idx)));
        size_t attributes_at = body.size;
        if (native) {
            write_native_stub(&pool, &body, class_desc, name, method_descriptor(&m, proto_idx), m.is_static);
            method_attributes(&pool, &body, attributes_at, &annotations, dm->method_idx, proto_idx, name);
            method_free(&m);
            continue;
        }
        if (!dm->code_off) {
            cf_u2(&body, 0);
            method_attributes(&pool, &body, attributes_at, &annotations, dm->method_idx, proto_idx, name);
            method_free(&m);
            continue;
        }
        CfBuffer code = {0};
        bool ok = dex_code(dex, dm->code_off, &m.code);
        if (!ok) fail(&m, "a bad code_item");
        if (ok) ok = translate_code(&m, &code);
        if (!ok) {
            cf_free(&code);
            snprintf(failure, sizeof failure, "%s.%s%s: %s", class_desc, name, method_descriptor(&m, proto_idx),
                     m.error);
            if (options->warn) options->warn(options->context, failure);
            unsigned locals = m.code.ins ? m.code.ins : 1;
            write_failing_code(&pool, &code, failure, locals);
        }
        cf_bytes(&body, code.data, code.size);
        cf_free(&code);
        method_attributes(&pool, &body, attributes_at, &annotations, dm->method_idx, proto_idx, name);
        method_free(&m);
    }

    /* the class's attributes: its source file and its annotations */
    size_t attributes_at = body.size;
    if (def.source_file_idx != DEX_NO_INDEX) {
        cf_u2(&body, 1);
        cf_u2(&body, cf_utf8(&pool, "SourceFile"));
        cf_u4(&body, 2);
        cf_u2(&body, cf_utf8(&pool, dex_string(dex, def.source_file_idx)));
    } else {
        cf_u2(&body, 0);
    }
    add_attributes(&body, attributes_at, cf_annotation_attributes(&pool, &body, dex, annotations.class_set));
    dex_class_data_free(&data);

    unsigned char* result = NULL;
    if (pool.failed || pool.bytes.failed || body.failed) {
        snprintf(error, error_size, "%s: %s", class_desc,
                 pool.failed ? "the constant pool is past 65535 entries" : "out of memory");
    } else {
        CfBuffer file = {0};
        cf_u4(&file, 0xcafebabeu);
        cf_u2(&file, 0);
        cf_u2(&file, 52);
        cf_u2(&file, pool.count ? pool.count : 1);
        cf_bytes(&file, pool.bytes.data, pool.bytes.size);
        cf_bytes(&file, body.data, body.size);
        if (file.failed) {
            snprintf(error, error_size, "out of memory");
            cf_free(&file);
        } else {
            result = file.data;
            *size = file.size;
        }
    }
    cf_pool_free(&pool);
    cf_free(&body);
    return result;
}
