#pragma once

// ChainApply - the non-UI half of the Chain Physics Preview's "Apply -> .ipfb",
// split out so tests/unit/chain_preview_prefab_test.cpp links and drives the
// SAME code the panel runs. It writes through ComponentTransferPrefab.cpp, the
// only sanctioned .ipfb writer (LoadPrefabDoc -> SetNodeComponentBlob ->
// SavePrefabDoc, which writes the .bak and patches AssetHeader::dataSize and
// PrefabMetadata::textLength). Never hand-edit or sed a .ipfb.
// No ImGui, no World, no renderer: text in, text out, plus the file.

#include "Editor/ComponentTransferWindow.h"   // Editor::ComponentTransfer::PrefabDoc

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SkeletonAsset;

namespace Editor::ChainApply
{
    // FNV-1a over boneCount and every bone NAME in index order - the rig identity
    // ChainPhysicsComponent is authored against (bone names are its serialized
    // identity; CapsuleCollider is authored by INDEX).
    std::uint32_t BoneNameHash(const SkeletonAsset& skel);

    // The node that carries the character's ChainPhysics block: the ONE node with
    // a "ChainPhysics" tag; with several, the one named `carrierName`; with none,
    // the one named `carrierName` that has a SceneRef (a subject whose chains were
    // auto-detected in the panel). Returns -1 and sets *why when ambiguous or
    // absent.
    int FindChainNode(const ComponentTransfer::PrefabDoc& doc, const std::string& carrierName,
                      std::string* why);

    // The skeleton the node's character is spawned with: its SceneRef
    // (percent-decoded) -> that .iscn's "K file=" line -> the .iskel path beside
    // it. Empty when any link is missing (the rig is then UNKNOWN).
    std::string SkeletonPathOfNode(const ComponentTransfer::PrefabDoc& doc, int node);

    // The node's CapsuleCollider bone indices, enabled capsules only.
    std::vector<std::pair<std::uint32_t, std::uint32_t>>
    CapsuleBonesOfNode(const ComponentTransfer::PrefabDoc& doc, int node);

    struct PrefabWrite
    {
        bool          ok = false;          // the file was rewritten
        int           node = -1;
        std::string   err;
        bool          bak = false;         // <path>.bak exists after the write
        bool          unchanged = false;   // the blob was already byte-identical
        std::uint32_t textLength = 0;      // PrefabMetadata::textLength as written
        std::uint32_t dataSize = 0;        // AssetHeader::dataSize as written
        bool          sizeOk = false;      // dataSize == textLength + 1 == file - header - meta
        bool          roundTrip = false;   // reloaded: the node's ChainPhysics blob == the one written
        std::uint64_t fileBytes = 0;
    };
    // Replaces the ChainPhysics blob of the carrier node of `path` and saves,
    // then VERIFIES the file on disk: the header sizes and a reload that finds the
    // same blob. `blob` is the serializer's "  ChainPhysics: ...\n" line.
    PrefabWrite WriteChainBlob(const std::string& path, const std::string& carrierName,
                               const std::string& blob);
}
