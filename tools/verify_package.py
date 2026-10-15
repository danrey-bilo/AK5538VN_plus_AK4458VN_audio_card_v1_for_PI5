#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify distribution integrity and saved test evidence, without touching hardware."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]


def verify(integrity_only=False):
    lines = (ROOT / 'SHA256SUMS').read_text(encoding='utf-8').splitlines()
    hashes = {}
    for line in lines:
        digest, name = line.split('  ', 1)
        path = (ROOT / name).resolve()
        assert path.is_relative_to(ROOT) and path.is_file(), name
        assert name not in hashes and re.fullmatch('[0-9a-f]{64}', digest), name
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, f'Checksum mismatch: {name}'
        hashes[name] = digest
    manifest = json.loads((ROOT / 'manifest.json').read_text())
    assert {k: v for k, v in hashes.items() if k != 'manifest.json'} == manifest['sha256']
    source_version = re.search(r'#define AKM_VERSION "([^"]+)"',
                               (ROOT / 'piaoip_akm.c').read_text()).group(1)
    assert manifest['version'] == source_version
    if integrity_only:
        print(f'PASS: {len(hashes)} distribution checksums')
        return

    for name in hashes:
        path = ROOT / name
        if path.suffix == '.py':
            ast.parse(path.read_text(encoding='utf-8'), filename=name)
        if path.suffix in ('.py', '.sh', '.c', '.h', '.dts'):
            assert b'\r' not in path.read_bytes(), f'Expected LF line endings: {name}'
        if path.suffix == '.md':
            for link in re.findall(r'!?\[[^\]]*\]\(([^\s)]+)\)', path.read_text(encoding='utf-8')):
                if '://' in link or link.startswith(('mailto:', '#')):
                    continue
                relative = unquote(link.split('#', 1)[0])
                assert (path.parent / relative).exists(), f'Broken local link in {name}: {link}'

    evidence = ROOT / 'validation/2026-10-15'
    rates = json.loads((evidence / 'all-rates-final.json').read_text())
    stress = json.loads((evidence / '384k-60s.json').read_text())
    assert rates['pass'] and stress['pass']
    assert [case['rate'] for case in rates['cases']] == manifest['rates_hz']
    assert stress['cases'][0]['rate'] == 384000 and stress['cases'][0]['seconds'] == 60
    for report in (rates, stress):
        for case in report['cases']:
            assert case['pass'] and case['idle_state']['i2c_errors'] == '0'
            for direction in ('capture', 'playback'):
                result = case[direction]
                assert result['frames'] == round(case['rate'] * case['seconds'])
                assert result['xruns'] == 0
                assert abs(result['io_seconds'] - case['seconds']) <= max(.1, case['seconds'] * .05)
    lifecycle = json.loads((evidence / 'lifecycle-final.json').read_text())
    assert lifecycle['pass'] and lifecycle['short_delay_filter_register_verified']
    assert lifecycle['playback_survives_capture_close']['eight_volume_registers_verified']
    assert lifecycle['playback_survives_capture_close']['mute_unmute_registers_verified']
    rt = json.loads((evidence / 'rt-summary.json').read_text())
    assert sum(thread['samples'] for thread in rt['threads']) == 60000
    assert all(thread['histogram_overflows'] == 0 for thread in rt['threads'])
    assert manifest['qualification']['analog_loopback'] is False
    standalone = evidence / 'standalone-check.json'
    if standalone.exists():
        check = json.loads(standalone.read_text())
        historical = evidence / 'manifest-0.1.0.json'
        base_hashes = json.loads(historical.read_text())['sha256'] if historical.exists() else hashes
        for field in ('standalone_kernel_build_w1_pass', 'portable_policy_test_pass',
                      'overlay_rebuild_matches_distribution', 'shell_syntax_pass',
                      'source_install_prerequisites_pass', 'prebuilt_install_prerequisites_pass',
                      'all_rates_duplex_pass', 'lifecycle_controls_pass',
                      'loaded_driver_source_unchanged', 'installed_binary_matches_distribution'):
            assert check[field] is True, field
        assert check['source_sha256'] == base_hashes['piaoip_akm.c']
        assert check['installed_module_sha256'] == base_hashes['dist/' + check['kernel'] + '/snd-soc-piaoip-akm.ko']
        for name in ('standalone-all-rates.json', 'standalone-lifecycle.json'):
            assert json.loads((evidence / name).read_text())['pass'] is True, name
    if source_version != '0.1.0':
        filters = json.loads((evidence / 'dac-filter-controls.json').read_text())
        assert filters['pass'] and filters['driver_version'] == source_version
        assert filters['source_sha256'] == hashes['piaoip_akm.c']
        assert filters['policy_sha256'] == hashes['akm_v5_config.h']
        assert filters['module_sha256'] == hashes['dist/' + filters['kernel'] + '/snd-soc-piaoip-akm.ko']
        assert len(filters['register_tests']['cases']) == 5
        assert filters['register_tests']['pass']
        for case in filters['register_tests']['cases']:
            assert case['pass'] and case['mute_preserves_filter']
            assert case['playback']['frames'] == filters['register_tests']['rate_hz']
            assert case['playback']['xruns'] == 0
    if source_version == '0.1.2-dev':
        timing = json.loads((evidence / 'profile-driver-after.json').read_text())
        smoke = json.loads((evidence / 'profile-driver-smoke.json').read_text())
        assert timing['pass'] and smoke['pass']
        assert timing['initial']['version'] == source_version and len(timing['trials']) == 21
        assert [row['rate'] for row in smoke['cases']] == manifest['rates_hz']
        assert all(row['state']['version'] == source_version and row['pass'] for row in smoke['cases'])
        assert all(row['xruns'] == 0 for row in timing['trials'])
    print(f'PASS: {len(hashes)} checksums, local documentation links, Python syntax and saved evidence')
    print('Saved evidence is historical; this check does not run the hardware tests.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--integrity-only', action='store_true')
    args = parser.parse_args()
    verify(args.integrity_only)
