#!/usr/bin/env python3
"""
BSM Export Cross-Reference Database
Parses ALL BSM files' name and export tables to build a unified database:
  - mesh_name → which BSMs contain it + export offset
  - texture_name → which BSMs contain it + export index
  - class → all instances across all maps

Also maps generic catalog names (e.g. "Texture_49") back to actual texture names
by reading the export table's objectName field.

Output: tools/bsm_xref.json
"""
import os, sys, json, struct, glob

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "re_tool", "config.json")

with open(CONFIG_PATH) as f:
    cfg = json.load(f)

GAME_DIR = cfg["game_content_dir"]
MAPS_DIR = os.path.join(GAME_DIR, "Maps")
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "bsm_xref.json")

# ── CompactIndex reader ───────────────────────────────────────────────────
def read_ci(data, pos):
    if pos >= len(data): return 0, 0
    b0 = data[pos]
    sign = (b0 & 0x80) != 0
    val = b0 & 0x3F
    br = 1
    if b0 & 0x40:
        if pos + br >= len(data): return (-val if sign else val), br
        b1 = data[pos + br]; br += 1
        val |= (b1 & 0x7F) << 6
        if b1 & 0x80:
            if pos + br >= len(data): return (-val if sign else val), br
            b2 = data[pos + br]; br += 1
            val |= (b2 & 0x7F) << 13
            if b2 & 0x80:
                if pos + br >= len(data): return (-val if sign else val), br
                b3 = data[pos + br]; br += 1
                val |= (b3 & 0x7F) << 20
                if b3 & 0x80:
                    if pos + br >= len(data): return (-val if sign else val), br
                    b4 = data[pos + br]; br += 1
                    val |= (b4 & 0x1F) << 27
    return (-val if sign else val), br

def read_int32(data, pos):
    return struct.unpack_from('<i', data, pos)[0]

# ── Parse BSM package header ──────────────────────────────────────────────
def parse_bsm(bsm_path):
    """Parse a BSM file's name table, import table, and export table.
    Returns (names, imports, exports) or None on failure."""
    file_size = os.path.getsize(bsm_path)
    
    # Read header first (64 bytes)
    with open(bsm_path, 'rb') as f:
        header = f.read(64)
    
    if len(header) < 64:
        return None
    
    sig = struct.unpack_from('<I', header, 0)[0]
    if sig != 0x9E2A83C1:
        return None
    
    ver = struct.unpack_from('<H', header, 4)[0]
    
    name_count = read_int32(header, 12)
    name_offset = read_int32(header, 16)
    export_count = read_int32(header, 20)
    export_offset = read_int32(header, 24)
    import_count = read_int32(header, 28)
    import_offset = read_int32(header, 32)
    
    if name_count <= 0 or name_count > 500000: return None
    if export_count <= 0 or export_count > 500000: return None
    
    # Only read the tail of the file containing name/import/export tables
    # These tables are at the end of BSM files (offsets > 200MB typically)
    min_offset = min(name_offset, import_offset, export_offset)
    if min_offset <= 0 or min_offset >= file_size:
        return None
    
    with open(bsm_path, 'rb') as f:
        f.seek(min_offset)
        tail_data = f.read()
    
    data = tail_data
    base = min_offset  # subtract this from all offsets to get position in data
    
    # ─── Name Table ───
    # BSM names: CI length (can be negative for unicode), UTF-16LE chars,
    # optional null terminator wchar, then 8 bytes of flags (not 4 like standard UE2)
    names = []
    pos = name_offset - base
    for i in range(name_count):
        if pos >= len(data): break
        raw_len, br = read_ci(data, pos)
        pos += br
        char_count = abs(raw_len) if raw_len != 0 else 0
        if char_count <= 0 or char_count > 65536:
            names.append("")
            continue
        
        s = ""
        for c in range(char_count):
            if pos + c * 2 + 1 >= len(data): break
            wc = struct.unpack_from('<H', data, pos + c * 2)[0]
            if wc == 0: break
            s += chr(wc & 0xFF)
        pos += char_count * 2
        
        # Skip null terminator wchar if present
        if pos + 1 < len(data):
            wc = struct.unpack_from('<H', data, pos)[0]
            if wc == 0:
                pos += 2
        
        if pos + 8 <= len(data):
            pos += 8  # flags (8 bytes in BSM, not 4)
        
        names.append(s)
    
    # ─── Import Table ───
    # BSM import entry: FName classPackage, FName className, INT32 outerIndex, FName objectName
    # FName = CI nameIndex + INT32 number
    imports = []
    pos = import_offset - base
    for i in range(import_count):
        if pos + 4 > len(data): break
        # FName ClassPackage
        class_pkg_idx, br = read_ci(data, pos); pos += br
        pos += 4  # classPackage number
        # FName ClassName
        class_name_idx, br = read_ci(data, pos); pos += br
        pos += 4  # className number
        # INT32 OuterIndex
        outer_ref = read_int32(data, pos); pos += 4
        # FName ObjectName
        obj_name_idx, br = read_ci(data, pos); pos += br
        pos += 4  # objectName number
        
        class_name = names[class_name_idx] if 0 <= class_name_idx < len(names) else ""
        obj_name = names[obj_name_idx] if 0 <= obj_name_idx < len(names) else ""
        imports.append({
            'className': class_name,
            'objectName': obj_name,
        })
    
    # ─── Export Table ───
    # BSM export entry layout (differs from standard UE2):
    #   CI classIndex, CI superIndex, INT32 outerIndex, INT32 unknownBS1,
    #   CI nameIndex, INT32 nameNumber, UINT64 objectFlags,
    #   CI serialSize, CI serialOffset (if serialSize>0), INT32 unknownBS2
    exports = []
    pos = export_offset - base
    for i in range(export_count):
        if pos + 28 > len(data): break
        class_idx, br = read_ci(data, pos); pos += br
        super_idx, br = read_ci(data, pos); pos += br
        group_ref = read_int32(data, pos); pos += 4
        pos += 4  # unknownBS1
        name_idx, br = read_ci(data, pos); pos += br
        name_num = read_int32(data, pos); pos += 4  # nameNumber (1-based)
        pos += 8  # objectFlags (uint64)
        serial_size, br = read_ci(data, pos); pos += br
        serial_offset = 0
        if serial_size > 0:
            serial_offset, br2 = read_ci(data, pos); pos += br2
        pos += 4  # unknownBS2
        
        obj_name = names[name_idx] if 0 <= name_idx < len(names) else ""
        # Append instance number (stored as N+1, so 0 means no suffix, 1 → _0, etc.)
        if name_num > 0:
            obj_name += f"_{name_num - 1}"
        
        exports.append({
            'objectName': obj_name,
            'classIdx': class_idx,
            'groupRef': group_ref,
            'serialOffset': serial_offset,
            'serialSize': serial_size,
            'exportIndex': i,
        })
    
    # Second pass: resolve class and group names (now all exports exist)
    for exp in exports:
        class_idx = exp.pop('classIdx')
        group_ref = exp.pop('groupRef')
        
        class_name = ""
        if class_idx == 0:
            class_name = "Class"
        elif class_idx > 0 and (class_idx - 1) < len(exports):
            class_name = exports[class_idx - 1]['objectName']
        elif class_idx < 0 and (-class_idx - 1) < len(imports):
            # Import objectName is the class name (className is the metaclass)
            class_name = imports[-class_idx - 1]['objectName']
        exp['className'] = class_name
        
        group_name = ""
        if group_ref > 0 and (group_ref - 1) < len(exports):
            group_name = exports[group_ref - 1]['objectName']
        elif group_ref < 0 and (-group_ref - 1) < len(imports):
            group_name = imports[-group_ref - 1]['objectName']
        exp['groupName'] = group_name
    
    return names, imports, exports

# ── Main ──────────────────────────────────────────────────────────────────
def main():
    bsm_files = sorted(glob.glob(os.path.join(MAPS_DIR, "*.bsm")))
    if not bsm_files:
        print(f"[XRef] No BSM files found in {MAPS_DIR}")
        return
    
    print(f"[XRef] Found {len(bsm_files)} BSM files")
    
    # Cross-reference databases
    mesh_db = {}       # meshName → [{map, exportIndex, serialOffset, serialSize}]
    texture_db = {}    # textureName → [{map, exportIndex, groupName}]
    class_db = {}      # className → [{map, objectName, exportIndex}]
    catalog_map = {}   # "Texture_49" → actual texture name (per map)
    map_stats = {}     # mapName → {exports, meshes, textures, ...}
    
    for bsm_path in bsm_files:
        map_name = os.path.splitext(os.path.basename(bsm_path))[0]
        print(f"  Parsing {map_name}...", end='', flush=True)
        
        result = parse_bsm(bsm_path)
        if not result:
            print(" FAILED")
            continue
        
        names, imports, exports = result
        
        mesh_count = 0
        tex_count = 0
        
        for exp in exports:
            cn = exp['className']
            on = exp['objectName']
            
            # Track StaticMesh exports
            if cn == 'StaticMesh':
                mesh_count += 1
                if on not in mesh_db:
                    mesh_db[on] = []
                mesh_db[on].append({
                    'map': map_name,
                    'exportIndex': exp['exportIndex'],
                    'serialOffset': exp['serialOffset'],
                    'serialSize': exp['serialSize'],
                })
            
            # Track Texture exports (Texture, Texture2D, ShadowMap, etc.)
            if cn in ('Texture', 'Texture2D', 'ShadowMapTexture2D') or 'Texture' in cn:
                tex_count += 1
                actual_name = on
                group = exp['groupName']
                
                if actual_name not in texture_db:
                    texture_db[actual_name] = []
                texture_db[actual_name].append({
                    'map': map_name,
                    'exportIndex': exp['exportIndex'],
                    'groupName': group,
                })
                
                # If this looks like a generic catalog name (Texture_N), map it
                if on.startswith('Texture_') and on[8:].isdigit():
                    # The group name is the actual texture group
                    key = f"{map_name}:{on}"
                    catalog_map[key] = {
                        'genericName': on,
                        'groupName': group,
                        'map': map_name,
                    }
            
            # Track all classes
            if cn:
                if cn not in class_db:
                    class_db[cn] = []
                if len(class_db[cn]) < 50:  # cap per class to keep file manageable
                    class_db[cn].append({
                        'map': map_name,
                        'objectName': on,
                        'exportIndex': exp['exportIndex'],
                    })
        
        map_stats[map_name] = {
            'exports': len(exports),
            'imports': len(imports),
            'names': len(names),
            'meshes': mesh_count,
            'textures': tex_count,
        }
        print(f" {len(exports)} exports, {mesh_count} meshes, {tex_count} textures")
    
    # Compute sharing stats
    shared_meshes = {k: v for k, v in mesh_db.items() if len(v) > 1}
    unique_meshes = {k: v for k, v in mesh_db.items() if len(v) == 1}
    
    # Build output
    db = {
        "version": 1,
        "stats": {
            "totalMaps": len(map_stats),
            "totalMeshes": len(mesh_db),
            "sharedMeshes": len(shared_meshes),
            "uniqueMeshes": len(unique_meshes),
            "totalTextures": len(texture_db),
            "totalClasses": len(class_db),
            "catalogMappings": len(catalog_map),
        },
        "mapStats": map_stats,
        "meshes": {k: v for k, v in sorted(mesh_db.items())},
        "textures": {k: v[0] if len(v) == 1 else v for k, v in sorted(texture_db.items())},
        "classes": {k: len(v) for k, v in sorted(class_db.items())},
        "catalogMap": catalog_map,
    }
    
    with open(OUTPUT_PATH, 'w', encoding='utf-8') as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
    
    size_kb = os.path.getsize(OUTPUT_PATH) / 1024
    print(f"\n[XRef] Output: {OUTPUT_PATH} ({size_kb:.0f} KB)")
    print(f"[XRef] Meshes: {len(mesh_db)} total ({len(shared_meshes)} shared across maps)")
    print(f"[XRef] Textures: {len(texture_db)} total")
    print(f"[XRef] Classes: {len(class_db)} unique class names")
    
    # Show top shared meshes
    print(f"\nTop 15 most-shared meshes:")
    for name, locs in sorted(shared_meshes.items(), key=lambda x: -len(x[1]))[:15]:
        maps = [l['map'] for l in locs]
        print(f"  {name:40s} in {len(maps)} maps")

if __name__ == "__main__":
    main()
