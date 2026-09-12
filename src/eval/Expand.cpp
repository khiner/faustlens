#include "eval/Expand.h"

#include "eval/Lift.h"
#include "query/Snapshot.h"

#include <algorithm>
#include <expected>
#include <unordered_set>
#include <vector>

namespace faustlens {
namespace {

std::expected<EnvId, const char *> ContextAt(Session &s, const FileView &f, RefId at) {
    if (at >= f.Refs.Refs.size()) return std::unexpected("nothing is selected");
    if (s.TermsOf(f.Path).Text != f.Text) return std::unexpected("the source changed; wait for the current diagram");
    EditContext refs{s.Terms, f.Refs};
    std::vector<RefId> ancestors;
    for (RefId r{at}; r != NoRef; r = refs.Parent(r)) ancestors.push_back(r);
    std::ranges::reverse(ancestors);
    EnvId env{s.FileEnv(f.Path).Env};
    for (size_t i{0}; i + 1 < ancestors.size(); ++i) {
        const RefId r{ancestors[i]}, child{ancestors[i + 1]};
        const ValueId v{refs.ValueOf(r)};
        const auto children{f.Refs.Children(r)};
        switch (s.Terms.KindOf(v)) {
            case Kind::Definition:
                if (children.size() != 1) return std::unexpected("select a call to choose a pattern clause");
                break;
            case Kind::Clause:
            case Kind::Lambda: {
                if (child != children.back()) return std::unexpected("select the function body or a call");
                std::vector<ValueId> params;
                for (size_t k{0}; k + 1 < children.size(); ++k) {
                    const ValueId param{refs.ValueOf(children[k])};
                    const Kind kind{s.Terms.KindOf(param)};
                    if (kind != Kind::Ident && kind != Kind::Str) return std::unexpected("select a call to supply the pattern arguments");
                    const ValueId name{s.Terms.MakeLeaf(Kind::Str, s.Terms.Get(param).Payload)};
                    if (std::ranges::contains(params, name)) return std::unexpected("select a call to supply the pattern arguments");
                    params.push_back(name);
                }
                ValueId lambda{s.Eval.NestLambda(params, refs.ValueOf(child))};
                for (size_t k{0}; k < params.size(); ++k) {
                    env = s.Eval.SymbolicEnvironment(lambda, env);
                    lambda = s.Terms.Child(lambda, 1);
                }
                break;
            }
            case Kind::With:
            case Kind::Environment: {
                const auto defs{s.Terms.Children(v).subspan(s.Terms.KindOf(v) == Kind::With)};
                env = s.Envs.Push(env, s.Eval.BindingsFromDefs(defs, v, BindKind::Definition, NilEnv));
                break;
            }
            case Kind::Iterate:
                if (child == children.back()) return std::unexpected("select the iteration to include all its instances");
                break;
            case Kind::LetRec: return std::unexpected("select the recursive expression to preserve its bindings");
            case Kind::Rule: return std::unexpected("select a call to supply the pattern arguments");
            default: break;
        }
    }
    return env;
}

std::vector<SlotName> VisibleSlots(const Session &s, EnvId env) {
    std::unordered_set<StrId> names;
    std::vector<SlotName> slots;
    for (EnvId e{env}; e != NilEnv; e = s.Envs.Parent(e))
        for (const Binding &b : s.Envs.Bindings(e)) {
            if (!names.insert(b.Name).second || b.Kind != BindKind::Value || s.Boxes.KindOf(b.Id) != BoxKind::Slot) continue;
            slots.push_back({s.Boxes.Get(b.Id).Aux, b.Name});
        }
    return slots;
}

} // namespace

Edit Expand(Session &session, const FileView &f, RefId at) {
    const auto context{ContextAt(session, f, at)};
    if (!context) return {NoRef, NoTerm, context.error()};
    const ValueId value{f.Refs.Refs[at].ValueId};
    if (!IsExpression(session.Terms.KindOf(value))) return {NoRef, NoTerm, "select an expression"};
    const BoxId box{session.Eval.ToSymbolic(session.Eval.Eval(value, *context))};
    const Lifted lifted{Lift(session.Terms, session.Boxes, box, VisibleSlots(session, *context))};
    return lifted ? Edit{at, lifted.Term} : Edit{NoRef, NoTerm, lifted.Declined};
}

} // namespace faustlens
