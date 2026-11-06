# ARM2 contiguous-register copy experiment

Baseline: shared core `6ee4747`, with the internal byte count table. Board:
Tab5 ESP32-P4 rev. 1.0 at 360 MHz, ESP-IDF 5.5.6, GCC 14.2.0, `-O2`.
Both hardware runs use A440/ARM2 at 8 MHz, RISC OS 3.11, 4 MB RAM and
sound enabled. This model leaves CPU headroom while Lander holds 50 FPS.

## Workload and candidate

Host `ARC_MIX` reporting now includes the eight most frequent executed
register lists for LDM and STM. In the deterministic Lander replay,
STM `{R0-R9}` accounts for 896,640 transfers, 41.3% of all STMs. Lists
`{R0,R1}` and `{R0-R3}` also occur frequently. The leading LDM lists
include `{R0-R2}` (11.3%) and `{R0,R1}` (9.2%).

The candidate recognizes a nonempty contiguous list starting at R0 with
`!(list & (list + 1))`, excluding lists containing PC. It copies the
first `n` registers in a counted loop on the existing direct one-page
path. Other lists retain the unrolled per-register bit tests. Writeback,
user-bank handling, PC/PSR handling, aborts and cycle accounting retain
the existing logic. The linked `arm2_run` shrinks from 43,196 to 43,154
bytes, a saving of 42 bytes. It introduces no memcpy call.

## Correctness

- All 190 CPU checks pass.
- All 100,000 randomized instructions agree with Unicorn, seed 1.
- 8,192 targeted cases agree with the committed core: all R0-prefix
  lengths 1–16, all 16 base registers, load/store, pre/post, up/down,
  writeback on/off and PSR/user bit on/off. Each compares all registers,
  PC, NZCV, reported exception and all 64 KB of RAM after one instruction.
  This is a behavioral regression comparison, not an independent ARM2
  architectural oracle. It does not compare cycle counts or exhaust
  abort and page-permission combinations.
- The host replay runs the same 88,360,660 instructions in frames
  9,000–10,000, with screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The targeted comparison can be reproduced without saved binaries:

```sh
python3 docs/measurements/block-copy-2026-11-06/check_prefix.py
```

It builds the committed baseline and saved candidate patch in a temporary
directory using the sibling `esp32emu` checkout and the host C compiler.
An optional argument selects another `esp32emu` checkout.

## Hardware method

Each run starts Lander from the same applications disc, warms up, checks
frame progress over 30 seconds, then captures five separate 32,000-sample
core 1 profiles at 20 kHz. Timing and sampling are separate. Percentages
measure interpreter CPU residency at a paced frame rate, not uncapped FPS.

## Results and disposition

| A440 Lander | Baseline | Contiguous R0-prefix copy |
| --- | --- | --- |
| ARM2 CPU residency captures | 70.4%, 69.8%, 69.8%, 69.8%, 69.6% | 68.1%, 68.6%, 68.3%, 69.1%, 68.4% |
| Mean residency | 69.88% | 68.50% |
| Frame-count interval | 1,503 / 30.054 s | 1,503 / 30.039 s |
| Paced frame rate | 50.011 FPS | 50.035 FPS |

**Kept:** the contiguous R0-prefix path. Mean interpreter residency falls
by 1.38 percentage points, approximately **1.97% relatively**, with
42 fewer bytes of code. These short sampled runs show a gain for this
workload; the added branch may cost time for other register-list mixes.

The candidate interpreter SHA256 is
`05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d`.
The prior byte-table baseline and this candidate were both profiled in
this session; prior experiment means are not substituted for baseline
measurements.

[Artifacts](measurements/block-copy-2026-11-06/) include the candidate
patch, reproducible prefix-list comparison, CPU checks, differential
result, deterministic replay output/screenshots, build fingerprints,
model confirmations, all ten profiles and both timing intervals.
Private matching binaries, ELFs and full boot logs are retained under
`/tmp/acorn-block-copy-2026-11-06/`; ROMs, discs and boot/network logs
are excluded from the repository.

The original A3020/ARM250 at 12 MHz, RISC OS 3.11 and 4 MB RAM was
restored with the tested candidate still installed. The final game check
completed 1,502 frames in 30.039 seconds, **50.001 FPS** (100.002% real
time). Final desktop verification is recorded with the measurements.

## Follow-up: contiguous lists above R0 (rejected)

A later candidate retained the existing R0-prefix path and added a path
for contiguous ranges starting at another register, excluding PC. Such
lists occur frequently in the recorded replay: STM `4000` (8.8% of STMs),
`0030` (5.0%), `001C` (3.8%) and `01C0` (2.8%); LDM `000C` (6.2% of LDMs),
`001C` (5.3%), `00F0` (5.0%) and `01C0` (3.8%). These are percentages of
each transfer class, not all instructions.

After the existing direct-page/permission/bank checks, the candidate
recognizes a bit run with `!(list & (list + (list & -list)))`. Empty lists
already return earlier, and lists containing PC are excluded. The first
register is `count16((list & -list) - 1)`, using the existing count table.
A counted loop copies registers in ascending order; the existing writeback,
watch, cycle and fallback rules remain. P4 interpreter code grows from
43,154 to 43,358 bytes (+204), with the same 88 indirect jump sites.

A fresh A440/ARM2 8 MHz comparison uses separate 30-second frame-counter
timing and five 32,000-sample core-1 captures per build:

| Build | ARM2 residency samples (%) | Mean (%) | Lander FPS |
| --- | --- | ---: | ---: |
| Existing R0-prefix optimization | 69.0, 68.5, 68.6, 68.2, 68.8 | 68.62 | 50.0020 |
| Additional non-prefix range path | 70.0, 69.7, 70.1, 69.8, 69.9 | 69.90 | 49.9991 |

The extension is rejected: CPU residency rises 1.28 percentage points
(1.87% relatively). The measurements do not isolate added detection cost
from compiler layout effects. Both builds remain paced at approximately
50 FPS. Rounded sampling percentages are approximate, not cycle counts.
A first startup attempt failed the A440 model confirmation and was
excluded before game timing or profiling; the successful retry confirmed
the model. The USB console had re-enumerated and temporary scripts were
adjusted accordingly.

Correctness validation passed:

- All 190 CPU checks and 100,000 Unicorn cases (seed 1).
- Exhaustive classification of all 65,536 register lists against the
  definition of a nonempty PC-free contiguous range; 120 ranges accepted,
  with correct starting-register indices.
- 222,720 baseline/candidate reference cases across all 136 nonempty
  contiguous lists (including PC-containing fallback cases), nine empty
  or sparse lists, load/store, both S and writeback bits, four address
  modes, base registers R0/R4/R14, user/SVC modes, and eight placements.
  Placements cover ordinary, unaligned, cross-page, watched, slow and
  privileged RAM plus bus access and abort. Comparison includes live and
  banked registers, PC/PSR, permissions, code page, cycles/ticks,
  instruction count, watch state, bus writes and affected RAM. Both traces
  have SHA256
  `0a6ad68a9e616cdf3b26e7f45bb11a23f06bdbbca4e4d3adcbfad2e436e3544e`.
- Identical deterministic host replay: 88,360,660 measured instructions
  and final PPM SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The candidate interpreter SHA256 is
`69ecbb56c51ae8b859c960c6812dec7639d0514b7c7205c85e022a4023342256`.
The original core and exact saved baseline firmware were restored,
retaining only the previously accepted R0-prefix optimization. Scratch
profiles, fixtures and the rejected patch remain outside the repositories
at `/tmp/acorn-block-range-2026-11-06`; no generated files are added here.

The restored A3020 follow-up check measured 49.9990 FPS over 30 seconds.
The board was returned to the 640×480 desktop and the serial port closed.

## Follow-up: STM {LR} and LDM {PC} (rejected)

The next candidate tested exact-list paths rather than general contiguous
range detection. STM `4000` (8.8% of recorded STMs) stores R14 directly
inside the existing direct-page path, after the R0-prefix check. LDM
`8000` (10.5% of LDMs) loads the PC value directly after the prefix check,
then uses the existing shared PC/PSR update. User-bank, permission, watch,
writeback, abort and cycle rules remain. The linked interpreter grows
43,154 → 43,198 bytes (+44), retaining 88 indirect jump sites.

The standard 190 checks and 100,000 Unicorn instructions (seed 1) passed.
The 222,720 block-transfer reference cases were extended to vary loaded
PSR mode bits across all four modes, assert masked IRQ/FIQ inputs before
the instruction and enter any newly enabled interrupt in another run.
The remaining matrix matches the range experiment above. Both state
traces have SHA256
`d20ff7e85cebab9e9659b2e747936dffcccb4050e08b9a2abe5f4f21608190c6`.
The deterministic host replay again matches 88,360,660 instructions and
PPM SHA256
`9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The same A440/ARM2 8 MHz protocol gives:

| Build | ARM2 residency samples (%) | Mean (%) | Lander FPS |
| --- | --- | ---: | ---: |
| Saved baseline | 68.8, 68.9, 68.3, 68.9, 68.5 | 68.68 | 49.9738 |
| Single-register paths | 70.3, 69.2, 70.0, 69.7, 69.8 | 69.80 | 50.0036 |
| Freshly rebuilt baseline after candidate | 68.7, 68.8, 69.0, 68.7, 69.0 | 68.84 | 50.0004 |

The extra control rebuild uses restored source and the same current
configuration/toolchain. All 6,241 defined functions match the saved
baseline byte-for-byte at identical addresses, including the 43,154-byte
`arm2_run` and its original SHA256. Whole app binaries differ, so this
comparison explicitly checks code rather than equating app hashes.
The rebuilt control reproduces lower CPU use: the candidate is 1.12
percentage points above the saved control and 0.96 above the rebuilt
control (1.63% and 1.39% relatively). Rejected. Extra checks and compiler
layout effects are not isolated; skipping scans at these source sites
does not establish an overall gain. Short frame-counter measurements
near the pacing limit vary slightly around 50 FPS.

The candidate interpreter SHA256 is
`35b2d8c891f3a9976f5195c1b4714f6cfd6c155c06f1283dff16622763f8306c`.
The extension was reverted, and the freshly rebuilt baseline firmware
was left installed. Only the previously accepted R0-prefix optimization
remains. Raw evidence and temporary fixtures stay outside the repositories
at `/tmp/acorn-single-block-2026-11-06`; no generated files are added here.

The restored A3020 check after the single-register experiment measured
49.9988 FPS over 30 seconds.
The desktop screenshot was verified and the serial port closed.

A subsequent [direct frame-cycle comparison](frame-cycle-measurement-2026-11-06.md)
re-tested these same single-register paths with an identical opt-in timing
hook in both builds. Mean whole-machine frame-call cost rose from
4,814,738.574 to 4,854,941.346 cycles (+0.835%), while guest instructions per
frame differed by -0.038%. All five candidate windows exceeded all five
baseline windows. The direct measurement supports retaining the rejection;
its whole-frame cycle metric is distinct from sampled ARM2 CPU residency.
The fast paths were again reverted, and the measurement facility retained.
