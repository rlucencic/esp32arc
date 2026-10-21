/*
 * The Archimedes: the ARM2's bus (what the page table doesn't map), the
 * frame loop and its events, reset, status and the debugger.
 */
#include <string.h>
#include "arc_internal.h"
#include "emudbg.h"

arc_machine_t arc;

static uint64_t s_frame_start;

/* ---- I/O: 0x3000000-0x33FFFFF ----
 *
 * Bits 16-18 choose a device, 19-20 the cycle's speed, 21 the IOC:
 *   bank 0 (bit 21) the IOC's registers   1 the WD1772   2 Econet
 *   3 the serial port   4 podules   5 latches (printer 10, B 18, A 40)
 *   6 the podules' interrupt backplane
 */

#define IO_TICKS    (arc.tick_mhz / 2)  /* an IOC cycle's wait: 0.5 us */

/* The A3020's IOEB (bank 5): ID, the VIDC's clock, the monitor's ID */
static uint8_t ioeb_read(uint32_t a)
{
    switch (a & 0xF8) {
    case 0x50:
        return 0x05;                    /* an IOEB */
    case 0x70:
        return vidc_hsync() ? 0x0F : 0x0E;      /* a TV-type monitor: ID with the sync on bit 0 */
    default:
        return 0xFF;
    }
}

static void ioeb_write(uint32_t a, uint8_t v)
{
    static const uint16_t khz[4] = { 24000, 25175, 36000, 24000 };
    if ((a & 0xF8) == 0x48) {
        vidc.clock_khz = khz[v & 3];
    }
}

static uint32_t io_read(uint32_t a)
{
    arc.cpu.left -= IO_TICKS;
    if (!(a & ((1u << 16) | (1u << 17) | (1u << 21)))) {
        return 0xFF;                    /* MEMC's podule space */
    }
    bool a3020 = arc.cfg.model == ARC_MODEL_A3020;
    switch ((a >> 16) & 7) {
    case 0:
        return (a & (1u << 21)) ? ioc_read(a) : 0xFF;
    case 1:
        return a3020 ? 0xFF : fdc_read((int)((a >> 2) & 3));
    case 5:
        if (a3020) {
            return ioeb_read(a);
        }
        switch (a & 0xFFFC) {
        case 0x50: return 0;            /* no IOEB */
        default: return 0xFF;
        }
    case 6:
        return 0;                       /* no podule interrupts */
    default:
        return 0xFF;
    }
}

static void io_write(uint32_t a, uint8_t v)
{
    arc.cpu.left -= IO_TICKS;
    if (!(a & ((1u << 16) | (1u << 17) | (1u << 21)))) {
        return;
    }
    bool a3020 = arc.cfg.model == ARC_MODEL_A3020;
    switch ((a >> 16) & 7) {
    case 0:
        if (a & (1u << 21)) {
            ioc_write(a, v);
        }
        break;
    case 1:
        if (!a3020) {
            fdc_write((int)((a >> 2) & 3), v);
        }
        break;
    case 5:
        if (a3020) {
            ioeb_write(a, v);
            break;
        }
        switch (a & 0xFFFC) {
        case 0x18: fdc_latch_b(v); break;
        case 0x40: fdc_latch_a(v); break;
        default: break;
        }
        break;
    default:
        break;
    }
}

/* ---- The bus ---- */

/* The A3020's 82C711 and its DMA acknowledges (any cycle speed) */
static inline bool c711_space(uint32_t addr)
{
    uint32_t a = addr & ~0x180000u;
    return a >= 0x3010000 && a < 0x302C000;
}

static bool user_mode(void)
{
    return (arc.cpu.psr & ARM2_MODE) == ARM2_USR;
}

static uint32_t bus_read(void *ctx, uint32_t addr, bool byte, bool *abort)
{
    (void)ctx;
    if (addr < 0x2000000) {
        *abort = true;                  /* unmapped, or not for this mode */
        return 0;
    }
    if (addr >= 0x3400000) {
        return 0xFFFFFFFF;              /* no low ROM (and the high one is mapped) */
    }
    if (user_mode()) {
        *abort = true;
        return 0;
    }
    if (arc.cfg.model == ARC_MODEL_A3020 && c711_space(addr)) {
        arc.cpu.left -= IO_TICKS;
        return c711_read(addr & ~0x180000u, byte);
    }
    if (addr < 0x3000000) {
        /* physical RAM, the first time after a reset */
        memc_latch_off();
        const uint8_t *p = memc_phys(addr - 0x2000000);
        uint32_t v;
        memcpy(&v, (const uint8_t *)((uintptr_t)p & ~(uintptr_t)3), 4);
        return byte ? *p : v;
    }
    return io_read(addr);
}

static void bus_write(void *ctx, uint32_t addr, uint32_t v, bool byte, bool *abort)
{
    (void)ctx;
    if (addr < 0x2000000) {
        if (!memc.rom_latch) {
            *abort = true;
        }
        return;                         /* while the ROM is at 0, writes go nowhere */
    }
    if (user_mode()) {
        *abort = true;
        return;
    }
    if (addr < 0x3000000) {
        memc_latch_off();
        uint8_t *p = memc_phys(addr - 0x2000000);
        if (byte) {
            *p = (uint8_t)v;
        } else {
            memcpy((uint8_t *)((uintptr_t)p & ~(uintptr_t)3), &v, 4);
        }
        return;
    }
    if (arc.cfg.model == ARC_MODEL_A3020 && c711_space(addr)) {
        arc.cpu.left -= IO_TICKS;
        c711_write(addr & ~0x180000u, v >> 16, byte);   /* written data on bits 16-31 */
    } else if (addr < 0x3400000) {
        io_write(addr, (uint8_t)(v >> 16));     /* the IOC's data is bits 16-23 */
    } else if (addr < 0x3600000) {
        vidc_write(v);
    } else if (addr < 0x3800000) {
        memc_write_register(addr);
    } else {
        memc_write_cam(addr);
    }
}

/* ---- Time ---- */

/* The next event but the raster's: never later than it is (arc_schedule()
 * lowers it; it's worked out again after the events, and each frame)
 */
static uint64_t s_other;

void arc_schedule(uint64_t t)
{
    if (t < s_other) {
        s_other = t;
    }
    if (t < arc.run_until) {
        arc.run_until = t;
        arm2_end_run(&arc.cpu);
    }
}

static uint64_t other_events(void)
{
    uint64_t t = ioc_next_event();
    uint64_t e = keyboard_next_event();
    if (e < t) t = e;
    e = arc.cfg.model == ARC_MODEL_A3020 ? c711_next_event() : fdc_next_event();
    if (e < t) t = e;
    return t;
}


void arc_run_frame(void)
{
    uint64_t frame_end = s_frame_start + FRAME_TICKS;
    uint32_t insns = arc.cpu.insns;
    s_other = other_events();
    for (;;) {
        uint64_t now = arc_now();
        if (now >= frame_end) {
            break;
        }
        uint64_t until = vidc.stop_at < s_other ? vidc.stop_at : s_other;
        if (until > frame_end) {
            until = frame_end;
        }
        if (until > now) {
            arc.run_until = until;
            arm2_run(&arc.cpu, (int32_t)(until - now));
            arc.run_until = NEVER;
            now = arc_now();
        }
        if (vidc.line_end <= now) {
            do {
                vidc_line_end();
                now = arc_now();
            } while (vidc.line_end <= now);
            sound_run();
        }
        if (s_other > now) {
            continue;
        }
        if (ioc_next_event() <= now) {
            ioc_event();
        }
        if (keyboard_next_event() <= now) {
            keyboard_event();
        }
        if (arc.cfg.model == ARC_MODEL_A3020) {
            if (c711_next_event() <= now) {
                c711_event();
            }
        } else if (fdc_next_event() <= now) {
            fdc_event();
        }
        s_other = other_events();
    }
    sound_run();
    s_frame_start = frame_end;
    arc.insns_last = arc.cpu.insns - insns;
    arc.frames++;
    space_frame();
}

/* ---- The debugger ---- */

#if EMUDBG_ENABLED
static emudbg_cpu_t s_dbg_cpu;

static void dbg_where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %u, VIDC line %d", (unsigned)arc.frames, vidc.line);
}

static void dbg_screen(int *w, int *h)
{
    arc_screen_size(w, h);
}

static const emudbg_machine_t s_dbg_machine = {
    .name = "Acorn Archimedes",
    .cpus = &s_dbg_cpu,
    .cpu_count = 1,
    .where = dbg_where,
    .reset = arc_reset,
    .screen = dbg_screen,
    .render_line = arc_render_line_rgb565,
    .type = arc_type,
};
#endif

/* ---- Starting ---- */

void arc_reset(void)
{
    memc_reset();
    ioc_reset();
    vidc_reset();
    sound_reset();
    cmos_reset();
    fdc_reset();
    if (arc.cfg.model == ARC_MODEL_A3020) {
        c711_reset();
    }
    arm2_reset(&arc.cpu);
    keyboard_reset();
    s_frame_start = arc_now();
    arc.run_until = NEVER;
    emudbg_rearm();
}

bool arc_init(const arc_config_t *cfg)
{
    if (!cfg->ram || !cfg->rom || !cfg->page_table || !cfg->cmos) {
        return false;
    }
    if ((cfg->ram_size & (cfg->ram_size - 1)) || (cfg->rom_size & (cfg->rom_size - 1))) {
        return false;                   /* powers of two: they wrap by masks */
    }
    arc.cfg = *cfg;
    arc.frames = 0;
    arc.tick_mhz = cfg->model == ARC_MODEL_A3020 ? 12 : 8;
    arm2_bus_t bus = { .read = bus_read, .write = bus_write, .ctx = NULL };
    arc.cpu.swp = cfg->model == ARC_MODEL_A3020;     /* the ARM250's SWP */
    arm2_init(&arc.cpu, &bus, cfg->page_table);
    memset(cfg->ram, 0, cfg->ram_size);
    arc_reset();
#if EMUDBG_ENABLED
    s_dbg_cpu = (emudbg_cpu_t){ .name = "ARM2", .arch = &emudbg_arch_arm2, .core = &arc.cpu };
    emudbg_set_machine(&s_dbg_machine);
#endif
    return true;
}

/* ---- State ---- */

void arc_get_status(arc_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->frames = arc.frames;
    s->pc = arc.cpu.pc;
    s->psr = arc.cpu.psr;
    s->insns = arc.insns_last;
    arc_screen_size(&s->width, &s->height);
    s->bpp = 1 << vidc.bpp_log;
    s->hz100 = (int)vidc.hz100;
    s->sound_on = memc.sound_dma;
    s->sample_rate = sound_byte_rate();
    uint32_t frac = 0;
    uint32_t line = vidc_line_ticks(&frac);
    s->dma_percent = line ? (int)(vidc.dma_ticks * 100 / line) : 0;
}

uint32_t arc_peek32(uint32_t addr)
{
    return arm2_peek32(&arc.cpu, addr);
}

uint8_t arc_peek8(uint32_t addr)
{
    return arm2_peek8(&arc.cpu, addr);
}
