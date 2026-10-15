# ALSA interface, controls and application integration

[Home](../README.md) · [Hardware](HARDWARE.md) · [Troubleshooting](TROUBLESHOOTING.md)

## PCM contract

| Property | Value |
|---|---|
| Stable card ID | `PiAoIPAKM` |
| Hardware PCM | `hw:PiAoIPAKM,0` |
| Directions | Capture and playback, independently or simultaneously |
| Channels | Exactly 8 in each direction |
| Format | `S32_LE`, signed little-endian 32-bit words |
| Access used by supplied tests | `SND_PCM_ACCESS_RW_INTERLEAVED` |
| Frame size | 8 × 4 = **32 bytes** per direction |
| Sample rates | 44100 / 48000 / 88200 / 96000 / 176400 / 192000 / 384000 Hz |
| Duplex constraints | Same rate, channel count and sample width in both directions |

A frame contains one sample from each of the eight channels. ALSA frame counts refer to frames **per channel**, not the sum of samples across channels. At 48 kHz the data rate is 1,536,000 bytes/s per direction; at 384 kHz it is 12,288,000 bytes/s per direction.

Use the card ID instead of a numerical card index: HDMI and USB devices can change index ordering. The hardware device does not resample, convert a stereo application to eight channels, or change word format. ALSA plugins or the application may do that explicitly; plugin buffering is additional to the raw PCM path.

`S32_LE` describes the digital container and serial word format. It does not establish 32-bit analog resolution, ADC noise floor or DAC performance.

## Application lifecycle

1. Open capture and/or playback at `hw:PiAoIPAKM,0`.
2. Set eight channels, `S32_LE`, the desired exact rate and suitable access/buffer/period parameters. Disable implicit resampling for hardware qualification.
3. If opening the second direction, request the **same rate** as the first. Mismatched duplex parameters are rejected.
4. Prepare the PCM streams, fill playback buffers before starting, and service both directions continuously. A blocking application needs separate scheduling for capture and playback; the supplied test uses two workers.
5. Handle partial transfers and `EAGAIN` when using nonblocking I/O. Treat `EPIPE`/XRUN as a discontinuity, record it, and explicitly recover or restart the application session.
6. For a shared-rate change, stop and release **both** directions, then reconfigure them. Do not change GPIOs/clock registers from the application.
7. Closing one direction leaves the other running. Releasing the final direction mutes and powers down the converters.

`hw_params` holds the common clock even before START. Consequently ADC filter changes can be busy while a stream is prepared but not transferring. Closing or calling the ALSA hardware-free operation releases its parameter claim.

The two PCM directions share a physical clock. The Python smoke test's thread barrier does not prove sample-exact simultaneous hardware start. Applications requiring phase-aligned capture/playback starts must qualify their own ALSA start procedure and physical loopback.

The module contains no AoIP/network queue, resampler or DSP graph. An AoIP service can read/write this PCM as its hardware backend, while independently managing network timestamps, channel routing and error counters. ALSA's exclusive raw-device access still applies.

## `akmctl`

The installed utility is a small Python 3 wrapper over ALSA controls plus the driver's read-only sysfs status. It never accesses I²C or GPIO directly.

| Command | Behavior |
|---|---|
| `akmctl status` | Print `key=value` driver status |
| `akmctl mute` | Disable the common playback output switch |
| `akmctl unmute` | Permit playback output when a playback stream is running |
| `akmctl volume -12` | Set all eight DAC channels to −12 dB |
| `akmctl hpf on` / `off` | Set ADC HPF preference while both PCMs are released |
| `akmctl adc-filter normal` / `short` | Normal-sharp / short-delay-sharp ADC filter, while both PCMs are released |
| `akmctl dac-filter sharp` / `slow` / `short-sharp` / `short-slow` / `super-slow` | DAC reconstruction filter, while both PCMs are released (development driver 0.1.1-dev) |

Volume range is −127.5 through 0 dB, quantized to 0.5 dB. Raw value 0 is attenuation mute, so the CLI's −127.5 dB endpoint means mute. The separate playback switch is the recommended common mute control.

`unmute` sets a permission, not a request to generate clocks or sound. The output remains muted without running playback. START requests unmute after a 50 ms startup hold; subsequent changes while running are scheduled through workqueue context. The DAC also has its own soft-mute ramp.

ADC HPF/filter selection is ineffective in 384 kHz OCT mode. The preference is retained for rates where the chip supports it. This restriction describes the ADC controls, not the separate DAC filter control.

The default DAC filter remains `short-sharp`, matching version 0.1.0. The five modes
map to ALSA enum values 0–4 in the order shown by `akmctl` above. All eight DAC
channels share this choice. A change is rejected while either PCM holds parameters;
the three filter registers are applied together during the next muted configuration.
Mute/unmute preserves the selected filter. A slower roll-off changes the response
near the top of the passband and can reduce square-wave ringing; it is not a
guarantee of a perfectly rectangular analog output. At 192 kHz, the short-delay
slow filter's specified digital passband is 48.4 kHz (±0.05 dB).

## Complete ALSA control list

| Control name | Type / values | Effect |
|---|---|---|
| `DAC Playback Volume` | 8 integers, 0–255 | Per-channel digital attenuation |
| `DAC Playback Switch` | One boolean | Common user output permission |
| `ADC High Pass Filter Switch` | One boolean | ADC HPF preference; changes require released PCMs |
| `ADC Short Delay Filter Switch` | One boolean | ADC normal/short filter preference; changes require released PCMs |
| `DAC Digital Filter` | Enum: Sharp / Slow / Short Sharp / Short Slow / Super Slow | Shared DAC reconstruction filter; changes require released PCMs |
| `Active Sample Rate` | Read-only integer | Configured rate, 0 when powered down |
| `I2C Error Count` | Read-only integer | Lifetime count of failed register transfers/readback checks |

List values:

```sh
amixer -c PiAoIPAKM contents
```

Set an eight-channel level vector:

```sh
amixer -c PiAoIPAKM cset name='DAC Playback Volume' 231,231,231,231,231,231,231,231
amixer -c PiAoIPAKM cset name='DAC Playback Switch' 0
```

For a raw value `v` from 1 through 255, attenuation is `(v - 255) × 0.5 dB`. Value 0 is mute; 255 is 0 dB; 231 is −12 dB. Hardware volume writes are read back while the board is powered. Settings made at idle are cached and applied on the next configuration.

For a native application, use ALSA control APIs with the exact names above; do not shell out for each sample block. Control changes belong on a management thread. Python/CLI processes are convenience tools, not the real-time PCM callback.

## Status and register diagnostics

```sh
cat /sys/bus/platform/devices/piaoip-akm/status
cat /sys/bus/platform/devices/piaoip-akm/registers
```

Both attributes are read-only. Register reads perform real I²C transfers and should be used for diagnostics rather than tight polling in an audio thread.

| Status field | Meaning |
|---|---|
| `version` | Driver version |
| `channels`, `format`, `bclk_fs` | Fixed hardware contract: `8x8`, `S32_LE`, `64` |
| `powered` | 1 when reset released and a converter configuration exists |
| `rate` | Configured shared sample rate; 0 at idle |
| `configured_mask` | Bit 0 playback, bit 1 capture; 3 means both hold parameters |
| `running_mask` | Bit 0 playback, bit 1 capture; set/cleared by PCM trigger |
| `output_enabled` | Saved user playback switch |
| `hpf` | Saved ADC HPF preference |
| `adc_short_delay` | Saved short-delay filter preference |
| `dac_filter` | Saved DAC filter enum 0–4; default 2 (`short-sharp`), development driver 0.1.1-dev |
| `hpf_effective` | 1 only when powered, at a supported non-OCT rate, and HPF requested |
| `adc_filter_selectable` | 1 when powered at ≤192 kHz; does not waive the busy check during streams |
| `i2c_errors` | Failed transfers/readback mismatches since module load |
| `last_error` | Last negative codec error; cleared by a successful complete configuration |

Idle `powered=0`, `rate=0`, zero masks, `hpf_effective=0` and `adc_filter_selectable=0` are normal. The latter fields reflect an active hardware mode, so they are zero at idle even when an idle preference may be changed.

`registers` returns `EHOSTDOWN` while idle. Open/configure a PCM stream before reading it. When powered, it dumps ADC `0x00`–`0x05` and DAC `0x00`–`0x14`. This is a snapshot; some DAC fields are read-only or reserved.

The driver reserves the converters even when reset is asserted. A userspace I²C scanner may show reserved addresses or no response; that is not a substitute for this driver's probe/readback result. Do not use `i2cset -f` or force another client onto these addresses.

## Persistence

CLI/control changes do not write to microSD on every operation. To persist an intentional setup through the system's ALSA state mechanism:

```sh
akmctl mute
sudo alsactl store PiAoIPAKM
```

How and when saved state is restored depends on the OS ALSA service. Module defaults are output switch off, attenuation 255/0 dB, HPF on and short-delay filter off. An existing system state can override these defaults after probe.

For the DAC-filter development build, validate the real register encodings and
mute preservation while other audio applications are stopped:

```sh
python3 tests/dac_filters.py --rate 192000 --json-out /tmp/akm-dac-filters.json
```

This test transmits digital zeros, briefly exercises the output switch and leaves
the common output muted. It restores the original DAC filter preference and makes
no claim about analog ringing or analog audio quality.

## Runnable I/O examples

Capture five seconds of all eight inputs into RAM:

```sh
arecord -D hw:PiAoIPAKM,0 -c 8 -r 48000 -f S32_LE -d 5 /tmp/input.wav
```

For simultaneous receive/transmit without an analog signal, the supplied [PCM smoke test](../tests/pcm_smoke.py) is a runnable `libasound.so.2` example: native ALSA calls through Python `ctypes`, interleaved nonblocking transfers, a separate worker per direction, zero TX and bounded capture statistics. It uses a conservative **40 ms buffer / approximately 10 ms period** and is a functional test, not a production real-time engine.

```sh
akmctl mute
akmctl hpf on
python3 tests/pcm_smoke.py --rates 48000 --seconds 5 --json-out /tmp/akm-48k.json
```

For low latency, choose and measure period/buffer settings in the actual native audio application. This release does not claim a qualified minimum buffer size. See [remaining qualification](VALIDATION.md#remaining-qualification).
