#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

// Root and parent path prefixes.
inline constexpr uint8_t PathRoot = 0xFE;
inline constexpr uint8_t PathParent = 0xFD;

struct PathSeg {
    std::string Name;
    uint8_t Group = 0; // 0 `v`, 1 `h`, 2 `t`
    bool IsGroup = true;
    bool operator==(const PathSeg &) const = default;

    bool Root() const { return IsGroup && Group == PathRoot; }
    bool Parent() const { return IsGroup && Group == PathParent; }
};

// Return path segments outermost first, including an empty leaf after a trailing group.
std::vector<PathSeg> LabelToPath(std::string_view label);

// Return the slash-separated path in reference .sig order, innermost first.
std::string PathText(std::span<const PathSeg>);

// Parse backslash escapes and nested brackets; trim key, value, and label whitespace.
void ExtractMetadata(std::string_view full, std::string &label, std::map<std::string, std::set<std::string>> &meta);

std::string LabelOnly(std::string_view full);

} // namespace faustlens
