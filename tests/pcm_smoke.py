#!/usr/bin/env python3
"""Real ALSA DMA test. TX is zero-filled; no analog loopback claim is made."""
import argparse
import array
import ctypes as C
import json
import math
from pathlib import Path
import threading
import time

lib = C.CDLL('libasound.so.2')
P = C.c_void_p
lib.snd_pcm_open.argtypes = [C.POINTER(P), C.c_char_p, C.c_int, C.c_int]
lib.snd_pcm_set_params.argtypes = [P, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_int, C.c_uint]
lib.snd_pcm_get_params.argtypes = [P, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong)]
lib.snd_pcm_format_value.argtypes = [C.c_char_p]
lib.snd_strerror.argtypes = [C.c_int]
lib.snd_strerror.restype = C.c_char_p
for operation in ('readi', 'writei'):
    function = getattr(lib, 'snd_pcm_' + operation)
    function.argtypes = [P, P, C.c_ulong]
    function.restype = C.c_long
for operation in ('drop', 'close', 'drain', 'start'):
    getattr(lib, 'snd_pcm_' + operation).argtypes = [P]
lib.snd_pcm_wait.argtypes = [P, C.c_int]
lib.snd_pcm_nonblock.argtypes = [P, C.c_int]

STATUS = Path('/sys/bus/platform/devices/piaoip-akm/status')
REGISTERS = STATUS.with_name('registers')

def checked(result, operation):
    if result < 0:
        raise RuntimeError(f'{operation}: {result}: {lib.snd_strerror(result).decode()}')
    return result

def state():
    return dict(line.split('=', 1) for line in STATUS.read_text().splitlines())

class PCM:
    def __init__(self, device, direction, rate, channels=8, sample_format='S32_LE'):
        self.handle = P()
        self.direction = direction
        self.rate = rate
        checked(lib.snd_pcm_open(C.byref(self.handle), device.encode(), direction, 1), 'open')
        try:
            # RW_INTERLEAVED=3, resampling disabled, conservative 40 ms buffer.
            checked(lib.snd_pcm_set_params(self.handle,
                    lib.snd_pcm_format_value(sample_format.encode()), 3,
                    channels, rate, 0, 40000), 'set_params')
            buffer, period = C.c_ulong(), C.c_ulong()
            checked(lib.snd_pcm_get_params(self.handle, C.byref(buffer), C.byref(period)), 'get_params')
            self.buffer_frames, self.period_frames = buffer.value, period.value
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.handle:
            lib.snd_pcm_drop(self.handle)
            lib.snd_pcm_close(self.handle)
            self.handle = P()

    def transfer(self, total, barrier):
        buffer = C.create_string_buffer(256 * 8 * 4)
        operation = lib.snd_pcm_readi if self.direction else lib.snd_pcm_writei
        frames, waits, samples = 0, 0, bytearray()
        barrier.wait()
        started = time.monotonic()
        if self.direction:
            checked(lib.snd_pcm_start(self.handle), 'capture start')
        deadline = started + total / self.rate + 5
        while frames < total:
            if time.monotonic() > deadline:
                raise TimeoutError('PCM did not advance at the requested rate')
            count = operation(self.handle, buffer, min(256, total - frames))
            if count == -11:  # EAGAIN: nonblocking ALSA DMA.
                checked(lib.snd_pcm_wait(self.handle, 500), 'wait')
                waits += 1
                continue
            checked(count, 'readi' if self.direction else 'writei')
            if not count:
                raise RuntimeError('PCM made no progress')
            if self.direction and len(samples) < 4096 * 32:
                samples += buffer.raw[:min(count * 32, 4096 * 32 - len(samples))]
            frames += count
        if not self.direction:
            checked(lib.snd_pcm_nonblock(self.handle, 0), 'drain blocking mode')
            checked(lib.snd_pcm_drain(self.handle), 'drain')
        result = {'frames': frames, 'io_seconds': time.monotonic() - started,
                  'waits': waits, 'xruns': 0,
                  'buffer_frames': self.buffer_frames, 'period_frames': self.period_frames}
        if self.direction:
            words = array.array('i')
            words.frombytes(samples)
            result['sample_statistics'] = []
            for channel in range(8):
                values = words[channel::8]
                result['sample_statistics'].append({
                    'channel': channel + 1, 'samples': len(values),
                    'min': min(values), 'max': max(values),
                    'unique': len(set(values)),
                    'rms_dbfs': round(20 * math.log10(max(1e-20,
                        math.sqrt(sum(v * v for v in values) / len(values)) / 2147483648)), 2),
                })
        return result

def duplex(device, rate, seconds):
    streams, threads, results = [], [], {}
    try:
        streams.append(PCM(device, 0, rate))
        streams.append(PCM(device, 1, rate))
        before = state()
        assert before['configured_mask'] == '3' and int(before['rate']) == rate, before
        registers = REGISTERS.read_text()
        expected = (0x47 if rate == 384000 else
                    (0x37 if rate <= 48000 else (0x1f if rate <= 96000 else 7)))
        assert f'ADC 02 {expected:02x}' in registers, registers
        assert 'DAC 00 8f' in registers and 'DAC 01 23' in registers, registers
        barrier = threading.Barrier(3)
        def run(stream):
            label = 'capture' if stream.direction else 'playback'
            try:
                results[label] = stream.transfer(round(rate * seconds), barrier)
            except BaseException as error:
                results[label] = {'error': str(error)}
        for stream in streams:
            thread = threading.Thread(target=run, args=(stream,))
            threads.append(thread)
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join()
        results.update(rate=rate, seconds=seconds, state=before, registers=registers)
    finally:
        for stream in reversed(streams):
            stream.close()
    results['idle_state'] = state()
    results['pass'] = (all(results.get(name, {}).get('frames') == round(rate * seconds)
                          for name in ('capture', 'playback'))
                       and all(abs(results.get(name, {}).get('io_seconds', -100) - seconds)
                               <= max(0.1, seconds * 0.05) for name in ('capture', 'playback'))
                       and results['idle_state']['powered'] == '0'
                       and results['idle_state']['i2c_errors'] == '0')
    return results

def rejection_tests(device):
    results = {}
    capture = PCM(device, 1, 48000)
    try:
        try:
            other = PCM(device, 0, 96000)
        except RuntimeError as error:
            results['duplex_different_rate_rejected'] = str(error)
        else:
            other.close()
            raise AssertionError('different playback/capture rates were accepted')
        assert int(state()['rate']) == 48000
    finally:
        capture.close()
    for label, args in [('unsupported_32k', (32000, 8, 'S32_LE')),
                        ('stereo_hardware', (48000, 2, 'S32_LE')),
                        ('s16_hardware', (48000, 8, 'S16_LE'))]:
        try:
            other = PCM(device, 0, *args)
        except RuntimeError as error:
            results[label] = str(error)
        else:
            other.close()
            raise AssertionError(f'invalid configuration accepted: {label}')
    return results

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--device', default='hw:PiAoIPAKM,0')
    p.add_argument('--rates', default='44100,48000,88200,96000,176400,192000,384000')
    p.add_argument('--seconds', type=float, default=2)
    p.add_argument('--json-out', type=Path, default=Path('/tmp/piaoip-pcm-smoke.json'))
    p.add_argument('--skip-rejections', action='store_true')
    a = p.parse_args()
    report = {'hardware': 'AK5538/AK4458 v5',
              'scope': 'physical I2S/DMA; zero TX; no known analog input or analog loopback',
              'cases': []}
    for rate in map(int, a.rates.split(',')):
        result = duplex(a.device, rate, a.seconds)
        report['cases'].append(result)
        print(json.dumps({'rate': rate, 'pass': result['pass'],
                          'capture_frames': result.get('capture', {}).get('frames'),
                          'playback_frames': result.get('playback', {}).get('frames'),
                          'errors': [result.get(name, {}).get('error') for name in ('capture', 'playback')]}), flush=True)
    if not a.skip_rejections:
        report['rejections'] = rejection_tests(a.device)
    report['pass'] = all(case['pass'] for case in report['cases'])
    report['final_state'] = state()
    a.json_out.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS' if report['pass'] else 'FAIL', flush=True)
    raise SystemExit(0 if report['pass'] else 1)

if __name__ == '__main__':
    main()
