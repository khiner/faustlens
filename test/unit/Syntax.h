#pragma once

#include "syntax/Edit.h"
#include "syntax/Lexer.h"
#include "syntax/Parser.h"
#include "syntax/Printer.h"
#include "syntax/Splice.h"

#include "doctest.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace faustlens::test {

// A compact s-expression of the value graph.
inline std::string Shape(const Terms &t, ValueId v) {
    const TermValue &n = t.Get(v);
    std::string out(KindName(t.KindOf(v)));
    switch (t.KindOf(v)) {
        case Kind::Int:
        case Kind::Real:
        case Kind::Ident:
        case Kind::Str:
        case Kind::NegIdent:
        case Kind::Access:
        case Kind::Import:
        case Kind::Definition:
        case Kind::RecDef:
        case Kind::Component:
        case Kind::Library:
        case Kind::Button:
        case Kind::Modulator: out += std::format("({})", t.Lexeme(v)); break;
        case Kind::Prim: out += std::format("({})", PrimText(Prim(n.Payload))); break;
        case Kind::BinOp: out += std::format("({})", TokenText(Tok(n.Payload))); break;
        default: break;
    }
    const auto kids = t.Children(v);
    if (kids.empty()) return out;
    out += "[";
    for (size_t i = 0; i < kids.size(); ++i) {
        if (i) out += " ";
        out += Shape(t, kids[i]);
    }
    return out + "]";
}

// The body of the first statement's first clause, which is what a `process = e;` test wants.
inline ValueId ClauseBody(const Terms &t, ValueId root) { return t.Children(t.Child(t.Child(root, 0), 0)).back(); }

inline bool Accepts(std::string_view src) {
    Terms terms;
    return Parse(terms, src).Diags.empty();
}

// PutGet: `value(parse(print(t))) == value(t)`, compared as interned ids.
inline void CheckPutGet(std::string_view src) {
    Terms terms;
    const ParseResult a = Parse(terms, src);
    REQUIRE(a.Diags.empty());
    const std::string printed = PrintTerm(terms, a.Root);
    CAPTURE(printed);
    const ParseResult b = Parse(terms, printed);
    REQUIRE(b.Diags.empty());
    CHECK(a.Root == b.Root);
}

// Every byte in exactly one token.
inline void CheckTiling(std::string_view src) {
    uint32_t at = 0;
    for (const Token &t : Lex(src).Tokens) {
        if (t.Kind == Tok::Eof) {
            CHECK(t.Begin == src.size());
            CHECK(t.End == src.size());
            continue;
        }
        REQUIRE(t.Begin == at);
        REQUIRE(t.End > t.Begin);
        at = t.End;
    }
    CHECK(at == src.size());
}

struct File {
    Terms Terms;
    std::string Src;
    ParseResult R;
    std::optional<SpliceContext> Ctx;
    std::optional<EditContext> Ed;

    explicit File(std::string text) : Src(std::move(text)) {
        R = Parse(Terms, Src);
        REQUIRE(R.Diags.empty());
        Ctx.emplace(Terms, Src, R.Refs, R.Tokens);
        Ed.emplace(Terms, R.Refs);
    }

    RefId RefFor(std::string_view needle) const {
        const auto at = uint32_t(Src.find(needle));
        REQUIRE(at != uint32_t(std::string::npos));
        const auto end = uint32_t(at + needle.size());
        for (RefId i = 0; i < R.Refs.Refs.size(); ++i)
            if (R.Refs.Refs[i].SpanBegin == at && R.Refs.Refs[i].SpanEnd == end) return i;
        FAIL("no ref spans ", needle);
        return NoRef;
    }

    EditScript ScriptFor(RefId target, ValueId new_root) const { return Ctx->Splice(target, new_root); }
    std::string SpliceTo(RefId target, ValueId new_root) const { return ApplyScript(Src, ScriptFor(target, new_root)); }

    std::string After(const Edit &e) const {
        REQUIRE_MESSAGE(e.Target != NoRef, std::string(e.Declined == nullptr ? "declined" : e.Declined));
        return SpliceTo(e.Target, e.Value);
    }
};

} // namespace faustlens::test
