#pragma once
#include "Input/InputTypes.h"
#include <string>
#include <unordered_map>
#include <vector>

// Data-defined gameplay vocabulary: profile actions are named, with no enum or 32-bit
// mask limit. GameAction stays the host / native surface; Registry::Bridge maps the
// active profile onto it.
namespace InputProfiles {
enum class Kind { Button, Axis1D, Axis2D };
struct Binding {
    int vk1=0, vk2=0, pad=0;
    int posX=0, negX=0, posY=0, negY=0;
    int stick=-1;             // -1 none, 0 left, 1 right
    int trigger=-1;           // -1 none, 0 left, 1 right (analog)
    bool mouse=false;         // independent pixel-displacement carrier
};
struct Value {
    float x=0, y=0, mouseX=0, mouseY=0;
    bool held=false, pressed=false, released=false;
};
struct Action {
    std::string name, bridge;
    Kind kind=Kind::Button;
    Binding defaults, binding;
    Value value;
    bool armed=false;         // re-arm only after this control returns neutral
};
struct Profile {
    std::vector<Action> actions;
    std::unordered_map<std::string,size_t> index;
};
struct Frame {
    const std::uint8_t* keys=nullptr; // 256 COOKED keyboard entries
    GamepadState pad;
    float mouseX=0, mouseY=0;
    bool allowed=true;
};
class Registry {
public:
    static Registry& Get();
    bool Define(const std::string& name, std::vector<Action> actions);
    bool Activate(const std::string& name);
    void Deactivate();
    void ResetState();
    void Evaluate(const Frame& frame);
    void Bridge(ActionState& command) const;
    const std::string& Active() const { return active; }
    const Profile* Find(const std::string& name) const;
    const Action* Find(const std::string& profile,const std::string& action) const;
    Value Read(const std::string& profile,const std::string& action) const;
    bool Set(const std::string& profile,const std::string& action,const Binding& binding);
    bool ResetBindings(const std::string& profile);
    bool Save() const;
    bool Reload();
    static bool Valid(const Binding& b);
private:
    std::unordered_map<std::string,Profile> profiles;
    std::string active;
};
}
