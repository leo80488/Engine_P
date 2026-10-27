#pragma once
#include <string>
namespace Resource { class AnimationClipSystem; }
class AnimGraphSystem;
namespace Editor::AnimSetEditor {
bool Open(const std::string& path);
void Draw(Resource::AnimationClipSystem* clips);
void ProcessPendingActions(AnimGraphSystem* system);
bool ConsumeFocusedLastDraw();
std::string Control(const std::string& request);
}
