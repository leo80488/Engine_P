#pragma once

#include "Resource/AssetHeader.h"

#include <cstddef>
#include <cstdint>

// The migration chain of one asset format: a file older than the format's current version is
// lifted to it, one version at a time, on the decoded data before anything reads it. The shape
// is the save reader's kMigrations (Gameplay/SaveGame.cpp); the version range is the format's
// row in Resource::kAssetFormats.
//
//   struct Decoded { ... };                       // what the reader has parsed so far
//   static void Lift1To2(Decoded& d) { ... }
//   static constexpr Resource::MigrateStep<Decoded> kMigrations[] = { Lift1To2 };
//   ...
//   Resource::MigrateAsset<Resource::MAGIC_X>(kMigrations, header.version, decoded);
//
// chain[i] lifts version MinimumVersion + i to MinimumVersion + i + 1. A step that changed
// nothing but the number is still listed, so the chain has no gap, and the call does not compile
// unless the chain covers every version from minimum to current.
namespace Resource
{
    template <class Data>
    using MigrateStep = void (*)(Data& data);

    // The steps from @p fromVersion up to @p current, in order; chain[0] lifts @p minimum.
    template <class Data, std::size_t N>
    void RunMigrationChain(const MigrateStep<Data> (&chain)[N], uint16_t minimum, uint16_t current,
                           uint16_t fromVersion, Data& data)
    {
        if (fromVersion < minimum)
            fromVersion = minimum;
        for (uint16_t v = fromVersion; v < current && static_cast<std::size_t>(v - minimum) < N; ++v)
            chain[v - minimum](data);
    }

    template <uint32_t Magic, class Data, std::size_t N>
    void MigrateAsset(const MigrateStep<Data> (&chain)[N], uint16_t fromVersion, Data& data)
    {
        static_assert(N == static_cast<std::size_t>(CurrentVersion(Magic) - MinimumVersion(Magic)),
                      "every version of the format needs a step to the next");
        RunMigrationChain(chain, MinimumVersion(Magic), CurrentVersion(Magic), fromVersion, data);
    }
}
