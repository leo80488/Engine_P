#include "Resource/VFXPrefab.h"

#include "Resource/AssetFS.h"   // pak-first read so .ivfx prefabs load in packed builds
#include "Resource/MeshLibIdentity.h"   // MESH sec=
#include "System/Log.h"
#include "System/PathUtil.h"    // Sys::OpenOfstream / CreateParentDirs (UTF-8, loose-disk write)

#include <DirectXMath.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace Resource
{

namespace
{
    // ==== Tokens ====

    // KV map populated from a "PARTICLE k1=v1 k2=v2 ..." line. Mirrors the
    // same per-key set ComponentSerializers writes for ParticleEmitterComponent
    // so designers can copy a line straight out of a saved .iscn into a new
    // .ivfx and have it work.
    using KV = std::unordered_map<std::string, std::string>;

    KV ParseInlineKV(const std::string& body)
    {
        KV out;
        std::istringstream iss(body);
        std::string tok;
        while (iss >> tok) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            out.emplace(tok.substr(0, eq), tok.substr(eq + 1));
        }
        return out;
    }

    int   GetI(const KV& kv, const char* k, int def) {
        auto it = kv.find(k); return (it == kv.end()) ? def : std::atoi(it->second.c_str());
    }
    float GetF(const KV& kv, const char* k, float def) {
        auto it = kv.find(k); return (it == kv.end()) ? def : static_cast<float>(std::atof(it->second.c_str()));
    }
    std::string GetS(const KV& kv, const char* k) {
        auto it = kv.find(k); return (it == kv.end()) ? std::string{} : it->second;
    }

    void ParseVec3(const KV& kv, const char* k, DirectX::XMFLOAT3& out)
    {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        sscanf_s(it->second.c_str(), "%f_%f_%f", &out.x, &out.y, &out.z);
    }
    void ParseVec4(const KV& kv, const char* k, DirectX::XMFLOAT4& out)
    {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        sscanf_s(it->second.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
    }

    // ==== Particle KV -> components ====
    // A .ivfx particle emitter is a ParticleRenderComponent (look) + a ParticleEmitterComponent
    // (standard emit behaviour), both from one KV blob.

    ParticleRenderComponent ParticleRenderFromKV(const KV& kv)
    {
        ParticleRenderComponent r;
        r.startSize      = GetF(kv, "startSize",     r.startSize);
        r.intensity      = GetF(kv, "intensity",     r.intensity);
        r.velocityStretch= GetF(kv, "velocityStretch", r.velocityStretch);
        r.emissiveFade   = GetF(kv, "emissiveFade",  r.emissiveFade);
        ParseVec3(kv, "startScale",     r.startScale);
        ParseVec4(kv, "startColor",     r.startColor);
        ParseVec4(kv, "endColor",       r.endColor);
        r.blendMode  = static_cast<ParticleBlendMode>(GetI(kv, "blendMode",
                            static_cast<int>(r.blendMode)));
        r.visualMode = static_cast<ParticleVisualMode>(GetI(kv, "visualMode",
                            static_cast<int>(r.visualMode)));
        // 3 = VelocityStretched, the one sparks/streaks almost always want, so it must be reachable from
        // an .ivfx.
        r.orientation = static_cast<ParticleOrientation>(GetI(kv, "orientation",
                            static_cast<int>(r.orientation)));
        // texturePath stays a path: ParticleSystem resolves bindless on first tick. .ivfx never persists
        // bindless indices (process-local).
        r.texturePath        = GetS(kv, "texturePath");
        r.textureBindlessIdx = -1;
        r.textureGpuHandle   = 0;
        return r;
    }

    ParticleEmitterComponent ParticleFromKV(const KV& kv)
    {
        ParticleEmitterComponent p;
        p.enabled        = GetI(kv, "enabled", 1) != 0;
        p.spawnRate      = GetF(kv, "spawnRate",     p.spawnRate);
        p.maxParticles   = GetI(kv, "maxParticles",  p.maxParticles);
        p.startLifetime  = GetF(kv, "startLifetime", p.startLifetime);
        p.burstCount     = GetI(kv, "burstCount",    p.burstCount);
        p.sizeVariance   = GetF(kv, "sizeVariance",  p.sizeVariance);
        p.lifeVariance   = GetF(kv, "lifeVariance",  p.lifeVariance);
        p.drag           = GetF(kv, "drag",          p.drag);

        ParseVec3(kv, "velMin",         p.velocityMin);
        ParseVec3(kv, "velMax",         p.velocityMax);
        ParseVec3(kv, "gravity",        p.gravity);
        ParseVec3(kv, "coneDir",        p.coneDirection);
        ParseVec3(kv, "boxHalfExtents", p.boxHalfExtents);
        ParseVec3(kv, "circleNormal",   p.circleNormal);

        p.shape      = static_cast<ParticleShape>(GetI(kv, "shape",
                            static_cast<int>(p.shape)));

        p.sphereRadius       = GetF(kv, "sphereRadius",  p.sphereRadius);
        p.sphereSpawnOnShell = GetI(kv, "sphereShell",   p.sphereSpawnOnShell ? 1 : 0) != 0;
        p.coneHalfAngle      = GetF(kv, "coneHalfAngle", p.coneHalfAngle);
        p.coneLength         = GetF(kv, "coneLength",    p.coneLength);
        p.circleRadius       = GetF(kv, "circleRadius",  p.circleRadius);

        // ---- Trail shape (weapon-swing sparks) ----
        // `swing=1` is the readable alias for shape=6 (ParticleShape::Trail); the numeric form also works.
        // Attach this emitter to the same weapon socket as the TRAIL block and match bladeTip to the
        // ribbon's last local point, and the sparks land on the edge the ribbon draws.
        if (GetI(kv, "swing", 0) != 0) p.shape = ParticleShape::Trail;
        // Both the .ivfx short form and the .iscn field name are accepted: this
        // loader silently ignores unknown keys, so a near-miss spelling would
        // otherwise fall back to the default with no diagnostic at all. Second
        // lookup only overwrites when that key is actually present.
        ParseVec3(kv, "bladeTip",      p.bladeTipLocal);
        ParseVec3(kv, "bladeTipLocal", p.bladeTipLocal);
        p.sparksPerMeter  = GetF(kv, "sparksPerMeter",   p.sparksPerMeter);
        p.minSwingSpeed   = GetF(kv, "minSwingSpeed",    p.minSwingSpeed);
        p.inheritVelocity = GetF(kv, "inherit",          p.inheritVelocity);
        p.inheritVelocity = GetF(kv, "inheritVelocity",  p.inheritVelocity);

        // meshSourceEntity is not persisted (cross-instance Entity IDs don't
        // survive prefab cloning).
        p.meshSourceEntity   = 0;

        return p;
    }

    // ==== Trail KV -> component (mirrors TrailComponent serialized fields) ====

    TrailComponent TrailFromKV(const KV& kv)
    {
        TrailComponent t;
        t.width             = GetF(kv, "width",   t.width);
        t.maxAge            = GetF(kv, "maxAge",  t.maxAge);
        // The .ivfx key is "minDist" (not "minSampleDistance"), unlike the scene serializer; existing
        // files depend on it.
        t.minSampleDistance = GetF(kv, "minDist", t.minSampleDistance);
        ParseVec4(kv, "startColor", t.startColor);
        ParseVec4(kv, "endColor",   t.endColor);
        t.enabled           = GetI(kv, "enabled", 1) != 0;

        // ---- Multi-point weapon ribbon (all optional -> a single-point trail) ----
        t.pointCount = static_cast<uint32_t>(GetI(kv, "pointCount", 1));
        if (t.pointCount < 1) t.pointCount = 1;
        if (t.pointCount > 4) t.pointCount = 4;
        ParseVec3(kv, "p0", t.localPoints[0]);
        ParseVec3(kv, "p1", t.localPoints[1]);
        ParseVec3(kv, "p2", t.localPoints[2]);
        ParseVec3(kv, "p3", t.localPoints[3]);
        // Paths must contain no spaces: .ivfx KV tokens are whitespace-split.
        t.texturePath        = GetS(kv, "texture");
        t.customShaderPath   = GetS(kv, "shader");
        t.textureBindlessIdx = -1;   // process-local, resolved on first tick
        t.uvMode             = static_cast<uint32_t>(GetI(kv, "uvMode", 1));
        t.uvTileLength       = GetF(kv, "uvTile", 0.0f);
        t.blendMode          = static_cast<uint32_t>(GetI(kv, "blend", 0));
        t.hdrIntensity       = GetF(kv, "hdri", 1.0f);
        t.excludeFromTAA     = GetI(kv, "taa", 0) != 0;
        t.subdivisions       = static_cast<uint32_t>(GetI(kv, "subdiv", 4));
        if (t.subdivisions < 1) t.subdivisions = 1;
        if (t.subdivisions > 4) t.subdivisions = 4;
        t.subSampleAngleDeg  = GetF(kv, "subAngle", 15.0f);
        return t;
    }

    // ==== Beam KV -> spec (control points come from separate BP lines) ====

    void BeamFromKV(const KV& kv, BeamVFXSpec& b)
    {
        b.beam.globalRadiusScale = GetF(kv, "radiusScale", b.beam.globalRadiusScale);
        b.beam.wobbleAmplitude   = GetF(kv, "wobbleAmp",   b.beam.wobbleAmplitude);
        b.beam.wobbleSpeed       = GetF(kv, "wobbleSpeed", b.beam.wobbleSpeed);
        std::string sh = GetS(kv, "shader");
        if (!sh.empty()) b.shaderPath = sh;
        std::string blend = GetS(kv, "blend");
        if (!blend.empty()) b.additive = (blend != "opaque" && blend != "core");
    }

    // ==== Decal / Tracer / Afterimage / Mesh KV -> spec ====

    void DecalFromKV(const KV& kv, DecalVFXSpec& d)
    {
        std::string m = GetS(kv, "material");
        if (m.empty()) m = GetS(kv, "materialName");
        d.materialName    = m;
        d.sizeX           = GetF(kv, "sizeX", d.sizeX);
        d.sizeY           = GetF(kv, "sizeY", d.sizeY);
        d.depth           = GetF(kv, "depth", d.depth);
        d.lifetime        = GetF(kv, "life",  d.lifetime);
        d.fadeOutDuration = GetF(kv, "fade",  d.fadeOutDuration);
        d.rollZ           = GetF(kv, "roll",  d.rollZ);
        ParseVec4(kv, "tint", d.tintOverride);
    }

    void TracerFromKV(const KV& kv, TracerVFXSpec& t)
    {
        ParseVec4(kv, "color", t.color);
        t.width    = GetF(kv, "width", t.width);
        t.lifetime = GetF(kv, "life",  t.lifetime);
        t.length   = GetF(kv, "len",   t.length);
        ParseVec3(kv, "dir", t.direction);
        const int noise = GetI(kv, "noise", -1);
        if (noise >= 0) t.noiseTexBindless = static_cast<uint32_t>(noise);
        // Path form (usable from disk; the raw `noise=` index is process-local).
        t.noiseTexPath = GetS(kv, "noiseTex");
    }

    void AfterimageFromKV(const KV& kv, AfterimageVFXSpec& a)
    {
        a.lifetime = GetF(kv, "life", a.lifetime);
        ParseVec4(kv, "color", a.color);
    }

    void MeshFromKV(const KV& kv, MeshVFXSpec& m)
    {
        m.meshPath      = GetS(kv, "mesh");
        m.materialPath  = GetS(kv, "material");
        if (const std::string sec = GetS(kv, "sec"); !sec.empty())
        {
            uint64_t k = 0;
            m.sectionKey = Resource::MeshKeyFromHex(sec, k) ? k : 0;
        }
        m.spinDegPerSec = GetF(kv, "spinDegPerSec", m.spinDegPerSec);
        ParseVec3(kv, "spinAxis", m.spinAxis);
        m.castShadow    = GetI(kv, "castShadow", 0) != 0;
    }

    // ==== Type token -> enum (case-insensitive) ====

    bool VFXTypeFromName(std::string s, VFXEmitterType& out)
    {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if      (s == "PARTICLE")   out = VFXEmitterType::Particle;
        else if (s == "TRAIL")      out = VFXEmitterType::Trail;
        else if (s == "BEAM")       out = VFXEmitterType::Beam;
        else if (s == "DECAL")      out = VFXEmitterType::Decal;
        else if (s == "TRACER")     out = VFXEmitterType::Tracer;
        else if (s == "AFTERIMAGE") out = VFXEmitterType::Afterimage;
        else if (s == "MESH")       out = VFXEmitterType::Mesh;
        else return false;
        return true;
    }

    // ==== Trim whitespace + drop '#' comments ====

    std::string Sanitize(const std::string& raw)
    {
        std::string s = raw;
        if (auto h = s.find('#'); h != std::string::npos) s.resize(h);
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last  = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }
}

void ParseVFXPrefabText(const std::string& text, VFXPrefab& out)
{
    out.emitters.clear();
    out.defaultDuration = 2.0f;

    std::istringstream f(text);

    int    version       = -1;
    bool   inEmitter     = false;
    VFXEmitterSpec cur;

    DirectX::XMFLOAT3 curEulerDeg{ 0, 0, 0 };
    bool eulerSeen = false;

    auto commitEulerIfNeeded = [&]() {
        if (!eulerSeen) return;
        using namespace DirectX;
        const float kDeg2Rad = XM_PI / 180.f;
        XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            curEulerDeg.x * kDeg2Rad,
            curEulerDeg.y * kDeg2Rad,
            curEulerDeg.z * kDeg2Rad);
        XMStoreFloat4(&cur.localTransform.rotation, q);
        eulerSeen = false;
    };

    auto flushEmitter = [&]() {
        if (!inEmitter) return;
        commitEulerIfNeeded();
        out.emitters.push_back(std::move(cur));
        cur = VFXEmitterSpec{};
        inEmitter = false;
    };

    std::string raw;
    while (std::getline(f, raw))
    {
        const std::string line = Sanitize(raw);
        if (line.empty()) continue;

        // Split off the first whitespace-delimited token as the directive.
        const auto sp = line.find_first_of(" \t");
        const std::string head = (sp == std::string::npos) ? line : line.substr(0, sp);
        const std::string body = (sp == std::string::npos) ? std::string{} : line.substr(sp + 1);

        if (head == "VFX") {
            version = std::atoi(body.c_str());
            if (version != 1) {
                LOG_WARNING("VFXPrefab: unsupported version %d (expected 1)", version);
                // Do not abort: keep parsing in case future versions stay wire-compatible. Worst case the file is
                // empty.
            }
        }
        else if (head == "DURATION") {
            out.defaultDuration = static_cast<float>(std::atof(body.c_str()));
        }
        else if (head == "EMITTER") {
            flushEmitter();
            inEmitter = true;
            cur = VFXEmitterSpec{};
            curEulerDeg = { 0, 0, 0 };
            eulerSeen   = false;
            // Optional type token directly on the EMITTER line ("EMITTER TRAIL").
            if (!body.empty()) {
                VFXEmitterType t;
                if (VFXTypeFromName(body, t)) cur.type = t;
            }
        }
        else if (head == "TYPE" && inEmitter) {
            VFXEmitterType t;
            if (VFXTypeFromName(body, t)) cur.type = t;
            else LOG_WARNING("VFXPrefab: unknown emitter TYPE '%s'", body.c_str());
        }
        else if ((head == "DELAY") && inEmitter) {
            cur.startDelay = static_cast<float>(std::atof(body.c_str()));
        }
        else if ((head == "DUR" || head == "LIFE") && inEmitter) {
            cur.durationOverride = static_cast<float>(std::atof(body.c_str()));
        }
        else if (head == "P" && inEmitter) {
            sscanf_s(body.c_str(), "%f %f %f",
                     &cur.localTransform.translation.x,
                     &cur.localTransform.translation.y,
                     &cur.localTransform.translation.z);
        }
        else if (head == "R" && inEmitter) {
            sscanf_s(body.c_str(), "%f %f %f",
                     &curEulerDeg.x, &curEulerDeg.y, &curEulerDeg.z);
            eulerSeen = true;
        }
        else if (head == "S" && inEmitter) {
            sscanf_s(body.c_str(), "%f %f %f",
                     &cur.localTransform.scale.x,
                     &cur.localTransform.scale.y,
                     &cur.localTransform.scale.z);
        }
        // ---- Per-type body directives ----
        // A recognized directive also sets the emitter type, so the type token is optional when the content
        // line is unambiguous (a forgiving loader).
        else if (head == "PARTICLE" && inEmitter) {
            cur.type           = VFXEmitterType::Particle;
            const KV pkv       = ParseInlineKV(body);
            cur.particleRender = ParticleRenderFromKV(pkv);
            cur.particle       = ParticleFromKV(pkv);
            cur.materialPath   = GetS(pkv, "material");   // .imat-driven look
        }
        else if (head == "TRAIL" && inEmitter) {
            cur.type  = VFXEmitterType::Trail;
            const KV tkv = ParseInlineKV(body);
            cur.trail = TrailFromKV(tkv);
            cur.materialPath = GetS(tkv, "material");     // .imat-driven look
        }
        else if (head == "BEAM" && inEmitter) {
            cur.type = VFXEmitterType::Beam;
            BeamFromKV(ParseInlineKV(body), cur.beam);
        }
        else if (head == "BP" && inEmitter) {
            // Beam control point: "x y z radius [r g b a]".
            BeamControlPoint cp;
            float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
            const int got = sscanf_s(body.c_str(), "%f %f %f %f %f %f %f %f",
                                     &cp.position.x, &cp.position.y, &cp.position.z,
                                     &cp.radius, &r, &g, &b, &a);
            if (got >= 3) {
                cp.colorTint = { r, g, b, a };
                cur.beam.beam.controlPoints.push_back(cp);
                cur.type = VFXEmitterType::Beam;
            }
        }
        else if (head == "DECAL" && inEmitter) {
            cur.type = VFXEmitterType::Decal;
            DecalFromKV(ParseInlineKV(body), cur.decal);
        }
        else if (head == "TRACER" && inEmitter) {
            cur.type = VFXEmitterType::Tracer;
            TracerFromKV(ParseInlineKV(body), cur.tracer);
        }
        else if (head == "AFTERIMAGE" && inEmitter) {
            cur.type = VFXEmitterType::Afterimage;
            AfterimageFromKV(ParseInlineKV(body), cur.afterimage);
        }
        else if (head == "MESH" && inEmitter) {
            cur.type = VFXEmitterType::Mesh;
            MeshFromKV(ParseInlineKV(body), cur.mesh);
        }
        // Unknown directives are ignored silently: designers can put extra notes in files without breaking
        // the loader.
    }
    flushEmitter();
}

bool LoadVFXPrefab(const std::string& path, VFXPrefab& out)
{
    out.emitters.clear();
    out.defaultDuration = 2.0f;

    // Read via AssetFS (game.ipak first, then loose disk): in packed builds the .ivfx lives only in
    // game.ipak, where a plain std::ifstream would make notify-driven VFX silently never spawn.
    std::string text;
    if (!::Resource::AssetFS::Get().ReadFileText(path, text)) {
        LOG_WARNING("VFXPrefab: cannot open '%s'", path.c_str());
        return false;
    }
    ParseVFXPrefabText(text, out);

    if (out.emitters.empty()) {
        LOG_WARNING("VFXPrefab: '%s' parsed but contains no EMITTER blocks",
                    path.c_str());
    }
    return true;
}

// ==== Writer: the exact inverse of the loader above ====
namespace
{
    // Compact float: "%g" drops trailing zeros and round-trips through atof for
    // authored (round-decimal) values. Precision loss does not break the
    // fixpoint: after the first save every value is already %g-quantised, so a
    // second load->save reproduces it byte-for-byte.
    std::string F(float v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%g", static_cast<double>(v));
        return b;
    }
    // Vector formatters: KV values use underscores; P/R/S lines use spaces.
    std::string U3(const DirectX::XMFLOAT3& v) { return F(v.x) + "_" + F(v.y) + "_" + F(v.z); }
    std::string U4(const DirectX::XMFLOAT4& v) { return F(v.x) + "_" + F(v.y) + "_" + F(v.z) + "_" + F(v.w); }
    std::string S3(const DirectX::XMFLOAT3& v) { return F(v.x) + " " + F(v.y) + " " + F(v.z); }

    const char* VFXTypeToToken(VFXEmitterType t)
    {
        switch (t) {
        case VFXEmitterType::Particle:   return "PARTICLE";
        case VFXEmitterType::Trail:      return "TRAIL";
        case VFXEmitterType::Beam:       return "BEAM";
        case VFXEmitterType::Decal:      return "DECAL";
        case VFXEmitterType::Tracer:     return "TRACER";
        case VFXEmitterType::Afterimage: return "AFTERIMAGE";
        case VFXEmitterType::Mesh:       return "MESH";
        default:                         return "PARTICLE";
        }
    }

    // Inverse of the loader's XMQuaternionRotationRollPitchYaw(pitchX, yawY,
    // rollZ). Derived from the DirectX RollPitchYaw matrix M = Rz(roll)*Rx(pitch)
    // *Ry(yaw) (row-vector): _32=-sin(pitch), _31/_33=cp*(sy,cy), _12/_22=cp*(sr,cr).
    // Only used when rotation is non-identity, so identity-rotation files (every
    // shipped .ivfx) never exercise it.
    DirectX::XMFLOAT3 QuatToEulerDeg(const DirectX::XMFLOAT4& q)
    {
        using namespace DirectX;
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(
            XMQuaternionNormalize(XMLoadFloat4(&q))));
        float sp = -m._32;
        sp = (sp < -1.f) ? -1.f : (sp > 1.f) ? 1.f : sp;
        const float pitch = std::asin(sp);
        float yaw, roll;
        if (std::fabs(sp) < 0.99999f) {         // not gimbal-locked
            yaw  = std::atan2(m._31, m._33);
            roll = std::atan2(m._12, m._22);
        } else {
            yaw  = std::atan2(-m._13, m._11);
            roll = 0.f;
        }
        const float r2d = 180.f / XM_PI;
        return { pitch * r2d, yaw * r2d, roll * r2d };
    }
}

std::string BuildVFXPrefabText(const VFXPrefab& prefab)
{
    std::ostringstream os;
    os << "VFX 1\n";
    os << "DURATION " << F(prefab.defaultDuration) << "\n";

    for (const VFXEmitterSpec& e : prefab.emitters)
    {
        os << "\nEMITTER " << VFXTypeToToken(e.type) << "\n";

        // Shared header: emitted only when it deviates from the LocalTransform identity, so files stay
        // readable (absent == default on reload).
        const LocalTransform& lt = e.localTransform;
        if (lt.translation.x != 0.f || lt.translation.y != 0.f || lt.translation.z != 0.f)
            os << "P " << S3(lt.translation) << "\n";
        if (!(lt.rotation.x == 0.f && lt.rotation.y == 0.f && lt.rotation.z == 0.f && lt.rotation.w == 1.f))
            os << "R " << S3(QuatToEulerDeg(lt.rotation)) << "\n";
        if (lt.scale.x != 1.f || lt.scale.y != 1.f || lt.scale.z != 1.f)
            os << "S " << S3(lt.scale) << "\n";
        if (e.startDelay != 0.f)        os << "DELAY " << F(e.startDelay) << "\n";
        if (e.durationOverride >= 0.f)  os << "DUR "   << F(e.durationOverride) << "\n";  // <0 = inherit (omit)

        switch (e.type)
        {
        case VFXEmitterType::Particle:
        {
            const ParticleRenderComponent&  r = e.particleRender;
            const ParticleEmitterComponent& p = e.particle;
            os << "PARTICLE"
               << " startSize=" << F(r.startSize)
               << " intensity=" << F(r.intensity)
               << " velocityStretch=" << F(r.velocityStretch)
               << " emissiveFade=" << F(r.emissiveFade)
               << " startScale=" << U3(r.startScale)
               << " startColor=" << U4(r.startColor)
               << " endColor=" << U4(r.endColor)
               << " blendMode=" << static_cast<int>(r.blendMode)
               << " visualMode=" << static_cast<int>(r.visualMode)
               << " orientation=" << static_cast<int>(r.orientation);
            if (!r.texturePath.empty()) os << " texturePath=" << r.texturePath;
            os << " enabled=" << (p.enabled ? 1 : 0)
               << " spawnRate=" << F(p.spawnRate)
               << " maxParticles=" << p.maxParticles
               << " startLifetime=" << F(p.startLifetime)
               << " burstCount=" << p.burstCount
               << " sizeVariance=" << F(p.sizeVariance)
               << " lifeVariance=" << F(p.lifeVariance)
               << " drag=" << F(p.drag)
               << " velMin=" << U3(p.velocityMin)
               << " velMax=" << U3(p.velocityMax)
               << " gravity=" << U3(p.gravity)
               << " coneDir=" << U3(p.coneDirection)
               << " boxHalfExtents=" << U3(p.boxHalfExtents)
               << " circleNormal=" << U3(p.circleNormal)
               << " shape=" << static_cast<int>(p.shape)   // 6 == Trail (swing); canonical, no `swing=` alias
               << " sphereRadius=" << F(p.sphereRadius)
               << " sphereShell=" << (p.sphereSpawnOnShell ? 1 : 0)
               << " coneHalfAngle=" << F(p.coneHalfAngle)
               << " coneLength=" << F(p.coneLength)
               << " circleRadius=" << F(p.circleRadius)
               << " bladeTip=" << U3(p.bladeTipLocal)
               << " sparksPerMeter=" << F(p.sparksPerMeter)
               << " minSwingSpeed=" << F(p.minSwingSpeed)
               << " inherit=" << F(p.inheritVelocity);
            // meshSourceEntity / textureBindlessIdx are process-local: never emitted.
            if (!e.materialPath.empty()) os << " material=" << e.materialPath;
            os << "\n";
            break;
        }
        case VFXEmitterType::Trail:
        {
            const TrailComponent& t = e.trail;
            os << "TRAIL"
               << " width=" << F(t.width)
               << " maxAge=" << F(t.maxAge)
               << " minDist=" << F(t.minSampleDistance)   // renamed vs .iscn
               << " startColor=" << U4(t.startColor)
               << " endColor=" << U4(t.endColor)
               << " enabled=" << (t.enabled ? 1 : 0)
               << " pointCount=" << t.pointCount
               << " p0=" << U3(t.localPoints[0])
               << " p1=" << U3(t.localPoints[1])
               << " p2=" << U3(t.localPoints[2])
               << " p3=" << U3(t.localPoints[3]);
            if (!t.texturePath.empty())      os << " texture=" << t.texturePath;     // renamed
            if (!t.customShaderPath.empty()) os << " shader="  << t.customShaderPath; // renamed
            os << " uvMode=" << t.uvMode
               << " uvTile=" << F(t.uvTileLength)          // renamed
               << " blend=" << t.blendMode                 // renamed
               << " hdri=" << F(t.hdrIntensity)            // renamed
               << " taa=" << (t.excludeFromTAA ? 1 : 0)    // renamed
               << " subdiv=" << t.subdivisions             // renamed
               << " subAngle=" << F(t.subSampleAngleDeg);  // renamed
            if (!e.materialPath.empty()) os << " material=" << e.materialPath;
            os << "\n";
            break;
        }
        case VFXEmitterType::Beam:
        {
            const BeamVFXSpec& b = e.beam;
            os << "BEAM"
               << " radiusScale=" << F(b.beam.globalRadiusScale)
               << " wobbleAmp=" << F(b.beam.wobbleAmplitude)
               << " wobbleSpeed=" << F(b.beam.wobbleSpeed)
               << " shader=" << b.shaderPath                // keep the "shaders/" prefix verbatim
               << " blend=" << (b.additive ? "additive" : "opaque")
               << "\n";
            for (const BeamControlPoint& cp : b.beam.controlPoints)
                os << "BP " << S3(cp.position) << " " << F(cp.radius)
                   << " " << F(cp.colorTint.x) << " " << F(cp.colorTint.y)
                   << " " << F(cp.colorTint.z) << " " << F(cp.colorTint.w) << "\n";
            break;
        }
        case VFXEmitterType::Decal:
        {
            const DecalVFXSpec& d = e.decal;
            os << "DECAL";
            if (!d.materialName.empty()) os << " material=" << d.materialName;  // library NAME, not a path
            os << " sizeX=" << F(d.sizeX)
               << " sizeY=" << F(d.sizeY)
               << " depth=" << F(d.depth)
               << " life=" << F(d.lifetime)
               << " fade=" << F(d.fadeOutDuration)
               << " roll=" << F(d.rollZ)
               << " tint=" << U4(d.tintOverride)
               << "\n";
            break;
        }
        case VFXEmitterType::Tracer:
        {
            const TracerVFXSpec& t = e.tracer;
            os << "TRACER"
               << " color=" << U4(t.color)
               << " width=" << F(t.width)
               << " life=" << F(t.lifetime)
               << " len=" << F(t.length)
               << " dir=" << U3(t.direction);
            if (!t.noiseTexPath.empty()) os << " noiseTex=" << t.noiseTexPath;  // raw `noise=` index is process-local
            os << "\n";
            break;
        }
        case VFXEmitterType::Afterimage:
        {
            const AfterimageVFXSpec& a = e.afterimage;
            os << "AFTERIMAGE"
               << " life=" << F(a.lifetime)
               << " color=" << U4(a.color)
               << "\n";
            break;
        }
        case VFXEmitterType::Mesh:
        {
            const MeshVFXSpec& m = e.mesh;
            os << "MESH";
            if (!m.meshPath.empty())     os << " mesh=" << m.meshPath;
            if (!m.materialPath.empty()) os << " material=" << m.materialPath;
            if (m.sectionKey != 0)       os << " sec=" << Resource::MeshKeyToHex(m.sectionKey);
            os << " spinDegPerSec=" << F(m.spinDegPerSec)
               << " spinAxis=" << U3(m.spinAxis)
               << " castShadow=" << (m.castShadow ? 1 : 0)
               << "\n";
            break;
        }
        default: break;
        }
    }

    return os.str();
}

bool SaveVFXPrefab(const std::string& path, const VFXPrefab& prefab)
{
    Sys::CreateParentDirs(path);
    std::ofstream f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f) { LOG_ERROR("VFXPrefab: cannot write '%s'", path.c_str()); return false; }
    const std::string text = BuildVFXPrefabText(prefab);
    f.write(text.c_str(), static_cast<std::streamsize>(text.size()));
    LOG_SUCCESS("VFXPrefab: saved '%s' (%zu emitters, %zu bytes)",
                path.c_str(), prefab.emitters.size(), text.size());
    return true;
}

bool VFXPrefabSelfTest()
{
    const char* files[] = { "asset/FX/weapon_swing.ivfx", "asset/FX/melee_hit.ivfx" };
    bool allOk = true;
    for (const char* path : files)
    {
        VFXPrefab v1;
        if (!LoadVFXPrefab(path, v1)) { LOG_WARNING("VFXPrefab self-test: skip '%s' (load failed)", path); continue; }
        const std::string a = BuildVFXPrefabText(v1);   // 1st generation text
        VFXPrefab v2;
        ParseVFXPrefabText(a, v2);                       // reload from our own text (no disk)
        const std::string b = BuildVFXPrefabText(v2);   // 2nd generation text
        if (a == b && v1.emitters.size() == v2.emitters.size())
            LOG_SUCCESS("VFXPrefab self-test: '%s' round-trips (%zu emitters)", path, v1.emitters.size());
        else {
            LOG_ERROR("VFXPrefab self-test: '%s' FIXPOINT FAIL (emitters %zu->%zu, text %zu vs %zu bytes)",
                      path, v1.emitters.size(), v2.emitters.size(), a.size(), b.size());
            allOk = false;
        }
    }
    return allOk;
}

} // namespace Resource
