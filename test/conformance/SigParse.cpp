#include "conformance/SigParse.h"

#include "property/Corpus.h"

#include <cstdlib>
#include <format>

namespace faustlens::test {
namespace {

// The reference's operator names and priorities, plus `@`, which prints as an infix at 8.
struct InfixOp {
    std::string_view Name;
    int Priority;
};
constexpr InfixOp Infix[] = {
    {">>>", 6}, {"<<", 6}, {">>", 6}, {">=", 5}, {"<=", 5}, {"==", 4}, {"!=", 4}, {"*", 8}, {"/", 8},
    {"%", 8},   {"@", 8},  {"+", 7},  {"-", 7},  {">", 5},  {"<", 5},  {"&", 3},  {"^", 2}, {"|", 1},
};

struct Parser {
    std::string_view S;
    size_t At = 0;

    bool ParseLine(SigTerm &out, std::string &why) {
        SkipSpace();
        if (!Expr(1, out, why)) return false;
        SkipSpace();
        if (!Eat(';')) return Fail(why, "expected `;`");
        SkipSpace();
        if (At != S.size()) return Fail(why, "trailing text");
        return true;
    }

    // Forces `SIG` to a list even at one output, where the printer wrote `(ID_29)`.
    bool ParseList(SigTerm &out, std::string &why) {
        SkipSpace();
        if (!Peek('(')) return Fail(why, "expected `(`");
        if (!Group(out, why)) return false;
        if (out.Kind != SigTerm::Kind::List) {
            SigTerm inner = std::move(out);
            out = SigTerm{};
            out.Kind = SigTerm::Kind::List;
            out.Args.push_back(std::move(inner));
        }
        SkipSpace();
        if (!Eat(';')) return Fail(why, "expected `;`");
        return true;
    }

    bool Fail(std::string &why, std::string_view what) const {
        why = std::format("{} at offset {}", what, At);
        return false;
    }

    void SkipSpace() {
        while (At < S.size() && (S[At] == ' ' || S[At] == '\t')) ++At;
    }
    bool Peek(char c) const { return At < S.size() && S[At] == c; }
    bool Eat(char c) {
        if (!Peek(c)) return false;
        ++At;
        return true;
    }
    bool Starts(std::string_view w) const { return S.compare(At, w.size(), w) == 0; }

    static bool IdentChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

    // Longest match, so `>=` wins over `>`. Only consulted after an operand, so a `-` here
    // never opens a negative literal.
    const InfixOp *PeekInfix() const {
        if (At >= S.size()) return nullptr;
        const InfixOp *best = nullptr;
        for (const InfixOp &op : Infix)
            if (Starts(op.Name) && (!best || op.Name.size() > best->Name.size())) best = &op;
        return best;
    }

    bool Expr(int min_priority, SigTerm &out, std::string &why) {
        if (!Postfix(out, why)) return false;
        for (;;) {
            SkipSpace();
            const InfixOp *op = PeekInfix();
            if (!op || op->Priority < min_priority) return true;
            At += op->Name.size();
            SigTerm rhs;
            if (!Expr(op->Priority + 1, rhs, why)) return false;
            SigTerm node;
            node.Kind = SigTerm::Kind::Op;
            node.Text = std::string(op->Name);
            node.Args.push_back(std::move(out));
            node.Args.push_back(std::move(rhs));
            out = std::move(node);
        }
    }

    bool Postfix(SigTerm &out, std::string &why) {
        if (!Primary(out, why)) return false;
        while (Peek('\'')) { // `x'`, a one-sample delay
            ++At;
            SigTerm node;
            node.Kind = SigTerm::Kind::Op;
            node.Text = "'";
            node.Args.push_back(std::move(out));
            out = std::move(node);
        }
        return true;
    }

    bool Number(SigTerm &out, std::string &why) {
        const size_t begin = At;
        if (Peek('-') || Peek('+')) ++At;
        bool real = false;
        while (At < S.size() && S[At] >= '0' && S[At] <= '9') ++At;
        if (Peek('.')) {
            real = true;
            ++At;
            while (At < S.size() && S[At] >= '0' && S[At] <= '9') ++At;
        }
        if (At < S.size() && (S[At] == 'e' || S[At] == 'E')) {
            real = true;
            ++At;
            if (Peek('-') || Peek('+')) ++At;
            while (At < S.size() && S[At] >= '0' && S[At] <= '9') ++At;
        }
        const std::string text(S.substr(begin, At - begin));
        if (text.empty()) return Fail(why, "expected a number");
        // Int against real is by spelling: a real always has a `.` or an exponent.
        out = SigTerm{};
        if (real) {
            out.Kind = SigTerm::Kind::Real;
            out.D = std::strtod(text.c_str(), nullptr);
        } else {
            out.Kind = SigTerm::Kind::Int;
            out.I = std::strtoll(text.c_str(), nullptr, 10);
        }
        return true;
    }

    bool Index(int64_t &n, char close, std::string &why) {
        const size_t begin = At;
        while (At < S.size() && S[At] >= '0' && S[At] <= '9') ++At;
        if (At == begin) return Fail(why, "expected an index");
        n = std::strtoll(std::string(S.substr(begin, At - begin)).c_str(), nullptr, 10);
        if (close && !Eat(close)) return Fail(why, "expected a closing bracket");
        return true;
    }

    // Everything up to and including the `)`. The opening bracket is already eaten.
    bool CommaList(std::vector<SigTerm> &args, std::string &why) {
        SkipSpace();
        if (!Peek(')')) {
            for (;;) {
                SigTerm arg;
                if (!Expr(1, arg, why)) return false;
                args.push_back(std::move(arg));
                SkipSpace();
                if (!Eat(',')) break;
                SkipSpace();
            }
        }
        return Eat(')') ? true : Fail(why, "expected `)`");
    }

    // `(a, b, c)` is a list, `(x op y)` grouping.
    bool Group(SigTerm &out, std::string &why) {
        ++At;
        SigTerm list;
        list.Kind = SigTerm::Kind::List;
        if (!CommaList(list.Args, why)) return false;
        if (list.Args.size() == 1) {
            out = std::move(list.Args[0]);
            return true;
        }
        out = std::move(list);
        return true;
    }

    bool Call(std::string name, SigTerm &out, std::string &why) {
        out = SigTerm{};
        out.Kind = SigTerm::Kind::Op;
        out.Text = std::move(name);
        ++At;
        return CommaList(out.Args, why);
    }

    // `letrec(W0 = (b0, b1))`. The body is a branch list even at one branch.
    bool LetRec(SigTerm &out, std::string &why) {
        out = SigTerm{};
        out.Kind = SigTerm::Kind::Op;
        out.Text = "letrec";
        ++At;
        SkipSpace();
        SigTerm var;
        if (!Primary(var, why)) return false;
        if (var.Kind != SigTerm::Kind::RecVar) return Fail(why, "expected a `W` variable");
        SkipSpace();
        if (!Eat('=')) return Fail(why, "expected `=`");
        SkipSpace();
        SigTerm body;
        if (!Expr(1, body, why)) return false;
        if (body.Kind != SigTerm::Kind::List) {
            SigTerm one;
            one.Kind = SigTerm::Kind::List;
            one.Args.push_back(std::move(body));
            body = std::move(one);
        }
        SkipSpace();
        if (!Eat(')')) return Fail(why, "expected `)`");
        out.Args.push_back(std::move(var));
        out.Args.push_back(std::move(body));
        return true;
    }

    bool Primary(SigTerm &out, std::string &why) {
        SkipSpace();
        if (At >= S.size()) return Fail(why, "expected an operand");

        const char c = S[At];
        if (c == '(') return Group(out, why);
        if (c == '"') {
            const size_t begin = ++At;
            while (At < S.size() && S[At] != '"') ++At;
            if (At >= S.size()) return Fail(why, "unterminated label");
            out = SigTerm{};
            out.Kind = SigTerm::Kind::String;
            out.Text = std::string(S.substr(begin, At - begin));
            ++At;
            return true;
        }
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') return Number(out, why);

        if (!IdentChar(c)) return Fail(why, "expected an operand");
        const size_t begin = At;
        while (At < S.size() && IdentChar(S[At])) ++At;
        std::string word(S.substr(begin, At - begin));

        if (word == "waveform" && Peek('{')) {
            while (At < S.size() && S[At] != '}') ++At;
            if (!Eat('}')) return Fail(why, "unterminated waveform");
            out = SigTerm{};
            out.Kind = SigTerm::Kind::Waveform;
            return true;
        }
        if (word == "IN" && Peek('[')) {
            ++At;
            out = SigTerm{};
            out.Kind = SigTerm::Kind::Input;
            return Index(out.I, ']', why);
        }
        if (word == "letrec" && Peek('(')) return LetRec(out, why);
        if (Peek('(')) return Call(std::move(word), out, why);

        out = SigTerm{};
        if (word.starts_with("ID_") && word.find_first_not_of("0123456789", 3) == std::string::npos && word.size() > 3) {
            out.Kind = SigTerm::Kind::Id;
            out.I = std::strtoll(word.c_str() + 3, nullptr, 10);
            return true;
        }
        if (word[0] == 'W' && word.size() > 1 && word.find_first_not_of("0123456789", 1) == std::string::npos) {
            out.Kind = SigTerm::Kind::RecVar;
            out.I = std::strtoll(word.c_str() + 1, nullptr, 10);
            return true;
        }
        out.Kind = SigTerm::Kind::Name;
        out.Text = std::move(word);
        return true;
    }
};

} // namespace

std::expected<SigFile, std::string> ParseSig(std::string_view text) {
    SigFile out;
    std::string why;
    size_t line_no = 0;
    const bool lines_ok = ForEachLine(text, [&](std::string_view raw) {
        ++line_no;
        const std::string_view line = Trim(raw);
        if (line.empty()) return true;

        const auto bad = [&](std::string_view what) {
            why = std::format("line {}: {} in `{}`", line_no, what, line);
            return false;
        };

        if (line.starts_with("//")) {
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos) {
                const std::string_view n = Trim(line.substr(eq + 1));
                out.Size = int32_t(std::strtol(std::string(n).c_str(), nullptr, 10));
            }
            return true;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return bad("no `=`");
        const std::string_view lhs = Trim(line.substr(0, eq));
        Parser p{line.substr(eq + 1)};
        std::string sub;

        if (lhs == "SIG") {
            if (!p.ParseList(out.Outputs, sub)) return bad(sub);
            return true;
        }
        if (!lhs.starts_with("ID_")) return bad("left-hand side is neither `ID_n` nor `SIG`");
        const size_t n = std::strtoull(std::string(lhs.substr(3)).c_str(), nullptr, 10);
        SigTerm rhs;
        if (!p.ParseLine(rhs, sub)) return bad(sub);
        // The dump is dense and in dependency order.
        if (n != out.Defs.size()) return bad(std::format("`ID_{}` out of sequence", n));
        out.Defs.push_back(std::move(rhs));
        return true;
    });
    if (!lines_ok) return std::unexpected(std::move(why));
    if (out.Outputs.Kind != SigTerm::Kind::List) return std::unexpected("no `SIG = (...)` line");
    return out;
}

std::string PrintSigTerm(const SigTerm &t) {
    switch (t.Kind) {
        case SigTerm::Kind::Id: return std::format("ID_{}", t.I);
        case SigTerm::Kind::RecVar: return std::format("W{}", t.I);
        case SigTerm::Kind::Input: return std::format("IN[{}]", t.I);
        case SigTerm::Kind::Int: return std::to_string(t.I);
        case SigTerm::Kind::Real: return std::to_string(t.D);
        case SigTerm::Kind::Waveform: return "waveform";
        case SigTerm::Kind::Name: return t.Text;
        case SigTerm::Kind::String: return "\"" + t.Text + "\"";
        case SigTerm::Kind::List:
        case SigTerm::Kind::Op: {
            std::string out = t.Kind == SigTerm::Kind::List ? "" : t.Text;
            out += "(";
            for (size_t i = 0; i < t.Args.size(); ++i) {
                if (i) out += ", ";
                out += PrintSigTerm(t.Args[i]);
            }
            return out + ")";
        }
    }
    return "?";
}

} // namespace faustlens::test
