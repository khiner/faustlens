#include "box/Label.h"
#include "eval/Eval.h"

#include <algorithm>
#include <format>
#include <span>
#include <string>

namespace faustlens {
namespace {

// Operators that take their padding wires first.
bool SpecialInfix(Prim p) { return (p >= Prim::Add && p <= Prim::Control) || p == Prim::Pow; }

bool Structural(BoxKind k) { return IsComposition(k) || k == BoxKind::Route; }

// The label's absolute path, reversed, group codes and metadata stripped. An empty
// name is no segment: no `minput` target names one.
std::vector<std::string> LabelPath(std::string_view label) {
    std::vector<std::string> abs;
    for (const PathSeg &seg : LabelToPath(label)) {
        if (seg.Root()) abs.clear();
        else if (seg.Parent()) {
            if (!abs.empty()) abs.pop_back();
        } else if (std::string name = LabelOnly(seg.Name); !name.empty()) abs.push_back(std::move(name));
    }
    std::ranges::reverse(abs);
    return abs;
}

std::vector<std::string> TargetToPath(std::string_view target) {
    std::vector<std::string> path;
    std::string current;
    for (const char c : target) {
        if (c == '/') {
            if (!current.empty()) path.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) path.push_back(current);
    std::ranges::reverse(path);
    return path;
}

// Ordered-subsequence containment, not equality, so a partial path selects.
bool PathMatchesLabel(std::span<const std::string> path, std::span<const std::string> label) {
    size_t j = 0;
    for (const std::string &s : path) {
        while (j < label.size() && label[j] != s) ++j;
        if (j == label.size()) return false;
        ++j;
    }
    return true;
}

// Paths are reversed, so a group matches the target's tail. `rest` carries inside.
bool MatchGroup(std::span<const std::string> group, std::span<const std::string> target, std::vector<std::string> &rest) {
    if (group.size() > target.size()) return false;
    const size_t at = target.size() - group.size();
    if (!std::equal(group.begin(), group.end(), target.begin() + at)) return false;
    rest.assign(target.begin(), target.begin() + at);
    return true;
}

} // namespace

BoxId Evaluator::Apply(BoxId fun, std::span<const BoxId> args, ValueId subject) {
    if (args.empty()) return fun;
    if (Boxes.IsError(fun)) return Boxes.Error;
    for (const BoxId a : args)
        if (Boxes.IsError(a)) return Boxes.Error;

    if (Boxes.KindOf(fun) == BoxKind::PatternMatcher) return Apply(MatchArgument(fun, args[0], subject), args.subspan(1), subject);

    if (Boxes.KindOf(fun) != BoxKind::Closure) {
        // A non-function `F` applied is `(a, b, ...) : F`, wires padding out to `F`'s inputs.
        const BoxId efun = ToSymbolic(fun);
        const auto par = [&](std::span<const BoxId> l) {
            BoxId r = l.back();
            for (size_t i = l.size() - 1; i-- > 0;) r = Compose(BoxKind::Par, l[i], r, subject);
            return r;
        };
        // Folding beats declining here: three corpus programs lost against twenty-seven.
        const auto seq = [&](BoxId l, BoxId r) { return Compose(BoxKind::Seq, l, r, subject); };
        std::vector<BoxId> list(args.begin(), args.end());
        const Arity fa = Boxes.ArityOf(efun); // by value: `Arities` grows below
        if (!fa.Known) return seq(par(list), fun);

        int32_t outs = 0;
        for (const BoxId a : args) {
            const Arity aa = Boxes.ArityOf(ToSymbolic(a));
            outs += aa.Known ? aa.Outs : 1;
        }
        if (outs > fa.Ins) return Fail(Code::EvalArityMismatch, subject, std::format("{} arguments against {}", outs, fa.Ins));

        std::vector<BoxId> wires(size_t(fa.Ins - outs), Boxes.Wire);
        const bool special_infix = outs == 1 && Boxes.KindOf(fun) == BoxKind::Prim && SpecialInfix(Prim(Boxes.Get(fun).Payload));
        if (special_infix) list.insert(list.begin(), wires.begin(), wires.end());
        else list.insert(list.end(), wires.begin(), wires.end());
        return seq(par(list), fun);
    }

    const BoxNode c = Boxes.Get(fun);
    if (c.Form == EnvClosure) return Fail(Code::EvalNotApplicable, subject, "an environment is not a function");
    const ValueId abstr = c.Payload;
    const EnvId env = c.Aux;
    if (Terms.KindOf(abstr) == Kind::Ident) return Apply(Eval(abstr, env), args, subject);
    if (Terms.KindOf(abstr) != Kind::Lambda) return Fail(Code::EvalNotApplicable, subject);

    const std::vector<ValueId> kids = TermKids(abstr);
    BoxId arg = args[0];
    if (const auto v = FoldConstant(Boxes, arg)) arg = MakeNum(Boxes, *v);
    const EnvId inner = Envs.PushValue(env, Terms.Get(kids[0]).Payload, BindKind::Value, arg);
    return Apply(Eval(kids[1], inner), args.subspan(1), subject);
}

// A closed 0->1 sub-pattern folds to its literal, so `f(2+3)` is `f(5)`.
BoxId Evaluator::SimplifyPattern(BoxId p) {
    if (const auto v = FoldConstant(Boxes, p)) return MakeNum(Boxes, *v);
    if (!Structural(Boxes.KindOf(p))) return p;
    std::vector<BoxId> kids;
    for (const BoxId c : BoxKids(p)) kids.push_back(SimplifyPattern(c));
    return Boxes.Rebuild(p, kids);
}

BoxId Evaluator::EvalCase(ValueId rules, EnvId env) {
    const uint64_t key = (uint64_t(rules) << 32) | env;
    if (const auto it = PmMemo.find(key); it != PmMemo.end()) return it->second;

    PMState state;
    // The barrier scopes a non-linear pattern's equality test to this match.
    const EnvId base = Envs.PushBarrier(env);
    for (const ValueId rule : TermKids(rules)) {
        const std::vector<ValueId> kids = TermKids(rule);
        std::vector<BoxId> patterns;
        for (size_t i = 0; i + 1 < kids.size(); ++i) patterns.push_back(SimplifyPattern(EvalInPattern(kids[i], env)));
        state.Patterns.push_back(std::move(patterns));
        state.RuleEnvs.push_back(base);
        state.Live.push_back(1);
    }
    const BoxId pm = Boxes.Make(BoxKind::PatternMatcher, 0, rules, Boxes.AddPMState(std::move(state)), {});
    PmMemo[key] = pm;
    return pm;
}

bool Evaluator::MatchPattern(BoxId pattern, BoxId arg, EnvId &rule_env) {
    if (Boxes.KindOf(pattern) == BoxKind::PatternVar) {
        const StrId name = Boxes.Get(pattern).Payload;
        // A repeated variable must get equal boxes, which is an id comparison.
        if (const Binding *b = Envs.Lookup(rule_env, name, /*stop_at_barrier=*/true)) return b->Kind == BindKind::Value && b->Id == arg;
        rule_env = Envs.PushValue(rule_env, name, BindKind::Value, arg);
        return true;
    }
    const BoxKind k = Boxes.KindOf(pattern);
    if (Structural(k) && Boxes.KindOf(arg) == k && Boxes.Children(pattern).size() == Boxes.Children(arg).size()) {
        for (size_t i = 0; i < Boxes.Children(pattern).size(); ++i)
            if (!MatchPattern(Boxes.Child(pattern, i), Boxes.Child(arg, i), rule_env)) return false;
        return true;
    }
    return pattern == arg;
}

BoxId Evaluator::MatchArgument(BoxId pm, BoxId arg, ValueId subject) {
    const BoxNode n = Boxes.Get(pm);
    PMState state = Boxes.PMStateAt(n.Aux); // by value: this step produces a new one
    const std::vector<ValueId> rules = TermKids(n.Payload);
    const auto consumed = size_t(n.ChildCount);

    // A recursive count would otherwise arrive as a graph and never match its base
    // case. Gated on a literal, or a graph binds as one.
    bool match_num = false;
    for (size_t r = 0; r < rules.size(); ++r) {
        if (!state.Live[r] || consumed >= state.Patterns[r].size()) continue;
        const BoxKind pk = Boxes.KindOf(state.Patterns[r][consumed]);
        match_num = match_num || pk == BoxKind::Int || pk == BoxKind::Real;
    }
    if (match_num) arg = SimplifyPattern(arg);

    bool any = false;
    for (size_t r = 0; r < rules.size(); ++r) {
        if (!state.Live[r]) continue;
        EnvId e = state.RuleEnvs[r];
        if (consumed < state.Patterns[r].size() && MatchPattern(state.Patterns[r][consumed], arg, e)) {
            state.RuleEnvs[r] = e;
            any = true;
        } else {
            state.Live[r] = 0;
        }
    }
    if (!any) return Fail(Code::EvalNoMatchingRule, subject);

    const size_t npat = state.Patterns.empty() ? 0 : state.Patterns[0].size();
    if (consumed + 1 == npat) {
        for (size_t r = 0; r < rules.size(); ++r) {
            if (!state.Live[r]) continue;
            const std::vector<ValueId> kids = TermKids(rules[r]);
            return Eval(kids.back(), state.RuleEnvs[r]);
        }
    }
    std::vector<BoxId> args = BoxKids(pm);
    args.push_back(arg);
    return Boxes.Make(BoxKind::PatternMatcher, 0, n.Payload, Boxes.AddPMState(std::move(state)), args);
}

// A bare identifier in a pattern position binds, never a lookup. The cases below are the rule.
BoxId Evaluator::EvalInPattern(ValueId t, EnvId env) {
    const TermValue n = Terms.Get(t);
    const std::vector<ValueId> kids = TermKids(t);
    const auto pat = [&](ValueId c) { return EvalInPattern(c, env); };

    switch (Terms.KindOf(t)) {
        case Kind::Ident: return Boxes.MakeLeaf(BoxKind::PatternVar, n.Payload);

        // Only the arguments recurse, so in `f(x)` `f` resolves and `x` binds.
        case Kind::Apply: {
            const BoxId fun = Terms.KindOf(kids[0]) == Kind::Ident ? Eval(kids[0], env) : pat(kids[0]);
            std::vector<BoxId> args;
            for (size_t i = 1; i < kids.size(); ++i) args.push_back(pat(kids[i]));
            return Apply(fun, args, t);
        }

        case Kind::NegIdent:
            return Compose(
                BoxKind::Seq, Compose(BoxKind::Par, Boxes.MakeInt(0), Boxes.MakeLeaf(BoxKind::PatternVar, n.Payload), t), Boxes.MakePrim(Prim::Sub), t
            );

        case Kind::Seq:
        case Kind::Par:
        case Kind::Split:
        case Kind::Merge:
        case Kind::RecComp:
        case Kind::BinOp:
        case Kind::Delay1:
        case Kind::With:
        case Kind::LetRec:
        case Kind::Iterate:
        case Kind::Inputs:
        case Kind::Outputs:
        case Kind::Group: return RealEval(t, env, /*pattern=*/true);

        case Kind::Route: {
            std::vector<BoxId> parts{pat(kids[0]), pat(kids[1])};
            parts.push_back(kids.size() > 2 ? pat(kids[2]) : Boxes.MakeInt(0));
            return Boxes.Make(BoxKind::Route, parts);
        }

        case Kind::ModifLocalDef:
        case Kind::Modulation: return Fail(Code::EvalInvalidPattern, t, Terms.KindOf(t) == Kind::Modulation ? "a modulation" : "a local definition modifier");

        // Everything else is opaque, so identifiers under it are not binders.
        default: return Eval(t, env);
    }
}

BoxId Evaluator::Iterate(IterKind kind, StrId var, int32_t n, ValueId body, EnvId env, ValueId subject, bool pattern) {
    const auto at = [&](int32_t i) {
        const ValueId iv = Terms.MakeLeaf(Kind::Int, Terms.InternStr(std::to_string(i)));
        const EnvId e = Envs.PushValue(env, var, BindKind::Term, iv);
        return pattern ? EvalInPattern(body, e) : Eval(body, e);
    };
    // The 0->0 circuit, so `sum(i, 0, e)` is *not* 0 and `prod(i, 0, e)` not 1.
    const auto empty = [&] {
        const BoxId zero = Boxes.MakeInt(0);
        const BoxId list = Boxes.Make(BoxKind::Par, {zero, zero});
        RouteTable table{0, 0, {0, 0}};
        return Boxes.Make(BoxKind::Route, 0, 0, Boxes.AddRoute(std::move(table)) + 1, {zero, zero, list});
    };

    if (n == 0) {
        if (kind != IterKind::Seq) return empty();
        const BoxId probe = ToSymbolic(at(0));
        if (Boxes.IsError(probe)) return probe;
        const Arity a = Boxes.ArityOf(probe);
        if (!a.Known || a.Ins != a.Outs) return Fail(Code::EvalArityMismatch, subject, "seq's body must have as many inputs as outputs");
        if (a.Outs == 0) return empty();
        BoxId bus = Boxes.Wire;
        for (int32_t j = 1; j < a.Outs; ++j) bus = Boxes.Make(BoxKind::Par, {bus, Boxes.Wire});
        return bus;
    }

    if (kind == IterKind::Sum || kind == IterKind::Prod) {
        const BoxId op = Boxes.MakeLeaf(BoxKind::Prim, uint32_t(kind == IterKind::Sum ? Prim::Add : Prim::Mul));
        BoxId res = at(0); // unfolded, so `sum(i, 3, i)` stays a chain
        for (int32_t i = 1; i < n; ++i)
            res = Compose(
                BoxKind::Seq, Compose(BoxKind::Par, res, at(i), subject), op, subject,
                /*fold=*/false
            );
        return res;
    }
    const BoxKind k = kind == IterKind::Par ? BoxKind::Par : BoxKind::Seq;
    BoxId res = at(n - 1);
    for (int32_t i = n - 2; i >= 0; --i) res = Compose(k, at(i), res, subject, /*fold=*/false);
    return res;
}

BoxId Evaluator::Modulate(ValueId modulator, ValueId body, EnvId env, ValueId subject) {
    const StrId label = EvalLabel(Terms.Get(modulator).Payload, env, subject);
    const std::vector<std::string> path = TargetToPath(Terms.Str(label));

    // An omitted circuit is `*`, with *two* inputs, so `["Wet" -> e]` adds an input.
    const std::vector<ValueId> entries = TermKids(modulator);
    const BoxId circuit = entries.empty() ? Boxes.MakePrim(Prim::Mul) : ToSymbolic(Eval(entries[0], env));
    if (Boxes.IsError(circuit)) return circuit;
    const Arity ca = Boxes.ArityOf(circuit);
    if (!ca.Known || ca.Ins > 2 || ca.Outs != 1)
        return Fail(Code::EvalBadModulationCircuit, subject, "a modulation circuit takes at most 2 inputs and produces exactly 1 output");

    const BoxId slot = ca.Ins == 2 ? Boxes.NewSlot(label) : NoBox;
    const BoxId evaluated = ToSymbolic(Eval(body, env));
    if (Boxes.IsError(evaluated)) return evaluated;

    bool matched = false;
    const BoxId rewritten = Implant(evaluated, path, slot, ca.Ins, circuit, matched);
    if (!matched) Raise(Code::EvalNoModulationTarget, subject, std::string(Terms.Str(label)), Severity::Warning);
    if (ca.Ins != 2) return rewritten;
    // A two-input circuit adds an input, bound to the slot at propagation.
    return Boxes.Make(BoxKind::Symbolic, {slot, rewritten});
}

BoxId Evaluator::Implant(BoxId box, std::span<const std::string> path, BoxId slot, int32_t ins, BoxId circuit, bool &matched) {
    const BoxNode n = Boxes.Get(box);
    const std::vector<BoxId> kids = BoxKids(box);
    const auto widget = [&](StrId label) {
        if (!PathMatchesLabel(path, LabelPath(Terms.Str(label)))) return box;
        matched = true;
        if (ins == 0) return circuit;
        if (ins == 1) return Boxes.Make(BoxKind::Seq, {box, circuit});
        return Boxes.Make(BoxKind::Seq, {Boxes.Make(BoxKind::Par, {box, slot}), circuit});
    };

    switch (Boxes.KindOf(box)) {
        case BoxKind::Button:
        case BoxKind::Checkbox:
        case BoxKind::NumericWidget:
        case BoxKind::Bargraph: return widget(n.Payload);
        case BoxKind::Group: {
            // The walk descends whether or not the group matched, so no group is pruned.
            std::vector<std::string> rest;
            const std::span<const std::string> inner = MatchGroup(LabelPath(Terms.Str(n.Payload)), path, rest) ? std::span<const std::string>(rest) : path;
            const BoxId body = Implant(kids[0], inner, slot, ins, circuit, matched);
            return Boxes.Make(BoxKind::Group, n.Form, n.Payload, n.Aux, {body});
        }
        default: {
            if (kids.empty()) return box;
            std::vector<BoxId> rebuilt;
            rebuilt.reserve(kids.size());
            for (const BoxId c : kids) rebuilt.push_back(Implant(c, path, slot, ins, circuit, matched));
            return Boxes.Rebuild(box, rebuilt);
        }
    }
}

BoxId Evaluator::ToSymbolic(BoxId b) {
    if (const auto it = Symbolic.find(b); it != Symbolic.end()) return it->second;
    BoxId out = b;
    const BoxNode n = Boxes.Get(b);
    switch (Boxes.KindOf(b)) {
        case BoxKind::Closure: {
            if (n.Form == EnvClosure) {
                out = Boxes.MakeLeaf(BoxKind::Environment);
                break;
            }
            const ValueId abstr = n.Payload;
            const EnvId env = n.Aux;
            if (Terms.KindOf(abstr) == Kind::Ident) {
                out = ToSymbolic(Eval(abstr, env));
                break;
            }
            if (Terms.KindOf(abstr) != Kind::Lambda) {
                out = Fail(Code::EvalNotAClosure, NoTerm);
                break;
            }
            // Applying to a slot is what normalises an unapplied definition.
            const std::vector<ValueId> kids = TermKids(abstr);
            const StrId name = Terms.Get(kids[0]).Payload;
            const BoxId slot = Boxes.NewSlot(name);
            const EnvId inner = Envs.PushValue(env, name, BindKind::Value, slot);
            const BoxId body = ToSymbolic(Eval(kids[1], inner));
            out = Boxes.Make(BoxKind::Symbolic, {slot, body});
            break;
        }
        case BoxKind::PatternMatcher: {
            const BoxId slot = Boxes.NewSlot(Terms.InternStr("PM"));
            const BoxId one[] = {slot};
            out = Boxes.Make(BoxKind::Symbolic, {slot, ToSymbolic(Apply(b, one, NoTerm))});
            break;
        }
        case BoxKind::Waveform: break;
        default: {
            const std::vector<BoxId> kids = BoxKids(b);
            if (kids.empty()) break;
            std::vector<BoxId> rebuilt;
            bool changed = false;
            for (const BoxId c : kids) {
                rebuilt.push_back(ToSymbolic(c));
                changed = changed || rebuilt.back() != c;
            }
            if (!changed) break;
            out = Boxes.Rebuild(b, rebuilt);
            // A composition whose children were closures is only checkable now.
            if (Boxes.IsError(out) && !Boxes.IsError(b)) Raise(Code::EvalArityMismatch, NoTerm, std::string(BoxKindName(Boxes.KindOf(b))));
            break;
        }
    }
    Symbolic[b] = out;
    return out;
}

} // namespace faustlens
