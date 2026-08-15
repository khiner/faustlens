#include "editor/Workspace.h"

namespace faustlens::app {

void Workspace::Open(const std::string &path, std::string text) {
    const auto it = Files.find(path);
    if (it == Files.end()) Files.emplace(path, Buffer(std::move(text)));
    else it->second.Restore(std::make_shared<const std::string>(std::move(text)), 0);
}

std::vector<std::string> Workspace::Paths() const {
    std::vector<std::string> out;
    out.reserve(Files.size());
    for (const auto &[path, buffer] : Files) out.push_back(path);
    return out;
}

Workspace::State Workspace::Now() const {
    State s;
    for (const auto &[path, buffer] : Files) {
        s.Texts.emplace(path, buffer.Shared);
        s.Cursors.emplace(path, buffer.Cursor);
    }
    // `Committed`, not `Controls`: to the history a drag in flight has not happened.
    s.Controls = std::make_shared<const controls::Values>(Committed);
    return s;
}

void Workspace::Push() {
    UndoStack.push_back(Now());
    RedoStack.clear();
}

std::vector<std::string> Workspace::Restore(const State &s) {
    std::vector<std::string> moved;
    for (const auto &[path, text] : s.Texts) {
        Buffer *b = Find(path);
        if (b == nullptr) continue;
        // Pointer identity: a splice reproducing the bytes still moved the file.
        if (text != b->Shared) moved.push_back(path);
        const auto at = s.Cursors.find(path);
        b->Restore(text, at == s.Cursors.end() ? 0 : at->second);
    }
    // Replaced, not merged: an unmentioned path is a control that had not moved.
    if (s.Controls) Controls = *s.Controls;
    else Controls.clear();
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
    // Against the pre-gesture state, so a drag that came back is no step.
    if (Committed == now) return false;
    Push(); // records `Committed`, so the entry is what to undo *to*
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
