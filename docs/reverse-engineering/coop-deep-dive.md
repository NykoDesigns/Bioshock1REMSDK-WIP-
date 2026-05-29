# BioShock 1 Co-op — Deep Dive Architecture & Game Systems Map

## Status: Active Development

This document maps every game system relevant to co-op, what we can hook, and the
implementation plan to achieve "as true as possible" co-op within the proxy DLL architecture.

---

## 1. Complete Game Class Hierarchy (from ShockGame.U / Engine.U)

### Player Classes
| Class | Role | Key Properties |
|-------|------|----------------|
| `ShockPlayer` | Player pawn (body in world) | Location, Rotation, Health, EVE (BioAmmo), Weapon, Inventory |
| `ShockPlayerController` | Player input/camera controller | ViewRotation, Pawn reference |
| `ShockHUD` | HUD drawing class | Overlay elements, health/EVE bars |
| `ShockCheatManager` | Console cheats | `SpawnSecurityBot`, `God`, `Fly`, etc. |

### Enemy / NPC Classes
| Class | Type | Notes |
|-------|------|-------|
| `ShockPawn` | Base for ALL pawns | Has Location, Rotation, Health, AI controller |
| `MeleeThug` | Leadpipe splicer | Melee AI, patrol routes |
| `RangedAggressorPistol` | Pistol splicer | Ranged AI, cover behavior |
| `RangedAggressorSMG` | Tommy gun splicer | Higher fire rate |
| `RangedAggressorMachineGun` | Machine gun splicer | Heavy damage |
| `CeilingCrawler` | Spider splicer | Ceiling movement, drops down |
| `Assassin` | Houdini splicer | Teleports, fireballs |
| `Grenadier` | Grenade splicer | Area denial, explosive |
| `Rosie` | Big Daddy (Rosie) | Heavy HP, rivet gun, protects Little Sisters |
| `Bouncer` | Big Daddy (Bouncer) | Melee charge, drill |
| `SecurityBot` | Flying security bot | Flies, shoots, follows alarm targets |
| `LittleSister` | Little Sister | Harvest/Rescue mechanic |
| `DecoyHuman` | Decoy plasmid clone | Spawned by ability, has LifeSpan |

### World Actor Classes
| Class | Type | Key Functions/Properties |
|-------|------|--------------------------|
| `Mover` | Generic moving brush (doors, elevators) | `Open`, `Close`, `bOpening`, `bOpen` |
| `Door` / `ShockDoor` | Doors | `Open`, `Close`, `Lock`, `Unlock` |
| `PickupBase` | Base pickup class | `PickedUp`, `Touch`, `bHidden` |
| `AmmoPickup` | Ammo items | Amount, AmmoType |
| `HealthPickup` | Health kits, food | HealAmount |
| `WeaponPickup` | Weapon pickups | WeaponClass |
| `CurrencyPickup` | Cash/ADAM | Value |
| `AudioLog` | Collectible audio diaries | `PlayAudioLog`, `bPlayed` |
| `VitaChamber` | Respawn points | Location, `Resurrect`, active state |
| `Trigger` / `TriggerVolume` | Script triggers | `TriggerEvent`, `Touch`, `UnTouch` |
| `Button` / `Switch` | Interactive buttons | `UsedBy`, `Trigger`, state |
| `SecurityCamera` | Security cameras | Alarm state, detection cone |
| `Turret` | Turrets | Same as camera + firing |

### Spawner Classes
| Class | Notes |
|-------|-------|
| `AggressorSpawner` | Ambient splicer spawning, `RepopulationAITypes` array |
| `ProtectorSpawner` | Big Daddy/Little Sister pair spawning |
| `SecurityBotSpawner` | Bot spawning for alarm system |
| `ActionSpawnAI` | Scripted encounter spawning (Kismet) |

---

## 2. ProcessEvent Functions We Can Hook

These fire through `UObject::ProcessEvent` and we can intercept ALL of them:

### Player Events
| Function | Fires When | Object | Parms |
|----------|-----------|--------|-------|
| `PlayerTick` | Every game tick | ShockPlayer | DeltaTime |
| `Tick` | Every game tick | ShockPlayerController | DeltaTime |
| `TakeDamage` | Player takes damage | ShockPlayer | int Damage, Pawn InstigatedBy, vector HitLoc, vector Momentum, class DamageType |
| `Died` | Player dies | ShockPlayer | Controller Killer, class DamageType, vector HitLoc |
| `Destroyed` | Actor being destroyed | Any | None |
| `PreBeginPlay` | Before actor is initialized | Any | None |
| `BeginPlay` | After actor is initialized | Any | None |
| `Touch` | Two actors overlap | Any | Actor Other |
| `HitWall` | Projectile hits wall | Projectile | vector HitNormal, Actor Wall |

### Combat Events
| Function | Fires When | Object |
|----------|-----------|--------|
| `TakeDamage` | NPC takes damage | ShockPawn (enemy) |
| `Died` | NPC dies | ShockPawn (enemy) |
| `SetBerserk` | AI goes berserk | ShockPawn |
| `AddForcedEnemy` | AI forced to target | ShockPawn |

### World Events
| Function | Fires When | Object |
|----------|-----------|--------|
| `Open` | Door/mover opens | Mover/Door |
| `Close` | Door/mover closes | Mover/Door |
| `PickedUp` | Item collected | PickupBase |
| `UsedBy` | Button/switch activated | Button/Switch |
| `TriggerEvent` | Trigger fires | Trigger |
| `Trigger` | Trigger activated | Trigger/Kismet |

### Ability/Weapon Events
| Function | Fires When | Object |
|----------|-----------|--------|
| `UseAbility` | Plasmid used | *Ability class |
| `Fire` | Weapon fires | Weapon |
| `AltFire` | Alt-fire | Weapon |

---

## 3. What We Already Have Working

| Feature | Status | How |
|---------|--------|-----|
| Position sync | ✅ Working | PlayerTick hook → PlayerStateData → UDP |
| Health sync | ✅ Working | Read Health property, display on marker |
| Diamond marker | ✅ Working | ImGui overlay with interpolation |
| Chat | ✅ Working | Chat packet type + overlay display |
| Damage sync | ✅ Framework | TakeDamage hook → DamageData packet |
| Door sync | ✅ Framework | Open/Close hook → WorldEvent packet |
| Pickup sync | ✅ Framework | PickedUp hook → WorldEvent packet |
| Alt-tab keepalive | ✅ Working | Background thread pings |
| Level mismatch detection | ✅ Working | Handshake includes levelName |
| Economy sync | ✅ Working | Shared ADAM/Credits via coop_economy |
| Puppet system | ✅ Working | Ghost NPC visualization via coop_puppet |
| World state sync | ✅ Framework | Actor batch sync via coop_world_sync |
| Level transitions | ✅ Framework | Auto-follow via coop_transitions |
| Save/restore | ✅ Framework | Co-op state persistence via coop_save |
| Testing framework | ✅ Working | Automated co-op tests via coop_testing |
| Inventory sync | ✅ Framework | Item tracking via coop_inventory |

**Next milestone:** True Co-op (Phase 1: Client AI freeze). See `docs/true-coop-plan.md`.

---

## 4. Implementation Plan — True Co-op

### Phase A: Enemy Death Sync (HIGH IMPACT)
**Goal:** When a splicer dies on one screen, kill the matching splicer on the other.

**Implementation:**
1. Add new packet type `EnemyDeath` (0x21)
2. Hook `Died` event on all ShockPawn subclasses (not ShockPlayer)
3. On death: send packet with { actorNameHash, className, posX/Y/Z }
4. Receiver: find closest matching enemy (by class + position) → set Health to 0
5. If no match found (enemy already dead or different state), log warning

**Why this is huge:** Right now both players fight their own separate enemies. With death
sync, you see enemies actually die when your partner kills them. Feels like real co-op.

### Phase B: Enemy Health Sync (MEDIUM IMPACT)
**Goal:** When Player A damages a splicer, the same splicer on Player B's screen also loses health.

**Current:** We already forward TakeDamage → DamageData packets. But matching is imprecise.

**Improvements needed:**
1. Better actor matching: use className + nameHash + position (triple match)
2. Sync enemy HP periodically (every 2s, send enemy list with HP values)
3. Host-authority: Host's HP values are canonical

### Phase C: Spawner Coordination (MEDIUM IMPACT)
**Goal:** Same enemies spawn in roughly the same places for both players.

**Implementation:**
1. Both players must be on the same level (already enforced via handshake)
2. The game uses deterministic spawners tied to level BSM data — enemies spawn at
   the same locations by default (same game, same level)
3. The issue is TIMING — spawners activate on proximity. If players are in different
   areas, spawners fire at different times.
4. Solution: Host sends "spawner activated" events; Guest delays/forces their spawners
   to match. OR: Accept slight desync and rely on death sync to keep things aligned.

### Phase D: Vita-Chamber / Respawn Sync (HIGH IMPACT)
**Goal:** When Player A dies, they respawn at the nearest Vita-Chamber (normal behavior).
Player B sees them reappear. Add option: respawn near partner instead.

**Implementation:**
1. Hook `Died` on ShockPlayer
2. On death: send death event to partner
3. Partner sees "Player A died!" message in console
4. On respawn (detect by health going from 0 to >0): send position update
5. Optional "respawn at partner" command: after respawn, teleport to partner's location

### Phase E: Weapon/Plasmid Use Sync (LOW-MEDIUM IMPACT)
**Goal:** See what weapon/plasmid your partner is using (visual feedback).

**Current:** We send `weaponId` in PlayerStateData but don't display it.

**Implementation:**
1. Map weaponId to weapon names in the marker display
2. Show "Partner: Shotgun" or plasmid icon near the diamond
3. Sync plasmid use animations (fire electro bolt → show bolt VFX on remote)
   This is cosmetic but adds immersion.

### Phase F: Trigger/Progression Sync (MEDIUM IMPACT)
**Goal:** Story triggers, Kismet events, quest progression stay aligned.

**Implementation:**
1. Hook `TriggerEvent` on all Trigger/TriggerVolume actors
2. When a story trigger fires on Host, send it to Guest
3. Guest's game fires the same trigger via ProcessEvent
4. This keeps story beats synchronized (e.g., Atlas radio messages, area unlocks)

---

## 5. Actor Matching Strategy

The fundamental challenge: "How do you find the same enemy on both screens?"

### Current approach: FNV hash of actor name + position
```
Name: "MeleeThug_3" → hash: 0xA1B2C3D4
Position: (1234.0, 5678.0, 90.0) ± 200 units tolerance
```

### Better approach: Triple match
```
1. className match (exact): "MeleeThug"
2. nameHash match: FNV hash of full actor name
3. position match: within 200 units (accounts for slight AI pathing differences)
```

### Fallback: Nearest-of-class
If name hash doesn't match (different instance numbers between games):
```
Find nearest actor of same className within tolerance radius
```

### Why this works well enough:
- Both games load the same BSM level file → same spawners at same locations
- Spawners create enemies at the same world positions
- Even if instance numbers differ, the className + position narrows it to 1 enemy
- 200-unit tolerance handles minor AI movement differences

---

## 6. Authority Model

**Host is authoritative for:**
- Enemy deaths (if Host says enemy is dead, it's dead on Guest too)
- Door/trigger states (Host opens door → Guest's door opens)
- Pickup collection (first-to-grab wins, notified to other)

**Both are authoritative for:**
- Their own player position/health (no lag compensation needed)
- Their own damage dealt (each reports their own TakeDamage events)

**Neither is authoritative for:**
- Enemy AI movement (each game runs its own AI — accepted desync)
- Enemy spawn timing (accepted desync, death sync compensates)

---

## 7. New Packet Types Needed

| Type | ID | Payload | Purpose |
|------|-----|---------|---------|
| `EnemyDeath` | 0x21 | className[32], nameHash, posX/Y/Z | Enemy killed |
| `EnemyState` | 0x22 | array of {nameHash, health, posX/Y/Z} | Periodic HP sync |
| `PlayerDeath` | 0x23 | posX/Y/Z, killerClassName[32] | Player died |
| `PlayerRespawn` | 0x24 | posX/Y/Z | Player respawned |
| `TriggerSync` | 0x31 | triggerNameHash, posX/Y/Z, state | Story trigger fired |
| `SpawnerSync` | 0x32 | spawnerNameHash, posX/Y/Z, count | Spawner activated |

---

## 8. Priority Order

| # | Feature | Impact | Effort | Do First? |
|---|---------|--------|--------|-----------|
| A | Enemy death sync | HIGH | LOW | ✅ YES |
| D | Player death/respawn | HIGH | LOW | ✅ YES |
| B | Enemy HP sync | MEDIUM | MEDIUM | ✅ YES |
| F | Trigger/story sync | MEDIUM | MEDIUM | Next |
| C | Spawner coordination | MEDIUM | HIGH | Later |
| E | Weapon/plasmid visual | LOW | LOW | Polish |

---

## 9. Known Game Quirks

1. **Enemy instance numbers differ** between game sessions — the same MeleeThug might
   be `MeleeThug_3` on Host and `MeleeThug_5` on Guest. Must match by class+position.

2. **Spawners are proximity-triggered** — enemies only spawn when a player gets close
   to a spawner. If players are in different areas, they'll have different enemies alive.

3. **Vita-Chambers are per-level** — respawn always at nearest one. Both players have
   the same Vita-Chambers in the same locations.

4. **Big Daddy encounters are scripted** — they patrol set routes. Both games will have
   Big Daddies at the same locations, making them good sync candidates.

5. **Audio diaries are collectible** — collecting one on Host should mark it on Guest.
   This uses the same pickup sync mechanism.

6. **Hack minigame pauses the game** — if one player is hacking, their game pauses.
   The background keepalive thread handles this (sends pings during pause).

7. **Gatherer's Garden (plasmid vending)** — these are independent per player.
   No sync needed — each player buys their own upgrades.

8. **Save system** — each player has independent saves. Co-op state is not persisted.
   Players must both load the same level manually.
