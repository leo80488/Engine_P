#pragma once
// SHA-256 as lowercase hex (Windows BCrypt), the digest Python's hashlib.sha256 gives for
// the same bytes. Records or checks a file's identity: the -dump_catalog manifest and the
// asset tool's expected-content-hash guard.

#include <cstddef>
#include <cstdint>
#include <string>

namespace Sys
{
    // Empty on failure.
    std::string Sha256Hex(const void* data, std::size_t size);
    // @p outBytes (optional) receives the file size. Empty when unreadable.
    std::string Sha256FileHex(const std::string& utf8Path, std::uint64_t* outBytes = nullptr);
}
