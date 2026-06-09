# Co-op Investigation Log

## Goal
Make BioShock Remastered playable in co-op: both players see each other, their actions, shared enemies, shared world state.

---

## Architecture (Working at 60 FPS)

- **D3D Present hook** → `CoopTick()` → `TrueCoopTick()` — runs every frame
- **UDP networking** — custom packets for state sync
- **Ghost puppet** — StaticMeshActor borrowed from world, diamond overlay marker
- **No unfiltered PE hooks** — see `performance-rules.md`

---

## What Works
- [x] Position sync (30Hz UDP)
- [x] Rotation sync
- [x] Health sync
- [x] Diamond marker overlay with health bar
- [x] Ghost puppet (in-world StaticMeshActor)
- [x] Chat system
- [x] Level transition detection
- [x] Save transfer
- [x] **Fire detection** — `LeftMousePressed` PE hook (confirmed working solo, needs 2P test)

## What Doesn't Work Yet
- [ ] **Fire overlay on remote side** — sender works, needs 2P test to confirm receiver sees flash
- [ ] Objective/trigger sync (host→client)
- [ ] Enemy sync (host→client)
- [ ] Full puppet model (currently just borrowed StaticMesh)

## Next Steps (in order)
1. **2P test of fire overlay** — confirm both sides see muzzle flash on diamond
2. **Enemy death sync** — poll actor health, detect deaths, send packets (no PE hooks)
3. **Trigger/objective sync** — poll trigger states, send changes to client

---

## Investigation: Fire Detection

### Attempt 1: FlashCount Polling (FAILED)
- `FlashCount` (byte at offset +1160) found on ShockPlayer via Pawn inheritance
- **Never changes** when firing in BioShock Remastered — dead property

### Attempt 2: Filtered PE Hook "BeginFiring" (FAILED)
- Registered `FunctionFilter = "BeginFiring"` — never triggered
- `BeginFiring` exists as a script function on ShockPlayer but is never called via ProcessEvent
- Conclusion: firing is handled entirely through native C++ code, not UScript

### Attempt 3: PE Class Dump (50 unique classes)
- ALL 50 entries were `Tick` calls on world objects (doors, spawners, particles, etc.)
- **Zero ShockPlayer PE calls** — not even Tick!
- ShockPlayer tick is native, not through ProcessEvent

### Attempt 4: Non-Tick PE Event Scan (SUCCESS)
- Filtered out Tick/PostRender/Reset/Get*/Process_* noise
- Captured 300 unique class.function combos during gameplay
- **Found weapon-related PE calls:**

#### Weapon PE Events (confirmed by test)
| Action | PE Event | Trigger |
|--------|---------|---------|
| Any fire input | `ShockPlayerController.LeftMousePressed` | **Best hook target — fires on ANY attack** |
| Pistol fire | `Pistol.InitiateDamage` | Each pistol shot |
| MachineGun fire | `MachineGun.Timer` / `.OnAnimationEnded` | During MG firing |
| Wrench swing | `Wrench_Swing.PreBeginPlay` | Swing actor spawned |
| Weapon equip | `Pistol.Attach` / `MachineGun.BaseChange` | Weapon change |
| Weapon unequip | `Pistol.OnDetached` / `Wrench.OnDetached` | Weapon swap |
| Plasmid use | `ElectroBoltHandLight.*` | Electro bolt cast |
| Bullet impact | `HitSpangConcrete_Gen.*` | Hit effects |
| Mouse release | `ShockPlayerController.LeftMouseReleased` | Stop firing |

### Solution: `LeftMousePressed` Filtered PE Hook
- `FunctionFilter = "LeftMousePressed"` — fires ONLY on this specific function
- Zero FPS impact (filter skips 5000+ non-matching calls per frame via cheap wchar compare)
- Sets `s_LocalFired` atomic bool → consumed by CoopTick → sent as `isFiring=1` in state packet
- Receiver triggers `NotifyPuppetAction(WeaponFire)` → yellow muzzle flash on overlay

### Key Findings
- ShockPlayer DOES appear in PE but rarely (e.g., `TriggerEffectEventWithMaterialTrace`)
- ShockPlayerController fires `LeftMousePressed`/`LeftMouseReleased` on input
- Weapon classes (Pistol, MachineGun, Wrench) have lifecycle PE calls (Attach/Detach/Timer)
- PlayerHands manages weapon attachment via `GainedChild`/`LostChild`/`BeginState`/`EndState`

---

## PE Call Landscape (What Goes Through ProcessEvent)

### High-frequency (every frame):
- `Tick` on ALL world actors (~3000-5000 calls/frame)
- `PostRender` on ShockHUD
- `Reset` on Canvas
- `GetUserSettings` on FlashGUIController
- `Process_PostRender` on InteractionMaster

### Classes confirmed in PE:
- World: DefaultPhysicsVolume, FluidVolume, HavokForceActor
- Doors: ShojiDoors, BulkheadDoors, ScienceDoors, LoadRoomDoor, ArcadiaGateDoor
- Spawners: AggressorSpawner, SecurityCameraSpawner, TurretSpawner, ProtectorSpawner, SecurityBotSpawner
- Effects: Fireflies_Area, BlossomFalling, Steam_LightRising, WaterSplash_Small, etc.
- Gameplay: KeypadWallsafe, ResurrectionStation, PlaceablePlasmidEquipStation
- Triggers: InPlayerViewTrigger, TriggerRadius
- AI: AIController

### Classes NOT in PE:
- ShockPlayer (tick is native)
- ShockPlayerController
- Weapon / Holdable / Hands
- Any ammo or projectile class

---

## Player Property Offsets (Cached at Runtime)
| Property | Offset | Type | Status |
|----------|--------|------|--------|
| Location | +472 | Vector (3 floats) | Working |
| Rotation | +484 | Rotator (3 int32s) | Working |
| Health | +1404 | Float | Working |
| FlashCount | +1160 | Byte | Found but never changes |

---

## Packet Types (net_common.h)
| Type | Code | Direction | Status |
|------|------|-----------|--------|
| PlayerState | 0x10 | Both | Working |
| Handshake | 0x11 | Both | Working |
| Damage | 0x20 | Both | Disabled (needs PE hook) |
| EnemyHPSync | 0x22 | Host→Client | Disabled |
| PlayerDeath | 0x23 | Both | Disabled |
| WorldEvent | 0x30 | Both | Disabled |
| TriggerSync | 0x31 | Host→Client | Disabled |
| LevelSync | 0x35 | Both | Working |
| EconomySync | 0x36 | Both | Disabled |
| Chat | 0x40 | Both | Working |
| Ping/Pong | 0xF0/0xF1 | Both | Working |

---

## Files Modified During Investigation
- `src/network/coop_bridge.cpp` — fire detection attempts, diagnostic hooks
- `src/network/net_common.h` — PlayerStateData.isFiring field exists
- `src/network/coop_render.cpp` — muzzle flash overlay effect (ready, needs trigger)
- `src/network/coop_puppet.cpp` — NotifyPuppetAction() ready to receive fire events
