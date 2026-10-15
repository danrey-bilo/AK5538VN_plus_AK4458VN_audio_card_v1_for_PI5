#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
# Non-root login shells may omit the directories containing kmod utilities.
export PATH="$PATH:/usr/sbin:/sbin"
cd -- "$(dirname -- "$0")"
mode=source
check=0
for arg in "$@"; do
    case "$arg" in
        --prebuilt) mode=prebuilt ;;
        --check) check=1 ;;
        -h|--help)
            echo 'Usage: sudo bash install.sh [--prebuilt] [--check]'
            echo 'Default: build for the running kernel. --prebuilt: use exact-ABI dist binaries.'
            echo '--check: read-only prerequisite check; no sudo required.'
            exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done
fail() { echo "Error: $*" >&2; exit 1; }
[[ $(uname -m) == aarch64 ]] || fail 'Raspberry Pi OS ARM64 is required.'
[[ -r /proc/device-tree/model ]] || fail 'Raspberry Pi device tree not found.'
tr '\0' '\n' </proc/device-tree/model | grep -q '^Raspberry Pi 5 Model B' || fail 'This overlay targets Raspberry Pi 5 Model B.'
for tool in python3 modinfo install depmod amixer alsactl; do
    command -v "$tool" >/dev/null || fail "Missing command: $tool (see docs/INSTALL.md)."
done
kernel=$(uname -r)
[[ -f /boot/firmware/config.txt && -d /boot/firmware/overlays ]] || fail '/boot/firmware is not ready.'
if [[ $mode == source ]]; then
    command -v make >/dev/null && command -v cc >/dev/null && command -v dtc >/dev/null || fail 'Install build-essential and device-tree-compiler.'
    [[ -d /lib/modules/$kernel/build ]] || fail "Install headers matching $kernel."
    module=snd-soc-piaoip-akm.ko
    overlay=piaoip-akm-v5.dtbo
else
    module="dist/$kernel/snd-soc-piaoip-akm.ko"
    overlay="dist/$kernel/piaoip-akm-v5.dtbo"
    [[ -f $module && -f $overlay ]] || fail "No prebuilt binaries for $kernel; build from source."
    [[ $(modinfo -F vermagic "$module") == "$kernel "* ]] || fail 'Prebuilt module kernel ABI mismatch.'
fi
python3 tools/verify_package.py --integrity-only
if ((check)); then
    echo "Prerequisites OK: $mode installation for $kernel. No files changed."
    exit 0
fi
[[ $EUID == 0 ]] || fail 'Run the installation with sudo.'
if [[ $mode == source ]]; then
    make -j2 all test
    [[ $(modinfo -F vermagic "./$module") == "$kernel "* ]] || fail 'Built module kernel ABI mismatch.'
fi
backup=$(mktemp -d /root/piaoip-akm-backup-XXXXXXXX)
chmod 0700 "$backup"
for path in /boot/firmware/config.txt /etc/modprobe.d/piaoip-akm.conf \
    /etc/modules-load.d/piaoip-akm.conf /usr/local/bin/akmctl \
    "/lib/modules/$kernel/extra/snd-soc-piaoip-akm.ko" \
    /boot/firmware/overlays/piaoip-akm-v5.dtbo; do
    [[ ! -f $path ]] || cp -a --parents -- "$path" "$backup/"
done
install -d -m 0755 "/lib/modules/$kernel/extra" /usr/local/src/piaoip-akm-v5
install -m 0644 "$module" "/lib/modules/$kernel/extra/snd-soc-piaoip-akm.ko"
install -m 0644 "$overlay" /boot/firmware/overlays/piaoip-akm-v5.dtbo
if [[ $(pwd -P) != /usr/local/src/piaoip-akm-v5 ]]; then
    python3 - <<'PY'
from pathlib import Path
import shutil
root = Path.cwd()
dest = Path('/usr/local/src/piaoip-akm-v5')
# Copy the verified distribution, excluding local compiler outputs and Git data.
paths = Path('SHA256SUMS').read_text().splitlines()
names = [line.split('  ', 1)[1] for line in paths] + ['SHA256SUMS']
for name in names:
    target = dest / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / name, target)
PY
fi
install -m 0755 tools/akmctl.py /usr/local/bin/akmctl
cat >/etc/modprobe.d/piaoip-akm.conf <<'EOF'
softdep snd_soc_piaoip_akm pre: designware_i2s
EOF
echo snd_soc_piaoip_akm >/etc/modules-load.d/piaoip-akm.conf
depmod -a "$kernel"
python3 - <<'PY'
from pathlib import Path
import re
path = Path('/boot/firmware/config.txt')
text = re.sub(r'\n# BEGIN PIAOIP AKM AUDIO\n.*?# END PIAOIP AKM AUDIO\n?', '\n', path.read_text(), flags=re.S)
text += '\n# BEGIN PIAOIP AKM AUDIO\n[pi5]\ndtoverlay=piaoip-akm-v5\n[all]\n# END PIAOIP AKM AUDIO\n'
path.write_text(text)
PY
if amixer -c PiAoIPAKM info >/dev/null 2>&1; then
    amixer -q -c PiAoIPAKM cset name='DAC Playback Switch' 0
    amixer -q -c PiAoIPAKM cset name='DAC Playback Volume' 231,231,231,231,231,231,231,231
    amixer -q -c PiAoIPAKM cset name='ADC High Pass Filter Switch' 1
    alsactl store
fi
sync
echo "Installed for $kernel. Backup: $backup"
echo 'Sources: /usr/local/src/piaoip-akm-v5. Reboot, then run akmctl mute and akmctl status.'
