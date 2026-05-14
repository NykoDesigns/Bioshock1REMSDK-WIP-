#pragma once

#include <string>
#include <vector>
#include <functional>

namespace bs1sdk {

// ─── Co-op Self-Testing Framework ────────────────────────────────────────
// Automated tests that run in-game and log results.
// Run via console: "cooptest all" or "cooptest phase1" etc.
// Results written to debug_dumps/coop_test_results.txt

// ─── Test Result ─────────────────────────────────────────────────────────

enum class TestResult { Pending, Running, Pass, Fail, Skipped };

struct CoopTestResult {
    std::string TestName;
    std::string Phase;
    TestResult Result = TestResult::Pending;
    std::string Details;
    float Duration = 0;
};

// ─── Public API ──────────────────────────────────────────────────────────

/// Initialize testing framework
void InitCoopTesting();
void ShutdownCoopTesting();

/// Run all tests for a specific phase (or "all")
void RunCoopTests(const std::string& phase = "all");

/// Is a test currently running?
bool IsCoopTestRunning();

/// Get test progress
std::string GetCoopTestProgress();

/// Get all results
std::vector<CoopTestResult> GetCoopTestResults();

/// Dump results to file
void DumpCoopTestResults();

/// Individual phase tests (can run standalone)
void TestPhase1_Freeze();
void TestPhase2_WorldSync();
void TestPhase3_P2Pawn();
void TestPhase4_Combat();
void TestPhase5_Inventory();
void TestPhase6_Transitions();

/// Quick validation (runs fast, no delays)
void QuickValidate();

} // namespace bs1sdk
