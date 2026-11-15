#!/usr/bin/env bash
set -euo pipefail

# Sources are read-only; keep CMake paths stable and export only build artifacts.
mkdir -p /tmp/ksphmi
cp -R --preserve=timestamps /src/hmi-cpp /src/tools /tmp/ksphmi/
cp /src/ci /tmp/ksphmi/ci
ln -s /out /tmp/ksphmi/build
cd /tmp/ksphmi
if [ "${1:-test}" = test ]; then
    exec bash ./ci test_hmi --sim
fi

export DISPLAY=:1 SDL_VIDEODRIVER=x11 HOME=/tmp/ksphmi-home
mkdir -p "$HOME" /out/desktop
bash ./ci build_hmi
pids=()
cleanup() {
    trap - EXIT INT TERM
    kill "${pids[@]}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' INT TERM
Xvfb :1 -screen 0 3000x1800x24 -nolisten tcp > /out/desktop/xvfb.log 2>&1 &
pids+=("$!")
for attempt in {1..100}; do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done
xdpyinfo >/dev/null
openbox > /out/desktop/openbox.log 2>&1 &
pids+=("$!")
x11vnc -display :1 -localhost -rfbport 5900 -nopw -forever -shared -noxdamage \
    > /out/desktop/vnc.log 2>&1 &
pids+=("$!")
websockify --web=/usr/share/novnc 6080 127.0.0.1:5900 > /out/desktop/web.log 2>&1 &
pids+=("$!")
cat > /tmp/ksphmi-desktop.toml <<'CONFIG'
[krpc]
discovery = false
rpc_port = 50300
stream_port = 50301
CONFIG
if [ "${1:-}" = desktop-smoke ]; then
    simulator=("$KSPSIM_PYTHON" tools/hmi-container/smoke.py)
else
    simulator=("$KSPSIM_PYTHON" -m kspsim --no-board --address 127.0.0.1
        --rpc-port 50300 --stream-port 50301 --skin apollo)
    export PYTHONPATH="$PWD/tools"
fi
"${simulator[@]}" > /out/desktop/simulator.log 2>&1 &
pids+=("$!")
./build/hmi-cpp/ksphmi --windowed --address 127.0.0.1 --size 1680x1120 \
    --config /tmp/ksphmi-desktop.toml > /out/desktop/hmi.log 2>&1 &
pids+=("$!")
# Place both real application windows on the same desktop, with room for the simulator tabs.
for attempt in {1..100}; do
    if wmctrl -l 2>/dev/null | grep -q 'KSP HMI' && wmctrl -l 2>/dev/null | grep -q 'KSP simulator:'; then
        wmctrl -r 'KSP simulator:' -e 0,0,0,1280,1700
        wmctrl -r 'KSP HMI' -e 0,1300,0,1680,1120
        break
    fi
    sleep 0.1
done
echo 'Desktop ready: open http://localhost:6080/vnc.html?autoconnect=true&resize=scale'
# Exit if either app or a desktop service stops, so a broken session is visible to Docker.
set +e
wait -n "${pids[@]}"
rc=$?
if [ "${1:-}" = desktop-smoke ]; then
    cat /out/desktop/simulator.log
    "$KSPSIM_PYTHON" -c 'import json; assert json.load(open("/out/desktop/smoke.json"))["passed"] is True' || rc=1
fi
exit "$rc"
