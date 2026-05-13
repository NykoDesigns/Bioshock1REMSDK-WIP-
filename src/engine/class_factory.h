#pragma once

#include "uobject.h"
#include <string>

namespace bs1sdk {

/// Runtime class injection for UE2.5
/// Allows creating new classes at runtime by cloning existing ones.

/// Register a new FName in GNames. Returns the name index.
/// If the name already exists, returns its existing index.
int32_t RegisterName(const std::string& name);

/// Find a name index in GNames (-1 if not found)
int32_t FindNameIndex(const std::string& name);

/// Clone an existing UClass to create a new one with a different name.
/// The new class inherits all properties and functions from the source.
/// Returns the new UClass, or nullptr on failure.
/// NOTE: The new class shares the source's vtable and native code.
UStruct* CloneClass(const std::string& sourceClassName, const std::string& newClassName);

/// Register a UObject in GObjects (finds an empty slot or expands).
/// Returns the assigned index, or -1 on failure.
int32_t RegisterObject(UObject* obj);

/// Get size of a UClass object in memory (needed for cloning)
size_t GetClassObjectSize();

} // namespace bs1sdk
