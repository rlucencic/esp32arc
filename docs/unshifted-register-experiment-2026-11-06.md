# ARM2 unshifted-register dispatch experiment

Follow-up to [dispatch-tail experiments](dispatch-tail-experiment-2026-11-05.md).
Baseline: shared repository `6a4cd8d` (CPU code from `7cf10ca`), with the
retained shift-field and S-bit dispatch optimizations. Hardware remains
Tab5, ESP32-P4 rev. 1.0 at 360 MHz, A3020/ARM250 at 12 MHz, 4 MB RAM,
RISC OS 3.11, sound enabled, ESP-IDF 5.5.6, GCC 14.2.0 and `-O2`.

## Workload evidence

The host instruction-mix report now counts unshifted register operands
and divides them by S. The deterministic Lander replay still executes
88,360,660 instructions from frame 9,000 to 10,000. Of these, 18,030,700
use plain register data-processing operands: 20.4% of all instructions,
or 51.2% of register-operand data-processing instructions. Of the plain
operands, 11,112,986 have S clear and 6,917,714 have S set. These counts
include only instructions whose condition passes.

## Candidate

Previously each register data-processing handler checked bit 4 for a
register-specified shift, then operand decoding checked bits 5–11 for
an immediate shift. The candidate checks bits 4–11 together. A zero field
selects a specialized plain-register operand form; the remaining path
keeps opcode-specific shifted-immediate handling and the existing shared
register-specified shift/multiply/SWP handling.

Plain Rm retains its PC+8/PSR behavior for R15, Rn retains PC+8, and the
shifter carry preserves the old C flag. The S-specialized handlers keep
their existing behavior. No guest instruction timing is changed.

The P4 disassembly confirms the combined entry check. It also exposes
register-allocation and scheduling changes, so fewer source-level checks
alone are not accepted as evidence of speed. `arm2_run` grows from 38,478
to 43,706 bytes, an additional 5,228 bytes (13.6%).

## Correctness

- All 190 CPU checks pass.
- All 100,000 randomized instructions agree with Unicorn 2.1.4, seed 1.
- The 10,000-frame host replay has identical instruction counts, mix,
  unshifted-register counts and screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

## Hardware workloads

Lander uses `tools/measure_lander.py`, with a fresh flash/launch and a
60-second frame-count interval without HTTP polling or profiling during
timing. The baseline is close to the 50 FPS pacing limit.

A separate BBC BASIC-assembled loop tests the targeted register operands:

```text
10 DIM c% 100
20 P%=c%
30 [OPT 0
40 mov r0,#0
45 mov r1,#1
47 mov r2,#&4000000
50 .l add r0,r0,r1
60 cmp r0,r2
70 bne l
80 mov pc,r14
90 ]
100 REPEAT:CALL c%:UNTIL FALSE
```

This replaces the earlier benchmark's immediate ADD/CMP operands with R1
and R2. The listing is captured before `RUN`. Three 32,000-sample, 20 kHz
profiles compare core 1 residency while guest execution remains paced at
real time. This is a targeted CPU workload, not an uncapped game benchmark.

## Initial baseline

Lander completes 3,006 frames in 60.131 seconds: 49.991 runtime FPS,
99.982% real time. Register-loop `arm2_run` residency is 62.9%, 62.3% and
62.3% (62.5% mean).

## Refinement: reuse the decoded field

The first candidate leaves a redundant zero-field check in the shifted
path. A second candidate extracts bits 4–11 once, uses its low bit for
register-specified shifts, and passes the same field to operand decoding
for shifted-immediate operands. That path is now reached only for a
nonzero field with bit 4 clear; plain operands use the explicit plain
form. This removes the duplicate zero test and re-extraction.

The refined interpreter is 43,178 bytes, 528 bytes smaller than the first
candidate and 4,700 bytes larger than baseline. It independently passes
all 190 checks, 100,000 differential instructions, and the identical host
replay. Both candidate patches are preserved.

The first candidate reaches 50.008 FPS (100.015% real time), which is
effectively the same capped Lander result as baseline. Its register-loop
residency is 60.1%, 60.8% and 60.3%: 60.4% mean versus 62.5% baseline.
This demonstrates a gain for the targeted loop; it is not evidence of
an uncapped Lander speedup.

The refined candidate also reaches the Lander pacing limit: 50.006 FPS,
100.012% real time. Its register-loop captures are 60.2%, 59.7% and 59.9%
(59.9% mean), compared with 62.5% baseline. Separate A3020 Lander profiles
still attribute approximately 96% of core 1 to `arm2_run`, with no clear
idle-time improvement.

## Whole-game headroom check on A440

Because the 12 MHz A3020 is close to the frame-rate limit, a second
whole-game comparison uses the supported A440 model at 8 MHz, RISC OS
3.11 and 4 MB RAM. This leaves measurable idle time while Lander runs at
real time. It is a separate emulated configuration, not a change to the
physical P4 clock. Both firmware variants use the same A440 configuration.

The candidate is tested first, followed by the restored baseline. Each
starts from desktop, launches Lander, measures 30 seconds of guest frame
progression, and captures three 32,000-sample core 1 profiles. Startup logs
confirm the selected model. The user's A3020 setting is restored afterward.

| A440 Lander build | ARM2 profile 1 | Profile 2 | Profile 3 | Mean ARM2 residency |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 71.4% | 71.1% | 71.1% | 71.2% |
| Reuse-field candidate | 70.5% | 70.7% | 70.7% | 70.6% |

Both maintain approximately 50 FPS. The candidate uses about 0.6 percentage
points less sampled core time in ARM2, or roughly 0.8% less relative
interpreter residency. All three candidate captures are below the three
baseline captures, but these are short sampled profiles on one game, not
a statistical guarantee or a measured uncapped FPS increase.

## Disposition

**Kept:** the refinement that reuses bits 4–11 across dispatch and shifted
operand decoding. It reduces mean register-loop residency from 62.5% to
59.9% (about 4.1% relatively), and shows a smaller benefit in A440 Lander.
A3020 Lander remains at approximately 100% real time. The internal code
RAM cost is 4,700 bytes; the 4,096-entry dispatch table does not grow.
The initial plain-path candidate is superseded by this smaller refinement.

The host instruction-mix counters are also retained so future experiments
can distinguish plain operands from shifted register operands.

The final source has explanatory comments and aligned macros. Its rebuilt
`arm2_run` machine code exactly matches the tested refinement, SHA256
`93a1d5d51b8b9dc6ebf2980cc24e96435eb504546e04488f0e3e68e610baaee7`,
and the final build passes all 190 CPU checks. The final firmware is
installed with the original A3020, RISC OS 3.11 and 4 MB setting restored,
back at the desktop with the serial port released.

## Artifacts

[Measurements](measurements/unshifted-register-2026-11-06/) contain baseline
and candidate frame counts, filtered runtime logs, register-loop and
Lander profiles, A440 model confirmation, host replay and CPU check output,
both candidate patches, code/application fingerprints, disassembly excerpts,
and screenshots. Working ELFs, binaries, full logs and scripts are under
`/tmp/acorn-register-2026-11-06/`. Private ROMs, discs and boot/network logs
are not added to the repository.
