# BS1SDK Modding Guide

> How to make mods for BioShock 1 Remastered using the BS1SDK

---

## Table of Contents

1. [Installation](#1-installation)
2. [Quick Start — Console Modding](#2-quick-start--console-modding)
3. [JSON Config — Tweak Built-In Mods](#3-json-config--tweak-built-in-mods)
4. [Lua Scripting — Write Your Own Mods](#4-lua-scripting--write-your-own-mods)
5. [Offline Map & Config Modding](#5-offline-map--config-modding)
6. [C++ Modding — Full Engine Access](#6-c-modding--full-engine-access)
7. [Distributing Your Mod](#7-distributing-your-mod)
8. [Console Command Reference](#8-console-command-reference)
9. [Lua API Reference](#9-lua-api-reference)
10. [Tips & Tricks](#10-tips--tricks)

---

## 1. Installation

### For Players & Modders

1. Go to your BioShock Remastered install folder:
   - Steam: Right-click game → Manage → Browse Local Files → `Build\Final\`
2. Drop these files into `Build\Final\`:
   ```
   winmm.dll         ← Proxy loader (auto-loads the SDK)
   BS1SDK.dll        ← The SDK itself
   mod_config.json   ← Settings file (optional, created automatically)
   ```
3. Launch the game normally through Steam. No injector needed.

### Verify It's Working

- Press **INSERT** — you should see the ImGui debug overlay
- Press **~** (tilde) — the console should appear
- Type `help` — you should see the command list

### Uninstall

Delete `winmm.dll`, `BS1SDK.dll`, and `mod_config.json` from `Build\Final\`.

---

## 2. Quick Start — Console Modding

Press **~** to open the console. You can mod the game live without writing any code.

### Give Yourself Resources
```
give adam 9999
give credits 9999
give health 9999
give eve 100
```

### Combat Cheats
```
god                     Toggle invincibility
initmods                Activate all gameplay mods
```

### Spawn Enemies & Objects
```
spawn Splicer           Spawn a splicer at your feet
spawn BigDaddy          Spawn a Big Daddy
spawn HealthStation     Spawn a health station
```

### Movement
```
noclip                  Fly through walls (toggle)
tp 1000 2000 -500       Teleport to coordinates
teleport 800            Blink forward 800 units
```

### Gameplay Mods (toggle on/off)
```
decoytp on              Decoy plasmid now teleports you
chain on                Electro Bolt chains between enemies
bots on                 Security Command spawns friendly bots
rivets on               Revolver fires Rosie rivets
factions on             Enable splicer factions
tag 1                   Tag nearest splicer to faction 1
tag 2                   Tag nearest splicer to faction 2
```

### Inspect the Game
```
actors ShockPawn        List all enemy pawns
nearby 1000             Find everything within 1000 units
props Health            Search player properties for "Health"
inspect ShockPlayer     Live-inspect all player properties
worldinfo               Current map name + actor count
```

### Edit Any Property Live
```
set Health 9999         Set player health
set ADAM 500            Set ADAM
setobj Pistol BaseFireRate 15.0    Make the pistol fire faster
setdefault Pistol BaseMagazineSize 99    All pistols spawn with 99 rounds
```

---

## 3. JSON Config — Tweak Built-In Mods

Edit `mod_config.json` in `Build\Final\` and restart the game:

```json
{
  "autoInitMods": true,
  "showOverlay": true,

  "decoyTeleport": true,

  "friendlyBots": true,
  "friendlyBotLimit": 5,

  "rivetPistol": false,

  "splicerFactions": false,

  "chainLightning": true,
  "chainRadius": 800.0,
  "chainMaxJumps": 5,
  "chainDamageFalloff": 0.3
}
```

| Setting | What It Does | Default |
|---------|-------------|---------|
| `autoInitMods` | Auto-activate mods on game start | `false` |
| `showOverlay` | Show ImGui overlay | `true` |
| `decoyTeleport` | Decoy plasmid becomes teleport | `true` |
| `friendlyBots` | Security Command spawns allies | `true` |
| `friendlyBotLimit` | Max friendly bots at once | `3` |
| `rivetPistol` | Revolver fires rivets | `false` |
| `splicerFactions` | Enable faction system | `false` |
| `chainLightning` | Electro Bolt chains | `true` |
| `chainRadius` | Chain jump distance (units) | `500` |
| `chainMaxJumps` | Max chain targets | `3` |
| `chainDamageFalloff` | Damage multiplier per jump | `0.5` |

---

## 4. Lua Scripting — Write Your Own Mods

The easiest way to make a custom mod. No compiling. Hot-reload in-game.

### Setup

1. Create folder: `Build\Final\BS1SDK_Scripts\`
2. Create `autorun.lua` inside it — this runs automatically on game start
3. To reload after editing: type `reload` in the console

### Example: Hello World
```lua
-- BS1SDK_Scripts/autorun.lua
sdk.log("Hello from my first mod!")
```

### Example: Vampire Mode (heal on kill)
```lua
-- Heal the player whenever an enemy dies
sdk.hookpe()
sdk.on("Died", function(objName, funcName)
    if not objName:find("ShockPlayer") then
        -- An enemy died, heal the player
        sdk.setProperty("ShockPlayer", "Health", 500.0)
        sdk.log("Vampire: absorbed life force!")
    end
    return false  -- don't block the event
end)
sdk.log("Vampire Mode loaded!")
```

### Example: Rapid Fire Pistol
```lua
-- Make the pistol fire 10x faster
sdk.setProperty("Pistol", "BaseFireRate", 15.0)
sdk.setProperty("Pistol", "BaseMagazineSize", 99)
sdk.log("Rapid fire pistol enabled!")
```

### Example: Custom God Mode (block all damage to player)
```lua
sdk.hookpe()
local hookId = sdk.on("TakeDamage", function(objName, funcName)
    if objName:find("ShockPlayer") then
        return true   -- block = player takes no damage
    end
    return false      -- enemies still take damage normally
end)
sdk.log("God mode ON (hook " .. hookId .. ")")
```

### Example: Spawn Wave on Tick
```lua
-- Spawn an enemy every 10 seconds
local timer = 0
local interval = 10.0
local tickId = sdk.onTick(function(dt)
    timer = timer + dt
    if timer >= interval then
        timer = 0
        local px, py, pz = sdk.getPlayerPos()
        if px then
            -- Spawn 500 units in front
            sdk.setActorPosition(nil, px + 500, py, pz)
            sdk.log("Wave spawned!")
        end
    end
end)
sdk.log("Wave spawner active (every " .. interval .. "s)")
```

### Example: Big Head Mode (CDO editing)
```lua
-- Make all future splicers spawn with 3x head scale
sdk.setDefault("ShockPawn", "HeadScale", 3.0)
sdk.log("Big head mode ON!")
```

### Example: Proximity Alert
```lua
-- Warn when enemies are nearby
sdk.onTick(function(dt)
    local px, py, pz = sdk.getPlayerPos()
    if not px then return end
    local nearby = sdk.getActorsInRadius(px, py, pz, 500)
    for _, actor in ipairs(nearby) do
        if actor.class:find("Splicer") or actor.class:find("BigDaddy") then
            sdk.log("WARNING: " .. actor.class .. " nearby!")
        end
    end
end)
```

### Loading Multiple Scripts
```lua
-- autorun.lua can load other scripts:
dofile("BS1SDK_Scripts/combat_mods.lua")
dofile("BS1SDK_Scripts/weapon_mods.lua")
dofile("BS1SDK_Scripts/my_custom_mod.lua")
```

---

## 5. Offline Map & Config Modding

These tools modify game files directly — no DLL injection needed.

### bsm_tool — Map File Editor

Analyze and patch `.bsm` map files (in `ContentBaked\pc\Maps\`).

```bash
# Analyze a map
bsm_tool analyze "1-Medical Pavilion.bsm"

# List all actors
bsm_tool actors "1-Medical Pavilion.bsm"

# List all spawners
bsm_tool spawners "1-Medical Pavilion.bsm"

# Double all enemy spawns
bsm_tool patch "1-Medical Pavilion.bsm" 2

# Change a property on export #100
bsm_tool setprop "1-Medical Pavilion.bsm" 100 Health 500

# List AI types on a spawner (export #200)
bsm_tool aitypes "1-Medical Pavilion.bsm" 200

# Change a spawner's AI type
bsm_tool setaitype "1-Medical Pavilion.bsm" 200 0 -15

# Search for names/imports/exports
bsm_tool findname "1-Medical Pavilion.bsm" Splicer
bsm_tool findexport "1-Medical Pavilion.bsm" BigDaddy
```

### ini_tool — Game Config Editor

Edit weapons, AI, loot, plasmids via INI files.

```bash
# Extract all INI files from the game's config archive
ini_tool extract "ContentBaked\pc\System\ConfigINI.IBF" extracted_ini

# View weapon settings
ini_tool dump extracted_ini\Weapons.ini

# Make the pistol do 100 damage
ini_tool set extracted_ini\Weapons.ini "Pistol" "Damage" "100"

# Search for a setting
ini_tool search extracted_ini\Weapons.ini "FireRate"

# Compare your changes to vanilla
ini_tool diff extracted_ini\Weapons.ini vanilla\Weapons.ini

# Repack (optional — game loads loose .ini files from ContentBaked/pc/System/)
ini_tool repack extracted_ini ConfigINI.IBF
```

**Shortcut:** You don't need to repack — just copy your edited `.ini` files to
`ContentBaked\pc\System\`. The engine loads loose files over the IBF archive.

### What You Can Edit via INI

| File | Contents |
|------|----------|
| `Weapons.ini` | Damage, fire rate, mag size, reload speed, accuracy per weapon |
| `AI.ini` | Aggression, perception range, combat behavior |
| `Loot.ini` | Drop tables, search body items, container contents |
| `Plasmids.ini` | EVE cost, cooldowns, damage, duration per plasmid/tonic |
| `Economy.ini` | Vending machine prices, ADAM costs |
| `Difficulty.ini` | Damage multipliers, enemy health scaling |

---

## 6. C++ Modding — Full Engine Access

For advanced modders who want to build something substantial.

### Build the SDK

```bash
git clone https://github.com/yourorg/BS1SDK.git
cd BS1SDK
cmake -B build -A Win32
cmake --build build --config Release
```

Output: `build\bin\Release\BS1SDK.dll` + all tools.

### Generate SDK Headers

1. Launch the game with BS1SDK loaded
2. Open console, type: `dumpsdk`
3. Check `sdk_gen/` folder — fully typed C++ headers for all 4,119 game classes

```
sdk_gen/
├── SDK.h                    ← Master include
├── SDK_Types.h              ← FVector, FString, TArray, FName
├── SDK_Enums.h              ← 177 enums (EPhysics, EInputKey, ...)
├── SDK_Structs.h            ← 213 struct layouts
├── SDK_NativeFunctions.txt  ← 9,597 native C++ pointers with addresses
├── Engine.h                 ← AActor, APawn, APlayerController
├── ShockGame.h              ← AShockPlayer, weapons, plasmids, items
├── ShockAI.h                ← BigDaddy, Splicer, AI behaviors
└── ... (38 packages total)
```

### Key APIs for C++ Mods

**Property Access:**
```cpp
#include "engine/uobject.h"
UObject* player = FindObjectByClassName("ShockPlayer");
auto props = WalkProperties(FindClass("ShockPlayer"));
player->SetField<float>(healthOffset, 9999.0f);
```

**ProcessEvent Hook:**
```cpp
#include "hooks/process_event.h"
int hookId = RegisterPECallback([](UObject* obj, UFunction* func, void* params) {
    if (func->GetName() == "TakeDamage" && IsPlayerActor(obj)) {
        return true;  // block damage
    }
    return false;
});
```

**World System:**
```cpp
#include "engine/world.h"
auto actors = GetActorsOfClass("ShockPawn");
for (auto* actor : actors) {
    FVec3 pos;
    GetActorPosition(actor, pos);
    // do something with every enemy in the level
}
```

**Function Calling:**
```cpp
#include "engine/function_caller.h"
UFunction* addAdam = FindFunctionOnClass(playerClass, "AddADAM");
struct { int32_t Amount; } parms{ 100 };
ProcessEvent(player, addAdam, &parms, nullptr);
```

**Tick Callback:**
```cpp
#include "engine/world.h"
int tickId = RegisterTickCallback([](float dt) {
    // Runs every frame — do your per-frame mod logic here
});
```

**CDO Editing (global class defaults):**
```cpp
UObject* cdo = GetDefaultObject("Pistol");
// All future Pistol instances will have 99 mag size
SetDefaultProperty("Pistol", "BaseMagazineSize", 99);
```

**Native Hooking (C++ engine functions):**
```cpp
#include "engine/world.h"
NativeFunc original = HookNative(279, [](UObject* ctx, void* stack, void* result) {
    // Hooked Destroy() — runs before any actor is destroyed
    LOG_INFO("Actor destroyed: {}", ctx->GetName());
    original(ctx, stack, result);  // call original
});
```

### Where to Put Your Code

Add your mod to `src/gameplay/`:
```
src/gameplay/
├── gameplay_mods.cpp    ← Main mod system (PE hooks)
├── gameplay_mods.h
├── teleport_plasmid.cpp ← Decoy→Teleport mod
├── teleport_plasmid.h
└── your_mod.cpp         ← Your new mod goes here
```

Register it in `gameplay_mods.cpp` or call it from `dllmain.cpp`.

---

## 7. Distributing Your Mod

### Package for End Users

```powershell
.\scripts\package_mod.ps1 -Name "MyAwesomeMod"
```

This creates `dist\MyAwesomeMod.zip` containing:
```
winmm.dll         ← Proxy loader
BS1SDK.dll        ← Your modded SDK
mod_config.json   ← User-editable settings
scripts/          ← Lua scripts (if any)
INSTALL.txt       ← Instructions
```

### Include Patched Map Files

```powershell
.\scripts\package_mod.ps1 -Name "HardMode" -IncludeBSM -BSMDir "patched_maps"
```

This also includes a `Maps/` folder with your patched `.bsm` files.

### What End Users Do

1. Extract the zip to `Build\Final\`
2. Launch BioShock Remastered
3. Done — your mod loads automatically

---

## 8. Console Command Reference

### Player & Properties
| Command | Description |
|---------|-------------|
| `set <prop> <value>` | Set player property (int or float) |
| `get <prop>` | Read a player property |
| `setobj <class> <prop> <val>` | Set property on first instance of a class |
| `give adam/credits/eve/health <N>` | Quick resource editing |
| `god` | Toggle god mode |
| `props [filter]` | List player properties |

### Movement & Spawning
| Command | Description |
|---------|-------------|
| `spawn <class>` | Spawn any actor at player position |
| `tp <x> <y> <z>` | Teleport to coordinates |
| `noclip` | Toggle fly + no collision |
| `teleport [dist]` | Blink forward (default 800 units) |

### Gameplay Mods
| Command | Description |
|---------|-------------|
| `initmods` | Activate all gameplay mods |
| `mods` | Show active mod status |
| `decoytp [on\|off]` | Toggle Decoy→Teleport |
| `bots [on\|off\|limit N]` | Toggle friendly bots / set limit |
| `rivets [on\|off]` | Toggle Rivet Pistol |
| `factions [on\|off]` | Toggle splicer factions |
| `tag <1\|2>` | Tag nearest splicer to faction |
| `chain [on\|off\|r N\|j N]` | Chain lightning config |
| `saveconfig` | Save settings to JSON |

### ProcessEvent & Functions
| Command | Description |
|---------|-------------|
| `hookpe` | Toggle ProcessEvent hook |
| `logpe [filter]` | Log function calls (toggle) |
| `pestats` | Show PE call statistics |
| `dumpevents` | Dump all events to file |
| `call <class> <func> [args]` | Call any UFunction |
| `funcs <class> [filter]` | List functions on a class |

### World & Actors
| Command | Description |
|---------|-------------|
| `actors [class]` | List level actors |
| `worldinfo` | Show level name + actor count |
| `nearby <radius> [class]` | Find actors within radius |
| `tickrate` | Show engine tick rate |

### Engine Internals
| Command | Description |
|---------|-------------|
| `cdo <class>` | Show ClassDefaultObject address |
| `setdefault <c> <p> <v>` | Set CDO property (affects all future instances) |
| `natives` | Show GNatives table info |
| `inspect <class\|0xAddr>` | Live-inspect object properties |
| `hierarchy` | Dump class inheritance tree |
| `functions` | Dump all UFunction objects |

### SDK Generation
| Command | Description |
|---------|-------------|
| `gensdk` | Regenerate SDK headers |
| `dumpsdk` | Full SDK dump (enums, structs, states, natives, assets) |
| `assets [filter]` | Dump loaded textures, meshes, sounds |

### Lua
| Command | Description |
|---------|-------------|
| `lua <code>` | Execute Lua inline |
| `luafile <path>` | Execute a Lua script file |
| `reload` | Hot-reload autorun.lua |

### Co-op (Experimental)
| Command | Description |
|---------|-------------|
| `host [port]` / `join <ip> [port]` | Ghost co-op session |
| `truehost` / `truejoin <ip>` | True co-op (WIP) |
| `chat <msg>` | Send message to co-op partner |
| `netstatus` | Connection info |
| `disconnect` | Leave session |

---

## 9. Lua API Reference

| Function | Returns | Description |
|----------|---------|-------------|
| `sdk.log(msg)` | — | Print to SDK log |
| `sdk.getProperty(class, prop)` | value, err | Read int/float/bool property |
| `sdk.setProperty(class, prop, val)` | ok, err | Write property |
| `sdk.callFunction(class, func, ...)` | ok, err | Invoke any UFunction |
| `sdk.findObjects(filter)` | table | Search objects by name/class |
| `sdk.on(funcName, callback)` | hookId | Hook ProcessEvent (return true to block) |
| `sdk.off(hookId)` | — | Remove event hook |
| `sdk.hookpe()` | — | Enable ProcessEvent interception |
| `sdk.getActors([class])` | table | All actors (optionally filter by class) |
| `sdk.getActorPosition(addr)` | x,y,z | Get actor position |
| `sdk.setActorPosition(addr, x,y,z)` | — | Move an actor |
| `sdk.getActorsInRadius(x,y,z,r)` | table | Spatial query |
| `sdk.getPlayerPos()` | x,y,z | Player position |
| `sdk.actorDistance(a, b)` | float | Distance between actors |
| `sdk.getLevelInfo()` | table | Level name + actor count |
| `sdk.getDefaultObject(class)` | addr | ClassDefaultObject pointer |
| `sdk.setDefault(class, prop, val)` | ok | Modify CDO (global effect) |
| `sdk.getFunctions(class)` | table | List functions with flags |
| `sdk.getEnum(name)` | table | Enum literal names |
| `sdk.onTick(callback)` | tickId | Register per-frame callback (receives dt) |
| `sdk.offTick(id)` | — | Remove tick callback |
| `sdk.getTickRate()` | float | Current engine FPS |
| `sdk.getNativeCount()` | int | Number of native functions |
| `sdk.getNativeAddress(idx)` | int | Native function pointer |

### Lua Hook Callback Format

```lua
sdk.on("FunctionName", function(objectName, functionName)
    -- objectName: "ShockPlayer_0", "Splicer_12", etc.
    -- functionName: "TakeDamage", "Died", "UseAbility", etc.
    -- Return true to BLOCK the event, false to allow it
    return false
end)
```

### Useful Events to Hook

| Event Name | Fires When | Useful For |
|------------|------------|------------|
| `TakeDamage` | Any actor takes damage | God mode, damage modifiers |
| `Died` | Any actor dies | Kill counters, loot drops, vampirism |
| `UseAbility` | Player uses plasmid/tonic | Plasmid hijacks, cooldown mods |
| `PickedUp` | Item is collected | Item tracking, custom rewards |
| `Fire` | Weapon fires | Ammo mods, custom projectiles |
| `Reload` | Weapon reloads | Reload speed mods |
| `PlayerTick` | Every player frame | Movement mods, HUD updates |

---

## 10. Tips & Tricks

### Finding Class & Property Names

```
classes Splicer          Find all splicer-related classes
props Health             Find all properties named "Health"
funcs ShockPlayer        List all functions on ShockPlayer
inspect ShockPawn        Live-view all properties on enemy pawns
hierarchy                Full class tree (saved to file)
dumpsdk                  Generate complete typed SDK headers
```

### Discovering What Functions Do

```
logpe TakeDamage         Log every TakeDamage call + which object
logpe UseAbility         Watch plasmid usage
logpe Fire               Watch weapon firing
dumpevents               Record ALL function calls to file (warning: big)
```

### Modding Workflow

1. **Explore:** Use `inspect`, `props`, `funcs` to find the right class/property
2. **Test live:** Use `set`, `setobj`, `call` to test changes in-console
3. **Script it:** Once it works, put it in a `.lua` file
4. **Distribute:** Package with `package_mod.ps1`

### Common Property Names

| Property | Class | Type | What It Does |
|----------|-------|------|-------------|
| `Health` | ShockPlayer / ShockPawn | float | Hit points |
| `ADAM` | ShockPlayer | int | ADAM currency |
| `Credits` | ShockPlayer | int | Dollar currency |
| `BioAmmo` | ShockPlayer | float | EVE (plasmid fuel) |
| `BaseFireRate` | Weapon subclasses | float | Shots per second |
| `BaseMagazineSize` | Weapon subclasses | int | Magazine capacity |
| `BaseAccuracy` | Weapon subclasses | float | Spread (lower = tighter) |
| `BaseReloadRate` | Weapon subclasses | float | Reload speed |
| `Damage` | Ammo subclasses | float | Damage per hit |

### Backing Up Game Files

Before modding `.bsm` or `.ini` files, always back up:
```
ContentBaked\pc\Maps\*.bsm          ← Map files
ContentBaked\pc\System\ConfigINI.IBF ← Config archive
```

### Performance Note

The ProcessEvent hook (`hookpe`) adds overhead to every script function call.
Only enable it when you need it. For Lua mods, call `sdk.hookpe()` at the start
and `sdk.off(hookId)` when done. For live gameplay, `initmods` handles this
efficiently by filtering early.
