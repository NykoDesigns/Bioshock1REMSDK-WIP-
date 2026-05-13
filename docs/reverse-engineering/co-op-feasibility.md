# BioShock 1 Remastered — Co-op Multiplayer Feasibility Study

## Status: Research Phase

## Executive Summary

BioShock 1 Remastered is a **singleplayer-only** game built on the Vengeance Engine (UE2.5 fork).
The networking subsystem (IpDrv, NetDriver, NetConnection) has been **completely stripped** from the
shipping build. There is no multiplayer code to reactivate. Co-op must be built entirely from
scratch using our SDK's runtime hooking capabilities.

This document analyzes what exists, what's missing, and the viable architectural paths.

## Engine Networking Analysis

### What Exists in Engine.U (compiled script)

The following UE2 networking **class definitions** still exist:

| Class/Property | Present | Notes |
|----------------|---------|-------|
| `PlayerController` | YES | Full class with `ReplicateMove`, `SavedMoves` |
| `GameInfo` | YES | Has `Login`, `RestartPlayer`, `ServerTravel`, `InitGame` |
| `ReplicationInfo` | YES | Base replication class |
| `PlayerReplicationInfo` | YES | `PlayerName`, `Team`, etc. |
| `GameReplicationInfo` | YES | `PRIArray` (player info array) |
| `LevelInfo.NetMode` | YES | ByteProperty — standalone/listen/dedicated enum |
| `RemoteRole` | YES | In ShockGame.U name table |
| `NetConnection` | YES | Name exists in Engine.U |

### What Is MISSING

| Component | Status | Impact |
|-----------|--------|--------|
| `IpDrv.dll` / `IpDrv.U` | NOT SHIPPED | No TCP/UDP networking driver at all |
| `NetDriver` native class | STRIPPED | Cannot create network connections |
| `UChannel` system | STRIPPED | No replication channels |
| `UNetConnection` native | STRIPPED | No client/server connection objects |
| Any `.dll` with socket code | ABSENT | Only 4 DLLs: amd_ags, bink2w, fmodex, steam_api |

**Conclusion:** The UE2 replication system cannot be reactivated. The script-side classes exist
as empty shells, but the native C++ networking implementation is not present in the executable.

### What The Previous Co-op Mod Attempted (Z:\Bioshock1Co-opMod)

The earlier Python-based co-op mod prototype used:
- **pymem** for external process memory reading/writing
- AOB pattern scanning to find player position, health, etc.
- Pointer chain discovery (base RVA `0x99FD48` → offsets)
- Plans for UDP state sync between two game instances

**Problems with that approach:**
1. External memory access is fragile (pointers shift between sessions)
2. Cannot intercept game events (damage, pickups, triggers)
3. Cannot spawn/control a second player avatar
4. No way to render Player 2 visually from outside the process
5. No ProcessEvent access for game logic interception

### Why BS1SDK Is the Right Foundation

Our SDK already has everything the Python mod lacked:

| Capability | Python Mod | BS1SDK |
|-----------|-----------|--------|
| Stable object access | Pointer chains (fragile) | GObjects/GNames (engine-native) |
| Event interception | None | ProcessEvent hook (all UFunctions) |
| Property read/write | Raw memory | UObject reflection (type-safe) |
| Visual rendering | External overlay (separate window) | In-process D3D11 ImGui |
| Actor spawning | Impossible | ProcessEvent → SpawnActor |
| NPC control | Impossible | Set Location/Rotation via reflection |
| Game state awareness | Guesswork | Full class hierarchy, all actors visible |

## Viable Co-op Architectures

### Architecture A: "Ghost Co-op" (Simplest — MVP)

Two game instances, each running BS1SDK with a network bridge module.

```
┌──────────────────────┐        UDP (30-60Hz)        ┌──────────────────────┐
│  Player 1 (Host)     │◄──────────────────────────► │  Player 2 (Guest)    │
│  BioshockHD.exe      │                             │  BioshockHD.exe      │
│  ┌────────────────┐  │   Position, Rotation,       │  ┌────────────────┐  │
│  │ BS1SDK.dll     │  │   Health, Weapon, EVE,      │  │ BS1SDK.dll     │  │
│  │ + coop_net.cpp │  │   Damage events, Pickups    │  │ + coop_net.cpp │  │
│  └────────────────┘  │                             │  └────────────────┘  │
│                      │                             │                      │
│  P2 = NPC puppet     │                             │  P1 = NPC puppet     │
│  (visual only)       │                             │  (visual only)       │
└──────────────────────┘                             └──────────────────────┘
```

**How it works:**
1. Each player runs their own full game instance
2. BS1SDK hooks ProcessEvent to capture player state every tick
3. A UDP socket module sends position/rotation/weapon/health to the other player
4. On the receiving side, an NPC is hijacked (or spawned) and its Location/Rotation
   are set to match the remote player's state
5. Each player sees a representation of the other player in their world

**Player 2 avatar options:**
- **NPC Puppet**: Find a Splicer or other humanoid in the level, set it friendly,
  override its Location/Rotation every frame. Cheapest approach.
- **Spawned Actor**: Use ProcessEvent to call SpawnActor for a specific pawn class,
  then puppet it. More control but riskier.
- **D3D11 Billboard**: Render a marker/sprite at P2's position via ImGui 3D overlay.
  Simplest visually but no physical presence.

**Pros:** Each player has their own full game state; no engine-level networking needed.
**Cons:** Two separate worlds — enemies, physics, triggers are independent. "Shared
experience" rather than true co-op.

### Architecture B: "Synced Co-op" (Medium Complexity)

Extends Architecture A with game state synchronization:

**Sync layers (incremental):**
1. **Position sync** — See each other moving (Architecture A baseline)
2. **Damage sync** — Hook `TakeDamage` ProcessEvent; forward damage to remote enemy
3. **Kill sync** — When an enemy dies on Host, kill it on Guest too
4. **Door/trigger sync** — Hook door interactions, forward to remote
5. **Pickup coordination** — Prevent double-looting; designate item ownership
6. **Enemy AI sync** — Host is authority for enemy positions; Guest receives updates

**Key technical requirements:**
- ProcessEvent hooks for: `TakeDamage`, `Destroyed`, `TriggerEvent`, `UsedBy`
- Ability to locate the "same" enemy on both instances (match by class + position hash)
- Host/Guest authority model (Host = source of truth for shared state)

### Architecture C: "True Co-op" (Maximum Complexity)

Single game instance acts as authoritative server; second player joins via injected networking.

**This would require:**
- Implementing a custom NetDriver (C++ native code in the DLL)
- Hooking the engine's actor replication system
- Spawning a second PlayerController + Pawn
- Splitting input between two controllers
- Camera management for split-screen or networked view

**Feasibility: VERY LOW** — The replication system's native code is stripped. Reimplementing
`UNetDriver`, `UNetConnection`, and the channel system from scratch would be enormous
(thousands of lines of complex C++) and would fight against engine assumptions about
singleplayer-only execution.

## Recommended Approach: Architecture A → B (Incremental)

### Phase 1: Network Transport Layer
- Add Winsock2 UDP module to BS1SDK (`src/network/`)
- Lobby system: Host broadcasts, Guest discovers via direct IP
- Packet protocol: header (type, seq, timestamp) + payload
- ~30Hz tick rate for state sync

### Phase 2: Player State Broadcasting
- Hook ProcessEvent tick to capture ShockPlayer position, rotation
- Serialize: Position (3×float), Rotation (3×int32), Health, EVE, CurrentWeapon
- Send to remote peer every tick (~33ms)

### Phase 3: Remote Player Visualization
- On receiving side: find or spawn a puppet NPC
- Set puppet Location/Rotation from received packets
- Set puppet to friendly disposition (no AI combat)
- Interpolate between packets for smooth movement
- Hide puppet's AI behavior (disable think/patrol)

### Phase 4: Damage Synchronization
- Hook `TakeDamage` on enemies — when Host damages an enemy, send damage event
- Guest finds matching enemy (by class + nearest position) and applies same damage
- Host is authority: if enemy dies on Host, force-kill on Guest

### Phase 5: World State Sync
- Door/button interactions → forward trigger events
- Pickup collection → notify remote to remove item
- Story triggers → sync progression markers

## Technical Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Two game instances have different enemy positions | HIGH | Host-authority model; sync enemy state periodically |
| NPC puppet looks wrong (wrong model, animations) | MEDIUM | Test with multiple NPC types; use Splicer models |
| UDP packet loss causes desync | MEDIUM | Sequence numbers, periodic full-state snapshots |
| Game saves conflict between players | LOW | Each player has independent save state |
| Performance impact of 30Hz state serialization | LOW | ProcessEvent hook already runs every tick |
| Anti-cheat/Steam interference | LOW | Singleplayer game; no VAC or anti-cheat present |

## Data From Previous Co-op Mod Research

The Python-based scanner at `Z:\Bioshock1Co-opMod` discovered:

- **Player position**: 3×float32 at dynamic heap address (confirmed via movement detection)
- **Pointer chain**: `BioshockHD.exe+0x99FD48 → +0x24C → +0x119` (to player position)
- **Duplicate positions**: At +0x60 (Pawn.Location), +0x740 (Camera/Controller)
- **Memory layout**: Standard UE2 UObject → AActor → APawn → ShockPlayer hierarchy

**Note:** With BS1SDK we don't need pointer chains — we access Location directly via
`UObject::GetField<FVector>(offset)` on the ShockPlayer instance from GObjects.

## Community Research

- No known working BioShock 1 co-op mod exists (as of 2026)
- A Steam group "Bioshock Multi-Player Mod" has been dormant since ~2010
- Reddit consensus: "impossible without source code" — BS1SDK changes this
- BioShock 2 Remastered has multiplayer code but it's a completely separate mode
- The closest precedent: System Shock 2 co-op (native engine support, not modded in)

## References

- `Z:\Bioshock1Co-opMod/` — Previous Python-based co-op prototype (memory scanning approach)
- `Z:\Bioshock1SDK/src/hooks/` — ProcessEvent hook infrastructure
- `Z:\Bioshock1SDK/src/engine/uobject.h` — UObject reflection for property access
- Engine.U exports: PlayerController, GameInfo, ReplicationInfo classes
- ShockGame.U exports: ShockPlayer, ShockPawn, Weapon classes
