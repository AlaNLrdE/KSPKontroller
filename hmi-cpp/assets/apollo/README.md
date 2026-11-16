# Apollo Flight Deck materials

Approved direction (2026-11-16): proposal A's light painted aerospace panels and black
instrument modules, with proposal B's black/stainless NavBall bezel and local pitch ladder.
The original concept renders are in `docs/design/apollo-flight-deck/`. They are concept art,
not screenshots, orbital measurements or telemetry fixtures.

## Files and runtime composition

- `panel.png`: pale grey stippled painted aluminum, narrow machined silver perimeter and
  four flush steel screws. Nine-sliced for the mounting plates and header.
- `module.png`: charcoal anodized inset with stainless perimeter and four flush screws.
  Nine-sliced around metric cards, side instruments, ORBIT and VESSEL wells.
- `navball-bezel.png`: Mission Control style black/stainless annulus with engraved tick marks.
  A triangle strip maps only its annulus around the quaternion-driven globe.
- The colour planet source now lives at `../steampunk/kerbin.png` / `kerbin.bmp`, as requested
  on 2026-11-16. Apollo uses a cached green-phosphor conversion of that illustrated surface.
  `OrbitArt` converts the luminance once; normal frames sample a green texture, not a colour
  planet with a translucent green rectangle over it. Other bodies use green shaded spheres.
  The artwork is illustrative, not cartographic game terrain or a live cloud/lighting sensor.
- `*.bmp`: lossless opaque runtime conversions, loaded with SDL2 itself. On macOS regenerate
  each using `sips -s format bmp source.png --out runtime.bmp`.

Static panels, glass windows, scale markings and the bezel are baked once per screen size and
screen kind. The body texture is loaded once on the live renderer and released with the skin.
Glyph atlases are cached by face/size; the NavBall's atlas includes its face in its cache key.

`flightdeck.cpp` composes these materials with Barlow Condensed SemiBold (bundled under
`../fonts/` with its SIL Open Font License). Source:
https://github.com/google/fonts/tree/main/ofl/barlowcondensed

All digits, resource bars, staging rows, graphs, ship silhouettes, navigation markers, orbit
curves and apsides are drawn from current telemetry. No values, trajectory or rocket picture
are baked into the assets. ORBIT keeps the body's projected radius, inertial view axes and
Cartesian vessel marker; its material callback does not alter conics or world coordinates.
In Apollo, the entire ORBIT viewport uses a scoped green CRT palette: atmosphere, current
and dashed maneuver trajectories, apsides, node/vessel markers and every annotation. The
maneuver remains distinguishable by dashes and a lower phosphor intensity. Opaque under-strokes
provide vector glow, and a size-cached alpha mask supplies scanlines and edge vignette. No
per-frame image readback or full-image CPU colour conversion is needed. Material hooks, the
CRT palette and clipping are restored on every exit; other skins retain their own colours. Resources remain vessel totals in KSP units, never invented litres or kg.

The flight layout uses the Apollo profile of `flight_layout`: equal-width halves, ORBIT and
VESSEL in a 33u top row, NavBall below; the left half has ten metric modules in two rows,
horizontal resources, rolling history and read-only staging. The six NavBall side readouts
are speed, heading, pitch, roll, throttle and G-force. Flight warnings share the header's
priority caution window (stale data, surface/atmosphere periapsis, power, escape, fuel).

## Generation provenance / prompts

All four original PNG assets (including the planet now moved to Steampunk) were generated
with the built-in image generation tool on
2026-11-16, without CLI/API fallback. They were generated as individual reusable parts,
not obtained by cropping a screenshot or drawing fixed telemetry into an image.

Panel prompt: Single square aerospace mounting panel, realistic 1960s Apollo flight deck,
front-facing orthographic, light warm grey painted aluminum with fine stippled powder coat,
extremely thin machined silver perimeter, shallow bevel and recessed inner seam. Flush
slotted steel screw at each corner. Empty uniform light-grey centre, outer 8 percent contains
trim and corner 12 percent contains decoration for nine-slicing. No text, gauges, icons,
digits, perspective, background or ornament. Fully opaque.

Module prompt: Single square black instrument module, Apollo spacecraft, orthographic,
charcoal anodized aluminum with subtle fine grain, narrow satin steel perimeter and dark
inner seam, flush slotted screw at each corner. Centre 84 percent empty charcoal, no lighting
gradient; outer 6 percent contains rails and corner 12 percent contains decoration. No brass,
lettering, switches, screens or gauges. Uniform straight rails for nine-slicing. Opaque.

Bezel prompt: Single centered circular Apollo Mission Control instrument bezel, outer radius
48.5 percent and inner opening radius 37 percent of canvas width. Satin stainless outer lip,
charcoal black anodized broad ring, machined inner lip, diagonal slotted steel screws.
Engraved ivory radial ticks every 5 degrees and longer every 30 degrees. Pure black outside
and empty hole. No letters, digits, globe, needles, extra objects, perspective or brass.
Complete symmetric circle, tactile realistic hardware, restrained highlights. Opaque.

Planet prompt: Single circular Kerbin-like fictional habitable globe, orthographic and centred,
94 percent canvas diameter. Deep blue oceans, irregular green/tan fictional continents,
subtle relief, wispy clouds, thin atmospheric limb, upper-left sunlight and gently shaded
lower-right terminator. No recognizable Earth geography, stars, orbit lines, markers, text,
border, moon, flare or UI. Pure black background, no glow beyond the circle, complete disk.
Realistic crisp illustrated planet surface. Opaque.

## Review captures

`./ci hmi --demo --skin apollo --size 1920x1080 --seconds 3 --screenshot build/apollo-flight-deck.png`
creates an actual renderer capture using demo telemetry; it is not a game screenshot.
An absolute `KSPHMI_SHOTS` directory with `ci test_hmi` / ctest saves VAB, editor, facility,
waiting and caution fixture screenshots. Tests cover multiple sizes, local-history reset,
text bounds, static-art reuse, skin switches, non-Kerbin material selection, clipping and
pixel equality between full and cached NavBall frames.
