#!/usr/bin/env python3
"""Time real ALSA configuration/duplex teardown. TX contains zeros only.

Close JACK and other PCM owners first. This measures software calls, not the
analog output gap, click amplitude or ADC-to-DAC latency.
"""
import argparse
import json
from pathlib import Path
import subprocess
import threading
import time

from pcm_smoke import PCM, state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    initial = state()
    assert initial['configured_mask'] == '0', 'Close all PCM owners before timing.'
    report = {'scope': __doc__, 'initial': initial, 'trials': []}
    def enabled(value):
        subprocess.run(['amixer', '-q', '-c', 'PiAoIPAKM', 'cset',
                        'name=DAC Playback Switch', str(value)], check=True)
    enabled(1)
    try:
        for iteration in range(args.rounds):
            for rate in (44100, 48000, 88200, 96000, 176400, 192000, 384000):
                streams = []
                start = time.monotonic_ns()
                try:
                    for direction in (0, 1):
                        streams.append(PCM('hw:PiAoIPAKM,0', direction, rate))
                    opened = (time.monotonic_ns() - start) / 1e6
                    errors = []
                    results = []
                    barrier = threading.Barrier(3)
                    def transfer(stream):
                        try:
                            results.append(stream.transfer(rate // 5, barrier))
                        except BaseException as error:
                            errors.append(str(error))
                    workers = [threading.Thread(target=transfer, args=(stream,)) for stream in streams]
                    for worker in workers: worker.start()
                    barrier.wait()
                    for worker in workers: worker.join()
                    assert not errors and len(results) == 2, errors
                    # Alternate close order: a playback close followed by a
                    # capture close exposed the former duplicate 100 ms wait.
                    order = (0, 1) if iteration % 2 == 0 else (1, 0)
                    closing = time.monotonic_ns()
                    first = closing
                    parts = []
                    for direction in order:
                        streams[direction].close()
                        now = time.monotonic_ns()
                        parts.append((now - first) / 1e6); first = now
                    idle = state()
                    assert idle['powered'] == '0' and idle['configured_mask'] == '0' and idle['i2c_errors'] == '0', idle
                    row = {'rate': rate, 'round': iteration, 'open_ms': opened,
                           'close_order': list(order), 'close_parts_ms': parts,
                           'close_ms': (time.monotonic_ns() - closing) / 1e6,
                           'frames_each': rate // 5, 'xruns': sum(v['xruns'] for v in results)}
                    report['trials'].append(row)
                    print(json.dumps(row), flush=True)
                finally:
                    for stream in streams: stream.close()
        report['pass'] = True
    finally:
        enabled(initial['output_enabled'])
        report['final'] = state()
        args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
