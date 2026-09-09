#include "signal/Plan.h"

namespace faustlens {
namespace {
constexpr std::array<std::string_view, size_t(Op::Count_)> OpNames = {
    "const.i", "const.f",   "input",   "output",  "binop",  "ext",  "int",  "float",      "bitcast",  "select2",     "select3",   "load",
    "store",   "sf.length", "sf.rate", "sf.read", "fconst", "fvar", "ffun", "loop.begin", "loop.end", "guard.begin", "guard.end",
};
}

std::string_view OpName(Op o) { return OpNames[size_t(o)]; }

} // namespace faustlens
