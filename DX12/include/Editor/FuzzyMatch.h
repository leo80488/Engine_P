#pragma once

// FuzzyMatch - ranks a label against a typed query the way command palettes do: every query
// character must appear in the label in order (case-insensitive); consecutive runs, word starts and
// an early first hit score higher, so "svsc" finds "Save Scene" and "prof" puts "Profiler" first.
// Header-only; the command palette, the Inspector's field search and the asset search share it.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Editor::Fuzzy
{
    struct Match
    {
        bool                    matched = false;
        int                     score   = 0;     // higher is better; only meaningful when matched
        std::vector<std::size_t> positions;      // label byte index of each matched query character
    };

    inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

    inline bool IsWordStart(std::string_view label, std::size_t i)
    {
        if (i == 0) return true;
        const char prev = label[i - 1];
        const char cur  = label[i];
        if (prev == ' ' || prev == '_' || prev == '-' || prev == '/' || prev == '.' || prev == ':' || prev == '(')
            return true;
        return (prev >= 'a' && prev <= 'z') && (cur >= 'A' && cur <= 'Z');   // camelCase boundary
    }

    // Spaces in @p query are ignored (they only separate words for the reader). An empty query
    // matches everything with score 0. Greedy left to right, preferring a word start or the
    // character right after the previous hit when one exists before the next plain hit.
    inline Match Score(std::string_view label, std::string_view query)
    {
        Match m;
        std::size_t li = 0;
        int  run = 0;
        bool any = false;
        for (std::size_t qi = 0; qi < query.size(); ++qi)
        {
            const char q = Lower(query[qi]);
            if (q == ' ') continue;
            // The first plain occurrence, and a better one (word start / adjacent) before the end.
            std::size_t first = std::string_view::npos;
            std::size_t best  = std::string_view::npos;
            for (std::size_t i = li; i < label.size(); ++i)
            {
                if (Lower(label[i]) != q) continue;
                if (first == std::string_view::npos) first = i;
                const bool adjacent = any && !m.positions.empty() && i == m.positions.back() + 1;
                if (adjacent || IsWordStart(label, i)) { best = i; break; }
            }
            const std::size_t at = best != std::string_view::npos ? best : first;
            if (at == std::string_view::npos) { m.matched = false; m.score = 0; m.positions.clear(); return m; }

            int s = 1;
            const bool adjacent = any && at == m.positions.back() + 1;
            run = adjacent ? run + 1 : 0;
            if (adjacent) s += 4 + run;            // consecutive characters
            if (IsWordStart(label, at)) s += 6;     // the start of a word
            if (!any) s += at == 0 ? 8 : (at < 4 ? 3 : 0);   // an early first hit
            if (label[at] == query[qi]) s += 1;     // exact case
            m.score += s;
            m.positions.push_back(at);
            any = true;
            li = at + 1;
        }
        m.matched = true;
        // A shorter label with the same hits is the more specific one.
        m.score -= static_cast<int>(label.size() / 8);
        return m;
    }
}
