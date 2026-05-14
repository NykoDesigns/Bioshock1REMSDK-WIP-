#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace bs1sdk {

// ─── Output directory for all debug dumps ────────────────────────────────
constexpr const char* DEBUG_DIR = "Z:\\Bioshock1SDK\\debug_dumps";

// ─── Session Log ─────────────────────────────────────────────────────────
// Append-mode log that persists across DLL loads. Every test session
// appends a timestamped block so we can diff between runs.

void DebugSessionLog(const std::string& msg);
void DebugSessionLogf(const char* fmt, ...);

// ─── Full Engine Snapshot ────────────────────────────────────────────────
// Dumps EVERYTHING I need to see after a test run into one file:
//   - Timestamp + session ID
//   - GObjects/GNames/GWorld/GEngine addresses
//   - Actor census (class -> count)
//   - All actor positions + classes
//   - Property offset cache for key classes
//   - PE hook stats
//   - Tick rate + frame count
//   - Network status (if co-op active)
// Output: debug_dumps/snapshot_YYYYMMDD_HHMMSS.txt

void DumpFullSnapshot(const std::string& label = "");

// ─── Actor Census ────────────────────────────────────────────────────────
// Categorize all actors by class name with counts and positions.
// Output: debug_dumps/actor_census.txt

struct ActorCensusEntry {
    std::string ClassName;
    int Count = 0;
    std::vector<std::string> SampleNames;  // first 5 instance names
};

std::map<std::string, ActorCensusEntry> RunActorCensus();
void DumpActorCensus();

// ─── Property Offset Table ───────────────────────────────────────────────
// For every actor class, discover offsets of: Location, Rotation, Health,
// Velocity, Physics, CollisionRadius, bHidden, bDeleteMe, LifeSpan.
// Output: debug_dumps/property_offsets.txt

struct ClassOffsets {
    std::string ClassName;
    int Location = -1;
    int Rotation = -1;
    int Velocity = -1;
    int Health = -1;
    int Physics = -1;
    int CollisionRadius = -1;
    int bHidden = -1;
    int bDeleteMe = -1;
    int LifeSpan = -1;
    int PropertiesSize = 0;
};

std::map<std::string, ClassOffsets> DiscoverAllClassOffsets();
void DumpPropertyOffsets();

// ─── ProcessEvent Frequency Tracker ──────────────────────────────────────
// Tracks which PE functions fire, how often, and on which classes.
// Enable with StartPETracker(), run for N seconds, then dump.
// Output: debug_dumps/pe_frequency.txt

struct PETrackEntry {
    std::string FunctionName;
    std::string ClassName;         // most common class that fires this
    uint64_t Count = 0;
    float FirstTime = 0.0f;
    float LastTime = 0.0f;
};

void StartPETracker(float durationSeconds = 10.0f);
void StopPETracker();
bool IsPETrackerRunning();
void DumpPEFrequency();

// ─── AI Function Catalog ─────────────────────────────────────────────────
// Scan all classes for functions with "Tick", "Timer", "AI", "Think",
// "Physics", "Spawn" in their name. Critical for knowing what to freeze.
// Output: debug_dumps/ai_functions.txt

void DumpAIFunctions();

// ─── Quick Diff ──────────────────────────────────────────────────────────
// Take two snapshots and see what changed (actors added/removed/moved).

void MarkSnapshotA();
void MarkSnapshotB();
void DumpSnapshotDiff();

// ─── Console integration ─────────────────────────────────────────────────
// All dump commands are also accessible from console.

// ─── Init/Shutdown ───────────────────────────────────────────────────────

void InitCoopDebug();
void ShutdownCoopDebug();

} // namespace bs1sdk
