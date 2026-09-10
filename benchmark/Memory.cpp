#include "Source.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <malloc/malloc.h>

using namespace corpus;

namespace {

struct Memory {
    int64_t Heap;
    uint64_t Resident;
    static Memory Read() {
        malloc_statistics_t heap{};
        malloc_zone_statistics(nullptr, &heap);
        mach_task_basic_info_data_t task{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &count) != KERN_SUCCESS)
            throw std::runtime_error("cannot read resident memory");
        return {int64_t(heap.size_in_use), task.resident_size};
    }
};

class Allocations {
    using Logger = void(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t);
    Logger **Slot;
    inline static thread_local Allocations *Active = nullptr;
    static void Log(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t result, uint32_t) {
        // Apple's malloc logger marks successful allocations with bit 1, including reallocations.
        if (Active && (type & 2) && result) ++Active->Count;
    }

public:
    uint64_t Count = 0;
    Allocations() : Slot(reinterpret_cast<Logger **>(dlsym(RTLD_DEFAULT, "malloc_logger"))) {
        if (!Slot || *Slot || Active) throw std::runtime_error("malloc logger unavailable or already active");
        Active = this;
        *Slot = Log;
    }
    ~Allocations() {
        *Slot = nullptr;
        Active = nullptr;
    }
};

void CheckAllocationLogger() {
    uint64_t count;
    {
        Allocations allocations;
        auto *zone = malloc_default_zone();
        void *p = malloc_zone_malloc(zone, 37);
        p = malloc_zone_realloc(zone, p, 71);
        void *q = malloc_zone_calloc(zone, 3, 19);
        malloc_zone_free(zone, p);
        malloc_zone_free(zone, q);
        count = allocations.Count;
    }
    if (count != 3) throw std::runtime_error("malloc logger failed its allocation self-check");
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: MEMORY_BENCH source.dsp native|llvm-scalar|llvm-vector OBJECT_FILE");
        PrepareThread();
        CheckAllocationLogger();
        const auto path = std::filesystem::canonical(argv[1]);
        std::ifstream file(path);
        if (!file) throw std::runtime_error("cannot read source");
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        const std::string backend = argv[2];
        const auto baseline = Memory::Read();
        auto compiled = Compile(path, source, backend);
        {
            ReferenceOwner warm(Create(compiled));
            warm.Dsp.Init(warm.Dsp.Object, SampleRate);
        }
        const auto factoryMemory = Memory::Read();
        Memory initialized{};
        uint64_t firstAllocations[2]{}, warmAllocations[2]{};
        int inputs = 0, outputs = 0;
        {
            ReferenceOwner owner(Create(compiled));
            const auto dsp = owner.Dsp;
            inputs = dsp.Inputs;
            outputs = dsp.Outputs;
            dsp.Init(dsp.Object, SampleRate);
            dsp.Control(dsp.Object, 1);
            initialized = Memory::Read();
            for (size_t b = 0; b < std::size(Blocks); ++b) {
                Buffers data(inputs, outputs, Blocks[b], 0.25);
                dsp.Init(dsp.Object, SampleRate);
                dsp.Control(dsp.Object, 1);
                {
                    Allocations allocations;
                    dsp.Compute(dsp.Object, Blocks[b], data.Ip.data(), data.Op.data());
                    firstAllocations[b] = allocations.Count;
                }
                {
                    Allocations allocations;
                    for (int i = 0; i < 32; ++i) dsp.Compute(dsp.Object, Blocks[b], data.Ip.data(), data.Op.data());
                    warmAllocations[b] = allocations.Count;
                }
            }
        }
        const auto retained = Memory::Read();
#ifdef BENCH_LLVM
        if (!writeDSPFactoryToMachineFile(compiled.Code.get(), argv[3], "")) throw std::runtime_error("cannot export LLVM object code");
        std::ofstream metadata(std::string(argv[3]) + ".json");
        metadata << compiled.Code->getJSON();
        metadata.close();
        if (!metadata) throw std::runtime_error("cannot export LLVM metadata");
        const auto options = compiled.Code->getCompileOptions();
#else
        const size_t codeBytes = compiled.Code->Program().Words.size() * sizeof(uint32_t);
        const size_t executableBytes = compiled.Code->CodeBytes();
#endif
        // Export and metadata temporaries are outside the factory release measurement.
        const auto beforeRelease = Memory::Read();
        compiled.Code.reset();
        const auto afterRelease = Memory::Read();
        std::cout << "{\"case\":" << std::quoted(path.stem().string()) << ",\"backend\":" << std::quoted(backend) << ",\"inputs\":" << inputs
                  << ",\"outputs\":" << outputs << ",\"compiler_heap_delta_bytes\":" << factoryMemory.Heap - baseline.Heap
                  << ",\"retained_factory_heap_bytes\":" << beforeRelease.Heap - afterRelease.Heap
                  << ",\"instance_heap_delta_bytes\":" << initialized.Heap - factoryMemory.Heap << ",\"runtime_resident_bytes\":" << initialized.Resident
                  << ",\"post_instance_heap_delta_bytes\":" << retained.Heap - factoryMemory.Heap << ",\"allocations\":[";
        for (size_t b = 0; b < std::size(Blocks); ++b) {
            if (b) std::cout << ',';
            std::cout << "{\"block\":" << Blocks[b] << ",\"first\":" << firstAllocations[b] << ",\"next_32\":" << warmAllocations[b] << '}';
        }
        std::cout << ']';
#ifdef BENCH_LLVM
        std::cout << ",\"options\":" << std::quoted(options);
#else
        std::cout << ",\"code_bytes\":" << codeBytes << ",\"executable_allocation_bytes\":" << executableBytes;
#endif
        std::cout << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
