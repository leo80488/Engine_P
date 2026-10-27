#pragma once

// PostProcessProfileSerializer - save / load a PostProcessProfile to / from the
// .ppprofile asset (the shared post-process look volumes and the engine default
// reference).
//
// Blob layout (like the engine's other text assets):
//   [AssetHeader (24 B)] [PostProcessProfileMetadata (16 B)] [text payload (null-terminated)]
// Text payload: one "group_member=value" line per OVERRIDDEN property. A key's
// presence encodes overrideState = true; an absent key leaves its property as it
// was (on a fresh profile: the default, not overriding). The key set comes from
// the X-macro in PostProcessProperties.inl, so a new property serializes
// automatically.

#include <string>

namespace PostProcess
{
struct PostProcessProfile;

// Write @p profile to a .ppprofile file at @p path. Returns true on success.
bool SaveProfile(const PostProcessProfile& profile, const std::string& path);

// Read the .ppprofile at @p path into @p profile (through AssetFS, so packed
// builds read from the .ipak). Keys the file lacks keep the profile's current
// values. Returns false only on an I/O or header-validation failure.
bool LoadProfile(const std::string& path, PostProcessProfile& profile);

} // namespace PostProcess
