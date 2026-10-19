/*
 * The keyboard's microcontroller, as the IOC's serial link sees it (the
 * protocol as Arculator speaks it, which RISC OS and Arthur accept):
 *
 *   reset   the host sends HRST (FF), the keyboard answers HRST; RAK1 (FE)
 *           is answered RAK1, RAK2 (FD) RAK2, and the keyboard is idle
 *   idle    LEDs (00-07), RQID (20): KBID 81 (an Archimedes keyboard),
 *           SACK/MACK/SMAK/NACK (30-33): keys (bit 0) and mouse (bit 1)
 *           on, HRST: reset again
 *   data    a key goes down: C0+row, C0+column; up: D0+row, D0+column; the
 *           mouse moved: dx, dy (7 bits each, up positive). The host
 *           acknowledges the first byte with BACK (3F), the second with
 *           SACK/MACK/SMAK/NACK
 *
 * Keys are row << 4 | column; the mouse's buttons are keys 70-72. Each
 * byte reaches the host 1 ms after it was asked for (or after the host's
 * byte), as Arculator's do. The keyboard looks for something to send every
 * 2 ms while idle.
 *
 * The host side (arc_key, arc_mouse, ...) is called from other tasks: a
 * queue of key events and the mouse's counts, taken by the emulation.
 */
#include <string.h>
#include "arc_internal.h"

#define HRST        0xFF
#define RAK1        0xFE
#define RAK2        0xFD
#define RQID        0x20
#define BACK        0x3F
#define KBID        0x81
#define REPLY_TICKS US(1000)
#define POLL_TICKS  US(2000)
#define QUEUE       64

typedef enum { KB_RESET, KB_IDLE, KB_DAT1, KB_DAT2, KB_SINGLE } kb_state_t;

static struct {
    kb_state_t state;
    bool keys_on, mouse_on;
    uint8_t pair[2];
    uint8_t rx_byte;
    uint64_t rx_due;                    /* the byte reaches the IOC, NEVER */
    uint64_t poll_due;
    uint8_t down[16];                   /* keys the host was told are down */
} kb;

/* ---- The host's side (other tasks) ---- */

static volatile uint8_t s_queue[QUEUE];     /* key | 0x80 when down */
static volatile uint32_t s_head, s_tail;    /* head: written by the producers */
static volatile int s_lock;
static volatile int32_t s_mouse_x, s_mouse_y;
static volatile uint8_t s_buttons;

/* The touch pointer: places to steer the mouse to, one after the other,
 * and the buttons to have there
 */
#define TARGETS     8

typedef struct {
    int16_t x, y;
    uint8_t buttons;
} target_t;

static target_t s_targets[TARGETS];
static volatile uint32_t s_targets_in, s_targets_out;

static void lock(void)
{
    while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock(void)
{
    __atomic_clear(&s_lock, __ATOMIC_RELEASE);
}

static void push(uint8_t ev)
{
    lock();
    if (s_head - s_tail < QUEUE) {
        s_queue[s_head % QUEUE] = ev;
        __atomic_store_n(&s_head, s_head + 1, __ATOMIC_RELEASE);
    }
    unlock();
}

void arc_key(uint8_t key, bool down)
{
    if (key < 0x80) {
        push((uint8_t)(key | (down ? 0x80 : 0)));
    }
}

void arc_mouse(int dx, int dy)
{
    __atomic_fetch_add(&s_mouse_x, dx, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_mouse_y, dy, __ATOMIC_RELAXED);
}

void arc_mouse_buttons(uint8_t buttons)
{
    uint8_t was = s_buttons;
    s_buttons = buttons;
    for (int b = 0; b < 3; b++) {
        if (((was ^ buttons) >> b) & 1) {
            arc_key((uint8_t)(0x70 + b), (buttons >> b) & 1);
        }
    }
}

void arc_pointer(int x, int y, uint8_t buttons)
{
    lock();
    uint32_t n = s_targets_in - s_targets_out;
    target_t *last = &s_targets[(s_targets_in - 1) % TARGETS];
    if ((n >= 2 && last->buttons == buttons) || n >= TARGETS) {
        /* a move with the same buttons: the latest place (never the one
         * being steered to: a press there has to happen there)
         */
        last->x = (int16_t)x;
        last->y = (int16_t)y;
        last->buttons = buttons;
    } else {
        s_targets[s_targets_in % TARGETS] = (target_t){ (int16_t)x, (int16_t)y, buttons };
        s_targets_in++;
    }
    unlock();
}

/* ---- The microcontroller (the emulation task) ---- */

/* A typed key is held 40 ms, then 40 ms pass before the next: RISC OS
 * takes keys at its 100 Hz tick, and loses one that went up before it
 */
#define TYPE_HOLD   US(40000)
#define TYPE_GAP    US(40000)

static struct {
    int key;                            /* the key held, -1 */
    bool shift;
    uint64_t due;                       /* tick of the next step */
} s_typed = { -1, false, 0 };

/* Steering the pointer (steer() below) */
#define STEER_WAIT      6               /* VIDC frames for counts to show */
#define STEER_LEARN     8               /* counts a move needs to learn from */
#define STEER_HOLD      US(40000)
#define STEER_HIDDEN    100             /* VIDC frames without a pointer: there is none */

static struct {
    uint32_t frame;                     /* the VIDC frame last looked at */
    int gain[2];
    int sent[2];                        /* counts on their way, or 0 */
    int from[2];                        /* where the pointer was when they went */
    int wait, stuck;
    int hidden;                         /* VIDC frames the pointer has been off */
    uint64_t hold_until;                /* the buttons just changed */
} s_steer = { .gain = { 171, 341 } };


bool arc_kb_log;

static void send(uint8_t byte)
{
    if (arc_kb_log) {
        ARC_LOG("KB -> %02X at %u ms", byte, (unsigned)(arc_now() / 8000));
    }
    kb.rx_byte = byte;
    kb.rx_due = arc_now() + REPLY_TICKS;
    arc_schedule(kb.rx_due);
}

static void send_single(uint8_t byte, kb_state_t then)
{
    send(byte);
    kb.state = then;
}

static void send_pair(uint8_t a, uint8_t b)
{
    kb.pair[0] = a;
    kb.pair[1] = b;
    send(a);
    kb.state = KB_DAT1;
}

void keyboard_reset(void)
{
    memset(&kb, 0, sizeof(kb));
    kb.rx_due = NEVER;
    kb.poll_due = NEVER;
    s_typed.key = -1;
    s_typed.due = 0;
    s_steer.hold_until = 0;
    /* the keyboard says hello at power-on */
    send_single(HRST, KB_RESET);
    kb.poll_due = arc_now() + POLL_TICKS;
}

static void enables(uint8_t cmd)
{
    /* sticky, as Arculator: RISC OS never needs them off again */
    if (cmd & 1) {
        kb.keys_on = true;
    }
    if (cmd & 2) {
        kb.mouse_on = true;
    }
}

void keyboard_from_host(uint8_t v)
{
    if (arc_kb_log) {
        ARC_LOG("KB <- %02X at %u ms, state %d", v, (unsigned)(arc_now() / 8000), kb.state);
    }
    if (v == HRST) {
        memset(kb.down, 0, sizeof(kb.down));
        kb.keys_on = kb.mouse_on = false;
        send_single(HRST, KB_RESET);
        return;
    }
    switch (kb.state) {
    case KB_RESET:
        if (v == RAK1) {
            send_single(RAK1, KB_RESET);
        } else if (v == RAK2) {
            send_single(RAK2, KB_IDLE);
        }
        break;
    case KB_IDLE:
        if (v == RQID) {
            send_single(KBID, KB_SINGLE);
        } else if (v >= 0x30 && v <= 0x33) {
            enables(v);
        }
        break;
    case KB_DAT1:
        if (v == BACK) {
            send(kb.pair[1]);
            kb.state = KB_DAT2;
        }
        break;
    case KB_DAT2:
    case KB_SINGLE:
        if (v >= 0x30 && v <= 0x33) {
            enables(v);
            kb.state = KB_IDLE;
        }
        break;
    }
}

static int clamp63(int v)
{
    return v < -63 ? -63 : v > 63 ? 63 : v;
}

/* Steering the pointer to the touch targets. Counts a pixel on each axis
 * (x 1/256: RISC OS 3's default 640 x 256 modes move 1.5 pixels across and
 * 0.75 down a count), learnt from the moves big enough to measure. After
 * sending counts it waits until the pointer has moved (RISC OS moves it at
 * the next flyback or the one after); a pointer that doesn't move is at the
 * edge of where it may go: there as near as it gets. Buttons pressed there
 * stay down 40 ms at least. With no pointer on the screen for two seconds
 * (a game; the desktop hides it for a second when it changes mode) the
 * buttons are pressed where it is, and right away while it stays off.
 */
static void learn(int axis, int moved)
{
    int sent = s_steer.sent[axis];
    if ((sent >= STEER_LEARN || sent <= -STEER_LEARN) && moved && (moved > 0) == (sent > 0)) {
        int g = sent * 256 / moved;
        s_steer.gain[axis] = g < 16 ? 16 : g > 4096 ? 4096 : g;
    }
}

static int counts(int d, int gain)
{
    return d ? clamp63((d * gain + (d > 0 ? 128 : -128)) / 256) : 0;
}

/* There: the target's buttons, and on to the next */
static void arrived(const target_t *t)
{
    s_steer.stuck = 0;
    s_steer.sent[0] = s_steer.sent[1] = 0;
    s_steer.wait = 0;
    if (t->buttons != s_buttons) {
        arc_mouse_buttons(t->buttons);
        s_steer.hold_until = arc_now() + STEER_HOLD;
    }
    lock();
    s_targets_out++;
    unlock();
}

/* The mouse counts towards the next target, at most once a VIDC frame;
 * false: none to send (there, or waiting for the last ones to show)
 */
static bool steer(int *cx, int *cy)
{
    if (s_targets_out == __atomic_load_n(&s_targets_in, __ATOMIC_ACQUIRE) || arc_now() < s_steer.hold_until) {
        return false;
    }
    lock();
    target_t t = s_targets[s_targets_out % TARGETS];
    unlock();
    int px, py;
    bool shown = vidc_pointer(&px, &py);
    if (vidc.vidc_frames == s_steer.frame) {
        return false;
    }
    s_steer.frame = vidc.vidc_frames;
    if (!shown) {
        if (++s_steer.hidden >= STEER_HIDDEN) {
            s_steer.hidden = STEER_HIDDEN;
            arrived(&t);
        }
        return false;
    }
    s_steer.hidden = 0;
    if (s_steer.sent[0] || s_steer.sent[1]) {
        int mx = px - s_steer.from[0], my = py - s_steer.from[1];
        if (!mx && !my && ++s_steer.wait < STEER_WAIT) {
            return false;
        }
        if (mx || my) {
            learn(0, mx);
            learn(1, my);
            s_steer.stuck = 0;
        } else {
            s_steer.stuck++;
        }
        s_steer.sent[0] = s_steer.sent[1] = 0;
        s_steer.wait = 0;
    }
    /* a pixel off is there: a count may move more than one */
    int dx = t.x - px, dy = t.y - py;
    *cx = dx >= -1 && dx <= 1 ? 0 : counts(dx, s_steer.gain[0]);
    *cy = dy >= -1 && dy <= 1 ? 0 : counts(dy, s_steer.gain[1]);
    if ((!*cx && !*cy) || s_steer.stuck >= 2) {
        arrived(&t);
        return false;
    }
    s_steer.from[0] = px;
    s_steer.from[1] = py;
    s_steer.sent[0] = *cx;
    s_steer.sent[1] = *cy;
    return true;
}

/* ---- Typing (scripts, the debugger, the console) ---- */

#define TEXT        512
#define SHIFT_KEY   0x4C

static char s_text[TEXT];
static volatile uint32_t s_text_in, s_text_out;

static const uint8_t s_letters[26] = {
    0x3C, 0x52, 0x50, 0x3E, 0x29, 0x3F, 0x40, 0x41, 0x2E, 0x42, 0x43, 0x44, 0x54,
    0x53, 0x2F, 0x30, 0x27, 0x2A, 0x3D, 0x2B, 0x2D, 0x51, 0x28, 0x4F, 0x2C, 0x4E,
};

/* The key for a character on the UK keyboard, -1 if none */
static int char_key(char c, bool *shift)
{
    static const char shifted_digits[] = ")!@#$%^&*(";
    static const struct { char plain, shifted; uint8_t key; } punct[] = {
        { '-', '_', 0x1B }, { '=', '+', 0x1C }, { '[', '{', 0x31 }, { ']', '}', 0x32 },
        { ';', ':', 0x45 }, { '\'', '"', 0x46 }, { ',', '<', 0x55 }, { '.', '>', 0x56 },
        { '/', '?', 0x57 }, { '\\', '|', 0x33 }, { '`', '~', 0x10 },
    };
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        return s_letters[c - 'a'];
    }
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        return s_letters[c - 'A'];
    }
    if (c >= '1' && c <= '9') {
        return 0x11 + (c - '1');
    }
    if (c == '0') {
        return 0x1A;
    }
    for (int i = 0; i < 10; i++) {
        if (c == shifted_digits[i]) {
            *shift = true;
            return i ? 0x11 + i - 1 : 0x1A;
        }
    }
    for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]); i++) {
        if (c == punct[i].plain) {
            return punct[i].key;
        }
        if (c == punct[i].shifted) {
            *shift = true;
            return punct[i].key;
        }
    }
    switch (c) {
    case ' ': return 0x5F;
    case '\n': return 0x47;
    case '\t': return 0x26;
    default: return -1;
    }
}

void arc_type(const char *text)
{
    for (; *text; text++) {
        if (s_text_in - s_text_out < TEXT) {
            s_text[s_text_in % TEXT] = *text;
            __atomic_store_n(&s_text_in, s_text_in + 1, __ATOMIC_RELEASE);
        }
    }
}

bool arc_typing(void)
{
    return s_text_in != s_text_out || s_head != s_tail || s_typed.key >= 0;
}

bool keyboard_typing(void)
{
    return arc_typing();
}

/* The typed text's key presses into the queue, a step at a time */
static void type_next(void)
{
    uint64_t now = arc_now();
    if (now < s_typed.due) {
        return;
    }
    if (s_typed.key >= 0) {
        push((uint8_t)s_typed.key);
        if (s_typed.shift) {
            push(SHIFT_KEY);
        }
        s_typed.key = -1;
        s_typed.due = now + TYPE_GAP;
        return;
    }
    while (s_text_out != __atomic_load_n(&s_text_in, __ATOMIC_ACQUIRE)) {
        char c = s_text[s_text_out % TEXT];
        s_text_out++;
        bool shift;
        int key = char_key(c, &shift);
        if (key < 0) {
            continue;
        }
        if (shift) {
            push(SHIFT_KEY | 0x80);
        }
        push((uint8_t)(key | 0x80));
        s_typed.key = key;
        s_typed.shift = shift;
        s_typed.due = now + TYPE_HOLD;
        return;
    }
}

static bool next_event(void)
{
    if (s_tail == __atomic_load_n(&s_head, __ATOMIC_ACQUIRE) && kb.keys_on) {
        type_next();
    }
    /* keys first (the mouse's buttons are keys too); keys wait while the
     * host has them off: a key held at power-on (Delete, R) goes in once
     * the OS asks for keys
     */
    while (s_tail != __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        uint8_t ev = s_queue[s_tail % QUEUE];
        uint8_t key = ev & 0x7F;
        bool down = ev & 0x80;
        if (!kb.keys_on && key < 0x70) {
            break;
        }
        s_tail++;
        bool was = (kb.down[key >> 3] >> (key & 7)) & 1;
        if (down == was) {
            continue;
        }
        kb.down[key >> 3] ^= (uint8_t)(1u << (key & 7));
        uint8_t base = down ? 0xC0 : 0xD0;
        send_pair((uint8_t)(base | (key >> 4)), (uint8_t)(base | (key & 15)));
        return true;
    }
    if (!kb.mouse_on) {
        return false;
    }
    if (s_targets_out != __atomic_load_n(&s_targets_in, __ATOMIC_ACQUIRE)) {
        int cx, cy;
        if (!steer(&cx, &cy)) {
            return false;
        }
        send_pair((uint8_t)(cx & 0x7F), (uint8_t)(-cy & 0x7F));
        return true;
    }
    int mx = s_mouse_x, my = s_mouse_y;
    if (mx == 0 && my == 0) {
        return false;
    }
    int dx = clamp63(mx), dy = clamp63(my);
    __atomic_fetch_sub(&s_mouse_x, dx, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&s_mouse_y, dy, __ATOMIC_RELAXED);
    send_pair((uint8_t)(dx & 0x7F), (uint8_t)(-dy & 0x7F));
    return true;
}

uint64_t keyboard_next_event(void)
{
    return kb.rx_due < kb.poll_due ? kb.rx_due : kb.poll_due;
}

void keyboard_event(void)
{
    uint64_t now = arc_now();
    if (kb.rx_due <= now) {
        kb.rx_due = NEVER;
        ioc_kart_receive(kb.rx_byte);
    }
    if (kb.poll_due <= now) {
        kb.poll_due = now + POLL_TICKS;
        if (kb.state == KB_IDLE && kb.rx_due == NEVER) {
            next_event();
        }
    }
}
