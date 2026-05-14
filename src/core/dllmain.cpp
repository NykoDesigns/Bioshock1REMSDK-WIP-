#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "pattern.h"
#include "mod_config.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../gameplay/gameplay_mods.h"
#include "../debug/coop_debug.h"
#include "../debug/crash_handler.h"
#include "../network/coop_true.h"

#include <Windows.h>
#include <thread>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bs1sdk {

// Forward declarations
void InitializeSDK();
void ShutdownSDK();
void FindEngineGlobals(uintptr_t baseAddr, size_t moduleSize);
static void DumpPropertyLayout();

static HMODULE g_Module = nullptr;
static bool g_Running = false;

void MainThread(HMODULE hModule)
{
    g_Module = hModule;
    g_Running = true;

    // Initialize logging first
    Log::Initialize();
    LOG_INFO("BS1SDK v{}.{}.{} initializing...", 0, 1, 0);

    // Wait for game to fully load
    // D3D9 device won't exist until rendering starts
    while (!GetModuleHandleA("d3d9.dll")) {
        if (!g_Running) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    LOG_INFO("d3d9.dll detected, proceeding with initialization");

    InitializeSDK();

    // Main loop - keep DLL alive, handle hot reload signals
    while (g_Running) {
        // Check for unload hotkey (F12)
        if (GetAsyncKeyState(VK_F12) & 1) {
            LOG_INFO("Unload hotkey pressed");
            g_Running = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    ShutdownSDK();
    
    // Give hooks time to unhook cleanly
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    FreeLibraryAndExitThread(g_Module, 0);
}

void InitializeSDK()
{
    LOG_INFO("Initializing hook framework...");
    
    if (!Hooks::Initialize()) {
        LOG_ERROR("Failed to initialize hook framework!");
        return;
    }

    // Install crash handler FIRST so we catch any crashes during init
    InstallCrashHandler();
    CrashSetContext("init:scanning");

    LOG_INFO("Scanning for engine structures...");
    
    // Get base address of main executable
    uintptr_t baseAddr = Memory::GetModuleBase(nullptr);
    size_t moduleSize = Memory::GetModuleSize(nullptr);
    LOG_INFO("Game module base: 0x{:08X}, size: 0x{:X}", baseAddr, moduleSize);

    // Phase 1: Hook D3D9 for rendering overlay
    if (!Hooks::InstallD3D9Hook()) {
        LOG_ERROR("Failed to install D3D9 hook!");
    } else {
        LOG_INFO("D3D9 hook installed successfully");
    }

    // Phase 2: Find GNames via runtime heuristic
    // In UE2.5, GNames is a TArray<FNameEntry*>. Layout:
    //   struct TArray { T* Data; int Count; int Max; }
    // FNameEntry[0] always contains "None" as a null-terminated ASCII or wide string.
    // Strategy: scan the game module's .data section for a TArray-like struct
    // where Data[0] points to something containing "None".
    FindEngineGlobals(baseAddr, moduleSize);
    
    // Fallback: if heuristic scan fails, try known offsets from previous session
    EngineGlobals& g = GetEngineGlobals();
    if (!g.GNames) {
        uintptr_t knownGNames = baseAddr + 0x13904EC;
        uintptr_t knownGObjects = baseAddr + 0x139042C;
        
        // Validate known GNames offset
        if (Memory::IsValidPtr(knownGNames)) {
            uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(knownGNames);
            int32_t count = *reinterpret_cast<int32_t*>(knownGNames + 4);
            if (count > 1000 && count < 200000 && Memory::IsValidPtr(dataPtr)) {
                uintptr_t firstEntry = *reinterpret_cast<uintptr_t*>(dataPtr);
                if (firstEntry && Memory::IsValidPtr(firstEntry)) {
                    wchar_t test[8] = {};
                    memcpy(test, reinterpret_cast<void*>(firstEntry + 16), 10);
                    if (test[0] == L'N' && test[1] == L'o' && test[2] == L'n' && test[3] == L'e') {
                        g.GNames = knownGNames;
                        g.GObjects = knownGObjects;
                        LOG_INFO("Engine globals found via known offsets (fallback)");
                        LOG_INFO("  GNames: 0x{:08X} (count={})", knownGNames, count);
                        int32_t objCount = *reinterpret_cast<int32_t*>(knownGObjects + 4);
                        LOG_INFO("  GObjects: 0x{:08X} (count={})", knownGObjects, objCount);
                    }
                }
            }
        }
    }

    // Phase 3: Discover UField/UStruct/UProperty layout
    if (GetEngineGlobals().IsValid()) {
        DumpPropertyLayout();
    }

    // Phase 4: World system + GNatives + Tick hook
    if (GetEngineGlobals().IsValid()) {
        // Wait for level to load (actors need to exist)
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        
        if (InitWorldSystem()) {
            LOG_INFO("World system ready - {} actors in level", GetCurrentLevel().ActorCount);
        }
        
        if (InitNativeTable()) {
            LOG_INFO("GNatives table found - {} native functions", GetNativeCount());
            DumpNativesToFile("Z:\\Bioshock1SDK\\gnatives_dump.txt");
        }
        
        if (InstallTickHook()) {
            LOG_INFO("Engine tick hook installed");
        }

        CrashSetContext("init:coop_debug");
        InitCoopDebug();

        CrashSetContext("init:true_coop");
        InitTrueCoop();
    }

    // Phase 5: Load mod config and auto-init gameplay mods
    ModConfig cfg = LoadModConfig();
    if (cfg.autoInitMods && GetEngineGlobals().IsValid()) {
        LOG_INFO("Auto-initializing gameplay mods from config...");
        // Wait a bit more for game classes to be fully loaded
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        if (InitGameplayMods()) {
            SetDecoyTeleportEnabled(cfg.decoyTeleport);
            SetFriendlyBotsEnabled(cfg.friendlyBots);
            SetFriendlyBotLimit(cfg.friendlyBotLimit);
            SetChainLightningEnabled(cfg.chainLightning);
            SetChainLightningRadius(cfg.chainRadius);
            SetChainLightningJumps(cfg.chainMaxJumps);
            SetChainLightningDamageFalloff(cfg.chainDamageFalloff);
            if (cfg.rivetPistol) SetRivetPistolEnabled(true);
            if (cfg.splicerFactions) SetSplicerFactionsEnabled(true);
            LOG_INFO("Gameplay mods configured from mod_config.json");
        }
    }

    CrashSetContext("runtime:normal");
    LOG_INFO("BS1SDK initialization complete");
}

// ─── Runtime Engine Global Discovery ─────────────────────────────────────
// Finds GNames and GObjects by scanning the game's data sections at runtime.
// This is more reliable than byte patterns because it uses structural invariants.

void FindEngineGlobals(uintptr_t baseAddr, size_t moduleSize)
{
    LOG_INFO("Scanning for GNames/GObjects (runtime heuristic)...");
    
    EngineGlobals& globals = GetEngineGlobals();
    
    // ─── Strategy for GNames ─────────────────────────────────────────────
    // GNames is a TArray<FNameEntry*>: { FNameEntry** Data; int Count; int Max; }
    // Invariant: Data[0]->Name == "None" (always first entry in UE name table)
    //
    // We scan the game module for a structure where:
    //   - DWORD at offset+0 is a valid pointer (Data)
    //   - DWORD at offset+4 is a reasonable count (100 < Count < 200000)
    //   - DWORD at offset+8 >= Count (Max >= Count)
    //   - Data[0] is a valid pointer
    //   - At Data[0]+some_offset, we find "None" as ASCII
    //
    // FNameEntry layout (UE2.5): varies, but name string is typically at offset 0x10 or after
    // an index + hash field. We'll try multiple offsets.
    
    int gnamesCandidates = 0;
    
    for (uintptr_t scan = baseAddr; scan < baseAddr + moduleSize - 12; scan += 4) {
        __try {
            uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(scan);
            int32_t count = *reinterpret_cast<int32_t*>(scan + 4);
            int32_t max = *reinterpret_cast<int32_t*>(scan + 8);
            
            // Filter: count must be reasonable for a name table
            if (count < 1000 || count > 200000) continue;
            if (max < count || max > 500000) continue;
            
            // Data pointer must be valid heap memory
            if (dataPtr < 0x10000 || dataPtr > 0x7FFFFFFF) continue;
            if (!Memory::IsValidPtr(dataPtr)) continue;
            
            // Read first entry pointer: Data[0]
            uintptr_t firstEntry = *reinterpret_cast<uintptr_t*>(dataPtr);
            if (firstEntry < 0x10000 || firstEntry > 0x7FFFFFFF) continue;
            if (!Memory::IsValidPtr(firstEntry)) continue;
            
            // Check if "None" string exists at various offsets in the FNameEntry
            // Common UE2 FNameEntry layouts:
            //   Layout A: [Index(4)] [HashNext(4)] [Name(char[])]  -> name at +8
            //   Layout B: [Index(4)] [Flags(4)] [HashNext(4)] [Name(char[])] -> name at +12
            //   Layout C: [HashNext(4)] [Index(4)] [Len(4)] [Name(char[])] -> name at +12
            //   Layout D: [NameLen(2)] [Name(wchar_t[])] -> name at +2 (wide)
            //   Layout E: Direct pointer to string
            
            bool found = false;
            int nameOffset = 0;
            bool isWide = false;
            
            // Try ASCII "None" at various offsets
            static const int offsets[] = { 0, 4, 8, 12, 16, 20, 24 };
            for (int off : offsets) {
                uintptr_t strAddr = firstEntry + off;
                if (!Memory::IsValidPtr(strAddr)) continue;
                
                // Check ASCII "None\0"
                char buf[8] = {};
                memcpy(buf, reinterpret_cast<void*>(strAddr), 5);
                if (buf[0] == 'N' && buf[1] == 'o' && buf[2] == 'n' && buf[3] == 'e' && buf[4] == '\0') {
                    found = true;
                    nameOffset = off;
                    isWide = false;
                    break;
                }
                
                // Check wide "None\0" (UTF-16LE: 'N' 0x00 'o' 0x00 ...)
                wchar_t wbuf[8] = {};
                memcpy(wbuf, reinterpret_cast<void*>(strAddr), 10);
                if (wbuf[0] == L'N' && wbuf[1] == L'o' && wbuf[2] == L'n' && wbuf[3] == L'e' && wbuf[4] == L'\0') {
                    found = true;
                    nameOffset = off;
                    isWide = true;
                    break;
                }
            }
            
            if (!found) continue;
            
            gnamesCandidates++;
            
            // Validate further: check Data[1] also has a reasonable name
            uintptr_t secondEntry = *reinterpret_cast<uintptr_t*>(dataPtr + 4);
            if (secondEntry < 0x10000 || !Memory::IsValidPtr(secondEntry)) continue;
            
            char nameBuf[64] = {};
            if (!isWide) {
                memcpy(nameBuf, reinterpret_cast<void*>(secondEntry + nameOffset), 32);
            } else {
                wchar_t wnameBuf[32] = {};
                memcpy(wnameBuf, reinterpret_cast<void*>(secondEntry + nameOffset), 64);
                // Convert to ASCII for logging
                for (int i = 0; i < 31 && wnameBuf[i]; i++)
                    nameBuf[i] = static_cast<char>(wnameBuf[i] & 0x7F);
            }
            
            // Name[1] should be printable ASCII (common: "ByteProperty", "IntProperty", etc.)
            bool validName = true;
            for (int i = 0; i < 32 && nameBuf[i]; i++) {
                if (nameBuf[i] < 0x20 || nameBuf[i] > 0x7E) {
                    validName = false;
                    break;
                }
            }
            if (!validName || nameBuf[0] == '\0') continue;
            
            // SUCCESS! Found GNames
            globals.GNames = scan;
            LOG_INFO("=== GNames FOUND at 0x{:08X} ===", scan);
            LOG_INFO("  Data=0x{:08X}, Count={}, Max={}", dataPtr, count, max);
            LOG_INFO("  FNameEntry name offset: {} ({})", nameOffset, isWide ? "wide" : "ASCII");
            LOG_INFO("  Names[0] = \"None\"");
            LOG_INFO("  Names[1] = \"{}\"", nameBuf);
            
            // Log a few more names for validation
            for (int i = 2; i < std::min(count, 10); i++) {
                uintptr_t entry = *reinterpret_cast<uintptr_t*>(dataPtr + i * 4);
                if (entry < 0x10000 || !Memory::IsValidPtr(entry)) continue;
                
                char nb[64] = {};
                if (!isWide) {
                    memcpy(nb, reinterpret_cast<void*>(entry + nameOffset), 32);
                } else {
                    wchar_t wb[32] = {};
                    memcpy(wb, reinterpret_cast<void*>(entry + nameOffset), 64);
                    for (int j = 0; j < 31 && wb[j]; j++)
                        nb[j] = static_cast<char>(wb[j] & 0x7F);
                }
                LOG_INFO("  Names[{}] = \"{}\"", i, nb);
            }
            
            break; // Found it, stop scanning
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    
    if (!globals.GNames) {
        LOG_WARN("GNames not found via heuristic scan (checked {} candidates)", gnamesCandidates);
    }
    
    // ─── Strategy for GObjects ───────────────────────────────────────────
    // GObjects is a TArray<UObject*>: { UObject** Data; int Count; int Max; }
    // Invariant: Count is large (10000+), Data[i]->VTable points to code
    // We'll find this after GNames is confirmed, since we need names to validate objects
    
    if (globals.GNames) {
        LOG_INFO("Scanning for GObjects...");
        
        for (uintptr_t scan = baseAddr; scan < baseAddr + moduleSize - 12; scan += 4) {
            __try {
                uintptr_t dataPtr = *reinterpret_cast<uintptr_t*>(scan);
                int32_t count = *reinterpret_cast<int32_t*>(scan + 4);
                int32_t max = *reinterpret_cast<int32_t*>(scan + 8);
                
                // GObjects typically has 10000-100000+ entries
                if (count < 5000 || count > 500000) continue;
                if (max < count || max > 1000000) continue;
                if (dataPtr < 0x10000 || dataPtr > 0x7FFFFFFF) continue;
                if (!Memory::IsValidPtr(dataPtr)) continue;
                
                // Skip if this is the same as GNames
                if (scan == globals.GNames) continue;
                
                // Validate: first few non-null entries should look like UObjects
                // (vtable pointer in code, followed by reasonable field values)
                int validObjects = 0;
                int nullCount = 0;
                
                for (int i = 0; i < 20; i++) {
                    uintptr_t objPtr = *reinterpret_cast<uintptr_t*>(dataPtr + i * 4);
                    if (objPtr == 0) { nullCount++; continue; }
                    if (objPtr < 0x10000 || !Memory::IsValidPtr(objPtr)) break;
                    
                    // First DWORD of object should be vtable (in code section)
                    uintptr_t vtable = *reinterpret_cast<uintptr_t*>(objPtr);
                    if (vtable < baseAddr || vtable > baseAddr + moduleSize) continue;
                    
                    validObjects++;
                }
                
                if (validObjects < 10) continue;
                
                globals.GObjects = scan;
                LOG_INFO("=== GObjects FOUND at 0x{:08X} ===", scan);
                LOG_INFO("  Data=0x{:08X}, Count={}, Max={}", dataPtr, count, max);
                LOG_INFO("  First 20 entries: {} valid objects, {} null", validObjects, nullCount);
                break;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }
        
        if (!globals.GObjects) {
            LOG_WARN("GObjects not found via heuristic scan");
        }
    }
    
    if (globals.IsValid()) {
        LOG_INFO("Engine globals discovered successfully!");
        LOG_INFO("  GNames offset from base: +0x{:X}", globals.GNames - baseAddr);
        LOG_INFO("  GObjects offset from base: +0x{:X}", globals.GObjects - baseAddr);
    }
}

// ─── UField / UStruct / UProperty Layout Discovery ──────────────────────
// Discovers key offsets: UField::Next, UStruct::Children, UProperty::Offset
// Dumps results to a file for easy analysis.
//
// Strategy:
//   1. Find known UClass objects (Object, Field, Struct, Class)
//   2. Confirm SuperField at +0x40 (Field->Object, Struct->Field)
//   3. Find Children by testing candidate offsets on the Object class
//      (Object has known properties: CheckpointType, Class, Name, etc.)
//   4. Walk the Children->Next chain, dumping each property's raw bytes
//   5. Identify UProperty::Offset by correlating property names with
//      known UObject field offsets (Name=+0x28, Class=+0x30, Outer=+0x1C)

static void DumpPropertyLayout()
{
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    // Output to file for easy reading
    std::ofstream out("Z:\\Bioshock1SDK\\property_layout.txt");
    if (!out.is_open()) {
        LOG_WARN("Failed to open property_layout.txt for writing");
        return;
    }
    
    auto W = [&](const char* fmt, auto... args) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        out << buf << "\n";
    };
    
    W("=== UField/UStruct/UProperty Layout Discovery ===");
    W("GObjects: 0x%08X, Count: %d", (uint32_t)globals.GObjects, objCount);
    W("");
    
    // Step 1: Find known class objects
    UObject* classObject = nullptr;
    UObject* classField = nullptr;
    UObject* classStruct = nullptr;
    UObject* classClass = nullptr;
    UObject* classProperty = nullptr;
    UObject* classPawn = nullptr;
    
    for (int i = 0; i < objCount && i < 2000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        std::string name = obj->GetName();
        std::string cn = obj->GetObjClassName();
        
        if (cn == "Class") {
            if (name == "Object") classObject = obj;
            else if (name == "Field") classField = obj;
            else if (name == "Struct") classStruct = obj;
            else if (name == "Class") classClass = obj;
            else if (name == "Property") classProperty = obj;
            else if (name == "Pawn") classPawn = obj;
        }
    }
    
    W("Known classes:");
    W("  Object   = 0x%08X", (uint32_t)(uintptr_t)classObject);
    W("  Field    = 0x%08X", (uint32_t)(uintptr_t)classField);
    W("  Struct   = 0x%08X", (uint32_t)(uintptr_t)classStruct);
    W("  Class    = 0x%08X", (uint32_t)(uintptr_t)classClass);
    W("  Property = 0x%08X", (uint32_t)(uintptr_t)classProperty);
    W("  Pawn     = 0x%08X", (uint32_t)(uintptr_t)classPawn);
    W("");
    
    // Step 2: Confirm SuperField at +0x40
    W("=== SuperField Verification (expected at +0x40) ===");
    if (classField) {
        uint32_t sf = *reinterpret_cast<uint32_t*>((uint8_t*)classField + 0x40);
        W("  Field.SuperField  (+40) = 0x%08X  %s", sf,
          sf == (uint32_t)(uintptr_t)classObject ? "== Object CONFIRMED" : "MISMATCH");
    }
    if (classStruct) {
        uint32_t sf = *reinterpret_cast<uint32_t*>((uint8_t*)classStruct + 0x40);
        W("  Struct.SuperField (+40) = 0x%08X  %s", sf,
          sf == (uint32_t)(uintptr_t)classField ? "== Field CONFIRMED" : "MISMATCH");
    }
    if (classClass) {
        uint32_t sf = *reinterpret_cast<uint32_t*>((uint8_t*)classClass + 0x40);
        W("  Class.SuperField  (+40) = 0x%08X  %s", sf,
          sf == (uint32_t)(uintptr_t)classStruct ? "== Struct CONFIRMED" :
          (sf == (uint32_t)(uintptr_t)classField ? "== Field" : "OTHER"));
    }
    W("");
    
    // Step 3: Find specific known UProperty objects on the Object class
    // by scanning GObjects for properties whose Outer == classObject.
    // We know their runtime offsets in UObject:
    //   Name   -> +0x28 (NameProperty)
    //   Class  -> +0x30 (ClassProperty)  
    //   Outer  -> +0x1C (ObjectProperty)
    //   VTable -> +0x00 (PointerProperty)
    // By finding these values in the raw property bytes, we identify UProperty::Offset.
    
    W("=== Direct Property Object Analysis ===");
    W("Looking for properties whose Outer == Object class (0x%08X)...",
      (uint32_t)(uintptr_t)classObject);
    W("");
    
    struct KnownProp { 
        const char* name; 
        const char* expectedType;
        uint32_t knownOffset;  // known offset within UObject instance
    };
    KnownProp knownProps[] = {
        {"VTable", "PointerProperty", 0x00},
        {"Name", "NameProperty", 0x28},
        {"Class", "ClassProperty", 0x30},
        {"Outer", "ObjectProperty", 0x1C},
        {"ObjectFlags", "StructProperty", 0x24},
        {"ObjectInternalIndex", "StructProperty", 0x04},
        {"CheckpointType", "IntProperty", 0xFFFFFFFF}, // unknown
        {"HashNext", "PointerProperty", 0xFFFFFFFF},
        {"ObjectArchetype", "ObjectProperty", 0xFFFFFFFF},
    };
    
    // Collect all properties whose Outer == Object class
    struct FoundProp { UObject* obj; std::string name; std::string cls; uint32_t knownOff; };
    std::vector<FoundProp> foundProps;
    
    // Also collect addresses of all Object-owned properties for Next detection
    std::vector<uint32_t> objectPropAddrs;
    
    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetOuter() != classObject) continue;
        
        std::string cn = obj->GetObjClassName();
        if (cn.find("Property") == std::string::npos) continue;
        
        std::string nm = obj->GetName();
        objectPropAddrs.push_back((uint32_t)ptr);
        
        // Check if it's one of our known properties
        uint32_t knownOff = 0xFFFFFFFF;
        for (auto& kp : knownProps) {
            if (nm == kp.name) {
                knownOff = kp.knownOffset;
                break;
            }
        }
        foundProps.push_back({obj, nm, cn, knownOff});
    }
    
    W("Found %d properties on Object class", (int)foundProps.size());
    W("Known Object-property addresses: %d total", (int)objectPropAddrs.size());
    W("");
    
    // Dump raw bytes of each found property, annotating known offset values
    for (auto& fp : foundProps) {
        W("--- '%s' (%s) at 0x%08X  [known UObject offset: %s] ---",
          fp.name.c_str(), fp.cls.c_str(), (uint32_t)(uintptr_t)fp.obj,
          fp.knownOff != 0xFFFFFFFF ? 
              (std::string("+0x") + [](uint32_t v){ char b[16]; std::snprintf(b,16,"%02X",v); return std::string(b); }(fp.knownOff)).c_str() 
              : "unknown");
        
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(fp.obj);
        for (int off = 0x34; off < 0x80; off += 4) {
            uint32_t val = *reinterpret_cast<const uint32_t*>(raw + off);
            
            // Annotate: is this a pointer to another Object-class property?
            const char* ptrNote = "";
            for (auto addr : objectPropAddrs) {
                if (val == addr) { ptrNote = " <-- PTR TO OBJECT PROP (Next?)"; break; }
            }
            
            // Annotate: does this match the known offset?
            const char* offNote = "";
            if (fp.knownOff != 0xFFFFFFFF && val == fp.knownOff) {
                offNote = " <-- MATCHES KNOWN OFFSET!";
            }
            
            W("  +%02X: %08X%s%s", off, val, ptrNote, offNote);
        }
        W("");
    }
    W("");
    
    // Step 4: Also dump raw bytes of the Object, Field, Struct classes
    // for cross-reference
    W("=== Raw class dumps (0x00 to 0xA0) ===");
    struct { const char* name; UObject* cls; } classes[] = {
        {"Object", classObject}, {"Field", classField},
        {"Struct", classStruct}, {"Class", classClass}
    };
    for (auto& c : classes) {
        if (!c.cls) continue;
        W("--- %s (0x%08X) ---", c.name, (uint32_t)(uintptr_t)c.cls);
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(c.cls);
        for (int row = 0; row < 0xA0; row += 16) {
            char hex[128] = {};
            for (int d = 0; d < 4; d++) {
                uint32_t val = *reinterpret_cast<const uint32_t*>(raw + row + d * 4);
                char tmp[16];
                std::snprintf(tmp, sizeof(tmp), "%08X ", val);
                strcat(hex, tmp);
            }
            W("  +%02X: %s", row, hex);
        }
        W("");
    }
    
    out.close();
    LOG_INFO("Property layout discovery written to Z:\\Bioshock1SDK\\property_layout.txt");
}

void ShutdownSDK()
{
    LOG_INFO("Shutting down BS1SDK...");
    ShutdownTrueCoop();
    ShutdownCoopDebug();
    Hooks::Shutdown();
    Log::Shutdown();
}

} // namespace bs1sdk

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Spawn main thread to avoid deadlock in DllMain
        std::thread(bs1sdk::MainThread, hModule).detach();
        break;
    case DLL_PROCESS_DETACH:
        bs1sdk::g_Running = false;
        break;
    }
    return TRUE;
}
