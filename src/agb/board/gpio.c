/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The cartridge's GPIO port and the real-time clock some cartridges wire to it (GBATEK "GBA Cart I/O Port (GPIO)" and
   "GBA Cart Real-Time Clock (RTC)"), answering the clock's serial protocol as mGBA's machine does */
#include "agb/board/board.h"
#include <string.h>

#define GPIO_SCK 1u
#define GPIO_SIO 2u
#define GPIO_CS  4u

/* the clock's commands, bits 4 to 6 of the command byte, and the bytes each moves */
enum { RTC_RESET = 0, RTC_DATETIME = 2, RTC_CONTROL = 4, RTC_TIME = 6 };
static const uint8_t rtc_bytes[8] = {0, 0, 7, 0, 1, 0, 3, 0};
#define RTC_MAGIC       0x6u  /* bits 0 to 3 of every command byte */
#define RTC_READ        0x80u /* bit 7: the command sends its bytes to the console */
#define RTC_HOUR24      0x40u /* the status register's 24 hour mode, set as the clock starts */
#define RTC_DAY_SECONDS 86400

static int command_of(const AgbGpio* gpio) { return gpio->command >> 4 & 7; }

/* the registers as the console reads them once the game wrote the port: pins, direction and control while it reads
   back, zeros while it is write-only */
static void show(AgbBoard* board) {
    AgbGpio* gpio = &board->gpio;
    uint8_t view[6] = {0};
    gpio->touched = 1;
    if (gpio->readable) {
        view[0] = gpio->pins;
        view[2] = gpio->direction;
        view[4] = 1;
    }
    memcpy(gpio->view, view, sizeof view);
    /* recompiled code reads the cartridge in the flat window: one memory behind its three windows, or a copy at each */
    if (board->memory.rom)
        for (uint32_t window = 0; window < board->memory.rom_windows; ++window)
            memcpy(board->memory.rom + window * AGB_ROM_MAX + AGB_GPIO_DATA, view, sizeof view);
}

/* the pins the console leaves as inputs (direction clear) take what the cartridge drives */
static void drive(AgbBoard* board, unsigned pins) {
    AgbGpio* gpio = &board->gpio;
    gpio->pins = (uint8_t)((gpio->pins & gpio->direction) | (pins & ~gpio->direction & 0xFu));
    if (gpio->readable) show(board);
}

/* a value 0 to 99 in the clock's binary coded decimal */
static uint8_t bcd(unsigned value) { return (uint8_t)((value / 10 % 10) << 4 | value % 10); }

/* the date and time the clock holds now, from its seconds since 2000-01-01 00:00, a Saturday */
static void latch(AgbGpio* gpio) {
    int64_t days = gpio->clock / RTC_DAY_SECONDS, second = gpio->clock % RTC_DAY_SECONDS;
    if (second < 0) second += RTC_DAY_SECONDS, --days;
    int64_t weekday = ((days + 6) % 7 + 7) % 7;
    /* the civil date of a day count, counted from 2000-03-01 in eras of 400 years (Howard Hinnant's civil_from_days) */
    int64_t z = days - 60, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t of_era = z - era * 146097;
    int64_t year_of_era = (of_era - of_era / 1460 + of_era / 36524 - of_era / 146096) / 365;
    int64_t of_year = of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int64_t shifted = (5 * of_year + 2) / 153, day = of_year - (153 * shifted + 2) / 5 + 1;
    int64_t month = shifted < 10 ? shifted + 3 : shifted - 9, year = year_of_era + era * 400 + (month <= 2);
    unsigned hour = (unsigned)(second / 3600);
    gpio->time[0] = bcd((unsigned)((year % 100 + 100) % 100));
    gpio->time[1] = bcd((unsigned)month);
    gpio->time[2] = bcd((unsigned)day);
    gpio->time[3] = bcd((unsigned)weekday);
    gpio->time[4] = bcd(gpio->control & RTC_HOUR24 ? hour : hour % 12);
    gpio->time[5] = bcd((unsigned)(second / 60 % 60));
    gpio->time[6] = bcd((unsigned)(second % 60));
}

/* the bytes left of a transfer, from the command's count again once they ran out */
static void next_byte(AgbGpio* gpio) {
    gpio->count = 0;
    gpio->left = gpio->left <= 1 ? rtc_bytes[command_of(gpio)] : (uint8_t)(gpio->left - 1);
}

/* the command byte, its bits in from bit 0: one with the fixed code starts its transfer */
static void begin(AgbGpio* gpio) {
    if ((gpio->bits & 0xF) == RTC_MAGIC) {
        gpio->command = gpio->bits;
        gpio->left = rtc_bytes[command_of(gpio)];
        gpio->active = 1;
        if (command_of(gpio) == RTC_RESET) gpio->control = 0;
        if (command_of(gpio) == RTC_DATETIME || command_of(gpio) == RTC_TIME) latch(gpio);
    }
    gpio->bits = 0;
    gpio->count = 0;
}

/* a byte the console wrote: the status register takes it */
static void take(AgbGpio* gpio) {
    if (command_of(gpio) == RTC_CONTROL) gpio->control = gpio->bits;
    gpio->bits = 0;
    next_byte(gpio);
}

/* the bit the clock puts on SIO for a read: the status register's, or the latched date's byte the transfer is at */
static unsigned give(const AgbGpio* gpio) {
    unsigned byte = 0xFF;
    if (command_of(gpio) == RTC_CONTROL) byte = gpio->control;
    if (command_of(gpio) == RTC_DATETIME || command_of(gpio) == RTC_TIME) byte = gpio->time[7 - gpio->left];
    return byte >> gpio->count & 1;
}

/* SIO's bit into the byte in transfer while SCK is low */
static void sample(AgbGpio* gpio) {
    uint8_t bit = (uint8_t)(1u << gpio->count);
    if (!(gpio->pins & GPIO_SCK)) gpio->bits = (uint8_t)((gpio->bits & ~bit) | (gpio->pins & GPIO_SIO ? bit : 0));
}

/* The clock sees the pins. It drives its own low but SIO, which it holds high outside a read. CS low ends any transfer;
   with CS high a rising SCK takes the bit sampled on SIO (the command, then a write's bytes, from bit 0), and in a read
   a falling SCK puts the clock's next bit on SIO */
static void rtc_pins(AgbBoard* board) {
    AgbGpio* gpio = &board->gpio;
    drive(board, gpio->pins & GPIO_SIO);
    if (!(gpio->pins & GPIO_CS)) {
        gpio->count = gpio->left = gpio->active = gpio->command = 0;
        gpio->sck = gpio->sio = 1;
        drive(board, GPIO_SIO);
        return;
    }
    int rising = !gpio->sck && (gpio->pins & GPIO_SCK);
    if (!gpio->active) {
        drive(board, GPIO_SIO);
        sample(gpio);
        if (rising && ++gpio->count == 8) begin(gpio);
    } else if (!(gpio->command & RTC_READ)) {
        drive(board, GPIO_SIO);
        sample(gpio);
        if (rising) {
            /* SIO changing as SCK rises: the clock takes 0 for a data bit */
            if ((gpio->bits >> gpio->count & 1) ^ (gpio->pins & GPIO_SIO) >> 1)
                gpio->bits &= (uint8_t)~(1u << gpio->count);
            if (++gpio->count == 8) take(gpio);
        }
    } else {
        if (gpio->sck && !(gpio->pins & GPIO_SCK)) {
            gpio->sio = (uint8_t)give(gpio);
            if (++gpio->count == 8) next_byte(gpio);
        }
        drive(board, (unsigned)gpio->sio << 1);
    }
    gpio->sck = (uint8_t)(gpio->pins & GPIO_SCK);
}

int agb_gpio_detect(const uint8_t* image, size_t size) {
    static const char tag[] = "SIIRTC_V";
    for (size_t i = 0; i + sizeof tag - 1 <= size; ++i)
        if (!memcmp(image + i, tag, sizeof tag - 1)) return 1;
    return 0;
}

void agb_gpio_reset(AgbBoard* board, int rtc) {
    int64_t clock = board->gpio.clock;
    board->gpio = (AgbGpio){0};
    board->gpio.rtc = (uint8_t)(rtc != 0);
    board->gpio.clock = clock;
    board->gpio.sck = board->gpio.sio = 1;
    board->gpio.control = RTC_HOUR24;
}

int agb_gpio_write(AgbBoard* board, uint32_t address, uint16_t value) {
    AgbGpio* gpio = &board->gpio;
    uint32_t at = address & 0x0FFFFFFE;
    if (!gpio->rtc || at < 0x08000000 + AGB_GPIO_DATA || at > 0x08000000 + AGB_GPIO_CONTROL) return 0;
    if (at == 0x08000000 + AGB_GPIO_CONTROL)
        gpio->readable = value & 1;
    else {
        if (at == 0x08000000 + AGB_GPIO_DATA)
            gpio->latch = value & 0xF;
        else
            gpio->direction = value & 0xF;
        gpio->pins = (uint8_t)((gpio->pins & ~gpio->direction) | (gpio->latch & gpio->direction));
        rtc_pins(board);
    }
    show(board);
    return 1;
}
