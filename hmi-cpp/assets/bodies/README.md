# Shared celestial-body materials

All three ORBIT themes use this catalog, selected by the body's telemetry name.
Classic and Steampunk show colour. Apollo converts the selected world's material to
cached green CRT phosphor; it does not substitute a generic green world.

## Coverage and identity

The 17 original-system bodies are Sun, Moho, Eve, Gilly, Kerbin, Mun, Minmus, Duna,
Ike, Dres, Jool, Laythe, Vall, Tylo, Bop, Pol and Eeloo. `Kerbol` aliases `Sun` and
`The Mun` aliases `Mun`; matching is case-insensitive. Each world has a distinct
illustrated surface, including Duna's rusty terrain and polar caps, Jool's green
bands, Eve's purple surface/seas and the individual moons' rocky/icy materials.

These are generated visual interpretations, not extracted game textures, exact
cartographic maps or live terrain/cloud/lighting measurements. They use the familiar
original KSP-system appearance, not mod-specific or sequel-specific terrain changes.
Original-system reference: [KSPedia](https://static-files.kerbalspaceprogram.com/pd/KSPedia-PS4.pdf).

## Sources and generation

The built-in image generation tool generated one independent production image per
new body on 2026-11-16. No CLI/API fallback was used. Exact prompts for the 16 new
sources are in `prompts.json`. Kerbin retains the earlier source, copied unchanged
from `../steampunk/kerbin.png`; its original prompt is in `../apollo/README.md`.

- `<key>.png` preserves the original generated source image.
- `<key>.bmp` is a 512 x 512 opaque runtime derivative, resampled with `sips` then
  saved as lossless BMP. SDL2 loads it without another runtime dependency:

  ```sh
  sips -z 512 512 -s format bmp duna.png --out duna.bmp
  ```

All original PNGs and runtime BMPs are kept in the project and shipped with `assets/`.
The triangle fan samples the centered circular material at UV radius .466; the
projected centre/radius still come from the actual orbital geometry. Atmosphere extent
is telemetry-driven; colour themes use rusty/violet/green atmospheric material for
Duna/Eve/Jool, while Apollo keeps green phosphor throughout. Irregular-moon
relief is illustrative inside that projection, not a replacement for collision or
altitude geometry. Static lighting remains artwork rather than current solar angle.

## Cache and custom worlds

`OrbitArt` caches one most-recent body texture per colour/CRT mode. A body-name change
replaces that mode's texture; returning to a previous world reloads its own material.
This bounds memory even during a tour of the entire system. No texture decoding or
phosphor conversion occurs on unchanged telemetry frames.

For a custom system, add `<key>.bmp` here: lowercase ASCII body name, spaces replaced
by underscores; letters, digits, underscores and hyphens only. For example telemetry
`Custom World` uses `custom_world.bmp`. Unknown bodies without an asset receive neutral
shading, never Kerbin's geography. Invalid/path-traversal names are rejected. Missing
stock assets raise a clear packaging error instead of silently showing another world.

## Review evidence

The render tests exercise every catalog entry in colour and CRT, aliases, distinct
surface fingerprints and Kerbin -> Duna -> Kerbin cache replacement. They also draw
flight ORBIT in each theme across Kerbin, Duna, Jool and Mun. Set an absolute
`KSPHMI_BODY_SHOTS` directory when running `ksphmi_render_tests` to save those complete
fixture-driven application frames. These are renderer evidence, not a game session.
