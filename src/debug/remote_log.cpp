#include "remote_log.h"
#include "crash_handler.h"
#include "../core/log.h"
#include "../network/net_common.h"
#include "../network/net_manager.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>

namespace bs1sdk {

// ═══════════════════════════════════════════════════════════════════════════
//  Remote Log — Client Side: intercept local logs and send to host
// ═══════════════════════════════════════════════════════════════════════════

static std::atomic<bool> s_ClientSending{false};
static DWORD s_InitTick = 0;

// Ring buffer of recent log messages for batch sending
static constexpr int LOG_RING_SIZE = 128;
static struct LogRingEntry {
    uint8_t  level;
    uint32_t timestamp;
    char     message[256];
} s_LogRing[LOG_RING_SIZE];
static std::atomic<int> s_LogRingHead{0};
static std::atomic<int> s_LogRingTail{0};
static std::mutex s_RingSendMutex;

// Thread-local recursion guard
static thread_local bool t_InLogCallback = false;

// Log callback installed into Log::SetCallback
static void RemoteLogCallback(uint8_t level, const char* msg)
{
    if (t_InLogCallback) return;
    if (!s_ClientSending) return;

    t_InLogCallback = true;

    int head = s_LogRingHead.load(std::memory_order_relaxed);
    int next = (head + 1) % LOG_RING_SIZE;
    if (next != s_LogRingTail.load(std::memory_order_acquire)) {
        s_LogRing[head].level = level;
        s_LogRing[head].timestamp = GetTickCount() - s_InitTick;
        strncpy(s_LogRing[head].message, msg, 255);
        s_LogRing[head].message[255] = '\0';
        s_LogRingHead.store(next, std::memory_order_release);
    }

    t_InLogCallback = false;
}

/// Drain ring buffer — call from CoopTick or NetTick on client side.
/// Sends up to 8 log messages per frame to avoid network flood.
void DrainRemoteLogRing()
{
    if (!s_ClientSending) return;
    if (!IsNetConnected()) return;

    std::lock_guard<std::mutex> lock(s_RingSendMutex);
    int tail = s_LogRingTail.load(std::memory_order_relaxed);
    int sent = 0;
    while (tail != s_LogRingHead.load(std::memory_order_acquire) && sent < 8) {
        RemoteLogData pkt{};
        pkt.level = s_LogRing[tail].level;
        pkt.timestamp = s_LogRing[tail].timestamp;
        strncpy(pkt.message, s_LogRing[tail].message, sizeof(pkt.message) - 1);
        pkt.message[sizeof(pkt.message) - 1] = '\0';

        uint16_t msgLen = (uint16_t)strnlen(pkt.message, sizeof(pkt.message));
        uint16_t pktSize = (uint16_t)(8 + msgLen + 1);
        NetSendRawPacket(PacketType::RemoteLog, &pkt, pktSize);

        tail = (tail + 1) % LOG_RING_SIZE;
        sent++;
    }
    s_LogRingTail.store(tail, std::memory_order_release);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Remote Log — Host Side: receive and write client logs to file
// ═══════════════════════════════════════════════════════════════════════════

static FILE* s_RemoteLogFile = nullptr;
static FILE* s_RemoteLogDev  = nullptr; // mirror to dev path
static char  s_RemoteLogPath[MAX_PATH] = {};
static char  s_RemoteLogDevPath[MAX_PATH] = {};
static std::mutex s_RemoteFileMutex;
static uint32_t s_RemoteLinesReceived = 0;

static const char* LogLevelStr(uint8_t level)
{
    switch (level) {
        case 0: return "TRACE";
        case 1: return "DEBUG";
        case 2: return "INFO ";
        case 3: return "WARN ";
        case 4: return "ERROR";
        case 5: return "FATAL";
        default: return "?????";
    }
}

void OnRemoteLogReceived(const void* data, uint16_t size)
{
    if (size < 9) return;
    auto* pkt = reinterpret_cast<const RemoteLogData*>(data);

    std::lock_guard<std::mutex> lock(s_RemoteFileMutex);

    uint32_t ms = pkt->timestamp;
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    uint32_t hr = min / 60;

    // Ensure message is null-terminated within bounds
    int maxMsgLen = (int)(size - 8);
    if (maxMsgLen > 1023) maxMsgLen = 1023;

    char line[2048];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [%s] %.*s\n",
             hr % 24, min % 60, sec % 60, ms % 1000,
             LogLevelStr(pkt->level),
             maxMsgLen, pkt->message);

    if (s_RemoteLogFile) fputs(line, s_RemoteLogFile);
    if (s_RemoteLogDev)  fputs(line, s_RemoteLogDev);

    s_RemoteLinesReceived++;

    if (s_RemoteLinesReceived % 10 == 0) {
        if (s_RemoteLogFile) fflush(s_RemoteLogFile);
        if (s_RemoteLogDev)  fflush(s_RemoteLogDev);
    }
}

void FlushRemoteLog()
{
    std::lock_guard<std::mutex> lock(s_RemoteFileMutex);
    if (s_RemoteLogFile) fflush(s_RemoteLogFile);
    if (s_RemoteLogDev)  fflush(s_RemoteLogDev);
}

const char* GetRemoteLogPath()
{
    return s_RemoteLogPath;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════════════

void InitRemoteLog()
{
    s_InitTick = GetTickCount();

    // ── Host side: open file to receive client logs ──
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* lastSlash = strrchr(exeDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    snprintf(s_RemoteLogPath, sizeof(s_RemoteLogPath),
             "%sBS1SDK_dumps\\client_log.txt", exeDir);

    // Dev path mirror
    snprintf(s_RemoteLogDevPath, sizeof(s_RemoteLogDevPath),
             "Z:\\Bioshock1SDK\\debug_dumps\\client_log.txt");

    {
        std::lock_guard<std::mutex> lock(s_RemoteFileMutex);

        s_RemoteLogFile = fopen(s_RemoteLogPath, "w");
        s_RemoteLogDev  = fopen(s_RemoteLogDevPath, "w");

        auto writeHeader = [](FILE* f) {
            if (!f) return;
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f,
                "======================================================================\n"
                "  CLIENT LOG (received by host via RemoteLog relay)\n"
                "======================================================================\n"
                "Session: %04d-%02d-%02d %02d:%02d:%02d\n"
                "Format:  [client_uptime] [LEVEL] message\n"
                "----------------------------------------------------------------------\n\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            fflush(f);
        };
        writeHeader(s_RemoteLogFile);
        writeHeader(s_RemoteLogDev);
    }

    // ── Client side: install log callback for relay ──
    Log::SetCallback(RemoteLogCallback);
    s_ClientSending = true;

    LOG_INFO("[RemoteLog] Initialized");
    LOG_INFO("[RemoteLog]   Host log path:  {}", s_RemoteLogPath);
    LOG_INFO("[RemoteLog]   Dev mirror:     {}", s_RemoteLogDevPath);
}

void ShutdownRemoteLog()
{
    s_ClientSending = false;
    Log::SetCallback(nullptr);

    std::lock_guard<std::mutex> lock(s_RemoteFileMutex);
    auto closeFile = [](FILE*& f) {
        if (f) { fclose(f); f = nullptr; }
    };

    if (s_RemoteLogFile) {
        fprintf(s_RemoteLogFile, "\n=== Session ended (%u client log lines received) ===\n",
                s_RemoteLinesReceived);
    }
    if (s_RemoteLogDev) {
        fprintf(s_RemoteLogDev, "\n=== Session ended (%u client log lines received) ===\n",
                s_RemoteLinesReceived);
    }

    closeFile(s_RemoteLogFile);
    closeFile(s_RemoteLogDev);
}

} // namespace bs1sdk
