# ARM2 normal post-indexed transfer experiment

Date: 2026-11-06. Baseline includes the retained P=1/W=0 pre-indexed
LDR/STR specialization and all earlier accepted changes.

## Candidate

The existing dispatch table selects eight additional handlers only for
P=0/W=0: normal post-indexed LDR/STR, byte/word, immediate/register offset.
The access uses the original base, then writes the moved base back unless
Rn is R15; this writeback also occurs on data abort as in the baseline.
P=0/W=1 user-transfer forms and P=1/W=1 remain general. P=1/W=0 keeps the
retained pre-indexed handlers. Direction and offset calculations are
unchanged, as are conditional execution and invalid register-offset guards.

A constant indexing enum replaces the prior fixed-pre boolean, selecting
general, fixed pre-indexed or fixed normal post-indexed behavior. Fixed
post-indexing removes runtime P/W address/user/writeback decisions, while
the general variant preserves the original dynamic expressions.

## Build and correctness

ESP-IDF 5.5.6 / GCC 14.2.0, Tab5 ESP32-P4. `arm2_run` grows from 45,704 to
50,716 bytes (+5,012). Allocated internal code plus end padding grows
4,992 bytes. Table/data and total BSS sizes are unchanged, though BSS
placement shifts between banks. The table stays 16,384 bytes. Its 64
single-transfer slots select 24 distinct targets instead of 16. An ELF
check verifies all fixed pre/post and general W=1 aliases for every
load/store, byte/word, offset form and direction combination.

All 190 CPU checks and 100,000 random Unicorn comparisons (seed 1) pass.
Baseline/candidate shared libraries also pass 2,883,584 reference
comparisons under UBSan without diagnostics, using the same fixtures
as the [pre-indexed experiment](preindexed-transfer-experiment-2026-11-06.md):

- 2,097,152 cases cover all six transfer control bits, all base/destination
  registers, four alignments, four processor modes and eight memory/bus
  scenarios.
- 524,288 cases additionally cover all conditions and NZCV combinations,
  all control bits, modes and scenarios with fixed Rn/Rd and alignment.
- 262,144 invalid register-offset bit-4 cases check undefined-instruction
  handling across conditions, flags, transfer fields, modes and scenarios.

Checks compare CPU/banked registers, cached permissions, pending IRQ/FIQ,
watch state, cycle results/counters, all 64 KB of RAM and callback trace
values/PC/timing, with host pointers normalized. Scenarios cover direct
and slow/watched pages, differing user/privileged permissions, successful
bus callbacks with wait states, bus aborts, out-of-range addresses, an
adjacent protected page, and callbacks that invalidate code, assert IRQ
and end the run. This is baseline regression coverage, not an independent
architectural oracle or exhaustive coverage of operand values/mappings.

The 6,000-frame RISC OS 3.11 A3020 mode-switch host replay also matches
all seven screenshots and the complete normalized CPU/device/instruction
mix log: 33,794,619 instructions from frame 5,000; final PPM SHA256
`4fc4d24353d1ceeb821113b0b2968f07f8af9a38f0140d8b058c07335fd74843`.

## Hardware method and startup failure

Tab5 at fixed 360 MHz, A3020 / ARM250 12 MHz, RISC OS 3.11, 4 MB,
sound enabled, Lander 320×256 8-bit. The bracket includes machine/device
work and preemption, excluding runtime pacing waits. No sampling profiler
runs during timing. Results include compiler layout/register-allocation
and memory-placement effects; they do not isolate removed P/W decisions.

The initial baseline used the usual `measure_lander.py --boot`, a separate
30-second pacing check and five 500-frame captures. The candidate failed
that fixed-delay startup's Lander-mode check: it was still at the desktop.
No valid timing comparison is reported for that first candidate attempt.
A subsequent screenshot is a normal desktop; runtime messages report
36–37 FPS around app uptime 107–122 seconds. Native correctness passing
does not establish acceptable target performance.

The candidate was freshly flashed again and given 45 seconds after Start,
then F12, a 0.3-second delay, the Lander launch text and 18 seconds to load.
It reached the expected game mode. The restored baseline repeats that
same extended startup. Both then run `measure_lander.py` without `--boot`,
with a separate 30-second pacing check and three 500-frame captures.
Only this pair has matched extended-startup timing; the earlier five-window
baseline is an additional reference, not a matched three-build sequence.
Neither the first failed start nor transient boot/loading FPS is used to
calculate the matched game speed difference.

## Hardware results (rejected)

| Build/startup | Frames | Cycles/frame | Guest instructions/frame | Frame-call cycles/instruction | FPS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Initial reference (usual startup) | 2,500 | 6,722,320.116 | 88,360.7548 | 76.07812 | 50.0094 |
| Candidate (extended startup) | 1,500 | 13,980,329.309 | 88,331.6133 | 158.27096 | 25.7314 |
| Restored control (extended startup) | 1,500 | 6,686,848.671 | 88,388.3693 | 75.65304 | 49.9994 |

Against the matched extended-startup control, candidate frame cost rises
109.072% and normalized cost rises 109.206%, while guest
instructions/frame differ by -0.0642%. Its paced-frame-counter
measurement falls from 49.9994 to 25.7314 FPS. All six matched-pair
captures are valid at 360 MHz. This establishes an unacceptable target
regression, not its root cause.

Individual cycles/frame:

- Initial reference (usual startup): 6,723,378.720; 6,721,943.526; 6,717,655.860; 6,724,513.984; 6,724,108.492.
- Candidate (extended startup): 13,976,813.980; 13,983,830.112; 13,980,343.836.
- Restored control (extended startup): 6,683,165.832; 6,688,765.114; 6,688,615.068.

The retry log reports the CPU page table in internal RAM, and both builds
use the same machine, ROM, RAM and sound settings. Changes in generated
code, layout, register allocation and BSS-bank placement remain coupled.
No individual mechanism is established as the reason for the slowdown.
A440 candidate performance was not tested; this A3020 regression is already
sufficient to reject the change.

The source was reverted and the exact saved pre-indexed baseline app
restored. A fresh rebuild matches all 6,244 saved defined functions at
identical addresses and bytes, despite possible binary metadata changes.
The previously retained P=1/W=0 optimization remains enabled. No narrower
post-indexed variant is claimed to be validated by this trial.

Raw builds, patch, reference fixtures, startup diagnostics, captures and
screenshots remain outside the repositories under
`/tmp/acorn-postindexed-2026-11-06`.

## Fresh retained-build profile

After all timing captures finished, one 32,000-sample core-1 profile at
20 kHz was collected on the restored A3020 Lander workload, using its
matching saved ELF. ARM2 has 30,685 samples (95.891%); sound has 489
(1.528%); `vidc_line_end` has 318 (0.994%); idle has 120 (0.375%).

The hottest ARM2 PC is `0x4ff025d8` (610 samples): disassembly identifies
it as the guest-opcode `lw` in the shared NEXT fetch/decode/dispatch tail.
The source line attached after transfer handlers does not make it a
transfer-specific load. Other top PCs also lie in that shared tail.

This single periodic burst is a hotspot scout, not a before/after speed
comparison or an exact utilization/headroom estimate. Periodic aliasing
and merged tails limit attribution. It profiles the restored build and
does not explain the rejected candidate's slowdown. No new distinct CPU
candidate is selected from it alone; previously rejected fetch/dispatch
variants remain rejected. Broader workload/regression coverage is the
next priority for the bounded pass.

The restored retained firmware is back at the A3020 640×480 desktop.
Its screenshot was verified with no command window or error visible.
The serial port is closed; no timing capture or profiler is running.
