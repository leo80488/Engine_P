#pragma once
#include "Gameplay/GameplayTags.h"
#include <nlohmann/json.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Attack {
using Json=nlohmann::json;
struct Key { float u=0, value=1, in=0, out=0; std::string interp="linear"; };
struct Curve {
    std::vector<Key> keys;
    float Sample(float u) const;
};
struct Window { float start=0.24f,end=0.52f; };
struct Definition {
    static constexpr int Samples=4096;

    Json document;
    std::string path,id,clip,alias;
    Curve speed,motion;
    float baseSpeed=1,clipSeconds=1,finish=.91f,windup=.24f,recovery=.52f;
    float cost=18,damage=36,staminaDamage=32,blendIn=.07f,blendOut=.2f;
    float moveEnd=.42f,distance=.25f,turnEnd=0,turnRate=6;
    float radius=.85f,halfHeight=.3f,offsetX=0,offsetY=.95f,offsetZ=.85f;
    bool weaponCollider=true;
    std::string weaponTag; // empty selects all attached AttackBox colliders
    // hitbox.requireTags / ignoreTags: gameplay tags the victim's root must / must not match (see HitboxParams).
    std::vector<Gameplay::Tag> requireTags,ignoreTags;
    float comboStart=.64f,comboEnd=.91f;
    std::string next;
    std::vector<Window> windows;

    std::array<double,Samples+1> clock{}; // unit-clip seconds at source progress

    double TimeAt(float u) const;
    float ProgressAt(double elapsed, float duration) const;
    double Duration(float duration) const { return TimeAt(finish)*duration; }
    float Displacement(float u) const;
};
std::shared_ptr<const Definition> Parse(const Json&,const std::string& path="");
std::shared_ptr<const Definition> Load(const std::string&,bool reload=false);
Json LoadSet(const std::string&,bool reload=false);
void Save(const std::string&,const Json&);
void Apply(const std::string&,const Json&); // replaces the cached definition; active attacks keep their snapshot
float ClipDuration(const std::string&);
}
