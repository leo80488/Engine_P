#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <functional>
namespace Editor::GraphLayout {
inline std::vector<std::array<float,2>> Layers(const std::vector<std::vector<size_t>>& inputs) {
    std::vector<int> depths(inputs.size(),-1);std::vector<bool> visiting(inputs.size());
    std::function<int(size_t)> depth=[&](size_t node) {
        if(node>=inputs.size() || visiting[node])return 0;
        if(depths[node]>=0)return depths[node];visiting[node]=true;int result=0;
        for(auto child:inputs[node])result=std::max(result,depth(child)+1);
        visiting[node]=false;return depths[node]=result;
    };
    std::vector<size_t> rows(inputs.size()+1);std::vector<std::array<float,2>> positions(inputs.size());
    for(size_t i=0;i<inputs.size();++i) {const auto layer=depth(i);positions[i]={layer*250.f,rows[layer]++*150.f};}
    return positions;
}

// Layers plus barycenter sweeps: the order inside each layer follows the average
// rank of a node's inputs (a sweep down), then of its outputs (a sweep up),
// @p sweeps times, so the edges cross less; each layer is centred on the tallest.
// @p inputs[i] are node i's predecessors; a cycle is cut where the depth search
// meets it (as Layers). Returns {x, y} per node.
inline std::vector<std::array<float,2>> LayersOrdered(const std::vector<std::vector<size_t>>& inputs,
                                                      float dx=260.f,float dy=110.f,int sweeps=2) {
    const size_t n=inputs.size();
    std::vector<int> depths(n,-1);std::vector<bool> visiting(n);
    std::function<int(size_t)> depth=[&](size_t node) {
        if(node>=n || visiting[node])return 0;
        if(depths[node]>=0)return depths[node];visiting[node]=true;int result=0;
        for(auto child:inputs[node])result=std::max(result,depth(child)+1);
        visiting[node]=false;return depths[node]=result;
    };
    int layers=0;for(size_t i=0;i<n;++i)layers=std::max(layers,depth(i)+1);
    std::vector<std::vector<size_t>> layer(layers),outputs(n);
    for(size_t i=0;i<n;++i){layer[depths[i]].push_back(i);for(auto p:inputs[i])if(p<n)outputs[p].push_back(i);}
    std::vector<float> rank(n);
    auto renumber=[&](std::vector<size_t>& l){for(size_t r=0;r<l.size();++r)rank[l[r]]=float(r);};
    for(auto& l:layer)renumber(l);
    auto order=[&](std::vector<size_t>& l,const std::vector<std::vector<size_t>>& by) {
        std::vector<std::pair<float,size_t>> keys;keys.reserve(l.size());
        for(auto node:l){float sum=0;int count=0;for(auto o:by[node])if(o<n){sum+=rank[o];++count;}
            keys.push_back({count?sum/count:rank[node],node});}
        std::stable_sort(keys.begin(),keys.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        for(size_t r=0;r<l.size();++r)l[r]=keys[r].second;renumber(l);
    };
    for(int s=0;s<sweeps;++s) {
        for(int k=1;k<layers;++k)order(layer[k],inputs);
        for(int k=layers-2;k>=0;--k)order(layer[k],outputs);
    }
    size_t tallest=0;for(auto& l:layer)tallest=std::max(tallest,l.size());
    std::vector<std::array<float,2>> positions(n);
    for(int k=0;k<layers;++k){const float top=(float(tallest)-float(layer[k].size()))*dy*.5f;
        for(size_t r=0;r<layer[k].size();++r)positions[layer[k][r]]={k*dx,top+r*dy};}
    return positions;
}
}
