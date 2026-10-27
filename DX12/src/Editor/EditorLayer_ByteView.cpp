// EditorLayer_ByteView.cpp - the Byte View panel: a read-only hex view (external/imgui_club's
// MemoryEditor) of a component, its reflected fields shaded and listed with their offsets and values
// (the Inspector header's right-click "View Bytes"), or of an asset file, the AssetHeader's magic and
// lengths named (the Content Browser's right-click). Always read-only: a byte written here would skip
// Reflect::WriteProperty and the undo history. The component is a copy taken at the frame boundary
// (World::CloneComponentByType); nothing is read back from the GPU.

#include "Editor/EditorLayer.h"

#include "Reflection/Property.h"
#include "Reflection/ReflectRegistry.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"
#include "imgui_club/imgui_memory_editor.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace
{
    // The view's state: one source at a time.
    struct ByteView
    {
        MemoryEditor                    editor;
        std::vector<unsigned char>      bytes;
        // A component: its type, entity and descriptor; the copy is refreshed every frame.
        Entity                          entity = NullEntity;
        std::type_index                 type = std::type_index(typeid(void));
        const Reflect::TypeDescriptor*  desc = nullptr;
        // A file: its path, read once (at most kMaxFileBytes).
        std::string                     path;
        std::string                     title;
    };

    constexpr std::size_t kMaxFileBytes = 4u << 20;

    ByteView& View()
    {
        static ByteView s_view;
        return s_view;
    }

    // The field whose bytes hold @p off (top-level data fields only).
    const Reflect::FieldDescriptor* FieldAt(const Reflect::TypeDescriptor& d, std::size_t off)
    {
        for (const Reflect::FieldDescriptor& f : d.fields)
            if (!Reflect::IsLayout(f.kind) && f.size && off >= f.offset && off < f.offset + f.size) return &f;
        return nullptr;
    }

    ImU32 FieldShade(const ImU8*, size_t off, void* user)
    {
        const auto* d = static_cast<const Reflect::TypeDescriptor*>(user);
        const Reflect::FieldDescriptor* f = d ? FieldAt(*d, off) : nullptr;
        if (!f) return 0;
        // Alternating shades by field order, so neighbours are told apart.
        const std::size_t index = static_cast<std::size_t>(f - d->fields.data());
        return (index & 1) ? IM_COL32(70, 110, 160, 90) : IM_COL32(160, 120, 60, 90);
    }
}

void EditorLayer::OpenByteViewForComponent(Entity e, std::type_index type)
{
    ByteView& v = View();
    v.entity = e;
    v.type = type;
    const Reflect::RuntimeEntry* r = Reflect::FindRuntime(type);
    v.desc = r ? r->desc : nullptr;
    v.path.clear();
    v.title = ComponentLabelFor(type) + " of " + (m_world && m_world->IsAlive(e) ? m_world->GetName(e) : std::string("?"));
    m_showByteView = true;
}

void EditorLayer::OpenByteViewForFile(const std::string& path)
{
    ByteView& v = View();
    v.entity = NullEntity;
    v.desc = nullptr;
    v.path = path;
    v.title = path;
    v.bytes.clear();
    std::ifstream f(Sys::U8Path(path), std::ios::binary);
    v.bytes.resize(kMaxFileBytes);
    f.read(reinterpret_cast<char*>(v.bytes.data()), static_cast<std::streamsize>(v.bytes.size()));
    v.bytes.resize(static_cast<std::size_t>(f.gcount()));
    m_showByteView = true;
}

void EditorLayer::TickByteView()
{
    // The component's copy, at the frame boundary (never mid-ImGui).
    ByteView& v = View();
    if (!m_showByteView || v.entity == NullEntity || !m_world) return;
    if (!m_world->IsAlive(v.entity)) { v.bytes.clear(); return; }
    const std::shared_ptr<void> copy = m_world->CloneComponentByType(v.entity, v.type);
    const std::size_t size = v.desc ? v.desc->size : 0;
    if (!copy || size == 0) { v.bytes.clear(); return; }
    v.bytes.assign(static_cast<const unsigned char*>(copy.get()), static_cast<const unsigned char*>(copy.get()) + size);
}

void EditorLayer::RenderByteViewWindow()
{
    ImGui::SetNextWindowSize(ImVec2(820.f, 480.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Byte View", &m_showByteView))
    {
        ImGui::End();
        return;
    }
    ByteView& v = View();
    v.editor.ReadOnly = true;
    v.editor.OptShowOptions = true;
    ImGui::TextUnformatted(v.title.empty() ? "(nothing: right-click a component header or an asset > View Bytes)"
                                           : v.title.c_str());
    if (v.bytes.empty())
    {
        ImGui::TextDisabled(v.entity != NullEntity ? "The component is gone." : "No bytes.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%zu byte(s), read-only", v.bytes.size());

    if (v.desc)
    {
        // The fields beside the bytes: offset, size, value.
        v.editor.BgColorFn = &FieldShade;
        v.editor.UserData = const_cast<Reflect::TypeDescriptor*>(v.desc);
        ImGui::BeginChild("##byteFields", ImVec2(300.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        for (const Reflect::FieldDescriptor& f : v.desc->fields)
        {
            if (Reflect::IsLayout(f.kind) || f.size == 0) continue;
            Reflect::PropertyPath p;
            p.type = v.desc;
            p.steps.push_back({ &f, -1, -1 });
            const std::string text = Reflect::Writable(p) ? Reflect::ToText(Reflect::Get(v.bytes.data(), p)) : std::string("...");
            ImGui::Text("%04zx +%-3zu %s", f.offset, f.size, f.name);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s = %s", Reflect::KeyOf(f), text.c_str());
            if (ImGui::IsItemClicked()) v.editor.GotoAddrAndHighlight(f.offset, f.offset + f.size);
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    else
    {
        v.editor.BgColorFn = nullptr;
        v.editor.UserData = nullptr;
        // An asset container: the header's magic and lengths.
        if (v.bytes.size() >= 16)
        {
            char magic[5] = {};
            std::memcpy(magic, v.bytes.data(), 4);
            std::uint32_t version = 0, dataSize = 0;
            std::memcpy(&version, v.bytes.data() + 4, 4);
            std::memcpy(&dataSize, v.bytes.data() + 12, 4);
            ImGui::TextDisabled("header: magic '%s', version %u, data size %u (AssetHeader; the text payload at 40)",
                                magic, version, dataSize);
        }
    }
    ImGui::BeginChild("##byteHex");
    v.editor.DrawContents(v.bytes.data(), v.bytes.size());
    ImGui::EndChild();
    ImGui::End();
}
