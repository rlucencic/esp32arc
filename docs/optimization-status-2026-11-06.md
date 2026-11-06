# Archimedes optimization status and remaining work

Updated 2026-11-06 after retaining the pre-indexed/no-writeback LDR/STR
handlers. This is a bounded performance-pass assessment, not a percentage
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

## Bounded remainder of this performance pass

1. **Normal post-indexed transfers (P=0/W=0).** Test selecting fixed
   indexing/writeback behavior through the existing dispatch table, while
   preserving user-transfer forms, abort writeback, R15 and timing.
   The pre-indexed result motivates a trial; it does not validate it.
2. **One fresh profile of the retained build.** CPU execution dominated
   earlier profiles, but the last handler change can move the hotspots.
   Investigate one further distinct CPU candidate only if that profile
   supports it. Previously rejected cached-fetch-pointer, compact-table,
   condition-table and compiler-tail variants remain rejected.
3. **Broader regression/workload coverage.** Check other available games,
   display modes, sound-heavy software and disk activity on both models.
   The deterministic mode-switch and CPU tests already used are useful
   correctness checks, but cannot substitute for that software coverage.

This is a stopping point for the current optimization pass: finish the
post-index trial, inspect a fresh profile, and pursue at most one further
profile-supported CPU candidate before moving emphasis to compatibility
and regression testing. Small sound/VIDC changes remain lower priority
unless a relevant workload shows those devices becoming expensive.
There is no measured basis for promising another fixed percentage gain.
New software may justify a new performance investigation later.

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
