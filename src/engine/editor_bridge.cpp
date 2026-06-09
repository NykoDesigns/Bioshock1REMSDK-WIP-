// ═══════════════════════════════════════════════════════════════════════════
// Editor Bridge — TCP server for live editor↔game sync.
// Uses Winsock2 non-blocking sockets on the engine tick.
// SAFE: only reads positions and does direct memcpy writes (no PE calls,
// no Havok triggers, no bStatic corruption).
// ═══════════════════════════════════════════════════════════════════════════

#include "editor_bridge.h"
#include "world.h"
#include "uobject.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <vector>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace bs1sdk {

// ── State ────────────────────────────────────────────────────────────────
static SOCKET          g_ListenSocket = INVALID_SOCKET;
static std::vector<SOCKET> g_Clients;
static std::mutex      g_BridgeMutex;
static int             g_TickCallbackId = -1;
static bool            g_Running = false;
static BridgeStats     g_Stats;

// Actor name hash cache: hash → UObject*
static std::unordered_map<uint32_t, UObject*> g_ActorHashMap;
static double          g_LastActorScan = 0.0;

// ── Helpers ──────────────────────────────────────────────────────────────

static uint32_t HashName(const std::string& name) {
    // FNV-1a 32-bit
    uint32_t h = 2166136261u;
    for (char c : name) { h ^= (uint8_t)c; h *= 16777619u; }
    return h;
}

static void RebuildActorHashMap() {
    g_ActorHashMap.clear();
    ForEachActor([](UObject* actor, const std::string& className) {
        if (!actor) return;
        std::string name = actor->GetName();
        if (name.empty()) return;
        uint32_t h = HashName(name);
        g_ActorHashMap[h] = actor;
    });
}

static UObject* FindActorByHash(uint32_t hash) {
    auto it = g_ActorHashMap.find(hash);
    if (it != g_ActorHashMap.end()) return it->second;
    return nullptr;
}

// ── Send helper ──────────────────────────────────────────────────────────

static bool SendMessage(SOCKET sock, uint16_t type, const void* payload, uint16_t payloadLen) {
    BridgeHeader hdr;
    hdr.magic = BRIDGE_MAGIC;
    hdr.type = type;
    hdr.payloadLen = payloadLen;

    char buf[512];
    if (sizeof(BridgeHeader) + payloadLen > sizeof(buf)) return false;
    memcpy(buf, &hdr, sizeof(hdr));
    if (payloadLen > 0 && payload)
        memcpy(buf + sizeof(hdr), payload, payloadLen);

    int total = (int)(sizeof(hdr) + payloadLen);
    int sent = send(sock, buf, total, 0);
    if (sent == total) { g_Stats.messagesSent++; return true; }
    return false;
}

// ── Message handlers ─────────────────────────────────────────────────────

static void HandleMoveActor(const MoveActorPayload& p) {
    UObject* actor = FindActorByHash(p.nameHash);
    if (!actor) return;

    FVec3 pos = { p.x, p.y, p.z };
    SetActorPosition(actor, pos);
    g_Stats.actorMoves++;
}

static void HandleRotateActor(const RotateActorPayload& p) {
    UObject* actor = FindActorByHash(p.nameHash);
    if (!actor) return;

    FRot3 rot = { p.pitch, p.yaw, p.roll };
    SetActorRotation(actor, rot);
    g_Stats.actorMoves++;
}

static void HandleQueryPlayer(SOCKET client) {
    FVec3 pos;
    if (!GetPlayerPosition(pos)) return;

    UObject* pc = GetPlayerController();
    FRot3 rot = { 0, 0, 0 };
    if (pc) GetActorRotation(pc, rot);

    PlayerPosPayload p;
    p.x = pos.X; p.y = pos.Y; p.z = pos.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    SendMessage(client, MSG_PLAYER_POS, &p, sizeof(p));
}

static void HandleQueryActors(SOCKET client) {
    // Rebuild map and send all actor positions
    RebuildActorHashMap();

    struct ActorEntry {
        uint32_t nameHash;
        uint32_t classHash;
        float x, y, z;
    };

    std::vector<ActorEntry> entries;
    entries.reserve(g_ActorHashMap.size());

    for (auto& [hash, actor] : g_ActorHashMap) {
        FVec3 pos;
        if (!GetActorPosition(actor, pos)) continue;
        std::string cls = actor->GetObjClassName();
        entries.push_back({ hash, HashName(cls), pos.X, pos.Y, pos.Z });
    }

    // Send header: count
    int32_t count = (int32_t)entries.size();
    size_t totalSize = 4 + entries.size() * sizeof(ActorEntry);

    // For large payloads, chunk into multiple messages
    // Each message max 60000 bytes
    constexpr int MAX_ENTRIES_PER_MSG = 2500;
    int offset = 0;
    while (offset < count) {
        int batch = (count - offset);
        if (batch > MAX_ENTRIES_PER_MSG) batch = MAX_ENTRIES_PER_MSG;

        std::vector<uint8_t> buf(4 + batch * sizeof(ActorEntry));
        memcpy(buf.data(), &batch, 4);
        memcpy(buf.data() + 4, entries.data() + offset, batch * sizeof(ActorEntry));
        SendMessage(client, MSG_ACTOR_LIST, buf.data(), (uint16_t)buf.size());
        offset += batch;
    }
}

// ── Process one client's incoming data ───────────────────────────────────

static bool ProcessClientData(SOCKET client) {
    char buf[256];
    int received = recv(client, buf, sizeof(buf), 0);
    if (received <= 0) {
        if (received == 0) return false; // client disconnected
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return true; // no data yet
        return false; // real error
    }

    // Process all complete messages in the buffer
    int pos = 0;
    while (pos + (int)sizeof(BridgeHeader) <= received) {
        BridgeHeader hdr;
        memcpy(&hdr, buf + pos, sizeof(hdr));

        if (hdr.magic != BRIDGE_MAGIC) { pos++; continue; } // resync

        int msgEnd = pos + (int)sizeof(BridgeHeader) + hdr.payloadLen;
        if (msgEnd > received) break; // incomplete message

        const void* payload = buf + pos + sizeof(BridgeHeader);
        g_Stats.messagesReceived++;

        switch (hdr.type) {
            case MSG_MOVE_ACTOR:
                if (hdr.payloadLen >= sizeof(MoveActorPayload))
                    HandleMoveActor(*(const MoveActorPayload*)payload);
                break;
            case MSG_ROTATE_ACTOR:
                if (hdr.payloadLen >= sizeof(RotateActorPayload))
                    HandleRotateActor(*(const RotateActorPayload*)payload);
                break;
            case MSG_SELECT_ACTOR:
                // TODO: highlight selected actor in-game
                break;
            case MSG_QUERY_ACTORS:
                HandleQueryActors(client);
                break;
            case MSG_QUERY_PLAYER:
                HandleQueryPlayer(client);
                break;
            case MSG_PING:
                SendMessage(client, MSG_PONG, nullptr, 0);
                break;
            default:
                break;
        }
        pos = msgEnd;
    }
    return true;
}

// ── Tick handler ─────────────────────────────────────────────────────────

static void BridgeTick(float deltaTime) {
    if (!g_Running || g_ListenSocket == INVALID_SOCKET) return;

    std::lock_guard<std::mutex> lock(g_BridgeMutex);

    // Accept new connections
    SOCKET newClient = accept(g_ListenSocket, nullptr, nullptr);
    if (newClient != INVALID_SOCKET) {
        u_long nonBlocking = 1;
        ioctlsocket(newClient, FIONBIO, &nonBlocking);
        g_Clients.push_back(newClient);
        printf("[EditorBridge] Client connected (%d total)\n", (int)g_Clients.size());
    }

    // Periodically rebuild actor hash map (every 5 seconds)
    static float timeSinceRebuild = 0.0f;
    timeSinceRebuild += deltaTime;
    if (timeSinceRebuild > 5.0f) {
        RebuildActorHashMap();
        timeSinceRebuild = 0.0f;
    }

    // Process each client
    for (int i = (int)g_Clients.size() - 1; i >= 0; i--) {
        if (!ProcessClientData(g_Clients[i])) {
            closesocket(g_Clients[i]);
            g_Clients.erase(g_Clients.begin() + i);
            printf("[EditorBridge] Client disconnected (%d remaining)\n", (int)g_Clients.size());
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────

bool StartEditorBridge() {
    if (g_Running) return true;

    if (!IsWorldSystemReady()) {
        printf("[EditorBridge] World system not ready\n");
        return false;
    }

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[EditorBridge] WSAStartup failed\n");
        return false;
    }

    // Create listen socket
    g_ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ListenSocket == INVALID_SOCKET) {
        printf("[EditorBridge] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    // Bind to localhost only (security)
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(BRIDGE_PORT);

    // Allow port reuse
    int yes = 1;
    setsockopt(g_ListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    if (bind(g_ListenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[EditorBridge] bind() failed on port %d: %d\n", BRIDGE_PORT, WSAGetLastError());
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(g_ListenSocket, 2) == SOCKET_ERROR) {
        printf("[EditorBridge] listen() failed: %d\n", WSAGetLastError());
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
        return false;
    }

    // Set non-blocking
    u_long nonBlocking = 1;
    ioctlsocket(g_ListenSocket, FIONBIO, &nonBlocking);

    // Build initial actor hash map
    RebuildActorHashMap();
    printf("[EditorBridge] Actor hash map: %d entries\n", (int)g_ActorHashMap.size());

    // Register tick callback
    g_TickCallbackId = RegisterTickCallback(BridgeTick);
    if (g_TickCallbackId < 0) {
        printf("[EditorBridge] Failed to register tick callback\n");
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
        return false;
    }

    g_Running = true;
    g_Stats = {};
    printf("[EditorBridge] Server started on localhost:%d\n", BRIDGE_PORT);
    return true;
}

void StopEditorBridge() {
    if (!g_Running) return;

    std::lock_guard<std::mutex> lock(g_BridgeMutex);

    if (g_TickCallbackId >= 0) {
        UnregisterTickCallback(g_TickCallbackId);
        g_TickCallbackId = -1;
    }

    for (auto& client : g_Clients)
        closesocket(client);
    g_Clients.clear();

    if (g_ListenSocket != INVALID_SOCKET) {
        closesocket(g_ListenSocket);
        g_ListenSocket = INVALID_SOCKET;
    }

    g_ActorHashMap.clear();
    g_Running = false;
    printf("[EditorBridge] Server stopped\n");
}

bool IsEditorBridgeRunning() { return g_Running; }

int GetEditorBridgeClientCount() {
    std::lock_guard<std::mutex> lock(g_BridgeMutex);
    return (int)g_Clients.size();
}

BridgeStats GetEditorBridgeStats() { return g_Stats; }

} // namespace bs1sdk
