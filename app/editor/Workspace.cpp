#include "editor/Workspace.h"

namespace faustlens::app {

void Workspace::Open(const std::string &path, std::string text) { Files.insert_or_assign(path, Buffer(std::move(text))); }

std::vector<std::string> Workspace::Paths() const {
    std::vector<std::string> out;
    out.reserve(Files.size());
    for (const auto &[path, buffer] : Files) out.push_back(path);
    return out;
}

Workspace::State Workspace::Now() const { return {Files, Committed}; }

void Workspace::Push() {
    UndoStack.push_back(Now());
    RedoStack.clear();
}

std::vector<std::string> Workspace::Restore(const State &s) {
    std::vector<std::string> moved;
    for (const auto &[path, buffer] : s.Files) {
        if (Buffer *b = Find(path)) {
            if (buffer.Shared != b->Shared) moved.push_back(path);
            *b = buffer;
        }
    }
    Controls = s.Controls;
    Committed = Controls;
    return moved;
}

bool Workspace::Edit(const std::string &path, const EditScript &script) {
    Buffer *b = Find(path);
    if (b == nullptr || script.empty()) return false;
    Push();
    b->Apply(script);
    return true;
}

bool Workspace::CommitGesture(const controls::Values &now) {
    // Compare with the pre-gesture value to omit gestures that restore it.
    if (Committed == now) return false;
    Push();
    Committed = now;
    Controls = now;
    return true;
}

Workspace::Step Workspace::Take(std::vector<State> &from, std::vector<State> &to) {
    if (from.empty()) return {};
    State const s = std::move(from.back());
    from.pop_back();
    to.push_back(Now());
    return {true, Restore(s)};
}

} // namespace faustlens::app
