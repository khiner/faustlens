// History spans all open buffers, selections, and control values.
#pragma once

#include "controls/Store.h"
#include "editor/Buffer.h"

#include <map>
#include <string>
#include <vector>

namespace faustlens::app {

struct Workspace {
    struct State {
        std::map<std::string, Buffer> Files;
        controls::Values Controls;
    };

    std::map<std::string, Buffer> Files;
    controls::Values Controls;
    // Last recorded state, preserved during a drag.
    controls::Values Committed;
    std::vector<State> UndoStack, RedoStack;

    // Open or replace a file without recording history.
    void Open(const std::string &path, std::string text);
    bool IsOpen(const std::string &path) const { return Files.contains(path); }
    auto *Find(this auto &&self, const std::string &path) {
        const auto it = self.Files.find(path);
        return it == self.Files.end() ? nullptr : &it->second;
    }
    std::vector<std::string> Paths() const;

    // Record a file edit as one history step.
    bool Edit(const std::string &path, const EditScript &);

    // Record changed controls against the last committed state.
    bool CommitGesture(const controls::Values &now);

    // `Texts` lists changed files; an empty list requires no recompilation.
    struct Step {
        bool Ok = false;
        std::vector<std::string> Texts;

        explicit operator bool() const { return Ok; }
    };
    Step Undo() { return Take(UndoStack, RedoStack); }
    Step Redo() { return Take(RedoStack, UndoStack); }

    State Now() const;
    // Return paths whose text changed.
    std::vector<std::string> Restore(const State &);
    void Push();
    Step Take(std::vector<State> &from, std::vector<State> &to);
};

} // namespace faustlens::app
