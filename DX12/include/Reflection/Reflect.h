#pragma once

// Reflect - minimal compile-time reflection for engine components: editor and tool code walks a
// component's fields by name, kind and metadata (range, colour hint, enum options) instead of
// repeating the property list per consumer.
// Declare a descriptor at namespace scope after the struct definition:
//     constexpr Reflect::EnumOption kBillboardModeOptions[] = {
//         { (int)BillboardMode::Opaque,      "Opaque" },
//         { (int)BillboardMode::Transparent, "Transparent" },
//     };
//     REFLECT_BEGIN(BillboardComponent)
//         REFLECT_ENUM (mode,      "Mode",       kBillboardModeOptions)
//         REFLECT_FLOAT(worldSize, "World Size", 0.01f, 100.f)
//         REFLECT_BOOL (fixedSize, "Fixed Size")
//     REFLECT_END()
// Reflect::Describe<BillboardComponent>() then returns the TypeDescriptor that the generic drawer
// (Reflection/ReflectionEditor.h) walks to build the inspector. Header-only, no run-time
// dependencies.
// Every value macro takes trailing attributes (Reflect::Attr), and REFLECT_FIELD deduces the kind
// from the member's type:
//     REFLECT_DRAG (linearDamping, "Linear Damping", 0.f, 1.f, Key("linDamp"), Unit("per_second"),
//                   Hard(0, kFloatMax))
//     REFLECT_FIELD(lockedAxes, "Constraints", FlagBits(kAxisLockBits), Presets(kAxisLockPresets))
// The attributes are the data contract the serializer, Lua, property paths and the catalog read:
// the key, legacy keys, unit, validation range, flags (Runtime, ReadOnly, LuaWrite, ...), category.

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace Reflect
{
struct FieldDescriptor;
struct TypeDescriptor;

// FieldKind: the data type stored in the field; drives how the drawer reads and writes raw
// memory at the offset. The widget hint then picks the ImGui control.
enum class FieldKind : uint8_t
{
    F32,            // float
    F32x2,          // DirectX::XMFLOAT2
    F32x3,          // DirectX::XMFLOAT3
    F32x4,          // DirectX::XMFLOAT4
    I32,            // int32_t
    U32,            // uint32_t
    Bool,           // bool
    Enum,           // 1/2/4-byte integer; the options describe its values
    String,         // std::string (optionally accepts drag-drop payload)
    Header,         // not a field: section divider with a label
    InfoText,       // not a field: TextDisabled line with the literal label
    InfoTextFn,     // not a field: TextDisabled line formatted by infoFn(obj)
    Vector,         // std::vector<T> or fixed array; elementDesc + vec*() ops type-erase T
    GroupBegin,     // hide subsequent fields until matching GroupEnd if visible() is false
    GroupEnd,
    CollapseBegin,  // wrap subsequent fields in an ImGui::CollapsingHeader
    CollapseEnd,
    Custom,         // drawn entirely by the editor drawer the field names (Reflection/ReflectDrawers.h)
    U8,             // uint8_t
    U16,            // uint16_t
    I64,            // int64_t
    F64,            // double
    Flags,          // a 1/2/4-byte bit set: enumOpts name the bits, presets are one-click values
    Quat,           // DirectX::XMFLOAT4 rotation (xyzw), edited as Euler degrees
    Struct,         // a nested struct held by value, walked through elementDesc
    EntityRef,      // an Entity id (uint32_t) of the same world
    AssetRef,       // std::string asset path; dropPayload is the browser payload it accepts
};

// FieldFlag: per-field policy the consumers read (inspector, serializer, Lua, undo, prefab, timeline).
enum class FieldFlag : uint32_t
{
    None        = 0,
    Runtime     = 1u << 0,   // run-time state: not saved; kept across a component rewrite
    ReadOnly    = 1u << 1,   // drawn disabled; writers refuse it
    Advanced    = 1u << 2,   // drawn only while the inspector shows advanced fields
    Hidden      = 1u << 3,   // saved, never drawn
    NoUndo      = 1u << 4,   // an edit pushes no undo entry (live tuning of run-time state)
    NoMultiEdit = 1u << 5,   // not applied to a multi-selection
    LuaRead     = 1u << 6,   // Entity.get reads it even when the component has no serializer
    LuaWrite    = 1u << 7,   // Entity.set may write it
    Overridable = 1u << 8,   // a prefab instance may override it
    Animatable  = 1u << 9,   // a Sequencer property track may drive it
    ClampHard   = 1u << 10,  // a write outside the hard range is clamped instead of refused
};

constexpr FieldFlag operator|(FieldFlag a, FieldFlag b)
{
    return static_cast<FieldFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// Widget: display hint chosen at descriptor time; several widgets can apply to one FieldKind
// (e.g. F32 -> Drag / Angle).
enum class Widget : uint8_t
{
    Default,    // Drag for numeric, Checkbox for bool, Combo for enum, InputText for string
    Drag,
    Color,      // F32x3 / F32x4 only: colour editor (EditorUI::ColorEdit3/4)
    Angle,      // F32 only: stored in radians, edited in degrees
    InputText,  // String only
    Combo,      // Enum (default)
    Checkbox,   // Bool (default)
};

// EnumOption: one entry of an enum option array. value is widened to int so one struct serves
// enums backed by uint8_t / uint32_t / int. Declare option arrays constexpr at namespace scope.
struct EnumOption
{
    int         value;
    const char* label;
};

// FlagPreset: a named value of a Flags field, offered as a button. add = OR it in instead of
// replacing the bits.
struct FlagPreset
{
    uint32_t    value;
    const char* label;
    bool        add = false;
};

// FieldDescriptor: one editable property of a component, stored by value in
// TypeDescriptor::fields. Every pointer member must reference static-storage data (string
// literals, constexpr arrays): descriptors live in function-local statics for the whole program.
struct FieldDescriptor
{
    const char*       name      = "";        // C++ member name (the key harness tools and the catalog dump look fields up by)
    const char*       label     = "";        // ImGui display label
    std::size_t       offset    = 0;         // byte offset from object base
    FieldKind         kind      = FieldKind::F32;
    Widget            widget    = Widget::Default;
    float             minVal    = 0.f;       // drag clamp; ignored for colour / bool
    float             maxVal    = 0.f;       // 0,0 => no clamp (drag widgets)
    float             speed     = 0.01f;     // drag step
    const char*       fmt       = nullptr;   // printf format for numeric (nullptr = ImGui default)
    const char*       tooltip   = nullptr;   // optional hover tooltip
    std::size_t       enumSize  = 0;         // sizeof(EnumType): 1/2/4 bytes
    const EnumOption* enumOpts  = nullptr;   // null when kind != Enum
    std::size_t       enumCount = 0;

    // Drag-drop payload type accepted by string fields. nullptr disables drop.
    const char*       dropPayload = nullptr;

    // Conditional rendering: when set and it returns false, hides this field (or, for GroupBegin,
    // the block up to the matching GroupEnd).
    bool (*visible)(const void* obj) = nullptr;

    // FieldKind::Custom: the id of the editor drawer that draws it (Reflection/ReflectDrawers.h). An
    // id, not a function: the descriptor is data EngineCore compiles, the widget is the editor's.
    const char* drawer = nullptr;

    // Format an info line each frame from the host object (kind == InfoTextFn).
    void (*infoFn)(const void* obj, char* outBuf, std::size_t outCap) = nullptr;

    // Vector-of-struct support (kind == Vector): MakeVector<T> binds all four pointers so the drawer
    // can iterate std::vector<T> without knowing T.
    const TypeDescriptor* elementDesc = nullptr;
    std::size_t (*vecSize  )(const void* vecPtr) = nullptr;
    void*       (*vecData  )(void* vecPtr, std::size_t i) = nullptr;
    void        (*vecAdd   )(void* vecPtr) = nullptr;
    void        (*vecRemove)(void* vecPtr, std::size_t i) = nullptr;

    // Fixed-size array support (kind == Vector, elementDesc set, vecSize == nullptr): countOffset
    // points at the sibling uint/int holding the live count; arrayCapacity is the static length.
    std::size_t       countOffset = 0;
    std::size_t       arrayCapacity = 0;
    bool              countIsInt   = false;  // true for `int count`, false for uint32

    // sizeof the member (0 for the layout entries: Header, Info, IF, Collapse). The REFLECT_*
    // macros set it with their type check; the descriptor self-test checks bounds and overlaps.
    std::size_t       size = 0;

    // The key the serializer, Lua and property paths use; nullptr = name. "a,b,c" names one key per
    // component of a vector field the serializer writes as scalars (UIRect anchorMin -> aMinX,aMinY).
    const char*       key        = nullptr;
    // "a|b": older keys a reader still accepts for this field.
    const char*       legacyKeys = nullptr;
    // A unit from the AuthoringSchema vocabulary ("kg", "per_second", "world_unit", "radians", ...).
    const char*       unit       = nullptr;
    const char*       category   = nullptr;   // inspector grouping and search
    // The validation range writers enforce (minVal / maxVal are the drag range, which a typed value
    // may leave): a write outside it is refused, or clamped under FieldFlag::ClampHard.
    double            hardMin    = 0.0;
    double            hardMax    = 0.0;
    bool              hasHard    = false;
    FieldFlag         flags      = FieldFlag::None;
    // Flags kind: the named values the inspector offers as buttons.
    const FlagPreset* presets     = nullptr;
    std::size_t       presetCount = 0;
};

// TypeDescriptor: the reflection record of one component type, built once by
// Descriptor<T>::Make() and cached in Describe<T>().
struct TypeDescriptor
{
    const char*                  name = "";  // unqualified C++ type name
    std::size_t                  size = 0;   // sizeof(T), a sanity check
    std::vector<FieldDescriptor> fields;

    // T's lifetime, type-erased (all null when T is not default constructible): defaults() is one
    // T{} whose members are every field's default value; construct / destroy / copy let a consumer
    // snapshot or reset an object it only knows by descriptor.
    std::size_t align = 0;
    const void* (*defaults)() = nullptr;
    void        (*construct)(void* p) = nullptr;
    void        (*destroy)(void* p) = nullptr;
    void        (*copy)(void* dst, const void* src) = nullptr;
    bool        trivial = false;   // trivially copyable: a byte copy is a copy, memcmp an equality
};

inline bool Has(const FieldDescriptor& f, FieldFlag flag)
{
    return (static_cast<uint32_t>(f.flags) & static_cast<uint32_t>(flag)) != 0;
}

// The layout entries (headers, info lines, IF / Collapse brackets) hold no data.
inline bool IsLayout(FieldKind k)
{
    return k == FieldKind::Header || k == FieldKind::InfoText || k == FieldKind::InfoTextFn ||
           k == FieldKind::GroupBegin || k == FieldKind::GroupEnd || k == FieldKind::CollapseBegin ||
           k == FieldKind::CollapseEnd;
}

// The field's key (name when it declares none). A split key ("aMinX,aMinY") is returned whole.
inline const char* KeyOf(const FieldDescriptor& f)
{
    return f.key && *f.key ? f.key : f.name;
}

// The fields that hold data, in declaration order: what a serializer, a diff or a schema walks.
inline std::vector<const FieldDescriptor*> DataFields(const TypeDescriptor& d)
{
    std::vector<const FieldDescriptor*> out;
    out.reserve(d.fields.size());
    for (const FieldDescriptor& f : d.fields)
        if (!IsLayout(f.kind)) out.push_back(&f);
    return out;
}

// One T{} for @p d, or nullptr when the type has none (not default constructible).
inline const void* DefaultInstance(const TypeDescriptor& d)
{
    return d.defaults ? d.defaults() : nullptr;
}

template <class T>
inline void BindLifetime(TypeDescriptor& d)
{
    d.align = alignof(T);
    d.trivial = std::is_trivially_copyable_v<T>;
    if constexpr (std::is_default_constructible_v<T>)
    {
        d.defaults  = []() -> const void* { static const T s_default{}; return &s_default; };
        d.construct = [](void* p) { new (p) T{}; };
        d.destroy   = [](void* p) { static_cast<T*>(p)->~T(); };
        if constexpr (std::is_copy_assignable_v<T>)
            d.copy = [](void* dst, const void* src) { *static_cast<T*>(dst) = *static_cast<const T*>(src); };
    }
}

// Descriptor<T>: specialized per component type by REFLECT_BEGIN. The primary template has no
// Make(), so Describe<T>() on a non-reflected type fails to compile with "no member named 'Make'".
template <typename T>
struct Descriptor;

template <typename T>
inline const TypeDescriptor& Describe()
{
    static const TypeDescriptor s_descriptor = Descriptor<T>::Make();
    return s_descriptor;
}

// ==== Field builders ====
// Called by the REFLECT_* macros; free functions so each macro is one call in the initializer list.
inline float BoundedDragSpeed(float mn, float mx)
{
    const float range = mx >= mn ? mx - mn : mn - mx;
    const float speed = range > 0.0f ? range / 500.0f : 0.01f;
    return speed < 0.0001f ? 0.0001f : (speed > 10.0f ? 10.0f : speed);
}

inline float BoundedIntDragSpeed(int mn, int mx)
{
    const double range = static_cast<double>(mx) - static_cast<double>(mn);
    const double absRange = range >= 0.0 ? range : -range;
    const float speed = static_cast<float>(absRange / 500.0);
    return speed < 1.0f ? 1.0f : speed;
}

inline FieldDescriptor MakeFloat(const char* name, const char* label, std::size_t off,
                                 float mn, float mx,
                                 Widget w = Widget::Drag,
                                 const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32; f.widget = w;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeFloat2(const char* name, const char* label, std::size_t off,
                                  float mn, float mx, const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x2; f.widget = Widget::Drag;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeFloat3(const char* name, const char* label, std::size_t off,
                                  float mn, float mx, const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x3; f.widget = Widget::Drag;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeFloat4(const char* name, const char* label, std::size_t off,
                                  float mn, float mx, const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x4; f.widget = Widget::Drag;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeColor3(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x3; f.widget = Widget::Color;
    return f;
}

inline FieldDescriptor MakeColor4(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x4; f.widget = Widget::Color;
    return f;
}

inline FieldDescriptor MakeInt(const char* name, const char* label, std::size_t off,
                               int mn, int mx, Widget w = Widget::Drag, float speed = 1.f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::I32; f.widget = w;
    f.minVal = static_cast<float>(mn); f.maxVal = static_cast<float>(mx); f.speed = speed;
    return f;
}

inline FieldDescriptor MakeUInt(const char* name, const char* label, std::size_t off,
                                unsigned mn, unsigned mx, Widget w = Widget::Drag, float speed = 1.f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::U32; f.widget = w;
    f.minVal = static_cast<float>(mn); f.maxVal = static_cast<float>(mx); f.speed = speed;
    return f;
}

inline FieldDescriptor MakeBool(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Bool; f.widget = Widget::Checkbox;
    return f;
}

inline FieldDescriptor MakeEnum(const char* name, const char* label, std::size_t off,
                                std::size_t enumByteSize,
                                const EnumOption* opts, std::size_t optCount)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Enum; f.widget = Widget::Combo;
    f.enumSize = enumByteSize; f.enumOpts = opts; f.enumCount = optCount;
    return f;
}

// Float stored in radians, edited in degrees; minDeg / maxDeg are in degrees.
inline FieldDescriptor MakeAngle(const char* name, const char* label, std::size_t off,
                                 float minDeg, float maxDeg)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32; f.widget = Widget::Angle;
    f.minVal = minDeg; f.maxVal = maxDeg;
    return f;
}

inline FieldDescriptor MakeString(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::String; f.widget = Widget::InputText;
    return f;
}

// std::string field that also accepts an ImGui drag-drop payload of payloadType (e.g.
// "ITEX_PATH", "ILUA_PATH"); the drop replaces the string.
inline FieldDescriptor MakeStringDrop(const char* name, const char* label,
                                       std::size_t off, const char* payloadType)
{
    FieldDescriptor f = MakeString(name, label, off);
    f.dropPayload = payloadType;
    return f;
}

// Free-form widget: the inspector skips its kind switch and calls the editor drawer named @p drawer.
// For fields whose UI does not fit the kind grid (e.g. a quaternion edited as cached Euler angles).
inline FieldDescriptor MakeCustom(const char* name, const char* label, std::size_t off, const char* drawer)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Custom; f.drawer = drawer;
    return f;
}

// std::vector<T>; T needs its own Reflect::Descriptor<T>. Element ops are type-erased through
// captureless lambdas so the drawer iterates without seeing T.
template <typename T>
inline FieldDescriptor MakeVector(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Vector;
    f.elementDesc = &Describe<T>();
    f.vecSize = [](const void* v) -> std::size_t {
        return static_cast<const std::vector<T>*>(v)->size();
    };
    f.vecData = [](void* v, std::size_t i) -> void* {
        return &(*static_cast<std::vector<T>*>(v))[i];
    };
    f.vecAdd = [](void* v) {
        static_cast<std::vector<T>*>(v)->emplace_back();
    };
    f.vecRemove = [](void* v, std::size_t i) {
        auto& vec = *static_cast<std::vector<T>*>(v);
        if (i < vec.size()) vec.erase(vec.begin() + i);
    };
    return f;
}

// Fixed-size array T arr[CAP] paired with a sibling count field. Reuses the Vector kind; only
// vecData is set, so the drawer iterates by count and capacity.
template <typename T>
inline FieldDescriptor MakeArray(const char* name, const char* label,
                                  std::size_t off, std::size_t capacity,
                                  std::size_t countOff, bool countIsInt)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Vector;
    f.elementDesc   = &Describe<T>();
    f.arrayCapacity = capacity;
    f.countOffset   = countOff;
    f.countIsInt    = countIsInt;
    // Pointer arithmetic on the raw element block, same drawer path as std::vector.
    f.vecData = [](void* arrayBase, std::size_t i) -> void* {
        return static_cast<T*>(arrayBase) + i;
    };
    return f;
}

// Pseudo-fields: UI that reads no object memory.

inline FieldDescriptor MakeInfoFn(const char* label,
                                   void (*fn)(const void*, char*, std::size_t))
{
    FieldDescriptor f;
    f.label = label;
    f.kind  = FieldKind::InfoTextFn;
    f.infoFn = fn;
    return f;
}

inline FieldDescriptor MakeGroupBegin(bool (*pred)(const void* obj))
{
    FieldDescriptor f;
    f.kind = FieldKind::GroupBegin;
    f.visible = pred;
    return f;
}

inline FieldDescriptor MakeGroupEnd()
{
    FieldDescriptor f;
    f.kind = FieldKind::GroupEnd;
    return f;
}

// Wraps the following fields in an ImGui::CollapsingHeader until MakeCollapseEnd; defaultOpen
// sets the first-frame state.
inline FieldDescriptor MakeCollapseBegin(const char* label, bool defaultOpen = false)
{
    FieldDescriptor f;
    f.kind  = FieldKind::CollapseBegin;
    f.label = label;
    // minVal = 1 marks "default open", so FieldDescriptor needs no extra member.
    f.minVal = defaultOpen ? 1.f : 0.f;
    return f;
}

inline FieldDescriptor MakeCollapseEnd()
{
    FieldDescriptor f;
    f.kind = FieldKind::CollapseEnd;
    return f;
}

inline FieldDescriptor MakeHeader(const char* label)
{
    FieldDescriptor f;
    f.label = label;
    f.kind  = FieldKind::Header;
    return f;
}

inline FieldDescriptor MakeInfoText(const char* label)
{
    FieldDescriptor f;
    f.label = label;
    f.kind  = FieldKind::InfoText;
    return f;
}

// Returns f with tooltip tip set, so it slots into an initializer list.
inline FieldDescriptor WithTooltip(FieldDescriptor f, const char* tip)
{
    f.tooltip = tip;
    return f;
}

// ==== Type checks ====
// The drawer reads and writes a field through its offset as the kind says (4 bytes for F32 / I32 /
// U32, 12 for F32x3, ...): a macro used on a member of another type would touch the wrong bytes, so
// each check is a compile error, and each records the member's size.
// A float vector type laid out as Expected (UI::Vec2 for XMFLOAT2): the same float members x, y
// (z, w), the same size, trivially copyable.
template <class Expected, class Member>
constexpr bool LayoutCompatible()
{
    if constexpr (std::is_same_v<Expected, Member>) return true;
    else if constexpr (!std::is_trivially_copyable_v<Member> || sizeof(Member) != sizeof(Expected)) return false;
    else if constexpr (std::is_same_v<Expected, DirectX::XMFLOAT2>)
        return requires(Member m) { requires std::is_same_v<decltype(m.x), float> && std::is_same_v<decltype(m.y), float>; };
    else if constexpr (std::is_same_v<Expected, DirectX::XMFLOAT3>)
        return requires(Member m) { requires std::is_same_v<decltype(m.x), float> && std::is_same_v<decltype(m.z), float>; };
    else if constexpr (std::is_same_v<Expected, DirectX::XMFLOAT4>)
        return requires(Member m) { requires std::is_same_v<decltype(m.x), float> && std::is_same_v<decltype(m.w), float>; };
    else return false;
}

template <class Expected, class Member>
inline FieldDescriptor Typed(FieldDescriptor f)
{
    static_assert(LayoutCompatible<Expected, std::remove_cv_t<Member>>(),
                  "REFLECT_*: the macro's field kind does not match the member's C++ type");
    f.size = sizeof(Member);
    return f;
}

template <class Member>
inline FieldDescriptor TypedEnum(FieldDescriptor f)
{
    static_assert(std::is_enum_v<Member> || std::is_integral_v<Member>,
                  "REFLECT_ENUM: the member is not an enum or an integer");
    static_assert(sizeof(Member) == 1 || sizeof(Member) == 2 || sizeof(Member) == 4,
                  "REFLECT_ENUM: the drawer reads 1, 2 or 4 bytes");
    f.size = sizeof(Member);
    return f;
}

template <class Element, class Member>
inline FieldDescriptor TypedVector(FieldDescriptor f)
{
    static_assert(std::is_same_v<std::vector<Element>, std::remove_cv_t<Member>>,
                  "REFLECT_VECTOR: the member is not a std::vector of the element type");
    f.size = sizeof(Member);
    return f;
}

template <class Element, std::size_t Capacity, class Member>
inline FieldDescriptor TypedArray(FieldDescriptor f)
{
    static_assert(std::is_same_v<Element, std::remove_cv_t<std::remove_extent_t<Member>>>,
                  "REFLECT_ARRAY: the member is not an array of the element type");
    static_assert(std::extent_v<Member> == Capacity, "REFLECT_ARRAY: the capacity is not the array's length");
    f.size = sizeof(Member);
    return f;
}

template <class Member>
inline FieldDescriptor Sized(FieldDescriptor f)
{
    f.size = sizeof(Member);
    return f;
}

// ==== Attributes ====
// Trailing arguments of every value macro and of REFLECT_FIELD, applied in order. REFLECT_BEGIN's
// Make() has `using namespace Reflect::Attr`, so a descriptor writes them unqualified.
namespace Attr
{
    struct Key        { const char* v; void Apply(FieldDescriptor& f) const { f.key = v; } };
    struct LegacyKeys { const char* v; void Apply(FieldDescriptor& f) const { f.legacyKeys = v; } };
    struct Unit       { const char* v; void Apply(FieldDescriptor& f) const { f.unit = v; } };
    struct Category   { const char* v; void Apply(FieldDescriptor& f) const { f.category = v; } };
    struct Tip        { const char* v; void Apply(FieldDescriptor& f) const { f.tooltip = v; } };
    struct Format     { const char* v; void Apply(FieldDescriptor& f) const { f.fmt = v; } };
    struct Speed      { float v;       void Apply(FieldDescriptor& f) const { f.speed = v; } };
    struct Drop       { const char* v; void Apply(FieldDescriptor& f) const { f.dropPayload = v; } };
    // The drag range (a typed value may leave it).
    struct Soft
    {
        double lo, hi;
        void Apply(FieldDescriptor& f) const { f.minVal = static_cast<float>(lo); f.maxVal = static_cast<float>(hi); }
    };
    // The validation range writers enforce.
    struct Hard
    {
        double lo, hi;
        void Apply(FieldDescriptor& f) const { f.hardMin = lo; f.hardMax = hi; f.hasHard = true; }
    };
    struct FlagAttr
    {
        FieldFlag v;
        void Apply(FieldDescriptor& f) const { f.flags = f.flags | v; }
    };
    struct VisibleIf
    {
        bool (*pred)(const void* obj);
        void Apply(FieldDescriptor& f) const { f.visible = pred; }
    };
    // A string holding an asset path: an AssetRef that accepts the browser's @p v payload.
    struct Asset
    {
        const char* v;
        void Apply(FieldDescriptor& f) const { f.kind = FieldKind::AssetRef; f.dropPayload = v; }
    };
    // Drawn by the editor drawer named @p v (Reflection/ReflectDrawers.h).
    struct Drawer
    {
        const char* v;
        void Apply(FieldDescriptor& f) const { f.kind = FieldKind::Custom; f.drawer = v; }
    };
    struct OptionList
    {
        const EnumOption* opts;
        std::size_t       count;
        FieldKind         kind;
        void Apply(FieldDescriptor& f) const
        {
            f.kind = kind; f.enumOpts = opts; f.enumCount = count;
            if (kind == FieldKind::Enum) f.widget = Widget::Combo;
        }
    };
    struct PresetList
    {
        const FlagPreset* presets;
        std::size_t       count;
        void Apply(FieldDescriptor& f) const { f.presets = presets; f.presetCount = count; }
    };
    // Kind and widget switches on a member whose C++ type has more than one meaning.
    struct KindAttr
    {
        FieldKind kind;
        Widget    widget;
        void Apply(FieldDescriptor& f) const { f.kind = kind; f.widget = widget; }
    };
    struct WidgetAttr
    {
        Widget v;
        void Apply(FieldDescriptor& f) const { f.widget = v; }
    };

    // An enum member's values.
    template <std::size_t N>
    constexpr OptionList Options(const EnumOption (&opts)[N]) { return { opts, N, FieldKind::Enum }; }
    // An integer member as a bit set: one option per bit. A label "Group/Name" draws the options of
    // one group on one row.
    template <std::size_t N>
    constexpr OptionList FlagBits(const EnumOption (&opts)[N]) { return { opts, N, FieldKind::Flags }; }
    template <std::size_t N>
    constexpr PresetList Presets(const FlagPreset (&presets)[N]) { return { presets, N }; }

    inline constexpr FlagAttr Runtime     { FieldFlag::Runtime };
    inline constexpr FlagAttr ReadOnly    { FieldFlag::ReadOnly };
    inline constexpr FlagAttr Advanced    { FieldFlag::Advanced };
    inline constexpr FlagAttr Hidden      { FieldFlag::Hidden };
    inline constexpr FlagAttr NoUndo      { FieldFlag::NoUndo };
    inline constexpr FlagAttr NoMultiEdit { FieldFlag::NoMultiEdit };
    inline constexpr FlagAttr LuaRead     { FieldFlag::LuaRead };
    inline constexpr FlagAttr LuaWrite    { FieldFlag::LuaWrite };
    inline constexpr FlagAttr Overridable { FieldFlag::Overridable };
    inline constexpr FlagAttr Animatable  { FieldFlag::Animatable };
    inline constexpr FlagAttr ClampHard   { FieldFlag::ClampHard };

    inline constexpr KindAttr   AsQuat   { FieldKind::Quat, Widget::Default };       // XMFLOAT4 rotation
    inline constexpr KindAttr   AsEntity { FieldKind::EntityRef, Widget::Default };  // uint32_t Entity
    inline constexpr WidgetAttr AsColor  { Widget::Color };                          // XMFLOAT3 / XMFLOAT4
    inline constexpr WidgetAttr AsAngle  { Widget::Angle };                          // float radians, shown in degrees

    inline constexpr double kFloatMax = 3.402823466e+38;
}

// Applies attributes to a field: With(field)(attrs...) - a call, so a macro with no trailing
// attributes expands to With(field)() under either preprocessor.
struct FieldBuilder
{
    FieldDescriptor f;

    template <class... Attrs>
    FieldDescriptor operator()(const Attrs&... attrs) const
    {
        FieldDescriptor out = f;
        (attrs.Apply(out), ...);
        return out;
    }
};

inline FieldBuilder With(FieldDescriptor f) { return { f }; }

// ==== REFLECT_FIELD: the kind from the member's type ====
template <class T, class = void>
struct HasDescriptor : std::false_type {};
template <class T>
struct HasDescriptor<T, std::void_t<decltype(Descriptor<T>::Make())>> : std::true_type {};

template <class T>
struct IsStdVector : std::false_type {};
template <class E, class A>
struct IsStdVector<std::vector<E, A>> : std::true_type { using Element = E; };

template <class T>
inline constexpr bool kAlwaysFalse = false;

template <class T>
inline FieldDescriptor MakeField(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.size = sizeof(T);
    f.enumSize = sizeof(T);
    constexpr bool kInteger = std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t> ||
                              std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, int64_t>;
    if constexpr (kInteger) f.speed = 1.f;
    if constexpr (std::is_same_v<T, float>)                  f.kind = FieldKind::F32;
    else if constexpr (std::is_same_v<T, double>)            f.kind = FieldKind::F64;
    else if constexpr (std::is_same_v<T, DirectX::XMFLOAT2>) f.kind = FieldKind::F32x2;
    else if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>) f.kind = FieldKind::F32x3;
    else if constexpr (std::is_same_v<T, DirectX::XMFLOAT4>) f.kind = FieldKind::F32x4;
    else if constexpr (std::is_same_v<T, int32_t>)           f.kind = FieldKind::I32;
    else if constexpr (std::is_same_v<T, uint32_t>)          f.kind = FieldKind::U32;
    else if constexpr (std::is_same_v<T, uint8_t>)           f.kind = FieldKind::U8;
    else if constexpr (std::is_same_v<T, uint16_t>)          f.kind = FieldKind::U16;
    else if constexpr (std::is_same_v<T, int64_t>)           f.kind = FieldKind::I64;
    else if constexpr (std::is_same_v<T, bool>)
    {
        f.kind = FieldKind::Bool; f.widget = Widget::Checkbox;
    }
    else if constexpr (std::is_enum_v<T>)
    {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4, "REFLECT_FIELD: an enum of 1, 2 or 4 bytes");
        f.kind = FieldKind::Enum; f.widget = Widget::Combo;   // Options(...) names the values
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        f.kind = FieldKind::String; f.widget = Widget::InputText;
    }
    else if constexpr (LayoutCompatible<DirectX::XMFLOAT2, T>()) f.kind = FieldKind::F32x2;
    else if constexpr (LayoutCompatible<DirectX::XMFLOAT3, T>()) f.kind = FieldKind::F32x3;
    else if constexpr (LayoutCompatible<DirectX::XMFLOAT4, T>()) f.kind = FieldKind::F32x4;
    else if constexpr (IsStdVector<T>::value)
    {
        FieldDescriptor v = MakeVector<typename IsStdVector<T>::Element>(name, label, off);
        v.size = sizeof(T);
        return v;
    }
    else if constexpr (HasDescriptor<T>::value)
    {
        f.kind = FieldKind::Struct;
        f.elementDesc = &Describe<T>();
    }
    else
        static_assert(kAlwaysFalse<T>, "REFLECT_FIELD: no field kind for this member type");
    return f;
}

} // namespace Reflect

// REFLECT_BEGIN / REFLECT_END define a Descriptor<T> specialization; the body is the fields
// initializer list, one REFLECT_* macro per field. Use at namespace scope (the macros open
// namespace Reflect themselves).

#define REFLECT_BEGIN(TYPE)                                              \
    namespace Reflect {                                                  \
    template <> struct Descriptor<TYPE> {                                \
        using ThisType = TYPE;                                           \
        static TypeDescriptor Make() {                                   \
            using namespace ::Reflect::Attr;                             \
            TypeDescriptor d;                                            \
            d.name = #TYPE;                                              \
            d.size = sizeof(TYPE);                                       \
            ::Reflect::BindLifetime<TYPE>(d);                            \
            d.fields = {

#define REFLECT_END()                                                    \
            };                                                           \
            return d;                                                    \
        }                                                                \
    };                                                                   \
    }

// Per-field macros: each emits one FieldDescriptor. offsetof() records the raw byte offset the
// drawer reads and writes through.

// The member's type (unevaluated: no object is needed). An element (`points[1]`) is an lvalue
// expression, so decltype gives a reference: strip it.
#define REFLECT_MEMBER_T(NAME) std::remove_reference_t<decltype(ThisType::NAME)>

// Every value macro ends in `...`: trailing Reflect::Attr attributes (Key, Unit, Hard, LuaWrite, ...).

// The kind deduced from the member's type (float, double, XMFLOAT2/3/4, bool, int32/uint32/uint8/
// uint16/int64, enum, std::string, std::vector<T>, a struct with its own descriptor); attributes
// pick the meanings a type does not say: Options / FlagBits / AsQuat / AsColor / AsAngle / AsEntity /
// Asset(payload) / Drawer(id).
#define REFLECT_FIELD(NAME, LABEL, ...) \
    Reflect::With(Reflect::MakeField<std::remove_cv_t<REFLECT_MEMBER_T(NAME)>>( \
        #NAME, LABEL, offsetof(ThisType, NAME)))(__VA_ARGS__),

#define REFLECT_FLOAT(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<float, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeFloat(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX))))(__VA_ARGS__),

#define REFLECT_FLOAT_FMT(NAME, LABEL, MIN, MAX, FMT, SPEED, ...) \
    Reflect::With(Reflect::Typed<float, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeFloat(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX), \
                           Reflect::Widget::Drag, FMT, (float)(SPEED))))(__VA_ARGS__),

#define REFLECT_DRAG(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<float, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeFloat(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX), \
                           Reflect::Widget::Drag, nullptr, \
                           Reflect::BoundedDragSpeed((float)(MIN), (float)(MAX)))))(__VA_ARGS__),

#define REFLECT_FLOAT2(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<DirectX::XMFLOAT2, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeFloat2(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX))))(__VA_ARGS__),

#define REFLECT_FLOAT3(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<DirectX::XMFLOAT3, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeFloat3(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX))))(__VA_ARGS__),

#define REFLECT_FLOAT4(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<DirectX::XMFLOAT4, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeFloat4(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX))))(__VA_ARGS__),

#define REFLECT_COLOR3(NAME, LABEL, ...) \
    Reflect::With(Reflect::Typed<DirectX::XMFLOAT3, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeColor3(#NAME, LABEL, offsetof(ThisType, NAME))))(__VA_ARGS__),

#define REFLECT_COLOR4(NAME, LABEL, ...) \
    Reflect::With(Reflect::Typed<DirectX::XMFLOAT4, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeColor4(#NAME, LABEL, offsetof(ThisType, NAME))))(__VA_ARGS__),

#define REFLECT_INT(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<int32_t, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeInt(#NAME, LABEL, offsetof(ThisType, NAME), (int)(MIN), (int)(MAX))))(__VA_ARGS__),

#define REFLECT_DRAG_INT(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<int32_t, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeInt(#NAME, LABEL, offsetof(ThisType, NAME), (int)(MIN), (int)(MAX), \
                         Reflect::Widget::Drag, \
                         Reflect::BoundedIntDragSpeed((int)(MIN), (int)(MAX)))))(__VA_ARGS__),

#define REFLECT_UINT(NAME, LABEL, MIN, MAX, ...) \
    Reflect::With(Reflect::Typed<uint32_t, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeUInt(#NAME, LABEL, offsetof(ThisType, NAME), (unsigned)(MIN), (unsigned)(MAX))))(__VA_ARGS__),

#define REFLECT_BOOL(NAME, LABEL, ...) \
    Reflect::With(Reflect::Typed<bool, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeBool(#NAME, LABEL, offsetof(ThisType, NAME))))(__VA_ARGS__),

// OPTIONS must be a constexpr Reflect::EnumOption array visible here. sizeof(((T*)0)->NAME) gives
// the underlying byte size (enum class with an explicit underlying type, or a plain enum).
#define REFLECT_ENUM(NAME, LABEL, OPTIONS, ...) \
    Reflect::With(Reflect::TypedEnum<REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeEnum(#NAME, LABEL, offsetof(ThisType, NAME), \
                          sizeof(((ThisType*)0)->NAME), \
                          (OPTIONS), sizeof(OPTIONS) / sizeof((OPTIONS)[0]))))(__VA_ARGS__),

// Float stored in radians; the UI shows degrees in MIN_DEG..MAX_DEG.
#define REFLECT_ANGLE(NAME, LABEL, MIN_DEG, MAX_DEG, ...) \
    Reflect::With(Reflect::Typed<float, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeAngle(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN_DEG), (float)(MAX_DEG))))(__VA_ARGS__),

#define REFLECT_STRING(NAME, LABEL, ...) \
    Reflect::With(Reflect::Typed<std::string, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeString(#NAME, LABEL, offsetof(ThisType, NAME))))(__VA_ARGS__),

// String field that also accepts the named ImGui drag-drop payload.
#define REFLECT_STRING_DROP(NAME, LABEL, PAYLOAD, ...) \
    Reflect::With(Reflect::Typed<std::string, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeStringDrop(#NAME, LABEL, offsetof(ThisType, NAME), PAYLOAD)))(__VA_ARGS__),

// std::vector<T>; T needs its own REFLECT_BEGIN/END descriptor.
#define REFLECT_VECTOR(NAME, LABEL, ELEMENT_T, ...) \
    Reflect::With(Reflect::TypedVector<ELEMENT_T, REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeVector<ELEMENT_T>(#NAME, LABEL, offsetof(ThisType, NAME))))(__VA_ARGS__),

// Fixed C array T arr[CAP] paired with a sibling count member COUNT_FIELD (unsigned or int);
// CAP is the static capacity.
#define REFLECT_ARRAY(NAME, LABEL, ELEMENT_T, CAP, COUNT_FIELD, ...) \
    Reflect::With(Reflect::TypedArray<ELEMENT_T, (std::size_t)(CAP), REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeArray<ELEMENT_T>(#NAME, LABEL, offsetof(ThisType, NAME), \
            (std::size_t)(CAP), offsetof(ThisType, COUNT_FIELD), \
            std::is_signed_v<decltype(ThisType::COUNT_FIELD)>)))(__VA_ARGS__),

// Field whose UI is entirely hand-written: DRAWER is the id of an editor drawer
// (Reflection/ReflectDrawers.h), a string literal.
#define REFLECT_CUSTOM(NAME, LABEL, DRAWER, ...) \
    Reflect::With(Reflect::Sized<REFLECT_MEMBER_T(NAME)>( \
        Reflect::MakeCustom(#NAME, LABEL, offsetof(ThisType, NAME), DRAWER)))(__VA_ARGS__),

// Visual-only entries: a Separator + label, or a TextDisabled line.
#define REFLECT_HEADER(LABEL) \
    Reflect::MakeHeader(LABEL),

#define REFLECT_INFO(TEXT) \
    Reflect::MakeInfoText(TEXT),

// Dynamic info line. FN is `void(const void* obj, char* buf, std::size_t cap)`.
#define REFLECT_INFO_FN(LABEL, FN) \
    Reflect::MakeInfoFn(LABEL, FN),

// Conditional block: fields between IF / ENDIF render only when PRED(obj) is true. PRED is
// bool(const void* obj); captureless lambdas convert.
#define REFLECT_IF(PRED) \
    Reflect::MakeGroupBegin(PRED),

#define REFLECT_ENDIF() \
    Reflect::MakeGroupEnd(),

// CollapsingHeader wrapper: fields between BEGIN / END render only while the header is open.
// REFLECT_COLLAPSE_OPEN starts open.
#define REFLECT_COLLAPSE(LABEL) \
    Reflect::MakeCollapseBegin(LABEL, false),

#define REFLECT_COLLAPSE_OPEN(LABEL) \
    Reflect::MakeCollapseBegin(LABEL, true),

#define REFLECT_COLLAPSE_END() \
    Reflect::MakeCollapseEnd(),

// A tooltip is the Tip("...") attribute; hand-written descriptors use Reflect::WithTooltip.
