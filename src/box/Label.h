// A widget label carries two grammars: a `/`-separated group path and `[key:value]`
// metadata, both read here.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

// The `/` and `../` prefixes, which name no group.
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

// Top-down: the first segment is the outermost, the last the widget's own name, empty
// where the label ends in a group.
std::vector<PathSeg> LabelToPath(std::string_view label);

// The path as `.sig` prints it: `/`-separated and innermost first.
std::string PathText(std::span<const PathSeg>);

// Escapes are `\`-prefixed, brackets nest, and key, value and label are blank-stripped.
void ExtractMetadata(std::string_view full, std::string &label, std::map<std::string, std::set<std::string>> &meta);

// The label alone, with whatever metadata it carried dropped.
std::string LabelOnly(std::string_view full);

} // namespace faustlens
