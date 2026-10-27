// Skeleton.* - see the header. Read-only: nothing here writes a component.

#include "ECS/LuaSkeletonBindings.h"

#include "ECS/AnimEntityResolve.h"       // Anim::ResolveAnimEntityOrSelf
#include "ECS/AnimationComponents.h"     // SkeletonComponent / SocketComponent
#include "ECS/AnimationSystem.h"         // BuildBoneMask
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"     // GlobalTransform (BoneWorld)
#include "ECS/LocomotionComponents.h"    // BoneMaskMode
#include "Graphics/SkinningBuffers.h"    // PoseRingBuffer (BoneWorld)
#include "Resource/HumanoidBone.h"
#include "Resource/SkeletonAsset.h"

#include <sol/sol.hpp>

#include "Scripting/LuaDoc.h"        // Skeleton.BoneWorld is described as it is bound
#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (MUST follow sol)

#include <DirectXMath.h>

#include <cstring>
#include <string>
#include <vector>

using namespace DirectX;

namespace
{
    std::string BoneNameOf(const SkeletonAsset& s, uint32_t b)
    {
        return std::string(s.boneNames[b], strnlen(s.boneNames[b], sizeof(s.boneNames[b])));
    }

    // The skeleton carrier under @p v (a character root or the carrier itself).
    Entity Carrier(World& world, const LuaEntity& v)
    {
        const Entity e = LuaId::ResolveLive(world, v);
        if (e == NullEntity) return NullEntity;
        return Anim::ResolveAnimEntityOrSelf(world, e);
    }

    sol::table Vec(sol::state_view lua, float x, float y, float z)
    {
        return lua.create_table_with("x", x, "y", y, "z", z);
    }
}

void RegisterLuaSkeletonBindings(sol::state& lua, World& world, SkeletonRegistry& skeletons,
                                 const PoseRingBuffer* poses)
{
    sol::table sk = lua["Skeleton"].get_or_create<sol::table>();

    // The asset of @p e's carrier, or null.
    auto assetOf = [&world, &skeletons](const LuaEntity& v, Entity& carrier) -> const SkeletonAsset*
    {
        carrier = Carrier(world, v);
        if (carrier == NullEntity) return nullptr;
        const SkeletonComponent* sc = world.GetComponent<SkeletonComponent>(carrier);
        if (!sc || sc->assetIndex == kInvalidSkeletonIndex || sc->assetIndex >= skeletons.Count()) return nullptr;
        return &skeletons.Get(sc->assetIndex);
    };

    sk.set_function("Bones", [assetOf](sol::this_state ts, const LuaEntity& v) -> sol::object
    {
        sol::state_view lua(ts);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        if (!s) return sol::make_object(lua, sol::lua_nil);
        sol::table out = lua.create_table();
        const uint32_t real = s->RealBoneCount();
        for (uint32_t b = 0; b < s->boneCount; ++b)
            out[b + 1] = lua.create_table_with("index", b, "name", BoneNameOf(*s, b), "parent", s->parentIndex[b],
                                               "virtual", b >= real);
        return sol::make_object(lua, out);
    });

    // The model's virtual bones (its VB records).
    sk.set_function("VirtualBones", [assetOf](sol::this_state ts, const LuaEntity& v) -> sol::object
    {
        sol::state_view lua(ts);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        if (!s) return sol::make_object(lua, sol::lua_nil);
        sol::table out = lua.create_table();
        int i = 0;
        for (const SkeletonAsset::VirtualBoneDef& d : s->virtualBones)
            out[++i] = lua.create_table_with("index", d.index, "name", BoneNameOf(*s, d.index),
                                             "from", BoneNameOf(*s, d.from), "to", BoneNameOf(*s, d.to),
                                             "fromIndex", d.from, "toIndex", d.to);
        return sol::make_object(lua, out);
    });

    sk.set_function("Find", [assetOf](sol::this_state ts, const LuaEntity& v, const std::string& name) -> sol::object
    {
        sol::state_view lua(ts);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        if (s)
            for (uint32_t b = 0; b < s->boneCount; ++b)
                if (BoneNameOf(*s, b) == name) return sol::make_object(lua, static_cast<int>(b));
        return sol::make_object(lua, sol::lua_nil);
    });

    sk.set_function("Sockets", [&world, assetOf](sol::this_state ts, const LuaEntity& v) -> sol::object
    {
        sol::state_view lua(ts);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        const Entity e = carrier != NullEntity ? carrier : LuaId::ResolveLive(world, v);
        const SocketComponent* sc = e != NullEntity ? world.GetComponent<SocketComponent>(e) : nullptr;
        if (!sc) return sol::make_object(lua, sol::lua_nil);
        sol::table out = lua.create_table();
        for (uint32_t i = 0; i < sc->count; ++i)
        {
            const SocketComponent::Socket& so = sc->sockets[i];
            const XMFLOAT4X4& m = so.worldTransform;
            XMVECTOR scl, rot, tr;
            XMFLOAT4 q = { 0.f, 0.f, 0.f, 1.f };
            if (XMMatrixDecompose(&scl, &rot, &tr, XMLoadFloat4x4(&m))) XMStoreFloat4(&q, rot);
            sol::table row = lua.create_table();
            row["name"]     = std::string(so.name, strnlen(so.name, sizeof(so.name)));
            row["bone"]     = so.boneIndex;
            row["boneName"] = so.boneName[0] ? std::string(so.boneName, strnlen(so.boneName, sizeof(so.boneName)))
                              : (s && so.boneIndex < s->boneCount ? BoneNameOf(*s, so.boneIndex) : std::string());
            row["derived"]  = (so.flags & SocketComponent::kDerived) != 0;
            row["mesh"]     = (so.flags & SocketComponent::kMeshSocket) != 0;   // a Mesh's socket, no bone
            row["missing"]  = (so.flags & SocketComponent::kBoneMissing) != 0;
            row["pos"]      = Vec(lua, m._41, m._42, m._43);
            row["rot"]      = lua.create_table_with("x", q.x, "y", q.y, "z", q.z, "w", q.w);
            // The offset in bone space, and the Inspector's Euler seed for it
            // (degrees; XMQuaternionRotationRollPitchYaw rebuilds the rotation).
            XMVECTOR ls, lr, lt;
            if (XMMatrixDecompose(&ls, &lr, &lt, XMLoadFloat4x4(&so.localOffset)))
            {
                XMFLOAT3 op, os;
                XMFLOAT4 oq;
                XMStoreFloat3(&op, lt);
                XMStoreFloat3(&os, ls);
                XMStoreFloat4(&oq, lr);
                row["offset"] = lua.create_table_with("pos", Vec(lua, op.x, op.y, op.z),
                                                      "rot", lua.create_table_with("x", oq.x, "y", oq.y, "z", oq.z, "w", oq.w),
                                                      "scale", Vec(lua, os.x, os.y, os.z));
            }
            row["eulerDeg"] = Vec(lua, so.rotationEulerDeg.x, so.rotationEulerDeg.y, so.rotationEulerDeg.z);
            out[i + 1] = row;
        }
        return sol::make_object(lua, out);
    });

    sk.set_function("SocketWorld", [&world](sol::this_state ts, const LuaEntity& v, const std::string& name) -> sol::object
    {
        sol::state_view lua(ts);
        Entity e = Carrier(world, v);
        const SocketComponent* sc = e != NullEntity ? world.GetComponent<SocketComponent>(e) : nullptr;
        if (!sc) return sol::make_object(lua, sol::lua_nil);
        for (uint32_t i = 0; i < sc->count; ++i)
        {
            const SocketComponent::Socket& so = sc->sockets[i];
            if (name != std::string(so.name, strnlen(so.name, sizeof(so.name)))) continue;
            // Its bone is gone: not placed on anything (SocketSystem keeps it on the character only so a
            // follower does not fly to the origin).
            if (so.flags & SocketComponent::kBoneMissing) return sol::make_object(lua, sol::lua_nil);
            XMVECTOR scl, rot, tr;
            if (!XMMatrixDecompose(&scl, &rot, &tr, XMLoadFloat4x4(&so.worldTransform)))
                return sol::make_object(lua, sol::lua_nil);
            XMFLOAT3 p, s3;
            XMFLOAT4 q;
            XMStoreFloat3(&p, tr);
            XMStoreFloat3(&s3, scl);
            XMStoreFloat4(&q, rot);
            return sol::make_object(lua, lua.create_table_with(
                "pos", Vec(lua, p.x, p.y, p.z), "rot", lua.create_table_with("x", q.x, "y", q.y, "z", q.z, "w", q.w),
                "scale", Vec(lua, s3.x, s3.y, s3.z)));
        }
        return sol::make_object(lua, sol::lua_nil);
    });

    // A bone where the GPU skinned it: the pose ring LocalToWorld filled (bindPose x skin x the
    // carrier's world, SocketSystem's formula), so it reflects the LAST frame and everything that posed
    // it (clips, IK, HeadTrack, face tracking, chain physics). nil before the first pose, for a bone the
    // skeleton lacks, or without the ring.
    LuaDoc::Fn(sk, "Skeleton", "BoneWorld", [&world, assetOf, poses](sol::this_state ts, const LuaEntity& v, const std::string& name) -> sol::object
    {
        sol::state_view lua(ts);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        if (!s || !poses) return sol::make_object(lua, sol::lua_nil);
        const SkeletonComponent* sc = world.GetComponent<SkeletonComponent>(carrier);
        if (!sc || sc->poseByteOffset == ~0u) return sol::make_object(lua, sol::lua_nil);
        uint32_t b = 0;
        while (b < s->boneCount && BoneNameOf(*s, b) != name) ++b;
        if (b >= s->boneCount || b >= sc->boneCount) return sol::make_object(lua, sol::lua_nil);
        const XMFLOAT4X4* skin = poses->ReadMapped(sc->poseByteOffset);
        if (!skin) return sol::make_object(lua, sol::lua_nil);
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(carrier);
        const XMMATRIX rootWorld = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();
        const XMMATRIX boneWorld = XMLoadFloat4x4(&s->bindPose[b]) * XMLoadFloat4x4(&skin[b]) * rootWorld;
        XMVECTOR scl, rot, tr;
        if (!XMMatrixDecompose(&scl, &rot, &tr, boneWorld)) return sol::make_object(lua, sol::lua_nil);
        XMFLOAT3 p, s3;
        XMFLOAT4 q;
        XMStoreFloat3(&p, tr);
        XMStoreFloat3(&s3, scl);
        XMStoreFloat4(&q, XMQuaternionNormalize(rot));
        return sol::make_object(lua, lua.create_table_with(
            "pos", Vec(lua, p.x, p.y, p.z), "rot", lua.create_table_with("x", q.x, "y", q.y, "z", q.z, "w", q.w),
            "scale", Vec(lua, s3.x, s3.y, s3.z), "bone", static_cast<int>(b)));
    },
        { .doc = "A bone as the GPU last skinned it - {pos, rot = {x, y, z, w}, scale, bone = index} in world space, "
                 "from the pose ring (bindPose x skin x the skeleton entity's world, the socket formula): last "
                 "frame's pose with clips, IK, HeadTrack, face tracking and chain physics in it. nil before the "
                 "first pose or for a bone the skeleton lacks; the name is matched exactly.",
          .params = { { "id", "the character (its root or its skeleton entity)" }, { "bone", "the bone name" } },
          .returns = "table?" });

    // The humanoid profile the skeleton plays with (the registry's copy: the .iskel's, the runtime's
    // lazy build, or an editor's edit).
    sk.set_function("Humanoid", [assetOf](sol::this_state ts, const LuaEntity& v) -> sol::object
    {
        sol::state_view lua(ts);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        if (!s) return sol::make_object(lua, sol::lua_nil);
        const HumanoidProfile& hp = s->humanoid;
        sol::table roles = lua.create_table();
        int mapped = 0;
        for (uint32_t r = 0; r < kHumanoidBoneCount; ++r)
            if (hp.boneForRole[r] >= 0)
            {
                roles[HumanoidRoleName(static_cast<HumanoidBone>(r))] = hp.boneForRole[r];
                ++mapped;
            }
        sol::table out = lua.create_table();
        out["valid"]           = hp.IsValid();
        out["convention"]      = static_cast<int>(hp.convention);
        out["userEdited"]      = hp.userEdited != 0;
        out["mapped"]          = mapped;
        out["hipsTranslation"] = hp.hipsTranslationBone;
        out["roles"]           = roles;
        return sol::make_object(lua, out);
    });

    // The bones an AnimLayer with this mask drives (AnimationSystem::BuildBoneMask).
    sk.set_function("BoneMask", [assetOf](sol::this_state ts, const LuaEntity& v, const std::string& mode,
                                          sol::optional<std::string> root) -> sol::object
    {
        sol::state_view lua(ts);
        BoneMaskMode m = BoneMaskMode::FullBody;
        if (mode == "full") m = BoneMaskMode::FullBody;
        else if (mode == "upper") m = BoneMaskMode::UpperBody;
        else if (mode == "lower") m = BoneMaskMode::LowerBody;
        else if (mode == "custom") m = BoneMaskMode::CustomRoot;
        else return sol::make_object(lua, sol::lua_nil);
        Entity carrier = NullEntity;
        const SkeletonAsset* s = assetOf(v, carrier);
        if (!s) return sol::make_object(lua, sol::lua_nil);
        std::vector<float> w;
        const bool resolved = AnimationSystem::BuildBoneMask(*s, static_cast<uint8_t>(m), root.value_or(""), w);
        sol::table bones = lua.create_table();
        int n = 0;
        for (uint32_t b = 0; b < w.size(); ++b)
            if (w[b] > 0.5f) bones[++n] = b;
        return sol::make_object(lua, lua.create_table_with("resolved", resolved, "bones", bones));
    });
}
