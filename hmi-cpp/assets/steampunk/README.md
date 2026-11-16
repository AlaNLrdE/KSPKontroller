# Steampunk instrument artwork

These are production UI assets, not screenshots of the running application.
The original approved concept is visual guidance; data and instrument markings are drawn live.

- `panel.png`: original generated brass-and-iron frame. Its four corners and rails are
  nine-sliced by `SteampunkArt::panel`, preserving circular screws at every aspect ratio.
- `navball-bezel.png`: original generated circular brass instrument bezel. Its annulus is
  mapped onto the NavBall's reserved bezel area; no black rectangular background is copied.
- `*.bmp`: lossless, opaque runtime copies, loaded with SDL2's built-in BMP reader.
  The Pi needs no image-decoding library. On macOS regenerate with
  `sips -s format bmp panel.png --out panel.bmp` (and the equivalent for the bezel).

Both source assets were generated with the built-in image generation tool on 2026-11-16.
No CLI/API fallback was used. They are baked into a per-screen static texture; no image
loading, nine-slicing, font file loading or texture generation happens on normal telemetry frames.
All files under `assets/` must travel with `config.toml`, as they do in `ci deploy_hmi`.

## Reference revision: convex glass and copper instruments

The three user-supplied dashboard references informed the new concept at
`docs/design/steampunk-v2/concept.png`: black/petrol enamel, aged copper housings,
teal convex-glass highlights, amber jewel lamps, analog needles and glass sight tubes.
The concept is a design illustration, not runtime evidence. Cobwebs and heavy dirt were
excluded from measurement areas so the instruments remain readable.

The built-in image generation tool produced that concept and these four new production
assets on 2026-11-16, using the concept as their visual reference. No CLI/API fallback
was used. Original v1 artwork is retained for provenance; it is no longer the Steampunk
panel/bezel selected at runtime.

- `panel-v2.png` / `.bmp`: black-petrol panel, copper pipe rails, couplers, bronze corner
  filigree and screws. Nine-slicing preserves the outer 16% corner squares.
- `dial-face.png` / `.bmp`: empty convex glass and layered bronze housing. The renderer
  maps a circular texture fan with UV radius .485, then draws calibrated scales and live
  gold needles. Labels, ticks, needle, hub and numerical values are not baked into the image.
- `navball-bezel-v2.png` / `.bmp`: ornate blackened-bronze annulus, teal reflections and
  amber inner lip. Only UV radii .337 through .488 are drawn around the live globe.
- `sight-tube.png` / `.bmp`: empty horizontal glass tube with bronze end fittings.
  A three-slice crop (source y=19%, height=57%, 20% end caps) protects the fittings while
  the separate inner rectangle displays the live resource fraction.

### Revision generation briefs

Concept: Front-facing flight HMI inspired by the three supplied steampunk dashboards.
Dark petrol enamel, realistic aged copper/bronze rails and filigree, convex teal glass,
legible gold instrument scales and needles, warm amber jewel lamps. Ten round flight
gauges in two rows of five at left, glass resource tubes, history and staging below;
colour planet/orbit and vessel diagram above an ornate bronze NavBall on the right.
Keep the composition functional and readable, with rich material depth rather than a
flat recolour. Avoid cobwebs over data.

Dial face: Single perfectly front-facing circular instrument, centered on a square opaque
black canvas. Deep petrol empty face under convex glass with cool teal reflections,
layered aged copper and bronze housing, screw fasteners and an amber inner rim.
Complete circle, outer radius approximately 48.5% of the image, empty inner face about
35%. Match the reference concept. No ticks, numbers, letters, needle, hub or telemetry.

Panel v2: Single square orthographic empty black-petrol instrument mounting panel.
Copper pipe perimeter rails with couplers, aged bronze corner filigree and screw heads;
straight uniform edges and quiet dark enamel center. Restrict ornament to outer rails
and 16% corner squares for nine-slicing. Match the concept's realistic aged metal and
teal/amber lighting. No text, gauges, controls or baked telemetry.

NavBall bezel v2: One perfectly centered front-facing circular blackened-bronze bezel.
Broad layered ring with radial screws, Victorian filigree, knurled edge, cool teal glints
and warm amber inner lip. Pure black hole and exterior, complete circle on square opaque
canvas; inner radius about 34%, outer about 48.8%. Match the reference concept. No globe,
dial, needle, text or data.

Sight tube: Single horizontal empty black-glass resource sight tube, about 3:1 proportions,
front-facing, bronze end fittings and curved copper connectors. Cyan upper glass
reflection, amber lower reflection, subtle unnumbered graduations. Match the reference
concept. Opaque black background with generous vertical margins. No fill, numbers, labels
or telemetry. Keep end fittings separate from a straight stretchable central glass span.

## Colour planet in ORBIT

The textured colour planet originally shown in the Apollo proposal was moved here on
2026-11-16: `kerbin.png` is the original illustrated source and `kerbin.bmp` its lossless
runtime copy. Steampunk's `OrbitArt` draws this full-colour material at the body's actual
projected centre/radius; conics, inertial view axes and Cartesian markers remain telemetry.
The body name sits above the plot instead of obscuring the globe. Other bodies use their
own shaded colours and never reuse Kerbin geography. This image is illustrative, not a
cartographic map or a live terrain/cloud sensor. Its original generation prompt is recorded
in `../apollo/README.md`. Apollo shares the source only to create a separately cached green
CRT material and never applies its CRT palette to Steampunk.

## Generation prompts

Panel: A single square steampunk instrument frame, perfectly front-facing orthographic,
symmetrical, edge-to-edge. Rich aged brass double moulding, machined bevels, polished
upper-left highlights, dark copper recesses, subtle wear and patina, slotted screw at each
corner. Empty charcoal iron interior with subtle grain. Frame restricted to outer 8 percent,
all decoration restricted to corner squares of 12 percent for nine-slicing. Straight uniform
rails, gently chamfered corners. Victorian scientific observatory craftsmanship. No lettering,
numbers, symbols, gauges, controls, gears, or exterior shadows. Fully opaque.

Bezel: A single centered circular brass instrument bezel, straight-on orthographic on square
canvas, outer radius 48 percent and inner hole radius 39 percent. Aged golden brass, double
raised concentric mouldings, radial engraving, realistic highlights, slotted screws at the four
diagonals. Pure black outside and center. Empty center, no dial face, glass, needles, lettering,
numbers or symbols. Refined Victorian observatory craftsmanship, copper recesses, restrained
patina, complete circle visible. Fully opaque. The renderer uses the actual generated annulus
(33.7 to 48.8 percent radii) rather than assuming that the requested hole size was exact.

## Typefaces

`../fonts/Cinzel.ttf` is the 600-weight instance for engraved headings; `CrimsonPro.ttf` is
the 600-weight instance for live figures and supporting text. Both are bundled with their
SIL Open Font License files. Original sources:

- https://github.com/google/fonts/tree/main/ofl/cinzel
- https://github.com/google/fonts/tree/main/ofl/crimsonpro

The variable source fonts were downloaded as `Cinzel[wght].ttf` and `CrimsonPro[wght].ttf`.
To instantiate locally with the development-only fontTools package:

```python
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
font = TTFont("source.ttf")
instantiateVariableFont(font, {"wght": 600}, inplace=True).save("runtime.ttf")
```

The runtime uses SDL_ttf and has no fontTools dependency. Font atlases are keyed by typeface,
size and weight, so switching to Classic or Apollo restores their original typography.

## Review actual rendering

Run `./ci hmi --demo --skin steampunk --size 1920x1080 --seconds 3 --screenshot build/steampunk-v2.png`.
The result is demo telemetry, not a game capture. `KSPHMI_SHOTS` must be an absolute directory
when invoking ctest to save fixture-driven editor and VAB screens. History collects actual
received altitude and speed samples (up to 180 seconds), resets on time rewind or a different
vessel/body, and shows a collecting message before two samples exist.
