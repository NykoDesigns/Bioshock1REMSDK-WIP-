# BS1SDK Architecture

## Overview

BS1SDK is an unofficial modding framework for BioShock 1 Remastered (2016).
The game runs on a heavily modified Unreal Engine 2.5 with proprietary systems.

## Target Binary

- **Executable**: BioshockRemastered.exe (or Bioshock.exe)
- **Architecture**: x86 (32-bit)
- **Compiler**: MSVC (Visual C++)
- **Graphics API**: Direct3D 9
- **Engine**: Unreal Engine 2.5 Vengeance (Irrational Games fork)
- **Map Format**: .bsm (BioShock Map - proprietary)

## Vengeance Engine Context

The Vengeance Engine is Irrational Games' named fork of UE2.5, shared with:
- SWAT 4 (2005) — has active modding community with RE knowledge
- Tribes: Vengeance (2004) — some community modding exists

Key Vengeance-specific systems vs stock UE2.5:
- Enhanced AI (behavior trees, sensory model)
- Havok physics integration
- Custom audio pipeline
- Level streaming system (.bsm sublevel architecture)
- Per-pixel lighting / advanced shaders
- UnrealScript VM retained (ProcessEvent still valid)
- Custom HUD/UI framework

Cross-referencing SWAT 4 SDK knowledge and Tribes: Vengeance modding
discoveries is a primary research strategy.

## Injection Methods

### 1. DLL Injector (Development)
- LoadLibraryA injection via CreateRemoteThread
- Used during development for quick iteration
- Requires running injector after game starts

### 2. Proxy DLL (Release)
- Ship as `d3d9.dll` or `dinput8.dll` in game directory
- Game auto-loads our DLL thinking it's the real one
- We forward all calls to the real DLL + initialize SDK
- Zero-friction for end users

### 3. ASI Loader (Alternative)
- Place as .asi file with a generic ASI loader
- Common in GTA/other moddable games

## Core Systems

### Memory & Pattern Scanning
- `Memory` class provides safe read/write with VirtualProtect
- `Pattern` class scans for byte signatures with wildcards
- Patterns are preferred over hardcoded addresses for version resilience

### Hook Framework
- MinHook for inline function hooks
- VTable patching for D3D9 and UObject virtuals
- IAT hooks for Win32 API interception
- All hooks are removable for clean unload

### Engine Reconstruction
- Progressive discovery of UE2.5 internals
- GObjects/GNames provide the object graph
- UObject vtable gives access to ProcessEvent, Serialize, etc.
- Offset-based access until structures are fully confirmed

## Key Design Decisions

1. **Offset-based access over struct definitions** - Until we're 100% confident in a struct layout, use `GetField<T>(offset)` rather than defining members. This prevents subtle memory corruption.

2. **Pattern scanning over hardcoded addresses** - Game updates can shift code. Patterns survive minor patches.

3. **Separate DLL thread** - Never do heavy work in DllMain. Spawn a thread and wait for the game to initialize.

4. **D3D9 dummy device for vtable** - We create a temporary D3D9 device to extract the vtable, then hook the real device's functions.

5. **Log everything during RE** - Aggressive logging helps identify what's happening when things go wrong.

## File Formats

### .bsm (BioShock Map)
- Confirmed standard Unreal Package format (magic 0x9E2A83C1, version 141, licensee 56)
- Contains: BSP geometry (UModel), StaticMeshes, actors, scripts, lighting, navigation
- Compressed via zlib chunks
- FBspNode: 100 bytes (vs stock UE2 64 bytes) — ZoneMask expanded to 128-bit, iZone[0] at +77, NumVertices at BYTE +78 (NOT INT32 +88)
- FBspSurf: 8B Vengeance header + CI Material + 24B fixed fields + CI Actor + FPlane(16B) + LightMapScale(4B). PanU/PanV SKIPPED for version >= 78
- FVert: 8B each (INT32 pVertex + INT32 iSide)
- StaticMeshInstance exports reference BulkContent (.blk) for gameplay mesh data
- See `bioshock1-bsm.md` for full specification, `docs/reverse-engineering/bsm-format.md` for early RE notes

## Build Requirements

- Visual Studio 2022
- CMake 3.20+
- Windows SDK (for D3D9 headers)
- **Must build as x86 (32-bit)** - game is 32-bit
