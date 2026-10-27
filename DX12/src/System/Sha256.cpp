#include "System/Sha256.h"

#include "System/PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <cstdio>
#include <fstream>
#include <vector>

namespace Sys
{
namespace
{
    struct Hasher
    {
        BCRYPT_ALG_HANDLE  alg  = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = false;

        Hasher()
        {
            ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                 BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
        }
        ~Hasher()
        {
            if (hash) BCryptDestroyHash(hash);
            if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
        }
        void Add(const void* p, std::size_t n)
        {
            if (ok && n) ok = BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(p)),
                                                            static_cast<ULONG>(n), 0));
        }
        std::string Hex()
        {
            unsigned char digest[32] = {};
            if (!ok || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) return {};
            std::string hex;
            char h[3];
            for (unsigned char c : digest) { std::snprintf(h, sizeof(h), "%02x", c); hex += h; }
            return hex;
        }
    };
}

std::string Sha256Hex(const void* data, std::size_t size)
{
    Hasher h;
    // BCryptHashData takes a ULONG: feed very large buffers in slices.
    const auto* p = static_cast<const unsigned char*>(data);
    while (size > 0)
    {
        const std::size_t n = size > (1u << 30) ? (1u << 30) : size;
        h.Add(p, n);
        p += n;
        size -= n;
    }
    return h.Hex();
}

std::string Sha256FileHex(const std::string& utf8Path, std::uint64_t* outBytes)
{
    if (outBytes) *outBytes = 0;
    std::ifstream f(U8Path(utf8Path), std::ios::binary);
    if (!f) return {};
    Hasher h;
    std::vector<char> buf(1 << 20);
    std::uint64_t total = 0;
    while (f)
    {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        h.Add(buf.data(), static_cast<std::size_t>(n));
        total += static_cast<std::uint64_t>(n);
    }
    if (outBytes) *outBytes = total;
    return h.Hex();
}
}
