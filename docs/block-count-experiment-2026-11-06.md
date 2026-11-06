# ARM2 block-transfer register-count experiment

Baseline: shared core `b6cff1f`, including the earlier operand/S dispatch
optimizations. Physical board: Tab5, ESP32-P4 rev. 1.0 at 360 MHz, 32 MB
PSRAM, ESP-IDF 5.5.6, GCC 14.2.0, `-O2`, sound enabled. The hardware
comparison uses the supported A440 model at 8 MHz, RISC OS 3.11 and 4 MB
RAM so Lander leaves measurable CPU headroom. The original A3020 setting
is restored after testing.

## Candidate

Every LDM/STM counts its 16-bit register list to calculate addresses,
writeback and emulated cycles. These instructions make up 1.8% and 2.5%
of the deterministic host Lander instruction mix, respectively.

The candidate replaces the software mask/shift/add population count with
two lookups in a 256-byte population-count table. Both bytes are masked,
preserving the helper's low-16-bit behavior for any 32-bit input. The
table is generated with compile-time macros and needs no initialization.
Transfer logic, empty-list handling and guest cycle counts are unchanged.

The linked P4 ELF places `s_count8` at `0x4ff44000`, in `.dram1.data`:
internal RAM, not a flash/cache lookup. Its 256 entries are checked
against byte population counts. The disassembly confirms the shorter
lookup sequence. `arm2_run` grows from 43,178 to 43,196 bytes (+18);
the table adds 256 bytes of internal data RAM. Register allocation and
branch layout also change, so code size alone does not predict speed.

## Correctness

- All 190 ARM2 host checks pass.
- 100,000 randomized instructions agree with Unicorn 2.1.4, seed 1.
- An exported test wrapper around the actual `count16` helper checks all
  65,536 low-word values under four upper-word patterns: 262,144 counts
  agree with the independent population-count oracle.
- All 256 bytes of the target ELF's table match the expected counts.
- The deterministic A3020 host replay still runs 88,360,660 instructions
  in frames 9,000–10,000 and produces identical screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

## Hardware workloads

Lander is launched from the SD applications disc after a fresh start.
A 30-second frame-count interval checks real-time operation; five separate
32,000-sample, 20 kHz core 1 profiles then measure interpreter residency.
Timing and profiling are separate. Both builds use the same 8 MHz model,
RAM, ROM, sound and video configuration, confirmed in startup logs.

A separate BBC BASIC-assembled workload repeatedly transfers eight
registers. It saves the initial register values to its buffer before the
loop and uses no writeback. The buffer is inside the allocated region:

```text
10 DIM c% 128
15 b%=c%+64
20 P%=c%
30 [OPT 0
40 ADR r2,b%
45 STMIA r2,{r4-r11}
50 MOV r12,#&4000000
60 .l LDMIA r2,{r4-r11}
70 STMIA r2,{r4-r11}
80 SUBS r12,r12,#1
90 BNE l
100 MOV pc,r14
110 ]
120 REPEAT:CALL c%:UNTIL FALSE
```

The listing is captured before `RUN`; the steady loop receives three
32,000-sample core 1 profiles. This tests block-transfer setup and its
fast one-page path, not every transfer mode or abort case. Host CPU tests
and differential tests provide the broader correctness coverage.

The initial baseline loop captures are 42.8%, 42.7% and 42.9% ARM2
residency (42.8% mean). Its five Lander captures are 71.1%, 70.9%, 70.9%,
70.7% and 70.5% (70.82% mean), while maintaining real time.

## Results

| A440 workload | Baseline ARM2 residency | Byte-table ARM2 residency |
| --- | --- | --- |
| Eight-register transfer loop | 42.8%, 42.7%, 42.9% | 42.0%, 42.2%, 41.7% |
| Lander | 71.1%, 70.9%, 70.9%, 70.7%, 70.5% | 70.1%, 70.6%, 69.9%, 70.0%, 69.9% |

Mean transfer-loop residency falls from **42.80% to 41.97%**, about
**1.95% relatively**. Mean Lander residency falls from **70.82% to 70.10%**,
about **1.02% relatively**. Both Lander builds maintain approximately
50 FPS; these are reductions in sampled interpreter CPU time at a paced
frame rate, not measured increases in uncapped FPS.

These are three short loop captures and five game captures per build.
Individual profiles vary, so the result is a modest improvement in these
workloads, not a universal speedup for ARM software.

## Disposition and artifacts

**Kept:** the two-byte count table. It improves both the targeted loop and
the whole-game headroom comparison for 256 bytes of internal data RAM and
18 more bytes of interpreter code. Other ARM2 instruction logic and the
4,096-entry dispatch table are unchanged.

The final firmware is the exact tested candidate, with interpreter SHA256
`fe80f6cb256b823763b66c447fd37a0ed088be49bcc2fbf052299d843932fcee`.
The original A3020/RISC OS 3.11/4 MB configuration was restored. Its final
Lander check completed 1,502 frames in 30.049 seconds: 49.985 FPS,
99.971% real time. The board was then reset and verified at the 640x480
RISC OS desktop; the serial port was released.

[Measurements](measurements/block-count-2026-11-06/) contain the patch,
CPU checks and differential results, target table verification, baseline
and candidate code/application fingerprints, disassembly excerpts, all
profiles, frame-count timing, model confirmation and screenshots.
Matching private ELFs, binaries and full logs are under
`/tmp/acorn-popcount-2026-11-06/`; ROMs, discs and network/boot logs are
not added to the repository.

The exhaustive helper check can be reproduced against the sibling core:

```sh
python3 docs/measurements/block-count-2026-11-06/check_count16.py
```
