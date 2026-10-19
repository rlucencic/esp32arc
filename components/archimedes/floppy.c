/*
 * The floppy disc images, for either controller (the A440's WD1772 in
 * fdc.c, the A3020's 82C711 in c711.c): tracks one after the other, the
 * two sides of a track together:
 *   819200   ADFS D/E   80 tracks, 2 sides, 5 sectors of 1024 (IDs 0-4)
 *   655360   ADFS L     80, 2, 16 of 256 (0-15)
 *   327680   ADFS M     80, 1, 16 of 256
 *   163840   ADFS S     40, 1, 16 of 256
 *   737280   DOS 720K   80, 2, 9 of 512 (1-9)
 *   1638400  ADFS F     80, 2, 10 of 1024 (0-9), high density
 *   1474560  DOS 1.44M  80, 2, 18 of 512 (1-18), high density
 * Kept in memory; written tracks go back to the file at arc_floppy_flush().
 * New 800 KB images come formatted (ADFS E, empty), others blank.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "arc_internal.h"

floppy_t floppies[ARC_FLOPPIES];
int floppy_light = -1;

long floppy_offset(const floppy_t *d, int track, int side, int sector)
{
    if (!d->data || track < 0 || track >= d->tracks || side >= d->sides) {
        return -1;
    }
    int s = sector - d->first;
    if (s < 0 || s >= d->spt) {
        return -1;
    }
    return (((long)track * d->sides + side) * d->spt + s) * d->ssize;
}

void floppy_mark_dirty(floppy_t *d, int track, int side)
{
    int t = track * d->sides + side;
    if (t < FLOPPY_MAX_TRACKS * 2) {
        d->dirty[t / 8] |= (uint8_t)(1u << (t & 7));
        d->any_dirty = true;
    }
}

static bool geometry(floppy_t *d)
{
    static const struct { uint32_t size; int tracks, sides, spt, ssize, first; bool hd; } formats[] = {
        { 819200, 80, 2, 5, 1024, 0, false },
        { 655360, 80, 2, 16, 256, 0, false },
        { 327680, 80, 1, 16, 256, 0, false },
        { 163840, 40, 1, 16, 256, 0, false },
        { 737280, 80, 2, 9, 512, 1, false },
        { 1638400, 80, 2, 10, 1024, 0, true },
        { 1474560, 80, 2, 18, 512, 1, true },
    };
    d->hd = false;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (d->size == formats[i].size) {
            d->tracks = formats[i].tracks;
            d->sides = formats[i].sides;
            d->spt = formats[i].spt;
            d->ssize = formats[i].ssize;
            d->first = formats[i].first;
            d->hd = formats[i].hd;
            return true;
        }
    }
    /* Shorter ADFS E images (the last tracks left out) */
    if (d->size % 10240 == 0 && d->size < 819200) {
        d->tracks = (int)(d->size / 10240);
        d->sides = 2;
        d->spt = 5;
        d->ssize = 1024;
        d->first = 0;
        return true;
    }
    return false;
}

bool arc_floppy_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= ARC_FLOPPIES) {
        return false;
    }
    floppy_t *d = &floppies[drive];
    arc_floppy_eject(drive);
    bool ro = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        ro = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 2 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    d->size = (uint32_t)size;
    if (!geometry(d)) {
        ARC_LOG("drive %d: %s: %ld bytes is no floppy format known here", drive, path, size);
        fclose(f);
        return false;
    }
    d->data = arc_malloc((size_t)size);
    if (!d->data) {
        fclose(f);
        return false;
    }
    bool ok = fread(d->data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(d->data);
        d->data = NULL;
        return false;
    }
    d->read_only = ro;
    d->changed = true;
    snprintf(d->path, sizeof(d->path), "%s", path);
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
    ARC_LOG("drive %d: %s, %d tracks, %d side%s, %d x %d%s", drive, path, d->tracks, d->sides,
            d->sides > 1 ? "s" : "", d->spt, d->ssize, ro ? ", read only" : "");
    return true;
}

static void flush_drive(floppy_t *d)
{
    if (!d->data || !d->any_dirty || d->read_only) {
        return;
    }
    FILE *f = fopen(d->path, "r+b");
    if (!f) {
        return;
    }
    uint32_t track_bytes = (uint32_t)d->spt * d->ssize;
    for (int t = 0; t < d->tracks * d->sides; t++) {
        if (d->dirty[t / 8] & (1u << (t & 7))) {
            long off = (long)t * track_bytes;
            fseek(f, off, SEEK_SET);
            fwrite(d->data + off, 1, track_bytes, f);
        }
    }
    fclose(f);
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
}

void arc_floppy_eject(int drive)
{
    if (drive < 0 || drive >= ARC_FLOPPIES) {
        return;
    }
    floppy_t *d = &floppies[drive];
    flush_drive(d);
    free(d->data);
    d->data = NULL;
    d->path[0] = 0;
    d->changed = true;
}

const char *arc_floppy_path(int drive)
{
    return drive >= 0 && drive < ARC_FLOPPIES && floppies[drive].data ? floppies[drive].path : NULL;
}

bool arc_floppy_write_protected(int drive)
{
    return drive >= 0 && drive < ARC_FLOPPIES && floppies[drive].read_only;
}

/* An empty ADFS E disc called "Blank", as RISC OS 3.11's *Format makes it:
 * the map's two copies (one zone, the disc record in it) and the root
 * directory, the rest zeros. Each blank disc gets a disc ID of its own
 * (bytes 18-19 of the map), and the map its check byte again.
 */
typedef struct {
    uint16_t at;
    uint8_t value;
} image_byte_t;

static const image_byte_t s_e_map[] = {
    { 0x001, 0x18 }, { 0x002, 0x82 }, { 0x003, 0xFF }, { 0x004, 0x0A }, { 0x005, 0x05 }, { 0x006, 0x02 },
    { 0x007, 0x02 }, { 0x008, 0x0F }, { 0x009, 0x07 }, { 0x00A, 0x01 }, { 0x00D, 0x01 }, { 0x00E, 0x20 },
    { 0x00F, 0x05 }, { 0x010, 0x03 }, { 0x011, 0x02 }, { 0x015, 0x80 }, { 0x016, 0x0C }, { 0x01A, 0x42 },
    { 0x01B, 0x6C }, { 0x01C, 0x61 }, { 0x01D, 0x6E }, { 0x01E, 0x6B }, { 0x01F, 0x20 }, { 0x020, 0x20 },
    { 0x021, 0x20 }, { 0x022, 0x20 }, { 0x023, 0x20 }, { 0x040, 0x02 }, { 0x043, 0x80 }, { 0x35F, 0x80 },
};

static const image_byte_t s_e_root[] = {
    { 0x001, 0x4E }, { 0x002, 0x69 }, { 0x003, 0x63 }, { 0x004, 0x6B }, { 0x7DA, 0x03 }, { 0x7DB, 0x02 },
    { 0x7DD, 0x42 }, { 0x7DE, 0x6C }, { 0x7DF, 0x61 }, { 0x7E0, 0x6E }, { 0x7E1, 0x6B }, { 0x7E2, 0x0D },
    { 0x7F0, 0x42 }, { 0x7F1, 0x6C }, { 0x7F2, 0x61 }, { 0x7F3, 0x6E }, { 0x7F4, 0x6B }, { 0x7F5, 0x0D },
    { 0x7FB, 0x4E }, { 0x7FC, 0x69 }, { 0x7FD, 0x63 }, { 0x7FE, 0x6B }, { 0x7FF, 0xB7 },
};

/* A new map zone's check byte (byte 0), as FileCore works it out */
static uint8_t zone_check(const uint8_t *zone, uint32_t size)
{
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uint32_t i = size - 4;
    for (; i > 0; i -= 4) {
        s0 += zone[i] + (s3 >> 8);
        s3 &= 0xFF;
        s1 += zone[i + 1] + (s0 >> 8);
        s0 &= 0xFF;
        s2 += zone[i + 2] + (s1 >> 8);
        s1 &= 0xFF;
        s3 += zone[i + 3] + (s2 >> 8);
        s2 &= 0xFF;
    }
    s0 += s3 >> 8;                      /* the check byte itself left out */
    s1 += zone[1] + (s0 >> 8);
    s2 += zone[2] + (s1 >> 8);
    s3 += zone[3] + (s2 >> 8);
    return (uint8_t)(s0 ^ s1 ^ s2 ^ s3);
}

bool arc_floppy_create(const char *path, uint32_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    static uint32_t serial;
    uint16_t id = (uint16_t)(time(NULL) * 31 + ++serial * 977 + (uintptr_t)path);
    uint8_t buf[1024];
    bool ok = true;
    for (uint32_t done = 0; ok && done < bytes; done += sizeof(buf)) {
        memset(buf, 0, sizeof(buf));
        if (bytes == 819200 && done < 0x1000) {
            if (done < 0x800) {
                /* the map, twice */
                for (size_t i = 0; i < sizeof(s_e_map) / sizeof(s_e_map[0]); i++) {
                    buf[s_e_map[i].at] = s_e_map[i].value;
                }
                buf[0x18] = (uint8_t)id;
                buf[0x19] = (uint8_t)(id >> 8);
                buf[0] = zone_check(buf, sizeof(buf));
            } else {
                for (size_t i = 0; i < sizeof(s_e_root) / sizeof(s_e_root[0]); i++) {
                    uint32_t at = 0x800 + s_e_root[i].at;
                    if (at >= done && at < done + sizeof(buf)) {
                        buf[at - done] = s_e_root[i].value;
                    }
                }
            }
        }
        uint32_t n = bytes - done < sizeof(buf) ? bytes - done : (uint32_t)sizeof(buf);
        ok = fwrite(buf, 1, n, f) == n;
    }
    return fclose(f) == 0 && ok;
}

bool arc_floppy_dirty(void)
{
    for (int i = 0; i < ARC_FLOPPIES; i++) {
        if (floppies[i].any_dirty) {
            return true;
        }
    }
    return false;
}

void arc_floppy_flush(void)
{
    for (int i = 0; i < ARC_FLOPPIES; i++) {
        flush_drive(&floppies[i]);
    }
}

void arc_floppy_status(arc_floppy_status_t *s)
{
    s->light = floppy_light;
    for (int i = 0; i < ARC_FLOPPIES; i++) {
        s->reads[i] = floppies[i].reads;
        s->writes[i] = floppies[i].writes;
        s->track[i] = (uint8_t)floppies[i].head;
    }
}

