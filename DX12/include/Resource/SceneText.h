#pragma once
// Resource/SceneText.h - the .iscene LINE formats, as LoadScene reads them and SaveScene
// writes them. Defined in SceneSerializer.cpp next to the loader's own parsers, so the offline
// document codec (Resource/SceneDocument.h) and the engine share one parser and one formatter.
//
//   N idx=<i> parent=<p> tx= ty= tz= qx= qy= qz= qw= sx= sy= sz= name=<percent-encoded>
//     <Tag>: k=v k=v            (a component of the N line above it)

#include "Resource/ComponentSerializers.h"   // KVMap

#include <string>

namespace Resource::SceneText
{
    struct NodeLine
    {
        int         idx    = -1;
        int         parent = -1;
        float       t[3] = { 0.f, 0.f, 0.f };
        float       q[4] = { 0.f, 0.f, 0.f, 1.f };   // quaternion xyzw
        float       s[3] = { 1.f, 1.f, 1.f };
        std::string name;                             // decoded
    };

    // An "N " line as LoadScene reads it; false when it is not one (idx < 0).
    bool ParseNodeLine(const std::string& line, NodeLine& out);
    // The "N " line SaveScene writes for @p n (no newline): %.6f fields,
    // the name percent-encoded.
    std::string FormatNodeLine(const NodeLine& n);
    // A "  Tag: k=v" component line as LoadScene reads it.
    bool ParseComponentLine(const std::string& line, std::string& tag, KVMap& kv);

    // The text encoding of names and paths inside a line (space and '%').
    std::string EncodeText(const std::string& s);
    std::string DecodeText(const std::string& s);
}
