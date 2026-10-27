#pragma once
// A face source: something that turns into FaceFrames - a take, a script, a socket, a camera
// or a video. Sources belong to FaceTracking::Service, never to a World, and reach it through
// their ring.

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceOverlay.h"
#include "FaceTracking/FaceRing.h"
#include "FaceTracking/FaceTake.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace FaceTracking {

struct FaceImage;

// About four seconds at 60 Hz: Service::Pump drains every ring once per engine frame.
using FaceFrameRing = SpscRing<FaceFrame, 256>;

// The wall clock live sources run on (so Edit mode, dt = 0, still walks), in microseconds;
// a live source's frames and its clock count from the moment it opened.
inline int64_t SteadyNowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

class IFaceSource
{
public:
    virtual ~IFaceSource() = default;

    // "replay", "synthetic", "vmc", "ifacialmocap", "camera", "fakecamera" or "video".
    virtual const char* Kind() const = 0;
    virtual const FaceSourceChannels& Channels() const = 0;

    // Once per Service::Pump, on the consumer thread, with the source's own clock
    // (microseconds since it opened; a simulation clock paused while dt is 0, or the wall
    // clock - see WallClock). A clocked source - a take - pushes the frames now due; a push
    // source has nothing to do here.
    virtual void Produce(int64_t clockUs, FaceFrameRing& ring)
    {
        (void)clockUs;
        (void)ring;
    }

    // Frames a push source delivers carry no time of their own: the history stamps them
    // with the source clock of the Pump that drains them, so frames pushed between two
    // engine frames are current at the next one (the last of them wins).
    virtual bool StampsOnDrain() const { return false; }

    // A take that played to its end without looping: readers hold its last frame.
    virtual bool Finished() const { return false; }

    // For Status and the log: the take's path and length, a socket's address and counts.
    virtual std::string Describe() const { return {}; }

    // A source with a producer thread of its own (a socket, a camera) gets its ring here,
    // once, before its first Pump - that thread is then the ring's only producer. epochUs is
    // SteadyNowUs() when it opened: its frames carry time since then. False refuses the
    // source (error says why: the port is taken, the device is gone).
    virtual bool Start(FaceFrameRing& ring, int64_t epochUs, std::string& error)
    {
        (void)ring;
        (void)epochUs;
        (void)error;
        return true;
    }

    // A live source runs on the wall clock: Pump sets its clock to now - DelayUs() (since it
    // opened) instead of adding dt, so a binding samples a little behind the newest packet -
    // between two of them, rather than past the last one.
    virtual bool WallClock() const { return false; }
    virtual int64_t DelayUs() const { return 0; }

    // A network source's bound port (0 for the rest) and its last receive error.
    virtual uint16_t Port() const { return 0; }
    virtual std::string LastError() const { return {}; }

    // A capture source's state (CaptureStateName: running, denied, busy, lost, ended, ...).
    virtual const char* StateName() const { return "running"; }
    // An image source's newest image since the last call, for the editor preview (the
    // Pump thread asks; false when there is none, or the source has no images).
    virtual bool TakePreview(FaceImage& out)
    {
        (void)out;
        return false;
    }
    // What the tracker made of one of the images TakePreview handed out (by its seq): an image source
    // with a tracker keeps the last few; false for any other source, or once that image's is gone.
    virtual bool OverlayFor(uint64_t imageSeq, FaceOverlay& out) const
    {
        (void)imageSeq;
        (void)out;
        return false;
    }
    // WM_DEVICECHANGE, forwarded by App: a lost camera whose device came back reopens.
    virtual void OnDeviceChange() {}
};

// A take played against the source clock: every frame up to the clock plus the
// next one, looping by the take's period when `loop` is set.
std::unique_ptr<IFaceSource> MakeReplaySource(FaceTake take, std::string path, bool loop);

// Frames pushed by a script (FaceTracking.Push) or the editor's virtual tracker; stamped
// with the source clock when drained. New extra channel names register on push.
class SyntheticFaceSource final : public IFaceSource
{
public:
    const char* Kind() const override { return "synthetic"; }
    const FaceSourceChannels& Channels() const override { return m_channels; }
    bool StampsOnDrain() const override { return true; }
    FaceSourceChannels& MutableChannels() { return m_channels; }

private:
    FaceSourceChannels m_channels;
};

// Network sources: VMC (OSC over UDP) and iFacialMocap.
// TODO(Face_Tracking_Plan D10): Live Link Face.
enum class NetworkProtocol : uint8_t { Vmc, IFacialMocap };

struct NetworkSourceOptions
{
    NetworkProtocol protocol = NetworkProtocol::Vmc;
    uint16_t port = 39539;    // 0 = an ephemeral port (Status reports the one taken)
    bool lan = false;         // false: 127.0.0.1 only; true: 0.0.0.0 (the firewall asks once)
    int64_t delayUs = 50000;  // how far behind the newest packet a binding samples
};

std::unique_ptr<IFaceSource> MakeNetworkSource(const NetworkSourceOptions& options);
// "vmc" / "ifacialmocap" -> the protocol and its usual port; false for anything else.
bool ParseNetworkProtocol(std::string_view name, NetworkProtocol& out, uint16_t& defaultPort);

} // namespace FaceTracking
