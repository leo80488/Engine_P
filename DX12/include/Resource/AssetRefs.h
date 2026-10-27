#pragma once
// AssetRefs: the one definition of "a value in a serialized container that names another
// asset", plus the guid stamp that lets such a value survive its target being renamed or moved.
// References serialize as path strings, so a moved asset would dangle every .imat / .iscene /
// .ipfb and every shipped save naming it. Each asset also has a stable 128-bit guid in its .meta
// sidecar (Resource::AssetGuidIndex); this layer writes that guid next to each reference and
// resolves through it when the path misses.
// - The path is the primary key: resolution stops at a path hit (one existence check per
//   reference; nothing at all in a packed build).
// - The guid is a fallback, a sibling token / line prefixed `$g_`
//   (`texturePath=asset/x.itex $g_texturePath=6f3a...`); no serializer prefix ("tex_", "ctex_",
//   "cparam_", "r<N>_b", ...) can collide with it.
// - Resolve*Text strips every `$g_` token, so no deserializer downstream knows this layer exists.
// - Augment*Text is idempotent: it strips, then re-derives (what re-saving a scene does).
// Resolve is gated on the text containing `$g_`, Augment on the index being available, so an
// unmigrated project pays nothing.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Resource::AssetRefs
{
    // Token/line prefix carrying a reference's guid.
    inline constexpr const char* kGuidPrefix = "$g_";

    // ==== Shared predicates and encodings ====
    // The reference policy: Editor::AssetReferenceScanner shares these, so "what counts as a
    // reference" has exactly one definition.

    std::string ToLowerAscii(std::string s);

    // Percent-decode (%XX). Mirrors ComponentSerializers' PercentDecode.
    std::string PercentDecode(std::string_view s);
    // Escapes only '%' and ' ', the two characters that would break the whitespace tokenizer of a
    // plain .iscene / .ipfb KV value.
    std::string PercentEncode(std::string_view s);
    // Allowlist encoder for a multi-line payload carried as one token (the MaterialInline `data=`
    // blob). Mirrors ComponentSerializers' PercentEncodeStrict.
    std::string PercentEncodeStrict(std::string_view s);

    // Index / compare form: percent-decoded, '\' -> '/', lowercased, lexically normal, leading "./"
    // stripped. Comparison only: never write it back into a container, it has lost the authored case.
    std::string NormalizeRefPath(const std::string& raw);

    // A value counts as an asset reference when it ends in a known engine
    // extension AND carries path shape (a separator, or an "asset" prefix).
    // `lowerValue` must already be lowercased.
    bool LooksLikeAssetPath(const std::string& lowerValue);

    // Same test on a raw (any-case) value.
    bool ValueIsAssetPath(const std::string& value);

    // ==== .iscene / .ipfb component-line text ====

    // Append a `$g_<key>=<guid>` token beside every reference whose target has
    // a guid. Recurses into MaterialInline `data=` blobs. Idempotent.
    std::string AugmentSceneText(const std::string& text);

    // Replace every reference whose path does not resolve with the current path of its `$g_` guid,
    // then strip every `$g_` token. @p forFile only labels the log line; @p outRepaired receives the
    // number of repaired values (non-zero: the container on disk is stale and worth re-saving).
    std::string ResolveSceneText(const std::string& text,
                                 const std::string& forFile,
                                 std::size_t* outRepaired = nullptr);

    // ==== .imat / .imatinst text ====

    // Same two operations for `key = value` material text. Handles the
    // two-token `ctex_<Name> = <uvset> <path>` shape.
    std::string AugmentImatText(const std::string& text);
    std::string ResolveImatText(const std::string& text,
                                const std::string& forFile,
                                std::size_t* outRepaired = nullptr);

    // True when @p text carries any guid token: the cheap gate every resolve entry point takes first.
    bool HasGuidTokens(const std::string& text);

    // ==== Enumeration (the asset catalog) ====
    // Every reference in a container's text, found by the same parse Augment / Resolve use, so what
    // the catalog lists and what the loader follows cannot drift. Read-only: nothing is resolved,
    // stamped or logged.
    struct TextRef
    {
        std::string key;        // "tex_BASECOLORMAP", "path", "navMesh", a MaterialOverride key...
        std::string tag;        // .iscene/.ipfb component tag ("" for material text / the W line)
        std::string path;       // as authored (percent-decoded, t: tag stripped)
        std::string guidHex;    // its `$g_` stamp, "" when it has none
        unsigned    line = 0;   // 1-based line in `text`
        bool        inlineMaterial = false;   // inside a MaterialInline data= blob
    };
    std::vector<TextRef> ListSceneTextRefs(const std::string& text);
    std::vector<TextRef> ListImatTextRefs(const std::string& text);

    // Resolution memoizes which reference targets exist (one stat per reference per session), which
    // is sound only while the asset tree is still. Call this whenever the tree moves; AssetGuidIndex
    // does on every Rebuild / Repoint / Forget / Invalidate. Do not skip it: a path stat'ed before a
    // move stays "present" forever, and the very reference the guid layer exists to repair is
    // skipped silently.
    void ForgetExistenceCache();
}
