# Archimedes optimization status and remaining work

Updated 2026-11-06 after retaining the pre-indexed/no-writeback LDR/STR
handlers, rejecting the post-indexed follow-up and refreshing the profile. This is a bounded performance-pass assessment, not a percentage
of emulator feature completion or a prediction of remaining speedup.

## Current measured position

The A440 and A3020 run the tested RISC OS 3.11 Lander workload near its
paced 50 FPS limit with sound enabled. The latest retained change reduces
whole-machine frame-call cycles by 2.270–2.498% on A440 and 2.429–2.622%
on A3020, compared with initial/restored baseline runs. Its cost is
2,560 additional internal code bytes. See the
[full comparison](preindexed-transfer-experiment-2026-11-06.md).

At 360 MHz, its measured mean frame-call costs are:

| Model | Cycles/frame | Frame-call time | Nominal frame budget |
| --- | ---: | ---: | ---: |
| A440 / ARM2 8 MHz | 4,665,534.445 | 12.960 ms | 20 ms |
| A3020 / ARM250 12 MHz | 6,706,348.008 | 18.629 ms | 20 ms |

These times include machine/device execution and preemption within the
frame-call bracket. Runtime pacing and work outside that bracket are
excluded. The remaining arithmetic budget is not guaranteed spare CPU
time, and averages do not establish worst-case deadlines. A3020 remains
the tighter target. These are Lander results, not guarantees for other
games or OS versions. Results from older sampling-residency experiments
must not be added to these cycle percentages as a cumulative speedup.

## Bounded pass: completed work and remainder

1. **Completed: normal post-indexed transfers (P=0/W=0).** The
   [candidate was rejected](postindexed-transfer-experiment-2026-11-06.md).
   It passed CPU/reference/replay correctness checks, but the matched
   extended-startup A3020 run used 109.072% more frame-call cycles and
   reached only 25.731 FPS versus the restored control's 49.999 FPS.
   Its extra 4,992 internal code bytes are not retained. The accepted
   pre-indexed implementation remains unchanged.
2. **Completed: fresh retained-build profile.** A 32,000-sample A3020
   Lander scout places 95.891% of periodic samples in ARM2. The hottest
   PC is the shared NEXT tail's guest-opcode load. This is not a speed
   comparison or exact CPU-utilization estimate, and does not validate
   previously rejected fetch/dispatch variants. No further distinct CPU
   candidate is selected from this scout alone.
3. **Remaining: broader regression/workload coverage.** Check other
   available games, display modes, sound-heavy software and disk activity
   on both models. Existing deterministic mode-switch and CPU tests are
   useful correctness checks, but cannot substitute for that coverage.

The currently identified micro-optimization trials are finished. Broader
software coverage is the next priority. At most one further bounded CPU
candidate should be pursued if a distinct profile-supported hypothesis
emerges; this is not an open-ended search through previously rejected
variants. Small sound/VIDC changes stay lower priority unless a relevant
workload shows those devices becoming expensive. There is no measured
basis for promising another fixed percentage gain. New software may
justify another performance investigation later.

## Remaining functional validation and scope

The README describes working desktops, machine integration, floppy/IDE
images and setup/web controls. It still records unverified hands-on
speaker and physical Bluetooth/USB/Tab5-keyboard checks, plus limited game
and demo coverage. The current optimization work does not close those
items. Scripted input and identical WAV bytes do not establish physical
input-device behavior or audible speaker quality.

ARM3/MEMC2 machines, podules, serial/printer/Econet and richer protected
disk formats are documented unsupported features. They are separate scope
choices, not prerequisites for completing this A440/A3020 performance pass.
