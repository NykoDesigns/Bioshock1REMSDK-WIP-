#pragma once

#include "uobject.h"
#include <string>
#include <vector>
#include <functional>

namespace bs1sdk {

/// Interface to the game's global object array (GObjects).
/// This is the primary discovery mechanism for all engine objects.
///
/// In UE2.5, GObjects is typically a TArray<UObject*> at a fixed address.
/// Objects are indexed by InternalIndex, and nullptr entries are possible
/// (deleted objects leave holes).
class GObjects {
public:
    /// Initialize with the discovered GObjects address
    static bool Initialize(uintptr_t address);

    /// Get the number of objects in the array
    static int32_t Count();

    /// Get object by index (may return nullptr for deleted objects)
    static UObject* GetByIndex(int32_t index);

    /// Find first object by name (slow linear search)
    static UObject* FindByName(const std::string& name);

    /// Find first object by full path (e.g., "Package.Class.Object")
    static UObject* FindByFullPath(const std::string& path);

    /// Find all objects of a given class name
    static std::vector<UObject*> FindAllOfClass(const std::string& className);

    /// Iterate all valid objects
    static void ForEach(std::function<void(UObject*, int32_t index)> callback);

    /// Dump all object names to a file (for RE purposes)
    static void DumpToFile(const std::string& filepath);

    /// Check if initialized
    static bool IsInitialized();

private:
    static uintptr_t s_Address;
    static bool s_Initialized;
};

/// Interface to the global name table (GNames).
/// Maps FName indices to string representations.
class GNames {
public:
    /// Initialize with the discovered GNames address
    static bool Initialize(uintptr_t address);

    /// Get name string by index
    static std::string GetName(int32_t index);

    /// Get the total number of names
    static int32_t Count();

    /// Dump all names to a file
    static void DumpToFile(const std::string& filepath);

    /// Check if initialized
    static bool IsInitialized();

private:
    static uintptr_t s_Address;
    static bool s_Initialized;
};

} // namespace bs1sdk
