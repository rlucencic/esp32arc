/*
 * The IDE (ATA) hard disc on the A3020's and A4000's 82C711: its task file
 * at ports 1F0-1F7, device control and alternate status at 3F6. Sectors of
 * 512 bytes go through the 16-bit data port by PIO; the drive interrupts
 * (IOC IRQ B 3) when a sector is there to read, or has been written.
 * Addresses are cylinder, head and sector in the geometry INITIALISE DEVICE
 * PARAMETERS sets (ADFS sets its disc record's), else 16 heads of 63
 * sectors. Only the master drive is there.
 *
 * The disc is an image file on the card, raw sectors (Arculator's images
 * with a 512-byte header in front work too: the boot block's disc record
 * shows where it is), read and written a sector at a time.
 */
#include <stdio.h>
#include <string.h>
#include "arc_internal.h"

#define SECTOR_TICKS    US(300)         /* a sector found and read */
#define COMMAND_TICKS   US(100)

enum {
    ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80,
    ER_ABRT = 0x04, ER_IDNF = 0x10,
};

static struct {
    FILE *f;
    char path[256];
    uint32_t sectors;                   /* in the image */
    uint32_t skip;                      /* bytes in front of sector 0 */
    int spt, heads;                     /* the geometry in use */
    int def_spt, def_heads;
    uint8_t error, features, count, sector, cyl_lo, cyl_hi, select, status, control;
    uint8_t command;
    int left;                           /* sectors of the command still to go */
    uint8_t buf[512];
    int pos;
    bool irq;
    uint64_t event;
    uint32_t reads, writes;
    bool dirty;
    bool active;
} ide;

/* ---- The image ---- */

static void geometry_from_image(void)
{
    /* the boot block's disc record (at 0xC00 + 0x1C0): sector size, sectors
     * a track, heads; at 0xFC0 the image has a 512-byte header
     */
    ide.skip = 0;
    ide.def_spt = 63;
    ide.def_heads = 16;
    static const uint32_t places[2] = { 0xDC0, 0xFC0 };
    for (int i = 0; i < 2; i++) {
        uint8_t rec[4];
        if (fseek(ide.f, (long)places[i], SEEK_SET) == 0 && fread(rec, 1, 4, ide.f) == 4 &&
            rec[0] == 9 && rec[1] && rec[1] <= 63 && rec[2] && rec[2] <= 16) {
            ide.skip = i ? 512 : 0;
            ide.def_spt = rec[1];
            ide.def_heads = rec[2];
            break;
        }
    }
    ide.spt = ide.def_spt;
    ide.heads = ide.def_heads;
}

bool arc_hd_insert(const char *path)
{
    arc_hd_eject();
    FILE *f = fopen(path, "r+b");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 512 * 64) {
        fclose(f);
        return false;
    }
    ide.f = f;
    snprintf(ide.path, sizeof(ide.path), "%s", path);
    geometry_from_image();
    ide.sectors = (uint32_t)((size - (long)ide.skip) / 512);
    ARC_LOG("IDE: %s, %u sectors, %d heads of %d", path, (unsigned)ide.sectors, ide.def_heads, ide.def_spt);
    space_forget(ARC_FLOPPIES);
    return true;
}

void arc_hd_eject(void)
{
    if (ide.f) {
        fclose(ide.f);
        ide.f = NULL;
    }
    ide.path[0] = 0;
    ide.dirty = false;
    space_forget(ARC_FLOPPIES);
}

bool ide_image_read(uint64_t offset, void *buf, uint32_t len)
{
    uint64_t size = (uint64_t)ide.sectors * 512;
    if (!ide.f || offset > size || len > size - offset) {
        return false;
    }
    return fseek(ide.f, (long)(ide.skip + offset), SEEK_SET) == 0 && fread(buf, 1, len, ide.f) == len;
}

const char *arc_hd_path(void)
{
    return ide.f ? ide.path : NULL;
}

uint64_t arc_hd_size(void)
{
    return ide.f ? (uint64_t)ide.sectors * 512 : 0;
}

bool arc_hd_dirty(void)
{
    return ide.dirty;
}

void arc_hd_flush(void)
{
    if (ide.f && ide.dirty) {
        fflush(ide.f);
        ide.dirty = false;
    }
}

void arc_hd_status(uint32_t *reads, uint32_t *writes, bool *active)
{
    *reads = ide.reads;
    *writes = ide.writes;
    *active = ide.active;
}

/* ---- The drive ---- */

static void update_irq(void)
{
    ioc_irqb(IRQB_HDD, ide.irq && !(ide.control & 2));
}

static void interrupt(void)
{
    ide.irq = true;
    update_irq();
}

static bool master(void)
{
    return !(ide.select & 0x10) && ide.f;
}

/* The task file's address as a sector of the image, or -1 */
static long lba(void)
{
    int cyl = ide.cyl_lo | ide.cyl_hi << 8, head = ide.select & 15, sec = ide.sector;
    if (sec < 1 || sec > ide.spt || head >= ide.heads) {
        return -1;
    }
    uint32_t n = ((uint32_t)cyl * ide.heads + head) * ide.spt + sec - 1;
    return n < ide.sectors ? (long)n : -1;
}

static void advance(void)
{
    if (++ide.sector > ide.spt) {
        ide.sector = 1;
        uint8_t head = (uint8_t)((ide.select & 15) + 1);
        if (head >= ide.heads) {
            head = 0;
            int cyl = (ide.cyl_lo | ide.cyl_hi << 8) + 1;
            ide.cyl_lo = (uint8_t)cyl;
            ide.cyl_hi = (uint8_t)(cyl >> 8);
        }
        ide.select = (uint8_t)((ide.select & 0xF0) | head);
    }
}

static void abort_command(uint8_t error)
{
    ide.error = error;
    ide.status = ST_DRDY | ST_DSC | ST_ERR;
    ide.active = false;
    interrupt();
}

static void later(uint64_t ticks)
{
    ide.status = ST_BSY | ST_DSC;
    ide.event = arc_now() + ticks;
    arc_schedule(ide.event);
}

static void identify(void)
{
    uint16_t w[256];
    memset(w, 0, sizeof(w));
    uint32_t cyls = ide.sectors / ((uint32_t)ide.def_heads * ide.def_spt);
    w[0] = 0x0040;                      /* fixed disc */
    w[1] = (uint16_t)(cyls > 65535 ? 65535 : cyls);
    w[3] = (uint16_t)ide.def_heads;
    w[6] = (uint16_t)ide.def_spt;
    static const char serial[] = "ESP32ARC0001        ";       /* 20, 8, 40 characters */
    static const char firmware[] = "1.0     ";
    static const char model[] = "esp32arc IDE disc                       ";
    for (int i = 0; i < 10; i++) w[10 + i] = (uint16_t)(serial[2 * i] << 8 | serial[2 * i + 1]);
    for (int i = 0; i < 4; i++) w[23 + i] = (uint16_t)(firmware[2 * i] << 8 | firmware[2 * i + 1]);
    for (int i = 0; i < 20; i++) w[27 + i] = (uint16_t)(model[2 * i] << 8 | model[2 * i + 1]);
    w[49] = 0x0200;                     /* LBA */
    w[53] = 1;
    uint32_t cur = ide.sectors / ((uint32_t)ide.heads * ide.spt);
    w[54] = (uint16_t)(cur > 65535 ? 65535 : cur);
    w[55] = (uint16_t)ide.heads;
    w[56] = (uint16_t)ide.spt;
    uint32_t cap = (uint32_t)w[54] * ide.heads * ide.spt;
    w[57] = (uint16_t)cap;
    w[58] = (uint16_t)(cap >> 16);
    w[60] = (uint16_t)ide.sectors;
    w[61] = (uint16_t)(ide.sectors >> 16);
    for (int i = 0; i < 256; i++) {
        ide.buf[2 * i] = (uint8_t)w[i];
        ide.buf[2 * i + 1] = (uint8_t)(w[i] >> 8);
    }
}

static void command(uint8_t c)
{
    if (!master()) {
        return;                         /* nobody there */
    }
    if (arc_fdc_log) {
        ARC_LOG("IDE %02X: count %u, sector %u, cylinder %u, head %u, features %02X", c, ide.count, ide.sector,
                ide.cyl_lo | ide.cyl_hi << 8, ide.select & 15, ide.features);
    }
    ide.command = c;
    ide.error = 0;
    ide.left = ide.count ? ide.count : 256;
    ide.active = true;
    switch (c) {
    case 0x20: case 0x21:               /* read sectors */
    case 0x40: case 0x41:               /* read verify */
        later(SECTOR_TICKS);
        break;
    case 0x30: case 0x31:               /* write sectors: the first sector, please */
    case 0x50:                          /* format track: its interleave table */
        ide.pos = 0;
        ide.status = ST_DRDY | ST_DSC | ST_DRQ;
        break;
    case 0x91:                          /* initialise device parameters */
        ide.spt = ide.count ? ide.count : 1;
        ide.heads = (ide.select & 15) + 1;
        later(COMMAND_TICKS);
        break;
    case 0xEC:                          /* identify device */
    case 0x90:                          /* execute device diagnostic */
    case 0xEF:                          /* set features */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x98:   /* power modes */
        later(COMMAND_TICKS);
        break;
    default:
        if ((c & 0xF0) == 0x10 || (c & 0xF0) == 0x70) {
            later(COMMAND_TICKS);       /* recalibrate, seek */
        } else {
            abort_command(ER_ABRT);
        }
        break;
    }
}

uint64_t ide_next_event(void)
{
    return ide.event;
}

void ide_event(void)
{
    ide.event = NEVER;
    long n;
    switch (ide.command) {
    case 0x20: case 0x21:
        if ((n = lba()) < 0) {
            abort_command(ER_IDNF);
            break;
        }
        if (fseek(ide.f, (long)ide.skip + n * 512, SEEK_SET) != 0 || fread(ide.buf, 1, 512, ide.f) != 512) {
            memset(ide.buf, 0, sizeof(ide.buf));
        }
        ide.reads++;
        ide.pos = 0;
        ide.status = ST_DRDY | ST_DSC | ST_DRQ;
        interrupt();
        break;
    case 0x40: case 0x41:
        if (lba() < 0) {
            abort_command(ER_IDNF);
            break;
        }
        if (--ide.left > 0) {
            advance();
            later(SECTOR_TICKS / 4);
            break;
        }
        ide.status = ST_DRDY | ST_DSC;
        ide.active = false;
        interrupt();
        break;
    case 0x30: case 0x31:
        if ((n = lba()) < 0) {
            abort_command(ER_IDNF);
            break;
        }
        fseek(ide.f, (long)ide.skip + n * 512, SEEK_SET);
        fwrite(ide.buf, 1, 512, ide.f);
        ide.writes++;
        ide.dirty = true;
        if (--ide.left > 0) {
            advance();
            ide.pos = 0;
            ide.status = ST_DRDY | ST_DSC | ST_DRQ;
        } else {
            ide.status = ST_DRDY | ST_DSC;
            ide.active = false;
        }
        interrupt();
        break;
    case 0x50: {                        /* format: the track's sectors zeroed */
        uint8_t save = ide.sector;
        ide.sector = 1;
        static const uint8_t zero[512];
        for (int i = 0; i < ide.spt; i++, ide.sector++) {
            if ((n = lba()) >= 0) {
                fseek(ide.f, (long)ide.skip + n * 512, SEEK_SET);
                fwrite(zero, 1, 512, ide.f);
            }
        }
        ide.sector = save;
        ide.dirty = true;
        ide.status = ST_DRDY | ST_DSC;
        ide.active = false;
        interrupt();
        break;
    }
    case 0xEC:
        identify();
        ide.pos = 0;
        ide.status = ST_DRDY | ST_DSC | ST_DRQ;
        interrupt();
        break;
    case 0x90:
        ide.error = 0x01;               /* the drive is fine */
        ide.status = ST_DRDY | ST_DSC;
        ide.active = false;
        interrupt();
        break;
    case 0xE5: case 0x98:
        ide.count = 0xFF;               /* active */
        /* fall through */
    default:
        ide.status = ST_DRDY | ST_DSC;
        ide.active = false;
        interrupt();
        break;
    }
}

/* A sector's 512 bytes are through the data port */
static void sector_done(void)
{
    switch (ide.command) {
    case 0x20: case 0x21:
        if (--ide.left > 0) {
            advance();
            later(SECTOR_TICKS);
        } else {
            ide.status = ST_DRDY | ST_DSC;
            ide.active = false;
        }
        break;
    case 0x30: case 0x31: case 0x50:
        later(SECTOR_TICKS);
        break;
    default:
        ide.status = ST_DRDY | ST_DSC;  /* identify */
        ide.active = false;
        break;
    }
}

uint32_t ide_read(uint32_t port, bool byte)
{
    if (port == 0x1F0) {
        if (!(ide.status & ST_DRQ)) {
            return 0xFFFF;
        }
        uint32_t v = ide.buf[ide.pos++];
        if (!byte) {
            v |= (uint32_t)ide.buf[ide.pos++] << 8;
        }
        if (ide.pos >= 512) {
            sector_done();
        }
        return v;
    }
    if (!master() && port != 0x1F6) {
        return 0;                       /* no drive: not ready */
    }
    switch (port) {
    case 0x1F1: return ide.error;
    case 0x1F2: return ide.count;
    case 0x1F3: return ide.sector;
    case 0x1F4: return ide.cyl_lo;
    case 0x1F5: return ide.cyl_hi;
    case 0x1F6: return (uint8_t)(ide.select | 0xA0);
    case 0x1F7:
        ide.irq = false;
        update_irq();
        return ide.status;
    default:                            /* 3F6: alternate status */
        return ide.status;
    }
}

void ide_write(uint32_t port, uint16_t v, bool byte)
{
    uint8_t b = (uint8_t)v;
    switch (port) {
    case 0x1F0:
        if (!(ide.status & ST_DRQ)) {
            break;
        }
        ide.buf[ide.pos++] = b;
        if (!byte && ide.pos < 512) {
            ide.buf[ide.pos++] = (uint8_t)(v >> 8);
        }
        if (ide.pos >= 512) {
            sector_done();
        }
        break;
    case 0x1F1: ide.features = b; break;
    case 0x1F2: ide.count = b; break;
    case 0x1F3: ide.sector = b; break;
    case 0x1F4: ide.cyl_lo = b; break;
    case 0x1F5: ide.cyl_hi = b; break;
    case 0x1F6: ide.select = b; break;
    case 0x1F7: command(b); break;
    case 0x3F6:
        if ((ide.control & 4) && !(b & 4)) {
            /* the end of a software reset */
            ide.event = NEVER;
            ide.status = ST_DRDY | ST_DSC;
            ide.error = 0x01;
            ide.count = ide.sector = 1;
            ide.cyl_lo = ide.cyl_hi = 0;
            ide.select = 0;
            ide.active = false;
        }
        ide.control = b;
        update_irq();
        break;
    default:
        break;
    }
}

void ide_reset(void)
{
    ide.event = NEVER;
    ide.status = ST_DRDY | ST_DSC;
    ide.error = 0x01;
    ide.count = ide.sector = 1;
    ide.cyl_lo = ide.cyl_hi = 0;
    ide.select = 0;
    ide.control = 0;
    ide.irq = false;
    ide.active = false;
    ide.pos = 0;
    if (ide.f) {
        ide.spt = ide.def_spt;
        ide.heads = ide.def_heads;
    }
}
