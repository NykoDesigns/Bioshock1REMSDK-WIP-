#include "udp_socket.h"
#include "../core/log.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace bs1sdk {

bool UdpSocket::s_WsaInit = false;

void UdpSocket::InitWsa()
{
    if (s_WsaInit) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("[Net] WSAStartup failed: {}", WSAGetLastError());
        return;
    }
    s_WsaInit = true;
}

UdpSocket::UdpSocket()
    : m_Socket(INVALID_SOCKET)
{
    InitWsa();
}

UdpSocket::~UdpSocket()
{
    Close();
}

bool UdpSocket::Bind(uint16_t port)
{
    if (!s_WsaInit) return false;

    m_Socket = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_Socket == (uintptr_t)INVALID_SOCKET) {
        LOG_ERROR("[Net] socket() failed: {}", WSAGetLastError());
        return false;
    }

    // Set non-blocking
    u_long mode = 1;
    ioctlsocket((SOCKET)m_Socket, FIONBIO, &mode);

    // Allow address reuse
    int reuse = 1;
    setsockopt((SOCKET)m_Socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Increase recv buffer to 256KB
    int bufSize = 256 * 1024;
    setsockopt((SOCKET)m_Socket, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind((SOCKET)m_Socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("[Net] bind({}) failed: {}", port, WSAGetLastError());
        closesocket((SOCKET)m_Socket);
        m_Socket = (uintptr_t)INVALID_SOCKET;
        return false;
    }

    m_Port = port;
    LOG_INFO("[Net] UDP socket bound to port {}", port);
    return true;
}

bool UdpSocket::SendTo(const void* data, int size, const std::string& ip, uint16_t port)
{
    if (m_Socket == (uintptr_t)INVALID_SOCKET) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);

    int sent = sendto((SOCKET)m_Socket, reinterpret_cast<const char*>(data), size, 0,
                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    return sent == size;
}

int UdpSocket::RecvFrom(void* buf, int maxSize, std::string& senderIp, uint16_t& senderPort)
{
    if (m_Socket == (uintptr_t)INVALID_SOCKET) return -1;

    sockaddr_in from{};
    int fromLen = sizeof(from);
    int received = recvfrom((SOCKET)m_Socket, reinterpret_cast<char*>(buf), maxSize, 0,
                            reinterpret_cast<sockaddr*>(&from), &fromLen);

    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0; // nothing available
        return -1; // real error
    }

    // Extract sender info
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
    senderIp = ipStr;
    senderPort = ntohs(from.sin_port);

    return received;
}

void UdpSocket::Close()
{
    if (m_Socket != (uintptr_t)INVALID_SOCKET) {
        closesocket((SOCKET)m_Socket);
        m_Socket = (uintptr_t)INVALID_SOCKET;
        m_Port = 0;
        LOG_INFO("[Net] Socket closed");
    }
}

} // namespace bs1sdk
