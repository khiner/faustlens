// The text buffer for one file. Undo lives in `Workspace`: an undo unit spans
// every open buffer.
#pragma once

#include "syntax/Splice.h"

#include <cstdint>
#include <memory>
#include <string>

namespace faustlens::app {

struct Buffer {
    // Never null. Shared and immutable so recording a snapshot is a refcount, and
    // the pointer is the whole snapshot.
    std::shared_ptr<const std::string> Shared;
    uint32_t Cursor = 0;

    explicit Buffer(std::string t = {}) : Shared(std::make_shared<const std::string>(std::move(t))) {}

    const std::string &Text() const { return *Shared; }
    void SetCursor(uint32_t offset);
    void Restore(std::shared_ptr<const std::string> t, uint32_t at);

    // Neither records anything undoable. The caller snapshots via `Workspace`.
    void Replace(uint32_t begin, uint32_t end, std::string_view with);
    void Apply(const EditScript &);

    static uint32_t Move(uint32_t offset, const EditScript &);
};

} // namespace faustlens::app
