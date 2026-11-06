# ARM2 dispatch-tail compiler experiments

Follow-up to [flag-setting dispatch](flags-dispatch-experiment-2026-11-05.md).
The baseline is shared-core commit `7cf10ca`, including separate S handlers.
The same Tab5, ESP32-P4 rev. 1.0 at 360 MHz, A3020/ARM250 at 12 MHz,
4 MB RAM, RISC OS 3.11, sound, ESP-IDF 5.5.6, GCC 14.2.0 and `-O2` are used.

## Hypothesis and candidates

The source expands `NEXT()` after each instruction handler, but GCC shares
some fetch/dispatch tails. Preserving additional indirect branch sites
could give the branch predictor more context about the preceding handler.
Code growth and lost compiler optimization may outweigh that benefit.

Two P4-only ARM2 component compiler settings are tested:

1. `-fno-crossjumping -fno-gcse`: disable RTL tail merging and global common
   subexpression elimination together.
2. `-fno-crossjumping`: retain common subexpression elimination while
   disabling RTL tail merging.

These are build-option experiments, with no CPU algorithm or assembly
source changes. The options apply to the ARM2 component, including its
out-of-line helpers, rather than only to `arm2_run`.

## Method and first result

Use `tools/measure_lander.py` to time 60 seconds of guest frame progression
after a fresh flash and launch. The baseline completes 3,005 frames in
60.095 seconds: 50.004 FPS, or 100.009% real time. Values marginally above
100% reflect frame quantization and pacing. The 50 FPS limit means faster
CPU execution may appear as extra idle time rather than higher FPS.

| Build | Interpreter bytes | Indirect jump sites | Lander FPS | Real-time speed |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 38,478 | 66 | 50.004 | 100.009% |
| No crossjumping, no GCSE | 44,338 | 138 | 47.876 | 95.753% |
| No crossjumping alone | 42,596 | 138 | 49.814 | 99.628% |

Indirect jump counts include `jr` and `jalr` instructions in the generated
`arm2_run`, not just instruction dispatch. More sites do not establish
better prediction. The combined options increase code by 5,860 bytes and
reduce Lander throughput by about 4.26%; this candidate is rejected.
Disabling crossjumping alone adds 4,118 bytes (10.7%). Its Lander result
does not demonstrate an improvement: it is 0.38% below this baseline,
with both close to the pacing limit. The loop comparison below is used
to look for execution headroom before deciding its disposition.

A second workload uses the existing BBC BASIC-assembled ADD/CMP/BNE loop
from the [first assembly experiment](assembly-experiment-2026-11-05.md).
Three separate 32,000-sample, 20 kHz core 1 profiles measure CPU residency
while the machine maintains real time. This exposes execution headroom
that the Lander FPS cap can hide. Sampling residency is not an uncapped
MIPS measurement. Profiles are collected outside timed Lander runs.

Both candidate host builds passed all 190 CPU checks, 100,000
randomized instructions against Unicorn 2.1.4 (seed 1), and the 10,000-frame
Lander replay. The replay preserves 88,360,660 instructions in frames
9,000–10,000 and screenshot SHA256
`9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.
Host checks use the candidate compiler flags; target boot/gameplay tests
exercise the actual RISC-V build.

## Loop results and final disposition

The listing was captured before `RUN` and checked against the intended
program. Runtime reports show approximately 89,000 instructions per
emulation frame while the loop runs. Each profile below contains 32,000
samples; percentages are core 1 samples in `arm2_run`.

| Build | Profile 1 | Profile 2 | Profile 3 | Mean |
| --- | ---: | ---: | ---: | ---: |
| Committed settings, restored | 56.8% | 57.0% | 56.6% | 56.8% |
| No crossjumping, no GCSE | 63.4% | 63.2% | 63.7% | 63.4% |
| No crossjumping alone | 59.5% | 59.7% | 60.0% | 59.7% |

Both candidates consume more sampled CPU time for the same paced guest
loop. Disabling crossjumping alone does not reveal hidden execution
headroom; it also adds code and shows no Lander improvement.

**Rejected:** both compiler settings. The original ARM2 compiler options
were restored; there is no retained CPU or build-option change from this
experiment. Preserving more indirect branch sites is not sufficient to
improve this P4 interpreter. Keeping GCC's current sharing and common
expression optimization performs better in these workloads.

The restored build passes all 190 CPU checks. Its 38,478-byte `arm2_run`
has the exact baseline machine-code SHA256
`e1a3d30425ef084b99b5f91c0885cac9c2e6c7785bf2300e2fb4d4562f95c8a6`.
The application fingerprint changes because the rebuilt application
includes the newer commit metadata. This restored firmware was flashed,
used for the baseline loop captures, and reset to the RISC OS desktop.

The next source-level candidate is the common unshifted-register path:
data-processing dispatch currently checks bit 4 for a register-specified
shift before operand decoding checks the remaining shift field. Combining
those checks warrants a separate correctness and hardware experiment.

## Artifacts

[Measurements](measurements/dispatch-tail-2026-11-05/) include both candidate
patches, build fingerprints and indirect jump counts, host checks and
differential results, Lander frame counts and runtime logs, all nine loop
profiles, program listings, running-loop screenshots and the restored
desktop. Matching ELFs, binaries, disassembly and full working logs remain
under `/tmp/acorn-dispatch-2026-11-05/`. Private ROMs and disc images are
not added to the repository.
