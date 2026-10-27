#pragma once

// Settings/SettingsStore.h - the settings store: each row's value in each of the seven
// layers, the effective value the row's policy picks from them, and who wrote what. One
// store per document: SettingsStore over the player rows (settings.json),
// DevSettingsStore over the developer rows (engine.json), PassSettingsStore over the
// pass rows.
//
// Every write is recorded, so `Settings.where(path)` can say which layer a value came
// from and who wrote it, and the per-frame snapshot is built from the effective values.
// For the player document the writers hand the owners the store's projection
// (OwnerValue), so a layer decides: a quality level under the player's own value, a
// -set: over it. engine.json's writers still push their own value to the owner
// (EngineDevSettings::ApplyTo, the Renderer setters) and the store only records it. The
// pass rows' getters read the effective value directly.
//
// The writers that reach it:
//   Project      engine.json's boot load (only the keys the file held), a save of it
//                (every row: Settings.saveDeveloper, the editor's Save to engine.json),
//                a reload (Settings.reloadDeveloper, the editor's Reload engine.json -
//                which also clears the other layers of every row it applied)
//   Scalability  the quality levels (Settings/Scalability.h): each group's rows at its
//                level's values, rewritten whenever a level's effective value changes
//   Player       the boot load of settings.json (only the keys the file held),
//                Settings.apply (the rows that changed), the editor's controls, Load &
//                Apply / Reset to Defaults
//   CommandLine  -set:<path>=<value> (any document's paths); -pathtrace / -gipath /
//                -pt_realtime (features.renderPath)
//   Scene        the scene's intent for features.ddgi / ssgi (IndirectLightingSettings),
//                ssao and volumetricFog, recorded by the Renderer - its defaults when
//                the scene has none
//   Runtime      Settings.setRuntime / clearRuntime, the pipe's `setting`,
//                Settings.setPTRealtime; for engine.json's rows the developer setters
//                (setRestirBackend / Compare / GISharc / Denoiser, setPTDirectLight,
//                setPTArea)
// Writers that do not reach it, so `where` cannot see their writes: a scene's .ippc,
// Audio.setBusVolume, every other flag.
//
// A Capped row (Policy::Capped) is the one place the store's effective value and the
// value its owner holds differ: the owner (the Renderer's authored mirror) holds the
// allowance - OwnerValue - and applies the scene's intent itself; the effective value
// is both, which is what runs.
//
// Written from the main thread only (checked).

#include "Settings/SettingsRows.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Settings
{
    // The seven layers, lowest first.
    enum class Layer : uint8_t
    {
        Default,       // the row's default: a default-constructed struct
        Scalability,   // the quality levels' values (Settings/Scalability.h)
        Project,       // engine.json
        Player,        // settings.json, the in-game menu, the editor's player pages
        Scene,         // scene components' intents
        CommandLine,   // flags and -set:<path>=<v>
        Runtime,       // developer setters, the pipe, the console; never saved
        Count,
    };
    inline constexpr size_t kLayerCount = static_cast<size_t>(Layer::Count);
    // "default" / "scalability" / "project" / "player" / "scene" /
    // "commandLine" / "runtime"; nullptr for Count.
    const char* LayerName(Layer l);
    bool        ParseLayer(std::string_view name, Layer& out);
    const char* PolicyName(Policy p);   // "layered" / "lastWrite" / "capped"

    // One row's value; the row's kind says which member holds it (Enum: the
    // label index, Choice: the value itself, Int: i, Float: f, String: s).
    struct Value
    {
        bool        b = false;
        int         i = 0;
        float       f = 0.f;
        std::string s;
    };
    template <class S> Value ValueOf(const RowT<S>& r, const S& s);
    template <class S> void  PutValue(const RowT<S>& r, const Value& v, S& s);
    template <class S> bool  SameValue(const RowT<S>& r, const Value& a, const Value& b);

    // A writer's value before validation: what Lua, a command line or the pipe hands in.
    // Coerce applies Settings.set's rules:
    //   Bool          a boolean
    //   Int / Float   a number; NaN refused, then clamped to [lo, hi] (Int rounds)
    //   Enum          the label index or the label itself; out of range refused
    //   Choice        a number that is one of the row's values
    //   String        a string; with a legal set (dlssRRModel) one of it
    struct Request
    {
        enum class Type : uint8_t { Bool, Number, Text, Other };
        Type        type   = Type::Other;
        bool        b      = false;
        double      number = 0.0;
        std::string text;
    };
    enum class Refusal : uint8_t { None, WrongType, NaN, OutOfRange, NotOffered };
    const char* RefusalName(Refusal r);   // "none" / "wrongType" / "nan" / "outOfRange" / "notOffered"
    template <class S> Refusal Coerce(const RowT<S>& r, const Request& in, Value& out);

    // A value typed as text (-set:<path>=<text>, the pipe's `setting set`) as
    // a request for the row's kind: a Bool takes true/false/1/0/on/off/yes/no,
    // Int / Float / Choice a number, an Enum its index, its label or its
    // settings.json token (FXAA_TAA), any case. Text that is none of those
    // stays Text, so Coerce refuses it as Settings.set would.
    template <class S> Request RequestFromText(const RowT<S>& r, const std::string& text);

    // A value as text for a reply or a log: an Enum's label, a Bool
    // true / false, a number, a String as is.
    template <class S> std::string ValueText(const RowT<S>& r, const Value& v);

    struct WriteRecord
    {
        Layer       layer   = Layer::Default;
        bool        cleared = false;   // a Clear, not a write (value unused)
        Value       value;
        uint64_t    frame   = 0;       // Logger::CurrentFrame() at the write
        std::string writer;            // "Settings.apply", "-pathtrace", a file path...
    };

    template <class S>
    class BasicStore
    {
    public:
        using RowType = RowT<S>;

        struct LayerSub
        {
            Layer        layer = Layer::Player;
            const Value* value = nullptr;
        };

        using Observer = void (*)(const RowType& r);

        static constexpr size_t kHistory = 8;

        // The process-wide store over this document's rows (main thread).
        static BasicStore& Get();
        // A store of its own over `rows` (the -settings_storetest harness).
        explicit BasicStore(std::span<const RowType> rows);

        // Record an already-coerced value in `layer`. Default cannot be
        // written (false); neither can a row this store does not hold.
        bool Write(const RowType& r, Layer layer, const Value& v, std::string_view writer);
        // Empty `layer` for the row (a no-op when it holds nothing).
        bool Clear(const RowType& r, Layer layer, std::string_view writer);
        // Every row of `s` into `layer` - a load, an apply. With `mask`, only
        // the rows whose entry is non-zero (indexed like Rows()).
        void WriteRows(const S& s, Layer layer, std::string_view writer, std::span<const uint8_t> mask = {});

        std::span<const RowType> Rows() const { return m_rows; }
        const RowType* Find(std::string_view path) const;

        const Value& Effective(const RowType& r) const;
        Layer        Source(const RowType& r) const;
        // The value a writer hands the row's owner (its apply hook) and a
        // menu's copy: the effective value - except a Capped row's, which is
        // the allowance (the highest layer but the Scene's). Its owner applies
        // the scene's intent on its own; handing it the effective value would
        // write a scene's "off" into the player's switch, and into the file
        // the player's settings are saved to.
        const Value& OwnerValue(const RowType& r) const;

        // OwnerValue with some layers' contents replaced: what a writer about to replace them
        // will leave (a reset empties the Player layer, and the quality levels that leaves
        // rewrite the Scalability layer). A replacing value counts as the newest write
        // (LastWrite); a null one empties its layer.
        Value OwnerValueWith(const RowType& r, std::span<const LayerSub> subs) const;

        // A writer that must see every change of an effective value, whoever made it
        // (Settings/Scalability.h: a quality level's change rewrites its group's Scalability
        // layer). Called after the Write / Clear that changed it, on the writing thread; it may
        // write other rows. One per store; nullptr: none.
        void SetObserver(Observer fn) { m_observer = fn; }
        const std::optional<Value>& At(const RowType& r, Layer l) const;
        // A global counter, bumped whenever any row's effective value changes;
        // a row's revision is the counter's value at its own last change (0 =
        // never changed from its default).
        uint64_t Revision() const { return m_revision; }
        uint64_t RowRevision(const RowType& r) const;
        // The row's last writes, oldest first (at most kHistory).
        std::vector<WriteRecord> History(const RowType& r) const;

        // How many rows hold a value in `l`; how many rows use `p`.
        size_t CountIn(Layer l) const;
        size_t CountPolicy(Policy p) const;

    private:
        struct Slot
        {
            std::array<std::optional<Value>, kLayerCount> layers;
            std::array<uint64_t, kLayerCount>             seq{};   // write order, for LastWrite
            Value    effective;
            Layer    source   = Layer::Default;
            uint64_t revision = 0;
            std::vector<WriteRecord> history;   // ring of kHistory
            size_t   next     = 0;              // the ring's next write
        };

        size_t Index(const RowType& r) const;   // m_rows.size() when not ours
        void   CheckThread(const RowType& r);
        void   Remember(Slot& s, WriteRecord rec);
        bool   Recompute(const RowType& r, Slot& s);   // true: the effective value changed

        std::span<const RowType> m_rows;
        std::vector<Slot>        m_slots;
        uint64_t                 m_seq      = 0;
        uint64_t                 m_revision = 0;
        std::thread::id          m_owner;   // the first writer's thread
        Observer                 m_observer = nullptr;
    };

    using SettingsStore     = BasicStore<Resource::EngineSettings>;      // settings.json's rows
    using DevSettingsStore  = BasicStore<Resource::EngineDevSettings>;   // engine.json's rows
    using PassSettingsStore = BasicStore<Resource::PassSettings>;        // the pass rows, not saved
    template <> SettingsStore&     SettingsStore::Get();
    template <> DevSettingsStore&  DevSettingsStore::Get();
    template <> PassSettingsStore& PassSettingsStore::Get();

    // The pass rows (Settings/PassSettings.h). Unlike the other two documents, the store's
    // effective value IS what the engine runs: a write goes through here, which records it
    // and publishes the row's new effective value to the getters (main thread). @p v null
    // clears the layer.
    bool WritePassSetting(const PassRow& r, Layer layer, const Value* v, std::string_view writer);
    // Boot, main thread, before any pass reads a setting: each row's
    // .legacyFlag (read as the pass read it, without marking it read) and the
    // command line's -set:pass.<...> into the CommandLine layer. Returns the
    // number of rows written.
    int ApplyPassCommandLine();

    // The boot line: [SETTINGS] rows= player= project= scene= cmdline= runtime= lastWrite=
    // capped=. App::BootRenderer calls it once the boot writers (settings.json, the render
    // path flags) have recorded.
    void LogBootSummary();
    // engine.json's: [SETTINGS] developer rows= project= cmdline= runtime= lastWrite=, once
    // its boot load (engine.json, -set:) has recorded.
    void LogDevBootSummary();
}
