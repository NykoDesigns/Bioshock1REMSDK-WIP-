#include "core/pattern.h"
#include <cassert>
#include <iostream>
#include <cstring>

using namespace bs1sdk;

// Minimal test framework
#define TEST(name) void test_##name(); \
    struct Register_##name { Register_##name() { tests.push_back({#name, test_##name}); } } reg_##name; \
    void test_##name()

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "FAIL: " #x " at line " << __LINE__ << "\n"; failures++; return; } } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAIL: " #a " != " #b " at line " << __LINE__ << "\n"; failures++; return; } } while(0)

struct TestEntry { const char* name; void(*func)(); };
static std::vector<TestEntry> tests;
static int failures = 0;

// ─── Tests ────────────────────────────────────────────────────────────────

TEST(BasicPatternFound)
{
    uint8_t data[] = { 0x00, 0x8B, 0x0D, 0xAA, 0xBB, 0xCC, 0xDD, 0x8B, 0x04, 0x81, 0x00 };
    
    auto result = Pattern::Scan(
        reinterpret_cast<uintptr_t>(data), sizeof(data),
        "8B 0D ?? ?? ?? ?? 8B 04 81"
    );
    
    ASSERT_TRUE(result.found);
    ASSERT_EQ(result.address, reinterpret_cast<uintptr_t>(data + 1));
}

TEST(PatternNotFound)
{
    uint8_t data[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    
    auto result = Pattern::Scan(
        reinterpret_cast<uintptr_t>(data), sizeof(data),
        "FF FF FF FF"
    );
    
    ASSERT_TRUE(!result.found);
}

TEST(WildcardPattern)
{
    uint8_t data[] = { 0xAA, 0xBB, 0x00, 0x00, 0xCC, 0xDD };
    
    auto result = Pattern::Scan(
        reinterpret_cast<uintptr_t>(data), sizeof(data),
        "AA BB ?? ?? CC DD"
    );
    
    ASSERT_TRUE(result.found);
    ASSERT_EQ(result.address, reinterpret_cast<uintptr_t>(data));
}

TEST(ScanAllFindsMultiple)
{
    uint8_t data[] = { 0xAA, 0xBB, 0x00, 0xAA, 0xBB, 0x00, 0xAA, 0xBB };
    
    auto results = Pattern::ScanAll(
        reinterpret_cast<uintptr_t>(data), sizeof(data),
        "AA BB"
    );
    
    ASSERT_EQ(results.size(), 3u);
}

TEST(EmptyPatternReturnsNotFound)
{
    uint8_t data[] = { 0x00 };
    
    auto result = Pattern::Scan(
        reinterpret_cast<uintptr_t>(data), sizeof(data),
        ""
    );
    
    ASSERT_TRUE(!result.found);
}

// ─── Main ─────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "Running pattern scanner tests...\n\n";
    
    for (auto& test : tests) {
        std::cout << "  " << test.name << "... ";
        int prevFailures = failures;
        test.func();
        if (failures == prevFailures) {
            std::cout << "PASS\n";
        }
    }
    
    std::cout << "\n" << tests.size() << " tests, " << failures << " failures\n";
    return failures > 0 ? 1 : 0;
}
