# Engine Source Recovery — BioShock Remastered (Vengeance UE2.5)

## Status: COMPLETED — 1,765 classes decompiled (4.97 MB UnrealScript source)

## What Exists

BioShock Remastered ships 12 compiled UnrealScript packages at:
```
D:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BakedScripts\pc\
```

| Package | Size | Classes | Functions | States | Purpose |
|---------|------|---------|-----------|--------|---------|
| **Core.U** | 197 KB | 10 | 232 | 0 | Base UObject system |
| **Engine.U** | 4.9 MB | 320 | 1,982 | 38 | Actor, Pawn, Controller, Canvas, HUD, Physics |
| **ShockGame.U** | 88 MB | 654 | 2,905 | 75 | ALL BioShock gameplay: weapons, plasmids, enemies, UI, doors, pickups, items |
| **ShockAI.U** | 7.4 MB | 540 | 3,495 | 138 | ALL AI logic: perception, targeting, pathfinding, combat states |
| **Scripting.U** | 505 KB | 99 | 359 | 2 | Kismet visual scripting system |
| **VengeanceShared.U** | 735 KB | 80 | 460 | 5 | Shared Vengeance engine extensions |
| **Tyrion.U** | 97 KB | 27 | 30 | 14 | AI behavior tree system |
| **FMODAudio.U** | 406 KB | 1 | — | — | FMOD audio integration |
| **IGEffectsSystem.U** | 75 KB | 8 | 26 | 0 | Visual/sound effects framework |
| **IGSoundEffectsSubsystem.U** | 619 KB | 14 | 91 | 0 | Sound effects |
| **IGVisualEffectsSubsystem.U** | 201 KB | 9 | 7 | 0 | Visual effects |
| **IGModEffectsSubsystem.U** | 61 KB | 3 | 6 | 0 | Mod effects |

**TOTAL: 1,765 classes, ~9,463 functions, ~272 states**

This IS the game logic source — just compiled to bytecode.

### Decompilation Status: ✅ COMPLETE

All 12 packages have been batch-decompiled using UELib v1.12.0 (NuGet).

**Output location:** `docs/reverse-engineering/decompiled/`

| Package | Classes | Output Size | Quality |
|---------|---------|-------------|--------|
| Core | 10 | 46.8 KB | Good (some native function body errors) |
| Engine | 320 | 895.7 KB | Good |
| ShockGame | 654 | 1,822.3 KB | Good (main game logic fully recovered) |
| ShockAI | 540 | 1,842.4 KB | Good (all AI states and behaviors) |
| Scripting | 99 | 192.7 KB | Good |
| VengeanceShared | 80 | 190.5 KB | Good |
| Tyrion | 27 | 28.0 KB | Good |
| FMODAudio | 1 | 6.2 KB | Good |
| IGEffectsSystem | 8 | 17.1 KB | Good |
| IGModEffectsSubsystem | 3 | 4.0 KB | Good |
| IGSoundEffectsSubsystem | 14 | 30.2 KB | Good |
| IGVisualEffectsSubsystem | 9 | 9.0 KB | Good |

**Known issues:**
- Some native function calls show as `__NFUN_XXX__` (need native table mapping)
- A few function bodies have partial decompilation errors (bytecode format differences)
- Default properties generally decompile correctly

**Decompiler tool:** `tools/ue_decompiler/` (.NET 9 console app using UELib NuGet)

---

## Three Paths to "Engine Source"

### Path 1: UnrealScript Decompilation (HIGHEST VALUE)

**Tool: [UE Explorer](https://github.com/UE-Explorer/UE-Explorer) + [Unreal-Library (UELib)](https://github.com/EliotVU/Unreal-Library)**

UELib has **confirmed Vengeance engine support** since v1.3.0 (explicitly lists BioShock 1 & 2).

This will decompile the .U bytecode back to readable UnrealScript source code, giving us:
- Every function body (the actual logic)
- Every class definition with inheritance
- Every state machine (AI states, player states)
- Every default property value
- Every variable declaration

**This is the single most impactful thing we can do.** It gives us the complete game logic layer.

#### How to use:
1. Download UE Explorer from https://github.com/UE-Explorer/UE-Explorer/releases
2. Open each .U file in UE Explorer
3. Browse the class tree, view decompiled functions
4. Export all classes as .uc files (UnrealScript source)

#### Or use UELib programmatically (.NET/C#):
```csharp
using UELib;

var package = UnrealLoader.LoadPackage(@"ShockGame.U", FileAccess.Read);
package.InitializePackage();

foreach (var obj in package.Objects) {
    if ((int)obj > 0 && obj is UELib.Core.UClass cls) {
        Console.WriteLine($"class {cls.Name} extends {cls.Super?.Name}");
        // Decompile all functions
        foreach (var child in cls.Children) {
            if (child is UELib.Core.UFunction func) {
                Console.WriteLine(func.Decompile());
            }
        }
    }
}
```

### Path 2: UE2.5 Reference Source (HIGH VALUE)

The Unreal Engine 2/2.5 source code is publicly available from various modding communities
(UT2004 Runtime source, Devastation SDK, etc.). Since Vengeance is a UE2.5 fork, the core
C++ engine code is structurally identical for:

- UObject system (GObjects, GNames, property reflection)
- ProcessEvent mechanics and bytecode interpreter
- Actor spawning and level loading
- Physics and collision
- Rendering pipeline (D3D hookable points)
- Network subsystem (even though it's stripped, knowing the original design helps)

We already reverse-engineered and confirmed the UObject layout matches UE2.5 exactly:
- UObject at 0x40 bytes
- UField at +0x40 (SuperField, Next)
- UStruct at +0x5C (Children, PropertiesSize)
- UProperty at +0x74 (Offset in instance)

The reference source fills in everything we can't get from bytecode: native C++ functions,
vtable layouts, engine tick ordering, memory management.

### Path 3: Static Analysis of BioshockHD.exe (MEDIUM VALUE)

For native-only functions (not implemented in UnrealScript), we need IDA Pro or Ghidra:

- **BioshockHD.exe** (~30 MB) — The main executable with all native C++ code
- Key targets: `UObject::ProcessEvent`, native function dispatch, rendering hooks
- We've already located most of what we need via pattern scanning, but a full disassembly
  would reveal every native function implementation

This is most useful for:
- Understanding the stripped networking code (if any remnants exist)
- Native physics/collision internals
- Renderer internals for advanced visual mods
- Finding undocumented engine globals

---

## What Decompilation Gives Us for Co-op

### Critical Functions We Need to See Inside

| Function | Package | Why |
|----------|---------|-----|
| `ShockPawn.Died` | ShockGame.U | Full death flow — what happens on kill |
| `ShockPawn.TakeDamage` | ShockGame.U | Damage calculation pipeline |
| `ShockPlayer.Resurrect` | ShockGame.U | Vita-Chamber respawn logic |
| `ShockDoor.Open/Close` | ShockGame.U | Door state machine |
| `VitaChamber.*` | ShockGame.U | Respawn point selection |
| `Pickup.PickedUp` | ShockGame.U | Item collection flow |
| `ShockPawn.IsFrozen/IsBerserk/IsShocked` | ShockGame.U | Status effect queries |
| `SecurityBot.* states` | ShockAI.U | Bot AI behavior |
| `Aggressor.* states` | ShockAI.U | Splicer AI combat states |
| `Protector.* states` | ShockAI.U | Big Daddy AI states |
| `IsFriendly/PawnIsFriendly` | ShockAI.U | AI friendship checks |
| `AddForcedEnemy` | ShockAI.U | AI targeting override |
| `AggressorSpawner.*` | ShockAI.U | Spawner logic and timing |

### What This Unlocks

With decompiled source, we can:
1. **See exact TakeDamage parameter layout** — stop guessing parms struct offsets
2. **Understand death flow** — what Died() actually does (ragdoll, loot drop, XP, etc.)
3. **See Vita-Chamber selection** — how nearest chamber is found
4. **Understand AI targeting priority** — how enemies choose targets (critical for co-op aggro)
5. **See door/trigger state machines** — exact conditions for open/close/lock
6. **Understand inventory system** — how items are stored, equipped, consumed
7. **See the hack minigame** — full flow for syncing hack results
8. **Understand Big Daddy aggro** — what triggers BD rage (essential for co-op BD fights)

---

## Completed Steps

1. ✅ **Batch decompile all .U packages** — 1,765 classes exported to `docs/reverse-engineering/decompiled/`
2. ✅ **Runtime SDK generation** — Complete class/function/property dump in `sdk_gen/` (4.5 MB)
3. ✅ **BSP struct layouts** — FBspNode (100B), FBspSurf (variable), FVert (8B) confirmed
4. ✅ **Package format** — Full BSM/U parsing in bsm_tool

## Remaining Steps

1. **Map __NFUN_XXX__ to actual function names** using SDK_NativeFunctions.txt
2. **Cross-reference** decompiled function bodies with our ProcessEvent hooks
3. **Fix parameter struct layouts** in coop_sync.cpp based on actual TakeDamage/Died signatures

## UE2.5 C++ Source Integration (June 2026)

Complete Unreal Engine 2.5 C++ source has been added to the project tree for cross-reference:

```
z:\Bioshock1SDK\unreal-src-part1\
  └── Unreal Tournament 2003 [v2107] [2002-10-01] (Retail)\
      ├── Editor\Classes\     — UnrealEd brush builders, tools
      ├── Engine\Classes\     — Actor, Pawn, Trigger, Mover .uc sources
      ├── Core\Classes\       — Object, Field, Package .uc sources
      └── ... (Fire, IpDrv, UWeb, XInterface)

z:\Bioshock1SDK\unreal-src-part-two\
  └── Unreal Engine [v2.5]_ Unreal Warfare [09-29-2007]\
      ├── Engine\Inc\         — UnRender.h, UnTex.h, UnRenderResource.h
      ├── Engine\Src\         — UnTex.cpp, UnRender.cpp, UnModel.cpp
      ├── Core\Inc\           — UnTemplate.h, UnObj.h
      └── Core\Src\           — UnObj.cpp, UnLinker.cpp
```

### Key Findings from Source Cross-Reference

| File | What We Learned | Applied To |
|------|-----------------|-----------|
| `Engine\Inc\UnRenderResource.h` | `ETextureFormat` enum: DXT1=3, DXT3=7, DXT5=8, RGBA8=5, 3DC=12 | UTexture metadata parsing |
| `Engine\Inc\UnTex.h` | `FMipmap` struct with `TLazyArray<BYTE> DataArray` + USize/VSize/UBits/VBits | BSM texture wire format validation |
| `Engine\Src\UnTex.cpp` | `UTexture::Serialize` mip loop, algorithmic texture handling | Confirmed no unknown fields between mips |
| `Core\Inc\UnTemplate.h` | `TLazyArray` serialization: `INT32 SeekPos` then `CI Count + Data[Count]` for Ver>61 | Bulk vs inline mip detection |
| `Engine\Inc\UnRender.h` | Stock `FBspNode` = 64 bytes, `ZoneMask` = 8 bytes | Confirmed Vengeance expanded to 100B/16B |
| `Engine\Src\UnModel.cpp` | FBspSurf serialization with version gates | Confirmed PanU/PanV skip for v>=78 |

### What This Resolved

- ✅ **FBspNode layout** — All 100 bytes fully mapped (previously 4 unknown fields)
- ✅ **FBspSurf PanU/PanV** — Confirmed SKIPPED, not missing or corrupt
- ✅ **TLazyArray format** — Validated inline vs external mip detection logic
- ✅ **Texture format ordinals** — No longer guessing format byte meanings
- ✅ **UTexture serialization** — Exact field order and sizes confirmed
- ✅ **FLightMapIndex** — WorldToLightMap matrix + tile packing formula from render code

---

## Key Game Classes Already Identified

### From ShockGame.U (654 classes):
**Gameplay-critical (found via bsm_tool export dump):**
- ShockPawn, ShockPlayer, ShockPlayerController, ShockHUD, ShockCheatManager
- ShockDoor, ThreeStateDoor, DoorAccessControl, DoorButtonControl, DoorKeypadControl
- Pickup, FusePickup, MedHypoPickup, SeaSlugPickup, WeaponPickup
- Ability (base), ElectricBoltAbility, IncinerationAbility, TelekinesisAbility, etc.
- Weapon, PlayerWeapon, MachineGun, all ammo types
- VendingStation, HealthStation, SecurityStation, WeaponUpgradeStation
- Container, DeadBodyContainer, AnimatedContainer, KeypadContainer
- SecurityBeacon, SecurityBeaconAbility, SecurityCrate
- HackInfo, GeneticHacker, ICanBeHacked
- ResearchCamera, CameraPhoto

### From ShockAI.U (540 classes):
- BioshockAIController, BaseShockAI
- All enemy AI behavior trees and states (138 states!)
- Perception, targeting, pathfinding logic
- IsFriendly/AddForcedEnemy/ForceAttackTarget

### From Engine.U (320 classes):
- Actor, Pawn, Controller, PlayerController
- Mover, Trigger, TriggerVolume
- Canvas, HUD, Interaction
- Level, LevelInfo, GameInfo
- Physics, Collision, Navigation
