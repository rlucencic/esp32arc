# esp32arc: Acorn Archimedes on the M5Stack Tab5

Two Archimedes, chosen on the setup screen (Model):
- **A440** (A305/A310/A440): ARM2 at 8 MHz, MEMC1a, IOC, VIDC1a, WD1772,
  512 KB to 4 MB, with Arthur 1.20, RISC OS 2.00 or RISC OS 3.11 in ROM.
- **A3020 / A4000**: ARM250 at 12 MHz (SWP), the 82C711 (floppy
  controller, IDE, serial and parallel ports), the IOEB (VIDC clocks,
  monitor ID), RISC OS 3.11 only, 2 or 4 MB, an IDE hard disc image.
  The A5000 is deliberately not done (the user's call: it would never
  reach full speed).

Bluetooth/USB keyboard and mouse, the touch screen as a touchpad,
`.adf` disc images in two floppy drives (and `.hdf` in the IDE drive), an
LVGL setup screen and a web page. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the ARM2 core (`components/arm2`, written for this project)
are the shared library in `../esp32emu`; its CLAUDE.md has the runtime and
the board-level lessons. Changes there need all its projects built, for
both boards (this one for the Tab5).

## Build, flash, debug

The Tab5 only (`/dev/cu.usbmodem3101`): `CMakeLists.txt` stops a build for
any other target. `sdkconfig.defaults` is the siblings' base (written for
the S3; its S3-only symbols give harmless "unknown kconfig symbol"
warnings); the Tab5's settings from `../esp32emu` go on top.

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build && \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash'
```

- `flash` also writes `roms/arthur120.rom`, `riscos200.rom`, `riscos311.rom`
  (not in git; `roms/README.md`) to partitions `rom_arthur120` (512K),
  `rom_riscos200` (512K), `rom_riscos311` (2M); `app-flash` only the app.
  The Tab5 is shared with the other projects: after another one, a full
  `flash`. NVS (WiFi, BLE pairings) is shared; this project's settings are
  namespace `arc` (`model`, `os`, `ram_kb`, `menu`, `cmos0`-`cmos2`: the
  A440's CMOS RAM for each OS, `cmos5` the A3020's), its drives
  `arcfloppy` (:0, :1 and the IDE disc).
- `roms/riscos310.rom` is there too (not flashed): `rom_store.c` takes 3.10
  from the SD card's `roms/` as RISC OS 3 as well.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset | profile ELF [N [TOP]]`
  with the IDF venv's Python (`-I`). `curl http://archimedes.local/api/status`.
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`emu/console.h`) and the Archimedes'
(`main/main.c`): `k <hex>` tap a key (row << 4 | column: &47 Return, &0C
F12, &34 Delete), `K <hex> 0|1`, `m dx dy btn` relative mouse (1 Select,
2 Menu, 4 Adjust), `P x y btn` the pointer to a screen position (steered,
see the core), `T <text>` type (`\n` Return; at most ~120 characters a
console line, ~12 characters a second), `R <os 0|1|2> <KB> [model 0|1]`
restart as Arthur / RISC OS 2 / RISC OS 3 with 512, 1024, 2048 or 4096 KB,
as the A440 (0) or the A3020 (1: RISC OS 3, 2048 or 4096 KB).

The touch screen is a touchpad (esp32emu's), so a place on the screen is
reached with `P x y 0` and clicked with `P x y 1` then `P x y 0` (machine
pixels): in the 640x256 desktops the icon bar's :1 is (32, 236), :0
(87, 236), Apps (132, 236). Touches anywhere: `t x y 120` a Select click
where the pointer is, `t x y 900` Menu held, `t x y ms x2 y2` a move
(the picture is 960x720 at x=160 on the Tab5). Setup screen: Start
`t 1065 637`, Model `t 433|788 227` (A440, A3020), then System (y 298),
Memory (y 369) and Menu button (y 440, `t 433|788`): for the A3020 System
`t 348 298` and Memory `t 433|788 369`; the A440's three and four buttons
are on the same lines (an `lcd` shot gives their x). (Since the shared
setup screen's rows scroll, 2026-10-21: the Amiga's first four rows,
measured there; before, 229, 291, 353, 415.) The corner long-press
`t 1270 10 2500`. The status lines show the WiFi name and IP: keep them
out of published images.

Lander on the board: `RO2Apps2.adf` (RISC OS 2 Applications Disc 2) in :0,
F12, `T run adfs::0.$.!Lander\n`; `m 0 0 1` holds thrust.

A benchmark on the board: F12 (`k 0c`), then type BBC BASIC (keywords in
capitals: Caps Lock is off in the RISC OS 3 desktop) with an ADD/CMP/BNE
loop of &4000000 turns (`10 DIM c% 100`, `20 P%=c%`, `30 [OPT 0`, `40 mov
r0,#0`, `50 .l add r0,r0,#1`, `60 cmp r0,#&4000000`, `70 bne l`, `80 mov
pc,r14`, `90 ]`, `100 CALL c%`, `RUN`; a few lines per `T`), and read
`speed` and `load` from the status while it runs.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Archimedes as an `emu_machine_t`: memory, start, CMOS saving, key map, mouse buttons, the touchpad (pixels to counts as RISC OS 3 moves the pointer), drives, status, console |
| `main/rom_store.c` | ROMs from their flash partitions, else files in the SD card's `roms/` holding that OS |
| `main/settings.c` | NVS `arc` |
| `components/archimedes/` | The machine, platform independent (`include/arc.h`) |
| `components/archimedes/host/` | Host build for testing on the Mac (see Testing) |
| `../esp32emu/components/arm2/` | The ARM2 core, its debugger adapter, disassembler, tests (`test/`) |
| `roms/` | The OS ROMs (copyrighted, not distributed) |
| `partitions.csv` | nvs, phy, factory 8 MB, the three ROMs (type 0x40), storage |

## The core (`components/archimedes`)

Hardware behaviour from MAME's `acorn_memc`, `acorn_ioc`, `acorn_vidc`,
`aa310` (LGPL; read, not copied) and Arculator (the keyboard protocol, the
memory wiring), and the ROMs' own code.

- `arc.c`: time in ticks of the memory clock (`arc.tick_mhz`: 8 on the
  A440, 12 on the A3020; the CPU's counter). Frames of 20 ms; the CPU runs
  up to the next event: the VIDC's next stop (`vidc.stop_at`: where the
  display starts or ends, every 8 display lines, every 64 flyback lines;
  the lines between are caught up after the run, see vidc.c) or the next
  other event (IOC timers, the keyboard, the FDC or 82C711 and IDE: cached
  in `s_other`, lowered by `arc_schedule()`, worked out again after the
  events and every frame; every event time set outside an event handler
  goes through `arc_schedule()`). The bus for what the page table doesn't
  map: physical RAM at 0x2000000 (first use after reset turns the ROM-at-0
  latch off), I/O at 0x3000000 (bit 21 the IOC; the A440: bank 1 the
  WD1772, bank 5 the latches; the A3020: the 82C711 at 0x3010000-0x302BFFF,
  bank 5 the IOEB), VIDC writes 0x3400000, MEMC registers 0x3600000, CAM
  0x3800000. An IOC cycle waits 0.5 µs.
- The A3020 (`swp`, 12 MHz, `c711.c`, `ide.c`, the IOEB in `arc.c`): the
  82C711's PC ports at 0x3010000 + port × 4, **read on data bits 0-15,
  written from bits 16-31**; its floppy DMA through FIQ 0 (DACK
  0x3012000, terminal count 0x302A000), IRQ B 4, the index on IRQ A 2;
  rates tell the formats apart (500 kbps HD, 250/300 DD). IDE: IRQ B 3,
  CHS with 16 heads of 63 sectors, an image with the disc record ADFS
  wants (`hd_blank.c`: templates HForm 2.19 made, 40/81/162 cylinders).
  IOEB: ID 5, the VIDC's clock (24, 25.175, 36 MHz), the monitor ID with
  the sync (`vidc_hsync()`). IOC control lines C2-C4 read 1. CMOS C7
  bits 7-6 the IDE discs.
- `memc.c`: the page table (4 KB entries, `arm2.h`) built from the CAM:
  logical pages of the current size, protection levels (0 user rw, 1 user
  read, 2-3 none; OS mode loosens), ROM at 0x3800000 (slow: 450/325/200 ns
  from the control register). CAM writes decode the physical page from
  address bits 6-0 and, by page size, 1-0 (see the lessons). The screen's
  pages are marked `ARM2_WATCH` (writes set the picture's signature).
- RAM wiring (`memc_ram_offset()`, `arc_internal.h`): which physical
  address bits reach the RAM. 2 and 4 MB plain (wrapping); below 2 MB as
  MAME's `dram_address()` row/column swap with its masks: 4 KB pages reach
  512 KB; 512 KB with 8/16 KB pages drops bit 12; with 32 KB pages 512 KB
  drops 12, 13, 20, 1 MB drops 13 and 20. Bits 0-11 always stay (a 4 KB page
  maps whole). RISC OS sizes memory by these aliases.
- `ioc.c`: IRQ A (latched: timers, vsync, POR) and B (levels), FIQ, timers
  0-3 at 2 MHz (0, 1 interrupt; 2, 3 only count, caught up when read), the
  KART (a byte to the keyboard takes 1 ms), control lines (I²C, floppy
  ready, flyback in bit 7).
- `keyboard.c`: the keyboard's microcontroller (HRST/RAK1/RAK2 handshake,
  RQID -> &81, key pairs C0/D0 + row/column, mouse pairs, BACK/SACK/SMAK
  acks; keys wait in a queue while the host has them off), typing
  (`arc_type()`: each key 40 ms down, 40 ms up), and the steered pointer
  (`arc_pointer()`, the console's `P` and host scripts): a queue of
  (x, y, buttons) steps steered by mouse counts.
- `cmos.c`: the PCF8583 bit by bit on the IOC's lines; the clock is the
  host's; RAM bytes C0/C1 the year (set at reset, the RISC OS 3 checksum in
  3F kept valid); `arc_cmos_defaults()`: each OS's Delete/R-power-on result
  with two floppies (C7 bits 2-0), made in the host build.
- `vidc.c`: registers, the raster (lines from HCR at the pixel rate,
  cached until those change; vtotal from VCR), vsync IRQ at display end,
  video DMA's bus share (charged to the CPU per display line as the lines
  are caught up: the display's lines run 8 at a time, the machine stops
  exactly where the display starts and ends; a timing register write
  stops it at once), the picture: a snapshot at each display start
  (address, palette, pointer shape) whose bytes `arc_copy_picture()`
  copies out of RAM (on the board a core 0 task, woken by
  `picture_ready`, above the presenter): three frames, the presenter's
  (taken in `arc_screen_signature()`), the last copied and the next; only
  the frame before the presenter is expected to take one is copied.
  1/2/4/8 bpp; the pointer 2 bits, 32 wide.
  `vidc_pointer()` is where it points: its shape's first pixel from the top
  left. Checked modes: 12, 13, 15 (TV), 20 (multisync 640x512), 28 (VGA
  640x480 in 256 colours at 57.14 Hz, as 24 MHz gives: 75% of the bus goes
  to the video).
- `sound.c`: MEMC's sound DMA (16 bytes at a time, buffer swap, IRQ B 1) and
  the VIDC's 8 logarithmic channels with stereo images, averaged into
  31,250 Hz samples.
- `fdc.c`: WD1772 (type I-IV commands, 32 µs a byte, 2 ms seek steps, 2 ms
  to find a sector), latches A (drive, side, motor) and B (density, reset),
  DRQ/INTRQ on FIQ, disc change on IRQ B 4 until a step.
- `floppy.c`: the images (both controllers'): in memory, written-to tracks
  back to the file (`arc_floppy_flush()`, main.c calls it between frames
  after 2 s); up to 2 MB (F and DOS HD formats on the A3020). New 800 KB
  images: an empty ADFS E disc "Blank" (RISC OS 3.11's own *Format result
  as a template, a random disc ID, the map's zone check recomputed). New
  hard disc images (40, 81, 162 MB): zeros and `hd_blank.c`'s template
  (slow on the card: over a minute for 40 MB).
- `rom.c`: the OS from its version string ("Arthur 1.", "RISC OS 2.",
  "RISC OS 3.1") and size.
- Free RAM (`arc_ram_free()`, the web page's row): application space pages
  (&8000 up to &1000000) at protection level 2. RISC OS 2 and 3 keep the
  free pool there (the running task's slot is level 0, other tasks' memory
  is mapped out): it equals the Task Manager's Free + Next (3072K + 640K =
  116 pages of 32 KB with 4 MB; 64K + 640K = 88 pages of 8 KB with 1 MB).
  Arthur gives all of it to the one program (level 0): no row there.

The ARM2 core (`../esp32emu/components/arm2`, `arm2.h`): 26-bit PC/PSR,
banked registers, exceptions, LDM/STM with ^, LDRT/STRT, rotated unaligned
LDR (not LDM), SWP (`swp`), cycle counts (S 1, N 2, I 1 tick; slow pages
`slow_s`, `slow_n`), aborts through the bus callbacks, coprocessor
instructions to the machine (none: the undefined trap, RISC OS's FPE).
`arm2_run()` is a threaded interpreter: a 4096-entry label table on bits
31-20 (the AL row by opcode, operand form and size; the other conditions
to a check of `cond_mask`, conditional B/BL to their own; NV skipped).
The PC, ticks and the fast page (`run_t.page`: the PC's page unless an
interrupt is pending or the debugger is on) are in registers and written
back around bus callbacks (which may read `arm2_now()`, change `left`,
end the run), the debugger and at the end. `arm2_end_run()` ends a run
with the ticks it used. Tests: `make -C components/arm2/test test` (190
cases), `diff.py N` against Unicorn (in a venv: `pip install unicorn`),
`build/bench` (host MIPS for four loops; no guide to the board, see the
lessons).

## Lessons (each one cost a debugging session)

- **MEMC CAM writes use address bits 1-0**: the core passed word stores'
  addresses with them cleared; with 8-32 KB pages they carry physical page
  bits. RISC OS 3's self-test failed "MEMC CAM mapping" (blinking the
  floppy LED) until the full address went to the bus.
- **Below 2 MB the RAM's wiring matters**: with RAM simply wrapping at its
  size, RISC OS 2 said 2048K on a 1 MB machine, Arthur and RISC OS 2 said
  "No keyboard present", RISC OS 3.10 never left its self-test. MAME's
  row/column swap fixed all of it.
- **Keys before the OS asks for them**: Delete held at power-on was dropped
  while the keyboard was disabled; RISC OS 3 enables it early enough,
  Arthur and RISC OS 2 don't. Keys now wait in the queue. Arthur's CMOS
  reset key is R, RISC OS's Delete.
- **Typed keys need time**: a key released 4 ms after it went down never
  made a character (RISC OS takes keys at its 100 Hz tick).
- **The pointer's tip is 21 columns into its sprite** (RISC OS 3's arrow):
  steering to the sprite's left edge missed. `vidc_pointer()` gives the
  first opaque pixel.
- **Steering must wait for the pointer to move**: RISC OS moves it at the
  next flyback or the one after; judging the counts' effect one frame later
  learnt gains of 0 or infinity and sent the pointer into a corner. Now:
  counts, then wait until it moved (up to 6 frames), learn only from moves
  of 8 counts or more, a pixel off is there (a count is 1.5 pixels across in
  640x256), no movement twice is the edge. A pointer that's off isn't a
  pointerless game until 2 s have passed: the desktop hides it for about a
  second when it changes mode (taps then clicked in place).
- **`emudbg_frame_end()` is the runtime's**: the core called it too, and the
  debugger counted two frames per frame.
- **Speed on the Tab5** (core 1, 360 MHz RISC-V): the first core needed ~126
  cycles an ARM instruction (a tight loop ran at 86%). Inlining everything
  into `arm2_run()` with the run state in locals: ~101; per-opcode handlers
  behind a 256-entry label table: ~71; LDM/STM in one page straight
  through: Arthur's desktop 90% -> 63%. **Un-merging the dispatch made it
  slower**: `-fno-gcse -fno-crossjumping -fno-tree-tail-merge` gave every
  handler its own indirect jump (48 instead of 7), but `arm2_run()` grew
  from 27 to 47 KB and Lander went from 90% to 95% (the P4's 16 KB L1
  instruction cache); internal RAM fell to 76 KB free. Keep GCC's merging.
- **IOC timers 2 and 3** (baud rates, periods of a few µs) were stepped a
  period at a time at every timer event: 3% of core 1. They're caught up
  when read.
- **The 82C711 is written on data bits 16-31** (as the IOC and its other
  devices): taking the low half, HForm said "Disc not understood" (the
  disc record went out as zeros and parts of the address).
- **HForm doesn't take Return for its defaults**: type each value.
- **`arm2_end_run()` made time jump**: it set the ticks left to 0, which
  the run then counted as used, so time went on to the run's planned end
  and the program lost the rest. With runs of a line nobody noticed; with
  runs of many lines Lander did a fifth less work (RISC OS writes the
  pointer's VIDC registers every flyback, which ends the run). It now
  takes what's left off the run's length (a test in `cases.c`).
- **Page-flipping programs and the presenter**: drawn from RAM some
  frames later, Lander's picture had black stripes (it clears the buffer
  just shown as soon as the next frame starts). Hence the copies, taken
  while the frame is shown.
- **Core 0's memory traffic slows core 1**: copying every frame (80 KB at
  70 Hz) cost Lander 6% (the P4's caches are shared: core 1's L1 data
  misses went from 1.8 to 6.3 million a second). Only the frame the
  presenter takes is copied now.
- **What the P4 pays for** (cache counters `CACHE_L1_[ID]BUS{0,1}_ACS_*`
  in `soc/cache_reg.h`, per core): a tight loop runs ~1 RISC-V
  instruction a cycle, Lander ~0.55 (~80 cycles an ARM instruction,
  ~6 million L1 instruction misses a second). Host MIPS don't predict
  the board: a 4096-entry dispatch doubled the host's speed and gave the
  board 11%; splitting the data processing handlers by S (+12 KB) helped
  the tight loop and not Lander. Moving the frame loop, lines and sound to
  internal RAM, or the exception code out of line, made no difference.
  64-bit divisions are library calls (the line length and sound had one
  each a line). The P4 has no `ror`, `ctz` or `popcount` (`__builtin_popcount`
  is a call: LDM/STM count their registers by bit tricks).

## Testing

- **Host build**: `make -C components/archimedes/host`, then
  `build/arc_host <rom> [frames] [out.ppm] [ram_kb]` (~100x real time).
  Environment: `ARC_CMOS=<file>` (none yet: the OS's defaults;
  `ARC_BLANKCMOS`: zeros), `ARC_FLOPPY0/1=<image>`, `ARC_SCRIPT=<file>`
  (`<frame> type <text> | key <hex> 0|1 | mouse dx dy | buttons b | pointer
  x y b | shot f.ppm | insert d path | eject d | reset | status | regs |
  where | peek a | dump a n | dis a n`; a line waits for typing before it),
  `ARC_WAV=<file>`, `ARC_STATUS=<n>`, `ARC_TRACE=<pc>:<n>` (`FFFFFFF`: any
  pc) with `ARC_TRACE_FRAME`, `ARC_PCLOG=<pc>:<n>`, `ARC_SWI=<frame>`,
  `ARC_KBLOG`, `ARC_FDCLOG`, `ARC_MEMCLOG`, `ARC_SNDLOG`,
  `ARC_DEBUG=<port>`. Shots are PPM (640x256 modes: double the lines for
  the picture's shape); a trace's mnemonics counted give the instruction
  mix (`ARC_TRACE=FFFFFFF:300000` from a frame).
- Checked on the host (2026-10-19): Arthur 1.20, RISC OS 2.00, 3.10, 3.11
  at 512 KB, 1, 2, 4 MB report the right size; all reach their desktops
  from the CMOS defaults with :0 and :1; Apps opens by touch, !Alarm by a
  double tap, a window drags; RISC OS 3.10's Applications Disc 1 opens
  (~6 s, the Filer reading sprites); *Format of a blank disc, *CDir, *Save,
  *Free; a new blank disc is read and written; Lander runs (mode 13, 3.2
  MIPS); BASIC's SOUND plays (~260 Hz for pitch 53).
- Host, the A3020: `ARC_MODEL=3020`, `ARC_HD=<image>` (an `.hdf`), the
  CMOS from a run with IDE discs; `ARC_MIX=<frame>` gives the instruction
  mix from that frame on (Lander: 40% data processing with a register,
  18% with an immediate, 36% conditional, 8% STRB, no multiplies). The
  A440 check: RISC OS 3.11 with apps1.adf and the floppy-open script, the
  shot at frame 2000 (md5 e73b0b95…, unchanged through the core's
  rewrites).
- Measured on the Tab5, the A440 (2026-10-19, before the A3020 work):
  100% real time on all three desktops; core 1 RISC OS 3.11 45%, Arthur
  63%, the BASIC loop 65% (3.3 MIPS), Lander 88-91% (3.4 MIPS). Internal
  RAM ~95 KB free then (the page table, 64 KB, is in internal RAM).
- Measured on the Tab5, the A3020 (2026-10-19): RISC OS 3.11 desktop
  (640x480, 16 colours, 59.94 Hz) 100% at core 1 37%, the BASIC loop
  100% at 61% (4.5 MIPS), Lander (mode 13 at 70 Hz) 98-100% at 100%
  (3.7-5.2 MIPS): no headroom. Internal RAM ~57-60 KB free while running.
- Checked there: the setup screen, starting each OS, touch (tap = Select,
  hold = Menu; when it still put the pointer under the finger), floppy upload/insert/open, a blank disc from the web page,
  a blank 40 MB hard disc made and used by the A3020, Lander's picture
  without stripes, the debugger (hello, pause, step, next, read,
  continue). Not checked by ear or hand: sound, Bluetooth and USB
  keyboards and mice, the Tab5 Keyboard.
- **Free disk space** (web page, `space.c`): ADFS old and new map, DOS
  FAT on floppies, counted on the emulation task when a disc goes in and a
  second after its last write (a disc put in before Start: none until it
  runs). Host: RISC OS 3.11's `*Free` on nine discs; Tab5 (2026-10-21):
  RISC OS 2's Applications Disc 2: 327680 of 819200, a blank 40 MB IDE disc 41773056.
- **Touchpad** (Tab5, 2026-10-21, RISC OS 3.11 on the A3020, the 640x480
  desktop): 150 panel pixels move the pointer 99 pixels each way (the counts
  rule in `main.c` holds there); a Select tap opened Apps, the long press
  its Filer menu. The 256-line modes not measured.

## Open items

- Lander plays no sound: from its start to a crash it calls no Sound SWI and
  no OS_Word 7 (the Sound_Configure/Stereo calls seen are the ROM's own, at
  mode changes), and the sound DMA plays silence. Whether the demo has
  sound on a real machine is unknown. The OS's sounds work.
- The VS Code extension (`../esp32emu-vscode`) knows the 6502, 65C816, Z80
  and 68000: the ARM2 (`arch` "arm2", 26-bit addresses) needs its stack
  view and disassembly window there.
- More speed for heavy games on the A3020 (Lander has no headroom): the
  P4's caches are the limit, not instruction counts (see the lessons); a
  decoded-instruction cache, say. The MaTouch hasn't been looked at.
- The A440 wasn't measured on the board again after this session's core
  and timing changes (the host's checks pass).
- Not tried: 8 bpp desktops, the podule/backplane space, the A3000. No
  serial, printer or Econet devices, no `.apd`/`.jfd` copy-protected
  images, no ST506 hard disc on the A440.
- The CMOS clock isn't set back (the host's time is the clock).
- **Clock** (the clock audit, 2026-10-21: `../esp32emu/CLAUDE.md`, To do):
  the PCF8583's writes are ignored (`cmos.c`): setting the time in RISC OS
  does nothing. The year bytes are set at reset only from a board year of
  2024 or later.

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing (the setup screen's status lines show both).
