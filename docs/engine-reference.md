# BioShock Remastered (UE2.5) Engine Reference

> Living document — updated as we reverse-engineer the engine.
> Last updated: 2026-05-26

## Module Info

| Field | Value |
|-------|-------|
| Executable | `BioshockHD.exe` (32-bit x86) |
| Engine | Unreal Engine 2.5 (modified, shipped DLLs stripped) |
| Shipped DLLs | `amd_ags_x86`, `bink2w32`, `fmodex`, `steam_api` |
| Networking | **Stripped** — IpDrv.dll/IpDrv.U removed, no NetDriver |

## Global Pointers

| Name | How Found | Notes |
|------|-----------|-------|
| `GObjects` | Pattern scan | TArray\<UObject*\>, data + count at +4 |
| `GNames` | Pattern scan | TArray\<FNameEntry*\> |
| `GNatives` | Heuristic scan | 4096-entry function pointer table |

## UObject Layout (base class)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x00 | 4 | VfTable | Virtual function table pointer |
| 0x04 | 4 | ObjectIndex | Index in GObjects |
| 0x28 | 4 | Name (FName) | Index into GNames |
| 0x30 | 4 | Outer | Owning UObject* |
| 0x34 | 4 | ObjectFlags | RF_* flags |
| 0x38 | 4 | Class | UClass* pointer |

## UField / UStruct Layout

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x40 | 4 | SuperField | Parent in class hierarchy |
| 0x44 | 4 | Next | Next field in linked list |
| 0x48 | 4 | Children | First child UField |

## UFunction Layout

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0x64 | 4 | FunctionFlags | uint32 | FUNC_Native=0x400, FUNC_Event=0x800, etc |
| 0x68 | 2 | iNative | uint16 | Index into GNatives table (0 = not native) |
| 0x6A | 1 | OperPrecedence | uint8 | Operator precedence (0 for non-operators) |
| 0x6B | 1 | NumParms | uint8 | Number of parameters |
| 0x6C | 2 | ParmsSize | uint16 | Total size of parameter block in bytes |
| 0x6E | 2 | ReturnValueOffset | uint16 | Offset of return value in parm block |
| 0x70 | 4 | NativeFunc | ptr | Direct function pointer for native dispatch |

### Critical Discovery: NativeFunc can be NULL/invalid

On some UFunctions (notably `Actor::SetLocation`, `Actor::SetRotation`), the `NativeFunc` pointer at +0x70 is **zeroed out or points to garbage memory**. However, the `iNative` index at +0x68 IS valid, and the corresponding `GNatives[iNative]` entry has the correct function pointer.

**Solution**: Before calling ProcessEvent on a native function, patch `NativeFunc` from `GNatives[iNative]`.

## UClass Layout

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x90 | 4 | ClassFlags | CLASS_* flags |
| 0x94 | 4 | ClassWithin | UClass* |
| 0x98 | 4 | ClassConfigName | FName |
| 0xC4 | 4 | CDO | Default object pointer |

## UEnum Layout

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x48 | 4 | Names.Data | TArray\<FName\> pointer |
| 0x4C | 4 | Names.Count | Number of enum values |

## UProperty Layout

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x50 | 4 | ArrayDim | Array dimension |
| 0x54 | 4 | ElementSize | Size of one element |
| 0x58 | 4 | PropertyFlags | CPF_* flags |
| 0x5C | 4 | Offset | Byte offset in owning object |

### Property Subclass Extra Fields

| Class | Offset 0x78 | Offset 0x7C |
|-------|-------------|-------------|
| UByteProperty | UEnum* Enum | — |
| UObjectProperty | UClass* PropertyClass | — |
| UClassProperty | UClass* PropertyClass | UClass* MetaClass |
| UStructProperty | UStruct* Struct | — |
| UArrayProperty | UProperty* Inner | — |
| UMapProperty | UProperty* Key | UProperty* Value |

## GNatives Table

4096-entry array of function pointers: `void (*)(UObject*, FFrame&, void*)`

### Key Native Indices

| Index | Function | Notes |
|-------|----------|-------|
| 4014 | Actor::SetLocation | Moves actor + updates render octree |
| 4343 | Actor::SetRotation | Updates actor rotation |
| 4338 | Actor::Move | Delta movement |
| 4336 | Actor::SetCastShadowMapShadow | |

## ProcessEvent Dispatch

`UObject::ProcessEvent(UFunction*, void* Parms, void* Result)` is a `__thiscall` virtual function.

**Dispatch logic** (to be confirmed by disassembly):
1. Reads `iNative` from UFunction+0x68
2. If `iNative > 0`: dispatches via `GNatives[iNative](this, Frame, Result)`
3. Else if `NativeFunc` at +0x70 is valid: calls `NativeFunc(this, Frame, Result)`
4. Else: executes UnrealScript bytecode

> **TODO**: Confirm this order from `pe_disasm.txt` — the crash at NativeFunc suggests the engine may check NativeFunc BEFORE iNative in this build.

## Actor Movement in UE2

Direct memory writes to `Location` property do NOT update the render octree for `StaticMeshActor`s. You MUST call:
- `Actor::SetLocation(FVector, bool bNoTest)` — native 4014
- `Actor::SetRotation(FRotator)` — native 4343
- `Actor::Move(FVector Delta)` — native 4338

### Physics Enum Values

| Value | Name | Notes |
|-------|------|-------|
| 0 | PHYS_None | |
| 1 | PHYS_Walking | |
| 2 | PHYS_Falling | |
| 5 | PHYS_Flying | |
| 9 | PHYS_Interpolating | Used for movers/puppets |

## Key Class Property Offsets (ShockPlayer)

| Property | Offset | Type |
|----------|--------|------|
| Location | +472 | FVector (3 floats) |
| Rotation | +484 | FRotator (3 int32) |
| Health | +1404 | float |

> These are instance offsets from the UObject base pointer.

## Networking (Custom Implementation)

Since IpDrv is stripped, we use raw Winsock2 UDP:
- Host binds to port 27015 (configurable)
- Client connects to host IP
- State sync at ~30Hz
- Keepalive thread survives alt-tab

### Packet Types

| ID | Name | Direction |
|----|------|-----------|
| 0x50 | WorldStateBatch | Host → Client |
| 0x51 | ActorSpawn | Both |
| 0x52 | ActorDestroy | Both |
| 0x61 | P2State | Both |
| 0x70 | FreezeAck | Host → Client |
| 0x71 | WorldReady | Client → Host |
