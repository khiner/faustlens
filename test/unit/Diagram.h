#pragma once

#include "boxview/Layout.h"
#include "property/Corpus.h"
#include "query/Query.h"
#include "query/Snapshot.h"

#include "doctest.h"

#include <filesystem>
#include <string>

namespace faustlens::test {

// Every node, parents before children.
template<typename F> void Walk(const boxview::Node &n, const F &f) {
    f(n);
    for (const boxview::Node &k : n.Kids) Walk(k, f);
}

// Every corpus `process` body laid out. A pattern-defined or absent `process` is skipped.
template<typename F> void ForEachDiagram(const F &fn) {
    for (const std::filesystem::path &p : DspPaths()) {
        Session s;
        s.AddSearchPath(p.parent_path());
        const std::string path = std::filesystem::weakly_canonical(p).string();
        const Snapshot snap = Publish(s, {path});
        const FileView *f = snap.File(path);
        REQUIRE(f != nullptr);
        const ValueId body = ProcessBody(s.Terms, f->Root);
        if (body == NoTerm) continue;
        boxview::Layout layout(s.Terms, boxview::Metrics{});
        fn(s, *f, body, layout.Run(body));
    }
}

} // namespace faustlens::test
