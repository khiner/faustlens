// One worker owns the Session; requests and publications own their data.
#pragma once

#include "Live.h"
#include "Trace.h"
#include "query/Snapshot.h"
#include "syntax/Edit.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace faustlens::app {

class Compiler {
public:
    struct Request {
        uint64_t Ticket = 0;
        std::string Root;
        std::map<std::string, std::shared_ptr<const std::string>> Buffers;
        std::vector<std::string> OpenFiles, Touched;
        std::shared_ptr<const Artifact> Current;
        controls::Values Controls;
        double SampleRate = 44100;
        uint64_t DocumentRevision = 0, ViewRevision = 0;
        std::string ViewText;
        std::vector<RefId> Expanded;
        std::optional<RefId> Materialize;
        std::optional<uint32_t> TraceLabel;
    };

    struct Publication {
        uint64_t Ticket = 0, DocumentRevision = 0;
        // UI term construction uses independent syntax pools.
        faustlens::Terms Terms;
        Snapshot Snap;
        Live::Prepared Audio;
        std::vector<std::string> AvailableFiles;
        std::unordered_map<RefId, ValueId> Expanded;
        std::string Refused;
        std::optional<Edit> Materialized;
        std::optional<Trace> Traced;
    };

    Compiler();
    ~Compiler();
    Compiler(const Compiler &) = delete;
    Compiler &operator=(const Compiler &) = delete;

    // Replace queued work and discard superseded results.
    uint64_t Submit(Request);
    std::unique_ptr<Publication> Poll();
    void Retire(std::vector<std::shared_ptr<Artifact>>);
    void Retire(std::unique_ptr<Publication>);
    void Stop();

private:
    std::mutex Mutex;
    std::condition_variable Wake;
    uint64_t Latest = 0;
    bool Stopping = false;
    std::optional<Request> Pending;
    std::unique_ptr<Publication> Ready;
    std::vector<std::shared_ptr<Artifact>> Retired;
    std::vector<std::unique_ptr<Publication>> Discarded;
    std::thread Thread;

    void Run();
    bool Superseded(uint64_t);
};

} // namespace faustlens::app
