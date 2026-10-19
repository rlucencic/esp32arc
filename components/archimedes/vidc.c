/*
 * The VIDC1a's video: its registers, the raster's lines and frames (the
 * vertical flyback interrupt at the display's end), the video DMA's share
 * of the bus, and the picture: the display area (1, 2, 4 or 8 bits a
 * pixel, from physical RAM between MEMC's Vstart and Vend, starting at
 * Vinit) with the hardware pointer (32 pixels wide, 2 bits, from Cinit).
 *
 * Registers (a write's bits 31-24; values in 23-0):
 *   00-3C palette 0-15 (bit 12 supremacy, B 11-8, G 7-4, R 3-0)
 *   40 border  44-4C pointer colours 1-3   60-7C stereo images (sound.c)
 *   80 HCR 84 HSWR 88 HBSR 8C HDSR 90 HDER 94 HBER 98 HCSR 9C HIR
 *   A0 VCR A4 VSWR A8 VBSR AC VDSR B0 VDER B4 VBER B8 VCSR BC VCER
 *   C0 sound frequency (sound.c)   E0 control: pixel rate (1-0), bits a
 *   pixel (3-2), DMA request (5-4), interlace (6), sync (7)
 * Horizontal values count pixel pairs (HDSR/HDER plus 19, 11, 7, 5 pixels
 * at 1, 2, 4, 8 bits, the pointer's HCSR single pixels plus 6), vertical
 * ones lines (plus 1), as MAME's acorn_vidc has them.
 *
 * The picture shown is a snapshot taken where each frame's display
 * begins (the address, the palette, the pointer), with the display's
 * bytes copied out of RAM while the frame is shown (arc_copy_picture()):
 * a program that flips between two screen buffers draws into the other
 * one meanwhile, but clears this one as soon as the next frame starts, so
 * the presenter (on core 0, some frames later) can't read it from RAM.
 * Three frames: the one the presenter draws, the last one copied, and
 * the next one.
 */
#include <stdlib.h>
#include <string.h>
#include "arc_internal.h"

vidc_t vidc;

#define R(r)            (vidc.reg[(r) >> 2])
#define RAW(r)          ((int)((R(r) >> 14) & 0x3FF))

#define POINTER_ROWS    64

typedef struct {
    bool valid;
    int width, height, bpp_log;
    uint32_t start, vstart, vend;       /* physical: the first byte, the buffer */
    uint32_t bytes;                     /* a line */
    uint16_t pal[16];
    uint32_t pairs[256];                /* 4 bits: a byte's two pixels */
    uint16_t pal8[256];
    bool pointer;
    int px, py, ph;                     /* the pointer: position, lines */
    uint32_t cinit;
    uint16_t pcol[4];
    uint8_t shape[POINTER_ROWS * 8];    /* 2 bits a pixel */
    uint32_t signature;
} picture_t;

typedef struct {
    picture_t pic;
    uint8_t *data;                      /* the display's lines (the wrap at Vend undone) */
    uint32_t cap;                       /* bytes allocated */
} frame_t;

static picture_t s_now;                 /* the last snapshot (the emulation's own) */
static frame_t s_frames[3];
/* Which frame is the last copied (bits 1-0), the presenter's (3-2), and
 * one being copied (bit 4, the frame in s_copying): changed atomically
 */
#define LATEST(st)      ((st) & 3u)
#define PINNED(st)      (((st) >> 2) & 3u)
#define COPYING         0x10u
static uint32_t s_state;
static int s_copying;
static uint32_t s_queued;               /* the signature last given to be copied */
static uint32_t s_taken;                /* the presenter took a picture (it sets it) */
static uint32_t s_since_taken, s_interval = 2 << 4;
static bool s_copied;                   /* a frame was copied since */
static uint32_t s_written;

static uint16_t rgb565(uint32_t p)
{
    uint32_t r = p & 15, g = (p >> 4) & 15, b = (p >> 8) & 15;
    return (uint16_t)(((r << 1 | r >> 3) << 11) | ((g << 2 | g >> 2) << 5) | (b << 1 | b >> 3));
}

/* A line: HCR pixels at the pixel rate (the clock / 3, / 2, / 1.5, / 1),
 * in ticks; *frac carries what's left over to the next line. In 32 bits
 * (at most 2050 pixels x 12 MHz x 3000, over at most 36 MHz x 2): 64-bit
 * divisions are library calls on the board, and this runs every line.
 */
static uint32_t s_line_whole;           /* a line's whole ticks (vidc_line_ticks()) */

uint32_t vidc_line_ticks(uint32_t *frac)
{
    static const uint8_t mul[4] = { 1, 1, 2, 1 }, div[4] = { 3, 2, 3, 1 };
    /* Worked out again only when what it depends on changes */
    static uint32_t hcr_was, clock_was, mhz_was, rem, den = 1;
    static int rate_was;
    uint32_t whole = s_line_whole;
    uint32_t hcr = (uint32_t)RAW(0x80) * 2 + 2;
    if (hcr != hcr_was || vidc.clock_khz != clock_was || vidc.pixel_rate != rate_was || arc.tick_mhz != mhz_was) {
        hcr_was = hcr;
        clock_was = vidc.clock_khz;
        rate_was = vidc.pixel_rate;
        mhz_was = arc.tick_mhz;
        if (hcr < 64) {
            whole = (uint32_t)US(64);   /* not set up yet: a TV's 64 us */
            rem = 0;
            den = 1;
        } else {
            uint32_t num = hcr * arc.tick_mhz * 1000 * div[vidc.pixel_rate];
            den = vidc.clock_khz * mul[vidc.pixel_rate];
            whole = num / den;
            rem = num % den;
        }
        s_line_whole = whole;
    }
    uint32_t f = *frac + rem, t = whole;
    if (f >= den) {
        f -= den;
        t++;
        if (f >= den) {                 /* (den just changed) */
            t += f / den;
            f %= den;
        }
    }
    *frac = f;
    return t;
}

static int vtotal(void)
{
    int v = RAW(0xA0) + 1;
    return v < 16 ? 312 : v;
}

void vidc_reset(void)
{
    memset(&vidc, 0, sizeof(vidc));
    memset(&s_now, 0, sizeof(s_now));
    for (int i = 0; i < 3; i++) {
        s_frames[i].pic.valid = false;  /* (a copy still running finishes into its frame) */
    }
    s_queued = 0;
    vidc.line_end = arc_now() + US(64);
    vidc.stop_at = vidc.line_end;
    vidc.hz100 = 5000;
    vidc.clock_khz = 24000;
}

void vidc_dma_changed(void)
{
}

void vidc_write(uint32_t v)
{
    uint32_t reg = (v >> 24) & 0xFC;
    uint32_t value = v & 0xFFFFFF;
    if (reg >= 0x60 && reg < 0x80) {
        sound_write_stereo((int)(((reg >> 2) - 1) & 7), value);
        return;
    }
    if (reg == 0xC0) {
        sound_write_sfr(value);
        return;
    }
    vidc.reg[reg >> 2] = value;
    if (reg >= 0x80) {
        /* the raster's timing or the pixel rate: the lines run at once
         * are worked out again at the line's end
         */
        vidc.stop_at = vidc.line_end;
        arc_schedule(vidc.line_end);
    }
    if (reg < 0x40) {
        vidc.palette[reg >> 2] = rgb565(value);
    } else if (reg == 0x40) {
        vidc.border = rgb565(value);
    } else if (reg < 0x50) {
        vidc.cursor[(reg - 0x40) >> 2] = rgb565(value);
    } else if (reg == 0xE0) {
        vidc.pixel_rate = value & 3;
        vidc.bpp_log = (value >> 2) & 3;
    }
}

/* ---- The picture's snapshot ---- */

static uint32_t hash(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static void queue_copy(void);

static void snapshot(void)
{
    picture_t *p = &s_now;
    static const int xstep[4] = { 19, 11, 7, 5 };
    int bpp = vidc.bpp_log;
    int hdsr = RAW(0x8C) * 2 + xstep[bpp];
    int hder = RAW(0x90) * 2 + xstep[bpp];
    p->width = hder - hdsr;
    p->height = RAW(0xB0) - RAW(0xAC);
    p->bpp_log = bpp;
    p->valid = memc.video_dma && p->width > 0 && p->width <= ARC_SCREEN_MAX_W && p->height > 0 &&
               p->height <= ARC_SCREEN_MAX_H;
    p->bytes = (uint32_t)(p->width << bpp) / 8;
    p->vstart = memc.vstart;
    p->vend = memc.vend + 16;
    p->start = memc.vinit;
    if (p->vend <= p->vstart) {
        p->vend = p->vstart + 16;
    }
    memcpy(p->pal, vidc.palette, sizeof(p->pal));
    if (bpp == 2) {
        for (int i = 0; i < 256; i++) {
            p->pairs[i] = vidc.palette[i & 15] | (uint32_t)vidc.palette[i >> 4] << 16;
        }
    } else if (bpp == 3) {
        for (int i = 0; i < 256; i++) {
            uint32_t e = vidc.reg[i & 15];
            uint32_t r = (e & 7) | ((i & 0x10) >> 1);
            uint32_t g = ((e >> 4) & 3) | ((i & 0x60) >> 3);
            uint32_t b = ((e >> 8) & 7) | ((i & 0x80) >> 4);
            p->pal8[i] = rgb565(r | g << 4 | b << 8);
        }
    }
    /* The pointer */
    int hcsr = (int)((R(0x98) >> 13) & 0x7FF) + 6;
    p->px = hcsr - hdsr;
    p->py = RAW(0xB8) - RAW(0xAC);
    p->ph = RAW(0xBC) - RAW(0xB8);
    p->cinit = memc.cinit;
    p->pointer = memc.video_dma && p->ph > 0;
    p->pcol[0] = 0;
    memcpy(&p->pcol[1], &vidc.cursor[1], 3 * sizeof(uint16_t));

    /* The signature: what the picture is made of, and the screen's writes */
    if (arc.cpu.watch_hit) {
        arc.cpu.watch_hit = false;
        s_written++;
    }
    uint32_t h = hash(2166136261u, &s_written, sizeof(s_written));
    h = hash(h, &p->width, sizeof(int) * 3);
    h = hash(h, &p->start, sizeof(uint32_t) * 4);
    h = hash(h, p->pal, sizeof(p->pal));
    h = hash(h, &p->pointer, sizeof(bool));
    h = hash(h, &p->px, sizeof(int) * 3);
    h = hash(h, p->pcol, sizeof(p->pcol));
    if (p->pointer) {
        int n = p->ph > POINTER_ROWS ? POINTER_ROWS : p->ph;
        memc_copy(p->shape, p->cinit, (uint32_t)n * 8);
        h = hash(h, p->shape, (size_t)n * 8);
    }
    p->signature = h;
    queue_copy();
}

/* A frame for the copy of this one, if it changed (none while the last
 * one is still being copied: the next snapshot tries again)
 */
static void queue_copy(void)
{
    /* The presenter on the board takes a picture every few frames: only
     * the frame before it's expected to is copied (copying all of them
     * would take the memory from the emulation for nothing). The frames
     * between its pictures, averaged (x 16).
     */
    if (arc.cfg.picture_ready) {
        s_since_taken++;
        if (__atomic_exchange_n(&s_taken, 0, __ATOMIC_ACQUIRE)) {
            s_interval = (s_interval * 3 + (s_since_taken << 4)) / 4;
            s_since_taken = 0;
            s_copied = false;
        }
        if (s_copied || (s_since_taken + 1) << 4 < s_interval) {
            return;
        }
    }
    if (s_now.signature == s_queued) {
        return;
    }
    uint32_t st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (st & COPYING) {
        return;
    }
    int f = 0;
    while ((uint32_t)f == LATEST(st) || (uint32_t)f == PINNED(st)) {
        f++;
    }
    s_frames[f].pic = s_now;
    s_copying = f;
    s_queued = s_now.signature;
    s_copied = true;
    __atomic_fetch_or(&s_state, COPYING, __ATOMIC_RELEASE);
    if (arc.cfg.picture_ready) {
        arc.cfg.picture_ready();
    } else {
        arc_copy_picture();
    }
}

void arc_copy_picture(void)
{
    uint32_t st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (!(st & COPYING)) {
        return;
    }
    frame_t *fr = &s_frames[s_copying];
    picture_t *p = &fr->pic;
    if (p->valid) {
        uint32_t need = p->bytes * (uint32_t)p->height;
        if (need > fr->cap) {
            free(fr->data);
            fr->cap = (need + 0xFFFF) & ~0xFFFFu;
            fr->data = arc_calloc(1, fr->cap);
            if (!fr->data) {
                fr->cap = 0;
            }
        }
        if (fr->data) {
            /* From Vinit on, wrapping from Vend to Vstart */
            uint32_t size = p->vend - p->vstart;
            uint32_t off = (p->start - p->vstart) % size;
            for (uint32_t done = 0; done < need;) {
                uint32_t n = size - off < need - done ? size - off : need - done;
                memc_copy(fr->data + done, p->vstart + off, n);
                done += n;
                off = 0;
            }
        } else {
            p->valid = false;
        }
    }
    /* The last copied now, and the copying done */
    uint32_t want;
    do {
        want = (st & ~(COPYING | 3u)) | (uint32_t)s_copying;
    } while (!__atomic_compare_exchange_n(&s_state, &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

/* The presenter's frame: the last copied, from its signature() on */
static const picture_t *shown(void)
{
    return &s_frames[PINNED(__atomic_load_n(&s_state, __ATOMIC_ACQUIRE))].pic;
}

/* ---- The raster ---- */

/* The display's lines run this many at a time: the video's bus time on
 * each goes in as they're caught up after the run (the CPU's progress
 * then moves by less than a line against the raster). The machine stops
 * exactly where the display starts and ends.
 */
#define LINES_AT_ONCE   8

void vidc_line_end(void)
{
    uint64_t start = vidc.line_end;     /* the next line's */
    vidc.line_end += vidc_line_ticks(&vidc.line_frac);

    vidc.line++;
    int total = vtotal();
    if (vidc.line >= total) {
        vidc.line = 0;
        uint64_t took = start - vidc.vidc_frame_start;
        if (took > 0 && took < arc.tick_mhz * 1000000u) {
            vidc.hz100 = (uint32_t)((uint64_t)arc.tick_mhz * 100000000u / took);
        }
        vidc.vidc_frame_start = start;
    }
    int vdsr = RAW(0xAC) + 1, vder = RAW(0xB0) + 1;
    if (vidc.line == vdsr) {
        vidc.display = true;
        vidc.flyback = false;
        vidc.addr = memc.vinit;
        snapshot();
        /* The video's DMA: a quad-word (N + 3S) for each 16 bytes of a line */
        uint32_t bytes = (uint32_t)((RAW(0x90) - RAW(0x8C)) * 2) << vidc.bpp_log >> 3;
        vidc.dma_ticks = memc.video_dma ? bytes / 16 * 5 : 0;
    }
    if (vidc.line == vder) {
        vidc.display = false;
        vidc.flyback = true;
        vidc.vidc_frames++;
        ioc_irqa(IRQA_VSYNC);
    }
    if (vidc.display && vidc.dma_ticks) {
        /* the CPU waits while the video takes the bus: time it doesn't get */
        arc.cpu.ticks += vidc.dma_ticks;
    }

    /* The next stop: where the display starts or ends, or LINES_AT_ONCE
     * lines on inside it (a little early: the lines' fractions left out)
     */
    int n = (vidc.display ? vder : vdsr) - vidc.line;
    if (n <= 0) {
        n += total;
    }
    int most = vidc.display ? LINES_AT_ONCE : 64;
    if (n <= 0 || n > most) {
        n = most;
    }
    vidc.stop_at = vidc.line_end + (uint64_t)(n - 1) * s_line_whole;
}

bool vidc_hsync(void)
{
    uint32_t frac = 0;
    uint32_t line = vidc_line_ticks(&frac);
    uint64_t now = arc_now();
    /* (the lines run at once may not be caught up yet) */
    uint64_t start = vidc.line_end - line;
    uint64_t into = now > start ? (now - start) % line : 0;
    /* the sync pulse: HSWR pixel pairs from the line's start */
    uint32_t hcr = (uint32_t)RAW(0x80) * 2 + 2, hsw = (uint32_t)RAW(0x84) * 2 + 2;
    return hcr && into * hcr < (uint64_t)line * hsw;
}

/* Where the pointer points: its shape's first pixel from the top left (the
 * arrow's tip; RISC OS keeps it some columns into the 32)
 */
bool vidc_pointer(int *x, int *y)
{
    const picture_t *p = &s_now;
    if (!p->valid || !p->pointer) {
        return false;
    }
    int n = p->ph > 64 ? 64 : p->ph;
    for (int row = 0; row < n; row++) {
        const uint8_t *c = memc_phys(p->cinit + (uint32_t)row * 8);
        for (int i = 0; i < 32; i++) {
            if ((c[i >> 2] >> ((i & 3) * 2)) & 3) {
                *x = p->px + i;
                *y = p->py + row;
                return true;
            }
        }
    }
    return false;
}

/* ---- Drawing (the presenter's task) ---- */

void arc_screen_size(int *width, int *height)
{
    const picture_t *p = shown();
    if (p->valid) {
        *width = p->width;
        *height = p->height;
    } else {
        *width = 640;
        *height = 256;
    }
}

/* The presenter's frame from now on: the last copied */
uint32_t arc_screen_signature(void)
{
    uint32_t st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~(3u << 2)) | LATEST(st) << 2;
    } while (!__atomic_compare_exchange_n(&s_state, &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    __atomic_store_n(&s_taken, 1, __ATOMIC_RELEASE);
    return s_frames[LATEST(st)].pic.signature;
}

void arc_render_line_rgb565(int y, uint16_t *dst)
{
    const frame_t *fr = &s_frames[PINNED(__atomic_load_n(&s_state, __ATOMIC_ACQUIRE))];
    const picture_t *p = &fr->pic;
    if (!p->valid || !fr->data) {
        memset(dst, 0, 640 * sizeof(uint16_t));
        return;
    }
    int w = p->width;
    if (y < 0 || y >= p->height) {
        memset(dst, 0, (size_t)w * sizeof(uint16_t));
        return;
    }
    const uint8_t *line = fr->data + (uint32_t)y * p->bytes;

    switch (p->bpp_log) {
    case 0:
        for (int x = 0; x < w; x += 8) {
            uint8_t b = line[x >> 3];
            for (int i = 0; i < 8; i++) {
                dst[x + i] = p->pal[(b >> i) & 1];
            }
        }
        break;
    case 1:
        for (int x = 0; x < w; x += 4) {
            uint8_t b = line[x >> 2];
            dst[x] = p->pal[b & 3];
            dst[x + 1] = p->pal[(b >> 2) & 3];
            dst[x + 2] = p->pal[(b >> 4) & 3];
            dst[x + 3] = p->pal[b >> 6];
        }
        break;
    case 2: {
        uint32_t *d = (uint32_t *)dst;
        for (int i = 0; i < w / 2; i++) {
            d[i] = p->pairs[line[i]];
        }
        break;
    }
    default:
        for (int x = 0; x < w; x++) {
            dst[x] = p->pal8[line[x]];
        }
        break;
    }

    /* The pointer on top: 32 pixels, colour 0 transparent */
    int row = y - p->py;
    if (p->pointer && row >= 0 && row < p->ph && row < POINTER_ROWS) {
        const uint8_t *c = p->shape + row * 8;
        for (int i = 0; i < 32; i++) {
            int v = (c[i >> 2] >> ((i & 3) * 2)) & 3;
            int x = p->px + i;
            if (v && x >= 0 && x < w) {
                dst[x] = p->pcol[v];
            }
        }
    }
}
