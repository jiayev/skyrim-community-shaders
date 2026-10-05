# Local cloud NDF assets

## Asset layout

Local clouds use optional, world-anchored NDF patches. They work with either the
procedural or imported global NDF. No cloud DDS is required for ordinary clouds.

```text
Data/Textures/PhysicalSky/
  LocalNdf/
    MyCloud/
      Height.dds
      Modeling.dds
      Mask.dds       (optional)
```

An asset ID is one folder name using ASCII letters, digits, `_` or `-`. It is not
a filesystem path. There is no per-asset manifest. Use distinct IDs ignoring case.
Assets can be reused by multiple instances, and can be installed as ordinary
texture mods. Scan Asset Folders discovers directories; Reload Cloud Textures
reloads selected inputs, retries failed loads, and rescans directories. Loading
failures show the path and HRESULT in Texture Inputs. A bad local pair skips that
instance; it never replaces the global procedural fallback with a null texture.

| Input          | Channels | Meaning                                              |
| -------------- | -------- | ---------------------------------------------------- |
| Height         | RG       | Bottom and top, normalized to 0–1                    |
| Modeling       | RGB      | Coverage, top profile type, bottom profile type      |
| Modeling       | A        | Influence mask by default; independent from coverage |
| Mask, optional | R        | Multiplier on the influence mask                     |

Height and Modeling must have matching dimensions and be linear, single 2D
textures. UNORM/float and suitable BC formats are accepted; sRGB, integer,
SNORM, arrays, cubemaps and volumes are rejected. Mask dimensions may differ;
it uses the same normalized UVs. BC5 Height and BC7 Modeling with full mip chains
are supported directly. RGB-only Modeling has alpha 1. Disable Use Modeling Alpha
as Mask if A contains unrelated data. An explicitly present but invalid Mask
rejects the instance instead of silently removing its influence constraint.

Empty and reversed height intervals are empty cloud, not automatically reordered.
Transparent influence leaves the underlying field unchanged. Zero coverage with
nonzero influence clears cloud coverage; coverage must not be reused as a mask.
No vertical flip is performed on NDF input. UV (0,0) is the negative local X/Y
corner, and positive rotation turns local +X toward world +Y.

## Placement and persistence

Physical Sky > Cloud Map > Local Cloud Instances provides creation, removal,
asset selection, a worldspace Editor ID, Move to Camera, X/Y position, dimensions,
rotation, altitude, height span, strength, independent modeling/height influence,
edge feather and priority. The older Global Generator Overlay remains a tileable
generator input and is separate from world placement. A newly added instance belongs to the current worldspace.
Empty or different worldspaces are inactive. Coordinates are in metres, converted
from game units without camera-relative anchoring. Atlas sampling clamps and never
repeats the patch outside its finite footprint.

Height is decoded as `altitude + Height.RG * heightSpan`, relative to the same
atmosphere ground datum as ordinary clouds. It does not inherit the global NDF
altitude scale. For an authoring convention centered at 0.5, set
`altitude = desiredCenterHeight - heightSpan / 2`. The texture cannot tell the
renderer its intended physical scale or datum; these belong to the instance.
Edge feather is measured inward from the rotated rectangle, in metres.

Instances have stable IDs; their editable display names do not define identity.
All properties live in `Physical Sky.cloudMap.instances` in the existing settings
serialization. Use the existing settings Save action to persist a layout. This is
configuration persistence, not per-savegame storage. No additional JSON format is
introduced. Texture bytes remain external assets.

`PhysicalSky::SetLocalCloudInstances(vector<LocalNdfInstance>)` replaces the layout
on the render/settings thread. It rejects missing or duplicate IDs, empty
worldspace IDs and malformed asset IDs. The next feature Reset resolves resources,
composes the field and publishes its bounds before shared sky constants are built.
File loading and rendering must not be called from a background controller thread.
An empty vector clears the layout. Missing assets are reported and skipped while
valid instances remain usable. This API does not animate a weather transition.

## Composition and renderer contract

Active patches are sorted by ascending priority, then their saved vector order.
Higher priorities are applied last. The fixed finite atlas encloses all active
rotated rectangles in the current worldspace, with transparent guard texels. It
is independent of the camera, global weather repeat scale, wind and frame index.
It rebuilds only after sanitized placement, inputs, source revision or shader
changes. Renaming an instance alone does not change cloud density.

For a source mask `a` including alpha, optional Mask and edge feather:

```text
wm = a * strength * modelingWeight
wh = a * strength * heightWeight
model.rgb = source.rgb * wm + previous.rgb * (1 - wm)
model.a   = wm + previous.a * (1 - wm)
height.rg = normalizedAbsoluteHeight * wh + previous.rg * (1 - wh)
height.b  = wh + previous.b * (1 - wh)
height.a  = shapingStart * wm + previous.a * (1 - wm)
```

Atlas heights are normalized to a published local altitude interval; they are not
kilometres or game units stored in half precision. The modeling atlas is
premultiplied by influence, independently from height. At density sampling, height
RG is decoded to absolute altitude and blended with the global field using B.
Model RGB blends with the global field using A. The internal shaping-start channel
uses the same top-type remap as the ordinary generator. No extra cellular noise,
storm animation or local weather controller is introduced.

Both atlases are RGBA16_FLOAT, with SRV ping-pong for composition. There are no
RGBA16 typed UAV reads, which are not guaranteed by the baseline D3D11 contract.
Source mips follow the atlas footprint; the resulting atlases use bilinear mip 0.
Requested texel size defaults to 32 m. The maximum dimension is 2048, so very large
or widely separated layouts increase the effective texel size; Debug reports it.
Four textures, including two composition scratch textures, cost at most 128 MiB.
No instance loop is added to each ray sample: within the finite atlas, density
uses two additional 2D reads regardless of the number of instances.

Main view, cubemap and shadow volume use the same composed maps and interval.
The shared ground-shadow constants use the same interval as volume generation.
Cubemap height reduction continues to constrain the global field, conservatively
including local heights for rays that intersect the finite atlas in XY. Boundary
predraw expands neighboring grid cells to the union of global and local height envelopes so a
small patch cannot fall between grid vertices. This is conservative acceleration,
not a second density definition; large sparse layouts can reduce its effectiveness.

Local macro shape stays anchored while detail noise can still evolve. Temporal
motion reduces global advection according to local modeling influence and counts
the competing detail motion as reconstruction uncertainty. Editing/reloading an
instance invalidates main and cube history through their existing shared validity
flag. This is a project approximation for mixed anchored/advection fields, not an
exact velocity decomposition of superimposed densities.

## Debug and validation

Debug > Cloud Shape > Local NDF Resources shows both composed atlases, the active
instance count, effective texel size, world origin and altitude interval, plus
source Height, Modeling/alpha and optional Mask. Existing per-channel, display
range and mip controls apply. English and Simplified Chinese labels are provided.

An inspection/staging tool is available:

```powershell
python tools/validate-local-ndf.py HeightInput.dds ModelingInput.dds --output build/local-ndf-check --stage-root build/local-ndf-test/Data/Textures/PhysicalSky/LocalNdf/MyCloud
```

It validates headers, formats, dimensions and mip payload sizes, optionally writes
channel previews using Pillow/NumPy, and stages byte-identical resources with the
required names. It does not execute the renderer or compile shaders. Legacy
uncompressed DDS can be loaded by the runtime but is not decoded by this tool's
header validator; use DX10 headers for automated inspection.

The supplied acceptance pair was validated as 512², ten mips, BC5_UNORM Height and
BC7_UNORM Modeling. Its alpha footprint covers about 33.27% of the image at the
0.01 threshold; no reversed or empty height interval was found inside that
footprint after inspection decoding. The staged fixture is under the ignored
`build/local-ndf-fixture/Data/Textures/PhysicalSky/LocalNdf/StormBackTest` directory.
Reference assets are not added to the distributed feature. The channel sheet and
text report are in `build/local-ndf-fixture/inspection`.

In-game acceptance still needs: no-texture startup; loading the fixture; moving
and rotating it with wind on/off; overlapping two priorities; zero alpha/strength;
leaving/reentering the worldspace; saving/reloading settings; replacing/removing
DDS files and reloading; and agreement between main view, cubemap, ground shadows
and boundary predraw above/below the global height range. Static and CPU checks
cannot establish GPU output or frame time. See [transition design](local-ndf-transitions.md)
for the deferred weather-controller integration.
