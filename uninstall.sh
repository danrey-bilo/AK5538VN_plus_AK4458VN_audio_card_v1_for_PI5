#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
[[ ${1:-} != --help ]] || { echo 'Usage: sudo bash uninstall.sh (disable autoload; retain files and RT kernel)'; exit 0; }
[[ $# == 0 ]] || { echo 'Unknown argument.' >&2; exit 2; }
[[ $EUID == 0 ]] || { echo 'Run with sudo.' >&2; exit 1; }
backup=$(mktemp -d /root/piaoip-akm-disable-XXXXXXXX)
chmod 0700 "$backup"
cp -a /boot/firmware/config.txt "$backup/config.txt"
python3 - <<'PY'
from pathlib import Path
import re
p = Path('/boot/firmware/config.txt')
p.write_text(re.sub(r'\n# BEGIN PIAOIP AKM AUDIO\n.*?# END PIAOIP AKM AUDIO\n?', '\n', p.read_text(), flags=re.S))
for name in ('/etc/modules-load.d/piaoip-akm.conf', '/etc/modprobe.d/piaoip-akm.conf'):
    Path(name).unlink(missing_ok=True)
PY
sync
echo "Automatic board loading disabled. Config backup: $backup/config.txt"
echo 'RT kernel and source files retained. Reboot to release the card and GPIOs.'
