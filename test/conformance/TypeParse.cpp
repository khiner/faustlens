#include "conformance/TypeParse.h"

#include "property/Corpus.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace faustlens::test {
namespace {

struct Cursor {
    std::string_view S;
    size_t I = 0;

    bool Done() const { return I >= S.size(); }
    char Peek() const { return I < S.size() ? S[I] : '\0'; }
    void Skip() {
        while (I < S.size() && (S[I] == ' ' || S[I] == '\t')) ++I;
    }
    bool Take(std::string_view lit) {
        Skip();
        if (S.compare(I, lit.size(), lit) != 0) return false;
        I += lit.size();
        return true;
    }
};

// `strtod` reads both spellings of an unbounded side, `inf` and `1.79769e+308`.
bool Number(Cursor &c, double &out) {
    c.Skip();
    const std::string text(c.S.substr(c.I));
    char *end = nullptr;
    out = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    c.I += size_t(end - text.c_str());
    return true;
}

bool Interval(Cursor &c, TypeEntry &t) {
    if (!c.Take("interval(")) return false;
    double lsb = 0;
    if (!Number(c, t.Lo) || !c.Take(",") || !Number(c, t.Hi) || !c.Take(",") || !Number(c, lsb)) return false;
    t.Lsb = int64_t(lsb);
    return c.Take(")");
}

bool Header(Cursor &c, TypeEntry &t) {
    c.Skip();
    const size_t begin = c.I;
    while (c.I < c.S.size() && (std::isalpha(static_cast<unsigned char>(c.S[c.I])) || c.S[c.I] == '?')) ++c.I;
    if (c.I == begin) return false;
    t.Code = std::string(c.S.substr(begin, c.I - begin));
    // A tuplet's two letters carry only a variability.
    if (t.Code.size() >= 5) {
        t.Nature = t.Code[0];
        t.Variability = t.Code[1];
    } else if (!t.Code.empty()) {
        t.Variability = t.Code[0];
    }
    return true;
}

bool Entry(Cursor &c, TypeEntry &t, bool nested);

// Fields separate with `*`. A comma appears only inside an interval.
bool Members(Cursor &c, TypeEntry &t) {
    for (;;) {
        TypeEntry m;
        if (!Entry(c, m, true)) return false;
        t.Members.push_back(std::move(m));
        if (c.Take("*")) continue;
        return true;
    }
}

bool Entry(Cursor &c, TypeEntry &t, bool nested) {
    if (!Header(c, t) || !Interval(c, t)) return false;
    if (nested) return true; // members are simple in every corpus file
    if (c.Take(":Table(")) {
        t.Shape = TypeEntry::Shape::Table;
        TypeEntry content;
        if (!Entry(c, content, true)) return false;
        t.Members.push_back(std::move(content));
        return c.Take(")");
    }
    if (c.Take(": {")) {
        t.Shape = TypeEntry::Shape::Tuplet;
        if (!Members(c, t)) return false;
        return c.Take("}");
    }
    return true;
}

std::string Num(double v) {
    if (v == 0) return "0"; // so -0 and 0 do not read as different bounds
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

} // namespace

std::expected<TypeFile, std::string> ParseType(std::string_view text) {
    TypeFile out;
    std::string why;
    int line_no = 0;
    const bool lines_ok = ForEachLine(text, [&](std::string_view line) {
        ++line_no;
        if (line.empty()) return true;

        Cursor c{line};
        if (c.Take("Size")) {
            double n = 0;
            if (!c.Take("=") || !Number(c, n)) {
                why = std::format("line {}: malformed Size header", line_no);
                return false;
            }
            out.Size = int32_t(n);
            return true;
        }
        if (!c.Take("Type") || !c.Take("=")) {
            why = std::format("line {}: expected `Type = `, got `{}`", line_no, line);
            return false;
        }
        TypeEntry t;
        if (!Entry(c, t, false)) {
            why = std::format("line {}: malformed type `{}`", line_no, line);
            return false;
        }
        c.Skip();
        if (!c.Done()) {
            why = std::format("line {}: trailing text in `{}`", line_no, line);
            return false;
        }
        out.Types.push_back(std::move(t));
        return true;
    });
    if (!lines_ok) return std::unexpected(std::move(why));
    if (out.Size >= 0 && size_t(out.Size) != out.Types.size()) return std::unexpected(std::format("Size says {} but read {}", out.Size, out.Types.size()));
    return out;
}

std::string TypeKey(const TypeEntry &t) {
    // Computability, vectorability, boolean and the lsb are deliberately absent.
    std::string out;
    out += t.Nature ? t.Nature : '_';
    out += t.Variability ? t.Variability : '_';
    out += "[" + Num(t.Lo) + "," + Num(t.Hi) + "]";
    switch (t.Shape) {
        case TypeEntry::Shape::Tuplet: out += "{"; break;
        case TypeEntry::Shape::Table: out += "T("; break;
        case TypeEntry::Shape::Simple: return out;
    }
    for (size_t i = 0; i < t.Members.size(); ++i) {
        if (i) out += ",";
        out += TypeKey(t.Members[i]);
    }
    out += t.Shape == TypeEntry::Shape::Tuplet ? "}" : ")";
    return out;
}

std::string PrintTypeEntry(const TypeEntry &t) {
    std::string out = std::format("{} interval({},{},{})", t.Code, Num(t.Lo), Num(t.Hi), t.Lsb);
    if (t.Shape == TypeEntry::Shape::Table) return std::format("{}:Table({})", out, PrintTypeEntry(t.Members[0]));
    if (t.Shape == TypeEntry::Shape::Tuplet) {
        out += " : {";
        for (size_t i = 0; i < t.Members.size(); ++i) {
            if (i) out += ", ";
            out += PrintTypeEntry(t.Members[i]);
        }
        out += "}";
    }
    return out;
}

} // namespace faustlens::test
