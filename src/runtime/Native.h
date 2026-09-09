#pragma once

#include "arm64/Program.h"
#include "runtime/Executable.h"
#include "runtime/Instance.h"

namespace faustlens {

class NativeCode {
public:
    // Registry bindings must remain valid and unchanged while this code or its instances exist.
    static std::expected<std::shared_ptr<const NativeCode>, std::string>
    Publish(std::shared_ptr<const arm64::Program>, const faustlens::Registry & = Registry::Builtin());
    const arm64::Program &Program() const { return *Program_; }
    size_t CodeBytes() const { return Executable_->Size(); }

private:
    friend class Native;
    using Entry = void (*)(Scalar *, Scalar *, const double *const *, double *const *, int32_t, const void *);
    std::shared_ptr<const arm64::Program> Program_;
    const Registry &Registry_;
    std::unique_ptr<Executable> Executable_;
    std::array<Entry, 3> Entries;

    NativeCode(std::shared_ptr<const arm64::Program> p, const Registry &r, std::unique_ptr<Executable> e)
        : Program_(std::move(p)), Registry_(r), Executable_(std::move(e)) {}
};

class Native final : public Instance {
public:
    static std::expected<std::unique_ptr<Native>, std::string>
    Compile(const faustlens::Plan &, const UiNode &, const faustlens::Registry & = Registry::Builtin());
    static std::unique_ptr<Native> Create(std::shared_ptr<const NativeCode>);
    const NativeCode &CompiledCode() const { return *Code; }
    size_t CodeBytes() const { return Code->CodeBytes(); }

private:
    explicit Native(std::shared_ptr<const NativeCode>);
    std::vector<Scalar> Scratch;
    std::shared_ptr<const NativeCode> Code;
    void Execute(Band, int32_t, const double *const *, double *const *) override;
};

} // namespace faustlens
