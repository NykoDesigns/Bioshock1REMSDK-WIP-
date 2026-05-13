#include "pattern.h"
#include "memory.h"

#include <sstream>
#include <algorithm>

namespace bs1sdk {

std::vector<Pattern::PatternByte> Pattern::ParsePattern(const char* pattern)
{
    std::vector<PatternByte> bytes;
    std::istringstream stream(pattern);
    std::string token;

    while (stream >> token) {
        if (token == "?" || token == "??") {
            bytes.push_back({ 0, true });
        } else {
            uint8_t value = static_cast<uint8_t>(std::stoul(token, nullptr, 16));
            bytes.push_back({ value, false });
        }
    }

    return bytes;
}

Pattern::Result Pattern::Scan(uintptr_t start, size_t size, const char* pattern)
{
    auto bytes = ParsePattern(pattern);
    if (bytes.empty()) return { false, 0 };

    const uint8_t* scanStart = reinterpret_cast<const uint8_t*>(start);
    const uint8_t* scanEnd = scanStart + size - bytes.size();

    for (const uint8_t* current = scanStart; current <= scanEnd; ++current) {
        bool match = true;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (!bytes[i].wildcard && current[i] != bytes[i].value) {
                match = false;
                break;
            }
        }
        if (match) {
            return { true, reinterpret_cast<uintptr_t>(current) };
        }
    }

    return { false, 0 };
}

Pattern::Result Pattern::ScanModule(const char* moduleName, const char* pattern)
{
    uintptr_t base = Memory::GetModuleBase(moduleName);
    size_t size = Memory::GetModuleSize(moduleName);
    
    if (base == 0 || size == 0) return { false, 0 };
    
    return Scan(base, size, pattern);
}

std::vector<uintptr_t> Pattern::ScanAll(uintptr_t start, size_t size, const char* pattern)
{
    std::vector<uintptr_t> results;
    auto bytes = ParsePattern(pattern);
    if (bytes.empty()) return results;

    const uint8_t* scanStart = reinterpret_cast<const uint8_t*>(start);
    const uint8_t* scanEnd = scanStart + size - bytes.size();

    for (const uint8_t* current = scanStart; current <= scanEnd; ++current) {
        bool match = true;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (!bytes[i].wildcard && current[i] != bytes[i].value) {
                match = false;
                break;
            }
        }
        if (match) {
            results.push_back(reinterpret_cast<uintptr_t>(current));
        }
    }

    return results;
}

Pattern::Result Pattern::ScanAndResolve(uintptr_t start, size_t size, const char* pattern,
                                         int offset, bool isRelative)
{
    Result scanResult = Scan(start, size, pattern);
    if (!scanResult.found) return scanResult;

    uintptr_t addrLocation = scanResult.address + offset;
    
    if (isRelative) {
        // Resolve RIP-relative (rare in x86-32 but possible with call/jmp)
        scanResult.address = Memory::ResolveRelative(addrLocation - (offset > 0 ? offset : 0));
    } else {
        // Read absolute address (common in x86-32: mov reg, [absolute_addr])
        scanResult.address = Memory::Read<uintptr_t>(addrLocation);
    }

    return scanResult;
}

} // namespace bs1sdk
