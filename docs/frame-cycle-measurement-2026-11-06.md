# Direct machine-frame cycle measurements

Added 2026-11-06 to supplement interrupted-PC residency profiles. Developer
console timing is opt-in; it does not add ARM2 instruction instrumentation.

## Use

Start Lander from drive :0 with RO2Apps2.adf mounted, then run in the
ESP-IDF Python environment:

```sh
python tools/measure_lander.py /dev/ttyACM0 /tmp/lander-cycles.json \
  --seconds 30 --cycle-frames 250 --cycle-captures 5
```

The port name can change after reconnecting the board. Without
`--cycle-frames`, the existing frame-counter timing behavior is retained.
`--boot` still starts the setup screen and launches the game after a flash.

The tool first times guest frames without cycle capture, then takes the
requested capture windows. Each uses exactly 1–1,000 machine frames
(250 frames is about five seconds at 50 Hz). JSON stores totals, minimum
and maximum frame-call cycles, wall microseconds, guest instruction counts,
clock frequency and derived costs per frame/instruction. Start/end guest
frame information and observed pacing accompany the windows.

Manual console commands:

- `C 250`: start a 250-frame capture. A running capture rejects another
  start with `busy`; it does not reset the counters.
- `C`: query idle/running/done/stopped state and counters.
- `C stop`: stop a capture without resetting the machine.

`EMUCYCLES` responses use compact JSON keys to fit the console's 256-byte
line buffer even with maximum-width counter values (at most 247 bytes).
The Python tool expands the short counter keys into descriptive field names.
Capture IDs detect replacement/restart. On failure, the tool stops only a
capture it owns and always closes the port.

## Meaning and limits

Two hardware cycle-counter reads bracket `arc_run_frame` in the board's
frame callback on emulation core 1. The existing runtime's audio/display
pacing waits are outside this call. Device events, sound generation,
disc-space work and any interrupts/preemption inside the call are included.
This is **whole machine-frame elapsed CPU-clock time**, not isolated ARM2
execution, retired instruction counts, or a replacement for profiling.
The cycles/guest-instruction ratio includes device work as well.

Use a fixed CPU frequency and compare identical workloads/configurations
with the same measurement hook in both builds. The tool rejects observed
clock changes and invalid or incomplete captures. Captures should also be
compared by guest instruction counts to detect workload changes. A 50 FPS
cap can hide throughput improvements; cost per emulated frame provides
another measurement without a 20 kHz sampling interrupt.

The 32-bit hardware counter is subtracted modulo 2^32 for each frame,
then accumulated in 64 bits. Crossing a rollover is valid; a single frame
lasting a full counter period cannot be recovered. A wall timer flags such
frames as invalid (about 11.93 seconds at 360 MHz). Captures can span many
counter rollovers. The wall timer reads and status collection sit outside
the cycle bracket. The counter reads themselves have a small fixed cost.

The inactive callback has a short state check; it does not read the cycle
counter or collect frame status. Capture state is protected between the
console and emulation cores. No lock is held during emulation. A stopped
in-flight frame cannot be added to a newly started capture.

## Initial hardware validation

Tab5 ESP32-P4 at 360 MHz, A440 / ARM2 8 MHz, RISC OS 3.11, 4 MB, sound on,
Lander in 320×256 8-bit mode. Initial guest pacing: 49.9995 FPS.
Five 250-frame captures produced these cycles per frame:

| Window | Cycles/frame | Guest instructions captured |
| --- | ---: | ---: |
| 1 | 4,812,345.788 | 15,352,838 |
| 2 | 4,812,861.892 | 15,349,816 |
| 3 | 4,800,828.236 | 15,337,255 |
| 4 | 4,824,336.088 | 15,354,071 |
| 5 | 4,823,320.864 | 15,355,553 |

Mean: 4,814,738.574 cycles/frame. The full window range is 0.488% of that
mean, so gains of a few tenths of a percent still need longer windows and
repeated controls. Wall timing differs from cycle-derived time by about
1.0–1.1 microseconds per frame, consistent with timer reads outside the
cycle bracket; this is not an independently measured instrumentation cost.

Protocol tests passed for invalid arguments, busy rejection, preservation
of another capture when the Python tool receives `busy`, stop/restart,
and an exact one-frame result. A 1,000-frame capture accumulated
4,810,172,656 cycles, exceeding 32 bits with no invalid frames.

The ARM2 source is unchanged by the hook. The linked interpreter remains
43,154 bytes at the same address. Compared with the prior uninstrumented
build, 77 instructions differ only in address immediates for data/external
calls (`addi`, `auipc`, `jalr`); no instruction locations or mnemonics change.
Those relocations mean these timings should not be substituted into old
uninstrumented comparisons.

Raw validation captures and protocol records stay outside the repositories
under `/tmp/acorn-frame-cycles-2026-11-06`.

## Re-testing the single-register transfer candidate

The previously rejected STM `{LR}` / LDM `{PC}` paths were re-applied to a
build with exactly the same frame timing hook. Their existing CPU cases,
differential checks and block-transfer reference results apply to the
same source rewrite; no new CPU algorithm was introduced for this test.
Both builds were freshly booted as A440 with the same game setup, followed
by a separate 30-second pacing check and five 250-frame capture windows.

| Build | Mean cycles/frame | Guest instructions/frame | Frame-call cycles/guest instruction |
| --- | ---: | ---: | ---: |
| Instrumented baseline | 4,814,738.574 | 61,399.626 | 78.4164 |
| Single-register paths | 4,854,941.346 | 61,376.343 | 79.1012 |

Candidate windows were 4,858,666.340; 4,851,172.848; 4,855,327.400;
4,865,027.088; and 4,844,513.052 cycles/frame. All exceed the largest
baseline window. Total guest instructions differ by -0.038%, while
cycles/frame increase 0.835% and the frame-call cost/instruction ratio
increases 0.873%. Both builds maintain approximately 50 FPS.

The direct measurement agrees with the earlier sampled comparison's
rejection, without equating their percentage changes or attributing the
cost specifically to ARM2, branch prediction or layout. The fast paths
were reverted. The opt-in frame-cycle facility is retained for subsequent
comparisons, with the restored ARM2 source.

## Restored A3020 check

The restored ARM250 12 MHz model measured 50.0092 FPS. Three further
250-frame captures completed with no invalid frames at 360 MHz:
6,903,009.120, 6,908,201.844, 6,904,819.624 cycles/frame.
The CPU rewrite is absent; the opt-in measurement hook remains available.
The desktop screenshot was verified and the serial port closed; no capture
is running.
