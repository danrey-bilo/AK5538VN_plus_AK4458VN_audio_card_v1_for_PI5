#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build portable source + exact-ABI binary archives from the explicit Git index."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import os
import re
from pathlib import Path
import subprocess
import tarfile
import zipfile

from verify_package import ROOT, verify

VERSION = re.search(r'#define AKM_VERSION "([^"]+)"', (ROOT / 'piaoip_akm.c').read_text()).group(1)
KERNEL = '6.18.50+rpt-rpi-v8-rt'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build')
    parser.add_argument('--refresh-checksums', action='store_true',
                        help='refresh manifest and checksums after intentional source/doc changes')
    args = parser.parse_args()
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    names = sorted(name for name in names if name and name not in ('manifest.json', 'SHA256SUMS'))
    assert 'piaoip_akm.c' in names and 'COPYING' in names, 'Stage project files with Git first.'
    assert not any(name.startswith(('.git/', 'build/', 'tmp/')) for name in names)
    if args.refresh_checksums:
        hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in names}
        manifest = {
            'version': VERSION, 'release_state': 'prerelease',
            'kernel_abi': KERNEL, 'architecture': 'aarch64',
            'board_revision': 'audio card v1; EasyEDA v5 2026-09-30',
            'board_sha256': '32218e21228911098f3cca51d3b827f022edd48c9390866c8f6940cfe198a2c3',
            'pcm': {'channels_per_direction': 8, 'format': 'S32_LE', 'bclk_fs': 64},
            'rates_hz': [44100, 48000, 88200, 96000, 176400, 192000, 384000],
            'qualification': {'evidence_date': '2026-10-15',
                              'base_evidence_driver_version': '0.1.0',
                              'historical_all_rates_duplex': True,
                              'historical_controls_and_shared_clocks': True,
                              'historical_384k_60s': True,
                              'dac_filter_registers_192k': VERSION != '0.1.0',
                              'current_all_rates_duplex': VERSION == '0.1.2-dev',
                              'current_teardown_timing_trials': 21 if VERSION == '0.1.2-dev' else 0,
                              'analog_loopback': False, 'suspend_resume': False},
            'sha256': hashes,
        }
        (ROOT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
        hashes['manifest.json'] = hashlib.sha256((ROOT / 'manifest.json').read_bytes()).hexdigest()
        (ROOT / 'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in sorted(hashes.items())), encoding='utf-8', newline='\n')
    verify()
    names += ['manifest.json', 'SHA256SUMS']
    names = sorted(names)
    # Fixed archive metadata makes unchanged distributions byte-reproducible.
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '1792022400'))  # 2026-10-15 00:00 UTC
    stamp = datetime.fromtimestamp(epoch, timezone.utc).timetuple()[:6]
    prefix = f'piaoip-akm-v5-driver-{VERSION}'
    args.output.mkdir(parents=True, exist_ok=True)
    zip_path = args.output / (prefix + '.zip')
    tar_path = args.output / (prefix + '.tar.gz')
    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in names:
            info = zipfile.ZipInfo(prefix + '/' + name, stamp)
            info.create_system = 3
            mode = 0o755 if name.endswith(('.py', '.sh')) else 0o644
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (ROOT / name).read_bytes())
    with tar_path.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name in names:
                data = (ROOT / name).read_bytes()
                info = tarfile.TarInfo(prefix + '/' + name)
                info.size, info.mtime = len(data), epoch
                info.mode = 0o755 if name.endswith(('.py', '.sh')) else 0o644
                archive.addfile(info, io.BytesIO(data))
    lines = []
    for path in (zip_path, tar_path):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f'{digest}  {path.name}\n')
        print(f'{path.name}: {path.stat().st_size} bytes; SHA256 {digest}')
    (args.output / 'SHA256SUMS').write_text(''.join(lines), encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
