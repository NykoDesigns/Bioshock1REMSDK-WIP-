#pragma once

#include "../engine/uobject.h"
#include <string>
#include <vector>
#include <map>

namespace bs1sdk {

/// Generates C++ SDK headers from live runtime UClass/UProperty data.
/// Walks all classes in GObjects, enumerates their properties,
/// and outputs typed struct definitions organized by package.
class SDKGenerator {
public:
    /// Run the full SDK generation. Outputs to the given directory.
    /// Returns number of classes processed.
    int Generate(const std::string& outputDir);

private:
    // ─── Internal types ────────────────────────────────────────────

    struct FunctionSDKInfo {
        std::string Name;
        std::string Signature;    // e.g., "void TakeDamage(int32_t Damage, APawn* InstigatedBy)"
        uint32_t Flags = 0;
        uint16_t NativeIndex = 0;
        bool IsNative = false;
        bool IsEvent = false;
        bool IsExec = false;
        bool IsLatent = false;
    };

    struct ClassInfo {
        UStruct* ClassObj = nullptr;
        std::string Name;
        std::string PackageName;
        std::string SuperClassName;
        int32_t PropertiesSize = 0;
        std::vector<PropertyInfo> OwnProperties; // only this class level
        std::vector<FunctionSDKInfo> OwnFunctions; // only this class level
    };

    // ─── Steps ─────────────────────────────────────────────────────

    /// Step 1: Find all UClass objects and build hierarchy
    void DiscoverClasses();

    /// Step 2: For each class, collect direct properties (not inherited)
    void CollectProperties();

    /// Step 2b: For each class, collect direct functions (not inherited)
    void CollectFunctions();

    /// Step 3: Write header files per package
    void WriteHeaders(const std::string& outputDir);

    /// Step 4: Write master SDK.h
    void WriteMasterHeader(const std::string& outputDir);

    // ─── Helpers ───────────────────────────────────────────────────

    /// Map a UProperty type name to a C++ type string
    std::string MapPropertyType(const PropertyInfo& prop);

    /// Get the package name for an object (top-level Outer)
    std::string GetPackageName(UObject* obj);

    /// Determine UE-style prefix (A for Actor derivatives, U otherwise)
    std::string GetClassPrefix(const std::string& className);

    /// Check if a class inherits from Actor
    bool InheritsFromActor(const std::string& className);

    // ─── Data ──────────────────────────────────────────────────────

    std::map<std::string, ClassInfo> m_Classes;          // className -> info
    std::map<std::string, std::vector<std::string>> m_Packages; // pkg -> class names
    std::map<std::string, std::string> m_Inheritance;    // className -> superClassName
    int m_TotalProps = 0;
    int m_TotalFuncs = 0;
};

} // namespace bs1sdk
