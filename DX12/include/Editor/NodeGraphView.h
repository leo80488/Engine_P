#pragma once
#include "imgui-node-editor/imgui_node_editor.h"
namespace Editor::NodeGraphView {
// Shared context policy for material and animation canvases. Positions live in assets.
inline ax::NodeEditor::EditorContext* CreateContext() {
    ax::NodeEditor::Config config;config.SettingsFile=nullptr;config.EnableSmoothZoom=false;
    return ax::NodeEditor::CreateEditor(&config);
}
inline void DestroyContext(void* context) {if(context)ax::NodeEditor::DestroyEditor(static_cast<ax::NodeEditor::EditorContext*>(context));}
inline ax::NodeEditor::PinId Pin(uint32_t node,uint16_t index,bool output) {
    return ax::NodeEditor::PinId((static_cast<uint64_t>(node)<<16)|(output?1ull<<15:0)|index);
}
inline ax::NodeEditor::LinkId Link(uint32_t node,uint16_t input) {
    return ax::NodeEditor::LinkId((static_cast<uint64_t>(node)<<16)|input|(1ull<<62));
}
}
