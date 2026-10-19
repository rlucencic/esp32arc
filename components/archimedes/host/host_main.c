/*
 * Host build of the Archimedes core, for testing on a desktop:
 *
 *   build/arc_host <rom> [frames] [out.ppm] [ram_kb]
 *
 * Environment:
 *   ARC_CMOS=<file>          the CMOS RAM (256 bytes), read and written back
 *                            (none yet: the OS's defaults; ARC_BLANKCMOS: zeros)
 *   ARC_MODEL=3020           an A3020/A4000 (ARM250, 82C711, IOEB), else an A440
 *   ARC_FLOPPY0=, _FLOPPY1=  disc images (.adf, .adl, .img)
 *   ARC_HD=<image>           the A3020's IDE disc
 *   ARC_SCRIPT=<file>        lines "<frame> <command> ..." (not before the
 *                            text typed before them is in):
 *                              type <text> (\n Return), key <hex> 0|1,
 *                              mouse <dx> <dy>, buttons <b> (1 Select,
 *                              2 Menu, 4 Adjust), pointer <x> <y> <b>,
 *                              shot <file.ppm>, insert <0|1> <path>,
 *                              eject <0|1>, reset, status, regs,
 *                              where (the pointer on the picture),
 *                              peek <hex>, dump <hex> <n>, dis <hex> <n>
 *   ARC_WAV=<file>           write the sound
 *   ARC_TRACE=<hex pc>:<n>   print n instructions from when pc is reached (FFFFFFF:
 *                            anywhere), from frame ARC_TRACE_FRAME on
 *   ARC_PCLOG=<hex pc>:<n>   the registers the first n times pc runs
 *   ARC_SWI=<frame>          print the SWIs from that frame on
 *   ARC_MIX=<frame>          the instructions run from that frame on, by kind
 *   ARC_STATUS=<n>           the machine's state every n frames
 *   ARC_KBLOG, ARC_FDCLOG, ARC_MEMCLOG  log the keyboard link, the WD1772's
 *                            commands, the MEMC's CAM writes
 *   ARC_DEBUG=<port>         serve the debugger (esp32emu's docs/debugger.md) on
 *                            127.0.0.1:<port> and run in real time until killed
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../arc_internal.h"
#include "emudbg.h"

static uint8_t s_cmos[ARC_CMOS_SIZE];
static const char *s_cmos_path;
static bool s_cmos_dirty;

static void cmos_changed(void)
{
    s_cmos_dirty = true;
}

static void save_cmos(void)
{
    if (s_cmos_path && s_cmos_dirty) {
        FILE *f = fopen(s_cmos_path, "wb");
        if (f) {
            fwrite(s_cmos, 1, sizeof(s_cmos), f);
            fclose(f);
        }
        s_cmos_dirty = false;
    }
}

static void write_ppm(const char *path)
{
    int w, h;
    arc_screen_signature();             /* the last picture copied */
    arc_screen_size(&w, &h);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    static uint16_t line[ARC_SCREEN_MAX_W];
    for (int y = 0; y < h; y++) {
        arc_render_line_rgb565(y, line);
        for (int x = 0; x < w; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = {
                (uint8_t)(((c >> 11) & 31) * 255 / 31),
                (uint8_t)(((c >> 5) & 63) * 255 / 63),
                (uint8_t)((c & 31) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("host: screenshot %s (%d x %d)\n", path, w, h);
}

static void print_regs(void)
{
    const arm2_t *c = &arc.cpu;
    static const char *const modes[4] = { "USR", "FIQ", "IRQ", "SVC" };
    printf("host: PC=%07X %s %c%c%c%c%c%c\n", (unsigned)c->pc, modes[c->psr & 3], (c->psr & ARM2_N) ? 'N' : 'n',
           (c->psr & ARM2_Z) ? 'Z' : 'z', (c->psr & ARM2_C) ? 'C' : 'c', (c->psr & ARM2_V) ? 'V' : 'v',
           (c->psr & ARM2_I) ? 'I' : 'i', (c->psr & ARM2_F) ? 'F' : 'f');
    for (int i = 0; i < 15; i++) {
        printf("%sR%-2d=%08X", i % 5 ? " " : "host: ", i, (unsigned)c->r[i]);
        if (i % 5 == 4) {
            printf("\n");
        }
    }
}

static void print_status(uint32_t frame)
{





    arc_status_t s;
    arc_get_status(&s);
    printf("host: frame %u PC=%07X insns %u screen %dx%d %d bpp %d.%02d Hz DMA %d%% sound %s %u B/s"
           " IRQ A %02X/%02X B %02X/%02X FIQ %02X/%02X MEMC %04X\n",
           frame, (unsigned)s.pc, (unsigned)s.insns, s.width, s.height, s.bpp, s.hz100 / 100, s.hz100 % 100,
           s.dma_percent, s.sound_on ? "on" : "off", (unsigned)s.sample_rate, ioc.irqa, ioc.irqa_mask, ioc.irqb,
           ioc.irqb_mask, ioc.fiq, ioc.fiq_mask, (unsigned)memc.control);
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[1024];
} line_t;

static line_t *s_script;
static int s_script_n, s_script_pos;
static uint32_t s_script_delay;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(1);
    }
    char buf[1200];
    s_script = calloc(8192, sizeof(line_t));
    while (fgets(buf, sizeof(buf), f) && s_script_n < 8192) {
        buf[strcspn(buf, "\r\n")] = 0;
        line_t *l = &s_script[s_script_n];
        int used = 0;
        if (sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &used) >= 2) {
            snprintf(l->arg, sizeof(l->arg), "%s", buf + used);
            s_script_n++;
        }
    }
    fclose(f);
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *o++ = '\n';
            s++;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void run_script(uint32_t frame)
{
    while (s_script_pos < s_script_n && s_script[s_script_pos].frame + s_script_delay <= frame) {
        if (arc_typing()) {
            s_script_delay++;
            return;
        }
        line_t *l = &s_script[s_script_pos++];
        if (!strcmp(l->cmd, "type")) {
            unescape(l->arg);
            arc_type(l->arg);
        } else if (!strcmp(l->cmd, "key")) {
            unsigned k = 0;
            int d = 0;
            sscanf(l->arg, "%x %d", &k, &d);
            arc_key((uint8_t)k, d);
        } else if (!strcmp(l->cmd, "mouse")) {
            int dx = 0, dy = 0;
            sscanf(l->arg, "%d %d", &dx, &dy);
            arc_mouse(dx, dy);
        } else if (!strcmp(l->cmd, "buttons")) {
            arc_mouse_buttons((uint8_t)atoi(l->arg));
        } else if (!strcmp(l->cmd, "pointer")) {
            int x = 0, y = 0, b = 0;
            sscanf(l->arg, "%d %d %d", &x, &y, &b);
            arc_pointer(x, y, (uint8_t)b);
        } else if (!strcmp(l->cmd, "shot")) {
            write_ppm(l->arg);
        } else if (!strcmp(l->cmd, "insert")) {
            int d = 0;
            char path[512];
            if (sscanf(l->arg, "%d %511s", &d, path) == 2 && !arc_floppy_insert(d, path)) {
                printf("host: can't insert %s\n", path);
            }
        } else if (!strcmp(l->cmd, "eject")) {
            arc_floppy_eject(atoi(l->arg));
        } else if (!strcmp(l->cmd, "reset")) {
            arc_reset();
        } else if (!strcmp(l->cmd, "status")) {
            print_status(frame);
        } else if (!strcmp(l->cmd, "regs")) {
            print_regs();
        } else if (!strcmp(l->cmd, "cam")) {
            /* how many physical pages each logical page has */
            int pages = (int)(arc.cfg.ram_size / (4096u << memc.psize));
            int count[8192] = { 0 };
            for (int p = 0; p < pages && p < 128; p++) {
                if (memc.phys_to_log[p] >= 0) {
                    count[memc.phys_to_log[p]]++;
                }
            }
            printf("host: frame %u CAM, %d pages of %u KB, mapped at:", frame, pages, 4u << memc.psize);
            for (int lp = 0; lp < 8192; lp++) {
                if (count[lp]) {
                    int run = lp;
                    while (run + 1 < 8192 && count[run + 1]) {
                        run++;
                    }
                    printf(" %07X-%07X (%d)", (unsigned)(lp << (12 + memc.psize)),
                           (unsigned)(((run + 1) << (12 + memc.psize)) - 1), run - lp + 1);
                    lp = run;
                }
            }
            printf("\n");
        } else if (!strcmp(l->cmd, "ppl")) {
            /* the protection level of each page below 0x1000000, in logical order */
            int pages = (int)(arc.cfg.ram_size / (4096u << memc.psize));
            char line[8192 + 1];
            int n = 0;
            for (int lp = 0; lp < (0x1000000 >> (12 + memc.psize)); lp++) {
                int phys = -1;
                for (int p = 0; p < pages && p < 128; p++) {
                    if (memc.phys_to_log[p] == lp) {
                        phys = p;
                    }
                }
                if (phys >= 0) {
                    line[n++] = (char)('0' + memc.phys_ppl[phys]);
                }
            }
            line[n] = 0;
            printf("host: frame %u PPL %s\n", frame, line);
        } else if (!strcmp(l->cmd, "where")) {
            int x, y;
            if (vidc_pointer(&x, &y)) {
                printf("host: frame %u pointer %d,%d\n", frame, x, y);
            } else {
                printf("host: frame %u no pointer\n", frame);
            }
        } else if (!strcmp(l->cmd, "peek")) {
            unsigned a = (unsigned)strtoul(l->arg, NULL, 16);
            printf("host: frame %u &%07X = &%08X\n", frame, a, (unsigned)arc_peek32(a));
        } else if (!strcmp(l->cmd, "dump")) {
            unsigned a = 0, n = 16;
            sscanf(l->arg, "%x %u", &a, &n);
            for (unsigned i = 0; i < n; i += 16) {
                printf("host: %07X:", a + i);
                char text[17] = { 0 };
                for (unsigned j = 0; j < 16 && i + j < n; j++) {
                    uint8_t b = arc_peek8(a + i + j);
                    printf(" %02X", b);
                    text[j] = isprint(b) ? (char)b : '.';
                }
                printf("  %s\n", text);
            }
        } else if (!strcmp(l->cmd, "dis")) {
            unsigned a = 0, n = 16;
            sscanf(l->arg, "%x %u", &a, &n);
            for (unsigned i = 0; i < n; i++) {
                char buf[64];
                arm2_disassemble(&arc.cpu, a + 4 * i, buf, sizeof(buf));
                printf("host: %07X  %08X  %s\n", a + 4 * i, (unsigned)arc_peek32(a + 4 * i), buf);
            }
        } else {
            printf("host: unknown command %s\n", l->cmd);
        }
    }
}

/* ---- Tracing ---- */

static int64_t s_trace_pc = -1, s_pclog_pc = -1;
static uint32_t s_trace_frame;
static int s_trace_n, s_pclog_n;
static bool s_tracing;
static int64_t s_swi_frame = -1;

/* ARC_MIX=<frame>: the instructions run from that frame on, by kind */
static int64_t s_mix_frame = -1;
enum { MIX_SKIPPED, MIX_DP_IMM, MIX_DP_REG, MIX_DP_SHIFT_REG, MIX_DP_S, MIX_MUL, MIX_LDR, MIX_STR, MIX_LDRB, MIX_STRB,
       MIX_LDM, MIX_STM, MIX_B, MIX_BL, MIX_SWI, MIX_OTHER, MIX_KINDS };
static const char *const s_mix_names[MIX_KINDS] = {
    "condition failed", "data processing, immediate", "data processing, register", "data processing, shift by register",
    "  (of those, setting flags)", "MUL, MLA", "LDR", "STR", "LDRB", "STRB", "LDM", "STM", "B", "BL", "SWI", "other",
};
static uint64_t s_mix[MIX_KINDS], s_mix_regs[2], s_mix_total;

static void mix_count(const arm2_t *c, uint32_t op)
{
    s_mix_total++;
    uint32_t cond = op >> 28, f = c->psr >> 28;
    bool n = f & 8, z = f & 4, cf = f & 2, v = f & 1, ok;
    switch (cond) {
    case 0x0: ok = z; break;
    case 0x1: ok = !z; break;
    case 0x2: ok = cf; break;
    case 0x3: ok = !cf; break;
    case 0x4: ok = n; break;
    case 0x5: ok = !n; break;
    case 0x6: ok = v; break;
    case 0x7: ok = !v; break;
    case 0x8: ok = cf && !z; break;
    case 0x9: ok = !cf || z; break;
    case 0xA: ok = n == v; break;
    case 0xB: ok = n != v; break;
    case 0xC: ok = !z && n == v; break;
    case 0xD: ok = z || n != v; break;
    case 0xE: ok = true; break;
    default: ok = false; break;
    }
    if (!ok) {
        s_mix[MIX_SKIPPED]++;
        return;
    }
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0FC000F0) == 0x00000090) {
            s_mix[MIX_MUL]++;
            return;
        }
        s_mix[(op & 0x10) ? MIX_DP_SHIFT_REG : MIX_DP_REG]++;
        if (op & (1u << 20)) {
            s_mix[MIX_DP_S]++;
        }
        return;
    case 1:
        s_mix[MIX_DP_IMM]++;
        if (op & (1u << 20)) {
            s_mix[MIX_DP_S]++;
        }
        return;
    case 2: case 3:
        s_mix[((op >> 20) & 1 ? MIX_LDR : MIX_STR) + ((op >> 22) & 1 ? 2 : 0)]++;
        return;
    case 4: {
        bool load = (op >> 20) & 1;
        s_mix[load ? MIX_LDM : MIX_STM]++;
        s_mix_regs[load] += (uint64_t)__builtin_popcount(op & 0xFFFF);
        return;
    }
    case 5:
        s_mix[(op >> 24) & 1 ? MIX_BL : MIX_B]++;
        return;
    case 7:
        if ((op >> 24) & 1) {
            s_mix[MIX_SWI]++;
            return;
        }
        /* fall through */
    default:
        s_mix[MIX_OTHER]++;
        return;
    }
}

static void mix_print(void)
{
    if (s_mix_frame < 0 || s_mix_total == 0) {
        return;
    }
    printf("host: %llu instructions from frame %lld\n", (unsigned long long)s_mix_total, (long long)s_mix_frame);
    for (int k = 0; k < MIX_KINDS; k++) {
        printf("  %-36s %5.1f%%", s_mix_names[k], 100.0 * (double)s_mix[k] / (double)s_mix_total);
        if (k == MIX_LDM || k == MIX_STM) {
            printf("  (%.1f registers)", s_mix[k] ? (double)s_mix_regs[k == MIX_LDM] / (double)s_mix[k] : 0.0);
        }
        printf("\n");
    }
}

void arm2_host_hook(arm2_t *c)
{
    uint32_t pc = c->pc;
    if (s_mix_frame >= 0 && arc.frames >= (uint32_t)s_mix_frame) {
        mix_count(c, arm2_peek32(c, pc));
    }
    if (s_swi_frame >= 0 && arc.frames >= (uint32_t)s_swi_frame) {
        uint32_t op = arm2_peek32(c, pc);
        if ((op & 0x0F000000) == 0x0F000000 && (op >> 28) != 0xF) {
            printf("%6u %07X SWI &%06X R0=%08X R1=%08X R2=%08X\n", (unsigned)arc.frames, (unsigned)pc,
                   (unsigned)(op & 0xFFFFFF), (unsigned)c->r[0], (unsigned)c->r[1], (unsigned)c->r[2]);
        }
    }
    if (s_pclog_pc >= 0 && pc == (uint32_t)s_pclog_pc && s_pclog_n > 0) {
        printf("%6u PC=%07X R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R13=%08X R14=%08X PSR=%08X\n",
               (unsigned)arc.frames, (unsigned)pc, (unsigned)c->r[0], (unsigned)c->r[1], (unsigned)c->r[2],
               (unsigned)c->r[3], (unsigned)c->r[4], (unsigned)c->r[13], (unsigned)c->r[14], (unsigned)c->psr);
        s_pclog_n--;
    }
    if (s_trace_pc < 0) {
        return;
    }
    if (!s_tracing && (pc == (uint32_t)s_trace_pc || s_trace_pc == 0xFFFFFFF) && arc.frames >= s_trace_frame) {
        s_tracing = true;
    }
    if (s_tracing && s_trace_n > 0) {
        char dis[64];
        arm2_disassemble(c, pc, dis, sizeof(dis));
        printf("%6u %07X  %-30s %c%c%c%c", (unsigned)arc.frames, (unsigned)pc, dis, (c->psr & ARM2_N) ? 'N' : '-',
               (c->psr & ARM2_Z) ? 'Z' : '-', (c->psr & ARM2_C) ? 'C' : '-', (c->psr & ARM2_V) ? 'V' : '-');
        for (int i = 0; i < 15; i++) {
            printf(" %X:%X", i, (unsigned)c->r[i]);
        }
        printf("\n");
        s_trace_n--;
    }
}

static uint8_t *load_file(const char *path, size_t *size, size_t align)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t alloc = ((size_t)n + align - 1) / align * align;
    uint8_t *buf = aligned_alloc(align, alloc ? alloc : align);
    *size = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: arc_host <rom> [frames] [out.ppm] [ram_kb]\n");
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 500;
    const char *out = argc > 3 ? argv[3] : NULL;
    uint32_t ram_kb = argc > 4 ? (uint32_t)atoi(argv[4]) : 4096;

    static arc_config_t cfg;
    cfg.model = getenv("ARC_MODEL") && strstr(getenv("ARC_MODEL"), "3020") ? ARC_MODEL_A3020 : ARC_MODEL_A440;
    size_t rom_size = 0;
    uint8_t *rom = load_file(argv[1], &rom_size, 4096);
    if (!rom) {
        fprintf(stderr, "can't open %s\n", argv[1]);
        return 1;
    }
    /* a 512 KB or 2 MB image (ROM120 has a byte too many) */
    cfg.rom_size = rom_size >= 2 * 1024 * 1024 ? 2 * 1024 * 1024 : 512 * 1024;
    cfg.rom = rom;
    cfg.ram_size = ram_kb * 1024;
    cfg.ram = aligned_alloc(4096, cfg.ram_size);
    static uintptr_t page_table[ARM2_PAGES];
    cfg.page_table = page_table;
    cfg.cmos = s_cmos;
    cfg.cmos_changed = cmos_changed;
    time_t now = time(NULL);
    cfg.cmos_year = (uint16_t)(localtime(&now)->tm_year + 1900);
    char version[64];
    arc_os_t os = ARC_OS_RISCOS3;
    if (arc_rom_identify(rom, cfg.rom_size, &os, version, sizeof(version))) {
        printf("host: %s\n", version);
    } else {
        printf("host: an unknown ROM\n");
    }
    /* The CMOS file, else the OS's defaults (ARC_BLANKCMOS: zeros) */
    s_cmos_path = getenv("ARC_CMOS");
    FILE *f = s_cmos_path ? fopen(s_cmos_path, "rb") : NULL;
    if (f) {
        if (fread(s_cmos, 1, sizeof(s_cmos), f) != sizeof(s_cmos)) {
            fprintf(stderr, "%s: short\n", s_cmos_path);
        }
        fclose(f);
    } else if (!getenv("ARC_BLANKCMOS")) {
        arc_cmos_defaults(cfg.model, os, s_cmos);
        s_cmos_dirty = true;
    }

    FILE *wav = NULL;
    if (getenv("ARC_WAV")) {
        wav = fopen(getenv("ARC_WAV"), "wb");
        if (wav) {
            uint8_t hdr[44] = { 0 };
            fwrite(hdr, 1, 44, wav);
        }
    }
    if (getenv("ARC_TRACE")) {
        unsigned pc = 0;
        int n = 200;
        sscanf(getenv("ARC_TRACE"), "%x:%d", &pc, &n);
        s_trace_pc = pc;
        s_trace_n = n;
    }
    if (getenv("ARC_TRACE_FRAME")) {
        s_trace_frame = (uint32_t)atoi(getenv("ARC_TRACE_FRAME"));
    }
    if (getenv("ARC_PCLOG")) {
        unsigned pc = 0;
        int n = 100;
        sscanf(getenv("ARC_PCLOG"), "%x:%d", &pc, &n);
        s_pclog_pc = pc;
        s_pclog_n = n;
    }
    if (getenv("ARC_SNDLOG")) {
        arc_sound_log = true;
    }
    if (getenv("ARC_KBLOG")) {
        arc_kb_log = true;
    }
    if (getenv("ARC_FDCLOG")) {
        arc_fdc_log = true;
    }
    if (getenv("ARC_MEMCLOG")) {
        arc_memc_log = true;
    }
    if (getenv("ARC_SWI")) {
        s_swi_frame = atoi(getenv("ARC_SWI"));
    }
    if (getenv("ARC_MIX")) {
        s_mix_frame = atoi(getenv("ARC_MIX"));
    }
    if (!arc_init(&cfg)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    arc_audio_enable(wav != NULL);
    if (getenv("ARC_HD") && !arc_hd_insert(getenv("ARC_HD"))) {
        fprintf(stderr, "can't open %s\n", getenv("ARC_HD"));
    }
    for (int d = 0; d < ARC_FLOPPIES; d++) {
        char name[16];
        snprintf(name, sizeof(name), "ARC_FLOPPY%d", d);
        const char *p = getenv(name);
        if (p && !arc_floppy_insert(d, p)) {
            fprintf(stderr, "can't load %s\n", p);
        }
    }
    if (getenv("ARC_SCRIPT")) {
        load_script(getenv("ARC_SCRIPT"));
    }
    emudbg_host_run("ARC_DEBUG", arc_run_frame, ARC_FRAME_HZ);
    int status_every = getenv("ARC_STATUS") ? atoi(getenv("ARC_STATUS")) : 0;
    long samples = 0;
    for (int f = 0; f < frames; f++) {
        run_script((uint32_t)f);
        arc_run_frame();
        if (arc_sound_log && f % 25 == 0) {
            arc_sound_log_frame();
        }
        if (wav) {
            int16_t buf[2 * 1024];
            int n;
            while ((n = arc_audio_render(buf, 1024)) > 0) {
                fwrite(buf, 4, (size_t)n, wav);
                samples += n;
            }
        }
        if (status_every && f % status_every == 0) {
            print_status((uint32_t)f);
        }
    }
    run_script((uint32_t)frames);
    if (out) {
        write_ppm(out);
    }
    arc_floppy_flush();
    arc_hd_flush();
    save_cmos();
    mix_print();
    if (wav) {
        uint32_t data = (uint32_t)samples * 4;
        uint8_t hdr[44] = { 'R', 'I', 'F', 'F' };
        uint32_t v;
        v = 36 + data; memcpy(hdr + 4, &v, 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        v = 16; memcpy(hdr + 16, &v, 4);
        uint16_t w = 1; memcpy(hdr + 20, &w, 2);
        w = 2; memcpy(hdr + 22, &w, 2);
        v = ARC_AUDIO_HZ; memcpy(hdr + 24, &v, 4);
        v = ARC_AUDIO_HZ * 4; memcpy(hdr + 28, &v, 4);
        w = 4; memcpy(hdr + 32, &w, 2);
        w = 16; memcpy(hdr + 34, &w, 2);
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(hdr, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
