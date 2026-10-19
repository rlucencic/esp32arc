/*
 * Between the Archimedes core's modules.
 *
 * Time: ticks of the memory clock since power-on (the ARM2's counter:
 * arc_now()): 8 MHz on the A440, 12 MHz on the A3020. A frame of
 * run_frame() is 20 ms of them; inside it the CPU runs up to the next
 * event: the end of the VIDC's line, an IOC timer, a byte to or from the
 * keyboard, the disc controller.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "arc.h"
#include "arm2.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define ARC_LOG(fmt, ...)   ESP_LOGI("arc", fmt, ##__VA_ARGS__)
/* The core's buffers in PSRAM: internal RAM is for WiFi, Bluetooth, the LCD */
#define arc_malloc(n)       heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define arc_calloc(n, s)    heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define ARC_LOG(fmt, ...)   printf("arc: " fmt "\n", ##__VA_ARGS__)
#define arc_malloc(n)       malloc(n)
#define arc_calloc(n, s)    calloc((n), (s))
#endif

#define NEVER               UINT64_MAX
#define FRAME_TICKS         (arc.tick_mhz * 1000000u / ARC_FRAME_HZ)
#define US(n)               ((uint64_t)(n) * arc.tick_mhz)     /* microseconds in ticks */

/* ---- arc.c: the machine ---- */

typedef struct {
    arc_config_t cfg;
    uint32_t tick_mhz;                  /* the memory clock: 8 or 12 */
    arm2_t cpu;
    uint32_t frames;
    uint64_t run_until;                 /* the tick the CPU's run ends */
    uint32_t insns_last;                /* instructions in the last frame */
} arc_machine_t;

extern arc_machine_t arc;

static inline uint64_t arc_now(void)
{
    return arm2_now(&arc.cpu);
}

/* An event at tick t: end the CPU's run early if it's before its end */
void arc_schedule(uint64_t t);

/* The interrupt lines from the IOC */
void arc_update_interrupts(void);

/* ---- memc.c: the MEMC1a ---- */

typedef struct {
    uint32_t control;
    int psize;                          /* page size: 4 KB << psize */
    bool os_mode, video_dma, sound_dma;
    bool rom_latch;                     /* after a reset: the ROM at 0, until physical RAM or the CAM is used */
    uint8_t rom_s, rom_n;               /* the ROM's cycles (ticks) */
    uint32_t vinit, vstart, vend, cinit;    /* physical RAM, bytes */
    uint32_t sstart, sendn;             /* the next sound buffer */
    uint32_t sptr, send;                /* the one playing */
    int16_t phys_to_log[128];           /* -1: none */
    uint8_t phys_ppl[128];
    int16_t log_to_phys[8192];          /* by logical page of the current size; -1 */
    uint32_t watch_lo, watch_hi;        /* the screen's bytes in physical RAM */
    uint32_t ram_mask;                  /* a physical address's bits that reach the RAM */
    int scramble;                       /* or 1-3: below 2 MB, 32 KB pages (and 8, 16 KB at 512 KB) */
} memc_t;

extern memc_t memc;
extern bool arc_memc_log;              /* host builds: log the CAM's writes */

void memc_reset(void);
void memc_build(void);                  /* the whole page table */
void memc_write_register(uint32_t addr);/* 0x3600000-0x37FFFFF (the address is the data) */
void memc_write_cam(uint32_t addr);     /* 0x3800000-0x3FFFFFF */
void memc_latch_off(void);              /* the reset's ROM at 0 goes */
/* Where a physical address (0-16 MB) is in the RAM. Bits 0-11 always stay;
 * which others reach the chips depends on the RAM fitted and, below 2 MB,
 * the page size (the MEMC's DRAM row and column wiring, as MAME's
 * dram_address() has it): RISC OS sizes the memory by its aliases.
 */
static inline uint32_t memc_ram_offset(uint32_t phys)
{
    switch (memc.scramble) {
    case 1: /* 512 KB, 8 or 16 KB pages: bit 12 doesn't reach the RAM */
        return (phys & 0xFFF) | ((phys >> 1) & 0x7F000);
    case 2: /* 512 KB, 32 KB pages: neither 12, 13 nor 20 */
        return (phys & 0xFFF) | ((phys >> 2) & 0x3F000) | ((phys >> 3) & 0x40000);
    case 3: /* 1 MB, 32 KB pages: neither 13 nor 20 */
        return (phys & 0x1FFF) | ((phys >> 1) & 0x7E000) | ((phys >> 2) & 0x80000);
    default:
        return phys & memc.ram_mask;
    }
}

static inline uint8_t *memc_phys(uint32_t offset)
{
    return arc.cfg.ram + memc_ram_offset(offset);
}

/* n bytes of physical RAM from phys (page by page: pages may be apart) */
void memc_copy(uint8_t *dst, uint32_t phys, uint32_t n);

/* ---- ioc.c: the IOC ---- */

enum {
    IRQA_PBSY = 0x01, IRQA_RI = 0x02, IRQA_PACK = 0x04, IRQA_VSYNC = 0x08,
    IRQA_POR = 0x10, IRQA_TM0 = 0x20, IRQA_TM1 = 0x40, IRQA_FORCE = 0x80,
    IRQB_PFIQ = 0x01, IRQB_SOUND = 0x02, IRQB_SERIAL = 0x04, IRQB_HDD = 0x08,
    IRQB_DISCCHG = 0x10, IRQB_PODULE = 0x20, IRQB_KTX = 0x40, IRQB_KRX = 0x80,
    FIQ_FDRQ = 0x01, FIQ_FIRQ = 0x02, FIQ_ECONET = 0x04, FIQ_PODULE = 0x40, FIQ_FORCE = 0x80,
};

typedef struct {
    uint8_t irqa, irqa_mask, irqb, irqb_mask, fiq, fiq_mask;
    uint8_t control;                    /* C0-C5 as written */
    uint8_t kart_rx;
    struct {
        uint16_t latch;
        uint16_t out;                   /* the count the latch command took */
        uint64_t expiry;                /* tick it next reaches zero */
    } timer[4];
    uint64_t ktx_done;                  /* the byte to the keyboard has gone, NEVER */
} ioc_t;

extern ioc_t ioc;

void ioc_reset(void);
uint8_t ioc_read(uint32_t addr);
void ioc_write(uint32_t addr, uint8_t value);
void ioc_irqa(uint8_t bits);            /* latched */
void ioc_irqb(uint8_t bits, bool on);   /* levels */
void ioc_fiq(uint8_t bits, bool on);
uint64_t ioc_next_event(void);
void ioc_event(void);
void ioc_kart_receive(uint8_t byte);    /* a byte from the keyboard */

/* ---- keyboard.c: the keyboard's microcontroller (and the mouse) ---- */

void keyboard_reset(void);
void keyboard_from_host(uint8_t byte);  /* the IOC sent it */
bool keyboard_typing(void);             /* text from arc_type() still going in */
extern bool arc_kb_log;                 /* host builds: log the serial link */
uint64_t keyboard_next_event(void);
void keyboard_event(void);

/* ---- cmos.c: the PCF8583 on the IOC's I2C lines ---- */

void cmos_reset(void);
void cmos_lines(bool scl, bool sda);    /* the IOC drives them (open drain: 1 releases) */
bool cmos_sda(void);                    /* what the PCF8583 drives (1: released) */

/* ---- vidc.c: the VIDC1a's video ---- */

typedef struct {
    uint32_t reg[64];                   /* by register >> 2: the 24-bit values */
    uint16_t palette[16], border, cursor[4];
    int bpp_log;                        /* 0-3: 1, 2, 4, 8 bits */
    int pixel_rate;                     /* 0-3: 8, 12, 16, 24 MHz */
    /* The raster */
    uint32_t clock_khz;                 /* the VIDC's clock: 24 MHz (the A3020's IOEB: 25.175, 36) */
    int line;                           /* 0 to vtotal - 1 */
    uint64_t line_end;                  /* tick the line ends */
    uint64_t stop_at;                   /* the next line end the machine stops at (vidc_line_end();
                                           the lines between are caught up after the run) */
    uint32_t line_frac;                 /* what a line's ticks left over (vidc_line_ticks()) */
    bool display;                       /* inside the display's lines */
    bool flyback;
    uint32_t addr;                      /* the DMA's next screen byte */
    uint32_t dma_ticks;                 /* the video's bus time on a display line */
    uint32_t vidc_frames;
    uint64_t vidc_frame_start;
    uint32_t hz100;
} vidc_t;

extern vidc_t vidc;

void vidc_reset(void);
void vidc_write(uint32_t value);
uint32_t vidc_line_ticks(uint32_t *frac);   /* a line of the current mode, in ticks */
bool vidc_hsync(void);                  /* in the horizontal sync pulse now */
void vidc_line_end(void);               /* the raster's line is over */
void vidc_dma_changed(void);            /* MEMC's video registers changed */
bool vidc_pointer(int *x, int *y);      /* the hardware pointer on the picture */

/* ---- sound.c: the VIDC's sound and MEMC's sound DMA ---- */

void sound_reset(void);
void sound_write_sfr(uint32_t value);
void sound_write_stereo(int channel, uint32_t value);
void sound_run(void);                   /* up to now */
uint32_t sound_byte_rate(void);
extern bool arc_sound_log;              /* host builds: log the sound bytes */
void arc_sound_log_frame(void);

/* ---- floppy.c: the disc images (both controllers') ---- */

#define FLOPPY_MAX_TRACKS   84

typedef struct {
    char path[256];
    uint8_t *data;
    uint32_t size;
    int tracks, sides, spt, ssize, first;
    bool hd;                            /* high density: 500 kbps (ADFS F, DOS 1.44 MB) */
    bool read_only;
    bool changed;                       /* the drive's disc-changed line */
    uint8_t dirty[FLOPPY_MAX_TRACKS * 2 / 8 + 1];
    bool any_dirty;
    int head;                           /* the track its head is on */
    uint32_t reads, writes;
} floppy_t;

extern floppy_t floppies[ARC_FLOPPIES];
extern int floppy_light;                /* the drive being read or written, -1 */

/* A sector's place in the image, -1 if there's none */
long floppy_offset(const floppy_t *d, int track, int side, int sector);
void floppy_mark_dirty(floppy_t *d, int track, int side);

/* ---- c711.c: the A3020's 82C711 (floppy controller, serial, parallel) ---- */

void c711_reset(void);
uint32_t c711_read(uint32_t addr, bool byte);       /* 0x3010000-0x302BFFF, speed bits off */
void c711_write(uint32_t addr, uint32_t value, bool byte);
uint64_t c711_next_event(void);
void c711_event(void);

/* ---- ide.c: its IDE hard disc ---- */

void ide_reset(void);
uint32_t ide_read(uint32_t port, bool byte);
void ide_write(uint32_t port, uint16_t value, bool byte);
uint64_t ide_next_event(void);
void ide_event(void);

/* ---- fdc.c: the A440's WD1772 and latches A and B ---- */

void fdc_reset(void);
uint8_t fdc_read(int reg);
void fdc_write(int reg, uint8_t value);
void fdc_latch_a(uint8_t value);
void fdc_latch_b(uint8_t value);
bool fdc_ready(void);                   /* IOC C2 */
extern bool arc_fdc_log;                /* host builds: log the commands */
uint64_t fdc_next_event(void);
void fdc_event(void);
