# BS1SDK — Runtime Engine Access Capabilities

This document summarizes what data the BS1SDK provides at runtime from inside the running BioShock Remastered process. The SDK is a native DLL injected into the game that hooks into engine internals.

---

## 1. ProcessEvent Hook — Full Script Function Interception

Every UnrealScript function call in the engine goes through `ProcessEvent`. We hook this and can:

- **Intercept any function call** before/after execution
- **Read/modify parameters** in the function's parameter buffer
- **Block execution** by skipping the original call
- **Log all events** with object context, function name, and flags

Example output from our event logger:
```
[PE] ShockPlayerController.PlayerTick (Native|Event) dt=0.016
[PE] ShockPawn.Tick (Native|Event) dt=0.016
[PE] StaticMeshActor.Bump (Event) Other=ShockPawn
```

---

## 2. UFunction Discovery — Full Function Metadata

For every UClass in the engine, we enumerate all UFunctions and extract:

| Field | Offset | Description |
|-------|--------|-------------|
| FunctionFlags | +0x64 | Bitmask: Native(0x400), Event(0x800), Static(0x2000), Exec(0x200), Latent(0x08) |
| iNative | +0x68 | Native dispatch table index (0 = bytecode, >0 = GNatives slot) |
| OperPrecedence | +0x6A | Operator precedence for operator functions |
| NumParms | +0x6B | Number of parameters |
| ParmsSize | +0x6C | Total byte size of parameter block |
| ReturnValueOffset | +0x6E | Offset to return value in parm block |
| NativeFunc | +0xC0 | Actual native function pointer (for native functions) |

### Full Signature Resolution

We walk each UFunction's Children chain to find UProperty parameters and resolve full typed signatures:

```
// Example output from SDK generator for class ShockPlayerController:
void PlayerTick(float DeltaTime)  [Native|Event]
void PlayerCalcView(out Actor ViewActor, out Vector CameraLocation, out Rotator CameraRotation)  [Event|Simulated]
bool FindPathToward(Actor Anch)  [Native]  iNative=518
void ConsoleCommand(String Command)  [Exec]
void Fire()  [Exec]
void AltFire()  [Exec]
Weapon GetActiveWeapon()  [Native]
```

### Property Type Resolution

For parameters, we resolve full types including:
- **UByteProperty** → resolves Enum name at +0x78 (e.g. `EPhysics`, `ENetMode`)
- **UObjectProperty** → resolves PropertyClass at +0x78 (e.g. `Actor`, `Pawn`)
- **UClassProperty** → resolves MetaClass at +0x7C
- **UStructProperty** → resolves Struct name at +0x78 (e.g. `FVector`, `FRotator`, `FColor`)
- **UArrayProperty** → resolves Inner type at +0x78 (e.g. `TArray<Actor>`)
- **UMapProperty** → resolves Key/Value at +0x78/+0x7C

---

## 3. GNatives Table — 4096-Entry Native Dispatch

The engine has a global `void (*GNatives[4096])(UObject*, FFrame&, void*)` dispatch table. We discover its address at runtime and provide:

- **Full table dump** — every populated slot with absolute address and module offset
- **Hook any native** by index — replace the function pointer, get the original back
- **Unhook** — restore originals at any time

Example dump:
```
=== GNatives Table Dump ===
Table address: 0x10B8A000
Module base:   0x00400000

  Native[   0] = 0x00763F10 (+0x363F10)   // GotoState
  Native[   1] = 0x00764A20 (+0x364A20)   // ==
  Native[ 113] = 0x0076E890 (+0x36E890)   // Spawn
  Native[ 222] = 0x007A5C40 (+0x3A5C40)   // IsA
  Native[ 281] = 0x0079BA30 (+0x39BA30)   // MoveSmooth
  Native[ 518] = 0x008C1F50 (+0x4C1F50)   // FindPathToward
  ...
Total populated: 347 / 4096
```

You can correlate iNative index from UFunction metadata with this table to get the exact native function address for any script-exposed native.

---

## 4. Full Type System / Class Hierarchy

### UObject Layout (confirmed offsets, x86):
```
+0x00: VTable*
+0x04: ObjectInternalIndex (8B, contains GObjects index)
+0x0C: HashNext*
+0x10: StateFrame*
+0x14: Linker*
+0x18: LinkerIndex
+0x1C: Outer*
+0x20: ObjectFlags (QWORD)
+0x28: FName (Index + Number)
+0x30: Class*
+0x34: ObjectArchetype*
+0x38: CheckpointType
+0x3C: Padding
```

### UField (+0x40): SuperField, Next
### UStruct (+0x5C): Children, PropertiesSize
### UProperty (+0x4C): ArrayDim, ElementSize, PropertyFlags, Offset(+0x74)
### UFunction (+0x64): FunctionFlags, iNative, NumParms, ParmsSize, NativeFunc(+0xC0)
### UClass (+0x90): ClassFlags, ClassWithin, ClassConfigName, CDO(+0xC4)

### CDO Access
We can read/write the Class Default Object for any class:
```lua
-- Example: change default damage for a weapon class
sdk.setDefault("ShockPistol", "Damage", 50.0)
```

---

## 5. World System — Runtime Actor Access

- **GWorld / GEngine / ULevel** discovered at runtime
- **TArray<AActor*> Actors** offset found via heuristic scan
- **Actor enumeration**: all actors, by class, in radius
- **Direct property read/write** on any actor (Location, Rotation, Health, etc.)
- **Spatial queries**: find actors within radius of a point

```
// Runtime stats from a loaded level:
Level actors: 8,563
Level name: 1-Medical
```

---

## 6. Engine Tick Hook

We hook `UGameEngine::Tick` via vtable replacement (heuristic: largest float-using function in vtable[25-60]):

- Frame-rate-independent callbacks at engine tick rate (~60Hz)
- Measured tick rate reported
- Multiple callbacks supported with registration/unregistration
- SEH-safe execution (bad callbacks don't crash the game)

---

## 7. SDK Generator Output

The `gensdk` command produces a full header dump of every class:

- Per-package `.h` files with all classes, properties, functions
- Property offsets + types + array dimensions
- Function signatures with full parameter types
- Enum definitions with all values
- CDO default values
- State definitions
- Struct layouts
- Class hierarchy (inheritance chain)

Total coverage: ~750+ classes, ~8000+ properties, ~3000+ functions across Engine, Core, ShockGame, etc.

---

## 8. Lua Scripting Bridge

All of the above is exposed to runtime Lua scripts:

```lua
-- Get player position
local pos = sdk.getPlayerPos()

-- Find nearby actors
local nearby = sdk.getActorsInRadius(pos.x, pos.y, pos.z, 1000)

-- Enumerate functions on a class
local funcs = sdk.getFunctions("ShockPlayerController")
for _, f in ipairs(funcs) do
    print(f.name, f.signature, f.flags)
end

-- Hook a native function
sdk.hookNative(113, function(ctx, stack, result)
    print("Spawn called by: " .. ctx:getName())
end)

-- Per-tick logic
sdk.onTick(function(dt)
    -- runs every frame
end)
```

---

## What This Enables

For your BSM parser / level editor work specifically:

1. **Verify property offsets at runtime** — if you're unsure about a field offset in a class, we can read it live from CDOs or actor instances
2. **Discover what functions manipulate specific data** — hook ProcessEvent and see what gets called when you e.g. open a door, move an actor, trigger a script
3. **Validate native function behavior** — hook any of the 347 populated GNatives slots to log parameters and return values
4. **Cross-reference class hierarchies** — our SDK generator outputs the full inheritance tree which you could compare against your rollup.txt
5. **Test property values** — read actual game-state property values to verify your parser's interpretation of tagged properties
6. **Map iNative indices to function names** — correlate our GNatives dump with UFunction metadata to identify what each native slot does

---

## Source Repository

The full SDK is at: https://github.com/NykoDesigns/Bioshock1REMSDK (or whatever access you'd like)

Key files:
- `src/engine/uobject.h` — All UObject/UField/UStruct/UFunction/UClass layouts
- `src/engine/world.h/.cpp` — GNatives, Tick hook, Actor access
- `src/sdk/sdk_generator.cpp` — Full SDK dump generation
- `src/hooks/process_event.h/.cpp` — PE hook with filtering
- `src/lua/lua_bridge.cpp` — All Lua-exposed APIs
