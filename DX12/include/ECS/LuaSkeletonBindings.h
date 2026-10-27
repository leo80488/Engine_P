#pragma once
// Skeleton.* - read-only Lua queries of a character's skeleton: its bones by name, and its sockets
// with the world transform SocketSystem wrote this frame - the model's own (derived from its SO
// records at spawn) and the entity's (its Sockets record). A character ROOT or the skeleton carrier
// both work (Anim::ResolveAnimEntityOrSelf).
//
//   Skeleton.Bones(e)              -> { {index, name, parent, virtual}, ... } | nil
//   Skeleton.VirtualBones(e)       -> { {index, name, from, to, fromIndex, toIndex}, ... } | nil
//                                     (the model's VB records, appended after the real bones)
//   Skeleton.Find(e, name)         -> bone index | nil   (exact, case-sensitive)
//   Skeleton.Sockets(e)            -> { {name, bone, boneName, derived, missing, mesh, pos = {x,y,z},
//                                        rot = {x,y,z,w}, offset = {pos, rot, scale},
//                                        eulerDeg = {x,y,z}}, ... }
//                                     bone = its index; offset is in bone space; missing = its bone
//                                     NAME is not in the skeleton (not placed); mesh = a StaticMesh
//                                     owner's mesh socket (no bone: offset x the entity's world, as
//                                     for every socket of an entity without a skeleton)
//   Skeleton.SocketWorld(e, name)  -> {pos = {x,y,z}, rot = {x,y,z,w}, scale = {x,y,z}} | nil
//   Skeleton.BoneWorld(e, boneName) -> {pos, rot, scale, bone = index} | nil
//                                     (the bone as last skinned, from the pose ring)
//   Skeleton.Humanoid(e)           -> {valid, convention, userEdited, mapped, hipsTranslation,
//                                      roles = {<role> = bone index, ...}} | nil
//                                     (the profile this skeleton plays with - retargeting, the
//                                     Upper / Lower Body layer masks; role names as HumanoidRoleName)
//   Skeleton.BoneMask(e, mode [, root]) -> {resolved, bones = {index, ...}} | nil
//                                     (the bones an AnimLayer of @p mode "full" | "upper" | "lower" |
//                                     "custom" (@p root = the bone) drives, per
//                                     AnimationSystem::BuildBoneMask; resolved false = the full-body
//                                     fallback)

namespace sol { class state; }
class World;
class SkeletonRegistry;
class PoseRingBuffer;

// @p poses (optional): Skeleton.BoneWorld's source; without it that answers nil.
void RegisterLuaSkeletonBindings(sol::state& lua, World& world, SkeletonRegistry& skeletons,
                                 const PoseRingBuffer* poses = nullptr);
