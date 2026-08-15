#include "conformance/FirParse.h"

#include "property/Corpus.h"

#include <cctype>
#include <cstdlib>
#include <format>
#include <utility>

namespace faustlens::test {

namespace {

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// One line's expression grammar: `Name`, `Str`, `Num` or a bracketed `Call`.
struct TermParser {
    std::string_view S;
    std::string &Why;
    size_t I = 0;

    bool ParseAll(FirTerm &out) {
        if (!Term(out)) return false;
        Space();
        if (I != S.size()) return Fail("trailing text");
        return true;
    }

    bool Fail(const std::string &m) {
        if (Why.empty()) Why = std::format("{} at offset {}", m, I);
        return false;
    }

    void Space() {
        while (I < S.size() && (S[I] == ' ' || S[I] == '\t')) ++I;
    }
    bool At(char c) const { return I < S.size() && S[I] == c; }

    bool Term(FirTerm &out) {
        if (!Primary(out)) return false;
        while (true) {
            Space();
            // The one member access, kept as a call so the vocabulary pin sees it.
            if (I + 1 < S.size() && S[I] == '-' && S[I + 1] == '>') {
                I += 2;
                FirTerm base = std::move(out);
                out = FirTerm{};
                out.Kind = FirTerm::Kind::Call;
                out.Name = "->";
                out.Args.push_back(std::move(base));
                FirTerm member;
                Space();
                if (I >= S.size() || !IsIdentStart(S[I])) return Fail("expected a field name");
                if (!Ident(member)) return false;
                out.Args.push_back(std::move(member));
                continue;
            }
            if (!At('[')) break;
            ++I;
            FirTerm idx;
            Space();
            if (At(']')) { // `fRec0[]`, an array of unstated size
                ++I;
                out.Index.push_back(FirTerm{});
                continue;
            }
            if (!Term(idx)) return false;
            Space();
            if (!At(']')) return Fail("expected `]`");
            ++I;
            out.Index.push_back(std::move(idx));
        }
        return true;
    }

    bool Primary(FirTerm &out) {
        Space();
        if (I >= S.size()) return Fail("expected a term");
        const char c = S[I];
        if (c == '"') return Str(out);
        if (c == '(' || c == '{') { // an unnamed group: a `StructType` field
            out.Kind = FirTerm::Kind::Call;
            out.Bracket = c;
            return Args(out);
        }
        // `AddSoundfile(..., &fSoundfile0)` is the one address-of.
        if (c == '&') {
            ++I;
            out.Kind = FirTerm::Kind::Call;
            out.Name = "&";
            FirTerm a;
            if (!Term(a)) return false;
            out.Args.push_back(std::move(a));
            return true;
        }
        if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) return Num(out);
        if (IsIdentStart(c)) return Ident(out);
        return Fail("unexpected character");
    }

    bool Str(FirTerm &out) {
        out.Kind = FirTerm::Kind::Str;
        ++I;
        while (I < S.size() && S[I] != '"') {
            if (S[I] == '\\' && I + 1 < S.size()) ++I;
            out.Name.push_back(S[I++]);
        }
        if (I >= S.size()) return Fail("unterminated string");
        ++I;
        return true;
    }

    bool Num(FirTerm &out) {
        const size_t start = I;
        if (At('-') || At('+')) ++I;
        while (I < S.size() && (std::isdigit(static_cast<unsigned char>(S[I])) || S[I] == '.')) ++I;
        if (I < S.size() && (S[I] == 'e' || S[I] == 'E')) {
            ++I;
            if (At('-') || At('+')) ++I;
            while (I < S.size() && std::isdigit(static_cast<unsigned char>(S[I]))) ++I;
        }
        if (I == start) return Fail("expected a number");
        out.Kind = FirTerm::Kind::Num;
        out.Name = std::string(S.substr(start, I - start));
        out.Num = std::strtod(out.Name.c_str(), nullptr);
        return true;
    }

    bool Ident(FirTerm &out) {
        const size_t start = I;
        while (I < S.size() && IsIdent(S[I])) ++I;
        while (I + 1 < S.size() && S[I] == '|' && IsIdentStart(S[I + 1])) {
            ++I;
            while (I < S.size() && IsIdent(S[I])) ++I;
        }
        out.Name = std::string(S.substr(start, I - start));
        // No `Space()`, so a bare `BlockInst` cannot swallow the next line.
        if (I < S.size() && (S[I] == '(' || S[I] == '{' || S[I] == '<')) {
            out.Kind = FirTerm::Kind::Call;
            out.Bracket = S[I];
            return Args(out);
        }
        if (I < S.size() && S[I] == '"') {
            out.Kind = FirTerm::Kind::Call;
            out.Bracket = '(';
            return ArgsBody(out, ')');
        }
        out.Kind = FirTerm::Kind::Name;
        return true;
    }

    bool Args(FirTerm &out) {
        const char open = S[I++];
        const char close = open == '(' ? ')' : open == '{' ? '}' : '>';
        return ArgsBody(out, close);
    }

    bool ArgsBody(FirTerm &out, char close) {
        while (true) {
            Space();
            if (I >= S.size()) return Fail("unterminated argument list");
            if (S[I] == close) {
                ++I;
                return true;
            }
            if (S[I] == ',') {
                ++I;
                continue;
            }
            FirTerm a;
            if (!Term(a)) return false;
            out.Args.push_back(std::move(a));
        }
    }
};

// `======= NAME ==========`, in both widths the printer uses.
bool Marker(const std::string &line, std::string &name) {
    if (line.size() < 3 || line[0] != '=') return false;
    size_t a = 0;
    while (a < line.size() && line[a] == '=') ++a;
    size_t b = line.size();
    while (b > a && line[b - 1] == '=') --b;
    if (b == line.size()) return false;
    name = Trim(std::string_view(line).substr(a, b - a));
    return true;
}

bool Ends(const std::string &s, const char *suffix) {
    const std::string t = suffix;
    return s.size() >= t.size() && s.ends_with(t);
}

const char *Closer(const std::string &opener) {
    if (opener == "BlockInst") return "EndBlockInst";
    if (opener == "ForLoopInst") return "EndForLoopInst";
    if (opener == "IfInst") return "EndIfInst";
    if (opener == "DeclareFunInst") return "EndDeclare";
    return nullptr;
}

size_t Indent(std::string_view line) {
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
    return n;
}

} // namespace

std::expected<FirFile, std::string> ParseFir(std::string_view text) {
    FirFile out;
    std::vector<std::string> lines;
    ForEachLine(text, [&](std::string_view line) {
        lines.emplace_back(line);
        return true;
    });

    out.Sections.clear();
    out.Sections.push_back(FirSection{"", {}, {}}); // anything before the first marker

    struct Frame {
        std::vector<FirStmt> *Into;
        std::string Closer;
    };
    std::vector<Frame> stack{{&out.Sections.back().Stmts, ""}};

    const auto fail = [&](size_t n, const std::string &m) { return std::unexpected(std::format("line {}: {} -- `{}`", n + 1, m, Trim(lines[n]))); };

    for (size_t n = 0; n < lines.size(); ++n) {
        const std::string body(Trim(lines[n]));
        if (body.empty()) continue;

        std::string marker;
        if (Marker(body, marker)) {
            // Inside an open block a marker is a sub-heading, not a section.
            if (stack.size() > 1) {
                out.Sections.back().Notes.push_back(body);
                continue;
            }
            if (Ends(marker, " end")) continue; // the section it closes is already recorded
            std::string name = marker;
            if (Ends(name, " begin")) name.resize(name.size() - 6);
            if (name.starts_with("Container ") && out.Container.empty()) {
                FirTerm t;
                std::string ignored;
                if (TermParser{std::string_view(name).substr(10), ignored}.ParseAll(t)) out.Container = t.Name;
            }
            out.Sections.push_back(FirSection{name, {}, {}});
            stack.assign(1, Frame{&out.Sections.back().Stmts, ""});
            continue;
        }

        // A closer has to match: unwinding to the wrong keyword reparents everything under it.
        if (body == "EndBlockInst" || body == "EndForLoopInst" || body == "EndIfInst" || body == "EndDeclare") {
            if (stack.size() > 1 && body == stack.back().Closer) {
                stack.pop_back();
                continue;
            }
            return fail(n, std::format("unmatched `{}`, expected `{}`", body, stack.size() > 1 ? stack.back().Closer : "no closer"));
        }

        if (!IsIdentStart(body[0])) {
            out.Sections.back().Notes.push_back(body);
            continue;
        }
        size_t k = 0;
        while (k < body.size() && IsIdent(body[k])) ++k;
        const std::string head = body.substr(0, k);

        FirTerm term;
        std::string sub;
        if (!TermParser{body, sub}.ParseAll(term)) {
            out.Sections.back().Notes.push_back(body);
            continue;
        }

        const char *closer = Closer(head);
        bool opens = closer != nullptr;
        if (head == "DeclareFunInst") {
            opens = false;
            for (size_t m = n + 1; m < lines.size(); ++m) {
                if (Trim(lines[m]).empty()) continue;
                opens = Indent(lines[m]) > Indent(lines[n]);
                break;
            }
        }

        stack.back().Into->push_back(FirStmt{std::move(term), {}});
        if (opens) stack.push_back(Frame{&stack.back().Into->back().Body, closer});
    }

    if (stack.size() > 1) return std::unexpected("unterminated `" + stack.back().Closer + "` at end of file");
    // The leading section only catches a file not starting with a marker.
    if (!out.Sections.empty() && out.Sections.front().Name.empty() && out.Sections.front().Stmts.empty() && out.Sections.front().Notes.empty())
        out.Sections.erase(out.Sections.begin());
    return out;
}

std::string PrintFirTerm(const FirTerm &t) {
    std::string s;
    switch (t.Kind) {
        case FirTerm::Kind::Str: s = "\"" + t.Name + "\""; break;
        case FirTerm::Kind::Num:
        case FirTerm::Kind::Name: s = t.Name; break;
        case FirTerm::Kind::Call: {
            const char close = t.Bracket == '(' ? ')' : t.Bracket == '{' ? '}' : '>';
            s = t.Name + t.Bracket;
            for (size_t i = 0; i < t.Args.size(); ++i) {
                if (i) s += ", ";
                s += PrintFirTerm(t.Args[i]);
            }
            s += close;
            break;
        }
    }
    for (const FirTerm &i : t.Index) s += "[" + PrintFirTerm(i) + "]";
    return s;
}

} // namespace faustlens::test
