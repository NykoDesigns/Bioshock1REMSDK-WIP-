#!/usr/bin/env python3
"""
Tool 3: Multi-Map Validator
Runs the BSM parser (ParseTest) against ALL 20 main BioShock maps and
reports parsing errors, mesh/BSP counts, and edge cases per map.

Requires: ParseTest.exe built from tools/level_editor/
"""
import subprocess, os, sys, json, re, time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "re_tool", "config.json")

with open(CONFIG_PATH) as f:
    cfg = json.load(f)

MAPS_DIR = os.path.join(cfg["game_content_dir"], "Maps")
UMODEL_EXPORT = cfg["umodel_export_dir"]

# Try to find ParseTest.exe
PARSE_TEST_CANDIDATES = [
    os.path.join(SCRIPT_DIR, "..", "tools", "level_editor", "build", "Release", "ParseTest.exe"),
    os.path.join(SCRIPT_DIR, "level_editor", "build", "Release", "ParseTest.exe"),
    os.path.join(SCRIPT_DIR, "..", "build_editor", "Release", "ParseTest.exe"),
]

SKIP_SUFFIXES = ("_chn.bsm", "_deu.bsm", "_esp.bsm", "_fra.bsm", "_int.bsm", "_ita.bsm", "_jpn.bsm")

OUTPUT_PATH = os.path.join(SCRIPT_DIR, "map_validation_report.txt")

def find_parse_test():
    for p in PARSE_TEST_CANDIDATES:
        full = os.path.normpath(p)
        if os.path.isfile(full):
            return full
    return None

def get_main_maps():
    maps = []
    for f in sorted(os.listdir(MAPS_DIR)):
        if not f.endswith(".bsm"):
            continue
        if any(f.endswith(s) for s in SKIP_SUFFIXES):
            continue
        full = os.path.join(MAPS_DIR, f)
        if os.path.getsize(full) < 1_000_000:
            continue
        maps.append(full)
    return maps

def run_parse_test(exe_path, bsm_path):
    """Run ParseTest on a single map and capture output."""
    name = os.path.splitext(os.path.basename(bsm_path))[0]
    
    try:
        result = subprocess.run(
            [exe_path, bsm_path, UMODEL_EXPORT],
            capture_output=True, timeout=120,
            cwd=os.path.dirname(exe_path)
        )
        stdout = result.stdout.decode('utf-8', errors='replace') if result.stdout else ''
        stderr = result.stderr.decode('utf-8', errors='replace') if result.stderr else ''
        return {
            "map": name,
            "exit_code": result.returncode,
            "stdout": stdout,
            "stderr": stderr,
            "error": None,
        }
    except subprocess.TimeoutExpired:
        return {"map": name, "exit_code": -1, "stdout": "", "stderr": "", "error": "TIMEOUT (120s)"}
    except Exception as e:
        return {"map": name, "exit_code": -1, "stdout": "", "stderr": "", "error": str(e)}

def extract_stats(output):
    """Extract key numbers from ParseTest output."""
    stats = {}
    
    patterns = {
        "exports": r'(\d+)\s+exports',
        "actors": r'(\d+)\s+actors',
        "static_meshes": r'(\d+)\s+[Ss]tatic\s*[Mm]esh',
        "bsp_nodes": r'(\d+)\s+nodes',
        "bsp_surfs": r'(\d+)\s+surfs',
        "bsp_chunks": r'(\d+)\s+(?:BSP\s+)?chunks',
        "bsp_verts": r'(\d+)\s+(?:BSP\s+)?vert',
        "bsp_tris": r'(\d+)\s+(?:BSP\s+)?tri',
        "textures_resolved": r'(\d+)/\d+.*textured',
        "planarity_fail": r'(\d+)/\d+.*planarity\s+fail',
        "no_mat": r'NO-MAT.*?(\d+)',
        "no_tga": r'NO-TGA.*?(\d+)',
        "bad_headers": r'(\d+)\s+bad\s+(?:Vengeance\s+)?header',
    }
    
    for key, pattern in patterns.items():
        m = re.search(pattern, output, re.IGNORECASE)
        if m:
            stats[key] = int(m.group(1))
    
    return stats

def main():
    exe = find_parse_test()
    if not exe:
        print("ERROR: ParseTest.exe not found. Build it first:")
        print("  cd tools/level_editor")
        print("  cmake -B build -A Win32")
        print("  cmake --build build --config Release")
        sys.exit(1)
    
    print(f"Using: {exe}")
    maps = get_main_maps()
    print(f"Found {len(maps)} main maps to validate\n")
    
    results = []
    start = time.time()
    
    for i, bsm_path in enumerate(maps):
        name = os.path.splitext(os.path.basename(bsm_path))[0]
        size_mb = os.path.getsize(bsm_path) / (1024*1024)
        print(f"[{i+1}/{len(maps)}] {name:30s} ({size_mb:.0f} MB) ...", end=" ", flush=True)
        
        r = run_parse_test(exe, bsm_path)
        stats = extract_stats(r["stdout"] + r["stderr"])
        r["stats"] = stats
        results.append(r)
        
        if r["error"]:
            print(f"ERROR: {r['error']}")
        elif r["exit_code"] != 0:
            print(f"EXIT CODE {r['exit_code']}")
        else:
            mesh_count = stats.get("static_meshes", "?")
            bsp_count = stats.get("bsp_chunks", "?")
            actors = stats.get("actors", "?")
            print(f"OK  actors={actors} meshes={mesh_count} bsp={bsp_count}")
        
    elapsed = time.time() - start
    
    # Write report
    with open(OUTPUT_PATH, 'w', encoding='utf-8') as f:
        f.write(f"{'='*80}\n")
        f.write(f"  MULTI-MAP VALIDATION REPORT\n")
        f.write(f"  {len(maps)} maps validated in {elapsed:.0f}s\n")
        f.write(f"{'='*80}\n\n")
        
        # Summary table
        f.write(f"{'Map':30s} {'Status':8s} {'Actors':>7s} {'Meshes':>7s} {'BSP':>5s} {'Nodes':>7s} {'PlnFail':>8s} {'BadHdr':>7s}\n")
        f.write(f"{'-'*30} {'-'*8} {'-'*7} {'-'*7} {'-'*5} {'-'*7} {'-'*8} {'-'*7}\n")
        
        total_actors = 0
        total_meshes = 0
        total_bsp = 0
        total_nodes = 0
        total_plan_fail = 0
        total_bad_hdr = 0
        errors = 0
        
        for r in results:
            s = r["stats"]
            status = "OK" if not r["error"] and r["exit_code"] == 0 else "FAIL"
            if status == "FAIL":
                errors += 1
            
            actors = s.get("actors", 0)
            meshes = s.get("static_meshes", 0)
            bsp = s.get("bsp_chunks", 0)
            nodes = s.get("bsp_nodes", 0)
            pf = s.get("planarity_fail", 0)
            bh = s.get("bad_headers", 0)
            
            total_actors += actors
            total_meshes += meshes
            total_bsp += bsp
            total_nodes += nodes
            total_plan_fail += pf
            total_bad_hdr += bh
            
            f.write(f"{r['map']:30s} {status:8s} {actors:7d} {meshes:7d} {bsp:5d} {nodes:7d} {pf:8d} {bh:7d}\n")
        
        f.write(f"{'-'*30} {'-'*8} {'-'*7} {'-'*7} {'-'*5} {'-'*7} {'-'*8} {'-'*7}\n")
        f.write(f"{'TOTALS':30s} {'':8s} {total_actors:7d} {total_meshes:7d} {total_bsp:5d} {total_nodes:7d} {total_plan_fail:8d} {total_bad_hdr:7d}\n")
        f.write(f"\nErrors: {errors}/{len(maps)} maps failed\n")
        f.write(f"Planarity failures: {total_plan_fail} (should be 0)\n")
        f.write(f"Bad Vengeance headers: {total_bad_hdr} (should be 0)\n")
        
        # Detailed output per map
        f.write(f"\n\n{'='*80}\n")
        f.write(f"  DETAILED OUTPUT PER MAP\n")
        f.write(f"{'='*80}\n")
        
        for r in results:
            f.write(f"\n{'='*60}\n")
            f.write(f"  {r['map']}\n")
            f.write(f"{'='*60}\n")
            if r["error"]:
                f.write(f"ERROR: {r['error']}\n")
            f.write(r["stdout"])
            if r["stderr"]:
                f.write(f"\nSTDERR:\n{r['stderr']}")
    
    print(f"\n{'='*60}")
    print(f"  VALIDATION COMPLETE ({elapsed:.0f}s)")
    print(f"{'='*60}")
    print(f"  Maps: {len(maps)}, Errors: {errors}")
    print(f"  Total actors: {total_actors}")
    print(f"  Total meshes: {total_meshes}")
    print(f"  Total BSP chunks: {total_bsp}")
    print(f"  Total BSP nodes: {total_nodes}")
    print(f"  Planarity failures: {total_plan_fail}")
    print(f"  Bad headers: {total_bad_hdr}")
    print(f"\n  Full report: {OUTPUT_PATH}")

if __name__ == "__main__":
    main()
