# BS1SDK — BioShock 1 Remastered Unofficial SDK

> A community-driven modding framework for BioShock 1 Remastered (2016)

## Status: Active Development

### Working Features
- **D3D11 ImGui Overlay** — Debug UI rendered via DXGI hook
- **Engine Globals** — Runtime access to GObjects/GNames (UE2.5 Vengeance)
- **Object Inspector** — Browse all live UObjects with property editing
- **Player Properties** — Real-time ADAM, Credits, Health, EVE editing
- **Console** — In-game command console (~) with 20+ commands
- **ProcessEvent Hook** — MinHook inline detour intercepting all UFunction calls
- **Function Caller** — Invoke any UFunction on any object from console
- **Weapon Editor** — Live editing of fire rate, accuracy, magazine size, ammo
- **Gameplay Mods** — God mode, one-hit kill, infinite ammo via PE hooks
- **Event Logger** — Dump all ProcessEvent calls to file for analysis
- **Lua 5.4 Scripting** — Full scripting bridge with hot-reload
- **SDK Generator** — Auto-generate C++ headers from runtime class data

### Planned Features
- Custom maps and level editing
- Asset importing/exporting
- AI editing / custom enemies
- Mod packaging and distribution
- Multiplayer features

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
- `build/bin/Release/bsm_tool.exe` — Standalone .bsm file analyzer

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
call <class> <func> [args]   Call a UFunction
funcs <class> [filter]       List functions on a class
hookpe                       Toggle ProcessEvent hook
logpe [filter]               Log function calls (toggle)
pestats                      Show PE statistics
dumpevents                   Dump all events to file
lua <code>                   Execute Lua code
luafile <path>               Execute Lua script
reload                       Hot-reload autorun.lua
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

## Project Structure
```
BS1SDK/
├── src/
│   ├── core/       — DLL entry, hooks, memory, patterns, logging
│   ├── engine/     — UObject, UProperty, UFunction, function caller
│   ├── hooks/      — DXGI Present hook, ProcessEvent hook (MinHook)
│   ├── render/     — ImGui overlay, mod menu, console
│   ├── scripting/  — Lua 5.4 bridge
│   └── sdk/        — Runtime SDK header generation
├── scripts/        — Example Lua scripts
├── injector/       — Standalone DLL injector
├── tools/          — Offline tools (bsm_tool)
├── external/       — Third-party (MinHook, ImGui, Lua via FetchContent)
└── tests/          — Unit tests
```

## Technical Details

### Engine: Unreal Engine 2.5 Vengeance (Irrational Games fork)
- **UObject size**: 0x40 bytes
- **GObjects**: Static TArray at game base + 0x139042C
- **GNames**: Static TArray at game base + 0x13904EC
- **Graphics**: D3D11 via DXGI (despite d3d9.dll being loaded)
- **ProcessEvent**: Virtual function, hooked via MinHook inline detour

### Confirmed UObject Layout
| Offset | Field |
|--------|-------|
| +0x00 | VTable |
| +0x04 | InternalIndex |
| +0x1C | Outer |
| +0x28 | Name (FName) |
| +0x30 | Class |

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
