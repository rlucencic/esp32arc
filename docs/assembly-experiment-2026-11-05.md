# ARM2 assembly experiment on Tab5

Date: 2026-11-05. This follows the project-by-project assessment in
[`esp32emu/docs/assembly-optimization-assessment.md`](../../esp32emu/docs/assembly-optimization-assessment.md).

## Scope and equipment

- Physical M5Stack Tab5, ESP32-P4 revision 1.0, 360 MHz, 32 MB PSRAM.
- Native USB console: `/dev/ttyACM1` on this Linux machine.
- ESP-IDF 5.5.6, ESP RISC-V GCC 14.2.0, `-O2` for the ARM2 core.
- Existing Acorn configuration: A3020/ARM250, 12 MHz emulated clock,
  4 MB RAM, RISC OS 3.11, 640x480 4-bpp screen, sound enabled.
- Both builds use the same configuration and dependencies. Firmware was
  flashed with its matching ELF used to symbolize the profiler samples.
- The board originally ran esp32cpc. Its complete 16 MB flash was backed
  up before the Acorn bootloader, partition table, application and ROMs
  were installed. NVS was not erased; the SD card was not reformatted.

## Baseline and target selection

The unmodified shared ARM2 core passed all **190 host checks**. The desktop
Archimedes build also booted RISC OS 3.11 for 1,000 frames and produced a
640x256 screenshot (A440 default on the host; this is a functional smoke
check, not a performance comparison with the A3020 board).

Hardware desktop sampling found:

| Core | Dominant samples | Interpretation |
| --- | --- | --- |
| 1, emulation | 35.8% `arm2_run`, 60.2% idle, 1.7% `sound_run` | ARM execution accounts for about 90% of non-idle samples. |
| 0, presenter/runtime | 94.5% idle | Static desktop rendering is not a useful first assembly target. |

These desktop captures used 20,000 samples at 20 kHz. They describe a
static desktop only; they do not establish VIDC cost in animated games.
An initial capture while the board was still in setup was discarded.

## Experiment

Replaced `sub_flags()` in the shared `components/arm2/arm2.c` with an inline
RISC-V sequence computing N, Z, C and V explicitly. This affects CMP, SUB,
RSB, SBC and RSC. The remainder of instruction dispatch, timing, memory
and machine integration stayed the same. The candidate was P4-only, with
the original C used on other targets.

Before initializing the emulated CPU, the experimental build checked the
actual assembly against a C reference for **20,162 combinations**: all
pairs of nine boundary operands, plus 10,000 deterministic random pairs,
each with and without borrow. A mismatch traps. The board completed this
check, booted RISC OS and ran the guest benchmark without trapping. This
checks the changed flag helper, not every instruction on the hardware.
The portable host tests also still passed 190/190; those exercise the C
fallback and must not be mistaken for testing RISC-V assembly.

The tested patch, including its temporary startup self-check, is preserved
at [rejected-sub-flags.patch](measurements/assembly-2026-11-05/rejected-sub-flags.patch).
It is an experiment record, not an enabled optimization.

### Guest workload

BBC BASIC assembled this program in guest RAM, then repeatedly called it:

```text
10 DIM c% 100
20 P%=c%
30 [OPT 0
40 mov r0,#0
50 .l add r0,r0,#1
60 cmp r0,#&4000000
70 bne l
80 mov pc,r14
90 ]
100 REPEAT:CALL c%:UNTIL FALSE
```

The steady loop stresses fetch/decode, immediate ADD/CMP, flag generation
and conditional branches. It does not represent memory-intensive code,
LDM/STM, the full barrel shifter, or Lander. Input commands were kept
below the console's 128-byte line limit; an earlier truncated input was
cleared with `NEW` before collecting baseline benchmark samples.

Each steady-state profile contains 32,000 samples at 20 kHz (1.6 seconds
of sampling). Captures are repeated; the profiler and background tasks
can perturb these short measurements. Percentages are sampled residency,
not cycle-accurate kernel timings or maximum-throughput MIPS.

## Results and disposition

| Build | `arm2_run` samples | Idle samples |
| --- | --- | --- |
| Original C, run 1 | 60.5% | 35.6% |
| Original C, run 2 | 60.5% | 35.6% |
| Assembly, run 1 | 61.0% | 35.1% |
| Assembly, run 2 | 60.9% | 34.9% |
| Assembly, run 3 | 60.4% | 33.8% |
| Original C restored, run 1 | 59.8% | 36.1% |
| Original C restored, run 2 | 59.5% | 36.5% |
| Original C restored, run 3 | 60.2% | 35.8% |

The third assembly capture also had 1.8% cross-core interrupt samples,
illustrating background variation. Both builds maintained approximately
50 frames/s, or 100% real time. The candidate provides **no useful measured
improvement** on this workload. Do not interpret these results as proof
that all assembly approaches, or other ARM2 workloads, cannot improve.

`arm2_run` grew from 31,982 to 32,296 bytes (**+314 bytes**). The application
grew from 2,001,248 to 2,001,856 bytes (+608 including the self-check and
other layout effects).

The candidate was rejected. The shared CPU source was restored, and the
original baseline binary was reflashed for a second reference pass.
The restored C results confirm that the assembly change did not improve
the measured workload. Rebuilding after restoring the source reproduced
the baseline application binary byte for byte (matching SHA256).
The guest was reset after the final benchmark and visually checked back
at the RISC OS desktop. The USB serial port was closed and released.

Hardware captures: [candidate running the benchmark](measurements/assembly-2026-11-05/candidate-benchmark.png)
and [restored C firmware at the desktop](measurements/assembly-2026-11-05/restored-desktop.png).

Raw function summaries are in [measurements/assembly-2026-11-05](measurements/assembly-2026-11-05/).
A candidate desktop sample taken shortly after boot is preserved there,
but is excluded from the A/B table because startup/CMOS work was still
visible and it does not match the settled baseline desktop.

## Reproduction and artifacts

From esp32arc, with the installed IDF environment active:

```sh
idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig build
python ../esp32emu/tools/emu_console.py /dev/ttyACM1 profile build-tab5/esp32arc.elf 32000 30 1
```

Baseline application SHA256:
`b7c9ea0f33e7d7acad71d89d05e35881cf36786ee66ace5bf4d541a433bc027f`.

Candidate application SHA256:
`6904c98edd99f5b21c64d89af3b7cf3b374f9d347e752518b99e216916335158`.

Working artifacts (matching ELFs/binaries, source disassembly, raw PC
samples, serial logs and screen captures) are under
`/tmp/acorn-assembly-2026-11-05/`. The original full-board flash backup is
also retained under the ignored `build-tab5/` directory as
`tab5-before-acorn-2026-11-05.bin`; it contains device settings and is not
part of the source repository.

One permanent tooling fix was needed: `esp32emu/tools/emu_console.py` now
recognizes Linux `ttyACM*` native USB names, including resolved symlinks,
so opening them uses the same DTR/RTS handling as macOS `usbmodem` ports.

## Next optimization target

CPU execution remains the first target. The baseline already compiles
rotate expressions into shifts/OR without an extra zero-count branch;
wrapping that expression in assembly would not remove the suspected work.
The broader next experiment should address opcode decode/state access
or the dispatch-to-handler boundary, with mixed guest instruction and
Lander measurements. Compare compact optimized C as well as assembly.
