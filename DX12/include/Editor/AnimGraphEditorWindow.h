#pragma once
#include <string>
class World;class AnimGraphSystem;class Renderer;
namespace Resource {class AssetManager;class AnimationClipSystem;}
namespace Editor::AnimGraphEditor {
bool Open(const std::string&);
void Draw(World*, AnimGraphSystem*);
void ProcessPendingActions(World*,Renderer*,Resource::AssetManager*,Resource::AnimationClipSystem*,bool editMode);
void Shutdown();
bool ConsumeFocusedLastDraw();
std::string Control(const std::string& request);
}
