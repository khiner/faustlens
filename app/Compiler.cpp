#include "Compiler.h"

#include "Expand.h"

#include <algorithm>

namespace faustlens::app {

Compiler::Compiler() : Thread([this] { Run(); }) {}
Compiler::~Compiler() { Stop(); }

void Compiler::Stop() {
    {
        std::lock_guard lock(Mutex);
        Stopping = true;
    }
    Wake.notify_one();
    if (Thread.joinable()) Thread.join();
}

uint64_t Compiler::Submit(Request request) {
    std::lock_guard lock(Mutex);
    request.Ticket = ++Latest;
    if (Pending)
        for (const auto &path : Pending->Touched)
            if (!std::ranges::contains(request.Touched, path)) request.Touched.push_back(path);
    Pending = std::move(request);
    if (Ready) Discarded.push_back(std::move(Ready));
    Wake.notify_one();
    return Latest;
}

std::unique_ptr<Compiler::Publication> Compiler::Poll() {
    std::lock_guard lock(Mutex);
    return std::move(Ready);
}

void Compiler::Retire(std::vector<std::shared_ptr<Artifact>> garbage) {
    if (garbage.empty()) return;
    std::lock_guard lock(Mutex);
    for (auto &a : garbage) Retired.push_back(std::move(a));
    Wake.notify_one();
}

void Compiler::Retire(std::unique_ptr<Publication> garbage) {
    if (!garbage) return;
    std::lock_guard lock(Mutex);
    Discarded.push_back(std::move(garbage));
    Wake.notify_one();
}

bool Compiler::Superseded(uint64_t ticket) {
    std::lock_guard lock(Mutex);
    return Stopping || ticket != Latest;
}

void Compiler::Run() {
    Session session;
    audio::Decoder sound;
    std::map<std::string, std::shared_ptr<const std::string>> buffers;
    for (;;) {
        std::optional<Request> request;
        std::vector<std::shared_ptr<Artifact>> retired;
        std::vector<std::unique_ptr<Publication>> discarded;
        bool stop;
        {
            std::unique_lock lock(Mutex);
            Wake.wait(lock, [&] { return Stopping || Pending || !Retired.empty() || !Discarded.empty(); });
            stop = Stopping;
            request = std::move(Pending);
            Pending.reset();
            retired.swap(Retired);
            discarded.swap(Discarded);
            if (stop && Ready) discarded.push_back(std::move(Ready));
        }
        retired.clear();
        discarded.clear();
        if (stop) break;
        if (!request) continue;
        const Request &in = *request;
        for (const auto &path : in.Touched) session.Touch(path);
        for (const auto &[path, text] : buffers)
            if (!in.Buffers.contains(path)) session.ClearBuffer(path);
        for (const auto &[path, text] : in.Buffers) {
            const auto old = buffers.find(path);
            if (old == buffers.end() || *old->second != *text) session.SetBuffer(path, *text);
        }
        buffers = in.Buffers;
        if (Superseded(in.Ticket)) continue;
        auto out = std::make_unique<Publication>();
        out->Ticket = in.Ticket;
        out->DocumentRevision = in.DocumentRevision;
        out->Audio = Live::Build(session, in.Root, in.Current, in.Controls, in.SampleRate, sound);
        if (Superseded(in.Ticket)) continue;
        out->Snap = Publish(session, in.OpenFiles);
        out->AvailableFiles = session.Parsed();
        const FileView *view = out->Snap.File(in.Root);
        const bool same_source = view && view->Text == in.ViewText;
        if (same_source) {
            for (RefId ref : in.Expanded) {
                const auto expanded = Expand(session, *view, ref);
                if (expanded) out->Expanded.emplace(ref, expanded.Value);
                else out->Refused = expanded.Declined ? expanded.Declined : "cannot expand this expression";
            }
        }
        if (in.Materialize) {
            if (!same_source || in.ViewRevision != in.DocumentRevision) {
                out->Refused = "the program changed; select the current expression again";
            } else {
                const auto expanded = Expand(session, *view, *in.Materialize);
                if (!expanded) out->Refused = expanded.Declined ? expanded.Declined : "cannot materialize this expression";
                else out->Materialized = expanded;
            }
        }
        if (in.TraceLabel && in.Current) out->Traced = TraceControl(session, in.Current->Plan, *in.TraceLabel);
        out->Terms = session.Terms;
        {
            std::lock_guard lock(Mutex);
            if (!Stopping && in.Ticket == Latest) {
                if (Ready) Discarded.push_back(std::move(Ready));
                Ready = std::move(out);
            }
        }
    }
}

} // namespace faustlens::app
