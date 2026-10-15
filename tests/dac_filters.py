#!/usr/bin/env python3
"""Real AK4458 register/mute checks with zero-only playback; close other PCMs first."""
import argparse
import json
from pathlib import Path
import subprocess
import threading
import time

from pcm_smoke import PCM, REGISTERS, state

CONTROL = 'DAC Digital Filter'
NAMES = ('sharp', 'slow', 'short-sharp', 'short-slow', 'super-slow')
EXPECTED = ((0x02, 0, 0), (0x02, 1, 0), (0x22, 0, 0), (0x22, 1, 0), (0x22, 0, 1))


def set_control(name, value, fail=False):
    p = subprocess.run(['amixer', '-q', '-c', 'PiAoIPAKM', 'cset', 'name='+name, str(value)],
                       capture_output=True, text=True)
    if fail:
        assert p.returncode and 'busy' in p.stderr.lower(), p.stderr
        return p.stderr.strip()
    p.check_returncode()


def registers():
    return {int(reg, 16): int(value, 16) for chip, reg, value in
            (line.split() for line in REGISTERS.read_text().splitlines()) if chip == 'DAC'}


def wait_mute(value, sd):
    deadline = time.monotonic()+.5
    while time.monotonic() < deadline:
        r = registers()
        assert r[1] & 0x20 == sd, r
        if r[1] & 1 == value:
            return r
        time.sleep(.01)
    raise AssertionError('Mute state did not converge')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rate', type=int, default=192000)
    parser.add_argument('--json-out', type=Path, default=Path('/tmp/akm-dac-filters.json'))
    args = parser.parse_args()
    original = state()
    assert original['configured_mask'] == '0', original
    report = {'rate_hz': args.rate, 'tx': 'digital zeros only',
              'scope': 'registers, busy guard, mute preservation and PCM DMA; not analog ringing',
              'driver_version': original['version'], 'cases': []}
    try:
        for i, name in enumerate(NAMES):
            set_control('DAC Playback Switch', 0)
            set_control(CONTROL, i)
            stream = PCM('hw:PiAoIPAKM,0', 0, args.rate)
            thread = None
            result = {}
            try:
                before = registers()
                ctrl2, slow, superslow = EXPECTED[i]
                assert before[1] == ctrl2 | 1, before
                assert before[2] == slow and before[5] == superslow, before
                busy = set_control(CONTROL, (i+1) % len(NAMES), fail=True)
                assert int(state()['dac_filter']) == i
                barrier = threading.Barrier(2)

                def playback():
                    try:
                        result.update(stream.transfer(args.rate, barrier))
                    except BaseException as error:
                        result['error'] = str(error)

                thread = threading.Thread(target=playback)
                thread.start()
                barrier.wait()
                time.sleep(.12)
                set_control('DAC Playback Switch', 1)
                unmuted = wait_mute(0, ctrl2 & 0x20)
                set_control('DAC Playback Switch', 0)
                muted = wait_mute(1, ctrl2 & 0x20)
                set_control('DAC Playback Switch', 1)
                resumed = wait_mute(0, ctrl2 & 0x20)
                assert all(r[2] == slow and r[5] == superslow for r in (unmuted, muted, resumed))
                thread.join()
                assert result.get('frames') == args.rate and result.get('xruns') == 0, result
                assert int(state()['i2c_errors']) == 0
                report['cases'].append({'filter': name, 'enum': i,
                    'configured_registers': {f'{k:02x}': before[k] for k in (1,2,5)},
                    'busy_guard': busy, 'mute_preserves_filter': True,
                    'playback': result, 'pass': True})
            finally:
                if thread is not None:
                    thread.join()
                stream.close()
    finally:
        set_control('DAC Playback Switch', 0)
        set_control(CONTROL, original['dac_filter'])
        report['final_state'] = state()
        report['pass'] = len(report['cases']) == len(NAMES) and int(report['final_state']['i2c_errors']) == 0
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2)+'\n')
    assert report['pass'], report
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
