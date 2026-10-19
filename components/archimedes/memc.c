/*
 * The MEMC1a: the logical-to-physical page map (its CAM), the protection
 * levels, the DMA address registers and the control register, as the
 * ARM2's page table (4 KB entries, arm2.h).
 *
 *   0x0000000-0x1FFFFFF  logical RAM, through the CAM; unmapped pages abort
 *   0x2000000-0x2FFFFFF  physical RAM (as the RAM fitted is wired: memc_ram_offset()),
 *                        privileged
 *   0x3000000-0x33FFFFF  I/O (the IOC and what hangs off it), privileged
 *   0x3400000-0x37FFFFF  reads: the low ROM (none here); writes: the VIDC
 *                        (0x3400000-) and the MEMC's registers (0x3600000-)
 *   0x3800000-0x3FFFFFF  reads: the OS ROM; writes: the CAM
 *
 * After a reset the ROM also shows at 0 until the program first uses
 * physical RAM or writes the CAM (MAME's acorn_memc): the reset vector
 * runs from ROM.
 *
 * A CAM write's address encodes the logical page, the physical page and
 * the protection level; how depends on the page size (4, 8, 16 or 32 KB):
 *   4K   11 1LLL LLLL LLLL LLAA xPPP PPPP   (L bits 11-10 the logical page's top)
 *   8K   11 1LLL LLLL LLLx LLAA xPPP PPPP   physical bit 6 from address bit 0
 *   16K  11 1LLL LLLL LLxx LLAA xPPP PPPP   bits 6-5 from 1-0
 *   32K  11 1LLL LLLL Lxxx LLAA xPPP PPPP   bit 4 from 0, 6 from 1, 5 from 2
 * Level 0: user read/write; 1: user read only; 2, 3: no user access (in OS
 * mode user reads them, and writes level 1). The privileged modes reach
 * everything mapped.
 */
#include <string.h>
#include "arc_internal.h"

memc_t memc;
bool arc_memc_log;

#define LOGICAL_PAGES   8192            /* 4 KB pages below 0x2000000 */
#define RW_PRIV         (ARM2_PRIV_R | ARM2_PRIV_W)
#define ROM_BITS        (ARM2_USER_R | ARM2_PRIV_R | ARM2_SLOW)

static uintptr_t *page(void)
{
    return arc.cfg.page_table;
}

static uintptr_t rom_entry(uint32_t offset)
{
    return (uintptr_t)(arc.cfg.rom + (offset & (arc.cfg.rom_size - 1))) | ROM_BITS;
}

static bool watched(uint32_t phys)
{
    return phys < memc.watch_hi && phys + ARM2_PAGE_SIZE > memc.watch_lo;
}

static uintptr_t ram_entry(uint32_t phys, uint32_t bits)
{
    return (uintptr_t)memc_phys(phys) | bits | (watched(phys) ? ARM2_WATCH : 0);
}

/* What reaches the RAM, for its size and the page size (memc_ram_offset()) */
static void set_ram_wiring(void)
{
    uint32_t size = arc.cfg.ram_size;
    memc.scramble = 0;
    memc.ram_mask = size - 1;
    if (size < 2 * 1024 * 1024) {
        if (memc.psize == 0) {
            memc.ram_mask = 0x7FFFF;    /* 4 KB pages reach 512 KB */
        } else if (size <= 512 * 1024) {
            memc.scramble = memc.psize == 3 ? 2 : 1;
        } else if (memc.psize == 3) {
            memc.scramble = 3;
        }
    }
}

void memc_copy(uint8_t *dst, uint32_t phys, uint32_t n)
{
    while (n) {
        uint32_t chunk = ARM2_PAGE_SIZE - (phys & (ARM2_PAGE_SIZE - 1));
        if (chunk > n) {
            chunk = n;
        }
        memcpy(dst, memc_phys(phys), chunk);
        dst += chunk;
        phys += chunk;
        n -= chunk;
    }
}

static uint32_t user_bits(int ppl)
{
    switch (ppl) {
    case 0: return ARM2_USER_R | ARM2_USER_W;
    case 1: return ARM2_USER_R | (memc.os_mode ? ARM2_USER_W : 0);
    default: return memc.os_mode ? ARM2_USER_R : 0;
    }
}

/* The 4 KB entries of logical page lp (of the current size) */
static void build_logical(int lp)
{
    int per = 1 << memc.psize;
    int first = lp << memc.psize;
    uintptr_t *pt = page();
    if (memc.rom_latch) {
        for (int i = first; i < first + per; i++) {
            pt[i] = rom_entry((uint32_t)i << ARM2_PAGE_SHIFT);
        }
        return;
    }
    int pp = memc.log_to_phys[lp];
    if (pp < 0) {
        for (int i = first; i < first + per; i++) {
            pt[i] = 0;
        }
        return;
    }
    uint32_t base = (uint32_t)pp * (ARM2_PAGE_SIZE << memc.psize);
    uint32_t bits = RW_PRIV | user_bits(memc.phys_ppl[pp]);
    for (int i = 0; i < per; i++) {
        pt[first + i] = ram_entry(base + (uint32_t)i * ARM2_PAGE_SIZE, bits);
    }
}

void memc_build(void)
{
    uintptr_t *pt = page();
    for (int lp = 0; lp < (LOGICAL_PAGES >> memc.psize); lp++) {
        build_logical(lp);
    }
    /* Physical RAM: through the bus while the ROM latch is on (that turns it off) */
    for (uint32_t i = 0x2000; i < 0x3000; i++) {
        pt[i] = memc.rom_latch ? 0 : ram_entry((i - 0x2000) << ARM2_PAGE_SHIFT, RW_PRIV);
    }
    for (uint32_t i = 0x3000; i < 0x3800; i++) {
        pt[i] = 0;
    }
    for (uint32_t i = 0x3800; i < 0x4000; i++) {
        pt[i] = rom_entry((i - 0x3800) << ARM2_PAGE_SHIFT);
    }
    arm2_flush(&arc.cpu);
}

static void set_rom_speed(void)
{
    /* The high ROM's access time: 450, 325, 200 ns, 200 with 60 ns nibble mode */
    static const uint8_t n[4] = { 4, 3, 2, 2 }, s[4] = { 4, 3, 2, 1 };
    int speed = (memc.control >> 6) & 3;
    memc.rom_n = n[speed];
    memc.rom_s = s[speed];
    arc.cpu.slow_n = memc.rom_n;
    arc.cpu.slow_s = memc.rom_s;
    arm2_flush(&arc.cpu);
}

void memc_reset(void)
{
    memc.control = 0;
    memc.psize = 0;
    memc.os_mode = memc.video_dma = memc.sound_dma = false;
    memc.rom_latch = true;
    memc.vinit = memc.vstart = memc.vend = memc.cinit = 0;
    memc.sstart = memc.sendn = memc.sptr = memc.send = 0;
    memset(memc.phys_to_log, 0xFF, sizeof(memc.phys_to_log));
    memset(memc.phys_ppl, 0, sizeof(memc.phys_ppl));
    memset(memc.log_to_phys, 0xFF, sizeof(memc.log_to_phys));
    memc.watch_lo = memc.watch_hi = 0;
    set_ram_wiring();
    set_rom_speed();
    memc_build();
}

void memc_latch_off(void)
{
    if (memc.rom_latch) {
        memc.rom_latch = false;
        memc_build();
    }
}

/* The screen's pages report writes (the picture's signature) */
static void update_watch(void)
{
    uint32_t lo = memc.vstart & ~(ARM2_PAGE_SIZE - 1);
    uint32_t hi = memc.vend + 16;
    if (hi < lo) {
        hi = lo;
    }
    if (lo != memc.watch_lo || hi != memc.watch_hi) {
        memc.watch_lo = lo;
        memc.watch_hi = hi;
        memc_build();
    }
}

void memc_write_register(uint32_t addr)
{
    uint32_t v = ((addr >> 2) & 0x7FFF) * 16;
    switch ((addr >> 17) & 7) {
    case 0:
        memc.vinit = v;
        break;
    case 1:
        memc.vstart = v;
        update_watch();
        vidc_dma_changed();
        break;
    case 2:
        memc.vend = v;
        update_watch();
        vidc_dma_changed();
        break;
    case 3:
        memc.cinit = v;
        break;
    case 4:
        sound_run();
        memc.sstart = v;
        ioc_irqb(IRQB_SOUND, false);
        break;
    case 5:
        sound_run();
        memc.sendn = v;
        break;
    case 6:
        /* the pointer reset: the next buffer now, and ask for another */
        sound_run();
        memc.sptr = memc.sstart;
        memc.send = memc.sendn;
        ioc_irqb(IRQB_SOUND, true);
        break;
    default: {
        uint32_t old = memc.control;
        sound_run();
        memc.control = addr & 0x1FFC;
        int psize = (int)((addr >> 2) & 3);
        bool os_mode = (addr >> 12) & 1;
        memc.video_dma = (addr >> 10) & 1;
        bool sound_was = memc.sound_dma;
        memc.sound_dma = (addr >> 11) & 1;
        if (memc.sound_dma && !sound_was) {
            memc.sptr = memc.sstart;
            memc.send = memc.sendn;
        }
        if (((old ^ memc.control) >> 6) & 3) {
            set_rom_speed();
        }
        if (psize != memc.psize) {
            /* the CAM's logical pages mean something else now: start again */
            memc.psize = psize;
            set_ram_wiring();
            memset(memc.phys_to_log, 0xFF, sizeof(memc.phys_to_log));
            memset(memc.log_to_phys, 0xFF, sizeof(memc.log_to_phys));
            memc.os_mode = os_mode;
            memc_build();
        } else if (os_mode != memc.os_mode) {
            memc.os_mode = os_mode;
            memc_build();
        }
        vidc_dma_changed();
        break;
    }
    }
}

uint32_t arc_ram_free(void)
{
    uint32_t page_bytes = ARM2_PAGE_SIZE << memc.psize;
    uint32_t pages = arc.cfg.ram_size / page_bytes;
    uint32_t free = 0;
    for (uint32_t p = 0; p < pages && p < 128; p++) {
        int lp = memc.phys_to_log[p];
        uint32_t addr = (uint32_t)lp * page_bytes;
        if (lp >= 0 && addr >= 0x8000 && addr < 0x1000000 && memc.phys_ppl[p] >= 2) {
            free += page_bytes;
        }
    }
    return free;
}

void memc_write_cam(uint32_t a)
{
    int phys, log;
    switch (memc.psize) {
    case 0:
        phys = a & 0x7F;
        log = (int)(((a >> 12) & 0x7FF) | (((a >> 10) & 3) << 11));
        break;
    case 1:
        phys = (int)(((a >> 1) & 0x3F) | ((a & 1) << 6));
        log = (int)(((a >> 13) & 0x3FF) | (((a >> 10) & 3) << 10));
        break;
    case 2:
        phys = (int)(((a >> 2) & 0x1F) | ((a & 3) << 5));
        log = (int)(((a >> 14) & 0x1FF) | (((a >> 10) & 3) << 9));
        break;
    default:
        phys = (int)(((a >> 3) & 0xF) | ((a & 1) << 4) | ((a & 2) << 5) | ((a & 4) << 3));
        log = (int)(((a >> 15) & 0xFF) | (((a >> 10) & 3) << 8));
        break;
    }
    if (a & 0x80) {
        return;                         /* another MEMC's (the A540 has four) */
    }
    if (arc_memc_log) {
        ARC_LOG("CAM %07X: logical %d (%07X) -> physical %d, level %d, page size %d", (unsigned)a, log,
                (unsigned)(log << (12 + memc.psize)), phys, (int)((a >> 8) & 3), memc.psize);
    }
    bool rebuild_all = memc.rom_latch;
    memc.rom_latch = false;
    int old = memc.phys_to_log[phys];
    memc.phys_to_log[phys] = (int16_t)log;
    memc.phys_ppl[phys] = (uint8_t)((a >> 8) & 3);
    if (old >= 0 && old != log && memc.log_to_phys[old] == phys) {
        memc.log_to_phys[old] = -1;
        if (!rebuild_all) {
            build_logical(old);
        }
    }
    memc.log_to_phys[log] = (int16_t)phys;
    if (rebuild_all) {
        memc_build();
    } else {
        build_logical(log);
        arm2_flush(&arc.cpu);
    }
}
