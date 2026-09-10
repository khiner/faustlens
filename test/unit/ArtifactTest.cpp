#include "conformance/Sweep.h"
#include "runtime/Native.h"

using namespace faustlens;

namespace {
std::shared_ptr<const arm64::Program> Compile(const char *source) {
    test::Program source_program("/artifact.dsp", source);
    REQUIRE(source_program.Ok);
    auto plan = source_program.Lower();
    REQUIRE(plan);
    auto program = arm64::Program::Compile(std::move(*plan), source_program.Ui("artifact"));
    REQUIRE(program);
    return *program;
}
} // namespace

TEST_CASE("ARM64 artifacts preserve metadata and reject damaged files") {
    auto program = Compile("process = _ * hslider(\"gain[unit:dB]\",0.5,0,1,0.01) : mem;");
    const auto bytes = program->Encode();
    auto decoded = arm64::Program::Decode(bytes);
    REQUIRE(decoded);
    CHECK((*decoded)->Encode() == bytes);
    CHECK((*decoded)->Layout.FieldAt == program->Layout.FieldAt);
    CHECK((*decoded)->Layout.Registers.Persistent == program->Layout.Registers.Persistent);
    for (size_t size = 0; size < bytes.size(); ++size) CHECK_FALSE(arm64::Program::Decode(std::span(bytes).first(size)));
    auto corrupt = bytes;
    corrupt[8] = 0xff;
    CHECK_FALSE(arm64::Program::Decode(corrupt));
    corrupt = bytes;
    corrupt.push_back(0);
    CHECK_FALSE(arm64::Program::Decode(corrupt));
}

TEST_CASE("ARM64 artifact relocation ranges are validated before publication") {
    const auto program = Compile("process = sin(_) + fvariable(float host, \"host.h\");");
    REQUIRE_FALSE(program->Relocations.empty());
    for (size_t k = 0; k < program->Relocations.size(); ++k) {
        auto broken = *program;
        broken.Relocations[k].Word = UINT32_MAX;
        CHECK_FALSE(arm64::Program::Decode(broken.Encode()));
        broken = *program;
        broken.Relocations[k].End = 0;
        CHECK_FALSE(arm64::Program::Decode(broken.Encode()));
    }
    auto broken = *program;
    broken.Entries[0] = uint32_t(broken.Words.size());
    CHECK_FALSE(arm64::Program::Decode(broken.Encode()));
}

TEST_CASE("ARM64 compilation stores layout without allocating delay state") {
    const auto program = Compile("process = _ @ 100000000;");
    CHECK(program->Layout.StateSize >= 100000000);
    CHECK(program->Encode().size() < 16384);
}

#if defined(__APPLE__) && defined(__aarch64__)
TEST_CASE("native instances share code and retain independent state and controls") {
    auto program = Compile("process = _ * hslider(\"gain\",0.5,0,1,0.01) : mem;");
    auto published = NativeCode::Publish(program);
    REQUIRE(published);
    std::weak_ptr<const NativeCode> lifetime = *published;
    auto first = Native::Create(*published), second = Native::Create(*published);
    CHECK(&first->CompiledCode() == &second->CompiledCode());
    CHECK(&first->Registers == &program->Layout.Registers);
    CHECK(&first->FieldAt == &second->FieldAt);
    published->reset();
    program.reset();
    first->Init(48000);
    second->Init(96000);
    first->SetControl(first->ControlsOfKind(UiKind::HSlider)[0], 0.25);
    double input[] = {1, 2, 3}, output[3];
    const double *in[] = {input};
    double *out[] = {output};
    first->Compute(3, in, out);
    CHECK(output[0] == 0);
    CHECK(output[1] == 0.25);
    CHECK(output[2] == 0.5);
    second->Compute(3, in, out);
    CHECK(output[0] == 0);
    CHECK(output[1] == 0.5);
    CHECK(output[2] == 1);
    first.reset();
    CHECK_FALSE(lifetime.expired());
    second->Compute(1, nullptr, out);
    CHECK(output[0] == 1.5);
    second.reset();
    CHECK(lifetime.expired());
}

TEST_CASE("one artifact binds independently to each host registry") {
    auto program = Compile("process = fvariable(float host, \"host.h\"), fconstant(int fSamplingFreq, \"math.h\");");
    const auto bytes = program->Encode();
    auto decoded = arm64::Program::Decode(bytes);
    REQUIRE(decoded);
    double value = 0.25;
    Registry registry = Registry::Builtin();
    registry.AddVariable("host", Nature::Real, &value);
    auto bound = NativeCode::Publish(*decoded, registry), missing = NativeCode::Publish(*decoded);
    REQUIRE(bound);
    REQUIRE(missing);
    auto first = Native::Create(*bound), second = Native::Create(*missing);
    CHECK(first->Diagnostics.empty());
    CHECK(second->Diagnostics.size() == 1);
    first->Init(48000);
    second->Init(96000);
    double host, rate;
    double *out[] = {&host, &rate};
    first->Compute(1, nullptr, out);
    CHECK(host == 0.25);
    CHECK(rate == 48000);
    second->Compute(1, nullptr, out);
    CHECK(host == 0);
    CHECK(rate == 96000);
    value = 0.75;
    first->Compute(1, nullptr, out);
    CHECK(host == 0.75);
    CHECK((*decoded)->Encode() == bytes);
}
#endif
