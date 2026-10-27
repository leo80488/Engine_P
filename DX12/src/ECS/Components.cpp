#include "ECS/Components.h"
#include <algorithm>

// ==== MaterialComponent ====

uint32_t MaterialComponent::GetStencilRef() const
{
    if (engineStencilRef == StencilRef::Custom)
        return static_cast<uint32_t>(userStencilRef);
    return 0u;
}

BlendMode MaterialComponent::GetBlendMode() const
{
    return userBlendMode;
}

// ==== MeshComponent ====

uint32_t MeshComponent::GetLODCount() const
{
    return subsetsPerLod == 0 ? 1u : (static_cast<uint32_t>(subsets.size()) / subsetsPerLod);
}

void MeshComponent::GetLODSubsetRange(uint32_t lod, uint32_t& firstSubset, uint32_t& lastSubset) const
{
    firstSubset = 0;
    lastSubset = static_cast<uint32_t>(subsets.size());
    if (subsetsPerLod > 0)
    {
        uint32_t lodCount = GetLODCount();
        lod = (lod >= lodCount) ? (lodCount - 1) : lod;
        firstSubset = subsetsPerLod * lod;
        lastSubset = firstSubset + subsetsPerLod;
    }
}

static void ComputeAABBFromPositions(
    const std::vector<DirectX::XMFLOAT3>& positions,
    AABB& outAABB)
{
    if (positions.empty())
    {
        outAABB._min = outAABB._max = DirectX::XMFLOAT3(0, 0, 0);
        return;
    }
    float minX = positions[0].x, minY = positions[0].y, minZ = positions[0].z;
    float maxX = minX, maxY = minY, maxZ = minZ;
    for (size_t i = 1; i < positions.size(); ++i)
    {
        const auto& p = positions[i];
        minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x);
        minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y);
        minZ = (std::min)(minZ, p.z); maxZ = (std::max)(maxZ, p.z);
    }
    outAABB._min = DirectX::XMFLOAT3(minX, minY, minZ);
    outAABB._max = DirectX::XMFLOAT3(maxX, maxY, maxZ);
}

void MeshComponent::ComputeNormals(bool smooth)
{
    if (vertexPositions.empty() || indices.empty())
        return;

    vertexNormals.resize(vertexPositions.size());
    for (auto& n : vertexNormals)
        n = DirectX::XMFLOAT3(0, 0, 0);

    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        if (i0 >= vertexPositions.size() || i1 >= vertexPositions.size() || i2 >= vertexPositions.size())
            continue;

        DirectX::XMVECTOR p0 = DirectX::XMLoadFloat3(&vertexPositions[i0]);
        DirectX::XMVECTOR p1 = DirectX::XMLoadFloat3(&vertexPositions[i1]);
        DirectX::XMVECTOR p2 = DirectX::XMLoadFloat3(&vertexPositions[i2]);
        DirectX::XMVECTOR e1 = DirectX::XMVectorSubtract(p1, p0);
        DirectX::XMVECTOR e2 = DirectX::XMVectorSubtract(p2, p0);
        DirectX::XMVECTOR n = DirectX::XMVector3Cross(e1, e2);

        DirectX::XMFLOAT3 fn;
        DirectX::XMStoreFloat3(&fn, n);
        vertexNormals[i0].x += fn.x; vertexNormals[i0].y += fn.y; vertexNormals[i0].z += fn.z;
        vertexNormals[i1].x += fn.x; vertexNormals[i1].y += fn.y; vertexNormals[i1].z += fn.z;
        vertexNormals[i2].x += fn.x; vertexNormals[i2].y += fn.y; vertexNormals[i2].z += fn.z;
    }

    for (size_t i = 0; i < vertexNormals.size(); ++i)
    {
        DirectX::XMVECTOR n = DirectX::XMLoadFloat3(&vertexNormals[i]);
        n = DirectX::XMVector3Normalize(n);
        DirectX::XMStoreFloat3(&vertexNormals[i], n);
    }
}

void MeshComponent::FlipCulling()
{
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
        std::swap(indices[i + 1], indices[i + 2]);
}

void MeshComponent::Recenter()
{
    if (vertexPositions.empty())
        return;

    AABB box;
    ComputeAABBFromPositions(vertexPositions, box);
    float cx = (box._min.x + box._max.x) * 0.5f;
    float cy = (box._min.y + box._max.y) * 0.5f;
    float cz = (box._min.z + box._max.z) * 0.5f;

    for (auto& p : vertexPositions)
    {
        p.x -= cx;
        p.y -= cy;
        p.z -= cz;
    }
    ComputeAABBFromPositions(vertexPositions, aabb);
}
