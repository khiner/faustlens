// An incremental recompile after an edit must equal a from-scratch compile, failures included.
#include "property/Corpus.h"
#include "query/Query.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include "doctest.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

struct Outcome {
    bool Evaluated = false;
    bool Lowered = false;
    uint64_t Plan = 0;
    int32_t Ins = 0, Outs = 0;
    std::string Ui;

    bool operator==(const Outcome &b) const {
        return Evaluated == b.Evaluated && Lowered == b.Lowered && Plan == b.Plan && Ins == b.Ins && Outs == b.Outs && Ui == b.Ui;
    }
};

void Flatten(const UiNode &n, const std::string &prefix, std::string &out) {
    const std::string here = std::format("{}/{}", prefix, n.Label);
    if (!n.IsGroup) {
        out += std::format("{} {} {} {} {} {}\n", here, int(n.Kind), n.Init, n.Min, n.Max, n.Step);
        return;
    }
    out += std::format("{} group {}\n", here, n.Orient);
    for (const UiNode &c : n.Children) Flatten(c, here, out);
}

Outcome CompileIn(Session &s, const std::string &path) {
    Outcome o;
    Signals sigs;
    const Graph g(s, path, sigs);
    if (!g.Ok) return o;
    o.Evaluated = true;
    const auto plan = g.Lower();
    if (!plan) return o;
    o.Lowered = true;
    o.Plan = Hash(*plan);
    o.Ins = plan->Inputs;
    o.Outs = plan->Outputs;
    Flatten(g.Ui("p"), "", o.Ui);
    return o;
}

struct Rng {
    uint64_t S;
    uint32_t Next() {
        S ^= S << 13;
        S ^= S >> 7;
        S ^= S << 17;
        return uint32_t(S >> 32);
    }
    size_t Below(size_t n) { return n == 0 ? 0 : Next() % n; }
};

// Six kinds, three meant to leave the program alone and three meant to break it.
std::string Edit(const std::string &text, Rng &r, std::string &what) {
    std::string out = text;
    switch (r.Below(6)) {
        case 0: {
            const size_t at = out.rfind('\n', r.Below(out.size()));
            what = "insert a comment";
            out.insert(at == std::string::npos ? 0 : at + 1, "// an edit\n");
            return out;
        }
        case 1: {
            what = "insert a blank line";
            const size_t at = out.rfind('\n', r.Below(out.size()));
            out.insert(at == std::string::npos ? 0 : at + 1, "\n");
            return out;
        }
        case 2: {
            what = "append an unused definition";
            out += "\nunused_by_the_property = 1;\n";
            return out;
        }
        case 3: {
            what = "perturb a digit";
            std::vector<size_t> digits;
            for (size_t i = 0; i < out.size(); ++i)
                if (out[i] >= '0' && out[i] <= '9') digits.push_back(i);
            if (digits.empty()) return out;
            const size_t at = digits[r.Below(digits.size())];
            out[at] = char('0' + (out[at] - '0' + 1 + r.Below(8)) % 10);
            return out;
        }
        case 4: {
            what = "delete a line";
            const size_t from = out.rfind('\n', r.Below(out.size()));
            const size_t begin = from == std::string::npos ? 0 : from + 1;
            const size_t end = out.find('\n', begin);
            out.erase(begin, end == std::string::npos ? std::string::npos : end + 1 - begin);
            return out;
        }
        default: {
            what = "insert a stray character";
            static constexpr char Stray[] = "(),:~<>!_*+/";
            const size_t at = r.Below(out.size() + 1);
            out.insert(at, 1, Stray[r.Below(sizeof Stray - 1)]);
            return out;
        }
    }
}

struct Verdict {
    std::string Name;
    int Compared = 0;
    // A sweep where every edit broke the file would agree vacuously, so both are reported.
    int Lowered = 0, Moved = 0;
    std::string Why;
};

constexpr int Edits = 6;

Verdict Sweep(const fs::path &path) {
    Verdict v;
    v.Name = path.stem().string();
    const std::string canonical = fs::weakly_canonical(path).string();

    std::string text = ReadText(path);
    if (text.empty()) {
        v.Why = "empty";
        return v;
    }

    // The original first, so later compiles hit a warm memo.
    Session live;
    live.AddSearchPath(path.parent_path());
    live.SetBuffer(canonical, text);
    CompileIn(live, canonical);

    // Seeded from the name, so a failure replays from it alone.
    Rng rng{0x9E3779B97F4A7C15ull ^ std::hash<std::string>{}(v.Name)};
    uint64_t previous = 0;
    for (int step = 0; step < Edits; ++step) {
        std::string what;
        text = Edit(text, rng, what);
        live.SetBuffer(canonical, text);
        const Outcome incremental = CompileIn(live, canonical);

        Session fresh;
        fresh.AddSearchPath(path.parent_path());
        fresh.SetBuffer(canonical, text);
        const Outcome scratch = CompileIn(fresh, canonical);

        ++v.Compared;
        v.Lowered += incremental.Lowered ? 1 : 0;
        v.Moved += incremental.Lowered && incremental.Plan != previous ? 1 : 0;
        previous = incremental.Lowered ? incremental.Plan : previous;
        if (!(incremental == scratch)) {
            v.Why = std::format("step {} ({}): ", step, what);
            if (incremental.Evaluated != scratch.Evaluated) v.Why += "one evaluated and one did not";
            else if (incremental.Lowered != scratch.Lowered) v.Why += "one lowered and one did not";
            else if (incremental.Ins != scratch.Ins || incremental.Outs != scratch.Outs) v.Why += "different channel counts";
            else if (incremental.Plan != scratch.Plan) v.Why += "different Plan";
            else v.Why += "different UI tree";
            return v;
        }
    }
    return v;
}

} // namespace

TEST_CASE("incremental equivalence: an incremental recompile equals a compile from scratch") {
    // The impulse programs: all 94 lower, so every step reaches the Plan. Examples cost minutes.
    const std::vector<fs::path> paths = DspPaths();
    REQUIRE_FALSE(paths.empty());

    const std::vector<Verdict> out = MapEach<Verdict>(paths, Sweep);

    int agreed = 0, compared = 0, lowered = 0, moved = 0;
    std::map<std::string, std::string> census; // reason -> first file
    for (const Verdict &v : out) {
        compared += v.Compared;
        lowered += v.Lowered;
        moved += v.Moved;
        if (v.Why.empty()) {
            ++agreed;
            continue;
        }
        if (!census.contains(v.Why)) census[v.Why] = v.Name;
    }
    MESSAGE(
        agreed << " of " << out.size() << " programs agree over " << compared << " edited recompiles; " << lowered << " of those compiled and " << moved
               << " produced a Plan the step before did not"
    );
    for (const auto &[why, who] : census) MESSAGE(who << ": " << why);
    CHECK(agreed == int(out.size()));
}
