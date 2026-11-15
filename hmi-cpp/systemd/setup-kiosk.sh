#!/bin/sh
# Kiosk boot for the KSP HMI: from power-on the screen shows only the HMI's loading screen, then the HMI.
# Run once as root after ./ci deploy_hmi:   sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh
# Undo everything (restores the boot file backups):   sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh --undo
# Idempotent: run it again after any change to this script. SSH, Wi-Fi/LAN and the serial console stay as they are.
#
# What it changes (each step is reverted by --undo):
#  - cmdline.txt: kernel/systemd text silenced (quiet, loglevel=3, no status), no logo, no console cursor, no
#    console blanking (backup: cmdline.txt.ksphmi-backup). console=tty1 stays: console=ttynull hung the Pi 4 early
#    in boot (black screen, no network, nothing in the journal; 2026-11-15), so it is removed if present;
#  - config.txt: no firmware rainbow splash, no under-voltage/warning overlays (backup: config.txt.ksphmi-backup);
#  - /etc/tmpfiles.d/ksphmi-cursor.conf: fbcon cursor blinking off at every boot;
#  - getty@tty1 masked: no login prompt on the screen (SSH logins are unaffected);
#  - /etc/udev/rules.d/99-ksphmi-fb.rules + /etc/systemd/system/ksphmi-splash.service: `ksphmi --splash` paints
#    the loading screen as soon as /dev/fb0 exists;
#  - /etc/systemd/journald.conf.d/50-ksphmi-persistent.conf: the journal survives reboots (Raspberry Pi OS keeps it
#    in RAM), so a failed boot can be read with journalctl -b -1;
#  - lingering for the HMI user, so the user unit ksphmi.service starts at boot without a login;
#  - /etc/ksphmi-kiosk.applied: sha256 of this script, so ./ci deploy_hmi knows when to run it again.
set -eu
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh [--undo]" >&2
    exit 1
fi

undo=no
user="${SUDO_USER:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --undo) undo=yes ;;
        --user) user="$2"; shift ;;
        *) echo "usage: setup-kiosk.sh [--undo] [--user NAME]" >&2; exit 2 ;;
    esac
    shift
done
here="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$user" ] || [ "$user" = root ]; then
    user="$(stat -c %U "$here")"  # the owner of ~/ksphmi/cpp/systemd
fi
home="$(getent passwd "$user" | cut -d: -f6)"
[ -n "$home" ] || { echo "Unknown user '$user' (use --user NAME)" >&2; exit 1; }

cmdline=/boot/firmware/cmdline.txt
[ -f "$cmdline" ] || cmdline=/boot/cmdline.txt
[ -f "$cmdline" ] || { echo "No Raspberry Pi boot command line found" >&2; exit 1; }
config="$(dirname "$cmdline")/config.txt"
unit=/etc/systemd/system/ksphmi-splash.service
rule=/etc/udev/rules.d/99-ksphmi-fb.rules
cursor=/etc/tmpfiles.d/ksphmi-cursor.conf
journal=/etc/systemd/journald.conf.d/50-ksphmi-persistent.conf
stamp=/etc/ksphmi-kiosk.applied

if [ "$undo" = yes ]; then
    for f in "$cmdline" "$config"; do
        if [ -f "$f.ksphmi-backup" ]; then
            cp "$f.ksphmi-backup" "$f" && rm -f "$f.ksphmi-backup"
            echo "restored $f"
        fi
    done
    systemctl disable ksphmi-splash.service 2>/dev/null || true
    rm -f "$unit" "$rule" "$cursor" "$journal" "$stamp"
    systemctl daemon-reload
    systemctl restart systemd-journald 2>/dev/null || true
    udevadm control --reload 2>/dev/null || true
    systemctl unmask getty@tty1.service
    systemctl enable getty@tty1.service 2>/dev/null || true
    echo "Kiosk boot removed (lingering for $user kept). Reboot to see the console and login prompt again."
    exit 0
fi

# Boot files: keep the root device, serial console and everything else; edit only what reaches the screen.
python3 - "$cmdline" <<'PY'
from pathlib import Path
import re
import sys
path = Path(sys.argv[1])
original = path.read_text()
backup = path.with_name(path.name + '.ksphmi-backup')
if not backup.exists():
    backup.write_text(original)
# Keep console=tty1 (and serial0): quiet + loglevel=3 + no systemd status already keep the screen clean, and
# console=ttynull hung the Pi 4 before the journal was written (black screen, no network). Drop it if an older
# version of this script added it.
first = []
add =['quiet', 'loglevel=3', 'logo.nologo', 'vt.global_cursor_default=0', 'consoleblank=0',
       'systemd.show_status=false', 'rd.systemd.show_status=false', 'plymouth.enable=0']
keys = {a.split('=', 1)[0] for a in add if '=' in a}
args = [a for a in original.split()
        if a != 'console=ttynull' and a not in add and a.split('=', 1)[0] not in keys]
path.write_text(' '.join(first + args + add) + '\n')
config = path.with_name('config.txt')
if config.exists():
    text = config.read_text()
    backup = config.with_name(config.name + '.ksphmi-backup')
    if not backup.exists():
        backup.write_text(text)
    marker = '# KSP HMI kiosk: hide firmware splash'
    if marker not in text:
        text = text.rstrip() + '\n\n[all]\n' + marker + '\ndisable_splash=1\n'
    warn = '# KSP HMI kiosk: no warning overlays on the screen'
    if warn not in text:
        text = text.rstrip() + '\n\n[all]\n' + warn + '\navoid_warnings=1\n'
    config.write_text(text)
PY

mkdir -p /etc/tmpfiles.d
printf '%s\n' 'w /sys/class/graphics/fbcon/cursor_blink - - - - 0' > "$cursor"
systemd-tmpfiles --create "$cursor" || true
# Masking persists across reboots and suppresses the local login prompt.
systemctl mask --now getty@tty1.service

# The splash runs as the HMI user (group video can write /dev/fb0); systemd only sees fb0 when udev tags it.
id -nG "$user" | grep -qw video || usermod -aG video "$user"
printf '%s\n' 'SUBSYSTEM=="graphics", KERNEL=="fb0", TAG+="systemd"' > "$rule"
udevadm control --reload 2>/dev/null || true
sed -e "s|@USER@|$user|g" -e "s|@HOME@|$home|g" "$here/ksphmi-splash.service" > "$unit"
systemctl daemon-reload
systemctl enable ksphmi-splash.service

# Keep the journal on disk: a boot that fails before SSH comes up can then be read after the next good boot.
mkdir -p "$(dirname "$journal")"
printf '%s\n' '[Journal]' 'Storage=persistent' > "$journal"
systemctl restart systemd-journald || true

loginctl enable-linger "$user"
sha256sum "$here/$(basename "$0")" | cut -d' ' -f1 > "$stamp"
if [ ! -e "$home/.config/systemd/user/default.target.wants/ksphmi.service" ]; then
    echo "WARNING: ksphmi.service is not enabled for $user: run ./ci deploy_hmi from the PC" >&2
fi
if [ ! -x "$home/ksphmi/cpp/build/ksphmi" ]; then
    echo "WARNING: $home/ksphmi/cpp/build/ksphmi missing: run ./ci deploy_hmi, the splash is skipped until then" >&2
fi
echo "Kiosk configured for $user. Takes effect at the next reboot (sudo reboot); SSH stays enabled."
