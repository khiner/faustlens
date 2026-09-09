#include "runtime/Executable.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace faustlens {
namespace {

#if defined(__APPLE__) && defined(__aarch64__)
struct Arena {
    // Hardened macOS processes permit one MAP_JIT region with physical pages committed on use.
    static constexpr size_t Capacity = 8 * 1024 * 1024;
    struct Region {
        size_t Offset, Size;
    };
    std::mutex Mutex;
    void *Base = MAP_FAILED;
    std::vector<Region> Free;

    ~Arena() {
        if (Base != MAP_FAILED) munmap(Base, Capacity);
    }
};

Arena &CodeArena() {
    static Arena arena;
    return arena;
}
#endif

} // namespace

std::expected<std::unique_ptr<Executable>, std::string> Executable::Publish(std::span<const uint32_t> code) {
#if defined(__APPLE__) && defined(__aarch64__)
    if (code.empty()) return std::unexpected("empty native code");
    Arena &a = CodeArena();
    std::lock_guard lock(a.Mutex);
    if (a.Base == MAP_FAILED) {
        a.Base = mmap(nullptr, Arena::Capacity, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (a.Base == MAP_FAILED) return std::unexpected("MAP_JIT allocation failed: " + std::string(std::strerror(errno)));
        a.Free.push_back({0, Arena::Capacity});
    }
    const size_t bytes = (code.size_bytes() + 15) & ~size_t(15);
    const auto it = std::ranges::find_if(a.Free, [&](const Arena::Region &r) { return r.Size >= bytes; });
    if (it == a.Free.end()) return std::unexpected("native code arena is full");
    void *address = static_cast<char *>(a.Base) + it->Offset;
    auto result = std::unique_ptr<Executable>(new Executable(address, bytes));
    it->Offset += bytes;
    it->Size -= bytes;
    if (!it->Size) a.Free.erase(it);
    pthread_jit_write_protect_np(0);
    std::memcpy(address, code.data(), code.size_bytes());
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(address, code.size_bytes());
    return result;
#else
    return std::unexpected("native execution requires macOS ARM64");
#endif
}

Executable::~Executable() {
#if defined(__APPLE__) && defined(__aarch64__)
    Arena &a = CodeArena();
    std::lock_guard lock(a.Mutex);
    a.Free.push_back({size_t(static_cast<char *>(Address_) - static_cast<char *>(a.Base)), Size_});
    std::ranges::sort(a.Free, {}, &Arena::Region::Offset);
    for (size_t i = 1; i < a.Free.size();)
        if (a.Free[i - 1].Offset + a.Free[i - 1].Size == a.Free[i].Offset) {
            a.Free[i - 1].Size += a.Free[i].Size;
            a.Free.erase(a.Free.begin() + i);
        } else ++i;
#endif
}

} // namespace faustlens
