#pragma once
#include "Anim/Graph/AnimGraphDefinition.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace Anim::Graph {
// Deterministic Bowyer-Watson triangulation. Insertion order breaks cocircular
// ties, producing one non-overlapping triangulation even for a square grid.
inline std::vector<std::array<int,3>> Triangulate(const std::vector<Sample>& samples) {
    struct Point{double x,y;};std::vector<Point> p;double extent=1;
    for(auto& s:samples){p.push_back({s.x,s.y});extent=std::max(extent,std::max(std::abs(double(s.x)),std::abs(double(s.y))));}
    int count=int(p.size());p.push_back({-32*extent,-16*extent});p.push_back({32*extent,-16*extent});p.push_back({0,32*extent});
    std::vector<std::array<int,3>> tris{{count,count+1,count+2}};
    auto inside=[&](auto t,Point q){auto a=p[t[0]],b=p[t[1]],c=p[t[2]];double ax=a.x-q.x,ay=a.y-q.y,bx=b.x-q.x,by=b.y-q.y,cx=c.x-q.x,cy=c.y-q.y;double determinant=(ax*ax+ay*ay)*(bx*cy-by*cx)-(bx*bx+by*by)*(ax*cy-ay*cx)+(cx*cx+cy*cy)*(ax*by-ay*bx);double area=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);return determinant*area>1e-12;};
    for(int i=0;i<count;++i){std::vector<std::array<int,2>> edges;std::vector<std::array<int,3>> keep;
        for(auto t:tris)if(inside(t,p[i])){for(int k=0;k<3;++k){std::array<int,2> e{t[k],t[(k+1)%3]};if(e[0]>e[1])std::swap(e[0],e[1]);auto it=std::find(edges.begin(),edges.end(),e);if(it!=edges.end())edges.erase(it);else edges.push_back(e);}}else keep.push_back(t);
        for(auto e:edges){double area=(p[e[1]].x-p[e[0]].x)*(p[i].y-p[e[0]].y)-(p[e[1]].y-p[e[0]].y)*(p[i].x-p[e[0]].x);if(std::abs(area)>1e-12)keep.push_back({e[0],e[1],i});}tris=std::move(keep);
    }
    tris.erase(std::remove_if(tris.begin(),tris.end(),[&](auto t){return t[0]>=count || t[1]>=count || t[2]>=count;}),tris.end());return tris;
}
inline std::vector<float> CartesianWeights(const Node& node,float x,float y) {
    const auto& s=node.samples;std::vector<float> out(s.size(),0);if(s.empty())return out;
    for(auto t:node.triangles){auto a=s[t[0]],b=s[t[1]],c=s[t[2]];double d=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);if(std::abs(d)<1e-12)continue;
        float u=float(((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/d),v=float(((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/d),w=1-u-v;
        if(u>=-1e-6f && v>=-1e-6f && w>=-1e-6f){out[t[0]]=std::max(0.f,u);out[t[1]]=std::max(0.f,v);out[t[2]]=std::max(0.f,w);float total=out[t[0]]+out[t[1]]+out[t[2]];for(auto& value:out)value/=total;return out;}
    }
    // Outside hull: project onto its nearest edge. Collinear/one/two point
    // sets use the same projection without constructing degenerate triangles.
    double best=std::numeric_limits<double>::max();int aBest=0,bBest=0;float tBest=0;
    auto edge=[&](int a,int b){double dx=s[b].x-s[a].x,dy=s[b].y-s[a].y,length=dx*dx+dy*dy;float t=length>0?float(std::clamp(((x-s[a].x)*dx+(y-s[a].y)*dy)/length,0.,1.)):0;double px=s[a].x+t*dx-x,py=s[a].y+t*dy-y,d=px*px+py*py;if(d<best){best=d;aBest=a;bBest=b;tBest=t;}};
    if(node.triangles.empty()){for(int a=0;a<int(s.size());++a)for(int b=a;b<int(s.size());++b)edge(a,b);}
    else for(auto t:node.triangles)for(int k=0;k<3;++k)edge(t[k],t[(k+1)%3]);
    out[aBest]+=1-tBest;out[bBest]+=tBest;return out;
}
}
