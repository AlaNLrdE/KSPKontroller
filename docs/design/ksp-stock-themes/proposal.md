# KSP1 Stock and KSP2 Redux: theme proposals

Status: both concepts approved on 2026-11-16. The generated images remain design
references; GameSkin implements the two themes and renderer tests provide runtime evidence.
No deployment or game validation is implied by the concept images. These are two additional skins; existing
Classic, Apollo and Steampunk remain available.

## Approval renders

| Theme | Flight | Other scene families |
|---|---|---|
| KSP1 Stock | [Flight v2](ksp1-flight-v2.png) | [Scene board v2](ksp1-scenes-v2.png) |
| KSP2 Redux | [Flight v2](redux-flight-v2.png) | [Scene board v2](redux-scenes-v2.png) |

Example values and incidental labels in these concept images are illustrative; the existing
telemetry model remains authoritative for data bindings and enabled/connected states.
The earlier v1 iterations are retained for provenance. The v2 scene boards correct
non-flight context: no flight-only NavBall mode / SAS / RCS controls in the VAB,
no live altitude in an editor header, and no CONNECTED lamp during link search.

## KSP1 Stock

Translate the original KSP 1.x game interface into a dedicated telemetry console:
grey moulded window frames, restrained bevels, charcoal inset screens, green LCD
figures, compact sans typography, tactile SAS/RCS controls and a stock-like spherical
NavBall instrument housing. Teal category accents distinguish editor sections;
amber/red remain warning/error colours. This is distinct from Apollo's aerospace
mounting plates and seven-segment modules.

## KSP2 Redux

Use KSP2's flat dark slate/navy layers, off-white angular/technical numerals, blue-violet
category chips, electric-green active states, cyan secondary data, compact segmented
resource blocks and sparse industrial warning stripes. The NavBall uses the game's
modern dark housing, side arcs and angular readout boxes. Avoid invented holograms,
excessive glow or Apollo/Steampunk hardware ornament.

## Scene coverage

Both directions apply to every existing view, not only the flight screen:

| View family | KSP1 Stock | KSP2 Redux |
|---|---|---|
| Connecting / reconnecting | Stock framed connection panel and illustrated search scope | Dark status card, green/cyan search scope and clear reconnect state |
| Connected without vessel | Same family, explicit NOT IN FLIGHT state | Same family, explicit NOT IN FLIGHT state |
| Flight | Ten metrics, resources, history, staging, ORBIT, VESSEL and stock NavBall | Same actual telemetry, modern HUD instruments and segmented resources |
| VAB construction | Stock-like delta-v windows, stage capsules, destination lamps, vehicle figures | Trip-planner-like delta-v cards, stage cards, destination matrix, vehicle figures |
| Actions | Tactile grey tabs and group/assignment lists | Flat category chips and high-contrast selected group |
| Crew | Seat manifest and crew complement in stock windows | Seat manifest and crew complement in dark cards |
| Cargo | Stored-parts / inventory panels with stock list rows | Stored-parts / inventory panels with compact item rows |
| Eight facilities | Consistent title, numbered preparation checklist and connection status | Same existing context/checklist content in the Redux visual system |
| Stale / unavailable values | Explicit unknown/missing figures, amber/red annunciation | Explicit unknown/missing figures, amber/red status and reduced emphasis |

Facilities: Space Center, VAB without report, SPH, Mission Control, Astronaut Complex,
Research & Development, Administration and Tracking Station. These remain the app's
existing information/checklist screens; no new game data services are implied.

## Shared functional constraints

- Keep readable at native Pi resolutions and retain a full degree-marked NavBall,
  its pole-safe pitch ladder, boresight, live attitude and standard navigation markers.
- Body material follows telemetry across all 17 stock worlds and custom-body assets.
  Neither new theme inherits Apollo's green-CRT planet treatment.
- Preserve actual resource absent/unknown/empty distinctions and red/green meaning.
- Use existing orbital physics, staging information, editor report and keyboard rules.
  Staging and facility cards remain read-only.
- Skin selection remains independent from KSP1/Redux telemetry source selection.
- Bundle production assets and licensed fonts when implementing. Cache static frames,
  faces and typography; dynamic values/needles/plots remain native renderer content.
- Before acceptance, review the flight image and the scene-family board together.
  During implementation compare actual renderer screenshots in every scene to these
  approved references; generated mockups are never runtime evidence.

## Reference provenance

Public game screenshots were consulted for visual language only; they are not production
UI assets. Local development-only references are kept under build/design-references.

- KSP1 flight: https://forum.kerbalspaceprogram.com/topic/190382-15-110-kerbalism-311/page/60/
- KSP1 editor: https://steamcommunity.com/sharedfiles/filedetails/?id=2183742970
- KSP1 official game page: https://store.steampowered.com/app/220200/Kerbal_Space_Program/
- KSP2 flight: https://forum.kerbalspaceprogram.com/topic/212306-ksp-2-and-proton/
- KSP2 editor: https://store.steampowered.com/app/954850/Kerbal_Space_Program_2/

Concepts use the built-in image-generation tool, not a CLI/API fallback. Exact prompts
are saved beside the finished renders in prompts.json. No existing theme is modified.
