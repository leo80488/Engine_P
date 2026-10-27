#include "Resource/ProceduralMesh.h"

#include <cmath>

// ==== Cube ====
ProceduralMesh::MeshData
ProceduralMesh::Cube(float h, std::array<float, 3> color)
{
    MeshData m;

    // 6 faces x 4 verts, winding CCW viewed from outside each face.
    // The tangent points along each face's U axis; w = bitangent handedness (-1).
    struct Face
    {
        float n[3];       // face normal
        float t[3];       // face tangent (U direction)
        float v[4][3];    // vertex positions
        float uv[4][2];   // UV coordinates: (0,1),(1,1),(1,0),(0,0) per face
    };
    const Face faces[6] =
    {
        // +X: T=-Z
        {{ 1, 0, 0}, {0, 0,-1},
         {{ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h}},
         {{0,1},{1,1},{1,0},{0,0}}},
        // -X: T=+Z
        {{-1, 0, 0}, {0, 0, 1},
         {{-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h}},
         {{0,1},{1,1},{1,0},{0,0}}},
        // +Y: T=+X
        {{ 0, 1, 0}, {1, 0, 0},
         {{-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h}},
         {{0,1},{1,1},{1,0},{0,0}}},
        // -Y: T=+X
        {{ 0,-1, 0}, {1, 0, 0},
         {{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}},
         {{0,1},{1,1},{1,0},{0,0}}},
        // +Z: T=+X
        {{ 0, 0, 1}, {1, 0, 0},
         {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}},
         {{0,1},{1,1},{1,0},{0,0}}},
        // -Z: T=-X
        {{ 0, 0,-1}, {-1, 0, 0},
         {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}},
         {{0,1},{1,1},{1,0},{0,0}}},
    };

    for (const auto& f : faces)
    {
        const auto base = static_cast<uint16_t>(m.positions.size());
        for (int i = 0; i < 4; ++i)
        {
            m.positions.push_back({ f.v[i][0], f.v[i][1], f.v[i][2] });
            m.normals.push_back({ f.n[0], f.n[1], f.n[2] });
            m.tangents.push_back({ f.t[0], f.t[1], f.t[2], -1.f });
            m.uvs.push_back({ f.uv[i][0], f.uv[i][1] });
            m.colors.push_back(color);
        }
        m.indices.push_back(base + 0); m.indices.push_back(base + 1); m.indices.push_back(base + 2);
        m.indices.push_back(base + 0); m.indices.push_back(base + 2); m.indices.push_back(base + 3);
    }
    return m;
}

// ==== Sphere ====
ProceduralMesh::MeshData
ProceduralMesh::Sphere(float radius, uint32_t rings, uint32_t slices,
                       std::array<float, 3> color)
{
    MeshData m;

    constexpr float PI = 3.14159265358979f;

    for (uint32_t r = 0; r <= rings; ++r)
    {
        const float theta    = PI * static_cast<float>(r) / static_cast<float>(rings);
        const float sinTheta = sinf(theta);
        const float cosTheta = cosf(theta);

        for (uint32_t s = 0; s <= slices; ++s)
        {
            const float phi = 2.f * PI * static_cast<float>(s) / static_cast<float>(slices);
            const float x   = sinTheta * cosf(phi);
            const float y   = cosTheta;
            const float z   = sinTheta * sinf(phi);

            // Tangent = dP/dphi (normalised): points in the +longitude direction.
            const float tx = -sinf(phi);
            const float tz =  cosf(phi);

            m.positions.push_back({ x * radius, y * radius, z * radius });
            m.normals.push_back({ x, y, z });
            m.tangents.push_back({ tx, 0.f, tz, 1.f });
            m.uvs.push_back({ static_cast<float>(s) / static_cast<float>(slices),
                               static_cast<float>(r) / static_cast<float>(rings) });
            m.colors.push_back(color);
        }
    }

    for (uint32_t r = 0; r < rings; ++r)
    {
        for (uint32_t s = 0; s < slices; ++s)
        {
            const auto v0 = static_cast<uint16_t>( r      * (slices + 1) + s    );
            const auto v1 = static_cast<uint16_t>((r + 1) * (slices + 1) + s    );
            const auto v2 = static_cast<uint16_t>((r + 1) * (slices + 1) + s + 1);
            const auto v3 = static_cast<uint16_t>( r      * (slices + 1) + s + 1);

            m.indices.push_back(v0); m.indices.push_back(v2); m.indices.push_back(v1);
            m.indices.push_back(v0); m.indices.push_back(v3); m.indices.push_back(v2);
        }
    }
    return m;
}

// ==== Cone ====
ProceduralMesh::MeshData
ProceduralMesh::Cone(float radius, float height, uint32_t slices,
                     std::array<float, 3> color)
{
    MeshData m;

    constexpr float PI = 3.14159265358979f;
    const float halfH = height * 0.5f;

    // Outward-tilted normal magnitude components for the side surface.
    const float len     = sqrtf(radius * radius + height * height);
    const float sideNY  = radius  / len;   // sin(half-angle)
    const float sideNXZ = height  / len;   // cos(half-angle)

    // Side rim vertices (outward-tilted normals).
    const auto sideRimBase = static_cast<uint16_t>(m.positions.size());
    for (uint32_t i = 0; i <= slices; ++i)
    {
        const float phi = 2.f * PI * static_cast<float>(i) / static_cast<float>(slices);
        const float cx  = cosf(phi), cz = sinf(phi);
        const float u   = static_cast<float>(i) / static_cast<float>(slices);
        m.positions.push_back({ cx * radius, -halfH, cz * radius });
        m.normals.push_back({ cx * sideNXZ, sideNY, cz * sideNXZ });
        m.tangents.push_back({ -sinf(phi), 0.f, cosf(phi), 1.f });
        m.uvs.push_back({ u, 1.f });
        m.colors.push_back(color);
    }

    // Apex vertices, duplicated per slice for correct normals / UVs.
    const auto apexBase = static_cast<uint16_t>(m.positions.size());
    for (uint32_t i = 0; i < slices; ++i)
    {
        // Center the apex normal and UV on the middle of the triangle.
        const float phi = 2.f * PI * (static_cast<float>(i) + 0.5f) / static_cast<float>(slices);
        const float cx  = cosf(phi), cz = sinf(phi);
        const float u   = (static_cast<float>(i) + 0.5f) / static_cast<float>(slices);
        m.positions.push_back({ 0.f, halfH, 0.f });
        m.normals.push_back({ cx * sideNXZ, sideNY, cz * sideNXZ });
        m.tangents.push_back({ -sinf(phi), 0.f, cosf(phi), 1.f });
        m.uvs.push_back({ u, 0.f });
        m.colors.push_back(color);
    }

    // Base cap rim vertices (downward normals).
    const auto baseRimBase = static_cast<uint16_t>(m.positions.size());
    for (uint32_t i = 0; i < slices; ++i)
    {
        const float phi = 2.f * PI * static_cast<float>(i) / static_cast<float>(slices);
        const float cx  = cosf(phi), cz = sinf(phi);
        const float u   = cx * 0.5f + 0.5f;
        const float v   = cz * 0.5f + 0.5f;
        m.positions.push_back({ cx * radius, -halfH, cz * radius });
        m.normals.push_back({ 0.f, -1.f, 0.f });
        m.tangents.push_back({ 1.f, 0.f, 0.f, 1.f });
        m.uvs.push_back({ u, v });
        m.colors.push_back(color);
    }

    // Base center.
    const auto centerIdx = static_cast<uint16_t>(m.positions.size());
    m.positions.push_back({ 0.f, -halfH, 0.f });
    m.normals.push_back({ 0.f, -1.f, 0.f });
    m.tangents.push_back({ 1.f, 0.f, 0.f, 1.f });
    m.uvs.push_back({ 0.5f, 0.5f });
    m.colors.push_back(color);

    // Side triangles: apex[i] -> rim[i+1] -> rim[i].
    for (uint32_t i = 0; i < slices; ++i)
    {
        const auto apex = static_cast<uint16_t>(apexBase + i);
        const auto curr = static_cast<uint16_t>(sideRimBase + i);
        const auto next = static_cast<uint16_t>(sideRimBase + i + 1);
        m.indices.push_back(apex);
        m.indices.push_back(next);
        m.indices.push_back(curr);
    }

    // Base cap: center -> rim[i] -> rim[i+1] (CCW from below).
    for (uint32_t i = 0; i < slices; ++i)
    {
        const auto curr = static_cast<uint16_t>(baseRimBase + i);
        const auto next = static_cast<uint16_t>(baseRimBase + (i + 1) % slices);
        m.indices.push_back(centerIdx);
        m.indices.push_back(curr);
        m.indices.push_back(next);
    }
    return m;
}

// ==== Plane ====
ProceduralMesh::MeshData
ProceduralMesh::Plane(float halfSize, uint32_t subdivX, uint32_t subdivZ,
                      std::array<float, 3> color)
{
    MeshData m;

    if (subdivX < 1) subdivX = 1;
    if (subdivZ < 1) subdivZ = 1;

    // Grid of (subdivX+1) x (subdivZ+1) vertices on the XZ plane, normal = +Y.
    for (uint32_t z = 0; z <= subdivZ; ++z)
    {
        const float fz = static_cast<float>(z) / static_cast<float>(subdivZ);
        for (uint32_t x = 0; x <= subdivX; ++x)
        {
            const float fx = static_cast<float>(x) / static_cast<float>(subdivX);
            m.positions.push_back({ (fx - 0.5f) * 2.f * halfSize,
                                    0.f,
                                    (fz - 0.5f) * 2.f * halfSize });
            m.normals.push_back({ 0.f, 1.f, 0.f });
            m.tangents.push_back({ 1.f, 0.f, 0.f, 1.f });
            m.uvs.push_back({ fx, fz });
            m.colors.push_back(color);
        }
    }

    const uint32_t stride = subdivX + 1;
    for (uint32_t z = 0; z < subdivZ; ++z)
    {
        for (uint32_t x = 0; x < subdivX; ++x)
        {
            const auto v0 = static_cast<uint16_t>( z      * stride + x    );
            const auto v1 = static_cast<uint16_t>( z      * stride + x + 1);
            const auto v2 = static_cast<uint16_t>((z + 1) * stride + x + 1);
            const auto v3 = static_cast<uint16_t>((z + 1) * stride + x    );

            // CCW when viewed from +Y (above).
            m.indices.push_back(v0); m.indices.push_back(v3); m.indices.push_back(v2);
            m.indices.push_back(v0); m.indices.push_back(v2); m.indices.push_back(v1);
        }
    }
    return m;
}

// ==== Torus ====
ProceduralMesh::MeshData
ProceduralMesh::Torus(float majorRadius, float minorRadius,
                      uint32_t majorSegs, uint32_t minorSegs,
                      std::array<float, 3> color)
{
    MeshData m;

    constexpr float PI = 3.14159265358979f;

    if (majorSegs < 3) majorSegs = 3;
    if (minorSegs < 3) minorSegs = 3;

    // Vertices: (majorSegs+1) rings around the axis, each with (minorSegs+1)
    // points around the tube. The +1 seam duplicates verts so UVs are correct.
    for (uint32_t i = 0; i <= majorSegs; ++i)
    {
        const float u    = static_cast<float>(i) / static_cast<float>(majorSegs);
        const float phi  = 2.f * PI * u;          // angle around the +Y axis
        const float cosP = cosf(phi), sinP = sinf(phi);

        for (uint32_t j = 0; j <= minorSegs; ++j)
        {
            const float v    = static_cast<float>(j) / static_cast<float>(minorSegs);
            const float theta = 2.f * PI * v;     // angle around the tube
            const float cosT = cosf(theta), sinT = sinf(theta);

            // Ring-centre direction in the XZ plane.
            const float dist = majorRadius + minorRadius * cosT;
            const float x = dist * cosP;
            const float y = minorRadius * sinT;
            const float z = dist * sinP;

            // Normal points from the tube centre outward to the surface.
            const float nx = cosT * cosP;
            const float ny = sinT;
            const float nz = cosT * sinP;

            // Tangent = dP/dphi (around the ring), normalised.
            const float tx = -sinP;
            const float tz =  cosP;

            m.positions.push_back({ x, y, z });
            m.normals.push_back({ nx, ny, nz });
            m.tangents.push_back({ tx, 0.f, tz, 1.f });
            m.uvs.push_back({ u, v });
            m.colors.push_back(color);
        }
    }

    const uint32_t stride = minorSegs + 1;
    for (uint32_t i = 0; i < majorSegs; ++i)
    {
        for (uint32_t j = 0; j < minorSegs; ++j)
        {
            const auto v0 = static_cast<uint16_t>( i      * stride + j    );
            const auto v1 = static_cast<uint16_t>((i + 1) * stride + j    );
            const auto v2 = static_cast<uint16_t>((i + 1) * stride + j + 1);
            const auto v3 = static_cast<uint16_t>( i      * stride + j + 1);

            // Winding matches Sphere: geometric normal parallel to the
            // outward vertex normal -> front-facing without double-sided.
            m.indices.push_back(v0); m.indices.push_back(v2); m.indices.push_back(v1);
            m.indices.push_back(v0); m.indices.push_back(v3); m.indices.push_back(v2);
        }
    }
    return m;
}
