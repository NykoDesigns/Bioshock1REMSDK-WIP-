# True Co-op Implementation Plan
## BioShock Remastered — Full Shared-World Multiplayer

**Status:** Planning (not started)  
**Estimated Total Effort:** 8–12 weeks of focused work  
**Prerequisite:** Current "Ghost Co-op" system (done)  
**Last Updated:** May 13, 2026

---

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│  HOST PC  (Authority)                           │
│  ┌───────────────┐  ┌────────────────────────┐  │
│  │ BioShock.exe   │  │ BS1SDK.dll             │  │
│  │ (full game)    │◄─┤ - ProcessEvent hooks   │  │
│  │ - AI running   │  │ - World state capture  │  │
│  │ - Physics      │  │ - Input injection      │  │
│  │ - Scripts      │  │ - UDP server           │  │
│  └───────────────┘  └─────────┬──────────────┘  │
│                               │ UDP 30-60Hz     │
└───────────────────────────────┼─────────────────┘
                                │
                   ┌────────────▼────────────┐
                   │    Network (UDP)         │
                   │    Player state: 60Hz    │
                   │    World state: 10-30Hz  │
                   │    Events: immediate     │
                   └────────────┬────────────┘
                                │
┌───────────────────────────────┼─────────────────┐
│  CLIENT PC  (Puppet)          │                  │
│  ┌───────────────┐  ┌────────▼───────────────┐  │
│  │ BioShock.exe   │  │ BS1SDK.dll             │  │
│  │ (rendering     │◄─┤ - Receives world state │  │
│  │  shell only)   │  │ - Overrides all actors │  │
│  │ - AI FROZEN    │  │ - Injects P2 inputs    │  │
│  │ - No sim       │  │ - UDP client           │  │
│  └───────────────┘  └────────────────────────┘  │
│                                                  │
└──────────────────────────────────────────────────┘
```

**Key Insight:** Both PCs run BioShock, but the CLIENT freezes its own
simulation and puppets everything from the HOST's world state. The client
becomes a "dumb renderer" that just positions actors where the host says.

---

## Phase 1: Client Simulation Freeze (Week 1–2)

### Goal
Client PC runs BioShock but all AI, physics, and game logic are frozen.
The game becomes a pure renderer waiting for host data.

### Tasks
1. **Freeze AI tick on client**
   - Hook `ShockAI.Tick` / `AIController.Tick` → return immediately on client
   - Hook `SpawningManager.Tick` → no-op on client
   - Hook `AggressorSpawner` repopulation → no-op on client
   - Estimated: ~200 lines

2. **Freeze game scripts on client**
   - Hook Kismet trigger evaluation → no-op (host sends trigger events)
   - Disable LifeSpan-based destruction on client
   - Estimated: ~150 lines

3. **Freeze physics on client**
   - Hook Havok tick → no-op on client (host sends positions)
   - Or: let physics run but override positions every frame
   - Estimated: ~100 lines

4. **Client role detection**
   - Add `NetRole::TrueClient` enum value
   - `CoopTick` checks role and skips local simulation
   - Estimated: ~50 lines

### Testing
- [ ] Client loads level, AI enemies stand completely still
- [ ] Client can walk around freely, no enemies react
- [ ] Host plays normally, AI works as expected
- [ ] No crashes from frozen subsystems

### Risk: Medium
Some systems may crash if ticked out of order. May need selective
freezing rather than blanket no-ops.

---

## Phase 2: Full World State Broadcast (Week 2–4)

### Goal
Host captures position/rotation/state of ALL actors every frame and
broadcasts to client.

### Tasks
1. **Actor registry on host**
   - Scan GObjects for all Actor subclasses each tick
   - Build a map: `actorNameHash → {position, rotation, health, state}`
   - Filter: only sync actors within ~10,000 units of either player
   - Estimated: ~400 lines

2. **World state packet format**
   - Batched: up to 20 actors per packet
   - Per actor: nameHash(4) + pos(12) + rot(12) + health(4) + flags(4) = 36 bytes
   - 20 actors × 36 = 720 bytes per packet (fits UDP)
   - At 10Hz with ~100 active actors = 5 packets/tick = 50 packets/sec
   - Estimated: ~200 lines (packet definition + serialization)

3. **World state application on client**
   - Receive actor batches
   - For each: find matching actor by nameHash, set Location/Rotation/Health
   - Interpolate between updates for smooth movement
   - Estimated: ~500 lines

4. **Delta compression**
   - Only send actors that moved since last tick
   - Reduces bandwidth by ~60-80%
   - Estimated: ~200 lines

### New Packet Types
```
WorldStateBatch  = 0x50  // batched actor positions
ActorSpawn       = 0x51  // new actor appeared
ActorDestroy     = 0x52  // actor removed/killed
AnimState        = 0x53  // animation sync
```

### Testing
- [ ] Client sees enemies at correct positions (static test)
- [ ] Client sees enemies moving smoothly (interpolation test)
- [ ] Enemy count matches between host and client
- [ ] No rubberbanding or teleporting at 10Hz update rate
- [ ] Bandwidth stays under 100KB/s

### Risk: High
This is the hardest phase. Actor matching by nameHash may be fragile.
Some actors may not have unique names. Location property offset must be
correct for ALL actor types (not just ShockPlayer).

---

## Phase 3: Player 2 Input Injection (Week 4–5)

### Goal
Client sends their inputs to the host. Host creates a second player
pawn that responds to those inputs.

### Tasks
1. **Input capture on client**
   - Hook input processing on client
   - Capture: movement vector, look direction, fire/use/jump inputs
   - Send at 60Hz as compact input packet
   - Estimated: ~300 lines

2. **Input packet format**
   ```
   struct P2InputData {
       float moveForward, moveRight;  // -1 to 1
       float lookPitch, lookYaw;      // absolute angles
       uint16_t buttonMask;           // fire, altfire, jump, use, crouch, reload
       uint8_t weaponSlot;            // weapon switch
       uint8_t _pad;
   };
   ```

3. **Player 2 pawn on host**
   - Spawn a ShockPlayer pawn on the host for P2
   - Apply received inputs to move this pawn
   - This pawn exists in the host's world → AI sees it, can damage it
   - Estimated: ~600 lines (this is the hardest single task)

4. **Player 2 camera on client**
   - Client camera follows the P2 pawn position (received from host)
   - Override local camera to match P2's viewpoint
   - Estimated: ~200 lines

### Testing
- [ ] Client WASD inputs move the P2 pawn on host's screen
- [ ] P2 pawn is visible to host
- [ ] Enemies react to P2 pawn (aggro, attack)
- [ ] P2 can look around independently from host
- [ ] P2 pawn takes damage from enemies
- [ ] No input lag >100ms on LAN

### Risk: Very High
Spawning a second ShockPlayer is the biggest unknown. BioShock assumes
one player. Many systems (HUD, camera, inventory) are singleton.
**Fallback:** Use a ShockAI pawn (like DecoyHuman) driven by P2 inputs
instead of a true ShockPlayer. Loses inventory/plasmids but gains
stability.

---

## Phase 4: Combat Sync (Week 5–7)

### Goal
Both players can shoot, use plasmids, and deal damage in the shared world.

### Tasks
1. **P2 weapon fire on host**
   - Client sends fire input → host triggers P2 pawn's weapon
   - Use ProcessEvent to call weapon Fire functions on the P2 pawn
   - Hitscan: trace from P2's aim direction on host
   - Projectile: spawn projectile at P2's position on host
   - Estimated: ~500 lines

2. **P2 plasmid use on host**
   - Client sends plasmid input → host triggers ability on P2 pawn
   - Map client plasmid slot to host-side ability class
   - Estimated: ~400 lines

3. **Damage attribution**
   - Track which player dealt damage for kill credit
   - Both players share ADAM from kills (already have EconomySync)
   - Estimated: ~200 lines

4. **P2 health/EVE sync**
   - P2 pawn health changes on host → broadcast to client
   - Client renders P2's health/EVE HUD
   - Death/respawn for P2
   - Estimated: ~300 lines

### Testing
- [ ] P2 can shoot enemies and they take damage
- [ ] P2 can use wrench melee
- [ ] P2 can use at least one plasmid (Electro Bolt)
- [ ] Enemies die from P2 damage
- [ ] P2 can die and respawn
- [ ] Both players get ADAM from P2's kills

### Risk: High
Weapon systems may have hard dependencies on "the" player controller.
Plasmid abilities especially complex (targeting, effects, upgrades).

---

## Phase 5: Inventory & Progression (Week 7–9)

### Goal
P2 has their own inventory, weapons, plasmids, and can interact with
the world (vending machines, pickups, etc.).

### Tasks
1. **P2 inventory management on host**
   - Create a shadow inventory struct for P2
   - Track: weapons owned, ammo counts, plasmids, tonics
   - Sync to client for HUD rendering
   - Estimated: ~600 lines

2. **P2 world interaction**
   - Use key → interact with doors, switches, vending machines
   - Pickup collection → add to P2 inventory
   - Search containers → P2 gets their own loot rolls
   - Estimated: ~500 lines

3. **Vending machine access**
   - P2 can buy from vending machines using shared credits
   - Need to suppress host's vending UI when P2 is using it
   - Estimated: ~300 lines

4. **P2 HUD on client**
   - Override client HUD to show P2's health, EVE, ammo, weapon
   - Minimap position from P2 pawn
   - Estimated: ~400 lines

### Testing
- [ ] P2 can pick up items
- [ ] P2 can switch weapons
- [ ] P2 can use vending machines
- [ ] P2 has correct ammo display
- [ ] Both players see correct ADAM count

### Risk: Very High
BioShock's inventory system is deeply tied to the singleton ShockPlayer.
May need significant workarounds.

---

## Phase 6: Polish & Edge Cases (Week 9–12)

### Goal
Handle all the weird stuff that breaks.

### Tasks
1. **Level transitions**
   - When host changes level, client auto-follows
   - P2 pawn re-created on new level
   - Estimated: ~300 lines

2. **Cutscenes & scripted sequences**
   - Pause P2 input during cutscenes
   - Both players see cutscenes simultaneously
   - Estimated: ~200 lines

3. **Big Daddy encounters**
   - Both players can damage Big Daddies
   - Big Daddy aggro splits between both players
   - Little Sister rescue/harvest → shared ADAM
   - Estimated: ~300 lines

4. **Save system**
   - Host's save includes P2 state
   - Resume co-op session from save
   - Estimated: ~200 lines

5. **Stability hardening**
   - Null pointer guards everywhere
   - Graceful disconnect handling
   - Client reconnect without restart
   - Estimated: ~500 lines

6. **Performance optimization**
   - Profile CPU/bandwidth usage
   - Reduce tick rates for distant actors
   - LOD-based sync (nearby = 30Hz, far = 5Hz)
   - Estimated: ~400 lines

### Testing
- [ ] Play through entire Medical Pavilion co-op without crash
- [ ] Level transition works seamlessly
- [ ] Big Daddy fight works with both players
- [ ] 2-hour play session with no desync
- [ ] Bandwidth under 200KB/s sustained

---

## Timeline Summary

| Phase | Description | Duration | Cumulative |
|-------|-------------|----------|------------|
| 1 | Client Simulation Freeze | 2 weeks | Week 2 |
| 2 | World State Broadcast | 2 weeks | Week 4 |
| 3 | Player 2 Input Injection | 1.5 weeks | Week 5.5 |
| 4 | Combat Sync | 2 weeks | Week 7.5 |
| 5 | Inventory & Progression | 2 weeks | Week 9.5 |
| 6 | Polish & Edge Cases | 2.5 weeks | Week 12 |

**Minimum viable co-op (walk around + shoot): Phases 1–4 = ~7 weeks**  
**Full featured co-op: All phases = ~12 weeks**

---

## Dependencies on Current Work

| Existing System | Reused? | Notes |
|----------------|---------|-------|
| UDP networking | ✅ Yes | Foundation for all sync |
| ProcessEvent hooks | ✅ Yes | Used to freeze/inject on client |
| GObjects scanning | ✅ Yes | Actor registry builds on this |
| Property read/write | ✅ Yes | Position/health override |
| Trigger sync | ✅ Yes | Events still flow same way |
| Economy sync | ✅ Yes | Shared ADAM/Credits unchanged |
| Puppet system | ⚠️ Replaced | P2 becomes a real pawn, not a borrowed actor |
| Teleport plasmid | ✅ Untouched | Gameplay mods independent |
| Chain lightning | ✅ Untouched | Gameplay mods independent |
| All other mods | ✅ Untouched | Hooks are additive, not destructive |

---

## Key Technical Risks

1. **Spawning a second ShockPlayer** — The game may hardcrash if two
   exist. Fallback: use ShockAI-derived pawn for P2.

2. **Actor matching on client** — nameHash collisions could cause wrong
   actors to move. Mitigation: use full name strings for initial sync,
   hash for subsequent updates.

3. **Bandwidth** — 100+ actors at 10Hz = ~50KB/s baseline. With delta
   compression should be manageable over LAN/VPN.

4. **Frame-rate coupling** — Host's tick rate affects client experience.
   Need interpolation buffer on client (2–3 frames).

5. **Singleton systems** — HUD, camera, PlayerController all assume one
   player. Client must completely override these.

---

## What NOT To Do

- ❌ Don't try to reactivate UE2 replication (IpDrv stripped, no native code)
- ❌ Don't try to run two players in one process (memory conflicts)
- ❌ Don't sync EVERY object (only relevant actors within range)
- ❌ Don't block on network (async UDP only, never wait for packets)

---

## How To Start (When Ready)

1. Create `src/network/coop_true.h` and `coop_true.cpp`
2. Add `NetRole::TrueHost` and `NetRole::TrueClient` 
3. Phase 1 first: freeze client AI — if this works cleanly, everything
   else builds on it. If it crashes, we know early.
4. Test Phase 1 thoroughly before moving on.

**This plan is parked until The War In Rapture and other mod work is done.**
