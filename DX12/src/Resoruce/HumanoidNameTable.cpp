#include "Resource/HumanoidNameTable.h"
#include "Resource/SkeletonAsset.h"

#include <cstring>
#include <cctype>
#include <string>

// Compiled with /utf-8 (EngineCore.vcxproj): the narrow string literals below carry
// UTF-8 bytes, so the Japanese PMX names compare byte-for-byte against
// SkeletonAsset::boneNames (also UTF-8).

namespace Resource
{
    namespace
    {
        struct NameRole { const char* name; HumanoidBone role; };

        // ---- Mixamo (names after the "mixamorig:" prefix is stripped) ----
        // T-pose, pure FK, perfectly consistent naming. Little finger = "Pinky".
        constexpr NameRole kMixamo[] = {
            {"Hips", HumanoidBone::Hips},
            {"Spine", HumanoidBone::Spine}, {"Spine1", HumanoidBone::Chest}, {"Spine2", HumanoidBone::UpperChest},
            {"Neck", HumanoidBone::Neck}, {"Head", HumanoidBone::Head},
            {"LeftShoulder", HumanoidBone::LeftShoulder}, {"LeftArm", HumanoidBone::LeftUpperArm},
            {"LeftForeArm", HumanoidBone::LeftLowerArm}, {"LeftHand", HumanoidBone::LeftHand},
            {"RightShoulder", HumanoidBone::RightShoulder}, {"RightArm", HumanoidBone::RightUpperArm},
            {"RightForeArm", HumanoidBone::RightLowerArm}, {"RightHand", HumanoidBone::RightHand},
            {"LeftUpLeg", HumanoidBone::LeftUpperLeg}, {"LeftLeg", HumanoidBone::LeftLowerLeg},
            {"LeftFoot", HumanoidBone::LeftFoot}, {"LeftToeBase", HumanoidBone::LeftToes},
            {"RightUpLeg", HumanoidBone::RightUpperLeg}, {"RightLeg", HumanoidBone::RightLowerLeg},
            {"RightFoot", HumanoidBone::RightFoot}, {"RightToeBase", HumanoidBone::RightToes},
            {"LeftHandThumb1", HumanoidBone::LeftThumbMetacarpal}, {"LeftHandThumb2", HumanoidBone::LeftThumbProximal}, {"LeftHandThumb3", HumanoidBone::LeftThumbDistal},
            {"LeftHandIndex1", HumanoidBone::LeftIndexProximal}, {"LeftHandIndex2", HumanoidBone::LeftIndexIntermediate}, {"LeftHandIndex3", HumanoidBone::LeftIndexDistal},
            {"LeftHandMiddle1", HumanoidBone::LeftMiddleProximal}, {"LeftHandMiddle2", HumanoidBone::LeftMiddleIntermediate}, {"LeftHandMiddle3", HumanoidBone::LeftMiddleDistal},
            {"LeftHandRing1", HumanoidBone::LeftRingProximal}, {"LeftHandRing2", HumanoidBone::LeftRingIntermediate}, {"LeftHandRing3", HumanoidBone::LeftRingDistal},
            {"LeftHandPinky1", HumanoidBone::LeftLittleProximal}, {"LeftHandPinky2", HumanoidBone::LeftLittleIntermediate}, {"LeftHandPinky3", HumanoidBone::LeftLittleDistal},
            {"RightHandThumb1", HumanoidBone::RightThumbMetacarpal}, {"RightHandThumb2", HumanoidBone::RightThumbProximal}, {"RightHandThumb3", HumanoidBone::RightThumbDistal},
            {"RightHandIndex1", HumanoidBone::RightIndexProximal}, {"RightHandIndex2", HumanoidBone::RightIndexIntermediate}, {"RightHandIndex3", HumanoidBone::RightIndexDistal},
            {"RightHandMiddle1", HumanoidBone::RightMiddleProximal}, {"RightHandMiddle2", HumanoidBone::RightMiddleIntermediate}, {"RightHandMiddle3", HumanoidBone::RightMiddleDistal},
            {"RightHandRing1", HumanoidBone::RightRingProximal}, {"RightHandRing2", HumanoidBone::RightRingIntermediate}, {"RightHandRing3", HumanoidBone::RightRingDistal},
            {"RightHandPinky1", HumanoidBone::RightLittleProximal}, {"RightHandPinky2", HumanoidBone::RightLittleIntermediate}, {"RightHandPinky3", HumanoidBone::RightLittleDistal},
        };

        // ---- Unreal Engine 5 Mannequin (lowercased) ----
        // Allowlist: ik_* / *_twist_* bones are absent, so they are excluded.
        // UE5 Manny has FIVE spine bones (spine_01..05, clavicles hang off spine_05):
        // Spine / Chest / UpperChest map to 01 / 03 / 05 so the absolute-orientation
        // transfer captures the full torso bend; 02 / 04 stay unmapped (their motion is in
        // 03 / 05's absolute rotation). UE4 rigs (spine_01..03): Chest = spine_03,
        // UpperChest unmapped (optional role).
        constexpr NameRole kUnreal[] = {
            {"pelvis", HumanoidBone::Hips},
            {"spine_01", HumanoidBone::Spine}, {"spine_03", HumanoidBone::Chest}, {"spine_05", HumanoidBone::UpperChest},
            {"neck_01", HumanoidBone::Neck}, {"head", HumanoidBone::Head},
            {"clavicle_l", HumanoidBone::LeftShoulder}, {"upperarm_l", HumanoidBone::LeftUpperArm},
            {"lowerarm_l", HumanoidBone::LeftLowerArm}, {"hand_l", HumanoidBone::LeftHand},
            {"clavicle_r", HumanoidBone::RightShoulder}, {"upperarm_r", HumanoidBone::RightUpperArm},
            {"lowerarm_r", HumanoidBone::RightLowerArm}, {"hand_r", HumanoidBone::RightHand},
            {"thigh_l", HumanoidBone::LeftUpperLeg}, {"calf_l", HumanoidBone::LeftLowerLeg},
            {"foot_l", HumanoidBone::LeftFoot}, {"ball_l", HumanoidBone::LeftToes},
            {"thigh_r", HumanoidBone::RightUpperLeg}, {"calf_r", HumanoidBone::RightLowerLeg},
            {"foot_r", HumanoidBone::RightFoot}, {"ball_r", HumanoidBone::RightToes},
            {"thumb_01_l", HumanoidBone::LeftThumbMetacarpal}, {"thumb_02_l", HumanoidBone::LeftThumbProximal}, {"thumb_03_l", HumanoidBone::LeftThumbDistal},
            {"index_01_l", HumanoidBone::LeftIndexProximal}, {"index_02_l", HumanoidBone::LeftIndexIntermediate}, {"index_03_l", HumanoidBone::LeftIndexDistal},
            {"middle_01_l", HumanoidBone::LeftMiddleProximal}, {"middle_02_l", HumanoidBone::LeftMiddleIntermediate}, {"middle_03_l", HumanoidBone::LeftMiddleDistal},
            {"ring_01_l", HumanoidBone::LeftRingProximal}, {"ring_02_l", HumanoidBone::LeftRingIntermediate}, {"ring_03_l", HumanoidBone::LeftRingDistal},
            {"pinky_01_l", HumanoidBone::LeftLittleProximal}, {"pinky_02_l", HumanoidBone::LeftLittleIntermediate}, {"pinky_03_l", HumanoidBone::LeftLittleDistal},
            {"thumb_01_r", HumanoidBone::RightThumbMetacarpal}, {"thumb_02_r", HumanoidBone::RightThumbProximal}, {"thumb_03_r", HumanoidBone::RightThumbDistal},
            {"index_01_r", HumanoidBone::RightIndexProximal}, {"index_02_r", HumanoidBone::RightIndexIntermediate}, {"index_03_r", HumanoidBone::RightIndexDistal},
            {"middle_01_r", HumanoidBone::RightMiddleProximal}, {"middle_02_r", HumanoidBone::RightMiddleIntermediate}, {"middle_03_r", HumanoidBone::RightMiddleDistal},
            {"ring_01_r", HumanoidBone::RightRingProximal}, {"ring_02_r", HumanoidBone::RightRingIntermediate}, {"ring_03_r", HumanoidBone::RightRingDistal},
            {"pinky_01_r", HumanoidBone::RightLittleProximal}, {"pinky_02_r", HumanoidBone::RightLittleIntermediate}, {"pinky_03_r", HumanoidBone::RightLittleDistal},
        };

        // ---- PMX / MMD Japanese semi-standard bone names ----
        // Exact UTF-8 match: it keeps the leg-IK, leg-D and toe-IK bones from colliding
        // with the leg and ankle bones. Finger digits are full-width (0..3) per the MMD
        // standard.
        // Hips SPLIT: MMD has no single Hips. Pelvis ROTATION -> the lower-body bone (this
        // table); root TRANSLATION -> the center bone (HumanoidProfile::hipsTranslationBone,
        // set by the builder). The full-pose delta writes each mapped bone's ABSOLUTE
        // model-space orientation, so the torso (the upper-body subtree, a sibling of the
        // lower body) still follows through its own Spine / Chest slots.
        // Legs map to the BASE FK bones (leg / knee / ankle); on leg-D rigs the runtime
        // rotation grant propagates the baked FK to the deform chain - do NOT map the
        // D / EX bones. Retargeted clips disable leg IK per clip (LegDriveMode::FK), so the
        // base bones keep their baked rotations.
        const NameRole kPmx[] = {
            {"下半身", HumanoidBone::Hips},
            {"上半身", HumanoidBone::Spine}, {"上半身2", HumanoidBone::Chest}, {"上半身3", HumanoidBone::UpperChest},
            {"首", HumanoidBone::Neck}, {"頭", HumanoidBone::Head},
            {"左目", HumanoidBone::LeftEye}, {"右目", HumanoidBone::RightEye},
            {"左肩", HumanoidBone::LeftShoulder}, {"左腕", HumanoidBone::LeftUpperArm},
            {"左ひじ", HumanoidBone::LeftLowerArm}, {"左手首", HumanoidBone::LeftHand},
            {"右肩", HumanoidBone::RightShoulder}, {"右腕", HumanoidBone::RightUpperArm},
            {"右ひじ", HumanoidBone::RightLowerArm}, {"右手首", HumanoidBone::RightHand},
            {"左足", HumanoidBone::LeftUpperLeg}, {"左ひざ", HumanoidBone::LeftLowerLeg},
            {"左足首", HumanoidBone::LeftFoot}, {"左つま先", HumanoidBone::LeftToes},
            {"右足", HumanoidBone::RightUpperLeg}, {"右ひざ", HumanoidBone::RightLowerLeg},
            {"右足首", HumanoidBone::RightFoot}, {"右つま先", HumanoidBone::RightToes},
            // Many PMX rigs have NO FK toe bone, only a toe-IK control bone whose position IS
            // the toe tip. Mapping it to Toes gives the FOOT a FORWARD direction (otherwise it
            // falls back to the shin-down direction and the ankle twists). The FK toe above
            // wins when present (earlier index); Toes only orient the foot and are never
            // retargeted (RetargetBaker::IsRetargetRole), so nothing is written to the IK bone.
            {"左つま先ＩＫ", HumanoidBone::LeftToes}, {"右つま先ＩＫ", HumanoidBone::RightToes},
            {"左親指０", HumanoidBone::LeftThumbMetacarpal}, {"左親指１", HumanoidBone::LeftThumbProximal}, {"左親指２", HumanoidBone::LeftThumbDistal},
            {"左人指１", HumanoidBone::LeftIndexProximal}, {"左人指２", HumanoidBone::LeftIndexIntermediate}, {"左人指３", HumanoidBone::LeftIndexDistal},
            {"左中指１", HumanoidBone::LeftMiddleProximal}, {"左中指２", HumanoidBone::LeftMiddleIntermediate}, {"左中指３", HumanoidBone::LeftMiddleDistal},
            {"左薬指１", HumanoidBone::LeftRingProximal}, {"左薬指２", HumanoidBone::LeftRingIntermediate}, {"左薬指３", HumanoidBone::LeftRingDistal},
            {"左小指１", HumanoidBone::LeftLittleProximal}, {"左小指２", HumanoidBone::LeftLittleIntermediate}, {"左小指３", HumanoidBone::LeftLittleDistal},
            {"右親指０", HumanoidBone::RightThumbMetacarpal}, {"右親指１", HumanoidBone::RightThumbProximal}, {"右親指２", HumanoidBone::RightThumbDistal},
            {"右人指１", HumanoidBone::RightIndexProximal}, {"右人指２", HumanoidBone::RightIndexIntermediate}, {"右人指３", HumanoidBone::RightIndexDistal},
            {"右中指１", HumanoidBone::RightMiddleProximal}, {"右中指２", HumanoidBone::RightMiddleIntermediate}, {"右中指３", HumanoidBone::RightMiddleDistal},
            {"右薬指１", HumanoidBone::RightRingProximal}, {"右薬指２", HumanoidBone::RightRingIntermediate}, {"右薬指３", HumanoidBone::RightRingDistal},
            {"右小指１", HumanoidBone::RightLittleProximal}, {"右小指２", HumanoidBone::RightLittleIntermediate}, {"右小指３", HumanoidBone::RightLittleDistal},
        };

        template <size_t N>
        HumanoidBone LookupExact(const NameRole (&table)[N], const char* name)
        {
            for (const auto& e : table)
                if (std::strcmp(e.name, name) == 0) return e.role;
            return HumanoidBone::Count;
        }
        HumanoidBone LookupExact(const NameRole* table, size_t n, const char* name)
        {
            for (size_t i = 0; i < n; ++i)
                if (std::strcmp(table[i].name, name) == 0) return table[i].role;
            return HumanoidBone::Count;
        }

        std::string ToLower(const char* s)
        {
            std::string r(s);
            for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return r;
        }

        bool StartsWith(const char* s, const char* prefix)
        {
            return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
        }

        // ---- Generic-English best-effort (Unity / hand-named FBX) ----
        // Normalize (lowercase, drop _ - . : space) then side + part heuristics.
        std::string Normalize(const char* s)
        {
            std::string r;
            for (const char* p = s; *p; ++p)
            {
                char c = *p;
                if (c == '_' || c == '-' || c == '.' || c == ':' || c == ' ') continue;
                r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return r;
        }
        bool Contains(const std::string& hay, const char* needle)
        {
            return hay.find(needle) != std::string::npos;
        }

        HumanoidBone GenericRole(const char* boneName)
        {
            const std::string n = Normalize(boneName);
            if (n.empty()) return HumanoidBone::Count;

            const bool left  = Contains(n, "left") || Contains(n, "_l") /* dead: Normalize strips '_' */;
            // Normalize stripped the separators: look for the word "left" / "right" or a
            // trailing single 'l' / 'r' after a body token.
            int side = 0; // 0 = center, 1 = left, 2 = right
            if (Contains(n, "left"))  side = 1;
            else if (Contains(n, "right")) side = 2;
            else if (!n.empty() && (n.back() == 'l')) side = 1;
            else if (!n.empty() && (n.back() == 'r')) side = 2;
            (void)left;

            auto sided = [side](HumanoidBone l, HumanoidBone r, HumanoidBone c) {
                return side == 2 ? r : (side == 1 ? l : c);
            };

            // Fingers first (more specific than "hand").
            auto fingerPhalanx = [&](const char* base, HumanoidBone lp, HumanoidBone li, HumanoidBone ld,
                                                       HumanoidBone rp, HumanoidBone ri, HumanoidBone rd) -> HumanoidBone {
                if (!Contains(n, base)) return HumanoidBone::Count;
                const bool distal = Contains(n, "distal") || Contains(n, "3");
                const bool inter  = Contains(n, "intermediate") || Contains(n, "middle2") || Contains(n, "2");
                const HumanoidBone L = distal ? ld : (inter ? li : lp);
                const HumanoidBone R = distal ? rd : (inter ? ri : rp);
                return sided(L, R, HumanoidBone::Count);
            };
            if (Contains(n, "thumb"))
                return sided(Contains(n,"distal")||Contains(n,"3") ? HumanoidBone::LeftThumbDistal
                             : Contains(n,"proximal")||Contains(n,"2") ? HumanoidBone::LeftThumbProximal
                             : HumanoidBone::LeftThumbMetacarpal,
                             Contains(n,"distal")||Contains(n,"3") ? HumanoidBone::RightThumbDistal
                             : Contains(n,"proximal")||Contains(n,"2") ? HumanoidBone::RightThumbProximal
                             : HumanoidBone::RightThumbMetacarpal, HumanoidBone::Count);
            if (HumanoidBone f = fingerPhalanx("index", HumanoidBone::LeftIndexProximal, HumanoidBone::LeftIndexIntermediate, HumanoidBone::LeftIndexDistal,
                                                        HumanoidBone::RightIndexProximal, HumanoidBone::RightIndexIntermediate, HumanoidBone::RightIndexDistal); f != HumanoidBone::Count) return f;
            if (HumanoidBone f = fingerPhalanx("middle", HumanoidBone::LeftMiddleProximal, HumanoidBone::LeftMiddleIntermediate, HumanoidBone::LeftMiddleDistal,
                                                         HumanoidBone::RightMiddleProximal, HumanoidBone::RightMiddleIntermediate, HumanoidBone::RightMiddleDistal); f != HumanoidBone::Count) return f;
            if (HumanoidBone f = fingerPhalanx("ring", HumanoidBone::LeftRingProximal, HumanoidBone::LeftRingIntermediate, HumanoidBone::LeftRingDistal,
                                                       HumanoidBone::RightRingProximal, HumanoidBone::RightRingIntermediate, HumanoidBone::RightRingDistal); f != HumanoidBone::Count) return f;
            if (Contains(n,"little") || Contains(n,"pinky"))
                if (HumanoidBone f = fingerPhalanx(Contains(n,"little")?"little":"pinky",
                        HumanoidBone::LeftLittleProximal, HumanoidBone::LeftLittleIntermediate, HumanoidBone::LeftLittleDistal,
                        HumanoidBone::RightLittleProximal, HumanoidBone::RightLittleIntermediate, HumanoidBone::RightLittleDistal); f != HumanoidBone::Count) return f;

            // Arms / legs (check the more specific token first).
            if (Contains(n, "forearm") || Contains(n, "lowerarm"))
                return sided(HumanoidBone::LeftLowerArm, HumanoidBone::RightLowerArm, HumanoidBone::Count);
            if (Contains(n, "upperarm") || (Contains(n, "arm") && !Contains(n, "forearm")))
                return sided(HumanoidBone::LeftUpperArm, HumanoidBone::RightUpperArm, HumanoidBone::Count);
            if (Contains(n, "shoulder") || Contains(n, "clavicle"))
                return sided(HumanoidBone::LeftShoulder, HumanoidBone::RightShoulder, HumanoidBone::Count);
            if (Contains(n, "hand") || Contains(n, "wrist"))
                return sided(HumanoidBone::LeftHand, HumanoidBone::RightHand, HumanoidBone::Count);
            if (Contains(n, "upperleg") || Contains(n, "upleg") || Contains(n, "thigh"))
                return sided(HumanoidBone::LeftUpperLeg, HumanoidBone::RightUpperLeg, HumanoidBone::Count);
            if (Contains(n, "lowerleg") || Contains(n, "calf") || Contains(n, "shin") || Contains(n, "knee"))
                return sided(HumanoidBone::LeftLowerLeg, HumanoidBone::RightLowerLeg, HumanoidBone::Count);
            if (Contains(n, "foot") || Contains(n, "ankle"))
                return sided(HumanoidBone::LeftFoot, HumanoidBone::RightFoot, HumanoidBone::Count);
            if (Contains(n, "toe") || Contains(n, "ball"))
                return sided(HumanoidBone::LeftToes, HumanoidBone::RightToes, HumanoidBone::Count);
            if (Contains(n, "eye"))
                return sided(HumanoidBone::LeftEye, HumanoidBone::RightEye, HumanoidBone::Count);
            // Leg-generic fallback ("leftleg"/"rightleg" with no upper/lower token).
            if (Contains(n, "leg"))
                return sided(HumanoidBone::LeftUpperLeg, HumanoidBone::RightUpperLeg, HumanoidBone::Count);

            // Center chain.
            if (Contains(n, "upperchest")) return HumanoidBone::UpperChest;
            if (Contains(n, "chest"))      return HumanoidBone::Chest;
            if (Contains(n, "spine"))      return HumanoidBone::Spine; // spine/spine1 collapse to Spine here
            if (Contains(n, "neck"))       return HumanoidBone::Neck;
            if (Contains(n, "head"))       return HumanoidBone::Head;
            if (Contains(n, "hips") || Contains(n, "pelvis")) return HumanoidBone::Hips;
            if (Contains(n, "jaw"))        return HumanoidBone::Jaw;
            return HumanoidBone::Count;
        }
    } // namespace

    HumanoidBone RoleFromCanonicalName(const char* name)
    {
        if (!name || !*name) return HumanoidBone::Count;
        for (uint32_t i = 0; i < kHumanoidBoneCount; ++i)
        {
            HumanoidBone r = static_cast<HumanoidBone>(i);
            if (std::strcmp(HumanoidRoleName(r), name) == 0) return r;
        }
        return HumanoidBone::Count;
    }

    HumanoidBone MapBoneNameToRole(HumanoidConvention conv, const char* boneName)
    {
        if (!boneName || !*boneName) return HumanoidBone::Count;

        switch (conv)
        {
        case HumanoidConvention::Mixamo:
        {
            const char* n = boneName;
            if (StartsWith(n, "mixamorig:")) n += 10;
            else if (StartsWith(n, "mixamorig")) n += 9; // some exporters drop the colon
            return LookupExact(kMixamo, n);
        }
        case HumanoidConvention::UnrealMannequin:
        {
            const std::string lower = ToLower(boneName);
            return LookupExact(kUnreal, lower.c_str());
        }
        case HumanoidConvention::PmxJp:
            return LookupExact(kPmx, sizeof(kPmx) / sizeof(kPmx[0]), boneName);

        case HumanoidConvention::Vrm1:
            return RoleFromCanonicalName(boneName);

        case HumanoidConvention::Vrm0:
        {
            // VRM 0.x thumb dialect: Proximal/Intermediate/Distal -> our
            // Metacarpal/Proximal/Distal. Non-thumb names are identical to VRM 1.0.
            if (std::strcmp(boneName, "leftThumbProximal")     == 0) return HumanoidBone::LeftThumbMetacarpal;
            if (std::strcmp(boneName, "leftThumbIntermediate") == 0) return HumanoidBone::LeftThumbProximal;
            if (std::strcmp(boneName, "rightThumbProximal")     == 0) return HumanoidBone::RightThumbMetacarpal;
            if (std::strcmp(boneName, "rightThumbIntermediate") == 0) return HumanoidBone::RightThumbProximal;
            return RoleFromCanonicalName(boneName);
        }
        case HumanoidConvention::GenericEnglish:
            return GenericRole(boneName);

        case HumanoidConvention::Unknown:
        default:
            return HumanoidBone::Count;
        }
    }

    HumanoidConvention DetectConvention(const SkeletonAsset& skel)
    {
        bool hasPelvis = false, hasSpine01 = false, hasUeIk = false;
        bool hasPmxUpper = false, hasHead = false, hasPmxArm = false;

        // Real bones only: a virtual bone is no naming signature.
        for (uint32_t i = 0; i < skel.RealBoneCount(); ++i)
        {
            const char* nm = skel.boneNames[i];
            if (StartsWith(nm, "mixamorig"))
                return HumanoidConvention::Mixamo;

            // PMX signature tokens (exact).
            if (std::strcmp(nm, "上半身") == 0) hasPmxUpper = true;
            if (std::strcmp(nm, "頭") == 0)     hasHead = true;
            if (std::strcmp(nm, "左腕") == 0 || std::strcmp(nm, "右腕") == 0) hasPmxArm = true;
            if (std::strcmp(nm, "センター") == 0) hasPmxUpper = true;

            // Unreal signature.
            std::string lo = ToLower(nm);
            if (lo == "pelvis")   hasPelvis = true;
            if (lo == "spine_01") hasSpine01 = true;
            if (lo == "ik_foot_root" || lo == "ik_hand_root") hasUeIk = true;
        }

        if (hasPmxUpper || (hasHead && hasPmxArm)) return HumanoidConvention::PmxJp;
        if (hasPelvis && (hasSpine01 || hasUeIk))  return HumanoidConvention::UnrealMannequin;

        // Fall back to generic-English if enough required roles resolve.
        uint32_t resolved = 0;
        bool seen[kHumanoidBoneCount] = {};
        for (uint32_t i = 0; i < skel.RealBoneCount(); ++i)
        {
            HumanoidBone r = GenericRole(skel.boneNames[i]);
            if (r != HumanoidBone::Count && IsRequiredHumanoidBone(r) && !seen[(uint32_t)r])
            {
                seen[(uint32_t)r] = true;
                ++resolved;
            }
        }
        if (resolved >= 10) return HumanoidConvention::GenericEnglish; // most of the 15 required
        return HumanoidConvention::Unknown;
    }
}
