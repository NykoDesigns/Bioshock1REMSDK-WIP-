/// winmm.dll Proxy Loader for BS1SDK
///
/// This DLL masquerades as winmm.dll. Windows' DLL search order loads DLLs
/// from the application directory first, so placing this in the game folder
/// makes it load automatically — no injector needed.
///
/// On load, it:
///   1. Loads the real winmm.dll from System32
///   2. Forwards ALL winmm exports to the real DLL via GetProcAddress trampolines
///   3. Loads BS1SDK.dll from the same directory
///
/// End users just drop winmm.dll + BS1SDK.dll into the game folder.

#include <Windows.h>
#include <cstdio>
#include <string>

// ─── Real winmm.dll handle ────────────────────────────────────────────

static HMODULE g_RealWinMM = nullptr;
static HMODULE g_BS1SDK = nullptr;

// ─── Generic trampoline approach ──────────────────────────────────────
// Store function pointers for all exports we forward. Each proxy function
// just calls through to the real one. We cover ALL functions that BioShock
// or its dependencies (fmodex, bink) might use.

typedef void* (WINAPI *GenericFunc)();

// Function pointers for all forwarded exports
static FARPROC fp[180] = {};

// Macro to define a proxy export that calls through to real winmm
#define DEFINE_PROXY(name, idx) \
    extern "C" __declspec(dllexport) void* __stdcall proxy_##name() { \
        if (!fp[idx]) return 0; \
        __asm { jmp [fp[idx]] } \
    }

// Instead of the asm approach (which is fragile), use a linker-level
// forwarding approach. We'll just implement the critical functions
// and use GetProcAddress dispatch for everything else.

// Store pointers for the critical functions plus a catch-all
typedef DWORD (WINAPI *fn_timeGetTime)(void);
typedef UINT  (WINAPI *fn_timeBeginPeriod)(UINT);
typedef UINT  (WINAPI *fn_timeEndPeriod)(UINT);

static fn_timeGetTime     real_timeGetTime     = nullptr;
static fn_timeBeginPeriod real_timeBeginPeriod = nullptr;
static fn_timeEndPeriod   real_timeEndPeriod   = nullptr;

// ─── Forwarded Exports (naked asm trampolines for x86) ────────────────
// These use naked functions that jump directly to the real DLL's function.
// This preserves the full call stack and all arguments perfectly.

#pragma warning(disable: 4740) // flow in/out of inline asm suppresses global optimization

#define PROXY_TRAMPOLINE(name, index) \
    extern "C" __declspec(naked, dllexport) void __stdcall proxy_##name() { \
        __asm { mov eax, dword ptr [fp + index * 4] } \
        __asm { jmp eax } \
    }

// Index assignments for each function
enum FuncIndex {
    FI_timeGetTime = 0,
    FI_timeBeginPeriod,
    FI_timeEndPeriod,
    FI_timeGetDevCaps,
    FI_timeSetEvent,
    FI_timeKillEvent,
    FI_waveOutGetNumDevs,
    FI_waveOutGetDevCapsA,
    FI_waveOutGetDevCapsW,
    FI_waveOutGetVolume,
    FI_waveOutSetVolume,
    FI_waveOutGetErrorTextA,
    FI_waveOutGetErrorTextW,
    FI_waveOutOpen,
    FI_waveOutClose,
    FI_waveOutPrepareHeader,
    FI_waveOutUnprepareHeader,
    FI_waveOutWrite,
    FI_waveOutPause,
    FI_waveOutRestart,
    FI_waveOutReset,
    FI_waveOutGetPosition,
    FI_waveOutMessage,
    FI_waveInGetNumDevs,
    FI_waveInGetDevCapsA,
    FI_waveInGetDevCapsW,
    FI_waveInOpen,
    FI_waveInClose,
    FI_waveInPrepareHeader,
    FI_waveInUnprepareHeader,
    FI_waveInAddBuffer,
    FI_waveInStart,
    FI_waveInStop,
    FI_waveInReset,
    FI_waveInGetPosition,
    FI_waveInMessage,
    FI_waveInGetErrorTextA,
    FI_waveInGetErrorTextW,
    FI_midiOutGetNumDevs,
    FI_midiOutGetDevCapsA,
    FI_midiOutGetDevCapsW,
    FI_midiOutOpen,
    FI_midiOutClose,
    FI_midiOutPrepareHeader,
    FI_midiOutUnprepareHeader,
    FI_midiOutShortMsg,
    FI_midiOutLongMsg,
    FI_midiOutReset,
    FI_midiOutGetVolume,
    FI_midiOutSetVolume,
    FI_midiOutGetErrorTextA,
    FI_midiOutGetErrorTextW,
    FI_midiOutMessage,
    FI_midiInGetNumDevs,
    FI_midiInGetDevCapsA,
    FI_midiInGetDevCapsW,
    FI_midiInOpen,
    FI_midiInClose,
    FI_midiInPrepareHeader,
    FI_midiInUnprepareHeader,
    FI_midiInAddBuffer,
    FI_midiInStart,
    FI_midiInStop,
    FI_midiInReset,
    FI_midiInGetErrorTextA,
    FI_midiInGetErrorTextW,
    FI_midiInMessage,
    FI_joyGetNumDevs,
    FI_joyGetDevCapsA,
    FI_joyGetDevCapsW,
    FI_joyGetPos,
    FI_joyGetPosEx,
    FI_joyGetThreshold,
    FI_joySetThreshold,
    FI_joySetCapture,
    FI_joyReleaseCapture,
    FI_mciSendCommandA,
    FI_mciSendCommandW,
    FI_mciSendStringA,
    FI_mciSendStringW,
    FI_mciGetErrorStringA,
    FI_mciGetErrorStringW,
    FI_mciGetDeviceIDA,
    FI_mciGetDeviceIDW,
    FI_PlaySoundA,
    FI_PlaySoundW,
    FI_sndPlaySoundA,
    FI_sndPlaySoundW,
    FI_mmioOpenA,
    FI_mmioOpenW,
    FI_mmioClose,
    FI_mmioRead,
    FI_mmioWrite,
    FI_mmioSeek,
    FI_mmioAscend,
    FI_mmioDescend,
    FI_mmioCreateChunk,
    FI_auxGetNumDevs,
    FI_auxGetDevCapsA,
    FI_auxGetDevCapsW,
    FI_auxGetVolume,
    FI_auxSetVolume,
    FI_mixerGetNumDevs,
    FI_mixerOpen,
    FI_mixerClose,
    FI_mixerGetDevCapsA,
    FI_mixerGetDevCapsW,
    FI_mixerGetLineInfoA,
    FI_mixerGetLineInfoW,
    FI_mixerGetLineControlsA,
    FI_mixerGetLineControlsW,
    FI_mixerGetControlDetailsA,
    FI_mixerGetControlDetailsW,
    FI_mixerSetControlDetails,
    FI_mixerMessage,
    FI_mixerGetID,
    FI_midiStreamOpen,
    FI_midiStreamClose,
    FI_midiStreamOut,
    FI_midiStreamPause,
    FI_midiStreamRestart,
    FI_midiStreamStop,
    FI_midiStreamPosition,
    FI_midiStreamProperty,
    FI_midiConnect,
    FI_midiDisconnect,
    FI_COUNT
};

// Define all trampolines
PROXY_TRAMPOLINE(timeGetTime, FI_timeGetTime)
PROXY_TRAMPOLINE(timeBeginPeriod, FI_timeBeginPeriod)
PROXY_TRAMPOLINE(timeEndPeriod, FI_timeEndPeriod)
PROXY_TRAMPOLINE(timeGetDevCaps, FI_timeGetDevCaps)
PROXY_TRAMPOLINE(timeSetEvent, FI_timeSetEvent)
PROXY_TRAMPOLINE(timeKillEvent, FI_timeKillEvent)
PROXY_TRAMPOLINE(waveOutGetNumDevs, FI_waveOutGetNumDevs)
PROXY_TRAMPOLINE(waveOutGetDevCapsA, FI_waveOutGetDevCapsA)
PROXY_TRAMPOLINE(waveOutGetDevCapsW, FI_waveOutGetDevCapsW)
PROXY_TRAMPOLINE(waveOutGetVolume, FI_waveOutGetVolume)
PROXY_TRAMPOLINE(waveOutSetVolume, FI_waveOutSetVolume)
PROXY_TRAMPOLINE(waveOutGetErrorTextA, FI_waveOutGetErrorTextA)
PROXY_TRAMPOLINE(waveOutGetErrorTextW, FI_waveOutGetErrorTextW)
PROXY_TRAMPOLINE(waveOutOpen, FI_waveOutOpen)
PROXY_TRAMPOLINE(waveOutClose, FI_waveOutClose)
PROXY_TRAMPOLINE(waveOutPrepareHeader, FI_waveOutPrepareHeader)
PROXY_TRAMPOLINE(waveOutUnprepareHeader, FI_waveOutUnprepareHeader)
PROXY_TRAMPOLINE(waveOutWrite, FI_waveOutWrite)
PROXY_TRAMPOLINE(waveOutPause, FI_waveOutPause)
PROXY_TRAMPOLINE(waveOutRestart, FI_waveOutRestart)
PROXY_TRAMPOLINE(waveOutReset, FI_waveOutReset)
PROXY_TRAMPOLINE(waveOutGetPosition, FI_waveOutGetPosition)
PROXY_TRAMPOLINE(waveOutMessage, FI_waveOutMessage)
PROXY_TRAMPOLINE(waveInGetNumDevs, FI_waveInGetNumDevs)
PROXY_TRAMPOLINE(waveInGetDevCapsA, FI_waveInGetDevCapsA)
PROXY_TRAMPOLINE(waveInGetDevCapsW, FI_waveInGetDevCapsW)
PROXY_TRAMPOLINE(waveInOpen, FI_waveInOpen)
PROXY_TRAMPOLINE(waveInClose, FI_waveInClose)
PROXY_TRAMPOLINE(waveInPrepareHeader, FI_waveInPrepareHeader)
PROXY_TRAMPOLINE(waveInUnprepareHeader, FI_waveInUnprepareHeader)
PROXY_TRAMPOLINE(waveInAddBuffer, FI_waveInAddBuffer)
PROXY_TRAMPOLINE(waveInStart, FI_waveInStart)
PROXY_TRAMPOLINE(waveInStop, FI_waveInStop)
PROXY_TRAMPOLINE(waveInReset, FI_waveInReset)
PROXY_TRAMPOLINE(waveInGetPosition, FI_waveInGetPosition)
PROXY_TRAMPOLINE(waveInMessage, FI_waveInMessage)
PROXY_TRAMPOLINE(waveInGetErrorTextA, FI_waveInGetErrorTextA)
PROXY_TRAMPOLINE(waveInGetErrorTextW, FI_waveInGetErrorTextW)
PROXY_TRAMPOLINE(midiOutGetNumDevs, FI_midiOutGetNumDevs)
PROXY_TRAMPOLINE(midiOutGetDevCapsA, FI_midiOutGetDevCapsA)
PROXY_TRAMPOLINE(midiOutGetDevCapsW, FI_midiOutGetDevCapsW)
PROXY_TRAMPOLINE(midiOutOpen, FI_midiOutOpen)
PROXY_TRAMPOLINE(midiOutClose, FI_midiOutClose)
PROXY_TRAMPOLINE(midiOutPrepareHeader, FI_midiOutPrepareHeader)
PROXY_TRAMPOLINE(midiOutUnprepareHeader, FI_midiOutUnprepareHeader)
PROXY_TRAMPOLINE(midiOutShortMsg, FI_midiOutShortMsg)
PROXY_TRAMPOLINE(midiOutLongMsg, FI_midiOutLongMsg)
PROXY_TRAMPOLINE(midiOutReset, FI_midiOutReset)
PROXY_TRAMPOLINE(midiOutGetVolume, FI_midiOutGetVolume)
PROXY_TRAMPOLINE(midiOutSetVolume, FI_midiOutSetVolume)
PROXY_TRAMPOLINE(midiOutGetErrorTextA, FI_midiOutGetErrorTextA)
PROXY_TRAMPOLINE(midiOutGetErrorTextW, FI_midiOutGetErrorTextW)
PROXY_TRAMPOLINE(midiOutMessage, FI_midiOutMessage)
PROXY_TRAMPOLINE(midiInGetNumDevs, FI_midiInGetNumDevs)
PROXY_TRAMPOLINE(midiInGetDevCapsA, FI_midiInGetDevCapsA)
PROXY_TRAMPOLINE(midiInGetDevCapsW, FI_midiInGetDevCapsW)
PROXY_TRAMPOLINE(midiInOpen, FI_midiInOpen)
PROXY_TRAMPOLINE(midiInClose, FI_midiInClose)
PROXY_TRAMPOLINE(midiInPrepareHeader, FI_midiInPrepareHeader)
PROXY_TRAMPOLINE(midiInUnprepareHeader, FI_midiInUnprepareHeader)
PROXY_TRAMPOLINE(midiInAddBuffer, FI_midiInAddBuffer)
PROXY_TRAMPOLINE(midiInStart, FI_midiInStart)
PROXY_TRAMPOLINE(midiInStop, FI_midiInStop)
PROXY_TRAMPOLINE(midiInReset, FI_midiInReset)
PROXY_TRAMPOLINE(midiInGetErrorTextA, FI_midiInGetErrorTextA)
PROXY_TRAMPOLINE(midiInGetErrorTextW, FI_midiInGetErrorTextW)
PROXY_TRAMPOLINE(midiInMessage, FI_midiInMessage)
PROXY_TRAMPOLINE(joyGetNumDevs, FI_joyGetNumDevs)
PROXY_TRAMPOLINE(joyGetDevCapsA, FI_joyGetDevCapsA)
PROXY_TRAMPOLINE(joyGetDevCapsW, FI_joyGetDevCapsW)
PROXY_TRAMPOLINE(joyGetPos, FI_joyGetPos)
PROXY_TRAMPOLINE(joyGetPosEx, FI_joyGetPosEx)
PROXY_TRAMPOLINE(joyGetThreshold, FI_joyGetThreshold)
PROXY_TRAMPOLINE(joySetThreshold, FI_joySetThreshold)
PROXY_TRAMPOLINE(joySetCapture, FI_joySetCapture)
PROXY_TRAMPOLINE(joyReleaseCapture, FI_joyReleaseCapture)
PROXY_TRAMPOLINE(mciSendCommandA, FI_mciSendCommandA)
PROXY_TRAMPOLINE(mciSendCommandW, FI_mciSendCommandW)
PROXY_TRAMPOLINE(mciSendStringA, FI_mciSendStringA)
PROXY_TRAMPOLINE(mciSendStringW, FI_mciSendStringW)
PROXY_TRAMPOLINE(mciGetErrorStringA, FI_mciGetErrorStringA)
PROXY_TRAMPOLINE(mciGetErrorStringW, FI_mciGetErrorStringW)
PROXY_TRAMPOLINE(mciGetDeviceIDA, FI_mciGetDeviceIDA)
PROXY_TRAMPOLINE(mciGetDeviceIDW, FI_mciGetDeviceIDW)
PROXY_TRAMPOLINE(PlaySoundA, FI_PlaySoundA)
PROXY_TRAMPOLINE(PlaySoundW, FI_PlaySoundW)
PROXY_TRAMPOLINE(sndPlaySoundA, FI_sndPlaySoundA)
PROXY_TRAMPOLINE(sndPlaySoundW, FI_sndPlaySoundW)
PROXY_TRAMPOLINE(mmioOpenA, FI_mmioOpenA)
PROXY_TRAMPOLINE(mmioOpenW, FI_mmioOpenW)
PROXY_TRAMPOLINE(mmioClose, FI_mmioClose)
PROXY_TRAMPOLINE(mmioRead, FI_mmioRead)
PROXY_TRAMPOLINE(mmioWrite, FI_mmioWrite)
PROXY_TRAMPOLINE(mmioSeek, FI_mmioSeek)
PROXY_TRAMPOLINE(mmioAscend, FI_mmioAscend)
PROXY_TRAMPOLINE(mmioDescend, FI_mmioDescend)
PROXY_TRAMPOLINE(mmioCreateChunk, FI_mmioCreateChunk)
PROXY_TRAMPOLINE(auxGetNumDevs, FI_auxGetNumDevs)
PROXY_TRAMPOLINE(auxGetDevCapsA, FI_auxGetDevCapsA)
PROXY_TRAMPOLINE(auxGetDevCapsW, FI_auxGetDevCapsW)
PROXY_TRAMPOLINE(auxGetVolume, FI_auxGetVolume)
PROXY_TRAMPOLINE(auxSetVolume, FI_auxSetVolume)
PROXY_TRAMPOLINE(mixerGetNumDevs, FI_mixerGetNumDevs)
PROXY_TRAMPOLINE(mixerOpen, FI_mixerOpen)
PROXY_TRAMPOLINE(mixerClose, FI_mixerClose)
PROXY_TRAMPOLINE(mixerGetDevCapsA, FI_mixerGetDevCapsA)
PROXY_TRAMPOLINE(mixerGetDevCapsW, FI_mixerGetDevCapsW)
PROXY_TRAMPOLINE(mixerGetLineInfoA, FI_mixerGetLineInfoA)
PROXY_TRAMPOLINE(mixerGetLineInfoW, FI_mixerGetLineInfoW)
PROXY_TRAMPOLINE(mixerGetLineControlsA, FI_mixerGetLineControlsA)
PROXY_TRAMPOLINE(mixerGetLineControlsW, FI_mixerGetLineControlsW)
PROXY_TRAMPOLINE(mixerGetControlDetailsA, FI_mixerGetControlDetailsA)
PROXY_TRAMPOLINE(mixerGetControlDetailsW, FI_mixerGetControlDetailsW)
PROXY_TRAMPOLINE(mixerSetControlDetails, FI_mixerSetControlDetails)
PROXY_TRAMPOLINE(mixerMessage, FI_mixerMessage)
PROXY_TRAMPOLINE(mixerGetID, FI_mixerGetID)
PROXY_TRAMPOLINE(midiStreamOpen, FI_midiStreamOpen)
PROXY_TRAMPOLINE(midiStreamClose, FI_midiStreamClose)
PROXY_TRAMPOLINE(midiStreamOut, FI_midiStreamOut)
PROXY_TRAMPOLINE(midiStreamPause, FI_midiStreamPause)
PROXY_TRAMPOLINE(midiStreamRestart, FI_midiStreamRestart)
PROXY_TRAMPOLINE(midiStreamStop, FI_midiStreamStop)
PROXY_TRAMPOLINE(midiStreamPosition, FI_midiStreamPosition)
PROXY_TRAMPOLINE(midiStreamProperty, FI_midiStreamProperty)
PROXY_TRAMPOLINE(midiConnect, FI_midiConnect)
PROXY_TRAMPOLINE(midiDisconnect, FI_midiDisconnect)

// ─── Initialization ────────────────────────────────────────────────────

static const char* g_FuncNames[] = {
    "timeGetTime", "timeBeginPeriod", "timeEndPeriod", "timeGetDevCaps",
    "timeSetEvent", "timeKillEvent",
    "waveOutGetNumDevs", "waveOutGetDevCapsA", "waveOutGetDevCapsW",
    "waveOutGetVolume", "waveOutSetVolume",
    "waveOutGetErrorTextA", "waveOutGetErrorTextW",
    "waveOutOpen", "waveOutClose", "waveOutPrepareHeader", "waveOutUnprepareHeader",
    "waveOutWrite", "waveOutPause", "waveOutRestart", "waveOutReset",
    "waveOutGetPosition", "waveOutMessage",
    "waveInGetNumDevs", "waveInGetDevCapsA", "waveInGetDevCapsW",
    "waveInOpen", "waveInClose", "waveInPrepareHeader", "waveInUnprepareHeader",
    "waveInAddBuffer", "waveInStart", "waveInStop", "waveInReset",
    "waveInGetPosition", "waveInMessage", "waveInGetErrorTextA", "waveInGetErrorTextW",
    "midiOutGetNumDevs", "midiOutGetDevCapsA", "midiOutGetDevCapsW",
    "midiOutOpen", "midiOutClose", "midiOutPrepareHeader", "midiOutUnprepareHeader",
    "midiOutShortMsg", "midiOutLongMsg", "midiOutReset",
    "midiOutGetVolume", "midiOutSetVolume",
    "midiOutGetErrorTextA", "midiOutGetErrorTextW", "midiOutMessage",
    "midiInGetNumDevs", "midiInGetDevCapsA", "midiInGetDevCapsW",
    "midiInOpen", "midiInClose", "midiInPrepareHeader", "midiInUnprepareHeader",
    "midiInAddBuffer", "midiInStart", "midiInStop", "midiInReset",
    "midiInGetErrorTextA", "midiInGetErrorTextW", "midiInMessage",
    "joyGetNumDevs", "joyGetDevCapsA", "joyGetDevCapsW",
    "joyGetPos", "joyGetPosEx", "joyGetThreshold", "joySetThreshold",
    "joySetCapture", "joyReleaseCapture",
    "mciSendCommandA", "mciSendCommandW", "mciSendStringA", "mciSendStringW",
    "mciGetErrorStringA", "mciGetErrorStringW", "mciGetDeviceIDA", "mciGetDeviceIDW",
    "PlaySoundA", "PlaySoundW", "sndPlaySoundA", "sndPlaySoundW",
    "mmioOpenA", "mmioOpenW", "mmioClose", "mmioRead", "mmioWrite",
    "mmioSeek", "mmioAscend", "mmioDescend", "mmioCreateChunk",
    "auxGetNumDevs", "auxGetDevCapsA", "auxGetDevCapsW", "auxGetVolume", "auxSetVolume",
    "mixerGetNumDevs", "mixerOpen", "mixerClose",
    "mixerGetDevCapsA", "mixerGetDevCapsW",
    "mixerGetLineInfoA", "mixerGetLineInfoW",
    "mixerGetLineControlsA", "mixerGetLineControlsW",
    "mixerGetControlDetailsA", "mixerGetControlDetailsW",
    "mixerSetControlDetails", "mixerMessage",
    "mixerGetID",
    "midiStreamOpen", "midiStreamClose", "midiStreamOut",
    "midiStreamPause", "midiStreamRestart", "midiStreamStop",
    "midiStreamPosition", "midiStreamProperty",
    "midiConnect", "midiDisconnect"
};

static bool LoadRealWinMM()
{
    // Load the real winmm.dll from System32 (use SysWOW64 on 64-bit OS for 32-bit process)
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string realPath = std::string(sysDir) + "\\winmm.dll";

    g_RealWinMM = LoadLibraryA(realPath.c_str());
    if (!g_RealWinMM) return false;

    // Resolve all forwarded functions
    for (int i = 0; i < FI_COUNT; i++) {
        fp[i] = GetProcAddress(g_RealWinMM, g_FuncNames[i]);
    }

    return true;
}

static DWORD WINAPI LoadBS1SDKThread(LPVOID param)
{
    // Wait for the game to actually start — Steam DRM must complete first.
    // We detect this by waiting for the game window to appear, which only
    // happens after Steam validates the exe and unpacks it.
    for (int i = 0; i < 60; i++) { // up to 30 seconds
        Sleep(500);
        // Look for the game window (BioShock uses D3D window)
        HWND hwnd = FindWindowA("LaunchUnrealUWindowsClient", nullptr);
        if (!hwnd) hwnd = FindWindowA("UnrealUWindowsClient", nullptr);
        if (!hwnd) hwnd = FindWindowA(nullptr, "BioShock");
        if (hwnd) {
            Sleep(1000); // extra 1s after window appears for stability
            break;
        }
    }

    // Load BS1SDK.dll from the same directory as the game exe
    char myPath[MAX_PATH];
    GetModuleFileNameA(nullptr, myPath, MAX_PATH);

    // Get directory
    std::string dir(myPath);
    size_t lastSlash = dir.find_last_of("\\/");
    if (lastSlash != std::string::npos) dir = dir.substr(0, lastSlash);

    std::string sdkPath = dir + "\\BS1SDK.dll";
    g_BS1SDK = LoadLibraryA(sdkPath.c_str());

    if (g_BS1SDK) {
        OutputDebugStringA("[BS1SDK Loader] BS1SDK.dll loaded successfully!\n");
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "[BS1SDK Loader] WARNING: Could not load BS1SDK.dll from: %s (error %lu)\n",
                 sdkPath.c_str(), GetLastError());
        OutputDebugStringA(msg);
    }
    return 0;
}

// ─── DllMain ───────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LoadRealWinMM();
        // Defer SDK loading to background thread — avoids interfering with
        // Steam DRM check and other early initialization
        CreateThread(nullptr, 0, LoadBS1SDKThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (g_BS1SDK) FreeLibrary(g_BS1SDK);
        if (g_RealWinMM) FreeLibrary(g_RealWinMM);
        break;
    }
    return TRUE;
}
