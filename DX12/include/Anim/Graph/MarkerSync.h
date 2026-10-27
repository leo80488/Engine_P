#pragma once
#include "Resource/SkeletonAsset.h"
namespace Anim::Graph {
// Map an unwrapped leader time by its neighbouring named sync markers. Missing
// names fall back to normalized phase; loop count and reverse direction survive.
inline float MarkerTime(const ClipAsset& leader,const ClipAsset& follower,float time) {
    struct Marker{float time;std::string name;};auto markers=[](const ClipAsset& clip){std::vector<Marker> out;for(const auto& track:clip.notifyTracks)if(!track.muted)for(const auto& n:track.notifies)if(n.category==NotifyCategory::Sync && n.time>=0 && n.time<clip.duration)out.push_back({n.time,n.displayName.empty()?n.params.GetString("marker", ""):n.displayName});std::stable_sort(out.begin(),out.end(),[](auto& a,auto& b){return a.time<b.time;});return out;};
    const float duration=std::max(leader.duration,1e-3f),fallback=time/duration*follower.duration;auto a=markers(leader),b=markers(follower);if(a.size()<2 || b.size()<2)return fallback;
    float cycle=std::floor(time/duration),local=time-cycle*duration;size_t previous=a.size()-1;for(size_t i=0;i<a.size();++i)if(a[i].time<=local)previous=i;
    size_t next=(previous+1)%a.size();float start=a[previous].time,end=a[next].time;if(end<=start)end+=duration;if(local<start){local+=duration;cycle-=1;}float alpha=(local-start)/std::max(end-start,1e-6f);
    for(size_t i=0;i<b.size();++i)if(b[i].name==a[previous].name){size_t j=(i+1)%b.size();if(b[j].name!=a[next].name)continue;float finish=b[j].time;if(finish<=b[i].time)finish+=follower.duration;return cycle*follower.duration+b[i].time+alpha*(finish-b[i].time);}
    return fallback;
}
}
