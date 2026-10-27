#include "Settings/SettingsStore.h"

#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Settings
{
const char* LayerName(Layer l)
{
    switch (l)
    {
    case Layer::Default:     return "default";
    case Layer::Scalability: return "scalability";
    case Layer::Project:     return "project";
    case Layer::Player:      return "player";
    case Layer::Scene:       return "scene";
    case Layer::CommandLine: return "commandLine";
    case Layer::Runtime:     return "runtime";
    case Layer::Count:       break;
    }
    return nullptr;
}

bool ParseLayer(std::string_view name, Layer& out)
{
    for (size_t i = 0; i < kLayerCount; ++i)
        if (name == LayerName(static_cast<Layer>(i))) { out = static_cast<Layer>(i); return true; }
    return false;
}

const char* PolicyName(Policy p)
{
    switch (p)
    {
    case Policy::LastWrite: return "lastWrite";
    case Policy::Capped:    return "capped";
    case Policy::Layered:   break;
    }
    return "layered";
}

const char* RefusalName(Refusal r)
{
    switch (r)
    {
    case Refusal::None:       return "none";
    case Refusal::WrongType:  return "wrongType";
    case Refusal::NaN:        return "nan";
    case Refusal::OutOfRange: return "outOfRange";
    case Refusal::NotOffered: return "notOffered";
    }
    return "none";
}

template <class S>
Value ValueOf(const RowT<S>& r, const S& s)
{
    Value v;
    switch (r.kind)
    {
    case Kind::Bool:   v.b = s.*r.mb; break;
    case Kind::Int:
    case Kind::Enum:
    case Kind::Choice: v.i = s.*r.mi; break;
    case Kind::Float:  v.f = s.*r.mf; break;
    case Kind::String: v.s = s.*r.ms; break;
    }
    return v;
}

template <class S>
void PutValue(const RowT<S>& r, const Value& v, S& s)
{
    switch (r.kind)
    {
    case Kind::Bool:   s.*r.mb = v.b; break;
    case Kind::Int:
    case Kind::Enum:
    case Kind::Choice: s.*r.mi = v.i; break;
    case Kind::Float:  s.*r.mf = v.f; break;
    case Kind::String: s.*r.ms = v.s; break;
    }
}

template <class S>
bool SameValue(const RowT<S>& r, const Value& a, const Value& b)
{
    switch (r.kind)
    {
    case Kind::Bool:   return a.b == b.b;
    case Kind::Int:
    case Kind::Enum:
    case Kind::Choice: return a.i == b.i;
    // Exact: both sides are values a writer stored, not results of arithmetic.
    case Kind::Float:  return a.f == b.f;
    case Kind::String: return a.s == b.s;
    }
    return false;
}

template <class S>
Refusal Coerce(const RowT<S>& r, const Request& in, Value& out)
{
    using T = Request::Type;
    switch (r.kind)
    {
    case Kind::Bool:
        if (in.type != T::Bool) return Refusal::WrongType;
        out.b = in.b;
        return Refusal::None;

    case Kind::Int:
    case Kind::Float:
    {
        if (in.type != T::Number) return Refusal::WrongType;
        double x = in.number;
        // Refused rather than clamped: std::clamp passes NaN straight through
        // (both comparisons are false), and a NaN in the nit fields reaches
        // the PQ encode in the present shader and blanks the screen.
        if (!(x == x)) return Refusal::NaN;
        x = std::clamp(x, double(r.spec.lo), double(r.spec.hi));
        if (r.kind == Kind::Int) out.i = static_cast<int>(std::lround(x));
        else                     out.f = static_cast<float>(x);
        return Refusal::None;
    }

    case Kind::Enum:
    {
        // The index or the label, whichever reads better at the call site. Anything else is
        // out of range, as Settings.set says.
        int idx = -1;
        if (in.type == T::Number) idx = static_cast<int>(std::lround(in.number));
        else if (in.type == T::Text)
        {
            for (size_t i = 0; i < r.labels.size(); ++i)
                if (r.labels[i] == in.text) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0 || idx >= static_cast<int>(r.labels.size())) return Refusal::OutOfRange;
        out.i = idx;
        return Refusal::None;
    }

    case Kind::Choice:
    {
        if (in.type != T::Number) return Refusal::WrongType;
        const int want = static_cast<int>(std::lround(in.number));
        if (std::find(r.values.begin(), r.values.end(), want) == r.values.end())
            return Refusal::NotOffered;
        out.i = want;
        return Refusal::None;
    }

    case Kind::String:
        if (in.type != T::Text) return Refusal::WrongType;
        // A String row with a legal set refuses anything else, as an Enum
        // refuses an out-of-range index; the free-form rows (ui.*) have none.
        if (!r.labels.empty() && std::find(r.labels.begin(), r.labels.end(), in.text) == r.labels.end())
            return Refusal::NotOffered;
        out.s = in.text;
        return Refusal::None;
    }
    return Refusal::WrongType;
}

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // The layer a row's value comes from, by its policy (Recompute's rule):
    // with `ownerValue`, a Capped row's allowance - the scene's intent left out
    // (OwnerValue). Default always holds a value, so there is always a pick.
    template <class Row>
    size_t PickLayer(const Row& r, const std::array<std::optional<Value>, kLayerCount>& layers,
                     const std::array<uint64_t, kLayerCount>& seq, bool ownerValue)
    {
        auto has = [&](size_t l) { return layers[l].has_value(); };
        size_t pick = static_cast<size_t>(Layer::Default);
        if (r.spec.policy == Policy::LastWrite)
        {
            // The most recent write still standing; Default (seq 0) when none.
            for (size_t l = 0; l < kLayerCount; ++l)
                if (has(l) && seq[l] > seq[pick]) pick = l;
        }
        else if (r.spec.policy == Policy::Capped && r.kind == Kind::Bool)
        {
            // Every layer but the scene's is an allowance, layered; the scene can
            // only take it away (Policy::Capped).
            const size_t scene = static_cast<size_t>(Layer::Scene);
            for (size_t l = kLayerCount; l-- > 0;)
                if (l != scene && has(l)) { pick = l; break; }
            if (!ownerValue && has(scene) && !layers[scene]->b && layers[pick]->b) pick = scene;
        }
        else
        {
            for (size_t l = kLayerCount; l-- > 0;)
                if (has(l)) { pick = l; break; }
        }
        return pick;
    }
}

template <class S>
Request RequestFromText(const RowT<S>& r, const std::string& text)
{
    using T = Request::Type;
    Request q;
    q.type = T::Text;
    q.text = text;
    const std::string low = Lower(text);
    char* end = nullptr;
    const double num = std::strtod(text.c_str(), &end);
    const bool isNumber = !text.empty() && end && *end == '\0';
    switch (r.kind)
    {
    case Kind::Bool:
        if (low == "true" || low == "1" || low == "on" || low == "yes")       { q.type = T::Bool; q.b = true; }
        else if (low == "false" || low == "0" || low == "off" || low == "no") { q.type = T::Bool; q.b = false; }
        break;
    case Kind::Int:
    case Kind::Float:
    case Kind::Choice:
        if (isNumber) { q.type = T::Number; q.number = num; }
        break;
    case Kind::Enum:
        // The index, the label, or the settings.json token ("FXAA_TAA" for
        // "FXAA + TAA" - a label with spaces is awkward to type).
        if (isNumber) { q.type = T::Number; q.number = num; break; }
        for (size_t i = 0; i < r.labels.size(); ++i)
        {
            const bool label = Lower(r.labels[i]) == low;
            const bool token = r.spec.codec && Lower(r.spec.codec->toToken(static_cast<int>(i))) == low;
            if (label || token) { q.type = T::Number; q.number = static_cast<double>(i); break; }
        }
        break;
    case Kind::String:
        break;
    }
    return q;
}

template <class S>
std::string ValueText(const RowT<S>& r, const Value& v)
{
    char buf[48];
    switch (r.kind)
    {
    case Kind::Bool:   return v.b ? "true" : "false";
    case Kind::Int:
    case Kind::Choice: return std::to_string(v.i);
    case Kind::Enum:   return (v.i >= 0 && v.i < static_cast<int>(r.labels.size())) ? r.labels[v.i] : std::to_string(v.i);
    case Kind::Float:  std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.f)); return buf;
    case Kind::String: return v.s;
    }
    return {};
}

// ---- The store ----

template <> SettingsStore& SettingsStore::Get()
{
    static SettingsStore s_store(PlayerRows());
    return s_store;
}

template <> DevSettingsStore& DevSettingsStore::Get()
{
    static DevSettingsStore s_store(DevRows());
    return s_store;
}

template <> PassSettingsStore& PassSettingsStore::Get()
{
    static PassSettingsStore s_store(PassRows());
    return s_store;
}

template <class S>
BasicStore<S>::BasicStore(std::span<const RowType> rows)
    : m_rows(rows), m_slots(rows.size())
{
    // The Default layer is a default-constructed struct: the value a row has
    // when nothing - no file, no menu, no flag - ever set it.
    const S defaults{};
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        Slot& s = m_slots[i];
        s.layers[static_cast<size_t>(Layer::Default)] = ValueOf(m_rows[i], defaults);
        s.effective = *s.layers[static_cast<size_t>(Layer::Default)];
        s.history.reserve(kHistory);
    }
}

template <class S>
size_t BasicStore<S>::Index(const RowType& r) const
{
    // Rows are compared by address: a store holds the rows of one table.
    if (m_rows.empty() || &r < m_rows.data() || &r >= m_rows.data() + m_rows.size())
        return m_rows.size();
    return static_cast<size_t>(&r - m_rows.data());
}

template <class S>
const typename BasicStore<S>::RowType* BasicStore<S>::Find(std::string_view path) const
{
    for (const RowType& r : m_rows)
        if (path == r.path) return &r;
    return nullptr;
}

template <class S>
void BasicStore<S>::CheckThread(const RowType& r)
{
    if (m_owner == std::thread::id{})
        m_owner = std::this_thread::get_id();
    ENGINE_CHECK_ONCE(std::this_thread::get_id() == m_owner,
                      "SettingsStore: '%s' written from a second thread - the store is main-thread only",
                      r.path);
}

template <class S>
void BasicStore<S>::Remember(Slot& s, WriteRecord rec)
{
    if (s.history.size() < kHistory) s.history.push_back(std::move(rec));
    else                             s.history[s.next] = std::move(rec);
    s.next = (s.next + 1) % kHistory;
}

template <class S>
bool BasicStore<S>::Recompute(const RowType& r, Slot& s)
{
    const size_t pick = PickLayer(r, s.layers, s.seq, /*ownerValue*/false);
    s.source = static_cast<Layer>(pick);
    if (SameValue(r, s.effective, *s.layers[pick]))
        return false;
    s.effective = *s.layers[pick];
    s.revision  = ++m_revision;
    return true;
}

template <class S>
bool BasicStore<S>::Write(const RowType& r, Layer layer, const Value& v, std::string_view writer)
{
    const size_t i = Index(r);
    if (i == m_rows.size() || layer == Layer::Default || layer == Layer::Count) return false;
    CheckThread(r);
    Slot& s = m_slots[i];
    const size_t l = static_cast<size_t>(layer);
    s.layers[l] = v;
    s.seq[l]    = ++m_seq;
    Remember(s, WriteRecord{ layer, false, v, Logger::CurrentFrame(), std::string(writer) });
    if (Recompute(r, s) && m_observer) m_observer(r);
    return true;
}

template <class S>
bool BasicStore<S>::Clear(const RowType& r, Layer layer, std::string_view writer)
{
    const size_t i = Index(r);
    if (i == m_rows.size() || layer == Layer::Default || layer == Layer::Count) return false;
    CheckThread(r);
    Slot& s = m_slots[i];
    const size_t l = static_cast<size_t>(layer);
    if (!s.layers[l]) return true;
    s.layers[l].reset();
    s.seq[l] = 0;
    Remember(s, WriteRecord{ layer, true, Value{}, Logger::CurrentFrame(), std::string(writer) });
    if (Recompute(r, s) && m_observer) m_observer(r);
    return true;
}

template <class S>
void BasicStore<S>::WriteRows(const S& s, Layer layer, std::string_view writer, std::span<const uint8_t> mask)
{
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (mask.empty() || (i < mask.size() && mask[i]))
            Write(m_rows[i], layer, ValueOf(m_rows[i], s), writer);
}

template <class S>
const Value& BasicStore<S>::Effective(const RowType& r) const
{
    static const Value kNone;
    const size_t i = Index(r);
    if (!ENGINE_CHECK(i < m_rows.size(), "SettingsStore::Effective: '%s' is not a row of this store", r.path))
        return kNone;
    return m_slots[i].effective;
}

template <class S>
Layer BasicStore<S>::Source(const RowType& r) const
{
    const size_t i = Index(r);
    return i < m_rows.size() ? m_slots[i].source : Layer::Default;
}

template <class S>
const Value& BasicStore<S>::OwnerValue(const RowType& r) const
{
    if (r.spec.policy != Policy::Capped || r.kind != Kind::Bool)
        return Effective(r);
    static const Value kNone;
    const size_t i = Index(r);
    if (!ENGINE_CHECK(i < m_rows.size(), "SettingsStore::OwnerValue: '%s' is not a row of this store", r.path))
        return kNone;
    // The allowance: Recompute's pick before the scene's intent. Default
    // always holds a value, so the loop always finds one.
    const Slot&  s     = m_slots[i];
    const size_t scene = static_cast<size_t>(Layer::Scene);
    for (size_t l = kLayerCount; l-- > 0;)
        if (l != scene && s.layers[l]) return *s.layers[l];
    return kNone;
}

template <class S>
Value BasicStore<S>::OwnerValueWith(const RowType& r, std::span<const LayerSub> subs) const
{
    const size_t i = Index(r);
    if (!ENGINE_CHECK(i < m_rows.size(), "SettingsStore::OwnerValueWith: '%s' is not a row of this store", r.path))
        return Value{};
    // The row's layers as the writer will leave them; Default is never
    // replaced, so the pick always finds a value.
    std::array<std::optional<Value>, kLayerCount> layers = m_slots[i].layers;
    std::array<uint64_t, kLayerCount>             seq    = m_slots[i].seq;
    uint64_t next = m_seq;
    for (const LayerSub& sub : subs)
    {
        const size_t l = static_cast<size_t>(sub.layer);
        if (sub.layer == Layer::Default || l >= kLayerCount) continue;
        if (sub.value) { layers[l] = *sub.value; seq[l] = ++next; }
        else           { layers[l].reset();      seq[l] = 0; }
    }
    return *layers[PickLayer(r, layers, seq, /*ownerValue*/true)];
}

template <class S>
const std::optional<Value>& BasicStore<S>::At(const RowType& r, Layer l) const
{
    static const std::optional<Value> kNone;
    const size_t i = Index(r);
    if (i == m_rows.size() || l == Layer::Count) return kNone;
    return m_slots[i].layers[static_cast<size_t>(l)];
}

template <class S>
uint64_t BasicStore<S>::RowRevision(const RowType& r) const
{
    const size_t i = Index(r);
    return i < m_rows.size() ? m_slots[i].revision : 0;
}

template <class S>
std::vector<WriteRecord> BasicStore<S>::History(const RowType& r) const
{
    std::vector<WriteRecord> out;
    const size_t i = Index(r);
    if (i == m_rows.size()) return out;
    const Slot& s = m_slots[i];
    // Before the ring wraps, `next` == size and the order is already right.
    const size_t n = s.history.size();
    const size_t start = n < kHistory ? 0 : s.next;
    out.reserve(n);
    for (size_t k = 0; k < n; ++k) out.push_back(s.history[(start + k) % n]);
    return out;
}

template <class S>
size_t BasicStore<S>::CountIn(Layer l) const
{
    if (l == Layer::Count) return 0;
    size_t n = 0;
    for (const Slot& s : m_slots)
        if (s.layers[static_cast<size_t>(l)]) ++n;
    return n;
}

template <class S>
size_t BasicStore<S>::CountPolicy(Policy p) const
{
    size_t n = 0;
    for (const RowType& r : m_rows)
        if (r.spec.policy == p) ++n;
    return n;
}

void LogBootSummary()
{
    const SettingsStore& st = SettingsStore::Get();
    LOG_INFO("[SETTINGS] rows=%zu player=%zu project=%zu scene=%zu cmdline=%zu runtime=%zu lastWrite=%zu capped=%zu",
             st.Rows().size(), st.CountIn(Layer::Player), st.CountIn(Layer::Project),
             st.CountIn(Layer::Scene), st.CountIn(Layer::CommandLine), st.CountIn(Layer::Runtime),
             st.CountPolicy(Policy::LastWrite), st.CountPolicy(Policy::Capped));
}

void LogDevBootSummary()
{
    // No developer row has a Player or Scene writer: only the layers that do.
    const DevSettingsStore& st = DevSettingsStore::Get();
    LOG_INFO("[SETTINGS] developer rows=%zu project=%zu cmdline=%zu runtime=%zu lastWrite=%zu",
             st.Rows().size(), st.CountIn(Layer::Project), st.CountIn(Layer::CommandLine),
             st.CountIn(Layer::Runtime), st.CountPolicy(Policy::LastWrite));
}

// ---- One instantiation per document ----
#define SETTINGS_STORE_INSTANTIATE(S)                                                     \
    template Value       ValueOf(const RowT<S>&, const S&);                               \
    template void        PutValue(const RowT<S>&, const Value&, S&);                      \
    template bool        SameValue(const RowT<S>&, const Value&, const Value&);           \
    template Refusal     Coerce(const RowT<S>&, const Request&, Value&);                  \
    template Request     RequestFromText(const RowT<S>&, const std::string&);             \
    template std::string ValueText(const RowT<S>&, const Value&);                         \
    template class BasicStore<S>;
SETTINGS_STORE_INSTANTIATE(Resource::EngineSettings)
SETTINGS_STORE_INSTANTIATE(Resource::EngineDevSettings)
SETTINGS_STORE_INSTANTIATE(Resource::PassSettings)
#undef SETTINGS_STORE_INSTANTIATE
}
