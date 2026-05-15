# BS1SDK — BioShock 1 Remastered Unofficial SDK

> A community-driven modding framework for BioShock 1 Remastered (2016)

## Status: Active Development

### Working Features
- **D3D11 ImGui Overlay** — Debug UI rendered via DXGI hook
- **Engine Globals** — Runtime access to GObjects/GNames/GWorld/GEngine (UE2.5 Vengeance)
- **Full Type System** — UFunction (flags, native index, params), UClass (CDO, ClassFlags), UState, UEnum (name resolution), and 8 specialized UProperty types with inner type resolution
- **World System** — Level actor enumeration, spatial queries, position get/set, actor distance
- **Engine Tick Hook** — Frame-rate-independent callbacks via UGameEngine::Tick vtable hook
- **GNatives Table** — 4096-entry native function dispatch table discovery, hooking, and dumping
- **Class Default Objects** — Read/write CDO properties to modify all future instances of a class
- **Live Object Inspector** — Browse all live UObjects with property editing: bitmask-aware bool toggles, editable FVector/FRotator fields, byte/int/float spinboxes, object reference links
- **Player Properties** — Real-time ADAM, Credits, Health, EVE editing
- **Console** — In-game command console (~) with 50+ commands
- **ProcessEvent Hook** — MinHook inline detour intercepting all UFunction calls
- **Function Caller** — Invoke any UFunction on any object from console with arg marshaling
- **Weapon Editor** — Live editing of fire rate, accuracy, magazine size, ammo
- **Gameplay Mods** — God mode, one-hit kill, infinite ammo via PE hooks
- **Plasmid Hijacks** — Security Bullseye → teleport to impact, Hypnotize → summon Big Daddy
- **Noclip / Teleport** — `noclip` toggles PHYS_Flying + collision off; `tp <x> <y> <z>` instant teleport
- **Actor Spawner** — `spawn <ClassName>` spawns any actor at the player's position
- **Event Logger** — Dump all ProcessEvent calls to file for analysis
- **Lua 5.4 Scripting** — Full scripting bridge with 25+ API functions and hot-reload
- **SDK Generator** — Auto-generate fully-typed C++ headers (4,100+ classes, 10,000+ properties across 38 packages) with enum values, struct layouts, native function addresses, AI states, and per-package headers modders can `#include`
- **Asset Browser** — Dump all loaded textures, meshes, sounds, materials, animations to file
- **BSM Package Tools** — Full .bsm/.U analysis, spawn patcher, property editor, AI type editor (bsm_tool v0.5.0)
- **INI Config Tool** — Parse, edit, diff, extract/repack IBF archives (ini_tool v2.0.0)
- **Gameplay Mods** — Decoy→Teleport, Chain Lightning, Friendly Bots, Rivet Pistol, Splicer Factions
- **Mod Distribution** — winmm.dll proxy loader (no injector needed) + JSON config + packager script
- **Co-op Framework** — UDP transport, player state sync, ghost puppet system, damage forwarding

### Implemented Gameplay Mods

All mods activate via `initmods` console command or automatically via `mod_config.json`:

- **Decoy → Teleport** — Use Decoy plasmid to instantly teleport to spawn position
- **Chain Lightning** — Electro Bolt chains to nearby enemies (configurable radius/jumps/falloff)
- **Friendly Security Bots** — Security Command recruits bots (3 max, oldest recycled)
- **Rivet Pistol** — Revolver fires Rosie rivet projectiles (toggle on/off)
- **Splicer Factions** — Tag splicers into factions that fight each other
- **BSM Spawn Patcher** — Multiply enemy spawners offline (bsm_tool patch)
- **BSM Property Editor** — Edit any actor property in map files (bsm_tool setprop)
- **INI Patcher** — Modify weapons, AI, loot, plasmid configs (ini_tool set)

### Mod Distribution

End users don't need the SDK — just drop two files in the game folder:
```
Build\Final\
  ├── winmm.dll         ← Proxy loader (auto-loads BS1SDK.dll)
  ├── BS1SDK.dll        ← All runtime mods
  └── mod_config.json   ← Settings (editable, restart to apply)
```
Package with: `.\scripts\package_mod.ps1 -Name "MyMod"`

### SDK Generator Output

Run `dumpsdk` in the console to generate a complete reverse-engineered SDK:

```
sdk_gen/
├── SDK.h                  ← Master include
├── SDK_Types.h            ← Base types (FVector, FString, TArray, FName)
├── SDK_Enums.h            ← All 177 game enums with values (EPhysics, EInputKey, etc.)
├── SDK_Structs.h          ← All 213 struct layouts (FVector, FRotator, FQuat, etc.)
├── SDK_NativeFunctions.txt ← 9,597 native C++ function pointers with addresses
├── SDK_States.txt         ← All AI/game states per class (Idle, Attacking, Patrol...)
├── SDK_Assets.txt         ← All loaded textures, meshes, sounds (via 'assets' command)
├── SDK_STATS.txt          ← Summary: 4,119 classes, 10,343 properties, 38 packages
├── Engine.h               ← AActor, APawn, APlayerController (370 classes)
├── ShockGame.h            ← AShockPlayer, AShockPawn, weapons (400+ classes)
├── ShockAI.h              ← BigDaddy, Splicer, AI behaviors (523 classes)
├── ShockDesignerClasses.h ← Level scripting, triggers (831 classes)
├── FXClass.h              ← Visual effects (799 classes)
└── ... (38 packages total)
```

### Planned Features

**Co-op Multiplayer** (see `docs/reverse-engineering/co-op-feasibility.md`)
- UDP network bridge between two game instances
- Player state sync (position, rotation, health, weapon)
- Ghost puppet system for remote player visualization
- Damage and world state synchronization

**Additional Content**
- Custom map creation — build new .bsm packages from scratch
- IBF archive repacker — inject modified INI files back into archives
- External companion GUI — desktop window via IPC with injected DLL

## Building

### Requirements
- Visual Studio 2022 (with C++ Build Tools)
- CMake 3.20+
- Windows SDK (10.0+)

### Setup
```bash
git clone https://github.com/yourorg/BS1SDK.git
cd BS1SDK

# Generate VS solution (MUST be Win32/x86)
cmake -B build -A Win32
cmake --build build --config Release
```

Dependencies (MinHook, ImGui, Lua 5.4) are fetched automatically via CMake FetchContent.

### Output
- `build/bin/Release/BS1SDK.dll` — Main SDK (inject into game)
- `build/bin/Release/BS1Injector.exe` — DLL injector
- `build/bin/Release/bsm_tool.exe` — BSM/UE package analyzer + spawn patcher (v0.5.0)
- `build/bin/Release/ini_tool.exe` — INI config tool + IBF archive support (v2.0.0)
- `build/bin/Release/winmm.dll` — Proxy loader (drop in game folder, no injector needed)

## Usage

### Quick Start
1. Launch BioShock Remastered
2. Run `BS1Injector.exe`
3. Press **INSERT** to toggle debug overlay
4. Press **F12** to unload SDK

### Hotkeys
| Key | Action |
|-----|--------|
| INSERT | Toggle overlay |
| F2 | Object Inspector |
| F4 | Player Properties |
| F5 | Generate SDK headers |
| F6 | Hook ProcessEvent |
| F7 | Gameplay Mods menu |
| ~ | Console |
| F12 | Unload SDK |

### Console Commands
```
set <prop> <value>           Set player property
get <prop>                   Read player property
setobj <class> <prop> <val>  Set property on any object
give adam/credits/eve/health  Quick resource editing
god                          Toggle god mode
list <filter>                Search objects (case-insensitive)
props [filter]               List player properties
classes [filter]             List class names
call <class> <func> [args]   Call a UFunction on any object
funcs <class> [filter]       List functions on a class
spawn <class>                Spawn actor at player position
tp <x> <y> <z>              Teleport to coordinates
noclip                       Toggle fly + no collision
hookpe                       Toggle ProcessEvent hook
logpe [filter]               Log function calls (toggle)
pestats                      Show PE statistics
dumpevents                   Dump all events to file
lua <code>                   Execute Lua code
luafile <path>               Execute Lua script
reload                       Hot-reload autorun.lua
actors [class]               List level actors (optionally filter by class)
worldinfo                    Show level info, actor count, tick rate
nearby <radius> [class]      Find actors within radius of player
tickrate                     Show engine tick rate
cdo <class>                  Show ClassDefaultObject address
setdefault <c> <p> <v>       Set a CDO property (affects all future instances)
natives                      Show GNatives table info
gensdk                       Regenerate SDK headers
assets [filter]              Dump all loaded game assets to file
dumpsdk                      Full SDK dump (enums, structs, states, natives)
inspect <class|0xAddr>       Live inspect object properties + values
hierarchy                    Dump full class inheritance tree
functions                    Dump all UFunction objects
truehost [port]              Host a co-op session
truejoin <ip> [port]         Join a co-op session
```

### Lua Scripting
Place scripts in `BS1SDK_Scripts/` next to the game executable or DLL:

```lua
-- autorun.lua runs automatically on init
sdk.log("Hello from Lua!")

-- Read/write properties
local hp = sdk.getProperty("ShockPlayer", "Health")
sdk.setProperty("ShockPlayer", "ADAM", 9999)

-- Hook game events
sdk.hookpe()
local id = sdk.on("TakeDamage", function(objName, funcName)
    if objName:find("ShockPlayer") then
        return true  -- block damage = god mode
    end
    return false
end)

-- Call game functions
sdk.callFunction("ShockPlayer", "AddADAM", 100)

-- Find objects
local weapons = sdk.findObjects("Pistol")
for _, w in ipairs(weapons) do
    sdk.log(w.name .. " (" .. w.class .. ")")
end
```

**Lua API:**
| Function | Description |
|----------|-------------|
| `sdk.log(msg)` | Print to log |
| `sdk.getProperty(class, prop)` | Read int/float/bool property |
| `sdk.setProperty(class, prop, val)` | Write property |
| `sdk.callFunction(class, func, ...)` | Invoke a UFunction |
| `sdk.findObjects(filter)` | Search objects by name/class |
| `sdk.on(funcName, callback)` | Hook ProcessEvent (return true to block) |
| `sdk.off(hookId)` | Remove event hook |
| `sdk.hookpe()` | Enable ProcessEvent hook |
| `sdk.getActors([class])` | Get all actors in level (optionally filter) |
| `sdk.getActorPosition(addr)` | Get actor X, Y, Z position |
| `sdk.setActorPosition(addr, x, y, z)` | Teleport an actor |
| `sdk.getActorsInRadius(x, y, z, r)` | Spatial query — find nearby actors |
| `sdk.getPlayerPos()` | Get player X, Y, Z |
| `sdk.actorDistance(addr1, addr2)` | Distance between two actors |
| `sdk.getLevelInfo()` | Get level name and actor count |
| `sdk.getDefaultObject(class)` | Get ClassDefaultObject address |
| `sdk.setDefault(class, prop, val)` | Modify CDO property (global effect) |
| `sdk.getFunctions(class)` | List functions with flags/native info |
| `sdk.getEnum(enumName)` | Get enum literal names |
| `sdk.onTick(callback)` | Register per-frame tick callback |
| `sdk.offTick(id)` | Remove tick callback |
| `sdk.getTickRate()` | Current engine FPS |
| `sdk.getNativeCount()` | Number of native functions |
| `sdk.getNativeAddress(index)` | Get native function address |

## Project Structure
```
BS1SDK/
├── src/
│   ├── core/       — DLL entry, hooks, memory, patterns, logging
│   ├── engine/     — UObject type system, world access, function caller, engine globals
│   ├── hooks/      — DXGI Present hook, ProcessEvent hook (MinHook)
│   ├── render/     — ImGui overlay, mod menu, console
│   ├── gameplay/   — Plasmid hijacks (teleport, bots, chain lightning, factions)
│   ├── scripting/  — Lua 5.4 bridge (25+ API functions)
│   ├── sdk/        — Runtime SDK header generation (fully-typed output)
│   └── network/    — UDP transport, co-op bridge, state sync, puppet system
├── scripts/        — Example Lua scripts
├── injector/       — Standalone DLL injector
├── tools/
│   ├── bsm_tool/   — BSM package analyzer + spawn patcher + prop editor
│   └── ini_tool/   — INI config parser + patcher (weapons, AI, loot)
├── docs/
│   └── reverse-engineering/
│       ├── bsm-format.md        — BSM file format specification
│       ├── co-op-feasibility.md — Co-op multiplayer research
│       └── gameplay-goals.md    — Gameplay mod feasibility & plans
├── external/       — Third-party (MinHook, ImGui, Lua via FetchContent)
└── tests/          — Unit tests
```

## Technical Details

### Engine: Unreal Engine 2.5 Vengeance (Irrational Games fork)
- **UObject size**: 0x40 bytes
- **GObjects**: Static TArray at game base + 0x139042C
- **GNames**: Static TArray at game base + 0x13904EC
- **GWorld/GEngine**: Discovered at runtime via object class scan
- **GNatives**: 4096-entry native function dispatch table (discovered via heuristic)
- **Graphics**: D3D11 via DXGI (despite d3d9.dll being loaded)
- **ProcessEvent**: Virtual function, hooked via MinHook inline detour
- **Engine Tick**: UGameEngine::Tick hooked via vtable for per-frame callbacks

### Confirmed UObject Layout
| Offset | Field |
|--------|-------|
| +0x00 | VTable |
| +0x04 | InternalIndex |
| +0x1C | Outer |
| +0x28 | Name (FName) |
| +0x30 | Class |

### Confirmed AActor Layout (selected key offsets)
| Offset | Field | Type |
|--------|-------|------|
| +0x0090 | Physics | uint8 (EPhysics) |
| +0x00CC | Bitfield: bStatic, bHidden, bStasis, bWorldGeometry... | uint32 bitmask |
| +0x00D0 | Bitfield: bAlwaysRelevant, bReplicateMovement... | uint32 bitmask |
| +0x01D8 | Location | FVector (12B) |
| +0x01E4 | Rotation | FRotator (12B) |
| +0x01F0 | Velocity | FVector (12B) |
| +0x02AC | DrawScale | float |
| +0x0304 | Bitfield: bCollideActors, bBlockActors, bBlockPlayers... | uint32 bitmask |
| +0x0450 | Controller (APawn+) | UObject* |

### UFunction Layout (extends UStruct at +0x64)
| Offset | Field |
|--------|-------|
| +0x64 | FunctionFlags (uint32) |
| +0x68 | iNative (uint16) |
| +0x6A | OperPrecedence (uint8) |
| +0x6B | NumParms (uint8) |
| +0x6C | ParmsSize (uint16) |
| +0x6E | ReturnValueOffset (uint16) |
| +0x70 | NativeFunc (void*) |

### UClass Layout (extends UState)
| Offset | Field |
|--------|-------|
| +0x90 | ClassFlags (uint32) |
| +0x94 | ClassWithin (UObject*) |
| +0x98 | ClassConfigName (FName) |
| +0xC4 | ClassDefaultObject (UObject*) |

### UEnum Layout (extends UField)
| Offset | Field |
|--------|-------|
| +0x48..0x60 | Names TArray<FName> (auto-probed) |

> **Note:** BoolProperty stores a bitmask at +0x78. Multiple bools share the same offset with different bitmasks. The SDK handles this correctly with OR/AND operations.

### Specialized UProperty Inner Types (all at +0x78)
| Property Type | Inner Field |
|---------------|-------------|
| UByteProperty | UEnum* Enum |
| UObjectProperty | UClass* PropertyClass |
| UClassProperty | +0x78 PropertyClass, +0x7C MetaClass |
| UStructProperty | UStruct* Struct |
| UArrayProperty | UProperty* Inner |
| UMapProperty | +0x78 Key, +0x7C Value |

## Contributing

This is a reverse engineering project. Key ways to contribute:
1. **RE Analysis** — Document structures, functions, patterns
2. **Code** — Implement subsystems
3. **Lua Mods** — Write example scripts
4. **Documentation** — Write guides for modders
5. **Testing** — Test on different game versions

## Legal

Unofficial community project for educational and modding purposes.
BioShock is a trademark of 2K Games / Take-Two Interactive.
No game code or assets are distributed.

## License

MIT License (for SDK code only)
