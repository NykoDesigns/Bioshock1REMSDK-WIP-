#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace bs1sdk {

/// All offsets CONFIRMED via runtime property_layout.txt analysis.

// ─── FName ────────────────────────────────────────────────────────────────
// UE2.5 Vengeance: FName is { int32 Index, int32 Number }
// FNameEntry: 16 bytes header + wide (UTF-16LE) null-terminated string

struct FName {
    int32_t Index;     // [CONFIRMED] Index into GNames array
    int32_t Number;    // [CONFIRMED] Instance number (0 = no suffix)

    std::string ToString() const;
    bool operator==(const FName& other) const { return Index == other.Index && Number == other.Number; }
    bool operator!=(const FName& other) const { return !(*this == other); }
};

// ─── FNameEntry ──────────────────────────────────────────────────────────
struct FNameEntry {
    static constexpr size_t NAME_OFFSET = 16; // [CONFIRMED] Wide string starts at +16
    
    uint8_t Header[16];
    
    const wchar_t* GetWideName() const {
        return reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(this) + NAME_OFFSET);
    }
    
    std::string GetAnsiName() const;
};

// ─── UObject ──────────────────────────────────────────────────────────────
// BioShock Remastered (UE2.5 Vengeance) UObject layout, 32-bit:
// Total size: 0x40 (64 bytes)
//
//   +0x00: VTable pointer                    [CONFIRMED]
//   +0x04: struct ObjectInternalIndex (8B)   [CONFIRMED] contains GObjects index
//   +0x0C: pointer HashNext                  [CONFIRMED]
//   +0x10: pointer StateFrame                [CONFIRMED]
//   +0x14: pointer Linker                    [CONFIRMED]
//   +0x18: pointer LinkerIndex               [CONFIRMED]
//   +0x1C: UObject* Outer                    [CONFIRMED]
//   +0x20: QWord ObjectFlags (8 bytes)       [CONFIRMED]
//   +0x28: FName Name (8 bytes)              [CONFIRMED]
//   +0x30: UObject* Class                    [CONFIRMED]
//   +0x34: UObject* ObjectArchetype          [CONFIRMED]
//   +0x38: int32 CheckpointType              [CONFIRMED]
//   +0x3C: int32 CheckpointTypePadding       [CONFIRMED]

class UObject {
public:
    static constexpr size_t UOBJECT_SIZE         = 0x40; // [CONFIRMED]
    
    static constexpr size_t OFFSET_VTABLE        = 0x00; // [CONFIRMED]
    static constexpr size_t OFFSET_INTERNAL_INDEX = 0x04; // [CONFIRMED]
    static constexpr size_t OFFSET_HASH_NEXT     = 0x0C; // [CONFIRMED]
    static constexpr size_t OFFSET_STATE_FRAME   = 0x10; // [CONFIRMED]
    static constexpr size_t OFFSET_LINKER        = 0x14; // [CONFIRMED]
    static constexpr size_t OFFSET_LINKER_INDEX  = 0x18; // [CONFIRMED]
    static constexpr size_t OFFSET_OUTER         = 0x1C; // [CONFIRMED]
    static constexpr size_t OFFSET_FLAGS         = 0x20; // [CONFIRMED] QWord (8 bytes)
    static constexpr size_t OFFSET_NAME          = 0x28; // [CONFIRMED]
    static constexpr size_t OFFSET_CLASS         = 0x30; // [CONFIRMED]
    static constexpr size_t OFFSET_ARCHETYPE     = 0x34; // [CONFIRMED]

    uintptr_t GetVTablePtr() const { return GetField<uintptr_t>(OFFSET_VTABLE); }
    int32_t GetInternalIndex() const { return GetField<int32_t>(OFFSET_INTERNAL_INDEX); }
    int32_t GetLinkerIndex() const { return GetField<int32_t>(OFFSET_LINKER_INDEX); }
    UObject* GetOuter() const { return GetField<UObject*>(OFFSET_OUTER); }
    uint64_t GetObjectFlags() const { return GetField<uint64_t>(OFFSET_FLAGS); }
    FName GetFName() const { return GetField<FName>(OFFSET_NAME); }
    UObject* GetClass() const { return GetField<UObject*>(OFFSET_CLASS); }

    std::string GetName() const;
    std::string GetObjClassName() const;
    std::string GetFullPath() const;

    template<typename T>
    T GetField(size_t offset) const {
        return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset);
    }

    template<typename T>
    void SetField(size_t offset, T value) {
        *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset) = value;
    }
};

// ─── UField ──────────────────────────────────────────────────────────────
// Extends UObject at +0x40
//   +0x40: UField* SuperField    [CONFIRMED]
//   +0x44: UField* Next          [CONFIRMED]

class UField : public UObject {
public:
    static constexpr size_t OFFSET_SUPER_FIELD = 0x40; // [CONFIRMED]
    static constexpr size_t OFFSET_NEXT        = 0x44; // [CONFIRMED]

    UField* GetSuperField() const { return GetField<UField*>(OFFSET_SUPER_FIELD); }
    UField* GetNext() const { return GetField<UField*>(OFFSET_NEXT); }
};

// ─── UStruct ─────────────────────────────────────────────────────────────
// Extends UField
//   +0x5C: UField* Children      [CONFIRMED]
//   +0x60: int32 PropertiesSize  [CONFIRMED]

class UStruct : public UField {
public:
    static constexpr size_t OFFSET_CHILDREN        = 0x5C; // [CONFIRMED]
    static constexpr size_t OFFSET_PROPERTIES_SIZE = 0x60; // [CONFIRMED]

    UField* GetChildren() const { return GetField<UField*>(OFFSET_CHILDREN); }
    int32_t GetPropertiesSize() const { return GetField<int32_t>(OFFSET_PROPERTIES_SIZE); }
};

// ─── UProperty ───────────────────────────────────────────────────────────
// Extends UField
//   +0x4C: int32 ArrayDim        [CONFIRMED]
//   +0x50: int32 ElementSize     [CONFIRMED]
//   +0x54: uint32 PropertyFlags  [CONFIRMED]
//   +0x74: int32 Offset          [CONFIRMED] - byte offset in owning object

class UProperty : public UField {
public:
    static constexpr size_t OFFSET_ARRAY_DIM      = 0x4C; // [CONFIRMED]
    static constexpr size_t OFFSET_ELEMENT_SIZE   = 0x50; // [CONFIRMED]
    static constexpr size_t OFFSET_PROPERTY_FLAGS = 0x54; // [CONFIRMED]
    static constexpr size_t OFFSET_PROP_OFFSET    = 0x74; // [CONFIRMED] offset in instance

    int32_t GetArrayDim() const { return GetField<int32_t>(OFFSET_ARRAY_DIM); }
    int32_t GetElementSize() const { return GetField<int32_t>(OFFSET_ELEMENT_SIZE); }
    uint32_t GetPropertyFlags() const { return GetField<uint32_t>(OFFSET_PROPERTY_FLAGS); }
    int32_t GetPropertyOffset() const { return GetField<int32_t>(OFFSET_PROP_OFFSET); }
};

// ─── Property Walker ─────────────────────────────────────────────────────

struct PropertyInfo {
    std::string Name;
    std::string TypeName;   // "IntProperty", "FloatProperty", etc.
    int32_t Offset;         // byte offset in owning object instance
    int32_t ElementSize;
    int32_t ArrayDim;
    UProperty* PropertyObj; // raw pointer to the UProperty
};

/// Walk all properties of a UStruct (class), including inherited.
/// Returns properties from most-derived to base class order.
std::vector<PropertyInfo> WalkProperties(UStruct* cls);

/// Find a specific property by name on a class (including inherited).
PropertyInfo* FindProperty(UStruct* cls, const std::string& name,
                           std::vector<PropertyInfo>& outProps);

// ─── Object Finder Helpers ───────────────────────────────────────────────

/// Find first UObject whose class name matches (e.g., "ShockPlayer")
UObject* FindObjectByClassName(const std::string& className);

/// Find all UObjects whose class name matches
std::vector<UObject*> FindAllObjectsByClassName(const std::string& className);

/// Find a UClass/UStruct by name (e.g., "ShockPlayer" returns the UClass)
UStruct* FindClass(const std::string& className);

// ─── Global Engine Pointers ───────────────────────────────────────────────

struct EngineGlobals {
    uintptr_t GObjects = 0;
    uintptr_t GNames = 0;
    uintptr_t GEngine = 0;
    uintptr_t GWorld = 0;
    
    bool IsValid() const { return GObjects != 0 && GNames != 0; }
};

EngineGlobals& GetEngineGlobals();

// ─── TArray<T> ────────────────────────────────────────────────────────────

template<typename T>
struct TArray {
    T* Data;
    int32_t Count;
    int32_t Max;

    T& operator[](int32_t index) { return Data[index]; }
    const T& operator[](int32_t index) const { return Data[index]; }
    bool IsValidIndex(int32_t index) const { return index >= 0 && index < Count; }
};

} // namespace bs1sdk
