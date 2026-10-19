/*
 * The 82C711 of the A3020 and A4000 (a PC's "super I/O" chip): its PC I/O
 * ports appear at 0x3010000 + port * 4, 16 bits wide (IOC banks 1 and 2,
 * any cycle speed; read on data bits 0-15, written from 16-31, as the
 * Archimedes' I/O is), and its floppy controller's DMA acknowledges at
 * 0x3012000-0x3029FFF (0x302A000-0x302BFFF: with the terminal count).
 *
 *   3F0, 3F1  configuration (55 55 to 3F0 enters, AA leaves; index, data)
 *   3F2       floppy: DOR (drive 1-0, /reset 2, motors 7-4)
 *   3F4       MSR (read), data rate select (write: rate 1-0, reset 7)
 *   3F5       the FIFO: commands in, results out
 *   3F7       DIR (read: disc changed 7), CCR (write: rate 1-0)
 *   1F0-1F7, 3F6  IDE (ide.c)
 *   278-27A   parallel port, 3F8-3FF serial port: nothing attached
 *
 * The floppy controller speaks the 765's commands (and the 82077's
 * version, configure, lock, dumpreg) as ADFS uses them. Its DMA request is
 * FIQ 0: ADFS's FIQ handler reads or writes the DMA acknowledge, the last
 * byte at the terminal count address. Its interrupt is IRQ B 4, the index
 * pulse IRQ A 2. A high density disc reads at 500 kbps (16 us a byte), a
 * double density one at 250 or 300 kbps (32 us); at another rate no sector
 * is found, which is how ADFS tells the formats apart. The discs are
 * floppy.c's images.
 */
#include <string.h>
#include "arc_internal.h"

#define SEARCH_TICKS    US(2000)        /* finding a sector */
#define STEP_TICKS      US(1000)        /* the head a track */
#define SETTLE_TICKS    US(2000)
#define NOT_FOUND_TICKS US(50000)       /* no address mark (two revolutions on the chip) */
#define REV_TICKS       US(200000)      /* 300 rpm */

typedef enum { PH_IDLE, PH_COMMAND, PH_EXEC, PH_RESULT } phase_t;
typedef enum { X_NONE, X_SEARCH, X_READ, X_WRITE, X_FORMAT, X_READ_ID, X_FAIL } exec_t;

static struct {
    /* the chip's configuration */
    int cfg_mode;                       /* 55s written; 2: configuring */
    uint8_t cfg_index, cfg[16];

    /* the floppy controller */
    uint8_t dor, rate;
    phase_t phase;
    uint8_t cmd[9];
    int cmd_len, cmd_pos;
    uint8_t res[10];
    int res_len, res_pos;
    bool res_clears_irq;                /* reading the first result byte clears the interrupt */
    bool irq;
    int reset_sense;                    /* sense interrupt results still due after a reset */
    bool sense_due[4];                  /* a finished seek's */
    uint8_t sense_st0[4];
    uint8_t pcn[4];                     /* each drive's cylinder */
    uint8_t busy;                       /* drives seeking (MSR 3-0) */
    uint64_t seek_done[4];
    uint8_t specify[2], config[3], perp;
    bool lock;

    /* the command running */
    exec_t exec;
    uint64_t event;
    int drive, head;
    uint8_t c, h, r, n, eot;
    bool mt;
    uint8_t st1, st2;
    uint8_t buf[1024];
    int pos, len;
    bool drq, tc;
    uint8_t data;                       /* the byte on the DMA */
    int format_left, format_pos;
    uint8_t format_id[4], format_fill;
    uint64_t format_step;
    int next_id;
    uint64_t index_next;

    /* the serial and parallel ports */
    uint8_t ser_lcr, ser_ier, ser_mcr, ser_scr;
    uint8_t lpt_data, lpt_ctrl;
} c711;

/* ---- Lines and drives ---- */

static void update_lines(void)
{
    ioc_irqb(IRQB_DISCCHG, c711.irq);
    ioc_fiq(FIQ_FDRQ, c711.drq);
}

static void set_drq(bool on)
{
    c711.drq = on;
    ioc_fiq(FIQ_FDRQ, on);
}

static floppy_t *drive(int n)
{
    return n >= 0 && n < ARC_FLOPPIES ? &floppies[n] : NULL;
}

static bool motor(int n)
{
    return (c711.dor >> (4 + n)) & 1;
}

static uint64_t byte_ticks(const floppy_t *d)
{
    return d->hd ? US(16) : US(32);
}

/* The disc turns, and the controller's rate reads it */
static bool readable(int n)
{
    floppy_t *d = drive(n);
    if (!d || !d->data || !motor(n)) {
        return false;
    }
    return d->hd ? c711.rate == 0 : (c711.rate == 1 || c711.rate == 2);
}

static uint8_t size_code(int bytes)
{
    return bytes == 128 ? 0 : bytes == 256 ? 1 : bytes == 512 ? 2 : 3;
}

static void schedule(uint64_t at)
{
    c711.event = at;
    arc_schedule(at);
}

/* ---- Results ---- */

static void result(int n, bool clears_irq)
{
    c711.phase = n ? PH_RESULT : PH_IDLE;
    c711.res_len = n;
    c711.res_pos = 0;
    c711.res_clears_irq = clears_irq;
}

/* The end of a read, write, format or read ID: seven result bytes */
static void finish(uint8_t st0, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    c711.exec = X_NONE;
    c711.event = NEVER;
    set_drq(false);
    floppy_light = -1;
    c711.res[0] = st0 | (uint8_t)(c711.head << 2) | (uint8_t)c711.drive;
    c711.res[1] = c711.st1;
    c711.res[2] = c711.st2;
    c711.res[3] = c;
    c711.res[4] = h;
    c711.res[5] = r;
    c711.res[6] = n;
    result(7, true);
    c711.irq = true;
    update_lines();
}

/* Abnormal termination, after `delay` (st1, st2 the reasons) */
static void fail(uint8_t st1, uint8_t st2, uint64_t delay)
{
    c711.st1 = st1;
    c711.st2 = st2;
    c711.exec = X_FAIL;
    c711.phase = PH_EXEC;
    schedule(arc_now() + delay);
}

/* ---- Commands ---- */

static int params_for(uint8_t op)
{
    switch (op & 0x1F) {
    case 0x03: return 2;                /* specify */
    case 0x04: return 1;                /* sense drive status */
    case 0x05: case 0x06: return 8;     /* write, read */
    case 0x07: return 1;                /* recalibrate */
    case 0x0A: return 1;                /* read ID */
    case 0x0D: return 5;                /* format track */
    case 0x0F: return 2;                /* seek */
    case 0x12: return 1;                /* perpendicular mode */
    case 0x13: return 3;                /* configure */
    default: return 0;
    }
}

static void seek(int n, int head, int target)
{
    floppy_t *d = drive(n);
    int from = c711.pcn[n];
    c711.pcn[n] = (uint8_t)target;
    c711.busy |= (uint8_t)(1u << n);
    int steps = from > target ? from - target : target - from;
    c711.seek_done[n] = arc_now() + (uint64_t)steps * STEP_TICKS + SETTLE_TICKS;
    arc_schedule(c711.seek_done[n]);
    c711.sense_st0[n] = (uint8_t)(0x20 | (head << 2) | n);     /* seek end */
    if (!d) {
        c711.sense_st0[n] |= 0x50;      /* no such drive: abnormal, equipment check */
    } else {
        d->head = target;
        if (steps && d->data) {
            d->changed = false;         /* a step with a disc in clears the change */
        }
    }
    c711.phase = PH_IDLE;
}

static void begin_sector(void)
{
    floppy_t *d = drive(c711.drive);
    if (!readable(c711.drive)) {
        fail(0x01, 0, NOT_FOUND_TICKS);
        return;
    }
    /* the sector headers on this track say: the head's cylinder, side, ID, size */
    long off = floppy_offset(d, d->head, c711.head, c711.r);
    if (c711.c != d->head || c711.n != size_code(d->ssize) || off < 0) {
        fail(0x04, c711.c != d->head ? 0x10 : 0, SEARCH_TICKS);
        return;
    }
    c711.pos = 0;
    c711.len = d->ssize;
    if (c711.exec == X_READ) {
        memcpy(c711.buf, d->data + off, (size_t)d->ssize);
        schedule(arc_now() + byte_ticks(d));
    } else {
        set_drq(true);                  /* the first byte, please */
        schedule(arc_now() + byte_ticks(d));
    }
}

/* After a sector: the next one, or the end (on the terminal count, or at
 * the track's last sector)
 */
static void next_sector(void)
{
    uint8_t c = c711.c, h = c711.h, r = (uint8_t)(c711.r + 1);
    bool end = c711.tc;
    if (c711.r == c711.eot) {
        floppy_t *d = drive(c711.drive);
        r = (uint8_t)d->first;
        if (c711.mt && !c711.head) {
            c711.head = 1;
            h = 1;
        } else {
            c++;
            end = true;
        }
    }
    if (end) {
        finish(0, c, h, r, c711.n);
        return;
    }
    c711.c = c;
    c711.h = h;
    c711.r = r;
    c711.event = arc_now() + US(200);   /* the gap to the next sector */
    arc_schedule(c711.event);
    c711.exec = c711.exec == X_READ ? X_READ : X_WRITE;
    c711.pos = -1;                      /* begin_sector() at the event */
}

static void start_transfer(bool write)
{
    c711.drive = c711.cmd[1] & 3;
    c711.head = (c711.cmd[1] >> 2) & 1;
    c711.c = c711.cmd[2];
    c711.h = c711.cmd[3];
    c711.r = c711.cmd[4];
    c711.n = c711.cmd[5];
    c711.eot = c711.cmd[6];
    c711.mt = c711.cmd[0] & 0x80;
    c711.st1 = c711.st2 = 0;
    c711.tc = false;
    c711.phase = PH_EXEC;
    floppy_t *d = drive(c711.drive);
    floppy_light = c711.drive;
    if (write && d && d->data && d->read_only) {
        fail(0x02, 0, US(100));         /* not writable */
        return;
    }
    c711.exec = write ? X_WRITE : X_READ;
    c711.pos = -1;
    schedule(arc_now() + SEARCH_TICKS);
}

static void execute(void)
{
    uint8_t op = c711.cmd[0];
    int n = c711.cmd[1] & 3;
    int head = (c711.cmd[1] >> 2) & 1;
    floppy_t *d;
    switch (op & 0x1F) {
    case 0x03:                          /* specify */
        c711.specify[0] = c711.cmd[1];
        c711.specify[1] = c711.cmd[2];
        result(0, false);
        break;
    case 0x04:                          /* sense drive status: ST3 */
        d = drive(n);
        c711.res[0] = (uint8_t)(0x28 | (head << 2) | n | (c711.pcn[n] == 0 ? 0x10 : 0) |
                                (d && d->read_only ? 0x40 : 0));
        result(1, false);
        break;
    case 0x05:
        start_transfer(true);
        break;
    case 0x06:
        start_transfer(false);
        break;
    case 0x07:                          /* recalibrate */
        seek(n, head, 0);
        break;
    case 0x08:                          /* sense interrupt status */
        if (c711.reset_sense > 0) {
            c711.res[0] = (uint8_t)(0xC0 | (4 - c711.reset_sense));
            c711.res[1] = 0;
            c711.reset_sense--;
            result(2, false);
        } else {
            int found = -1;
            for (int i = 0; i < 4 && found < 0; i++) {
                if (c711.sense_due[i]) {
                    found = i;
                }
            }
            if (found < 0) {
                c711.res[0] = 0x80;     /* nothing to tell: invalid */
                result(1, false);
            } else {
                c711.sense_due[found] = false;
                c711.res[0] = c711.sense_st0[found];
                c711.res[1] = c711.pcn[found];
                result(2, false);
            }
        }
        c711.irq = c711.reset_sense > 0;
        for (int i = 0; i < 4; i++) {
            c711.irq = c711.irq || c711.sense_due[i];
        }
        update_lines();
        break;
    case 0x0A:                          /* read ID */
        c711.drive = n;
        c711.head = head;
        c711.st1 = c711.st2 = 0;
        c711.phase = PH_EXEC;
        c711.exec = X_READ_ID;
        floppy_light = n;
        schedule(arc_now() + SEARCH_TICKS);
        break;
    case 0x0D:                          /* format track */
        c711.drive = n;
        c711.head = head;
        c711.st1 = c711.st2 = 0;
        c711.n = c711.cmd[2];
        c711.format_left = c711.cmd[3] ? c711.cmd[3] : 1;
        c711.format_fill = c711.cmd[5];
        c711.format_pos = 0;
        c711.phase = PH_EXEC;
        floppy_light = n;
        d = drive(n);
        if (!readable(n)) {
            fail(0x01, 0, NOT_FOUND_TICKS);
        } else if (d->read_only) {
            fail(0x02, 0, US(100));
        } else {
            /* the IDs come over a revolution, four bytes a sector */
            c711.exec = X_FORMAT;
            c711.format_step = REV_TICKS / ((uint64_t)c711.format_left * 4);
            set_drq(true);
            schedule(arc_now() + c711.format_step);
        }
        break;
    case 0x0E:                          /* dumpreg */
        c711.res[0] = c711.pcn[0];
        c711.res[1] = c711.pcn[1];
        c711.res[2] = c711.pcn[2];
        c711.res[3] = c711.pcn[3];
        c711.res[4] = c711.specify[0];
        c711.res[5] = c711.specify[1];
        c711.res[6] = c711.eot;
        c711.res[7] = (uint8_t)((c711.lock ? 0x80 : 0) | (c711.perp & 0x7F));
        c711.res[8] = c711.config[1];
        c711.res[9] = c711.config[2];
        result(10, false);
        break;
    case 0x0F:                          /* seek */
        seek(n, head, c711.cmd[2]);
        break;
    case 0x10:                          /* version: an 82077 */
        c711.res[0] = 0x90;
        result(1, false);
        break;
    case 0x12:                          /* perpendicular mode */
        c711.perp = c711.cmd[1];
        result(0, false);
        break;
    case 0x13:                          /* configure */
        memcpy(c711.config, &c711.cmd[1], 3);
        result(0, false);
        break;
    case 0x14:                          /* lock, unlock */
        c711.lock = op & 0x80;
        c711.res[0] = c711.lock ? 0x10 : 0;
        result(1, false);
        break;
    default:
        c711.res[0] = 0x80;             /* invalid */
        result(1, false);
        break;
    }
}

/* ---- The command running, as time goes ---- */

static void exec_event(void)
{
    floppy_t *d = drive(c711.drive);
    c711.event = NEVER;
    switch (c711.exec) {
    case X_FAIL:
        finish(0x40, c711.c, c711.h, c711.r, c711.n);
        break;
    case X_READ_ID:
        if (!readable(c711.drive) || c711.head >= d->sides) {
            c711.st1 = 0x01;            /* no address mark */
            finish(0x40, 0, 0, 0, 0);
            break;
        }
        c711.next_id = (c711.next_id + 1) % d->spt;
        finish(0, (uint8_t)d->head, (uint8_t)c711.head, (uint8_t)(d->first + c711.next_id),
               size_code(d->ssize));
        break;
    case X_READ:
        if (c711.pos < 0) {
            begin_sector();
            break;
        }
        if (c711.drq) {
            c711.st1 = 0x10;            /* overrun: the byte wasn't taken */
            finish(0x40, c711.c, c711.h, c711.r, c711.n);
            break;
        }
        if (c711.pos < c711.len && !c711.tc) {
            c711.data = c711.buf[c711.pos++];
            set_drq(true);
            schedule(arc_now() + byte_ticks(d));
            break;
        }
        d->reads++;
        next_sector();
        break;
    case X_WRITE:
        if (c711.pos < 0) {
            begin_sector();
            break;
        }
        if (c711.drq) {
            c711.st1 = 0x10;            /* overrun: no byte came */
            finish(0x40, c711.c, c711.h, c711.r, c711.n);
            break;
        }
        c711.buf[c711.pos++] = c711.data;
        if (c711.pos < c711.len && !c711.tc) {
            set_drq(true);
            schedule(arc_now() + byte_ticks(d));
            break;
        }
        {
            long off = floppy_offset(d, d->head, c711.head, c711.r);
            if (off >= 0) {
                memcpy(d->data + off, c711.buf, (size_t)d->ssize);
                floppy_mark_dirty(d, d->head, c711.head);
                d->writes++;
            }
        }
        next_sector();
        break;
    case X_FORMAT:
        if (c711.drq) {
            c711.st1 = 0x10;
            finish(0x40, 0, 0, 0, 0);
            break;
        }
        c711.format_id[c711.format_pos++] = c711.data;
        if (c711.format_pos == 4) {
            /* a sector's ID: its data the filler byte, where the image has it */
            c711.format_pos = 0;
            long off = floppy_offset(d, d->head, c711.head, c711.format_id[2]);
            if (off >= 0 && c711.format_id[3] == size_code(d->ssize)) {
                memset(d->data + off, c711.format_fill, (size_t)d->ssize);
                floppy_mark_dirty(d, d->head, c711.head);
            }
            if (--c711.format_left == 0) {
                d->writes++;
                finish(0, c711.format_id[0], c711.format_id[1], (uint8_t)(c711.format_id[2] + 1),
                       c711.format_id[3]);
                break;
            }
        }
        set_drq(true);
        schedule(arc_now() + c711.format_step);
        break;
    default:
        break;
    }
}

/* ---- The floppy controller's registers ---- */

static void fd_reset(void)
{
    c711.phase = PH_IDLE;
    c711.exec = X_NONE;
    c711.event = NEVER;
    c711.busy = 0;
    c711.reset_sense = 0;
    memset(c711.sense_due, 0, sizeof(c711.sense_due));
    c711.irq = false;
    set_drq(false);
    floppy_light = -1;
    update_lines();
}

/* Out of reset: the four drives' "ready changed" interrupts */
static void fd_reset_done(void)
{
    c711.reset_sense = 4;
    c711.irq = true;
    update_lines();
}

static uint8_t fd_read(int reg)
{
    switch (reg) {
    case 1:
        return 0x50;
    case 3:
        return 0x20;
    case 4: {                           /* MSR */
        static const uint8_t base[4] = { 0x80, 0x90, 0x10, 0xD0 };
        return (uint8_t)(base[c711.phase] | c711.busy);
    }
    case 5: {                           /* results */
        if (c711.phase != PH_RESULT) {
            return 0xFF;
        }
        uint8_t v = c711.res[c711.res_pos++];
        if (c711.res_pos == 1 && c711.res_clears_irq) {
            c711.irq = false;
            update_lines();
        }
        if (c711.res_pos >= c711.res_len) {
            c711.phase = PH_IDLE;
        }
        return v;
    }
    case 7: {                           /* DIR: the selected drive's disc change */
        int n = c711.dor & 3;
        floppy_t *d = drive(n);
        return motor(n) && (!d || !d->data || d->changed) ? 0x80 : 0;
    }
    default:
        return 0xFF;
    }
}

static void fd_write(int reg, uint8_t v)
{
    switch (reg) {
    case 2: {                           /* DOR */
        bool was_reset = !(c711.dor & 4);
        c711.dor = v;
        if (!(v & 4)) {
            fd_reset();
        } else if (was_reset) {
            fd_reset_done();
        }
        if ((v & 0xF0) && c711.index_next == NEVER) {
            c711.index_next = arc_now() + REV_TICKS;
            arc_schedule(c711.index_next);
        }
        break;
    }
    case 4:                             /* data rate select */
        c711.rate = v & 3;
        if (v & 0x80) {
            fd_reset();
            fd_reset_done();
        }
        break;
    case 5:                             /* commands */
        if (c711.phase == PH_IDLE) {
            c711.cmd[0] = v;
            c711.cmd_pos = 1;
            c711.cmd_len = 1 + params_for(v);
            c711.phase = PH_COMMAND;
        } else if (c711.phase == PH_COMMAND) {
            c711.cmd[c711.cmd_pos++] = v;
        } else {
            break;
        }
        if (c711.cmd_pos >= c711.cmd_len) {
            if (arc_fdc_log) {
                ARC_LOG("82C711 FDC %02X %02X %02X %02X %02X %02X %02X %02X %02X (%d) at %u ms, rate %d",
                        c711.cmd[0], c711.cmd[1], c711.cmd[2], c711.cmd[3], c711.cmd[4], c711.cmd[5],
                        c711.cmd[6], c711.cmd[7], c711.cmd[8], c711.cmd_len,
                        (unsigned)(arc_now() / (arc.tick_mhz * 1000u)), c711.rate);
            }
            execute();
        }
        break;
    case 7:                             /* CCR */
        c711.rate = v & 3;
        break;
    default:
        break;
    }
}

/* ---- The DMA acknowledges ---- */

static uint8_t dack_read(bool tc)
{
    if (tc) {
        c711.tc = true;
    }
    set_drq(false);
    return c711.data;
}

static void dack_write(uint8_t v, bool tc)
{
    if (tc) {
        c711.tc = true;
    }
    c711.data = v;
    set_drq(false);
}

/* ---- The serial and parallel ports: nothing attached ---- */

static uint8_t serial_read(int reg)
{
    switch (reg) {
    case 1: return (c711.ser_lcr & 0x80) ? 0 : c711.ser_ier;
    case 2: return 0x01;                /* no interrupt */
    case 3: return c711.ser_lcr;
    case 4: return c711.ser_mcr;
    case 5: return 0x60;                /* transmitter empty */
    case 7: return c711.ser_scr;
    default: return 0;
    }
}

static void serial_write(int reg, uint8_t v)
{
    switch (reg) {
    case 1: if (!(c711.ser_lcr & 0x80)) c711.ser_ier = v; break;
    case 3: c711.ser_lcr = v; break;
    case 4: c711.ser_mcr = v; break;
    case 7: c711.ser_scr = v; break;
    default: break;
    }
}

/* ---- The chip ---- */

void c711_reset(void)
{
    memset(&c711, 0, sizeof(c711));
    static const uint8_t defaults[16] = { 0x3F, 0x9F, 0xDC };
    memcpy(c711.cfg, defaults, sizeof(c711.cfg));
    c711.event = NEVER;
    c711.index_next = NEVER;
    c711.rate = 2;
    c711.ser_scr = 0x55;
    for (int i = 0; i < 4; i++) {
        c711.seek_done[i] = NEVER;
    }
    for (int i = 0; i < ARC_FLOPPIES; i++) {
        floppies[i].changed = true;
    }
    fd_reset();
    ide_reset();
}

uint32_t c711_read(uint32_t addr, bool byte)
{
    if (addr >= 0x3012000) {
        return dack_read(addr >= 0x302A000);
    }
    uint32_t port = (addr >> 2) & 0x7FF;
    if (c711.cfg_mode == 2 && port == 0x3F1) {
        return c711.cfg[c711.cfg_index & 15];
    }
    if ((port & ~7u) == 0x1F0 || port == 0x3F6) {
        return ide_read(port, byte);
    }
    if ((port & ~7u) == 0x3F0) {
        return fd_read((int)(port & 7));
    }
    if ((port & ~7u) == 0x3F8) {
        return serial_read((int)(port & 7));
    }
    switch (port) {
    case 0x278: return c711.lpt_data;
    case 0x279: return 0xDF;            /* no printer: not busy, selected, no error */
    case 0x27A: return c711.lpt_ctrl;
    default: return 0xFF;
    }
}

void c711_write(uint32_t addr, uint32_t v, bool byte)
{
    if (addr >= 0x3012000) {
        dack_write((uint8_t)v, addr >= 0x302A000);
        return;
    }
    uint32_t port = (addr >> 2) & 0x7FF;
    uint8_t b = (uint8_t)v;
    /* configuration: 55 55 to 3F0, then index and data, AA to leave */
    if (port == 0x3F0) {
        if (c711.cfg_mode < 2) {
            c711.cfg_mode = b == 0x55 ? c711.cfg_mode + 1 : 0;
            return;
        }
        if (b == 0xAA) {
            c711.cfg_mode = 0;
        } else {
            c711.cfg_index = b;
        }
        return;
    }
    if (port == 0x3F1 && c711.cfg_mode == 2) {
        c711.cfg[c711.cfg_index & 15] = b;
        return;
    }
    if ((port & ~7u) == 0x1F0 || port == 0x3F6) {
        ide_write(port, (uint16_t)v, byte);
        return;
    }
    if ((port & ~7u) == 0x3F0) {
        fd_write((int)(port & 7), b);
        return;
    }
    if ((port & ~7u) == 0x3F8) {
        serial_write((int)(port & 7), b);
        return;
    }
    if (port == 0x278) {
        c711.lpt_data = b;
    } else if (port == 0x27A) {
        c711.lpt_ctrl = b;
    }
}

uint64_t c711_next_event(void)
{
    uint64_t t = c711.event < c711.index_next ? c711.event : c711.index_next;
    for (int i = 0; i < 4; i++) {
        if (c711.seek_done[i] < t) {
            t = c711.seek_done[i];
        }
    }
    uint64_t e = ide_next_event();
    return e < t ? e : t;
}

void c711_event(void)
{
    uint64_t now = arc_now();
    for (int i = 0; i < 4; i++) {
        if (c711.seek_done[i] <= now) {
            c711.seek_done[i] = NEVER;
            c711.busy &= (uint8_t)~(1u << i);
            c711.sense_due[i] = true;
            c711.irq = true;
            update_lines();
        }
    }
    if (c711.index_next <= now) {
        /* an index pulse a revolution while a motor runs with a disc in */
        if (c711.dor & 0xF0) {
            floppy_t *d = drive(c711.dor & 3);
            if (d && d->data && motor(c711.dor & 3)) {
                ioc_irqa(IRQA_PACK);
            }
            c711.index_next = now + REV_TICKS;
        } else {
            c711.index_next = NEVER;
        }
    }
    if (c711.event <= now) {
        exec_event();
    }
    if (ide_next_event() <= now) {
        ide_event();
    }
}
