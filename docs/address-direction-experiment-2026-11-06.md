# ARM2 single-transfer direction dispatch experiment

Baseline: shared core `96fd93c`, including the contiguous-register copy
path. Target: physical Tab5 ESP32-P4 rev. 1.0 at 360 MHz, ESP-IDF 5.5.6,
GCC 14.2.0, `-O2`. Both hardware comparisons select A440/ARM2 at 8 MHz,
RISC OS 3.11, 4 MB RAM, sound enabled. Lander is paced at 50 FPS, leaving
measurable idle CPU time on this model.

## Candidate

The 4,096-entry instruction dispatch table already includes bit 23,
which chooses whether LDR/STR adds or subtracts the offset. Previously,
both entries selected the same handler and decoded that bit at runtime.
The candidate selects separate up/down handlers for each byte/word,
load/store and immediate/register-offset combination. `single_transfer`
receives a constant direction, allowing the compiler to remove the
runtime direction check. Pre/post-index and writeback remain dynamic.
Condition checks and undefined register-offset encodings retain their
existing handling.

LDR, STR, LDRB and STRB account for 5.4%, 1.2%, 1.4% and 8.0% of the
established deterministic host Lander instruction mix. The linked
candidate grows `arm2_run` from 43,154 to 45,934 bytes (+2,780). The
dispatch table still has 4,096 entries.

## Correctness

- All 190 CPU checks pass.
- All 100,000 randomized instructions agree with Unicorn, seed 1.
- 65,536 targeted single transfers match the committed baseline: all
  load/store, byte/word, pre/post, up/down, writeback and offset-form bits,
  all 16 base and destination registers, and four address alignments.
  Register offsets exercise plain and shifted Rm, including aliases.
  Each case compares registers, PC, full PSR, emulated cycles, reported
  exception and all 64 KB of RAM. This is regression coverage against
  the baseline, not an independent ARM2 architectural oracle. It does
  not exhaust page permissions, processor modes or offset values.
- The deterministic A3020 host replay runs the same 88,360,660 instructions
  in frames 9,000–10,000 and produces screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The targeted comparison builds baseline `96fd93c` and the saved candidate
patch in a temporary directory; no binary artifacts are required:

```sh
python3 docs/measurements/address-direction-2026-11-06/check_transfers.py
```

It uses the sibling `esp32emu` checkout and the host C compiler. An
optional argument selects another shared-core checkout.

## Hardware comparison

Each build starts Lander from the same applications disc, warms up,
measures frame progress over 30 seconds and captures five separate
32,000-sample core 1 profiles at 20 kHz. Timing and sampling are separate.
Residency percentages measure interpreter CPU use at the paced frame
rate, not uncapped FPS.

| A440 Lander | Baseline | Direction dispatch |
| --- | --- | --- |
| ARM2 residency captures | 68.9%, 68.5%, 68.3%, 68.7%, 68.6% | 69.6%, 69.4%, 69.6%, 69.7%, 69.4% |
| Mean residency | 68.60% | 69.54% |
| Frame-count interval | 1,504 / 30.072 s | 1,502 / 30.042 s |
| Paced frame rate | 50.013 FPS | 49.997 FPS |

## Disposition

**Rejected.** Interpreter residency increases by 0.94 percentage points,
approximately **1.37% relatively**, for 2,780 additional bytes of code.
These five short captures per build favor the original handlers in this
workload. Both still maintain real time. Fewer runtime checks do not
necessarily compensate for changes in code size, layout and register
allocation; this experiment does not isolate which of those caused the
regression.

The candidate patch is saved for reproduction; production source is
restored to `96fd93c`. The board is reflashed with the exact previously
tested baseline binary, whose interpreter SHA256 is
`05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d`.

[Artifacts](measurements/address-direction-2026-11-06/) include the
candidate patch, reproducible transfer comparison, CPU/differential
results, deterministic replay output/screenshot, target fingerprints,
all ten profiles, timing intervals and model confirmations. Matching
private ELFs, binaries and full logs remain under
`/tmp/acorn-address-direction-2026-11-06/`; ROMs, discs and boot/network
logs are excluded from the repository.

After restoring the source, the P4 build's interpreter matches the
baseline byte for byte (43,154 bytes and the same SHA256 above), and all
190 restored CPU checks pass. The rebuilt application's overall hash
changes with commit metadata; the board receives the original baseline
binary, recorded separately in `builds.json`.

## Follow-up: plain register offsets (no useful gain)

A separate candidate added an early return to `offset_register` for
`(op & 0xFF0) == 0`. Plain Rm offsets then bypass the general LSL/LSR/ASR/ROR
decode. Register selection, R15/PSR semantics and all memory-transfer paths
remain unchanged. P4 disassembly confirms a shift/mask and branch before
the general shifter. The interpreter grows from 43,154 to 43,258 bytes
(+104), at the same address `0x4ff02368`.

The comparison uses the opt-in [frame-cycle hook](frame-cycle-measurement-2026-11-06.md)
in both builds: Tab5 at 360 MHz, A440 / ARM2 8 MHz, RISC OS 3.11, 4 MB,
Lander in 320×256 8-bit mode. Each row has a separate 30-second guest
pacing check and five captures of 250 machine frames. The candidate and
restored control use the same fresh-flash `--boot` startup sequence; the
first baseline was already running after a model restart and game launch.

| Build | Mean cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction | FPS |
| --- | ---: | ---: | ---: | ---: |
| Initial baseline | 4,805,134.200 | 61,389.663 | 78.2727 | 50.0059 |
| Plain-offset candidate | 4,808,186.863 | 61,370.842 | 78.3464 | 49.9949 |
| Restored baseline control | 4,798,565.498 | 61,368.129 | 78.1931 | 50.0000 |

The candidate costs 0.064% more cycles/frame than the initial baseline
and 0.201% more than the restored control. Guest instruction counts per
frame differ by -0.031% and +0.004%, respectively. Capture ranges overlap:
this establishes no useful gain, rather than a precise small regression.
All fifteen windows completed without invalid frames or observed clock
changes. These are whole-machine frame-call costs, including device work
and preemption, not isolated ARM2 instruction costs.

Cycles/frame for the individual windows:

- Initial baseline: 4,798,808.060; 4,812,665.640; 4,807,045.496;
  4,807,336.540; 4,799,815.264.
- Candidate: 4,807,642.716; 4,785,929.968; 4,804,362.504;
  4,817,201.212; 4,825,797.916.
- Restored control: 4,799,408.604; 4,802,120.224; 4,799,157.700;
  4,797,196.892; 4,794,944.068.

Validation passed all 190 CPU checks and 100,000 Unicorn differential
instructions (seed 1). Another 4,718,592 baseline/candidate offset-helper
comparisons passed under UBSan: all sixteen Rm fields, 128 valid immediate
shift encodings, sixteen NZCV values, four modes, four PCs including
26-bit wrap boundaries, and nine register values. Independent 2,000-frame
A3020 host desktop runs match 25,082,558 measured instructions from frame
1,000, the full reported instruction mix and final PPM SHA256
`82b7ce6633d9645f50ef8b7fb1ec976e0e688c216ec274f939992af481f5c74c`.
The old Lander host replay inputs were unavailable, so that replay was
not repeated; the hardware comparison uses Lander.

The candidate was reverted. A fresh baseline rebuild matches the saved
instrumented interpreter byte-for-byte at the same address, SHA256
`7b7173e6e558e62219fce13393863fa6b46b380bdc30a06ac65077b6bcfc85f4`.
Candidate interpreter SHA256:
`17aeebc4496b95e7e9eb16d3f1396e54e68240502510b1db660afef4d2654548`.
The exact saved baseline firmware was restored for the control run.
Scratch builds, patch, reference checker and captures remain outside the
repositories at `/tmp/acorn-offset-2026-11-06`.
The board was returned to the A3020's 640×480 desktop, its screenshot
verified, and the serial port closed. No cycle capture remains running.

The original A3020/ARM250 at 12 MHz, RISC OS 3.11 and 4 MB RAM was
restored. Its final Lander check on the restored firmware completes
1,503 frames in 30.046 seconds: **50.024 FPS**, 100.047% real time.
The emulator was then reset and visually verified at the 640x480 RISC OS
desktop. The serial port was released.
