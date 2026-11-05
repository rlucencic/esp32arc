#!/usr/bin/env python3
"""Time Lander from guest frame counters without HTTP polling or a profiler.

Run with the ESP-IDF Python environment (pyserial is required). Mount
RO2Apps2.adf in :0 first. --boot assumes freshly flashed firmware at setup
and the standard Tab5 Start button position. Without --boot, start Lander
manually before measuring. The result and a sibling .log file containing
runtime messages are written; the port is released on completion or failure.
"""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import time

parser = argparse.ArgumentParser(description='Measure Lander using guest frame counters on a Tab5 running esp32arc.')
parser.add_argument('port', help='Native USB console, for example /dev/ttyACM1')
parser.add_argument('output')
parser.add_argument('--seconds', type=int, default=60)
parser.add_argument('--boot', action='store_true', help='After flashing: start the setup screen, then launch Lander from drive :0')
args = parser.parse_args()
if args.seconds <= 0:
    parser.error('--seconds must be positive')
console_path = Path(__file__).resolve().parents[2] / 'esp32emu' / 'tools' / 'emu_console.py'
spec = importlib.util.spec_from_file_location('console', console_path)
console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console)
port = console.open_port(args.port)
logs = io.StringIO()

def drain(seconds):
    with contextlib.redirect_stdout(logs):
        console.log(port, seconds)

def send(command):
    port.write(command.encode() + b'\n')

def info():
    port.reset_input_buffer()
    sent = time.monotonic()
    send('i')
    deadline = sent + 5
    while time.monotonic() < deadline:
        line = port.readline().decode(errors='replace').strip()
        if line.startswith('EMUINFO'):
            received = time.monotonic()
            match = re.search(r'frames (\d+)', line)
            if not match:
                raise RuntimeError(line)
            return {'time': (sent + received) / 2, 'latency': received-sent,
                    'frames': int(match[1]), 'info': line}
    raise RuntimeError('No console state response')

try:
    if args.boot:
        print('Starting Archimedes and Lander', flush=True)
        drain(8)
        send('t 1065 637')
        drain(25)
        send('k 0c')
        drain(0.3)
        send('T run adfs::0.$.!Lander\\n')
        drain(12)
    state = info()
    if 'screen 320x256 8 bpp' not in state['info']:
        raise RuntimeError('Lander video mode missing: ' + state['info'])
    drain(5)
    start = info()
    print('Measuring guest frame throughput', flush=True)
    drain(args.seconds)
    end = info()
    if any('screen 320x256 8 bpp' not in sample['info'] for sample in (start, end)):
        raise RuntimeError('Lander video mode changed during measurement')
    elapsed = end['time'] - start['time']
    frames = end['frames'] - start['frames']
    result = {'start': start, 'end': end, 'elapsed': elapsed, 'frames': frames,
              'fps': frames/elapsed, 'speed_percent': frames/elapsed*2}
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    runtime_log = '\n'.join(line for line in logs.getvalue().splitlines() if ') emu:' in line)
    Path(args.output).with_suffix('.log').write_text(runtime_log + '\n')
    print(json.dumps(result), flush=True)
finally:
    port.close()
