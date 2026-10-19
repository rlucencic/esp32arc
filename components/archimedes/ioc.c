/*
 * The IOC: interrupts (IRQ A latched and level sources, IRQ B levels, FIQ),
 * four 16-bit timers counting at 2 MHz (0 and 1 interrupt; 2 and 3 set the
 * serial and keyboard baud rates), the keyboard's serial link (KART) and
 * the control port's lines C0-C5 (C0 and C1 the I2C bus to the PCF8583,
 * C2 the floppy drive's ready, C3-C5 unused here), with the vertical
 * flyback in bit 7.
 *
 *   0x00 control   0x04 KART data  0x10/14/18 IRQ A status/request(clear)/mask
 *   0x20/24/28 IRQ B               0x30/34/38 FIQ
 *   0x40-0x7C timers 0-3: latch low, latch high, go, latch the count
 *
 * Writes come on data bits 16-23 (a byte store repeats its byte there).
 */
#include <string.h>
#include "arc_internal.h"

ioc_t ioc;

/* The byte to the keyboard takes this long (31250 baud is 320 us a byte;
 * Arculator's 1 ms suits RISC OS's handshake)
 */
#define KART_BYTE_TICKS     US(1000)

/* The timers count at 2 MHz */
#define TIMER_TICKS     (arc.tick_mhz / 2)

static uint64_t timer_period(int t)
{
    uint32_t latch = ioc.timer[t].latch ? ioc.timer[t].latch : 0x10000;
    return (uint64_t)(latch + 1) * TIMER_TICKS;
}

void arc_update_interrupts(void)
{
    bool irq = ((ioc.irqa | IRQA_FORCE) & ioc.irqa_mask) || (ioc.irqb & ioc.irqb_mask);
    bool fiq = ((ioc.fiq | FIQ_FORCE) & ioc.fiq_mask) != 0;
    if (irq != arc.cpu.irq_line) {
        arm2_set_irq(&arc.cpu, irq);
    }
    if (fiq != arc.cpu.fiq_line) {
        arm2_set_fiq(&arc.cpu, fiq);
    }
}

void ioc_irqa(uint8_t bits)
{
    ioc.irqa |= bits;
    arc_update_interrupts();
}

void ioc_irqb(uint8_t bits, bool on)
{
    ioc.irqb = on ? ioc.irqb | bits : ioc.irqb & ~bits;
    arc_update_interrupts();
}

void ioc_fiq(uint8_t bits, bool on)
{
    ioc.fiq = on ? ioc.fiq | bits : ioc.fiq & ~bits;
    arc_update_interrupts();
}

void ioc_reset(void)
{
    memset(&ioc, 0, sizeof(ioc));
    ioc.irqa = IRQA_POR;
    ioc.irqb = IRQB_KTX;                /* nothing being sent */
    ioc.control = 0xFF;
    ioc.ktx_done = NEVER;
    for (int t = 0; t < 4; t++) {
        ioc.timer[t].expiry = NEVER;
    }
    arc_update_interrupts();
}

void ioc_kart_receive(uint8_t byte)
{
    ioc.kart_rx = byte;
    ioc_irqb(IRQB_KRX, true);
}

/* The count now (2 MHz ticks to the next zero, minus one) */
static uint16_t timer_count(int t)
{
    if (ioc.timer[t].expiry == NEVER) {
        return ioc.timer[t].latch;
    }
    uint64_t now = arc_now();
    if (ioc.timer[t].expiry <= now) {
        /* timers 2 and 3 have no events: catch up */
        uint64_t period = timer_period(t);
        ioc.timer[t].expiry += ((now - ioc.timer[t].expiry) / period + 1) * period;
    }
    uint64_t left = ioc.timer[t].expiry - now;
    uint32_t count = (uint32_t)(left / TIMER_TICKS);
    return (uint16_t)(count ? count - 1 : 0);
}

uint8_t ioc_read(uint32_t addr)
{
    switch (addr & 0x7C) {
    case 0x00: {
        uint8_t v = ioc.control & 0x3C;
        v |= (ioc.control & 0x02);                          /* SCL */
        v |= (ioc.control & 0x01) && cmos_sda() ? 0x01 : 0; /* SDA: either side pulls it low */
        if (arc.cfg.model == ARC_MODEL_A3020) {
            v |= 0x1C;                  /* C2 a high density drive, C3 no ID chip, C4 */
        } else {
            v = fdc_ready() ? v | 0x04 : v & ~0x04;
        }
        v |= 0x40;                                          /* printer ack: idle */
        if (vidc.flyback) {
            v |= 0x80;
        }
        return v;
    }
    case 0x04:
        ioc_irqb(IRQB_KRX, false);
        return ioc.kart_rx;
    case 0x10: return ioc.irqa | IRQA_FORCE;
    case 0x14: return (ioc.irqa | IRQA_FORCE) & ioc.irqa_mask;
    case 0x18: return ioc.irqa_mask;
    case 0x20: return ioc.irqb;
    case 0x24: return ioc.irqb & ioc.irqb_mask;
    case 0x28: return ioc.irqb_mask;
    case 0x30: return ioc.fiq | FIQ_FORCE;
    case 0x34: return (ioc.fiq | FIQ_FORCE) & ioc.fiq_mask;
    case 0x38: return ioc.fiq_mask;
    case 0x40: case 0x50: case 0x60: case 0x70:
        return (uint8_t)ioc.timer[(addr >> 4) & 3].out;
    case 0x44: case 0x54: case 0x64: case 0x74:
        return (uint8_t)(ioc.timer[(addr >> 4) & 3].out >> 8);
    default:
        return 0xFF;
    }
}

void ioc_write(uint32_t addr, uint8_t v)
{
    int t = (int)((addr >> 4) & 3);
    switch (addr & 0x7C) {
    case 0x00:
        ioc.control = v;
        cmos_lines((v & 0x02) != 0, (v & 0x01) != 0);
        break;
    case 0x04:
        /* to the keyboard: it answers once the byte is through */
        ioc_irqb(IRQB_KTX, false);
        keyboard_from_host(v);
        ioc.ktx_done = arc_now() + KART_BYTE_TICKS;
        arc_schedule(ioc.ktx_done);
        break;
    case 0x14:
        ioc.irqa &= (uint8_t)~(v & 0x7C);
        arc_update_interrupts();
        break;
    case 0x18:
        ioc.irqa_mask = v;
        arc_update_interrupts();
        break;
    case 0x28:
        ioc.irqb_mask = v;
        arc_update_interrupts();
        break;
    case 0x38:
        ioc.fiq_mask = v;
        arc_update_interrupts();
        break;
    case 0x40: case 0x50: case 0x60: case 0x70:
        ioc.timer[t].latch = (uint16_t)((ioc.timer[t].latch & 0xFF00) | v);
        break;
    case 0x44: case 0x54: case 0x64: case 0x74:
        ioc.timer[t].latch = (uint16_t)((ioc.timer[t].latch & 0x00FF) | (v << 8));
        break;
    case 0x48: case 0x58: case 0x68: case 0x78:
        /* go: the count from the latch now */
        ioc.timer[t].expiry = arc_now() + timer_period(t);
        arc_schedule(ioc.timer[t].expiry);
        break;
    case 0x4C: case 0x5C: case 0x6C: case 0x7C:
        ioc.timer[t].out = timer_count(t);
        break;
    default:
        break;
    }
}

uint64_t ioc_next_event(void)
{
    uint64_t t = ioc.ktx_done;
    for (int i = 0; i < 2; i++) {
        if (ioc.timer[i].expiry < t) {
            t = ioc.timer[i].expiry;
        }
    }
    return t;
}

void ioc_event(void)
{
    uint64_t now = arc_now();
    if (ioc.ktx_done <= now) {
        ioc.ktx_done = NEVER;
        ioc_irqb(IRQB_KTX, true);
    }
    /* Timers 0 and 1 interrupt; 2 and 3 only count (timer_count() catches
     * them up when they're read)
     */
    for (int i = 0; i < 2; i++) {
        if (ioc.timer[i].expiry == NEVER || ioc.timer[i].expiry > now) {
            continue;
        }
        uint64_t period = timer_period(i);
        ioc.timer[i].expiry += period;
        if (ioc.timer[i].expiry <= now) {
            ioc.timer[i].expiry += ((now - ioc.timer[i].expiry) / period + 1) * period;
        }
        ioc_irqa(i ? IRQA_TM1 : IRQA_TM0);
    }
}
