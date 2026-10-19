/*
 * Acorn Archimedes emulator on the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Archimedes into it: memory, ROM, settings, CMOS RAM, keyboard map, mouse
 * and touch, floppy discs, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "arc.h"
#include "arm2.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static arc_config_t s_cfg;
static uint8_t *s_rom_copy;
static const char *s_rom_name;
static uint8_t s_cmos[ARC_CMOS_SIZE];
static int s_cmos_model = -1, s_cmos_os = -1;   /* whose CMOS RAM s_cmos holds */
static volatile bool s_cmos_dirty;

/* ---- Memory ----
 *
 * The biggest RAM (4 MB: the smaller sizes use the start of it), the ROM
 * copy and the ARM2's page table (64 KB, in internal RAM if it fits: every
 * access of the CPU looks there), first thing at power-on.
 */
#define RAM_MAX         (4 * 1024 * 1024)

static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_aligned_alloc(ARM2_PAGE_SIZE, RAM_MAX, MALLOC_CAP_SPIRAM);
    s_rom_copy = heap_caps_aligned_alloc(ARM2_PAGE_SIZE, ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    size_t table = ARM2_PAGES * sizeof(uintptr_t);
    s_cfg.page_table = heap_caps_malloc(table, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_cfg.page_table == NULL) {
        s_cfg.page_table = heap_caps_malloc(table, MALLOC_CAP_SPIRAM);
    }
    ESP_LOGI(TAG, "RAM %s, ROM %s, page table in %s RAM", s_cfg.ram ? "ok" : "MISSING",
             s_rom_copy ? "ok" : "MISSING",
             esp_ptr_internal(s_cfg.page_table) ? "internal" : "PSRAM");
}

/* ---- CMOS RAM: each model and OS keeps its own, saved from a core 0 task ---- */

static void cmos_changed(void)
{
    s_cmos_dirty = true;
}

static void save_cmos(void)
{
    if (s_cmos_dirty && s_cmos_os >= 0) {
        s_cmos_dirty = false;
        uint8_t copy[ARC_CMOS_SIZE];
        memcpy(copy, s_cmos, sizeof(copy));
        settings_save_cmos(s_cmos_model, s_cmos_os, copy);
    }
}

static void load_cmos(void)
{
    if (s_cmos_os != settings.os || s_cmos_model != settings.model) {
        save_cmos();
        s_cmos_model = settings.model;
        s_cmos_os = settings.os;
        if (!settings_load_cmos(settings.model, settings.os, s_cmos)) {
            arc_cmos_defaults(settings.model, settings.os, s_cmos);
            s_cmos_dirty = true;
        }
    }
}

/* ---- The picture: each frame's bytes copied out of RAM on core 0 ----
 *
 * From the start of the frame's display, before the program (flipping
 * between two screen buffers) clears it for the frame after next: above
 * the presenter, which draws the copies.
 */
static TaskHandle_t s_copier;

static void copier_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        arc_copy_picture();
    }
}

static void picture_ready(void)
{
    xTaskNotifyGive(s_copier);
}

/* ---- Discs: changed floppy tracks and hard disc sectors go to the card ---- */

static void flush(void *arg)
{
    (void)arg;
    arc_floppy_flush();
    arc_hd_flush();
}

static void every_second(void)
{
    save_cmos();
    static int dirty_for;
    if (arc_floppy_dirty() || arc_hd_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Starting ---- */

static const char *const os_names[] = { "Arthur 1.20", "RISC OS 2.00", "RISC OS 3.11", "" };

static bool start(void)
{
    uint32_t rom_size;
    if (s_rom_copy == NULL || s_cfg.ram == NULL || s_cfg.page_table == NULL) {
        emu_set_message("Not enough memory for the Archimedes.");
        return false;
    }
    if (!rom_store_load(settings.os, s_rom_copy, &rom_size, &s_rom_name)) {
        static char text[200];
        static const char *const files[] = { "arthur120.rom", "riscos200.rom", "riscos311.rom" };
        snprintf(text, sizeof(text),
                 "No %s ROM: put it in roms/%s and flash with idf.py flash, or copy it into the "
                 "\"roms\" folder on the SD card.", os_names[settings.os], files[settings.os]);
        emu_set_message(text);
        return false;
    }
    load_cmos();
    s_cfg.model = settings.model;
    s_cfg.ram_size = settings.ram_kb * 1024u;
    s_cfg.rom = s_rom_copy;
    s_cfg.rom_size = rom_size;
    s_cfg.cmos = s_cmos;
    s_cfg.cmos_changed = cmos_changed;
    if (s_copier == NULL) {
        xTaskCreatePinnedToCore(copier_task, "picture", 3072, NULL, 6, &s_copier, 0);
    }
    s_cfg.picture_ready = s_copier ? picture_ready : NULL;
    /* the year for RISC OS's own two bytes, once the clock was set */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    s_cfg.cmos_year = tm.tm_year + 1900 >= 2024 ? (uint16_t)(tm.tm_year + 1900) : 0;
    if (!arc_init(&s_cfg)) {
        emu_set_message("The Archimedes could not be set up: see the log.");
        return false;
    }
    ESP_LOGI(TAG, "%s, %s, %u KB", settings.model == ARC_MODEL_A3020 ? "A3020" : "A440", s_rom_name,
             (unsigned)settings.ram_kb);
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    arc_screen_size(&mode->width, &mode->height);
    mode->hz = ARC_FRAME_HZ;
}

/* ---- Keyboard, mouse, touch ---- */

/* HID keyboard usages 0x00-0x67 to Archimedes key numbers (row << 4 |
 * column, UK positions); K() marks an entry (key 0 is Escape)
 */
#define K(code)     ((code) | 0x80)
static const uint8_t usage_to_arc[0x68] = {
    [0x04] = K(0x3C), [0x05] = K(0x52), [0x06] = K(0x50), [0x07] = K(0x3E),     /* a b c d */
    [0x08] = K(0x29), [0x09] = K(0x3F), [0x0A] = K(0x40), [0x0B] = K(0x41),     /* e f g h */
    [0x0C] = K(0x2E), [0x0D] = K(0x42), [0x0E] = K(0x43), [0x0F] = K(0x44),     /* i j k l */
    [0x10] = K(0x54), [0x11] = K(0x53), [0x12] = K(0x2F), [0x13] = K(0x30),     /* m n o p */
    [0x14] = K(0x27), [0x15] = K(0x2A), [0x16] = K(0x3D), [0x17] = K(0x2B),     /* q r s t */
    [0x18] = K(0x2D), [0x19] = K(0x51), [0x1A] = K(0x28), [0x1B] = K(0x4F),     /* u v w x */
    [0x1C] = K(0x2C), [0x1D] = K(0x4E),                                         /* y z */
    [0x1E] = K(0x11), [0x1F] = K(0x12), [0x20] = K(0x13), [0x21] = K(0x14),     /* 1 2 3 4 */
    [0x22] = K(0x15), [0x23] = K(0x16), [0x24] = K(0x17), [0x25] = K(0x18),     /* 5 6 7 8 */
    [0x26] = K(0x19), [0x27] = K(0x1A),                                         /* 9 0 */
    [0x28] = K(0x47),      /* Return */
    [0x29] = K(0x00),      /* Escape */
    [0x2A] = K(0x1E),      /* Backspace */
    [0x2B] = K(0x26),      /* Tab */
    [0x2C] = K(0x5F),      /* Space */
    [0x2D] = K(0x1B),      /* - */
    [0x2E] = K(0x1C),      /* = */
    [0x2F] = K(0x31),      /* [ */
    [0x30] = K(0x32),      /* ] */
    [0x31] = K(0x33),      /* \ */
    [0x32] = K(0x33),      /* non-US # */
    [0x33] = K(0x45),      /* ; */
    [0x34] = K(0x46),      /* ' */
    [0x35] = K(0x10),      /* ` */
    [0x36] = K(0x55),      /* , */
    [0x37] = K(0x56),      /* . */
    [0x38] = K(0x57),      /* / */
    [0x39] = K(0x5D),      /* Caps Lock */
    [0x3A] = K(0x01), [0x3B] = K(0x02), [0x3C] = K(0x03), [0x3D] = K(0x04),     /* F1-F4 */
    [0x3E] = K(0x05), [0x3F] = K(0x06), [0x40] = K(0x07), [0x41] = K(0x08),     /* F5-F8 */
    [0x42] = K(0x09), [0x43] = K(0x0A), [0x44] = K(0x0B), [0x45] = K(0x0C),     /* F9-F12 */
    [0x46] = K(0x0D),      /* Print */
    [0x47] = K(0x0E),      /* Scroll Lock */
    [0x48] = K(0x0F),      /* Pause -> Break */
    [0x49] = K(0x1F),      /* Insert */
    [0x4A] = K(0x20),      /* Home */
    [0x4B] = K(0x21),      /* Page Up */
    [0x4C] = K(0x34),      /* Delete */
    [0x4D] = K(0x35),      /* End -> Copy */
    [0x4E] = K(0x36),      /* Page Down */
    [0x4F] = K(0x64),      /* Right */
    [0x50] = K(0x62),      /* Left */
    [0x51] = K(0x63),      /* Down */
    [0x52] = K(0x59),      /* Up */
    [0x53] = K(0x22),      /* Num Lock */
    [0x54] = K(0x23),      /* keypad / */
    [0x55] = K(0x24),      /* keypad * */
    [0x56] = K(0x3A),      /* keypad - */
    [0x57] = K(0x4B),      /* keypad + */
    [0x58] = K(0x67),      /* keypad Enter */
    [0x59] = K(0x5A), [0x5A] = K(0x5B), [0x5B] = K(0x5C),                       /* keypad 1 2 3 */
    [0x5C] = K(0x48), [0x5D] = K(0x49), [0x5E] = K(0x4A),                       /* keypad 4 5 6 */
    [0x5F] = K(0x37), [0x60] = K(0x38), [0x61] = K(0x39),                       /* keypad 7 8 9 */
    [0x62] = K(0x65),      /* keypad 0 */
    [0x63] = K(0x66),      /* keypad . */
    [0x64] = K(0x33),      /* non-US \ (left of Z) */
    [0x67] = K(0x25),      /* keypad = -> keypad # */
};

/* Modifier usages $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI
 * (the Archimedes has no GUI keys: they're Alt)
 */
static const uint8_t modifier_to_arc[8] = { 0x3B, 0x4C, 0x5E, 0x5E, 0x61, 0x58, 0x60, 0x60 };

static uint8_t s_modifiers;

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        uint8_t code = modifier_to_arc[bit];
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) {
            mask |= modifier_to_arc[i] == code ? 1u << i : 0;
        }
        bool was = (s_modifiers & mask) != 0;
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        bool now = (s_modifiers & mask) != 0;
        if (was != now) {
            arc_key(code, now);
        }
        return;
    }
    if (usage < sizeof(usage_to_arc) && usage_to_arc[usage]) {
        arc_key(usage_to_arc[usage] & 0x7F, pressed);
    }
}

/* HID buttons (bit 0 left, 1 right, 2 middle) to Select, Menu, Adjust:
 * Menu on the right button (two-button mice), or the middle one as on
 * Acorn's mouse
 */
static uint8_t arc_buttons(uint8_t hid)
{
    uint8_t b = hid & 1;
    if (settings.menu_middle) {
        b |= (hid & 4) ? 2 : 0;
        b |= (hid & 2) ? 4 : 0;
    } else {
        b |= hid & 6;
    }
    return b;
}

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    arc_mouse(dx, dy);
    arc_mouse_buttons(arc_buttons(buttons));
}

/* A finger on the picture: the pointer goes there. A tap clicks Select,
 * holding still for half a second clicks Menu, moving while down drags
 * with Select (pressed where the finger went down)
 */
#define TOUCH_MENU_US   500000
#define TOUCH_SLOP      6               /* screen pixels before it's a drag */

static void touch(int x, int y, bool pressed)
{
    static enum { IDLE, PENDING, DRAG, DONE } state;
    static int x0, y0;
    static int64_t t0;
    int64_t now = esp_timer_get_time();
    if (!pressed) {
        if (state == PENDING) {
            arc_pointer(x0, y0, 1);
            arc_pointer(x0, y0, 0);
        } else if (state == DRAG) {
            arc_pointer(x, y, 0);
        }
        state = IDLE;
        return;
    }
    switch (state) {
    case IDLE:
        x0 = x;
        y0 = y;
        t0 = now;
        arc_pointer(x, y, 0);
        state = PENDING;
        break;
    case PENDING:
        if (abs(x - x0) > TOUCH_SLOP || abs(y - y0) > TOUCH_SLOP) {
            arc_pointer(x0, y0, 1);
            arc_pointer(x, y, 1);
            state = DRAG;
        } else if (now - t0 >= TOUCH_MENU_US) {
            arc_pointer(x0, y0, 2);
            arc_pointer(x0, y0, 0);
            state = DONE;
        }
        break;
    case DRAG:
        arc_pointer(x, y, 1);
        break;
    case DONE:
        break;
    }
}

/* ---- Setup screen choices ---- */

enum { CHOICE_MODEL, CHOICE_OS, CHOICE_MEMORY, CHOICE_MENU, CHOICES };

static const char *const model_names[] = { "A440\nARM2, 8 MHz", "A3020 / A4000\nARM250, 12 MHz", "" };
static const char *const os_names_a3020[] = { "RISC OS 3.11", "" };
static const uint16_t ram_sizes_a440[] = { 512, 1024, 2048, 4096 };
static const char *const ram_names_a440[] = { "512 KB", "1 MB", "2 MB", "4 MB", "" };
static const uint16_t ram_sizes_a3020[] = { 2048, 4096 };
static const char *const ram_names_a3020[] = { "2 MB", "4 MB", "" };
static const char *const menu_names[] = { "Right", "Middle", "" };

static bool a3020(void)
{
    return settings.model == ARC_MODEL_A3020;
}

static int model_get(void)
{
    return settings.model;
}

/* The A3020 runs RISC OS 3 with 2 or 4 MB */
static void model_set(int i)
{
    if (i == ARC_MODEL_A440 || i == ARC_MODEL_A3020) {
        settings.model = i;
        if (i == ARC_MODEL_A3020) {
            settings.os = ARC_OS_RISCOS3;
            if (settings.ram_kb < 2048) {
                settings.ram_kb = 2048;
            }
        }
        settings_save();
    }
}

static int os_get(void)
{
    return a3020() ? 0 : settings.os;
}

static void os_set(int i)
{
    if (a3020()) {
        settings.os = ARC_OS_RISCOS3;
    } else if (i >= ARC_OS_ARTHUR && i <= ARC_OS_RISCOS3) {
        settings.os = i;
    }
    settings_save();
}

static const uint16_t *ram_sizes(int *n)
{
    *n = a3020() ? 2 : 4;
    return a3020() ? ram_sizes_a3020 : ram_sizes_a440;
}

static int ram_get(void)
{
    int n;
    const uint16_t *sizes = ram_sizes(&n);
    for (int i = 0; i < n; i++) {
        if (sizes[i] == settings.ram_kb) {
            return i;
        }
    }
    return n - 1;
}

static void ram_set(int i)
{
    int n;
    const uint16_t *sizes = ram_sizes(&n);
    if (i >= 0 && i < n) {
        settings.ram_kb = sizes[i];
        settings_save();
    }
}

static int menu_get(void)
{
    return settings.menu_middle;
}

static void menu_set(int i)
{
    if (i == 0 || i == 1) {
        settings.menu_middle = i;
        settings_save();
    }
}

static const emu_choice_t choices[CHOICES] = {
    { "Model", model_names, model_get, model_set, false },
    { "System", os_names, os_get, os_set, false },
    { "Memory", ram_names_a440, ram_get, ram_set, false },
    { "Menu button", menu_names, menu_get, menu_set, true },
};

/* The System and Memory rows follow the model */
static const char *const *choice_names(int i)
{
    if (i == CHOICE_OS) {
        return a3020() ? os_names_a3020 : os_names;
    }
    if (i == CHOICE_MEMORY) {
        return a3020() ? ram_names_a3020 : ram_names_a440;
    }
    return NULL;
}

/* ---- Discs: two floppy drives and the A3020's IDE disc ---- */

#define DRIVE_IDE       ARC_FLOPPIES
#define DRIVES          (ARC_FLOPPIES + 1)

static const char *const extensions[] = { ".adf", ".adl", ".img", ".hdf", NULL };
static const char *const drive_names[DRIVES] = { ":0", ":1", "IDE disc" };
static const disks_size_t sizes[] = {
    { 819200, "800 KB floppy (E)" },
    { 40 * 516096, "20 MB hard disc" },     /* cylinders of 16 heads x 63 sectors */
    { 81 * 516096, "40 MB hard disc" },
    { 162 * 516096, "80 MB hard disc" },
};

static bool hd_name(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".hdf");
}

static bool disc_accepts(int drive, const char *name)
{
    return (drive == DRIVE_IDE) == hd_name(name);
}

static uint32_t disc_default_size(int drive)
{
    return drive == DRIVE_IDE ? 81 * 516096 : 819200;
}

static const char *disc_extension_for(uint32_t bytes)
{
    return bytes > 2 * 1024 * 1024 ? ".hdf" : ".adf";
}

static bool disc_insert(int drive, const char *path)
{
    return drive == DRIVE_IDE ? arc_hd_insert(path) : arc_floppy_insert(drive, path);
}

static void disc_eject(int drive)
{
    if (drive == DRIVE_IDE) {
        arc_hd_eject();
    } else {
        arc_floppy_eject(drive);
    }
}

static const char *disc_path(int drive)
{
    return drive == DRIVE_IDE ? arc_hd_path() : arc_floppy_path(drive);
}

static bool disc_write_protected(int drive)
{
    return drive == DRIVE_IDE ? false : arc_floppy_write_protected(drive);
}

static bool disc_create(const char *path, uint32_t bytes)
{
    return bytes > 2 * 1024 * 1024 ? arc_hd_create(path, bytes) : arc_floppy_create(path, bytes);
}

static const disks_config_t disks = {
    .folder = "floppies",
    .extensions = extensions,
    .description = "Disc images (.ADF, .ADL, .IMG; .HDF the A3020's hard disc)",
    .title = "Discs",
    .button = "Discs",
    .nvs_namespace = "arcfloppy",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".adf",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 512ull * 1024 * 1024,
    .insert = disc_insert,
    .eject = disc_eject,
    .path = disc_path,
    .write_protected = disc_write_protected,
    .create = disc_create,
    .accepts = disc_accepts,
    .default_size = disc_default_size,
    .extension_for = disc_extension_for,
};

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        arc_status_t s;
        arc_get_status(&s);
        bool arm250 = s_cfg.model == ARC_MODEL_A3020;
        const char *model = arm250 ? "Acorn A3020 / A4000" : "Acorn Archimedes A440";
        if (s_cfg.ram_size >= 1024 * 1024) {
            snprintf(text, sizeof(text), "%s, %u MB", model, (unsigned)(s_cfg.ram_size >> 20));
        } else {
            snprintf(text, sizeof(text), "%s, %u KB", model, (unsigned)(s_cfg.ram_size >> 10));
        }
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROM", s_rom_name ? s_rom_name : "-");
        snprintf(text, sizeof(text), "%s <span class=\"dim\">(%.2f million instructions a second)</span>",
                 arm250 ? "ARM250 at 12 MHz" : "ARM2 at 8 MHz", s.insns * (double)ARC_FRAME_HZ / 1e6);
        emu_json_row(j, "CPU", text);
        snprintf(text, sizeof(text), "%d × %d, %d colours, %d.%02d Hz <span class=\"dim\">(video %d%% of the bus)</span>",
                 s.width, s.height, 1 << s.bpp, s.hz100 / 100, s.hz100 % 100, s.dma_percent);
        emu_json_row(j, "Screen", text);
        if (settings.os != ARC_OS_ARTHUR && s_cfg.ram_size) {
            uint32_t free = arc_ram_free();
            char total[16];
            if (s_cfg.ram_size >= 1024 * 1024) {
                snprintf(total, sizeof(total), "%u MB", (unsigned)(s_cfg.ram_size >> 20));
            } else {
                snprintf(total, sizeof(total), "%u KB", (unsigned)(s_cfg.ram_size >> 10));
            }
            snprintf(text, sizeof(text), "%u KB of %s <span class=\"dim\">(for applications: the Task "
                     "Manager's Free and Next)</span>", (unsigned)(free >> 10), total);
            emu_json_row_bar(j, "Free RAM", text, 100 - (int)((uint64_t)free * 100 / s_cfg.ram_size));
        }
        if (s.sound_on) {
            snprintf(text, sizeof(text), "on, %u bytes a second", (unsigned)s.sample_rate);
        } else {
            snprintf(text, sizeof(text), "off");
        }
        emu_json_row(j, "Sound", text);
        return;
    }
    arc_floppy_status_t fs;
    arc_floppy_status(&fs);
    for (int d = 0; d < ARC_FLOPPIES; d++) {
        const char *name = disks_name(d);
        snprintf(text, sizeof(text), "track %u", (unsigned)fs.track[d]);
        emu_json_drive(j, drive_names[d], name, arc_floppy_write_protected(d), fs.light == d, fs.reads[d],
                       fs.writes[d], name ? text : NULL);
    }
    {
        const char *name = disks_name(DRIVE_IDE);
        uint32_t reads, writes;
        bool active;
        arc_hd_status(&reads, &writes, &active);
        snprintf(text, sizeof(text), "%u MB%s", (unsigned)(arc_hd_size() >> 20),
                 s_cfg.model == ARC_MODEL_A3020 ? "" : " <span class=\"dim\">(the A3020's)</span>");
        emu_json_drive(j, drive_names[DRIVE_IDE], name, false, active, reads, writes, name ? text : NULL);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    arc_status_t s;
    arc_get_status(&s);
    static const char *const modes[4] = { "USR", "FIQ", "IRQ", "SVC" };
    snprintf(buf, len, "frames %lu PC &%07lX %s insns %lu screen %dx%d %d bpp", (unsigned long)s.frames,
             (unsigned long)s.pc, modes[s.psr & 3], (unsigned long)s.insns, s.width, s.height, s.bpp);
}

/*   k <hex>            tap Archimedes key <hex> (e.g. 47 Return)
 *   K <hex> <0|1>      key down (1) or up (0)
 *   m <dx> <dy> <btn>  move the mouse; buttons 1 Select, 2 Menu, 4 Adjust
 *   P <x> <y> <btn>    the pointer to screen position x, y with those buttons
 *   T <text>           type text (\n Return)
 *   R <os> <KB> [model]  restart with OS 0 Arthur 1.20, 1 RISC OS 2.00, 2 RISC
 *                      OS 3.11, that RAM (512, 1024, 2048, 4096), as model 0 the
 *                      A440 or 1 the A3020 (RISC OS 3.11, 2048 or 4096)
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned code = strtoul(line + 1, NULL, 16);
        arc_key(code, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        arc_key(code, false);
        return true;
    }
    case 'K': {
        unsigned code = 0, down = 0;
        if (sscanf(line + 1, "%x %u", &code, &down) == 2) {
            arc_key(code, down);
        }
        return true;
    }
    case 'm': {
        int dx = 0, dy = 0, buttons = 0;
        sscanf(line + 1, "%d %d %d", &dx, &dy, &buttons);
        arc_mouse(dx, dy);
        arc_mouse_buttons(buttons);
        return true;
    }
    case 'P': {
        int x = 0, y = 0, buttons = 0;
        sscanf(line + 1, "%d %d %d", &x, &y, &buttons);
        arc_pointer(x, y, buttons);
        return true;
    }
    case 'T': {
        static char text[256];
        const char *p = line + 1;
        while (*p == ' ') {
            p++;
        }
        int n = 0;
        for (; *p && n < (int)sizeof(text) - 1; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                text[n++] = '\n';
                p++;
            } else {
                text[n++] = *p;
            }
        }
        text[n] = 0;
        arc_type(text);
        return true;
    }
    case 'R': {
        int os = 0, kb = 0, model = 0;
        int n = sscanf(line + 1, "%d %d %d", &os, &kb, &model);
        if (n >= 2 && os >= ARC_OS_ARTHUR && os <= ARC_OS_RISCOS3 && model >= 0 && model <= ARC_MODEL_A3020 &&
            (model == ARC_MODEL_A440 || (os == ARC_OS_RISCOS3 && kb >= 2048))) {
            for (int i = 0; i < 4; i++) {
                if (ram_sizes_a440[i] == kb) {
                    settings.model = model;
                    settings.os = os;
                    settings.ram_kb = kb;
                    settings_save();
                    emu_restart();
                    break;
                }
            }
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Acorn Archimedes",
    .short_name = "Archimedes",
    .view_width = 800,          /* 4:3 for every mode (640 x 256 TV ones too): 960 x 720 on the Tab5 */
    .scale = EMU_SCALE_SMOOTH,
    .reserve_memory = reserve_memory,
    .start = start,
    .reset = arc_reset,
    .run_frame = arc_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = arc_screen_signature,
    .render_line = arc_render_line_rgb565,
    .audio_hz = ARC_AUDIO_HZ,
    .audio_enable = arc_audio_enable,
    .audio_render = arc_audio_render,
    .audio_pending = arc_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touch = touch,
    .choices = choices,
    .choice_count = CHOICES,
    .choice_names = choice_names,
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
