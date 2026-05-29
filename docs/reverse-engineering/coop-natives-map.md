# Co-op Native Function Map
## Critical Engine Functions for True Co-op

**Purpose:** Identify exactly which native functions need to be understood/hooked
for each True Co-op phase, and the recommended call strategy for each.

**Last Updated:** May 26, 2026

---

## Current Engine Access (What We Have)

| Capability | Implementation | Location |
|------------|---------------|----------|
| ProcessEvent hook | Full intercept + dispatch | `src/hooks/process_event.h` |
| GNatives table (4096 slots) | Address dump, hook/unhook | `src/engine/world.cpp` |
| Actor iteration | GetAllActors, ForEachActor | `src/engine/world.h` |
| Property read/write | Walk class hierarchy → offset | `GetActorPosition`, `SetActorProperty` |
| SetLocation via PE | SEH-safe ProcessEvent call | `world.cpp:304` |
| UFunction metadata | FunctionFlags, iNative, NativeFunc | `src/engine/uobject.h` |
| CDO access | Read/write class defaults | `uobject.h: GetDefaultObject()` |
| Tick callback | Frame-rate independent | `RegisterTickCallback()` |

---

## __NFUN_XXX__ → Function Name Mapping

These are bytecode native indices used in the decompiled .uc files. Mapped by
analyzing call context in `Controller.uc`, `Actor.uc`, `Pawn.uc`.

### Core Operations (operators)

| Index | Function | Evidence |
|-------|----------|----------|
| 112 | `$` (string concat) | `__NFUN_112__("str1", "str2")` |
| 113 | `GotoState` | `__NFUN_113__('Dead')`, `__NFUN_113__('GameEnded')` |
| 114 | `==` (Object) | `__NFUN_114__(Pawn, none)` |
| 119 | `!=` (Object) | `__NFUN_119__(C, none)` — null checks |
| 129 | `!` (logical NOT) | `__NFUN_129__(bJustLanded)` |
| 130 | `&&` (logical AND) | `__NFUN_130__(a, b)` |
| 132 | `&&` (variant?) | `__NFUN_132__(a, b, c, d)` |
| 150 | `>` (int) | `__NFUN_150__(Rotation.Pitch, 32768)` |
| 154 | `!=` (int) | `__NFUN_154__(int(Role), int(ROLE_Authority))` |
| 171 | `*` (float) | `__NFUN_171__(0.2, FRand())` |
| 172 | `/` (float) | `__NFUN_172__(Dist2D, VSize(...))` |
| 174 | `+` (float) | `__NFUN_174__(Result.Z, ...)` |
| 175 | `-` (float) | `__NFUN_175__(Level.TimeSeconds, OldMessageTime)` |
| 176 | `<` (float) | `__NFUN_176__(diff, float(10))` |
| 184 | `+=` (float) | `__NFUN_184__(YPos, YL)` |
| 195 | `FRand()` | `__NFUN_195__()` |
| 213 | `*` (float × Vector) | `__NFUN_213__(float(512), Vector(Rotation))` |
| 215 | `+` (Vector) | `__NFUN_215__(Location, offset)` |
| 216 | `-` (Vector) | `__NFUN_216__(Dest2D, Start)` |
| 225 | `VSize()` | `__NFUN_225__(delta)` |
| 226 | `Normal()` | `__NFUN_226__(Result)` |
| 254 | `==` (name) | `__NFUN_254__(BroadcastType, 'Global')` |
| 258 | `ClassIsChildOf` | `__NFUN_258__(GetActiveAbilityClass(), Class'...')` |

### Actor Functions (★ = co-op critical)

| Index | Function | GNatives Addr | Evidence |
|-------|----------|---------------|----------|
| ★267 | `SetLocation` | NOT in GNatives | `Pawn.__NFUN_267__(NewLocation)` in ClientSetLocation |
| 272 | `SetOwner` | — | `__NFUN_272__(none)` in UnPossessed |
| ★278 | `Spawn` | `0x10A8771C` | `__NFUN_278__(Class, Owner,, vect(), rot())` in PostBeginPlay |
| ★279 | `Destroy` | `0x1063C5A0` | Confirmed: `native(279)` in Actor.uc |
| 284 | `GetStateName` | — | `__NFUN_284__()` → returns state name |
| ★299 | `SetRotation` | NOT in GNatives | `Pawn.__NFUN_299__(NewRotation)` in ClientSetRotation |
| 303 | `IsA` | `0x1063C6C0` | `C.__NFUN_303__('PlayerController')` |

### Controller Functions

| Index | Function | GNatives Addr | Evidence |
|-------|----------|---------------|----------|
| 508 | `FinishRotation` | — | `native(508)` in Controller.uc |
| 525 | `FindRandomDest` | — | `native(525)` in Controller.uc |
| 527 | `WaitForLanding` | — | `native(527)` in Controller.uc |
| ★529 | `AddController` | — | `native(529)` — called in Controller.PreBeginPlay |
| ★530 | `RemoveController` | — | `native(530)` — called in Controller.Destroyed |

### Other Confirmed Natives

| Index | Function | Class | Evidence |
|-------|----------|-------|----------|
| 301 | `FinishInterpolation` | Actor | `native(301)` |
| 532 | `PlayerCanSeeMe` | Actor | `native(532)` |
| 544 | `ResetKeyboard` | PlayerController | `native(544)` |
| 547 | `GetURLMap` | Actor | `native(547)` |

---

## Critical Function Signatures (from decompiled .uc)

### Actor.Spawn
```unrealscript
function Actor Spawn(
    Class<Actor> SpawnClass,
    optional Actor SpawnOwner,
    optional name SpawnTag,
    optional Vector SpawnLocation,
    optional Rotator SpawnRotation,
    optional bool bNoCollisionFail,
    optional name SpawnLabel
)
```
**GNatives[278] = 0x10A8771C** — This IS in the table and can be hooked.

ProcessEvent params struct (estimated):
```cpp
struct SpawnParms {
    UObject* SpawnClass;      // UClass*
    UObject* SpawnOwner;      // optional
    FName    SpawnTag;        // optional (8 bytes: index + instance)
    float    SpawnLocation[3];// optional FVector
    int32_t  SpawnRotation[3];// optional FRotator (Pitch,Yaw,Roll)
    uint32_t bNoCollisionFail;// optional bool
    FName    SpawnLabel;      // optional
    UObject* ReturnValue;     // Actor*
};
```

### Actor.SetLocation
```unrealscript
function bool SetLocation(Vector NewLocation, optional bool bNoTest)
```
**NOT in GNatives** (267 is empty) — dispatched via UFunction::NativeFunc at +0x70.
Already implemented in `world.cpp:304` via ProcessEvent.

### Actor.SetRotation
```unrealscript
function bool SetRotation(Rotator NewRotation)
```
**NOT in GNatives** (299 is empty) — dispatched via UFunction::NativeFunc.
**Need to implement** — same pattern as SetLocation in world.cpp.

### Actor.Trace
```unrealscript
function Actor Trace(
    out Vector HitLocation,
    out Vector HitNormal,
    Vector TraceEnd,
    optional Vector TraceStart,
    optional bool bTraceActors,
    optional Vector Extent,
    optional out Material Material,
    optional bool bCheckSkeletalBoneModels,
    optional out Actor.ESkeletalRegion SkeletalRegionHit
)
```
**Call via ProcessEvent** on any Actor.

### Actor.Destroy
```unrealscript
native(279) final function bool Destroy()
```
**GNatives[279] = 0x1063C5A0** — simple, no params.

### Controller.Possess / UnPossess
```unrealscript
// NOT native — these are script functions
function Possess(Pawn aPawn)
{
    aPawn.PossessedBy(self);
    Pawn = aPawn;
    // set PlayerReplicationInfo.bIsFemale, FocalPoint, etc.
    Restart();
}

function UnPossess()
{
    Pawn.UnPossessed();
    Pawn = none;
}
```
**These are pure script** — callable via ProcessEvent.

### IDamagee.TakeDamage (BioShock-specific)
```unrealscript
function TakeDamage(
    DamageStimuliSet DamageStimuli,
    float CritChance,
    Actor Damager,
    Vector HitLocation,
    Vector HitNormal,
    Vector HitImpulseDirection,
    name EffectEventName,
    float DamageAttenuation,
    name HitHighBone,
    name HitLowBone,
    bool WasMeleeAttack
)
```
**Note:** BioShock uses `DamageStimuliSet` instead of standard UE2 damage types.
Need to find this class's layout to construct valid params.

---

## Per-Phase Native Requirements

### Phase 1: Client AI Freeze ✅ ALREADY CODED
**Natives needed: NONE** — Uses ProcessEvent hook to block function calls.
Implementation: `coop_true.cpp:83` `ShouldBlockFunction()`

### Phase 2: World State Broadcast
**Host side:**
- `GetActorPosition()` — ✅ Already works (property read)
- Actor rotation read — **Need:** Read Rotation property (same pattern as Location)
- Health property read — ✅ Already works in co-op bridge

**Client side:**
- `SetActorPosition()` → `SetLocation` via PE — ✅ Already works
- `SetActorRotation()` — **Need to add** (same pattern as SetLocation)
- Health property write — **Need:** `SetActorProperty("Health", ...)`

**New code needed:** `SetActorRotation()` function in world.cpp (clone SetActorPosition pattern, find SetRotation UFunction, call via PE).

### Phase 3: P2 Pawn Spawn + Input
**Spawn P2 pawn:**
- `Spawn()` via ProcessEvent — **GNatives[278] = 0x10A8771C**
  - Call on existing player actor: `player->ProcessEvent(SpawnFunc, &parms)`
  - Use `DecoyHuman` class (already proven in teleport mod) OR `ShockAI` pawn
  - Fallback: find existing NPC and commandeer

**Drive P2 movement:**
- `SetLocation` via PE — ✅ Already works
- `SetRotation` via PE — Need to add (see Phase 2)
- `SetPhysics(PHYS_None)` — Disable physics on P2 pawn so host-driven position isn't fought

**P2 input → position:**
- Convert client input to delta movement on host
- Apply via SetLocation at 60Hz
- No need to understand MoveActor internals — just teleport

### Phase 4: P2 Combat
**Hitscan (P2 fires weapon):**
- `Trace()` via ProcessEvent — Cast ray from P2 position in aim direction
- On hit: Call `TakeDamage()` on hit actor via ProcessEvent
- Need DamageStimuliSet object — find existing ones at runtime via GObjects

**Projectile (P2 fires projectile weapon):**
- `Spawn()` projectile via ProcessEvent
- Set projectile Velocity/Direction properties

### Phase 5: Inventory
**P2 weapon switching:**
- Read/write `Weapon` property on P2 pawn
- Read available weapons from ShockPlayer inventory arrays
- **May need deeper RE** of ShockPlayer inventory layout (~210KB .uc file)

### Phase 6: Transitions
**Level change:**
- Hook `ServerTravel` or `SwitchLevel` via ProcessEvent
- Already have level name from handshake packets
- Trigger `open <mapname>` console command on client

---

## Recommended Implementation Order

| Step | What | Natives/Functions | Effort |
|------|------|-------------------|--------|
| 1 | Add `SetActorRotation()` | SetRotation UFunction via PE | 1 hour |
| 2 | Add `GetActorRotation()` | Read Rotation property | 1 hour |
| 3 | Test Phase 1 freeze on 2nd PC | None new | 1 day |
| 4 | Implement Phase 2 world sync | GetPos/Rot + SetPos/Rot | 2-3 days |
| 5 | Spawn P2 pawn via Spawn PE | GNatives[278] | 2-3 days |
| 6 | Drive P2 via SetLocation 60Hz | Already have | 1 day |
| 7 | P2 hitscan via Trace PE | Trace UFunction | 2-3 days |
| 8 | P2 TakeDamage forwarding | TakeDamage UFunction | 2-3 days |

**Total new RE needed: ~5 functions (SetRotation, Spawn params, Trace params, TakeDamage params, SetPhysics)**

**Total new RE NOT needed:** Move/MoveActor internals, physics pipeline, Controller binding, navigation, full engine decompilation.

---

## Key Insight

SetLocation (267) and SetRotation (299) are **missing from GNatives** but their
UFunction objects still have valid `NativeFunc` pointers at +0x70. The existing
`SetActorPosition()` in world.cpp already demonstrates this works — it finds the
SetLocation UFunction, validates NativeFunc, and calls via ProcessEvent. The same
pattern works for SetRotation, Spawn, Trace, and TakeDamage.

**We do NOT need to reverse-engineer the native function internals.** We call them
as black boxes through ProcessEvent with correctly structured param buffers.
