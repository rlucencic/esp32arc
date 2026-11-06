# ARM2 pre-indexed transfer dispatch experiment

Date: 2026-11-06. Baseline retains VIDC line timing, palette-table reuse,
sound accumulators and all earlier accepted ARM2 changes.

## Candidate

The existing 4,096-entry dispatch table includes the P (bit 24) and W
(bit 21) fields for LDR/STR. The candidate routes only P=1/W=0 to eight
additional byte/word, load/store, immediate/register-offset handlers.
`single_transfer` receives a constant selecting pre-indexing without base
writeback. The compiler removes address-selection, user-mode transfer and
base-writeback checks from those handlers. The P4 disassembly of the
new immediate STRB handler retains U-direction decoding, with no P/W
decoding. Other transfers
retain the original general code. The register-offset undefined-encoding
check is retained in both handler sets, and conditional execution still
uses the existing condition check and AL second-stage dispatch.

Unlike the rejected up/down dispatch experiment, the direction field
remains dynamic. Unlike the rejected plain-Rm offset shortcut, the
operand-offset calculation is unchanged. This candidate specializes
indexing/writeback only; it does not add an opcode table or assembly.

## Build and correctness

ESP-IDF 5.5.6 / GCC 14.2.0 on Tab5 ESP32-P4. `arm2_run` grows from
43,154 to 45,704 bytes (+2,550). Allocated internal code plus end padding
grows 2,560 bytes. Initialized data and total BSS are unchanged, although
BSS placement shifts between banks. The dispatch table remains 16,384
bytes. Its 64 single-transfer slots select sixteen distinct targets
instead of eight. An ELF check verifies that only P=1/W=0 selects the
new target for each form, both directions share it, and all P=0 and
P=1/W=1 combinations retain their original general-handler aliases.

The candidate passes all 190 CPU checks and 100,000 random instructions
against Unicorn (seed 1). Separate baseline/candidate shared libraries
pass 2,883,584 reference comparisons under UBSan without diagnostics:

- 2,097,152 cases cover every combination of six transfer-control bits,
  all sixteen base/destination registers, four address alignments,
  four processor modes and eight memory/callback scenarios.
- 524,288 additional cases cover all sixteen conditions and sixteen
  NZCV states, every transfer-control combination, all four modes and
  all eight scenarios with fixed Rn/Rd and aligned accesses.
- 262,144 cases set the invalid register-offset bit 4, across conditions,
  flags, all transfer fields, modes and scenarios, checking both the
  original and new undefined-encoding guards.

Scenarios include direct pages, slow/watched pages, user-read-only and
privileged-write permissions, successful bus accesses with wait states,
bus aborts, out-of-range addresses, a differently protected adjacent page,
and callbacks that invalidate code, assert IRQ and end the run. Inputs
include immediate and plain/shifted register offsets, register aliases,
R15 base/destination forms, and a poisoned stored R15. Each comparison
checks the complete CPU struct after normalizing host-only pointers,
all banked registers, cached permissions, pending IRQ/FIQ, watch state,
cycle return/counters, all 64 KB of RAM and a bus-access trace including
callback PC, remaining ticks and `arm2_now`. These are reference checks
against the baseline, not an independent ARM2 architectural oracle, and
do not exhaust operand/offset values or all memory mappings.

A 6,000-frame RISC OS 3.11 A3020 mode-switch replay matches all seven
PPM screenshots and the complete CPU/device-status and instruction-mix
log after normalizing output paths. The interval from frame 5,000 runs
33,794,619 instructions. Final PPM SHA256:
`4fc4d24353d1ceeb821113b0b2968f07f8af9a38f0140d8b058c07335fd74843`.

## Hardware method

Tab5 at fixed 360 MHz, A3020 / ARM250 12 MHz, RISC OS 3.11, 4 MB RAM,
sound enabled, Lander in 320×256 8-bit mode. Each fresh app flash uses
the same saved settings and `measure_lander.py --boot` startup, a separate
30-second guest pacing check, then five 500-frame direct cycle captures.
No sampling profiler runs during timing. Frame-call cycles include machine
and device work plus preemption; runtime pacing waits are outside the
bracket. This compares whole firmware, including code-layout effects.

The A440 / ARM2 8-MHz comparison uses the same method after saving model
0 with RISC OS 3.11, 4 MB and sound on. Each model has its own initial
baseline, candidate and restored-baseline app flash; all game starts and
measurements use identical scripted steps within that model.

The restored baseline rebuild matches all 6,244 defined functions at
identical addresses and bytes. Formatting cleanup of the retained candidate
also matches all 6,244 measured candidate functions at the same addresses
and bytes. These checks do not claim identical whole-binary metadata.

## A3020 / ARM250 12 MHz hardware results

Each row captures 2,500 machine frames.

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 6,886,912.829 | 88,343.8388 | 77.95578 | 49.9754 |
| Candidate | 6,706,348.008 | 88,346.3912 | 75.90970 | 49.9982 |
| Restored baseline | 6,873,307.713 | 88,348.4232 | 77.79774 | 49.9991 |

Candidate frame cost changes by -2.622% versus the
initial baseline and -2.429% versus the restored control.
Normalized cost changes by -2.625% and -2.427%, respectively.
Guest instructions/frame differ by +0.0029% and
-0.0023%; the baseline cycle means differ by
-0.198%. All fifteen captures are valid at fixed 360 MHz.

Individual cycles/frame:

- Initial baseline: 6,886,507.564; 6,886,019.184; 6,887,736.788; 6,886,920.408; 6,887,380.202.
- Candidate: 6,707,639.764; 6,702,897.770; 6,707,345.330; 6,702,095.118; 6,711,762.058.
- Restored baseline: 6,870,043.780; 6,877,229.796; 6,871,969.354; 6,870,814.116; 6,876,481.518.

## A440 / ARM2 8 MHz hardware results

Each row captures 2,500 machine frames.

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 4,773,919.419 | 61,379.7472 | 77.77679 | 49.9992 |
| Candidate | 4,665,534.445 | 61,380.2960 | 76.01030 | 49.9982 |
| Restored baseline | 4,785,050.074 | 61,378.3104 | 77.95995 | 49.9995 |

Candidate frame cost changes by -2.270% versus the
initial baseline and -2.498% versus the restored control.
Normalized cost changes by -2.271% and -2.501%, respectively.
Guest instructions/frame differ by +0.0009% and
+0.0032%; the baseline cycle means differ by
+0.233%. All fifteen captures are valid at fixed 360 MHz.

Individual cycles/frame:

- Initial baseline: 4,772,722.080; 4,774,395.926; 4,777,937.514; 4,778,787.728; 4,765,753.846.
- Candidate: 4,664,363.098; 4,667,512.362; 4,667,931.388; 4,662,337.964; 4,665,527.412.
- Restored baseline: 4,777,608.752; 4,785,958.458; 4,788,080.776; 4,792,579.140; 4,781,023.242.

## Disposition

Retained. Both models use fewer frame-call cycles than both of their
baseline runs, while remaining paced near 50 FPS. This is lower CPU cost,
not a measured increase in paced frame rate. The additional internal code
cost is 2,560 bytes; the table and CPU-state sizes are unchanged.

The comparison includes compiler layout, register allocation, tail merging
and handler specialization together. It does not isolate the cycle savings
from removing any individual P/W check, nor establish a gain for other
games, OS versions or host architectures. Further dispatch specialization
still needs its own measurements.

The final source retains the pre-indexed/no-writeback handlers and all
previously accepted changes. The board receives the exact measured
candidate app; the formatted source rebuild matches its defined functions.
Raw firmware, reference fixtures, disassembly, logs, screenshots and capture
JSON remain outside the repositories under
`/tmp/acorn-preindexed-2026-11-06`.

The retained candidate firmware was returned to the A3020 640×480 desktop.
The screenshot was verified, with no command window or error visible.
The serial port is closed; no timing capture or profiler is running.
