#pragma once

#include <cstdint>
#include <string>

namespace bs1sdk {

/// Lightweight non-blocking UDP socket wrapper over Winsock2.
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    // No copy
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Bind to a local port. Returns true on success.
    bool Bind(uint16_t port);

    /// Send raw bytes to a remote address.
    bool SendTo(const void* data, int size, const std::string& ip, uint16_t port);

    /// Non-blocking receive. Returns bytes read (0 = nothing, -1 = error).
    /// On success, fills senderIp and senderPort.
    int RecvFrom(void* buf, int maxSize, std::string& senderIp, uint16_t& senderPort);

    /// Close the socket.
    void Close();

    bool IsOpen() const { return m_Socket != ~0ULL; }
    uint16_t GetLocalPort() const { return m_Port; }

private:
    uintptr_t m_Socket; // SOCKET is uintptr_t on x86
    uint16_t  m_Port = 0;

    static bool s_WsaInit;
    static void InitWsa();
};

} // namespace bs1sdk
