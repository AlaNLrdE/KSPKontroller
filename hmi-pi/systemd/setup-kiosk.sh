#!/bin/sh
# Run once as root after deploying the HMI. SSH remains available.
set -eu
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo sh ~/ksphmi/app/systemd/setup-kiosk.sh" >&2
    exit 1
fi

cmdline=/boot/firmware/cmdline.txt
[ -f "$cmdline" ] || cmdline=/boot/cmdline.txt
[ -f "$cmdline" ] || { echo "No Raspberry Pi boot command line found" >&2; exit 1; }

# Preserve the root device and all other boot settings; edit only display verbosity.
python3 - "$cmdline" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
original = path.read_text()
backup = path.with_name(path.name + '.ksphmi-backup')
if not backup.exists():
    backup.write_text(original)
remove = ('console=tty1', 'quiet', 'logo.nologo', 'vt.global_cursor_default=0',
          'systemd.show_status=false', 'loglevel=3')
args = [arg for arg in original.split() if arg not in remove]
args += list(remove[1:])
path.write_text(' '.join(args) + '\n')
config = path.with_name('config.txt')
if config.exists():
    text = config.read_text()
    backup = config.with_name(config.name + '.ksphmi-backup')
    if not backup.exists():
        backup.write_text(text)
    marker = '# KSP HMI kiosk: hide firmware splash'
    if marker not in text:
        config.write_text(text.rstrip() + '\n\n[all]\n' + marker + '\ndisable_splash=1\n')
PY

mkdir -p /etc/tmpfiles.d
printf '%s\n' 'w /sys/class/graphics/fbcon/cursor_blink - - - - 0' > /etc/tmpfiles.d/ksphmi-cursor.conf
systemd-tmpfiles --create /etc/tmpfiles.d/ksphmi-cursor.conf
# Masking persists across reboots and suppresses the local login prompt.
systemctl mask --now getty@tty1.service
echo "Kiosk configured. Boot text changes take effect on the next reboot; SSH stays enabled."
