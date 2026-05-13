# .BSM File Format - Reverse Engineering Notes

## Status: EARLY RESEARCH

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

- [ ] Collect multiple .bsm files from game install
- [ ] Check first 4 bytes for magic number
- [ ] Compare headers across files to identify fixed vs variable fields
- [ ] Hook CreateFileW to observe .bsm load order
- [ ] Set breakpoints on file read to trace deserialization code
- [ ] Look for decompression calls (zlib inflate) during .bsm loading
- [ ] Identify name table by looking for known strings (level names, class names)
- [ ] Cross-reference with UE2.5 package format documentation

## Tools

- `bsm_tool analyze <file>` - Our custom analysis tool
- HxD - Hex editor for manual inspection
- 010 Editor - With binary template support
- IDA Pro / Ghidra - For tracing deserialization code

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
| File Version | 142 | 142 | YES |
| Licensee Version | 56 | 56 | YES |
| Name encoding | UTF-16LE | UTF-16LE | YES |
| First name entry | "None" | "None" | YES |
| Name entry format | `len_u8 + wchar_t[] + null + flags` | Same | YES |

**Header layout (confirmed):**
```
+0x00: uint32 Magic           = 0x9E2A83C1
+0x04: uint16 FileVersion     = 142
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
