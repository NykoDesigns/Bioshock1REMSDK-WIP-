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

// ─── UFunction ──────────────────────────────────────────────────────────
// Extends UStruct with bytecode and native function data.
// Layout discovered via cross-referencing known ProcessEvent dispatch:
//   UStruct size = 0x64 (UField ends at 0x48, UStruct adds ScriptText, Children,
//                        CppText, Line, TextPos, ScriptSize, PropertiesSize)
//   UFunction fields after UStruct:
//     +0x64: uint32 FunctionFlags
//     +0x68: uint16 iNative
//     +0x6A: uint8  OperPrecedence
//     +0x6B: uint8  NumParms
//     +0x6C: uint16 ParmsSize
//     +0x6E: uint16 ReturnValueOffset
//     +0x70: void*  (unknown — NOT Func, points to UObject data)
//     +0x80: uint32 FunctionFlags2 / iNative copy
//     +0xA0: uint8[16] GUID (function signature hash)
//     +0xB0: uint8[16] metadata
//     +0xC0: void*  Func (native function pointer) [CONFIRMED on 4 UFunctions]

class UFunction : public UStruct {
public:
    static constexpr size_t OFFSET_FUNCTION_FLAGS   = 0x64;
    static constexpr size_t OFFSET_INATIVE          = 0x68;
    static constexpr size_t OFFSET_OPER_PRECEDENCE  = 0x6A;
    static constexpr size_t OFFSET_NUM_PARMS        = 0x6B;
    static constexpr size_t OFFSET_PARMS_SIZE       = 0x6C;
    static constexpr size_t OFFSET_RETURN_OFFSET    = 0x6E;
    static constexpr size_t OFFSET_NATIVE_FUNC      = 0xC0; // [CONFIRMED] was wrongly assumed 0x70

    uint32_t GetFunctionFlags() const { return GetField<uint32_t>(OFFSET_FUNCTION_FLAGS); }
    uint16_t GetNativeIndex() const { return GetField<uint16_t>(OFFSET_INATIVE); }
    uint8_t GetOperPrecedence() const { return GetField<uint8_t>(OFFSET_OPER_PRECEDENCE); }
    uint8_t GetNumParms() const { return GetField<uint8_t>(OFFSET_NUM_PARMS); }
    uint16_t GetParmsSize() const { return GetField<uint16_t>(OFFSET_PARMS_SIZE); }
    uint16_t GetReturnValueOffset() const { return GetField<uint16_t>(OFFSET_RETURN_OFFSET); }
    uintptr_t GetNativeFunc() const { return GetField<uintptr_t>(OFFSET_NATIVE_FUNC); }

    bool IsNative() const { return (GetFunctionFlags() & 0x0400) != 0; }
    bool IsEvent() const { return (GetFunctionFlags() & 0x0800) != 0; }
    bool IsStatic() const { return (GetFunctionFlags() & 0x2000) != 0; }
    bool IsExec() const { return (GetFunctionFlags() & 0x0200) != 0; }
    bool IsLatent() const { return (GetFunctionFlags() & 0x0008) != 0; }
};

// ─── UState ─────────────────────────────────────────────────────────────
// UState extends UStruct — contains state execution data and function map.
//   +0x64: uint32 ProbeMask
//   +0x68: uint32 IgnoreMask
//   +0x6C: uint16 LabelTableOffset
//   +0x6E: uint32 StateFlags
//   +0x74: TMap<FName, UFunction*> FuncMap

class UState : public UStruct {
public:
    static constexpr size_t OFFSET_PROBE_MASK       = 0x64;
    static constexpr size_t OFFSET_IGNORE_MASK      = 0x68;
    static constexpr size_t OFFSET_STATE_FLAGS      = 0x6E;

    uint32_t GetProbeMask() const { return GetField<uint32_t>(OFFSET_PROBE_MASK); }
    uint32_t GetIgnoreMask() const { return GetField<uint32_t>(OFFSET_IGNORE_MASK); }
    uint32_t GetStateFlags() const { return GetField<uint32_t>(OFFSET_STATE_FLAGS); }
};

// ─── UClass ─────────────────────────────────────────────────────────────
// UClass extends UState with full class metadata.
// Key offsets (UE2.5 Vengeance, x86):
//   After UState base (~0x90+):
//     +0x90: uint32 ClassFlags
//     +0x94: int32  ClassWithin (index or pointer to enclosing class)
//     +0x98: FName  ClassConfigName
//     +0xA0: TArray<FRepRecord> ClassReps
//     +0xAC: TArray<UField*> NetFields
//     +0xC4: UObject* ClassDefaultObject (CDO)
//
// NOTE: Exact offsets need runtime confirmation. The CDO offset is the most
// critical — we discover it dynamically via heuristic.

class UClass : public UState {
public:
    // These offsets may vary ±4 bytes between builds; use discovery functions
    static constexpr size_t OFFSET_CLASS_FLAGS      = 0x90;
    static constexpr size_t OFFSET_CLASS_WITHIN     = 0x94;
    static constexpr size_t OFFSET_CLASS_CONFIG_NAME = 0x98;
    static constexpr size_t OFFSET_CDO              = 0xC4; // ClassDefaultObject

    uint32_t GetClassFlags() const { return GetField<uint32_t>(OFFSET_CLASS_FLAGS); }
    UObject* GetClassWithin() const { return GetField<UObject*>(OFFSET_CLASS_WITHIN); }
    FName GetClassConfigName() const { return GetField<FName>(OFFSET_CLASS_CONFIG_NAME); }
    UObject* GetDefaultObject() const { return GetField<UObject*>(OFFSET_CDO); }

    void SetDefaultObject(UObject* obj) { SetField<UObject*>(OFFSET_CDO, obj); }
};

// ─── UEnum ──────────────────────────────────────────────────────────────
// UEnum extends UField with a TArray<FName> of enum literals.
//   +0x48: TArray<FName> Names

class UEnum : public UField {
public:
    // TArray<FName> Names at +0x48: { FName* Data; int32 Count; int32 Max; }
    static constexpr size_t OFFSET_ENUM_NAMES_DATA  = 0x48;
    static constexpr size_t OFFSET_ENUM_NAMES_COUNT = 0x4C;
    static constexpr size_t OFFSET_ENUM_NAMES_MAX   = 0x50;

    FName* GetNamesData() const { return GetField<FName*>(OFFSET_ENUM_NAMES_DATA); }
    int32_t GetNamesCount() const { return GetField<int32_t>(OFFSET_ENUM_NAMES_COUNT); }

    int32_t GetNumEnums() const { return GetNamesCount(); }

    std::string GetEnumName(int32_t index) const {
        FName* data = GetNamesData();
        int32_t count = GetNamesCount();
        if (!data || index < 0 || index >= count) return "<invalid>";
        return data[index].ToString();
    }

    std::vector<std::string> GetAllEnumNames() const {
        std::vector<std::string> result;
        FName* data = GetNamesData();
        int32_t count = GetNamesCount();
        if (!data || count <= 0) return result;
        for (int32_t i = 0; i < count; i++) {
            result.push_back(data[i].ToString());
        }
        return result;
    }
};

// ─── Specialized Property Types ─────────────────────────────────────────
// These extend UProperty with inner type references.

// UByteProperty: +0x78 = UEnum* Enum (inner enum, or nullptr for raw byte)
class UByteProperty : public UProperty {
public:
    static constexpr size_t OFFSET_ENUM = 0x78;
    UEnum* GetEnum() const { return GetField<UEnum*>(OFFSET_ENUM); }
};

// UObjectProperty: +0x78 = UClass* PropertyClass
class UObjectProperty : public UProperty {
public:
    static constexpr size_t OFFSET_PROPERTY_CLASS = 0x78;
    UObject* GetPropertyClass() const { return GetField<UObject*>(OFFSET_PROPERTY_CLASS); }
};

// UClassProperty: +0x78 = UClass* PropertyClass, +0x7C = UClass* MetaClass
class UClassProperty : public UProperty {
public:
    static constexpr size_t OFFSET_PROPERTY_CLASS = 0x78;
    static constexpr size_t OFFSET_META_CLASS = 0x7C;
    UObject* GetPropertyClass() const { return GetField<UObject*>(OFFSET_PROPERTY_CLASS); }
    UObject* GetMetaClass() const { return GetField<UObject*>(OFFSET_META_CLASS); }
};

// UStructProperty: +0x78 = UStruct* Struct
class UStructProperty : public UProperty {
public:
    static constexpr size_t OFFSET_STRUCT = 0x78;
    UStruct* GetStruct() const { return GetField<UStruct*>(OFFSET_STRUCT); }
};

// UArrayProperty: +0x78 = UProperty* Inner
class UArrayProperty : public UProperty {
public:
    static constexpr size_t OFFSET_INNER = 0x78;
    UProperty* GetInner() const { return GetField<UProperty*>(OFFSET_INNER); }
};

// UMapProperty: +0x78 = UProperty* Key, +0x7C = UProperty* Value
class UMapProperty : public UProperty {
public:
    static constexpr size_t OFFSET_KEY = 0x78;
    static constexpr size_t OFFSET_VALUE = 0x7C;
    UProperty* GetKey() const { return GetField<UProperty*>(OFFSET_KEY); }
    UProperty* GetValue() const { return GetField<UProperty*>(OFFSET_VALUE); }
};

// UDelegateProperty: +0x78 = UFunction* SignatureFunction
class UDelegateProperty : public UProperty {
public:
    static constexpr size_t OFFSET_SIGNATURE_FUNC = 0x78;
    UFunction* GetSignatureFunction() const { return GetField<UFunction*>(OFFSET_SIGNATURE_FUNC); }
};

// UInterfaceProperty: +0x78 = UClass* InterfaceClass
class UInterfaceProperty : public UProperty {
public:
    static constexpr size_t OFFSET_INTERFACE_CLASS = 0x78;
    UObject* GetInterfaceClass() const { return GetField<UObject*>(OFFSET_INTERFACE_CLASS); }
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

/// Find a UEnum by name (e.g., "EPhysics")
UEnum* FindEnum(const std::string& enumName);

/// Find a UFunction on a specific class (walks inheritance)
UFunction* FindFunctionOnClass(const std::string& className, const std::string& funcName);

/// Get the ClassDefaultObject (CDO) for a named class
UObject* GetDefaultObject(const std::string& className);

/// Set a property on a CDO (affects all future instances)
bool SetDefaultProperty(const std::string& className, const std::string& propName, float value);
bool SetDefaultPropertyInt(const std::string& className, const std::string& propName, int32_t value);

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
