#!/usr/bin/env python3
"""Control the PiAoIP v5 board through its ALSA controls; never bypass the driver."""
import argparse
from pathlib import Path
import subprocess
import sys

STATUS = Path('/sys/bus/platform/devices/piaoip-akm/status')

def set_control(name, value):
    subprocess.run(['amixer', '-q', '-c', 'PiAoIPAKM', 'cset', 'name=' + name, value], check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    actions.add_parser('status')
    actions.add_parser('mute')
    actions.add_parser('unmute')
    volume = actions.add_parser('volume', help='set all eight DAC channels in dB')
    volume.add_argument('db', type=float)
    hpf = actions.add_parser('hpf', help='ADC HPF preference; stop PCM streams first')
    hpf.add_argument('value', choices=['on', 'off'])
    filt = actions.add_parser('adc-filter', help='stop PCM streams first; ignored by ADC at 384 kHz')
    filt.add_argument('value', choices=['normal', 'short'])
    args = parser.parse_args()
    if not STATUS.exists():
        parser.error('PiAoIPAKM driver is not bound; check kernel, overlay and board power')
    if args.action == 'status':
        print(STATUS.read_text(), end='')
    elif args.action in ('mute', 'unmute'):
        set_control('DAC Playback Switch', '0' if args.action == 'mute' else '1')
    elif args.action == 'volume':
        if not -127.5 <= args.db <= 0:
            parser.error('volume must be between -127.5 and 0 dB')
        setting = round(255 + 2 * args.db)
        set_control('DAC Playback Volume', ','.join([str(setting)] * 8))
    elif args.action == 'hpf':
        set_control('ADC High Pass Filter Switch', '1' if args.value == 'on' else '0')
    else:
        set_control('ADC Short Delay Filter Switch', '1' if args.value == 'short' else '0')

if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
