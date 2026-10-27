#include "Settings/SettingsRows.h"
#include "Settings/SettingsStore.h"
#include "Settings/SetFlags.h"
#include "Settings/LegacyFlags.h"

#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The pass rows: include/Settings/Rows/Passes.inl expanded into a table, the getters
// the passes read (Settings/PassSettings.h), and the boot read of each row's old flag
// (Settings/LegacyFlags.h, the pass style).
namespace Settings
{
namespace
{
    using PS = Resource::PassSettings;

    // One id per row, in Passes.inl order: the table's index and the getters'.
    enum class PassId : uint32_t
    {
#define SETTING(K, P, M, ...) M,
#include "Settings/Rows/Passes.inl"
#undef SETTING
        Count
    };
    constexpr size_t kPassRows = static_cast<size_t>(PassId::Count);

    // The effective values the getters read. The main thread publishes a row's
    // value after every write (WritePassSetting); a getter reads it on any
    // thread. Bool / Int / Enum / Choice / Float as 32 bits in an atomic; a
    // String behind a mutex. `given`: a layer above Default holds the value.
    struct Published
    {
        std::array<std::atomic<uint32_t>, kPassRows> bits{};
        std::array<std::atomic<bool>, kPassRows>     given{};
        std::array<std::atomic<bool>, kPassRows>     asked{};   // the getter ran: the old flag is marked read
        std::mutex                                   textLock;
        std::array<std::string, kPassRows>           text;
        std::atomic<bool>                            booted{ false };   // ApplyPassCommandLine ran

        Published();
    };

    // "pass.<pass>.<field>" -> "<pass>".
    std::string_view PassOf(std::string_view path)
    {
        path.remove_prefix(path.find('.') + 1);
        return path.substr(0, path.find('.'));
    }

    const std::vector<PassRow>& Table()
    {
        static const std::vector<PassRow> s_rows = []
        {
            std::vector<PassRow> v;
#define SETTING(K, P, M, ...) v.push_back(detail::Make##K(P, &PS::M, PassRowSpec{ __VA_ARGS__ }));
#include "Settings/Rows/Passes.inl"
#undef SETTING
            ENGINE_CHECK(v.size() == kPassRows, "Passes.inl: %zu rows for %zu ids", v.size(), kPassRows);
            for (size_t i = 0; i < v.size(); ++i)
            {
                const PassRow& r = v[i];
                const std::string_view path = r.path;
                detail::CheckHooks(r.path, r.spec.phase, r.spec.apply != nullptr, r.spec.capture != nullptr);
                // The passes read every value through its getter: nothing to
                // apply, nothing to capture.
                ENGINE_CHECK(r.spec.phase == Phase::None, "%s: a pass row is Phase::None (phase %d)", r.path,
                             static_cast<int>(r.spec.phase));
                ENGINE_CHECK(path.rfind("pass.", 0) == 0 && path.find('.', 5) != std::string_view::npos,
                             "%s: a pass row's path is pass.<pass>.<field>", r.path);
                // One path, one row, whichever document it lives in.
                ENGINE_CHECK(FindPlayerRow(r.path) == nullptr && FindDevRow(r.path) == nullptr,
                             "%s is both a pass row and a player / developer row", r.path);
                // A pass row's old flag is one flag read one way: the syntax,
                // never a hook (LegacyFlags.h).
                const std::string legacy = LegacySpecError(r);
                ENGINE_CHECK(legacy.empty(), "%s: .legacyFlag: %s", r.path, legacy.c_str());
                ENGINE_CHECK(!r.spec.legacyRead, "%s: a pass row reads its old flag by .legacyFlag, not a hook", r.path);
                // A pass's rows are one block: the editor's page draws a tree per pass.
                for (size_t j = 0; j + 1 < i; ++j)
                    ENGINE_CHECK(PassOf(v[j].path) != PassOf(path) || PassOf(v[i - 1].path) == PassOf(path),
                                 "%s: the rows of pass '%.*s' are not one block in Passes.inl", r.path,
                                 static_cast<int>(PassOf(path).size()), PassOf(path).data());
            }
            return v;
        }();
        return s_rows;
    }

    void Store(Published& p, size_t i, const PassRow& r, const Value& v, bool given)
    {
        switch (r.kind)
        {
        case Kind::Bool:   p.bits[i].store(v.b ? 1u : 0u, std::memory_order_relaxed); break;
        case Kind::Int:
        case Kind::Enum:
        case Kind::Choice: p.bits[i].store(std::bit_cast<uint32_t>(v.i), std::memory_order_relaxed); break;
        case Kind::Float:  p.bits[i].store(std::bit_cast<uint32_t>(v.f), std::memory_order_relaxed); break;
        case Kind::String:
        {
            std::lock_guard<std::mutex> lock(p.textLock);
            p.text[i] = v.s;
            break;
        }
        }
        p.given[i].store(given, std::memory_order_relaxed);
    }

    Published::Published()
    {
        // Until a write, every row is its default - the member's initializer.
        const PS defaults{};
        const std::vector<PassRow>& t = Table();
        for (size_t i = 0; i < t.size(); ++i) Store(*this, i, t[i], ValueOf(t[i], defaults), false);
    }

    Published& Pub()
    {
        static Published s_instance;
        return s_instance;
    }

    size_t IndexOf(const PassRow& r)
    {
        const std::vector<PassRow>& t = Table();
        ENGINE_CHECK(&r >= t.data() && &r < t.data() + t.size(), "'%s' is not a pass row", r.path);
        return static_cast<size_t>(&r - t.data());
    }

    // A row's first read marks its old flag read, so run.json's unreadFlags still names a
    // flag nothing in this run asked for.
    void FirstRead(Published& p, size_t i)
    {
        if (p.asked[i].exchange(true, std::memory_order_relaxed)) return;
        const PassRow& r = Table()[i];
        ENGINE_CHECK(p.booted.load(std::memory_order_acquire),
                     "Settings::Pass: '%s' read before Settings::ApplyPassCommandLine - its old flag and "
                     "-set:%s are not applied yet", r.path, r.path);
        for (const LegacySpelling& sp : SplitLegacy(r.spec.legacyFlag))
            Sys::CommandLine::Get().MarkRead(sp.flag.c_str());
    }

    template <class T>
    T Read(PassId id)
    {
        Published& p = Pub();
        const size_t i = static_cast<size_t>(id);
        if (!p.asked[i].load(std::memory_order_relaxed)) FirstRead(p, i);
        if constexpr (std::is_same_v<T, bool>)
            return p.bits[i].load(std::memory_order_relaxed) != 0;
        else if constexpr (std::is_same_v<T, int>)
            return std::bit_cast<int>(p.bits[i].load(std::memory_order_relaxed));
        else if constexpr (std::is_same_v<T, float>)
            return std::bit_cast<float>(p.bits[i].load(std::memory_order_relaxed));
        else
        {
            static_assert(std::is_same_v<T, std::string>, "a pass row's member is bool, int, float or std::string");
            std::lock_guard<std::mutex> lock(p.textLock);
            return p.text[i];
        }
    }

    bool Given(PassId id)
    {
        Published& p = Pub();
        const size_t i = static_cast<size_t>(id);
        if (!p.asked[i].load(std::memory_order_relaxed)) FirstRead(p, i);
        return p.given[i].load(std::memory_order_relaxed);
    }
}

std::span<const PassRow> PassRows()
{
    const std::vector<PassRow>& t = Table();
    return { t.data(), t.size() };
}

const PassRow* FindPassRow(std::string_view path)
{
    for (const PassRow& r : Table())
        if (path == r.path) return &r;
    return nullptr;
}

bool WritePassSetting(const PassRow& r, Layer layer, const Value* v, std::string_view writer)
{
    PassSettingsStore& st = PassSettingsStore::Get();
    if (!(v ? st.Write(r, layer, *v, writer) : st.Clear(r, layer, writer)))
        return false;
    Store(Pub(), IndexOf(r), r, st.Effective(r), st.Source(r) != Layer::Default);
    return true;
}

int ApplyPassCommandLine()
{
    int written = 0;
    // Each row's old flag, read as the pass read it and left unread until the
    // pass asks (LegacyFlags.h, the pass style). The flag is the writer's name.
    for (const PassRow& r : Table())
    {
        Value v;
        std::string writer;
        if (r.spec.legacyFlag &&
            ReadLegacy(r, LegacyStyle::Pass, v, writer, nullptr) == LegacyResult::Taken &&
            WritePassSetting(r, Layer::CommandLine, &v, writer))
            ++written;
    }
    // -set:pass.<...>=<v>, after the old flags: given both, the new spelling
    // wins. A refused one is reported - and refuses the run - by settings.json's
    // boot load, which reads every -set: (ApplyCommandLineSettings).
    for (const SetFlag<PS>& f : ParseSetFlags(nullptr).pass)
        if (WritePassSetting(*f.row, Layer::CommandLine, &f.value, f.flag)) ++written;
    Pub().booted.store(true, std::memory_order_release);
    const PassSettingsStore& st = PassSettingsStore::Get();
    LOG_INFO("[SETTINGS] pass rows=%zu cmdline=%zu", st.Rows().size(), st.CountIn(Layer::CommandLine));
    return written;
}
}

namespace Settings::Pass
{
#define SETTING(K, P, M, ...)                                                                                       \
    decltype(::Resource::PassSettings::M) M() { return Read<decltype(::Resource::PassSettings::M)>(PassId::M); } \
    bool M##Given() { return Given(PassId::M); }
#include "Settings/Rows/Passes.inl"
#undef SETTING
}
