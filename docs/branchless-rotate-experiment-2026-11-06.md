# ARM2 branchless rotate compilation check

Baseline: shared core `8314212` (CPU source unchanged from `96fd93c`).
Target build: Tab5 ESP32-P4, ESP-IDF 5.5.6, GCC 14.2.0, `-O2`.

## Candidate

Replace the rotate helper's explicit zero-count conditional:

```c
n &= 31;
return n ? (v >> n) | (v << (32 - n)) : v;
```

with masked shifts that also handle zero safely:

```c
n &= 31;
return (v >> n) | (v << ((32 - n) & 31));
```

Both shift counts remain in 0–31, including zero. The helper is used by
immediate operands, register rotations and unaligned word reads. The
candidate adds no table or data RAM and changes no CPU fields or guest
cycle accounting.

## Correctness

- All 190 CPU checks pass.
- All 100,000 randomized instructions agree with Unicorn, seed 1.
- 18,270 rotate checks pass with undefined-behavior sanitization enabled:
  70 input patterns, including every single set bit and every single
  cleared bit, under 261 counts (0–255 and five larger values). Expected
  results use an independent per-bit position mapping.
- 24,576 immediate value/carry checks pass: all 4,096 encodings, both
  incoming carry values and three upper-opcode patterns.
- The deterministic A3020 host replay still executes 88,360,660 instructions
  in frames 9,000–10,000 and produces screenshot SHA256
  `9e9f915326be41b8cb7485beac45621e255ccade404f5b18a4e55e9011392104`.

The saved candidate can be checked without a binary artifact or Unicorn.
These scripts build baseline `8314212` with the saved patch in temporary
directories using the host C compiler:

```sh
python3 docs/measurements/branchless-rotate-2026-11-06/check_rotate.py
python3 docs/measurements/branchless-rotate-2026-11-06/check_immediates.py
```

An optional argument selects another sibling `esp32emu` checkout. The
rotate checker requires a compiler with UBSan support.

## Target compiler result

**No machine-code change.** Both linked builds have exactly the same
43,154 bytes for `arm2_run`, at `0x4ff02368`, SHA256:

```text
05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d
```

The instruction bytes are directly compared as well as fingerprinted.
All uses of the helper in the interpreter therefore compile identically.
The overall application hashes differ and are recorded separately; they
are not used to claim interpreter equivalence.

## Disposition

**Not retained:** the rewrite provides no interpreter speed improvement
in this tested P4 build. Production source is restored to the original
helper. The equivalent candidate is not flashed or separately timed on
the board; doing so would compare identical interpreter instructions.
No candidate FPS or CPU-residency improvement is claimed.

The existing baseline firmware remains installed. Five baseline A440
Lander profiles and a frame-count interval were collected while the
candidate compiled; they are saved as control observations, not paired
candidate measurements. The original A3020 setting is restored afterward.

[Artifacts](measurements/branchless-rotate-2026-11-06/) contain the patch,
reproducible checks, CPU/differential results, identical host replay,
code/application fingerprints, direct byte comparison and baseline
observations. Private matching ELFs, binaries and full logs remain under
`/tmp/acorn-branchless-rotate-2026-11-06/`; ROMs, discs and boot/network
logs are excluded from the repository.

The original A3020/ARM250 at 12 MHz, RISC OS 3.11 and 4 MB RAM was
restored. Its final Lander check completed 1,503 frames in 30.040 seconds:
**50.034 FPS**, 100.067% real time. The emulator was reset and visually
verified at the 640x480 RISC OS desktop. The serial port was released.
All 190 restored CPU checks pass. No outlined `ror32` symbol exists in
any of the three target builds; the helper is inlined.
