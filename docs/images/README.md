# Screenshot sources

These are captures of the actual applications, taken from the Linux ARM64 Docker environment.
They show generated demo/simulator telemetry, not a recorded KSP game session or photographs of hardware.

| File | Source | Capture size |
| --- | --- | --- |
| `hmi-apollo.png` | C++ `ksphmi`, `DemoSource`, Apollo skin, off-screen screenshot output | 1680 x 1120 |
| `hmi-classic.png` | The same executable and demo configuration, Classic skin | 1680 x 1120 |
| `simulator-desktop.png` | X11 screen capture of the real Tk app and connected SDL HMI windows | 3000 x 1150 |

The PNGs have losslessly recompressed IDAT streams. Decoded scanlines were compared byte for byte
before and after recompression. There is no compositing, redrawing, color correction or scaling.
The desktop capture uses a screen rectangle that excludes the unused lower part of the virtual desktop.

## Regenerate the HMI captures

Start the Docker desktop and wait until the C++ build finishes:

```bash
./ci hmi_desktop
```

Then run the executable directly inside that container:

```bash
docker exec ksphmi-desktop /tmp/ksphmi/build/hmi-cpp/ksphmi \
  --config /tmp/ksphmi/hmi-cpp/config.toml --demo --skin apollo \
  --size 1680x1120 --seconds 1.5 --screenshot /out/readme-apollo.png

docker exec ksphmi-desktop /tmp/ksphmi/build/hmi-cpp/ksphmi \
  --config /tmp/ksphmi/hmi-cpp/config.toml --demo --skin classic \
  --size 1680x1120 --seconds 1.5 --screenshot /out/readme-classic.png
```

The original outputs are in `build/hmi-container/linux-arm64/` on the host (adjust for another
`HMI_PLATFORM`). The app uses the image's FreeSansBold font. Native captures can use
`./ci hmi --demo --skin apollo --size 1680x1120 --screenshot ...`, but macOS font fallback may differ.
Values can vary slightly with sample timing; do not alter them in an image editor.

## Regenerate the desktop capture

Open the browser desktop, select the orbit scenario and Apollo skin, open **Manual values**, and
enable **Manual mode** so the current values populate the fields. The Arduino remains disabled.
Arrange the windows as follows, preserving the HMI's original 1680 x 1120 client area:

```bash
docker exec -e DISPLAY=:1 ksphmi-desktop \
  wmctrl -r 'KSP simulator:' -e 0,0,0,1280,1120
docker exec -e DISPLAY=:1 ksphmi-desktop \
  wmctrl -r 'KSP HMI' -e 0,1300,0,1680,1120

docker exec -e DISPLAY=:1 ksphmi-desktop /usr/bin/python3 -c \
  'from PIL import ImageGrab; ImageGrab.grab(bbox=(0,0,3000,1150), xdisplay=":1").save("/out/desktop/readme-desktop.png")'
```

Pillow is supplied by the Debian desktop environment. The screenshot is exported under
`build/hmi-container/linux-arm64/desktop/`. Copy the reviewed captures here, optionally recompress
them losslessly, and keep filenames stable for the README. Review full-resolution images before
replacing them; check that neither window is cut off and that no personal information is visible.
