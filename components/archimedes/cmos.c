/*
 * The PCF8583 clock and RAM on the IOC's I2C lines (C1 the clock, C0 the
 * data, open drain: either side pulls a line low). Address A0/A1; a write
 * sets the register pointer and stores bytes, a read sends bytes from the
 * pointer, which counts up either way.
 *
 *   00 control  01 1/100 s  02 seconds  03 minutes  04 hours (24 h)
 *   05 year (bits 7-6, 0-3) and date    06 weekday (bits 7-5) and month
 *   07-0F timer and alarm               10-FF RAM: RISC OS's CMOS memory
 *
 * The clock is the host's (in BCD); setting it is not passed on. RISC OS
 * keeps the year itself in RAM bytes C0 (00-99) and C1 (the century):
 * those are set from the config at power-on. RISC OS 3 keeps a checksum of
 * the RAM in 3F (and finds the configuration bad if it's wrong).
 */
#include <string.h>
#include <time.h>
#include "arc_internal.h"

#define DEVICE      0xA0

typedef enum { I2C_IDLE, I2C_ADDR, I2C_REG, I2C_WRITE, I2C_READ } i2c_state_t;

static struct {
    bool scl, sda;                      /* the IOC's side */
    bool slave_sda;                     /* ours: true released */
    i2c_state_t state;
    int bit;
    uint8_t shift;
    bool reading;
    bool acking;                        /* the ninth clock */
    bool nacked;
    uint8_t pointer;
    uint8_t control;
} i2c;

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t read_register(uint8_t r)
{
    if (r >= 0x10) {
        return arc.cfg.cmos[r];
    }
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    switch (r) {
    case 0x00: return i2c.control;
    case 0x01: return 0;
    case 0x02: return bcd(tm.tm_sec > 59 ? 59 : tm.tm_sec);
    case 0x03: return bcd(tm.tm_min);
    case 0x04: return bcd(tm.tm_hour);
    case 0x05: return (uint8_t)(((tm.tm_year + 1900) & 3) << 6 | bcd(tm.tm_mday));
    case 0x06: return (uint8_t)((tm.tm_wday & 7) << 5 | bcd(tm.tm_mon + 1));
    default: return arc.cfg.cmos[r];
    }
}

static void write_register(uint8_t r, uint8_t v)
{
    if (r == 0x00) {
        i2c.control = v;
        return;
    }
    if (r < 0x07) {
        return;                         /* the clock is the host's */
    }
    if (arc.cfg.cmos[r] != v) {
        arc.cfg.cmos[r] = v;
        if (r >= 0x10 && arc.cfg.cmos_changed) {
            arc.cfg.cmos_changed();
        }
    }
}

/* RISC OS 3's checksum (in 3F) of the RAM's other bytes */
static uint8_t checksum(const uint8_t *c)
{
    unsigned sum = 1;
    for (int i = 0x10; i < 0x100; i++) {
        sum += i == 0x3F ? 0 : c[i];
    }
    return (uint8_t)sum;
}

void cmos_reset(void)
{
    memset(&i2c, 0, sizeof(i2c));
    i2c.scl = i2c.sda = i2c.slave_sda = true;
    if (arc.cfg.cmos_year >= 1900) {
        /* a RISC OS 3 checksum that was right stays right */
        uint8_t *c = arc.cfg.cmos;
        bool valid = c[0x3F] == checksum(c);
        c[0xC0] = (uint8_t)(arc.cfg.cmos_year % 100);
        c[0xC1] = (uint8_t)(arc.cfg.cmos_year / 100);
        if (valid) {
            c[0x3F] = checksum(c);
        }
    }
}

/* Each OS's own defaults (what R-power-on on Arthur, Delete-power-on on
 * RISC OS sets: the desktop at start, a TV's modes), with two floppy
 * drives (C7 bits 2-0) instead of one, and on the A3020 its IDE disc (C7
 * bits 7-6); the year is set at reset
 */
typedef struct {
    uint8_t at, value;
} cmos_byte_t;

static const cmos_byte_t s_arthur[] = {
    { 0x41, 0xFE }, { 0x43, 0xEB }, { 0x45, 0x08 }, { 0x4A, 0x10 }, { 0x4B, 0x60 }, { 0x4C, 0x20 },
    { 0x4D, 0x08 }, { 0x4E, 0x0A }, { 0x4F, 0x2C }, { 0x50, 0x80 }, { 0xC5, 0x01 }, { 0xC6, 0x06 },
    { 0xC7, 0x02 }, { 0xC8, 0xFF }, { 0xCA, 0x08 }, { 0xCB, 0xFF }, { 0xCC, 0xFF }, { 0xCD, 0xFF },
    { 0xD2, 0x02 }, { 0xD3, 0x01 }, { 0xD4, 0xF0 }, { 0xF9, 0x03 }, { 0xFA, 0x01 },
};

static const cmos_byte_t s_riscos2[] = {
    { 0x41, 0xFE }, { 0x43, 0xEB }, { 0x45, 0x08 }, { 0x4A, 0x10 }, { 0x4B, 0x60 }, { 0x4C, 0x20 },
    { 0x4D, 0x08 }, { 0x4E, 0x0A }, { 0x4F, 0x2C }, { 0x50, 0x80 }, { 0x51, 0x02 }, { 0xC4, 0x04 },
    { 0xC5, 0x01 }, { 0xC6, 0x06 }, { 0xC7, 0x02 }, { 0xC8, 0xFF }, { 0xC9, 0x01 }, { 0xCA, 0x08 },
    { 0xCB, 0xFF }, { 0xCC, 0xFF }, { 0xCD, 0xFF }, { 0xD2, 0x02 }, { 0xD3, 0x01 }, { 0xD4, 0xF0 },
    { 0xF9, 0x03 }, { 0xFA, 0x01 },
};

static const cmos_byte_t s_riscos3[] = {
    { 0x12, 0x03 }, { 0x13, 0x14 }, { 0x15, 0x6F }, { 0x16, 0x40 }, { 0x41, 0xFE }, { 0x43, 0xEB },
    { 0x45, 0x08 }, { 0x4A, 0x10 }, { 0x4B, 0x50 }, { 0x4C, 0x20 }, { 0x4D, 0x08 }, { 0x4E, 0x0A },
    { 0x4F, 0x2C }, { 0x50, 0x80 }, { 0x51, 0x02 }, { 0x5C, 0x01 }, { 0xC4, 0x04 }, { 0xC5, 0xFD },
    { 0xC6, 0x08 }, { 0xC7, 0x02 }, { 0xC8, 0xFF }, { 0xC9, 0x01 }, { 0xD4, 0xF0 }, { 0xF9, 0x0A },
    { 0xFA, 0x01 },
};

void arc_cmos_defaults(arc_model_t model, arc_os_t os, uint8_t *cmos)
{
    static const struct {
        const cmos_byte_t *bytes;
        size_t n;
    } tables[] = {
        { s_arthur, sizeof(s_arthur) / sizeof(s_arthur[0]) },
        { s_riscos2, sizeof(s_riscos2) / sizeof(s_riscos2[0]) },
        { s_riscos3, sizeof(s_riscos3) / sizeof(s_riscos3[0]) },
    };
    memset(cmos, 0, ARC_CMOS_SIZE);
    if ((unsigned)os > ARC_OS_RISCOS3) {
        return;
    }
    for (size_t i = 0; i < tables[os].n; i++) {
        cmos[tables[os].bytes[i].at] = tables[os].bytes[i].value;
    }
    if (model == ARC_MODEL_A3020) {
        cmos[0xC7] |= 0x40;             /* IDEDiscs 1 */
    }
    if (os == ARC_OS_RISCOS3) {
        cmos[0x3F] = checksum(cmos);
    }
}

bool cmos_sda(void)
{
    return i2c.slave_sda;
}

static void rising(bool sda)
{
    if (i2c.state == I2C_IDLE) {
        return;
    }
    if (i2c.acking) {
        if (i2c.state == I2C_READ && sda) {
            i2c.nacked = true;          /* the master wants no more */
        }
        return;
    }
    if (i2c.state != I2C_READ) {
        i2c.shift = (uint8_t)(i2c.shift << 1 | sda);
    }
    i2c.bit++;
}

static void falling(void)
{
    if (i2c.state == I2C_IDLE) {
        return;
    }
    if (i2c.acking) {
        /* the ninth clock is over */
        i2c.acking = false;
        i2c.slave_sda = true;
        i2c.bit = 0;
        i2c.shift = 0;
        if (i2c.state == I2C_READ) {
            if (i2c.nacked) {
                i2c.state = I2C_IDLE;
                return;
            }
            i2c.shift = read_register(i2c.pointer++);
            i2c.slave_sda = (i2c.shift >> 7) & 1;
        }
        return;
    }
    if (i2c.state == I2C_READ) {
        if (i2c.bit < 8) {
            i2c.slave_sda = (i2c.shift >> (7 - i2c.bit)) & 1;
        } else {
            i2c.slave_sda = true;       /* the master's acknowledge */
            i2c.acking = true;
        }
        return;
    }
    if (i2c.bit < 8) {
        return;
    }
    uint8_t b = i2c.shift;
    switch (i2c.state) {
    case I2C_ADDR:
        if ((b & 0xFE) != DEVICE) {
            i2c.state = I2C_IDLE;       /* not us */
            return;
        }
        i2c.reading = b & 1;
        i2c.state = i2c.reading ? I2C_READ : I2C_REG;
        break;
    case I2C_REG:
        i2c.pointer = b;
        i2c.state = I2C_WRITE;
        break;
    case I2C_WRITE:
        write_register(i2c.pointer++, b);
        break;
    default:
        break;
    }
    i2c.slave_sda = false;              /* acknowledge */
    i2c.acking = true;
}

void cmos_lines(bool scl, bool sda)
{
    bool line_before = i2c.sda && i2c.slave_sda;
    bool line = sda && i2c.slave_sda;
    if (i2c.scl && scl) {
        if (line_before && !line) {
            /* start (or a repeated one) */
            i2c.state = I2C_ADDR;
            i2c.bit = 0;
            i2c.shift = 0;
            i2c.acking = i2c.nacked = false;
            i2c.slave_sda = true;
        } else if (!line_before && line && !(i2c.state == I2C_READ && !i2c.slave_sda)) {
            i2c.state = I2C_IDLE;       /* stop */
            i2c.slave_sda = true;
        }
    } else if (!i2c.scl && scl) {
        rising(line);
    } else if (i2c.scl && !scl) {
        falling();
    }
    i2c.scl = scl;
    i2c.sda = sda;
}
