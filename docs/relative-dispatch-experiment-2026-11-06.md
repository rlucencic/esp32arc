# ARM2 compact relative-dispatch experiment

Date: 2026-11-06. Baseline includes the retained VIDC line timing,
palette-table and sound-accumulator optimizations.

## Candidate and build

The 4,096-entry computed-goto table already resides in internal RAM.
The candidate replaces RV32's 32-bit absolute handler pointers with signed
16-bit byte displacements from the `next` label. Dispatch sign-extends the
table entry and adds that label address before jumping. Other targets keep
the original pointer table. Conditional second-stage dispatch uses the
same relative representation. No guest instruction semantics are changed.

The table shrinks from 16,384 to 8,192 bytes. P4 `arm2_run` grows from
43,154 to 43,344 bytes (+190). Allocated internal code including end
padding grows 256 bytes; initialized data plus noinit shrinks 8,192 bytes;
total BSS is unchanged. Individual DRAM sections move across banks.
Disassembly confirms `lh` and an additional base-address add in the
shared tail, with the base occupying another saved register. This is a
whole-firmware comparison, not an isolation of that add's cost.

An initial scaled halfword-offset expression was rejected by GCC as a
nonconstant initializer. The measured candidate uses unscaled byte offsets.
The signed range is a build-layout constraint, not guaranteed by C.
An intentionally forced 16-bit instrumented native-host trial exceeded the
layout constraint and produced invalid jumps; it was discarded and never
flashed. Retaining such a representation would require an enforced range
check for every supported target/compiler configuration.

## Validation and limits

Before flashing, all 4,096 actual P4 table entries were checked against
full-width target-minus-base relocation addresses in a companion ELF
retaining local assembler symbols. Every allocated section of that ELF
matches the normal candidate ELF at identical addresses and bytes.
All offsets match exactly, range from 0 to 10,942, and resolve to aligned
instructions inside `arm2_run`. Entries 0xF00–0xFFF are literal zero
(`next`) and have no relocation pair. No displacement is truncated in this
measured build. The compiler emits the same target labels from the
unchanged dispatch macros; decoding differs only by representation.

The normal portable fallback passes all 190 CPU checks. To exercise the
relative-dispatch logic on the native host without imposing the P4 layout
limit, a scratch version uses signed 32-bit relative entries. Under UBSan
it passes 190 checks and 100,000 random instruction comparisons against
Unicorn (seed 1), without sanitizer diagnostics. These host checks validate
the relative-dispatch logic, not the P4 entry width or target performance.

The wider-relative host version also matches the prior 6,000-frame
RISC OS 3.11 A3020 mode-switch replay: all seven PPM screenshots and the
complete device/CPU status and instruction-mix log match after normalizing
output paths. The measured interval from frame 5,000 executes 33,794,619
instructions. Final PPM SHA256:
`4fc4d24353d1ceeb821113b0b2968f07f8af9a38f0140d8b058c07335fd74843`.

## Hardware method

Tab5 ESP32-P4 at fixed 360 MHz, ESP-IDF 5.5.6 / GCC 14.2.0,
A3020 / ARM250 12 MHz, RISC OS 3.11, 4 MB RAM, sound enabled, Lander in
320×256 8-bit mode. Saved settings select A3020 for all runs. Each fresh
app flash uses the same `measure_lander.py --boot` startup, a separate
30-second guest pacing check, and five 500-frame direct cycle captures.
No sampling profiler runs during timing. The frame-call bracket includes
machine/device work and preemption, excluding runtime pacing waits.

## Hardware results (rejected)

Each row captures 2,500 frames.

| Build | Cycles/frame | Guest instructions/frame | Frame-call cycles/instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 6,836,669.101 | 88,356.3836 | 77.37606 | 50.0008 |
| Relative table | 7,178,979.389 | 88,345.0372 | 81.26070 | 49.9934 |
| Restored baseline | 6,845,699.821 | 88,352.3992 | 77.48176 | 49.9845 |

Candidate frame cost rises 5.007% against the first
baseline and 4.868% against the restored control.
Normalized cost rises 5.020% and 4.877%, respectively.
Guest instructions/frame differ by -0.0128% and
-0.0083%. The baseline cycle means differ by
+0.132%. All fifteen captures are valid at fixed 360 MHz.

Individual cycles/frame:

- Initial baseline: 6,833,928.594; 6,832,669.794; 6,840,964.970; 6,837,958.986; 6,837,823.160.
- Relative table: 7,159,941.762; 7,180,344.238; 7,187,417.426; 7,168,300.786; 7,198,892.734.
- Restored baseline: 6,847,693.776; 6,844,892.350; 6,844,788.552; 6,848,749.422; 6,842,375.006.

The candidate remains paced near 50 FPS, but spends more CPU cycles in
frame calls. Saving 8 KB of table RAM does not compensate for the increased
frame cost on this workload. The extra address dependency, register
pressure and changed interpreter layout are possible contributors; these
measurements do not isolate their individual effects. The hot guest-opcode
load in the preceding profile is not proof that table footprint causes
its cost. No candidate A440 speed test was performed.

The candidate was reverted and the exact saved baseline app reinstalled.
The restored source rebuild matches all 6,244 defined baseline functions
at identical addresses and bytes; binary metadata may differ. All earlier
accepted optimizations remain. Further investigation should avoid treating
smaller dispatch data or fewer C operations as demonstrated throughput gains.

Raw firmware, rejected patch, local-symbol ELF, relocation checker,
host fixtures, logs, screenshots and capture JSON remain outside the
repositories under `/tmp/acorn-relative-dispatch-2026-11-06`.

The restored firmware is back at the A3020 640×480 desktop. The final
screenshot was verified with the command window closed. The serial port
is closed and no timing capture or profiler is running.
