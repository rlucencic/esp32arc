# ARM2 fetch, register-selection and PC/PSR experiments

Date: 2026-11-06. All four candidates below were rejected. The original
core and exact saved baseline firmware were restored after testing.

## Profile and method

Tab5 ESP32-P4, 360 MHz, RISC OS 3.11, 4 MB RAM, sound enabled. An A3020 /
ARM250 12 MHz Lander profile collected five 32,000-sample core-1 bursts at
20 kHz: 152,942 of 160,000 samples (95.59%) fell in `arm2_run`.
Inline source attribution within ARM2 included data processing 15.18%,
operand2 10.28%, single transfers 10.42%, and block transfers 9.83%.
The Rn-selection source line accounted for 3.69%. The shared fetch tail
accounted for 9.53%, with at least 67 incoming static branches; its source
location after block transfer does not make that cost LDM/STM-specific.
The hottest sampled PC was the guest-instruction load (1.75% of ARM2).

Comparisons used A440 / ARM2 8 MHz Lander to expose idle CPU headroom.
Each build had a separate 30-second guest-frame-counter measurement,
followed by five 32,000-sample core-1 profiles. The machine configuration
and 320×256 8-bit game mode were checked. Lower ARM2 residency at the same
paced 50 FPS indicates lower CPU demand. Samples are approximate, rounded
to 0.1 percentage point, and are not retired instruction or cycle counts.
Inlining, shared tails, periodic sampling and instruction-cache layout
limit causal attribution. Short frame measurements can slightly exceed
50 FPS because of counter and query timing.

## Cached host fetch pointer

The candidate cached the host address for the guest PC, advanced it for
sequential fetches, and recomputed it for redirects. The executable-page
guard and mapping slow path remained in place. P4 disassembly confirmed
a direct cached-address load; pointer advancement replaced the address add.

| Build | ARM2 residency samples (%) | Mean (%) | FPS | Interpreter bytes |
| --- | --- | ---: | ---: | ---: |
| Baseline | 68.9, 68.7, 68.8, 68.0, 68.2 | 68.52 | 49.9869 | 43,154 |
| Cached pointer | 71.2, 71.9, 71.8, 71.7, 70.9 | 71.50 | 49.9890 | 44,086 |

CPU residency rose 2.98 percentage points (4.35% relatively), and code grew
932 bytes. Rejected. All 190 existing checks and 100,000 Unicorn cases
(seed 1) passed. Another 44 assertions passed on both baseline and candidate:
noncontiguous pages, backward branches, MOV/LDR/LDM PC, SWI, IRQ, prefetch
abort, coprocessor mapping changes, bus invalidation/fetch, self-modifying
code, and 26-bit wrap.

## Load Rn before testing R15

The candidates loaded `REG(c, rn)` before the R15 override, preserving
PC+8 for ordinary forms and PC+12 for register-specified shifts, without
PSR bits. One variant marked R15 unlikely; the other used no branch hint.
Disassembly confirmed the early load. Both were slower despite the second
variant producing smaller code.

| Build | ARM2 residency samples (%) | Mean (%) | FPS | Interpreter bytes |
| --- | --- | ---: | ---: | ---: |
| Fresh baseline | 68.6, 68.7, 68.6, 68.6, 69.0 | 68.70 | 49.9994 | 43,154 |
| Early load, unlikely R15 | 71.7, 71.3, 71.1, 71.5, 71.4 | 71.40 | 49.9992 | 43,500 |
| Early load, no hint | 70.5, 70.6, 70.4, 71.0, 70.5 | 70.60 | 50.0346 | 42,788 |

Residency increased 3.93% and 2.77% relatively. Both were rejected. Each
passed 190 existing checks, 100,000 Unicorn cases (seed 1), and 256 extra
assertions covering all 16 Rn fields, four operand forms, both S bits,
a poisoned stored R15, result values and the guest PC.

## Shared PC/PSR write paths

The next candidate made `run_write_psr` a shared noinline/noclone internal-RAM
helper and outlined data-processing writes to PC into a second helper.
It retained the original PSR privilege rules, refresh logic and guest cycles.
`arm2_run` shrank to 30,388 bytes; the helpers added 340 and 44 bytes, for
30,772 combined bytes (12,382 fewer than baseline). The compiler also
merged more dispatch tails: 35 indirect jump sites versus 88 in baseline.

| Build | Combined ARM2 residency samples (%) | Mean (%) | FPS |
| --- | --- | ---: | ---: |
| Fresh baseline | 68.9, 68.7, 68.8, 68.2, 68.8 | 68.68 | 50.0199 |
| Shared helpers | 72.9, 72.9, 72.6, 73.4, 72.9 | 72.94 | 50.0001 |

Candidate totals include `arm2_run`, `run_write_psr`, and
`data_processing_pc`; moved work is not counted as saved time. Rounded
helper samples contribute 0.7–0.9 percentage points per capture. CPU use
increased 4.26 percentage points (6.20% relatively). Rejected: reduced code
size did not translate into reduced CPU demand. The measurements do not
isolate the contribution of call overhead, register allocation or dispatch
merging.

The candidate passed 190 existing checks, 100,000 Unicorn cases (seed 1),
and 40,960 baseline/candidate state comparisons covering all sixteen
opcodes, both S bits, four operand forms, four CPU modes, all sixteen
initial NZCV combinations and five operand values. Comparisons included
PC, PSR, condition mask, pending interrupts, executable page, permissions,
cycle return, remaining ticks, accumulated ticks, instruction count, live
registers and all banked registers. Newly enabled interrupts were entered
in a subsequent run. Both state traces have SHA256
`bf7eddb06d216f9d64c2ebb73f64c14852e28759537bdc078ed13b765632d598`.
The deterministic host replay also matched. Original core and baseline
firmware restored; scratch evidence is in `/tmp/acorn-pc-outline-2026-11-06`.

## Replay and restoration

Each candidate preserved the deterministic A3020 host Lander replay:
88,360,660 instructions in the measured frame interval and final PPM SHA256
`9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The restored P4 `arm2_run` is 43,154 bytes with SHA256
`05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d`.
The exact saved baseline app SHA256 is
`530d5a1051343cf3c6f60f83abe0b4be16c9f032470d084f94a7ec821c7d0205`.
Restored A3020 Lander measured 49.9949 FPS after the fetch experiment and
49.9987 FPS after the register experiments. After the PC/PSR experiment,
restored A3020 Lander measured 50.0323 FPS in the same short timing check.
All sessions ended at the
640×480 desktop with the serial port closed.

Raw profiles, temporary fixtures and rejected patches remain outside the
repositories under `/tmp/acorn-fetch-pointer-2026-11-06` and
`/tmp/acorn-rn-select-2026-11-06`; those scratch paths are not durable evidence
archives. The measurements needed to assess the conclusions are recorded
above. No ROMs, disk images, private boot logs or generated builds are added.

Further outlining experiments should isolate PSR updates from PC writes;
the combined rewrite above regressed. Code-size savings alone do not
justify retaining a change.
