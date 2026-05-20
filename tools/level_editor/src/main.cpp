#include "app.h"
#include <cstdio>
#include <SDL.h>

#ifdef _WIN32
#include <Windows.h>
#endif

FILE* g_LogFile = nullptr;
void LogMsg(const char* msg) {
    if (!g_LogFile) g_LogFile = fopen("Z:\\Bioshock1SDK\\tools\\level_editor\\editor_log.txt", "a");
    if (g_LogFile) { fputs(msg, g_LogFile); fputc('\n', g_LogFile); fflush(g_LogFile); }
}

static int RunApp(const char* mapPath)
{
    App app;
    if (!app.Init(mapPath)) {
        LogMsg("[Main] Init failed");
        return 1;
    }
    app.Run();
    app.Shutdown();
    return 0;
}

int main(int argc, char* argv[])
{
    g_LogFile = fopen("Z:\\Bioshock1SDK\\tools\\level_editor\\editor_log.txt", "w");
    if (g_LogFile) { fputs("[Main] Starting BS1 Level Editor...\n", g_LogFile); fflush(g_LogFile); }

#ifdef _WIN32
    __try {
#endif
        return RunApp(argc > 1 ? argv[1] : nullptr);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[Main] CRASH: exception code 0x%08lX", GetExceptionCode());
        LogMsg(buf);
        return 1;
    }
#endif
    return 0;
}
