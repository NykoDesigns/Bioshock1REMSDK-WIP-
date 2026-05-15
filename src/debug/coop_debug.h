#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace bs1sdk {

// ─── Output directory for all debug dumps ────────────────────────────────
// Resolved at runtime — see GetDebugDir() in coop_debug.cpp
const char* GetDebugDir();

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

// ─── SDK Generation ──────────────────────────────────────────────────────
// Generate full Unreal-style SDK headers from runtime reflection.
// Dumps every class, struct, enum, property (with offset/size), and function.
// Output: debug_dumps/SDK/

void DumpSDKHeaders();

// ─── Object Inspector ────────────────────────────────────────────────────
// Deep inspection of a single object by class name or address.
// Returns formatted string with all properties and their current values.

std::string InspectObject(const std::string& classNameOrAddr);

// ─── Class Hierarchy ─────────────────────────────────────────────────────
// Dump full inheritance tree of all classes.
// Output: debug_dumps/class_hierarchy.txt

void DumpClassHierarchy();

// ─── Function Catalog ────────────────────────────────────────────────────
// Dump all UFunction objects with their flags, parameter info.
// Output: debug_dumps/all_functions.txt

void DumpAllFunctions();

// ─── Event Catalog (Co-op Sync Blueprint) ────────────────────────────────
// Records every unique ProcessEvent with parameter layouts, categorized by
// sync relevance: interaction, damage, state change, movement, AI, UI.
// This is the blueprint for what needs to be network-replicated in co-op.
// Output: debug_dumps/event_catalog.txt

struct EventCatalogEntry {
    std::string FuncName;
    std::string OwnerClass;
    std::string Category;        // interaction, damage, state, movement, AI, UI, other
    uint32_t FunctionFlags = 0;
    int32_t ParamsSize = 0;
    uint64_t HitCount = 0;
    bool SyncRelevant = false;   // true if this event should be replicated
    std::vector<std::string> ParamDescriptions; // "Name: Type (size)" for each param
    std::vector<std::string> SampleActors;      // first 3 actor instances that fired
};

void StartEventCatalog(float durationSeconds = 30.0f);
void StopEventCatalog();
bool IsEventCatalogRunning();
void DumpEventCatalog();

// ─── Interaction Discovery ───────────────────────────────────────────────
// Finds all Use, Touch, UnTouch, Trigger, TakeDamage, Bump, HitWall,
// Landed, Destroyed events across all classes with full param layouts.
// These are the events P2 needs to simulate for world interaction.
// Output: debug_dumps/interaction_events.txt

void DumpInteractionEvents();

// ─── Actor State Diff ────────────────────────────────────────────────────
// Captures property snapshots of tracked actors and detects changes per tick.
// Useful for finding which properties change when doors open, items disappear, etc.
// Output: debug_dumps/state_diff.txt

void StartStateDiff(const std::string& classFilter = "", float durationSeconds = 10.0f);
void StopStateDiff();
bool IsStateDiffRunning();
void DumpStateDiff();

// ─── Console integration ─────────────────────────────────────────────────
// All dump commands are also accessible from console.

// ─── Init/Shutdown ───────────────────────────────────────────────────────

void InitCoopDebug();
void ShutdownCoopDebug();

} // namespace bs1sdk
