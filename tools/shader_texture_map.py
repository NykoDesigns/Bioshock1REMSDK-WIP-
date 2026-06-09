#!/usr/bin/env python3
"""
Shader→Texture Mapper
Parses all .props.txt files from umodel Shader/FacingShader exports to build a
mapping of shader_name → diffuse_texture_name. This resolves the NO-TGA entries
where meshes reference a shader name instead of a texture name.

Output: tools/shader_to_texture.json
"""
import os, sys, re, json

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "re_tool", "config.json")

with open(CONFIG_PATH) as f:
    cfg = json.load(f)

EXPORT_DIR = cfg["umodel_export_dir"]
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "shader_to_texture.json")

# Patterns to extract texture references from .props.txt
# Shader: Diffuse = Texture'GroupName.TextureName'
# FacingShader: FacingDiffuse = Texture'GroupName.TextureName'
RE_DIFFUSE = re.compile(r"(?:Diffuse|FacingDiffuse|EdgeDiffuse)\s*=\s*Texture'([^']+)'")
RE_NORMAL = re.compile(r"NormalMap\s*=\s*Texture'([^']+)'")
RE_SPECULAR = re.compile(r"(?:Specular|SpecularityMask)\s*=\s*Texture'([^']+)'")
RE_SELFILLUM = re.compile(r"SelfIllumination\s*=\s*Texture'([^']+)'")

def parse_props_file(filepath):
    """Parse a .props.txt file and extract texture references."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except:
        return None
    
    result = {}
    
    m = RE_DIFFUSE.search(content)
    if m:
        # Format: "GroupName.TextureName" -> just TextureName
        full = m.group(1)
        result["diffuse"] = full.split('.')[-1] if '.' in full else full
        result["diffuse_full"] = full
    
    m = RE_NORMAL.search(content)
    if m:
        full = m.group(1)
        result["normal"] = full.split('.')[-1] if '.' in full else full
    
    m = RE_SPECULAR.search(content)
    if m:
        full = m.group(1)
        result["specular"] = full.split('.')[-1] if '.' in full else full
    
    m = RE_SELFILLUM.search(content)
    if m:
        full = m.group(1)
        result["emissive"] = full.split('.')[-1] if '.' in full else full
    
    return result if result else None

def main():
    shader_map = {}  # shader_name -> {diffuse, normal, specular, ...}
    total_files = 0
    resolved = 0
    
    # Scan all map export directories
    for map_dir in sorted(os.listdir(EXPORT_DIR)):
        map_path = os.path.join(EXPORT_DIR, map_dir)
        if not os.path.isdir(map_path):
            continue
        
        for shader_type in ["Shader", "FacingShader"]:
            shader_dir = os.path.join(map_path, shader_type)
            if not os.path.isdir(shader_dir):
                continue
            
            for f in os.listdir(shader_dir):
                if not f.endswith('.props.txt'):
                    continue
                
                total_files += 1
                shader_name = f.replace('.props.txt', '')
                filepath = os.path.join(shader_dir, f)
                
                textures = parse_props_file(filepath)
                if textures and "diffuse" in textures:
                    # Only add if we don't already have this shader, or if new entry has more info
                    if shader_name not in shader_map or len(textures) > len(shader_map[shader_name]):
                        shader_map[shader_name] = textures
                        resolved += 1
    
    # Also build a reverse map: texture_name -> [shader_names]
    # Useful for looking up which shaders use a given texture
    reverse_map = {}
    for shader_name, textures in shader_map.items():
        if "diffuse" in textures:
            tex = textures["diffuse"]
            if tex not in reverse_map:
                reverse_map[tex] = []
            reverse_map[tex].append(shader_name)
    
    # Build output
    db = {
        "version": 1,
        "stats": {
            "totalPropsFiles": total_files,
            "shadersWithDiffuse": len(shader_map),
            "uniqueTextures": len(reverse_map),
        },
        "shaderToTexture": shader_map,
        "textureToShaders": reverse_map,
    }
    
    with open(OUTPUT_PATH, 'w', encoding='utf-8') as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
    
    size_kb = os.path.getsize(OUTPUT_PATH) / 1024
    print(f"Parsed {total_files} .props.txt files")
    print(f"Resolved {len(shader_map)} shaders → diffuse texture")
    print(f"Unique textures referenced: {len(reverse_map)}")
    print(f"Output: {OUTPUT_PATH} ({size_kb:.0f} KB)")
    
    # Show some examples of what was resolved
    print(f"\nSample mappings:")
    examples = ["cam_smallcam_shader", "PistolShader", "Shotgun_NoUpgrades_Diffuse_shader", 
                 "WP_WrenchMesh_diffuse_shader", "frozenbase_Shader", "SanctuaryShell_Shader",
                 "ToiletWater_Shader", "Cyclone_Shader", "headlamprimshader",
                 "ammo_pickup_antipersonell_diffuse_shader", "ls_nursehat_rimshader",
                 "ConeDrillRimShader", "BJ_BlackWigRimShader", "LS_BlackHatRimShader"]
    for name in examples:
        if name in shader_map:
            print(f"  {name:45s} → {shader_map[name].get('diffuse', '?')}")
        else:
            # Try case-insensitive
            found = None
            for k in shader_map:
                if k.lower() == name.lower():
                    found = k
                    break
            if found:
                print(f"  {name:45s} → {shader_map[found].get('diffuse', '?')} (as {found})")
            else:
                print(f"  {name:45s} → NOT FOUND")

if __name__ == "__main__":
    main()
