#pragma once

// Authoring contracts (field descriptors, creation defaults, validation), independent of a live
// World, reflection and the editor. They describe strict authoring patches, not the permissive
// scene loader. Keep validation and discovery on the same descriptors; adapters must validate
// before any mutation.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <set>
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"

namespace Resource::Authoring
{
    struct Field
    {
        const char* key;
        const char* type;
        int arity;
        const char* unit;
        double minimum, maximum;
        bool exclusiveMinimum = false, exclusiveMaximum = false;
        const char* semantics = "";
    };
    inline constexpr double FloatMax = (std::numeric_limits<float>::max)();
    inline constexpr double Pi = 3.14159265358979323846;
    const std::vector<Field>& MaterialFields(); // Generated from the serializer's PBR schema.
    inline const std::vector<Field>& Fields(std::string_view tag)
    {
        if (tag == "Material") return MaterialFields();
        static const std::vector<Field> kTransform = {
            {"position", "vector", 3, "world_unit", -FloatMax, FloatMax, false, false, "local translation xyz"},
            {"rotation", "quaternion", 4, "unitless", -FloatMax, FloatMax, false, false, "xyzw; nonzero; normalized on write"},
            {"scale", "vector", 3, "unitless", -FloatMax, FloatMax, false, false, "local scale xyz; zero and negative allowed"},
        };
        static const std::vector<Field> kLight = {
            {"type", "integer", 1, "enum", 0, 2, false, false, "0=Directional, 1=Point, 2=Spot"},
            {"radius", "number", 1, "world_unit", 0, FloatMax},
            {"intensity", "number", 1, "engine_intensity", 0, FloatMax},
            {"color", "vector", 3, "linear_rgb", 0, FloatMax},
            {"spotAngle", "number", 1, "radians", 0, Pi / 2, true, true, "spot half-angle"},
            {"castsShadow", "boolean", 1, "unitless", 0, 1},
            {"enabled", "boolean", 1, "unitless", 0, 1},
            {"sourceRadius", "number", 1, "world_unit", 0, FloatMax, false, false, "emitter radius for ray-traced soft shadows; 0 = punctual"},
        };
        static const std::vector<Field> kCamera = {
            {"fov", "number", 1, "radians", 0, Pi, true, true, "vertical field of view"},
            {"nearZ", "number", 1, "world_unit", 0, FloatMax, true},
            {"farZ", "number", 1, "world_unit", 0, FloatMax, true, false, "must exceed nearZ"},
        };
        static const std::vector<Field> kNone;
        static const std::vector<Field> kRigid = {
            {"motion", "integer", 1, "enum", 0, 2, false, false, "0=Static, 1=Kinematic, 2=Dynamic"},
            {"mass", "number", 1, "kg", 0, FloatMax, false, false, "must be positive for Dynamic"},
            {"linDamp", "number", 1, "per_second", 0, FloatMax},
            {"angDamp", "number", 1, "per_second", 0, FloatMax},
            {"friction", "number", 1, "unitless", 0, FloatMax},
            {"restitution", "number", 1, "unitless", 0, 1},
            {"gravity", "number", 1, "multiplier", -FloatMax, FloatMax},
            {"lockedAxes", "integer", 1, "bitmask", 0, 63, false, false, "bits 0..2 lock translation xyz; 3..5 rotation xyz"},
        };
        static const std::vector<Field> kAnimation = {
            {"default", "string", 1, "alias", 0, 0, false, false, "must name a clip when autoplay is true"},
            {"autoplay", "boolean", 1, "unitless", 0, 1},
            {"n", "integer", 1, "count", 0, 128},
            {"name{index}", "string", 1, "alias", 0, 0, false, false, "unique nonempty alias; zero-based index less than n"},
            {"path{index}", "asset", 1, "path", 0, 0, false, false, "existing .ianim asset; verify skeleton coverage with assets.compat"},
            {"bone{index}", "string", 1, "clip_selector", 0, 0, false, false, "empty selects loader default bone clip"},
            {"morph{index}", "string", 1, "clip_selector", 0, 0},
            {"loop{index}", "boolean", 1, "unitless", 0, 1},
            {"speed{index}", "number", 1, "multiplier", 0, FloatMax, false, false, "zero pauses; positive advances playback"},
        };
        // The Canvas UI components: every key their serializers write, so Entity.set, the .iui codec
        // (UI/UIAsset.h) and asset_tool `ui patch` judge one contract (tests/unit/test_ui_asset.py checks
        // contract keys == serializer keys). Anchors and pivots are fractions the layout clamps to 0..1
        // (the contract does not refuse what a scene may hold); sizes and offsets are reference px (a
        // stretched axis adds the size to the anchor span). Floats are stored as float32 at general
        // stream precision. CvParent has no contract: the edge is Editor.Reparent's (or a .iui's nesting).
        static const std::vector<Field> kCvCanvas = {
            {"renderMode", "integer", 1, "enum", 0, 0, false, false, "0=ScreenSpaceOverlay"},
            {"scaleMode", "integer", 1, "enum", 0, 1, false, false, "0=ConstantPixelSize, 1=ScaleWithScreenSize"},
            {"refW", "number", 1, "px", 0, FloatMax, true, false, "reference resolution width"},
            {"refH", "number", 1, "px", 0, FloatMax, true, false, "reference resolution height"},
            {"match", "number", 1, "unitless", 0, 1, false, false, "0 = match width, 1 = match height"},
            // sortOrder x 100000 is the int sort base (UICanvasSystem / UIViewService layout); +-21000 keeps
            // it inside int32.
            {"sortOrder", "integer", 1, "order", -21000, 21000, false, false, "canvases paint in ascending order"},
            {"visible", "boolean", 1, "unitless", 0, 1},
        };
        static const std::vector<Field> kCvRect = {
            {"aMinX", "number", 1, "fraction", -FloatMax, FloatMax, false, false, "anchor min x of the parent; clamped to 0..1"},
            {"aMinY", "number", 1, "fraction", -FloatMax, FloatMax, false, false, "anchor min y of the parent; clamped to 0..1"},
            {"aMaxX", "number", 1, "fraction", -FloatMax, FloatMax, false, false, "anchor max x; == aMinX is a point anchor"},
            {"aMaxY", "number", 1, "fraction", -FloatMax, FloatMax, false, false, "anchor max y; == aMinY is a point anchor"},
            {"pivX", "number", 1, "fraction", -FloatMax, FloatMax, false, false, "pivot x of the rect; clamped to 0..1"},
            {"pivY", "number", 1, "fraction", -FloatMax, FloatMax, false, false, "pivot y of the rect; clamped to 0..1"},
            {"szX", "number", 1, "reference_px", -FloatMax, FloatMax, false, false, "size; a stretched axis adds it to the span"},
            {"szY", "number", 1, "reference_px", -FloatMax, FloatMax, false, false, "size; a stretched axis adds it to the span"},
            {"offX", "number", 1, "reference_px", -FloatMax, FloatMax, false, false, "the pivot's offset from its anchor point"},
            {"offY", "number", 1, "reference_px", -FloatMax, FloatMax, false, false, "the pivot's offset from its anchor point"},
            {"visible", "boolean", 1, "unitless", 0, 1, false, false, "the whole subtree"},
            {"sibling", "integer", 1, "order", -1e9, 1e9, false, false, "paint order among siblings (Editor.Reparent's index)"},
        };
        static const std::vector<Field> kCvClip = {
            {"enabled", "boolean", 1, "unitless", 0, 1},
        };
        static const std::vector<Field> kCvImage = {
            {"visible", "boolean", 1, "unitless", 0, 1},
            {"color", "vector", 4, "linear_rgba", 0, FloatMax},
            {"uv0", "vector", 2, "uv", -FloatMax, FloatMax},
            {"uv1", "vector", 2, "uv", -FloatMax, FloatMax, false, false, "past 1 tiles with wrap"},
            {"wrap", "integer", 1, "enum", 0, 2, false, false, "0=Clamp, 1=Wrap, 2=Mirror"},
            {"point", "boolean", 1, "unitless", 0, 1, false, false, "nearest sampling"},
            {"slice", "vector", 4, "texture_px", 0, FloatMax, false, false, "9-slice borders: left, top, right, bottom"},
            {"tex", "string", 1, "path", 0, 0, false, false, "a .itex path; empty = a solid colour fill"},
        };
        static const std::vector<Field> kCvText = {
            {"visible", "boolean", 1, "unitless", 0, 1},
            {"text", "text", 1, "utf8", 0, 0, false, false, "newlines allowed"},
            {"font", "string", 1, "path", 0, 0, false, false, "a .ttf path; empty = the engine default font"},
            {"color", "vector", 4, "linear_rgba", 0, FloatMax},
            {"fontScale", "number", 1, "multiplier", 0, FloatMax},
            {"alignH", "integer", 1, "enum", 0, 2, false, false, "0=Left, 1=Center, 2=Right"},
            {"alignV", "integer", 1, "enum", 0, 2, false, false, "0=Top, 1=Middle, 2=Bottom"},
            {"fxO", "boolean", 1, "unitless", 0, 1, false, false, "outline"},
            {"fxG", "boolean", 1, "unitless", 0, 1, false, false, "glow"},
            {"fxS", "boolean", 1, "unitless", 0, 1, false, false, "shadow"},
            {"fxJ", "boolean", 1, "unitless", 0, 1, false, false, "jitter"},
            {"outlineCol", "vector", 4, "linear_rgba", 0, FloatMax},
            {"outlineW", "number", 1, "px", 0, FloatMax},
            {"glowCol", "vector", 4, "linear_rgba", 0, FloatMax},
            {"glowW", "number", 1, "px", 0, FloatMax},
            {"shadowCol", "vector", 4, "linear_rgba", 0, FloatMax},
            {"shadowOX", "number", 1, "px", -FloatMax, FloatMax},
            {"shadowOY", "number", 1, "px", -FloatMax, FloatMax},
            {"jitterA", "number", 1, "px", 0, FloatMax},
            {"jitterF", "number", 1, "hz", 0, FloatMax},
            {"customShader", "string", 1, "path", 0, 0, false, false, "a text_fx pixel shader; empty = the stock SDF path"},
            {"cpA", "vector", 4, "unitless", -FloatMax, FloatMax, false, false, "custom shader params A"},
            {"cpB", "vector", 4, "unitless", -FloatMax, FloatMax, false, false, "custom shader params B"},
            {"wrapW", "number", 1, "reference_px", 0, FloatMax, false, false, "word-wrap width; 0 = no wrapping"},
        };
        static const std::vector<Field> kCvInteract = {
            {"raycast", "boolean", 1, "unitless", 0, 1, false, false, "receives clicks"},
            {"disabled", "boolean", 1, "unitless", 0, 1},
            {"focusable", "boolean", 1, "unitless", 0, 1, false, false, "takes keyboard / pad focus"},
            {"absorbNavX", "boolean", 1, "unitless", 0, 1, false, false, "left / right adjust it instead of moving focus"},
            {"tint", "boolean", 1, "unitless", 0, 1, false, false, "tint the image by state"},
            {"normal", "vector", 4, "linear_rgba", 0, FloatMax},
            {"hover", "vector", 4, "linear_rgba", 0, FloatMax},
            {"pressed", "vector", 4, "linear_rgba", 0, FloatMax},
            {"disabledCol", "vector", 4, "linear_rgba", 0, FloatMax},
        };
        static const std::vector<Field> kCvSprite = {
            {"cols", "integer", 1, "count", 0, 4096, false, false, "frames per row of the sheet"},
            {"rows", "integer", 1, "count", 0, 4096},
            {"frames", "integer", 1, "count", 0, 1 << 20, false, false, "0 = cols x rows"},
            {"fps", "number", 1, "hz", 0, FloatMax},
            {"loop", "boolean", 1, "unitless", 0, 1},
            {"playing", "boolean", 1, "unitless", 0, 1},
            {"pingpong", "boolean", 1, "unitless", 0, 1},
        };
        // Every key the "FaceTracking" serializer writes.
        static const std::vector<Field> kFaceTracking = {
            {"enabled", "boolean", 1, "unitless", 0, 1},
            {"source", "string", 1, "name", 0, 0, false, false, "a face source name (FaceTracking.Sources)"},
            {"profile", "string", 1, "path", 0, 0, false, false, "an existing .ifacemap; empty = the built-in mapping that fits the model"},
            {"wMorph", "number", 1, "fraction", 0, 1, false, false, "how much of the tracked face reaches the morphs"},
            {"wHead", "number", 1, "fraction", 0, 1, false, false, "how much reaches the neck / head chain"},
            {"wEyes", "number", 1, "fraction", 0, 1, false, false, "how much reaches the eye bones"},
            {"mirror", "boolean", 1, "unitless", 0, 1, false, false, "the character mirrors the performer"},
            {"hold", "number", 1, "seconds", 0, FloatMax, false, false, "the last face is held this long after it is lost"},
            {"fadeOut", "number", 1, "seconds", 0, FloatMax, false, false, "then handed back over this long"},
            {"fadeIn", "number", 1, "seconds", 0, FloatMax, false, false, "and taken over again over this long"},
            {"suppressBlink", "boolean", 1, "unitless", 0, 1, false, false, "auto-blink pauses while the tracked lids own the eyes"},
            {"handoff", "boolean", 1, "unitless", 0, 1, false, false, "HeadTrack gives way by the face's authority"},
        };
        // A scene's reference to a .iui (its instance is runtime-only).
        static const std::vector<Field> kCvDoc = {
            {"path", "string", 1, "path", 0, 0, false, false, "a .iui UI asset; empty = no instance"},
        };
        if (tag.size() > 2 && tag[0] == 'C' && tag[1] == 'v')
            return tag == "CvCanvas" ? kCvCanvas : tag == "CvRect" ? kCvRect : tag == "CvClip" ? kCvClip :
                   tag == "CvImage" ? kCvImage : tag == "CvText" ? kCvText : tag == "CvInteract" ? kCvInteract :
                   tag == "CvSpriteAnim" ? kCvSprite : tag == "CvDoc" ? kCvDoc : kNone;
        return tag == "Transform" ? kTransform : tag == "LightData" ? kLight : tag == "Camera" ? kCamera :
            tag == "RigidBody" ? kRigid : tag == "AnimationSet" ? kAnimation : tag == "FaceTracking" ? kFaceTracking : kNone;
    }

    // The tags with a contract, in a stable order (the catalog dump, Entity.schema).
    inline const std::vector<const char*>& ContractTags()
    {
        static const std::vector<const char*> kTags = { "Transform", "LightData", "Camera", "RigidBody", "AnimationSet",
            "Material", "CvCanvas", "CvRect", "CvClip", "CvImage", "CvText", "CvInteract", "CvSpriteAnim", "CvDoc",
            "FaceTracking" };
        return kTags;
    }
    using Values = std::vector<std::pair<std::string, std::string>>;

    inline const Field* FindField(std::string_view tag, std::string_view key)
    {
        for (const auto& field : Fields(tag))
        {
            const std::string_view pattern(field.key);
            const auto at = pattern.find("{index}");
            if (at == std::string_view::npos) { if (key == pattern) return &field; continue; }
            if (key.substr(0, at) != pattern.substr(0, at) || key.size() <= at) continue;
            const auto index = key.substr(at);
            if (index.size() > 3 || (index.size() > 1 && index[0] == '0')) continue;
            int n = 0;
            for (char c : index) { if (c < '0' || c > '9') { n = 128; break; } n = n * 10 + c - '0'; }
            if (n < 128) return &field;
        }
        return nullptr;
    }

    inline std::string SlotDefault(std::string_view key)
    {
        return key.substr(0, 4) == "loop" || key.substr(0, 5) == "speed" ? "1" : "";
    }

    // Serialized authoring defaults. Creation adapters submit these explicitly;
    // they do not depend on the Inspector's context-dependent add callback.
    inline Values Defaults(std::string_view tag)
    {
        if (tag == "Transform") return {{"position", "0_0_0"}, {"rotation", "0_0_0_1"}, {"scale", "1_1_1"}};
        if (tag == "LightData") return {{"type", "0"}, {"radius", "10"}, {"intensity", "1"},
            {"color", "1_1_1"}, {"spotAngle", "0.5236"}, {"castsShadow", "0"}, {"enabled", "1"}};
        if (tag == "Camera") return {{"fov", "1.047"}, {"nearZ", "0.1"}, {"farZ", "200"}};
        if (tag == "RigidBody") return {{"motion", "2"}, {"mass", "1"}, {"linDamp", "0.05"},
            {"angDamp", "0.05"}, {"friction", "0.5"}, {"restitution", "0"}, {"gravity", "1"}, {"lockedAxes", "0"}};
        if (tag == "AnimationSet") return {{"default", ""}, {"autoplay", "0"}, {"n", "0"}};
        // The Canvas UI components: exactly what each deserializer makes of an absent key
        // (tests/unit/test_ui_asset.py checks them against the serializers). They also fix each key's
        // text shape, by which Engine.AddComponent / Entity.create convert a Lua value: a boolean reads
        // "1", a colour "1_1_1_1".
        if (tag == "CvCanvas") return {{"renderMode", "0"}, {"scaleMode", "1"}, {"refW", "1920"}, {"refH", "1080"},
            {"match", "0.5"}, {"sortOrder", "0"}, {"visible", "1"}};
        if (tag == "CvRect") return {{"aMinX", "0.5"}, {"aMinY", "0.5"}, {"aMaxX", "0.5"}, {"aMaxY", "0.5"},
            {"pivX", "0.5"}, {"pivY", "0.5"}, {"szX", "160"}, {"szY", "40"}, {"offX", "0"}, {"offY", "0"},
            {"visible", "1"}, {"sibling", "0"}};
        if (tag == "CvClip") return {{"enabled", "1"}};
        if (tag == "CvImage") return {{"visible", "1"}, {"color", "1_1_1_1"}, {"uv0", "0_0"}, {"uv1", "1_1"},
            {"wrap", "0"}, {"point", "0"}, {"slice", "0_0_0_0"}, {"tex", ""}};
        if (tag == "CvText") return {{"visible", "1"}, {"text", ""}, {"font", ""}, {"color", "1_1_1_1"},
            {"fontScale", "1"}, {"alignH", "1"}, {"alignV", "1"}, {"fxO", "0"}, {"fxG", "0"}, {"fxS", "0"},
            {"fxJ", "0"}, {"outlineCol", "0_0_0_1"}, {"outlineW", "2"}, {"glowCol", "1_0.85_0.4_1"}, {"glowW", "4"},
            {"shadowCol", "0_0_0_0.6"}, {"shadowOX", "2"}, {"shadowOY", "2"}, {"jitterA", "2"}, {"jitterF", "12"},
            {"customShader", ""}, {"cpA", "0_0_0_0"}, {"cpB", "0_0_0_0"}, {"wrapW", "0"}};
        if (tag == "CvInteract") return {{"raycast", "1"}, {"disabled", "0"}, {"focusable", "1"},
            {"absorbNavX", "0"}, {"tint", "0"}, {"normal", "1_1_1_1"}, {"hover", "0.92_0.92_0.92_1"},
            {"pressed", "0.78_0.78_0.78_1"}, {"disabledCol", "0.5_0.5_0.5_0.5"}};
        if (tag == "CvSpriteAnim") return {{"cols", "4"}, {"rows", "4"}, {"frames", "0"}, {"fps", "12"},
            {"loop", "1"}, {"playing", "1"}, {"pingpong", "0"}};
        if (tag == "CvDoc") return {{"path", ""}};
        if (tag == "FaceTracking") return {{"enabled", "1"}, {"source", "default"}, {"profile", ""}, {"wMorph", "1"},
            {"wHead", "1"}, {"wEyes", "1"}, {"mirror", "1"}, {"hold", "0.25"}, {"fadeOut", "0.6"}, {"fadeIn", "0.2"},
            {"suppressBlink", "1"}, {"handoff", "1"}};
        return {};
    }

    inline void FillDefaults(std::string_view tag, Values& values)
    {
        for (const auto& def : Defaults(tag))
        {
            bool found = false;
            for (const auto& value : values) if (value.first == def.first) { found = true; break; }
            if (!found) values.push_back(def);
        }
    }

    inline bool KnownField(std::string_view tag, std::string_view key)
    {
        return FindField(tag, key) != nullptr;
    }

    inline bool ValidateSubmitted(std::string_view tag, const Values& submitted, const Values& merged, std::string& error)
    {
        if (Fields(tag).empty()) return true;
        int count = 0;
        for (const auto& [key, value] : merged) if (key == "n") count = std::atoi(value.c_str());
        for (const auto& [key, value] : submitted)
        {
            const auto* field = FindField(tag, key);
            if (!field) { error = std::string(tag) + "." + key + ": unknown authoring key"; return false; }
            const auto at = std::string_view(field->key).find("{index}");
            if (at != std::string_view::npos && std::atoi(key.c_str() + at) >= count)
            { error = std::string(tag) + "." + key + ": slot index must be less than n"; return false; }
        }
        return true;
    }

    // Validate the merged record. Unknown fields are preserved, not reinterpreted; the caller checks
    // patch keys against its supported fields. LightData / Camera / RigidBody serialize with %.4f, so
    // the persisted value is validated too: an accepted near plane cannot round to zero on save.
    inline bool Validate(std::string_view tag, const Values& values, std::string& error)
    {
        double nearZ = 0, farZ = 0;
        bool hasNear = false, hasFar = false;
        for (const auto& [key, value] : values)
        {
            const auto* descriptor = FindField(tag, key);
            if (!descriptor) continue; // Preserve unknown tokens.
            if (tag == "AnimationSet")
            {
                const auto at = std::string_view(descriptor->key).find("{index}");
                if (at != std::string_view::npos)
                    for (const auto& count : values) if (count.first == "n" &&
                        std::atoi(key.c_str() + at) >= std::atoi(count.second.c_str()))
                    { descriptor = nullptr; break; }
                if (!descriptor) continue; // Slots at index >= n are not loaded.
            }
            const auto& field = *descriptor;
            const std::string* text = &value;
            auto fail = [&](const char* why) {
                error = std::string(tag) + "." + key + ": " + why;
                return false;
            };
            if (std::string_view(field.type) == "string" || std::string_view(field.type) == "asset" ||
                std::string_view(field.type) == "text")
            {
                const auto decoded = ::Resource::AssetRefs::PercentDecode(value);
                if (decoded.find('\0') != std::string::npos) return fail("text cannot contain a NUL");
                // "text" is multi-line (a UI label); a name or a path is one line.
                if (std::string_view(field.type) != "text" && decoded.find_first_of("\r\n\t") != std::string::npos)
                    return fail("text cannot contain control characters");
                if (std::string_view(field.type) == "asset" && (decoded.size() < 6 || decoded.substr(decoded.size() - 6) != ".ianim" ||
                    !::Resource::AssetFS::Get().ExistsExact(decoded))) return fail("requires an existing .ianim path");
                continue;
            }
            const char* p = text->c_str();
            double norm = 0;
            for (int i = 0; i < field.arity; ++i)
            {
                char* end = nullptr;
                const double d = std::strtod(p, &end);
                if (end == p || !std::isfinite(d) || std::fabs(d) > FloatMax)
                    return fail("requires finite float32 numbers");
                if (i + 1 < field.arity ? *end != '_' : *end != '\0')
                    return fail("wrong vector length or numeric text");
                p = end + (i + 1 < field.arity ? 1 : 0);
                auto inRange = [&](double x) {
                    return x >= field.minimum && x <= field.maximum &&
                        (!field.exclusiveMinimum || x > field.minimum) &&
                        (!field.exclusiveMaximum || x < field.maximum);
                };
                if (!inRange(d)) return fail("outside the authoring range (see Entity.schema)");
                if ((std::string_view(field.type) == "integer" || std::string_view(field.type) == "boolean") && std::floor(d) != d)
                    return fail("requires an integer (boolean: 0 or 1)");
                if (std::string_view(field.type) == "integer" || std::string_view(field.type) == "boolean")
                {
                    char* integerEnd = nullptr;
                    std::strtoll(text->c_str(), &integerEnd, 10);
                    if (!integerEnd || *integerEnd) return fail("requires decimal integer text, without exponent or fraction");
                }
                double stored = static_cast<float>(d);
                if (tag == "LightData" || tag == "Camera" || tag == "RigidBody")
                {
                    char rounded[80];
                    std::snprintf(rounded, sizeof(rounded), "%.4f", stored);
                    stored = std::strtod(rounded, nullptr);
                    if (!inRange(stored)) return fail("outside range after float32 / four-decimal serialization");
                }
                norm += stored * stored;
                if (field.key == std::string_view("nearZ")) { nearZ = stored; hasNear = true; }
                if (field.key == std::string_view("farZ")) { farZ = stored; hasFar = true; }
            }
            if (std::string_view(field.type) == "quaternion" && norm < 1e-12)
                return fail("quaternion has zero length");
        }
        if (hasNear && hasFar && farZ <= nearZ)
        {
            error = "Camera.farZ: must exceed nearZ after serialization";
            return false;
        }
        auto get = [&](const std::string& key, const std::string& fallback) {
            for (const auto& pair : values) if (pair.first == key) return pair.second;
            return fallback;
        };
        if (tag == "RigidBody" && std::strtod(get("motion", "2").c_str(), nullptr) == 2)
        {
            char persisted[80];
            std::snprintf(persisted, sizeof(persisted), "%.4f", static_cast<float>(std::strtod(get("mass", "1").c_str(), nullptr)));
            if (std::strtod(persisted, nullptr) <= 0)
            { error = "RigidBody.mass: Dynamic mass must stay positive after serialization"; return false; }
        }
        if (tag == "FaceTracking")
        {
            const auto profile = ::Resource::AssetRefs::PercentDecode(get("profile", ""));
            if (!profile.empty() && (profile.size() < 9 || profile.substr(profile.size() - 9) != ".ifacemap" ||
                                     !::Resource::AssetFS::Get().ExistsExact(profile)))
            { error = "FaceTracking.profile: requires an existing .ifacemap path (or empty)"; return false; }
        }
        if (tag == "AnimationSet")
        {
            const int count = static_cast<int>(std::strtod(get("n", "0").c_str(), nullptr));
            std::set<std::string> names;
            for (int i = 0; i < count; ++i)
            {
                const auto suffix = std::to_string(i);
                const auto name = ::Resource::AssetRefs::PercentDecode(get("name" + suffix, ""));
                if (name.empty() || !names.insert(name).second || get("path" + suffix, "").empty())
                { error = "AnimationSet: every slot needs a unique name and asset path"; return false; }
            }
            if (get("autoplay", "0") != "0" && !names.count(::Resource::AssetRefs::PercentDecode(get("default", ""))))
            { error = "AnimationSet.default: autoplay requires an existing alias"; return false; }
            // n may shrink: slot tokens past it are preserved but not loaded. ValidateSubmitted rejects a
            // submitted out-of-range index.
        }
        return true;
    }
}
