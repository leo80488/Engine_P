// Compiled without the PCH and without its forced include (EngineCore.vcxproj, like
// TracyClient.cpp): pch.h pulls <windows.h> in without WIN32_LEAN_AND_MEAN, which brings
// winsock.h, and <winsock2.h> after it redefines everything (C2011). Winsock2 first, then
// windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h> // SIO_UDP_CONNRESET
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#include "FaceTracking/UdpReceiver.h"

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace FaceTracking {
struct UdpReceiver::Impl
{
    static constexpr int kDrainPerWake = 256;

    SOCKET sock = INVALID_SOCKET;
    WSAEVENT readEvent = WSA_INVALID_EVENT;
    HANDLE stopEvent = nullptr;
    std::thread thread;
    bool wsa = false;
    uint16_t port = 0;
    bool lan = false;
    std::atomic<uint64_t> packets{0};
    mutable std::mutex errorMutex;
    std::string lastError;
    std::mutex sendMutex;
    OnPacket onPacket;

    void SetError(const std::string& e)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = e;
    }

    void Run()
    {
        std::vector<uint8_t> buf(65536);
        const HANDLE waits[2] = {stopEvent, readEvent};
        for (;;)
        {
            const DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) return; // stop
            if (r != WAIT_OBJECT_0 + 1)
            {
                SetError("WaitForMultipleObjects failed (" + std::to_string(GetLastError()) + ")");
                return;
            }
            WSANETWORKEVENTS ne{};
            WSAEnumNetworkEvents(sock, readEvent, &ne); // resets the event
            // Drain, at most kDrainPerWake datagrams a wake: the socket is non-blocking under
            // WSAEventSelect, a recvfrom that leaves data queued sets the event again, and the wait
            // above ranks the stop event first - a sender faster than the parsing cannot keep
            // Close waiting.
            for (int k = 0; k < kDrainPerWake; ++k)
            {
                sockaddr_in from{};
                int fromLen = sizeof(from);
                const int n = recvfrom(sock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n == SOCKET_ERROR)
                {
                    const int e = WSAGetLastError();
                    if (e == WSAEMSGSIZE || e == WSAECONNRESET) continue; // one datagram dropped: read on
                    if (e != WSAEWOULDBLOCK) SetError("recvfrom failed (" + std::to_string(e) + ")");
                    break;
                }
                packets.fetch_add(1, std::memory_order_relaxed);
                UdpPeer peer;
                peer.ipv4 = ntohl(from.sin_addr.s_addr);
                peer.port = ntohs(from.sin_port);
                if (onPacket) onPacket(buf.data(), static_cast<size_t>(n), peer);
            }
        }
    }
};

std::string UdpPeer::ToString() const
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ipv4 >> 24) & 255u, (ipv4 >> 16) & 255u, (ipv4 >> 8) & 255u,
                  ipv4 & 255u, static_cast<unsigned>(port));
    return buf;
}

UdpReceiver::UdpReceiver() : m(std::make_unique<Impl>()) {}
UdpReceiver::~UdpReceiver() { Close(); }

bool UdpReceiver::Open(uint16_t port, bool lan, OnPacket onPacket, std::string& error)
{
    Close();
    WSADATA wd{};
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
    {
        error = "WSAStartup failed";
        return false;
    }
    m->wsa = true;
    m->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m->sock == INVALID_SOCKET)
    {
        error = "socket failed (" + std::to_string(WSAGetLastError()) + ")";
        Close();
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(m->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        const int e = WSAGetLastError();
        error = "bind to " + std::string(lan ? "0.0.0.0" : "127.0.0.1") + ":" + std::to_string(port) + " failed (" +
                std::to_string(e) + (e == WSAEADDRINUSE ? ": the port is in use" : "") + ")";
        Close();
        return false;
    }
    int len = sizeof(addr);
    getsockname(m->sock, reinterpret_cast<sockaddr*>(&addr), &len);
    m->port = ntohs(addr.sin_port);
    m->lan = lan;
    const int rcvbuf = 1 << 20;
    setsockopt(m->sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    // A handshake sent to a peer that is gone makes Windows report WSAECONNRESET on the next
    // receive (the ICMP "port unreachable"); a receiver has no use for that.
    BOOL connReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(m->sock, SIO_UDP_CONNRESET, &connReset, sizeof(connReset), nullptr, 0, &bytes, nullptr, nullptr);
    m->readEvent = WSACreateEvent();
    m->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m->readEvent == WSA_INVALID_EVENT || !m->stopEvent || WSAEventSelect(m->sock, m->readEvent, FD_READ) == SOCKET_ERROR)
    {
        error = "WSAEventSelect failed (" + std::to_string(WSAGetLastError()) + ")";
        Close();
        return false;
    }
    m->onPacket = std::move(onPacket);
    m->packets = 0;
    m->SetError({});
    m->thread = std::thread([impl = m.get()] { impl->Run(); });
    return true;
}

void UdpReceiver::Close()
{
    if (m->thread.joinable())
    {
        SetEvent(m->stopEvent);
        m->thread.join();
    }
    if (m->sock != INVALID_SOCKET)
    {
        closesocket(m->sock);
        m->sock = INVALID_SOCKET;
    }
    if (m->readEvent != WSA_INVALID_EVENT)
    {
        WSACloseEvent(m->readEvent);
        m->readEvent = WSA_INVALID_EVENT;
    }
    if (m->stopEvent)
    {
        CloseHandle(m->stopEvent);
        m->stopEvent = nullptr;
    }
    if (m->wsa)
    {
        WSACleanup();
        m->wsa = false;
    }
    m->onPacket = nullptr;
}

uint16_t UdpReceiver::Port() const { return m->port; }
bool UdpReceiver::Lan() const { return m->lan; }
uint64_t UdpReceiver::Packets() const { return m->packets.load(std::memory_order_relaxed); }

std::string UdpReceiver::LastError() const
{
    std::lock_guard<std::mutex> lock(m->errorMutex);
    return m->lastError;
}

bool UdpReceiver::SendTo(const UdpPeer& to, const void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(m->sendMutex);
    if (m->sock == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    addr.sin_addr.s_addr = htonl(to.ipv4);
    return sendto(m->sock, static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) != SOCKET_ERROR;
}

} // namespace FaceTracking
