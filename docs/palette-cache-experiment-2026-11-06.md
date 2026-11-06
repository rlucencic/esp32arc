# VIDC palette lookup-table reuse experiment

Date: 2026-11-06. The baseline includes the retained
[sound-accumulator optimization](sound-accumulator-experiment-2026-11-06.md).

## Change and invalidation

The video snapshot previously rebuilt all 256 entries of the active
4-bit paired-pixel or 8-bit colour lookup table at every display start.
Both tables already live in the emulation thread's private `s_now` picture.
A one-byte validity mask now permits reuse until any palette register
write (`00`–`3C`) or a VIDC reset invalidates both tables.

Each table has its own validity bit. Visiting a different pixel depth
does not lose the other table's validity, while palette writes in any
mode invalidate both. A return to 4- or 8-bit mode therefore either
reuses a valid table or rebuilds it before taking the snapshot. Even a
write of the same colour conservatively invalidates the tables.

The palette register arrays have no other production writers outside
`vidc_write` and `vidc_reset`. New direct writers would also need to
invalidate the mask. Cursor/border colours are still captured on every
snapshot. The presenter's three-frame ownership and copies are unchanged:
lookup tables remain copied into each frame's own picture, so a palette
write cannot recolour a previously copied or pinned frame.

No lookup table grows. Only the validity mask is new; it is accessed on
the emulation thread along with the existing snapshot and register writes.
ARM2 and sound execution sources are unchanged by this experiment.

## Correctness validation

Original and candidate VIDC implementations were compiled separately
under UBSan and exercised by the same reference driver. Their
serialized VIDC/snapshot state, copy/signature markers and rendered rows
match exactly. The deterministic gzip-compressed traces have SHA256
`9a63dcce0e3de8aece52ee08e684ef8d73eb2d006f301613152ade92812d446e`.
Both drivers complete without UBSan diagnostics.

The driver covers 131,072 palette-write cases: all 4,096 twelve-bit
colours in each of sixteen registers, with both immediate copying and
simulated deferred presenter copies. It takes 548,864 snapshots, including
unchanged repeats, all four pixel depths, mode changes that revisit a
previous table, high register bits, palette updates while another frame
is pinned, border/cursor updates, RAM wrapping and 98 resets while frame
storage remains allocated. It compares 5,726,208 rendered rows, including
out-of-range rows. Delayed copies simulate ownership transitions; this is
not a host multithreaded race test.

A separate 6,000-frame RISC OS 3.11 A3020 replay enters BASIC and switches
among display modes 13, 12, 0 and 1, exercising 1-, 2-, 4- and 8-bit pixel
formats. All seven screenshots match byte-for-byte, as do reported
CPU/device state and the full instruction mix. Both execute 33,794,619
measured instructions from frame 5,000. The final 320x256 8-bit screenshot
shows the typed test text and has PPM SHA256
`4fc4d24353d1ceeb821113b0b2968f07f8af9a38f0140d8b058c07335fd74843`.

## P4 build and hardware method

ESP-IDF 5.5.6 / GCC 14.2.0. `snapshot` is inlined into `vidc_line_end`;
that function grows from 1,710 to 1,788 bytes. `vidc_write` and `vidc_reset`
each grow by eight bytes, for 94 more bytes across the three functions.
The linked `.flash.text` section grows by 128 bytes after alignment.
The validity variable itself occupies one byte; this target layout adds
eight bytes to `.dram1.bss` and eight bytes of heap-start alignment padding.

The ARM2 interpreter retains its 43,154-byte size, address, instruction
locations and mnemonics. Its 61 changed instruction encodings are in
address-immediate instructions (`jalr`: 58, `auipc`: 3). These linker
changes mean the comparison measures the full firmware change, rather
than isolated palette-table cycles. A fresh restored-baseline build
matches all 6,242 saved baseline functions at identical addresses and bytes.

Tests use the same opt-in
[whole-machine frame-call cycle capture](frame-cycle-measurement-2026-11-06.md)
in each build: Tab5 at 360 MHz, A440 / ARM2 8 MHz, RISC OS 3.11, 4 MB,
sound enabled, Lander in 320x256 8-bit mode. Each run starts from a fresh
flash with the same `--boot` sequence, has a separate 30-second guest
pacing check, then five captures of 500 machine frames each.

## Hardware results (retained)

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 4,796,717.274 | 61,378.1804 | 78.15020 | 49.9960 |
| Palette-table reuse | 4,771,596.051 | 61,379.1584 | 77.73968 | 50.0029 |
| Restored baseline control | 4,799,287.318 | 61,380.0756 | 78.18966 | 49.9994 |

Each row captures 2,500 machine frames. The candidate costs 0.524% fewer
cycles/frame than the initial baseline and 0.577% fewer than the restored
control. Guest instructions/frame differ by +0.0016% and -0.0015%,
respectively; the frame-call cost per guest instruction falls 0.525% and
0.576%. All builds remain near the 50 FPS pacing cap. This is reduced
CPU-clock cost for the tested workload, not an observed FPS increase.

The two baseline means differ by 0.054%. All five candidate windows are
below all ten baseline/control windows. All fifteen captures have no
invalid frames and the same observed 360 MHz clock. One candidate boot
was compared with two baseline boots; the five windows within a boot
are not five independent boots. The fresh restored control supports
retaining the change, without claiming general gains for palette-heavy
software or attributing all elapsed-cycle changes solely to table reuse.

Individual cycles/frame:

- Initial baseline: 4,795,709.730; 4,795,809.594; 4,791,554.560; 4,799,681.690; 4,800,830.798.
- Candidate: 4,768,730.834; 4,772,139.198; 4,772,436.358; 4,773,827.670; 4,770,846.196.
- Restored control: 4,799,737.992; 4,805,783.920; 4,800,810.666; 4,788,738.822; 4,801,365.190.

Raw firmware, disassembly, reference driver and traces, replay scripts,
screenshots and cycle captures stay outside the repositories under
`/tmp/acorn-palette-cache-2026-11-06`. The source change and this report
are retained. The earlier sound-accumulator optimization remains enabled.

## Fresh A3020 comparison

The retained build was also tested on the ARM250 12 MHz model. Its
initial sanity capture differed from an older sound-only run, so a
freshly flashed sound-only baseline was measured with the same setup
rather than treating historical numbers as a control. The candidate
was measured first, then the baseline; each used the same `--boot`
sequence, separate 30-second pacing check and three 250-frame captures.

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Fresh sound-only baseline | 6,912,518.948 | 88,338.8347 | 78.25006 | 49.9998 |
| Palette-table reuse | 6,898,677.704 | 88,341.4973 | 78.09102 | 49.9980 |

The candidate costs 0.200% fewer cycles/frame, with 0.0030% more guest
instructions/frame; the frame-call cost per guest instruction falls
0.203%. Candidate windows are 6,899,137.240; 6,894,557.416; and
6,902,338.456 cycles/frame. Baseline windows are 6,917,186.832;
6,910,283.372; and 6,910,086.640. Every candidate window is below every
baseline window. All six captures complete without invalid frames at
360 MHz. This is a small gain from one fresh boot per build on this
model, with no observed FPS change at the pacing cap.

The earlier A3020 run was not a contemporary baseline; its difference
from this experiment illustrates why historical captures should not
replace a fresh control. The game screenshot was verified before the
baseline run, and the candidate firmware was reinstalled afterward.
The retained firmware was returned to the A3020's 640×480 desktop, its
screenshot verified and the serial port closed. No capture remains running.
