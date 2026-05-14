#include "sdk_generator.h"
#include "../core/log.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>
#include <filesystem>
#include <cstdio>

namespace bs1sdk {

// ─── Public entry point ─────────────────────────────────────────────────

int SDKGenerator::Generate(const std::string& outputDir)
{
    LOG_INFO("SDK Generator: Starting...");

    m_Classes.clear();
    m_Packages.clear();
    m_Inheritance.clear();
    m_TotalProps = 0;
    m_TotalFuncs = 0;

    // Create output directory
    std::filesystem::create_directories(outputDir);

    DiscoverClasses();
    CollectProperties();
    CollectFunctions();
    WriteHeaders(outputDir);
    WriteMasterHeader(outputDir);

    LOG_INFO("SDK Generator: Done! {} classes, {} properties, {} functions, {} packages",
             m_Classes.size(), m_TotalProps, m_TotalFuncs, m_Packages.size());

    return (int)m_Classes.size();
}

// ─── Step 1: Discover all UClass objects ────────────────────────────────

void SDKGenerator::DiscoverClasses()
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;

        UObject* obj = reinterpret_cast<UObject*>(ptr);
        std::string className = obj->GetObjClassName();

        // We want Class objects and ScriptStruct objects
        if (className != "Class" && className != "ScriptStruct") continue;

        std::string name = obj->GetName();
        if (name.empty() || name == "<invalid>") continue;

        // Skip if already found (first instance wins)
        if (m_Classes.count(name)) continue;

        UStruct* cls = reinterpret_cast<UStruct*>(obj);
        ClassInfo info;
        info.ClassObj = cls;
        info.Name = name;
        info.PackageName = GetPackageName(obj);
        info.PropertiesSize = cls->GetPropertiesSize();

        // Get super class name
        UField* super = cls->GetSuperField();
        if (super) {
            info.SuperClassName = super->GetName();
            m_Inheritance[name] = info.SuperClassName;
        }

        m_Classes[name] = info;
        m_Packages[info.PackageName].push_back(name);
    }

    // Sort class lists within each package
    for (auto& [pkg, classes] : m_Packages) {
        std::sort(classes.begin(), classes.end());
    }

    LOG_INFO("SDK Generator: Found {} classes in {} packages",
             m_Classes.size(), m_Packages.size());
}

// ─── Step 2: Collect direct properties for each class ──────────────────

void SDKGenerator::CollectProperties()
{
    for (auto& [name, info] : m_Classes) {
        if (!info.ClassObj) continue;

        // Walk only this class's Children chain (not inherited)
        UField* child = info.ClassObj->GetChildren();
        int limit = 2000;

        while (child && limit-- > 0) {
            std::string childClass = child->GetObjClassName();
            if (childClass.find("Property") != std::string::npos) {
                UProperty* prop = reinterpret_cast<UProperty*>(child);
                PropertyInfo pi;
                pi.Name = prop->GetName();
                pi.TypeName = childClass;
                pi.Offset = prop->GetPropertyOffset();
                pi.ElementSize = prop->GetElementSize();
                pi.ArrayDim = prop->GetArrayDim();
                pi.PropertyObj = prop;
                info.OwnProperties.push_back(pi);
                m_TotalProps++;
            }
            child = child->GetNext();
        }

        // Sort properties by offset for clean output
        std::sort(info.OwnProperties.begin(), info.OwnProperties.end(),
                  [](const PropertyInfo& a, const PropertyInfo& b) {
                      return a.Offset < b.Offset;
                  });
    }
}

// ─── Step 2b: Collect functions for each class ─────────────────────────

void SDKGenerator::CollectFunctions()
{
    for (auto& [name, info] : m_Classes) {
        if (!info.ClassObj) continue;

        UField* child = info.ClassObj->GetChildren();
        int limit = 2000;

        while (child && limit-- > 0) {
            std::string childClass = child->GetObjClassName();
            if (childClass == "Function") {
                UFunction* func = reinterpret_cast<UFunction*>(child);
                FunctionSDKInfo fi;
                fi.Name = func->GetName();
                fi.Flags = func->GetFunctionFlags();
                fi.NativeIndex = func->GetNativeIndex();
                fi.IsNative = func->IsNative();
                fi.IsEvent = func->IsEvent();
                fi.IsExec = func->IsExec();
                fi.IsLatent = func->IsLatent();

                // Build signature string
                std::string retType = "void";
                std::string params;

                UField* paramChild = func->GetChildren();
                int paramLimit = 200;
                while (paramChild && paramLimit-- > 0) {
                    std::string pClass = paramChild->GetObjClassName();
                    if (pClass.find("Property") != std::string::npos) {
                        UProperty* prop = reinterpret_cast<UProperty*>(paramChild);
                        uint32_t pflags = prop->GetPropertyFlags();
                        if (pflags & 0x0080) { // CPF_Parm
                            PropertyInfo pi;
                            pi.Name = prop->GetName();
                            pi.TypeName = pClass;
                            pi.ElementSize = prop->GetElementSize();
                            pi.PropertyObj = prop;
                            pi.Offset = prop->GetPropertyOffset();
                            pi.ArrayDim = prop->GetArrayDim();

                            std::string mappedType = MapPropertyType(pi);

                            if (pflags & 0x0400) { // CPF_ReturnParm
                                retType = mappedType;
                            } else {
                                if (!params.empty()) params += ", ";
                                if (pflags & 0x0100) params += "out "; // CPF_OutParm
                                params += mappedType + " " + pi.Name;
                            }
                        }
                    }
                    paramChild = paramChild->GetNext();
                }

                fi.Signature = retType + " " + fi.Name + "(" + params + ")";
                info.OwnFunctions.push_back(fi);
                m_TotalFuncs++;
            }
            child = child->GetNext();
        }
    }

    LOG_INFO("SDK Generator: Collected {} functions", m_TotalFuncs);
}

// ─── Step 3: Write per-package headers ─────────────────────────────────

void SDKGenerator::WriteHeaders(const std::string& outputDir)
{
    for (auto& [pkg, classNames] : m_Packages) {
        std::string filename = outputDir + "/" + pkg + ".h";
        std::ofstream out(filename);
        if (!out.is_open()) {
            LOG_WARN("SDK Generator: Failed to open {}", filename);
            continue;
        }

        // Header guard
        std::string guard = pkg;
        std::transform(guard.begin(), guard.end(), guard.begin(), ::toupper);
        for (auto& c : guard) { if (!isalnum(c)) c = '_'; }

        out << "#pragma once\n";
        out << "// Auto-generated SDK header for package: " << pkg << "\n";
        out << "// Generated by BS1SDK from live runtime data\n";
        out << "// DO NOT EDIT - regenerate with F5 in overlay\n";
        out << "\n";
        out << "#include \"SDK_Types.h\"\n";
        out << "\n";
        out << "namespace sdk {\n";
        out << "\n";

        // Forward declarations for all classes in this package
        out << "// ─── Forward Declarations ────────────────────────────────────\n";
        for (auto& cn : classNames) {
            auto& info = m_Classes[cn];
            std::string prefix = GetClassPrefix(cn);
            out << "class " << prefix << cn << ";\n";
        }
        out << "\n";

        // Class definitions
        for (auto& cn : classNames) {
            auto& info = m_Classes[cn];
            std::string prefix = GetClassPrefix(cn);
            std::string fullName = prefix + cn;

            out << "// ─────────────────────────────────────────────────────────────\n";
            out << "// " << fullName;
            if (!info.SuperClassName.empty()) {
                out << " : public " << GetClassPrefix(info.SuperClassName) << info.SuperClassName;
            }
            out << "\n";
            out << "// Size: 0x" << std::hex << info.PropertiesSize << std::dec
                << " (" << info.PropertiesSize << " bytes)\n";
            out << "// Package: " << info.PackageName << "\n";
            out << "// ─────────────────────────────────────────────────────────────\n";

            out << "class " << fullName;
            if (!info.SuperClassName.empty()) {
                out << " : public " << GetClassPrefix(info.SuperClassName) << info.SuperClassName;
            }
            out << " {\n";
            out << "public:\n";

            if (info.OwnProperties.empty() && info.OwnFunctions.empty()) {
                out << "    // No properties at this class level\n";
            }

            // Emit properties
            for (auto& prop : info.OwnProperties) {
                std::string cppType = MapPropertyType(prop);
                std::string arrayStr;
                if (prop.ArrayDim > 1) {
                    arrayStr = "[" + std::to_string(prop.ArrayDim) + "]";
                }

                char offsetStr[32];
                std::snprintf(offsetStr, sizeof(offsetStr), "0x%04X", prop.Offset);

                out << "    " << cppType << " " << prop.Name << arrayStr << ";";
                out << " // " << offsetStr << " (" << prop.TypeName
                    << ", " << prop.ElementSize << "B)\n";
            }

            // Emit functions
            if (!info.OwnFunctions.empty()) {
                out << "\n    // ─── Functions ───\n";
                for (auto& fi : info.OwnFunctions) {
                    out << "    // " << fi.Signature;
                    if (fi.IsNative) out << " [Native " << fi.NativeIndex << "]";
                    if (fi.IsEvent) out << " [Event]";
                    if (fi.IsExec) out << " [Exec]";
                    out << "\n";
                }
            }

            out << "\n";
            out << "    static UStruct* StaticClass() { return FindClass(\"" << cn << "\"); }\n";
            out << "};\n";

            // Static assert for size if we have it
            if (info.PropertiesSize > 0) {
                out << "// static_assert(sizeof(" << fullName << ") == 0x"
                    << std::hex << info.PropertiesSize << std::dec
                    << ", \"Size mismatch\");\n";
            }
            out << "\n";
        }

        out << "} // namespace sdk\n";
        out.close();
    }
}

// ─── Step 4: Master SDK.h ──────────────────────────────────────────────

void SDKGenerator::WriteMasterHeader(const std::string& outputDir)
{
    // Write SDK_Types.h with basic type definitions
    {
        std::ofstream out(outputDir + "/SDK_Types.h");
        out << "#pragma once\n";
        out << "// Auto-generated SDK base types\n";
        out << "// Generated by BS1SDK from live runtime data\n";
        out << "\n";
        out << "#include <cstdint>\n";
        out << "\n";
        out << "namespace sdk {\n";
        out << "\n";
        out << "// Forward declare engine base\n";
        out << "class UObject;\n";
        out << "class UStruct;\n";
        out << "UStruct* FindClass(const char* name);\n";
        out << "\n";
        out << "// ─── Basic Types ─────────────────────────────────────────────\n";
        out << "\n";
        out << "struct FName {\n";
        out << "    int32_t Index;\n";
        out << "    int32_t Number;\n";
        out << "};\n";
        out << "\n";
        out << "struct FString {\n";
        out << "    wchar_t* Data;\n";
        out << "    int32_t Count;\n";
        out << "    int32_t Max;\n";
        out << "};\n";
        out << "\n";
        out << "template<typename T>\n";
        out << "struct TArray {\n";
        out << "    T* Data;\n";
        out << "    int32_t Count;\n";
        out << "    int32_t Max;\n";
        out << "};\n";
        out << "\n";
        out << "struct FVector {\n";
        out << "    float X, Y, Z;\n";
        out << "};\n";
        out << "\n";
        out << "struct FRotator {\n";
        out << "    int32_t Pitch, Yaw, Roll;\n";
        out << "};\n";
        out << "\n";
        out << "struct FColor {\n";
        out << "    uint8_t B, G, R, A;\n";
        out << "};\n";
        out << "\n";
        out << "struct FLinearColor {\n";
        out << "    float R, G, B, A;\n";
        out << "};\n";
        out << "\n";
        out << "struct FVector2D {\n";
        out << "    float X, Y;\n";
        out << "};\n";
        out << "\n";
        out << "struct FQuat {\n";
        out << "    float X, Y, Z, W;\n";
        out << "};\n";
        out << "\n";
        out << "struct FMatrix {\n";
        out << "    float M[4][4];\n";
        out << "};\n";
        out << "\n";
        out << "struct FGuid {\n";
        out << "    uint32_t A, B, C, D;\n";
        out << "};\n";
        out << "\n";
        out << "struct FScriptDelegate {\n";
        out << "    UObject* Object;\n";
        out << "    FName FunctionName;\n";
        out << "};\n";
        out << "\n";
        out << "// QWord for ObjectFlags\n";
        out << "typedef uint64_t QWord;\n";
        out << "\n";
        out << "} // namespace sdk\n";
        out.close();
    }

    // Write master SDK.h
    {
        std::ofstream out(outputDir + "/SDK.h");
        out << "#pragma once\n";
        out << "// Auto-generated BioShock Remastered SDK\n";
        out << "// Generated by BS1SDK from live runtime data\n";
        out << "//\n";
        out << "// Classes: " << m_Classes.size() << "\n";
        out << "// Properties: " << m_TotalProps << "\n";
        out << "// Packages: " << m_Packages.size() << "\n";
        out << "\n";
        out << "#include \"SDK_Types.h\"\n";
        out << "\n";

        // Include packages in dependency order (Core first, then Engine, then game)
        std::vector<std::string> orderedPkgs;
        // Priority packages first
        std::vector<std::string> priorityOrder = {"Core", "Engine"};
        for (auto& p : priorityOrder) {
            if (m_Packages.count(p)) orderedPkgs.push_back(p);
        }
        // Then everything else alphabetically
        for (auto& [pkg, _] : m_Packages) {
            if (pkg != "Core" && pkg != "Engine")
                orderedPkgs.push_back(pkg);
        }

        for (auto& pkg : orderedPkgs) {
            out << "#include \"" << pkg << ".h\"\n";
        }
        out.close();
    }

    // Write a stats summary
    {
        std::ofstream out(outputDir + "/SDK_STATS.txt");
        out << "BS1SDK - Auto-Generated SDK Statistics\n";
        out << "======================================\n\n";
        out << "Total Classes: " << m_Classes.size() << "\n";
        out << "Total Properties: " << m_TotalProps << "\n";
        out << "Total Packages: " << m_Packages.size() << "\n\n";

        out << "Packages:\n";
        for (auto& [pkg, classes] : m_Packages) {
            out << "  " << pkg << ": " << classes.size() << " classes\n";
        }

        out << "\nClass Hierarchy (sorted by size):\n";
        std::vector<std::pair<std::string, ClassInfo*>> sorted;
        for (auto& [name, info] : m_Classes) {
            sorted.push_back({name, &info});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) {
                      return a.second->PropertiesSize > b.second->PropertiesSize;
                  });
        for (auto& [name, info] : sorted) {
            char line[256];
            std::snprintf(line, sizeof(line), "  0x%04X (%5d) %-40s : %s  [%d props]\n",
                         info->PropertiesSize, info->PropertiesSize,
                         name.c_str(),
                         info->SuperClassName.empty() ? "(root)" : info->SuperClassName.c_str(),
                         (int)info->OwnProperties.size());
            out << line;
        }
        out.close();
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────

std::string SDKGenerator::MapPropertyType(const PropertyInfo& prop)
{
    const std::string& t = prop.TypeName;

    if (t == "IntProperty")        return "int32_t";
    if (t == "FloatProperty")      return "float";
    if (t == "BoolProperty")       return "uint32_t";  // bitmask
    if (t == "ByteProperty") {
        // Resolve inner enum if available
        if (prop.PropertyObj) {
            UByteProperty* bp = reinterpret_cast<UByteProperty*>(prop.PropertyObj);
            UEnum* innerEnum = bp->GetEnum();
            if (innerEnum && (uintptr_t)innerEnum > 0x10000) {
                std::string enumName = innerEnum->GetName();
                if (!enumName.empty() && enumName != "<invalid>")
                    return "/* " + enumName + " */ uint8_t";
            }
        }
        return "uint8_t";
    }
    if (t == "NameProperty")       return "FName";
    if (t == "StrProperty")        return "FString";
    if (t == "ObjectProperty" || t == "ComponentProperty") {
        // Resolve inner class
        if (prop.PropertyObj) {
            UObjectProperty* op = reinterpret_cast<UObjectProperty*>(prop.PropertyObj);
            UObject* innerCls = op->GetPropertyClass();
            if (innerCls && (uintptr_t)innerCls > 0x10000) {
                std::string clsName = innerCls->GetName();
                if (!clsName.empty() && clsName != "<invalid>") {
                    std::string prefix = InheritsFromActor(clsName) ? "A" : "U";
                    return "class " + prefix + clsName + "*";
                }
            }
        }
        return "class UObject*";
    }
    if (t == "ClassProperty") {
        if (prop.PropertyObj) {
            UClassProperty* cp = reinterpret_cast<UClassProperty*>(prop.PropertyObj);
            UObject* metaCls = cp->GetMetaClass();
            if (metaCls && (uintptr_t)metaCls > 0x10000) {
                std::string clsName = metaCls->GetName();
                if (!clsName.empty() && clsName != "<invalid>")
                    return "class UClass* /* " + clsName + " */";
            }
        }
        return "class UClass*";
    }
    if (t == "InterfaceProperty") {
        if (prop.PropertyObj) {
            UInterfaceProperty* ip = reinterpret_cast<UInterfaceProperty*>(prop.PropertyObj);
            UObject* ifaceCls = ip->GetInterfaceClass();
            if (ifaceCls && (uintptr_t)ifaceCls > 0x10000) {
                std::string clsName = ifaceCls->GetName();
                if (!clsName.empty() && clsName != "<invalid>")
                    return "FScriptInterface /* " + clsName + " */";
            }
        }
        return "FScriptInterface";
    }
    if (t == "StructProperty") {
        // Resolve the actual inner struct name
        if (prop.PropertyObj) {
            UStructProperty* sp = reinterpret_cast<UStructProperty*>(prop.PropertyObj);
            UStruct* innerStruct = sp->GetStruct();
            if (innerStruct && (uintptr_t)innerStruct > 0x10000) {
                std::string sName = innerStruct->GetName();
                if (!sName.empty() && sName != "<invalid>") {
                    // Map to known C++ types
                    if (sName == "Vector")       return "FVector";
                    if (sName == "Rotator")      return "FRotator";
                    if (sName == "Color")        return "FColor";
                    if (sName == "LinearColor")  return "FLinearColor";
                    if (sName == "Vector2D")     return "FVector2D";
                    if (sName == "Quat")         return "FQuat";
                    if (sName == "Matrix")       return "FMatrix";
                    if (sName == "Guid")         return "FGuid";
                    if (sName == "Box")          return "FBox";
                    if (sName == "Plane")        return "FPlane";
                    // Generic struct — emit as F-prefixed
                    return "F" + sName;
                }
            }
        }
        // Fallback by size
        if (prop.ElementSize == 12) return "FVector";
        if (prop.ElementSize == 16) return "FLinearColor";
        return "uint8_t[" + std::to_string(prop.ElementSize) + "]";
    }
    if (t == "ArrayProperty") {
        // Resolve inner property type
        if (prop.PropertyObj) {
            UArrayProperty* ap = reinterpret_cast<UArrayProperty*>(prop.PropertyObj);
            UProperty* inner = ap->GetInner();
            if (inner && (uintptr_t)inner > 0x10000) {
                std::string innerClass = inner->GetObjClassName();
                // Recursively map the inner type
                PropertyInfo innerPI;
                innerPI.TypeName = innerClass;
                innerPI.ElementSize = inner->GetElementSize();
                innerPI.PropertyObj = inner;
                innerPI.Name = "Inner";
                innerPI.ArrayDim = 1;
                innerPI.Offset = 0;
                std::string innerType = MapPropertyType(innerPI);
                // Clean up array notation from inner type
                if (innerType.find('[') != std::string::npos)
                    innerType = "uint8_t"; // fallback for raw byte arrays
                return "TArray<" + innerType + ">";
            }
        }
        return "TArray<uint8_t>";
    }
    if (t == "MapProperty") {
        if (prop.PropertyObj) {
            UMapProperty* mp = reinterpret_cast<UMapProperty*>(prop.PropertyObj);
            UProperty* key = mp->GetKey();
            UProperty* value = mp->GetValue();
            if (key && value && (uintptr_t)key > 0x10000 && (uintptr_t)value > 0x10000) {
                // Simple map description
                return "TMap<" + key->GetObjClassName() + ", " + value->GetObjClassName() + ">";
            }
        }
        return "TMap<uint8_t, uint8_t>";
    }
    if (t == "DelegateProperty")   return "FScriptDelegate";
    if (t == "PointerProperty")    return "void*";

    // Unknown - emit as raw bytes
    return "uint8_t[" + std::to_string(prop.ElementSize) + "]";
}

std::string SDKGenerator::GetPackageName(UObject* obj)
{
    if (!obj) return "Unknown";

    // Walk Outer chain to find top-level package
    UObject* current = obj;
    UObject* topLevel = obj;
    int depth = 0;

    while (current && depth < 32) {
        UObject* outer = current->GetOuter();
        if (!outer) {
            topLevel = current;
            break;
        }
        current = outer;
        topLevel = current;
        depth++;
    }

    std::string name = topLevel->GetName();
    return name.empty() ? "Unknown" : name;
}

std::string SDKGenerator::GetClassPrefix(const std::string& className)
{
    if (InheritsFromActor(className)) return "A";
    // Special cases
    if (className == "Object" || className == "Field" || className == "Struct" ||
        className == "Class" || className == "State" || className == "Property" ||
        className == "Function" || className == "Enum" || className == "Const" ||
        className == "Package")
        return "U";
    return "U";
}

bool SDKGenerator::InheritsFromActor(const std::string& className)
{
    std::string current = className;
    int depth = 0;

    while (!current.empty() && depth < 64) {
        if (current == "Actor") return true;
        auto it = m_Inheritance.find(current);
        if (it == m_Inheritance.end()) break;
        current = it->second;
        depth++;
    }
    return false;
}

} // namespace bs1sdk
