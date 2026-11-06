# IOC timer-count division experiment

Date: 2026-11-06. The baseline includes the retained VIDC line-timing,
palette-table and sound-accumulator optimizations.

## Candidate

`timer_count` computes the timer's remaining machine ticks as a 64-bit
value, then divides by `arc.tick_mhz / 2` to obtain the IOC's 2 MHz
counter. The candidate uses a 32-bit division when the remaining ticks
fit in `UINT32_MAX`, keeping the original 64-bit expression otherwise.
The overdue-timer catch-up calculation remains entirely unchanged.

For normal running timers, the remaining duration is bounded by one
period: at most 65,537 counter ticks, or 393,222 machine ticks at 12 MHz.
Changing a latch before GO can leave the previous period in flight, but
it obeys the same maximum. The wider guard also handles representable
future values beyond this normal bound without narrowing their meaning.

For a positive 32-bit divisor and a dividend that fits in 32 bits,
casting before division produces the same quotient as the original
64-bit division. Larger dividends retain the original calculation and
its cast to 32 bits. Expiry arithmetic, latch/out registers, stopped
states, IRQs and scheduler calls are unchanged.

## Correctness checks

Original and candidate IOC sources, linked with the real ARM2 IRQ/FIQ
setters, produce byte-identical reference traces under UBSan. SHA256:
`739d6cef56e25909f00ff636ab504a6d4864799f5f504e9c4282331725317b2a`.
Neither build emits UBSan diagnostics. The driver covers:

- 7,864,320 scalar cases: both 8/12-MHz machine clocks, four timers,
  all 65,536 latch values and fifteen expiry placements. These include
  stopped timers, count boundaries, overdue periods, large catch-up,
  `UINT32_MAX` boundaries, wide future values and the far timestamp limit.
  Count reads, output latching, low/high bytes and resulting expiry match.
- 200,000 programmed sequences: latch rewrites before GO, time advances,
  large gaps, IRQ masks/acknowledgements, events and keyboard traffic.
  IOC state, schedule markers, next-event timestamps and CPU interrupt
  state match.
- 100,352 synthetic wrap/boundary cases near zero, 2^32 and 2^64, with
  sampled latches and all timer indices. The wide fallback preserves
  the original behavior even where expiry arithmetic wraps.

A 6,000-frame RISC OS 3.11 A3020 mode-switch replay also matches all seven
screenshots, reported device/CPU state and the full instruction mix.
Both builds execute 33,794,619 measured instructions from frame 5,000.
The final PPM SHA256 is
`4fc4d24353d1ceeb821113b0b2968f07f8af9a38f0140d8b058c07335fd74843`.

## P4 build and method

ESP-IDF 5.5.6 / GCC 14.2.0. `timer_count` is inlined into `ioc_write`.
That function grows from 868 to 878 bytes. Disassembly confirms a
hardware `divu` on the guarded path, with separate `__udivdi3` call sites
for overdue catch-up and the wide fallback. Linked flash-text and RAM
section sizes are unchanged after alignment.

ARM2 source is unchanged. Its interpreter retains its 43,154-byte size,
address, instruction locations and mnemonics. The only changed interpreter
encodings are 58 `jalr` address immediates. The whole-firmware comparison
therefore includes linker effects and does not isolate timer-read cycles.

Hardware runs use the same opt-in
[whole-machine frame-cycle capture](frame-cycle-measurement-2026-11-06.md):
Tab5 at 360 MHz, A440 / ARM2 8 MHz, RISC OS 3.11, 4 MB and sound enabled,
Lander in 320x256 8-bit mode. Each run uses the same fresh-flash `--boot`
sequence, separate 30-second guest pacing check, and five 500-frame
cycle windows. Frame-call cycles include device work and preemption,
while runtime audio/display pacing waits remain outside the bracket.

## Hardware results (no useful gain; reverted)

Each row captures 2,500 machine frames.

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 4,768,807.606 | 61,378.3104 | 77.69532 | 49.9992 |
| Guarded 32-bit divide | 4,780,974.012 | 61,380.6856 | 77.89053 | 49.9986 |
| Restored baseline control | 4,786,073.528 | 61,384.4288 | 77.96885 | 49.9863 |

Candidate frame cost is 0.255% higher than the first baseline and 0.107%
lower than the restored control. Guest instructions/frame differ by
+0.0039% and -0.0061%, respectively. The candidate windows all exceed the
initial baseline windows but overlap the restored control windows.
The two baseline means differ by 0.362%, more than either apparent
candidate effect. This establishes no consistent useful gain, not a
precise regression or improvement. All builds remain paced near 50 FPS;
all fifteen captures complete without invalid frames at 360 MHz.

Individual cycles/frame:

- Initial baseline: 4,769,619.670; 4,769,012.882; 4,768,187.880; 4,762,509.238; 4,774,708.360.
- Candidate: 4,783,946.912; 4,782,662.654; 4,782,830.732; 4,776,959.454; 4,778,470.310.
- Restored control: 4,780,340.440; 4,783,556.712; 4,786,381.480; 4,788,817.230; 4,791,271.776.

The guard was reverted and the exact saved baseline app restored.
A fresh baseline rebuild matches all 6,244 saved defined functions at
identical addresses and bytes, despite possible whole-binary metadata
changes. The comparison does not isolate timer-read frequency, guard
cost, call overhead, linker effects or the source of run-to-run variation.
The instruction-level simplification alone does not justify retaining it.

Candidate hardware performance was tested on A440 only; the A3020 host
replay validates correctness, not an A3020 hardware speedup. The prior
sound, palette and line-timing changes remain enabled.

Raw builds, rejected patch, reference driver/traces, replay inputs,
screenshots and measurement captures remain outside the repositories
under `/tmp/acorn-ioc-count-2026-11-06`. Only this report is retained.

## Restored-baseline profile

After all cycle captures finished, one additional 32,000-sample core-1
profile was collected at 20 kHz on the restored A440 Lander workload.
It is a hotspot scout, not part of the cycle comparison:

- `arm2_run`: 22,042 samples (68.881% of all samples).
- Idle `esp_cpu_wait_for_intr`: approximately 28.1% (rounded symbol report).
- `sound_run`: 501 samples (1.566%).
- `ioc_read`: four samples; `ioc_event`: three; `ioc_write` itself: zero.

The absence of samples in `ioc_write` does not measure its call frequency
or attribute out-of-line division work to its callers. One periodic
sampling burst also cannot explain the preceding timing variation.
It does put CPU execution ahead of more small IOC arithmetic changes.
The hottest ARM2 PC is `0x4ff02e44` (413 samples), the guest opcode `lw`
inside the shared fetch/decode/dispatch tail. Its source mapping after
block transfer does not make that load a block-transfer-specific cost.
Fetch/dispatch remains an investigation target; previously rejected
cached-pointer and compiler-tail variants are not newly validated by
this profile.

The restored firmware was returned to the A3020's 640×480 desktop,
its screenshot verified and the serial port closed. No capture is running.
