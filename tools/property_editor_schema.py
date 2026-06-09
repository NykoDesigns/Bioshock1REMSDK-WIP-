#!/usr/bin/env python3
"""
Property Editor Schema Generator
Processes property_db.json into a compact schema file that the level editor's
PropertiesPanel can load to render typed property editors instead of raw hex.

Output: tools/prop_schema.json
  - Per-class: which properties exist, their types, enum values, defaults, ranges
  - Enum definitions with named values
  - Struct definitions (FVector, FRotator, FColor, etc.)
"""
import os, json, re
from collections import OrderedDict

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROP_DB_PATH = os.path.join(SCRIPT_DIR, "property_db.json")
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "prop_schema.json")

# Known struct types and their fields
KNOWN_STRUCTS = {
    "Vector": {"fields": [("X", "float"), ("Y", "float"), ("Z", "float")], "size": 12},
    "Rotator": {"fields": [("Pitch", "int"), ("Yaw", "int"), ("Roll", "int")], "size": 12},
    "Color": {"fields": [("R", "byte"), ("G", "byte"), ("B", "byte"), ("A", "byte")], "size": 4},
    "LinearColor": {"fields": [("R", "float"), ("G", "float"), ("B", "float"), ("A", "float")], "size": 16},
    "Scale": {"fields": [("X", "float"), ("Y", "float"), ("Z", "float"), ("SheerRate", "float"), ("SheerAxis", "byte")], "size": 17},
    "Plane": {"fields": [("X", "float"), ("Y", "float"), ("Z", "float"), ("W", "float")], "size": 16},
    "Box": {"fields": [("Min", "Vector"), ("Max", "Vector"), ("IsValid", "byte")], "size": 25},
    "Sphere": {"fields": [("Center", "Vector"), ("W", "float")], "size": 16},
    "Quat": {"fields": [("X", "float"), ("Y", "float"), ("Z", "float"), ("W", "float")], "size": 16},
    "Matrix": {"fields": [("XPlane", "Plane"), ("YPlane", "Plane"), ("ZPlane", "Plane"), ("WPlane", "Plane")], "size": 64},
    "IntPoint": {"fields": [("X", "int"), ("Y", "int")], "size": 8},
    "Range": {"fields": [("Min", "float"), ("Max", "float")], "size": 8},
    "RangeVector": {"fields": [("X", "Range"), ("Y", "Range"), ("Z", "Range")], "size": 24},
    "Guid": {"fields": [("A", "int"), ("B", "int"), ("C", "int"), ("D", "int")], "size": 16},
}

# Map property_db type names to editor widget types
TYPE_MAP = {
    "IntProperty": "int",
    "FloatProperty": "float",
    "BoolProperty": "bool",
    "StrProperty": "string",
    "NameProperty": "name",
    "ByteProperty": "byte",
    "ObjectProperty": "object",
    "ClassProperty": "class",
    "ArrayProperty": "array",
    "StructProperty": "struct",
    "DelegateProperty": "delegate",
    "ComponentProperty": "component",
    "InterfaceProperty": "interface",
    "MapProperty": "map",
}

# Property categories for organizing the editor panel
CATEGORY_RULES = [
    (["Location", "Rotation", "DrawScale", "DrawScale3D", "PrePivot", "Scale"],
     "Transform"),
    (["CollisionRadius", "CollisionHeight", "bCollide", "bBlock", "Physics",
      "Mass", "Buoyancy", "bPathColliding"],
     "Collision"),
    (["LightBrightness", "LightRadius", "LightColor", "LightHue", "LightSaturation",
      "LightType", "LightEffect", "bDirectional", "bCorona", "bSpecialLit",
      "bDynamic", "LightCone"],
     "Lighting"),
    (["StaticMesh", "Mesh", "Skins", "Materials", "DrawType", "Texture",
      "AmbientGlow", "ScaleGlow", "bUnlit", "bShadowCast", "bAcceptsProjectors"],
     "Rendering"),
    (["AmbientSound", "SoundRadius", "SoundVolume", "SoundPitch"],
     "Sound"),
    (["Tag", "Group", "Event", "bHidden", "bStatic", "bNoDelete", "Label",
      "InitialState", "bDeleteMe"],
     "General"),
    (["NetPriority", "bAlwaysRelevant", "RemoteRole", "Role", "NetUpdateFrequency",
      "bNetTemporary"],
     "Networking"),
]

def categorize_property(prop_name):
    for patterns, category in CATEGORY_RULES:
        for p in patterns:
            if p.lower() in prop_name.lower():
                return category
    return "Properties"

def main():
    with open(PROP_DB_PATH) as f:
        db = json.load(f)

    classes_dict = db.get("classes", {})
    print(f"[PropSchema] {len(classes_dict)} classes in property DB")

    # Collect all enums
    enums = {}
    for cls_name, cls_info in classes_dict.items():
        if not isinstance(cls_info, dict):
            continue
        for prop in cls_info.get("properties", []):
            if not isinstance(prop, dict):
                continue
            enum_name = prop.get("enumName", "")
            enum_values = prop.get("enumValues", [])
            if enum_name and enum_values and enum_name not in enums:
                enums[enum_name] = enum_values

    print(f"[PropSchema] {len(enums)} enums collected")

    # Build per-class property schemas
    class_schemas = {}
    total_props = 0

    for cls_name, cls_info in sorted(classes_dict.items()):
        if not isinstance(cls_info, dict):
            continue

        props = cls_info.get("properties", [])
        if not props:
            continue

        schema_props = []
        for prop in props:
            if not isinstance(prop, dict):
                continue

            prop_name = prop.get("name", "")
            prop_type_raw = prop.get("type", "")
            if not prop_name:
                continue

            # Resolve type
            editor_type = TYPE_MAP.get(prop_type_raw, "unknown")

            entry = {
                "name": prop_name,
                "type": editor_type,
                "category": categorize_property(prop_name),
            }

            # Add enum info
            enum_name = prop.get("enumName", "")
            if enum_name:
                entry["enum"] = enum_name

            # Add struct info
            struct_name = prop.get("structName", "")
            if struct_name:
                entry["struct"] = struct_name

            # Add inner type for arrays
            inner_type = prop.get("innerType", "")
            if inner_type:
                entry["innerType"] = inner_type

            # Add default value if present
            default_val = prop.get("default")
            if default_val is not None:
                entry["default"] = default_val

            # Add resolved type info (e.g. "TArray<StaticMeshActor>")
            resolved = prop.get("resolvedType", "")
            if resolved:
                entry["resolved"] = resolved

            schema_props.append(entry)
            total_props += 1

        parent = cls_info.get("parent", "")
        class_schemas[cls_name] = {
            "parent": parent,
            "properties": schema_props,
        }

    # Build category summary
    categories = {}
    for cls_name, schema in class_schemas.items():
        for prop in schema["properties"]:
            cat = prop.get("category", "Properties")
            categories[cat] = categories.get(cat, 0) + 1

    # Output
    output = {
        "version": 1,
        "description": "Property editor schema for BS1LevelEditor PropertiesPanel",
        "stats": {
            "classes": len(class_schemas),
            "totalProperties": total_props,
            "enums": len(enums),
            "structs": len(KNOWN_STRUCTS),
            "categories": categories,
        },
        "structs": KNOWN_STRUCTS,
        "enums": enums,
        "classes": class_schemas,
    }

    with open(OUTPUT_PATH, 'w', encoding='utf-8') as f:
        json.dump(output, f, indent=1, ensure_ascii=False)

    size_kb = os.path.getsize(OUTPUT_PATH) / 1024
    print(f"\n[PropSchema] Output: {OUTPUT_PATH} ({size_kb:.0f} KB)")
    print(f"  Classes: {len(class_schemas)}")
    print(f"  Properties: {total_props}")
    print(f"  Enums: {len(enums)}")
    print(f"  Structs: {len(KNOWN_STRUCTS)}")
    print(f"\n  Categories:")
    for cat, count in sorted(categories.items(), key=lambda x: -x[1]):
        print(f"    {cat}: {count}")

if __name__ == "__main__":
    main()
