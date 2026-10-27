// Network face sources: VMC and iFacialMocap over UDP. The receive thread (UdpReceiver) parses
// each datagram and pushes the frames it completes into the source's ring - the ring's only
// producer; Service::Pump drains it. Frames are stamped on arrival with the wall clock, and the
// source's clock is the wall clock minus a small delay, so a binding samples between two packets -
// in Edit mode too.

#include "FaceTracking/FaceSource.h"
#include "FaceTracking/IFacialMocapParser.h"
#include "FaceTracking/UdpReceiver.h"
#include "FaceTracking/VmcParser.h"
#include "System/Log.h"

#include <atomic>
#include <cstdio>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace FaceTracking {
namespace {

class NetworkFaceSource final : public IFaceSource
{
public:
    explicit NetworkFaceSource(const NetworkSourceOptions& o) : m_opts(o) {}
    ~NetworkFaceSource() override { m_udp.Close(); } // joins the thread before the parsers go

    const char* Kind() const override { return m_opts.protocol == NetworkProtocol::Vmc ? "vmc" : "ifacialmocap"; }
    // Fixed from construction (VmcParser registers its VRM presets up front): readers on the
    // main thread never race the receive thread over it.
    const FaceSourceChannels& Channels() const override
    {
        return m_opts.protocol == NetworkProtocol::Vmc ? m_vmc.Channels() : m_arkit;
    }
    bool WallClock() const override { return true; }
    int64_t DelayUs() const override { return m_opts.delayUs; }
    uint16_t Port() const override { return m_udp.Port(); }
    std::string LastError() const override { return m_udp.LastError(); }

    std::string Describe() const override
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "udp %s:%u%s, %llu packets (%llu malformed), %llu frames, %llu names dropped, %llu ring-full",
                      m_udp.Lan() ? "0.0.0.0" : "127.0.0.1", static_cast<unsigned>(m_udp.Port()),
                      m_udp.Lan() ? " (LAN)" : "", static_cast<unsigned long long>(m_packets.load()),
                      static_cast<unsigned long long>(m_malformed.load()), static_cast<unsigned long long>(m_frames.load()),
                      static_cast<unsigned long long>(m_dropped.load()), static_cast<unsigned long long>(m_ringFull.load()));
        return buf;
    }

    bool Start(FaceFrameRing& ring, int64_t epochUs, std::string& error) override
    {
        m_ring = &ring;
        m_epochUs = epochUs;
        return m_udp.Open(m_opts.port, m_opts.lan,
                          [this](const uint8_t* data, size_t size, const UdpPeer& from) { OnPacket(data, size, from); }, error);
    }

private:
    // On the receive thread.
    void OnPacket(const uint8_t* data, size_t size, const UdpPeer& from)
    {
        const int64_t t = SteadyNowUs() - m_epochUs;
        m_batch.clear();
        if (m_opts.protocol == NetworkProtocol::Vmc)
        {
            m_vmc.Feed(data, size, m_batch);
            m_packets.store(m_vmc.Stats().packets, std::memory_order_relaxed);
            m_malformed.store(m_vmc.Stats().malformed, std::memory_order_relaxed);
            m_dropped.store(m_vmc.Stats().droppedNames, std::memory_order_relaxed);
        }
        else
        {
            FaceFrame f;
            if (ParseIFacialMocap(std::string_view(reinterpret_cast<const char*>(data), size), f, m_ifmStats)) m_batch.push_back(f);
            m_packets.store(m_ifmStats.packets, std::memory_order_relaxed);
            m_malformed.store(m_ifmStats.malformed, std::memory_order_relaxed);
            m_dropped.store(m_ifmStats.droppedTokens, std::memory_order_relaxed);
            // The app shows "connected" (and keeps sending) once its first packet is answered.
            const uint64_t key = (static_cast<uint64_t>(from.ipv4) << 16) | from.port;
            if (!m_batch.empty() && m_answered.insert(key).second)
            {
                m_udp.SendTo(from, kIFacialMocapHandshake.data(), kIFacialMocapHandshake.size());
                LOG_INFO("[FACETRACK] ifacialmocap: answered %s", from.ToString().c_str());
            }
        }
        for (FaceFrame& f : m_batch)
        {
            f.seq = m_nextSeq++;
            f.sourceTimeUs = t;
            f.captureQpcUs = t + m_epochUs;
            if (m_ring->TryPush(f)) m_frames.fetch_add(1, std::memory_order_relaxed);
            else m_ringFull.fetch_add(1, std::memory_order_relaxed);
        }
    }

    NetworkSourceOptions m_opts;
    FaceSourceChannels m_arkit; // iFacialMocap sends ARKit channels only
    VmcParser m_vmc;
    IFacialMocapStats m_ifmStats;
    std::set<uint64_t> m_answered;
    std::vector<FaceFrame> m_batch;
    FaceFrameRing* m_ring = nullptr;
    int64_t m_epochUs = 0;
    uint64_t m_nextSeq = 1;
    std::atomic<uint64_t> m_packets{0}, m_malformed{0}, m_frames{0}, m_dropped{0}, m_ringFull{0};
    UdpReceiver m_udp; // last: destroyed first, so the thread stops before anything it uses
};

} // namespace

std::unique_ptr<IFaceSource> MakeNetworkSource(const NetworkSourceOptions& options)
{
    return std::make_unique<NetworkFaceSource>(options);
}

bool ParseNetworkProtocol(std::string_view name, NetworkProtocol& out, uint16_t& defaultPort)
{
    if (name == "vmc") { out = NetworkProtocol::Vmc; defaultPort = 39539; return true; }
    if (name == "ifacialmocap") { out = NetworkProtocol::IFacialMocap; defaultPort = kIFacialMocapPort; return true; }
    return false;
}

} // namespace FaceTracking
