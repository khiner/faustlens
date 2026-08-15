// Every open buffer and the one history over them: undo covers every buffer's text plus
// the control values, as a state.
#pragma once

#include "controls/Store.h"
#include "editor/Buffer.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace faustlens::app {

struct Workspace {
    // One moment of the editable state, sharing its texts with the buffers.
    struct State {
        std::map<std::string, std::shared_ptr<const std::string>> Texts;
        std::map<std::string, uint32_t> Cursors;
        std::shared_ptr<const controls::Values> Controls;
    };

    std::map<std::string, Buffer> Files;
    controls::Values Controls;
    // The values as of the last recorded step, where a pre-gesture value
    // survives a drag.
    controls::Values Committed;
    std::vector<State> UndoStack, RedoStack;

    // Opens or replaces a file's bytes. Not an undoable step.
    void Open(const std::string &path, std::string text);
    bool IsOpen(const std::string &path) const { return Files.contains(path); }
    // One body for both constnesses: `self` carries the caller's.
    auto *Find(this auto &&self, const std::string &path) {
        const auto it = self.Files.find(path);
        return it == self.Files.end() ? nullptr : &it->second;
    }
    // Deterministically ordered.
    std::vector<std::string> Paths() const;

    // One edit to one file, recorded as one step over the whole state.
    bool Edit(const std::string &path, const EditScript &);

    // No `Begin`: the last step already holds the pre-gesture state.
    bool CommitGesture(const controls::Values &now);

    // `Texts` names every file whose bytes moved. Empty means a gesture step,
    // with nothing to recompile.
    struct Step {
        bool Ok = false;
        std::vector<std::string> Texts;

        explicit operator bool() const { return Ok; }
    };
    Step Undo() { return Take(UndoStack, RedoStack); }
    Step Redo() { return Take(RedoStack, UndoStack); }

    State Now() const;
    // Answers with the paths whose bytes moved.
    std::vector<std::string> Restore(const State &);
    void Push();
    Step Take(std::vector<State> &from, std::vector<State> &to);
};

} // namespace faustlens::app
