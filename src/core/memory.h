#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>
#include <string>

namespace bs1sdk {

/// Low-level memory utilities for reading/writing game memory
/// and resolving module information.
class Memory {
public:
    /// Get base address of a module (nullptr = main executable)
    static uintptr_t GetModuleBase(const char* moduleName = nullptr);
    
    /// Get size of a module's image in memory
    static size_t GetModuleSize(const char* moduleName = nullptr);

    /// Read a value from memory (no protection check - use for mapped memory)
    template<typename T>
    static T Read(uintptr_t address) {
        return *reinterpret_cast<T*>(address);
    }

    /// Write a value to memory with VirtualProtect
    template<typename T>
    static bool Write(uintptr_t address, T value) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), 
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        *reinterpret_cast<T*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), 
                      oldProtect, &oldProtect);
        return true;
    }

    /// Read a block of bytes
    static std::vector<uint8_t> ReadBytes(uintptr_t address, size_t size);

    /// Write a block of bytes with protection change
    static bool WriteBytes(uintptr_t address, const std::vector<uint8_t>& bytes);

    /// Patch bytes at address (with protection change)
    static bool Patch(uintptr_t address, const uint8_t* data, size_t size);

    /// NOP a region of code
    static bool Nop(uintptr_t address, size_t size);

    /// Resolve a relative call/jump target
    static uintptr_t ResolveRelative(uintptr_t instructionAddress, size_t instructionSize = 5);

    /// Check if an address is valid (readable)
    static bool IsValidPtr(uintptr_t address);

    /// Read a null-terminated string from memory
    static std::string ReadString(uintptr_t address, size_t maxLen = 256);

    /// Read a wide string from memory
    static std::wstring ReadWString(uintptr_t address, size_t maxLen = 256);

    /// Get the address of a virtual function from an object's vtable
    static uintptr_t GetVFunc(uintptr_t object, int index);

    /// Hook a virtual function by patching the vtable
    static uintptr_t HookVFunc(uintptr_t object, int index, uintptr_t hookFunc);
};

} // namespace bs1sdk
