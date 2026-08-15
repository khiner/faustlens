// Scored against the rule each `norm*.dsp` states, where the `.sig` ratchets compare to the dump.
#include "conformance/SigCompare.h"
#include "conformance/Sweep.h"

#include "doctest.h"

#include <array>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

struct Probe {
    int Channel;
    const char *Rule;
    const char *Expected; // the source's "Expected:" line, in `.sig` notation
};

// Three of the six delay probes expect an outcome the reference does not produce: the
// order scale is read off by one.
const Probe Norm3[] = {
    {0, "(k*s)' -> k*s'", R"(SIG = (hslider("1.1_c",2.0,1.0,10.0,1.0)*(IN[0]'));)"},
    {1, "(s/k)' -> s'/k", R"(SIG = ((IN[1]')/hslider("1.2_c",2.0,1.0,10.0,1.0));)"},
    {2, "(s@n)@m -> s@(n+m), n a literal", R"(SIG = (IN[2]@30);)"},
    {3, "(s@n)@m -> s@(n+m), n control-rate", R"(SIG = (IN[3]@(int(hslider("1.4_c1",10.0,1.0,100.0,1.0))+int(hslider("1.4_c2",20.0,1.0,100.0,1.0))));)"},
    // The source writes `abs(s2)*10`, where the canonical product puts the coefficient first.
    {4, "(s@n)@m not simplified, n audio-rate", R"(SIG = ((IN[4]@int(10.0*abs(IN[5])))@int(hslider("1.5_c",20.0,1.0,100.0,1.0)));)"},
    {5, "s@0 -> s", R"(SIG = (IN[6]);)"},
    {6, "int(const) -> const", R"(SIG = (3);)"},
    {7, "float(const) -> const", R"(SIG = (5.0);)"},
    {8, "select2(0,a,b) -> a", R"(SIG = (hslider("3.1_x",0.0,-1.0,1.0,1.0));)"},
    {9, "select2(1,a,b) -> b", R"(SIG = (hslider("3.2_y",0.0,-1.0,1.0,1.0));)"},
    {10, "select2(c,x,x) -> x", R"(SIG = (IN[7]);)"},
};

// `4.4_x`, `5.4_x` and `5.5_x` start at 1 to define their divisions, and promotion makes
// `x*0` the real zero. Eleven expectations the reference misses: operand order
// (1, 11, 12, 34), a `pow` the product normalizer cannot see (28, 29), factoring off by
// a stage (20, 24, 31, 33), a negative sum's sign kept outside (3).
const Probe Norm1[] = {
    {0, "x+x -> 2*x", R"(SIG = (2.0*hslider("1.1_x",0.0,-10.0,10.0,0.1));)"},
    {1, "x+y+x -> 2*x+y", R"(SIG = ((2.0*hslider("1.2_x",0.0,-10.0,10.0,0.1)+hslider("1.2_y",0.0,-10.0,10.0,0.1)));)"},
    {2, "5x-2x -> 3x", R"(SIG = (3.0*hslider("1.3_x",0.0,-10.0,10.0,0.1));)"},
    {3, "2x-5x -> -3x", R"(SIG = (-3.0*hslider("1.4_x",0.0,-10.0,10.0,0.1));)"},
    {4, "x+y-x -> y", R"(SIG = (hslider("1.5_y",0.0,-10.0,10.0,0.1));)"},
    {5, "2.5x+1.2x -> 3.7x", R"(SIG = (3.7*hslider("1.6_x",0.0,-10.0,10.0,0.1));)"},
    // Pairs 6/7, 8/9, 10/11 are one expression written both ways, so failing together is order.
    {6, "x+y", R"(SIG = ((hslider("2.1_x",0.0,-10.0,10.0,0.1)+hslider("2.1_y",0.0,-10.0,10.0,0.1)));)"},
    {7, "y+x -> x+y", R"(SIG = ((hslider("2.1_x",0.0,-10.0,10.0,0.1)+hslider("2.1_y",0.0,-10.0,10.0,0.1)));)"},
    {8, "x*y", R"(SIG = (hslider("2.2_x",0.0,-10.0,10.0,0.1)*hslider("2.2_y",0.0,-10.0,10.0,0.1));)"},
    {9, "y*x -> x*y", R"(SIG = (hslider("2.2_x",0.0,-10.0,10.0,0.1)*hslider("2.2_y",0.0,-10.0,10.0,0.1));)"},
    {10, "(x+y)+z", R"(SIG = (((hslider("2.3_x",0.0,-10.0,10.0,0.1)+hslider("2.3_y",0.0,-10.0,10.0,0.1))+hslider("2.3_z",0.0,-10.0,10.0,0.1)));)"},
    {11, "x+(y+z) -> (x+y)+z", R"(SIG = (((hslider("2.4_x",0.0,-10.0,10.0,0.1)+hslider("2.4_y",0.0,-10.0,10.0,0.1))+hslider("2.4_z",0.0,-10.0,10.0,0.1)));)"},
    {12, "3y+2x-y -> 2*(x+y)", R"(SIG = (2.0*(hslider("2.5_x",0.0,-10.0,10.0,0.1)+hslider("2.5_y",0.0,-10.0,10.0,0.1)));)"},
    {13, "x+0 -> x", R"(SIG = (hslider("3.1_x",0.0,-10.0,10.0,0.1));)"},
    {14, "x-0 -> x", R"(SIG = (hslider("3.2_x",0.0,-10.0,10.0,0.1));)"},
    {15, "x*1 -> x", R"(SIG = (hslider("3.3_x",0.0,-10.0,10.0,0.1));)"},
    {16, "x*0 -> 0", R"(SIG = (0.0);)"},
    {17, "x-x -> 0", R"(SIG = (0);)"},
    {18, "xy+z-yx -> z", R"(SIG = (hslider("3.6_z",0.0,-10.0,10.0,0.1));)"},
    {19, "3x-3x+y -> y", R"(SIG = (hslider("3.7_y",0.0,-10.0,10.0,0.1));)"},
    {20, "2*(x+y) -> 2x+2y", R"(SIG = ((2.0*hslider("4.1_x",0.0,-10.0,10.0,0.1)+2.0*hslider("4.1_y",0.0,-10.0,10.0,0.1)));)"},
    {21, "x-(y-z) -> (x+z)-y", R"(SIG = (((hslider("4.2_x",0.0,-10.0,10.0,0.1)+hslider("4.2_z",0.0,-10.0,10.0,0.1))-hslider("4.2_y",0.0,-10.0,10.0,0.1)));)"},
    {22, "(xy)*(zx) -> x^2*y*z",
     R"(SIG = ((pow(hslider("4.3_x",0.0,-10.0,10.0,0.1),2.0)*hslider("4.3_y",0.0,-10.0,10.0,0.1))*hslider("4.3_z",0.0,-10.0,10.0,0.1));)"},
    {23, "(xy)/x -> y", R"(SIG = (hslider("4.4_y",0.0,-10.0,10.0,0.1));)"},
    {24, "(x+y)/2 -> 0.5x+0.5y", R"(SIG = ((0.5*hslider("4.5_x",0.0,-10.0,10.0,0.1)+0.5*hslider("4.5_y",0.0,-10.0,10.0,0.1)));)"},
    {25, "pow(x,2) -> x^2", R"(SIG = (pow(hslider("5.1_x",0.0,-10.0,10.0,0.1),2.0));)"},
    {26, "x*x -> x^2", R"(SIG = (pow(hslider("5.2_x",0.0,-10.0,10.0,0.1),2.0));)"},
    {27, "(xxy)*(yy) -> x^2*y^3", R"(SIG = (pow(hslider("5.3_x",0.0,-10.0,10.0,0.1),2.0)*pow(hslider("5.3_y",0.0,-10.0,10.0,0.1),3.0));)"},
    {28, "x^5/x^3 -> x^2", R"(SIG = (pow(hslider("5.4_x",1.0,-10.0,10.0,0.1),2.0));)"},
    {29, "x^2/(x*x) -> 1", R"(SIG = (1.0);)"},
    {30, "xy+xz -> x*(y+z)", R"(SIG = (hslider("6.1_x",0.0,-10.0,10.0,0.1)*(hslider("6.1_y",0.0,-10.0,10.0,0.1)+hslider("6.1_z",0.0,-10.0,10.0,0.1)));)"},
    {31, "4x+2y -> 2*(2x+y)", R"(SIG = (2.0*((2.0*hslider("6.2_x",0.0,-10.0,10.0,0.1)+hslider("6.2_y",0.0,-10.0,10.0,0.1))));)"},
    {32, "xxy+xyy -> (x*y)*(x+y)",
     R"(SIG = ((hslider("6.3_x",0.0,-10.0,10.0,0.1)*hslider("6.3_y",0.0,-10.0,10.0,0.1))*(hslider("6.3_x",0.0,-10.0,10.0,0.1)+hslider("6.3_y",0.0,-10.0,10.0,0.1)));)"},
    {33, "3xx-6x -> (3x)*(x-2)", R"(SIG = ((3.0*hslider("6.4_x",0.0,-10.0,10.0,0.1))*((hslider("6.4_x",0.0,-10.0,10.0,0.1)-2.0)));)"},
    {34, "ax+ay+bx+by -> (a+b)*(x+y)",
     R"(SIG = ((hslider("6.5_a",0.0,-10.0,10.0,0.1)+hslider("6.5_b",0.0,-10.0,10.0,0.1))*(hslider("6.5_x",0.0,-10.0,10.0,0.1)+hslider("6.5_y",0.0,-10.0,10.0,0.1)));)"},
};

// `norm2.dsp` states its ordering rule both ways, 2.2 and 4.3 disagreeing. The reference
// builds from order 0 up, so 4.3 is right.
const Probe Norm2[] = {
    {0, "(c+s)-c -> s", R"(SIG = (IN[0]);)"},
    {1, "s+c+s -> c+2s", R"(SIG = ((hslider("c_add_2",0.0,-10.0,10.0,0.1)+2.0*IN[1]));)"},
    {2, "c+s+c -> 2c+s", R"(SIG = ((2.0*hslider("c_add_3",0.0,-10.0,10.0,0.1)+IN[2]));)"},
    {3, "(c*s)/c -> s", R"(SIG = (IN[3]);)"},
    {4, "c*s*c -> s*c^2", R"(SIG = (IN[4]*pow(hslider("c_mul_2",1.0,-10.0,10.0,0.1),2.0));)"},
    {5, "(cc+s)-cc -> s", R"(SIG = (IN[5]);)"},
    {6, "cs+sc -> 2*(s*c)", R"(SIG = (2.0*(IN[6]*hslider("c_cmpd_2",1.0,-10.0,10.0,0.1)));)"},
    {7, "s*c1+s*c2 -> s*(c1+c2)", R"(SIG = (IN[7]*(hslider("c_adv_1",1.0,-10.0,10.0,0.1)+hslider("c_adv_2",1.0,-10.0,10.0,0.1)));)"},
    {8, "(sc3+c4)-(c3s-c5) -> c4+c5", R"(SIG = ((hslider("c_adv_4",1.0,-10.0,10.0,0.1)+hslider("c_adv_5",1.0,-10.0,10.0,0.1)));)"},
    {9, "s*c*s -> c*s^2", R"(SIG = (hslider("c_adv_6",1.0,-10.0,10.0,0.1)*pow(IN[9],2.0));)"},
};

// `math_simp.dsp` has no probe table: it states no expected forms, only `.fir` remarks.

SigFile Channel(const SigFile &f, int c) {
    SigFile one = f;
    one.Outputs.Args = {f.Outputs.Args[size_t(c)]};
    return one;
}

enum class Verdict { Documented, ReferenceDiffers, OursDiffers, DidNotParse };

void RunProbes(const std::string &name, const Probe *probes, size_t n, std::map<Verdict, int> &census, std::vector<std::string> &notes) {
    Program const prog(ImpulseDir() / "dsp" / (name + ".dsp"));
    REQUIRE(prog.Ok);
    const Signals &sigs = prog.Sigs;
    const std::vector<SigId> &outs = prog.Outs;

    const auto dump = ParseSig(ReadText(OracleDir() / (name + ".sig")));
    const bool have_dump = dump && dump->Outputs.Args.size() == outs.size();

    for (size_t i = 0; i < n; ++i) {
        const Probe &p = probes[i];
        const std::array one{outs[size_t(p.Channel)]};

        const auto want = ParseSig(p.Expected);
        if (!want) {
            ++census[Verdict::DidNotParse];
            notes.push_back(std::format("{} {}: the expectation did not parse: {}", name, p.Channel, want.error()));
            continue;
        }
        const auto documented = SigIsomorphic(sigs, one, *want);
        if (documented) {
            ++census[Verdict::Documented];
            continue;
        }
        const std::string ours = PrintSig(sigs, one[0], 4);
        if (have_dump && SigIsomorphic(sigs, one, Channel(*dump, p.Channel))) {
            ++census[Verdict::ReferenceDiffers];
            notes.push_back(std::format("{} {} [{}]: agrees with the dump, not with its comment -- {}", name, p.Channel, p.Rule, ours));
            continue;
        }
        ++census[Verdict::OursDiffers];
        notes.push_back(std::format("{} {} [{}]: {}", name, p.Channel, p.Rule, documented.error()));
    }
}

} // namespace

TEST_CASE("the normalization probes against their own documented rules") {
    std::map<Verdict, int> census;
    std::vector<std::string> notes;

    RunProbes("norm1", Norm1, std::size(Norm1), census, notes);
    RunProbes("norm2", Norm2, std::size(Norm2), census, notes);
    RunProbes("norm3", Norm3, std::size(Norm3), census, notes);

    for (const std::string &n : notes) MESSAGE(n);
    MESSAGE(
        "probes: ", census[Verdict::Documented], " meet their documented rule, ", census[Verdict::ReferenceDiffers], " the reference does not meet either, ",
        census[Verdict::OursDiffers], " ours differ, ", census[Verdict::DidNotParse], " expectations did not parse"
    );
    CHECK(census[Verdict::DidNotParse] == 0);
    CHECK(census[Verdict::Documented] >= 39); // a ratchet
}
