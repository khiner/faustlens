#include "Expand.h"

#include "eval/Lift.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace faustlens::app {
namespace {

struct Context {
    EnvId Env = NilEnv;
    const char *Declined = nullptr;
};

// Return the selected occurrence's lexical environment with symbolic function parameters, or a refusal.
Context ContextAt(Session &s, const FileView &f, RefId at) {
    if (at >= f.Refs.Refs.size()) return {NilEnv, "nothing is selected"};
    if (s.TermsOf(f.Path).Text != f.Text) return {NilEnv, "the source changed; wait for the current diagram"};
    EditContext refs(s.Terms, f.Refs);
    std::vector<RefId> ancestors;
    for (RefId r = at; r != NoRef; r = refs.Parent(r)) ancestors.push_back(r);
    std::ranges::reverse(ancestors);
    EnvId env = s.FileEnv(f.Path).Env;
    for (size_t i = 0; i + 1 < ancestors.size(); ++i) {
        const RefId r = ancestors[i], child = ancestors[i + 1];
        const ValueId v = refs.ValueOf(r);
        const auto children = f.Refs.Children(r);
        switch (s.Terms.KindOf(v)) {
            case Kind::Definition:
                if (children.size() != 1) return {NilEnv, "select a call to choose a pattern clause"};
                break;
            case Kind::Clause:
            case Kind::Lambda: {
                if (child != children.back()) return {NilEnv, "select the function body or a call"};
                std::vector<ValueId> params;
                std::unordered_set<StrId> names;
                for (size_t k = 0; k + 1 < children.size(); ++k) {
                    const ValueId param = refs.ValueOf(children[k]);
                    const Kind kind = s.Terms.KindOf(param);
                    if (kind != Kind::Ident && kind != Kind::Str) return {NilEnv, "select a call to supply the pattern arguments"};
                    const StrId name = s.Terms.Get(param).Payload;
                    if (!names.insert(name).second) return {NilEnv, "select a call to supply the pattern arguments"};
                    params.push_back(s.Terms.MakeLeaf(Kind::Str, name));
                }
                ValueId lambda = s.Eval.NestLambda(params, refs.ValueOf(child));
                for (size_t k = 0; k < params.size(); ++k) {
                    env = s.Eval.SymbolicEnvironment(lambda, env);
                    lambda = s.Terms.Child(lambda, 1);
                }
                break;
            }
            case Kind::With:
            case Kind::Environment: {
                auto defs = s.Eval.TermKids(v);
                if (s.Terms.KindOf(v) == Kind::With) defs.erase(defs.begin());
                env = s.Envs.Push(env, s.Eval.BindingsFromDefs(defs, v, BindKind::Definition, NilEnv));
                break;
            }
            case Kind::Iterate:
                if (child == children.back()) return {NilEnv, "select the iteration to include all its instances"};
                break;
            case Kind::LetRec: return {NilEnv, "select the recursive expression to preserve its bindings"};
            case Kind::Rule: return {NilEnv, "select a call to supply the pattern arguments"};
            default: break;
        }
    }
    return {env};
}

std::vector<SlotName> VisibleSlots(const Session &s, EnvId env) {
    std::unordered_set<StrId> names;
    std::vector<SlotName> slots;
    for (EnvId e = env; e != NilEnv; e = s.Envs.Parent(e))
        for (const Binding &b : s.Envs.Bindings(e)) {
            if (!names.insert(b.Name).second || b.Kind != BindKind::Value || s.Boxes.KindOf(b.Id) != BoxKind::Slot) continue;
            slots.push_back({s.Boxes.Get(b.Id).Aux, b.Name});
        }
    return slots;
}

} // namespace

Edit Expand(Session &session, const FileView &f, RefId at) {
    const Context context = ContextAt(session, f, at);
    if (context.Declined) return {NoRef, NoTerm, context.Declined};
    const ValueId value = f.Refs.Refs[at].ValueId;
    if (!IsExpression(session.Terms.KindOf(value))) return {NoRef, NoTerm, "select an expression"};
    const BoxId box = session.Eval.ToSymbolic(session.Eval.Eval(value, context.Env));
    const Lifted lifted = Lift(session.Terms, session.Boxes, box, VisibleSlots(session, context.Env));
    return lifted ? Edit{at, lifted.Term} : Edit{NoRef, NoTerm, lifted.Declined};
}

} // namespace faustlens::app
