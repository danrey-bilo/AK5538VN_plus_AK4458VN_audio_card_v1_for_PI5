# Validation and qualification status

[Home](../README.md) · [Test commands](DEVELOPMENT.md#hardware-tests) · [Raw evidence](../validation/2026-10-15)

**Version 0.1.0 is a prototype / prerelease.** Tests below used a physical audio card and Pi 5. DAC playback data was zero-filled; capture had no calibrated input or analog loopback. Functional I²S/DMA operation and codec register readback are verified. Analog transfer, quality and end-to-end latency are not yet qualified.

Current **0.1.2-dev** qualification and the separate before/after teardown
measurements are documented in [Profile switching](PROFILE-SWITCHING.md).
The sections below retain the original 0.1.0 evidence and its test durations.

## Test platform

| Item | Recorded configuration |
|---|---|
| Evidence date | 2026-10-15 |
| Hardware | Raspberry Pi 5 Model B, 4 GB RAM; audio card v1 / schematic v5 |
| OS | Debian 13 / Raspberry Pi OS Lite ARM64 |
| Kernel | `6.18.50+rpt-rpi-v8-rt` |
| Kernel configuration | `CONFIG_PREEMPT_RT=y`, 4 KB pages, RP1 pinctrl and DWC I²S |
| PCM | 8 capture + 8 playback channels, interleaved `S32_LE` |
| Clocking | ADC provider; four stereo lanes each way; 64fs BCLK |
| Smoke-test buffering | 40 ms buffer, approximately 10 ms period |
| Initial output setting | Playback switch disabled |
| CPU governor for scheduler test | `ondemand` |

## Codec communication and controls

- Both devices responded at their expected addresses: ADC `0x10`, DAC `0x11`.
- Every driver configuration register write is followed by hardware readback.
- Lifecycle tests read back all eight distinct DAC attenuation registers.
- Muted/unmuted DAC register values `0x23`/`0x22` were observed during zero playback.
- ADC short-delay selection was read back from `0x04`.
- Filter/HPF changes while a PCM configuration existed were rejected as busy.
- Closing capture while playback continued preserved clocks and playback progress; the reverse direction also passed.
- Closing the final stream returned both converters and oscillators to idle, with output disabled.
- Boot autoload, idle unload/reload and a subsequent boot/48 kHz check passed on the prototype.

Evidence: [initial I²C](../validation/2026-10-15/initial-i2c.json), [lifecycle/controls](../validation/2026-10-15/lifecycle-final.json). The initial I²C snapshot was taken on the stock kernel before RT installation; it is separate from the final RT PCM tests.

## All-rate simultaneous capture and playback

Each case requested two seconds in each direction. The test checked transfer progress/timing, register state, exact requested frame counts, zero recorded I²C errors and idle shutdown. ALSA transfer errors are fatal; XRUN recovery is not silently performed.

| Rate, Hz | Duration | Frames per channel, each direction | Result |
|---:|---:|---:|---|
| 44100 | 2 s | 88200 | PASS; XRUN 0 |
| 48000 | 2 s | 96000 | PASS; XRUN 0 |
| 88200 | 2 s | 176400 | PASS; XRUN 0 |
| 96000 | 2 s | 192000 | PASS; XRUN 0 |
| 176400 | 2 s | 352800 | PASS; XRUN 0 |
| 192000 | 2 s | 384000 | PASS; XRUN 0 |
| 384000 | 2 s | 768000 | PASS; XRUN 0 |

Invalid-rate, stereo hardware, 16-bit hardware and mismatched duplex-rate requests were rejected. [Raw all-rate report](../validation/2026-10-15/all-rates-final.json).

Frame counts establish progress through ALSA/DMA for the requested number of frames. They do not by themselves prove bit-perfect input/output, calibrated converter clock accuracy or absence of every possible sample slip.

## 384 kHz, 60-second duplex run

| Measurement | Capture | Playback |
|---|---:|---:|
| Rate | 384000 Hz | 384000 Hz |
| Channels | 8 | 8 |
| Frames per channel | 23040000 | 23040000 |
| Total PCM bytes | 737280000 | 737280000 |
| Transfer time reported | 60.0017 s | 60.0080 s |
| Buffer / period frames | 15360 / 3840 | 15360 / 3840 |
| XRUNs observed | 0 | 0 |

The final driver's I²C error count and last error were both zero. The ADC produced changing data on all eight channels; the saved first-4096-sample statistics are diagnostic observations, **not** noise-floor, SNR or calibrated signal measurements. At 384 kHz, OCT mode disables effective ADC HPF/filter selection.

Average whole-system CPU use during this run was **4.69%**. The application did not save the full capture to SD. [Raw 60-second report](../validation/2026-10-15/384k-60s.json), [RT/CPU summary](../validation/2026-10-15/rt-summary.json).

## Scheduler wakeup measurements under audio load

`cyclictest` ran for 30 seconds with two threads on CPUs 0/1, FIFO priority 80 and a 1000 µs interval, concurrently with the 384 kHz duplex workload. There were 30,000 samples per thread and no histogram overflows.

| CPU | Samples | p50, µs | p95, µs | p99, µs | Maximum, µs |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 30000 | 1 | 1 | 3 | 7 |
| 1 | 30000 | 1 | 3 | 5 | 8 |

These are **scheduler wakeup delays**, not physical ADC/DAC latency, network latency or the application's minimum safe audio buffer. The measurements characterize this short workload on this instance, not a hard bound for all workloads. [Histogram](../validation/2026-10-15/cyclictest.txt), [summary](../validation/2026-10-15/rt-summary.json).

Network missing/late/queue/TX/deadline counters are not applicable to this standalone driver test because no AoIP transport was running. Application deadline/queue metrics and inter-receive gaps were not instrumented. Do not interpret their absence as measured zero.

## Source and binary provenance

The standalone repository retains the tested kernel source, clock policy, overlay and hardware-test source unchanged. Installation/packaging scripts and documentation were prepared for standalone distribution separately.

| Artifact | SHA-256 |
|---|---|
| `piaoip_akm.c` | `2341b0f098b4daf314c1d2b4573b7dcdbf981274cff788739105fb1f2e6e0eeb` |
| Prebuilt RT module | `02e97ed88863d0a7888c7b5fefda0da1e463704137f2829c7b72e7869426da9c` |
| Prebuilt overlay | `395106d10d13fb8e197b1dafb4d153649ce6da581ca815d8d7f12a2be3d235d9` |

The module has the expected `6.18.50+rpt-rpi-v8-rt` vermagic and is accompanied by its corresponding source/build files. Binary hashes can differ after a new build even with unchanged source; the included binary hashes identify the tested installed artifact, not every possible recompilation.

The distribution [manifest](../manifest.json) and [SHA256SUMS](../SHA256SUMS) cover sources, documentation, selected evidence and binaries. Verification with `tools/verify_package.py` checks consistency of the saved evidence without executing a hardware test. Repository checks/CI are distinguished from physical tests.

## Standalone distribution recheck

After separating the repository, its archive was copied to RAM on the same Pi. The module was rebuilt against the real RT headers with `W=1`, the portable test passed, the rebuilt overlay matched the supplied overlay, and both source/prebuilt installer prerequisite modes passed without changing installed files. The installed driver source and binary matched the distribution hashes above.

The repository's all-rate duplex and lifecycle tests were then rerun successfully on the installed module with identical source. The final state remained muted and idle with zero I²C errors. The new installation script's mutating install path was not executed during this recheck; the existing working installation was retained.

Evidence: [standalone check summary](../validation/2026-10-15/standalone-check.json), [build log](../validation/2026-10-15/standalone-build.txt), [all rates](../validation/2026-10-15/standalone-all-rates.json), [lifecycle](../validation/2026-10-15/standalone-lifecycle.json).

## Remaining qualification

Before treating this release as a fully qualified analog audio interface:

1. Apply a known input signal independently to each analog input; confirm connector/channel mapping, polarity, level and clipping margin.
2. Play known per-channel signals; verify every physical DAC output, gain, polarity and channel order.
3. Measure a physical DAC-to-ADC loopback at the intended rate and application buffer. Report analog round-trip latency separately from scheduler/network measurements.
4. Measure noise, distortion, crosstalk and frequency response with suitable terminations/instrumentation. Unconnected-input sample statistics are insufficient.
5. Run sustained native-application/AoIP/DSP workloads with actual buffer sizes, CPU load, XRUNs, missed deadlines, queue counters and receive gaps recorded.
6. Qualify induced XRUN recovery and physical channel identity after recovery; the existing tests stop on transfer failure.
7. Test suspend/resume, power interruption/recovery, supply and temperature conditions. Suspend/resume handlers exist but have not been bench-tested.
8. Measure BCLK/LRCLK/data timing and signal integrity, particularly at 24.576 MHz BCLK in the 384 kHz mode.

The 50 ms playback startup hold and 100 ms shutdown mute allowance are control-sequencing decisions. They must not be described as the measured steady-state ADC/DAC latency.
