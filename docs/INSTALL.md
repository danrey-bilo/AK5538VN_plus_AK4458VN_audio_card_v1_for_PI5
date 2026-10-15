# Installation and recovery

[Home](../README.md) · [Русское руководство](../README.ru.md) · [Troubleshooting](TROUBLESHOOTING.md)

## Requirements

- Raspberry Pi 5 Model B, 64-bit Raspberry Pi OS, and the matching audio card wiring in [Hardware](HARDWARE.md).
- The board is connected and powered before the driver binds. Probe verifies both converters over I²C.
- No other overlay or process owns GPIO 5, 6, 16, 17 or 18–27, or I²C addresses `0x10`/`0x11` on bus 1.
- Python 3.9+, ALSA utilities, `kmod`, and a mounted `/boot/firmware` boot partition.
- Source builds additionally need GCC/make, `dtc` and headers matching the running kernel.

The tested kernel is **`6.18.50+rpt-rpi-v8-rt`**, ARM64 with `CONFIG_PREEMPT_RT=y` and 4 KB pages. The source uses Linux 6.18 ASoC APIs. Other versions, stock kernels and other Pi models are not qualified by this release.

Use a GPIO/header connection only with the power arrangement designed for this board. H10 pins 2/4 are connected to the board's +5P rail; they are not isolated from the Pi 5 V header. Refer to the board power design before combining power supplies. The driver cannot validate power wiring.

## Get the distribution

Clone the repository, or extract a release archive on the Pi:

```sh
git clone https://github.com/danrey-bilo/AK5538VN_plus_AK4458VN_audio_card_v1_for_PI5.git
cd AK5538VN_plus_AK4458VN_audio_card_v1_for_PI5
python3 tools/verify_package.py
```

Archive users enter the extracted `piaoip-akm-v5-driver-0.1.0` directory. Verify the downloaded archive with its release `SHA256SUMS` before extraction, then verify the internal manifest as above. Git is needed for development/package generation, but not for installing an extracted archive.

## Prepare packages

Install runtime dependencies:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends python3 alsa-utils libasound2t64 kmod
```

`libasound2t64` is the Debian 13 / Raspberry Pi OS Trixie library package used for testing. On a different distribution, install the package providing `libasound.so.2` instead.

For a source build, also install:

```sh
sudo apt-get install --no-install-recommends build-essential device-tree-compiler linux-headers-rpi-v8-rt
test -d "/lib/modules/$(uname -r)/build"
```

The header metapackage above is for the tested RT kernel family. Installing the latest headers does not guarantee they match an older running kernel. Compare `uname -r` with the header directory. Reboot into the intended kernel before building/installing for it.

## Optional: select the RT kernel

This is an OS step, separate from the audio installer. On the tested Raspberry Pi OS package repository, RT packages were available as follows:

```sh
apt-cache policy linux-image-rpi-v8-rt linux-headers-rpi-v8-rt
sudo apt-get install --no-install-recommends linux-image-rpi-v8-rt linux-headers-rpi-v8-rt
```

Check that `/boot/firmware/kernel8_rt.img`, `/boot/firmware/initramfs8_rt`, and the matching `/lib/modules/...` directory exist before selecting them. Save the current boot configuration and keep the stock Pi 5 kernel (`kernel_2712.img`) and its modules as a fallback.

Add this selection at the end of `/boot/firmware/config.txt` when these exact filenames are provided by the installed packages:

```ini
# BEGIN PIAOIP AKM RT
[pi5]
kernel=kernel8_rt.img
initramfs initramfs8_rt followkernel
[all]
# END PIAOIP AKM RT
```

Reboot and verify:

```sh
uname -r
grep '^CONFIG_PREEMPT_RT=' "/boot/config-$(uname -r)"
test -d "/lib/modules/$(uname -r)/build"
```

The overlay enables I²C1 at 100 kHz itself. No separate GPIO manipulation or codec probing script is required. Installing RT does not by itself enable the audio card. The audio installer below does not switch kernels or install packages automatically.

## Option A: use the supplied binary

Only for **`uname -r` = `6.18.50+rpt-rpi-v8-rt`**:

```sh
bash install.sh --prebuilt --check
sudo bash install.sh --prebuilt
sudo reboot
```

`--check` performs read-only prerequisite, checksum and module ABI checks. The `.ko` belongs to this exact kernel build. Matching just the major/minor version is insufficient; a kernel built with different symbol versions/configuration can also reject it. If the kernel differs, use a source build. Never force-load an incompatible module.

## Option B: build for the running kernel

```sh
bash install.sh --check
make -j2 all test
sudo bash install.sh
sudo reboot
```

`make` builds the external module with `W=1`, compiles the overlay, and checks the portable clock/configuration policy. The installer repeats the build as needed and validates its kernel version before copying files.

## What installation changes

| Path | Purpose |
|---|---|
| `/lib/modules/$(uname -r)/extra/snd-soc-piaoip-akm.ko` | ASoC board/codec module |
| `/boot/firmware/overlays/piaoip-akm-v5.dtbo` | RP1, I²C, reset and oscillator GPIO configuration |
| `/etc/modprobe.d/piaoip-akm.conf` | Loads `designware_i2s` before this module |
| `/etc/modules-load.d/piaoip-akm.conf` | Automatic module loading |
| `/usr/local/bin/akmctl` | ALSA control utility |
| `/usr/local/src/piaoip-akm-v5` | Verified sources, documentation, binaries and tests |
| `/boot/firmware/config.txt` | Adds one `BEGIN/END PIAOIP AKM AUDIO` section |

The installer backs up existing configuration/module/overlay/CLI files under a newly created, private `/root/piaoip-akm-backup-*` directory. Its exact location is printed. Repeated installation replaces its own audio section without adding duplicates. It does not remove other overlays; resolve pin conflicts before installing.

When the card already exists, the installer selects output disabled, DAC level −12 dB and ADC HPF enabled, then saves the system ALSA state. On a first boot the module's output switch defaults to disabled and volume defaults to 0 dB; explicitly select mute and your intended level before playback. Generic ALSA state restoration may restore previous settings.

## Verify after reboot

```sh
akmctl mute
akmctl volume -12
akmctl status
arecord -l
aplay -l
python3 /usr/local/src/piaoip-akm-v5/tests/pcm_smoke.py
python3 /usr/local/src/piaoip-akm-v5/tests/lifecycle.py
```

The smoke test transmits only zeros. It requires HPF enabled and the output switch disabled; its register assertions intentionally check this starting state. Lifecycle tests also use zero playback and leave output disabled; they reset the ADC filter preferences and set DAC attenuation to −12 dB during testing.

Expected idle state: `powered=0`, `rate=0`, both masks `0`, `i2c_errors=0`. Idle reset/power-down is normal. During duplex, `configured_mask=3`, the active rate is reported and both converters have clocks. See [validation](VALIDATION.md) and [controls](CONTROLS.md).

If the user has no sound-device access:

```sh
sudo usermod -aG audio "$USER"
```

Log out and back in before testing. Do not run normal audio applications as root just to bypass device permissions.

## Updating the kernel or driver

This version does **not** provide DKMS. Each kernel ABI needs its own build and installed module.

After a kernel update, keep the previous working kernel available. Boot into the intended new kernel, install its matching headers, run a source build/install, reboot and verify the card. The supplied old binary is not a fallback for a new ABI.

For intentional edits to a Git checkout, refresh its manifest before installation; see [development](DEVELOPMENT.md). Checksum verification rejects edited distributions until their manifest is deliberately rebuilt.

## Disable and roll back

Disable automatic loading while retaining source files and RT:

```sh
sudo bash /usr/local/src/piaoip-akm-v5/uninstall.sh
sudo reboot
```

This removes the managed **audio** section and the two module configuration files. The current module remains active until reboot; close streams before doing maintenance. A separate manual `dtoverlay=piaoip-akm-v5` line must also be removed if one was added outside the managed section.

For a boot problem, mount the boot partition on another computer. Remove the managed audio section to disable the board. To undo the optional RT selection, also remove its RT section and restore the preceding kernel/initramfs selection from your own backup. Retain a stock kernel and its matching modules. Do not copy an old whole `config.txt` over unrelated later changes without comparing them.

## Operating with a 4 GB microSD

The prototype operates on a 4 GB card with 4 GB RAM. After reviewed OS cleanup, a 2026-10-15 snapshot had about **1.54 GiB free on root**, with RT headers/build tools and a stock Pi 5 fallback retained. This is a measured installation snapshot, not a minimum-free-space guarantee for another OS image.

- Use release binaries when the exact ABI matches; avoid storing several kernel build trees and recordings.
- APT's `sudo apt-get clean` removes downloaded package archives after installation. Do not purge the running kernel, matching modules or needed firmware.
- Keep test outputs in bounded `/tmp` files or on external/network storage. At 384 kHz, eight `S32_LE` channels use **12,288,000 bytes/s per direction**: a one-minute recording is about 703 MiB.
- `/tmp` may be a RAM filesystem; check it with `df -h /tmp`. RAM recordings still have a finite limit.
- The prototype uses 2 GiB of virtual zram swap without an SD backing file, configured through the OS `rpi-swap` service. This is an optional system configuration, not an audio-driver dependency. Avoid swap activity during latency measurements.

No firmware/locale/kernel cleanup is performed by this repository's installer.
