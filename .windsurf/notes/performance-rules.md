# BS1SDK Performance Rules & Critical Notes

## The #1 Rule: No Unfiltered PE Hooks

ProcessEvent fires ~5000 times per frame. The hook dispatch runs for EVERY call.

**NEVER register a PE hook with empty `FunctionFilter`.**

Violations cause 3-4 FPS. Every callback that does `func->GetName()` or `obj->GetObjClassName()` performs a heap allocation. At 5000 calls/frame × N hooks = catastrophic.

### What's acceptable:
- `hook.FunctionFilter = "TakeDamage"` — only fires for that exact function
- Zero-allocation FName compare as the FIRST operation in callback (bail immediately if no match)
- No PE hook at all — use tick callbacks + direct memory reads instead

### What's NOT acceptable:
- `hook.FunctionFilter = ""` (fires on everything)
- `hook.FunctionFilter = "*"` (fires on everything)
- `func->GetName()` anywhere in a PE callback hot path
- `obj->GetObjClassName()` anywhere in a PE callback hot path
- `IsSafeToRead()` in PE callbacks (SEH probe on every call)

---

## Architecture: How Ticking Works

| Layer | Mechanism | Frequency | Use For |
|-------|-----------|-----------|---------|
| D3D Present hook | Overlay Render() | Every frame | CoopTick, UI |
| CoopTick | Called from Render() | Every frame | Network, state sync, puppet |
| TrueCoopTick | Called from CoopTick | Every frame | World sync, P2 |
| PE Tick hook | Registered via PE system | Every "Tick" PE call | AVOID — adds dispatch overhead |

**Prefer:** D3D Present → CoopTick → direct function calls
**Avoid:** RegisterTickCallback (requires PE hook active)

---

## Systems Currently Disabled (FPS reasons)

All code preserved. Re-enable only after rewriting to use filtered hooks or polling.

| System | File | Why Disabled |
|--------|------|--------------|
| CoopSync (5 hooks) | coop_sync.cpp | Empty FunctionFilter, GetName() in callback |
| EconomySync | coop_economy.cpp | Empty FunctionFilter, GetName() + GetObjClassName() |
| TransitionHooks (3) | coop_transitions.cpp | Empty FunctionFilter, GetName() in callback |
| GameplayMods | gameplay_mods.cpp | Empty FunctionFilter, GetName() in callback |
| PlasmidHijacks | teleport_plasmid.cpp | Empty FunctionFilter, GetName() in callback |
| EventCatalog | coop_debug.cpp | Empty FunctionFilter, records ALL PE calls |
| StateDiff | coop_debug.cpp | Same as above |

### How to re-enable safely:
1. Split each hook into MULTIPLE hooks with specific FunctionFilters
   - e.g., GameplayMods → one hook for "TakeDamage", one for "UseAbility", one for "Fire"
2. OR: Replace PE hooks with tick-based polling (check state every N frames)
3. OR: Use zero-alloc FName index comparison as first operation, bail if no match

---

## Co-op Architecture (What Works at 60 FPS)

```
Game Boot → winmm.dll proxy → BS1SDK.dll loaded
  → InitProcessEventHook (MinHook on PE function)
  → PE dispatch: if no hooks registered → fast path (just call original)

User types 'truehost' →
  → SetTrueCoopRole(TrueHost)
    → EnsureSubsystemsReady() [no PE hooks]
  → CoopHost(port)
    → InitCoopBridge()
      → CachePlayerOffsets()
      → InitCoopRender() [no PE hooks — just overlay drawing]
      → InitGhostPuppet() [no PE hooks — direct memory writes]
      → NO sync hooks, NO economy hooks

Every frame (D3D Present) →
  → CoopTick(deltaTime)
    → TrueCoopTick(deltaTime)
    → ReadLocalPlayerState() [direct memory read]
    → SetLocalCamera()
    → NetSendPlayerState() [at 30Hz]
    → CoopSyncProcessPackets()
```

Zero PE hooks registered during co-op = PE dispatch hits empty fast path = zero overhead.

---

## Memory Layout Quick Reference

- UObject FName index: +0x18 (OFFSET_NAME)
- UObject Class pointer: +0x30
- UFunction flags: +0x64
- UClass CDO: +0xC4
- Player Location: cached via WalkProperties (varies per class)
- Player Rotation: cached via WalkProperties (varies per class)

---

## Build & Deploy

```powershell
# From z:\Bioshock1SDK
cmake --build build --config Release
# Auto-deploys to D:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final
```

Game must be RESTARTED to load new DLL. Hot-reload not possible.

---

## Remote Log Relay — NOT SAFE YET

The remote_log.cpp system (client→host UDP log relay) caused FPS drops when compiled in.
Source files preserved at `src/debug/remote_log.cpp` / `remote_log.h` but NOT in the build.
Do NOT re-add to CMakeLists.txt without thorough isolated testing.

## DLL Verification

Always confirm the game loaded the correct DLL:
1. Check `LastWriteTime` of deployed DLL matches your build
2. Check the `bs1sdk.log` timestamp in the game dir matches when you launched
3. The game MUST be fully restarted — no hot reload possible

## Config File

`D:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\mod_config.json`

- `autoInitMods: false` — MUST stay false or FPS tanks
- Users can type `initmods` or `initplasmid` in overlay console to opt-in (accepting FPS hit)
