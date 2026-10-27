#pragma once

// Component lines no serializer in this build reads (a module this exe does not link, or a
// format it does not know), kept verbatim on the entity they were loaded onto so the next
// save writes them back: a scene round-trips through a build without the module that owns
// them. Nothing reads them at run time. Serialized as one "Unresolved: line=<encoded line>"
// per line, which a build with the module reads back as the component itself.

#include <string>
#include <vector>

struct UnresolvedComponents
{
    std::vector<std::string> lines;   // "  Tag: k=v k=v", in load order
};
