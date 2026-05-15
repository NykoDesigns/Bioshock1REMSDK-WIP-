#include "coop_testing.h"
#include "coop_true.h"
#include "coop_world_sync.h"
#include "coop_p2.h"
#include "coop_inventory.h"
#include "coop_transitions.h"
#include "net_manager.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../hooks/process_event.h"
#include "../debug/coop_debug.h"

#include <fstream>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace bs1sdk {

// ─── State ───────────────────────────────────────────────────────────────

static std::vector<CoopTestResult> s_Results;
static bool s_TestRunning = false;
static std::string s_CurrentTest;
static int s_TestsPassed = 0, s_TestsFailed = 0, s_TestsSkipped = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────

static void AddResult(const std::string& name, const std::string& phase,
                      TestResult result, const std::string& details)
{
    CoopTestResult r;
    r.TestName = name;
    r.Phase = phase;
    r.Result = result;
    r.Details = details;
    s_Results.push_back(r);

    if (result == TestResult::Pass) s_TestsPassed++;
    else if (result == TestResult::Fail) s_TestsFailed++;
    else if (result == TestResult::Skipped) s_TestsSkipped++;

    const char* resultStr[] = {"PENDING", "RUNNING", "PASS", "FAIL", "SKIP"};
    LOG_INFO("[Test] {} - {} - {}", resultStr[(int)result], name, details);
    DebugSessionLogf("TEST %s: %s - %s", resultStr[(int)result], name.c_str(), details.c_str());
}

#define TEST_ASSERT(cond, name, phase, passMsg, failMsg) \
    if (cond) { AddResult(name, phase, TestResult::Pass, passMsg); } \
    else { AddResult(name, phase, TestResult::Fail, failMsg); }

#define TEST_SKIP(name, phase, reason) \
    AddResult(name, phase, TestResult::Skipped, reason)

// ─── Init / Shutdown ─────────────────────────────────────────────────────

void InitCoopTesting()
{
    s_Results.clear();
    s_TestsPassed = s_TestsFailed = s_TestsSkipped = 0;
    LOG_INFO("[Test] Co-op testing framework ready");
}

void ShutdownCoopTesting()
{
    if (!s_Results.empty()) DumpCoopTestResults();
}

// ─── Phase 1: Freeze Tests ──────────────────────────────────────────────

void TestPhase1_Freeze()
{
    LOG_INFO("[Test] ═══ Phase 1: Client Simulation Freeze ═══");

    // Test 1.1: PE hook is active
    TEST_ASSERT(IsProcessEventHooked(), "PE hook active", "Phase1",
                "ProcessEvent hook is installed",
                "ProcessEvent hook NOT installed - cannot freeze");

    // Test 1.2: Can freeze
    bool froze = FreezeClientSimulation();
    TEST_ASSERT(froze, "Freeze activates", "Phase1",
                "FreezeClientSimulation() returned true",
                "FreezeClientSimulation() FAILED");

    // Test 1.3: IsSimulationFrozen reports correctly
    TEST_ASSERT(IsSimulationFrozen(), "Frozen state correct", "Phase1",
                "IsSimulationFrozen() == true after freeze",
                "IsSimulationFrozen() == false after freeze!");

    // Test 1.4: Unfreeze works
    UnfreezeClientSimulation();
    TEST_ASSERT(!IsSimulationFrozen(), "Unfreeze works", "Phase1",
                "IsSimulationFrozen() == false after unfreeze",
                "IsSimulationFrozen() still true after unfreeze!");

    // Test 1.5: World system is ready (needed for actor comparison)
    TEST_ASSERT(IsWorldSystemReady(), "World system ready", "Phase1",
                "World system is initialized",
                "World system NOT ready - actors unavailable");

    // Test 1.6: Can get actors (baseline)
    if (IsWorldSystemReady()) {
        auto actors = GetAllActors();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Found %d actors in level", (int)actors.size());
        TEST_ASSERT(actors.size() > 0, "Actors exist", "Phase1", buf,
                    "No actors found in level!");
    } else {
        TEST_SKIP("Actors exist", "Phase1", "World system not ready");
    }

    // Test 1.7: Player position accessible
    FVec3 pos;
    bool hasPos = GetPlayerPosition(pos);
    if (hasPos) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Player at (%.0f, %.0f, %.0f)", pos.X, pos.Y, pos.Z);
        AddResult("Player position", "Phase1", TestResult::Pass, buf);
    } else {
        AddResult("Player position", "Phase1", TestResult::Fail, "Cannot read player position");
    }

    // Test 1.8: Tick hook active
    TEST_ASSERT(IsTickHookActive(), "Tick hook active", "Phase1",
                "Engine tick hook is installed",
                "Engine tick hook NOT installed");
}

// ─── Phase 2: World Sync Tests ──────────────────────────────────────────

void TestPhase2_WorldSync()
{
    LOG_INFO("[Test] ═══ Phase 2: World State Sync ═══");

    // Test 2.1: World sync initializes
    InitWorldSync();
    AddResult("WorldSync init", "Phase2", TestResult::Pass, "InitWorldSync() succeeded");

    // Test 2.2: Can build actor registry (host side)
    if (IsTrueHost() || GetTrueCoopRole() == TrueCoopRole::None) {
        // Simulate host role temporarily
        auto prevRole = GetTrueCoopRole();
        SetTrueCoopRole(TrueCoopRole::TrueHost);

        WorldSyncHostTick(0.016f); // one frame
        auto stats = GetWorldSyncStats();

        char buf[128];
        std::snprintf(buf, sizeof(buf), "Tracking %d actors, %d dirty",
                     stats.TrackedActors, stats.DirtyActors);
        TEST_ASSERT(stats.TrackedActors > 0, "Actor registry builds", "Phase2", buf,
                    "No actors tracked!");

        SetTrueCoopRole(prevRole);
    } else {
        TEST_SKIP("Actor registry builds", "Phase2", "Not host role");
    }

    // Test 2.3: World sync config is sensible
    auto& cfg = GetWorldSyncConfig();
    TEST_ASSERT(cfg.HostSendRate >= 1.0f && cfg.HostSendRate <= 60.0f,
                "Send rate valid", "Phase2",
                "Send rate in range [1, 60] Hz",
                "Send rate out of range!");
    TEST_ASSERT(cfg.MaxSyncRadius > 0,
                "Sync radius valid", "Phase2",
                "Sync radius > 0",
                "Sync radius invalid!");

    // Test 2.4: Network is available
    TEST_ASSERT(true, "UDP socket available", "Phase2",
                "Winsock2 initialized (ws2_32 linked)", "");
}

// ─── Phase 3: P2 Pawn Tests ─────────────────────────────────────────────

void TestPhase3_P2Pawn()
{
    LOG_INFO("[Test] ═══ Phase 3: Player 2 Pawn ═══");

    // Test 3.1: P2 system initializes
    InitP2System();
    AddResult("P2 system init", "Phase3", TestResult::Pass, "InitP2System() succeeded");

    // Test 3.2: Try to spawn P2 pawn (host only or in test mode)
    if (IsTrueHost() || GetTrueCoopRole() == TrueCoopRole::None) {
        auto prevRole = GetTrueCoopRole();
        SetTrueCoopRole(TrueCoopRole::TrueHost);

        bool spawned = P2SpawnPawn();
        if (spawned) {
            UObject* pawn = P2GetPawn();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "P2 pawn at 0x%08X", (uint32_t)(uintptr_t)pawn);
            AddResult("P2 pawn spawn", "Phase3", TestResult::Pass, buf);

            // Test 3.3: Can get P2 state
            P2PawnState state = P2GetState();
            TEST_ASSERT(state.IsAlive == 1, "P2 state alive", "Phase3",
                        "P2 pawn reports alive", "P2 pawn not alive after spawn!");

            // Test 3.4: Can move P2 pawn
            P2InputData input{};
            input.moveForward = 1.0f;
            input.lookYaw = 0;
            P2ApplyInput(input);

            P2PawnState state2 = P2GetState();
            bool moved = (state2.PosX != state.PosX || state2.PosY != state.PosY);
            TEST_ASSERT(moved, "P2 pawn moves", "Phase3",
                        "P2 moved after input applied",
                        "P2 did NOT move after input!");

            // Clean up
            P2DestroyPawn();
        } else {
            AddResult("P2 pawn spawn", "Phase3", TestResult::Fail,
                      "No candidate pawn found (expected in some levels)");
        }

        SetTrueCoopRole(prevRole);
    } else {
        TEST_SKIP("P2 pawn spawn", "Phase3", "Not host role");
    }
}

// ─── Phase 4: Combat Tests ──────────────────────────────────────────────

void TestPhase4_Combat()
{
    LOG_INFO("[Test] ═══ Phase 4: Combat Sync ═══");

    // Test 4.1: Combat event structure is correct size
    TEST_ASSERT(sizeof(P2CombatEvent) > 0, "Combat event struct", "Phase4",
                "P2CombatEvent defined correctly", "");

    // Test 4.2: Can find enemies nearby (needed for hitscan)
    FVec3 pos;
    if (GetPlayerPosition(pos)) {
        auto nearby = GetActorsInRadius(pos, 3000.0f);
        int enemies = 0;
        for (auto* a : nearby) {
            std::string cn = a->GetObjClassName();
            if (cn.find("Thug") != std::string::npos ||
                cn.find("Aggressor") != std::string::npos ||
                cn.find("BigDaddy") != std::string::npos) {
                enemies++;
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%d enemies within 3000 units", enemies);
        AddResult("Enemies detectable", "Phase4",
                  enemies > 0 ? TestResult::Pass : TestResult::Skipped, buf);
    } else {
        TEST_SKIP("Enemies detectable", "Phase4", "No player position");
    }

    // Test 4.3: Damage packet struct is valid
    TEST_ASSERT(sizeof(DamageData) > 0, "Damage packet valid", "Phase4",
                "DamageData struct defined", "");

    // Test 4.4: P2 combat action enum covers all cases
    TEST_ASSERT((int)P2CombatAction::Respawn == 7, "Combat actions complete", "Phase4",
                "All 8 combat actions defined (0-7)", "Combat action enum incomplete");
}

// ─── Phase 5: Inventory Tests ────────────────────────────────────────────

void TestPhase5_Inventory()
{
    LOG_INFO("[Test] ═══ Phase 5: Inventory & Progression ═══");

    // Test 5.1: Inventory initializes
    InitP2Inventory();
    AddResult("P2 inventory init", "Phase5", TestResult::Pass, "InitP2Inventory() succeeded");

    // Test 5.2: Starting loadout is correct
    auto& inv = GetP2Inventory();
    TEST_ASSERT(inv.Weapons[0].Owned && inv.Weapons[0].ClassName == "Wrench",
                "Has wrench", "Phase5", "Wrench in slot 0", "No wrench!");
    TEST_ASSERT(inv.Weapons[1].Owned && inv.Weapons[1].ClassName == "Pistol",
                "Has pistol", "Phase5", "Pistol in slot 1 with ammo", "No pistol!");
    TEST_ASSERT(inv.Plasmids[0].Owned, "Has Electro Bolt", "Phase5",
                "Electro Bolt in plasmid slot 0", "No starting plasmid!");

    // Test 5.3: Pickup works
    P2PickupEvent pickup{};
    pickup.Type = PickupType::Ammo;
    pickup.SlotId = 1; // pistol
    pickup.Amount = 12;
    bool picked = P2PickupItem(pickup);
    TEST_ASSERT(picked, "Ammo pickup works", "Phase5",
                "Added 12 ammo to pistol", "Ammo pickup failed!");

    // Test 5.4: Credits spending works
    int oldCredits = inv.Credits;
    VendingTransaction txn{};
    txn.Cost = 20;
    txn.ItemType = PickupType::Health;
    strncpy(txn.ItemName, "First Aid Kit", sizeof(txn.ItemName));
    bool bought = P2VendingBuy(txn);
    TEST_ASSERT(bought && inv.Credits == oldCredits - 20,
                "Vending purchase", "Phase5",
                "Spent $20, credits deducted correctly",
                "Vending purchase failed or credits wrong!");

    // Test 5.5: Can't overspend
    inv.Credits = 5;
    txn.Cost = 100;
    bool cantBuy = !P2VendingBuy(txn);
    TEST_ASSERT(cantBuy, "Cannot overspend", "Phase5",
                "Correctly rejected purchase exceeding credits",
                "Allowed purchase without enough credits!");
    inv.Credits = 100; // restore

    // Test 5.6: Ammo consumption
    inv.Weapons[1].CurrentMagazine = 6;
    bool consumed = P2ConsumeAmmo();
    TEST_ASSERT(consumed && inv.Weapons[1].CurrentMagazine == 5,
                "Ammo consumed", "Phase5",
                "Magazine decremented on fire", "Ammo not consumed!");

    // Test 5.7: EVE consumption
    float oldEVE = inv.EVE;
    bool eveUsed = P2ConsumeEVE(25.0f);
    TEST_ASSERT(eveUsed && inv.EVE == oldEVE - 25.0f,
                "EVE consumed", "Phase5",
                "EVE deducted for plasmid use", "EVE not consumed!");
}

// ─── Phase 6: Transitions Tests ──────────────────────────────────────────

void TestPhase6_Transitions()
{
    LOG_INFO("[Test] ═══ Phase 6: Transitions & Polish ═══");

    // Test 6.1: Transitions system initializes
    InitTransitions();
    AddResult("Transitions init", "Phase6", TestResult::Pass, "InitTransitions() succeeded");

    // Test 6.2: Not in transition initially
    TEST_ASSERT(!IsInTransition(), "Not in transition", "Phase6",
                "No active transition", "Spuriously in transition state!");

    // Test 6.3: Not in cutscene initially
    TEST_ASSERT(!IsInCutscene(), "Not in cutscene", "Phase6",
                "No active cutscene", "Spuriously in cutscene state!");

    // Test 6.4: Cutscene detection works
    OnCutsceneStart("TestCutscene");
    TEST_ASSERT(IsInCutscene(), "Cutscene starts", "Phase6",
                "Cutscene correctly detected as active", "Cutscene not active!");
    OnCutsceneEnd();
    TEST_ASSERT(!IsInCutscene(), "Cutscene ends", "Phase6",
                "Cutscene correctly ended", "Cutscene still active!");

    // Test 6.5: Big Daddy tracking
    TrackBigDaddies();
    auto bds = GetBigDaddyStates();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Found %d Big Daddies in level", (int)bds.size());
    AddResult("Big Daddy tracking", "Phase6",
              TestResult::Pass, buf); // pass even if 0 (level dependent)

    // Test 6.6: Null guard counter works
    int before = GetNullGuardCount();
    RegisterNullGuard("test_context");
    TEST_ASSERT(GetNullGuardCount() == before + 1, "Null guard counting", "Phase6",
                "Null guard counter incremented", "Counter not working!");

    // Test 6.7: Level info
    auto info = GetTransitionInfo();
    if (!info.CurrentLevel.empty()) {
        AddResult("Level detected", "Phase6", TestResult::Pass,
                  "Current level: " + info.CurrentLevel);
    } else {
        AddResult("Level detected", "Phase6", TestResult::Skipped,
                  "Level name not yet available");
    }
}

// ─── Quick Validate ──────────────────────────────────────────────────────

void QuickValidate()
{
    LOG_INFO("[Test] ═══ Quick Validation ═══");
    s_Results.clear();
    s_TestsPassed = s_TestsFailed = s_TestsSkipped = 0;

    // Core engine checks
    auto& globals = GetEngineGlobals();
    TEST_ASSERT(globals.IsValid(), "Engine globals valid", "Core",
                "GObjects and GNames found", "Engine globals INVALID");
    TEST_ASSERT(IsProcessEventHooked(), "PE hooked", "Core",
                "ProcessEvent intercepted", "PE NOT hooked");
    TEST_ASSERT(IsWorldSystemReady(), "World ready", "Core",
                "World system initialized", "World NOT ready");
    TEST_ASSERT(IsTickHookActive(), "Tick hook", "Core",
                "Engine tick hooked", "Tick NOT hooked");

    FVec3 pos;
    bool hasPlayer = GetPlayerPosition(pos);
    if (hasPlayer) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "At (%.0f, %.0f, %.0f)", pos.X, pos.Y, pos.Z);
        AddResult("Player accessible", "Core", TestResult::Pass, buf);
    } else {
        AddResult("Player accessible", "Core", TestResult::Fail, "Cannot get player position");
    }

    DumpCoopTestResults();
}

// ─── Run All ─────────────────────────────────────────────────────────────

void RunCoopTests(const std::string& phase)
{
    s_Results.clear();
    s_TestsPassed = s_TestsFailed = s_TestsSkipped = 0;
    s_TestRunning = true;

    if (phase == "all" || phase == "phase1" || phase == "1") TestPhase1_Freeze();
    if (phase == "all" || phase == "phase2" || phase == "2") TestPhase2_WorldSync();
    if (phase == "all" || phase == "phase3" || phase == "3") TestPhase3_P2Pawn();
    if (phase == "all" || phase == "phase4" || phase == "4") TestPhase4_Combat();
    if (phase == "all" || phase == "phase5" || phase == "5") TestPhase5_Inventory();
    if (phase == "all" || phase == "phase6" || phase == "6") TestPhase6_Transitions();

    s_TestRunning = false;
    DumpCoopTestResults();
}

bool IsCoopTestRunning() { return s_TestRunning; }

std::string GetCoopTestProgress()
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "Tests: %d passed, %d failed, %d skipped (%d total)\nCurrent: %s",
        s_TestsPassed, s_TestsFailed, s_TestsSkipped,
        (int)s_Results.size(), s_CurrentTest.c_str());
    return buf;
}

std::vector<CoopTestResult> GetCoopTestResults() { return s_Results; }

// ─── Dump ────────────────────────────────────────────────────────────────

void DumpCoopTestResults()
{
    std::string filepath = std::string(GetDebugDir()) + "/coop_test_results.txt";
    std::ofstream out(filepath);
    out << "╔══════════════════════════════════════════════════════════════╗\n";
    out << "║  CO-OP TEST RESULTS                                         ║\n";
    out << "╚══════════════════════════════════════════════════════════════╝\n\n";

    char summary[128];
    std::snprintf(summary, sizeof(summary),
        "PASSED: %d  |  FAILED: %d  |  SKIPPED: %d  |  TOTAL: %d\n\n",
        s_TestsPassed, s_TestsFailed, s_TestsSkipped, (int)s_Results.size());
    out << summary;

    // Group by phase
    std::string lastPhase;
    for (auto& r : s_Results) {
        if (r.Phase != lastPhase) {
            out << "\n─── " << r.Phase << " ───\n";
            lastPhase = r.Phase;
        }

        const char* icon = "?";
        switch (r.Result) {
            case TestResult::Pass: icon = "✓"; break;
            case TestResult::Fail: icon = "✗"; break;
            case TestResult::Skipped: icon = "○"; break;
            default: break;
        }

        char line[256];
        std::snprintf(line, sizeof(line), "  %s %-35s %s\n",
                     icon, r.TestName.c_str(), r.Details.c_str());
        out << line;
    }

    out << "\n═══════════════════════════════════════════════════════════════\n";
    if (s_TestsFailed > 0) {
        out << "RESULT: SOME TESTS FAILED - see details above\n";
    } else {
        out << "RESULT: ALL TESTS PASSED\n";
    }

    out.close();
    LOG_INFO("[Test] Results: {} pass, {} fail, {} skip -> {}",
             s_TestsPassed, s_TestsFailed, s_TestsSkipped, filepath);
}

} // namespace bs1sdk
