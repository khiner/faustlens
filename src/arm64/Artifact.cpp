#include "arm64/Program.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faustlens::arm64 {
namespace {

template<class Archive, class T> void Record(Archive &a, T &v) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<V, Field>) a(v.Kind, v.Nature, v.Sig, v.Hash, v.Shape, v.Origin, v.Extent, v.Ring, v.MaxDelay, v.Label, v.Desc, v.Loop);
    else if constexpr (std::is_same_v<V, Instr>) a(v.Op, v.Form, v.Nature, v.Dst, v.Imm, v.Aux, v.Args, v.ArgCount);
    else if constexpr (std::is_same_v<V, SoundfileDesc>) a(v.Label, v.Channels, v.Urls);
    else if constexpr (std::is_same_v<V, ForeignDesc>) a(v.Kind, v.Name, v.Result, v.Args);
    else if constexpr (std::is_same_v<V, Plan>) a(v.Fields, v.Bands, v.Operands, v.Waves, v.Soundfiles, v.Foreign, v.RequiredMath, v.Labels, v.Regs, v.Inputs, v.Outputs);
    else if constexpr (std::is_same_v<V, UiNode>)
        a(v.IsGroup, v.Orient, v.Kind, v.Label, v.Raw, v.Meta, v.Init, v.Min, v.Max, v.Step, v.WidgetLabel, v.Children);
    else if constexpr (std::is_same_v<V, Relocation>) a(v.Kind, v.Index, v.Word, v.Begin, v.End);
    else static_assert(sizeof(T) == 0, "missing artifact record");
}

template<bool Reading> struct Archive {
    std::conditional_t<Reading, std::span<const uint8_t>, std::vector<uint8_t>> Bytes;
    template<class... T> void operator()(T &&...v) { (Value(v), ...); }
    template<class T> void Value(T &v) {
        using V = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<V, bool>) {
            uint8_t value = 0;
            if constexpr (!Reading) value = v;
            Value(value);
            if constexpr (Reading) {
                if (value > 1) throw std::runtime_error("invalid artifact boolean");
                v = value;
            }
        } else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
            std::array<uint8_t, sizeof(V)> bytes;
            if constexpr (Reading) {
                if (Bytes.size() < bytes.size()) throw std::runtime_error("truncated ARM64 artifact");
                std::ranges::copy(Bytes.first(bytes.size()), bytes.begin());
                Bytes = Bytes.subspan(bytes.size());
            } else bytes = std::bit_cast<decltype(bytes)>(v);
            if constexpr (std::endian::native == std::endian::big) std::ranges::reverse(bytes);
            if constexpr (Reading) v = std::bit_cast<V>(bytes);
            else Bytes.insert(Bytes.end(), bytes.begin(), bytes.end());
        } else if constexpr (requires { std::tuple_size<V>::value; }) {
            for (auto &x : v) Value(x);
        } else if constexpr (requires { v.size(); }) {
            uint32_t size = 0;
            if constexpr (!Reading) {
                if (v.size() > UINT32_MAX) throw std::runtime_error("artifact collection exceeds 32-bit size");
                size = uint32_t(v.size());
            }
            Value(size);
            if constexpr (Reading) {
                if (size > Bytes.size()) throw std::runtime_error("invalid artifact collection size");
                for (uint32_t k = 0; k < size; ++k) {
                    if constexpr (requires { typename V::mapped_type; }) {
                        typename V::key_type key;
                        typename V::mapped_type value;
                        (*this)(key, value);
                        if (!v.emplace(std::move(key), std::move(value)).second) throw std::runtime_error("duplicate artifact metadata key");
                    } else {
                        typename V::value_type value;
                        Value(value);
                        if constexpr (requires { v.push_back(std::move(value)); }) v.push_back(std::move(value));
                        else if (!v.insert(std::move(value)).second) throw std::runtime_error("duplicate artifact metadata value");
                    }
                }
            } else
                for (const auto &x : v) {
                    if constexpr (requires { typename V::mapped_type; }) (*this)(x.first, x.second);
                    else Value(x);
                }
        } else Record(*this, v);
    }
};

constexpr std::array<uint8_t, 8> Magic{'F', 'A', 'U', 'S', 'T', 'A', '6', '4'};
constexpr uint32_t Version = 2;

} // namespace

std::vector<uint8_t> Program::Encode() const {
    Archive<false> writer;
    writer(Magic, Version, Plan, Ui, Words, Entries, Relocations);
    return std::move(writer.Bytes);
}

std::expected<std::shared_ptr<const Program>, std::string> Program::Decode(std::span<const uint8_t> bytes) {
    try {
        Archive<true> reader{bytes};
        std::array<uint8_t, 8> magic;
        uint32_t version;
        reader(magic, version);
        if (magic != Magic || version != Version) return std::unexpected("unsupported ARM64 artifact format");
        faustlens::Plan plan;
        UiNode ui;
        reader(plan, ui);
        if (plan.Regs > bytes.size()) return std::unexpected("invalid artifact register count");
        auto program = std::shared_ptr<Program>(new Program(std::move(plan), std::move(ui)));
        reader(program->Words, program->Entries, program->Relocations);
        if (!reader.Bytes.empty()) return std::unexpected("trailing ARM64 artifact data");
        const size_t size = program->Words.size();
        for (uint32_t entry : program->Entries)
            if (entry >= size) return std::unexpected("invalid artifact entry offset");
        for (const auto &r : program->Relocations) {
            if (r.Begin > r.Word || size_t(r.Word) + 4 > r.End || r.End > size || r.End - r.Begin >= (1u << 25))
                return std::unexpected("invalid artifact relocation range");
            if (r.Kind > Binding::Sound || (r.Kind == Binding::Foreign && r.Index >= program->Plan.Foreign.size()) ||
                (r.Kind == Binding::Math && r.Index >= uint32_t(Ext::AssertBounds)) ||
                (r.Kind == Binding::Sound && r.Index != uint32_t(Op::SoundfileLength) && r.Index != uint32_t(Op::SoundfileRate) &&
                 r.Index != uint32_t(Op::SoundfileRead)))
                return std::unexpected("invalid artifact relocation symbol");
            for (uint32_t k = 0; k < 4; ++k)
                if (program->Words[r.Word + k] != Relocation::AddressWord(program->Words[r.Word] & 31, k))
                    return std::unexpected("invalid artifact address placeholder");
        }
        return program;
    } catch (const std::runtime_error &e) { return std::unexpected(e.what()); }
}

} // namespace faustlens::arm64
