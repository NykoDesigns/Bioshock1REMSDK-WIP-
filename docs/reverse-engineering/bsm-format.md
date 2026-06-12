# .BSM File Format - Reverse Engineering Notes

## Status: CONFIRMED — Full name/import/export/property parsing working (v0.3.0)

## Hypothesis

The `.bsm` format is likely a customized Unreal Package format. BioShock was built on the **Vengeance Engine** (Irrational's UE2.5 fork), which uses `.u` (code), `.utx` (textures), `.usx` (static meshes), `.unr` (maps) — all sharing the same underlying package structure.

The `.bsm` extension may stand for "**B**io**S**hock **M**ap" and is probably the equivalent of `.unr` with Vengeance Engine streaming extensions.

## Vengeance Engine Relationship

SWAT 4 (same engine) uses standard `.unr` map files. The switch to `.bsm` in BioShock likely reflects:
- Added streaming sublevel support (BioShock's seamless level transitions)
- Compressed chunk tables for faster loading
- Possibly a custom TOC for the streaming manager to pre-plan loads
- Additional metadata for the enhanced lighting system

SWAT 4's package format is well-documented by its modding community and should serve as baseline for comparison.

## Known Facts

- Used for level/map data
- Located in the game's Content directory
- Referenced by the engine's streaming system
- Multiple .bsm files per level (streaming sublevels)

## Expected Structure (Based on UE2.5 Package Format)

```
┌─────────────────────────────────────┐
│ Package Header (variable size)      │
│   • Magic (0x9E2A83C1 or custom)    │
│   • Version info                    │
│   • Table offsets & counts          │
│   • GUID                            │
│   • Generation info                 │
├─────────────────────────────────────┤
│ Name Table                          │
│   • All string identifiers used     │
│   • Length-prefixed, null-terminated │
├─────────────────────────────────────┤
│ Import Table                        │
│   • References to external packages │
│   • Class/object cross-references   │
├─────────────────────────────────────┤
│ Export Table                         │
│   • Objects defined in this package │
│   • Class, name, flags, offsets     │
├─────────────────────────────────────┤
│ Object Data (bulk of the file)      │
│   • Serialized object properties    │
│   • BSP tree data                   │
│   • Vertex/triangle data            │
│   • Lightmap data                   │
│   • Actor instances                 │
│   • Navigation mesh                 │
│   • Sound cue references            │
│   • Texture references              │
└─────────────────────────────────────┘
```

## Research Tasks

- [x] Collect multiple .bsm files from game install
- [x] Check first 4 bytes for magic number — `0x9E2A83C1` (standard UE)
- [x] Compare headers across files to identify fixed vs variable fields
- [x] Cross-reference with UE2.5 package format documentation
- [x] Parse name table (UTF-16LE with CompactIndex length, 8-byte flags)
- [x] Parse import table (FName refs with class/package/outer)
- [x] Parse export table (with BioShock-specific extra fields)
- [x] Deserialize UE1-style property tags from export serial data
- [x] Auto-detect header skip before properties (brute-force scoring)
- [x] Port property parser from TheWarInRapture Python to C++ bsm_tool
- [x] BSP geometry parsing (FBspNode 100B, FBspSurf variable, FVert 8B)
- [x] StaticMesh parsing (48B vertex stride, heuristic scanner)
- [x] UModel serialization order verified
- [x] Batch decompile all .U packages (1,765 classes)
- [x] Runtime SDK generation (4.5 MB headers)
- [x] FBspNode layout fully resolved (all 100 bytes mapped)
- [x] UTexture metadata parsing (Format + mip0 dimensions from BSM)
- [x] Shader/Material export parsing (Diffuse/NormalMap object references)
- [x] Bulk texture loading pipeline (Catalog.bdc → .blk → GPU)
- [x] FLightMapIndex/FLightMapLight/FLightMapTexture parsing (atlas descriptors)
- [x] FBspSurf PanU/PanV confirmed SKIPPED (v>=78, validated via UE2.5 source)
- [x] UE2.5 C++ source cross-reference (UnTex.h/cpp, UnRender.h, UnTemplate.h)
- [ ] Implement round-trip BSM writing (modify properties, rewrite package)
- [ ] Export cloning (duplicate actors with offset positions)
- [ ] Lightmap UV computation (WorldToLightMap × vertPos → atlas UV)

## Tools

- `bsm_tool analyze <file>` — Header analysis and validation
- `bsm_tool names <file>` — Dump name table
- `bsm_tool imports <file>` — Dump import table
- `bsm_tool exports <file>` — Dump export table with resolved class names
- `bsm_tool actors <file>` — Dump non-structural exports only
- `bsm_tool props <file> <idx>` — Dump all properties of an export (with values)
- `bsm_tool spawners <file>` — Find spawner actors with 3D locations
- `bsm_tool dump <file>` — Full dump (names + imports + exports)
- `bsm_tool hexdump <file> [off] [len]` — Raw hex dump
- `bsm_tool compare <a> <b>` — Compare two package headers
- HxD — Hex editor for manual inspection
- 010 Editor — With binary template support
- IDA Pro / Ghidra — For tracing deserialization code

## References

- Unreal Package format documentation (UE1/UE2 era)
- UModel source code (Unreal package extractor by Gildor)
- Epic Games UE2 Runtime source (if available)
- SWAT 4 SDK and modding documentation (same Vengeance Engine base)
- Tribes: Vengeance community research
- Previous community research on BioShock modding
- Vengeance Engine class hierarchy (compare SWAT 4 .u packages)

## Confirmed Findings

### 2026-05-10: Format Identification — CONFIRMED STANDARD UE PACKAGE

**.bsm IS a standard Unreal Engine package file.** Identical binary format to `.U` files.

| Field | Entry.bsm | Core.U | Match? |
|-------|-----------|--------|--------|
| Magic | 0x9E2A83C1 | 0x9E2A83C1 | YES |
| File Version | 141 | 141 | YES |
| Licensee Version | 56 | 56 | YES |
| Name encoding | UTF-16LE | UTF-16LE | YES |
| First name entry | "None" | "None" | YES |
| Name entry format | `len_u8 + wchar_t[] + null + flags` | Same | YES |

**Header layout (confirmed):**
```
+0x00: uint32 Magic           = 0x9E2A83C1
+0x04: uint16 FileVersion     = 141
+0x06: uint16 LicenseeVersion = 56
+0x08: uint32 PackageFlags
+0x0C: int32  NameCount
+0x10: int32  NameOffset
+0x14: int32  ExportCount
+0x18: int32  ExportOffset
+0x1C: int32  ImportCount
+0x20: int32  ImportOffset
+0x24: GUID   (16 bytes)
+0x34: int32  GenerationCount
+0x38: ...    Generation entries
```

**Name entry format:**
```
uint8     StringLength   (in wchar_t units, NOT bytes)
wchar_t[] Characters     (UTF-16LE, null-terminated)
uint8[6]  Flags          (purpose TBD, often 10 00 07 00 00 00 or 10 04 07 04 00 00)
```

**Sample names from Entry.bsm:**
None, AIClassName, AnimationName, MeleeThug, CeilingCrawler,
RangedAggressorPistol, Assassin, RangedAggressorSMG, Grenadier,
Vector, Color, CheckpointType, Scale, Level

**Sample names from Core.U:**
None, CheckpointTypePadding, FlushDetail, Type, LOD, Core, System,
==, !=, *, -, *=, M...

### 2026-05-10: Game Install Structure

**Executable:** `BioshockHD.exe` (20MB, x86)
**Audio:** FMOD (`fmodex.dll` confirmed)
**Graphics:** D3D9 (via `dxgi.dll` wrapper — possibly D3D9→D3D11 translation)

**File types discovered:**
- `.bsm` — Map packages (standard UE package format, 20KB–227MB)
- `.bsm.PackagePatch` — Binary patches applied by remaster
- `.U` — Script packages (Core, Engine, ShockGame, ShockAI, etc.)
- `.U.PackagePatch` — Script package patches
- `.blk` — Bulk content chunks (textures/meshes, ~20–70MB each)
- `.tfc` — Texture file cache (up to 2GB — UE3-style addition)
- `.bdc` — Bulk data catalog (indexes into .blk files)
- `.IBF` — Irrational Bundle Format (config bundles)
- `.lbf` — Localized bundle format (per-language data)
- `_lang.bsm` — Localized map packages (~4–16MB, contain dialogue/text)

**Script packages (BakedScripts/pc/):**
- `Core.U` (196KB) — Base engine types
- `Engine.U` (4.8MB) — Engine classes
- `ShockGame.U` (88MB) — Main game logic
- `ShockAI.U` (7.4MB) — AI subsystem
- `Tyrion.U` (96KB) — Behavior tree AI system (Irrational's)
- `VengeanceShared.U` (735KB) — Vengeance engine shared code
- `FMODAudio.U` (405KB) — FMOD audio integration
- `Scripting.U` (505KB) — Script system extensions
- `IGEffectsSystem.U` — Irrational effects framework
- `IGSoundEffectsSubsystem.U` — Sound effects
- `IGVisualEffectsSubsystem.U` — Visual effects
- `IGModEffectsSubsystem.U` — Mod effects (!)

**Note:** `dxgi.dll` in Build/Final suggests the remaster wraps D3D9 calls
through a D3D9→D3D11 translation layer. Our D3D9 hooks should still work
since the game code still calls D3D9 APIs, but we need to verify.

### 2025-06-14: Import & Export Table Format — FULLY CONFIRMED

Cross-referenced with **EliotVU/Unreal-Library** (C# decompiler with explicit BioShock build support)
and validated by parsing 3 BSM files with 100% success.

**BioShock-specific differences from standard UE2.5:**

1. **FName in file** = `CompactIndex(name_index) + int32(number+1)` — always includes Number field
2. **Name table strings**: `CompactIndex(length)`, negated for Vengeance engine → always UTF-16LE
3. **Export table** has 2 extra int32 fields (purpose unknown, version >= 130/132)
4. **Export ObjectFlags** is uint64 (not uint32) for licenseeVersion >= 40

**Name table entry format (corrected):**
```
CompactIndex  StringLength   (Vengeance negates → always unicode)
wchar_t[]     Characters     (UTF-16LE, null-terminated)
uint64        Flags          (8 bytes for version >= 141)
```

**Import table entry format:**
```
FName    ClassPackage   (CompactIndex + int32)  e.g. "Core"
FName    ClassName      (CompactIndex + int32)  e.g. "Class", "Package", "Texture"
int32    OuterIndex     (object reference, always 32-bit)
FName    ObjectName     (CompactIndex + int32)  e.g. "Engine", "PlayerStart"
```

**Export table entry format (BioShock v142, licensee 56):**
```
CompactIndex  ClassIndex     Object reference (neg=import, pos=export, 0=Class)
CompactIndex  SuperIndex     Parent class reference
int32         OuterIndex     Outer/package reference (always 32-bit)
int32         UnknownBS1     BioShock extra (version >= 132, always 0 observed)
FName         ObjectName     CompactIndex + int32(number+1)
uint64        ObjectFlags    BioShock: 8 bytes (licensee >= 40)
CompactIndex  SerialSize     Size of serialized object data
CompactIndex  SerialOffset   File offset to data (only if SerialSize > 0)
int32         UnknownBS2     BioShock extra (version >= 130, always 0 observed)
```

**CompactIndex encoding (1-5 bytes):**
```
Byte 0: bit7=sign, bit6=continue, bits5-0 = 6 data bits
Bytes 1-3: bit7=continue, bits6-0 = 7 data bits each
Byte 4: bits7-0 = 8 data bits (no continue)
Value = data_bits, negated if sign bit set
```

**Validation results:**
| File | Names | Imports | Exports | Status |
|------|-------|---------|---------|--------|
| Entry.bsm (20KB) | 153/153 | 21/21 | 40/40 | ✅ |
| 1-Medical.bsm (204MB) | 29295/29295 | 1381/1381 | 49140/49140 | ✅ |
| 0-Lighthouse.bsm (187MB) | 24096/24096 | 596/596 | 22780/22780 | ✅ |

**Reference:** EliotVU/Unreal-Library `UExportTableItem.cs` Deserialize method with `#if BIOSHOCK` conditionals.

### 2026-05-13: Property Deserialization — FULLY WORKING

Ported from **TheWarInRapture** Python codebase (`core/bsm_parser.py`) to C++ in `bsm_tool`.

**UE1-style property tag format:**
```
NameRef       PropertyName    (CompactIndex + int32 number)
uint8         InfoByte        bits[3:0]=type, bits[6:4]=sizeClass, bit7=arrayFlag
[NameRef]     StructName      (only if type==Struct)
[variable]    PackedSize      (depends on sizeClass 0-7)
[variable]    ArrayIndex      (only if arrayFlag && type!=Bool)
byte[]        Value           (PackedSize bytes)
```

**Property types (16):**
| ID | Type | Size | Value Encoding |
|----|------|------|----------------|
| 0 | None | 0 | End-of-properties sentinel |
| 1 | Byte | 1 | uint8 |
| 2 | Int | 4 | int32 |
| 3 | Bool | 0 | Value in arrayFlag bit (no payload) |
| 4 | Float | 4 | IEEE 754 float |
| 5 | Object | var | CompactIndex (object reference) |
| 6 | Name | 5+ | CompactIndex + int32 (FName) |
| 10 | Struct | var | Nested data (extra NameRef for struct type) |
| 11 | Vector | 12 | 3× float (X, Y, Z) |
| 12 | Rotator | 12 | 3× int32 (Pitch, Yaw, Roll) |

**Packed size classes:**
| SizeClass | Size |
|-----------|------|
| 0 | 1 byte |
| 1 | 2 bytes |
| 2 | 4 bytes |
| 3 | 12 bytes |
| 4 | 16 bytes |
| 5 | uint8 (next byte) |
| 6 | uint16 (next 2 bytes) |
| 7 | uint32 (next 4 bytes) |

**Header skip detection:**
Export serial data has a variable-length header before properties start (~4-80 bytes).
We brute-force test offsets 4-80, parsing properties at each, scoring against known
property names (Tag, Location, Rotation, Level, Region, etc.) to find the best match.
TheWarInRapture uses 57 as default; our testing confirms 57 is correct for most actor types.

**Example — AggressorSpawner from 1-Welcome.bsm (export #5091):**
```
RepopulationPatrol     Name       = 'TransitHubPatrol'
RepopulationAITypes    Array      size=3
SpawnZones             Array      size=7
Level                  Object     = LevelInfo_0 [Export#2972]
OwnerGroups            Array      size=7
Region                 Struct     struct=PointRegion
Tag                    Name       = 'AggressorSpawner'
PhysicsVolume          Object     = DefaultPhysicsVolume_5 [Export#2973]
Location               Struct     struct=Vector = (-9032.0, 7064.0, 2160.0)
Rotation               Struct     struct=Rotator = (0, 0, 0)
Label                  Name       = 'AggressorSpawner'
```

**Validation results (property parsing):**
| File | Exports Tested | Props Parsed | Status |
|------|---------------|-------------|--------|
| 1-Welcome.bsm | AggressorSpawner #5091, #5433 | 11-12 props each | ✅ |
| 1-Medical.bsm | 27 spawners, 60 ActionSpawnAI | All props readable | ✅ |

### 2026-05-19: FBspNode Layout — CONFIRMED (100 bytes)

Reverse-engineered via Ghidra decompilation + brute-force data correlation against ZoneMask bits.

**Vengeance FBspNode (100 bytes) vs stock UE2 FBspNode (64 bytes):**

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| +0 | 16B | FPlane Plane | Same as UE2 |
| +16 | 16B | ZoneMask | **Expanded from 8B** (128-bit, MAX_ZONES=128) |
| +32 | 4B | INT32 iVertPool | UE2 was at +24 (+8 shift from ZoneMask) |
| +36 | 4B | INT32 iSurf | UE2 was at +28 |
| +40 | 4B | INT32 iBack | UE2 was at +32 |
| +44 | 4B | INT32 iFront | UE2 was at +36 |
| +48 | 4B | INT32 iPlane | UE2 was at +40 |
| +52 | 4B | float BoundOrigin.X | **Vengeance-added** bounding sphere |
| +56 | 4B | float BoundOrigin.Y | |
| +60 | 4B | float BoundOrigin.Z | |
| +64 | 4B | float BoundRadius | |
| +68 | 4B | INT32 iCollisionBound | UE2's +44 — often -1 (INDEX_NONE) |
| +72 | 4B | INT32 iRenderBound | UE2's +48 — 0 or -1 |
| +76 | 1B | BYTE NodeFlags | UE2's +55 |
| **+77** | **1B** | **BYTE iZone[0]** | **CONFIRMED 100% ZoneMask correlation** (back-side zone) |
| **+78** | **1B** | **BYTE NumVertices** | **CRITICAL: Was misidentified as iZone[1]. See bug note below.** |
| +79 | 1B | BYTE iZone[1] / Pad | Front-side zone or alignment padding |
| +80 | 4B | INT32 iLeaf[0] | UE2's +56 — leaf index (often -1) |
| +84 | 4B | INT32 iLeaf[1] | UE2's +60 — leaf index (often -1) |
| +88 | 4B | INT32 (unknown) | **NOT NumVertices** — was wrongly used, purpose TBD |
| +92 | 4B | INT32 iContentBound | **Vengeance-added** — increasing values, content index |
| +96 | 4B | INT32 iRenderZone | **Vengeance-added** — small values 0-127 |

**CRITICAL BUG FIX (2026):** NumVertices is at **BYTE offset +78**, NOT INT32 at +88.
Reading INT32 at +88 produced garbage vertex counts (64% planarity failures, maxDist=37618).
Reading BYTE at +78 gives 100% planarity match (0/7125 failures, maxDist=0.25 on 1-Medical).
Confirmed via exhaustive probe of all 68 candidate byte offsets across 800 nodes.
The field at +78 was originally misidentified as iZone[1] in Ghidra analysis. Commit: 6612df0.

**Key discovery method:** Brute-force tested ALL byte offsets 52-99 for correlation with ZoneMask bit presence. Only byte offset +77 showed 100% match (4768/4768 polygon nodes on 1-Medical). Offset +96 (previously hypothesized as iZone) showed only 1% correlation.

**Zone visibility filtering:**
1. Traverse BSP tree from root (node 0): at each node, compute `PlaneDot(camera)`, follow iFront (positive) or iBack (negative)
2. When reaching -1 (leaf), take `iZone` (+77) from last valid node = camera's zone
3. For each BSP chunk, check if camera zone bit is SET in chunk's ZoneMask: `zoneMask[camZone/8] & (1 << (camZone%8))`
4. If bit is set → render; if not → cull

**Stock UE2 FBspNode for reference (64 bytes):**
```
+0:  FPlane Plane (16B)
+16: QWORD ZoneMask (8B)
+24: INT iVertPool (4B)
+28: INT iSurf (4B)
+32: INT iBack (4B)
+36: INT iFront (4B)
+40: INT iPlane (4B)
+44: INT iCollisionBound (4B)
+48: INT iRenderBound (4B)
+52: BYTE iZone[2] (2B)
+54: BYTE NumVertices (1B)
+55: BYTE NodeFlags (1B)
+56: INT iLeaf[2] (8B)
```

### 2026-06: Shader/Material Export Parsing — CONFIRMED

BSM packages contain Shader/FinalBlend/Modifier exports with tagged properties that
hold object references to Texture exports. Parsing these directly from BSM binary
provides a reliable shader→texture mapping without requiring UEViewer file exports.

**Export classes parsed:**
- `Shader` — Primary material class with `Diffuse`, `NormalMap`, `Specular` properties
- `FinalBlend` — Alpha/blend wrapper with `Material` property pointing to inner Shader
- `ColorModifier`, `OpacityModifier` — Tint/alpha wrappers
- `TexModifier`, `TexOscillator`, `TexPanner`, `TexRotator`, `TexScaler` — UV animation wrappers
- `Combiner` — Multi-texture blend
- `ConstantMaterial` — Flat color

**Property resolution:**
```
Shader export "PistolShader":
  Diffuse = ObjRef → Export #1234 "WP_Pistol_Diffuse" (Texture)
  NormalMap = ObjRef → Export #1235 "WP_Pistol_Normal" (Texture)

FinalBlend export "glass_blend":
  Material = ObjRef → Export #567 "glass_shader" (Shader)
    → Shader "glass_shader".Diffuse = ObjRef → "glass_diffuse" (Texture)
```

**Indirection chain following (up to 5 deep):**
- Shader.Diffuse → TexPanner (not a texture!) → resolve TexPanner.Material → Texture
- FinalBlend.Material → Shader → Shader.Diffuse → Texture

**Results on 1-Medical.bsm:**
- 767 Shader/Material exports parsed
- 621 with resolved Diffuse texture reference
- Supplements .mat/.props.txt file-based resolution (fills gaps where UEViewer didn't export shader files)

### 2026-06: UTexture Metadata Extraction — CONFIRMED

UTexture exports in the BSM contain the `Format` property (ETextureFormat ordinal)
plus mip0 dimensions in the mip table. This allows bulk texture loading without
requiring UEViewer exports.

**Parsing pipeline:**
1. `DetectHeaderSkip` + `ParsePropsMinimal` on each UTexture export
2. Extract `Format` property (default to DXT1=3 if absent)
3. After tagged properties: `ObjHeader(4,1)` + `INT64 CachedBulkDataSize` + `CI MipCount`
4. First mip: skip `ObjHeader(4,2)` + `INT32 SkipOffset` + `INT32 BulkA` + `INT32 BulkB` + `CI Num`
5. After inline/skip: read `INT32 USize` + `INT32 VSize` = mip0 dimensions
6. Compute `mip0Size = blockSize(format, USize, VSize)`

**Results on 1-Medical.bsm:**
- 5777 UTexture metadata entries parsed (99.5% success rate)
- Format distribution: DXT1=4200+, DXT3=~50, DXT5=~800, 3DC=~200, RGBA8=~100
