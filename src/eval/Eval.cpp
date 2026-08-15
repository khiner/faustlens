#include "eval/Eval.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>

namespace faustlens {
namespace {

constexpr BoxId InFlight = 0xFFFFFFFEu;

// Divergence bound: past any hand-written program and inside the stack.
constexpr uint32_t MaxEvalDepth = 2048;

// A literal's lexeme may carry a sign, unlike the reference's separate production.
int32_t ParseInt(std::string_view s) {
    bool neg = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    uint32_t r = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) r = r * 10u + uint32_t(s[i] - '0');
    // Negated in unsigned space: signed negation of `INT32_MIN` is undefined.
    return int32_t(neg ? 0u - r : r);
}

// `from_chars` reads the view in place, so no copy, and it is locale-independent where
// `strtod` would read a comma as the decimal point under some `LC_NUMERIC`. It stops at
// the `f` suffix the lexer allows, and leaves zero where nothing converts.
double ParseReal(std::string_view s) {
    // The sign is read here for the same reason as in `ParseInt`: it is part of the
    // lexeme, and `from_chars` reads `-` but never `+`.
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    double v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return neg ? -v : v;
}

std::string Unquote(std::string_view s) {
    std::string out(StripQuotes(s));
    for (char &c : out)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return out;
}

bool IsIdentChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Merged by name across the *whole* list, since the parser groups only consecutive runs.
template<class Admit> std::vector<std::pair<StrId, std::vector<ValueId>>> GroupClauses(const Terms &terms, std::span<const ValueId> stmts, Admit admit) {
    std::vector<std::pair<StrId, std::vector<ValueId>>> out;
    for (const ValueId s : stmts) {
        if (terms.KindOf(s) != Kind::Definition || !admit(s)) continue;
        const StrId name = terms.Get(s).Payload;
        const auto at = std::ranges::find_if(out, [&](const auto &e) { return e.first == name; });
        std::vector<ValueId> &clauses = at == out.end() ? out.emplace_back(name, std::vector<ValueId>{}).second : at->second;
        const auto kids = terms.Children(s);
        clauses.insert(clauses.end(), kids.begin(), kids.end());
    }
    return out;
}

} // namespace

void MetaSet::Add(std::string key, std::string value) {
    for (const auto &[k, v] : Entries)
        if (k == key && v == value) return;
    Entries.emplace_back(std::move(key), std::move(value));
}

Evaluator::Evaluator(faustlens::Terms &t, faustlens::Boxes &b, faustlens::Envs &e)
    : Terms(t), Boxes(b), Envs(e), ProcessName(Terms.InternStr("process")), LetrecBody(Terms.InternStr(" LETRECBODY")) {
    // Unwritable in the surface syntax, so `letrec` projections cannot collide with a user name.
}

void Evaluator::Raise(Code code, ValueId subject, std::string payload, Severity severity) {
    Diags.push_back({.Severity = severity, .Code = code, .Subject = subject == NoTerm ? SubjectNow : subject, .Payload = std::move(payload)});
}

BoxId Evaluator::Fail(Code code, ValueId subject, std::string payload) {
    Raise(code, subject, std::move(payload));
    return Boxes.Error;
}

void Evaluator::ClearMemo() {
    Memo.clear();
    PmMemo.clear();
    Symbolic.clear();
    EvaluatedEnvs.clear();
}

Evaluator::Evaluated Evaluator::EvaluatedIn(ValueId v) const {
    const auto it = EvaluatedEnvs.find(v);
    return it == EvaluatedEnvs.end() ? Evaluated{} : it->second;
}

// Resolves against the nearest enclosing file, so a deeply nested `component` still reaches it.
EnvId Evaluator::Target(EnvId env, std::string_view spec) const {
    const uint32_t file = Envs.ResolutionOf(env);
    if (file == NoResolution || !Resolve) return NilEnv;
    return Resolve(Terms.Str(file), spec);
}

BoxId Evaluator::Compose(BoxKind kind, BoxId a, BoxId b, ValueId subject, bool fold) {
    if (Boxes.IsError(a) || Boxes.IsError(b)) return Boxes.Error;
    if (!Boxes.Composable(kind, a, b)) {
        const Arity x = Boxes.ArityOf(a), y = Boxes.ArityOf(b);
        return Fail(Code::EvalArityMismatch, subject, std::format("{} against {}", x.Outs, y.Ins));
    }
    const BoxId re = Boxes.Make(kind, 0, 0, 0, {a, b});
    if (kind != BoxKind::Seq || !fold) return re;

    // `2, 3 : +` folds to `5`. Narrow on purpose: numbers in parallel into a wire or 1-2 arg prim.
    const BoxKind bk = Boxes.KindOf(b);
    const bool applicable =
        bk == BoxKind::Wire || (bk == BoxKind::Prim && PrimArity(Prim(Boxes.Get(b).Payload)) >= 1 && PrimArity(Prim(Boxes.Get(b).Payload)) <= 2);
    if (!applicable || Boxes.IsError(re)) return re;

    std::vector<BoxId> stack{a};
    while (!stack.empty()) {
        const BoxId t = stack.back();
        stack.pop_back();
        if (Boxes.KindOf(t) == BoxKind::Par) {
            stack.push_back(Boxes.Child(t, 0));
            stack.push_back(Boxes.Child(t, 1));
        } else if (Boxes.KindOf(t) != BoxKind::Int && Boxes.KindOf(t) != BoxKind::Real) {
            return re;
        }
    }
    if (const auto v = FoldConstant(Boxes, re)) return MakeNum(Boxes, *v);
    return re;
}

std::optional<Num> Evaluator::EvalNumber(ValueId t, EnvId env, ValueId subject) {
    const BoxId b = ToSymbolic(Eval(t, env));
    if (Boxes.IsError(b)) return std::nullopt;
    const Arity a = Boxes.ArityOf(b);
    if (!a.Known || a.Ins != 0 || a.Outs != 1) {
        Raise(Code::EvalArityMismatch, subject, "not a constant expression of type (0->1)");
        return std::nullopt;
    }
    const auto v = FoldConstant(Boxes, b);
    if (!v) Raise(Code::EvalNonConstantIterationCount, subject);
    return v;
}

int32_t Evaluator::Eval2Int(ValueId t, EnvId env, ValueId subject, bool &ok) {
    const auto v = EvalNumber(t, env, subject);
    ok = v.has_value();
    return ok ? v->AsInt() : 0;
}

double Evaluator::Eval2Double(ValueId t, EnvId env, ValueId subject, bool &ok) {
    const auto v = EvalNumber(t, env, subject);
    ok = v.has_value();
    return ok ? v->AsDouble() : 0.0;
}

// `%ident`, `%{ident}` and `%2ident` substitute the identifier as an integer, `printf` field width.
StrId Evaluator::EvalLabel(StrId raw, EnvId env, ValueId subject) {
    const std::string src = Unquote(Terms.Str(raw));
    if (src.find('%') == std::string::npos) return Terms.InternStr(src);

    std::string dst, ident, format;
    size_t i = 0;
    const auto write = [&] {
        bool ok = false;
        const ValueId id = Terms.MakeLeaf(Kind::Ident, Terms.InternStr(ident));
        const int32_t n = Eval2Int(id, env, subject, ok);
        // `printf`'s minimum field width, space padded, which `{:{}}` reproduces exactly.
        // Capped so a `%99` in a label cannot widen the result without bound.
        int width = 0;
        std::from_chars(format.data(), format.data() + format.size(), width);
        dst += std::format("{:{}}", n, std::min(4, std::max(width, 0)));
    };
    while (i < src.size()) {
        if (src[i] != '%') {
            dst += src[i++];
            continue;
        }
        ++i;
        ident.clear();
        format.clear();
        while (i < src.size() && src[i] >= '0' && src[i] <= '9') format += src[i++];
        if (i < src.size() && src[i] == '{') {
            ++i;
            while (i < src.size() && IsIdentChar(src[i])) ident += src[i++];
            if (i < src.size() && src[i] == '}') {
                ++i;
                write();
            } else {
                dst += '%';
                dst += format;
                break;
            }
        } else if (i < src.size() && IsIdentChar(src[i])) {
            while (i < src.size() && IsIdentChar(src[i])) ident += src[i++];
            write();
        } else {
            dst += '%';
            dst += format;
        }
    }
    return Terms.InternStr(dst);
}

ValueId Evaluator::NestLambda(std::span<const ValueId> params, ValueId body) {
    ValueId t = body;
    for (size_t i = params.size(); i-- > 0;) { t = Terms.Make(Kind::Lambda, {params[i], t}); }
    return t;
}

ValueId Evaluator::DefinitionTerm(std::span<const ValueId> clauses, ValueId subject) {
    const auto params_of = [&](ValueId c) { return Terms.Children(c).size() - 1; };
    if (clauses.size() == 1) {
        const std::vector<ValueId> kids = TermKids(clauses[0]);
        if (kids.size() == 1) return kids[0]; // `f = e;`
        // Anything but distinct plain identifiers is a pattern.
        bool standard = true;
        std::vector<StrId> seen;
        for (size_t i = 0; i + 1 < kids.size(); ++i) {
            if (Terms.KindOf(kids[i]) != Kind::Ident) standard = false;
            else if (std::ranges::contains(seen, Terms.Get(kids[i]).Payload)) standard = false;
            else seen.push_back(Terms.Get(kids[i]).Payload);
            if (!standard) break;
        }
        if (standard) {
            std::vector<ValueId> params;
            for (size_t i = 0; i + 1 < kids.size(); ++i) params.push_back(Terms.MakeLeaf(Kind::Str, Terms.Get(kids[i]).Payload));
            return NestLambda(params, kids.back());
        }
        return Terms.Make(Kind::Case, {Terms.Make(Kind::Rule, kids)});
    }

    const size_t npat = params_of(clauses[0]);
    if (npat == 0) {
        Raise(Code::EvalRedefinition, subject); // several variants, no patterns
        return clauses[0];
    }
    std::vector<ValueId> rules;
    for (const ValueId c : clauses) {
        if (params_of(c) != npat) {
            Raise(Code::EvalArityMismatch, subject, "clauses disagree on their pattern count");
            continue;
        }
        rules.push_back(Terms.Make(Kind::Rule, TermKids(c)));
    }
    return Terms.Make(Kind::Case, rules);
}

// `e letrec { 'x = ex; 'y = ey; where W }` becomes `e with {
//   B = \(x, y).((ex, ey) with W) ~ bus(2); x = B : select(2,0); y = B : select(2,1); }`.
ValueId Evaluator::LetRecToWith(ValueId t) {
    const std::vector<ValueId> kids = TermKids(t);
    size_t rec_end = 1;
    while (rec_end < kids.size() && Terms.KindOf(kids[rec_end]) != Kind::Definition) ++rec_end;
    const auto n = uint32_t(rec_end - 1);

    const ValueId wire = Terms.MakePrim(Prim::Wire);
    const ValueId cut = Terms.MakePrim(Prim::Cut);
    const auto par = [&](ValueId a, ValueId b) { return Terms.Make(Kind::Par, {a, b}); };
    const auto seq = [&](ValueId a, ValueId b) { return Terms.Make(Kind::Seq, {a, b}); };

    if (n == 0) {
        std::vector<ValueId> plain{kids[0]};
        for (size_t i = rec_end; i < kids.size(); ++i) plain.push_back(kids[i]);
        return Terms.Make(Kind::With, plain);
    }

    // `(e1, e2, …)`, with the `where` definitions wrapped around it.
    ValueId body = Terms.Child(kids[n], 0);
    for (size_t i = n; i-- > 1;) body = par(Terms.Child(kids[i], 0), body);
    if (rec_end < kids.size()) {
        std::vector<ValueId> withkids{body};
        for (size_t i = rec_end; i < kids.size(); ++i) withkids.push_back(kids[i]);
        body = Terms.Make(Kind::With, withkids);
    }

    std::vector<ValueId> params;
    params.reserve(n);
    for (uint32_t i = 0; i < n; ++i) params.push_back(Terms.MakeLeaf(Kind::Str, Terms.Get(kids[1 + i]).Payload));
    ValueId bus = wire;
    for (uint32_t i = 1; i < n; ++i) bus = par(bus, wire);
    const ValueId rec = Terms.Make(Kind::RecComp, {NestLambda(params, body), bus});

    const ValueId body_name = Terms.MakeLeaf(Kind::Ident, LetrecBody);
    std::vector<ValueId> defs;
    {
        const ValueId clause = Terms.Make(Kind::Clause, {rec});
        defs.push_back(Terms.Make(Kind::Definition, 0, 0, LetrecBody, {clause}));
    }
    for (uint32_t i = 0; i < n; ++i) {
        ValueId sel = i == 0 ? wire : cut; // `!,!,_,!,!`
        for (uint32_t j = 1; j < n; ++j) sel = par(sel, j == i ? wire : cut);
        const ValueId clause = Terms.Make(Kind::Clause, {seq(body_name, sel)});
        defs.push_back(Terms.Make(Kind::Definition, 0, 0, Terms.Get(kids[1 + i]).Payload, {clause}));
    }

    std::vector<ValueId> withkids{kids[0]};
    withkids.insert(withkids.end(), defs.begin(), defs.end());
    return Terms.Make(Kind::With, withkids);
}

std::vector<Binding> Evaluator::BindingsFromDefs(std::span<const ValueId> defs, ValueId subject, BindKind kind, EnvId closure_env) {
    std::vector<Binding> out;
    for (const auto &[name, clauses] : GroupClauses(Terms, defs, [](ValueId) { return true; })) {
        Binding b;
        b.Name = name;
        b.Kind = kind;
        b.Env = closure_env;
        b.Id = DefinitionTerm(clauses, subject);
        out.push_back(b);
    }
    return out;
}

FileLayer
Evaluator::BuildLayer(std::span<const ValueId> stmts, EnvId parent, StrId file, std::string_view file_key, bool is_root, std::span<const Binding> imported) {
    // Every drop is reported, since a silently dropped definition reads as a compiler bug.
    constexpr uint16_t BuildMode = Double;
    const auto admitted = [&](ValueId s) {
        const uint16_t v = Terms.Get(s).Variants;
        if (v == 0 || (v & BuildMode) != 0) return true;
        Raise(Code::InfoPrecisionFiltered, s, {}, Severity::Info);
        return false;
    };

    std::map<StrId, std::vector<std::pair<std::string, std::string>>> per_definition;
    for (const ValueId s : stmts) {
        if (Terms.KindOf(s) != Kind::DeclareDef || !admitted(s)) continue;
        const std::string key = std::format("{}/{}:{}", file_key, Terms.Lexeme(s), Unquote(Terms.Lexeme(Terms.Child(s, 0))));
        per_definition[Terms.Get(s).Payload].emplace_back(key, Unquote(Terms.Lexeme(Terms.Child(s, 1))));
    }

    FileLayer out;
    for (const ValueId s : stmts) {
        if (Terms.KindOf(s) != Kind::Declare || !admitted(s)) continue;
        std::string key(Terms.Lexeme(s));
        if (!is_root) key = std::format("{}/{}", file_key, key);
        out.Meta.Add(std::move(key), Unquote(Terms.Lexeme(Terms.Child(s, 0))));
    }

    out.Bindings.assign(imported.begin(), imported.end());

    for (const auto &[name, clauses] : GroupClauses(Terms, stmts, admitted)) {
        Binding b;
        b.Name = name;
        b.Kind = BindKind::Definition;
        b.Id = DefinitionTerm(clauses, NoTerm);
        if (const auto it = per_definition.find(name); it != per_definition.end()) {
            MetaGroups.push_back(it->second);
            b.Meta = uint32_t(MetaGroups.size() - 1);
        }
        const auto clash = std::ranges::find_if(out.Bindings, [&](const Binding &x) { return x.Name == name; });
        if (clash != out.Bindings.end()) {
            if (clash->Id != b.Id || clash->Kind != b.Kind) Raise(Code::EvalRedefinition, NoTerm, std::string(Terms.Str(name)));
            *clash = b;
        } else {
            out.Bindings.push_back(b);
        }
    }

    out.Env = Envs.Push(parent, out.Bindings, false, file);
    return out;
}

BoxId Evaluator::Eval(ValueId t, EnvId env) {
    const uint64_t key = (uint64_t(t) << 32) | env;
    if (const auto it = Memo.find(key); it != Memo.end()) {
        // The in-flight mark doubles as the loop detector: `foo = foo;` re-enters at the same key.
        if (it->second == InFlight) return Fail(Code::EvalLoopDetected, t);
        return it->second;
    }
    if (Depth >= MaxEvalDepth) return Fail(Code::EvalDepthExceeded, t);
    Memo[key] = InFlight;
    ++Depth;
    const ValueId saved = SubjectNow;
    SubjectNow = t;
    Boxes.OriginNow = t;
    const BoxId r = RealEval(t, env, /*pattern=*/false);
    Boxes.OriginNow = saved;
    SubjectNow = saved;
    --Depth;
    Memo[key] = r;
    // Looked up now rather than held across `RealEval`, which inserts.
    Evaluated &seen = EvaluatedEnvs[t];
    if (seen.Envs++ == 0) {
        seen.Env = env;
        seen.Box = r;
    } else if (r != seen.Box) {
        seen.Ambiguous = true;
    }
    return r;
}

BoxId Evaluator::EvalBinding(const Binding &b, EnvId layer) {
    if (b.Meta != 0xFFFFFFFFu)
        for (const auto &[k, v] : MetaGroups[b.Meta]) Meta.Add(k, v);
    switch (b.Kind) {
        case BindKind::Value: return b.Id;
        case BindKind::Term: return Eval(b.Id, NilEnv);
        case BindKind::Definition:
        case BindKind::Closure: {
            const EnvId e = b.Kind == BindKind::Definition ? layer : b.Env;
            if (Terms.KindOf(b.Id) == Kind::Lambda) return Boxes.Make(BoxKind::Closure, TermClosure, b.Id, e, {});
            return Eval(b.Id, e);
        }
    }
    return Boxes.Error;
}

BoxId Evaluator::EvalIdent(StrId name, EnvId env, ValueId subject) {
    for (EnvId e = env; e != NilEnv; e = Envs.Parent(e)) {
        if (const Binding *b = Envs.LookupLocal(e, name)) {
            const Binding copy = *b; // `EvalBinding` pushes layers, moving the pool
            return EvalBinding(copy, e);
        }
    }
    return Fail(Code::EvalUnboundName, subject, std::string(Terms.Str(name)));
}

BoxId Evaluator::EvalEntry(EnvId env, StrId name) {
    const ValueId id = Terms.MakeLeaf(Kind::Ident, name == 0 ? ProcessName : name);
    return ToSymbolic(Eval(id, env));
}

BoxId Evaluator::RealEval(ValueId t, EnvId env, bool pattern) {
    const TermValue n = Terms.Get(t); // by value: the rules below grow the arena
    const std::vector<ValueId> kids = TermKids(t);
    const auto eval_in = [&](ValueId c, EnvId e) { return pattern ? EvalInPattern(c, e) : Eval(c, e); };
    const auto eval = [&](ValueId c) { return eval_in(c, env); };
    const auto binary = [&](BoxKind k) { return Compose(k, eval(kids[0]), eval(kids[1]), t); };

    switch (Terms.KindOf(t)) {
        case Kind::Int: return Boxes.MakeInt(ParseInt(Terms.Lexeme(t)));
        case Kind::Real: return Boxes.MakeReal(ParseReal(Terms.Lexeme(t)));
        // `_` and `!` are box kinds, not prims: fold admits a wire, patterns treat them as opaque.
        case Kind::Prim:
            if (n.Payload == uint32_t(Prim::Wire)) return Boxes.MakeLeaf(BoxKind::Wire);
            if (n.Payload == uint32_t(Prim::Cut)) return Boxes.MakeLeaf(BoxKind::Cut);
            return Boxes.MakeLeaf(BoxKind::Prim, n.Payload);
        case Kind::Ident: return EvalIdent(n.Payload, env, t);

        case Kind::Seq: return binary(BoxKind::Seq);
        case Kind::Par: return binary(BoxKind::Par);
        case Kind::Split: return binary(BoxKind::Split);
        case Kind::Merge: return binary(BoxKind::Merge);
        case Kind::RecComp: return binary(BoxKind::Rec);

        case Kind::BinOp: {
            const BoxId op = Boxes.MakePrim(PrimForToken(Tok(n.Payload)));
            return Compose(BoxKind::Seq, Compose(BoxKind::Par, eval(kids[0]), eval(kids[1]), t), op, t);
        }
        case Kind::Delay1: return Compose(BoxKind::Seq, eval(kids[0]), Boxes.MakePrim(Prim::Mem), t);
        case Kind::NegIdent: {
            const BoxId zero = Boxes.MakeInt(0);
            const BoxId x = EvalIdent(n.Payload, env, t);
            return Compose(BoxKind::Seq, Compose(BoxKind::Par, zero, x, t), Boxes.MakePrim(Prim::Sub), t);
        }

        // The body is recursed, the definitions are not, so pattern identifiers in it still bind.
        case Kind::With: {
            const std::vector<ValueId> defs(kids.begin() + 1, kids.end());
            return eval_in(kids[0], Envs.Push(env, BindingsFromDefs(defs, t, BindKind::Definition, NilEnv)));
        }
        case Kind::LetRec: return eval(LetRecToWith(t));

        case Kind::ModifLocalDef: {
            // The replacements close over the current environment, not the captured one.
            const BoxId val = eval(kids[0]);
            if (Boxes.IsError(val)) return val;
            if (Boxes.KindOf(val) != BoxKind::Closure) return Fail(Code::EvalNotAClosure, t);
            const std::vector<ValueId> replacements(kids.begin() + 1, kids.end());
            const EnvId modified = Envs.ReplaceDefs(Boxes.Get(val).Aux, BindingsFromDefs(replacements, t, BindKind::Closure, env));
            if (Boxes.Get(val).Form == EnvClosure) return Boxes.Make(BoxKind::Closure, EnvClosure, 0, modified, {});
            const ValueId term = Boxes.Get(val).Payload;
            if (Terms.KindOf(term) == Kind::Lambda) return Boxes.Make(BoxKind::Closure, TermClosure, term, modified, {});
            return Eval(term, modified);
        }

        case Kind::Apply: {
            std::vector<BoxId> args;
            for (size_t i = 1; i < kids.size(); ++i) args.push_back(eval(kids[i]));
            return Apply(eval(kids[0]), args, t);
        }
        case Kind::Access: {
            // `.` resolves in the captured environment, which makes `environment{}` a value.
            const BoxId val = eval(kids[0]);
            if (Boxes.IsError(val)) return val;
            if (Boxes.KindOf(val) != BoxKind::Closure) return Fail(Code::EvalBadAccess, t);
            return EvalIdent(n.Payload, Boxes.Get(val).Aux, t);
        }
        case Kind::Lambda: {
            if (kids.size() == 2) return Boxes.Make(BoxKind::Closure, TermClosure, t, env, {});
            std::vector<ValueId> params(kids.begin(), kids.end() - 1);
            return Boxes.Make(BoxKind::Closure, TermClosure, NestLambda(params, kids.back()), env, {});
        }
        case Kind::Case: return EvalCase(t, env);

        case Kind::Modulation: {
            if (kids.size() == 2) return Modulate(kids[0], kids[1], env, t);
            std::vector<ValueId> rest(kids.begin() + 1, kids.end());
            return Modulate(kids[0], Terms.Make(Kind::Modulation, rest), env, t);
        }

        case Kind::Iterate: {
            bool ok = false;
            const int32_t count = Eval2Int(kids[0], env, t, ok);
            if (!ok) return Boxes.Error;
            return Iterate(IterKind(n.Form), n.Payload, count, kids[1], env, t, pattern);
        }

        case Kind::Inputs:
        case Kind::Outputs: {
            const BoxId b = ToSymbolic(eval(kids[0]));
            if (Boxes.IsError(b)) return b;
            const Arity a = Boxes.ArityOf(b);
            if (!a.Known) return Fail(Code::EvalArityMismatch, t, "arity is not determined");
            return Boxes.MakeInt(Terms.KindOf(t) == Kind::Inputs ? a.Ins : a.Outs);
        }

        case Kind::Environment: {
            // The layer sits on whatever was in scope, so nesting needs no rule.
            std::vector<Binding> imported;
            for (const ValueId s : kids) {
                if (Terms.KindOf(s) != Kind::Import) continue;
                const EnvId target = Target(env, Unquote(Terms.Lexeme(s)));
                if (target == NilEnv) continue;
                const auto bs = Envs.Bindings(target);
                imported.insert(imported.end(), bs.begin(), bs.end());
            }
            const FileLayer layer = BuildLayer(kids, env, Envs.ResolutionOf(env), "", false, imported);
            return Boxes.Make(BoxKind::Closure, EnvClosure, 0, layer.Env, {});
        }

        case Kind::Component:
        case Kind::Library: {
            // Both evaluate in the target file's environment, never the importer's.
            const std::string spec = Unquote(Terms.Lexeme(t));
            const EnvId target = Target(env, spec);
            if (target == NilEnv) return Fail(Code::ResFileNotFound, t, spec);
            if (Terms.KindOf(t) == Kind::Library) return Boxes.Make(BoxKind::Closure, EnvClosure, 0, target, {});
            return Boxes.Make(BoxKind::Closure, TermClosure, Terms.MakeLeaf(Kind::Ident, ProcessName), target, {});
        }

        case Kind::Waveform: {
            // A waveform's nature is syntactic: `{0., 1.}` real, `{0, 1}` int.
            std::vector<double> values;
            bool all_int = true;
            for (const ValueId c : kids) {
                const bool is_int = Terms.KindOf(c) == Kind::Int;
                all_int = all_int && is_int;
                values.push_back(is_int ? double(ParseInt(Terms.Lexeme(c))) : ParseReal(Terms.Lexeme(c)));
            }
            return Boxes.Make(BoxKind::Waveform, all_int ? 0 : 1, 0, Boxes.AddWaveform(std::move(values)), {});
        }

        case Kind::Route: {
            const BoxId ins = ToSymbolic(eval(kids[0]));
            const BoxId outs = ToSymbolic(eval(kids[1]));
            BoxId entries;
            if (kids.size() > 2) {
                entries = ToSymbolic(eval(kids[2]));
            } else {
                const BoxId zero = Boxes.MakeInt(0);
                entries = Boxes.Make(BoxKind::Par, {zero, zero});
            }
            if (Boxes.IsError(ins) || Boxes.IsError(outs) || Boxes.IsError(entries)) return Boxes.Error;
            const auto a = FoldConstant(Boxes, ins);
            const auto b = FoldConstant(Boxes, outs);
            const auto e = Boxes.ArityOf(entries).Known && Boxes.ArityOf(entries).Ins == 0 ? FoldOutputs(Boxes, entries, {}) : std::nullopt;
            if (!a || !b || !e || e->size() < 2 || e->size() % 2 != 0) return Boxes.Make(BoxKind::Route, {ins, outs, entries});
            RouteTable table;
            table.Ins = a->AsInt();
            table.Outs = b->AsInt();
            std::vector<BoxId> ints;
            for (const Num &v : *e) {
                table.Pairs.push_back(v.AsInt());
                ints.push_back(Boxes.MakeInt(table.Pairs.back()));
            }
            BoxId list = ints.back();
            for (size_t i = ints.size() - 1; i-- > 0;) list = Boxes.Make(BoxKind::Par, {ints[i], list});
            const BoxId norm[] = {Boxes.MakeInt(table.Ins), Boxes.MakeInt(table.Outs), list};
            return Boxes.Make(BoxKind::Route, 0, 0, Boxes.AddRoute(std::move(table)) + 1, norm);
        }

        case Kind::Button:
        case Kind::Checkbox: return Boxes.MakeLeaf(Terms.KindOf(t) == Kind::Button ? BoxKind::Button : BoxKind::Checkbox, EvalLabel(n.Payload, env, t));
        case Kind::NumericWidget:
        case Kind::Bargraph: {
            const StrId label = EvalLabel(n.Payload, env, t);
            const bool numeric = Terms.KindOf(t) == Kind::NumericWidget;
            bool ok = true, all = true;
            double v[4] = {};
            for (uint32_t i = 0; i < (numeric ? 4u : 2u); ++i) {
                v[i] = Eval2Double(kids[i], env, t, ok);
                all = all && ok;
            }
            if (!all) return Boxes.Error;
            const Bounds b = numeric ? Bounds{v[0], v[1], v[2], v[3]} : Bounds{0, v[0], v[1], 0};
            return Boxes.Make(numeric ? BoxKind::NumericWidget : BoxKind::Bargraph, n.Form, label, Boxes.AddBounds(b), {});
        }
        case Kind::Group: {
            const StrId label = EvalLabel(n.Payload, env, t);
            const BoxId body = eval(kids[0]);
            if (Boxes.IsError(body)) return body;
            return Boxes.Make(BoxKind::Group, n.Form, label, 0, {body});
        }
        case Kind::SoundfileBox: {
            const StrId label = EvalLabel(n.Payload, env, t);
            bool ok = false;
            const int32_t channels = Eval2Int(kids[0], env, t, ok);
            if (!ok) return Boxes.Error;
            return Boxes.MakeLeaf(BoxKind::Soundfile, label, uint32_t(channels));
        }

        case Kind::FFun: {
            Signature sig;
            sig.Result = FType(Terms.Lexeme(kids[0]) == "int" ? 0 : 1);
            const uint32_t names = n.Form;
            // `|`-separated names picked by build precision: at f64, `sinf|sin|sinl` is `sin`.
            const uint32_t pick = std::min(names, 2u) - 1;
            const StrId selected = Terms.Get(kids[1 + pick]).Payload;
            const auto total = uint32_t(kids.size());
            for (uint32_t i = 1 + names; i + 2 < total; ++i) {
                const auto ty = Terms.Lexeme(kids[i]);
                sig.Args.push_back(ty == "int" ? 0 : ty == "float" ? 1 : 2);
            }
            sig.Include = Terms.Get(kids[total - 2]).Payload;
            sig.Library = Terms.Get(kids[total - 1]).Payload;
            return Boxes.MakeLeaf(BoxKind::FFun, selected, Boxes.AddSignature(std::move(sig)));
        }
        case Kind::FConst:
        case Kind::FVar:
            return Boxes.Make(Terms.KindOf(t) == Kind::FConst ? BoxKind::FConst : BoxKind::FVar, n.Form, n.Payload, Terms.Get(kids[0]).Payload, {});

        // A `Hole` is already a syntax diagnostic, so it only poisons here.
        case Kind::Hole: return Boxes.Error;

        default: return Fail(Code::EvalNotApplicable, t, std::string(KindName(Terms.KindOf(t))));
    }
}

} // namespace faustlens
