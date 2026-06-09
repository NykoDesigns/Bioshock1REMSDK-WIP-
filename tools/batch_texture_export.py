#!/usr/bin/env python3
"""
Tool 1: Batch Texture Export
Exports textures from ALL main BioShock BSM maps via umodel.
Closes the ~60 missing-TGA gap by ensuring every map's textures are available.
"""
import subprocess, os, sys, json, time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "re_tool", "config.json")

with open(CONFIG_PATH) as f:
    cfg = json.load(f)

UMODEL = cfg["umodel_exe"]
EXPORT_DIR = cfg["umodel_export_dir"]
MAPS_DIR = os.path.join(cfg["game_content_dir"], "Maps")
GAME_TAG = cfg["umodel_game_tag"]

# Only export from main maps (skip localized _chn/_deu/_esp/_fra/_int/_ita/_jpn variants)
SKIP_SUFFIXES = ("_chn.bsm", "_deu.bsm", "_esp.bsm", "_fra.bsm", "_int.bsm", "_ita.bsm", "_jpn.bsm")

def get_main_maps():
    maps = []
    for f in sorted(os.listdir(MAPS_DIR)):
        if not f.endswith(".bsm"):
            continue
        if any(f.endswith(s) for s in SKIP_SUFFIXES):
            continue
        full = os.path.join(MAPS_DIR, f)
        if os.path.getsize(full) < 1_000_000:  # skip tiny/empty
            continue
        maps.append(full)
    return maps

def export_textures(bsm_path):
    name = os.path.splitext(os.path.basename(bsm_path))[0]
    print(f"\n{'='*60}")
    print(f"  Exporting textures from: {name}")
    print(f"{'='*60}")
    
    cmd = [
        UMODEL,
        f"-path={os.path.dirname(bsm_path)}",
        f"-out={EXPORT_DIR}",
        f"-game={GAME_TAG}",
        "-export",
        "-nostat",          # skip StaticMesh (we only want textures)
        "-nomesh",          # skip SkeletalMesh
        "-noanim",          # skip animations
        os.path.basename(bsm_path)
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        lines = (result.stdout + result.stderr).strip().split('\n')
        # Show summary
        exported = [l for l in lines if 'export' in l.lower() or 'texture' in l.lower()]
        for l in exported[-5:]:
            print(f"  {l.strip()}")
        return True
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT on {name}")
        return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False

def main():
    maps = get_main_maps()
    print(f"Found {len(maps)} main BSM maps to export textures from:")
    for m in maps:
        name = os.path.splitext(os.path.basename(m))[0]
        size_mb = os.path.getsize(m) / (1024*1024)
        print(f"  {name:30s} {size_mb:7.1f} MB")
    
    # Check which already have exports
    already = set()
    if os.path.isdir(EXPORT_DIR):
        already = set(os.listdir(EXPORT_DIR))
    
    print(f"\nAlready exported: {', '.join(sorted(already)) if already else 'none'}")
    print(f"\nStarting batch export...\n")
    
    start = time.time()
    results = {}
    for bsm_path in maps:
        name = os.path.splitext(os.path.basename(bsm_path))[0]
        ok = export_textures(bsm_path)
        results[name] = ok
    
    elapsed = time.time() - start
    
    print(f"\n{'='*60}")
    print(f"  BATCH EXPORT COMPLETE  ({elapsed:.0f}s)")
    print(f"{'='*60}")
    
    # Count total textures now
    total_tga = 0
    for root, dirs, files in os.walk(EXPORT_DIR):
        for f in files:
            if f.lower().endswith('.tga'):
                total_tga += 1
    
    print(f"Total TGA files in export dir: {total_tga}")
    for name, ok in results.items():
        status = "OK" if ok else "FAIL"
        print(f"  {name:30s} [{status}]")

if __name__ == "__main__":
    main()
