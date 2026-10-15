# Troubleshooting

[Home](../README.md) · [Installation / rollback](INSTALL.md) · [Controls](CONTROLS.md)

## The ALSA card is missing

Collect:

```sh
uname -r
modinfo snd_soc_piaoip_akm
cat /proc/asound/cards
journalctl -k -b --no-pager | grep -Ei 'piaoip|akm|i2s|i2c'
grep -n 'piaoip-akm' /boot/firmware/config.txt
```

Check the running kernel first. A module built for another ABI must be rebuilt for the current kernel. Verify the overlay file exists, its managed section is enabled, and the Pi was rebooted after installation. If the module/overlay are present but probe fails, read the first specific error in the boot log.

This overlay targets Raspberry Pi 5 Model B and RP1; it does not provide Pi 4 compatibility. A competing audio/PWM/GPIO overlay may reserve the same pins. Resolve that ownership conflict rather than overriding GPIOs at runtime.

## Probe reports an I²C initialization or readback failure

The board must be powered and the H10 connection must match [the pinout](HARDWARE.md). Check SDA/SCL continuity, shared ground, local supply rails and the two PDN/OE routes. Both devices must use the fixed 7-bit addresses `0x10` / `0x11`.

The module holds the chips in reset while idle, so failure to respond to an idle external scan does not establish a fault. Use the probe log, `akmctl status` and an active stream's register snapshot. Do not force userspace writes while the module is loaded.

`i2c_errors` is cumulative for the module lifetime. A successful later configuration clears `last_error` but not that counter. Save logs before unloading/reloading if investigating an intermittent fault.

## `EBUSY` / device or resource busy

Possible causes are:

- A second application already owns the raw capture/playback device.
- Playback and capture requested different rates.
- An ADC filter/HPF change was attempted while either PCM held parameters, even if stopped/prepared.
- Another driver reserved the GPIOs or I²C addresses during probe.

For a filter or rate change, stop applications and close/free both PCM directions, apply the preference, then open them at the same rate. Inspect open sound devices if needed:

```sh
fuser -v /dev/snd/*
```

## `EINVAL` / unsupported parameters

The hardware interface accepts exactly eight channels, `S32_LE`, and one of the seven documented rates. A stereo/16-bit file needs application/plugin conversion. 364 kHz is not a supported rate; the implemented high rate is **384000 Hz**. Rates supported by an individual chip are not automatically supported by this board/driver.

## Capture works but analog playback is silent

Check `akmctl status`: `output_enabled=0` is intentional mute. Select a conservative level, permit output, and start a matching eight-channel PCM file:

```sh
akmctl volume -12
akmctl unmute
aplay -D hw:PiAoIPAKM,0 eight-channel.wav
akmctl mute
```

The initial 50 ms playback hold and DAC soft-mute ramp can suppress the start of a very short file. A zero-filled test stream is silent by design. Capture and zero-TX tests do not establish working analog output circuitry, wiring or gain; verify those with a known signal and suitable measurement setup.

## `powered=0`, `rate=0`, or registers return “Host is down”

This is the expected idle condition after both streams release their parameters. Opening/configuring a stream enables clocks and codec power state automatically. `registers` intentionally returns `EHOSTDOWN` while reset is asserted.

## HPF / filter preference has no effect at 384 kHz

AK5538 OCT mode ignores both settings. This is a chip mode limitation. `hpf` shows the saved preference; `hpf_effective=0` shows that it is not active in this mode. The same saved preference applies again at a supported lower rate.

## XRUNs or suspicious channel order

Save the application's rate, period, buffer, XRUN count, CPU load and kernel log. Compare with the supplied conservative-buffer smoke test. Do not treat `cyclictest` wakeup latency as ADC-to-DAC latency.

The recorded tests completed with no XRUN. They did not qualify recovery from an induced XRUN or verify every physical analog channel after recovery. If an application experiences an XRUN, record the discontinuity, restart/recover its PCM session and check channel identity with a known signal before relying on routing. Validate smaller buffers and sustained network/DSP loads separately.

## The hardware test fails at register assertions

The smoke test expects the output switch off and HPF preference on. Close other audio programs, then set:

```sh
akmctl mute
akmctl hpf on
python3 tests/pcm_smoke.py
```

The test checks expected registers before streaming. An assertion after a deliberate alternative profile is not necessarily a DMA failure. Inspect the saved report/log before changing the driver.

## No space left on a 4 GB card

Check `df -h / /boot/firmware /tmp`. Use bounded recordings and external storage. Keep the running kernel and its modules. `sudo apt-get clean` can remove cached downloads; kernel/firmware purges require checking actual installed hardware and a bootable fallback. The driver installer performs no such cleanup. See [storage notes](INSTALL.md#operating-with-a-4-gb-microsd).

## Useful issue report

Include the board wiring revision, kernel version, driver version, installation method, application parameters, driver status and relevant kernel errors. For a PCM problem include exact buffer/period sizes, duration and XRUN/error counters. State whether there was an actual known analog signal or physical loopback. Omit credentials, network addresses and unrelated personal/order data from shared logs.
