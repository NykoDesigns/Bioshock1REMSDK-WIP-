# Engine Source Recovery — BioShock Remastered (Vengeance UE2.5)

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
| **FMODAudio.U** | 406 KB | — | — | — | FMOD audio integration |
| **IGEffectsSystem.U** | 75 KB | — | — | — | Visual/sound effects framework |
| **IGSoundEffectsSubsystem.U** | 619 KB | — | — | — | Sound effects |
| **IGVisualEffectsSubsystem.U** | 201 KB | — | — | — | Visual effects |
| **IGModEffectsSubsystem.U** | 61 KB | — | — | — | Mod effects |

**TOTAL: ~1,730 classes, ~9,463 functions, ~258 states**

This IS the game logic source — just compiled to bytecode.

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

## Immediate Next Steps

1. **Download UE Explorer** and open ShockGame.U — verify decompilation works
2. **Export all classes** from ShockGame.U and ShockAI.U as .uc source files
3. **Store decompiled source** in `docs/reverse-engineering/decompiled/` for reference
4. **Cross-reference** decompiled function bodies with our ProcessEvent hooks
5. **Fix parameter struct layouts** in coop_sync.cpp based on actual TakeDamage/Died signatures

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
