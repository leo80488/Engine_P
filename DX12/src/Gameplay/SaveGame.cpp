#include "Gameplay/SaveGame.h"
#include "Gameplay/GameFlowState.h"
#include "Gameplay/GameState.h"
#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"      // LocalTransform + PrefabSourcePath
#include "Scripting/ScriptComponent.h"    // pre-remove before applying Script lines (its loader APPENDS slots)
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"      // patch a live component, keep its runtime state
#include "Resource/PrefabSerializer.h"    // the prefab loader a respawn goes through
#include "Resource/PrefabSpawn.h"         // SpawnPrefab: the respawn is a runtime spawn (its census)
#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"           // saves outlive the asset tree: carry identities
#include "System/Log.h"
#include "Platform/PlatformServices.h"   // the save folder
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include "System/CommandLine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string_view>

namespace Gameplay
{

static constexpr int kSaveVersion = 2;

// ==== Version migration ====
// A save outlives the build that wrote it. The reader lifts an older document one version at a time,
// in place, before any section is decoded; kMigrations[i] lifts version i + 1 to i + 2 and may
// reshape only what the next version reads. A step that changed nothing but the number is still
// listed, so the chain has no gap.
using MigrateStep = void (*)(nlohmann::json& doc);

static constexpr MigrateStep kMigrations[] = {
    [](nlohmann::json&) {},   // 1 -> 2: the sections are unchanged
};
static_assert(std::size(kMigrations) == kSaveVersion - 1, "every version needs a step to the next");

// True under -save_nomigrate (harness builds only): the chain is off and a save of another version
// than the current one is refused, as the reader did before the chain existed.
static bool MigrationOff()
{
#if WITH_HARNESS
    static const bool s_off = Sys::CommandLine::Get().Has("-save_nomigrate");
    return s_off;
#else
    return false;
#endif
}

// Parses @p slot's file and lifts it to kSaveVersion. False with @p error (the log line's text) for
// a missing file, a document that is not an object, or a version outside 1..kSaveVersion.
// @p outVersion receives the version the file was written at.
static bool ReadSave(int slot, nlohmann::json& doc, std::string& error, int& outVersion)
{
    const std::string path = SaveSlotPath(slot);
    auto f = Sys::OpenIfstream(path, std::ios::binary);
    if (!f)
    {
        error = "no save file '" + path + "'";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();

    // Non-throwing parse; comments tolerated (players/devs may annotate).
    doc = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
    if (doc.is_discarded() || !doc.is_object())
    {
        error = "'" + path + "' is not valid JSON";
        return false;
    }

    int version = doc.value("version", 0);
    outVersion = version;
    if (version < 1 || version > kSaveVersion || (MigrationOff() && version != kSaveVersion))
    {
        error = "'" + path + "' has unsupported version " + std::to_string(version) + " (engine supports 1.." +
                std::to_string(kSaveVersion) + ")";
        return false;
    }
    for (; version < kSaveVersion; ++version)
        kMigrations[version - 1](doc);
    doc["version"] = kSaveVersion;
    return true;
}

// Component tags captured AND applied per persistent entity: a curated whitelist, enforced on
// both sides (saves are hand-editable). The header says why SceneRef / PrefabRef / RigidBody are
// not on it.
static constexpr const char* kEntitySnapshotTags[] = {
    "Tag", "Script", "Collider", "LightData", "Visibility", "Persistent", "Attributes",
    // Without this a save taken mid-cutscene loses the playhead, and playOnLoad
    // cheerfully replays the whole thing on the next load.
    "SequencePlayer",
};

static bool IsSnapshotTag(const std::string& tag)
{
    for (const char* t : kEntitySnapshotTags)
        if (tag == t) return true;
    return false;
}

// One entity's snapshot: the same "  <Tag>: k=v ...\n" line format the scene serializer uses
// (values percent-encoded -> the token split is space-safe), prefixed with a hand-rolled "  T:"
// LocalTransform line.
static std::string CaptureEntityBlock(World& world, Entity e)
{
    std::ostringstream ss;
    if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
    {
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "  T: tx=%.6f ty=%.6f tz=%.6f qx=%.6f qy=%.6f qz=%.6f qw=%.6f"
                 " sx=%.6f sy=%.6f sz=%.6f\n",
                 lt->translation.x, lt->translation.y, lt->translation.z,
                 lt->rotation.x, lt->rotation.y, lt->rotation.z, lt->rotation.w,
                 lt->scale.x, lt->scale.y, lt->scale.z);
        ss << buf;
    }
    ComponentSerializerRegistry& reg = GetComponentRegistry();
    for (const char* tag : kEntitySnapshotTags)
        if (const ComponentSerializer* cs = reg.FindByTag(tag))
            if (cs->has(world, e))
                cs->serialize(world, e, ss);
    return ss.str();
}

static float KvF(const KVMap& kv, const char* key, float def)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

// Patches one live entity from its snapshot block. Returns true if anything was applied.
static bool ApplyEntityBlock(World& world, Entity e, const std::string& block,
                             Resource::AssetManager* assetMgr)
{
    ComponentSerializerRegistry& reg = GetComponentRegistry();
    bool applied = false;
    bool scriptCleared = false;

    std::istringstream stream(block);
    std::string line;
    while (std::getline(stream, line))
    {
        const std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        const std::size_t colon = line.find(':', b);
        if (colon == std::string::npos) continue;
        const std::string tag = line.substr(b, colon - b);

        KVMap kv;
        {
            std::istringstream toks(line.substr(colon + 1));
            std::string tok;
            while (toks >> tok)
            {
                const std::size_t eq = tok.find('=');
                if (eq != std::string::npos)
                    kv.emplace(tok.substr(0, eq), tok.substr(eq + 1));
            }
        }

        if (tag == "T")
        {
            LocalTransform lt;
            if (const LocalTransform* cur = world.GetComponent<LocalTransform>(e))
                lt = *cur;
            lt.translation = { KvF(kv, "tx", lt.translation.x),
                               KvF(kv, "ty", lt.translation.y),
                               KvF(kv, "tz", lt.translation.z) };
            lt.rotation    = { KvF(kv, "qx", lt.rotation.x),
                               KvF(kv, "qy", lt.rotation.y),
                               KvF(kv, "qz", lt.rotation.z),
                               KvF(kv, "qw", lt.rotation.w) };
            lt.scale       = { KvF(kv, "sx", lt.scale.x),
                               KvF(kv, "sy", lt.scale.y),
                               KvF(kv, "sz", lt.scale.z) };
            world.AddComponent<LocalTransform>(e, lt);   // Pool::Add replaces
            applied = true;
            continue;
        }

        if (!IsSnapshotTag(tag)) continue;   // hand-edited save with a non-whitelisted tag
        const ComponentSerializer* cs = reg.FindByTag(tag);
        if (!cs) continue;

        // The Script loader get-or-creates the component and APPENDS a slot per line: clear it once so
        // saved slots REPLACE the authored ones instead of duplicating them. Safe before the instances
        // exist: the reconcile pass spawns them from the final slot list, with the saved var overrides
        // injected before OnSpawn.
        if (tag == "Script")
        {
            if (!scriptCleared)
            {
                world.RemoveComponent<ScriptComponent>(e);
                scriptCleared = true;
            }
            cs->deserialize(world, e, kv, assetMgr);
        }
        else
        {
            // A PATCH of a live component: keep what the save cannot carry
            // (ComponentWrite) - a patched light keeps its gather order instead
            // of becoming the newest light, and with it possibly the sun.
            ComponentWrite::RewriteFromKV(reg, tag, world, e, kv, assetMgr);
        }
        applied = true;
    }
    return applied;
}

// Every save-file operation (exists / write / read) funnels through here, so this decides where
// a player's progress lives. Never a bare name resolved against the working directory - each of
// these loses saves silently: under C:\Program Files the write is UAC-denied or redirected into
// the per-user VirtualStore; two Windows accounts would share one file; a shortcut, the debugger
// or elevation changes the CWD, so one build sees different save sets. The writable root avoids
// all three: in Game.exe the per-user root (%LOCALAPPDATA%\DX12Engine), in the editor the project
// directory. See PathUtil.h "The two roots" and "Writable root POLICY", and main.cpp for where
// the choice is made.
std::string SaveSlotPath(int slot)
{
    if (slot < 0) slot = 0;
    // The platform's save folder: a store's cloud-synced one, else the local services' ProjectDataDir -
    // not WritableDataDir, so two projects do not share one save0.json. ProjectDataDir is
    // WritableDataDir() itself until a project manifest names the project, so an install without a
    // project name keeps finding the saves it wrote.
    const std::string dir = Plat::Services().SaveFolder();
    Sys::CreateDirs(dir);
    return dir + "/save" + std::to_string(slot) + ".json";
}

void InstallPersistenceHooks(World& world)
{
    static World* s_hooked = nullptr;
    if (s_hooked == &world) return;
    s_hooked = &world;
    // Fires BEFORE components are erased (World contract), so the Guid is still readable.
    // World::Clear (scene switch) fires no listeners - right: only explicit runtime destroys are
    // "gameplay events".
    (void)world.AddEntityDestroyListener([&world](Entity e)
    {
        if (!world.GetComponent<PersistentTag>(e)) return;
        const GuidComponent* gc = world.GetComponent<GuidComponent>(e);
        if (!gc || !gc->guid.IsValid()) return;
        GameState::Get().MarkPersistentDestroyed(gc->guid.ToHex());
    });
}

bool SaveGameExists(int slot)
{
    std::error_code ec;
    return std::filesystem::exists(Sys::U8Path(SaveSlotPath(slot)), ec);
}

std::vector<SaveInfo> ListSaves()
{
    std::vector<SaveInfo> out;
    std::error_code ec;
    const std::filesystem::path dir = Sys::U8Path(Plat::Services().SaveFolder());
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        // save<digits>.json exactly: a .tmp left by a crash mid-save and a foreign file are not slots.
        const std::wstring name = it->path().filename().wstring();
        constexpr std::wstring_view kPrefix = L"save", kSuffix = L".json";
        if (name.size() <= kPrefix.size() + kSuffix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0 ||
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        const std::wstring digits = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
        if (digits.size() > 6 || !std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            continue;

        SaveInfo info;
        info.slot  = std::stoi(digits);
        info.bytes = it->file_size(ec);
        nlohmann::json doc;
        std::string error;
        info.loadable = ReadSave(info.slot, doc, error, info.version);
        if (doc.is_object())
        {
            info.sceneName       = doc.value("sceneName", std::string{});
            info.playtimeSeconds = doc.value("playtimeSeconds", 0.0);
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.slot < b.slot; });
    return out;
}

bool DeleteSave(int slot)
{
    if (slot < 0) return false;
    std::error_code ec;
    const std::string path = SaveSlotPath(slot);
    std::filesystem::remove(Sys::U8Path(path + ".tmp"), ec);   // a crash mid-save leaves one
    return std::filesystem::remove(Sys::U8Path(path), ec);
}

// ==== Asset identity in save files ====
// A save outlives the asset tree it was written against and sits where no editor tool can reach
// it, so a path-only reference there is unrecoverable once asset/ is reorganised. Every asset path
// a save carries travels next to its guid: written by Resource::GuidHexForPath, read back by
// Resource::ResolveStoredPath.

bool SaveGame(int slot, const std::string& sceneName, const std::string& scenePath,
              World* world, const GameFlowStateMachine* flow)
{
    const GameState& gs = GameState::Get();

    nlohmann::json j;
    j["_comment"]        = "DX12 Engine save game - hand-editable during development.";
    j["version"]         = kSaveVersion;
    j["sceneName"]       = sceneName;
    j["scenePath"]       = scenePath;
    j["scenePathGuid"]   = Resource::GuidHexForPath(scenePath);
    j["playtimeSeconds"] = gs.GetPlaytime();
    j["numbers"]         = gs.Numbers();
    j["strings"]         = gs.Strings();
    j["items"]           = gs.Items();

    // Flow record, additive like the entity records: a reader that does not know the key ignores
    // it, and a save written without a flow restores the world and the scene only.
    if (flow && flow->HasCurrentState())
    {
        nlohmann::json f;
        f["path"] = flow->SerializeRuntimePath();
        nlohmann::json params;
        for (const auto& [key, value] : flow->CurrentParams().values)
        {
            std::visit([&](const auto& v)
            {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, DirectX::XMFLOAT3>)
                    params[key] = { v.x, v.y, v.z };
                else if constexpr (std::is_same_v<V, DirectX::XMFLOAT4>)
                    params[key] = { v.x, v.y, v.z, v.w };
                else
                    params[key] = v;
            }, value);
        }
        f["params"] = std::move(params);
        j["flow"] = std::move(f);
    }

    // ---- PersistentTag entity record ----
    // Scene-authored marked entities -> "entities" (patched in place on load); RuntimeSpawnedTag
    // marked entities -> "spawned" (re-instantiated from their PrefabSourcePath); runtime destroys ->
    // "destroyed" (the GameState graveyard, re-destroyed after load).
    std::size_t entityCount = 0;
    if (world)
    {
        nlohmann::json ents    = nlohmann::json::object();
        nlohmann::json spawned = nlohmann::json::object();
        if (auto* pool = world->GetPool<PersistentTag>())
        {
            // Copy the entity list first: EnsureGuidOn may AddComponent (GuidComponent pool growth is
            // fine, but never mutate a pool while iterating its own dense array).
            const std::vector<Entity> marked(pool->Entities().begin(),
                                             pool->Entities().end());
            for (Entity e : marked)
            {
                if (!world->IsAlive(e)) continue;
                // Stamp a Guid if absent so THIS session round-trips; a save-time-only Guid cannot match
                // after an app restart - the header tells authors to re-save the scene after tagging (the
                // name fallback covers the gap meanwhile).
                const ECS::Guid guid = ECS::EnsureGuidOn(*world, e);
                if (!guid.IsValid()) continue;

                nlohmann::json ent;
                ent["name"]  = world->GetName(e);
                ent["block"] = CaptureEntityBlock(*world, e);

                if (world->GetComponent<RuntimeSpawnedTag>(e))
                {
                    const PrefabSourcePath* src = world->GetComponent<PrefabSourcePath>(e);
                    if (!src || src->path.empty())
                    {
                        LOG_WARNING("SaveGame: runtime-spawned persistent '%s' has no "
                                    "PrefabSourcePath — cannot be recreated on load, skipped "
                                    "(only Engine.SpawnPrefab roots carry a recipe)",
                                    world->GetName(e).c_str());
                        continue;
                    }
                    ent["prefab"]     = src->path;
                    ent["prefabGuid"] = Resource::GuidHexForPath(src->path);
                    spawned[guid.ToHex()] = std::move(ent);
                }
                else
                {
                    ents[guid.ToHex()] = std::move(ent);
                }
                ++entityCount;
            }
        }
        j["entities"] = std::move(ents);
        j["spawned"]  = std::move(spawned);
    }

    // Graveyard: always saved (it is GameState session state, world or not).
    {
        nlohmann::json dead = nlohmann::json::array();
        for (const std::string& hex : gs.DestroyedPersistents())
            dead.push_back(hex);
        j["destroyed"] = std::move(dead);
    }

    const std::string finalPath = SaveSlotPath(slot);
    const std::string tmpPath   = finalPath + ".tmp";
    {
        auto f = Sys::OpenOfstream(tmpPath, std::ios::binary);
        if (!f)
        {
            LOG_ERROR("SaveGame: cannot open '%s' for write", tmpPath.c_str());
            return false;
        }
        f << j.dump(2) << "\n";
        f.flush();
        if (!f.good())
        {
            LOG_ERROR("SaveGame: write failed for '%s'", tmpPath.c_str());
            return false;
        }
    }

    // Atomic replace: a crash mid-save leaves either the OLD complete save or the new one, never a
    // truncated file.
    std::error_code ec;
    std::filesystem::rename(Sys::U8Path(tmpPath), Sys::U8Path(finalPath), ec);
    if (ec)
    {
        LOG_ERROR("SaveGame: rename '%s' -> '%s' failed: %s",
                  tmpPath.c_str(), finalPath.c_str(), ec.message().c_str());
        return false;
    }

    LOG_INFO("SaveGame: wrote '%s' (scene='%s', playtime=%.1fs, %zu numbers / %zu strings / %zu items, %zu entities)",
             finalPath.c_str(), sceneName.c_str(), gs.GetPlaytime(),
             gs.Numbers().size(), gs.Strings().size(), gs.Items().size(), entityCount);
    return true;
}

bool LoadGame(int slot, std::string* outSceneName, std::string* outScenePath,
              GameFlowStateMachine* flow)
{
    const std::string path = SaveSlotPath(slot);
    nlohmann::json j;
    std::string error;
    int fileVersion = 0;
    if (!ReadSave(slot, j, error, fileVersion))
    {
        // A missing or newer-than-this-build save is the player's situation, not an engine fault.
        if (!SaveGameExists(slot) || fileVersion != 0)
            LOG_WARNING("LoadGame: %s — GameState untouched", error.c_str());
        else
            LOG_ERROR("LoadGame: %s — GameState untouched", error.c_str());
        return false;
    }

    // Decode EVERYTHING before touching GameState so a malformed section
    // can't leave it half-replaced.
    std::unordered_map<std::string, double>      numbers;
    std::unordered_map<std::string, std::string> strings;
    std::unordered_map<std::string, int>         items;
    try
    {
        numbers = j.value("numbers", nlohmann::json::object())
                      .get<std::unordered_map<std::string, double>>();
        strings = j.value("strings", nlohmann::json::object())
                      .get<std::unordered_map<std::string, std::string>>();
        items   = j.value("items", nlohmann::json::object())
                      .get<std::unordered_map<std::string, int>>();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("LoadGame: '%s' has malformed sections (%s) — GameState untouched",
                  path.c_str(), e.what());
        return false;
    }

    GameState& gs = GameState::Get();
    gs.ReplaceAll(std::move(numbers), std::move(strings), std::move(items));
    gs.SetPlaytime(j.value("playtimeSeconds", 0.0));

    // The graveyard is session state like the maps: replace it so later saves carry the loaded
    // history forward.
    {
        std::unordered_set<std::string> dead;
        const auto it = j.find("destroyed");
        if (it != j.end() && it->is_array())
            for (const auto& v : *it)
                if (v.is_string()) dead.insert(v.get<std::string>());
        gs.ReplaceDestroyedPersistents(std::move(dead));
    }

    // Flow record. QUEUED, never applied here: the states being restored spawn entities into a world
    // that is not loaded yet - the scene swap brings it up, and App applies the queued restore on
    // SceneLoadedEvent. Absent key = a save taken with no flow running, or written without `flow`;
    // either way the queue is cleared so a stale one cannot fire.
    if (flow)
    {
        std::string leaf;
        PropertyBag params;
        if (const auto it = j.find("flow"); it != j.end() && it->is_object())
        {
            if (const auto p = it->find("path"); p != it->end() && p->is_array()
                && !p->empty() && p->back().is_string())
            {
                // The LEAF alone is enough: entering it walks every ancestor on
                // the way down. The rest of the path is stored for diagnostics.
                leaf = p->back().get<std::string>();
            }
            if (const auto pv = it->find("params"); pv != it->end() && pv->is_object())
            {
                for (const auto& [key, value] : pv->items())
                {
                    if (value.is_string())              params.SetString(key, value.get<std::string>());
                    else if (value.is_boolean())        params.SetInt(key, value.get<bool>() ? 1 : 0);
                    else if (value.is_number_integer()) params.SetInt(key, value.get<int>());
                    else if (value.is_number())         params.SetFloat(key, value.get<float>());
                    else if (value.is_array() && value.size() == 3)
                        params.SetVec3(key, DirectX::XMFLOAT3{
                            value[0].get<float>(), value[1].get<float>(), value[2].get<float>() });
                    else if (value.is_array() && value.size() == 4)
                        params.SetVec4(key, DirectX::XMFLOAT4{
                            value[0].get<float>(), value[1].get<float>(),
                            value[2].get<float>(), value[3].get<float>() });
                }
            }
        }
        flow->QueueRestore(std::move(leaf), std::move(params));
    }

    const std::string sceneName = j.value("sceneName", std::string{});
    const std::string scenePath = Resource::ResolveStoredPath(
        j.value("scenePath",     std::string{}),
        j.value("scenePathGuid", std::string{}), "scene");
    if (outSceneName) *outSceneName = sceneName;
    if (outScenePath) *outScenePath = scenePath;

    LOG_INFO("LoadGame: restored '%s' (scene='%s', playtime=%.1fs)",
             path.c_str(), sceneName.c_str(), gs.GetPlaytime());
    return true;
}

int ApplySavedEntities(int slot, World& world, Resource::AssetManager* assetMgr,
                       Renderer* renderer, Resource::AnimationClipSystem* animClipSys)
{
    const std::string path = SaveSlotPath(slot);
    nlohmann::json j;
    std::string error;
    int fileVersion = 0;
    if (!ReadSave(slot, j, error, fileVersion)) return 0;

    // Unique-name fallback index for entities whose scene lacks a serialized
    // Guid (built lazily on the first miss; skips duplicated names).
    std::unordered_map<std::string, Entity> byName;
    bool byNameBuilt = false;
    auto buildByName = [&]() {
        std::unordered_map<std::string, int> counts;
        for (Entity e : world.GetEntities())
        {
            const std::string n = world.GetName(e);
            if (n.empty()) continue;
            if (++counts[n] == 1) byName[n] = e;
            else                  byName.erase(n);   // ambiguous: unusable
        }
        byNameBuilt = true;
    };

    int patched = 0, destroyed = 0, respawned = 0;

    // ---- Pass 1: patch scene-authored persistent entities ----
    const auto entsIt = j.find("entities");
    if (entsIt != j.end() && entsIt->is_object())
    {
        for (auto it = entsIt->begin(); it != entsIt->end(); ++it)
        {
            const ECS::Guid guid = ECS::Guid::FromHex(it.key());
            const std::string name  = it.value().value("name",  std::string{});
            const std::string block = it.value().value("block", std::string{});
            if (block.empty()) continue;

            Entity e = guid.IsValid() ? ECS::GuidRegistry::Get().Find(guid) : NullEntity;
            if (e == NullEntity && !name.empty())
            {
                if (!byNameBuilt) buildByName();
                auto nIt = byName.find(name);
                if (nIt != byName.end()) e = nIt->second;
            }
            if (e == NullEntity || !world.IsAlive(e))
            {
                LOG_WARNING("ApplySavedEntities: no live entity for saved '%s' (guid %s) — skipped "
                            "(tag the entity with a scene-saved GuidComponent to re-attach across runs)",
                            name.c_str(), it.key().c_str());
                continue;
            }
            if (ApplyEntityBlock(world, e, block, assetMgr)) ++patched;
        }
    }

    // ---- Pass 2: re-destroy graveyarded scene entities ----
    // Skips guids in this save's own spawned set: a respawned entity whose old destroy record
    // lingers must not be killed by its own save.
    const auto spawnedIt = j.find("spawned");
    const auto deadIt    = j.find("destroyed");
    if (deadIt != j.end() && deadIt->is_array())
    {
        for (const auto& v : *deadIt)
        {
            if (!v.is_string()) continue;
            const std::string hex = v.get<std::string>();
            if (spawnedIt != j.end() && spawnedIt->is_object()
                && spawnedIt->contains(hex)) continue;
            const ECS::Guid guid = ECS::Guid::FromHex(hex);
            if (!guid.IsValid()) continue;
            const Entity e = ECS::GuidRegistry::Get().Find(guid);
            if (e == NullEntity || !world.IsAlive(e)) continue;
            world.DestroyEntity(e);
            ++destroyed;
        }
    }

    // ---- Pass 3: re-instantiate runtime-spawned entities ----
    if (spawnedIt != j.end() && spawnedIt->is_object())
    {
        for (auto it = spawnedIt->begin(); it != spawnedIt->end(); ++it)
        {
            const ECS::Guid guid = ECS::Guid::FromHex(it.key());
            if (!guid.IsValid()) continue;
            const std::string block  = it.value().value("block",  std::string{});
            const std::string prefab = Resource::ResolveStoredPath(
                it.value().value("prefab",     std::string{}),
                it.value().value("prefabGuid", std::string{}), "prefab");

            Entity e = ECS::GuidRegistry::Get().Find(guid);
            if (e != NullEntity && world.IsAlive(e))
            {
                // Same-session apply: the spawn is still alive; just patch it.
                if (ApplyEntityBlock(world, e, block, assetMgr)) ++patched;
                continue;
            }
            if (prefab.empty() || !assetMgr) continue;

            // Seed the instantiation with the saved Guid: the root comes back on
            // that exact identity and every descendant is derived from it, so a
            // respawned prefab is byte-identical in identity to the one that was
            // saved (a random seed would re-randomise the descendants on every
            // load and break anything referencing them).
            e = Resource::SpawnPrefab(prefab, world, *assetMgr, renderer, animClipSys, &guid,
                                      "SaveGame.Respawn");
            if (e == NullEntity)
            {
                LOG_WARNING("ApplySavedEntities: LoadPrefab('%s') failed for saved spawn '%s'",
                            prefab.c_str(), it.value().value("name", std::string{}).c_str());
                continue;
            }
            // Re-stamp the SAVED Guid on the root. Redundant when the .ipfb has a
            // Guid block (the seed already put it there), but prefabs saved
            // without one give the root no GuidComponent at all.
            ECS::GuidRegistry::Get().UnregisterEntity(e);
            world.AddComponent<GuidComponent>(e, GuidComponent{ guid });
            ECS::GuidRegistry::Get().Register(guid, e);
            world.AddComponent<PersistentTag>(e, {});
            world.AddComponent<RuntimeSpawnedTag>(e, {});
            ApplyEntityBlock(world, e, block, assetMgr);
            // Its own destroy record (if any lingered) must not outlive the respawn.
            GameState::Get().UnmarkPersistentDestroyed(it.key());
            ++respawned;
        }
    }

    const int touched = patched + destroyed + respawned;
    if (touched > 0)
        LOG_INFO("ApplySavedEntities: '%s' — patched %d, destroyed %d, respawned %d",
                 path.c_str(), patched, destroyed, respawned);
    return touched;
}

} // namespace Gameplay
