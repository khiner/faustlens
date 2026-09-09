#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace faustlens {

// Publish immutable code off the audio thread and retain it until execution has retired.
class Executable {
public:
    static std::expected<std::unique_ptr<Executable>, std::string> Publish(std::span<const uint32_t>);
    ~Executable();
    Executable(const Executable &) = delete;
    Executable &operator=(const Executable &) = delete;
    const void *Address() const { return Address_; }
    size_t Size() const { return Size_; }

private:
    Executable(void *address, size_t size) : Address_(address), Size_(size) {}
    void *Address_;
    size_t Size_;
};

} // namespace faustlens
