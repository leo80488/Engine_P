#pragma once
// A UDP socket with its own receive thread: the thread waits on the socket's event and a stop
// event together (WSAEventSelect + WaitForMultipleObjects - select / WSAPoll cannot wait on a stop
// event), and drains every datagram into the callback, on that thread. Binds 127.0.0.1 unless asked
// for the LAN (0.0.0.0: the first bind shows the Windows firewall prompt); port 0 takes an
// ephemeral port, which Port() reports. Winsock lives in the .cpp only: it clashes with the
// <windows.h> the PCH forces into every other translation unit.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace FaceTracking {

struct UdpPeer
{
    uint32_t ipv4 = 0;  // host byte order
    uint16_t port = 0;
    std::string ToString() const;
};

class UdpReceiver
{
public:
    using OnPacket = std::function<void(const uint8_t* data, size_t size, const UdpPeer& from)>;

    UdpReceiver();
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool Open(uint16_t port, bool lan, OnPacket onPacket, std::string& error);
    void Close(); // joins the thread; idempotent

    uint16_t Port() const;
    bool Lan() const;
    uint64_t Packets() const;
    std::string LastError() const;
    // From any thread (the receive thread answers a handshake with it).
    bool SendTo(const UdpPeer& to, const void* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace FaceTracking
