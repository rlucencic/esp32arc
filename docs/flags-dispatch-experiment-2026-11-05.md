# ARM2 flag-setting dispatch experiment

Follow-up to [operand decoding](operand-decode-experiment-2026-11-05.md).
The baseline is the committed shared core at `da76365`, including the retained
register shift-field decode. Hardware and workload are unchanged: Tab5,
ESP32-P4 rev. 1.0 at 360 MHz, A3020/ARM250 at 12 MHz, 4 MB RAM,
RISC OS 3.11, sound enabled, ESP-IDF 5.5.6, GCC 14.2.0, `-O2`.

## Candidate

The 4,096-entry dispatch table already indexes instruction bits 31–20,
including the S (set flags) bit. Previously each data-processing pair
selected the same handler, which tested S at runtime. The candidate sends
each pair to separate handlers with a constant `s` argument. This lets the
compiler remove unused arithmetic flags and shifter carry calculations
from instructions that preserve the flags.

The operand forms and opcode table size are unchanged. Register-specified
shifts, multiply/SWP detection, R15 writes, PSR changes and conditional
execution keep their existing paths. Test/compare encodings with S clear
retain the existing behavior.

`arm2_run` grows from 31,996 to 38,478 bytes: 6,482 bytes (20.3%). This
code-size cost must be justified by hardware performance.

The generated RISC-V dispatch entries confirm that ADD/ADDS immediate
now select `0x4ff04a34`/`0x4ff04a96`; both selected `0x4ff03a40` in the
baseline. The candidate's ordinary ADD handler stores its result and
continues without testing S or constructing NZCV. The matching disassembly
excerpt is included with the measurements. This is a C specialization
that improves generated assembly; no handwritten assembly is added.

## Correctness

- All 190 ARM2 host checks pass.
- 100,000 randomized instructions agree with Unicorn 2.1.4, seed 1.
- The deterministic 10,000-frame A3020 Lander replay matches the baseline:
  88,360,660 instructions in frames 9,000–10,000 and final screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

## Hardware method

Use `tools/measure_lander.py` after flashing each build, starting from the
same setup screen and launching Lander from the SD applications disc.
Measure guest frame progression over 60 seconds without HTTP polling or
profiling during the timed interval. Start/end console responses must both
report 320x256 at 8 bpp. Compare fresh boots so that the game is measured
at a similar point after launch. A warm, long-running baseline is recorded
separately and is not substituted for the fresh-boot comparison.

## Initial hardware result

| Build | Frames | Elapsed seconds | Runtime FPS | Real-time speed |
| --- | ---: | ---: | ---: | ---: |
| Baseline, fresh boot | 2,973 | 60.099 | 49.468 | 98.936% |
| Split S handlers, fresh boot | 2,995 | 60.092 | 49.840 | 99.680% |

The initial relative throughput gain is 0.752%. The separate warm baseline
was 49.360 FPS (98.719% real time); it is not used in this calculation.
The runtime is capped at 50 FPS, so these measurements cannot establish
uncapped CPU throughput or the same gain for other ARM software.

A separate 32,000-sample baseline profile attributes 95.66% of core 1
samples to `arm2_run`. A merged fetch/dispatch tail at `0x4ff02648` through
`0x4ff02662` accounts for 8.46% of all samples. Source attribution to a
`NEXT()` expansion does not identify the preceding guest opcode: GCC
shares this tail across handlers. It must not be read as evidence that
multiply is expensive merely because the shared tail maps to its source
line. Raw PCs and matching interpreter fingerprints are preserved for
future dispatch experiments. Profiles are captured outside timed runs.

## Repeat and disposition

The builds were flashed in baseline/candidate/baseline/candidate order.
The second baseline completed 2,984 frames in 60.096 seconds (49.654 FPS,
99.308% real time); the second candidate completed 2,990 frames in 60.102
seconds (49.749 FPS, 99.498% real time).

| Build | Both runs, frames | Both runs, seconds | Pooled FPS | Pooled real-time speed |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 5,957 | 120.195 | 49.561 | 99.122% |
| Split S handlers | 5,985 | 120.194 | 49.795 | 99.589% |

The pooled relative throughput gain is **0.471%**. The two paired gains
are 0.752% and 0.192%; the candidate improves both comparisons, but the
variation is material relative to this small gain. These are two runs per
build on one workload, close to the 50 FPS cap, not a statistical guarantee
or an uncapped CPU benchmark.

**Kept:** separate S handlers in the shared ARM2 core. The measured Tab5
benefit costs 6,482 bytes of additional internal code RAM. The table size
does not grow. The tested candidate was left installed and reset to the
RISC OS desktop; the serial port was released.

The next performance investigation should examine instruction fetch and
dispatch, using the raw PC profiles to distinguish shared dispatch costs
from operand handling. Changing compiler tail merging needs its own
code-size, correctness and hardware comparison.

## Artifacts

Frame counts and filtered runtime logs for all four fresh runs, the warm
baseline, source-level profiles and raw PCs, CPU check results, differential
results, the candidate patch, interpreter/application hashes and screenshots
are in [measurements/flags-dispatch-2026-11-05](measurements/flags-dispatch-2026-11-05/).
Matching private working ELFs, binaries, source snapshots, full build logs
and host replay inputs remain under `/tmp/acorn-flags-2026-11-05/`.
ROMs and disc images are not added to the repository.
