# VIDC sound accumulator width experiment

Date: 2026-11-06. This experiment narrows the mixer's private integration
accumulators from signed 64-bit to signed 32-bit, keeping its sampling,
rounding, high-pass filter, DMA and ring-buffer behavior.

## Exact arithmetic bound

The logarithmic decoder's maximum magnitude is
`((16 + 15) * 128 - 16) * 8 = 31,616`. A stereo gain is at most 256.
Each accumulated output sample integrates at most 32 microseconds, so
`abs(acc) <= 31,616 * 256 * 32 = 258,998,272 < INT32_MAX`. This bound also
covers mixed signs and changing gains during the sample. Each individual
product fits, and the sum of absolute weighted contributions fits.

The loop already limits `step` to `32 - acc_us`, and `emit` resets both
accumulators and `acc_us`. Multiplication uses an explicitly signed
32-bit `step`; retaining an unsigned multiplication would change its
C arithmetic semantics for negative levels. Division still truncates
toward zero, with the same denominator 8,192. No approximation is added.

## Correctness validation

The original and candidate sound sources were compiled separately with
UBSan and compared using the same driver. Serialized state and all
rendered PCM bytes match exactly:

- 135,168 direct integration/emit cases: all 256 logarithmic samples,
  eight stereo images, every split of a 32-us sample and both unchanged
  and opposite signs.
- 4,096 timelines: both 8/12-MHz machine clocks, all 256 sound frequency
  register settings and eight stereo patterns, each with 256 irregular
  time advances. They cover live frequency/stereo writes, DMA off/on,
  output disable/enable, buffer changes and IRQ counts.
- Long catch-up through the 64-bit tick-division fallback, a full output
  ring, fractional ticks, muted output and reset behavior.

The trace SHA256 is
`09b50d7dcd688d74d2cfbbf5f941231556f83cfb62ea85fd30dd4cabe6087f9e`.
The accumulator bound was asserted throughout. Both sources complete
without UBSan diagnostics.

A 4,000-frame A3020 host replay boots RISC OS 3.11, enters BASIC at frame
2,010 and types `SOUND 1,-15,53,100` at frame 2,150. Both builds have the
same 30,211,349 measured instructions from frame 3,000 and identical
reported CPU/device state and instruction mix. Screenshots confirm the
command was entered. The output WAV matches byte for byte (5,000,004
PCM values, 4,477,170 nonzero; range -15,873 to 15,860), SHA256
`eefff4a4442e70e447868a3125c149715de0f9f2120421b7f954e971ac598db6`.
The final PPM SHA256 is
`a697cbfa7c43c8a5f9f9ee04e9f15dca952ad6e9bfeffa07f1379a03b4a991fb`.

## P4 generated code and comparison method

ESP-IDF 5.5.6 / GCC 14.2.0. `sound_run` shrinks from 1,018 to 934 bytes
(84 fewer), with 315 disassembled instructions versus 339. Two signed
multiply-high instructions used for accumulation disappear; the remaining
multiply-high instruction belongs to the tick-to-microsecond conversion.
The private sound state shrinks from 120 to 112 bytes.

ARM2 source is unchanged. The interpreter retains its 43,154-byte size,
address and instruction locations/mnemonics, with 62 changed encodings in
address-immediate instructions (`addi`: 1, `auipc`: 3, `jalr`: 58).
These linker effects mean the test measures the complete firmware change,
without attributing every elapsed cycle specifically to sound arithmetic.
Fresh restored-baseline and candidate rebuilds each match their saved
firmware's 6,242 defined functions at identical addresses and bytes.

Hardware comparisons use the same opt-in
[whole-machine frame-call cycle capture](frame-cycle-measurement-2026-11-06.md)
on Tab5 at 360 MHz: A440 / ARM2 8 MHz, RISC OS 3.11, 4 MB, Lander in
320x256 8-bit mode, with sound enabled. Each build is flashed and launched
using the same `--boot` sequence, with a separate 30-second guest frame
pacing check before capture. Device work, interrupts and preemption inside
`arc_run_frame` are included; audio/display pacing waits are excluded.
This is elapsed frame-call cost, not isolated mixer execution time.

## Hardware results (retained)

| Run | Captured frames | Cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Initial baseline | 1,250 | 4,801,918.590 | 61,382.0832 | 78.22997 | 49.9937 |
| First candidate | 1,250 | 4,786,131.098 | 61,376.7528 | 77.97954 | 49.9973 |
| Restored baseline control | 2,500 | 4,801,520.241 | 61,386.5708 | 78.21776 | 49.9844 |
| Repeated candidate | 2,500 | 4,778,501.651 | 61,386.5708 | 77.84279 | 50.0226 |

The first pair uses five 250-frame windows per build; the second pair
uses five 500-frame windows. Frame cost falls 0.329% in the first pair
and 0.479% in the second. Guest instruction totals differ by -0.0087%
in the first pair and are identical in the second. Pooled over 3,750
captured frames per build, cost falls from 4,801,653.024 to 4,781,044.800
cycles/frame (-0.429%). Each build still runs near the 50 FPS pacing cap;
this is a small reduction in CPU-clock cost, not an observed FPS gain.

The two baseline means differ by 0.0083%. All five longer candidate
windows are below all five longer baseline windows. All twenty captures
complete with no invalid frames and the same observed 360 MHz clock.
The paired results support retaining this small change for the tested
workload, without claiming general emulator speedups or isolating sound
cost from linked-code/data layout and preemption.

Individual window results (cycles/frame):

- Initial baseline, 250 frames/window: 4,805,258.848; 4,796,514.744; 4,806,409.068; 4,788,709.372; 4,812,700.920.
- First candidate, 250 frames/window: 4,788,606.936; 4,785,754.008; 4,789,385.840; 4,787,987.056; 4,778,921.648.
- Restored control, 500 frames/window: 4,798,805.668; 4,804,365.454; 4,803,953.812; 4,798,257.120; 4,802,219.150.
- Repeated candidate, 500 frames/window: 4,782,307.946; 4,780,450.588; 4,781,939.086; 4,776,617.516; 4,771,193.120.

Raw firmware, disassembly, reference driver, state traces, replay inputs,
WAVs and measurement captures remain outside the repositories under
`/tmp/acorn-sound-accum-2026-11-06`. Only the source change and this report
are retained. ARM2 execution source is unchanged.

## A3020 follow-up

The retained build runs Lander at 49.9830 FPS on the ARM250 12 MHz model.
Three 250-frame captures complete at 360 MHz with no invalid frames:
6,875,872.144; 6,883,481.568; and 6,878,804.132 cycles/frame.
The game screenshot was verified. These are sanity measurements of the
retained build; there is no fresh A3020 baseline comparison in this
experiment, so the A440 percentage gains are not applied to this model.
The board was returned to the A3020's 640×480 desktop, the screenshot
verified and the serial port closed. No capture remains running.
