#pragma once

#include "syntax/Splice.h"

#include <cstdint>
#include <memory>
#include <string>

namespace faustlens::app {

struct Buffer {
    // Non-null immutable text shared with history entries.
    std::shared_ptr<const std::string> Shared;
    uint32_t Cursor = 0, Anchor = 0;

    explicit Buffer(std::string t = {}) : Shared(std::make_shared<const std::string>(std::move(t))) {}

    const std::string &Text() const { return *Shared; }
    void SetCursor(uint32_t offset);
    void SetSelection(uint32_t cursor, uint32_t anchor);

    // The caller records undo state through Workspace.
    void Replace(uint32_t begin, uint32_t end, std::string_view with);
    void Apply(const EditScript &);

    static uint32_t Move(uint32_t offset, const EditScript &);
};

} // namespace faustlens::app
