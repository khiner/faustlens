#include "controls/Style.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace faustlens::controls {

namespace {

// A label may repeat a key, and the values are a set, so the lowest wins.
std::string Meta(const UiNode &n, const std::string &key) {
    const auto it = n.Meta.find(key);
    if (it == n.Meta.end() || it->second.empty()) return "";
    return *it->second.begin();
}

double Lerp(double lo, double hi, double t) { return lo + (hi - lo) * t; }

double Unlerp(double lo, double hi, double v) { return hi == lo ? 0.0 : (v - lo) / (hi - lo); }

double LogSafe(double v) { return std::log(std::max(DBL_EPSILON, v)); }

double ExpSafe(double v) { return std::min(DBL_MAX, std::exp(v)); }

// From the declared step: 1 prints an integer, 0.01 two, and a declared 0 three.
int Decimals(double step) {
    if (!(step > 0)) return 3;
    for (int d = 0; d < 5; ++d) {
        const double scaled = step * std::pow(10.0, d);
        if (std::fabs(scaled - std::round(scaled)) < 1e-9) return d;
    }
    return 5;
}

} // namespace

Style StyleOf(const UiNode &n) {
    Style s;
    const std::string hidden = Meta(n, "hidden");
    // `[hidden]` with no value means hidden. Only an explicit `0` does not.
    s.Hidden = n.Meta.contains("hidden") && hidden != "0";
    s.Knob = Meta(n, "style") == "knob";
    const std::string scale = Meta(n, "scale");
    if (scale == "log") s.Scale = Scale::Log;
    else if (scale == "exp") s.Scale = Scale::Exp;
    s.Unit = Meta(n, "unit");
    s.Tooltip = Meta(n, "tooltip");
    return s;
}

double ToValue(const UiNode &n, Scale scale, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (scale) {
        case Scale::Log: return std::exp(Lerp(LogSafe(n.Min), LogSafe(n.Max), t));
        case Scale::Exp: return std::log(Lerp(ExpSafe(n.Min), ExpSafe(n.Max), t));
        case Scale::Linear: break;
    }
    return Lerp(n.Min, n.Max, t);
}

double ToPosition(const UiNode &n, Scale scale, double v) {
    double t = 0;
    switch (scale) {
        case Scale::Log: t = Unlerp(LogSafe(n.Min), LogSafe(n.Max), LogSafe(v)); break;
        case Scale::Exp: t = Unlerp(ExpSafe(n.Min), ExpSafe(n.Max), ExpSafe(v)); break;
        case Scale::Linear: t = Unlerp(n.Min, n.Max, v); break;
    }
    return std::clamp(t, 0.0, 1.0);
}

double Quantize(const UiNode &n, double v) {
    const double lo = std::min(n.Min, n.Max), hi = std::max(n.Min, n.Max);
    if (n.Step > 0) v = lo + std::round((v - lo) / n.Step) * n.Step;
    return std::clamp(v, lo, hi);
}

std::string Format(const UiNode &n, const Style &s, double v) {
    std::string out = std::format("{:.{}f}", v, Decimals(n.Step));
    if (!s.Unit.empty()) {
        out += ' ';
        out += s.Unit;
    }
    return out;
}

} // namespace faustlens::controls
