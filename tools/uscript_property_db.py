#!/usr/bin/env python3
"""
Tool 2: UnrealScript Property Database
Parses all decompiled .uc files and generates a JSON database of:
- Every class with its parent class
- All properties (name, type, default value)
- Inheritance chains
- Class categories (Actor subtypes, etc.)

Output: tools/property_db.json
"""
import os, sys, re, json

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "re_tool", "config.json")

with open(CONFIG_PATH) as f:
    cfg = json.load(f)

DECOMPILED_DIR = cfg["decompiled_dir"]
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "property_db.json")

# Regex patterns for UnrealScript parsing
RE_CLASS = re.compile(r'^\s*class\s+(\w+)\s+extends\s+(\w+)', re.MULTILINE)
RE_VAR = re.compile(
    r'^\s*var(?:\s*\(([^)]*)\))?\s+'           # var or var(group)
    r'(?:(config|transient|const|editconst|native|private|protected|localized|globalconfig|input|travel|export|editinline|deprecated|noexport|editconstarray|edfindable|interp|repnotify|databinding|edithide|editfixedsize|noclear|editinlineuse|noimport|duplicatetransient|serializetext|repretry)\s+)*'  # modifiers
    r'(\w[\w<>\s,.*]*?)\s+'                     # type
    r'(\w+)'                                     # name
    r'(?:\[([^\]]+)\])?\s*;',                    # optional array size
    re.MULTILINE
)
RE_DEFAULT = re.compile(r'^\s*(\w+)\s*=\s*(.+)', re.MULTILINE)
RE_ENUM = re.compile(r'^\s*enum\s+(\w+)\s*\{([^}]*)\}', re.MULTILINE | re.DOTALL)
RE_STRUCT = re.compile(r'^\s*struct\s+(?:native\s+)?(\w+)', re.MULTILINE)
RE_FUNCTION = re.compile(
    r'^\s*(?:(native|exec|event|static|final|simulated|latent|iterator|singular|private|protected)\s+)*'
    r'function\s+(?:(\w[\w<>]*)\s+)?(\w+)\s*\(',
    re.MULTILINE
)

def parse_uc_file(filepath):
    """Parse a single .uc file and extract class info."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except:
        return None
    
    # Find class declaration
    class_match = RE_CLASS.search(content)
    if not class_match:
        return None
    
    class_name = class_match.group(1)
    parent_name = class_match.group(2)
    
    # Extract properties
    properties = []
    for m in RE_VAR.finditer(content):
        group = m.group(1) or ""
        prop_type = m.group(3).strip() if m.group(3) else "unknown"
        prop_name = m.group(4)
        array_size = m.group(5)
        
        prop = {
            "name": prop_name,
            "type": prop_type,
        }
        if group:
            prop["group"] = group
        if array_size:
            prop["arraySize"] = array_size
        properties.append(prop)
    
    # Extract default values from defaultproperties block
    defaults = {}
    dp_match = re.search(r'defaultproperties\s*\{(.*?)\}', content, re.DOTALL | re.IGNORECASE)
    if dp_match:
        dp_block = dp_match.group(1)
        for dm in RE_DEFAULT.finditer(dp_block):
            defaults[dm.group(1)] = dm.group(2).strip()
    
    # Extract enums
    enums = {}
    for em in RE_ENUM.finditer(content):
        enum_name = em.group(1)
        enum_body = em.group(2)
        values = [v.strip().rstrip(',') for v in enum_body.split('\n') if v.strip() and not v.strip().startswith('//')]
        enums[enum_name] = values
    
    # Extract structs (just names)
    structs = [sm.group(1) for sm in RE_STRUCT.finditer(content)]
    
    # Extract functions
    functions = []
    for fm in RE_FUNCTION.finditer(content):
        modifiers = fm.group(1) or ""
        ret_type = fm.group(2) or "void"
        func_name = fm.group(3)
        functions.append({
            "name": func_name,
            "returnType": ret_type,
            "modifiers": modifiers,
        })
    
    result = {
        "className": class_name,
        "parent": parent_name,
        "properties": properties,
        "file": os.path.relpath(filepath, DECOMPILED_DIR).replace('\\', '/'),
    }
    
    if defaults:
        result["defaults"] = defaults
    if enums:
        result["enums"] = enums
    if structs:
        result["structs"] = structs
    if functions:
        result["functions"] = functions
    
    return result

def build_inheritance(classes):
    """Build inheritance chains for all classes."""
    class_map = {c["className"]: c for c in classes}
    
    for cls in classes:
        chain = []
        current = cls["className"]
        seen = set()
        while current in class_map and current not in seen:
            seen.add(current)
            chain.append(current)
            current = class_map[current].get("parent", "")
        if current and current not in seen:
            chain.append(current)  # add root even if not in our DB (e.g., "Object")
        cls["inheritanceChain"] = chain
    
    return classes

def categorize_class(cls):
    """Categorize a class based on its inheritance chain."""
    chain = cls.get("inheritanceChain", [])
    chain_set = set(chain)
    
    if "Light" in chain_set or "PointLight" in chain_set or "SpotLight" in chain_set:
        return "Light"
    if "Pawn" in chain_set:
        return "Pawn"
    if "Controller" in chain_set or "AIController" in chain_set:
        return "AI"
    if "Trigger" in chain_set:
        return "Trigger"
    if "Volume" in chain_set:
        return "Volume"
    if "Pickup" in chain_set or "Inventory" in chain_set or "Weapon" in chain_set:
        return "Pickup"
    if "Emitter" in chain_set:
        return "Effect"
    if "NavigationPoint" in chain_set or "PathNode" in chain_set:
        return "Navigation"
    if "Actor" in chain_set:
        return "Actor"
    return "Other"

def main():
    if not os.path.isdir(DECOMPILED_DIR):
        print(f"ERROR: Decompiled dir not found: {DECOMPILED_DIR}")
        sys.exit(1)
    
    # Find all .uc files
    uc_files = []
    for root, dirs, files in os.walk(DECOMPILED_DIR):
        for f in files:
            if f.endswith('.uc'):
                uc_files.append(os.path.join(root, f))
    
    print(f"Found {len(uc_files)} .uc files in {DECOMPILED_DIR}")
    
    # Parse all files
    classes = []
    errors = 0
    for filepath in sorted(uc_files):
        result = parse_uc_file(filepath)
        if result:
            classes.append(result)
        else:
            errors += 1
    
    print(f"Parsed {len(classes)} classes ({errors} files had no class declaration)")
    
    # Build inheritance chains
    classes = build_inheritance(classes)
    
    # Categorize
    categories = {}
    for cls in classes:
        cat = categorize_class(cls)
        cls["category"] = cat
        categories[cat] = categories.get(cat, 0) + 1
    
    # Count total properties
    total_props = sum(len(c.get("properties", [])) for c in classes)
    total_defaults = sum(len(c.get("defaults", {})) for c in classes)
    total_funcs = sum(len(c.get("functions", [])) for c in classes)
    total_enums = sum(len(c.get("enums", {})) for c in classes)
    
    # Build output
    db = {
        "version": 1,
        "source": DECOMPILED_DIR,
        "stats": {
            "totalClasses": len(classes),
            "totalProperties": total_props,
            "totalDefaults": total_defaults,
            "totalFunctions": total_funcs,
            "totalEnums": total_enums,
            "categories": categories,
        },
        "classes": {c["className"]: c for c in classes},
    }
    
    with open(OUTPUT_PATH, 'w', encoding='utf-8') as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
    
    size_mb = os.path.getsize(OUTPUT_PATH) / (1024*1024)
    print(f"\nOutput: {OUTPUT_PATH} ({size_mb:.1f} MB)")
    print(f"\nStats:")
    print(f"  Classes:    {len(classes)}")
    print(f"  Properties: {total_props}")
    print(f"  Defaults:   {total_defaults}")
    print(f"  Functions:  {total_funcs}")
    print(f"  Enums:      {total_enums}")
    print(f"\nCategories:")
    for cat, count in sorted(categories.items(), key=lambda x: -x[1]):
        print(f"  {cat:15s} {count}")

if __name__ == "__main__":
    main()
