/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic programs only. Field positions follow Mesa's mme_tu104_encode, independently of the decoder. */
#include "nx/gpu/maxwell/mme_tu104.c"
#include <stdarg.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
GpuState g_gpu;
int g_verbose;
static u32 sent[128], methods[128], engines[128], nsent, pause_method;
static u8 memory[128];
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void fatal(const char* format, ...) {
    (void)format;
    abort();
}
void r3d_const_flush(GpuChannel* ch) { (void)ch; }
void r3d_pending_flush(void) {}
int gmem_read(u64 address, void* out, u64 size) {
    if (address < 0x10000 || address - 0x10000 > sizeof memory || size > sizeof memory - (address - 0x10000)) return -1;
    memcpy(out, memory + (address - 0x10000), (size_t)size);
    return 0;
}
int gmem_write(u64 address, const void* in, u64 size) {
    if (address < 0x10000 || address - 0x10000 > sizeof memory || size > sizeof memory - (address - 0x10000)) return -1;
    memcpy(memory + (address - 0x10000), in, (size_t)size);
    return 0;
}
static void emit_method(GpuChannel* ch, u32 engine, u32 method, u32 value) {
    CHECK(nsent < 128);
    sent[nsent] = value;
    methods[nsent] = method;
    engines[nsent++] = engine;
    registers(ch, engine)[method] = value;
    if (method == pause_method) {
        ch->park = GPU_PARK_REPORT;
        pause_method = ~0u;
    }
}
void maxwell3d_method(GpuChannel* ch, u32 method, u32 value, int last) {
    (void)last;
    emit_method(ch, 1, method, value);
}
void compute_method(GpuChannel* ch, u32 method, u32 value, int native) {
    (void)native;
    emit_method(ch, 2, method, value);
}

/* A bit-at-a-time assembler does not use the runtime's field reader. */
typedef struct {
    u32 op[2], dst[2], a[2], b[2], imm[2], method[2], emit[2], pred, pred_reg, end;
} Inst;
static Inst nop(void) { return (Inst){.dst = {24, 24}, .a = {24, 24}, .b = {24, 24}, .pred_reg = 24}; }
static void bits(u32 code[3], u32 low, u32 width, u32 value) {
    for (u32 bit = 0; bit < width; ++bit)
        if (value & (1u << bit)) code[2 - (low + bit) / 32] |= 1u << ((low + bit) % 32);
}
static void instruction(GpuChannel* ch, u32 engine, Inst in) {
    u32 code[3] = {0};
    bits(code, 0, 1, in.end);
    bits(code, 1, 4, in.pred);
    bits(code, 5, 5, in.pred_reg);
    for (u32 lane = 0; lane < 2; ++lane) {
        bits(code, 10 + 36 * lane, 5, in.op[lane]);
        bits(code, 15 + 36 * lane, 5, in.dst[lane]);
        bits(code, 20 + 36 * lane, 5, in.a[lane]);
        bits(code, 25 + 36 * lane, 5, in.b[lane]);
        bits(code, 30 + 36 * lane, 16, in.imm[lane]);
        bits(code, 82 + 7 * lane, 3, in.method[lane]);
        bits(code, 85 + 7 * lane, 4, in.emit[lane]);
    }
    for (u32 word = 0; word < 3; ++word) CHECK(mme_tu104_method(ch, engine, 0x46, code[word], 1));
}
static void begin(GpuChannel* ch, u32 engine, u32 address) {
    CHECK(mme_tu104_method(ch, engine, 0x45, address, 1));
    CHECK(mme_tu104_method(ch, engine, 0x47, 0, 1));
    CHECK(mme_tu104_method(ch, engine, 0x48, address, 1));
}
static void method(GpuChannel* ch, u32 engine, u32 address) {
    Inst in = nop();
    in.imm[0] = 0x1000 | address;
    in.method[0] = 5;
    instruction(ch, engine, in);
}
static void send_reg(GpuChannel* ch, u32 engine, u32 reg, int end) {
    Inst in = nop();
    in.a[0] = reg;
    in.emit[0] = 1;
    in.end = (u32)end;
    instruction(ch, engine, in);
}
static void call(GpuChannel* ch, u32 engine, const u32* parameters, u32 count) {
    nsent = 0;
    pause_method = ~0u;
    CHECK(count > 0);
    for (u32 i = 0; i < count; ++i)
        CHECK(mme_tu104_method(ch, engine, 0xE00 + (i != 0), parameters[i], i + 1 == count));
}
static GpuChannel a, b, c;
static void reset(void) {
    mme_tu104_free(&a);
    mme_tu104_free(&b);
    mme_tu104_free(&c);
    for (u32 i = 0; i < g_gpu.tu104_capacity; ++i)
        if (g_gpu.tu104[i]) mme_tu104_tsg_close(g_gpu.tu104[i]->tsg);
    a = (GpuChannel){.id = 1};
    b = (GpuChannel){.id = 2};
    c = (GpuChannel){.id = 3};
    g_gpu.chan[1] = &a;
    g_gpu.chan[2] = &b;
    g_gpu.chan[3] = &c;
    nsent = 0;
    pause_method = ~0u;
}
static void arithmetic(void) {
    static const struct {
        u32 op, x, y, imm, expected;
    } cases[] = {{0, 0xFFFFFFFF, 2, 0, 1},
                 {2, 0, 1, 0, 0xFFFFFFFF},
                 {4, 0xFFFFFFFD, 7, 0, 0xFFFFFFEB},
                 {6, 0xFFFFFFFF, 2, 0, 0xFFFFFFFE},
                 {8, 0, 0, 0, 32},
                 {8, 0x1000, 0, 0, 19},
                 {9, 0x80000001, 33, 0, 2},
                 {10, 0x80000001, 33, 0, 0x40000000},
                 {11, 0x80000001, 33, 0, 0xC0000000},
                 {12, 0xF0F0, 0xFF00, 0, 0xF000},
                 {13, 0xF0F0, 0xFF00, 0, 0xFFFF0FFF},
                 {14, 0xF0F0, 0xFF00, 0, 0xFFF0},
                 {15, 0xF0F0, 0xFF00, 0, 0xFF0},
                 {16, 0xFFFFFFFF, 0x12345678, (8 << 10) | (8 << 5) | 4, 0xFFFF67FF},
                 {17, 0xFFFFFFFF, 1, 0, 0xFFFFFFFF},
                 {18, 0xFFFFFFFF, 1, 0, 0},
                 {19, 1, 1, 0, 0xFFFFFFFF},
                 {20, 1, 0, 0, 0},
                 {21, 0xABC, 0xABC, 0, 0xFFFFFFFF}};
    for (u32 i = 0; i < sizeof cases / sizeof *cases; ++i) {
        begin(&a, 1, 0x31);
        method(&a, 1, 0x220);
        Inst in = nop();
        in.op[0] = cases[i].op;
        in.a[0] = 28;
        in.b[0] = 29;
        in.imm[0] = cases[i].imm;
        in.emit[0] = 1;
        in.end = 1;
        instruction(&a, 1, in);
        instruction(&a, 1, nop());
        u32 p[] = {cases[i].x, cases[i].y};
        call(&a, 1, p, 2);
        CHECK(!a.mme.running && nsent == 1 && methods[0] == 0x220 && sent[0] == cases[i].expected);
    }
    for (u32 subtract = 0; subtract < 2; ++subtract) {
        begin(&a, 1, 0);
        method(&a, 1, 0x220);
        Inst in = nop();
        in.op[0] = subtract ? 2 : 0;
        in.op[1] = subtract ? 3 : 1;
        in.a[0] = 28;
        in.b[0] = 29;
        in.emit[0] = 1;
        in.emit[1] = 2;
        in.end = 1;
        instruction(&a, 1, in);
        instruction(&a, 1, nop());
        u32 p[] = {subtract ? 0 : ~0u, 1};
        call(&a, 1, p, 2);
        CHECK(nsent == 2 && sent[0] == (subtract ? ~0u : 0) && sent[1] == (subtract ? ~0u : 1));
    }
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    Inst in = nop();
    in.op[0] = 4;
    in.a[0] = 28;
    in.b[0] = 29;
    in.op[1] = 5;
    in.emit[0] = 1;
    in.emit[1] = 2;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    const u32 p[] = {0x80000000, 3};
    call(&a, 1, p, 2);
    CHECK(nsent == 2 && sent[0] == 0x80000000 && sent[1] == 0xFFFFFFFE);
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    in.op[0] = 6;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    call(&a, 1, p, 2);
    CHECK(nsent == 2 && sent[0] == 0x80000000 && sent[1] == 1);
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    in = nop();
    in.a[0] = 27;
    in.imm[0] = 0xFFFF;
    in.imm[1] = 0xABCD;
    in.emit[0] = 1;
    instruction(&a, 1, in);
    in.emit[0] = 8;
    in.emit[1] = 9;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    call(&a, 1, p, 1);
    CHECK(nsent == 3 && sent[0] == 0xFFFFABCD && sent[1] == 15 && sent[2] == 10);
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    in = nop();
    in.dst[0] = 0;
    in.a[0] = 25;
    in.imm[0] = 7;
    instruction(&a, 1, in);
    in.imm[0] = 11;
    in.a[1] = 0;
    in.b[1] = 25;
    in.imm[1] = 2;
    in.emit[1] = 2;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    call(&a, 1, p, 1);
    CHECK(nsent == 1 && sent[0] == 9); /* ALU1 sees r0 before ALU0 replaces it with 11. */
}
static void predicates_and_loops(void) {
    for (u32 truth = 0; truth < 2; ++truth) {
        begin(&a, 1, 0);
        method(&a, 1, 0x220);
        Inst in = nop();
        in.dst[0] = 0;
        in.a[0] = 25;
        in.imm[0] = truth;
        instruction(&a, 1, in);
        in = nop();
        in.pred = 1;
        in.pred_reg = 0;
        in.a[0] = 28;
        in.b[0] = 29;
        in.emit[0] = 1;
        instruction(&a, 1, in);
        send_reg(&a, 1, 28, 1);
        instruction(&a, 1, nop());
        const u32 p[] = {5, 7, 11};
        call(&a, 1, p, 3);
        CHECK(!a.mme.running && nsent == (truth ? 1 : 2) && sent[nsent - 1] == 11 && a.mme.next == 3);
        if (!truth) CHECK(sent[0] == 12);
    }
    for (u32 count = 0; count < 4; ++count) {
        begin(&a, 1, 0);
        method(&a, 1, 0x220);
        Inst in = nop();
        in.dst[0] = 0;
        instruction(&a, 1, in);
        in = nop();
        in.dst[0] = 1;
        in.a[0] = 25;
        in.imm[0] = count;
        instruction(&a, 1, in);
        in = nop();
        in.op[0] = 23;
        in.a[0] = 1;
        in.imm[0] = 3;
        instruction(&a, 1, in);
        in = nop();
        in.dst[0] = 0;
        in.a[0] = 0;
        in.b[0] = 28;
        instruction(&a, 1, in);
        instruction(&a, 1, nop());
        send_reg(&a, 1, 0, 1);
        instruction(&a, 1, nop());
        const u32 p[] = {1, 2, 3};
        call(&a, 1, p, 3);
        CHECK(!a.mme.running && nsent == 1 && sent[0] == count * (count + 1) / 2 && a.mme.next == count);
    }
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    Inst in = nop();
    in.dst[0] = 0;
    in.a[0] = 25;
    in.imm[0] = 3;
    instruction(&a, 1, in);
    in = nop();
    in.dst[0] = 0;
    in.a[0] = 0;
    in.b[0] = 25;
    in.imm[0] = 0xFFFF;
    in.emit[0] = 1;
    instruction(&a, 1, in);
    in = nop();
    in.op[0] = 29;
    in.a[0] = 0;
    in.imm[0] = 0x5FFF;
    in.end = 1;
    instruction(&a, 1, in);
    in = nop();
    in.a[0] = 25;
    in.imm[0] = 0x55;
    in.emit[0] = 1;
    instruction(&a, 1, in);
    const u32 p[] = {0};
    call(&a, 1, p, 1);
    CHECK(!a.mme.running && nsent == 4 && sent[0] == 2 && sent[1] == 1 && sent[2] == 0 && sent[3] == 0x55);

    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    in = nop();
    in.op[0] = 24;
    in.imm[0] = 0x8002;
    instruction(&a, 1, in);
    send_reg(&a, 1, 28, 0); /* the jump must skip this FIFO read */
    in = nop();
    in.op[0] = 29;
    in.imm[0] = 0x9000;
    in.imm[1] = 42;
    in.emit[0] = 6;
    instruction(&a, 1, in);
    call(&a, 1, p, 1);
    CHECK(!a.mme.running && nsent == 1 && sent[0] == 42 && a.mme.next == 0);
}
static void comparisons(void) {
    static const struct {
        u32 op, x, y, result;
    } cases[] = {{25, 0xFFFFFFFF, 1, 1},
                 {26, 0xFFFFFFFF, 1, 0},
                 {27, 0x80000000, 0x80000000, 1},
                 {28, 0xFFFFFFFF, 0, 0},
                 {29, 7, 8, 0}};
    for (u32 i = 0; i < sizeof cases / sizeof *cases; ++i)
        for (u32 expected = 0; expected < 2; ++expected) {
            begin(&a, 1, 0);
            method(&a, 1, 0x220);
            Inst in = nop();
            in.op[0] = cases[i].op;
            in.a[0] = 28;
            in.b[0] = 29;
            in.imm[0] = expected << 15 | 2;
            instruction(&a, 1, in);
            in = nop();
            in.a[0] = 25;
            in.imm[0] = 10;
            in.emit[0] = 1;
            instruction(&a, 1, in);
            in.imm[0] = 20;
            in.end = 1;
            instruction(&a, 1, in);
            instruction(&a, 1, nop());
            u32 p[] = {cases[i].x, cases[i].y};
            call(&a, 1, p, 2);
            int taken = cases[i].result == expected;
            CHECK(nsent == (taken ? 1 : 2) && sent[nsent - 1] == 20);
            if (!taken) CHECK(sent[0] == 10);
        }
}
static void ram_and_dma(void) {
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    Inst in = nop();
    in.op[0] = 31;
    in.a[0] = 25;
    in.imm[0] = 7;
    in.b[0] = 28;
    instruction(&a, 1, in);
    in = nop();
    in.op[0] = 30;
    in.a[0] = 25;
    in.imm[0] = 7;
    in.emit[0] = 1;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    const u32 p[] = {0x12345678};
    call(&a, 1, p, 1);
    CHECK(nsent == 1 && sent[0] == p[0]);
    /* A second call reads RAM without a write. Compute runs the same program on the same RAM. */
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    call(&a, 1, p, 1);
    CHECK(nsent == 1 && sent[0] == p[0]);
    call(&a, 2, p, 1);
    CHECK(nsent == 1 && sent[0] == p[0] && engines[0] == 2);
    snx_st_le32(memory, 17);
    snx_st_le32(memory + 4, 25);
    CHECK(mme_tu104_method(&a, 1, 0x154, 0, 1));
    CHECK(mme_tu104_method(&a, 1, 0x155, 0x10000, 1));
    CHECK(mme_tu104_method(&a, 1, 0x156, 7, 1));
    CHECK(mme_tu104_method(&a, 1, 0x157, 2, 1));
    call(&a, 1, p, 1);
    CHECK(nsent == 1 && sent[0] == 17);
    CHECK(mme_tu104_method(&a, 1, 0x15C, 99, 1));
    call(&a, 1, p, 1);
    CHECK(nsent == 1 && sent[0] == 99);
    snx_st_le32(memory + 8, 0xDEADBEEF);
    CHECK(mme_tu104_method(&a, 1, 0x159, 2, 1));
    CHECK(snx_ld_le32(memory) == 99 && snx_ld_le32(memory + 4) == 25 && snx_ld_le32(memory + 8) == 0xDEADBEEF);
    snx_st_le32(memory, 17);

    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    in = nop();
    in.imm[0] = 2;
    in.method[0] = 6;
    in.imm[1] = 0x158;
    in.emit[0] = 5;
    instruction(&a, 1, in);
    in = nop();
    in.op[0] = 7;
    in.a[0] = 25;
    in.b[0] = 26;
    in.imm[0] = 0x1000;
    in.imm[1] = 1;
    instruction(&a, 1, in);
    method(&a, 1, 0x220);
    in = nop();
    in.a[0] = 28;
    in.b[0] = 29;
    in.emit[0] = 1;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    call(&a, 1, p, 1);
    CHECK(!a.mme.running && nsent == 1 && sent[0] == 42);
    /* A FIFO read issued before the call stays pending until its barrier, independently of compute. */
    CHECK(mme_tu104_method(&a, 1, 0x47, 0, 1));
    CHECK(mme_tu104_method(&a, 1, 0x48, 2, 1));
    CHECK(mme_tu104_method(&a, 1, 0x158, 2, 1));
    begin(&a, 2, 0);
    method(&a, 2, 0x230);
    in = nop();
    in.a[0] = 28;
    in.b[0] = 25;
    in.imm[0] = 1;
    in.emit[0] = 1;
    in.end = 1;
    instruction(&a, 2, in);
    instruction(&a, 2, nop());
    call(&a, 2, p, 1);
    CHECK(nsent == 1 && sent[0] == p[0] + 1);
    call(&a, 1, p, 1);
    CHECK(!a.mme.running && nsent == 1 && sent[0] == 42);
}
static void sharing_and_resume(void) {
    reset();
    mme_tu104_set_group(&a, 100);
    mme_tu104_set_group(&b, 100);
    mme_tu104_set_group(&c, 101);
    begin(&a, 1, 0x31);
    method(&a, 1, 0x220);
    Inst in = nop();
    in.a[0] = 28;
    in.b[0] = 26;
    in.imm[1] = 7;
    in.emit[0] = 1;
    in.emit[1] = 6;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    const u32 p[] = {23};
    mme_tu104_free(&a);
    call(&b, 1, p, 1);
    CHECK(nsent == 2 && sent[0] == 30 && sent[1] == 7); /* the uploader closed before the sibling's first call */
    call(&c, 1, p, 1);
    CHECK(!c.mme.running && nsent == 0); /* a different TSG must not inherit code */
    nsent = 0;
    pause_method = 0x220;
    CHECK(mme_tu104_method(&b, 1, 0xE00, p[0], 1));
    CHECK(b.park && b.mme.running && nsent == 1 && sent[0] == 30);
    b.park = 0;
    mme_tu104_resume(&b);
    CHECK(!b.mme.running && nsent == 2 && methods[1] == 0x221 && sent[1] == 7 && b.mme.next == 1);
    /* A compute upload replaces only compute's program, not graphics or another TSG's code. */
    begin(&b, 2, 0);
    method(&b, 2, 0x240);
    in = nop();
    in.op[0] = 22;
    in.a[0] = 25;
    in.imm[0] = 0x250;
    in.emit[0] = 1;
    in.end = 1;
    instruction(&b, 2, in);
    instruction(&b, 2, nop());
    b.rcomp[0x250] = 0xABC;
    call(&b, 2, p, 1);
    CHECK(nsent == 1 && engines[0] == 2 && methods[0] == 0x240 && sent[0] == 0xABC);
    call(&b, 1, p, 1);
    CHECK(nsent == 2 && sent[0] == 30);
    mme_tu104_set_group(&b, 102);
    call(&b, 1, p, 1);
    CHECK(!b.mme.running && nsent == 0);
}
static void compute_program_resume(void) {
    reset();
    mme_tu104_set_group(&a, 200);
    mme_tu104_set_group(&b, 200);
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    send_reg(&a, 1, 28, 0);
    Inst in = nop();
    in.imm[0] = 91;
    in.emit[0] = 5;
    in.end = 1;
    instruction(&a, 1, in);
    instruction(&a, 1, nop());
    nsent = 0;
    pause_method = 0x220;
    CHECK(mme_tu104_method(&b, 2, 0xE00, 23, 1));
    CHECK(b.park && b.mme.running && nsent == 1 && sent[0] == 23);
    begin(&a, 2, 0);
    method(&a, 2, 0x240);
    in.imm[0] = 777;
    instruction(&a, 2, in);
    instruction(&a, 2, nop());
    b.park = 0;
    mme_tu104_resume(&b);
    CHECK(!b.mme.running && nsent == 2 && sent[1] == 91 && methods[1] == 0x221);
    const u32 p[] = {0};
    call(&b, 2, p, 1);
    CHECK(nsent == 1 && sent[0] == 777 && methods[0] == 0x240);
}
/* A compute bind alone gives compute its own start table and program, as a compute upload does. */
static void compute_bind_switch(void) {
    reset();
    mme_tu104_set_group(&a, 300);
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    send_reg(&a, 1, 28, 1);
    instruction(&a, 1, nop());
    const u32 p[] = {5};
    call(&a, 2, p, 1);
    CHECK(!a.mme.running && nsent == 1 && sent[0] == 5 && engines[0] == 2);
    CHECK(mme_tu104_method(&a, 2, 0x47, 0, 1));
    CHECK(mme_tu104_method(&a, 2, 0x48, 0, 1));
    call(&a, 2, p, 1);
    CHECK(!a.mme.running && nsent == 0);
    call(&a, 1, p, 1);
    CHECK(!a.mme.running && nsent == 1 && sent[0] == 5 && engines[0] == 1);
}
static void lane1_resume(void) {
    reset();
    for (u32 loop = 0; loop < 2; ++loop) {
        begin(&a, 1, 0);
        Inst in = nop();
        in.imm[0] = 0x1220;
        in.method[0] = 5;
        in.end = !loop;
        instruction(&a, 1, in);
        if (loop) {
            in = nop();
            in.op[0] = 23;
            in.a[0] = 28;
            in.imm[0] = 2;
            instruction(&a, 1, in);
        }
        in = nop();
        in.imm[0] = 11;
        in.imm[1] = 22;
        in.emit[0] = 5;
        in.emit[1] = 6;
        instruction(&a, 1, in);
        in = nop();
        in.imm[0] = 33;
        in.emit[0] = loop ? 0 : 5;
        in.end = 1;
        instruction(&a, 1, in);
        instruction(&a, 1, nop());
        nsent = 0;
        pause_method = 0x221;
        CHECK(mme_tu104_method(&a, 1, 0xE00, 2, 1));
        CHECK(a.park && nsent == 2 && sent[0] == 11 && sent[1] == 22);
        a.park = 0;
        mme_tu104_resume(&a);
        CHECK(!a.mme.running && nsent == (loop ? 4 : 2));
        if (loop) CHECK(sent[2] == 11 && sent[3] == 22 && methods[2] == 0x222 && methods[3] == 0x223);
    }
}
static void parameter_overflow(void) {
    reset();
    begin(&a, 1, 0);
    method(&a, 1, 0x220);
    send_reg(&a, 1, 28, 1);
    instruction(&a, 1, nop());
    u32 count = sizeof a.mme_params / sizeof *a.mme_params + 2;
    for (u32 i = 0; i < count; ++i) CHECK(mme_tu104_method(&a, 1, 0xE00 + (i != 0), 23, i + 1 == count));
    CHECK(!a.mme_collecting && !a.mme.running && nsent == 0);
    const u32 p[] = {31};
    call(&a, 1, p, 1);
    CHECK(nsent == 1 && sent[0] == 31);
}
static void group_growth(void) {
    reset();
    for (u32 token = 1; token <= 129; ++token) {
        mme_tu104_set_group(&a, token);
        begin(&a, 1, 0);
        Inst in = nop();
        in.imm[0] = 0x1220;
        in.imm[1] = token;
        in.method[0] = 5;
        in.emit[0] = 6;
        in.end = 1;
        instruction(&a, 1, in);
        instruction(&a, 1, nop());
    }
    mme_tu104_free(&a);
    const u32 p[] = {0};
    for (u32 token = 1; token <= 129; token += 128) {
        mme_tu104_set_group(&b, token);
        call(&b, 1, p, 1);
        CHECK(nsent == 1 && sent[0] == token);
    }
}
static void emitted_bind(void) {
    reset();
    for (u32 engine = 1; engine <= 2; ++engine) {
        begin(&a, engine, 0);
        method(&a, engine, 0x47);
        Inst in = nop();
        in.imm[0] = 1;
        in.emit[0] = 5;
        instruction(&a, engine, in);
        in.imm[0] = 6;
        in.end = 1;
        instruction(&a, engine, in);
        for (u32 i = 0; i < 3; ++i) instruction(&a, engine, nop());
        in = nop();
        in.imm[0] = 0x1220;
        in.imm[1] = 0xCAFE;
        in.method[0] = 5;
        in.emit[0] = 6;
        in.end = 1;
        instruction(&a, engine, in);
        instruction(&a, engine, nop());
        const u32 p[] = {0};
        call(&a, engine, p, 1);
        nsent = 0;
        CHECK(mme_tu104_method(&a, engine, 0xE02, 0, 1));
        CHECK(nsent == 1 && engines[0] == engine && sent[0] == 0xCAFE);
    }
}
int main(void) {
    reset();
    arithmetic();
    predicates_and_loops();
    comparisons();
    ram_and_dma();
    sharing_and_resume();
    compute_program_resume();
    compute_bind_switch();
    lane1_resume();
    parameter_overflow();
    group_growth();
    emitted_bind();
    reset();
    for (u32 i = 0; i < g_gpu.tu104_capacity; ++i) CHECK(!g_gpu.tu104[i]);
    free(g_gpu.tu104);
    printf("mme_tu104: %u checks passed\n", checks);
    return 0;
}
