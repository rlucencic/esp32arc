# VIDC line-timing cache experiment

Date: 2026-11-06. The baseline includes the retained sound-accumulator
and palette-table optimizations.

## Change and refresh points

`vidc_line_ticks` previously loaded and compared four timing dependencies
on every query: HCR, VIDC clock, pixel rate and machine tick frequency.
It recalculated the whole ticks, remainder and denominator only when a
comparison changed. The candidate moves that calculation to the writes
that can change those inputs, leaving a reader that only applies the
fractional remainder and existing carry correction.

The refresh points are VIDC reset, HCR (`80`) writes, control/pixel-rate
(`E0`) writes and IOEB clock writes. IOEB now uses a private-module
`vidc_set_clock` function instead of assigning the clock field directly.
The machine's 8/12-MHz tick rate is set by `arc_init` before VIDC reset;
it has no other production writer. The other timing inputs also have
no production writers outside the audited reset/register paths.
Future direct writers would need to refresh the timing cache as well.
Repeated writes conservatively recalculate even when their effective
inputs are unchanged.

The denominator, remainder, fallback 64-us line and unsigned fractional
carry algorithm are unchanged. In particular, a fraction left from a
larger denominator still uses the existing multi-carry correction after
a mode/clock change. Updating the cache does not consume a fraction or
change the register-write scheduling rules. Cache refresh happens after
the new HCR/control fields or clock have been stored.

Status queries also call this helper. They now read cached values and
update only their caller-owned fraction, rather than writing timing
cache state. This does not make the full machine status an atomic
snapshot; its existing asynchronous status semantics are unchanged.
ARM2, audio, palette conversion and frame ownership are unchanged.

## Correctness checks

Separate original/candidate VIDC builds under UBSan produce identical
binary timing/state traces, SHA256
`8db0575cac824759cfbeba9dec1a780965fed90c5802c7226cdf7ac1f19f5e5a`.
Neither emits UBSan diagnostics. The driver covers:

- 24,576 combinations of all 1,024 HCR fields, all four pixel rates,
  all three distinct IOEB clocks (24,000 / 25,175 / 36,000 kHz), and
  both 8/12-MHz machine clocks. Each uses ten initial fraction values
  and seventeen consecutive line queries, covering the unset-HCR
  fallback, denominator boundaries, oversized remainders and unsigned wrap.
- 98,304 horizontal-sync queries around different positions in a line.
- 131,072 changing-input sequences carrying fractions across clock/mode
  changes, repeated equivalent writes, unrelated border writes and resets.
- 40,000 actual raster steps with HCR/control/clock changes, display
  events, DMA tick charges, sync IRQs, batched stop points and independent
  status-style fraction queries. Serialized VIDC state, CPU ticks and
  scheduling/IRQ markers match.

A separate 6,000-frame RISC OS 3.11 A3020 replay switches BASIC among
modes 13, 12, 0 and 1. All seven screenshots and all reported CPU/device
state and instruction-mix output match. Both execute 33,794,619 measured
instructions from frame 5,000. The final PPM SHA256 is
`4fc4d24353d1ceeb821113b0b2968f07f8af9a38f0140d8b058c07335fd74843`.

## P4 build and measurement

ESP-IDF 5.5.6 / GCC 14.2.0. The `vidc_line_ticks` reader shrinks from
258 to 52 bytes; calculation moves into a 150-byte helper and a 12-byte
clock setter. Some callers inline the smaller reader. Total linked
flash-text size is unchanged; aligned BSS shrinks by 16 bytes.

ARM2 source is unchanged. Its interpreter retains its 43,154-byte size,
address, instruction locations and mnemonics. The 59 changed encodings
are in address-immediate instructions (`jalr`: 58, `auipc`: 1). The
measurements therefore include linker effects, rather than isolating
only the removed dependency checks.

The same opt-in
[whole-machine frame-cycle hook](frame-cycle-measurement-2026-11-06.md)
is used in each build. Tab5 remains at 360 MHz, with RISC OS 3.11,
4 MB and sound enabled. Lander runs in 320x256 8-bit mode. Runs use the
same fresh-flash `--boot` startup and a separate 30-second guest pacing
check before cycle capture. Frame-call cycles include device work,
interrupts and preemption, while audio/display pacing waits are excluded.

## A440 measurements

Each build captures five windows of 500 machine frames (2,500 total).

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 4,791,966.837 | 61,378.4512 | 78.07246 | 49.9989 |
| Write-refreshed timing cache | 4,770,919.408 | 61,378.3104 | 77.72973 | 49.9970 |
| Restored baseline control | 4,781,519.298 | 61,377.4976 | 77.90346 | 49.9994 |

The candidate costs 0.439% fewer cycles/frame than the initial baseline
and 0.222% fewer than the restored control. Guest instructions/frame
remain within 0.0014%; the frame-call cost per instruction falls 0.439%
and 0.223%, respectively. All five candidate windows are below all ten
control windows. No capture has invalid frames or an observed clock change.

The baseline means differ by 0.218%, which is material next to this small
gain. These are one candidate boot and two baseline boots, not five
independent boots per row. The results favour the candidate on this
workload, without establishing a single precise general speedup.
All builds retain approximately 50 FPS at the pacing cap.

Individual cycles/frame:

- Initial baseline: 4,794,564.212; 4,790,710.256; 4,794,154.952; 4,786,064.920; 4,794,339.846.
- Candidate: 4,770,752.438; 4,772,430.928; 4,772,492.910; 4,770,582.416; 4,768,338.350.
- Restored control: 4,783,839.698; 4,777,517.210; 4,794,092.844; 4,775,946.908; 4,776,199.828.

Fresh baseline and candidate rebuilds match their saved firmware's
6,242 and 6,244 defined functions, respectively, at identical addresses
and bytes. Whole binaries can still differ in metadata.

Raw builds, disassembly, reference driver/traces, replay inputs,
screenshots and timing captures remain outside the repositories at
`/tmp/acorn-line-cache-2026-11-06`.

## Fresh A3020 comparison

The ARM250 12 MHz model was measured separately, with one fresh candidate
boot followed by one fresh baseline boot. Each uses the same startup,
30-second pacing check and three 250-frame captures (750 frames total).

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Palette-optimized baseline | 6,885,921.489 | 88,338.8347 | 77.94897 | 49.9960 |
| Write-refreshed timing cache | 6,879,247.019 | 88,373.6867 | 77.84271 | 49.9951 |

Candidate mean cost is 0.097% lower per frame, with 0.0395% more guest
instructions/frame; frame-call cost per guest instruction is 0.136%
lower. These differences are small next to the capture ranges, and
one boot per build does not establish a useful A3020 speedup. The
A440 percentage gains are not extrapolated to this model. Both
builds hold approximately 50 FPS; all six captures are valid at 360 MHz.

- Baseline cycles/frame: 6,883,302.036; 6,891,076.824; 6,883,385.608.
- Candidate cycles/frame: 6,877,799.232; 6,876,709.292; 6,883,232.532.

## Outcome

Retained for the modest measured A440 benefit, smaller per-line reader
and 16-byte BSS saving, with exact reference/replay output preserved.
There is no demonstrated A3020 throughput gain. Software that repeatedly
writes unchanged timing inputs now recalculates at those writes; its
performance was not measured here. This is a workload-specific result,
not a general emulator speedup or a claim of isolated CPU-core savings.
The existing sound and palette optimizations remain enabled.

The candidate A3020 Lander screenshot was verified before the baseline
run. The candidate firmware was reinstalled afterward.
The retained firmware was returned to the A3020's 640×480 desktop,
its screenshot verified and the serial port closed. No capture is running.
