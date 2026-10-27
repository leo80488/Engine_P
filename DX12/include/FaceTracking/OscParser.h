#pragma once
// OSC 1.0 packets: what VMC rides on. A packet is a message or a "#bundle" of elements, each a
// message or a bundle; everything is big-endian and every string and blob is padded to 4 bytes.
// Packets come off the network, so this is a strict reader of untrusted bytes: every length is
// checked against the end, nesting and argument counts are capped, and a malformed packet delivers
// nothing at all.
// Header-only, no engine dependencies: tests/unit/face_parsers_test.cpp fuzzes it.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace FaceTracking::Osc {

inline constexpr int kMaxArgs = 16;
inline constexpr int kMaxBundleDepth = 8;
inline constexpr size_t kMaxMessages = 4096;

struct Arg
{
    char type = 0;          // the type tag: i f s b h d t T F N I (others refuse the message)
    int32_t i = 0;          // 'i'
    float f = 0.0f;         // 'f'
    int64_t h = 0;          // 'h', 't' (the time tag's raw 64 bits)
    double d = 0.0;         // 'd'
    std::string_view s;     // 's' (into the packet), 'b' (the blob's bytes)
};

struct Message
{
    std::string_view address; // into the packet
    int argc = 0;
    Arg args[kMaxArgs];

    // Typed reads: false when the argument is missing or of another type.
    bool Str(int k, std::string_view& out) const
    {
        if (k >= argc || args[k].type != 's') return false;
        out = args[k].s;
        return true;
    }
    bool Float(int k, float& out) const
    {
        if (k >= argc) return false;
        switch (args[k].type)
        {
        case 'f': out = args[k].f; return true;
        case 'd': out = static_cast<float>(args[k].d); return true;
        case 'i': out = static_cast<float>(args[k].i); return true;
        default: return false;
        }
    }
    bool Int(int k, int32_t& out) const
    {
        if (k >= argc) return false;
        switch (args[k].type)
        {
        case 'i': out = args[k].i; return true;
        case 'T': out = 1; return true;
        case 'F': out = 0; return true;
        default: return false;
        }
    }
};

namespace detail {

inline uint32_t Be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }
inline size_t Pad4(size_t n) { return (n + 3) & ~size_t(3); }

// An OSC-string at [pos, end): NUL-terminated, padded with NULs to 4 bytes.
inline bool ReadString(const uint8_t* data, size_t end, size_t& pos, std::string_view& out)
{
    if (pos >= end) return false;
    const void* nul = std::memchr(data + pos, 0, end - pos);
    if (!nul) return false;
    const size_t len = static_cast<const uint8_t*>(nul) - (data + pos);
    const size_t next = pos + Pad4(len + 1);
    if (next > end) return false;
    for (size_t k = pos + len; k < next; ++k)
        if (data[k] != 0) return false; // the padding must be NULs
    out = std::string_view(reinterpret_cast<const char*>(data + pos), len);
    pos = next;
    return true;
}

inline bool ParseMessage(const uint8_t* data, size_t begin, size_t end, Message& m, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    size_t pos = begin;
    if (!ReadString(data, end, pos, m.address) || m.address.empty() || m.address[0] != '/')
        return fail("a message without an address");
    std::string_view types;
    if (pos == end) // OSC 1.0 allows a message with no type tag string: no arguments
    {
        m.argc = 0;
        return true;
    }
    if (!ReadString(data, end, pos, types) || types.empty() || types[0] != ',') return fail("a malformed type tag string");
    types.remove_prefix(1);
    if (types.size() > static_cast<size_t>(kMaxArgs)) return fail("more arguments than a face message has");
    m.argc = 0;
    for (const char t : types)
    {
        Arg& a = m.args[m.argc++];
        a.type = t;
        switch (t)
        {
        case 'i':
            if (end - pos < 4) return fail("a truncated int");
            a.i = static_cast<int32_t>(Be32(data + pos));
            pos += 4;
            break;
        case 'f':
        {
            if (end - pos < 4) return fail("a truncated float");
            const uint32_t bits = Be32(data + pos);
            std::memcpy(&a.f, &bits, 4);
            pos += 4;
            break;
        }
        case 'h':
        case 't':
            if (end - pos < 8) return fail("a truncated 64-bit argument");
            a.h = static_cast<int64_t>(Be64(data + pos));
            pos += 8;
            break;
        case 'd':
        {
            if (end - pos < 8) return fail("a truncated double");
            const uint64_t bits = Be64(data + pos);
            std::memcpy(&a.d, &bits, 8);
            pos += 8;
            break;
        }
        case 's':
            if (!ReadString(data, end, pos, a.s)) return fail("a malformed string argument");
            break;
        case 'b':
        {
            if (end - pos < 4) return fail("a truncated blob size");
            const uint32_t n = Be32(data + pos);
            pos += 4;
            if (n > end - pos || Pad4(n) > end - pos) return fail("a blob past the end");
            a.s = std::string_view(reinterpret_cast<const char*>(data + pos), n);
            pos += Pad4(n);
            break;
        }
        case 'T':
        case 'F':
        case 'N':
        case 'I':
            break;
        default:
            return fail("an argument type face tracking does not read");
        }
    }
    if (pos != end) return fail("bytes after the last argument");
    return true;
}

inline bool ParseElement(const uint8_t* data, size_t begin, size_t end, int depth, std::vector<Message>& out, std::string* error)
{
    if (end - begin >= 8 && std::memcmp(data + begin, "#bundle\0", 8) == 0)
    {
        if (depth >= kMaxBundleDepth)
        {
            if (error) *error = "bundles nested too deep";
            return false;
        }
        size_t pos = begin + 8;
        if (end - pos < 8)
        {
            if (error) *error = "a bundle without a time tag";
            return false;
        }
        pos += 8; // the time tag: arrival time is what face tracking uses
        while (pos < end)
        {
            if (end - pos < 4)
            {
                if (error) *error = "a truncated bundle element size";
                return false;
            }
            const uint32_t n = Be32(data + pos);
            pos += 4;
            if (n == 0 || n % 4 != 0 || n > end - pos)
            {
                if (error) *error = "a bundle element of a bad size";
                return false;
            }
            if (!ParseElement(data, pos, pos + n, depth + 1, out, error)) return false;
            pos += n;
        }
        return true;
    }
    if (out.size() >= kMaxMessages)
    {
        if (error) *error = "too many messages in one packet";
        return false;
    }
    Message m;
    if (!ParseMessage(data, begin, end, m, error)) return false;
    out.push_back(m);
    return true;
}

} // namespace detail

// Every message of the packet, bundles flattened in order. On a malformed packet `out` is
// left empty and false returned (`error` says why) - nothing of it is half-applied.
inline bool ParsePacket(const uint8_t* data, size_t size, std::vector<Message>& out, std::string* error = nullptr)
{
    out.clear();
    if (!data || size == 0 || size % 4 != 0)
    {
        if (error) *error = "an OSC packet is a non-empty multiple of 4 bytes";
        return false;
    }
    if (!detail::ParseElement(data, 0, size, 0, out, error))
    {
        out.clear();
        return false;
    }
    return true;
}

} // namespace FaceTracking::Osc
