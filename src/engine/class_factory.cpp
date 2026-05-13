#include "class_factory.h"
#include "../core/log.h"
#include <Windows.h>
#include <cstring>

namespace bs1sdk {

// ─── Name Registration ──────────────────────────────────────────────────

int32_t FindNameIndex(const std::string& name)
{
    auto& globals = GetEngineGlobals();
    if (!globals.GNames) return -1;

    uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(globals.GNames);
    int32_t count = *reinterpret_cast<int32_t*>(globals.GNames + 4);

    for (int32_t i = 0; i < count; i++) {
        uintptr_t entryPtr = *reinterpret_cast<uintptr_t*>(dataPtr + i * 4);
        if (!entryPtr) continue;
        FNameEntry* entry = reinterpret_cast<FNameEntry*>(entryPtr);
        if (entry->GetAnsiName() == name)
            return i;
    }
    return -1;
}

int32_t RegisterName(const std::string& name)
{
    // Check if already exists
    int32_t existing = FindNameIndex(name);
    if (existing >= 0) return existing;

    auto& globals = GetEngineGlobals();
    if (!globals.GNames) return -1;

    uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(globals.GNames);
    int32_t count = *reinterpret_cast<int32_t*>(globals.GNames + 4);
    int32_t max = *reinterpret_cast<int32_t*>(globals.GNames + 8);

    // FNameEntry layout: 16 bytes header + wide string (null terminated)
    // We need to allocate a new FNameEntry
    size_t wideLen = name.size() + 1; // +1 for null
    size_t entrySize = 16 + wideLen * sizeof(wchar_t);

    // Allocate the entry (game uses its own allocator, but HeapAlloc works
    // as long as we never free it - the game won't either)
    uint8_t* newEntry = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, entrySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!newEntry) {
        LOG_ERROR("Failed to allocate FNameEntry for '{}'", name);
        return -1;
    }

    // Zero header
    memset(newEntry, 0, 16);
    // Write the wide string
    wchar_t* wideStr = reinterpret_cast<wchar_t*>(newEntry + 16);
    for (size_t i = 0; i < name.size(); i++)
        wideStr[i] = static_cast<wchar_t>(name[i]);
    wideStr[name.size()] = L'\0';

    // Find an empty slot in GNames, or use the end
    int32_t newIndex = -1;

    // First try to find a null slot
    for (int32_t i = 1; i < count; i++) {
        uintptr_t slot = *reinterpret_cast<uintptr_t*>(dataPtr + i * 4);
        if (slot == 0) {
            newIndex = i;
            break;
        }
    }

    if (newIndex < 0) {
        // Append at end if there's room
        if (count < max) {
            newIndex = count;
            // Increment count
            *reinterpret_cast<int32_t*>(globals.GNames + 4) = count + 1;
        } else {
            LOG_ERROR("GNames full ({}/{}), cannot register '{}'", count, max, name);
            VirtualFree(newEntry, 0, MEM_RELEASE);
            return -1;
        }
    }

    // Write the pointer
    *reinterpret_cast<uintptr_t*>(dataPtr + newIndex * 4) = reinterpret_cast<uintptr_t>(newEntry);

    LOG_INFO("Registered name '{}' at GNames[{}]", name, newIndex);
    return newIndex;
}

// ─── Object Registration ────────────────────────────────────────────────

int32_t RegisterObject(UObject* obj)
{
    auto& globals = GetEngineGlobals();
    if (!globals.GObjects) return -1;

    uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t count = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    int32_t max = *reinterpret_cast<int32_t*>(globals.GObjects + 8);

    // Find an empty slot
    int32_t newIndex = -1;
    for (int32_t i = 1; i < count; i++) {
        uintptr_t slot = *reinterpret_cast<uintptr_t*>(dataPtr + i * 4);
        if (slot == 0) {
            newIndex = i;
            break;
        }
    }

    if (newIndex < 0) {
        if (count < max) {
            newIndex = count;
            *reinterpret_cast<int32_t*>(globals.GObjects + 4) = count + 1;
        } else {
            LOG_ERROR("GObjects full ({}/{}), cannot register object", count, max);
            return -1;
        }
    }

    // Write the pointer
    *reinterpret_cast<uintptr_t*>(dataPtr + newIndex * 4) = reinterpret_cast<uintptr_t>(obj);

    // Update the object's InternalIndex to match its GObjects slot
    // InternalIndex is at +0x04, but it's a struct of 8 bytes where the first 4 is the index
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x04) = newIndex;

    LOG_INFO("Registered object at GObjects[{}]", newIndex);
    return newIndex;
}

// ─── UClass Size Discovery ──────────────────────────────────────────────

size_t GetClassObjectSize()
{
    // In UE2.5: Object(0x40) > Field(+8=0x48) > Struct(varies) > State(varies) > Class(varies)
    // We estimate UClass size by measuring distance between two consecutive class objects
    // in GObjects, or use a safe upper bound.
    //
    // From our confirmed offsets:
    //   UObject = 0x40
    //   UField = UObject + {SuperField(4) + Next(4)} = 0x48
    //   UStruct extends UField with more fields up to Children(0x5C) + PropertiesSize(0x60) + more
    //   UState extends UStruct
    //   UClass extends UState
    //
    // A safe upper bound for UClass is ~0x200 (512 bytes).
    // We'll allocate more than needed and memcpy the source.
    return 0x200;
}

// ─── Class Cloning ──────────────────────────────────────────────────────

UStruct* CloneClass(const std::string& sourceClassName, const std::string& newClassName)
{
    // Find the source class
    UStruct* sourceClass = FindClass(sourceClassName);
    if (!sourceClass) {
        LOG_ERROR("CloneClass: source class '{}' not found", sourceClassName);
        return nullptr;
    }

    // Register the new name
    int32_t nameIdx = RegisterName(newClassName);
    if (nameIdx < 0) {
        LOG_ERROR("CloneClass: failed to register name '{}'", newClassName);
        return nullptr;
    }

    // Allocate memory for the new class (same size as source)
    size_t classSize = GetClassObjectSize();
    uint8_t* newMem = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, classSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!newMem) {
        LOG_ERROR("CloneClass: VirtualAlloc failed");
        return nullptr;
    }

    // Copy the entire source class memory
    memcpy(newMem, reinterpret_cast<void*>(sourceClass), classSize);

    UObject* newClass = reinterpret_cast<UObject*>(newMem);

    // Patch the name to our new name
    FName* nameField = reinterpret_cast<FName*>(newMem + UObject::OFFSET_NAME);
    nameField->Index = nameIdx;
    nameField->Number = 0;

    // Clear some fields that should be unique
    *reinterpret_cast<uintptr_t*>(newMem + UObject::OFFSET_HASH_NEXT) = 0;

    // The SuperField should point to the SOURCE class (our new class extends the source)
    // Actually, for a plasmid we want the new class to have the SAME super as the source
    // So it's a sibling, not a child. Leave SuperField as-is.

    // Register in GObjects
    int32_t objIdx = RegisterObject(newClass);
    if (objIdx < 0) {
        LOG_ERROR("CloneClass: failed to register in GObjects");
        VirtualFree(newMem, 0, MEM_RELEASE);
        return nullptr;
    }

    LOG_INFO("CloneClass: Created '{}' (cloned from '{}') at GObjects[{}]",
             newClassName, sourceClassName, objIdx);

    return reinterpret_cast<UStruct*>(newClass);
}

} // namespace bs1sdk
