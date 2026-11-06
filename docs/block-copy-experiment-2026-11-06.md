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
