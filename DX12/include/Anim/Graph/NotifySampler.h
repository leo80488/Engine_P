#pragma once
#include "ECS/NotifyTypes.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>

namespace Anim::Graph {
enum class NotifyEdge : uint8_t { Point, Begin, End };
struct SampledNotify {
    NotifyEdge edge = NotifyEdge::Point;
    uint16_t source = 0;
    float weight = 1;
    bool forcedEnd = false;
    Notify point;
    NotifyState state;
    std::string sourcePath;
    bool activeBranch=true;
};
// Each player owns its intervals. State copies remain valid across resource reloads.
class NotifySampler {
public:
    void BeginFrame() { for (auto& [key, open] : m_open) open.seen = false; }
    void EndFrame(std::vector<SampledNotify>& out,bool endLost=true) {
        if(!endLost)return;
        for (auto it=m_open.begin();it!=m_open.end();) {
            if (it->second.seen) { ++it; continue; }
            const auto& open=it->second;
            SampledNotify event; event.edge=NotifyEdge::End; event.source=open.source;
            event.weight=open.weight; event.forcedEnd=true; event.state=open.state;
            event.sourcePath=open.sourcePath;event.activeBranch=open.activeBranch;
            out.push_back(std::move(event)); it=m_open.erase(it);
        }
    }
    void EndAll(std::vector<SampledNotify>& out) { BeginFrame(); EndFrame(out); }
    void Sample(uint64_t player, uint16_t source, float weight,
                const std::vector<NotifyTrack>& tracks, double previous, double current,
                double duration, bool looping, std::vector<SampledNotify>& out,
                std::string_view sourcePath={}, bool activeBranch=true) {
        if (!(duration>0) || !std::isfinite(previous) || !std::isfinite(current)) return;
        struct Crossing { double time; NotifyEdge edge; uint32_t track, index; };
        std::vector<Crossing> crossings;
        const bool forward=current>=previous;
        const auto lo=std::min(previous,current),hi=std::max(previous,current);
        auto crossed=[&](double time,NotifyEdge edge,uint32_t track,uint32_t index) {
            if (lo==hi) return;
            const int64_t first=looping ? static_cast<int64_t>(forward?std::floor((lo-time)/duration)+1:std::ceil((lo-time)/duration)) : 0;
            const int64_t last=looping ? static_cast<int64_t>(forward?std::floor((hi-time)/duration):std::ceil((hi-time)/duration)-1) : 0;
            for(int64_t cycle=first;cycle<=last;++cycle) {
                const double at=time+cycle*duration;
                // Forward (previous,current], reverse [current,previous).
                if (forward ? at>previous && at<=current : at>=current && at<previous)
                    crossings.push_back({at,edge,track,index});
            }
        };
        double phase=looping ? std::fmod(previous,duration) : previous;
        if(phase<0)phase+=duration;
        for(uint32_t t=0;t<tracks.size();++t) {
            const auto& track=tracks[t]; if(track.muted)continue;
            for(uint32_t i=0;i<track.states.size();++i) {
                const auto& state=track.states[i]; const Key key{player,t,i};
                auto open=m_open.find(key);
                if(open!=m_open.end()) { open->second.seen=true; open->second.weight=weight;open->second.activeBranch=activeBranch; }
                else if(phase>=state.startTime && phase<state.endTime) {
                    m_open.emplace(key,Open{state,source,weight,true,std::string(sourcePath),activeBranch});
                    SampledNotify event;event.edge=NotifyEdge::Begin;event.source=source;
                    event.weight=weight;event.state=state;event.sourcePath=sourcePath;event.activeBranch=activeBranch;out.push_back(std::move(event));
                }
                crossed(state.startTime,forward?NotifyEdge::Begin:NotifyEdge::End,t,i);
                crossed(state.endTime,forward?NotifyEdge::End:NotifyEdge::Begin,t,i);
            }
            for(uint32_t i=0;i<track.notifies.size();++i)if(track.notifies[i].category!=NotifyCategory::Sync)crossed(track.notifies[i].time,NotifyEdge::Point,t,i);
        }
        std::stable_sort(crossings.begin(),crossings.end(),[&](const auto& a,const auto& b) {
            return forward?a.time<b.time:a.time>b.time;
        });
        for(const auto& crossing:crossings) {
            SampledNotify event;event.edge=crossing.edge;event.source=source;event.weight=weight;event.sourcePath=sourcePath;event.activeBranch=activeBranch;
            if(crossing.edge==NotifyEdge::Point)event.point=tracks[crossing.track].notifies[crossing.index];
            else {
                const Key key{player,crossing.track,crossing.index};
                event.state=tracks[crossing.track].states[crossing.index];
                if(crossing.edge==NotifyEdge::Begin) {
                    if(m_open.contains(key))continue;
                    m_open.emplace(key,Open{event.state,source,weight,true,std::string(sourcePath),activeBranch});
                }else if(!m_open.erase(key))continue;
            }
            out.push_back(std::move(event));
        }
    }

private:
    struct Key {
        uint64_t player; uint32_t track, state;
        auto operator<=>(const Key&) const = default;
    };
    struct Open { NotifyState state; uint16_t source; float weight; bool seen = false; std::string sourcePath; bool activeBranch=true; };

    std::map<Key, Open> m_open;
};
}
