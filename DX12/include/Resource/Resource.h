#pragma once

// Resource base class: concrete resources returned by loaders (Texture, Mesh, Sound, etc.) derive from this.
namespace Resource
{
    class Resource
    {
    public:
        virtual ~Resource() = default;

    };
}
