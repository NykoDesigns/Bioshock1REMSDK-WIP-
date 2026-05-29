# BioShock 1 Level Editor (BS1LevelEditor)

A standalone 3D level editor that loads BioShock 1's `.bsm` map files and renders the complete level geometry with full lighting, textures, and post-processing. Built on SDL2 + OpenGL 3.3 + Dear ImGui (docking branch).

## Build System

- **CMake 3.20+**, C++17
- Dependencies fetched automatically via `FetchContent`:
  - **SDL2** (2.28.5) — windowing, input, GL context
  - **Dear ImGui** (docking branch) — all UI panels
- Links: `opengl32`, `SDL2-static`, `imgui_lib`
- Targets:
  - `BS1LevelEditor` — full GUI application
  - `ParseTest` — CLI-only BSM parsing test (no SDL/GUI)

```
cd tools/level_editor
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Or from repo root using the workspace build:
```
cmake --build build_editor --config Release --target BS1LevelEditor
```

---

## Architecture Overview

```
main.cpp          → Entry point, creates App
app.cpp/h         → Main application loop, ImGui panels, input routing, file I/O
viewport.cpp/h    → OpenGL 3D renderer, shaders, post-processing, gizmo
camera.cpp/h      → Orbit + FPS fly camera, projection matrices
bsm_document.cpp/h → BSM file loader (actors, meshes, BSP, properties)
bsp_parser.cpp/h  → BSP geometry extraction (nodes, surfs, verts → triangulated meshes)
mesh_parser.cpp/h → StaticMesh binary parser (UE2.5 Vengeance format)
gltf_loader.cpp   → Fallback: load pre-exported glTF meshes from UEViewer
texture_loader.cpp/h → TGA texture loading + caching (diffuse, normal, specular)
content_browser.cpp/h → Asset browser panel (lists available meshes)
scene_tree.cpp/h  → Hierarchical actor list with category filters
properties_panel.cpp/h → Selected actor property editor (location, rotation, scale)
gl_funcs.cpp/h    → OpenGL function loader (no GLEW dependency)
parse_test.cpp    → Standalone parsing validation tool
```

---

## Data Pipeline

### Input Files
- **`.bsm`** — BioShock's cooked map package (Vengeance/UE2.5 format, Epic v141, Licensee v56)
  - Contains: actors, StaticMeshes, BSP geometry (UModel), materials, textures (stripped)
  - Compressed with zlib chunks
- **UEViewer export directory** — Pre-exported textures (TGA) and optional glTF meshes
  - Expected layout: `<exportDir>/<mapName>/Texture/*.tga`
  - Normal maps: `*_Normal.tga`, Specular maps: `*_Specular.tga`

### Parsing Pipeline
1. **BSMDocument::Load()** — Decompresses zlib, parses package header, name table, import/export tables
2. **Actor extraction** — Reads tagged properties (Location, Rotation, DrawScale, StaticMesh ref, Light properties)
3. **StaticMesh parsing** — 48B vertex stride (pos+normal+tangent), separate UV streams, uint16 indices
4. **BSP geometry** — UModel export: Vectors → Points → Nodes (100B each) → Surfs (variable) → Verts (8B each)
5. **glTF fallback loading** — Loads pre-exported meshes from UEViewer export dir (93 supplementary on 1-Medical)
6. **Actor-to-mesh linking** (4-pass):
   - Pass 1: Direct export ref (positive index → UStaticMesh export)
   - Pass 2: Import mesh name matching (normalized: strip underscores + lowercase)
   - Pass 3: StaticMeshInstance resolution (outer chain follow + binary scan fallback)
   - Pass 4: Class-default mesh mapping (gameplay classes → known placeholder meshes)
7. **Texture resolution** — Maps material names to TGA files via UEViewer export directory
8. **GPU upload** — VAO/VBO/IBO per mesh chunk, textures to GL_TEXTURE_2D

### Actor-to-Mesh Class Defaults
Gameplay actors without per-instance mesh refs use class-default placeholders:
| Actor Class | Placeholder Mesh | Real Mesh (in BulkContent) |
|-------------|-----------------|---------------------------|
| PlaceableVendingStation | ResStationBody | VendingWide |
| PlaceableHealthStation | ResStationBody | Broken_Health |
| SecurityCameraSpawner | SmCamWallBase | — |
| TurretSpawner | Turret_Cover | — |
| FlowerVaseContainer | flower_vase | — |
| MedHypoPickup | Health | — |
| EVEHypoPickup | eve_hypo_ad | — |

**Local class resolution fix:** When `classIdx > 0` in export table, resolve class name from `exports[classIdx-1].objectName`. Without this, all locally-defined classes collapse into "ExportClass".

---

## BSP Parsing (bsp_parser.cpp)

Fully reverse-engineered BioShock's Vengeance-engine BSP format:

- **FBspNode** (100 bytes): FPlane(16B) + ZoneMask(16B) + iVertPool(INT32) + iSurf(INT32) + children + bounds + zones + NumVertices(BYTE at +78)
- **FBspSurf** (variable): 8B Vengeance header + CI Material + 24B fixed fields (PolyFlags, pBase, vNormal, vTextureU, vTextureV, iBrushPoly) + CI Actor + 20B (FPlane + LightMapScale)
- **FVert** (8 bytes): INT32 pVertex + INT32 iSide
- **Zone visibility**: 128-bit bitmask per node, camera zone detection via BSP tree traversal

Key stats (1-Medical.bsm): 7125 nodes (0 planarity failures), 3386 surfs (0 bad Vengeance headers), 590 vectors, 11652 points → 29726 vertices, 17706 triangles in 379 material chunks

Total meshes: 569 inline UStaticMesh + 93 glTF fallback = 662 unique meshes. 4,155 actors linked to mesh geometry.

---

## Rendering Pipeline (viewport.cpp)

### Fragment Shader Features
All in a single uber-shader (`fragSrc`):

| Feature | Implementation |
|---------|---------------|
| **PBR-lite lighting** | Directional sun + fill + hemisphere ambient (sky/ground) |
| **Point lights** | Up to 128, extracted from BSM Light actors, sorted by distance |
| **Normal mapping** | TBN matrix from vertex tangents, texture unit 2 |
| **Specular mapping** | Gloss/reflectance mask, texture unit 3 |
| **Environment reflections** | Hemisphere gradient based on reflection direction × fresnel × specMask |
| **Alpha testing** | `discard` if texture alpha < 0.5 |
| **Two-sided materials** | Auto-detected from texture name (foliage, facing, glass) |
| **Emissive surfaces** | Self-illuminated neon signs, light beams |
| **Water rendering** | Separate alpha-blended pass (0.4 opacity), dark blue-green tint |
| **Shadow mapping** | 2048×2048 depth map from sun direction |
| **Distance fog** | Quadratic falloff, color-matched to clear color |
| **Tone mapping** | ACES filmic + exposure + gamma correction |
| **Transparency** | `uAlpha` uniform for water and future glass surfaces |

### Post-Processing (Composite Shader)
Applied in fullscreen pass after scene render:

| Effect | Details |
|--------|---------|
| **FXAA** | Nvidia FXAA 3.11 (simplified), edge-detect + blend |
| **SSAO** | 8-tap Poisson disk sampling from depth texture |
| **God Rays** | 32-tap radial blur from sun screen position, brightness threshold |
| **Bloom** | Threshold extract → 2-pass Gaussian blur → additive composite |
| **Vignette** | Radial darkening from screen edges |
| **Film Grain** | Hash-based procedural noise overlay |

### FBO Architecture
- **Scene FBO**: RGBA16F color + GL_DEPTH_COMPONENT24 texture (for SSAO)
- **Bloom FBOs**: 2× half-res ping-pong for Gaussian blur
- **Shadow FBO**: 2048×2048 depth-only

---

## UI Panels (ImGui)

### Scene Tree
- Hierarchical actor list grouped by category (Spawner, Trigger, Light, Door, Pickup, Effect, PlayerStart, StaticMesh, Other)
- Text filter, category toggles
- Click to select, scroll to selected

### Properties Panel
- Shows: class name, object name, export index
- Editable: Location (X/Y/Z), Rotation (Pitch/Yaw/Roll), Scale
- Changes written back to BSM data in memory

### Content Browser
- Lists all StaticMesh assets from UEViewer export directory
- Filter/search by name
- Double-click to spawn as new actor

### Viewport Controls
- **Lighting Panel**: Sun direction/color/intensity, fill light, ambient sky/ground, fog, exposure/gamma/tone mapping, point light multiplier
- **Post-Processing Panel**: FXAA, SSAO (intensity), Vignette (strength), Film Grain (strength), God Rays (density/intensity/decay), Water Surfaces toggle
- **Bloom Panel**: Enable, threshold, intensity
- **Presets**: "BioShock Interior" (dark ambient, low sun, fog, bloom, all post-FX)
- **View Modes** (F2): Lit, Unlit, Wireframe, Lit No Texture

### Rendering Controls
- **Draw radius** — Distance-based BSP culling
- **Zone filter** — Only show BSP visible from camera's zone
- **Section clip** — Z-height range clipping
- **Wireframe BSP** (F1)
- **Orthographic** (T)

---

## Camera System (camera.cpp)

- **Orbit mode** (default): Rotate around target point, pan, zoom
- **FPS fly mode** (RMB hold): WASD + mouse look, Shift = fast, Q/E = up/down
- **Bookmarks**: Ctrl+0-9 to save, 0-9 to recall
- **Focus** (F): Snap camera to selected actor

---

## Editor Features (app.cpp)

### Actor Manipulation
- **Select**: Click in viewport (ray-cast picking against mesh AABBs)
- **Multi-select**: Shift+click, or box-select (drag in viewport)
- **Move**: Translate gizmo (W), drag along X/Y/Z axis
- **Rotate**: Rotate gizmo (E)
- **Scale**: Scale gizmo (R)
- **Snap to grid**: Configurable grid snap
- **Delete**: Del key
- **Duplicate**: Ctrl+D
- **Copy/Paste**: Ctrl+C / Ctrl+V

### File I/O
- **Open BSM**: File dialog, loads any BioShock .bsm map
- **Save BSM**: Writes modified actor positions back to .bsm file
- **Layout JSON**: Save/Load editor-placed actors as JSON overlays
- **Recent files**: Auto-tracked recent maps
- **Autosave**: Every 5 minutes (configurable)

### Undo/Redo
- Position changes tracked in undo stack
- Ctrl+Z / Ctrl+Y

---

## Actor Spawning System

- **Content Browser**: Browse all StaticMesh assets, double-click to spawn
- **Spawner Palette** (Ctrl+Shift+S): Spawn actors by class name
- **Spawn classes**: Dynamically creates new EditorActor entries

---

## Texture System (texture_loader.cpp)

- Loads TGA files (uncompressed + RLE) from UEViewer export directory
- Automatic normal map detection: `*_Normal.tga` suffix
- Automatic specular map detection: `*_Specular.tga` suffix  
- Alpha channel scanning: flags textures with meaningful transparency
- Anisotropic filtering: 16× on all textures
- Texture dimensions used for BSP UV normalization (texel → [0,1])
- UV precision fix: large UV offsets shifted near zero to avoid GPU interpolation artifacts

### Stats (1-Medical)
- 569 inline UStaticMesh + 93 glTF fallback meshes loaded
- 471/569 inline meshes textured
- 58 normal-mapped meshes
- 32 specular-mapped meshes
- 361/379 BSP chunks textured
- 765 point lights extracted
- 4,065 actors linked by direct export ref
- 6 actors linked by import name (normalized)
- 84 actors linked by class-default mapping
- ~147 import-referenced actors unresolved (BulkContent gap)

---

## BSP Material Handling

- Material references resolved via export/import name tables
- Shader names stripped (`_Shader`/`_shader` suffix removed) to find diffuse texture
- Surfaces grouped by (material, zone) for efficient draw batching
- Water surfaces detected by name ("Water", "CalmWater") → rendered with transparency
- Utility surfaces skipped: FakeBackdrop, ZoningOnly, Portal, Invisible, Trigger, Volume, Fog, Sky

---

## Zone Visibility System

- BSP tree traversal determines camera's zone (0-127)
- Each BSP chunk stores union of zone visibility bitmasks from its constituent nodes
- When zone filter enabled: only chunks visible from camera's zone are rendered
- Massive performance improvement for large maps (Medical has 73 zones)

---

## Lightmap Status

### What We Know
- Lightmap textures live under a **`LightMaps_BSP`** group export (export[4085] in 1-Medical.bsm)
- 14 lightmap textures on 1-Medical: exports whose `outer` index points to LightMaps_BSP
  - All named `Texture_42` through `Texture_55` (generic UE2 naming, not descriptive)
  - All class `Texture`, format = DXT1, USize = VSize = 1024
  - `HasBeenStripped = true`, `StrippedMips = 5`
  - `totalBulkSize = 696320` bytes (mip0=524288 + mip1=131072 + mip2=32768 + mip3=8192)

### Where the Pixel Data Lives
- Pixel data is **NOT** in `.bsm.PackagePatch` — the PackagePatch only has property/serial patches
- Pixel data is in external **`.blk`** bulk chunk files at:
  ```
  D:\SteamLibrary\steamapps\common\BioShock Remastered\ContentBaked\pc\BulkContent\
  ```
  - 201 `.blk` files (~8 GB total), names like `BulkChunk1_123.blk`
  - A **`Catalog.bdc`** file (~500 KB) indexes which `.blk` file contains which texture

### Catalog.bdc Format (Reverse-Engineered)
- All strings are **UTF-16LE** with a 1-byte length prefix (char count including null terminator)
- Entry structure (repeating):
  ```
  [len:1B] [chunk_filename: len×2B UTF-16LE]   e.g. "BulkChunk1_123.blk\0"
  [len:1B] [object_name: len×2B UTF-16LE]      e.g. "Texture43\0"
  [len:1B] [package_name: len×2B UTF-16LE]     e.g. "1-Medical\0"
  [5× INT32 metadata (20 bytes)]:
    meta[0] = always 0 (flags? unknown)
    meta[1] = offset in .blk file (e.g. 0x8000, 0x2B8000)
    meta[2] = uncompressed size (696320 for 1024² DXT1 all mips)
    meta[3] = compressed size (same as meta[2] = not compressed)
    meta[4] = unknown (small int, possibly export index)
  ```

### Current Extraction Attempt
- Tool: `tools/lightmap_probe/main.cpp`
- Process:
  1. Parse BSM export table, find textures whose `outer` = LightMaps_BSP
  2. For each lightmap, search Catalog.bdc for matching object name + package name
  3. Read `meta[1]` bytes as offset into the associated `.blk` file
  4. Read 524288 bytes (mip0 of 1024×1024 DXT1) from that offset
  5. Write a 128-byte DDS header + raw mip0 data as `.dds` file

### Problem: Extracted DDS Files Look Wrong
- The extracted textures show **very saturated colorful blocks** (pure blue, green, red rectangles) on black
- Expected: smooth warm-toned gradients with soft falloff (typical lightmap appearance)
- DXT1 delta metric shows mixed results — some textures register as "smooth" (delta < 0.15) but still look wrong visually
- The `.blk` files start with large zero-padded regions (first ~32KB all zeros), then actual DXT1 data begins
- Possible issues:
  - The catalog offset (`meta[1]`) may not point to the start of the lightmap — it might point to a header or different mip level
  - The DDS width/height might be wrong (maybe not 1024×1024?)
  - The format might not be DXT1 despite the BSM property saying Format=3
  - The lightmaps might be stored in a different order (interleaved mips? channel-separated?)
  - There could be an additional per-texture header before the raw DXT1 blocks

### Workaround (Current)
- SSAO + point lights + hemisphere ambient provides decent indirect lighting approximation
- Real lightmaps would dramatically improve visual quality

---

## Need Help — Lightmap Extraction

### Goal
Extract the 14 lightmap DDS files from BioShock Remastered's bulk content so they can be applied to BSP surfaces in the level editor.

### What Was Done
1. **Identified lightmap exports** by finding the `LightMaps_BSP` group export in the BSM and collecting all Texture-class exports whose `outer` field references it
2. **Discovered bulk content system**: pixel data is NOT inline in the BSM — it's in external `.blk` files indexed by `Catalog.bdc`
3. **Reverse-engineered Catalog.bdc format**: UTF-16LE strings with 1-byte length prefixes, followed by 5× INT32 metadata per entry
4. **Mapped each lightmap** to its `.blk` file and offset via catalog lookup (matching object name "Texture43" + package name "1-Medical")
5. **Extracted 14 DDS files** by reading 524288 bytes at `meta[1]` offset from the correct `.blk` file
6. **Result**: the DDS files show saturated colorful blocks, NOT the smooth gradients expected from lightmaps

### What Needs Investigation
- **Is `meta[1]` truly the raw data offset?** Or does it point to a FByteBulkData header that precedes the actual pixels?
- **Is the format really DXT1?** The BSM export property says Format=3 (TEXF_DXT1), but the Remastered version might store lightmaps differently
- **Are the dimensions correct?** USize/VSize=1024 from BSM, but the external data might use different dimensions
- **Is there a per-texture bulk header** in the `.blk` file before the raw block data? (UE3 FByteBulkData typically has flags + element count + size + offset before the payload)
- **Could the data be compressed** even though `meta[2] == meta[3]`? Maybe zlib or LZO within the bulk data

### Key File Paths
- BSM: `D:\SteamLibrary\steamapps\common\BioShock Remastered\ContentBaked\pc\Maps\1-Medical.bsm` (214 MB)
- BulkContent: `D:\SteamLibrary\steamapps\common\BioShock Remastered\ContentBaked\pc\BulkContent\`
- Catalog: `...BulkContent\Catalog.bdc` (~500 KB)
- Probe tool: `z:\Bioshock1SDK\tools\lightmap_probe\main.cpp`

### Example Catalog Entry (Texture43)
```
Chunk file:  BulkChunk1_123.blk
Object name: Texture43
Package:     1-Medical
meta[0]=0  meta[1]=32768(0x8000)  meta[2]=696320  meta[3]=696320  meta[4]=2878
```
The .blk file is 36,708,372 bytes. First non-zero byte is at offset 0x80D0 (32,976), which is 208 bytes past meta[1].

---

## Performance

- Frustum culling via distance radius
- Zone-based visibility culling
- Material batching (one draw call per unique material×zone combination)
- uint16 index buffers (65K vertex limit per chunk, auto-split)
- GPU-resident geometry (static VBO/IBO)
- Deferred uniform updates (point lights sorted by camera distance, closest N uploaded)

---

## Key Bindings

| Key | Action |
|-----|--------|
| F1 | Toggle wireframe BSP |
| F2 | Cycle view mode (Lit/Unlit/Wireframe/LitNoTex) |
| T | Toggle orthographic/perspective |
| W/E/R | Translate/Rotate/Scale gizmo |
| F | Focus on selected actor |
| Del | Delete selected |
| Ctrl+D | Duplicate |
| Ctrl+Z/Y | Undo/Redo |
| Ctrl+C/V | Copy/Paste |
| Ctrl+A | Select all |
| Ctrl+S | Save |
| 0-9 | Recall camera bookmark |
| Ctrl+0-9 | Save camera bookmark |
| RMB+WASD | FPS fly mode |
| MMB drag | Pan |
| Scroll | Zoom |

---

## File Structure

```
tools/level_editor/
├── CMakeLists.txt          (101 lines)
├── LEVEL_EDITOR.md         (this file)
├── src/
│   ├── main.cpp            Entry point
│   ├── app.cpp/h           Application class (~1672 lines)
│   ├── viewport.cpp/h      3D renderer + shaders (~2563/270 lines)
│   ├── bsm_document.cpp/h  BSM file parser (~913/124 lines)
│   ├── bsp_parser.cpp/h    BSP geometry parser (~909/31 lines)
│   ├── mesh_parser.cpp/h   StaticMesh parser
│   ├── gltf_loader.cpp     glTF fallback loader
│   ├── texture_loader.cpp/h TGA texture cache
│   ├── camera.cpp/h        Camera system
│   ├── scene_tree.cpp/h    Scene hierarchy panel
│   ├── properties_panel.cpp/h Actor property editor
│   ├── content_browser.cpp/h Asset browser
│   ├── gl_funcs.cpp/h      OpenGL function loader
│   └── parse_test.cpp      CLI parse validation
├── build/                  CMake build output
└── ghidra_scripts/         Ghidra analysis scripts
```

---

## Dependencies on External Tools

- **UEViewer** (`Z:\UEViewer\`) — Exports textures as TGA and optional glTF meshes
- **bsm_tool** (`build/bin/Release/bsm_tool.exe`) — BSM inspection/patching utility
- Default map path: `D:\SteamLibrary\steamapps\common\BioShock Remastered\ContentBaked\pc\Maps\1-Medical.bsm`
- Default texture dir: `Z:\UEViewer\export\<mapName>\Texture`

---

## Not Yet Implemented

| Feature | Blocker |
|---------|---------|
| **Lightmaps** | Pixel data in external .blk files; extraction produces wrong colors (see Need Help section) |
| **Decals** | Needs projected texture system + decal actor parsing |
| **Particles** | Needs emitter actor data + billboard/sprite system |
| **Skeletal meshes** | Needs .psk bone data loading + skinning |
| **Glass/windows** | Needs proper sorted transparency (currently skipped) |
