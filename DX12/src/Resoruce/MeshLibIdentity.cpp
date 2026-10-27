#ifndef NOMINMAX
#define NOMINMAX
#endif

// MeshLibIdentity.cpp - see Resource/MeshLibIdentity.h (the contract and the layout).
//
// Every read of the blob goes through memcpy: a .meshlib is read into a plain byte vector,
// so nothing past the 24-byte header is guaranteed to be aligned for the 64-bit key fields.

#include "Resource/MeshLibIdentity.h"
#include "Resource/MeshLibTail.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace Resource
{
namespace
{
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime  = 1099511628211ull;
    // The source-key prefix of every index-derived key (see LegacySectionKey).
    constexpr const char* kLegacyPrefix = "legacy#";

    // The fixed part of a .meshlib: the metadata, where the entry table starts
    // and where the index buffer ends. Computed from the COUNTS exactly the way
    // MeshLibrary::Load and every other reader does, which is what makes the
    // tail invisible to them.
    struct Core
    {
        MeshLibraryMetadata meta{};
        size_t   tableOffset = 0;
        size_t   coreEnd     = 0;
        uint32_t entryCount  = 0;
    };

    // A chunk's bytes, copied: only the WRITERS hold these (they lay every
    // chunk out again). Readers go through the directory and read in place.
    struct Chunk
    {
        uint32_t             fourcc  = 0;
        uint32_t             version = 0;
        std::vector<uint8_t> bytes;
    };

    using TailRead = MeshLibTailState;

    template <typename T>
    T ReadPod(const uint8_t* p)
    {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }

    template <typename T>
    void AppendPod(std::vector<uint8_t>& out, const T& v)
    {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }

    // @p head holds the first @p headSize bytes of a file of @p size bytes (the
    // whole blob, or just its header and metadata).
    bool ReadCoreHead(const uint8_t* head, size_t headSize, size_t size, Core& c, std::string* err)
    {
        auto fail = [&](const char* why) { if (err) *err = why; return false; };
        if (!head || headSize < sizeof(AssetHeader) || size < headSize) return fail("smaller than an AssetHeader");
        const AssetHeader h = ReadPod<AssetHeader>(head);
        if (h.magic != MAGIC_MESHLIB) return fail("not a .meshlib (magic)");
        if (h.metadataSize < sizeof(MeshLibraryMetadata)) return fail("metadata smaller than MeshLibraryMetadata");
        if (sizeof(AssetHeader) + size_t(h.metadataSize) > size) return fail("truncated metadata");
        if (sizeof(AssetHeader) + sizeof(MeshLibraryMetadata) > headSize) return fail("truncated metadata");
        c.meta        = ReadPod<MeshLibraryMetadata>(head + sizeof(AssetHeader));
        c.tableOffset = sizeof(AssetHeader) + h.metadataSize;
        c.entryCount  = c.meta.meshCount;
        const size_t entryBytes  = size_t(c.meta.meshCount)   * sizeof(MeshLibraryEntry);
        const size_t vertexBytes = size_t(c.meta.vertexCount) * c.meta.vertexStride;
        const size_t indexBytes  = size_t(c.meta.indexCount)  * c.meta.indexStride;
        c.coreEnd = c.tableOffset + entryBytes + vertexBytes + indexBytes;
        if (c.coreEnd > size) return fail("truncated entry table / vertex / index data");
        return true;
    }

    bool ReadCore(const uint8_t* blob, size_t size, Core& c, std::string* err)
    {
        return ReadCoreHead(blob, size, size, c, err);
    }

    MeshLibraryEntry EntryAt(const uint8_t* blob, const Core& c, uint32_t i)
    {
        return ReadPod<MeshLibraryEntry>(blob + c.tableOffset + size_t(i) * sizeof(MeshLibraryEntry));
    }

    // The chunk directory of a tail, parsed in place - no chunk byte is read.
    // @p buf holds the file's bytes from offset @p base up to offset @p bufEnd
    // (the whole blob: base 0, bufEnd = size; a stream reader: just the tail
    // header and the directory). @p size is the FILE's size: every chunk must
    // lie inside it.
    TailRead ReadTailDirAt(const uint8_t* buf, size_t base, size_t bufEnd, size_t size, const Core& c,
                           MeshLibTailDirectory& dir, std::string* err)
    {
        dir = MeshLibTailDirectory{};
        if (!(c.meta.flags & MESHLIB_FLAG_HAS_TAIL)) return TailRead::None;

        auto bad = [&](const std::string& why) { if (err) *err = why; return TailRead::Malformed; };
        // File offset -> the bytes read; every offset used below is in [base, bufEnd).
        auto at = [&](size_t fileOffset) { return buf + (fileOffset - base); };
        const size_t off = c.meta.reserved;
        if (off < c.coreEnd) return bad("tail offset points inside the geometry");
        if (off < base) return bad("tail offset before the bytes read");
        if (off + sizeof(MeshLibTailHeader) > size) return bad("tail header past the end of the file");
        if (off + sizeof(MeshLibTailHeader) > bufEnd) return bad("tail header past the bytes read");
        const MeshLibTailHeader th = ReadPod<MeshLibTailHeader>(at(off));
        if (th.magic != MESHLIB_TAIL_MAGIC) return bad("tail magic mismatch");
        if (th.version != MESHLIB_TAIL_VERSION)
            return bad("unknown tail version " + std::to_string(th.version));
        const size_t dirOff = off + sizeof(MeshLibTailHeader);
        const size_t dirEnd = dirOff + size_t(th.chunkCount) * sizeof(MeshLibChunkDesc);
        if (dirEnd > size) return bad("chunk directory past the end of the file");
        if (dirEnd > bufEnd) return bad("chunk directory past the bytes read");
        dir.chunks.reserve(th.chunkCount);
        for (uint32_t k = 0; k < th.chunkCount; ++k)
        {
            const MeshLibChunkDesc d =
                ReadPod<MeshLibChunkDesc>(at(dirOff + size_t(k) * sizeof(MeshLibChunkDesc)));
            if (size_t(d.offset) < dirEnd || size_t(d.offset) + size_t(d.size) > size)
                return bad("chunk " + std::to_string(k) + " lies outside the file");
            dir.chunks.push_back(MeshLibChunkRef{ d.fourcc, d.version, d.offset, d.size });
        }
        dir.tailOffset = off;
        return TailRead::Ok;
    }

    TailRead ReadTailDir(const uint8_t* blob, size_t size, const Core& c, MeshLibTailDirectory& dir,
                         std::string* err)
    {
        return ReadTailDirAt(blob, 0, size, size, c, dir, err);
    }

    // The writers' read: the directory, then every chunk's bytes copied.
    TailRead ReadTail(const uint8_t* blob, size_t size, const Core& c,
                      size_t& tailOffset, std::vector<Chunk>& chunks, std::string* err)
    {
        chunks.clear();
        tailOffset = 0;
        MeshLibTailDirectory dir;
        const TailRead tr = ReadTailDir(blob, size, c, dir, err);
        if (tr != TailRead::Ok) return tr;
        chunks.reserve(dir.chunks.size());
        for (const MeshLibChunkRef& r : dir.chunks)
        {
            Chunk ch;
            ch.fourcc  = r.fourcc;
            ch.version = r.version;
            ch.bytes.assign(blob + r.offset, blob + r.offset + r.size);
            chunks.push_back(std::move(ch));
        }
        tailOffset = dir.tailOffset;
        return TailRead::Ok;
    }

    // One chunk of a file open in @p f (@p size bytes, its core in @p c): the
    // tail header, the directory and that chunk - nothing else is read.
    MeshLibChunkRead ReadChunkFromStream(std::istream& f, size_t size, const Core& c, uint32_t fourcc,
                                         std::vector<uint8_t>& bytes, uint32_t& version, std::string* err)
    {
        bytes.clear();
        version = 0;
        if (!(c.meta.flags & MESHLIB_FLAG_HAS_TAIL)) return MeshLibChunkRead::NoChunk;
        auto bad = [&](const char* why) { if (err) *err = why; return MeshLibChunkRead::Malformed; };
        const size_t off = c.meta.reserved;
        if (off < c.coreEnd || off + sizeof(MeshLibTailHeader) > size) return bad("tail offset outside the file");
        MeshLibTailHeader th{};
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        if (!f.read(reinterpret_cast<char*>(&th), sizeof(th))) return bad("tail header unreadable");
        const size_t dirBytes = sizeof(MeshLibTailHeader) + size_t(th.chunkCount) * sizeof(MeshLibChunkDesc);
        if (off + dirBytes > size) return bad("chunk directory past the end of the file");
        std::vector<uint8_t> dirBuf(dirBytes);
        f.seekg(static_cast<std::streamoff>(off));
        if (!f.read(reinterpret_cast<char*>(dirBuf.data()), static_cast<std::streamsize>(dirBytes)))
            return bad("chunk directory unreadable");
        MeshLibTailDirectory dir;
        if (ReadTailDirAt(dirBuf.data(), off, off + dirBytes, size, c, dir, err) != TailRead::Ok)
            return MeshLibChunkRead::Malformed;
        const MeshLibChunkRef* r = dir.Find(fourcc);
        if (!r) return MeshLibChunkRead::NoChunk;
        bytes.resize(r->size);
        f.seekg(static_cast<std::streamoff>(r->offset));
        if (r->size && !f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(r->size)))
        {
            bytes.clear();
            return bad("chunk unreadable");
        }
        version = r->version;
        return MeshLibChunkRead::Ok;
    }

    // The IDNT chunk's bytes -> an identity for a library of @p entryCount entries.
    MeshIdentityParse ParseIdentityChunk(const uint8_t* b, size_t n, uint32_t entryCount, MeshIdentity& out,
                                         std::string* err);

    // Truncate at cutAt, append [header][directory][chunks, 8-aligned], then
    // point the metadata at the tail and make the header's dataSize cover it.
    // All-or-nothing: the one failure is decided before the blob is touched.
    bool WriteTail(std::vector<uint8_t>& blob, size_t cutAt, const std::vector<Chunk>& chunks,
                   std::string* err)
    {
        auto align8 = [](size_t v) { return (v + 7) & ~size_t(7); };
        size_t finalSize = align8(cutAt) + sizeof(MeshLibTailHeader) + chunks.size() * sizeof(MeshLibChunkDesc);
        for (const Chunk& ch : chunks) finalSize = align8(finalSize) + ch.bytes.size();
        if (finalSize > std::numeric_limits<uint32_t>::max() || chunks.size() > 0xFFFFu)
        {
            if (err) *err = "the identity tail would push the file past 4 GiB";
            return false;
        }

        blob.resize(cutAt);
        while (blob.size() % 8) blob.push_back(0);
        const size_t tailOffset = blob.size();

        MeshLibTailHeader th{};
        th.magic      = MESHLIB_TAIL_MAGIC;
        th.version    = MESHLIB_TAIL_VERSION;
        th.chunkCount = static_cast<uint16_t>(chunks.size());
        AppendPod(blob, th);
        const size_t dirOff = blob.size();
        blob.resize(dirOff + chunks.size() * sizeof(MeshLibChunkDesc), 0);

        std::vector<MeshLibChunkDesc> descs(chunks.size());
        for (size_t k = 0; k < chunks.size(); ++k)
        {
            while (blob.size() % 8) blob.push_back(0);
            descs[k].fourcc  = chunks[k].fourcc;
            descs[k].version = chunks[k].version;
            descs[k].offset  = static_cast<uint32_t>(blob.size());
            descs[k].size    = static_cast<uint32_t>(chunks[k].bytes.size());
            blob.insert(blob.end(), chunks[k].bytes.begin(), chunks[k].bytes.end());
        }
        if (!descs.empty())
            std::memcpy(blob.data() + dirOff, descs.data(), descs.size() * sizeof(MeshLibChunkDesc));

        AssetHeader h = ReadPod<AssetHeader>(blob.data());
        MeshLibraryMetadata m = ReadPod<MeshLibraryMetadata>(blob.data() + sizeof(AssetHeader));
        m.flags   |= MESHLIB_FLAG_HAS_TAIL;
        m.reserved = static_cast<uint32_t>(tailOffset);
        std::memcpy(blob.data() + sizeof(AssetHeader), &m, sizeof(m));
        h.dataSize = static_cast<uint32_t>(blob.size() - sizeof(AssetHeader) - h.metadataSize);
        std::memcpy(blob.data(), &h, sizeof(h));
        return true;
    }

    std::vector<uint8_t> SerializeIdentity(const MeshIdentity& id)
    {
        std::string pool(1, '\0');                 // offset 0 == ""
        std::unordered_map<std::string, uint32_t> interned;
        auto intern = [&](const std::string& s) -> uint32_t
        {
            // A NUL inside a std::string would end the pooled copy early; cut there.
            std::string clean = s.substr(0, s.find('\0'));
            if (clean.empty()) return 0;
            auto it = interned.find(clean);
            if (it != interned.end()) return it->second;
            const uint32_t off = static_cast<uint32_t>(pool.size());
            pool += clean;
            pool.push_back('\0');
            interned.emplace(std::move(clean), off);
            return off;
        };

        std::vector<MeshLibMeshRecord> meshRecs(id.meshes.size());
        std::vector<uint32_t>          list;
        for (size_t i = 0; i < id.meshes.size(); ++i)
        {
            const MeshIdentity::Mesh& m = id.meshes[i];
            MeshLibMeshRecord& r = meshRecs[i];
            std::memset(&r, 0, sizeof(r));
            r.key             = m.key;
            r.nameOffset      = intern(m.name);
            r.sourceKeyOffset = intern(m.sourceKey);
            r.firstInList     = static_cast<uint32_t>(list.size());
            r.sectionCount    = static_cast<uint32_t>(m.sections.size());
            for (int a = 0; a < 3; ++a) { r.aabbMin[a] = m.aabbMin[a]; r.aabbMax[a] = m.aabbMax[a]; }
            r.flags = m.flags;
            list.insert(list.end(), m.sections.begin(), m.sections.end());
        }
        std::vector<MeshLibSectionRecord> secRecs(id.sections.size());
        for (size_t e = 0; e < id.sections.size(); ++e)
        {
            const MeshIdentity::Section& s = id.sections[e];
            MeshLibSectionRecord& r = secRecs[e];
            std::memset(&r, 0, sizeof(r));
            r.key             = s.key;
            r.slotNameOffset  = intern(s.slotName);
            r.sourceKeyOffset = intern(s.sourceKey);
            r.ownerMesh       = s.ownerMesh;
            r.flags           = s.flags;
        }

        MeshLibIdentityHeader hdr{};
        hdr.modelKey         = id.modelKey;
        hdr.meshCount        = static_cast<uint32_t>(meshRecs.size());
        hdr.sectionCount     = static_cast<uint32_t>(secRecs.size());
        hdr.sectionListCount = static_cast<uint32_t>(list.size());
        hdr.stringPoolBytes  = static_cast<uint32_t>(pool.size());
        hdr.flags            = id.flags;

        std::vector<uint8_t> out;
        out.reserve(sizeof(hdr) + meshRecs.size() * sizeof(MeshLibMeshRecord) +
                    secRecs.size() * sizeof(MeshLibSectionRecord) + list.size() * 4 + pool.size());
        AppendPod(out, hdr);
        for (const auto& r : meshRecs) AppendPod(out, r);
        for (const auto& r : secRecs)  AppendPod(out, r);
        for (uint32_t v : list)        AppendPod(out, v);
        out.insert(out.end(), pool.begin(), pool.end());
        return out;
    }

    // Case-folded, separator-folded form of a name: what a sanitiser would have made of it
    // (spaces and path separators to '_', other ASCII punctuation dropped). Lets a re-import
    // match "Body Top" to a previously sanitised "Body_Top".
    std::string LooseName(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s)
        {
            if (c >= 'A' && c <= 'Z')                         out.push_back(static_cast<char>(c - 'A' + 'a'));
            else if (c == ' ' || c == '/' || c == '\\' ||
                     c == ':' || c == '-' || c == '.' || c == '_') out.push_back('_');
            else if (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))
                                                              out.push_back(static_cast<char>(c));
        }
        return out;
    }

    uint64_t DeriveUniqueKey(const char* domain, std::string& sourceKey,
                             std::unordered_set<uint64_t>& used, const char* what)
    {
        const std::string base = sourceKey;
        uint64_t k = MeshIdentityHash(std::string(domain) + "|" + sourceKey);
        for (uint32_t n = 1; used.count(k); ++n)
        {
            sourceKey = base + "#dup" + std::to_string(n);
            k = MeshIdentityHash(std::string(domain) + "|" + sourceKey);
        }
        if (sourceKey != base)
            LOG_WARNING("MeshLibIdentity: %s source key '%s' is not unique in this library; "
                        "keyed as '%s'", what, base.c_str(), sourceKey.c_str());
        used.insert(k);
        return k;
    }

    struct ProtoMesh
    {
        std::string           name, sourceKey;
        std::vector<uint32_t> sections;
        uint32_t              flags = 0;
    };
    struct ProtoSection
    {
        std::string slotName, sourceKey;
        uint32_t    flags = 0;
    };

    // Keys, AABBs and owners from protos. sections.size() == c.entryCount.
    // Entries no mesh lists get a one-section mesh keyed "entry|<section source>".
    void Assemble(uint64_t modelKey, uint32_t idFlags,
                  std::vector<ProtoMesh> meshes, const std::vector<ProtoSection>& sections,
                  const uint8_t* blob, const Core& c, MeshIdentity& out)
    {
        out = MeshIdentity{};
        out.modelKey = modelKey;
        out.flags    = idFlags;

        std::unordered_set<uint64_t> usedS, usedM;
        out.sections.resize(c.entryCount);
        for (uint32_t e = 0; e < c.entryCount; ++e)
        {
            MeshIdentity::Section& s = out.sections[e];
            s.slotName  = sections[e].slotName;
            s.sourceKey = sections[e].sourceKey;
            s.flags     = sections[e].flags;
            s.key       = DeriveUniqueKey("sec", s.sourceKey, usedS, "section");
        }

        // In-range, no repeats inside one mesh, no empty meshes.
        for (ProtoMesh& pm : meshes)
        {
            std::vector<uint32_t>        clean;
            std::unordered_set<uint32_t> seen;
            clean.reserve(pm.sections.size());
            for (uint32_t e : pm.sections)
                if (e < c.entryCount && seen.insert(e).second) clean.push_back(e);
            pm.sections.swap(clean);
        }
        meshes.erase(std::remove_if(meshes.begin(), meshes.end(),
                                    [](const ProtoMesh& m) { return m.sections.empty(); }),
                     meshes.end());

        std::vector<char> covered(c.entryCount, 0);
        for (const ProtoMesh& pm : meshes)
            for (uint32_t e : pm.sections) covered[e] = 1;
        for (uint32_t e = 0; e < c.entryCount; ++e)
        {
            if (covered[e]) continue;
            ProtoMesh pm;
            pm.name      = out.sections[e].slotName.empty() ? "section_" + std::to_string(e)
                                                            : out.sections[e].slotName;
            pm.sourceKey = "entry|" + out.sections[e].sourceKey;
            pm.sections  = { e };
            meshes.push_back(std::move(pm));
        }

        out.meshes.resize(meshes.size());
        for (size_t i = 0; i < meshes.size(); ++i)
        {
            MeshIdentity::Mesh& m = out.meshes[i];
            m.name      = meshes[i].name;
            m.sourceKey = meshes[i].sourceKey;
            m.sections  = meshes[i].sections;
            m.flags     = meshes[i].flags;
            m.key       = DeriveUniqueKey("mesh", m.sourceKey, usedM, "mesh");

            bool first = true;
            for (uint32_t e : m.sections)
            {
                const MeshLibraryEntry en = EntryAt(blob, c, e);
                for (int a = 0; a < 3; ++a)
                {
                    m.aabbMin[a] = first ? en.aabbMin[a] : std::min(m.aabbMin[a], en.aabbMin[a]);
                    m.aabbMax[a] = first ? en.aabbMax[a] : std::max(m.aabbMax[a], en.aabbMax[a]);
                }
                first = false;
                if (out.sections[e].ownerMesh == UINT32_MAX)
                    out.sections[e].ownerMesh = static_cast<uint32_t>(i);
            }
        }
        out.RebuildIndex();
    }
} // namespace

// ==== Keys ====
uint64_t MeshIdentityHash(std::string_view text)
{
    uint64_t h = kFnvOffset;
    for (unsigned char c : text)
    {
        h ^= c;
        h *= kFnvPrime;
    }
    return h ? h : 1ull;
}

std::string MeshLibCanonicalPath(const std::string& path)
{
    if (path.empty()) return path;
    return Sys::RelativizeAssetPath(Sys::PathToUtf8(Sys::U8Path(path).lexically_normal()));
}

std::string MeshKeyToHex(uint64_t key)
{
    static const char* kDigits = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        s[static_cast<size_t>(i)] = kDigits[key & 0xF];
        key >>= 4;
    }
    return s;
}

bool MeshKeyFromHex(std::string_view hex, uint64_t& out)
{
    if (hex.size() != 16) return false;
    uint64_t v = 0;
    for (char ch : hex)
    {
        v <<= 4;
        if      (ch >= '0' && ch <= '9') v |= uint64_t(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= uint64_t(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= uint64_t(ch - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

std::string MeshAssetRef(const std::string& modelPath, uint64_t meshKey)
{
    return MeshLibCanonicalPath(modelPath) + '#' + MeshKeyToHex(meshKey);
}

bool ParseMeshAssetRef(std::string_view ref, std::string& outModel, uint64_t& outKey)
{
    const size_t hash = ref.rfind('#');
    if (hash == std::string_view::npos || hash < 5) return false;
    std::string_view model = ref.substr(0, hash);
    static constexpr std::string_view kExt = ".iscn";
    for (size_t i = 0; i < kExt.size(); ++i)
    {
        char c = model[model.size() - kExt.size() + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kExt[i]) return false;
    }
    uint64_t key = 0;
    if (!MeshKeyFromHex(ref.substr(hash + 1), key) || key == 0) return false;
    outModel.assign(model.data(), model.size());
    outKey = key;
    return true;
}

// ==== MeshIdentity lookups ====
void MeshIdentity::RebuildIndex()
{
    m_meshByKey.clear();
    m_sectionByKey.clear();
    m_meshByKey.reserve(meshes.size());
    m_sectionByKey.reserve(sections.size());
    for (uint32_t i = 0; i < meshes.size(); ++i)   m_meshByKey.emplace(meshes[i].key, i);
    for (uint32_t i = 0; i < sections.size(); ++i) m_sectionByKey.emplace(sections[i].key, i);
}

int MeshIdentity::FindMesh(uint64_t key) const
{
    auto it = m_meshByKey.find(key);
    return it == m_meshByKey.end() ? -1 : static_cast<int>(it->second);
}

int MeshIdentity::FindSection(uint64_t key) const
{
    auto it = m_sectionByKey.find(key);
    return it == m_sectionByKey.end() ? -1 : static_cast<int>(it->second);
}

int MeshIdentity::FindMeshByName(std::string_view name) const
{
    for (size_t i = 0; i < meshes.size(); ++i)
        if (meshes[i].name == name) return static_cast<int>(i);
    return -1;
}

// ==== Building ====
bool BuildMeshIdentity(const MeshIdentitySpec& spec, const uint8_t* blob, size_t size,
                       MeshIdentity& out, std::string* err)
{
    Core c;
    if (!ReadCore(blob, size, c, err)) return false;
    if (spec.sections.size() != c.entryCount)
    {
        if (err) *err = "spec has " + std::to_string(spec.sections.size()) + " sections for " +
                        std::to_string(c.entryCount) + " library entries";
        return false;
    }
    std::vector<ProtoMesh> meshes;
    meshes.reserve(spec.meshes.size());
    for (const auto& m : spec.meshes)
        meshes.push_back(ProtoMesh{ m.name, m.sourceKey, m.sections, m.flags });
    std::vector<ProtoSection> sections;
    sections.reserve(spec.sections.size());
    for (const auto& s : spec.sections)
        sections.push_back(ProtoSection{ s.slotName, s.sourceKey, s.flags });

    Assemble(MeshIdentityHash("model|" + spec.modelSource), 0, std::move(meshes), sections,
             blob, c, out);
    return ValidateMeshIdentity(out, c.entryCount, err);
}

uint64_t LegacySectionKey(uint32_t entry)
{
    return MeshIdentityHash(std::string("sec|") + kLegacyPrefix + std::to_string(entry));
}

uint64_t SectionKeyOfEntry(const uint8_t* blob, size_t size, uint32_t entry)
{
    Core c;
    if (!ReadCore(blob, size, c, nullptr) || entry >= c.entryCount) return 0;
    MeshIdentity id;
    if (ParseMeshLibIdentity(blob, size, id, nullptr) == MeshIdentityParse::Ok)
        return id.sections[entry].key;
    return LegacySectionKey(entry);
}

MeshIdentity SynthesizeMeshIdentity(const std::string& canonicalPath,
                                    const uint8_t* blob, size_t size,
                                    const std::vector<MeshGroupHint>& groups,
                                    const std::vector<std::string>& slotNames)
{
    MeshIdentity out;
    out.flags = IDNT_FLAG_SYNTHESIZED;
    Core c;
    if (!ReadCore(blob, size, c, nullptr)) return out;

    // NOT path-qualified: a key is unique within its model and every reference is (model, key),
    // so a path would only make moving a tail-less model's folder re-key it. LegacySectionKey
    // spells the section half of this; keep the two together.
    const std::string prefix = kLegacyPrefix;
    std::vector<ProtoSection> sections(c.entryCount);
    for (uint32_t e = 0; e < c.entryCount; ++e)
    {
        sections[e].slotName  = (e < slotNames.size() && !slotNames[e].empty())
                              ? slotNames[e] : "section_" + std::to_string(e);
        sections[e].sourceKey = prefix + std::to_string(e);
    }

    // Groups: keyed by their FIRST entry, so a one-section group and an ungrouped
    // entry get the same key - a later grouping hint that merely wraps an entry
    // does not re-key it. A group repeating an earlier group's list (the same
    // geometry placed by two nodes) is the same Mesh and is dropped.
    std::vector<ProtoMesh>      meshes;
    std::unordered_set<std::string> seenLists;
    std::vector<char>           covered(c.entryCount, 0);
    for (const MeshGroupHint& g : groups)
    {
        ProtoMesh pm;
        std::unordered_set<uint32_t> seen;
        for (uint32_t e : g.sections)
            if (e < c.entryCount && seen.insert(e).second) pm.sections.push_back(e);
        if (pm.sections.empty()) continue;
        std::string sig;
        for (uint32_t e : pm.sections) sig += std::to_string(e) + ",";
        if (!seenLists.insert(sig).second) continue;
        pm.name      = g.name.empty() ? sections[pm.sections[0]].slotName : g.name;
        pm.sourceKey = prefix + std::to_string(pm.sections[0]);
        for (uint32_t e : pm.sections) covered[e] = 1;
        meshes.push_back(std::move(pm));
    }
    for (uint32_t e = 0; e < c.entryCount; ++e)
    {
        if (covered[e]) continue;
        ProtoMesh pm;
        pm.name      = sections[e].slotName;
        pm.sourceKey = prefix + std::to_string(e);
        pm.sections  = { e };
        meshes.push_back(std::move(pm));
    }

    Assemble(MeshIdentityHash("model|legacy|" + canonicalPath), IDNT_FLAG_SYNTHESIZED,
             std::move(meshes), sections, blob, c, out);
    return out;
}

// ==== Reading / writing the file ====
MeshIdentityParse ParseMeshLibIdentity(const uint8_t* blob, size_t size,
                                       MeshIdentity& out, std::string* err)
{
    out = MeshIdentity{};
    Core c;
    if (!ReadCore(blob, size, c, err)) return MeshIdentityParse::Malformed;

    // The directory only: the identity is parsed in place, and no other chunk
    // (a multi-megabyte MLET) is copied to find it.
    MeshLibTailDirectory dir;
    switch (ReadTailDir(blob, size, c, dir, err))
    {
    case TailRead::None:      return MeshIdentityParse::NoTail;
    case TailRead::Malformed: return MeshIdentityParse::Malformed;
    case TailRead::Ok:        break;
    }

    const MeshLibChunkRef* idnt = dir.Find(MESHLIB_CHUNK_IDNT);
    if (!idnt) return MeshIdentityParse::NoIdentity;
    if (idnt->version != MESHLIB_IDNT_VERSION)
    {
        if (err) *err = "unknown IDNT version " + std::to_string(idnt->version);
        return MeshIdentityParse::Malformed;
    }
    return ParseIdentityChunk(blob + idnt->offset, idnt->size, c.entryCount, out, err);
}

namespace
{
MeshIdentityParse ParseIdentityChunk(const uint8_t* b, size_t n, uint32_t entryCount, MeshIdentity& out,
                                     std::string* err)
{
    auto bad = [&](const char* why) { if (err) *err = why; out = MeshIdentity{}; return MeshIdentityParse::Malformed; };
    size_t p = 0;
    if (!b || n < sizeof(MeshLibIdentityHeader)) return bad("IDNT smaller than its header");
    const MeshLibIdentityHeader hdr = ReadPod<MeshLibIdentityHeader>(b);
    p += sizeof(hdr);

    const size_t meshBytes = size_t(hdr.meshCount) * sizeof(MeshLibMeshRecord);
    const size_t secBytes  = size_t(hdr.sectionCount) * sizeof(MeshLibSectionRecord);
    const size_t listBytes = size_t(hdr.sectionListCount) * sizeof(uint32_t);
    if (p + meshBytes + secBytes + listBytes + hdr.stringPoolBytes > n)
        return bad("IDNT records overrun the chunk");
    if (hdr.stringPoolBytes == 0) return bad("IDNT string pool is empty (offset 0 must be \"\")");

    const size_t meshOff = p;
    const size_t secOff  = meshOff + meshBytes;
    const size_t listOff = secOff + secBytes;
    const size_t poolOff = listOff + listBytes;
    const char*  pool    = reinterpret_cast<const char*>(b + poolOff);
    const size_t poolLen = hdr.stringPoolBytes;

    bool strOk = true;
    auto str = [&](uint32_t off) -> std::string
    {
        if (off >= poolLen) { strOk = false; return {}; }
        const void* nul = std::memchr(pool + off, '\0', poolLen - off);
        if (!nul) { strOk = false; return {}; }
        return std::string(pool + off, static_cast<const char*>(nul) - (pool + off));
    };

    out.modelKey = hdr.modelKey;
    out.flags    = hdr.flags;
    out.meshes.resize(hdr.meshCount);
    for (uint32_t i = 0; i < hdr.meshCount; ++i)
    {
        const MeshLibMeshRecord r =
            ReadPod<MeshLibMeshRecord>(b + meshOff + size_t(i) * sizeof(MeshLibMeshRecord));
        MeshIdentity::Mesh& m = out.meshes[i];
        m.key       = r.key;
        m.name      = str(r.nameOffset);
        m.sourceKey = str(r.sourceKeyOffset);
        m.flags     = r.flags;
        for (int a = 0; a < 3; ++a) { m.aabbMin[a] = r.aabbMin[a]; m.aabbMax[a] = r.aabbMax[a]; }
        if (size_t(r.firstInList) + r.sectionCount > hdr.sectionListCount)
            return bad("a mesh's section run overruns the section list");
        m.sections.resize(r.sectionCount);
        for (uint32_t k = 0; k < r.sectionCount; ++k)
            m.sections[k] = ReadPod<uint32_t>(b + listOff + (size_t(r.firstInList) + k) * 4);
    }
    out.sections.resize(hdr.sectionCount);
    for (uint32_t e = 0; e < hdr.sectionCount; ++e)
    {
        const MeshLibSectionRecord r =
            ReadPod<MeshLibSectionRecord>(b + secOff + size_t(e) * sizeof(MeshLibSectionRecord));
        MeshIdentity::Section& s = out.sections[e];
        s.key       = r.key;
        s.slotName  = str(r.slotNameOffset);
        s.sourceKey = str(r.sourceKeyOffset);
        s.ownerMesh = r.ownerMesh;
        s.flags     = r.flags;
    }
    if (!strOk) return bad("a string offset is outside the pool or unterminated");

    std::string verr;
    if (!ValidateMeshIdentity(out, entryCount, &verr))
    {
        if (err) *err = verr;
        out = MeshIdentity{};
        return MeshIdentityParse::Malformed;
    }
    out.RebuildIndex();
    return MeshIdentityParse::Ok;
}
}   // namespace

size_t MeshLibCoreEnd(const uint8_t* blob, size_t size)
{
    Core c;
    return ReadCore(blob, size, c, nullptr) ? c.coreEnd : 0;
}

bool StripMeshLibTail(std::vector<uint8_t>& blob, std::string* err)
{
    Core c;
    if (!ReadCore(blob.data(), blob.size(), c, err)) return false;
    blob.resize(c.coreEnd);
    AssetHeader h = ReadPod<AssetHeader>(blob.data());
    MeshLibraryMetadata m = c.meta;
    m.flags   &= ~MESHLIB_FLAG_HAS_TAIL;
    m.reserved = 0;
    std::memcpy(blob.data() + sizeof(AssetHeader), &m, sizeof(m));
    h.dataSize = static_cast<uint32_t>(c.coreEnd - sizeof(AssetHeader) - h.metadataSize);
    std::memcpy(blob.data(), &h, sizeof(h));
    return true;
}

bool ValidateMeshIdentity(const MeshIdentity& id, uint32_t entryCount, std::string* err)
{
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    if (id.sections.size() != entryCount)
        return fail(std::to_string(id.sections.size()) + " sections for " +
                    std::to_string(entryCount) + " library entries");
    std::unordered_set<uint64_t> keys;
    for (size_t e = 0; e < id.sections.size(); ++e)
    {
        const auto& s = id.sections[e];
        if (s.key == 0) return fail("section " + std::to_string(e) + " has key 0");
        if (!keys.insert(s.key).second) return fail("duplicate section key " + MeshKeyToHex(s.key));
        if (s.ownerMesh != UINT32_MAX && s.ownerMesh >= id.meshes.size())
            return fail("section " + std::to_string(e) + " names a mesh that does not exist");
    }
    keys.clear();
    for (size_t i = 0; i < id.meshes.size(); ++i)
    {
        const auto& m = id.meshes[i];
        if (m.key == 0) return fail("mesh " + std::to_string(i) + " has key 0");
        if (!keys.insert(m.key).second) return fail("duplicate mesh key " + MeshKeyToHex(m.key));
        if (m.sections.empty()) return fail("mesh " + std::to_string(i) + " has no sections");
        for (uint32_t e : m.sections)
            if (e >= entryCount) return fail("mesh " + std::to_string(i) + " lists entry " +
                                             std::to_string(e) + " past the table");
    }
    return true;
}

bool WriteMeshLibIdentity(std::vector<uint8_t>& blob, const MeshIdentity& id, std::string* err)
{
    Core c;
    if (!ReadCore(blob.data(), blob.size(), c, err)) return false;
    if (!ValidateMeshIdentity(id, c.entryCount, err)) return false;

    size_t tailOff = 0;
    std::vector<Chunk> chunks;
    const TailRead tr = ReadTail(blob.data(), blob.size(), c, tailOff, chunks, err);
    if (tr == TailRead::Malformed) return false;   // do not guess over a damaged tail

    chunks.erase(std::remove_if(chunks.begin(), chunks.end(),
                                [](const Chunk& ch) { return ch.fourcc == MESHLIB_CHUNK_IDNT; }),
                 chunks.end());
    Chunk idnt;
    idnt.fourcc  = MESHLIB_CHUNK_IDNT;
    idnt.version = MESHLIB_IDNT_VERSION;
    idnt.bytes   = SerializeIdentity(id);
    chunks.insert(chunks.begin(), std::move(idnt));

    // Keep every byte before the current tail (or, with none, the whole file - a tail-less file
    // has nothing after its index buffer that anything reads).
    const size_t cutAt = (tr == TailRead::Ok) ? tailOff : blob.size();
    return WriteTail(blob, cutAt, chunks, err);
}

// ==== The generic tail (Resource/MeshLibTail.h) ====
MeshLibTailState ReadMeshLibTailDirectory(const uint8_t* blob, size_t size, MeshLibTailDirectory& out,
                                          std::string* err)
{
    out = MeshLibTailDirectory{};
    Core c;
    if (!ReadCore(blob, size, c, err)) return MeshLibTailState::Malformed;
    return ReadTailDir(blob, size, c, out, err);
}

bool WriteMeshLibChunk(std::vector<uint8_t>& blob, uint32_t fourcc, uint32_t version,
                       const uint8_t* bytes, size_t size, std::string* err)
{
    Core c;
    if (!ReadCore(blob.data(), blob.size(), c, err)) return false;
    size_t tailOff = 0;
    std::vector<Chunk> chunks;
    const TailRead tr = ReadTail(blob.data(), blob.size(), c, tailOff, chunks, err);
    if (tr == TailRead::Malformed) return false;   // do not guess over a damaged tail

    Chunk fresh;
    fresh.fourcc  = fourcc;
    fresh.version = version;
    if (size) fresh.bytes.assign(bytes, bytes + size);
    // In place when the tail has it (every other chunk keeps its position),
    // else appended - except the identity, which is always first.
    auto it = std::find_if(chunks.begin(), chunks.end(), [&](const Chunk& ch) { return ch.fourcc == fourcc; });
    if (it != chunks.end())
    {
        *it = std::move(fresh);
        chunks.erase(std::remove_if(std::next(it), chunks.end(),
                                    [&](const Chunk& ch) { return ch.fourcc == fourcc; }),
                     chunks.end());
    }
    else if (fourcc == MESHLIB_CHUNK_IDNT) chunks.insert(chunks.begin(), std::move(fresh));
    else                                   chunks.push_back(std::move(fresh));

    const size_t cutAt = (tr == TailRead::Ok) ? tailOff : blob.size();
    return WriteTail(blob, cutAt, chunks, err);
}

bool RemoveMeshLibChunk(std::vector<uint8_t>& blob, uint32_t fourcc, std::string* err)
{
    Core c;
    if (!ReadCore(blob.data(), blob.size(), c, err)) return false;
    size_t tailOff = 0;
    std::vector<Chunk> chunks;
    const TailRead tr = ReadTail(blob.data(), blob.size(), c, tailOff, chunks, err);
    if (tr == TailRead::Malformed) return false;
    if (tr == TailRead::None) return true;
    const size_t before = chunks.size();
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(), [&](const Chunk& ch) { return ch.fourcc == fourcc; }),
                 chunks.end());
    if (chunks.size() == before) return true;
    if (chunks.empty()) return StripMeshLibTail(blob, err);
    return WriteTail(blob, tailOff, chunks, err);
}

MeshLibChunkRead ReadMeshLibChunkAt(const std::string& path, uint32_t fourcc, std::vector<uint8_t>& bytes,
                                    uint32_t& version, std::string* err)
{
    namespace fs = std::filesystem;
    bytes.clear();
    version = 0;
    std::error_code ec;
    const std::string onDisk = AssetFS::Get().ResolveOnDisk(path);
    const fs::path    p      = Sys::U8Path(onDisk.empty() ? path : onDisk);
    if (fs::is_regular_file(p, ec))
    {
        const uint64_t size = fs::file_size(p, ec);
        if (ec) return MeshLibChunkRead::NotMeshLib;
        std::ifstream f(p, std::ios::binary);
        uint8_t head[sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)] = {};
        if (!f.read(reinterpret_cast<char*>(head), sizeof(head))) return MeshLibChunkRead::NotMeshLib;
        Core c;
        if (!ReadCoreHead(head, sizeof(head), static_cast<size_t>(size), c, err)) return MeshLibChunkRead::NotMeshLib;
        return ReadChunkFromStream(f, static_cast<size_t>(size), c, fourcc, bytes, version, err);
    }
    // Not a loose file: the pak's copy, whole (a shipped build).
    std::vector<uint8_t> blob;
    if (!AssetFS::Get().ReadFile(path, blob) || blob.empty()) return MeshLibChunkRead::NotMeshLib;
    Core c;
    if (!ReadCore(blob.data(), blob.size(), c, err)) return MeshLibChunkRead::NotMeshLib;
    MeshLibTailDirectory dir;
    switch (ReadTailDir(blob.data(), blob.size(), c, dir, err))
    {
    case TailRead::None:      return MeshLibChunkRead::NoChunk;
    case TailRead::Malformed: return MeshLibChunkRead::Malformed;
    case TailRead::Ok:        break;
    }
    const MeshLibChunkRef* r = dir.Find(fourcc);
    if (!r) return MeshLibChunkRead::NoChunk;
    bytes.assign(blob.data() + r->offset, blob.data() + r->offset + r->size);
    version = r->version;
    return MeshLibChunkRead::Ok;
}

// ==== Re-import ====
MeshIdentityAdoption AdoptMeshIdentityKeys(const MeshIdentity& previous,
                                           const uint8_t* previousBlob, size_t previousSize,
                                           MeshIdentity& fresh,
                                           const uint8_t* freshBlob, size_t freshSize,
                                           const MeshAdoptOptions* options)
{
    MeshIdentityAdoption r;
    r.previous = true;
    Core pc, fc;
    const bool pcOk = previousBlob && ReadCore(previousBlob, previousSize, pc, nullptr);
    const bool fcOk = freshBlob && ReadCore(freshBlob, freshSize, fc, nullptr);
    auto sameGeometry = [&](uint32_t pe, uint32_t fe) -> bool
    {
        if (!pcOk || !fcOk || pe >= pc.entryCount || fe >= fc.entryCount) return false;
        const MeshLibraryEntry a = EntryAt(previousBlob, pc, pe);
        const MeshLibraryEntry b = EntryAt(freshBlob, fc, fe);
        return a.vertexCount == b.vertexCount && a.indexCount == b.indexCount;
    };

    // ---- Meshes ----
    const size_t PM = previous.meshes.size(), FM = fresh.meshes.size();
    std::vector<int>  meshOf(FM, -1);
    std::vector<char> prevMeshUsed(PM, 0);
    auto matchMeshes = [&](auto keyOf)
    {
        std::unordered_map<std::string, std::vector<uint32_t>> buckets;
        for (uint32_t j = 0; j < PM; ++j)
        {
            if (prevMeshUsed[j]) continue;
            const std::string k = keyOf(previous.meshes[j]);
            if (!k.empty()) buckets[k].push_back(j);
        }
        std::unordered_map<std::string, size_t> cursor;
        for (uint32_t i = 0; i < FM; ++i)
        {
            if (meshOf[i] >= 0) continue;
            const std::string k = keyOf(fresh.meshes[i]);
            if (k.empty()) continue;
            auto it = buckets.find(k);
            if (it == buckets.end()) continue;
            size_t& cur = cursor[k];
            while (cur < it->second.size() && prevMeshUsed[it->second[cur]]) ++cur;
            if (cur >= it->second.size()) continue;
            const uint32_t j = it->second[cur++];
            meshOf[i] = static_cast<int>(j);
            prevMeshUsed[j] = 1;
        }
    };
    // How a pair was made, for the report (0 = by a name / source key).
    enum : char { kByName = 0, kExplicit = 1, kGeometry = 2 };
    std::vector<char> pairedBy(FM, kByName);

    // An explicit rename first: the author said which previous Mesh this is.
    if (options)
        for (const MeshRename& rn : options->renames)
        {
            int      j = -1;
            uint64_t k = 0;
            if (rn.from.size() == 16 && MeshKeyFromHex(rn.from, k))
                for (uint32_t p = 0; p < PM && j < 0; ++p)
                    if (!prevMeshUsed[p] && previous.meshes[p].key == k) j = static_cast<int>(p);
            for (uint32_t p = 0; p < PM && j < 0; ++p)
                if (!prevMeshUsed[p] && previous.meshes[p].name == rn.from) j = static_cast<int>(p);
            int i = -1;
            for (uint32_t f = 0; f < FM && i < 0; ++f)
                if (meshOf[f] < 0 && fresh.meshes[f].name == rn.to) i = static_cast<int>(f);
            if (i < 0 || j < 0)
            {
                r.unresolvedRenames.push_back(rn.from + " -> " + rn.to);
                continue;
            }
            meshOf[static_cast<size_t>(i)] = j;
            prevMeshUsed[static_cast<size_t>(j)] = 1;
            pairedBy[static_cast<size_t>(i)] = kExplicit;
        }

    matchMeshes([](const MeshIdentity::Mesh& m) { return m.sourceKey; });
    matchMeshes([](const MeshIdentity::Mesh& m) { return m.name; });
    matchMeshes([](const MeshIdentity::Mesh& m) { return LooseName(m.name); });

    // A Mesh no name matched on either side whose geometry agrees with exactly
    // one on the other side: the same Mesh, renamed in the source. The
    // signature is the multiset of its sections' (vertex, index) counts, so a
    // reordered material list still agrees.
    if (options && options->geometryRenames && pcOk && fcOk)
    {
        auto signature = [](const Core& c, const uint8_t* blob, const std::vector<uint32_t>& secs)
        {
            std::vector<std::pair<uint32_t, uint32_t>> g;
            for (uint32_t e : secs)
            {
                if (e >= c.entryCount) return std::string();
                const MeshLibraryEntry x = EntryAt(blob, c, e);
                g.emplace_back(x.vertexCount, x.indexCount);
            }
            std::sort(g.begin(), g.end());
            std::string s;
            for (const auto& [v, ix] : g) s += std::to_string(v) + "/" + std::to_string(ix) + ";";
            return s;
        };
        std::unordered_map<std::string, std::vector<uint32_t>> prevBy, freshBy;
        for (uint32_t j = 0; j < PM; ++j)
            if (!prevMeshUsed[j])
                if (std::string s = signature(pc, previousBlob, previous.meshes[j].sections); !s.empty())
                    prevBy[s].push_back(j);
        for (uint32_t i = 0; i < FM; ++i)
            if (meshOf[i] < 0)
                if (std::string s = signature(fc, freshBlob, fresh.meshes[i].sections); !s.empty())
                    freshBy[s].push_back(i);
        for (const auto& [s, fi] : freshBy)
        {
            const auto it = prevBy.find(s);
            if (fi.size() != 1 || it == prevBy.end() || it->second.size() != 1) continue;   // ambiguous: not guessed
            meshOf[fi[0]] = static_cast<int>(it->second[0]);
            prevMeshUsed[it->second[0]] = 1;
            pairedBy[fi[0]] = kGeometry;
        }
    }

    // ---- Sections ----
    const size_t PS = previous.sections.size(), FS = fresh.sections.size();
    std::vector<int>  secOf(FS, -1);
    std::vector<char> prevSecUsed(PS, 0);
    auto take = [&](uint32_t f, uint32_t p) { secOf[f] = static_cast<int>(p); prevSecUsed[p] = 1; };

    for (uint32_t i = 0; i < FM; ++i)
    {
        if (meshOf[i] < 0) continue;
        const auto& fl = fresh.meshes[i].sections;
        const auto& pl = previous.meshes[static_cast<size_t>(meshOf[i])].sections;
        auto within = [&](auto keyOf)
        {
            std::unordered_map<std::string, std::vector<uint32_t>> buckets;
            for (uint32_t p : pl)
            {
                if (p >= PS || prevSecUsed[p]) continue;
                const std::string k = keyOf(previous.sections[p]);
                if (!k.empty()) buckets[k].push_back(p);
            }
            std::unordered_map<std::string, size_t> cursor;
            for (uint32_t f : fl)
            {
                if (f >= FS || secOf[f] >= 0) continue;
                const std::string k = keyOf(fresh.sections[f]);
                if (k.empty()) continue;
                auto it = buckets.find(k);
                if (it == buckets.end()) continue;
                size_t& cur = cursor[k];
                while (cur < it->second.size() && prevSecUsed[it->second[cur]]) ++cur;
                if (cur >= it->second.size()) continue;
                take(f, it->second[cur++]);
            }
        };
        within([](const MeshIdentity::Section& s) { return s.sourceKey; });
        within([](const MeshIdentity::Section& s) { return s.slotName; });
        within([](const MeshIdentity::Section& s) { return LooseName(s.slotName); });

        // Whatever is left, pairwise in order - but only where the geometry agrees,
        // so a reordered material list cannot hand a key to the wrong section.
        std::vector<uint32_t> fr, pr;
        for (uint32_t f : fl) if (f < FS && secOf[f] < 0) fr.push_back(f);
        for (uint32_t p : pl) if (p < PS && !prevSecUsed[p]) pr.push_back(p);
        for (size_t k = 0; k < std::min(fr.size(), pr.size()); ++k)
            if (sameGeometry(pr[k], fr[k])) take(fr[k], pr[k]);
    }

    // Anywhere, by source key.
    {
        std::unordered_map<std::string, std::vector<uint32_t>> buckets;
        for (uint32_t p = 0; p < PS; ++p)
            if (!prevSecUsed[p] && !previous.sections[p].sourceKey.empty())
                buckets[previous.sections[p].sourceKey].push_back(p);
        for (uint32_t f = 0; f < FS; ++f)
        {
            if (secOf[f] >= 0) continue;
            auto it = buckets.find(fresh.sections[f].sourceKey);
            if (it == buckets.end()) continue;
            for (uint32_t p : it->second)
                if (!prevSecUsed[p]) { take(f, p); break; }
        }
    }
    // An index-derived previous identity (synthesized / backfilled) was keyed BY
    // position, so position with agreeing geometry is its natural match.
    if (previous.flags & (IDNT_FLAG_SYNTHESIZED | IDNT_FLAG_BACKFILLED))
    {
        for (uint32_t f = 0; f < FS && f < PS; ++f)
            if (secOf[f] < 0 && !prevSecUsed[f] && sameGeometry(f, f)) take(f, f);
    }

    // ---- Apply ----
    // Adopted keys win; a freshly derived key that happens to equal an adopted one is
    // re-derived (the fresh item is the newcomer).
    std::unordered_set<uint64_t> usedM, usedS;
    for (uint32_t i = 0; i < FM; ++i)
        if (meshOf[i] >= 0) { fresh.meshes[i].key = previous.meshes[static_cast<size_t>(meshOf[i])].key; usedM.insert(fresh.meshes[i].key); ++r.meshesKept; }
    for (uint32_t f = 0; f < FS; ++f)
        if (secOf[f] >= 0) { fresh.sections[f].key = previous.sections[static_cast<size_t>(secOf[f])].key; usedS.insert(fresh.sections[f].key); ++r.sectionsKept; }
    for (uint32_t i = 0; i < FM; ++i)
    {
        if (meshOf[i] >= 0) continue;
        ++r.meshesNew;
        if (usedM.count(fresh.meshes[i].key))
            fresh.meshes[i].key = DeriveUniqueKey("mesh", fresh.meshes[i].sourceKey, usedM, "mesh");
        else
            usedM.insert(fresh.meshes[i].key);
    }
    for (uint32_t f = 0; f < FS; ++f)
    {
        if (secOf[f] >= 0) continue;
        ++r.sectionsNew;
        if (usedS.count(fresh.sections[f].key))
            fresh.sections[f].key = DeriveUniqueKey("sec", fresh.sections[f].sourceKey, usedS, "section");
        else
            usedS.insert(fresh.sections[f].key);
    }
    for (char u : prevMeshUsed) if (!u) ++r.meshesGone;
    for (char u : prevSecUsed)  if (!u) ++r.sectionsGone;

    // ---- The report ----
    // A section two Meshes list is reported once.
    std::unordered_set<uint32_t> seenFresh, seenPrev;
    for (uint32_t i = 0; i < FM; ++i)
    {
        const MeshIdentity::Mesh& fm = fresh.meshes[i];
        if (meshOf[i] < 0)
        {
            r.addedMeshes.push_back({ fm.key, fm.name });
            continue;
        }
        const MeshIdentity::Mesh& pm = previous.meshes[static_cast<size_t>(meshOf[i])];
        if (pm.name != fm.name || pairedBy[i] != kByName)
            r.renamedMeshes.push_back({ fm.key, pm.name, fm.name,
                                        pairedBy[i] == kExplicit ? MeshIdentityAdoption::MeshRenamed::How::Explicit
                                        : pairedBy[i] == kGeometry ? MeshIdentityAdoption::MeshRenamed::How::Geometry
                                                                   : MeshIdentityAdoption::MeshRenamed::How::Name });
        for (uint32_t f : fm.sections)
        {
            if (f >= FS || !seenFresh.insert(f).second) continue;
            const MeshIdentity::Section& fs = fresh.sections[f];
            if (secOf[f] < 0)
                r.addedSections.push_back({ fs.key, fm.name, fs.slotName });
            else if (const auto& ps = previous.sections[static_cast<size_t>(secOf[f])]; ps.slotName != fs.slotName)
                r.renamedSlots.push_back({ fs.key, fm.name, ps.slotName, fs.slotName });
        }
        for (uint32_t p : pm.sections)
            if (p < PS && !prevSecUsed[p] && seenPrev.insert(p).second)
                r.removedSections.push_back({ previous.sections[p].key, pm.name, previous.sections[p].slotName });
    }
    for (uint32_t j = 0; j < PM; ++j)
        if (!prevMeshUsed[j]) r.removedMeshes.push_back({ previous.meshes[j].key, previous.meshes[j].name });

    fresh.flags &= ~(IDNT_FLAG_SYNTHESIZED | IDNT_FLAG_BACKFILLED);
    fresh.RebuildIndex();
    return r;
}

// ==== Section keys by path ====
namespace
{
    // What the file's bytes say: the IDNT's keys, else LegacySectionKey's. @p idnt is the IDNT
    // chunk's bytes (null: the file has none or it did not read).
    void KeysFromIdentity(const Core& c, const uint8_t* idnt, size_t idntSize, uint32_t idntVersion,
                          MeshLibKeyTable& t)
    {
        MeshIdentity id;
        t.ownIdentity = idnt && idntVersion == MESHLIB_IDNT_VERSION &&
                        ParseIdentityChunk(idnt, idntSize, c.entryCount, id, nullptr) == MeshIdentityParse::Ok;
        t.keys.resize(c.entryCount);
        for (uint32_t e = 0; e < c.entryCount; ++e)
            t.keys[e] = t.ownIdentity ? id.sections[e].key : LegacySectionKey(e);
        t.entryOf.reserve(t.keys.size());
        for (uint32_t e = 0; e < c.entryCount; ++e) t.entryOf.emplace(t.keys[e], e);
    }

    std::shared_ptr<const MeshLibKeyTable> ReadKeyTable(const std::string& path, bool& loose, uint64_t& sig0,
                                                        uint64_t& sig1)
    {
        namespace fs = std::filesystem;
        auto t = std::make_shared<MeshLibKeyTable>();
        std::error_code ec;
        // The loose file wherever the search roots find it - a worktree's
        // shared content root too; not loose = the pak's copy.
        const std::string onDisk = AssetFS::Get().ResolveOnDisk(path);
        const fs::path p = Sys::U8Path(onDisk.empty() ? path : onDisk);
        loose = fs::is_regular_file(p, ec);
        if (loose)
        {
            const uint64_t size = fs::file_size(p, ec);
            if (ec) return nullptr;
            sig0 = size;
            sig1 = static_cast<uint64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
            std::ifstream f(p, std::ios::binary);
            uint8_t head[sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)] = {};
            if (!f.read(reinterpret_cast<char*>(head), sizeof(head))) return nullptr;
            Core c;
            if (!ReadCoreHead(head, sizeof(head), static_cast<size_t>(size), c, nullptr)) return nullptr;
            // The header, the directory and the IDNT chunk only - never [tail, EOF), which also carries
            // the MLET.
            std::vector<uint8_t> idnt;
            uint32_t             version = 0;
            const bool have = ReadChunkFromStream(f, static_cast<size_t>(size), c, MESHLIB_CHUNK_IDNT, idnt, version,
                                                  nullptr) == MeshLibChunkRead::Ok;
            KeysFromIdentity(c, have ? idnt.data() : nullptr, idnt.size(), version, *t);
            return t;
        }
        // Not a loose file: the pak's copy, whole (a shipped build; immutable).
        std::vector<uint8_t> blob;
        if (!AssetFS::Get().ReadFile(path, blob) || blob.empty()) return nullptr;
        sig0 = sig1 = 0;
        Core c;
        if (!ReadCore(blob.data(), blob.size(), c, nullptr)) return nullptr;
        MeshLibTailDirectory dir;
        const MeshLibChunkRef* idnt = ReadTailDir(blob.data(), blob.size(), c, dir, nullptr) == TailRead::Ok
                                    ? dir.Find(MESHLIB_CHUNK_IDNT) : nullptr;
        KeysFromIdentity(c, idnt ? blob.data() + idnt->offset : nullptr, idnt ? idnt->size : 0,
                         idnt ? idnt->version : 0, *t);
        return t;
    }
}

std::shared_ptr<const MeshLibKeyTable> MeshLibKeyTableAt(const std::string& path)
{
    if (path.empty()) return nullptr;
    struct Cached { std::shared_ptr<const MeshLibKeyTable> table; bool loose = false; uint64_t sig0 = 0, sig1 = 0; };
    static std::mutex                              s_mutex;
    static std::unordered_map<std::string, Cached> s_cache;

    const std::string k = MeshLibCanonicalPath(path);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (auto it = s_cache.find(k); it != s_cache.end())
        {
            const Cached& c = it->second;
            if (!c.loose) return c.table;   // the pak's copy does not change
            namespace fs = std::filesystem;
            std::error_code ec;
            const std::string onDisk = AssetFS::Get().ResolveOnDisk(path);
            const fs::path p = Sys::U8Path(onDisk.empty() ? path : onDisk);
            const uint64_t size = fs::file_size(p, ec);
            const uint64_t when = ec ? 0 : static_cast<uint64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
            if (!ec && size == c.sig0 && when == c.sig1) return c.table;
        }
    }
    Cached fresh;
    fresh.table = ReadKeyTable(path, fresh.loose, fresh.sig0, fresh.sig1);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[k] = fresh;
    return fresh.table;
}

} // namespace Resource
