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
    WriteEnums(outputDir);
    WriteCDODefaults(outputDir);
    WriteStates(outputDir);
    WriteNativeFunctions(outputDir);
    WriteStructs(outputDir);

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

        // We want Class objects and Struct/ScriptStruct objects
        if (className != "Class" && className != "ScriptStruct" && className != "Struct") continue;

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

// ─── Step 5: Dump all enums with their values ────────────────────────────

void SDKGenerator::WriteEnums(const std::string& outputDir)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    std::ofstream out(outputDir + "/SDK_Enums.h");
    out << "#pragma once\n";
    out << "// Auto-generated SDK enums - ALL game enum values\n";
    out << "// Generated by BS1SDK from live runtime data\n\n";
    out << "namespace sdk {\n\n";

    int enumCount = 0;
    int enumsFound = 0;
    int enumsSkippedCount = 0;
    int enumsSkippedData = 0;

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() != "Enum") continue;

        UEnum* uenum = reinterpret_cast<UEnum*>(obj);
        std::string enumName = uenum->GetName();
        if (enumName.empty() || enumName == "<invalid>") continue;
        enumsFound++;

        // Get owner class
        std::string ownerName;
        UObject* outer = uenum->GetOuter();
        if (outer && (uintptr_t)outer > 0x10000) {
            ownerName = outer->GetName();
        }

        // Probe for TArray<FName> at various offsets after UField (0x48)
        // TArray layout: { ptr Data, int32 Count, int32 Max }
        // Valid if: Data is a valid pointer (>0x10000), Count in [1..512], Max >= Count
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(uenum);
        FName* data = nullptr;
        int32_t count = 0;
        int probeOffset = -1;

        for (int off = 0x48; off <= 0x60; off += 4) {
            uintptr_t candidateData = *reinterpret_cast<const uintptr_t*>(raw + off);
            int32_t candidateCount = *reinterpret_cast<const int32_t*>(raw + off + 4);
            int32_t candidateMax = *reinterpret_cast<const int32_t*>(raw + off + 8);
            if (candidateData > 0x10000 && candidateCount > 0 && candidateCount <= 512
                && candidateMax >= candidateCount && candidateMax <= 1024) {
                data = reinterpret_cast<FName*>(candidateData);
                count = candidateCount;
                probeOffset = off;
                break;
            }
        }

        // Diagnostic: log first few enums
        if (enumsFound <= 5) {
            LOG_INFO("[SDK] Enum '{}' owner='{}' probeOffset=0x{:02X} data=0x{:08X} count={}",
                     enumName, ownerName, probeOffset, (uint32_t)(uintptr_t)data, count);
            // Dump raw bytes for further analysis
            LOG_INFO("[SDK]   Raw +0x48..+0x63: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                     *reinterpret_cast<const uint32_t*>(raw + 0x48),
                     *reinterpret_cast<const uint32_t*>(raw + 0x4C),
                     *reinterpret_cast<const uint32_t*>(raw + 0x50),
                     *reinterpret_cast<const uint32_t*>(raw + 0x54),
                     *reinterpret_cast<const uint32_t*>(raw + 0x58),
                     *reinterpret_cast<const uint32_t*>(raw + 0x5C),
                     *reinterpret_cast<const uint32_t*>(raw + 0x60));
        }

        if (!data || count <= 0 || count > 512) {
            if (!data) enumsSkippedData++;
            else enumsSkippedCount++;
            continue;
        }

        out << "// Owner: " << ownerName << "\n";
        out << "enum " << enumName << " {\n";
        for (int32_t j = 0; j < count; j++) {
            std::string valueName = data[j].ToString();
            if (valueName.empty() || valueName == "<invalid>") continue;
            out << "    " << valueName << " = " << j << ",\n";
        }
        out << "};\n\n";
        enumCount++;
    }

    out << "} // namespace sdk\n";
    out.close();
    LOG_INFO("SDK Generator: Enums: found={} dumped={} skippedData={} skippedCount={}",
             enumsFound, enumCount, enumsSkippedData, enumsSkippedCount);
}

// ─── Step 6: Dump CDO defaults ──────────────────────────────────────────

void SDKGenerator::WriteCDODefaults(const std::string& outputDir)
{
    std::ofstream out(outputDir + "/SDK_Defaults.txt");
    out << "// BioShock Remastered - ClassDefaultObject Values\n";
    out << "// These are the DEFAULT values for every property on key game classes.\n";
    out << "// Generated by BS1SDK from live runtime data\n\n";

    // Key classes we care about for modding
    std::vector<std::string> keyClasses = {
        "Actor", "Pawn", "Controller", "AIController", "PlayerController",
        "ShockPlayer", "ShockPlayerController",
        "Weapon", "ShockWeapon", "Inventory",
        "Projectile", "ShockProjectile",
        "NavigationPoint", "Volume", "PhysicsVolume",
        "Light", "PointLight", "SpotLight",
        "Trigger", "ScriptedTrigger",
        "ThugMelee", "Splicer", "BigDaddy",
        "GameInfo", "ShockGameInfo",
        "HUD", "ShockHUD",
        "LevelInfo", "ZoneInfo"
    };

    int dumped = 0;
    int cdoChecked = 0;
    for (auto& className : keyClasses) {
        auto it = m_Classes.find(className);
        if (it == m_Classes.end()) continue;

        UStruct* cls = it->second.ClassObj;
        if (!cls) continue;

        // Try to get CDO
        UClass* uclass = reinterpret_cast<UClass*>(cls);
        UObject* cdo = uclass->GetDefaultObject();
        cdoChecked++;
        if (cdoChecked <= 5) {
            LOG_INFO("[SDK] CDO check '{}': classPtr=0x{:08X} cdo=0x{:08X}",
                     className, (uint32_t)(uintptr_t)uclass, (uint32_t)(uintptr_t)cdo);
        }
        if (!cdo || (uintptr_t)cdo < 0x10000) continue;

        out << "// ═══════════════════════════════════════════════════════════════\n";
        out << "// CDO: " << className << " (0x" << std::hex << (uintptr_t)cdo << std::dec << ")\n";
        out << "// Size: " << it->second.PropertiesSize << " bytes\n";
        out << "// ═══════════════════════════════════════════════════════════════\n";

        // Walk ALL properties (including inherited)
        std::vector<PropertyInfo> allProps = WalkProperties(cls);
        std::sort(allProps.begin(), allProps.end(),
                  [](const PropertyInfo& a, const PropertyInfo& b) { return a.Offset < b.Offset; });

        const uint8_t* raw = reinterpret_cast<const uint8_t*>(cdo);
        for (auto& p : allProps) {
            if (p.Offset < 0 || p.Offset > 8192) continue;

            char line[512];
            if (p.TypeName == "IntProperty" || p.TypeName == "BoolProperty") {
                int32_t val = 0;
                memcpy(&val, raw + p.Offset, std::min(p.ElementSize, 4));
                std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = %d\n",
                             p.Offset, p.TypeName.c_str(), p.Name.c_str(), val);
            } else if (p.TypeName == "FloatProperty") {
                float val = 0;
                memcpy(&val, raw + p.Offset, 4);
                std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = %.6f\n",
                             p.Offset, p.TypeName.c_str(), p.Name.c_str(), val);
            } else if (p.TypeName == "ByteProperty") {
                uint8_t val = *(raw + p.Offset);
                std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = %u\n",
                             p.Offset, p.TypeName.c_str(), p.Name.c_str(), val);
            } else if (p.TypeName == "StructProperty" && p.ElementSize == 12) {
                float x, y, z;
                memcpy(&x, raw + p.Offset, 4);
                memcpy(&y, raw + p.Offset + 4, 4);
                memcpy(&z, raw + p.Offset + 8, 4);
                std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = (%.2f, %.2f, %.2f)\n",
                             p.Offset, p.TypeName.c_str(), p.Name.c_str(), x, y, z);
            } else if (p.TypeName == "ObjectProperty") {
                uintptr_t val = *reinterpret_cast<const uintptr_t*>(raw + p.Offset);
                if (val && val > 0x10000) {
                    UObject* ref = reinterpret_cast<UObject*>(val);
                    std::string refName = ref->GetName();
                    std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = -> %s\n",
                                 p.Offset, p.TypeName.c_str(), p.Name.c_str(), refName.c_str());
                } else {
                    std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = None\n",
                                 p.Offset, p.TypeName.c_str(), p.Name.c_str());
                }
            } else {
                uint32_t val = 0;
                memcpy(&val, raw + p.Offset, std::min(p.ElementSize, 4));
                std::snprintf(line, sizeof(line), "  [0x%04X] %-20s %-30s = 0x%08X\n",
                             p.Offset, p.TypeName.c_str(), p.Name.c_str(), val);
            }
            out << line;
        }
        out << "\n";
        dumped++;
    }

    out.close();
    LOG_INFO("SDK Generator: Dumped CDO defaults for {} classes", dumped);
}

// ─── Step 7: Dump states (AI/game states per class) ─────────────────────

void SDKGenerator::WriteStates(const std::string& outputDir)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    std::ofstream out(outputDir + "/SDK_States.txt");
    out << "// BioShock Remastered - All UState Objects\n";
    out << "// States are UnrealScript execution contexts (e.g., Idle, Attacking, Patrol)\n";
    out << "// Generated by BS1SDK\n\n";

    struct StateEntry {
        std::string ownerClass;
        std::string stateName;
        uint32_t probeMask;
        uint32_t ignoreMask;
        uint32_t stateFlags;
    };
    std::vector<StateEntry> states;

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() != "State") continue;

        UState* state = reinterpret_cast<UState*>(obj);
        std::string stateName = state->GetName();
        if (stateName.empty()) continue;

        std::string ownerName;
        UObject* outer = state->GetOuter();
        if (outer && (uintptr_t)outer > 0x10000) {
            ownerName = outer->GetName();
        }

        states.push_back({ownerName, stateName,
                         state->GetProbeMask(), state->GetIgnoreMask(),
                         state->GetStateFlags()});
    }

    // Sort by owner class
    std::sort(states.begin(), states.end(), [](const StateEntry& a, const StateEntry& b) {
        if (a.ownerClass != b.ownerClass) return a.ownerClass < b.ownerClass;
        return a.stateName < b.stateName;
    });

    std::string lastOwner;
    for (auto& s : states) {
        if (s.ownerClass != lastOwner) {
            out << "\n// ── " << s.ownerClass << " ─────────────────────────────────\n";
            lastOwner = s.ownerClass;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "  state %-30s  probe=0x%08X ignore=0x%08X flags=0x%08X\n",
                     s.stateName.c_str(), s.probeMask, s.ignoreMask, s.stateFlags);
        out << line;
    }

    out.close();
    LOG_INFO("SDK Generator: Dumped {} states", states.size());
}

// ─── Step 8: Dump native function table ─────────────────────────────────

void SDKGenerator::WriteNativeFunctions(const std::string& outputDir)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    std::ofstream out(outputDir + "/SDK_NativeFunctions.txt");
    out << "// BioShock Remastered - Native Function Table\n";
    out << "// Functions with native C++ implementations (directly callable)\n";
    out << "// Generated by BS1SDK\n\n";

    struct NativeEntry {
        uint16_t index;
        std::string ownerClass;
        std::string funcName;
        uintptr_t nativePtr;
        uint8_t numParms;
        uint16_t parmsSize;
    };
    std::vector<NativeEntry> natives;

    int funcFound = 0;
    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() != "Function") continue;

        UFunction* func = reinterpret_cast<UFunction*>(obj);
        funcFound++;
        // Diagnostic: log first few functions
        if (funcFound <= 3) {
            LOG_INFO("[SDK] Function '{}' flags=0x{:08X} iNative={} nativePtr=0x{:08X}",
                     func->GetName(), func->GetFunctionFlags(),
                     func->GetNativeIndex(), (uint32_t)func->GetNativeFunc());
        }
        // Check if native: either has FUNC_Native flag OR has a non-zero native function pointer
        bool isNative = func->IsNative() || (func->GetNativeFunc() > 0x10000);
        if (!isNative) continue;

        uint16_t nativeIdx = func->GetNativeIndex();
        uintptr_t nativeFunc = func->GetNativeFunc();
        
        std::string funcName = func->GetName();
        std::string ownerName;
        UObject* outer = func->GetOuter();
        if (outer && (uintptr_t)outer > 0x10000) {
            ownerName = outer->GetName();
        }

        natives.push_back({nativeIdx, ownerName, funcName, nativeFunc,
                          func->GetNumParms(), func->GetParmsSize()});
    }

    // Sort by native index
    std::sort(natives.begin(), natives.end(), [](const NativeEntry& a, const NativeEntry& b) {
        return a.index < b.index;
    });

    out << "// Format: [NativeIndex] Address  Owner::Function (params)\n\n";
    for (auto& n : natives) {
        char line[256];
        std::snprintf(line, sizeof(line), "  [%4u] 0x%08X  %s::%s (%u parms, %u bytes)\n",
                     n.index, (uint32_t)n.nativePtr,
                     n.ownerClass.c_str(), n.funcName.c_str(),
                     n.numParms, n.parmsSize);
        out << line;
    }

    out.close();
    LOG_INFO("SDK Generator: Dumped {} native functions", natives.size());
}

// ─── Step 9: Dump structs with resolved inner properties ────────────────

void SDKGenerator::WriteStructs(const std::string& outputDir)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    std::ofstream out(outputDir + "/SDK_Structs.h");
    out << "#pragma once\n";
    out << "// Auto-generated SDK struct definitions\n";
    out << "// These are inner structs used by game properties (FVector, FRotator, custom structs)\n";
    out << "// Generated by BS1SDK\n\n";
    out << "#include \"SDK_Types.h\"\n\n";
    out << "namespace sdk {\n\n";

    int structCount = 0;
    std::set<std::string> written;

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        // In Vengeance UE2.5, the class name is "Struct" (not "ScriptStruct")
        std::string cn = obj->GetObjClassName();
        if (cn != "ScriptStruct" && cn != "Struct") continue;

        UStruct* s = reinterpret_cast<UStruct*>(obj);
        std::string structName = s->GetName();
        if (structName.empty() || structName == "<invalid>") continue;
        if (written.count(structName)) continue;
        written.insert(structName);

        int32_t propSize = s->GetPropertiesSize();

        // Get super struct
        std::string superName;
        UField* super = s->GetSuperField();
        if (super && (uintptr_t)super > 0x10000) {
            superName = super->GetName();
        }

        // Get owner/package
        std::string ownerName;
        UObject* outer = s->GetOuter();
        if (outer && (uintptr_t)outer > 0x10000) {
            ownerName = outer->GetName();
        }

        out << "// Struct: F" << structName << " (Size: 0x"
            << std::hex << propSize << std::dec << ")\n";
        if (!superName.empty()) out << "// Super: F" << superName << "\n";
        if (!ownerName.empty()) out << "// Owner: " << ownerName << "\n";

        out << "struct F" << structName;
        if (!superName.empty()) out << " : public F" << superName;
        out << " {\n";

        // Walk properties
        UField* child = s->GetChildren();
        int limit = 500;
        std::vector<PropertyInfo> props;
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
                props.push_back(pi);
            }
            child = child->GetNext();
        }

        std::sort(props.begin(), props.end(),
                  [](const PropertyInfo& a, const PropertyInfo& b) { return a.Offset < b.Offset; });

        for (auto& p : props) {
            std::string cppType = MapPropertyType(p);
            std::string arrayStr;
            if (p.ArrayDim > 1) arrayStr = "[" + std::to_string(p.ArrayDim) + "]";

            char offsetStr[32];
            std::snprintf(offsetStr, sizeof(offsetStr), "0x%04X", p.Offset);
            out << "    " << cppType << " " << p.Name << arrayStr << ";";
            out << " // " << offsetStr << " (" << p.ElementSize << "B)\n";
        }

        if (props.empty()) {
            out << "    uint8_t UnknownData[" << propSize << "]; // no resolved properties\n";
        }

        out << "}; // 0x" << std::hex << propSize << std::dec << "\n\n";
        structCount++;
    }

    out << "} // namespace sdk\n";
    out.close();
    LOG_INFO("SDK Generator: Dumped {} structs", structCount);
}

} // namespace bs1sdk
