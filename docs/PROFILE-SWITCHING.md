# Profile switching · 0.1.2-dev

[Home](../README.md) · [Rey Effects integration](https://github.com/danrey-bilo/Rey-Effects-Linux-arm/tree/codex/fast-profile-switch) · [Русский](PROFILE-SWITCHING.ru.md)

The driver now records when DAC soft mute was asserted and waits only for its
remaining settling interval. Closing playback and then capture no longer waits
the full interval twice. Repeated mute requests do not restart the ramp or write
the same control register again. Suspend/removal use the same process-context guard.

The guard is `ceil(4112 × 1,000,000 / fs) + 1000` microseconds, including the
conservative full `4080/fs` ATS=00 transition, a filter-tail allowance and margin.
It accounts for time already spent muted. It does not select a faster, unqualified
attenuation ramp. See AKM's [AK4458 datasheet, Table 19 and Soft Mute Operation](https://www.mouser.com/datasheet/2/1431/ak4458vn_en_datasheet-3010057.pdf).

Clock/reset sequencing, the ADC's `578/fs` startup guard, the 50 ms playback-start
guard and real register readback are retained. Idle still powers down both
converters and oscillators. PCM constraints and shared-rate rejection are unchanged.

## Measured on the connected Pi 5

Kernel `6.18.50+rpt-rpi-v8-rt`, 8×8 `S32_LE`, all seven rates, three rounds per
rate. Playback transmitted zeros; each direction transferred 0.2 seconds.
Close order alternated by round. These times cover **ALSA duplex teardown**, not
the analog silence interval or ADC-to-DAC latency. Nearest-rank percentiles:

| Teardown | Before · 0.1.1-dev | After · 0.1.2-dev |
|---|---:|---:|
| p50 | 207.93 ms | 38.46 ms |
| p95 | 210.12 ms | 89.59 ms |
| p99 / max | 210.14 ms | 89.61 ms |
| Trials | 21 | 21 |

All transfers completed without ALSA errors or XRUN recovery. All-rate duplex,
shared-clock lifecycle/controls and five DAC filters at 192 kHz were retested.
I²C error count remained zero. Analog click amplitude, power-cycle transients and
suspend/resume still require separate physical qualification.

Raw reports: [before](../validation/2026-10-15/profile-driver-before.json),
[after](../validation/2026-10-15/profile-driver-after.json),
[all-rate DMA](../validation/2026-10-15/profile-driver-smoke.json),
[lifecycle](../validation/2026-10-15/profile-driver-lifecycle.json),
[DAC filter registers](../validation/2026-10-15/dac-filter-controls.json).

## Reproduce

Stop every PCM owner first; the timing test briefly enables the DAC with zero TX
and restores its original playback-switch preference:

```sh
make test
sudo python3 tests/switch_timing.py --rounds 3 --output /tmp/akm-switch.json
```

The matching Rey Effects change reduces service/readiness and SD-card handoff
overhead. A sample-rate change still reconstructs the JACK/MOD graph and adapts
the active graph's IR assets. It is not a seamless clock change.
