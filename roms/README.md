# Acorn Archimedes ROM images

Put images of the OS ROMs here (they are copyrighted and not part of this
repository). The emulator recognises each by the version string in it and
its size:

| File | OS | Size |
|---|---|---|
| `arthur120.rom` | Arthur 1.20 (25 Sep 1987) | 512 KB |
| `riscos200.rom` | RISC OS 2.00 (05 Oct 1988) | 512 KB |
| `riscos311.rom` | RISC OS 3.11 (29 Sep 1992); 3.10 works too | 2 MB |

`idf.py flash` writes them to the `rom_arthur120`, `rom_riscos200` and
`rom_riscos311` flash partitions.

Without flashing, the emulator also finds them in a `roms` folder on the SD
card, under any file name (an Arthur image with one byte too many, as some
are, is fine).
