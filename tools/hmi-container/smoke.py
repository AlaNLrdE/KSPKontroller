"""Exercise actual Tk controls while the windowed C++ HMI consumes their wire data."""
import pathlib
import json
import sys
import time
import socket
import urllib.request
from tkinter import ttk

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from kspsim.app import App
from kspsim.__main__ import main

original_run = App.run
passed = False
result = pathlib.Path('/out/desktop/smoke.json')
result.write_text(json.dumps({'passed': False}))


def widgets(root):
    for child in root.winfo_children():
        yield child
        yield from widgets(child)


def run(app):
    deadline = time.monotonic() + 40
    changed = False

    def poll():
        nonlocal changed
        global passed
        try:
            log = pathlib.Path('/out/desktop/hmi.log').read_text()
            if not changed and 'following Orbiter around Kerbin' in log:
                combo = next(w for w in widgets(app.root)
                             if isinstance(w, ttk.Combobox) and w.cget('textvariable') == str(app.skin))
                combo.set('classic')
                combo.event_generate('<<ComboboxSelected>>')
                app.throttle_scale.set(37)
                changed = True
            elif changed and 'ksphmi.display: skin: classic (context' in log:
                import krpc
                conn = krpc.connect(address='127.0.0.1', rpc_port=50300, stream_port=50301)
                try:
                    throttle = conn.space_center.active_vessel.control.throttle
                finally:
                    conn.close()
                assert abs(throttle - 0.37) < 0.001, throttle
                # Check the same HTTP -> WebSocket -> VNC path used by the browser.
                with urllib.request.urlopen('http://127.0.0.1:6080/vnc.html', timeout=3) as response:
                    assert response.status == 200
                with socket.create_connection(('127.0.0.1', 6080), timeout=3) as sock:
                    sock.sendall(b'GET /websockify HTTP/1.1\r\nHost: localhost:6080\r\n'
                                 b'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                                 b'Sec-WebSocket-Key: MDEyMzQ1Njc4OWFiY2RlZg==\r\n'
                                 b'Sec-WebSocket-Version: 13\r\n\r\n')
                    stream = sock.makefile('rb')
                    assert b'101' in stream.readline()
                    while True:
                        line = stream.readline()
                        assert line, 'WebSocket closed before upgrade completed'
                        if line == b'\r\n':
                            break
                    header = stream.read(2)
                    assert header == b'\x82\x0c', header
                    assert stream.read(12).startswith(b'RFB ')
                passed = True
                result.write_text(json.dumps({'passed': True, 'throttle': throttle,
                                              'skin': 'classic', 'transport': 'kRPC + UDP', 'browser': True}))
                print('PASS: Tk skin -> C++ display (UDP), Tk throttle -> kRPC, noVNC HTTP/WebSocket -> VNC', flush=True)
                app.root.destroy()
                return
            if time.monotonic() > deadline:
                raise TimeoutError('GUI changes did not reach the C++ HMI and kRPC')
            app.root.after(250, poll)
        except Exception as exc:
            print(f'FAIL: {exc!r}', flush=True)
            app.root.destroy()

    app.root.after(250, poll)
    original_run(app)


App.run = run
main(['--no-board', '--address', '127.0.0.1', '--rpc-port', '50300',
      '--stream-port', '50301', '--skin', 'apollo'])
sys.exit(0 if passed else 1)
