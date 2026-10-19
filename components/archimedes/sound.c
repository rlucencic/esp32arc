/*
 * The VIDC's sound and MEMC's sound DMA.
 *
 * MEMC fetches 16 bytes at a time from physical RAM (Sptr, running from
 * Sstart to SendN); at a buffer's end it starts the next one (the Sstart
 * and SendN the program set meanwhile) and raises the sound interrupt
 * (IOC IRQ B 1), which writing Sstart clears. The VIDC plays one byte
 * every (SFR + 2) us of its 1 MHz sound clock, the bytes taking the 8
 * channels' stereo positions in turn (registers 60-7C: 60 is channel 7, 64
 * channel 0, ...). A byte is 8-bit logarithmic: bit 0 the sign, 7-5 the
 * chord, 4-1 the point (MAME's VIDC1a table).
 *
 * The output: each 32 us of the DAC's levels averaged into a sample at
 * 31,250 Hz, in a ring the speaker's task takes from.
 */
#include <string.h>
#include "arc_internal.h"

#define RING        8192                /* frames, ~260 ms */
#define SAMPLE_US   32

static struct {
    uint32_t period;                    /* us a byte */
    int16_t gain_l[8], gain_r[8];       /* of 256 */
    uint8_t fifo[16];
    int fifo_pos;                       /* 16: empty */
    int channel;
    int32_t level_l, level_r;           /* the byte playing */
    uint32_t byte_left;                 /* its us left */
    int64_t acc_l, acc_r;
    uint32_t acc_us;
    uint64_t last;                      /* tick sound ran to */
    uint32_t tick_frac;                 /* ticks short of a us */
    bool enabled;
    int32_t dc_l, dc_r;                 /* the output's DC (high pass) */
} snd;

static int16_t s_log[256];
bool arc_sound_log;
static uint32_t s_log_bytes, s_log_loud;
static int16_t *s_ring;                 /* RING stereo frames */
static volatile uint32_t s_write, s_read;

static void set_stereo(int ch, uint32_t image)
{
    /* the left side's share of 18 stereo clocks; 4 centre, 1 left, 7 right */
    static const uint8_t left[8] = { 9, 18, 15, 12, 9, 6, 3, 0 };
    int l = left[image & 7];
    snd.gain_l[ch] = (int16_t)(l * 256 / 18);
    snd.gain_r[ch] = (int16_t)((18 - l) * 256 / 18);
}

void sound_reset(void)
{
    if (!s_ring) {
        s_ring = arc_calloc(RING * 2, sizeof(int16_t));
    }
    for (int raw = 0; raw < 256; raw++) {
        int chord = raw >> 5, point = (raw & 0x1E) >> 1;
        int v = ((16 + point) << chord) - 16;
        s_log[raw] = (int16_t)((raw & 1) ? -v * 8 : v * 8);
    }
    bool enabled = snd.enabled;
    memset(&snd, 0, sizeof(snd));
    snd.enabled = enabled;
    snd.period = 48;
    snd.fifo_pos = 16;
    snd.byte_left = snd.period;
    snd.last = arc_now();
    for (int ch = 0; ch < 8; ch++) {
        set_stereo(ch, 4);
    }
    s_write = s_read = 0;
}

void sound_write_stereo(int ch, uint32_t value)
{
    sound_run();
    set_stereo(ch, value & 7);
}

void sound_write_sfr(uint32_t value)
{
    sound_run();
    snd.period = (value & 0xFF) + 2;
}

void arc_sound_log_frame(void)
{
    if (arc_sound_log && s_log_bytes) {
        ARC_LOG("sound: %u bytes, %u not silent, period %u us, stereo %d %d %d %d %d %d %d %d", (unsigned)s_log_bytes,
                (unsigned)s_log_loud, (unsigned)snd.period, snd.gain_l[0], snd.gain_l[1], snd.gain_l[2],
                snd.gain_l[3], snd.gain_l[4], snd.gain_l[5], snd.gain_l[6], snd.gain_l[7]);
    }
    s_log_bytes = s_log_loud = 0;
}

uint32_t sound_byte_rate(void)
{
    return memc.sound_dma ? 1000000 / snd.period : 0;
}

/* The next byte: from the FIFO, which MEMC refills a quad-word at a time */
static void next_byte(void)
{
    snd.byte_left = snd.period;
    if (!memc.sound_dma) {
        snd.level_l = snd.level_r = 0;
        return;
    }
    if (snd.fifo_pos >= 16) {
        memcpy(snd.fifo, memc_phys(memc.sptr), 16);
        memc.sptr += 16;
        if (memc.sptr > memc.send) {
            memc.sptr = memc.sstart;
            memc.send = memc.sendn;
            ioc_irqb(IRQB_SOUND, true);
        }
        snd.fifo_pos = 0;
        snd.channel = 0;
    }
    if (arc_sound_log) {
        s_log_bytes++;
        s_log_loud += (snd.fifo[snd.fifo_pos] & 0xFE) != 0;
    }
    int32_t v = s_log[snd.fifo[snd.fifo_pos++]];
    int ch = snd.channel;
    snd.channel = (snd.channel + 1) & 7;
    snd.level_l = v * snd.gain_l[ch];
    snd.level_r = v * snd.gain_r[ch];
}

static void emit(void)
{
    int32_t l = (int32_t)(snd.acc_l / (SAMPLE_US * 256));
    int32_t r = (int32_t)(snd.acc_r / (SAMPLE_US * 256));
    snd.acc_l = snd.acc_r = 0;
    snd.acc_us = 0;
    /* a slow high pass: the DAC's offset away */
    snd.dc_l += (l * 256 - snd.dc_l) >> 10;
    snd.dc_r += (r * 256 - snd.dc_r) >> 10;
    l -= snd.dc_l >> 8;
    r -= snd.dc_r >> 8;
    if (!snd.enabled || s_write - s_read >= RING) {
        return;
    }
    uint32_t i = (s_write % RING) * 2;
    s_ring[i] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
    s_ring[i + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    __atomic_store_n(&s_write, s_write + 1, __ATOMIC_RELEASE);
}

void sound_run(void)
{
    uint64_t now = arc_now();
    if (now <= snd.last) {
        return;
    }
    uint64_t ticks = now - snd.last + snd.tick_frac;
    snd.last = now;
    uint32_t us;
    if (ticks < (1u << 24)) {
        /* A line's worth (this runs every line): divided by the clock's
         * MHz as a multiplication by its reciprocal, rounded up, which is
         * exact below 2^24 for divisors up to 256
         */
        static uint32_t mhz, recip;
        if (arc.tick_mhz != mhz) {
            mhz = arc.tick_mhz;
            recip = (uint32_t)(0xFFFFFFFFu / mhz) + 1;
        }
        us = (uint32_t)(((uint64_t)ticks * recip) >> 32);
        snd.tick_frac = (uint32_t)ticks - us * mhz;
    } else {
        us = (uint32_t)(ticks / arc.tick_mhz);
        snd.tick_frac = (uint32_t)(ticks % arc.tick_mhz);
    }
    while (us > 0) {
        uint32_t step = us;
        if (step > snd.byte_left) {
            step = snd.byte_left;
        }
        if (step > SAMPLE_US - snd.acc_us) {
            step = SAMPLE_US - snd.acc_us;
        }
        snd.acc_l += (int64_t)snd.level_l * step;
        snd.acc_r += (int64_t)snd.level_r * step;
        snd.acc_us += step;
        snd.byte_left -= step;
        us -= step;
        if (snd.acc_us >= SAMPLE_US) {
            emit();
        }
        if (snd.byte_left == 0) {
            next_byte();
        }
    }
}

/* ---- The speaker's side ---- */

void arc_audio_enable(bool on)
{
    snd.enabled = on;
    if (!on) {
        s_read = s_write;
    }
}

int arc_audio_pending(void)
{
    return (int)(__atomic_load_n(&s_write, __ATOMIC_ACQUIRE) - s_read);
}

int arc_audio_render(int16_t *dst, int frames)
{
    uint32_t avail = __atomic_load_n(&s_write, __ATOMIC_ACQUIRE) - s_read;
    int n = (int)avail < frames ? (int)avail : frames;
    for (int i = 0; i < n; i++) {
        uint32_t k = ((s_read + (uint32_t)i) % RING) * 2;
        dst[2 * i] = s_ring[k];
        dst[2 * i + 1] = s_ring[k + 1];
    }
    s_read += (uint32_t)n;
    return n;
}
