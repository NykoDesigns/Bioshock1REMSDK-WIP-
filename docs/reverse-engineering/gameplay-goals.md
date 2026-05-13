# BioShock 1 Remastered SDK — Gameplay Goals & Feasibility

## Summary

All 7 goals below are **feasible** with our current SDK capabilities. Each one has been
researched against the compiled game packages (ShockGame.U, ShockAI.U, Engine.U) and the
existing War In Rapture codebase.

---

## Goal 1: Absorb War In Rapture Logic

**What:** Port spawning multiplications, unlocked vending, plasmid changes, weapon changes
from The War In Rapture Python mod into this SDK.

**Feasibility: ✅ CONFIRMED — All logic already reverse-engineered**

### What War In Rapture does:
| Feature | Method | File |
|---------|--------|------|
| Spawn multiplication (2x-5x) | BSM binary patching (clone exports) | `bsm_spawn_patcher.py` |
| Scripted encounter expansion | BSM patching (clone ActionSpawnAI) | `bsm_script_patcher.py` |
| Weapon stats (mag, fire rate, reload, damage) | INI patching (Weapons.ini) | `ini_config.py` |
| Plasmid ADAM costs | INI patching (Plasmids.ini) | `ini_config.py` |
| Enemy health/damage per deck | INI patching (Ai.ini) | `ini_config.py` |
| Loot tables (drop rates, items) | INI patching (LootTables.ini) | `ini_config.py` |
| Spawning timers/distances | INI patching (Spawning.ini) | `ini_config.py` |
| Vending machine unlocks | INI patching (per-deck vending tables) | `ini_config.py` |

### What we already have:
- `bsm_tool v0.3.0` — Full BSM parsing with property deserialization
- ProcessEvent hook — Can intercept all game events at runtime

### What we need to implement:
1. **BSM Writer** — Port `bsm_spawn_patcher.py`'s export cloning + header rewrite to C++
2. **INI Patcher tool** — Port INI round-trip parser from `ini_config.py` (or new `ini_tool`)
3. **IBF Extractor** — Port `ibf_extract.py` to C++ for accessing ConfigINI.IBF

### Implementation approach:
- **Option A (Offline):** Standalone tools that patch .bsm and .INI files before game launch
- **Option B (Runtime):** DLL modifies weapon/plasmid properties in-memory via UObject reflection
- **Recommended:** Both — offline BSM patching for spawns + runtime for live tuning

---

## Goal 2: Hijack Decoy Plasmid for Teleportation

**What:** Replace the Decoy plasmid's functionality with teleportation. Use the Decoy
instead of Security Bullseye for teleport.

**Feasibility: ✅ CONFIRMED — Decoy already places actor at a location**

### How Decoy works (from ShockGame.U analysis):
- Class: `DecoyHumanAbility` (Export #218)
- Fires no projectile — calls `UseAbility` directly
- `UseAbility` (Export #1551) spawns a `DecoyHuman` (Export #485) at a calculated position
- Has `SpawnOffset` (StructProperty) — offset from player
- Has `DecoyHumanClass` — ClassProperty pointing to the decoy pawn class
- The `DecoyHuman` has `LifeSpan` — self-destructs after timer

### Why Decoy is BETTER than Security Bullseye for teleport:
| Factor | Security Bullseye | Decoy |
|--------|------------------|-------|
| Projectile | Fires a beacon bolt | No projectile — instant placement |
| Placement | Where bolt lands (can be random) | Calculated offset from player aim |
| Visual feedback | Bolt flight animation | Clone appears at location |
| Can fail? | Yes (bolt can miss/break) | No — always places |

### Implementation plan:
1. Hook `ProcessEvent` for `DecoyHumanAbility.UseAbility`
2. Instead of spawning DecoyHuman, read the calculated spawn position
3. Teleport player to that position (set Location property)
4. Play teleport VFX/sound instead of decoy spawn
5. Optionally rename via GNames reflection or UI overlay label

### Key game symbols:
- `TeleportPlayer` (Function, Export #909) — Already exists on ShockPlayer!
- `TeleportBeaconLocation/Rotation` — Built-in teleport properties on ShockPlayer
- `DecoyHumanAbility.SpawnOffset` — The offset we'll use as teleport target

### Renaming:
- The plasmid display name comes from a localized string table
- At runtime we can patch the FName or intercept the HUD display function
- Alternatively, patch the `.lbf` localization file offline

---

## Goal 3: Security Command → Friendly Bot Spawner (Limit 3)

**What:** Hijack Security Command (Security Bullseye/Beacon) to spawn friendly security
bots directly instead of tagging enemies, with a hard cap of 3 bots.

**Feasibility: ✅ CONFIRMED — SpawnSecurityBot + IsFriendly exist**

### How Security Beacon currently works:
- Class: `SecurityBeaconAbility` (Export #404)
- Fires: `BeaconProjectile` (Export #115) at target
- On impact: Tags enemy with `SecurityBeacon` stimuli set
- Security cameras/bots then target the tagged enemy
- Duration: `SecurityBeaconDuration` property (timed)

### Key game symbols for our approach:
- `SpawnSecurityBot` (Function, ShockCheatManager Export #2751) — ALREADY spawns bots!
- `SecurityBot` (Class) — The bot actor class
- `PawnIsFriendly` / `IsFriendly` — AI friendship check functions in ShockAI.U
- `AddForcedEnemy` — Force AI to attack specific target
- `AlarmBotCount` / `AlarmBotType` — Bot count tracking

### Implementation:
1. Hook `SecurityBeaconAbility` ProcessEvent (UseAbility or fire)
2. Instead of firing a beacon, call `SpawnSecurityBot` spawning logic
3. Set the spawned bot's AI to friendly (call `PawnIsFriendly` or set team property)
4. Track spawned bots in an array (max 3)
5. If count >= 3: either refuse to spawn or kill oldest bot before spawning new one
6. Bots auto-target enemies via their existing AI (they already do this normally)

### Hard limit enforcement:
```cpp
static std::vector<UObject*> g_friendlyBots;
// On spawn: g_friendlyBots.push_back(newBot);
// On tick: remove destroyed bots from list
// Before spawn: if (g_friendlyBots.size() >= 3) return;
```

---

## Goal 4: Custom Splicer Type Control in Encounters

**What:** Control exactly which splicer/enemy types spawn in each encounter.

**Feasibility: ✅ CONFIRMED — BSM patching + INI together**

### How enemy type selection works:
- `AggressorSpawner` has `RepopulationAITypes` (Array property) — list of AI classes
- AI class names: `MeleeThug`, `RangedAggressorPistol`, `RangedAggressorSMG`,
  `CeilingCrawler`, `Assassin`, `Grenadier`, etc. (confirmed in Entry.bsm names)
- `ActionSpawnAI` (Kismet scripted) has a class reference for what to spawn
- `Spawning.ini` has per-deck AI type growth tables

### What War In Rapture already does:
- `bsm_spawn_patcher.py` — Duplicates spawners (quantity control)
- `bsm_script_patcher.py` — Clones scripted encounters
- Does NOT control enemy type selection (limitation of their tool)

### Implementation (extends War In Rapture):
1. **BSM property editing:** Modify `RepopulationAITypes` array in spawner exports
   to set exact enemy class list (e.g., all Assassins, or mixed Thug+Grenadier)
2. **INI patching:** Modify per-deck growth tables in `Spawning.ini` to change
   the probability weights for each AI type
3. **Runtime hook:** Intercept spawner's `GetAIClass` / spawn function and override
   the class selection with our configured type

### Available enemy types (from ShockGame.U names):
- `MeleeThug` — Leadpipe splicer
- `RangedAggressorPistol` — Pistol splicer
- `RangedAggressorSMG` — Tommy gun splicer
- `RangedAggressorMachineGun` — Machine gun splicer
- `CeilingCrawler` — Spider splicer
- `Assassin` — Houdini splicer
- `Grenadier` — Grenade splicer
- `Rosie` — Rosie Big Daddy
- `SecurityBot` — Flying security bot

---

## Goal 5: Revolver Shoots Rivets (Rosie Rivet Gun)

**What:** Make the player's Pistol/Revolver fire rivets like the Rosie Big Daddy's weapon.

**Feasibility: ✅ CONFIRMED — ProjectileClass swappable at runtime**

### How weapon projectiles work:
- Each weapon ammo type has a `ProjectileClass` (ClassProperty)
- `GetProjectileClass` function returns what to spawn when firing
- `Pistol` (Export #170) — Standard bullet, armor-piercing, anti-personnel ammo types
- `RosieRangedWeapon` (name #2740) — Rosie's rivet gun (fires heavy projectile)

### Implementation options:

**Option A — Runtime property swap:**
1. Find `Pistol_Bullet` object (the ammo type for standard revolver rounds)
2. Change its `ProjectileClass` property to point to Rosie's rivet projectile class
3. Adjust damage values via `DamageStimuliSet` to match rivet damage

**Option B — INI patch:**
1. In `Weapons.ini`, change `[ShockGame.Pistol_Bullet]` section's `ProjectileClass`
   to reference the Rosie projectile
2. Adjust stimuli set for rivet-level damage

**Option C — ProcessEvent hook:**
1. Hook `Pistol.GetProjectileClass` function call
2. Return Rosie's projectile class instead of bullet class

### Considerations:
- Visual: Rivet projectile has different model/FX — will look different (cool!)
- Sound: May need to swap fire sound to rivet gun sound
- Damage: Rivets do significantly more damage — may need balancing
- Physics: Rivets may have different speed/arc

---

## Goal 6: Splicer Factions (Groups Fighting Each Other)

**What:** Create two splicer groups that fight each other.

**Feasibility: ✅ CONFIRMED — AI already has friendship/enemy targeting system**

### Existing AI systems (from ShockAI.U):
- `IsFriendly` / `PawnIsFriendly` — Check if two pawns are friendly
- `AddForcedEnemy` — Force AI to consider another pawn as enemy
- `ForceAttackTarget` — Make AI immediately attack specific target
- `IsAttackingTarget` / `GetAttackTarget` — Query current combat state
- `AttackSpecifiedTarget` / `ScriptedAttackTarget` — Override AI targeting
- `Enrage` / `SetBerserk` — Make AI go berserk (attacks everything)

### How BioShock AI friendship works:
- By default, all splicers are friendly to each other and hostile to player
- The `Enrage` plasmid makes a target hostile to ALL (it attacks everything)
- `AddForcedEnemy` can make specific AI units hostile to specific targets
- Security bots use `IsFriendly` to determine who to attack

### Implementation plan:
1. Assign a "faction tag" to splicers (stored in our DLL, indexed by UObject pointer)
2. Hook the AI's `IsFriendly` / `PawnIsFriendly` function:
   - Same faction → return true (friendly)
   - Different faction → return false (enemy)
3. To create a fight: spawn or tag splicers as Faction A or Faction B
4. Hook `GetAttackTarget` to prioritize other-faction enemies over player
5. Optionally: visual indicator (different colored glow via material property)

### Simplified approach (using existing Enrage):
- Call `AddForcedEnemy` on each splicer in group A targeting each splicer in group B
- This makes them attack each other using existing AI combat behavior
- No need to modify `IsFriendly` check — just force targeting

---

## Goal 7: Chain Lightning Electro Bolt

**What:** Create an electro bolt that chains between nearby enemies, inspired by the
Static Discharge / Electric Body gene tonic.

**Feasibility: ✅ CONFIRMED — Electro Bolt + radius damage + area search all exist**

### Existing systems:
- `ElectricBoltAbility` (Export #493) — Base electro bolt plasmid
- `ElectricBoltTwoAbility`, `ElectricBoltThreeAbility` — Upgraded versions
- `ElectricBody` (Export #491) — Gene tonic that zaps nearby enemies on hit
- `ElectricBoltStimuliSet` — Damage type for electricity
- `OuterDamageRadius` / `InnerDamageRadius` — Existing radius damage system
- `GetOuterDamageRadius` / `SetOuterDamageRadius` — Functions to control it

### How Electric Body tonic works:
- When player takes damage, fires electric stimuli at nearby enemies
- Uses a radius search to find pawns within range
- Applies `ElectricBoltStimuliSet` damage to each found pawn
- This is EXACTLY the chain mechanic we want, just triggered differently

### Implementation plan:
1. Hook `ProcessEvent` for Electro Bolt's damage application (when bolt hits target)
2. On hit: get the hit enemy's Location (FVector)
3. Search all nearby pawns within radius (iterate GObjects for ShockPawn in range)
4. For each nearby enemy within chain range (excluding already-hit):
   - Apply electric damage via ProcessEvent → `TakeDamage` with `ElectricBoltStimuliSet`
   - Spawn electric VFX arc between the two enemies (optional visual)
   - Optionally chain recursively (hit → chain → chain) with diminishing damage
5. Use `ElectricBody`'s radius/damage logic as reference for values

### Chain parameters:
```
ChainRadius    = 500 units (nearby enemies within this get zapped)
ChainDamage    = 50% of original bolt damage per jump
MaxChainJumps  = 3 (bolt → 1st chain → 2nd chain → 3rd chain)
ChainDelay     = 100ms between jumps (visual effect)
```

### Gene tonic integration idea:
- If player has Electric Body equipped, enable chain lightning on electro bolt
- Check `ElectricBody_Exists` (name #4138) at runtime to gate the feature
- This ties it to the existing tonic system naturally

---

## Priority & Implementation Order

| # | Goal | Effort | Dependencies | Priority |
|---|------|--------|-------------|----------|
| 1 | Absorb War In Rapture | HIGH (BSM writer + INI tool) | bsm_tool, ibf_tool | 🔴 Critical |
| 2 | Decoy → Teleport | LOW (PE hook, ~100 lines) | ProcessEvent already working | 🟢 Quick win |
| 3 | Security → Friendly Bots | MEDIUM (spawn + track + limit) | ProcessEvent, actor spawning | 🟡 Medium |
| 4 | Custom splicer types | MEDIUM (BSM property edit) | Goal 1 (BSM writer) | 🟡 Medium |
| 5 | Revolver → Rivets | LOW (property swap, ~50 lines) | Runtime UObject access | 🟢 Quick win |
| 6 | Splicer factions | MEDIUM (AI hook + tracking) | ProcessEvent, AI function calls | 🟡 Medium |
| 7 | Chain Lightning | MEDIUM (damage hook + radius search) | ProcessEvent, GObjects iteration | 🟡 Medium |

### Recommended start order:
1. **Goal 2** (Decoy teleport) — Quick win, demonstrates plasmid hijacking on a new target
2. **Goal 5** (Rivet revolver) — Quick win, demonstrates weapon property manipulation
3. **Goal 3** (Friendly bots) — Medium, demonstrates actor spawning + AI control
4. **Goal 7** (Chain lightning) — Medium, demonstrates area-of-effect mechanics
5. **Goal 6** (Factions) — Medium, demonstrates AI system manipulation
6. **Goal 1** (War In Rapture port) — Offline tooling, enables Goals 4 and persistent mods
7. **Goal 4** (Splicer type control) — Requires Goal 1's BSM writer

---

## Verdict: START? ✅ YES

All 7 goals are confirmed doable. The SDK already has the core infrastructure:
- **ProcessEvent hook** — intercept any game function call
- **GObjects/GNames** — find and manipulate any object at runtime
- **UObject reflection** — read/write properties on any game object
- **bsm_tool** — parse and (soon) write map files
- **D3D11 overlay** — visual feedback for all features

The game's compiled packages contain all the classes, functions, and properties needed.
No goals require reimplementing stripped engine code (unlike co-op networking).
