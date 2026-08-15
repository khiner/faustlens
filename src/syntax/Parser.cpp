#include "syntax/Parser.h"

#include "syntax/Prec.h"

#include <cassert>

namespace faustlens {
namespace {

struct ParseError {};

// A recovery scope, each with its own sync set.
enum class Frame : uint8_t { StmtList, DefList, RecList, RuleList, ArgList };

bool FrameConsumes(Frame f, Tok t) { return f != Frame::ArgList && t == Tok::EndDef; }

bool FrameStopsBefore(Frame f, Tok t) {
    switch (f) {
        case Frame::StmtList:
        case Frame::RuleList: return t == Tok::RBraq;
        case Frame::DefList: return t == Tok::RBraq || t == Tok::RCroc;
        case Frame::RecList: return t == Tok::RBraq || t == Tok::Where;
        case Frame::ArgList: return t == Tok::Par || t == Tok::RPar;
    }
    return false;
}

constexpr bool IsOpener(Tok t) { return t == Tok::LPar || t == Tok::LBraq || t == Tok::LCroc; }
constexpr bool IsCloser(Tok t) { return t == Tok::RPar || t == Tok::RBraq || t == Tok::RCroc; }

struct Built {
    ValueId V = NoTerm;
    RefId R = NoRef;
};

// Refs are built bottom-up, then renumbered once into the pre-order `RefTree` requires.
struct RefBuilder {
    std::vector<TermRef> Refs;
    std::vector<RefId> Pool;

    RefId Add(ValueId v, uint32_t begin, uint32_t end, std::span<const Built> kids) {
        TermRef r;
        r.ValueId = v;
        r.SpanBegin = r.OuterBegin = begin;
        r.SpanEnd = r.OuterEnd = end;
        r.FirstChild = uint32_t(Pool.size());
        r.ChildCount = uint32_t(kids.size());
        for (const Built &k : kids) Pool.push_back(k.R);
        Refs.push_back(r);
        return RefId(Refs.size() - 1);
    }

    void SetOuter(RefId r, uint32_t begin, uint32_t end) {
        Refs[r].OuterBegin = begin;
        Refs[r].OuterEnd = end;
    }

    RefTree Finish(RefId root) {
        RefTree out;
        if (root == NoRef) return out;
        out.Refs.reserve(Refs.size());
        out.ChildPool.reserve(Pool.size());
        Emit(root, out);
        return out;
    }

    // Explicit stack rather than recursion, so this needs no depth bound.
    void Emit(RefId root, RefTree &out) {
        struct Pending {
            RefId Src;
            RefId Self;
            uint32_t Next;
        };
        std::vector<Pending> stack;
        const auto place = [&](RefId src) {
            const auto self = RefId(out.Refs.size());
            out.Refs.push_back(Refs[src]);
            out.Refs[self].FirstChild = uint32_t(out.ChildPool.size());
            out.ChildPool.resize(out.Refs[self].FirstChild + Refs[src].ChildCount);
            stack.push_back({src, self, 0});
        };
        place(root);
        while (!stack.empty()) {
            Pending &p = stack.back();
            const TermRef &s = Refs[p.Src];
            if (p.Next == s.ChildCount) {
                stack.pop_back();
                continue;
            }
            const uint32_t i = p.Next++;
            out.ChildPool[out.Refs[p.Self].FirstChild + i] = RefId(out.Refs.size());
            place(Pool[s.FirstChild + i]); // invalidates `p`
        }
    }
};

struct Parser {
    Terms &Terms;
    std::string_view Src;
    uint32_t File;
    TokenVector Tokens;
    std::vector<Diagnostic> Diags;
    RefBuilder Refs;
    uint32_t Cur = 0;
    uint32_t LastEnd = 0;
    uint32_t Depth = 0;
    std::vector<Frame> Frames;
    // Completed nodes of the innermost item. Whatever is left becomes a hole's children.
    std::vector<Built> Orphans;
    size_t OrphanBase = 0;

    Parser(faustlens::Terms &t, std::string_view s, uint32_t f) : Terms(t), Src(s), File(f) {
        LexResult lex = Lex(Src);
        Tokens = std::move(lex.Tokens);
        Diags = std::move(lex.Diags);
        for (Diagnostic &d : Diags) d.File = File;
        SkipTrivia();
    }

    ParseResult Run() {
        std::vector<Built> stmts;
        {
            const FrameGuard g(*this, Frame::StmtList);
            ParseStmtsInto(stmts, Tok::Eof);
        }
        const Built program = Node(Kind::Program, 0, 0, 0, stmts, 0, uint32_t(Src.size()));

        ParseResult out;
        out.Root = program.V;
        out.Refs = Refs.Finish(program.R);
        out.Tokens = std::move(Tokens);
        SortAndDedupe(Diags);
        out.Diags = std::move(Diags);
        return out;
    }

    struct FrameGuard {
        Parser &P;
        FrameGuard(Parser &parser, Frame f) : P(parser) { P.Frames.push_back(f); }
        ~FrameGuard() { P.Frames.pop_back(); }
    };
    struct ItemGuard {
        Parser &P;
        size_t Saved;
        explicit ItemGuard(Parser &parser) : P(parser), Saved(parser.OrphanBase) { P.OrphanBase = P.Orphans.size(); }
        ~ItemGuard() { P.OrphanBase = Saved; }
    };

    void SkipTrivia() {
        while (Cur < Tokens.size() && IsTrivia(Tokens[Cur].Kind)) ++Cur;
    }
    Tok Peek() const { return Tokens[Cur].Kind; }
    Tok PeekAhead(uint32_t n) const {
        uint32_t i = Cur;
        while (n-- > 0) {
            ++i;
            while (i < Tokens.size() && IsTrivia(Tokens[i].Kind)) ++i;
        }
        return i < Tokens.size() ? Tokens[i].Kind : Tok::Eof;
    }
    bool At(Tok t) const { return Peek() == t; }
    uint32_t Begin() const { return Tokens[Cur].Begin; }
    uint32_t End() const { return Tokens[Cur].End; }
    std::string_view Text() const { return Src.substr(Begin(), Tokens[Cur].End - Begin()); }
    void Bump() {
        if (Peek() == Tok::Eof) return;
        LastEnd = Tokens[Cur].End;
        ++Cur;
        SkipTrivia();
    }
    bool Accept(Tok t) {
        if (!At(t)) return false;
        Bump();
        return true;
    }
    void Expect(Tok t) {
        if (!Accept(t)) Fail();
    }
    [[noreturn]] void Fail() {
        Diags.push_back({.Code = Code::SynUnexpectedToken, .File = File, .Begin = Begin(), .End = End(), .Payload = std::string(TokenName(Peek()))});
        throw ParseError{};
    }
    [[noreturn]] void FailWith(Code code) {
        Diags.push_back({.Code = code, .File = File, .Begin = Begin(), .End = End()});
        throw ParseError{};
    }

    // Every nesting construct reaches `ParseExpr`, so counting there bounds recursion.
    struct DepthGuard {
        Parser &P;
        explicit DepthGuard(Parser &parser) : P(parser) {
            // Raised before the increment. A throwing constructor leaves nothing to undo.
            if (P.Depth >= MaxTermDepth) P.FailWith(Code::SynDepthExceeded);
            ++P.Depth;
        }
        ~DepthGuard() { --P.Depth; }
    };

    Built Node(Kind k, uint8_t form, uint16_t variants, uint32_t payload, std::span<const Built> kids, uint32_t begin, uint32_t end) {
        std::vector<ValueId> ids;
        ids.reserve(kids.size());
        for (const Built &b : kids) ids.push_back(b.V);
        const ValueId v = Terms.Make(k, form, variants, payload, ids);
        const RefId r = Refs.Add(v, begin, end, kids);
        // Children are always the most recently completed nodes, in order.
        assert(orphans.size() >= kids.size());
        Orphans.resize(Orphans.size() - kids.size());
        const Built b{v, r};
        Orphans.push_back(b);
        return b;
    }
    Built Node(Kind k, std::span<const Built> kids, uint32_t begin) { return Node(k, 0, 0, 0, kids, begin, LastEnd); }
    Built Node(Kind k, uint8_t form, uint16_t variants, uint32_t payload, std::initializer_list<Built> kids, uint32_t begin, uint32_t end) {
        return Node(k, form, variants, payload, std::span<const Built>(kids.begin(), kids.size()), begin, end);
    }
    Built Node(Kind k, std::initializer_list<Built> kids, uint32_t begin) {
        return Node(k, 0, 0, 0, std::span<const Built>(kids.begin(), kids.size()), begin, LastEnd);
    }
    Built Leaf(Kind k, uint32_t payload, uint32_t begin, uint32_t end) { return Node(k, 0, 0, payload, {}, begin, end); }
    Built LeafHere(Kind k) {
        const uint32_t b = Begin(), e = End();
        const StrId s = Terms.InternStr(Text());
        Bump();
        return Leaf(k, s, b, e);
    }
    StrId ExpectLexeme(Tok t) {
        if (!At(t)) Fail();
        const StrId s = Terms.InternStr(Text());
        Bump();
        return s;
    }
    Built ExpectLeaf(Tok t, Kind k) {
        if (!At(t)) Fail();
        return LeafHere(k);
    }

    bool StopsHere(Tok t) const {
        if (t == Tok::Eof) return true;
        // A frame never consumes an enclosing frame's stop token, keeping holes local.
        for (const Frame f : Frames)
            if (FrameStopsBefore(f, t)) return true;
        return false;
    }

    std::pair<uint32_t, uint32_t> Sync() {
        const Frame frame = Frames.back();
        const uint32_t from = Begin();
        uint32_t to = from;
        int nesting = 0; // a sync token counts only at the entry bracket depth
        while (!At(Tok::Eof)) {
            const Tok t = Peek();
            if (nesting == 0) {
                if (StopsHere(t)) break;
                if (FrameConsumes(frame, t)) {
                    to = End();
                    Bump();
                    break;
                }
            }
            if (IsOpener(t)) ++nesting;
            else if (IsCloser(t) && nesting > 0) --nesting;
            to = End();
            Bump();
        }
        return {from, to};
    }

    Built MakeHole() {
        const auto [from, to] = Sync();
        std::vector<Built> kids(Orphans.begin() + OrphanBase, Orphans.end());
        const StrId text = Terms.InternStr(Src.substr(from, to - from));
        return Node(Kind::Hole, 0, 0, text, kids, from, to);
    }

    template<class Stop, class Item> void ParseItems(std::vector<Built> &out, Stop stop, Item item) {
        while (!At(Tok::Eof) && !stop()) {
            const uint32_t before = Cur;
            {
                const ItemGuard guard(*this);
                try {
                    item();
                } catch (const ParseError &) { out.push_back(MakeHole()); }
            }
            if (Cur == before) Bump();
        }
    }

    void ParseStmtsInto(std::vector<Built> &out, Tok terminator) {
        ParseItems(out, [&] { return At(terminator); }, [&] { ParseStatement(out); });
    }

    uint16_t ParseVariants() {
        uint16_t v = 0;
        for (;;) {
            switch (Peek()) {
                case Tok::FloatMode: v |= Single; break;
                case Tok::DoubleMode: v |= Double; break;
                case Tok::QuadMode: v |= Quad; break;
                case Tok::FixedPointMode: v |= FixedPoint; break;
                default: return v;
            }
            Bump();
        }
    }

    void ParseStatement(std::vector<Built> &out) {
        const uint32_t begin = Begin();
        const uint16_t variants = ParseVariants();
        switch (Peek()) {
            case Tok::Import: {
                Bump();
                Expect(Tok::LPar);
                const StrId spec = ExpectLexeme(Tok::String);
                Expect(Tok::RPar);
                Expect(Tok::EndDef);
                out.push_back(Node(Kind::Import, 0, variants, spec, {}, begin, LastEnd));
                return;
            }
            case Tok::Declare: {
                Bump();
                const StrId name = ExpectLexeme(Tok::Ident);
                if (At(Tok::Ident)) { // `declare name name "string"`
                    const Built key = LeafHere(Kind::Str);
                    const Built value = ExpectLeaf(Tok::String, Kind::Str);
                    Expect(Tok::EndDef);
                    out.push_back(Node(Kind::DeclareDef, 0, variants, name, {key, value}, begin, LastEnd));
                    return;
                }
                const Built value = ExpectLeaf(Tok::String, Kind::Str);
                Expect(Tok::EndDef);
                out.push_back(Node(Kind::Declare, 0, variants, name, {value}, begin, LastEnd));
                return;
            }
            case Tok::BDoc: out.push_back(ParseMdoc(variants)); return;
            default: break;
        }
        ParseDefinitionInto(out, begin, variants);
    }

    // Only adjacent same-name clauses merge, never same-name definitions file-wide.
    void ParseDefinitionInto(std::vector<Built> &out, uint32_t begin, uint16_t variants) {
        StrId name = 0;
        std::vector<Built> clauses{ParseClause(name)};
        const uint32_t first = begin;
        uint32_t end = LastEnd;
        while (!At(Tok::Eof) && SameNameClauseAhead(name, variants)) {
            const ItemGuard item(*this);
            ParseVariants();
            StrId again = 0;
            clauses.push_back(ParseClause(again));
            end = LastEnd;
        }
        out.push_back(Node(Kind::Definition, 0, variants, name, clauses, first, end));
    }

    // Restores `LastEnd` too, or a later node takes its span end from a peeked token.
    bool SameNameClauseAhead(StrId name, uint16_t variants) {
        const uint32_t saved_cur = Cur, saved_end = LastEnd;
        const uint16_t v = ParseVariants();
        const bool match = v == variants && At(Tok::Ident) && Terms.Str(name) == Text() && (PeekAhead(1) == Tok::LPar || PeekAhead(1) == Tok::Def);
        Cur = saved_cur;
        LastEnd = saved_end;
        return match;
    }

    // `name(params) = body;`. The params are patterns, not identifiers.
    Built ParseClause(StrId &name) {
        const uint32_t begin = Begin();
        name = ExpectLexeme(Tok::Ident);
        std::vector<Built> kids;
        if (Accept(Tok::LPar)) {
            const FrameGuard g(*this, Frame::ArgList);
            ParseArgListInto(kids);
            Expect(Tok::RPar);
        }
        Expect(Tok::Def);
        // The body is the last child, so the parameter count needs no encoding.
        kids.push_back(ParseExpr(0, Level::Expression));
        Expect(Tok::EndDef);
        return Node(Kind::Clause, kids, begin);
    }

    void ParseDefListInto(std::vector<Built> &out, Tok terminator) {
        const FrameGuard g(*this, Frame::DefList);
        ParseItems(
            out, [&] { return At(terminator); },
            [&] {
                const uint32_t begin = Begin();
                const uint16_t variants = ParseVariants();
                ParseDefinitionInto(out, begin, variants);
            }
        );
    }

    void ParseRecListInto(std::vector<Built> &out) {
        const FrameGuard g(*this, Frame::RecList);
        ParseItems(
            out, [&] { return At(Tok::RBraq) || At(Tok::Where); },
            [&] {
                const uint32_t begin = Begin();
                Expect(Tok::Delay1);
                const StrId name = ExpectLexeme(Tok::Ident);
                Expect(Tok::Def);
                const Built body = ParseExpr(0, Level::Expression);
                Expect(Tok::EndDef);
                out.push_back(Node(Kind::RecDef, 0, 0, name, {body}, begin, LastEnd));
            }
        );
    }

    Built ParseMdoc(uint16_t variants) {
        const uint32_t begin = Begin();
        Expect(Tok::BDoc);
        std::vector<Built> parts;
        while (!At(Tok::Eof) && !At(Tok::EDoc)) {
            switch (Peek()) {
                case Tok::DocChar: parts.push_back(LeafHere(Kind::MdocProse)); break;
                case Tok::BEqn: parts.push_back(ParseMdocExpr(Kind::MdocEquation, Tok::EEqn)); break;
                case Tok::BDgm: parts.push_back(ParseMdocExpr(Kind::MdocDiagram, Tok::EDgm)); break;
                case Tok::Notice: {
                    const uint32_t b = Begin(), e = End();
                    Bump();
                    parts.push_back(Node(Kind::MdocNotice, 0, 0, 0, {}, b, e));
                    break;
                }
                case Tok::BLst: parts.push_back(ParseMdocListing()); break;
                case Tok::BMetadata: {
                    const uint32_t b = Begin();
                    Bump();
                    const StrId name = ExpectLexeme(Tok::Ident);
                    Expect(Tok::EMetadata);
                    parts.push_back(Node(Kind::MdocMetadata, 0, 0, name, {}, b, LastEnd));
                    break;
                }
                default: Fail();
            }
        }
        Expect(Tok::EDoc);
        return Node(Kind::MdocBlock, 0, variants, 0, parts, begin, LastEnd);
    }

    Built ParseMdocExpr(Kind k, Tok closer) {
        const uint32_t begin = Begin();
        Bump();
        const Built e = ParseExpr(0, Level::Expression);
        Expect(closer);
        return Node(k, 0, 0, 0, {e}, begin, LastEnd);
    }

    Built ParseMdocListing() {
        const uint32_t begin = Begin();
        Expect(Tok::BLst);
        uint8_t form = 0;
        while (!At(Tok::Eof) && !At(Tok::ELst)) {
            uint8_t value_bit = 0, set_bit = 0;
            switch (Peek()) {
                case Tok::LstDependencies:
                    value_bit = LstDependencies;
                    set_bit = LstDependenciesSet;
                    break;
                case Tok::LstMdoctags:
                    value_bit = LstMdoctags;
                    set_bit = LstMdoctagsSet;
                    break;
                case Tok::LstDistributed:
                    value_bit = LstDistributed;
                    set_bit = LstDistributedSet;
                    break;
                default: FailWith(Code::SynBadListingAttribute);
            }
            Bump();
            Expect(Tok::LstEq);
            Expect(Tok::LstQ);
            if (At(Tok::LstTrue)) {
                form |= value_bit;
            } else if (!At(Tok::LstFalse)) {
                Fail();
            } else {
                form &= uint8_t(~value_bit);
            }
            Bump();
            Expect(Tok::LstQ);
            form |= set_bit;
        }
        Expect(Tok::ELst);
        return Node(Kind::MdocListing, form, 0, 0, {}, begin, LastEnd);
    }

    Built ParseExpr(uint16_t min_bp, Level level) {
        const DepthGuard guard(*this);
        const uint32_t begin = Begin();
        Built lhs = ParsePrimary();
        for (;;) {
            const Tok t = Peek();
            const OpRow &row = RowOf(t);
            if (row.Level == 0) break;
            if (level == Level::Argument && (t == Tok::Par || t == Tok::With || t == Tok::LetRec)) break;
            if (LeftBp(row) <= min_bp) break;
            lhs = ParseLed(lhs, t, row, level, begin);
        }
        return lhs;
    }

    Built ParseLed(Built lhs, Tok t, const OpRow &row, Level level, uint32_t begin) {
        switch (row.Shape) {
            case OpShape::Infix: {
                const uint8_t form = row.Kind == Kind::Merge ? uint8_t(Text() == "+>" ? MergeSpelling::Plus : MergeSpelling::Colon) : 0;
                const uint32_t payload = row.Kind == Kind::BinOp ? uint32_t(t) : 0;
                Bump();
                const Built rhs = ParseExpr(RightBp(row), level);
                return Node(row.Kind, form, 0, payload, {lhs, rhs}, begin, LastEnd);
            }
            case OpShape::Postfix: return ParsePostfix(lhs, t, begin);
            case OpShape::Suffix: return ParseSuffix(lhs, t, begin);
        }
        Fail();
    }

    Built ParsePostfix(Built lhs, Tok t, uint32_t begin) {
        switch (t) {
            case Tok::Delay1: {
                Bump();
                return Node(Kind::Delay1, {lhs}, begin);
            }
            case Tok::Dot: {
                Bump();
                const StrId name = ExpectLexeme(Tok::Ident);
                return Node(Kind::Access, 0, 0, name, {lhs}, begin, LastEnd);
            }
            case Tok::LPar: {
                Bump();
                std::vector<Built> kids{lhs};
                {
                    const FrameGuard g(*this, Frame::ArgList);
                    ParseArgListInto(kids);
                }
                Expect(Tok::RPar);
                return Node(Kind::Apply, kids, begin);
            }
            case Tok::LCroc: {
                Bump();
                std::vector<Built> kids{lhs};
                ParseDefListInto(kids, Tok::RCroc);
                Expect(Tok::RCroc);
                return Node(Kind::ModifLocalDef, kids, begin);
            }
            default: Fail();
        }
    }

    Built ParseSuffix(Built lhs, Tok t, uint32_t begin) {
        Bump();
        Expect(Tok::LBraq);
        std::vector<Built> kids{lhs};
        if (t == Tok::With) {
            ParseDefListInto(kids, Tok::RBraq);
            Expect(Tok::RBraq);
            return Node(Kind::With, kids, begin);
        }
        ParseRecListInto(kids);
        if (Accept(Tok::Where)) ParseDefListInto(kids, Tok::RBraq);
        Expect(Tok::RBraq);
        // The rec/`where` split reads back off the child kinds.
        return Node(Kind::LetRec, kids, begin);
    }

    void ParseArgListInto(std::vector<Built> &out) {
        for (;;) {
            out.push_back(ParseExpr(0, Level::Argument));
            if (!Accept(Tok::Par)) return;
        }
    }

    Built ParsePrimary() {
        const uint32_t begin = Begin();
        const Tok t = Peek();
        switch (t) {
            case Tok::Int: return LeafHere(Kind::Int);
            case Tok::Float: return LeafHere(Kind::Real);
            // Minus binds a literal or identifier only, so a bare `-` is the primitive.
            case Tok::Add:
            case Tok::Sub: {
                const Tok next = PeekAhead(1);
                if (next == Tok::Int || next == Tok::Float) return SignedLiteral(begin, next);
                if (t == Tok::Sub && next == Tok::Ident) {
                    Bump();
                    const uint32_t end = End();
                    const StrId name = ExpectLexeme(Tok::Ident);
                    return Leaf(Kind::NegIdent, name, begin, end);
                }
                return PrimHere();
            }
            case Tok::Ident: return LeafHere(Kind::Ident);
            case Tok::LPar: {
                Bump();
                Built inner = ParseExpr(0, Level::Expression);
                Expect(Tok::RPar);
                // Grouping parens build no value, only a wider ref span.
                Refs.SetOuter(inner.R, begin, LastEnd);
                return inner;
            }
            case Tok::Lambda: return ParseLambda(begin);
            case Tok::LCroc: return ParseModulation(begin);
            case Tok::Case: return ParseCase(begin);
            case Tok::FFunction: return ParseFFun(begin);
            case Tok::FConstant:
            case Tok::FVariable: return ParseForeignValue(begin, t);
            case Tok::Component:
            case Tok::Library:
            case Tok::Button:
            case Tok::Checkbox: {
                Bump();
                Expect(Tok::LPar);
                const StrId s = ExpectLexeme(Tok::String);
                Expect(Tok::RPar);
                const Kind k = t == Tok::Component ? Kind::Component : t == Tok::Library ? Kind::Library : t == Tok::Button ? Kind::Button : Kind::Checkbox;
                return Leaf(k, s, begin, LastEnd);
            }
            case Tok::Environment: {
                Bump();
                Expect(Tok::LBraq);
                std::vector<Built> stmts;
                {
                    const FrameGuard g(*this, Frame::StmtList);
                    ParseStmtsInto(stmts, Tok::RBraq);
                }
                Expect(Tok::RBraq);
                return Node(Kind::Environment, stmts, begin);
            }
            case Tok::Waveform: return ParseWaveform(begin);
            case Tok::Route: return ParseRoute(begin);
            case Tok::VSlider:
            case Tok::HSlider:
            case Tok::NEntry: return ParseNumericWidget(begin, t);
            case Tok::VBargraph:
            case Tok::HBargraph: return ParseBargraph(begin, t);
            case Tok::VGroup:
            case Tok::HGroup:
            case Tok::TGroup: return ParseGroup(begin, t);
            case Tok::Soundfile: return ParseLabeled(begin, Kind::SoundfileBox, 0, 1);
            case Tok::IPar:
            case Tok::ISeq:
            case Tok::ISum:
            case Tok::IProd: return ParseIterate(begin, t);
            case Tok::Inputs:
            case Tok::Outputs: {
                Bump();
                Expect(Tok::LPar);
                const Built e = ParseExpr(0, Level::Expression);
                Expect(Tok::RPar);
                return Node(t == Tok::Inputs ? Kind::Inputs : Kind::Outputs, {e}, begin);
            }
            default: break;
        }
        return PrimHere();
    }

    Built PrimHere() {
        const uint32_t b = Begin(), e = End();
        const Tok t = Peek();
        const Prim p = PrimForToken(t);
        if (p == Prim::Count_) Fail();
        const auto form = uint8_t(t == Tok::PowFun ? PowSpelling::Fun : PowSpelling::Caret);
        Bump();
        return Node(Kind::Prim, form, 0, uint32_t(p), {}, b, e);
    }

    // Parameters are binding occurrences, so they are `Str`, not `Ident`.
    Built ParseLambda(uint32_t begin) {
        Bump();
        Expect(Tok::LPar);
        std::vector<Built> kids;
        for (;;) {
            kids.push_back(ExpectLeaf(Tok::Ident, Kind::Str));
            if (!Accept(Tok::Par)) break;
        }
        Expect(Tok::RPar);
        Expect(Tok::Dot);
        Expect(Tok::LPar);
        kids.push_back(ParseExpr(0, Level::Expression));
        Expect(Tok::RPar);
        return Node(Kind::Lambda, kids, begin);
    }

    // Prefix `[` modulates, infix `[` modifies (`ModifLocalDef`). Position alone splits them.
    Built ParseModulation(uint32_t begin) {
        Bump();
        std::vector<Built> kids;
        for (;;) {
            const uint32_t mb = Begin();
            const StrId name = ExpectLexeme(Tok::String);
            std::vector<Built> value;
            if (Accept(Tok::Seq)) value.push_back(ParseExpr(0, Level::Argument));
            kids.push_back(Node(Kind::Modulator, 0, 0, name, value, mb, LastEnd));
            if (!Accept(Tok::Par)) break;
        }
        Expect(Tok::LApply);
        kids.push_back(ParseExpr(0, Level::Expression));
        Expect(Tok::RCroc);
        return Node(Kind::Modulation, kids, begin);
    }

    Built ParseCase(uint32_t begin) {
        Bump();
        Expect(Tok::LBraq);
        std::vector<Built> rules;
        {
            const FrameGuard g(*this, Frame::RuleList);
            ParseItems(rules, [&] { return At(Tok::RBraq); }, [&] { rules.push_back(ParseRule()); });
        }
        Expect(Tok::RBraq);
        if (rules.empty()) Diags.push_back({.Code = Code::SynEmptyCase, .File = File, .Begin = begin, .End = LastEnd});
        return Node(Kind::Case, rules, begin);
    }

    Built ParseRule() {
        const uint32_t begin = Begin();
        Expect(Tok::LPar);
        std::vector<Built> kids;
        {
            const FrameGuard g(*this, Frame::ArgList);
            ParseArgListInto(kids);
        }
        Expect(Tok::RPar);
        Expect(Tok::Arrow);
        kids.push_back(ParseExpr(0, Level::Expression));
        Expect(Tok::EndDef);
        return Node(Kind::Rule, kids, begin);
    }

    Built ParseFFun(uint32_t begin) {
        Bump();
        Expect(Tok::LPar);
        std::vector<Built> kids;
        kids.push_back(ExpectTypeName(/*allow_any=*/false)); // return type
        for (;;) {
            kids.push_back(ExpectLeaf(Tok::Ident, Kind::Str));
            if (!Accept(Tok::Or)) break;
        }
        const auto name_count = uint8_t(kids.size() - 1);
        if (name_count > 4) Fail();
        Expect(Tok::LPar);
        if (!At(Tok::RPar)) {
            for (;;) {
                kids.push_back(ExpectTypeName(/*allow_any=*/true));
                if (!Accept(Tok::Par)) break;
            }
        }
        Expect(Tok::RPar);
        Expect(Tok::Par);
        kids.push_back(ExpectFString());
        Expect(Tok::Par);
        kids.push_back(ExpectLeaf(Tok::String, Kind::Str));
        Expect(Tok::RPar);
        return Node(Kind::FFun, name_count, 0, 0, kids, begin, LastEnd);
    }

    Built ExpectTypeName(bool allow_any) {
        if (At(Tok::IntCast) || At(Tok::FloatCast) || (allow_any && At(Tok::NoTypeCast))) return LeafHere(Kind::Str);
        Fail();
    }
    Built ExpectFString() {
        if (At(Tok::String) || At(Tok::FString)) return LeafHere(Kind::Str);
        Fail();
    }

    Built ParseForeignValue(uint32_t begin, Tok t) {
        Bump();
        Expect(Tok::LPar);
        if (!At(Tok::IntCast) && !At(Tok::FloatCast)) Fail();
        const auto type = uint8_t(At(Tok::IntCast) ? FType::Int : FType::Float);
        Bump();
        const StrId name = ExpectLexeme(Tok::Ident);
        Expect(Tok::Par);
        const Built include = ExpectFString();
        Expect(Tok::RPar);
        return Node(t == Tok::FConstant ? Kind::FConst : Kind::FVar, type, 0, name, {include}, begin, LastEnd);
    }

    Built ParseWaveform(uint32_t begin) {
        Bump();
        Expect(Tok::LBraq);
        std::vector<Built> kids;
        for (;;) {
            kids.push_back(ParseNumber());
            if (!Accept(Tok::Par)) break;
        }
        Expect(Tok::RBraq);
        return Node(Kind::Waveform, kids, begin);
    }

    Built ParseNumber() {
        const uint32_t begin = Begin();
        if (At(Tok::Add) || At(Tok::Sub)) {
            const Tok next = PeekAhead(1);
            if (next != Tok::Int && next != Tok::Float) Fail();
            return SignedLiteral(begin, next);
        }
        if (At(Tok::Int)) return LeafHere(Kind::Int);
        if (At(Tok::Float)) return LeafHere(Kind::Real);
        Fail();
    }

    Built SignedLiteral(uint32_t begin, Tok next) {
        Bump();
        const uint32_t end = End();
        const StrId lex = Terms.InternStr(Src.substr(begin, end - begin));
        Bump();
        return Leaf(next == Tok::Int ? Kind::Int : Kind::Real, lex, begin, end);
    }

    Built ParseRoute(uint32_t begin) {
        Bump();
        Expect(Tok::LPar);
        std::vector<Built> kids;
        kids.push_back(ParseExpr(0, Level::Argument));
        Expect(Tok::Par);
        kids.push_back(ParseExpr(0, Level::Argument));
        if (Accept(Tok::Par)) kids.push_back(ParseExpr(0, Level::Expression));
        Expect(Tok::RPar);
        return Node(Kind::Route, kids, begin);
    }

    // `name("label" (, argument)*)`. Widgets differ only in `args`.
    Built ParseLabeled(uint32_t begin, Kind k, uint8_t form, int args) {
        Bump();
        Expect(Tok::LPar);
        const StrId label = ExpectLexeme(Tok::String);
        std::vector<Built> kids;
        for (int i = 0; i < args; ++i) {
            Expect(Tok::Par);
            kids.push_back(ParseExpr(0, Level::Argument));
        }
        Expect(Tok::RPar);
        return Node(k, form, 0, label, kids, begin, LastEnd);
    }

    Built ParseNumericWidget(uint32_t begin, Tok t) {
        const auto kind = uint8_t(t == Tok::VSlider ? WidgetKind::VSlider : t == Tok::HSlider ? WidgetKind::HSlider : WidgetKind::NEntry);
        return ParseLabeled(begin, Kind::NumericWidget, kind, 4);
    }

    Built ParseBargraph(uint32_t begin, Tok t) {
        const auto kind = uint8_t(t == Tok::VBargraph ? BargraphKind::VBargraph : BargraphKind::HBargraph);
        return ParseLabeled(begin, Kind::Bargraph, kind, 2);
    }

    Built ParseGroup(uint32_t begin, Tok t) {
        Bump();
        Expect(Tok::LPar);
        const StrId label = ExpectLexeme(Tok::String);
        Expect(Tok::Par);
        const Built body = ParseExpr(0, Level::Expression);
        Expect(Tok::RPar);
        const auto kind = uint8_t(t == Tok::VGroup ? GroupKind::VGroup : t == Tok::HGroup ? GroupKind::HGroup : GroupKind::TGroup);
        return Node(Kind::Group, kind, 0, label, {body}, begin, LastEnd);
    }

    Built ParseIterate(uint32_t begin, Tok t) {
        Bump();
        Expect(Tok::LPar);
        const StrId var = ExpectLexeme(Tok::Ident);
        Expect(Tok::Par);
        std::vector<Built> kids;
        kids.push_back(ParseExpr(0, Level::Argument));
        Expect(Tok::Par);
        kids.push_back(ParseExpr(0, Level::Expression));
        Expect(Tok::RPar);
        const auto kind = uint8_t(t == Tok::IPar ? IterKind::Par : t == Tok::ISeq ? IterKind::Seq : t == Tok::ISum ? IterKind::Sum : IterKind::Prod);
        return Node(Kind::Iterate, kind, 0, var, kids, begin, LastEnd);
    }
};

} // namespace

ParseResult Parse(Terms &terms, std::string_view src, uint32_t file_index) { return Parser(terms, src, file_index).Run(); }

} // namespace faustlens
