#!/usr/bin/env python3
"""
Actor Class Visualizer Config Generator
Uses the UnrealScript property database to generate a JSON config mapping
actor class names to visual shapes, colors, and sizes for the level editor viewport.

Output: tools/actor_viz.json
"""
import os, json

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROP_DB_PATH = os.path.join(SCRIPT_DIR, "property_db.json")
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "actor_viz.json")
XREF_PATH = os.path.join(SCRIPT_DIR, "bsm_xref.json")

# ── Visual config rules ────────────────────────────────────────────────────
# Based on class hierarchy and common BioShock classes
RULES = [
    # Lights — yellow-orange family
    {"match": ["Light", "PointLight", "SpotLight", "DirectionalLight", "SkyLight",
               "DominantDirectionalLight", "DominantPointLight", "DominantSpotLight",
               "PointLightMovable", "SpotLightMovable"],
     "shape": "sphere", "color": [1.0, 0.9, 0.2], "radius": 32, "icon": "light"},
    {"match": ["TriggerLight", "LightRig"],
     "shape": "sphere", "color": [1.0, 0.8, 0.1], "radius": 32, "icon": "light"},

    # Triggers — green wireframes
    {"match": ["Trigger", "TriggerVolume", "EncounterTriggerVolume",
               "ActorGroupTrigger", "SequenceTrigger", "TriggerStreamVolume",
               "DamageTrigger", "Trigger_LOS"],
     "shape": "box", "color": [0.2, 1.0, 0.3], "radius": 48, "icon": "trigger"},

    # Sound emitters — blue
    {"match": ["AmbientSound", "AmbientSoundSimple", "AmbientSoundNonLoop",
               "SoundEmitter", "SoundGroup", "AudioComponent"],
     "shape": "sphere", "color": [0.3, 0.5, 1.0], "radius": 24, "icon": "sound"},

    # Volumes — semi-transparent outlines
    {"match": ["BlockingVolume", "PhysicsVolume", "WaterVolume", "PainCausingVolume",
               "PostProcessVolume", "ReverbVolume", "LevelStreamingVolume",
               "FogVolume", "AudioVolume"],
     "shape": "box", "color": [0.6, 0.3, 1.0], "radius": 64, "icon": "volume"},

    # Navigation — cyan
    {"match": ["PathNode", "FlyingPathNode", "NavigationPoint", "JumpPoint",
               "PlayerStart", "Teleporter", "Door", "CoverNode"],
     "shape": "diamond", "color": [0.0, 1.0, 1.0], "radius": 20, "icon": "nav"},

    # Spawners — red
    {"match": ["AggressorSpawner", "AISpawnPoint", "SpawnPoint",
               "CreatureSpawner", "NPCSpawnPoint"],
     "shape": "cone", "color": [1.0, 0.2, 0.2], "radius": 40, "icon": "spawner"},

    # Cameras / Matinee — pink
    {"match": ["CameraActor", "SecurityCamera", "InterpActor", "Matinee",
               "SceneCapture", "SceneCaptureActor"],
     "shape": "cone", "color": [1.0, 0.4, 0.8], "radius": 24, "icon": "camera"},

    # Pickups / Items — gold
    {"match": ["Pickup", "InventoryPickup", "WeaponPickup", "AmmoPickup",
               "HealthPickup", "AudioDiaryPickup", "ItemPickup",
               "VendingMachine", "VendingWide", "UpgradeStation",
               "LootSlot", "LootItemSpecification"],
     "shape": "sphere", "color": [1.0, 0.7, 0.0], "radius": 16, "icon": "pickup"},

    # Emitters / FX — orange
    {"match": ["Emitter", "SpriteEmitter", "MeshEmitter", "BeamEmitter",
               "ParticleSystem", "ParticleEmitter"],
     "shape": "sphere", "color": [1.0, 0.5, 0.0], "radius": 20, "icon": "fx"},

    # Info actors — white (small)
    {"match": ["LevelInfo", "GameInfo", "ZoneInfo", "PhysicsInfo",
               "ActorGroup", "Note", "Bookmark"],
     "shape": "diamond", "color": [0.8, 0.8, 0.8], "radius": 12, "icon": "info"},

    # Kismet / Scripting — magenta
    {"match": ["SequenceAction", "SequenceEvent", "SequenceCondition",
               "SequenceVariable", "SequenceFrame", "Sequence",
               "Script", "ScriptedAction"],
     "shape": "diamond", "color": [1.0, 0.0, 1.0], "radius": 16, "icon": "script"},
]

def main():
    # Load xref to get actual classes used in maps
    used_classes = set()
    if os.path.exists(XREF_PATH):
        with open(XREF_PATH) as f:
            xref = json.load(f)
        used_classes = set(xref.get("classes", {}).keys())
        print(f"[ActorViz] {len(used_classes)} classes from BSM xref")

    # Load property DB for class hierarchy
    class_parents = {}
    if os.path.exists(PROP_DB_PATH):
        with open(PROP_DB_PATH) as f:
            db = json.load(f)
        classes_dict = db.get("classes", {})
        for name, info in classes_dict.items():
            parent = info.get("parent", "") if isinstance(info, dict) else ""
            class_parents[name] = parent
        print(f"[ActorViz] {len(class_parents)} classes from property DB")

    # Build class→config mapping
    config = {}

    # First pass: exact matches
    for rule in RULES:
        for match_name in rule["match"]:
            entry = {
                "shape": rule["shape"],
                "color": rule["color"],
                "radius": rule["radius"],
            }
            config[match_name] = entry

    # Pattern-based rules for BioShock-specific classes
    PATTERNS = [
        # Lights
        (["Light", "Omni", "Corona", "LightRig", "Brine"],
         {"shape": "sphere", "color": [1.0, 0.9, 0.2], "radius": 32}),
        # Sounds
        (["Sound", "Audio", "Speech", "Voice", "vo_"],
         {"shape": "sphere", "color": [0.3, 0.5, 1.0], "radius": 24}),
        # Spawners
        (["Spawner", "SpawnZone", "SpawnPoint", "Repop"],
         {"shape": "cone", "color": [1.0, 0.2, 0.2], "radius": 40}),
        # Triggers / Volumes
        (["Trigger", "Volume", "Zone", "Blocking"],
         {"shape": "box", "color": [0.2, 1.0, 0.3], "radius": 48}),
        # Navigation / Pathing
        (["PathNode", "FlyingPathNode", "NavigationPoint", "CoverNode", "Door"],
         {"shape": "diamond", "color": [0.0, 1.0, 1.0], "radius": 20}),
        # Pickups / Items
        (["Pickup", "Loot", "Vending", "Item", "Ammo", "Health", "Adam"],
         {"shape": "sphere", "color": [1.0, 0.7, 0.0], "radius": 16}),
        # FX / Emitters
        (["Emitter", "Particle", "Effect", "FX", "Bubbles", "Steam", "Fire", "Smoke",
          "Spray", "Drip", "Mist", "Dust", "Fog", "Splash", "Cascade", "Water"],
         {"shape": "sphere", "color": [1.0, 0.5, 0.0], "radius": 20}),
        # AI / NPC
        (["AI", "Aggressor", "Splicer", "BigDaddy", "LittleSister", "Turret", "Camera",
          "SecurityBot", "Pawn", "NPC"],
         {"shape": "cone", "color": [1.0, 0.3, 0.3], "radius": 36}),
        # Scripting / Kismet
        (["Sequence", "Script", "Matinee", "Interp", "Cinematic"],
         {"shape": "diamond", "color": [1.0, 0.0, 1.0], "radius": 16}),
    ]

    # Apply patterns
    pattern_matched = 0
    for cls in sorted(used_classes):
        if cls in config:
            continue
        for patterns, viz in PATTERNS:
            if any(p.lower() in cls.lower() for p in patterns):
                config[cls] = viz.copy()
                pattern_matched += 1
                break

    # Inheritance pass: for remaining classes, walk parent chain
    def get_viz(class_name, depth=0):
        if depth > 20:
            return None
        if class_name in config:
            return config[class_name]
        parent = class_parents.get(class_name, "")
        if parent:
            return get_viz(parent, depth + 1)
        return None

    inherited = 0
    for cls in sorted(used_classes):
        if cls not in config:
            viz = get_viz(cls)
            if viz:
                config[cls] = viz
                inherited += 1

    # Output
    output = {
        "version": 1,
        "description": "Actor class → visual shape/color config for BS1LevelEditor viewport",
        "totalEntries": len(config),
        "directRules": sum(len(r["match"]) for r in RULES),
        "inheritedRules": inherited,
        "config": config,
    }

    with open(OUTPUT_PATH, 'w') as f:
        json.dump(output, f, indent=2)

    print(f"[ActorViz] Output: {OUTPUT_PATH}")
    print(f"  Direct rules: {sum(len(r['match']) for r in RULES)}")
    print(f"  Inherited rules: {inherited}")
    print(f"  Total: {len(config)} class visualizations")

    # Show coverage
    covered = sum(1 for c in used_classes if c in config)
    print(f"  Coverage: {covered}/{len(used_classes)} classes used in maps ({100*covered//max(1,len(used_classes))}%)")

if __name__ == "__main__":
    main()
