#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bs1sdk {

/// Pattern scanning utilities for finding byte sequences in memory.
/// Patterns use IDA-style format: "8B 0D ?? ?? ?? ?? 8B 04 81"
/// where ?? represents a wildcard byte.
class Pattern {
public:
    struct Result {
        bool found = false;
        uintptr_t address = 0;
    };

    /// Scan for a pattern in a memory region
    /// @param start Start address of the search region
    /// @param size Size of the search region in bytes
    /// @param pattern IDA-style pattern string (e.g., "8B 0D ?? ?? ?? ??")
    /// @return Result containing whether pattern was found and its address
    static Result Scan(uintptr_t start, size_t size, const char* pattern);

    /// Scan for a pattern in a specific module
    /// @param moduleName Module name (nullptr = main executable)
    /// @param pattern IDA-style pattern string
    static Result ScanModule(const char* moduleName, const char* pattern);

    /// Scan for all occurrences of a pattern
    static std::vector<uintptr_t> ScanAll(uintptr_t start, size_t size, const char* pattern);

    /// Scan for a pattern and resolve a relative offset at the found address
    /// Useful for patterns like: mov ecx, [addr] where you want 'addr'
    /// @param offset Byte offset from pattern start to the relative/absolute address
    /// @param isRelative If true, resolves as RIP-relative (not common in x86-32)
    static Result ScanAndResolve(uintptr_t start, size_t size, const char* pattern, 
                                  int offset, bool isRelative = false);

private:
    struct PatternByte {
        uint8_t value;
        bool wildcard;
    };

    static std::vector<PatternByte> ParsePattern(const char* pattern);
};

} // namespace bs1sdk
