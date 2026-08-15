#include "box/Label.h"

#include <span>

namespace faustlens {
namespace {

std::string Strip(const std::string &s) {
    const size_t i = s.find_first_not_of(" \t");
    const size_t j = s.find_last_not_of(" \t");
    if (i == std::string::npos || j == std::string::npos) return "";
    return s.substr(i, 1 + j - i);
}

} // namespace

std::vector<PathSeg> LabelToPath(std::string_view s) {
    std::vector<PathSeg> out;
    for (;;) {
        if (s.empty()) {
            out.push_back({std::string(), 0, false});
            return out;
        }
        if (s.front() == '/') {
            out.push_back({std::string(), PathRoot, true});
            s.remove_prefix(1);
            continue;
        }
        if (s.starts_with("../")) {
            out.push_back({std::string(), PathParent, true});
            s.remove_prefix(3);
            continue;
        }
        if (s.starts_with("./")) {
            s.remove_prefix(2);
            continue;
        }
        if (s.size() >= 2 && s[1] == ':') {
            const char g = s.front();
            const uint8_t code = g == 'h' ? 1 : (g == 't' ? 2 : 0);
            const size_t slash = s.find('/', 2);
            out.push_back({std::string(s.substr(2, slash == std::string_view::npos ? std::string_view::npos : slash - 2)), code, true});
            if (slash == std::string_view::npos) {
                // A trailing group still leaves a leaf, spelled as the empty name.
                out.push_back({std::string(), 0, false});
                return out;
            }
            s.remove_prefix(slash + 1);
            continue;
        }
        // The leaf, taken whole: a `/` with no `x:` prefix does *not* split.
        out.push_back({std::string(s), 0, false});
        return out;
    }
}

std::string PathText(std::span<const PathSeg> p) {
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (i) out += '/';
        out += p[i].Name;
    }
    return out;
}

void ExtractMetadata(std::string_view full, std::string &label, std::map<std::string, std::set<std::string>> &meta) {
    enum { Label, Escape1, Escape2, Escape3, Key, Value };
    int state = Label, deep = 0;
    std::string key, value, plain;
    const auto close = [&](const std::string &v) {
        meta[Strip(key)].insert(v);
        state = Label;
        key.clear();
        value.clear();
    };

    for (const char c : full) {
        switch (state) {
            case Label:
                if (c == '\\') state = Escape1;
                else if (c == '[') {
                    state = Key;
                    ++deep;
                } else plain += c;
                break;
            case Escape1:
                plain += c;
                state = Label;
                break;
            case Escape2:
                key += c;
                state = Key;
                break;
            case Escape3:
                value += c;
                state = Value;
                break;
            case Key:
                if (c == '\\') state = Escape2;
                else if (c == '[') {
                    ++deep;
                    key += c;
                }
                // Only the outermost colon separates. A nested one belongs to the key.
                else if (c == ':' && deep == 1)
                    state = Value;
                // A key with no colon carries the empty value.
                else if (c == ']' && --deep < 1) close("");
                else key += c;
                break;
            case Value:
                if (c == '\\') state = Escape3;
                else if (c == '[') {
                    ++deep;
                    value += c;
                } else if (c == ']' && --deep < 1) close(Strip(value));
                else value += c;
                break;
            default: break;
        }
    }
    label = Strip(plain);
}

std::string LabelOnly(std::string_view full) {
    std::string label;
    std::map<std::string, std::set<std::string>> ignored;
    ExtractMetadata(full, label, ignored);
    return label;
}

} // namespace faustlens
