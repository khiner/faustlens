#include "editor/Buffer.h"

#include <algorithm>

namespace faustlens::app {

void Buffer::SetCursor(uint32_t offset) { Cursor = std::min<uint32_t>(offset, uint32_t(Shared->size())); }

void Buffer::Restore(std::shared_ptr<const std::string> t, uint32_t at) {
    Shared = std::move(t);
    SetCursor(at);
}

uint32_t Buffer::Move(uint32_t offset, const EditScript &script) {
    int64_t delta = 0;
    for (const Replacement &r : script) {
        if (r.Begin >= offset) break; // a script is disjoint and in source order
        if (r.End <= offset) {
            delta += int64_t(r.Text.size()) - (r.End - r.Begin);
            continue;
        }
        // Inside a replaced region: clamped rather than drifting into new text.
        return uint32_t(int64_t(r.Begin) + delta + std::min<int64_t>(offset - r.Begin, int64_t(r.Text.size())));
    }
    return uint32_t(int64_t(offset) + delta);
}

void Buffer::Apply(const EditScript &script) {
    if (script.empty()) return;
    Shared = std::make_shared<const std::string>(ApplyScript(*Shared, script));
    SetCursor(Move(Cursor, script));
}

void Buffer::Replace(uint32_t begin, uint32_t end, std::string_view with) {
    EditScript one;
    one.push_back({begin, end, std::string(with)});
    Apply(one);
}

} // namespace faustlens::app
