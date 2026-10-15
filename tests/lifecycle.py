#!/usr/bin/env python3
"""Check shared clocks and hardware readback; playback contains only zeros."""
import json
from pathlib import Path
import subprocess
import threading
import time

from pcm_smoke import PCM, REGISTERS, state

DEVICE = 'hw:PiAoIPAKM,0'

def control(name, value, fail=False):
    result = subprocess.run(['amixer', '-q', '-c', 'PiAoIPAKM', 'cset',
                             'name=' + name, value], text=True, capture_output=True)
    assert (result.returncode != 0) == fail, result.stderr
    return result.stderr.strip()

def registers():
    return {f'{chip}:{address}': int(value, 16)
            for chip, address, value in
            (line.split() for line in REGISTERS.read_text().splitlines())}

def wait_mute(value):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if registers()['DAC:01'] == value:
            return
        time.sleep(0.01)
    raise AssertionError('DAC SMUTE register did not change')

def shared_clock_case(first_direction):
    main = PCM(DEVICE, first_direction, 48000)
    results, worker = {}, None
    barrier = threading.Barrier(2)
    def run_main():
        try:
            results['stream'] = main.transfer(48000 * 4, barrier)
        except BaseException as error:
            results['error'] = str(error)
    try:
        worker = threading.Thread(target=run_main)
        worker.start()
        barrier.wait()
        time.sleep(0.3)
        second = PCM(DEVICE, 1 - first_direction, 48000)
        try:
            second.transfer(48000 // 2, threading.Barrier(1))
        finally:
            second.close()
        after = state()
        assert after['powered'] == '1' and after['rate'] == '48000', after
        assert int(after['configured_mask']) == (1 << first_direction), after
        results['state_after_other_direction_closed'] = after
        if not first_direction:
            volumes = list(range(120, 128))
            control('DAC Playback Volume', ','.join(map(str, volumes)))
            actual = registers()
            for reg, value in zip([3, 4, 15, 16, 17, 18, 19, 20], volumes):
                assert actual[f'DAC:{reg:02x}'] == value, actual
            results['eight_volume_registers_verified'] = True
            control('DAC Playback Switch', '1')
            wait_mute(0x22)
            control('DAC Playback Switch', '0')
            wait_mute(0x23)
            results['mute_unmute_registers_verified'] = True
            results['filter_change_busy'] = control('ADC High Pass Filter Switch', '0', fail=True)
            control('DAC Playback Volume', ','.join(['231'] * 8))
        worker.join()
        assert 'error' not in results, results
        assert results['stream']['frames'] == 48000 * 4
    finally:
        if worker and worker.is_alive():
            worker.join()
        main.close()
    results['idle_state'] = state()
    assert results['idle_state']['powered'] == '0'
    return results

def main():
    control('DAC Playback Switch', '0')
    control('ADC High Pass Filter Switch', '1')
    report = {'playback_survives_capture_close': shared_clock_case(0),
              'capture_survives_playback_close': shared_clock_case(1)}
    control('ADC Short Delay Filter Switch', '1')
    capture = PCM(DEVICE, 1, 48000)
    try:
        assert registers()['ADC:04'] == 2
        report['short_delay_filter_register_verified'] = True
    finally:
        capture.close()
    control('ADC Short Delay Filter Switch', '0')
    report['pass'] = state()['i2c_errors'] == '0'
    Path('/tmp/piaoip-lifecycle.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'pass': report['pass'], 'shared_clocks': 'both directions',
                      'volume': '8 hardware registers', 'mute': 'hardware readback',
                      'adc_filter': 'hardware readback'}), flush=True)

if __name__ == '__main__':
    main()
