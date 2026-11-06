# ARM2 condition lookup experiment

Baseline: shared core `ca5ba3d` (same CPU source as `96fd93c`). Physical
Tab5: ESP32-P4 rev. 1.0 at 360 MHz, ESP-IDF 5.5.6, GCC 14.2.0, `-O2`.
Hardware comparisons select A440/ARM2 at 8 MHz, RISC OS 3.11, 4 MB RAM,
sound enabled. Lander is paced at 50 FPS, leaving measurable CPU headroom.

## Candidate

The baseline updates `cond_mask` from the 16-entry condition table after
every flag change, PSR write and run entry. Conditional instructions
then load that cached mask. The candidate reads the same table using
the current PSR only when evaluating a conditional instruction. The AL
and NV dispatch entries retain their existing direct paths.

This removes the eager cache updates and the now-unused `cond_mask`
member of `arm2_t`. All consumers must rebuild against the updated
header. The Tab5 application and host harness are rebuilt together.
Removing the member changes subsequent struct offsets, so code layout and register
allocation effects accompany the lookup change.

Host mix reporting now counts AL, NV and checked condition fields,
including instructions whose conditions fail. Lander's deterministic
replay executes 57,592,451 AL instructions and 30,768,209 checked
conditional instructions (34.8%), with no NV instructions. Its established
mix has 27.7% flag-setting data-processing instructions.

The linked `arm2_run` shrinks from 43,154 to 40,390 bytes (-2,764).
The existing 32-byte condition table and 4,096-entry dispatch table remain.

## Correctness

- All 190 ARM2 CPU checks pass.
- All 100,000 randomized instructions agree with Unicorn, seed 1.
- An independent Boolean oracle checks all 16 conditions under all 16
  NZCV combinations, for MOV, B and BL. Each runs both directly from the
  initial flags and after a TEQP changes flags within the same interpreter
  run: 1,536 cases pass. The check verifies instruction execution, PC,
  link register and resulting flags.
- The deterministic A3020 host replay executes the same 88,360,660
  instructions in frames 9,000–10,000 and produces screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The focused condition check rebuilds the saved candidate from committed
baseline `ca5ba3d` and the saved patch with the host C compiler:

```sh
python3 docs/measurements/condition-lookup-2026-11-06/check_conditions.py
```

An optional argument selects another `esp32emu` checkout. The script
builds a temporary wrapper around that candidate core; no saved binaries
or Unicorn installation are required for this focused check. It does
not exhaust debugger edits, bus callbacks or processor-mode transitions;
the broader CPU cases and replay provide additional coverage.

## Hardware method

Each build starts Lander from the same applications disc, warms up,
checks frame progress over 30 seconds, then receives five separate
32,000-sample core 1 profiles at 20 kHz. Timing and sampling are separate.
Residency percentages measure interpreter CPU time at a paced frame
rate, not uncapped FPS. Baseline and candidate are measured in this
session with matching machine configurations.

| A440 Lander | Baseline | On-demand lookup |
| --- | --- | --- |
| ARM2 residency captures | 68.5%, 69.2%, 68.2%, 68.4%, 68.9% | 69.7%, 69.1%, 68.9%, 69.5%, 69.0% |
| Mean residency | 68.64% | 69.24% |
| Frame-count interval | 1,503 / 30.056 s | 1,502 / 30.040 s |
| Paced frame rate | 50.007 FPS | 50.000 FPS |

## Disposition

**Rejected.** These short captures show no CPU improvement for Lander:
mean interpreter residency rises by 0.60 percentage points, approximately
0.87% relatively. Individual captures overlap, so this is a modest
observed difference, not proof of a universal slowdown. Both builds hold
real time. The 2,764-byte code saving does not justify retaining the
change for the current performance objective.

Production source and the public CPU header are restored to the committed
baseline. The restored P4 interpreter matches the baseline byte for byte:
43,154 bytes, SHA256
`05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d`.
All 190 restored CPU checks pass. The board receives the exact previously
tested baseline binary; `builds.json` also fingerprints the separately
rebuilt application. The focused test builds the saved candidate from
its patch, so it remains reproducible after restoring production source.

[Artifacts](measurements/condition-lookup-2026-11-06/) contain the patch,
condition checker, CPU and differential results, host replay/screenshot,
model confirmations, all ten profiles, timing intervals and fingerprints.
Matching private ELFs, binaries and full logs remain under
`/tmp/acorn-condition-lookup-2026-11-06/`; ROMs, discs and boot/network logs
are excluded from the repository.

The original A3020/ARM250 at 12 MHz, RISC OS 3.11 and 4 MB RAM was
restored. The final game check on restored firmware completed 1,502
frames in 30.041 seconds: **49.998 FPS**, 99.997% real time. The emulator
was reset and visually verified at the 640x480 RISC OS desktop. The
serial port was released.
