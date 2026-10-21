/*
 * Free space on the discs, for the web page, as their filesystems count it
 * (ADFS old and new map, DOS FAT on floppies: ../esp32emu's diskspace).
 *
 * Worked out on the emulation's task (space_frame(), from the frame loop,
 * once a second), which owns the images: when a disc went in, or a second
 * after its last write (its map written by then). Kept for the web page's
 * task.
 */
#include <string.h>
#include "arc_internal.h"
#include "diskspace.h"

#define DRIVES          (ARC_FLOPPIES + 1)

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
    bool done;                          /* worked out for the disc as it is */
    uint32_t writes;                    /* its write count then */
} space_t;

static space_t s_space[DRIVES];
static int s_frames;

static bool floppy_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    const floppy_t *f = ctx;
    if (offset > f->size || len > f->size - offset) {
        return false;
    }
    memcpy(buf, f->data + offset, len);
    return true;
}

static bool hd_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    (void)ctx;
    return ide_image_read(offset, buf, len);
}

bool space_count(int drive, uint64_t *free_bytes, uint64_t *size)
{
    diskspace_t s;
    if (drive >= 0 && drive < ARC_FLOPPIES) {
        floppy_t *f = &floppies[drive];
        diskspace_disk_t d = { floppy_read, f, f->size };
        if (f->data == NULL || (!diskspace_adfs(&d, &s) && !diskspace_fat_disk(&d, &s))) {
            return false;
        }
    } else if (drive == ARC_FLOPPIES) {
        diskspace_disk_t d = { hd_read, NULL, arc_hd_size() };
        if (arc_hd_path() == NULL || !diskspace_adfs(&d, &s)) {
            return false;
        }
    } else {
        return false;
    }
    *free_bytes = s.free;
    *size = s.size;
    return true;
}

static uint32_t writes(int drive)
{
    if (drive < ARC_FLOPPIES) {
        return floppies[drive].writes;
    }
    uint32_t reads, w;
    bool active;
    arc_hd_status(&reads, &w, &active);
    return w;
}

static void update(space_t *s, bool known, uint64_t free_bytes, uint64_t size)
{
    s->known = false;
    s->free = free_bytes;
    s->size = size;
    s->known = known;
}

void space_forget(int drive)
{
    if (drive >= 0 && drive < DRIVES) {
        update(&s_space[drive], false, 0, 0);
        s_space[drive].done = false;
        s_space[drive].writes = writes(drive);
    }
}

void space_frame(void)
{
    if (++s_frames < ARC_FRAME_HZ) {
        return;
    }
    s_frames = 0;
    for (int d = 0; d < DRIVES; d++) {
        space_t *s = &s_space[d];
        uint32_t w = writes(d);
        if (w != s->writes) {
            s->writes = w;              /* written: the next second, if it stays so */
            s->done = false;
            continue;
        }
        if (s->done) {
            continue;
        }
        s->done = true;
        uint64_t f = 0, size = 0;
        bool known = space_count(d, &f, &size);
        update(s, known, f, size);
    }
}

bool arc_disc_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= DRIVES || !s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}
