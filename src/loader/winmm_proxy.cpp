/// winmm.dll Proxy Loader for BS1SDK
///
/// This DLL masquerades as winmm.dll. Windows' DLL search order loads DLLs
/// from the application directory first, so placing this in the game folder
/// makes it load automatically — no injector needed.
///
/// On load, it:
///   1. Loads the real winmm.dll from System32
///   2. Forwards all winmm exports to the real DLL
///   3. Loads BS1SDK.dll from the same directory
///
/// End users just drop winmm.dll + BS1SDK.dll into the game folder.

#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <cstdio>
#include <string>

// ─── Real winmm.dll handle + forwarded function pointers ───────────────

static HMODULE g_RealWinMM = nullptr;
static HMODULE g_BS1SDK = nullptr;

// We define function pointer types for the most commonly used winmm exports.
// BioShock (via fmodex.dll) uses timeGetTime, timeBeginPeriod, timeEndPeriod,
// and potentially waveOut* functions. We forward ALL common exports.

#define PROXY_FUNC(name) \
    static decltype(&name) real_##name = nullptr;

// Load-time resolution: we populate these in DllMain
typedef DWORD (WINAPI *fn_timeGetTime)(void);
typedef MMRESULT (WINAPI *fn_timeBeginPeriod)(UINT);
typedef MMRESULT (WINAPI *fn_timeEndPeriod)(UINT);
typedef MMRESULT (WINAPI *fn_timeGetDevCaps)(LPTIMECAPS, UINT);
typedef MMRESULT (WINAPI *fn_timeSetEvent)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef MMRESULT (WINAPI *fn_timeKillEvent)(UINT);
typedef MMRESULT (WINAPI *fn_waveOutGetNumDevs)(void);
typedef MMRESULT (WINAPI *fn_joyGetNumDevs)(void);
typedef MMRESULT (WINAPI *fn_mciSendCommandW)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
typedef MMRESULT (WINAPI *fn_mciSendStringW)(LPCWSTR, LPWSTR, UINT, HWND);
typedef MMRESULT (WINAPI *fn_PlaySoundA)(LPCSTR, HMODULE, DWORD);
typedef MMRESULT (WINAPI *fn_PlaySoundW)(LPCWSTR, HMODULE, DWORD);

static fn_timeGetTime       real_timeGetTime       = nullptr;
static fn_timeBeginPeriod   real_timeBeginPeriod   = nullptr;
static fn_timeEndPeriod     real_timeEndPeriod     = nullptr;
static fn_timeGetDevCaps    real_timeGetDevCaps    = nullptr;
static fn_timeSetEvent      real_timeSetEvent      = nullptr;
static fn_timeKillEvent     real_timeKillEvent     = nullptr;
static fn_waveOutGetNumDevs real_waveOutGetNumDevs = nullptr;
static fn_joyGetNumDevs     real_joyGetNumDevs     = nullptr;
static fn_mciSendCommandW   real_mciSendCommandW   = nullptr;
static fn_mciSendStringW    real_mciSendStringW    = nullptr;
static fn_PlaySoundA        real_PlaySoundA        = nullptr;
static fn_PlaySoundW        real_PlaySoundW        = nullptr;

// ─── Forwarded Exports ─────────────────────────────────────────────────

extern "C" {

__declspec(dllexport) DWORD WINAPI proxy_timeGetTime(void) {
    return real_timeGetTime ? real_timeGetTime() : GetTickCount();
}

__declspec(dllexport) MMRESULT WINAPI proxy_timeBeginPeriod(UINT uPeriod) {
    return real_timeBeginPeriod ? real_timeBeginPeriod(uPeriod) : TIMERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI proxy_timeEndPeriod(UINT uPeriod) {
    return real_timeEndPeriod ? real_timeEndPeriod(uPeriod) : TIMERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI proxy_timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc) {
    return real_timeGetDevCaps ? real_timeGetDevCaps(ptc, cbtc) : MMSYSERR_ERROR;
}

__declspec(dllexport) MMRESULT WINAPI proxy_timeSetEvent(UINT uDelay, UINT uResolution,
                                                          LPTIMECALLBACK fptc, DWORD_PTR dwUser, UINT fuEvent) {
    return real_timeSetEvent ? real_timeSetEvent(uDelay, uResolution, fptc, dwUser, fuEvent) : 0;
}

__declspec(dllexport) MMRESULT WINAPI proxy_timeKillEvent(UINT uTimerID) {
    return real_timeKillEvent ? real_timeKillEvent(uTimerID) : MMSYSERR_ERROR;
}

__declspec(dllexport) UINT WINAPI proxy_waveOutGetNumDevs(void) {
    return real_waveOutGetNumDevs ? real_waveOutGetNumDevs() : 0;
}

__declspec(dllexport) UINT WINAPI proxy_joyGetNumDevs(void) {
    return real_joyGetNumDevs ? real_joyGetNumDevs() : 0;
}

__declspec(dllexport) MCIERROR WINAPI proxy_mciSendCommandW(MCIDEVICEID id, UINT msg, DWORD_PTR p1, DWORD_PTR p2) {
    return real_mciSendCommandW ? real_mciSendCommandW(id, msg, p1, p2) : MCIERR_DRIVER;
}

__declspec(dllexport) MCIERROR WINAPI proxy_mciSendStringW(LPCWSTR cmd, LPWSTR ret, UINT sz, HWND cb) {
    return real_mciSendStringW ? real_mciSendStringW(cmd, ret, sz, cb) : MCIERR_DRIVER;
}

__declspec(dllexport) BOOL WINAPI proxy_PlaySoundA(LPCSTR pszSound, HMODULE hmod, DWORD fdwSound) {
    return real_PlaySoundA ? real_PlaySoundA(pszSound, hmod, fdwSound) : FALSE;
}

__declspec(dllexport) BOOL WINAPI proxy_PlaySoundW(LPCWSTR pszSound, HMODULE hmod, DWORD fdwSound) {
    return real_PlaySoundW ? real_PlaySoundW(pszSound, hmod, fdwSound) : FALSE;
}

} // extern "C"

// ─── Initialization ────────────────────────────────────────────────────

static bool LoadRealWinMM()
{
    // Load the real winmm.dll from System32
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string realPath = std::string(sysDir) + "\\winmm.dll";

    g_RealWinMM = LoadLibraryA(realPath.c_str());
    if (!g_RealWinMM) return false;

    // Resolve all forwarded functions
    real_timeGetTime       = (fn_timeGetTime)GetProcAddress(g_RealWinMM, "timeGetTime");
    real_timeBeginPeriod   = (fn_timeBeginPeriod)GetProcAddress(g_RealWinMM, "timeBeginPeriod");
    real_timeEndPeriod     = (fn_timeEndPeriod)GetProcAddress(g_RealWinMM, "timeEndPeriod");
    real_timeGetDevCaps    = (fn_timeGetDevCaps)GetProcAddress(g_RealWinMM, "timeGetDevCaps");
    real_timeSetEvent      = (fn_timeSetEvent)GetProcAddress(g_RealWinMM, "timeSetEvent");
    real_timeKillEvent     = (fn_timeKillEvent)GetProcAddress(g_RealWinMM, "timeKillEvent");
    real_waveOutGetNumDevs = (fn_waveOutGetNumDevs)GetProcAddress(g_RealWinMM, "waveOutGetNumDevs");
    real_joyGetNumDevs     = (fn_joyGetNumDevs)GetProcAddress(g_RealWinMM, "joyGetNumDevs");
    real_mciSendCommandW   = (fn_mciSendCommandW)GetProcAddress(g_RealWinMM, "mciSendCommandW");
    real_mciSendStringW    = (fn_mciSendStringW)GetProcAddress(g_RealWinMM, "mciSendStringW");
    real_PlaySoundA        = (fn_PlaySoundA)GetProcAddress(g_RealWinMM, "PlaySoundA");
    real_PlaySoundW        = (fn_PlaySoundW)GetProcAddress(g_RealWinMM, "PlaySoundW");

    return true;
}

static void LoadBS1SDK()
{
    // Load BS1SDK.dll from the same directory as this proxy
    char myPath[MAX_PATH];
    GetModuleFileNameA(nullptr, myPath, MAX_PATH);

    // Get directory
    std::string dir(myPath);
    size_t lastSlash = dir.find_last_of("\\/");
    if (lastSlash != std::string::npos) dir = dir.substr(0, lastSlash);

    std::string sdkPath = dir + "\\BS1SDK.dll";
    g_BS1SDK = LoadLibraryA(sdkPath.c_str());

    if (g_BS1SDK) {
        // BS1SDK's DllMain will fire DLL_PROCESS_ATTACH and start its thread
        OutputDebugStringA("[BS1SDK Loader] BS1SDK.dll loaded successfully!\n");
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "[BS1SDK Loader] WARNING: Could not load BS1SDK.dll from: %s (error %lu)\n",
                 sdkPath.c_str(), GetLastError());
        OutputDebugStringA(msg);
    }
}

// ─── DllMain ───────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LoadRealWinMM();
        LoadBS1SDK();
        break;

    case DLL_PROCESS_DETACH:
        if (g_BS1SDK) FreeLibrary(g_BS1SDK);
        if (g_RealWinMM) FreeLibrary(g_RealWinMM);
        break;
    }
    return TRUE;
}
