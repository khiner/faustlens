#pragma once

#include "runtime/Layout.h"

#include <memory>

namespace faustlens::arm64 {

enum class Binding : uint8_t { Math, Foreign, Sound };

struct Relocation {
    Binding Kind;
    uint32_t Index, Word, Begin, End;

    static uint32_t AddressWord(uint32_t reg, uint32_t part, uint64_t address = 0) {
        return (part ? 0xf2800000 | part << 21 : 0xd2800000) | uint32_t((address >> (16 * part)) & 0xffff) << 5 | reg;
    }
};

struct Program {
    faustlens::Plan Plan;
    UiNode Ui;
    InstanceLayout Layout;
    std::vector<uint32_t> Words;
    std::array<uint32_t, 2> Entries; // Initialization and block processing.
    std::vector<Relocation> Relocations;

    static std::expected<std::shared_ptr<const Program>, std::string> Compile(faustlens::Plan, UiNode);
    std::vector<uint8_t> Encode() const;
    // Artifacts contain executable code and must come from a trusted compiler.
    static std::expected<std::shared_ptr<const Program>, std::string> Decode(std::span<const uint8_t>);

private:
    Program(faustlens::Plan p, UiNode ui) : Plan(std::move(p)), Ui(std::move(ui)), Layout(Plan) {}
};

} // namespace faustlens::arm64
