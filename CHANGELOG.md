# Changelog

## Unreleased — 0.1.1-dev

- Add the shared five-mode AK4458 reconstruction-filter ALSA control and `akmctl dac-filter`.
- Keep the previous short-delay sharp filter as the default and preserve its selection during mute/unmute.
- Apply filter changes on the next coordinated configuration after both PCMs release their parameters.
- Add register, busy-guard and zero-playback hardware checks for all five DAC filter modes.

The original 0.1.0 validation is retained as historical evidence with its original
manifest. The development module has separate filter-control verification at
192 kHz. The user reported reduced square-wave ringing after selecting
short-delay slow; this is not a calibrated analog performance measurement.

## 0.1.0 — 2026-10-15 — Prototype / prerelease

- Initial standalone ASoC driver for the audio card v1, EasyEDA v5 wiring.
- AK5538VN capture and AK4458VN playback, 8 channels each, `S32_LE`.
- Seven sample rates from 44.1 kHz to 384 kHz; shared-rate duplex enforcement.
- Four stereo lanes each way through the Pi 5 RP1 I²S1/DMA controller.
- Coordinated oscillator selection, reset and power sequencing with I²C readback.
- ALSA per-channel DAC attenuation, output switch, ADC HPF and filter controls.
- Read-only status and register diagnostics, plus the `akmctl` utility.
- Source installer, exact-ABI prebuilt installer, backup and disable scripts.
- English and Russian documentation, reproducible archives and SHA-256 manifests.
- Hardware evidence: all rates, lifecycle/controls and 60 seconds of 384 kHz duplex.

Analog loopback, calibrated output/channel tests, sustained application workloads,
small-buffer latency and actual suspend/resume are pending. This version does not
claim analog end-to-end latency or production audio qualification.
