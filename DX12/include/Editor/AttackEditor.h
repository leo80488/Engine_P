#pragma once
#include <string>
namespace AttackEditor {
void Open(const std::string& path="asset/RossiDuel/Attacks/Enemy/combo1.attack.json");
void Draw();
std::string Control(const std::string& request);
}
