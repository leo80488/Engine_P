#pragma once
#include "Gameplay/AttackDefinition.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AttackEditor {
struct TimingTrackDrag {
    int band=-1,edge=0;
    float mouse=0,start=0,end=0;
};
inline bool DrawTimingTracks(const Attack::Definition& d, Attack::Json& document, TimingTrackDrag& drag, float progress, float& scrubProgress) {

    struct Band {
        const char* label; const char* startKey; const char* endKey;
        int window; float start,end,minimum,maximum; ImU32 color;
    };
    float finishMinimum=std::max({d.recovery,d.moveEnd,d.turnEnd,d.comboEnd});
    for(const auto& w:d.windows)finishMinimum=std::max(finishMinimum,w.end);
    std::vector<Band> bands={
        {"Windup",nullptr,"windup",-1,0,d.windup,0,d.recovery,IM_COL32(170,140,65,220)},
        {"Recovery","recovery","finish",-1,d.recovery,d.finish,d.windup,1,IM_COL32(80,130,155,220)}};
    for(int i=0;i<int(d.windows.size());++i) {
        const auto& w=d.windows[i];
        bands.push_back({"Hit","start","end",i,w.start,w.end,
            i?d.windows[i-1].end:0,i+1<int(d.windows.size())?d.windows[i+1].start:d.finish,
            IM_COL32(220,80,60,220)});
    }
    bands.push_back({"Move",nullptr,"moveEnd",-1,0,d.moveEnd,.001f,d.finish,IM_COL32(70,170,120,220)});
    bands.push_back({"Turn",nullptr,"turnEnd",-1,0,d.turnEnd,0,d.finish,IM_COL32(170,100,180,220)});
    bands.push_back({"Combo","comboStart","comboEnd",-1,d.comboStart,d.comboEnd,0,d.finish,IM_COL32(90,130,220,220)});
    ImGui::TextUnformatted("Source timing (0-100%)");
    ImGui::TextWrapped("Drag edges to resize; drag Hit / Combo bars to move. Drag empty space to scrub.");
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    const float labelWidth=90, rowHeight=ImGui::GetFrameHeight()+6;
    const float width=std::max(30.f,ImGui::GetContentRegionAvail().x-labelWidth-8);
    const float height=rowHeight*(bands.size()+1);
    ImGui::InvisibleButton("Source timeline",ImVec2(labelWidth+width+8,height));
    const bool active=ImGui::IsItemActive();
    const auto mouse=ImGui::GetIO().MousePos;
    const float u=(mouse.x-origin.x-labelWidth)/width;
    const int row=int(std::floor((mouse.y-origin.y)/rowHeight))-1;
    int hovered=-1,edge=0;
    if(ImGui::IsItemHovered()&&row>=0&&row<int(bands.size())) {
        const auto& b=bands[row];
        float left=std::abs((u-b.start)*width),right=std::abs((u-b.end)*width);
        if(right<=7&&(!b.startKey||right<=left)){hovered=row;edge=1;}
        else if(b.startKey&&left<=7){hovered=row;edge=-1;}
        else if(u>=b.start&&u<=b.end){hovered=row;edge=b.startKey?0:1;}
        // Recovery's ends are phase boundaries, not a movable interval.
        if(hovered==1&&edge==0)edge=left<right?-1:1;
    }
    if(ImGui::IsItemActivated()) {
        drag.band=hovered;drag.edge=edge;drag.mouse=u;
        if(hovered>=0){drag.start=bands[hovered].start;drag.end=bands[hovered].end;}
    }
    if(active&&ImGui::IsMouseDown(0)) {
        if(drag.band>=0&&drag.band<int(bands.size())) {
            const auto& b=bands[drag.band];
            float start=drag.start,end=drag.end;
            const float delta=u-drag.mouse;
            const float gap=b.window>=0?std::min(.001f,(drag.end-drag.start)*.5f):0;
            if(drag.edge<0)start=std::clamp(drag.start+delta,b.minimum,end-gap);
            else if(drag.edge>0)end=std::clamp(drag.end+delta,
                drag.band==1?std::max(start,finishMinimum):std::max(start+gap,b.minimum),b.maximum);
            else {
                const float shift=std::clamp(delta,b.minimum-drag.start,b.maximum-drag.end);
                start+=shift;end+=shift;
            }
            // Merely selecting a bar must not dirty an otherwise unchanged JSON
            // document by converting its decimal doubles to sampled floats.
            if(delta!=0) {
                Attack::Json& target=b.window>=0?document["hitWindows"][b.window]:document;
                if(b.startKey)target[b.startKey]=start;
                target[b.endKey]=end;
            }
        } else {
            scrubProgress=std::clamp(u,0.f,d.finish);
        }
    }
    if(hovered>=0|| (ImGui::IsItemActive()&&drag.band>=0))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    auto* dl=ImGui::GetWindowDrawList();
    const float x=origin.x+labelWidth;
    for(int tick=0;tick<=10;++tick) {
        float tx=x+width*tick/10.f;
        dl->AddLine(ImVec2(tx,origin.y+rowHeight),ImVec2(tx,origin.y+height),IM_COL32(65,70,80,110));
        char text[12];snprintf(text,sizeof(text),"%d",tick*10);
        dl->AddText(ImVec2(tx-ImGui::CalcTextSize(text).x*.5f,origin.y),IM_COL32(185,190,200,255),text);
    }
    for(int i=0;i<int(bands.size());++i) {
        const auto& b=bands[i];
        const float y=origin.y+(i+1)*rowHeight;
        char label[32];if(b.window>=0)snprintf(label,sizeof(label),"Hit %d",b.window+1);else snprintf(label,sizeof(label),"%s",b.label);
        dl->AddText(ImVec2(origin.x,y),IM_COL32_WHITE,label);
        dl->AddRectFilled(ImVec2(x,y),ImVec2(x+width,y+rowHeight-4),IM_COL32(40,45,54,150));
        dl->AddRectFilled(ImVec2(x+b.start*width,y+2),ImVec2(x+b.end*width,y+rowHeight-6),b.color,2);
        for(int side=0;side<2;++side) {
            if(side==0&&!b.startKey)continue;
            const float hx=x+(side?b.end:b.start)*width;
            dl->AddRectFilled(ImVec2(hx-3,y+1),ImVec2(hx+3,y+rowHeight-5),IM_COL32(230,230,235,255),1);
        }
    }

    dl->AddLine(ImVec2(x+progress*width,origin.y+rowHeight),ImVec2(x+progress*width,origin.y+height),IM_COL32_WHITE,2);
    if(hovered>=0) {
        const auto& b=bands[hovered];
        ImGui::SetTooltip("%s: %.1f%% - %.1f%%",b.label,b.start*100,b.end*100);
    }
    if(!ImGui::IsMouseDown(0))drag.band=-1;
    return active;
}

}
