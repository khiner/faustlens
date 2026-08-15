// The reference's `.type` notation: a `Size = N` header then N lexically sorted
// `Type = ...` lines, one per node and not per type. The five letters are nature,
// variability, computability, vectorability and boolean, a tuplet printing two.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens::test {

struct TypeEntry {
    enum class Shape { Simple, Tuplet, Table };

    Shape Shape = Shape::Simple;
    std::string Code;
    char Nature = 0; // 'N' or 'R'
    char Variability = 0; // 'K', 'B' or 'S'
    double Lo = 0, Hi = 0;
    int64_t Lsb = 0; // read but not compared
    std::vector<TypeEntry> Members; // a `Tuplet`'s fields, a `Table`'s one content type
};

struct TypeFile {
    int32_t Size = -1; // the `Size = N` header, -1 if absent
    std::vector<TypeEntry> Types;
};

// Unexpected gets the offending line and a reason.
std::expected<TypeFile, std::string> ParseType(std::string_view text);

// A comparable key: nature, variability, the bounds, and the shape.
std::string TypeKey(const TypeEntry &);

std::string PrintTypeEntry(const TypeEntry &);

} // namespace faustlens::test
