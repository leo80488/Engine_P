#pragma once
#include <array>
#include <algorithm>
#include <vector>
namespace Editor {
struct AnimStateMachineView {
    std::array<float,2> pan{0,0};float zoom=1;
    std::array<float,2> ToCanvas(float x,float y)const{return {(x-pan[0])/zoom,(y-pan[1])/zoom};}
    int Hit(float x,float y,const std::vector<std::array<float,2>>& positions)const {
        const auto p=ToCanvas(x,y);
        for(int i=static_cast<int>(positions.size())-1;i>=0;--i)
            if(p[0]>=positions[i][0] && p[0]<=positions[i][0]+150 && p[1]>=positions[i][1] && p[1]<=positions[i][1]+60)return i;
        return -1;
    }
    std::vector<int> Select(float x0,float y0,float x1,float y1,const std::vector<std::array<float,2>>& positions)const {
        auto a=ToCanvas(x0,y0),b=ToCanvas(x1,y1);std::vector<int> result;
        for(int i=0;i<positions.size();++i)if(positions[i][0]<=std::max(a[0],b[0]) && positions[i][0]+150>=std::min(a[0],b[0]) &&
            positions[i][1]<=std::max(a[1],b[1]) && positions[i][1]+60>=std::min(a[1],b[1]))result.push_back(i);
        return result;
    }
};
}
