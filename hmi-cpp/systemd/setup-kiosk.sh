#!/bin/sh
# Kiosk boot for the KSP HMI: from power-on the screen shows only the intro video (its last frame held), then the HMI.
# Run once as root after ./ci deploy_hmi:   sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh
# Undo everything (restores the boot file backups):   sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh --undo
# Idempotent: run it again after any change to this script. SSH, the network profiles and the serial console stay
# as they are.
#
# What it changes (each step is reverted by --undo):
#  - cmdline.txt: kernel/systemd text silenced (quiet, loglevel=3, no status), no logo, no console cursor, no
#    console blanking (backup: cmdline.txt.ksphmi-backup). console=tty1 stays: console=ttynull hung the Pi 4 early
#    in boot (black screen, no network, nothing in the journal; 2026-11-15), so it is removed if present;
#  - config.txt: no firmware rainbow splash, no under-voltage/warning overlays (backup: config.txt.ksphmi-backup);
#  - /etc/tmpfiles.d/ksphmi-cursor.conf: fbcon cursor blinking off at every boot;
#  - getty@tty1 masked: no login prompt on the screen (SSH logins are unaffected);
#  - mpv (apt, only when missing; /etc/ksphmi-mpv.installed records that this script installed it, so --undo
#    removes it again): plays the boot intro video;
#  - /etc/systemd/system/ksphmi-splash.service runs ksphmi-intro.sh: mpv plays assets/intro/KSPKontroller_pi_Intro.mp4
#    on DRM with HDMI audio and holds its last frame until ksphmi takes the display over mpv's IPC socket
#    /run/ksphmi-intro/mpv.sock; without mpv or the video it falls back to `ksphmi --splash` on /dev/fb0. The udev
#    rule /etc/udev/rules.d/99-ksphmi-fb.rules still tags the DRM framebuffer (vc4drmfb) fb0 for that fallback;
#  - /etc/systemd/journald.conf.d/50-ksphmi-persistent.conf: the journal survives reboots (Raspberry Pi OS keeps it
#    in RAM), so a failed boot can be read with journalctl -b -1;
#  - /etc/NetworkManager/dispatcher.d/90-wifi-fallback: Ethernet first, the Wi-Fi profile named wifi-fallback only
#    without the cable (eth0 up takes the Wi-Fi down, so kspKontrollerPi.local never has two addresses). The profile
#    holds the network name and password, so it is never in the repo: create it once on the Pi with
#      sudo nmcli --ask device wifi connect <SSID> name wifi-fallback
#      sudo nmcli connection modify wifi-fallback ipv4.route-metric 600 ipv6.route-metric 600
#    --undo removes the dispatcher and keeps the profile;
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
mpvmark=/etc/ksphmi-mpv.installed
rule=/etc/udev/rules.d/99-ksphmi-fb.rules
cursor=/etc/tmpfiles.d/ksphmi-cursor.conf
journal=/etc/systemd/journald.conf.d/50-ksphmi-persistent.conf
stamp=/etc/ksphmi-kiosk.applied
wifi=/etc/NetworkManager/dispatcher.d/90-wifi-fallback

if [ "$undo" = yes ]; then
    for f in "$cmdline" "$config"; do
        if [ -f "$f.ksphmi-backup" ]; then
            cp "$f.ksphmi-backup" "$f" && rm -f "$f.ksphmi-backup"
            echo "restored $f"
        fi
    done
    systemctl disable --now ksphmi-splash.service 2>/dev/null || true
    rm -f "$unit" "$rule" "$cursor" "$journal" "$stamp" "$wifi"
    if [ -f "$mpvmark" ]; then
        DEBIAN_FRONTEND=noninteractive apt-get remove -y mpv || echo "WARNING: could not remove mpv" >&2
        rm -f "$mpvmark"
    fi
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

# The boot intro video: mpv, installed once. No network must not break the kiosk: the unit then shows the loading
# screen (ksphmi --splash) and the next run of this script installs mpv.
if ! command -v mpv >/dev/null 2>&1; then
    echo "Installing mpv for the boot intro video"
    if DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends mpv ||
        { apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends mpv; }; then
        date > "$mpvmark"
    else
        echo "WARNING: could not install mpv (no network?): the boot shows the loading screen instead of the intro"              "video; run this script again later" >&2
    fi
fi
[ -r "$home/ksphmi/cpp/assets/intro/KSPKontroller_pi_Intro.mp4" ] ||
    echo "WARNING: no intro video in $home/ksphmi/cpp/assets/intro: run ./ci deploy_hmi" >&2

# The unit runs as the HMI user: group video opens /dev/dri/card1 and /dev/fb0, render the GPU, audio the HDMI
# sound card (the unit adds them as supplementary groups too); systemd only sees fb0 when udev tags it.
for group in video render audio; do
    if getent group "$group" >/dev/null && ! id -nG "$user" | grep -qw "$group"; then
        usermod -aG "$group" "$user"
    fi
done
# Only the DRM framebuffer: the firmware's simplefb is fb0 for ~2 s first and is then replaced by vc4drmfb;
# tagging it started the splash on a device about to disappear.
printf '%s\n' 'SUBSYSTEM=="graphics", KERNEL=="fb0", ATTR{name}=="vc4drmfb", TAG+="systemd"' > "$rule"
udevadm control --reload 2>/dev/null || true
sed -e "s|@USER@|$user|g" -e "s|@HOME@|$home|g" "$here/ksphmi-splash.service" > "$unit"
systemctl daemon-reload
systemctl enable ksphmi-splash.service

# Keep the journal on disk: a boot that fails before SSH comes up can then be read after the next good boot.
mkdir -p "$(dirname "$journal")"
printf '%s\n' '[Journal]' 'Storage=persistent' > "$journal"
systemctl restart systemd-journald || true

# Wi-Fi only as a fallback for the Ethernet cable (NetworkManager runs the dispatcher on every link change).
if [ -d "$(dirname "$wifi")" ]; then
    install -m 755 -o root -g root "$here/90-wifi-fallback" "$wifi"
    if ! nmcli -t -f NAME connection show 2>/dev/null | grep -qx wifi-fallback; then
        echo "NOTE: no Wi-Fi fallback profile; to use one: sudo nmcli --ask device wifi connect <SSID> name" \
            "wifi-fallback && sudo nmcli connection modify wifi-fallback ipv4.route-metric 600 ipv6.route-metric 600" >&2
    fi
fi

loginctl enable-linger "$user"
sha256sum "$here/$(basename "$0")" | cut -d' ' -f1 > "$stamp"
if [ ! -e "$home/.config/systemd/user/default.target.wants/ksphmi.service" ]; then
    echo "WARNING: ksphmi.service is not enabled for $user: run ./ci deploy_hmi from the PC" >&2
fi
if [ ! -x "$home/ksphmi/cpp/build/ksphmi" ]; then
    echo "WARNING: $home/ksphmi/cpp/build/ksphmi missing: run ./ci deploy_hmi (the HMI and its fallback loading screen)" >&2
fi
echo "Kiosk configured for $user. Takes effect at the next reboot (sudo reboot); SSH stays enabled."
