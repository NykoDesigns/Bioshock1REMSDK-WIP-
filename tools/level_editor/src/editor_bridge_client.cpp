// ═══════════════════════════════════════════════════════════════════════════
// Editor Bridge Client — TCP client for live editor→game communication.
// ═══════════════════════════════════════════════════════════════════════════

#include "editor_bridge_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <cstdio>
#include <cstring>
#include <chrono>

static bool g_WsaInitialized = false;

static void EnsureWsa() {
    if (!g_WsaInitialized) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        g_WsaInitialized = true;
    }
}

static double GetTimeMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

EditorBridgeClient::~EditorBridgeClient() {
    Disconnect();
}

bool EditorBridgeClient::Connect(const std::string& host, uint16_t port) {
    if (m_Connected) Disconnect();
    EnsureWsa();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printf("[BridgeClient] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[BridgeClient] connect() failed: %d (is the game running with 'eb' command?)\n",
               WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Set non-blocking for Poll()
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    m_Socket = (uintptr_t)sock;
    m_Connected = true;
    m_Sent = 0;
    m_Received = 0;
    m_HasPlayerPos = false;

    printf("[BridgeClient] Connected to game at %s:%d\n", host.c_str(), port);

    // Send initial ping
    SendPing();
    return true;
}

void EditorBridgeClient::Disconnect() {
    if (!m_Connected) return;
    closesocket((SOCKET)m_Socket);
    m_Socket = (uintptr_t)INVALID_SOCKET;
    m_Connected = false;
    printf("[BridgeClient] Disconnected\n");
}

void EditorBridgeClient::Poll() {
    if (!m_Connected) return;

    SOCKET sock = (SOCKET)m_Socket;
    char buf[4096];
    int received = recv(sock, buf, sizeof(buf), 0);

    if (received == 0) {
        // Server closed connection
        Disconnect();
        return;
    }
    if (received < 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            printf("[BridgeClient] recv error: %d\n", err);
            Disconnect();
        }
        return;
    }

    // Process messages
    int pos = 0;
    while (pos + (int)sizeof(BridgeHeader) <= received) {
        BridgeHeader hdr;
        memcpy(&hdr, buf + pos, sizeof(hdr));

        if (hdr.magic != BRIDGE_MAGIC) { pos++; continue; }

        int msgEnd = pos + (int)sizeof(BridgeHeader) + hdr.payloadLen;
        if (msgEnd > received) break;

        const void* payload = buf + pos + sizeof(BridgeHeader);
        m_Received++;

        switch (hdr.type) {
            case MSG_PLAYER_POS:
                if (hdr.payloadLen >= sizeof(PlayerPosUpdate)) {
                    memcpy(&m_PlayerPos, payload, sizeof(PlayerPosUpdate));
                    m_HasPlayerPos = true;
                }
                break;
            case MSG_PONG:
                if (m_PingSentTime > 0.0) {
                    m_LatencyMs = (float)(GetTimeMs() - m_PingSentTime);
                    m_PingSentTime = 0.0;
                }
                break;
            case MSG_ACTOR_POS:
            case MSG_ACTOR_LIST:
                // TODO: process actor list responses
                break;
            default:
                break;
        }
        pos = msgEnd;
    }
}

bool EditorBridgeClient::SendRaw(uint16_t type, const void* payload, uint16_t len) {
    if (!m_Connected) return false;

    BridgeHeader hdr;
    hdr.magic = BRIDGE_MAGIC;
    hdr.type = type;
    hdr.payloadLen = len;

    char buf[512];
    if (sizeof(BridgeHeader) + len > sizeof(buf)) return false;
    memcpy(buf, &hdr, sizeof(hdr));
    if (len > 0 && payload) memcpy(buf + sizeof(hdr), payload, len);

    int total = (int)(sizeof(hdr) + len);
    int sent = send((SOCKET)m_Socket, buf, total, 0);
    if (sent == total) { m_Sent++; return true; }

    // Check for disconnect
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) Disconnect();
    }
    return false;
}

bool EditorBridgeClient::SendMoveActor(uint32_t nameHash, float x, float y, float z) {
    struct { uint32_t h; float px, py, pz; } p = { nameHash, x, y, z };
    return SendRaw(MSG_MOVE_ACTOR, &p, sizeof(p));
}

bool EditorBridgeClient::SendRotateActor(uint32_t nameHash, int32_t pitch, int32_t yaw, int32_t roll) {
    struct { uint32_t h; int32_t p, y, r; } p = { nameHash, pitch, yaw, roll };
    return SendRaw(MSG_ROTATE_ACTOR, &p, sizeof(p));
}

bool EditorBridgeClient::SendSelectActor(uint32_t nameHash) {
    return SendRaw(MSG_SELECT_ACTOR, &nameHash, sizeof(nameHash));
}

bool EditorBridgeClient::SendQueryActors() {
    return SendRaw(MSG_QUERY_ACTORS, nullptr, 0);
}

bool EditorBridgeClient::SendQueryPlayer() {
    return SendRaw(MSG_QUERY_PLAYER, nullptr, 0);
}

bool EditorBridgeClient::SendPing() {
    m_PingSentTime = GetTimeMs();
    return SendRaw(MSG_PING, nullptr, 0);
}

uint32_t EditorBridgeClient::HashName(const std::string& name) {
    uint32_t h = 2166136261u;
    for (char c : name) { h ^= (uint8_t)c; h *= 16777619u; }
    return h;
}
