/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB board: the console the recompiled ROM runs on. It holds the cartridge, the memory map and the
   devices behind the I/O registers. */
#ifndef SNX_AGB_BOARD_H
#define SNX_AGB_BOARD_H
#include "agb/apu/apu.h"
#include "agb/cartridge/cartridge.h"
#include "agb/ppu/ppu.h"
#include "agb/board/memory.h"
#include "agb/board/save.h"

/* The opcode the BIOS has fetched last when control leaves it, the value reads of the BIOS return from
   outside it: after the boot, after a software interrupt, inside an interrupt handler (the BIOS called it),
   and after the handler returned through the BIOS. The console's BIOS is not shipped, its prefetch is. */
enum {
    AGB_BIOS_PREFETCH_BOOT = 0xE129F000u,
    AGB_BIOS_PREFETCH_SWI = 0xE3A02004u,
    AGB_BIOS_PREFETCH_IRQ = 0xE25EF004u,
    AGB_BIOS_PREFETCH_IRQ_RETURN = 0xE55EC002u
};

/* Where the BIOS's boot leaves the display when it starts the cartridge: the line and the cycles into it */
enum { AGB_BOOT_LINE = 126, AGB_BOOT_DOT = 919 };

/* The interrupt lines devices raise, the bits of the interrupt controller's registers. */
enum {
    AGB_IRQ_VBLANK = 0x001,
    AGB_IRQ_HBLANK = 0x002,
    AGB_IRQ_VCOUNT = 0x004,
    AGB_IRQ_SERIAL = 0x080,
    AGB_IRQ_KEYPAD = 0x1000,
    AGB_IRQ_GAMEPAK = 0x2000
};
#define AGB_IRQ_TIMER(n) (0x008u << (n))
#define AGB_IRQ_DMA(n)   (0x100u << (n))

/* The DMA trigger lines from the PPU and the FIFOs. */
enum { AGB_DMA_VBLANK, AGB_DMA_HBLANK, AGB_DMA_FIFO_A, AGB_DMA_FIFO_B };

typedef struct {
    uint32_t source, destination, source_now, destination_now;
    uint32_t latch; /* the last unit the channel read, which a read of nothing (the BIOS, below EWRAM) moves again */
    uint16_t count, control;
} AgbDmaChannel;

typedef struct {
    uint16_t reload, control;
    uint32_t counter, cycles;
} AgbTimer;

/* The cartridge's GPIO port (GBATEK "GBA Cart I/O Port (GPIO)"): its data, direction (a bit set: the console drives that
   pin) and control (bit 0: the port reads back, else it reads zeros) registers sit in the ROM's header. The cartridges
   with the real-time clock wire it there: pin 0 its clock (SCK), pin 1 its data (SIO), pin 2 its chip select */
enum { AGB_GPIO_DATA = 0xC4, AGB_GPIO_DIRECTION = 0xC6, AGB_GPIO_CONTROL = 0xC8 };
/* the seconds from 1970-01-01 to 2000-01-01, where the clock's years start */
#define AGB_RTC_EPOCH INT64_C(946684800)

typedef struct {
    uint8_t rtc;     /* the cartridge has the clock: its ROM carries the clock library's tag (agb_gpio_detect) */
    uint8_t touched; /* a write reached the port: view stands over the ROM's bytes at 0xC4 to 0xC9 */
    uint8_t latch, pins, direction, readable;
    uint8_t view[6];
    /* the clock's serial transfer: the command byte it took, the byte in transfer, its bits so far, the bytes left, and
       the SCK and SIO it last saw and drove */
    uint8_t command, active, bits, count, left, sck, sio;
    uint8_t control; /* the clock's status register: bit 6 its 24 hour mode */
    uint8_t time[7]; /* what a command latched, BCD: year, month, day, weekday, hour, minute, second */
    int64_t clock;   /* the local time the clock reads, seconds from 2000-01-01 00:00: the runner keeps it */
} AgbGpio;

typedef struct AgbBoard {
    AgbCartridge cartridge;
    AgbMemory memory;
    AgbPpu ppu;
    AgbApu apu;
    AgbSave save;
    AgbDmaChannel dma[4];
    AgbTimer timer[4];
    AgbGpio gpio;
    uint16_t irq_ie, irq_if, irq_ime;
    uint32_t cpu_halted;       /* the CPU waits for an interrupt (Halt) */
    uint32_t held_cycles;      /* the cycles the bus was busy since the CPU last ran (a DMA's), which it waits out */
    uint32_t counted_cycles;   /* while counting is set, the cycles of the accesses through agb_read and agb_write */
    uint8_t counting;
    uint16_t keys;             /* the keypad's keys held, KEYINPUT's bits set (the register reads them inverted) */
    uint32_t bios_prefetch;    /* the opcode the BIOS fetched last, what reads of the BIOS return */
    uint32_t open_bus;         /* the opcode the CPU fetched last, as its last read through the bus hook gave it */
    uint16_t io[AGB_IO_COUNT]; /* the I/O register file; devices claim registers here */
    uint16_t memcnt[2];        /* the internal memory control at 0x04000800, its two halfwords as written */
    int has_cartridge;
    uint32_t multiboot_size; /* a multiboot program's bytes at EWRAM's start; 0 with a cartridge */
    uint64_t multiboot_hash; /* the program's FNV-1a, which names its recompiled code as a ROM's hash does */
} AgbBoard;

void agb_board_init(AgbBoard* board);
/* Takes a copy of the image: returns 0, or -1 with a message when the image is not a GBA ROM. */
int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size);
void agb_board_destroy(AgbBoard* board);
/* Takes a multiboot program, as the BIOS leaves one it received through the cable: no cartridge, the image at EWRAM's
   start, so once EWRAM is the map's (agb_map_init). Returns 0, or -1 with a message naming the image's size when it
   is empty or EWRAM cannot hold it */
int agb_board_multiboot(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size);
/* Where the machine starts: the cartridge's first word, or a multiboot program's entry */
uint32_t agb_board_entry(const AgbBoard* board);

/* The I/O window at 0x04000000: the devices own their registers here. Recompiled code reaches them
   through the bus hook of AgbCpu, which routes every access to these. A store's mask is the lanes it writes,
   0xFFFF a halfword, 0x00FF or 0xFF00 a byte: the other lane keeps the register as written. */
uint16_t agb_io_read16(AgbBoard* board, uint32_t address);
void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value, uint16_t mask);
/* Devices request interrupts here. */
void agb_irq_request(AgbBoard* board, uint32_t mask);
/* The keypad's keys held now (KEYINPUT's bits set): KEYCNT's interrupt when its keys meet its condition */
void agb_board_keys(AgbBoard* board, uint16_t held);

/* The DMA controller and the timers (dma.c, timers.c). */
void agb_dma_write(AgbBoard* board, uint32_t offset, uint16_t value);
uint16_t agb_dma_read(AgbBoard* board, uint32_t offset);
uint16_t agb_dma_written(AgbBoard* board, uint32_t offset); /* as written: the addresses a read gives as 0 */
void agb_dma_event(AgbBoard* board, int event);
void agb_timer_write(AgbBoard* board, uint32_t offset, uint16_t value);
uint16_t agb_timer_read(AgbBoard* board, uint32_t offset);
uint16_t agb_timer_written(AgbBoard* board, uint32_t offset); /* as written: TMxCNT_L's reload, not its counter */
void agb_timer_step(AgbBoard* board, uint32_t cycles);
/* The cartridge's GPIO port and clock (gpio.c): 1 when the image carries the clock library's tag; the port as a
   cartridge starts it, with the clock or without; a halfword store at the port, 1 when the port took it */
int agb_gpio_detect(const uint8_t* image, size_t size);
void agb_gpio_reset(AgbBoard* board, int rtc);
int agb_gpio_write(AgbBoard* board, uint32_t address, uint16_t value);
#endif
