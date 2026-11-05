# ARM2 operand decode and conditional dispatch experiments

Follow-up to [the subtraction-flags experiment](assembly-experiment-2026-11-05.md).
Date: 2026-11-05. All hardware measurements use the connected M5Stack
Tab5, ESP32-P4 rev. 1.0 at 360 MHz, A3020/ARM250 at 12 MHz, 4 MB RAM,
RISC OS 3.11, sound enabled, ESP-IDF 5.5.6 and GCC 14.2.0 with `-O2`.

## Workloads and baseline

Lander was launched from the existing `RO2Apps2.adf` on the SD card with
F12 followed by `run adfs::0.$.!Lander`. No game input was held during
the measurements. Its screen is 320x256, 8 bpp, with a 70.08 Hz emulated
video raster. The runtime advances the machine in 50 emulation frames
per second; reported runtime FPS and speed refer to these frames.

The initial baseline sampled 30 web status responses, one second apart
plus request latency. It averaged **98.2% real-time speed** (49.09 runtime
FPS), with core 1 fully busy. A separate 32,000-sample profile attributed
95.7% of core 1 to `arm2_run`. Core 0 was 83.1% idle, with 5.9% in VIDC
pixel conversion and 4.1% in `memcpy`. CPU execution remains the first
target even while the game is drawing.

A deterministic host replay started Lander after boot and measured the
instruction mix from frame 9,000 to 10,000: 88,360,660 instructions,
39.8% data processing with register operands, 18.1% with immediates,
and 11.3% skipped because their condition failed. All three candidates
reproduced the baseline instruction count, instruction mix and final
320x256 screenshot byte for byte.

The second workload is the guest BBC BASIC-assembled ADD/CMP/BNE loop
documented in the earlier experiment. Each profile uses 32,000 samples
at 20 kHz. Samples show residency, not cycle-accurate instruction costs.

## Candidates tested separately

1. **Unrotated immediate fast path.** Return the low byte before doing
   the rotate when the encoded rotate count is zero. Preserve the old C
   flag for an unrotated operand. This tests small constants used by ADD,
   CMP and other immediate operations. `arm2_run` shrank by 44 bytes.
2. **Register shift-field decode.** Extract shift type and immediate
   count together as one seven-bit field. A zero field is an unshifted
   register and takes a single early check, instead of checking type and
   then amount separately. R15 operands, all other shifts and carry
   behavior retain their existing handling. Code grew by 14 bytes.
3. **Direct conditional data-processing dispatch.** Conditional opcode
   table entries identify the data-processing operation and operand form
   before checking its condition. Passing conditions jump directly to
   the corresponding existing handler, avoiding a second opcode-table
   lookup and indirect dispatch. Failing conditions still advance as
   skipped instructions. The table remains 4,096 entries. Code grew by
   462 bytes; existing compiler dispatch merging remains enabled.

These are portable C experiments. Inspection of the generated RISC-V
instructions was used to check what changed. No handwritten assembly
was added for operations that the compiler already emits directly.

## Correctness checks

Each candidate passed:

- All 190 existing ARM2 host checks.
- 100,000 randomized instructions against Unicorn 2.1.4, seed 1.
- The deterministic 10,000-frame host Lander replay, with identical
  screenshot SHA256:
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.
- Hardware boot, desktop operation, Lander launch and the guest loop.

The host replay uses A3020, 4 MB, the same ROM and applications disc.
Its script taps F12 at frames 6,000/6,005, types the launch command at
6,020, and runs to frame 10,000. The host's typing delay is included in
the script runner's scheduling. Host execution speed is not used to
predict Tab5 performance.

## Initial hardware results

| Build | Lander mean runtime FPS | Mean real-time speed | Guest loop `arm2_run` samples | Interpreter bytes |
| --- | ---: | ---: | --- | ---: |
| Baseline | 49.090 | 98.20% | 60.3%, 59.4%, 60.2% | 31,982 |
| Immediate fast path | 48.953 | 97.93% | 60.3%, 60.6%, 60.3% | 31,938 |
| Register shift-field decode | 49.507 | 99.07% | 59.6%, 59.5%, 59.1% | 31,996 |
| Direct conditional dispatch | 49.183 | 98.40% | 60.1%, 59.9%, 60.2% | 32,444 |

One baseline loop capture had 1.9% cross-core interrupt samples; its lower
`arm2_run` percentage does not establish faster CPU execution. Lander
status collection itself creates HTTP/network work on core 0. The small
differences in this initial table require longer confirmation.

## Confirmation method

The best initial candidate is compared with restored C using guest frame
counts over a full minute. Start/end `i` console responses provide the
emulation frame counters; monotonic host timestamps bracket each response.
The measurement reports frames divided by elapsed time and by the
runtime's 50 Hz target. Unlike the initial status polling, this test does
not generate HTTP traffic during the measured interval. The game must
be in 320x256 8-bpp mode before timing begins.

The restored baseline completed 2,953 frames in 60.085 seconds:
49.147 FPS, or **98.294%** real-time speed. Its `arm2_run` machine-code
SHA256 exactly matches the initial baseline. The application hashes
differ because the earlier commit changed build metadata; the interpreter
code, compiler and configuration used for the baseline remain the same.

The register shift-field candidate completed **2,975 frames in 60.099
seconds**, or 49.501 FPS and **99.003%** real-time speed. This is a
**0.721% relative throughput gain** over the restored baseline. The
minute-long result agrees with the smaller lead in the initial status
samples. It is a small gain on this workload, not a claim of the same
speedup for all ARM software or enough headroom for every game.

**Kept:** register shift-field decode in the shared ARM2 core.
**Rejected:** immediate early return and direct conditional dispatch;
neither provided a useful Lander improvement in this test. Candidate
patches remain in the measurement artifacts for reference.

The final source adds an explanatory comment to the tested register
candidate. Rebuilding preserves the exact tested `arm2_run` machine-code
SHA256 `05e4947500a8d64f9d0fe52fe962bfd6edc8687b9997f011b4e65a892dfe6248`;
the app/ELF fingerprints change with debug/build metadata. The final
Tab5 build and all 190 host checks passed again.
The final build was flashed and visually checked back at the RISC OS
desktop; the serial port was released. The [desktop capture](measurements/operand-decode-2026-11-05/retained-desktop.png)
and final application fingerprint are included with the measurements.

The reusable tool is [`tools/measure_lander.py`](../tools/measure_lander.py):

```sh
python tools/measure_lander.py /dev/ttyACM1 result.json --boot --seconds 60
```

Use `--boot` after flashing while the standard Tab5 setup screen is open.
Otherwise launch Lander first and omit the flag. Drive :0 must contain
the applications disc. The script checks the video mode, measures guest
frame progression and closes the port. Its saved log filters out boot
and network messages.

## Artifacts

Durable profiles, status samples, frame-count results, screenshots,
candidate patches, test output and binary/code fingerprints are under
[`docs/measurements/operand-decode-2026-11-05`](measurements/operand-decode-2026-11-05/).
Working binaries, matching ELFs, disassembly and full logs are under
`/tmp/acorn-decode-2026-11-05/`. ROMs and the applications disc are private
test inputs and are not added to the repository.
