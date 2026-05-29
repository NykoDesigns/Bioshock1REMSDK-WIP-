#pragma once

#include "uobject.h"
#include <string>
#include <vector>
#include <functional>

namespace bs1sdk {

// ─── ULevel ──────────────────────────────────────────────────────────────
// ULevel contains the Actors TArray and other level data.
// In UE2.5: ULevel extends ULevelBase extends UObject
// Key field: TArray<AActor*> Actors (offset discovered at runtime)

struct LevelInfo {
    uintptr_t LevelPtr = 0;         // ULevel* address
    uintptr_t ActorsArrayPtr = 0;   // Address of the TArray<AActor*> within ULevel
    int32_t ActorCount = 0;         // Number of actors in level
    std::string LevelName;
};

// ─── World System ────────────────────────────────────────────────────────

/// Initialize the world system - finds GWorld, GEngine, ULevel.
/// Should be called after GObjects/GNames are valid.
bool InitWorldSystem();

/// Check if the world system is initialized
bool IsWorldSystemReady();

/// Get current level info
LevelInfo GetCurrentLevel();

/// Get all actors in the current level
std::vector<UObject*> GetAllActors();

/// Get all actors of a specific class
std::vector<UObject*> GetActorsOfClass(const std::string& className);

/// Find the first actor of a class
UObject* FindActor(const std::string& className);

/// Find actors within a radius of a position
struct FVec3 { float X, Y, Z; };
std::vector<UObject*> GetActorsInRadius(FVec3 center, float radius, const std::string& classFilter = "");

/// Get player pawn position (from Location property)
bool GetPlayerPosition(FVec3& outPos);

/// Get any actor's position (reads Location property)
bool GetActorPosition(UObject* actor, FVec3& outPos);

/// Set an actor's position (writes Location property)
bool SetActorPosition(UObject* actor, const FVec3& pos);

/// Rotation (Pitch, Yaw, Roll) as int32 — UE2 FRotator units (65536 = 360°)
struct FRot3 { int32_t Pitch, Yaw, Roll; };

/// Get any actor's rotation (reads Rotation property)
bool GetActorRotation(UObject* actor, FRot3& outRot);

/// Set an actor's rotation (calls SetRotation via ProcessEvent, fallback to raw write)
bool SetActorRotation(UObject* actor, const FRot3& rot);

/// Get distance between two actors
float GetActorDistance(UObject* a, UObject* b);

/// Write a raw property value on an actor by property name
bool SetActorProperty(UObject* actor, const char* propName, const void* value, int size);

/// Read a raw property value from an actor by property name
bool GetActorProperty(UObject* actor, const char* propName, void* outValue, int size);

/// Iterate all actors with a callback
void ForEachActor(std::function<void(UObject* actor, const std::string& className)> callback);

// ─── GEngine Access ──────────────────────────────────────────────────────

/// Get GEngine pointer (UGameEngine)
UObject* GetEngine();

/// Get the current GameInfo (game mode object)
UObject* GetGameInfo();

/// Get the current PlayerController
UObject* GetPlayerController();

// ─── Native Function Table (GNatives) ────────────────────────────────────
// UE2.5 has a global dispatch table: void (*GNatives[4096])(UObject*, FFrame&, void*)
// Each entry is indexed by iNative from UFunction.

/// Native function signature: void Native(UObject* Context, void* Stack, void* Result)
typedef void (*NativeFunc)(UObject* Context, void* Stack, void* Result);

/// Initialize the GNatives table discovery
bool InitNativeTable();

/// Get a native function by index
NativeFunc GetNative(uint16_t index);

/// Hook a native function by index. Returns the original.
NativeFunc HookNative(uint16_t index, NativeFunc replacement);

/// Restore a native function to its original
void UnhookNative(uint16_t index);

/// Get the GNatives table base address
uintptr_t GetNativesTableAddress();

/// Get the number of populated native entries
int GetNativeCount();

/// Dump all natives to a file for analysis
void DumpNativesToFile(const std::string& filepath);

// ─── Engine Tick Hook ────────────────────────────────────────────────────

/// Tick callback signature
typedef std::function<void(float deltaTime)> TickCallback;

/// Install the engine tick hook (hooks UGameEngine::Tick)
bool InstallTickHook();

/// Remove the tick hook
void RemoveTickHook();

/// Register a callback to be called every engine tick
int RegisterTickCallback(TickCallback callback);

/// Remove a tick callback by ID
void UnregisterTickCallback(int id);

/// Check if tick hook is active
bool IsTickHookActive();

/// Get approximate tick rate (ticks per second)
float GetTickRate();

} // namespace bs1sdk
