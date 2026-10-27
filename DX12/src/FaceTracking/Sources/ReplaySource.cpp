#include "FaceTracking/FaceSource.h"
#include "FaceTracking/FaceTakePlayer.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace FaceTracking {
namespace {

int64_t NowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

class ReplaySource final : public IFaceSource
{
public:
    ReplaySource(FaceTake take, std::string path, bool loop) : m_take(std::move(take)), m_path(std::move(path))
    {
        m_player.Reset(&m_take.frames, loop); // borrows m_take.frames; the source never moves (unique_ptr)
    }

    const char* Kind() const override { return "replay"; }
    const FaceSourceChannels& Channels() const override { return m_take.channels; }

    void Produce(int64_t clockUs, FaceFrameRing& ring) override
    {
        const int64_t now = NowUs();
        m_player.Advance(clockUs, [&](const FaceFrame& f) {
            FaceFrame out = f;
            out.captureQpcUs = now;
            if (m_take.mirrored) out.flags |= kFaceMirrored;
            return ring.TryPush(out); // full: Advance resumes at this frame next Pump
        });
    }

    bool Finished() const override { return m_player.Finished(); }

    std::string Describe() const override
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), ": %zu frames, %.3f s, %s", m_player.FrameCount(),
                      static_cast<double>(m_player.SpanUs()) / 1e6, m_player.Looping() ? "looping" : "once");
        return m_path + buf;
    }

private:
    FaceTake m_take;
    std::string m_path;
    FaceTakePlayer m_player;
};

} // namespace

std::unique_ptr<IFaceSource> MakeReplaySource(FaceTake take, std::string path, bool loop)
{
    return std::make_unique<ReplaySource>(std::move(take), std::move(path), loop);
}

} // namespace FaceTracking
