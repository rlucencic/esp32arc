#!/usr/bin/env python3
"""Check the actual private ARM2 count16 helper against Python's bit_count.

Run from any directory with Python 3 and a C compiler. The sibling esp32emu
checkout supplies the core. Temporary build files are removed afterward.
"""
import ctypes
from pathlib import Path
import subprocess
import tempfile

shared = Path(__file__).resolve().parents[4] / 'esp32emu'
core = shared / 'components' / 'arm2'
with tempfile.TemporaryDirectory(prefix='arm2-count16-') as directory:
    directory = Path(directory)
    wrapper = directory / 'count16.c'
    wrapper.write_text(
        f'#include "{core / "arm2.c"}"\n'
        'int checked_count16(uint32_t value) { return count16(value); }\n'
    )
    library = directory / 'count16.so'
    subprocess.run([
        'cc', '-O2', '-shared', '-fPIC', '-DEMUDBG_OFF',
        '-I' + str(core / 'include'),
        '-I' + str(shared / 'components' / 'emudbg' / 'include'),
        str(wrapper), str(core / 'arm2_dis.c'), '-o', str(library),
    ], check=True)
    function = ctypes.CDLL(str(library)).checked_count16
    function.argtypes = [ctypes.c_uint32]
    function.restype = ctypes.c_int
    checks = 0
    for high in (0, 0xFFFF0000, 0xA5A50000, 0x80000000):
        for low in range(65536):
            value = high | low
            actual = function(value)
            expected = low.bit_count()
            if actual != expected:
                raise SystemExit(f'{value:08x}: got {actual}, expected {expected}')
            checks += 1
    print(f'{checks} counts agree: all 65536 low-word patterns under four high-word patterns')
