# ARM2 constant-condition dispatch experiment

Baseline: shared core `1acf375` (CPU source unchanged from `96fd93c`).
Physical Tab5: ESP32-P4 rev. 1.0 at 360 MHz, ESP-IDF 5.5.6, GCC 14.2.0,
`-O2`. Hardware comparison uses A440/ARM2 at 8 MHz, RISC OS 3.11, 4 MB
RAM and sound enabled. Lander holds a paced 50 FPS on this model, leaving
CPU headroom to measure.

## Candidates

The 4,096-entry instruction dispatch table already encodes the condition
field. The baseline sends the fourteen checked conditions to shared
handlers which extract bits 31–28 and dynamically shift the cached
condition mask. The full candidate gives each condition a handler that tests
its known constant bit in `cond_mask`.

Each condition has handlers for ordinary instructions, B and BL. Failed
conditions take the existing next-instruction path; ordinary instructions
that pass are dispatched through the same AL opcode table. Branches
that pass enter the existing B or BL body. AL and NV retain their direct
paths. The condition cache, PSR updates, table dimensions, CPU struct and
guest cycle accounting are unchanged.

The established deterministic Lander replay checks conditions on 34.8%
of instructions. This experiment tests whether removing dynamic condition
extraction and shifting improves that path without moving lookup work
onto conditional instructions, as the rejected on-demand lookup did.

The linked `arm2_run` shrinks from 43,154 to 42,320 bytes (-834).
The compiler also changes code layout and indirect dispatch sites, so
the timing comparison evaluates the compiled change as a whole.
The P4 disassembly confirms that EQ uses a mask load, constant AND and
branch, removing the prior condition-field shift and dynamic mask shift.
The saved excerpts contain all 42 condition-mask load sites in the
candidate, versus the three shared sites in the baseline.

A second candidate specializes EQ and NE only, retaining the three shared
handlers for the other twelve checked conditions. It shrinks the linked
interpreter to 41,082 bytes (-2,072 versus baseline), with two additional
indirect jump sites rather than thirteen. Both variants retain the
same cache and architectural behavior.

## Correctness

- All 190 CPU checks pass.
- All 100,000 randomized instructions agree with Unicorn, seed 1.
- All 1,536 focused condition cases pass an independent Boolean oracle:
  every NZCV combination, every condition including AL/NV, MOV/B/BL,
  both directly and after an in-run TEQP changes the flags. The checks
  cover execution, PC, link register and resulting flags.
- The deterministic A3020 host replay still executes 88,360,660 instructions
  in frames 9,000–10,000 and produces screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

Both candidates pass the checks above. The full candidate can be checked without binary artifacts or Unicorn:

```sh
python3 docs/measurements/condition-dispatch-2026-11-06/check_conditions.py
```

This builds committed baseline `1acf375` with the saved patch in a
temporary directory using the host C compiler. An optional argument
selects another sibling `esp32emu` checkout. Add `--variant eqne` to
check the smaller candidate instead. It does not exhaust every
opcode and bus/debugger interaction; the broader CPU tests and replay
provide additional coverage.

## Hardware method

Both builds start Lander from the same applications disc and warm up,
then measure frame progress over 30 seconds and capture five separate
32,000-sample core 1 profiles at 20 kHz. Timing and sampling are separate.
Residency percentages measure interpreter CPU use at a paced frame rate,
not uncapped FPS. Both builds are measured in this session.

| A440 Lander | Baseline | All checked conditions | EQ/NE only |
| --- | --- | --- | --- |
| ARM2 residency captures | 68.7%, 68.6%, 68.7%, 68.7%, 68.8% | 72.1%, 71.3%, 72.1%, 71.3%, 71.6% | 70.1%, 69.7%, 70.5%, 70.8%, 70.4% |
| Mean residency | 68.70% | 71.68% | 70.30% |
| Frame-count interval | 1,504 / 30.083 s | 1,503 / 30.040 s | 1,503 / 30.041 s |
| Paced frame rate | 49.995 FPS | 50.033 FPS | 50.032 FPS |

## Disposition

**Both rejected.** Full condition specialization increases mean ARM2
residency by 2.98 percentage points, approximately 4.34% relatively.
Specializing only EQ/NE increases it by 1.60 points, approximately 2.33%
relatively. Both maintain paced real time, as does the baseline. These
are short sampled comparisons of this workload, not universal results
for all ARM software.

The disassembly confirms that constant condition checks use fewer
instructions. Changes in handler placement, branch sites and compiler
optimization accompany those checks; this experiment does not isolate
which effects cause the regression. Code size alone does not predict
the outcome on this P4 workload.

The source is restored to the committed baseline. The board receives
the exact previously tested baseline binary, with interpreter SHA256
`05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d`.
Both candidate patches and their checks remain reproducible after the
production source is restored.

[Artifacts](measurements/condition-dispatch-2026-11-06/) include both
patches, the variant-selectable condition checker, CPU/differential
results, identical host replay outputs/screenshots, disassembly excerpts,
model confirmations, all fifteen profiles, timing intervals and target
fingerprints. Private matching ELFs, binaries and full logs remain under
`/tmp/acorn-condition-dispatch-2026-11-06/`; ROMs, discs and boot/network
logs are excluded from the repository.

After restoring the source, all 190 CPU checks pass and the rebuilt P4
interpreter matches the baseline byte for byte (43,154 bytes, same SHA256
above). The separately rebuilt application's overall fingerprint is also
saved; the board is flashed with the original baseline binary.

The original A3020/ARM250 at 12 MHz, RISC OS 3.11 and 4 MB RAM was
restored. Its final Lander check completed 1,502 frames in 30.042 seconds:
**49.997 FPS**, 99.994% real time.
The emulator was then reset and visually verified at the 640x480 RISC OS
desktop. The serial port was released.
