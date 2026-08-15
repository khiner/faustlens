# FaustLens: a bidirectional editor and compiler for Faust

A from-scratch Faust compiler built around bidirectional editing: a box graph and a text editor showing the same program, either one editable, updating each other as you type, with audio running throughout.

Named for the *lens* of bidirectional programming: the text and the diagram are two views of one program, related by a `get` that renders and a `put` that splices edits back into the source.
It is a *retentive* lens specifically — an edit keeps the bytes of everything it did not change, comments and formatting included.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the design.

## Building

```sh
git submodule update --init                       # doctest, tree-sitter, faust
git -C lib/faust submodule update --init libraries # names the path: --recursive drags in oboe, CLAP, py2max
cmake -S . -B build -G Ninja
ninja -C build
build/test/faustlens_tests        # unit, conformance and property suites
build/test/faustlens_acceptance   # differential accept/reject oracle vs tree-sitter-faust
```

## The oracle

`lib/faust` is an oracle: never linked, never ported, used to generate reference outputs and as the definition of correct behaviour.
Building it and regenerating the five comparison levels:

```sh
cd lib/faust/build
cmake -C backends/regular.cmake -DFIR_BACKEND=COMPILER -B faustdir -G Ninja . && ninja -C faustdir faust
cd ../../.. && test/conformance/regenerate_oracle.sh all
test/conformance/regenerate_oracle.sh compare   # against the shipped reference set
```
