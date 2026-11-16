#!/bin/sh
# Boot intro of the KSP HMI: ExecStart of the system unit ksphmi-splash.service (installed by setup-kiosk.sh).
# Plays assets/intro/KSPKontroller_pi_Intro.mp4 once, with sound over HDMI, through mpv on DRM (GPU scaling to the
# monitor's native mode), then keeps the last frame on the screen (--keep-open=always) until the HMI takes over:
# ksphmi waits for eof-reached on mpv's IPC socket $RUNTIME_DIRECTORY/mpv.sock (/run/ksphmi-intro/mpv.sock, see
# hmi-cpp/include/intro.hpp), sends quit, and opens the display once mpv has released DRM master.
# Without mpv or the video it runs the old animated loading screen instead (ksphmi --splash on /dev/fb0).
#
# Settings (environment, e.g. in ~/ksphmi/intro.env, read by the unit):
#   KSPHMI_INTRO_VIDEO  the video file (default ~/ksphmi/cpp/assets/intro/KSPKontroller_pi_Intro.mp4)
#   KSPHMI_INTRO_AUDIO  mpv --audio-device (default alsa/hdmi:CARD=vc4hdmi0,DEV=0, the HDMI port next to USB-C)
#   KSPHMI_INTRO_DRM    the KMS device (default /dev/dri/by-path/platform-gpu-card, else /dev/dri/card1)
#   KSPHMI_INTRO_VO     mpv --vo (default gpu: V3D scales; drm scales on the CPU, too slow for a 4K mode)
#   KSPHMI_INTRO_HWDEC  mpv --hwdec (default v4l2m2m-copy, the Pi 4 H.264 decoder). With the gpu VO's default
#                       high-quality scaling to 4K the video ran ~3 s behind its audio; --profile=fast keeps it in sync
#   KSPHMI_INTRO_LOG    mpv's log file of the last boot (default ~/ksphmi/intro-mpv.log; --no-terminal is silent)
set -u

app="${KSPHMI_APP_DIR:-$HOME/ksphmi/cpp}"
video="${KSPHMI_INTRO_VIDEO:-$app/assets/intro/KSPKontroller_pi_Intro.mp4}"
audio="${KSPHMI_INTRO_AUDIO:-alsa/hdmi:CARD=vc4hdmi0,DEV=0}"
vo="${KSPHMI_INTRO_VO:-gpu}"
hwdec="${KSPHMI_INTRO_HWDEC:-v4l2m2m-copy}"
mpvlog="${KSPHMI_INTRO_LOG:-$HOME/ksphmi/intro-mpv.log}"
drm="${KSPHMI_INTRO_DRM:-}"
if [ -z "$drm" ]; then
    drm=/dev/dri/by-path/platform-gpu-card
    [ -e "$drm" ] || drm=/dev/dri/card1
fi
# Connector status files (tests point this at fake ones).
connectors="${KSPHMI_INTRO_CONNECTORS:-/sys/class/drm/card*-HDMI-A-*/status}"
dir="${RUNTIME_DIRECTORY:-/run/ksphmi-intro}"
sock="$dir/mpv.sock"
starting="$dir/starting"
ready="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ksphmi.ready"

say() { echo "ksphmi-intro: $*" >&2; }

# The old loading screen: only on the DRM framebuffer (the firmware's simplefb is fb0 for ~2 s, then replaced).
splash() {
    rm -f "$starting"
    i=0
    while [ "$(cat /sys/class/graphics/fb0/name 2>/dev/null)" != vc4drmfb ] && [ $i -lt 150 ]; do
        sleep 0.2
        i=$((i + 1))
    done
    exec "$app/build/ksphmi" --splash
}

[ -e "$ready" ] && exit 0  # the HMI already shows its frames (unit restarted by hand)
if ! command -v mpv >/dev/null 2>&1; then
    say "mpv not installed (sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh): loading screen instead"
    splash
fi
if [ ! -r "$video" ]; then
    say "no intro video $video: loading screen instead"
    splash
fi

# Tells ksphmi to wait for mpv's socket (it may start while the monitor is still being detected).
: > "$starting"
# The monitor is often detected late (4K over HDMI): mpv needs a connected connector to pick a mode. 20 s at most.
i=0
while [ $i -lt 100 ]; do
    # shellcheck disable=SC2086 # the glob must expand
    for s in $connectors; do
        [ "$(cat "$s" 2>/dev/null)" = connected ] && [ -e "$drm" ] && break 2
    done
    sleep 0.2
    i=$((i + 1))
done
[ $i -lt 100 ] || say "no connected HDMI monitor after 20 s: trying mpv anyway"

pid=
stop() {
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    rm -f "$starting"
    exit 0
}
trap stop TERM INT HUP

say "playing $video on $drm (vo $vo, hwdec $hwdec, audio $audio)"
mpv --no-config --no-terminal --log-file="$mpvlog" \
    --vo="$vo" --gpu-context=drm --drm-device="$drm" --profile=fast --hwdec="$hwdec" \
    --ao=alsa --audio-device="$audio" --audio-fallback-to-null=yes \
    --keep-open=always --osd-level=0 --no-osc --no-input-default-bindings --no-input-cursor \
    --input-ipc-server="$sock" "$video" &
pid=$!

# Until mpv quits (ksphmi asks it to) or the HMI is ready; 10 minutes at most (the unit's RuntimeMaxSec is 11).
i=0
while kill -0 "$pid" 2>/dev/null; do
    [ -e "$starting" ] && [ -S "$sock" ] && rm -f "$starting"
    if [ -e "$ready" ] || [ $i -ge 3000 ]; then
        kill "$pid" 2>/dev/null
        break
    fi
    sleep 0.2
    i=$((i + 1))
done
wait "$pid"
code=$?
rm -f "$starting"
[ $code -eq 0 ] || say "mpv exited with code $code (see $mpvlog)"
exit 0
