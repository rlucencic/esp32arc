/*
 * Acorn Archimedes (A305, A310, A440: ARM2, MEMC1a, VIDC1a, IOC, WD1772),
 * platform independent: the board side is ../../main, a host build for
 * testing is in host/.
 *
 * The machine runs in frames of 20 ms of its own time (160,000 ticks of
 * its 8 MHz clock); the VIDC's frames (50 Hz on a TV, 57-60 on VGA and
 * multisync monitors) run inside them as the VIDC is programmed. The
 * picture is the VIDC's display area as it was at the start of the last
 * frame shown, with the hardware pointer on top: its bytes copied out of
 * RAM by arc_copy_picture() while the frame is shown.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARC_FRAME_HZ        50          /* the frames run_frame() runs */
#define ARC_AUDIO_HZ        31250       /* 1 MHz / 32: the VIDC's sound clock / 32 */
#define ARC_SCREEN_MAX_W    1024
#define ARC_SCREEN_MAX_H    1024
#define ARC_CMOS_SIZE       256         /* the PCF8583: clock (0-15) and RAM */
#define ARC_FLOPPIES        2
#define ARC_ROM_MAX         (2 * 1024 * 1024)

/* The machines */
typedef enum {
    ARC_MODEL_A440,                     /* A305, A310, A440: ARM2 at 8 MHz, WD1772 floppies */
    ARC_MODEL_A3020,                    /* A3020, A4000: ARM250 at 12 MHz, the 82C711 (floppies,
                                           IDE, serial, parallel), the IOEB */
} arc_model_t;

/* The operating systems in ROM */
typedef enum {
    ARC_OS_ARTHUR,                      /* Arthur 1.20, 512 KB */
    ARC_OS_RISCOS2,                     /* RISC OS 2.00, 512 KB */
    ARC_OS_RISCOS3,                     /* RISC OS 3.10, 3.11, 2 MB */
} arc_os_t;

/* What the ROM image is ("RISC OS 3.11 (29 Sep 1992)", into buf), and its
 * OS; NULL if it's none of them
 */
const char *arc_rom_identify(const uint8_t *rom, uint32_t size, arc_os_t *os, char *buf, int len);

/* The CMOS RAM as the OS sets it up itself (Delete-power-on), with two
 * floppy drives (and the A3020's IDE disc)
 */
void arc_cmos_defaults(arc_model_t model, arc_os_t os, uint8_t *cmos);

typedef struct {
    arc_model_t model;
    uint8_t *ram;                       /* ram_size bytes, 4 KB aligned */
    uint32_t ram_size;                  /* 512 KB, 1, 2 or 4 MB */
    const uint8_t *rom;                 /* the OS ROM, 4 KB aligned */
    uint32_t rom_size;                  /* 512 KB (Arthur, RISC OS 2) or 2 MB (RISC OS 3) */
    uintptr_t *page_table;              /* ARM2_PAGES entries (64 KB), internal RAM if it fits */
    uint8_t *cmos;                      /* ARC_CMOS_SIZE bytes, kept by the caller */
    void (*cmos_changed)(void);         /* the program wrote it (from the emulation) */
    void (*picture_ready)(void);        /* a frame's display began: call arc_copy_picture() soon
                                           (before the frame ends; on another core if you like).
                                           NULL: the emulation copies it itself */
    uint16_t cmos_year;                 /* the year now (for RISC OS's two year bytes) */
} arc_config_t;

/* Set up and cold reset (call again to start over with another config) */
bool arc_init(const arc_config_t *cfg);
void arc_reset(void);

/* 20 ms of the machine */
void arc_run_frame(void);

/* ---- The picture ---- */

/* The picture's bytes, copied out of RAM while its frame is shown (a
 * program flipping between two screen buffers draws into the other one,
 * and clears this one when the next frame starts)
 */
void arc_copy_picture(void);

/* The presenter's: arc_screen_signature() takes the last picture copied,
 * which the others then draw
 */
void arc_screen_size(int *width, int *height);
uint32_t arc_screen_signature(void);
void arc_render_line_rgb565(int y, uint16_t *dst);

/* ---- Sound: 16-bit stereo at ARC_AUDIO_HZ ---- */

void arc_audio_enable(bool on);
int arc_audio_render(int16_t *dst, int frames);
int arc_audio_pending(void);

/* ---- Keyboard and mouse ---- */

/* Archimedes key numbers: row << 4 | column (0x00 Escape ... 0x67 keypad
 * Enter; 0x70-0x72 the mouse's Select, Menu, Adjust)
 */
void arc_key(uint8_t key, bool down);
void arc_mouse(int dx, int dy);         /* pixels of the host's mouse; down positive */
void arc_mouse_buttons(uint8_t buttons);/* bit 0 Select, 1 Menu, 2 Adjust */
/* The pointer to a place on the picture (pixels): moved there by the
 * mouse's quadrature counts; buttons as arc_mouse_buttons()
 */
void arc_pointer(int x, int y, uint8_t buttons);
/* Type text on the UK keyboard ('\n' Return); still typing? */
void arc_type(const char *text);
bool arc_typing(void);

/* ---- Floppy disks (.adf, .adl, .img) ---- */

bool arc_floppy_insert(int drive, const char *path);
void arc_floppy_eject(int drive);
const char *arc_floppy_path(int drive);
bool arc_floppy_write_protected(int drive);
/* A blank image of this size (819200: ADFS E, 655360: L, 737280: DOS) */
bool arc_floppy_create(const char *path, uint32_t bytes);
bool arc_floppy_dirty(void);
void arc_floppy_flush(void);

/* ---- The A3020's IDE hard disc (.hdf: raw 512-byte sectors) ---- */

bool arc_hd_insert(const char *path);
void arc_hd_eject(void);
const char *arc_hd_path(void);
uint64_t arc_hd_size(void);
/* A blank disc; formatted (empty, as HForm leaves it) at the sizes
 * arc_hd_blank_size(0, 1, 2) gives (40, 81, 162 cylinders: 20, 40, 80 MB)
 */
bool arc_hd_create(const char *path, uint32_t bytes);
uint32_t arc_hd_blank_size(int i);
bool arc_hd_dirty(void);
void arc_hd_flush(void);
void arc_hd_status(uint32_t *reads, uint32_t *writes, bool *active);

typedef struct {
    int light;                          /* the drive being read or written, -1 */
    uint32_t reads[ARC_FLOPPIES], writes[ARC_FLOPPIES];
    uint8_t track[ARC_FLOPPIES];
} arc_floppy_status_t;

void arc_floppy_status(arc_floppy_status_t *status);

/* A disc's free space and size in bytes, as its filesystem counts them
 * (ADFS old or new map; DOS FAT on floppies): drives 0 and 1, ARC_FLOPPIES
 * the IDE disc. Worked out on the emulation's task once a second (when the
 * disc went in, or a second after its last write); false while it isn't
 * known (no disc, no filesystem known here, not worked out yet).
 */
bool arc_disc_space(int drive, uint64_t *free_bytes, uint64_t *size);

/* ---- State ---- */

typedef struct {
    uint32_t frames;
    uint32_t pc, psr;
    uint32_t insns;                     /* instructions in the last frame */
    int width, height, bpp, hz100;      /* the VIDC's display: pixels, bits, frames a second x 100 */
    int dma_percent;                    /* bus time the video takes */
    bool sound_on;
    uint32_t sample_rate;               /* the VIDC's bytes a second */
} arc_status_t;

void arc_get_status(arc_status_t *status);

/* Memory free for applications, bytes: the application space pages no
 * task has (RISC OS 2 and 3 keep them at protection level 2: the Task
 * Manager's Free and Next together). Arthur gives all of it to the one
 * program running: 0 there.
 */
uint32_t arc_ram_free(void);

/* Memory as the CPU sees it in a privileged mode, no side effects */
uint32_t arc_peek32(uint32_t addr);
uint8_t arc_peek8(uint32_t addr);
