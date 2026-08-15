#include "Expand.h"

#include "eval/Lift.h"

#include <span>

namespace faustlens::app {

Expansion Expand(Session &session, ValueId v) {
    Expansion out;
    if (v == NoTerm) return out;
    const Evaluator::Evaluated seen = session.Eval.EvaluatedIn(v);
    // An unused definition is legal, so this is a reason, not an empty box.
    if (seen.Envs == 0) {
        out.Declined = "this is not reached by the program, so it has no evaluated form";
        return out;
    }
    out.Ambiguous = seen.Ambiguous;
    const Lifted lifted = Lift(session.Terms, session.Boxes, session.Eval.Eval(v, seen.Env));
    if (!lifted) {
        out.Declined = lifted.Declined;
        return out;
    }
    out.Term = lifted.Term;
    return out;
}

std::unordered_map<ValueId, ValueId> ExpandAll(Session &session, std::span<const ValueId> open) {
    std::unordered_map<ValueId, ValueId> out;
    for (const ValueId v : open)
        if (const Expansion e = Expand(session, v)) out.emplace(v, e.Term);
    return out;
}

Edit Materialize(Session &session, const FileView &f, const boxview::Selection &sel) {
    const RefId at = boxview::SelectedRef(f, sel);
    if (at == NoRef) return {NoRef, NoTerm, "nothing is selected"};
    const Expansion e = Expand(session, f.Refs.Refs[at].ValueId);
    if (!e) return {NoRef, NoTerm, e.Declined};
    return {at, e.Term};
}

} // namespace faustlens::app
