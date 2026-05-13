#include "memory.h"
#include <Psapi.h>

namespace bs1sdk {

uintptr_t Memory::GetModuleBase(const char* moduleName)
{
    HMODULE hModule = GetModuleHandleA(moduleName);
    return reinterpret_cast<uintptr_t>(hModule);
}

size_t Memory::GetModuleSize(const char* moduleName)
{
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (!hModule) return 0;

    MODULEINFO modInfo{};
    if (GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo))) {
        return modInfo.SizeOfImage;
    }
    return 0;
}

std::vector<uint8_t> Memory::ReadBytes(uintptr_t address, size_t size)
{
    std::vector<uint8_t> buffer(size);
    memcpy(buffer.data(), reinterpret_cast<void*>(address), size);
    return buffer;
}

bool Memory::WriteBytes(uintptr_t address, const std::vector<uint8_t>& bytes)
{
    return Patch(address, bytes.data(), bytes.size());
}

bool Memory::Patch(uintptr_t address, const uint8_t* data, size_t size)
{
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, 
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(reinterpret_cast<void*>(address), data, size);
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
    return true;
}

bool Memory::Nop(uintptr_t address, size_t size)
{
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, 
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memset(reinterpret_cast<void*>(address), 0x90, size);
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
    return true;
}

uintptr_t Memory::ResolveRelative(uintptr_t instructionAddress, size_t instructionSize)
{
    // For a relative call/jmp: target = instruction_end + offset
    int32_t offset = Read<int32_t>(instructionAddress + (instructionSize - 4));
    return instructionAddress + instructionSize + offset;
}

bool Memory::IsValidPtr(uintptr_t address)
{
    if (address == 0) return false;
    
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    
    return true;
}

std::string Memory::ReadString(uintptr_t address, size_t maxLen)
{
    if (!IsValidPtr(address)) return "";
    
    std::string result;
    result.reserve(64);
    
    for (size_t i = 0; i < maxLen; ++i) {
        char c = Read<char>(address + i);
        if (c == '\0') break;
        result += c;
    }
    return result;
}

std::wstring Memory::ReadWString(uintptr_t address, size_t maxLen)
{
    if (!IsValidPtr(address)) return L"";
    
    std::wstring result;
    result.reserve(64);
    
    for (size_t i = 0; i < maxLen; ++i) {
        wchar_t c = Read<wchar_t>(address + i * 2);
        if (c == L'\0') break;
        result += c;
    }
    return result;
}

uintptr_t Memory::GetVFunc(uintptr_t object, int index)
{
    uintptr_t vtable = Read<uintptr_t>(object);
    return Read<uintptr_t>(vtable + index * sizeof(uintptr_t));
}

uintptr_t Memory::HookVFunc(uintptr_t object, int index, uintptr_t hookFunc)
{
    uintptr_t vtable = Read<uintptr_t>(object);
    uintptr_t targetAddr = vtable + index * sizeof(uintptr_t);
    uintptr_t original = Read<uintptr_t>(targetAddr);
    
    Write<uintptr_t>(targetAddr, hookFunc);
    
    return original;
}

} // namespace bs1sdk
