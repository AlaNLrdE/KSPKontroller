# KSP2 instrument artwork

This theme implements the approved concept in docs/design/ksp-stock-themes. Concept
renders are design references; review actual SDL screenshots for runtime evidence.

- navball-bezel.png: original generated empty ring, no globe, symbols or telemetry.
- navball-bezel.bmp: 768px runtime derivative loaded by SDL2, resampled with sips.
- generation.json: exact production prompt and built-in imagegen provenance.

The built-in image generation tool produced the bezel on 2026-11-16 using the approved
flight render as its style reference. No CLI/API fallback was used. The renderer maps
only the .40 to .485 UV annulus around the independently drawn navigation globe.
Source PNGs and runtime BMPs remain in the project and deployment package.

Window frames, raised/flat cards, title tabs, resource pictograms, bars and the Stock
odometer are native Canvas primitives. Static chrome is baked into one texture per
size/scene/editor mode/resource page. Values, warning states, resource fractions,
selection highlights, orbit and NavBall remain live. Generated images never fix them.

Typeface: ../fonts/ChakraPetch-SemiBold.ttf, distributed with its OFL license. Source: https://github.com/google/fonts/tree/main/ofl/chakrapetch.
Roboto Condensed is instantiated at weight 600 from the upstream variable font;
Chakra Petch uses the upstream static Semibold face. Fonts are scoped per draw, and
NavBall glyph caches include the active typeface.

Run an offscreen demo with --skin ksp2. KSPHMI_SHOTS set to an absolute directory
when running render tests captures waiting/no-vessel, all facilities, editor modes,
cautions and zenith attitude. These fixtures are not a game session. Skin choice is
independent of the KSP1/Redux telemetry source choice.
