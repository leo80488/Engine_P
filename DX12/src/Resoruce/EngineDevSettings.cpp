#include "Resource/EngineDevSettings.h"
#include "Settings/SettingsRows.h"      // the developer rows: load / save walk them
#include "Settings/SettingsRowJson.h"   // a row's JSON read / write / sanitize
#include "Settings/SettingsStore.h"     // the boot load records the Project / CommandLine layers
#include "Settings/SetFlags.h"          // -set: on engine.json's paths
#include "Settings/LegacyFlags.h"       // the rows' old spellings (the A/B pins)
#include "Harness/ExitCodes.h"          // a refused old spelling refuses the run, as -set: does

#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/RenderPass/PTRtxdiCore.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Resource/AssetFS.h"
#include "System/HostPolicy.h"          // Sys::IsEditorHost - never #ifdef WITH_EDITOR
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <fstream>

namespace Resource
{
    namespace
    {
        // Reserved for the first key whose MEANING changes: empty, but present and called,
        // so a migration has a function to go in rather than a version number nobody reads.
        void MigrateEngineDevConfig(int fromVersion, nlohmann::json& /*j*/)
        {
            (void)fromVersion;
        }

        // One key, or @p fallback when it is absent OR holds the wrong type. nlohmann's
        // value() throws on a type mismatch ("gpuCulling": "yes"), which would abort the whole
        // document; here one bad key costs only itself. A well-typed value reads exactly as
        // value() would: both go through get<T>().
        template <class T>
        T ValueOr(const nlohmann::json& obj, const char* key, const T& fallback)
        {
            const auto it = obj.find(key);
            if (it == obj.end()) return fallback;
            try
            {
                return it->template get<T>();
            }
            catch (const nlohmann::json::exception&)
            {
                LOG_WARNING("EngineDevSettings: '%s' holds a %s, not the expected type - keeping %s",
                            key, it->type_name(), "the previous value");
                return fallback;
            }
        }

        // Where a WRITE lands: -engine_settings, else the writable root, which editor hosts
        // seed from the CWD (main.cpp) - the project directory beside game.json.
        std::string WritePath()
        {
            const auto& cmd=Sys::CommandLine::Get();
            if(cmd.Has("-engine_settings")) return cmd.Get("-engine_settings",kEngineDevConfigName);
            return Sys::WritableDataDir() + "/" + kEngineDevConfigName;
        }
    }

    int PeekEngineDevRayReconstructionLoad()
    {
        const auto& cmd=Sys::CommandLine::Get();
        const bool explicitPath=cmd.Has("-engine_settings");
        const std::string readName=cmd.Get("-engine_settings",kEngineDevConfigName);
        std::string src;
        bool found = explicitPath ? Sys::ReadFileText(readName,src) : AssetFS::Get().ReadFileText(readName,src);
        if (!found && !explicitPath) found = Sys::ReadFileText(Sys::ExeDir() + "/" + kEngineDevConfigName, src);
        int load = 0;
        if (found)
        {
            try
            {
                const nlohmann::json j = nlohmann::json::parse(src, nullptr, true, true);
                if (j.contains("raytracing") && j["raytracing"].is_object())
                    load = std::clamp(j["raytracing"].value("rayReconstructionLoad", 0), 0, 2);
            }
            catch (const std::exception&) {}
        }
        // A -set: wins here as it does at the load (Coerce kept it in 0..2).
        for (const auto& f : Settings::ParseSetFlags(nullptr).developer)
            if (f.row->mi == &EngineDevSettings::rayReconstructionLoad) load = f.value.i;
        return load;
    }

    // One Int or Float row, @p section.@p key, from the file LoadEngineDevSettings reads, or its -set:,
    // without logging: for an owner that sizes something itself, before or apart from the boot load.
    // The row's default when the file is absent or lacks the key; the file's value clamped to
    // [@p lo, @p hi] (a -set: was clamped by Coerce).
    template <class T>
    static T PeekEngineDevValue(const char* section, const char* key, T EngineDevSettings::* member, T lo, T hi)
    {
        const auto& cmd=Sys::CommandLine::Get();
        const bool explicitPath=cmd.Has("-engine_settings");
        const std::string readName=cmd.Get("-engine_settings",kEngineDevConfigName);
        std::string src;
        bool found = explicitPath ? Sys::ReadFileText(readName,src) : AssetFS::Get().ReadFileText(readName,src);
        if (!found && !explicitPath) found = Sys::ReadFileText(Sys::ExeDir() + "/" + kEngineDevConfigName, src);
        T value = EngineDevSettings{}.*member;
        if (found)
        {
            try
            {
                const nlohmann::json j = nlohmann::json::parse(src, nullptr, true, true);
                if (j.contains(section) && j[section].is_object())
                    value = std::clamp(j[section].value(key, value), lo, hi);
            }
            catch (const std::exception&) {}
        }
        // A -set: wins, as at the load.
        for (const auto& f : Settings::ParseSetFlags(nullptr).developer)
        {
            if constexpr (std::is_same_v<T, float>)
            {
                if (f.row->mf == member) value = f.value.f;
            }
            else if (f.row->mi == member)
                value = f.value.i;
        }
        return value;
    }

    int PeekEngineDevStreamRingMiB()
    {
        return PeekEngineDevValue("upload", "streamRingMiB", &EngineDevSettings::streamRingMiB, 1, 512);
    }

    int PeekEngineDevSkinVertexBudget()
    {
        return PeekEngineDevValue("skinning", "vertexBudget", &EngineDevSettings::skinVertexBudget, 0, 33554432);
    }

    PhysicsDevSettings PeekEngineDevPhysics()
    {
        PhysicsDevSettings out;
        out.gravity         = PeekEngineDevValue("physics", "gravity", &EngineDevSettings::physicsGravity, -200.f, 200.f);
        out.timeBeforeSleep = PeekEngineDevValue("physics", "timeBeforeSleep", &EngineDevSettings::physicsTimeBeforeSleep, 0.f, 60.f);
        return out;
    }

    static bool ParseEngineDevSettings(const std::string& src, const std::string& readName, EngineDevSettings& out,
                                       std::vector<uint8_t>* rowFromFile);

    bool LoadEngineDevSettings(EngineDevSettings& out, EngineDevLoadInfo* info)
    {
        std::string src;
        const auto& cmd=Sys::CommandLine::Get();
        const bool explicitPath=cmd.Has("-engine_settings");
        const std::string readName=cmd.Get("-engine_settings",kEngineDevConfigName);
        std::string loadedAs = readName;

        // 1) AssetFS: the sealed copy inside game.ipak, else a CWD-relative disk
        //    read (which in the editor IS the project directory).
        bool found = explicitPath ? Sys::ReadFileText(readName,src) : AssetFS::Get().ReadFileText(readName,src);

        // 2) The ExeDir fallback, not redundant: AssetFS's disk path is CWD-relative, and a
        //    --no-pack build launched from a shortcut has a CWD that is not the exe
        //    directory. Without it the file is silently absent and every authored toggle
        //    reverts to its C++ default.
        if (!found && !explicitPath)
        {
            const std::string exeSide = Sys::ExeDir() + "/" + kEngineDevConfigName;
            found = Sys::ReadFileText(exeSide, src);
            if (found)
            {
                LOG_INFO("EngineDevSettings: loaded '%s' (ExeDir fallback — no pak entry)",
                         exeSide.c_str());
                loadedAs = exeSide;
            }
        }
        if (info)
        {
            info->found    = found;
            info->parsed   = false;
            info->loadedAs = loadedAs;
            info->rowFromFile.assign(Settings::DevRows().size(), 0);
        }

        if (!found)
        {
            // Not an error (unless the path was explicit): without engine.json every field
            // keeps its C++ default. Logged at INFO so "why is compaction off" has an answer.
            if(explicitPath) LOG_ERROR("EngineDevSettings: cannot read explicit config %s",readName.c_str());
            LOG_INFO("EngineDevSettings: no '%s' found — using engine defaults",
                     readName.c_str());
            return false;
        }
        const bool parsed = ParseEngineDevSettings(src, readName, out, info ? &info->rowFromFile : nullptr);
        if (info) info->parsed = parsed;
        return parsed;
    }

    bool LoadEngineDevSettingsFile(const std::string& path, EngineDevSettings& out)
    {
        std::string src;
        if (!Sys::ReadFileText(path, src))
        {
            LOG_ERROR("EngineDevSettings: cannot read '%s'", path.c_str());
            return false;
        }
        return ParseEngineDevSettings(src, path, out, nullptr);
    }

    bool LoadBootEngineDevSettings(EngineDevSettings& out)
    {
        EngineDevLoadInfo info;
        const bool parsed = LoadEngineDevSettings(out, &info);
        // The Project layer gets what engine.json supplied. A key the file did
        // not hold stays in the Default layer - `where` must not credit the file
        // with a compiled-in value - and a file that did not parse supplied
        // nothing. The sanitize only moves values the file supplied.
        Settings::DevSettingsStore& store = Settings::DevSettingsStore::Get();
        if (parsed)
            store.WriteRows(out, Settings::Layer::Project, info.loadedAs, info.rowFromFile);
        // The rows' old spellings (the A/B pins folded into the rows they pinned;
        // Settings/LegacyFlags.h, developer style): read as -set: reads a value, marked read,
        // into the CommandLine layer under the flag's name - before -set:, which wins. A
        // number's pin without a value, malformed or NaN is ignored with a warning; a name
        // without a value, or one the row does not offer, refuses the run, as a bad -set: does.
        std::vector<std::string> refused;
        for (const Settings::DevRow& r : Settings::DevRows())
        {
            if (!r.spec.legacyFlag && !r.spec.legacyRead) continue;
            Settings::Value v;
            std::string writer, why;
            const Settings::LegacyResult got = r.spec.legacyRead
                ? r.spec.legacyRead(v, writer, &why)
                : Settings::ReadLegacy(r, Settings::LegacyStyle::Developer, v, writer, &why);
            if (got == Settings::LegacyResult::Refused) refused.push_back(why);
            if (got == Settings::LegacyResult::Ignored && !why.empty()) LOG_WARNING("%s", why.c_str());
            if (got != Settings::LegacyResult::Taken) continue;
            Settings::PutValue(r, v, out);
            store.Write(r, Settings::Layer::CommandLine, v, writer);
        }
        if (!refused.empty())
        {
            for (const std::string& e : refused)
                LOG_ERROR("%s", e.c_str());
            LOG_ERROR("%zu old flag(s) on engine.json's rows cannot be applied. Refusing to run.", refused.size());
            PostQuitMessage(Harness::PreconditionFail);
        }
        // -set: on engine.json's paths. settings.json's boot load parsed the
        // whole command line first and refused the run over a bad override.
        for (const auto& f : Settings::ParseSetFlags(nullptr).developer)
        {
            Settings::PutValue(*f.row, f.value, out);
            store.Write(*f.row, Settings::Layer::CommandLine, f.value, f.flag);
        }
        return parsed;
    }

    // The document's bytes, already read, into `out`: the version check and
    // migration, every key (ValueOr: absent or mistyped keeps the value `out`
    // has), then the clamps and name canonicalisation. @p rowFromFile (when
    // given, indexed like Settings::DevRows()) says which rows the file held.
    static bool ParseEngineDevSettings(const std::string& src, const std::string& readName, EngineDevSettings& out,
                                       std::vector<uint8_t>* rowFromFile)
    {
        const auto rows = Settings::DevRows();
        if (rowFromFile) rowFromFile->assign(rows.size(), 0);
        try
        {
            nlohmann::json j = nlohmann::json::parse(src, nullptr, /*allow_exceptions*/true,
                                                     /*ignore_comments*/true);

            const int fileVersion = ValueOr(j, "version", 1);
            if (fileVersion > kEngineDevConfigVersion)
            {
                // Loading anyway is safe: an absent key keeps its value and a key this build does
                // not know is never looked at. The EXPECTED case for a sealed document: an older exe
                // opening a pak built by a newer engine.
                LOG_WARNING("EngineDevSettings: '%s' was written by a newer build "
                            "(version %d > %d) — loading what this build understands",
                            readName.c_str(), fileVersion, kEngineDevConfigVersion);
            }
            else if (fileVersion < kEngineDevConfigVersion)
            {
                LOG_INFO("EngineDevSettings: migrating '%s' from version %d to %d",
                         readName.c_str(), fileVersion, kEngineDevConfigVersion);
                MigrateEngineDevConfig(fileVersion, j);
            }

            // ---- The developer rows (Settings/Rows/Developer.inl) ----
            // A row whose section is absent keeps its current value; a present section reads
            // the row's key, and an absent or mistyped key keeps the current value too
            // (Settings::ReadRow).
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto sec = j.find(rows[i].section);
                if (sec != j.end() && sec->is_object())
                {
                    const bool got = Settings::ReadRow(rows[i], *sec, out, "EngineDevSettings");
                    if (rowFromFile) (*rowFromFile)[i] = got ? 1 : 0;
                }
            }
            // raytracing.ptRealtime is not a row (the render path picks real-time PT): warn a
            // file that still sets it.
            if (const auto t = j.find("raytracing"); t != j.end() && t->is_object())
                if (const auto rt = t->find("ptRealtime"); rt != t->end() && rt->is_boolean() && rt->get<bool>())
                    LOG_WARNING("EngineDevSettings: raytracing.ptRealtime is retired - real-time path tracing is the "
                                "render path 'Path Trace (Real-time)' (settings.json features.renderPath = "
                                "PathTrace_Realtime, -gipath=pathtrace_realtime); ignored");
        }
        catch (const std::exception& e)
        {
            // Never fatal. A corrupt developer config must degrade to defaults,
            // not stop the engine booting.
            LOG_ERROR("EngineDevSettings: failed to parse '%s' (%s) — using engine defaults",
                      readName.c_str(), e.what());
            return false;
        }

        // Clamp so a hand-edited file cannot wedge the engine: each row's own
        // sanitize (its `san` in Developer.inl), then the name rules, which
        // make a name canonical and warn about one they do not know.
        for (const Settings::DevRow& r : Settings::DevRows())
            Settings::SanitizeRow(r, out);
        // The SHaRC content mode: parity | multi; anything else loads as parity, said once
        // (the CLI's -gi_sharc_mode refuses instead).
        if (out.restirGiSharcMode != "parity" && out.restirGiSharcMode != "multi")
        {
            LOG_WARNING("EngineDevSettings: raytracing.restirGiSharcMode='%s' is not parity|multi; using parity",
                        out.restirGiSharcMode.c_str());
            out.restirGiSharcMode = "parity";
        }
        // Denoiser names: canonical lower case; anything else loads as auto, said once.
        const auto denoiserName = [](std::string& value, const char* key)
        {
            ReSTIR::Denoiser d;
            if (ReSTIR::ParseDenoiser(value, d)) { value = ReSTIR::DenoiserName(d); return; }
            LOG_WARNING("EngineDevSettings: raytracing.%s='%s' is not auto|relax|reblur|none; using auto",
                        key, value.c_str());
            value = "auto";
        };
        denoiserName(out.restirDiDenoiser, "restirDiDenoiser");
        denoiserName(out.restirGiDenoiser, "restirGiDenoiser");
        {
            PTRtxdi::DirectLight d;
            if (!PTRtxdi::ParseDirectLight(out.ptDirectLight.c_str(), d))
            {
                LOG_WARNING("EngineDevSettings: raytracing.ptDirectLight='%s' is not auto|nee|restir_di|unified; using auto",
                            out.ptDirectLight.c_str());
                out.ptDirectLight = "auto";
            }
        }
        // Backend names: canonical lower case; an unknown or illegal one is the
        // stage's default request, said once here rather than refused (the CLI
        // refuses).
        const auto backendName = [](std::string& value, ReSTIR::Stage stage, const char* key)
        {
            ReSTIR::Backend b;
            if (ReSTIR::ParseBackend(stage, value, b)) { value = ReSTIR::BackendName(b); return; }
            LOG_WARNING("EngineDevSettings: raytracing.%s='%s' is not %s; using %s",
                     key, value.c_str(), ReSTIR::LegalBackends(stage).c_str(),
                     ReSTIR::BackendName(ReSTIR::DefaultRequest(stage)));
            value = ReSTIR::BackendName(ReSTIR::DefaultRequest(stage));
        };
        backendName(out.restirBackendDI, ReSTIR::Stage::DI, "restirBackendDI");
        backendName(out.restirBackendGI, ReSTIR::Stage::GI, "restirBackendGI");
        backendName(out.restirBackendPT, ReSTIR::Stage::PT, "restirBackendPT");
        {
            ReSTIR::Compare c;
            if (ReSTIR::ParseCompare(out.restirCompare, c)) out.restirCompare = ReSTIR::CompareName(c);
            else
            {
                LOG_WARNING("EngineDevSettings: raytracing.restirCompare='%s' is not off|di|gi; using off", out.restirCompare.c_str());
                out.restirCompare = "off";
            }
        }

        LOG_INFO("EngineDevSettings: loaded '%s'", readName.c_str());
        return true;
    }

    std::string SerializeEngineDevSettings(const EngineDevSettings& s)
    {
        nlohmann::json j;
        j["_comment"] = "Engine/developer configuration. NOT player options — those live in "
                        "settings.json. Sealed into game.ipak by tools/package_game.py; the "
                        "shipped runtime reads it and never writes it. Edit via the editor's "
                        "Graphics Settings -> Developer node, or by hand.";
        j["version"]  = kEngineDevConfigVersion;

        // Every developer row at its "section.key" (Developer.inl). The
        // objects' keys are sorted (nlohmann::json is a std::map), so the order
        // the rows are written in never reaches the file.
        for (const Settings::DevRow& r : Settings::DevRows())
            j[r.section][r.key] = Settings::RowJson(r, s);
        return j.dump(2) + "\n";
    }

    bool SaveEngineDevSettings(const EngineDevSettings& s)
    {
        // Sealed content a shipped build must never rewrite. A runtime check, not an
        // #ifdef: EngineCore is compiled once for both exes and cannot see WITH_EDITOR.
        if (!Sys::IsEditorHost())
        {
            LOG_WARNING("EngineDevSettings: refusing to write '%s' from a game host "
                        "— it is sealed content, not player options",
                        kEngineDevConfigName);
            return false;
        }
        const std::string text = SerializeEngineDevSettings(s);
        const std::string full = WritePath();
        std::ofstream f(Sys::U8Path(full), std::ios::binary | std::ios::trunc);
        if (!f)
        {
            LOG_ERROR("EngineDevSettings: cannot open '%s' for writing", full.c_str());
            return false;
        }
        f << text;
        LOG_SUCCESS("EngineDevSettings: saved '%s'", full.c_str());
        return true;
    }

    bool EditEngineDevSettingsFile(const std::function<void(EngineDevSettings&)>& edit, const char* writer,
                                   bool requireFile)
    {
        EngineDevSettings s;
        if (!LoadEngineDevSettings(s) && requireFile)
            return false;
        edit(s);
        if (!SaveEngineDevSettings(s))
            return false;
        // Save writes every row, so the file holds each one now.
        Settings::DevSettingsStore::Get().WriteRows(s, Settings::Layer::Project, writer);
        return true;
    }

    bool ReloadEngineDevSettings(Renderer& r, const char* writer, bool requireFile)
    {
        EngineDevSettings s;
        EngineDevLoadInfo info;
        const bool parsed = LoadEngineDevSettings(s, &info);
        if (!parsed && requireFile)
            return false;
        s.ApplyTo(r);
        Settings::DevSettingsStore& st = Settings::DevSettingsStore::Get();
        const auto rows = Settings::DevRows();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (!rows[i].spec.apply)
                continue;   // read by its owner at boot: the reload changed nothing that runs
            for (const Settings::Layer l : { Settings::Layer::Scalability, Settings::Layer::Player, Settings::Layer::Scene,
                                             Settings::Layer::CommandLine, Settings::Layer::Runtime })
                st.Clear(rows[i], l, writer);
            if (parsed && info.rowFromFile[i])
                st.Write(rows[i], Settings::Layer::Project, Settings::ValueOf(rows[i], s), writer);
            else
                st.Clear(rows[i], Settings::Layer::Project, writer);
        }
        return parsed;
    }

    // ==== Apply / capture: the developer rows ====
    // Each row's hook (Settings/Rows/Developer.inl), in the .inl's order.
    void EngineDevSettings::ApplyTo(Renderer& r) const
    {
        Settings::DevApplyCtx c{ r };
        Settings::ApplyDevRows(*this, c);

        LOG_INFO("EngineDevSettings: RTGI distance=%.3f tier=%u",r.GetRestirGIMaxDistance(),r.GetRestirGITier());
        LOG_INFO("EngineDevSettings: ReSTIR backends DI=%s GI=%s PT=%s compare=%s",
            ReSTIR::BackendName(r.GetRestirBackend(ReSTIR::Stage::DI)),ReSTIR::BackendName(r.GetRestirBackend(ReSTIR::Stage::GI)),
            ReSTIR::BackendName(r.GetRestirBackend(ReSTIR::Stage::PT)),ReSTIR::CompareName(r.GetRestirCompare()));
        LOG_INFO("EngineDevSettings: ReSTIR DI reuse=%u distance=%.3f passes=%u radius=%u k=%u mCap=%u ageCap=%u GI reuse=%u radius=%u ageCap=%u",
            r.GetRestirDIReuse()?1u:0u,r.GetDIRayTMax(),r.GetRestirDISpatialPasses(),r.GetRestirDISpatialRadius(),r.GetRestirDIK(),r.GetRestirDIMCap(),r.GetRestirDIAgeCap(),
            r.GetRestirGIReuse()?1u:0u,r.GetRestirGISpatialRadius(),r.GetRestirGIAgeCap());
        LOG_INFO("EngineDevSettings: PT maxBounces=%u reference=%d",r.GetPTMaxBounces(),r.GetPTReferenceArm());

        // Echo what landed: a config that is read but never reaches the engine looks the
        // same as one never read, and this sealed document has no UI in a shipped build. One
        // line answers "did engine.json take effect".
        LOG_INFO("EngineDevSettings applied: cull(gpu=%d indirect=%d compact=%d hiz=%d anim=%d) "
                 "chainLod=%d mesh=%d lod=%.2f shadowLod=%.2f perf(prepass=%d perObjShadow=%d) "
                 "rt(rough=%.2f nrd=%d blendOcc=%d guard=%d %.1fms) surfaceCache(%d, %d/frame)",
                 gpuCulling, indirectDraw, gpuCompaction, hiZOcclusion, animCulling,
                 chainPhysicsLod, meshShading, meshletLodError, meshletShadowLodError,
                 depthPrepass, perObjectShadow,
                 rtReflectionsMaxRoughness, rtReflectionsUseNRD, rtShadowBlendOccluders,
                 rtBudgetGuard, rtBudgetMs,
                 materialSurfaceCache, materialSurfaceCachePagesPerFrame);
    }

    void EngineDevSettings::CaptureFrom(const Renderer& r)
    {
        // The rows read REQUESTED values wherever a capability demotion exists
        // (gpuCompaction, restirGiSharc, the backends): a machine that failed to create a
        // buffer must not commit that failure into a sealed file. The rows with no capture
        // keep their value: chainPhysicsLod (applied only), rayReconstructionLoad, gameFlow,
        // debug, upload.streamRingMiB and skinning.vertexBudget (read from the file by their
        // owners).
        Settings::DevCaptureCtx c{ r };
        Settings::CaptureDevRows(*this, c);
    }
}
