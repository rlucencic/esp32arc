/*
 * The WD1772 floppy controller (IOC bank 1, 0x3310000: status/command,
 * track, sector, data at +0, +4, +8, +C) with latch A (0x3350040: drive
 * select bits 3-0 low, side bit 4 (low: side 1), motor bit 5 (low: on))
 * and latch B (0x3350018: double density bit 1 low, controller reset bit 3
 * low). DRQ is FIQ bit 0, INTRQ FIQ bit 1: ADFS's FIQ handler moves each
 * byte. A disc that was changed shows in IRQ B bit 4 until the head steps;
 * the drive's ready line (a disc in it) is IOC C2.
 *
 * Bytes come every 32 us, as a double density drive's; seeks and finding a
 * sector take a few milliseconds (faster than the real drive).
 *
 * The A440's (A305, A310): the discs are floppy.c's images.
 */
#include <string.h>
#include "arc_internal.h"

#define BYTE_TICKS      US(32)
#define STEP_TICKS      US(2000)
#define SETTLE_TICKS    US(3000)
#define SEARCH_TICKS    US(2000)
#define SPINUP_TICKS    US(50000)       /* the 1772 waits 6 revolutions; less here */
#define NOT_FOUND_TICKS US(50000)       /* an empty drive: 5 revolutions on the chip */
#define MOTOR_OFF_TICKS US(1800000)     /* 9 revolutions idle */
#define REV_TICKS       US(200000)      /* 300 rpm */
#define INDEX_TICKS     US(4000)
#define TRACK_BYTES     6250

typedef enum {
    ST_IDLE, ST_STEP, ST_SEARCH, ST_READ, ST_WRITE, ST_WRITE_TRACK,
} state_t;

static struct {
    uint8_t latch_a, latch_b;
    uint8_t status, track, sector, data, command;
    bool drq, intrq;
    bool motor;
    uint64_t motor_off;                 /* tick the motor stops, NEVER */
    state_t state;
    uint64_t event;
    int target, direction;
    uint8_t *buf;
    int pos, len;
    uint32_t track_pos;
} fdc;

static floppy_t *selected(int *side)
{
    uint8_t a = fdc.latch_a;
    int d = !(a & 1) ? 0 : !(a & 2) ? 1 : -1;
    if (side) {
        *side = (a & 0x10) ? 0 : 1;
    }
    return d >= 0 && d < ARC_FLOPPIES ? &floppies[d] : NULL;
}


/* ---- The lines ---- */

static void update_changed(void)
{
    floppy_t *d = selected(NULL);
    ioc_irqb(IRQB_DISCCHG, d && d->changed);
}

static void set_intrq(bool on)
{
    fdc.intrq = on;
    ioc_fiq(FIQ_FIRQ, on);
}

static void set_drq(bool on)
{
    fdc.drq = on;
    fdc.status = on ? fdc.status | 0x02 : fdc.status & ~0x02;
    ioc_fiq(FIQ_FDRQ, on);
}

static void schedule(uint64_t delay)
{
    fdc.event = arc_now() + delay;
    arc_schedule(fdc.event);
}

static void motor_on(void)
{
    fdc.motor = true;
    fdc.motor_off = NEVER;
}

static void finish(uint8_t extra)
{
    fdc.status = (uint8_t)((fdc.status & ~0x03) | extra);
    fdc.state = ST_IDLE;
    fdc.event = NEVER;
    set_drq(false);
    set_intrq(true);
    floppy_light = -1;
    fdc.motor_off = arc_now() + MOTOR_OFF_TICKS;
    arc_schedule(fdc.motor_off);
}

bool fdc_ready(void)
{
    floppy_t *d = selected(NULL);
    return d && d->data && !(fdc.latch_a & 0x20);
}

/* ---- Commands ---- */

static bool has_disc(void)
{
    floppy_t *d = selected(NULL);
    return d && d->data;
}

static uint8_t type1_status(void)
{
    floppy_t *d = selected(NULL);
    uint8_t s = 0x20;                   /* spun up */
    if (fdc.motor) {
        s |= 0x80;
    }
    if (d && d->read_only) {
        s |= 0x40;
    }
    if (d && d->head == 0) {
        s |= 0x04;
    }
    if (has_disc() && fdc.motor && (arc_now() % REV_TICKS) < INDEX_TICKS) {
        s |= 0x02;
    }
    return s;
}

static void step_head(floppy_t *d, int by)
{
    if (!d) {
        return;
    }
    d->head += by;
    if (d->head < 0) d->head = 0;
    if (d->head > FLOPPY_MAX_TRACKS - 1) d->head = FLOPPY_MAX_TRACKS - 1;
    if (by && d->data) {
        d->changed = false;             /* a step with a disc in clears it */
        update_changed();
    }
}

static uint64_t spin_up(uint8_t cmd)
{
    bool was = fdc.motor;
    motor_on();
    return (!was && !(cmd & 0x08)) ? SPINUP_TICKS : 0;
}

static void start_type1(uint8_t cmd)
{
    floppy_t *d = selected(NULL);
    int steps = 0;
    switch (cmd >> 4) {
    case 0x0:                           /* restore */
        steps = d ? d->head : 0;
        step_head(d, -steps);
        fdc.track = 0;
        break;
    case 0x1:                           /* seek */
        steps = (int)fdc.data - (int)fdc.track;
        fdc.direction = steps > 0 ? 1 : -1;
        step_head(d, steps);
        steps = steps < 0 ? -steps : steps;
        fdc.track = fdc.data;
        break;
    default:
        if ((cmd >> 4) >= 4) {
            fdc.direction = (cmd & 0x20) ? -1 : 1;  /* 4x/5x in, 6x/7x out */
        }
        steps = 1;
        step_head(d, fdc.direction);
        if (cmd & 0x10) {
            fdc.track = (uint8_t)(fdc.track + fdc.direction);
        }
        break;
    }
    fdc.status = 0x01;
    fdc.state = ST_STEP;
    schedule(spin_up(cmd) + (uint64_t)steps * STEP_TICKS + ((cmd & 0x04) ? SETTLE_TICKS : 0) + US(100));
}

static void start_type2(uint8_t cmd)
{
    floppy_t *d = selected(NULL);
    fdc.status = 0x01;
    uint64_t delay = spin_up(cmd) + ((cmd & 0x04) ? SETTLE_TICKS : 0);
    if (!has_disc()) {
        fdc.state = ST_SEARCH;          /* searches, and finds nothing */
        schedule(delay + NOT_FOUND_TICKS);
        return;
    }
    if ((cmd & 0xE0) == 0xA0 && d->read_only) {
        finish(0x40);
        return;
    }
    if ((cmd & 0xF0) == 0xF0 && d->read_only) {
        finish(0x40);
        return;
    }
    fdc.state = ST_SEARCH;
    floppy_light = (int)(d - floppies);
    schedule(delay + SEARCH_TICKS);
}

static void begin_sector(void)
{
    int side;
    floppy_t *d = selected(&side);
    uint8_t cmd = fdc.command;
    if (!d || !d->data) {
        finish(0x10);                   /* record not found */
        return;
    }
    if ((cmd & 0xF0) == 0xC0) {
        /* read address: the next sector's ID */
        static int next;
        next = (next + 1) % (d->spt ? d->spt : 1);
        fdc.buf[0] = (uint8_t)d->head;
        fdc.buf[1] = (uint8_t)side;
        fdc.buf[2] = (uint8_t)(d->first + next);
        fdc.buf[3] = d->ssize == 128 ? 0 : d->ssize == 256 ? 1 : d->ssize == 512 ? 2 : 3;
        fdc.buf[4] = fdc.buf[5] = 0;
        fdc.sector = (uint8_t)d->head;  /* the track goes to the sector register */
        fdc.pos = 0;
        fdc.len = 6;
        fdc.state = ST_READ;
        schedule(BYTE_TICKS);
        return;
    }
    if ((cmd & 0xF0) == 0xE0) {
        /* read track: the sectors' data without gaps */
        int n = 0;
        for (int s = 0; s < d->spt && n + d->ssize <= TRACK_BYTES; s++) {
            long off = floppy_offset(d, d->head, side, d->first + s);
            if (off >= 0) {
                memcpy(fdc.buf + n, d->data + off, (size_t)d->ssize);
                n += d->ssize;
            }
        }
        fdc.pos = 0;
        fdc.len = n;
        fdc.state = ST_READ;
        schedule(BYTE_TICKS);
        return;
    }
    if ((cmd & 0xF0) == 0xF0) {
        fdc.track_pos = 0;
        fdc.pos = 0;
        fdc.state = ST_WRITE_TRACK;
        set_drq(true);
        schedule(BYTE_TICKS * 3);
        return;
    }
    long off = fdc.track == d->head ? floppy_offset(d, d->head, side, fdc.sector) : -1;
    if (off < 0) {
        finish(0x10);
        return;
    }
    fdc.pos = 0;
    fdc.len = d->ssize;
    if ((cmd & 0xE0) == 0x80) {
        memcpy(fdc.buf, d->data + off, (size_t)d->ssize);
        d->reads++;
        fdc.state = ST_READ;
        schedule(BYTE_TICKS);
    } else {
        fdc.state = ST_WRITE;
        set_drq(true);
        schedule(BYTE_TICKS * 3);
    }
}

/* Write track: the sector IDs (after FE: track, side, sector, size) and the
 * data after each FB go into the image
 */
static void write_track_end(void)
{
    int side;
    floppy_t *d = selected(&side);
    if (!d || !d->data) {
        return;
    }
    const uint8_t *b = fdc.buf;
    int id_sector = -1;
    for (int i = 1; i < fdc.pos; i++) {
        if (b[i] == 0xFE && i + 4 < fdc.pos && (b[i - 1] == 0xF5 || b[i - 1] == 0x00)) {
            id_sector = b[i + 3];
            i += 4;
        } else if (b[i] == 0xFB && id_sector >= 0 && i + d->ssize < fdc.pos) {
            long off = floppy_offset(d, d->head, side, id_sector);
            if (off >= 0) {
                memcpy(d->data + off, b + i + 1, (size_t)d->ssize);
                floppy_mark_dirty(d, d->head, side);
            }
            i += d->ssize;
            id_sector = -1;
        }
    }
    d->writes++;
}

static void state_event(void)
{
    fdc.event = NEVER;
    floppy_t *d;
    switch (fdc.state) {
    case ST_STEP: {
        uint8_t s = type1_status();
        if ((fdc.command & 0x04) && !has_disc()) {
            s |= 0x10;                  /* verify: no ID found */
        }
        fdc.state = ST_IDLE;
        fdc.status = s;
        set_intrq(true);
        fdc.motor_off = arc_now() + MOTOR_OFF_TICKS;
        arc_schedule(fdc.motor_off);
        break;
    }
    case ST_SEARCH:
        begin_sector();
        break;
    case ST_READ:
        if (fdc.drq) {
            fdc.status |= 0x04;         /* not taken in time: lost data */
        }
        if (fdc.pos < fdc.len) {
            fdc.data = fdc.buf[fdc.pos++];
            set_drq(true);
            schedule(BYTE_TICKS);
            break;
        }
        if ((fdc.command & 0xF0) == 0x90) {
            fdc.sector++;               /* multiple: the next sector */
            begin_sector();
            break;
        }
        finish(fdc.status & 0x04);
        break;
    case ST_WRITE:
        if (fdc.drq) {
            fdc.status |= 0x04;
            fdc.data = 0;
        }
        fdc.buf[fdc.pos++] = fdc.data;
        if (fdc.pos < fdc.len) {
            set_drq(true);
            schedule(BYTE_TICKS);
            break;
        }
        {
            int side;
            d = selected(&side);
            long off = d ? floppy_offset(d, d->head, side, fdc.sector) : -1;
            if (off >= 0) {
                memcpy(d->data + off, fdc.buf, (size_t)d->ssize);
                floppy_mark_dirty(d, d->head, side);
                d->writes++;
            }
        }
        if ((fdc.command & 0xF0) == 0xB0) {
            fdc.sector++;
            begin_sector();
            break;
        }
        finish(fdc.status & 0x04);
        break;
    case ST_WRITE_TRACK:
        if (fdc.drq) {
            fdc.status |= 0x04;
            fdc.data = 0;
        }
        if (fdc.pos < TRACK_BYTES) {
            fdc.buf[fdc.pos++] = fdc.data;
        }
        if (++fdc.track_pos >= TRACK_BYTES) {
            write_track_end();
            finish(fdc.status & 0x04);
            break;
        }
        set_drq(true);
        schedule(BYTE_TICKS);
        break;
    default:
        break;
    }
}

bool arc_fdc_log;

static void command(uint8_t cmd)
{
    if (arc_fdc_log) {
        int side;
        floppy_t *d = selected(&side);
        ARC_LOG("FDC %02X at %u ms: track %d sector %d data %d, drive %d side %d head %d, latch A %02X B %02X",
                cmd, (unsigned)(arc_now() / 8000), fdc.track, fdc.sector, fdc.data,
                d ? (int)(d - floppies) : -1, side, d ? d->head : -1, fdc.latch_a, fdc.latch_b);
    }
    if ((cmd & 0xF0) == 0xD0) {
        /* force interrupt: stop; with bit 3 an interrupt now */
        bool busy = fdc.state != ST_IDLE;
        fdc.state = ST_IDLE;
        fdc.event = NEVER;
        set_drq(false);
        fdc.status = busy ? (uint8_t)(fdc.status & ~0x01) : type1_status();
        floppy_light = -1;
        if (cmd & 0x08) {
            set_intrq(true);
        }
        return;
    }
    if (fdc.state != ST_IDLE) {
        return;                         /* busy: ignored */
    }
    fdc.command = cmd;
    set_intrq(false);
    set_drq(false);
    if (!(cmd & 0x80)) {
        start_type1(cmd);
    } else {
        start_type2(cmd);
    }
}

void fdc_reset(void)
{
    if (!fdc.buf) {
        fdc.buf = arc_calloc(1, TRACK_BYTES);
    }
    fdc.latch_a = 0xFF;
    fdc.latch_b = 0xFF;
    fdc.status = 0;
    fdc.track = fdc.sector = fdc.data = 0;
    fdc.state = ST_IDLE;
    fdc.event = NEVER;
    fdc.motor = false;
    fdc.motor_off = NEVER;
    floppy_light = -1;
    set_drq(false);
    set_intrq(false);
    for (int i = 0; i < ARC_FLOPPIES; i++) {
        floppies[i].changed = true;
    }
}

uint8_t fdc_read(int reg)
{
    switch (reg & 3) {
    case 0: {
        uint8_t s;
        if (fdc.state == ST_IDLE && !(fdc.command & 0x80)) {
            s = (uint8_t)(type1_status() | (fdc.status & 0x18));
        } else {
            s = fdc.state == ST_IDLE ? fdc.status : fdc.status | 0x01;
            s = fdc.motor ? s | 0x80 : s & ~0x80;
        }
        if (fdc.intrq) {
            set_intrq(false);
        }
        return s;
    }
    case 1: return fdc.track;
    case 2: return fdc.sector;
    default:
        if (fdc.drq) {
            set_drq(false);
        }
        return fdc.data;
    }
}

void fdc_write(int reg, uint8_t v)
{
    switch (reg & 3) {
    case 0: command(v); break;
    case 1: fdc.track = v; break;
    case 2: fdc.sector = v; break;
    default:
        fdc.data = v;
        if (fdc.drq) {
            set_drq(false);
        }
        break;
    }
}

void fdc_latch_a(uint8_t v)
{
    fdc.latch_a = v;
    update_changed();
}

void fdc_latch_b(uint8_t v)
{
    bool reset = !(v & 0x08) && (fdc.latch_b & 0x08);
    fdc.latch_b = v;
    if (reset) {
        fdc.state = ST_IDLE;
        fdc.event = NEVER;
        fdc.status = 0;
        fdc.command = 0;
        set_drq(false);
        set_intrq(false);
    }
}

uint64_t fdc_next_event(void)
{
    return fdc.event < fdc.motor_off ? fdc.event : fdc.motor_off;
}

void fdc_event(void)
{
    uint64_t now = arc_now();
    if (fdc.motor_off <= now) {
        fdc.motor_off = NEVER;
        if (fdc.state == ST_IDLE) {
            fdc.motor = false;
        }
    }
    if (fdc.event <= now) {
        state_event();
    }
}
