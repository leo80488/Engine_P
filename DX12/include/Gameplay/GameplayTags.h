#pragma once

// Gameplay tags: dotted names ("State.IFrame", "Team.Enemy") interned to a 32-bit id, and a counting
// container per entity.
//   * A Tag is the FNV-1a hash of the full dotted name, so an id is the same in every process and a
//     save, a table or a script can name it by text; the TagTable remembers the name and the parent
//     ("Team" for "Team.Enemy") and refuses a name whose hash another name already holds.
//   * A name is 1..8 segments of [A-Za-z0-9_], at most 63 characters; case matters.
//   * TagCounts counts per tag: Add / Remove stack (two sources granting the same tag need two
//     removes), Has asks for that exact tag, Matches asks for the tag or anything below it
//     (Matches("Team") is true for an entity holding "Team.Enemy"). It is a small sorted vector:
//     an entity carries a handful of tags.
// Main thread, except TagTable, whose reads and interning take a lock (a notify is read on a worker).

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Gameplay
{
    struct Tag
    {
        uint32_t id = 0;   // 0 = no tag

        bool IsValid() const { return id != 0; }
        bool operator==(Tag o) const { return id == o.id; }
        bool operator!=(Tag o) const { return id != o.id; }
        bool operator<(Tag o) const { return id < o.id; }
    };

    class TagTable
    {
    public:
        static TagTable& Get();

        // True when @p name is a legal dotted name; otherwise false, with the reason in @p error.
        static bool ValidName(std::string_view name, std::string* error = nullptr);

        // The tag for @p name, interning it and every ancestor ("A.B.C" also interns "A" and "A.B").
        // An invalid Tag, with the reason in @p error, for an illegal name or a hash another name holds.
        Tag Intern(std::string_view name, std::string* error = nullptr);
        // The tag for @p name when it has been interned, else an invalid Tag. Never interns.
        Tag Find(std::string_view name) const;

        // The dotted name; empty for an invalid or unknown tag.
        std::string Name(Tag tag) const;
        // The tag one segment up; invalid for a root.
        Tag Parent(Tag tag) const;
        // True when @p tag is @p ancestor or is below it.
        bool IsSelfOrBelow(Tag tag, Tag ancestor) const;
        size_t Size() const;

    private:
        struct Node
        {
            std::string name;
            Tag         parent;
        };

        Tag InternLocked(std::string_view name, std::string* error);

        mutable std::shared_mutex             m_lock;
        std::unordered_map<uint32_t, Node>    m_nodes;
    };

    // Splits "A.B, C.D" into interned tags. False, with the first illegal name in @p error, on a bad
    // one; the tags read before it stay in @p out.
    bool ParseTagList(std::string_view text, std::vector<Tag>& out, std::string* error = nullptr);

    class TagCounts
    {
    public:
        // Adds @p n to the tag's own count; returns the count after. An invalid tag adds nothing.
        int Add(Tag tag, int n = 1);
        // Takes up to @p n off the tag's own count (never below zero); returns the count after.
        int Remove(Tag tag, int n = 1);
        // Drops the tag's own count.
        void Clear(Tag tag);
        void ClearAll() { m_entries.clear(); }

        // The tag's own count.
        int  Count(Tag tag) const;
        bool Has(Tag tag) const { return Count(tag) > 0; }
        // The tag or any tag below it is held.
        bool Matches(Tag tag) const;
        bool MatchesAll(const std::vector<Tag>& tags) const;
        bool MatchesAny(const std::vector<Tag>& tags) const;

        bool   Empty() const { return m_entries.empty(); }
        // Every tag with a count of its own, ascending by id: fn(Tag, int count).
        template <class Fn>
        void ForEach(Fn&& fn) const
        {
            for (const Entry& e : m_entries)
                if (e.own > 0) fn(Tag{ e.id }, e.own);
        }

        // "State.IFrame:1,Team.Enemy:2" - the scene / save text form.
        std::string ToText() const;
        // Replaces the contents from ToText's form; false, with the first problem in @p error, when an
        // entry is malformed (the entries before it are kept).
        bool FromText(std::string_view text, std::string* error = nullptr);

    private:
        struct Entry
        {
            uint32_t id    = 0;
            int32_t  own   = 0;   // added to this tag
            int32_t  below = 0;   // added to a tag under it
        };

        Entry* Find(uint32_t id);
        const Entry* Find(uint32_t id) const;
        Entry& Obtain(uint32_t id);
        void   Adjust(Tag tag, int delta);

        std::vector<Entry> m_entries;   // sorted by id
    };

    // The tags the engine itself reads.
    namespace Std
    {
        Tag IFrame();      // "State.IFrame"
        Tag Invincible();  // "State.Invincible"
    }
}
