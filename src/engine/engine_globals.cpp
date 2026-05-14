#include "uobject.h"
#include <cstring>

namespace bs1sdk {

static EngineGlobals s_EngineGlobals;

EngineGlobals& GetEngineGlobals()
{
    return s_EngineGlobals;
}

// ─── FNameEntry ──────────────────────────────────────────────────────────

std::string FNameEntry::GetAnsiName() const
{
    const wchar_t* wide = GetWideName();
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 256 && wide[i]; i++) {
        result += static_cast<char>(wide[i] & 0x7F);
    }
    return result;
}

// ─── FName ───────────────────────────────────────────────────────────────

std::string FName::ToString() const
{
    auto& globals = GetEngineGlobals();
    if (!globals.GNames || Index < 0) return "<invalid>";
    
    int32_t count = *reinterpret_cast<int32_t*>(globals.GNames + 4);
    if (Index >= count) return "<out_of_range>";
    
    uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(globals.GNames);
    uintptr_t entryPtr = *reinterpret_cast<uintptr_t*>(dataPtr + Index * 4);
    if (!entryPtr) return "<null>";
    
    FNameEntry* entry = reinterpret_cast<FNameEntry*>(entryPtr);
    std::string name = entry->GetAnsiName();
    
    if (Number > 0) {
        name += "_";
        name += std::to_string(Number - 1);
    }
    
    return name;
}

// ─── UObject ─────────────────────────────────────────────────────────────

std::string UObject::GetName() const
{
    return GetFName().ToString();
}

std::string UObject::GetObjClassName() const
{
    UObject* cls = GetClass();
    if (!cls) return "<no_class>";
    return cls->GetName();
}

std::string UObject::GetFullPath() const
{
    std::string path = GetName();
    UObject* outer = GetOuter();
    
    // Walk up the Outer chain (limit depth to prevent infinite loops)
    int depth = 0;
    while (outer && depth < 16) {
        path = outer->GetName() + "." + path;
        outer = outer->GetOuter();
        depth++;
    }
    
    return path;
}

// ─── Property Walker ─────────────────────────────────────────────────────

std::vector<PropertyInfo> WalkProperties(UStruct* cls)
{
    std::vector<PropertyInfo> props;
    if (!cls) return props;
    
    // Walk the class hierarchy (current class -> super -> super -> ...)
    UStruct* current = cls;
    int safetyLimit = 64;
    
    while (current && safetyLimit-- > 0) {
        // Walk the Children chain for this class level
        UField* child = current->GetChildren();
        int childLimit = 2000;
        
        while (child && childLimit-- > 0) {
            // Only collect UProperty-derived objects (class name contains "Property")
            std::string childClass = child->GetObjClassName();
            if (childClass.find("Property") != std::string::npos) {
                UProperty* prop = reinterpret_cast<UProperty*>(child);
                PropertyInfo info;
                info.Name = prop->GetName();
                info.TypeName = childClass;
                info.Offset = prop->GetPropertyOffset();
                info.ElementSize = prop->GetElementSize();
                info.ArrayDim = prop->GetArrayDim();
                info.PropertyObj = prop;
                props.push_back(info);
            }
            child = child->GetNext();
        }
        
        // Move to parent class
        UField* super = current->GetSuperField();
        current = super ? reinterpret_cast<UStruct*>(super) : nullptr;
    }
    
    return props;
}

PropertyInfo* FindProperty(UStruct* cls, const std::string& name,
                           std::vector<PropertyInfo>& outProps)
{
    if (outProps.empty())
        outProps = WalkProperties(cls);
    
    for (auto& p : outProps) {
        if (p.Name == name)
            return &p;
    }
    return nullptr;
}

// ─── Object Finder Helpers ───────────────────────────────────────────────

UObject* FindObjectByClassName(const std::string& className)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return nullptr;
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == className)
            return obj;
    }
    return nullptr;
}

std::vector<UObject*> FindAllObjectsByClassName(const std::string& className)
{
    std::vector<UObject*> results;
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return results;
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == className)
            results.push_back(obj);
    }
    return results;
}

UStruct* FindClass(const std::string& className)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return nullptr;
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == "Class" && obj->GetName() == className)
            return reinterpret_cast<UStruct*>(obj);
    }
    return nullptr;
}

UEnum* FindEnum(const std::string& enumName)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return nullptr;
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == "Enum" && obj->GetName() == enumName)
            return reinterpret_cast<UEnum*>(obj);
    }
    return nullptr;
}

UFunction* FindFunctionOnClass(const std::string& className, const std::string& funcName)
{
    UStruct* cls = FindClass(className);
    if (!cls) return nullptr;

    UStruct* current = cls;
    int safetyLimit = 64;

    while (current && safetyLimit-- > 0) {
        UField* child = current->GetChildren();
        int childLimit = 2000;

        while (child && childLimit-- > 0) {
            std::string childClass = child->GetObjClassName();
            if (childClass == "Function") {
                if (child->GetName() == funcName)
                    return reinterpret_cast<UFunction*>(child);
            }
            child = child->GetNext();
        }

        UField* super = current->GetSuperField();
        current = super ? reinterpret_cast<UStruct*>(super) : nullptr;
    }
    return nullptr;
}

UObject* GetDefaultObject(const std::string& className)
{
    UStruct* cls = FindClass(className);
    if (!cls) return nullptr;
    
    // Cast to UClass to access CDO
    UClass* uclass = reinterpret_cast<UClass*>(cls);
    return uclass->GetDefaultObject();
}

bool SetDefaultProperty(const std::string& className, const std::string& propName, float value)
{
    UObject* cdo = GetDefaultObject(className);
    if (!cdo) return false;
    
    UStruct* cls = FindClass(className);
    if (!cls) return false;
    
    auto props = WalkProperties(cls);
    for (auto& p : props) {
        if (p.Name == propName && p.TypeName == "FloatProperty") {
            cdo->SetField<float>(p.Offset, value);
            return true;
        }
    }
    return false;
}

bool SetDefaultPropertyInt(const std::string& className, const std::string& propName, int32_t value)
{
    UObject* cdo = GetDefaultObject(className);
    if (!cdo) return false;
    
    UStruct* cls = FindClass(className);
    if (!cls) return false;
    
    auto props = WalkProperties(cls);
    for (auto& p : props) {
        if (p.Name == propName && p.TypeName == "IntProperty") {
            cdo->SetField<int32_t>(p.Offset, value);
            return true;
        }
    }
    return false;
}

} // namespace bs1sdk
