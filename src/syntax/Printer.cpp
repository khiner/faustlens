#include "syntax/Printer.h"

#include "syntax/Lexer.h"

#include <array>
#include <format>

namespace faustlens {
namespace {

// Postfix and suffix operators chain, so only an operand from below needs parens.
constexpr uint8_t PostfixFloor = 12;
constexpr uint8_t SuffixFloor = 1;

enum class Side { Left, Right };

uint8_t MinPrecFor(const OpRow &parent, Side side) {
    switch (parent.Shape) {
        case OpShape::Postfix: return PostfixFloor;
        case OpShape::Suffix: return SuffixFloor;
        case OpShape::Infix: break;
    }
    // Parenthesize at equal precedence on the side associativity disfavours.
    const bool disfavoured = (side == Side::Left) ? parent.Assoc == Assoc::Right : parent.Assoc == Assoc::Left;
    return uint8_t(parent.Level + (disfavoured ? 1 : 0));
}

constexpr std::array<std::string_view, 4> IterNames = {"par", "seq", "sum", "prod"};
constexpr std::array<std::string_view, 3> WidgetNames = {"vslider", "hslider", "nentry"};
constexpr std::array<std::string_view, 2> BargraphNames = {"vbargraph", "hbargraph"};
constexpr std::array<std::string_view, 3> GroupNames = {"vgroup", "hgroup", "tgroup"};
constexpr std::array<std::string_view, 2> FTypeNames = {"int", "float"};

struct Printer {
    const Terms &T;
    Sink &Sink;
    uint32_t Depth = 0;

    void Render(ValueId v, const Ctx &ctx) {
        Sink.Enter(v, ctx);
        const bool parens = NeedsParens(T, v, ctx);
        const bool grouped = parens || Sink.AlreadyGrouped(v);
        if (Sink.Retain(v, parens)) return;
        // Backstop for edit-built terms. Truncating fails a round-trip check loudly.
        if (Depth >= MaxTermDepth) return;
        ++Depth;
        if (parens) Sink.Print("(");
        Own(v, grouped ? Ctx{0, Level::Expression, ctx.Indent, ctx.Name} : ctx);
        if (parens) Sink.Print(")");
        --Depth;
    }

    void P(std::string_view s) { Sink.Print(s); }
    void Lexeme(ValueId v) { P(T.Lexeme(v)); }

    // Normalizes order and repetition, which the bitmask has already lost.
    void Variants(uint16_t variants) {
        if (variants & Single) P("singleprecision ");
        if (variants & Double) P("doubleprecision ");
        if (variants & Quad) P("quadprecision ");
        if (variants & FixedPoint) P("fixedpointprecision ");
    }
    void Newline(uint32_t indent) {
        P("\n");
        P(std::string(indent, ' '));
    }

    Ctx Operand(ValueId v, Side side, const Ctx &ctx) const { return {MinPrecFor(RowOf(T, v), side), ctx.Level, ctx.Indent, 0}; }
    static Ctx Fresh(Level level, uint32_t indent) { return {0, level, indent, 0}; }

    // No space after the comma, which is how the corpus overwhelmingly writes it.
    void CommaList(std::span<const ValueId> kids, const Ctx &ctx) {
        for (size_t i = 0; i < kids.size(); ++i) {
            if (i > 0) P(",");
            Render(kids[i], ctx);
        }
    }

    void Lines(std::span<const ValueId> kids, uint32_t indent) {
        for (const ValueId k : kids) {
            Newline(indent);
            Render(k, Fresh(Level::Expression, indent));
        }
    }
    void Block(std::string_view head, std::span<const ValueId> kids, uint32_t ind) {
        P(head);
        Lines(kids, ind + 4);
        Newline(ind);
        P("}");
    }

    void Own(ValueId v, const Ctx &ctx) {
        const TermValue &n = T.Get(v);
        const auto kids = T.Children(v);
        const uint32_t ind = ctx.Indent, inner = ctx.Indent + 4;
        switch (T.KindOf(v)) {
            case Kind::Program:
                for (uint32_t i = 0; i < kids.size(); ++i) {
                    if (i > 0) Newline(ind);
                    Render(kids[i], Fresh(Level::Expression, ind));
                }
                return;

            case Kind::Import:
                Variants(n.Variants);
                P("import(");
                Lexeme(v);
                P(");");
                return;
            case Kind::Declare:
            case Kind::DeclareDef:
                Variants(n.Variants);
                P("declare ");
                Lexeme(v);
                for (const ValueId k : kids) {
                    P(" ");
                    Render(k, ctx);
                }
                P(";");
                return;

            case Kind::Definition: {
                Ctx c = Fresh(Level::Expression, ind);
                c.Name = n.Payload;
                for (uint32_t i = 0; i < kids.size(); ++i) {
                    if (i > 0) Newline(ind);
                    // The prefix rides on every clause, or a reparse would not merge them.
                    Variants(n.Variants);
                    Render(kids[i], c);
                }
                return;
            }
            case Kind::Clause: {
                P(T.Str(ctx.Name));
                const auto params = kids.first(kids.size() - 1); // body is last
                if (!params.empty()) {
                    P("(");
                    CommaList(params, Fresh(Level::Argument, ind));
                    P(")");
                }
                P(" = ");
                Render(kids.back(), Fresh(Level::Expression, ind));
                P(";");
                return;
            }
            case Kind::RecDef:
                P("'");
                Lexeme(v);
                P(" = ");
                Render(kids[0], Fresh(Level::Expression, ind));
                P(";");
                return;

            case Kind::MdocBlock:
                Variants(n.Variants);
                P("<mdoc>");
                for (const ValueId k : kids) Render(k, ctx);
                P("</mdoc>");
                return;
            case Kind::MdocProse: Lexeme(v); return;
            case Kind::MdocEquation:
            case Kind::MdocDiagram: {
                const bool eqn = T.KindOf(v) == Kind::MdocEquation;
                P(eqn ? "<equation>" : "<diagram>");
                Render(kids[0], Fresh(Level::Expression, ind));
                P(eqn ? "</equation>" : "</diagram>");
                return;
            }
            case Kind::MdocMetadata:
                P("<metadata>");
                Lexeme(v);
                P("</metadata>");
                return;
            case Kind::MdocNotice: P("<notice/>"); return;
            case Kind::MdocListing: {
                P("<listing");
                // One `Print` per attribute, or a seam check splits the quoted value.
                const auto attr = [&](uint8_t set, uint8_t value, std::string_view key) {
                    if (!(n.Form & set)) return;
                    P(std::format(" {}=\"{}\"", key, (n.Form & value) ? "true" : "false"));
                };
                attr(LstDependenciesSet, LstDependencies, "dependencies");
                attr(LstMdoctagsSet, LstMdoctags, "mdoctags");
                attr(LstDistributedSet, LstDistributed, "distributed");
                P("/>");
                return;
            }

            case Kind::Seq:
            case Kind::Par:
            case Kind::Split:
            case Kind::Merge:
            case Kind::RecComp:
            case Kind::BinOp: {
                const OpRow &row = RowOf(T, v);
                Render(kids[0], Operand(v, Side::Left, ctx));
                if (row.Spaced) P(" ");
                P(OperatorText(v));
                if (row.Spaced) P(" ");
                Render(kids[1], Operand(v, Side::Right, ctx));
                return;
            }
            case Kind::Delay1:
                Render(kids[0], Operand(v, Side::Left, ctx));
                P("'");
                return;
            case Kind::NegIdent:
                P("-");
                Lexeme(v);
                return;

            case Kind::With:
                Render(kids[0], Operand(v, Side::Left, ctx));
                Block(" with {", kids.subspan(1), ind);
                return;
            case Kind::LetRec: {
                // Splitting at the first `Definition` keeps a recovered `Hole` on the rec side.
                uint32_t rec_end = 1;
                while (rec_end < kids.size() && T.KindOf(kids[rec_end]) != Kind::Definition) ++rec_end;
                Render(kids[0], Operand(v, Side::Left, ctx));
                P(" letrec {");
                Lines(kids.subspan(1, rec_end - 1), inner);
                if (rec_end < kids.size()) {
                    Newline(inner);
                    P("where");
                    Lines(kids.subspan(rec_end), inner);
                }
                Newline(ind);
                P("}");
                return;
            }
            case Kind::ModifLocalDef:
                Render(kids[0], Operand(v, Side::Left, ctx));
                P("[");
                for (size_t i = 1; i < kids.size(); ++i) {
                    if (i > 1) P(" ");
                    Render(kids[i], Fresh(Level::Expression, ind));
                }
                P("]");
                return;

            case Kind::Apply:
                Render(kids[0], Operand(v, Side::Left, ctx));
                P("(");
                CommaList(kids.subspan(1), Fresh(Level::Argument, ind));
                P(")");
                return;
            case Kind::Access:
                Render(kids[0], Operand(v, Side::Left, ctx));
                P(".");
                Lexeme(v);
                return;
            case Kind::Lambda:
                P("\\(");
                CommaList(kids.first(kids.size() - 1), ctx);
                P(").(");
                Render(kids.back(), Fresh(Level::Expression, ind));
                P(")");
                return;
            case Kind::Case: Block("case {", kids, ind); return;
            case Kind::Rule:
                P("(");
                CommaList(kids.first(kids.size() - 1), Fresh(Level::Argument, ind));
                P(") => ");
                Render(kids.back(), Fresh(Level::Expression, ind));
                P(";");
                return;
            case Kind::Modulation:
                P("[");
                CommaList(kids.first(kids.size() - 1), ctx);
                P(" -> ");
                Render(kids.back(), Fresh(Level::Expression, ind));
                P("]");
                return;
            case Kind::Modulator:
                Lexeme(v);
                if (!kids.empty()) {
                    P(": ");
                    Render(kids[0], Fresh(Level::Argument, ind));
                }
                return;
            case Kind::Iterate:
                P(IterNames[n.Form]);
                P("(");
                Lexeme(v);
                P(",");
                Render(kids[0], Fresh(Level::Argument, ind));
                P(",");
                Render(kids[1], Fresh(Level::Expression, ind));
                P(")");
                return;
            case Kind::Inputs:
            case Kind::Outputs:
                P(T.KindOf(v) == Kind::Inputs ? "inputs(" : "outputs(");
                Render(kids[0], Fresh(Level::Expression, ind));
                P(")");
                return;
            case Kind::Environment: Block("environment {", kids, ind); return;
            case Kind::Component:
            case Kind::Library:
                P(T.KindOf(v) == Kind::Component ? "component(" : "library(");
                Lexeme(v);
                P(")");
                return;
            case Kind::Waveform:
                P("waveform{");
                CommaList(kids, Fresh(Level::Argument, ind));
                P("}");
                return;
            case Kind::Route:
                P("route(");
                Render(kids[0], Fresh(Level::Argument, ind));
                P(",");
                Render(kids[1], Fresh(Level::Argument, ind));
                if (kids.size() > 2) {
                    P(",");
                    Render(kids[2], Fresh(Level::Expression, ind));
                }
                P(")");
                return;

            case Kind::Hole: Lexeme(v); return; // verbatim, children included

            case Kind::Int:
            case Kind::Real:
            case Kind::Ident:
            case Kind::Str: Lexeme(v); return;
            case Kind::Prim: P(n.Payload == uint32_t(Prim::Pow) && n.Form == uint8_t(PowSpelling::Fun) ? "pow" : PrimText(Prim(n.Payload))); return;

            case Kind::Button:
            case Kind::Checkbox:
                P(T.KindOf(v) == Kind::Button ? "button(" : "checkbox(");
                Lexeme(v);
                P(")");
                return;
            case Kind::NumericWidget:
            case Kind::Bargraph:
            case Kind::SoundfileBox: {
                P(T.KindOf(v) == Kind::NumericWidget ? WidgetNames[n.Form] : T.KindOf(v) == Kind::Bargraph ? BargraphNames[n.Form] : "soundfile");
                P("(");
                Lexeme(v);
                for (const ValueId k : kids) {
                    P(",");
                    Render(k, Fresh(Level::Argument, ind));
                }
                P(")");
                return;
            }
            case Kind::Group:
                P(GroupNames[n.Form]);
                P("(");
                Lexeme(v);
                P(",");
                Render(kids[0], Fresh(Level::Expression, ind));
                P(")");
                return;

            case Kind::FFun: {
                const uint32_t names = n.Form;
                const auto total = uint32_t(kids.size());
                P("ffunction(");
                Render(kids[0], ctx);
                P(" ");
                for (uint32_t i = 0; i < names; ++i) {
                    if (i > 0) P("|");
                    Render(kids[1 + i], ctx);
                }
                P("(");
                for (uint32_t i = 1 + names; i + 2 < total; ++i) {
                    if (i > 1 + names) P(",");
                    Render(kids[i], ctx);
                }
                P("),");
                Render(kids[total - 2], ctx);
                P(",");
                Render(kids[total - 1], ctx);
                P(")");
                return;
            }
            case Kind::FConst:
            case Kind::FVar:
                P(T.KindOf(v) == Kind::FConst ? "fconstant(" : "fvariable(");
                P(FTypeNames[n.Form]);
                P(" ");
                Lexeme(v);
                P(",");
                Render(kids[0], ctx);
                P(")");
                return;

            case Kind::Count_: break;
        }
    }

    std::string_view OperatorText(ValueId v) const {
        // `:>` and `+>` lex as one token, so the spelling rides in `Form`.
        if (T.KindOf(v) == Kind::Merge) return T.Get(v).Form == uint8_t(MergeSpelling::Plus) ? "+>" : ":>";
        return TokenText(RowOf(T, v).Tok);
    }
};

struct StringSink : Sink {
    std::string Out;
    size_t Anchor = 0;

    void Print(std::string_view s) override { AppendUnfused(Out, Anchor, s); }
};

} // namespace

bool NeedsParens(const Terms &terms, ValueId v, const Ctx &ctx) {
    if (ctx.Level == Level::Argument && ExcludedFromArgument(terms, v)) return true;
    return PrecOf(terms, v) < ctx.MinPrec;
}

void Render(const Terms &terms, ValueId v, const Ctx &ctx, Sink &sink) { Printer{terms, sink}.Render(v, ctx); }

std::string PrintTerm(const Terms &terms, ValueId v, const Ctx &ctx) {
    StringSink sink;
    Render(terms, v, ctx, sink);
    return std::move(sink.Out);
}

} // namespace faustlens
