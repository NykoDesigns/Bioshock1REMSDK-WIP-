# Level Editor — Roadmap & Breakthroughs

## Status: Functional Map Viewer with Partial Texture Coverage

The BS1 Level Editor renders BioShock 1 Remastered `.bsm` maps with BSP geometry,
StaticMeshes, point lights, lightmap support, and atmospheric rendering. It provides
a usable 3D preview of any BioShock level for modding and analysis.

---

## Recent Breakthroughs (June 2026)

### 1. UE2.5 Source Code Integration

Added the complete Unreal Engine 2.5 source tree (UT2003 v2107 + UT2004 "Unreal Warfare" v09-29-2007)
to the project for cross-reference during reverse engineering:

- `unreal-src-part1/` — UT2003 Retail (Editor, Engine, Core, Fire, IpDrv, UWeb, XInterface)
- `unreal-src-part-two/` — UT2004 Warfare (full Engine/Inc + Engine/Src + Core + more)

**Key findings from source cross-reference:**

| Source File | Finding | Impact |
|-------------|---------|--------|
| `UnTex.h` | `FMipmap` uses `TLazyArray<BYTE>` serialization | Validated our mip parsing |
| `UnTex.cpp` | `UTexture::Serialize` mip loop matches our wire format exactly | Confirmed no missing fields |
| `UnTemplate.h` | `TLazyArray` for Ver>61: `INT32 SeekPos` then `CI Count + Data[Count]` | Validated bulk loading |
| `UnRenderResource.h` | `ETextureFormat` enum ordinals (DXT1=3, DXT3=7, DXT5=8, 3DC=12) | Confirmed format mapping |
| `UnRender.h` / FBspNode | 64B stock vs our confirmed 100B Vengeance layout | Proved ZoneMask expansion |
| `BrushBuilder.uc` | Editor brush class structure | Reference for future brush tools |

### 2. FBspSurf Serialization — Fully Decoded

Confirmed via UT2004 Engine.dll disassembly at RVA 0x001C1850:

- PanU/PanV are **SKIPPED** for package version >= 78 (BioShock = v141)
- Pan offsets are baked into `pBase` (the texture origin point index)
- The 20B block after Actor CI = FPlane(16B surface normal+dist) + LightMapScale(4B)
- Actor ref is a CompactIndex (NOT fixed INT32) — validated on all 16 maps

### 3. BSM Shader/Material Export Parsing

New parser extracts texture mappings directly from BSM binary Shader exports:

- **767 Shader/Material/FinalBlend exports** parsed per map
- **621 with Diffuse texture references** resolved via object reference chain
- Indirection following: Shader→Modifier→TexPanner→Texture (up to 5 deep)
- Also extracts NormalMap references from Shader exports
- Supplements `.mat`/`.props.txt` file-based resolution from UEViewer exports

### 4. Bulk Texture Pipeline — Complete

End-to-end texture loading from `.blk` bulk files without UEViewer dependency:

1. Parse `UTexture` exports from BSM → extract Format + mip0 dimensions
2. Parse `Catalog.bdc` → map texture names to `.blk` file offsets
3. Read raw DXT data from `.blk` → upload to GPU via `glCompressedTexImage2D`
4. Re-link meshes after bulk load to bind newly available textures

Supports: DXT1, DXT3, DXT5, BC5 (3DC) formats.

### 5. Actor Filtering — Phantom Geometry Eliminated

Identified and fixed three sources of phantom/black geometry:

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| Objects at world origin | 22,477 actors without `Location` property rendered at (0,0,0) | Skip `!hasLocation` in render loop |
| Large black quads | `VisibleEmitter` actors (particle billboards) rendered as solid meshes | Skip emitter render type |
| Solid decal volumes | `VisibleDecal` actors (projection volumes) rendered as opaque geometry | Skip decal render type |
| Wireframes at origin | Decal projectors without location rendering their frustum at origin | Skip `!hasLocation` in projector loop |

### 6. Multi-Material Sub-Mesh Inheritance

UEViewer splits multi-material StaticMeshes into `base_name`, `base_name_2`, `base_name_3`.
The sub-meshes now inherit texture from the parent mesh when no material is found.

---

## Current Capabilities

| Feature | Status | Notes |
|---------|--------|-------|
| BSP geometry | ✅ 361/379 chunks textured | Only water cubemaps unresolved |
| StaticMeshes | ✅ 605/814 textured | 209 untextured (missing TGAs from other packages) |
| Normal mapping | ✅ 608 meshes + 354 BSP chunks | From .mat/.props.txt + BSM shader parsing |
| Specular mapping | ✅ 597 meshes + 87 BSP chunks | |
| Point lights | ✅ 128 lights from BSM actors | Configurable multiplier |
| Shadow mapping | ✅ Sun/directional light | PCF soft shadows |
| Zone visibility | ✅ BSP tree traversal | ZoneMask bit testing |
| Lightmap atlases | ⚠️ Loaded from bulk | Binding to BSP surfaces incomplete |
| Atmospheric fog | ✅ Blue-green depth fog | Configurable range/color |
| FXAA/SSAO/Bloom | ✅ Post-processing pipeline | Configurable per-effect |
| Actor picking | ✅ Click-to-select | Properties panel |
| Content browser | ✅ Texture/mesh thumbnails | |
| Game preview mode | ✅ Hide editor-only actors | |

---

## TODO — Remaining Work (Priority Order)

### Priority 1: Export Missing Textures (HIGH impact, LOW effort)

203 meshes have resolved texture names but no TGA file on disk. These textures
exist in other BSM packages (weapons, decorations, effects) that weren't exported
by UEViewer for the current map.

**Action:** Batch export textures from source packages:
- `Shared_Art.bsm` — common textures (railings, metal, glass)
- `SP_MedicalPavilion_Art.bsm` — level-specific props
- Weapon packages — `WP_AI_Pistol`, `tommygun_ammo`, etc.
- Effect packages — `Light_Beams`, `Cam_Beam`, `Flowers`, etc.

**Alternative:** Parse ALL `.bsm` files to build a global UTexture metadata table,
then load any missing texture from bulk on demand.

### Priority 2: Lightmap Binding to BSP (HIGH impact, MEDIUM effort)

Lightmap atlases are loaded (14 DXT1 1024x1024 textures for Medical) but not yet
applied to BSP surfaces. The data exists:

- `FLightMapIndex` per surface: `iSurf`, `SizeX`, `SizeY`, `WorldToLightMap` matrix
- `FLightMapLight` per baked light: `iAtlas`, `TileX`, `TileY`
- UV formula: `UV' = WorldToLightMap × worldPos`, scaled by `(Size/1024)` + offset `(Tile+0.5)/1024`
- The shader already has `uHasLightMap` and `uLightMapTex` uniforms ready

**Action:** In the BSP triangulation pass, compute lightmap UVs per vertex using
the `WorldToLightMap` matrix and `TileX/TileY` offset, bind the correct atlas
texture per chunk.

### Priority 3: Transparency / Alpha Blending (MEDIUM impact, MEDIUM effort)

Glass panels, windows, and alpha-tested foliage render opaque or are culled.

**Action:**
1. Detect alpha materials (glass, foliage, FacingShader textures)
2. Separate into an alpha-blend render pass (back-to-front sorted)
3. Use `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`

### Priority 4: Decal Projection (MEDIUM impact, MEDIUM effort)

Posters, blood splatters, graffiti (e.g., Steinman's "BEAUTY" ads) are projected
textures that should appear on nearby BSP/mesh surfaces.

**Action:**
1. Parse `Projector` actor properties: FOV, MaxDistance, ProjectTexture
2. For each projector, find BSP/mesh triangles within its frustum
3. Project texture UVs using the projector's view matrix
4. Render as alpha-blended overlay

### Priority 5: Bloom / Emissive Post-Process (MEDIUM impact, MEDIUM effort)

Neon signs ("Medical Pavilion"), lamp glows, light beams need self-illumination + bloom.
Currently detected heuristically by mesh name keywords.

**Action:**
1. Render emissive meshes to a separate brightness buffer
2. Threshold → Gaussian blur (2-pass separable, 5 mip levels)
3. Additive composite back to main framebuffer

### Priority 6: Per-Zone Fog (LOW impact, LOW effort)

BioShock has per-zone fog with different colors/densities. Currently we have a single
global linear fog.

**Action:** Parse `ZoneInfo` actor fog properties (`bDistanceFog`, `DistanceFogColor`,
`DistanceFogStart`, `DistanceFogEnd`), apply camera-zone fog in fragment shader.

### Priority 7: Skeletal Meshes (MEDIUM impact, HIGH effort)

NPC bodies, chains, ropes, animated props use `SkeletalMesh` exports (bone hierarchy
+ vertex weights). Currently only StaticMeshes are rendered.

**Action:**
1. Parse `SkeletalMesh` export wire format (bones, LODs, vertex influences)
2. Render in bind/reference pose (no animation needed for editor)
3. Link to actors via `Mesh` property reference

### Priority 8: Particle Effects (LOW impact, HIGH effort)

Dust motes, steam vents, sparks, underwater bubbles use `Emitter` actors with
sprite/beam/mesh particle sub-types.

**Action:**
1. Parse `SpriteEmitter`/`BeamEmitter`/`MeshEmitter` properties
2. Implement billboard particle rendering (quad facing camera)
3. Spawn initial burst for static preview

---

## Architecture Notes

### Texture Resolution Priority Chain

```
1. Direct TGA match (UEViewer export)
2. .mat/.props.txt Diffuse= mapping (UEViewer shader exports)
3. BSM binary Shader export → Diffuse property → Texture name (new)
4. Bulk texture metadata (loadable from .blk without TGA)
5. _N suffix mesh → inherit from parent mesh (new)
```

### Actor Classification (`ActorRenderType`)

| Type | Rendered As | Notes |
|------|-------------|-------|
| `VisibleStaticMesh` | Solid textured mesh | Main architectural geometry |
| `VisibleDecal` | SKIPPED | Projection volumes, not solid geometry |
| `VisibleEmitter` | SKIPPED | Particle billboards, not solid geometry |
| `LightOnly` | Point light sphere | Contributes to scene lighting |
| `EditorOnly` | Billboard icon | Triggers, sounds, paths — hidden in game preview |

### BSP Parsing Pipeline

```
UModel export → 8B Vengeance header → Skip tagged properties
  → FBox(25B) + FSphere(16B) → 8B class header (check=4, sv=7)
  → Vectors[] → Points[] → Nodes[] (100B each)
  → Surfs[] (variable, CI-encoded refs) → FVert[] (8B each)
  → FLightMapIndex[] → FLightMapLight[] → FLightMapTexture[]
  → Triangulate by (material, zone, atlas) → Upload to GPU
```

---

## Files Modified (Level Editor)

| File | Purpose |
|------|---------|
| `bsm_document.h` | TextureMetadata struct, BSM shader map accessors |
| `bsm_document.cpp` | UTexture parsing, Shader export parsing, material resolution |
| `viewport.cpp` | Actor filtering, shadow map fixes, RelinkMeshTextures |
| `viewport.h` | LightingSettings defaults, RelinkMeshTextures declaration |
| `app.cpp` | Bulk texture loading, mesh re-linking after bulk load |
| `bsp_parser.cpp` | BSP geometry with zone visibility, lightmap descriptors |
| `texture_loader.cpp` | Cross-map TGA search with suffix variants |
| `mesh_parser.h` | ParsedMesh struct with material/texture fields |

---

## Known Limitations

- **19 meshes with no material at all** — UEViewer multi-material splits where parent also lacks material
- **Water surfaces** render as untextured (need cubemap environment mapping)
- **No undo/redo** for actor operations
- **No BSM write-back** — editor is currently read-only for geometry
- **Single-map at a time** — no streaming sublevel support
