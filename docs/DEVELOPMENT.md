# Development and packaging

[Home](../README.md) · [Hardware](HARDWARE.md) · [Validation](VALIDATION.md)

## Source layout

```text
piaoip_akm.c                    Combined ASoC board and codec-control module
akm_v5_config.h                 Shared clock/rate/register policy
piaoip-akm-v5-overlay.dts       Pi 5 RP1 I²S1, I²C1 and board GPIOs
Makefile                       External Kbuild, overlay and portable test
install.sh / uninstall.sh      Install, back up and disable autoload
tools/akmctl.py                 User-facing ALSA controls
tools/verify_package.py         Offline integrity/evidence/doc-link checks
tools/package.py                Reproducible distribution archives
tests/test_config.c            Portable clock/configuration policy test
tests/pcm_smoke.py              Physical ALSA/DMA duplex test; zero TX
tests/lifecycle.py              Shared-clock and hardware-control readback test
dist/<kernel>/                 Exact-ABI tested module and overlay
validation/2026-10-15/          Selected physical-test evidence
docs/                          Installation, hardware, controls and validation
manifest.json / SHA256SUMS      Distribution identity and file hashes
COPYING                        GPL v2 license text
```

The board module reuses the kernel's `designware_i2s`, ALSA SoC and DMAengine components. It does not replace RP1's DMA implementation. The board is represented by one platform device, one ASoC card and one codec DAI with eight-channel capture/playback. Two I²C regmaps handle converter registers.

## Build

On the tested Pi kernel with matching headers:

```sh
make -j2 all test
modinfo ./snd-soc-piaoip-akm.ko
```

On any Linux host with a C11 compiler, the portable policy test can run without kernel headers or hardware:

```sh
make test
```

It verifies clock ratios/families, ADC register format, all supported rate combinations for shared-clock conflicts, rejection of invalid rates, and unique DAC attenuation addresses. It does not emulate I²C, DMA or an analog converter.

Compile the overlay with `make overlay` (`device-tree-compiler` required). Kernel module builds default to:

```sh
make -C /lib/modules/$(uname -r)/build M="$PWD" W=1 modules
```

`KVER` and `KDIR` may be supplied for an alternate kernel build tree; an ARM64 cross build additionally requires an appropriate toolchain/kernel configuration. The installer intentionally installs for the **running** kernel, not an arbitrary cross-build target.

## Ownership and concurrency

- The ASoC card owns platform device drvdata; board state is retrieved through `snd_soc_card_get_drvdata`.
- The board mutex protects I²C, power state, configuration and controls.
- Trigger callbacks only update atomic stream bits and schedule mute work; no blocking register access is performed there.
- `configured_mask` keeps common clocks active while either direction owns parameters, regardless of which streams are running.
- I²C adapter references and device-tree node references are balanced through the matching APIs and managed cleanup actions.
- Card/component module ownership allows idle unload without a permanent self-reference; open PCM streams keep ownership.
- Reserved register fields are not filled from guessed cache defaults; writable configuration is explicitly selected and read back.

Changes to sequencing, reset, clock roles, rate policy or DAI format require fresh physical tests. Passing a host-side policy test does not qualify such changes on hardware.

## Hardware tests

Run from the source directory on the powered board, with other PCM clients closed:

```sh
akmctl mute
akmctl hpf on
python3 tests/pcm_smoke.py
python3 tests/lifecycle.py
python3 tests/pcm_smoke.py --rates 384000 --seconds 60 --skip-rejections --json-out /tmp/akm-384k-60s.json
```

Default smoke output is `/tmp/piaoip-pcm-smoke.json`; lifecycle output is `/tmp/piaoip-lifecycle.json`. Copy reports elsewhere before reboot if `/tmp` is volatile. The default smoke test checks all seven rates and rejects incompatible hardware/duplex parameters. It is deliberately sensitive to the expected muted/HPF-on profile.

Playback is zero-filled. Capture records only bounded first-sample statistics in RAM; it does not save a minute of raw audio. Any negative ALSA transfer result other than retryable `EAGAIN` fails the transfer; the test does not hide XRUNs by recovery. Reported zero XRUN means the test completed without such an error, not that a separate hardware counter exists.

The lifecycle test changes real volume/mute/filter controls while transmitting zeros, verifies register readback and checks that one direction survives closing the other. It sets output mute and returns filter preferences to normal at completion. Use an appropriate controlled analog setup for later nonzero signal tests.

## Offline distribution checks

```sh
bash -n install.sh uninstall.sh
python3 tools/verify_package.py
```

The verifier checks SHA-256, the manifest, Python syntax, local documentation links and the internal consistency of selected historical reports. It does not make new physical measurements.

GitHub Actions runs the portable policy test, compiles the overlay, checks shell syntax and verifies/packages the distribution. CI has no Pi or audio board; it does not build an ARM64 kernel module or claim a hardware pass. The prebuilt module and physical results are separately identified by hash/date.

## Build a distribution

Only staged/tracked repository paths enter the package. `.git`, local build products and other workspace directories are excluded. Stage intentional changes first; do not stage unrelated files.

After an intentional change to sources/docs/evidence:

```sh
git add README.md docs tools tests install.sh uninstall.sh
python3 tools/package.py --refresh-checksums
git add manifest.json SHA256SUMS
python3 tools/verify_package.py
```

To package an unchanged verified checkout:

```sh
python3 tools/package.py
```

Outputs under ignored `build/`:

- `piaoip-akm-v5-driver-0.1.0.zip`
- `piaoip-akm-v5-driver-0.1.0.tar.gz`
- `SHA256SUMS` for the two archives

The archives contain source, all guides, license, selected evidence and the exact-ABI binaries. Root-level `SHA256SUMS` covers their extracted contents. Archive timestamps/permissions and gzip metadata are fixed; `SOURCE_DATE_EPOCH` can override the default 2026-10-15 UTC timestamp. Identical files produce identical archives in the same packaging environment.

Do not refresh qualification flags blindly when driver behavior changes. First rerun the relevant physical tests, identify the new binary/source hashes, update evidence and document remaining limits. Prototype releases remain prereleases until analog/application qualification is complete.
