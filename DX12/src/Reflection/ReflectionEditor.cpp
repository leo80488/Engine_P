#include "Editor/ColorWidgets.h"
#include "Reflection/ReflectionEditor.h"

#include "Reflection/ReflectDrawers.h"
#include "Reflection/Property.h"
#include "Editor/PropertyRow.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // ImGuiItemFlags_MixedValue
#include "Editor/ImGuiFlow.h"   // ChainPreview UI layout: wrapped REFLECT_INFO

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <limits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace Reflect
{
namespace
{
    inline void* FieldPtr(void* obj, std::size_t off)
    {
        return static_cast<unsigned char*>(obj) + off;
    }

    // Read an integer of `size` bytes (1/2/4) into a widened int. Used for
    // enum fields whose underlying type may be uint8_t / int / uint32_t.
    inline int ReadEnumInt(const void* p, std::size_t size)
    {
        switch (size)
        {
            case 1: return static_cast<int>(*static_cast<const uint8_t*>(p));
            case 2: return static_cast<int>(*static_cast<const uint16_t*>(p));
            case 4: return *static_cast<const int*>(p);
            default: return 0;
        }
    }

    // Narrow `value` and write into the enum-sized field.
    inline void WriteEnumInt(void* p, std::size_t size, int value)
    {
        switch (size)
        {
            case 1: *static_cast<uint8_t*>(p)  = static_cast<uint8_t>(value);  break;
            case 2: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(value); break;
            case 4: *static_cast<int*>(p)      = value;                        break;
            default: break;
        }
    }

    EntityNameFn g_entityName = nullptr;
    bool         g_showAdvanced = false;
    const std::vector<const FieldDescriptor*>* g_mixed = nullptr;
    int          g_selected = 1;
    std::string  g_tag;
    double       g_luaEntity = 0.0;
    AssetListFn  g_assetLister = nullptr;
    bool         g_rowActionWrote = false;
    // The object DrawObject is walking at the top level, for a field's default and its path.
    const TypeDescriptor* g_topType = nullptr;
    const void*           g_topObj = nullptr;
    int                   g_depth = 0;

    bool IsMixed(const FieldDescriptor& f)
    {
        if (!g_mixed) return false;
        for (const FieldDescriptor* m : *g_mixed)
            if (m == &f) return true;
        return false;
    }

    // A numeric field's format: its own, else "%.3f" with the unit's suffix ("%.3f kg").
    const char* NumberFormat(const FieldDescriptor& f, const char* fallback, char* buf, std::size_t cap)
    {
        if (f.fmt) return f.fmt;
        const char* suffix = UnitSuffix(f.unit);
        if (!suffix) return fallback;
        std::snprintf(buf, cap, "%s %s", fallback, suffix);
        return buf;
    }

    // Keeps an edited value inside the field's hard range (the drag range can be typed past).
    template <class T>
    void ClampHard(T* v, std::size_t n, const FieldDescriptor& f)
    {
        if (!f.hasHard) return;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double x = static_cast<double>(v[i]);
            if (x < f.hardMin) v[i] = static_cast<T>(f.hardMin);
            else if (x > f.hardMax) v[i] = static_cast<T>(f.hardMax);
        }
    }

    bool DrawFloat(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        char fmtBuf[48];
        const char* fmt = NumberFormat(f, "%.3f", fmtBuf, sizeof(fmtBuf));
        if (f.widget == Reflect::Widget::Angle)
        {
            float deg = DirectX::XMConvertToDegrees(*v);
            if (ImGui::DragFloat(f.label, &deg, 0.1f,
                                 f.minVal, f.maxVal, "%.1f deg"))
            {
                *v = DirectX::XMConvertToRadians(deg);
                return true;
            }
            return false;
        }
        // Default = Drag.
        const bool changed = ImGui::DragFloat(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
        if (changed) ClampHard(v, 1, f);
        return changed;
    }

    bool DrawFloat2(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        char fmtBuf[48];
        const char* fmt = NumberFormat(f, "%.3f", fmtBuf, sizeof(fmtBuf));
        const bool changed = ImGui::DragFloat2(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
        if (changed) ClampHard(v, 2, f);
        return changed;
    }

    bool DrawFloat3(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        bool changed = false;
        if (f.widget == Reflect::Widget::Color)
            changed = EditorUI::ColorEdit3(f.label, v);
        else
        {
            char fmtBuf[48];
            const char* fmt = NumberFormat(f, "%.3f", fmtBuf, sizeof(fmtBuf));
            changed = ImGui::DragFloat3(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
        }
        if (changed) ClampHard(v, 3, f);
        return changed;
    }

    bool DrawFloat4(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        bool changed = false;
        if (f.widget == Reflect::Widget::Color)
            changed = EditorUI::ColorEdit4(f.label, v);
        else
        {
            char fmtBuf[48];
            const char* fmt = NumberFormat(f, "%.3f", fmtBuf, sizeof(fmtBuf));
            changed = ImGui::DragFloat4(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
        }
        if (changed) ClampHard(v, 4, f);
        return changed;
    }

    bool DrawInt(void* p, const FieldDescriptor& f)
    {
        int* v = static_cast<int*>(p);
        const int mn = static_cast<int>(f.minVal);
        const int mx = static_cast<int>(f.maxVal);
        const bool changed = ImGui::DragInt(f.label, v, f.speed, mn, mx);
        if (changed) ClampHard(v, 1, f);
        return changed;
    }

    // uint8_t / uint16_t through an int: their whole range round-trips.
    template <class T>
    bool DrawSmallUInt(void* p, const FieldDescriptor& f)
    {
        T* v = static_cast<T*>(p);
        int tmp = static_cast<int>(*v);
        const bool ranged = !(f.minVal == 0.f && f.maxVal == 0.f);
        const int mn = ranged ? static_cast<int>(f.minVal) : 0;
        const int mx = ranged ? static_cast<int>(f.maxVal) : static_cast<int>((std::numeric_limits<T>::max)());
        const bool changed = ImGui::DragInt(f.label, &tmp, f.speed, mn, mx);
        if (changed)
        {
            const int hi = static_cast<int>((std::numeric_limits<T>::max)());
            *v = static_cast<T>(tmp < 0 ? 0 : (tmp > hi ? hi : tmp));
            ClampHard(v, 1, f);
        }
        return changed;
    }

    bool DrawInt64(void* p, const FieldDescriptor& f)
    {
        auto* v = static_cast<int64_t*>(p);
        const bool ranged = !(f.minVal == 0.f && f.maxVal == 0.f);
        const int64_t mn = static_cast<int64_t>(f.minVal), mx = static_cast<int64_t>(f.maxVal);
        const bool changed = ImGui::DragScalar(f.label, ImGuiDataType_S64, v, f.speed, ranged ? &mn : nullptr,
                                               ranged ? &mx : nullptr);
        if (changed) ClampHard(v, 1, f);
        return changed;
    }

    bool DrawDouble(void* p, const FieldDescriptor& f)
    {
        auto* v = static_cast<double*>(p);
        const bool ranged = !(f.minVal == 0.f && f.maxVal == 0.f);
        const double mn = f.minVal, mx = f.maxVal;
        char fmtBuf[48];
        const char* fmt = NumberFormat(f, "%.4f", fmtBuf, sizeof(fmtBuf));
        const bool changed = ImGui::DragScalar(f.label, ImGuiDataType_Double, v, f.speed, ranged ? &mn : nullptr,
                                               ranged ? &mx : nullptr, fmt);
        if (changed) ClampHard(v, 1, f);
        return changed;
    }

    bool DrawUInt(void* p, const FieldDescriptor& f)
    {
        // ImGui has no DragUInt: edit through a signed int, so only 0..INT_MAX round-trips; a negative
        // result clamps to 0.
        unsigned* v  = static_cast<unsigned*>(p);
        int       tmp = static_cast<int>(*v);
        const int mn  = static_cast<int>(f.minVal);
        const int mx  = static_cast<int>(f.maxVal);
        bool changed = ImGui::DragInt(f.label, &tmp, f.speed, mn, mx);
        if (changed)
        {
            *v = static_cast<unsigned>(tmp < 0 ? 0 : tmp);
            ClampHard(v, 1, f);
        }
        return changed;
    }

    bool DrawBool(void* p, const FieldDescriptor& f)
    {
        bool* v = static_cast<bool*>(p);
        return ImGui::Checkbox(f.label, v);
    }

    bool DrawEnum(void* p, const FieldDescriptor& f)
    {
        if (!f.enumOpts || f.enumCount == 0) return false;

        const int current = ReadEnumInt(p, f.enumSize);
        // Find current option to show its label as the combo preview.
        const char* preview = "<unknown>";
        int currentIdx = -1;
        for (std::size_t i = 0; i < f.enumCount; ++i)
            if (f.enumOpts[i].value == current) { preview = f.enumOpts[i].label; currentIdx = (int)i; break; }

        bool changed = false;
        if (ImGui::BeginCombo(f.label, preview))
        {
            for (std::size_t i = 0; i < f.enumCount; ++i)
            {
                const bool selected = ((int)i == currentIdx);
                if (ImGui::Selectable(f.enumOpts[i].label, selected))
                {
                    WriteEnumInt(p, f.enumSize, f.enumOpts[i].value);
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool DrawString(void* p, const FieldDescriptor& f)
    {
        // Fixed scratch-buffer round-trip (no imgui_stdlib in this project); 512 bytes covers every
        // reflected string field (paths, labels).
        constexpr std::size_t kCap = 512;
        auto* str = static_cast<std::string*>(p);
        char buf[kCap];
        const std::size_t n = (str->size() < kCap - 1) ? str->size() : (kCap - 1);
        std::memcpy(buf, str->data(), n);
        buf[n] = '\0';
        bool changed = false;
        if (ImGui::InputText(f.label, buf, kCap))
        {
            *str = buf;
            changed = true;
        }
        // Optional drop target: the dropped payload (a null-terminated path) replaces the string.
        if (f.dropPayload && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(f.dropPayload))
            {
                *str = static_cast<const char*>(p->Data);
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

    // An asset path: a "..." button that lists the assets taking the field's payload, then the text
    // field (which also takes the drop).
    bool DrawAssetRef(void* p, const FieldDescriptor& f)
    {
        bool changed = false;
        if (g_assetLister && f.dropPayload)
        {
            ImGui::PushID(f.label);
            if (ImGui::SmallButton("...")) ImGui::OpenPopup("##assetpick");
            const float buttonW = ImGui::GetItemRectSize().x;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Pick an asset");
            if (ImGui::BeginPopup("##assetpick"))
            {
                static char s_filter[64] = {};
                if (ImGui::IsWindowAppearing()) s_filter[0] = '\0';
                ImGui::SetNextItemWidth(260.f);
                ImGui::InputTextWithHint("##assetfilter", "Filter", s_filter, sizeof(s_filter));
                auto* str = static_cast<std::string*>(p);
                const std::vector<std::string> assets = g_assetLister(f.dropPayload);
                EditorUI::RowFilter filter;
                filter.text = s_filter;
                int shown = 0;
                ImGui::BeginChild("##assetlist", ImVec2(360.f, 240.f));
                if (ImGui::Selectable("(none)", str->empty())) { str->clear(); changed = true; ImGui::CloseCurrentPopup(); }
                for (const std::string& a : assets)
                {
                    if (!filter.Matches(a.c_str(), nullptr, nullptr)) continue;
                    ++shown;
                    if (ImGui::Selectable(a.c_str(), a == *str))
                    {
                        *str = a;
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (shown == 0) ImGui::TextDisabled("No asset takes %s", f.dropPayload);
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - buttonW - ImGui::GetStyle().ItemInnerSpacing.x);
        }
        changed |= DrawString(p, f);
        return changed;
    }

    uint32_t ReadBits(const void* p, std::size_t size)
    {
        switch (size)
        {
            case 1: return *static_cast<const uint8_t*>(p);
            case 2: return *static_cast<const uint16_t*>(p);
            case 4: return *static_cast<const uint32_t*>(p);
            default: return 0;
        }
    }

    void WriteBits(void* p, std::size_t size, uint32_t bits)
    {
        switch (size)
        {
            case 1: *static_cast<uint8_t*>(p)  = static_cast<uint8_t>(bits);  break;
            case 2: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(bits); break;
            case 4: *static_cast<uint32_t*>(p) = bits;                        break;
            default: break;
        }
    }

    // One checkbox per bit; options labelled "Group/Name" with the same group share a row behind the
    // group's name. Presets are buttons; a 32-bit set shows its raw value for people editing files.
    bool DrawFlags(void* p, const FieldDescriptor& f)
    {
        uint32_t bits = ReadBits(p, f.enumSize);
        const uint32_t before = bits;
        ImGui::PushID(f.label);
        ImGui::TextUnformatted(f.label);
        const char* rowGroup = nullptr;
        std::size_t rowGroupLen = 0;
        for (std::size_t i = 0; i < f.enumCount; ++i)
        {
            const EnumOption& o = f.enumOpts[i];
            const char* slash = o.label ? std::strchr(o.label, '/') : nullptr;
            const char* name = slash ? slash + 1 : o.label;
            if (slash)
            {
                const std::size_t len = static_cast<std::size_t>(slash - o.label);
                const bool sameRow = rowGroup && len == rowGroupLen && std::strncmp(rowGroup, o.label, len) == 0;
                if (sameRow) ImGui::SameLine();
                else
                {
                    ImGui::Text("%.*s:", static_cast<int>(len), o.label);
                    ImGui::SameLine();
                    rowGroup = o.label;
                    rowGroupLen = len;
                }
            }
            else rowGroup = nullptr;
            const uint32_t mask = static_cast<uint32_t>(o.value);
            bool on = (bits & mask) == mask && mask != 0;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox(name, &on)) bits = on ? (bits | mask) : (bits & ~mask);
            ImGui::PopID();
        }
        for (std::size_t i = 0; i < f.presetCount; ++i)
        {
            const FlagPreset& preset = f.presets[i];
            if (i > 0) ImGui::SameLine();
            ImGui::PushID(1000 + static_cast<int>(i));
            if (ImGui::SmallButton(preset.label)) bits = preset.add ? (bits | preset.value) : preset.value;
            ImGui::PopID();
        }
        if (f.enumSize == 4) ImGui::TextDisabled("Raw: 0x%08X", bits);
        ImGui::PopID();
        if (bits == before) return false;
        WriteBits(p, f.enumSize, bits);
        return true;
    }

    // Quaternion -> Euler degrees with a per-field cache, so dragging stays smooth instead of
    // re-deriving the angles every frame (which loses precision near gimbal lock).
    bool DrawQuat(void* p, const FieldDescriptor& f)
    {
        struct Cache { DirectX::XMFLOAT4 q; DirectX::XMFLOAT3 e; };
        static std::unordered_map<void*, Cache> s_cache;
        auto* q = static_cast<DirectX::XMFLOAT4*>(p);
        Cache& c = s_cache[p];
        if (q->x != c.q.x || q->y != c.q.y || q->z != c.q.z || q->w != c.q.w)
        {
            using namespace DirectX;
            const XMVECTOR qv = XMLoadFloat4(q);
            const XMMATRIX m  = XMMatrixRotationQuaternion(qv);
            // Euler degrees (x = pitch, y = yaw, z = roll) as the exact inverse of the write below
            // (XMQuaternionRotationRollPitchYaw = Rz * Rx * Ry, row-vector):
            //   row2 = (cp*sy, -sp, cp*cy) -> _31 = cp*sy, _32 = -sp, _33 = cp*cy
            //   row0.y = sr*cp = _12, row1.y = cr*cp = _22
            // so pitch = asin(-_32), yaw = atan2(_31, _33), roll = atan2(_12, _22). SequenceEval uses the same
            // extraction. Do not change one side alone: a non-inverse shows a straight-down Spot as yaw -90,
            // and nudging the field then writes that back and swings the light horizontal.
            const float sp = -m.r[2].m128_f32[1];                       // -_32
            const float clamped = sp > 0.999999f ? 0.999999f : (sp < -0.999999f ? -0.999999f : sp);
            const float ex = asinf(clamped);
            float ey, ez;
            if (fabsf(clamped) > 0.999999f)
            {
                // Gimbal lock (pitch = +-90 deg): yaw and roll are not separable, so fold everything into yaw,
                // as SequenceEval does.
                ey = atan2f(-m.r[0].m128_f32[2], m.r[0].m128_f32[0]);   // -_13, _11
                ez = 0.f;
            }
            else
            {
                ey = atan2f(m.r[2].m128_f32[0], m.r[2].m128_f32[2]);    // _31, _33
                ez = atan2f(m.r[0].m128_f32[1], m.r[1].m128_f32[1]);    // _12, _22
            }
            c.e.x = XMConvertToDegrees(ex);
            c.e.y = XMConvertToDegrees(ey);
            c.e.z = XMConvertToDegrees(ez);
            c.q = *q;
        }
        if (ImGui::DragFloat3(f.label, &c.e.x, 0.5f))
        {
            using namespace DirectX;
            const XMVECTOR qv = XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(c.e.x),
                XMConvertToRadians(c.e.y),
                XMConvertToRadians(c.e.z));
            XMStoreFloat4(q, qv);
            c.q = *q;
            return true;
        }
        return false;
    }

    // "#id name", a drop target for an Outliner row, and a clear button.
    bool DrawEntityRef(void* p, const FieldDescriptor& f)
    {
        auto* id = static_cast<uint32_t*>(p);
        bool changed = false;
        ImGui::PushID(f.label);
        const char* name = (*id != 0 && g_entityName) ? g_entityName(*id) : nullptr;
        char text[160];
        if (*id == 0) std::snprintf(text, sizeof(text), "(none)");
        else std::snprintf(text, sizeof(text), "#%u %s", *id, name ? name : "");
        ImGui::Button(text, ImVec2(ImGui::CalcItemWidth(), 0.f));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY"))
            {
                if (payload->DataSize == sizeof(uint32_t))
                {
                    std::memcpy(id, payload->Data, sizeof(uint32_t));
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x") && *id != 0)
        {
            *id = 0;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(f.label);
        ImGui::PopID();
        return changed;
    }

}

// Forward declaration: DrawObject and DrawField recurse (vector elements are objects). Elements
// are drawn without edit-signal tracking; the parent Vector field's own item activity, sampled in
// DrawField, brackets a vector edit.
bool DrawObject(void* obj, const TypeDescriptor& desc, EditSignal* sig);

namespace
{
    bool DrawVectorImpl(void* obj, const FieldDescriptor& f)
    {
        // The vector field lives at obj+f.offset; for std::vector that's the
        // vector itself, for fixed arrays it's the array base.
        void* vecOrArr = static_cast<unsigned char*>(obj) + f.offset;
        const TypeDescriptor* ed = f.elementDesc;
        if (!ed) return false;

        const bool isArray = (f.vecSize == nullptr);
        std::size_t count = 0;
        std::size_t cap   = 0;

        if (isArray)
        {
            void* countPtr = static_cast<unsigned char*>(obj) + f.countOffset;
            count = f.countIsInt
                ? static_cast<std::size_t>(*static_cast<int*>(countPtr))
                : static_cast<std::size_t>(*static_cast<uint32_t*>(countPtr));
            cap = f.arrayCapacity;
        }
        else
        {
            count = f.vecSize(vecOrArr);
        }

        bool changed = false;
        ImGui::Text("%s (%zu)", f.label, count);
        ImGui::SameLine();
        const bool canAdd = isArray ? (count < cap) : true;
        if (canAdd && ImGui::SmallButton("+"))
        {
            if (isArray)
            {
                // Zero-fill the next slot and bump count: T is unknown here, so there is no placement-new. Only
                // valid for POD element types whose all-zero state is a valid default.
                void* slot = f.vecData(vecOrArr, count);
                std::memset(slot, 0, ed->size);
                void* countPtr = static_cast<unsigned char*>(obj) + f.countOffset;
                if (f.countIsInt) ++(*static_cast<int*>(countPtr));
                else              ++(*static_cast<uint32_t*>(countPtr));
                ++count;
            }
            else
            {
                f.vecAdd(vecOrArr);
                ++count;
            }
            changed = true;
        }

        int removeIdx = -1;
        for (std::size_t i = 0; i < count; ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            char hdr[32];
            std::snprintf(hdr, sizeof(hdr), "[%zu]", i);
            if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen))
            {
                void* elem = f.vecData(vecOrArr, i);
                changed |= DrawObject(elem, *ed);
                if (ImGui::SmallButton("Remove")) removeIdx = static_cast<int>(i);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIdx >= 0)
        {
            if (isArray)
            {
                // Shift down and decrement count; the element type is treated as trivially copyable.
                for (std::size_t j = (std::size_t)removeIdx; j + 1 < count; ++j)
                {
                    void* dst = f.vecData(vecOrArr, j);
                    void* src = f.vecData(vecOrArr, j + 1);
                    std::memcpy(dst, src, ed->size);
                }
                void* countPtr = static_cast<unsigned char*>(obj) + f.countOffset;
                if (f.countIsInt) --(*static_cast<int*>(countPtr));
                else              --(*static_cast<uint32_t*>(countPtr));
            }
            else
            {
                f.vecRemove(vecOrArr, (std::size_t)removeIdx);
            }
            changed = true;
        }
        return changed;
    }
}

namespace { int g_infoWrap = 0; bool g_forceOpen = false; }   // Chain Physics Preview layout state
void PushInfoWrap() { ++g_infoWrap; }
void PopInfoWrap()  { if (g_infoWrap > 0) --g_infoWrap; }
void SetForceOpenSections(bool on) { g_forceOpen = on; }

void SetShowAdvanced(bool on) { g_showAdvanced = on; }
void SetMultiSelection(const std::vector<const FieldDescriptor*>* mixedFields, int selected)
{
    g_mixed = mixedFields;
    g_selected = selected < 1 ? 1 : selected;
}
bool ShowAdvanced() { return g_showAdvanced; }
void SetEntityNameResolver(EntityNameFn fn) { g_entityName = fn; }
void SetDrawContext(const char* tag, double luaEntity)
{
    g_tag = tag ? tag : "";
    g_luaEntity = luaEntity;
}
void SetAssetLister(AssetListFn fn) { g_assetLister = fn; }
bool ConsumeRowActionEdit()
{
    const bool wrote = g_rowActionWrote;
    g_rowActionWrote = false;
    return wrote;
}

const char* UnitSuffix(const char* unit)
{
    if (!unit || !*unit) return nullptr;
    static const std::pair<const char*, const char*> kSuffixes[] = {
        { "kg", "kg" }, { "per_second", "/s" }, { "world_unit", "m" }, { "seconds", "s" }, { "px", "px" },
        { "reference_px", "px" }, { "texture_px", "px" }, { "hz", "Hz" }, { "multiplier", "x" },
    };
    for (const auto& [name, suffix] : kSuffixes)
        if (std::strcmp(unit, name) == 0) return suffix;
    return nullptr;
}

namespace
{
    bool DrawStruct(void* p, const FieldDescriptor& f)
    {
        if (!f.elementDesc) return false;
        bool changed = false;
        if (ImGui::TreeNodeEx(f.label, ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed = DrawObject(p, *f.elementDesc);
            ImGui::TreePop();
        }
        return changed;
    }

    // The hover text: the tooltip, then the key and unit tools and files use.
    void FieldTooltip(const FieldDescriptor& f)
    {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
        const bool hasTip = f.tooltip && *f.tooltip;
        const bool hasKey = f.key && *f.key;
        const bool thisOnly = g_selected > 1 && Has(f, FieldFlag::NoMultiEdit);
        const bool mixed = IsMixed(f);
        if (!hasTip && !hasKey && !f.unit && !thisOnly && !mixed) return;
        ImGui::BeginTooltip();
        if (hasTip) ImGui::TextUnformatted(f.tooltip);
        if (mixed) ImGui::TextDisabled("Mixed: the %d selected entities differ; an edit sets them all.", g_selected);
        if (thisOnly) ImGui::TextDisabled("Applies to this entity only, not the other %d selected.", g_selected - 1);
        if (hasKey || f.unit)
            ImGui::TextDisabled("key %s%s%s", KeyOf(f), f.unit ? "  unit " : "", f.unit ? f.unit : "");
        ImGui::EndTooltip();
    }

    // The single-step path of a top-level field (its key, or its C++ name for a split key).
    PropertyPath FieldPath(const FieldDescriptor& f)
    {
        PropertyPath path;
        path.type = g_topType;
        path.steps.push_back({ &f, -1, -1 });
        const char* key = KeyOf(f);
        path.text = std::strchr(key, ',') ? f.name : key;
        return path;
    }

    // Whether the top-level field holds a value other than its type's default.
    bool FieldModified(const FieldDescriptor& f, const void* obj)
    {
        const void* defaults = g_topType ? DefaultInstance(*g_topType) : nullptr;
        if (!defaults || !obj) return false;
        const PropertyPath path = FieldPath(f);
        if (!Writable(path)) return false;
        return !Equal(Get(obj, path), Get(defaults, path));
    }

    // The row chrome of a top-level field: the modified marker and the right-click menu; a paste or a
    // reset writes through WriteProperty and counts as the widget's change.
    bool FieldChrome(const FieldDescriptor& f, void* obj)
    {
        if (g_depth != 1 || !g_topType || g_topObj != obj) return false;
        const PropertyPath path = FieldPath(f);
        if (!Writable(path)) return false;
        EditorUI::PropertyRowInfo row;
        row.path = g_tag.empty() ? path.text : g_tag + "." + path.text;
        const PropertyValue value = Get(obj, path);
        row.valueText = ToText(value);
        row.modified = FieldModified(f, obj);
        row.writable = !Has(f, FieldFlag::ReadOnly);
        if (g_luaEntity != 0.0 && !g_tag.empty())
        {
            char id[32];
            std::snprintf(id, sizeof(id), "%.0f", g_luaEntity);
            row.luaSetter = std::string("Editor.SetProperty(") + id + ", \"" + row.path + "\", " +
                            EditorUI::LuaLiteral(row.valueText, std::holds_alternative<std::string>(value)) + ")";
        }
        std::string pasted;
        switch (EditorUI::PropertyRowChrome(row, pasted))
        {
        case EditorUI::RowAction::Paste:
        {
            PropertyValue v;
            const bool wrote = FromText(path, pasted, v) && WriteProperty(obj, path, v, WriteOrigin::Inspector).ok;
            g_rowActionWrote |= wrote;
            return wrote;
        }
        case EditorUI::RowAction::Reset:
        {
            const bool wrote = ResetToDefault(obj, path, WriteOrigin::Inspector).ok;
            g_rowActionWrote |= wrote;
            return wrote;
        }
        default:
            return false;
        }
    }

    // Whether the active row filter shows the data field @p f of @p obj.
    bool FilterShows(const FieldDescriptor& f, const void* obj)
    {
        const EditorUI::RowFilter* filter = EditorUI::ActiveRowFilter();
        if (!filter || !filter->Active() || g_depth != 1) return true;
        return filter->Shows(f.label, KeyOf(f), f.tooltip, FieldModified(f, obj));
    }
}

bool AnyFieldShown(const void* obj, const TypeDescriptor& desc)
{
    const EditorUI::RowFilter* filter = EditorUI::ActiveRowFilter();
    if (!filter || !filter->Active()) return true;
    const TypeDescriptor* type = g_topType;
    const void* top = g_topObj;
    const int depth = g_depth;
    g_topType = &desc;
    g_topObj = obj;
    g_depth = 1;
    bool any = false;
    for (const FieldDescriptor& f : desc.fields)
        if (!IsLayout(f.kind) && !Has(f, FieldFlag::Hidden) && FilterShows(f, obj)) { any = true; break; }
    g_topType = type;
    g_topObj = top;
    g_depth = depth;
    return any;
}

bool DrawField(void* obj, const FieldDescriptor& f, EditSignal* sig)
{
    void* fp = static_cast<unsigned char*>(obj) + f.offset;

    if (Has(f, FieldFlag::Hidden)) return false;
    if (Has(f, FieldFlag::Advanced) && !g_showAdvanced) return false;
    const bool readOnly = Has(f, FieldFlag::ReadOnly);
    if (readOnly) ImGui::BeginDisabled();
    // A mixed value: checkboxes draw their "-" state, value frames take a tint.
    const bool mixed = IsMixed(f);
    if (mixed)
    {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        ImVec4 tint = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        tint.x = tint.x * 0.6f + 0.25f;
        tint.y = tint.y * 0.6f + 0.18f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, tint);
    }

    bool changed = false;
    switch (f.kind)
    {
        case FieldKind::Header:
            ImGui::Separator();
            if (f.label && *f.label) ImGui::TextUnformatted(f.label);
            if (readOnly) ImGui::EndDisabled();
            return false;

        case FieldKind::InfoText:
            if (f.label && *f.label)
            {
                if (g_infoWrap > 0) EditorUI::Flow::DisabledWrapped("%s", f.label);
                else                ImGui::TextDisabled("%s", f.label);
            }
            if (readOnly) ImGui::EndDisabled();
            return false;

        case FieldKind::InfoTextFn:
        {
            char buf[256] = {};
            if (f.infoFn) f.infoFn(obj, buf, sizeof(buf));
            if (g_infoWrap > 0)
            {
                if (f.label && *f.label) EditorUI::Flow::DisabledWrapped("%s: %s", f.label, buf);
                else                     EditorUI::Flow::DisabledWrapped("%s", buf);
            }
            else if (f.label && *f.label) ImGui::TextDisabled("%s: %s", f.label, buf);
            else                          ImGui::TextDisabled("%s", buf);
            if (readOnly) ImGui::EndDisabled();
            return false;
        }

        case FieldKind::Custom:
            if (const DrawerFn draw = FindDrawer(f.drawer)) changed = draw(fp, f);
            else ImGui::TextDisabled("%s: no editor drawer '%s'", f.label, f.drawer ? f.drawer : "");
            break;

        case FieldKind::Vector:
            changed = DrawVectorImpl(obj, f);
            break;

        case FieldKind::F32:    changed = DrawFloat (fp, f); break;
        case FieldKind::F32x2:  changed = DrawFloat2(fp, f); break;
        case FieldKind::F32x3:  changed = DrawFloat3(fp, f); break;
        case FieldKind::F32x4:  changed = DrawFloat4(fp, f); break;
        case FieldKind::I32:    changed = DrawInt   (fp, f); break;
        case FieldKind::U32:    changed = DrawUInt  (fp, f); break;
        case FieldKind::Bool:   changed = DrawBool  (fp, f); break;
        case FieldKind::Enum:   changed = DrawEnum  (fp, f); break;
        case FieldKind::String: changed = DrawString(fp, f); break;
        case FieldKind::AssetRef: changed = DrawAssetRef(fp, f); break;
        case FieldKind::U8:     changed = DrawSmallUInt<uint8_t>(fp, f); break;
        case FieldKind::U16:    changed = DrawSmallUInt<uint16_t>(fp, f); break;
        case FieldKind::I64:    changed = DrawInt64 (fp, f); break;
        case FieldKind::F64:    changed = DrawDouble(fp, f); break;
        case FieldKind::Flags:  changed = DrawFlags (fp, f); break;
        case FieldKind::Quat:   changed = DrawQuat  (fp, f); break;
        case FieldKind::Struct: changed = DrawStruct(fp, f); break;
        case FieldKind::EntityRef: changed = DrawEntityRef(fp, f); break;
        default: break;
    }
    if (mixed)
    {
        ImGui::PopStyleColor();
        ImGui::PopItemFlag();
    }
    if (readOnly)
    {
        ImGui::EndDisabled();
        changed = false;
    }

    // Sample the just-submitted widget's edit lifecycle for undo bracketing.
    // Pseudo-fields (Header/Info) returned early above, so this only runs for
    // real widgets; for non-interactive items these all read false.
    if (sig)
    {
        sig->changed   |= changed;
        sig->activated |= ImGui::IsItemActivated();
        sig->active    |= ImGui::IsItemActive();
    }

    if (!readOnly) changed |= FieldChrome(f, obj);
    if (sig) sig->changed |= changed;
    FieldTooltip(f);
    return changed;
}

bool DrawObject(void* obj, const TypeDescriptor& desc, EditSignal* sig)
{
    bool anyChanged = false;
    ImGui::PushID(desc.name);
    // The top-level object the row chrome and the filter read; nested objects (vector elements,
    // structs) draw without them.
    const TypeDescriptor* outerType = g_topType;
    const void* outerObj = g_topObj;
    if (g_depth == 0) { g_topType = &desc; g_topObj = obj; }
    ++g_depth;
    const EditorUI::RowFilter* rowFilter = EditorUI::ActiveRowFilter();
    const bool filtering = rowFilter && rowFilter->Active() && g_depth == 1;

    // Nesting depth for GroupBegin / CollapseBegin: hideDepth > 0 = inside a hidden block, closed by
    // the matching GroupEnd / CollapseEnd. Groups and collapses share it so they nest.
    int hideDepth = 0;

    for (const FieldDescriptor& f : desc.fields)
    {
        if (f.kind == FieldKind::GroupBegin)
        {
            const bool show = hideDepth == 0 && (!f.visible || f.visible(obj));
            if (!show) ++hideDepth;
            else if (hideDepth > 0) ++hideDepth;  // keep counting nested begins
            continue;
        }
        if (f.kind == FieldKind::GroupEnd)
        {
            if (hideDepth > 0) --hideDepth;
            continue;
        }
        // A search shows the matching fields flat: no headers or info lines, every section open.
        if (filtering && (f.kind == FieldKind::Header || f.kind == FieldKind::InfoText || f.kind == FieldKind::InfoTextFn))
            continue;
        if (filtering && f.kind == FieldKind::CollapseBegin)
        {
            ImGui::PushID(f.label);
            continue;
        }
        if (f.kind == FieldKind::CollapseBegin)
        {
            if (hideDepth > 0)
            {
                ++hideDepth;
                ImGui::PushID(f.label);
                continue;
            }
            const ImGuiTreeNodeFlags flags = (f.minVal > 0.f)
                ? ImGuiTreeNodeFlags_DefaultOpen : 0;
            if (g_forceOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);   // UI layout harness
            const bool open = ImGui::CollapsingHeader(f.label, flags);
            // Scope subsequent widget IDs by section label so sibling
            // collapses (e.g. "Spring Bones (Root)" / "(Child)") can reuse
            // the same field labels ("Stiffness", "Damping") without
            // colliding on ImGui's internal ID hash.
            ImGui::PushID(f.label);
            if (!open) ++hideDepth;
            continue;
        }
        if (f.kind == FieldKind::CollapseEnd)
        {
            if (hideDepth > 0 && !filtering) --hideDepth;
            ImGui::PopID();
            continue;
        }

        if (hideDepth > 0) continue;

        // Per-field visibility: gates a single field without opening a group.
        if (f.visible && !f.visible(obj)) continue;
        if (filtering && !IsLayout(f.kind) && !FilterShows(f, obj)) continue;

        anyChanged |= DrawField(obj, f, sig);
    }

    --g_depth;
    if (g_depth == 0) { g_topType = outerType; g_topObj = outerObj; }
    ImGui::PopID();
    return anyChanged;
}

} // namespace Reflect
