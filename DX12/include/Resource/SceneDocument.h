#pragma once
// Resource/SceneDocument.h - a .iscene FILE as a document, not a World.
//
// Resource::SaveScene can only write a World, and loading one needs a device
// (skinned instances need the MeshLibrary). Offline authoring - add a prefab
// instance, move a node, change a data component, then let a fresh process
// load the result - needs neither: it edits the file's own lines.
//
// The codec is exact: a document loaded and written back without an edit is
// byte-identical to the file (the asset tool's `roundtrip` proves it over every
// scene). An edit rewrites only the lines it touches, token by token - every
// other line, and every token it does not know, stays as it was. Lines are
// parsed by the loader's own parsers (SceneText.h), so the tool cannot read a
// line differently from the engine.
//
//   [AssetHeader 24 B][WorldMeta 16 B: nodeCount, textLength][text][\0]
//   text:  # comment / W name=... / N idx= parent= tx.. name= / "  Tag: k=v" /
//          MO MR MRI MOI CPR MC FS ... (per-mesh / per-placement overrides)
//
// A component line belongs to the most recent N line (the loader's rule).
// nodeCount is informational (SaveScene counts every entity, the loader never
// reads it); an added node adds one.

#include "Resource/AssetHeader.h"
#include "Resource/SceneText.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Resource
{
    class SceneDocument
    {
    public:
        struct Node
        {
            SceneText::NodeLine      fields;           // as the loader reads its N line
            std::size_t              line = 0;         // its N line in Lines()
            std::vector<std::size_t> componentLines;   // its component lines in Lines()
            std::vector<std::string> tags;             // their tags, in order
        };

        struct Finding
        {
            bool        error = false;    // false = warning
            std::string code;             // e.g. duplicate_idx, unknown_component
            std::size_t line = 0;         // 1-based text line (0 = the header)
            std::string message;
        };

        bool LoadFile(const std::string& utf8Path, std::string& error);
        bool LoadBytes(const std::vector<std::uint8_t>& bytes, std::string& error);
        // The file this document is: the loaded bytes exactly while unedited.
        std::vector<std::uint8_t> Bytes() const;

        const AssetHeader&              Header() const { return m_header; }
        const std::vector<std::string>& Lines() const  { return m_lines; }
        const std::vector<Node>&        Nodes() const  { return m_nodes; }
        std::uint32_t                   NodeCount() const;       // WorldMeta.nodeCount
        std::uint32_t                   MetaTextLength() const;  // WorldMeta.textLength as stored
        std::size_t                     TextSize() const;        // the text's actual byte count
        bool                            Edited() const { return m_edited; }
        // key=value pairs of the W line (decoded values), empty when absent.
        std::vector<std::pair<std::string, std::string>> World() const;

        // A node by idx ("12") or by a unique name; -1 and @p error otherwise.
        int Find(const std::string& ref, std::string& error) const;

        // ---- Edits: each rewrites only the lines it touches ----
        // nullptr leaves that part as it is; rotation is a quaternion xyzw.
        bool SetTransform(int node, const float* t, const float* q, const float* s, std::string& error);
        bool Rename(int node, const std::string& name, std::string& error);
        // Keys of the node's ONE line of @p tag; only keys that line has. Values
        // are line text (already encoded). The line's $g_ reference stamps are
        // re-derived (AssetRefs::AugmentSceneText).
        bool SetComponentValues(int node, const std::string& tag,
                                const std::vector<std::pair<std::string, std::string>>& values,
                                std::string& error, std::string* errorCode = nullptr);
        // A new node after the last node's lines, with its component lines
        // ("  Tag: k=v", reference stamps added). @p parentIdx -1 = a root.
        // Returns the new node's idx, or -1 and @p error.
        int AddNode(int parentIdx, const std::string& name, const float t[3], const float q[4],
                    const float s[3], const std::vector<std::string>& componentLines, std::string& error);
        // Refused while a node names it as parent or an override line (CPR
        // idx=, MRI / MOI root=) refers to it.
        bool RemoveNode(int node, std::string& error);

        // Header, lines, ids, parents, numbers, component tags (the registry),
        // and - with @p references - that referenced asset files exist.
        std::vector<Finding> Validate(bool references) const;

    private:
        void Reindex();
        void ReplaceTokens(std::size_t lineIndex, const std::vector<std::pair<std::string, std::string>>& values);

        AssetHeader               m_header{};
        std::vector<std::uint8_t> m_meta;          // metadataSize bytes, as read
        std::vector<std::string>  m_lines;         // the text, split at '\n'
        bool                      m_finalNewline = true;
        std::string               m_payloadTail;   // payload bytes after the text (normally "\0")
        std::vector<std::uint8_t> m_fileTail;      // bytes after the payload (normally none)
        std::vector<Node>         m_nodes;
        int                       m_nodeDelta = 0; // nodes added - removed
        bool                      m_edited = false;
    };
}
