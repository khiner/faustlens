# FaustLens: a bidirectional editor and compiler for Faust

FaustLens is a from-scratch Faust compiler with synchronized text and diagram editing and live audio.
Both editors update one source document through shared undo history.
Structural edits preserve linked source occurrences, including comments and formatting.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the representations, lens contracts, compilation, and runtime.

## Building

```sh
git submodule update --init
git -C lib/faust submodule update --init libraries
cmake -S . -B build -G Ninja
ninja -C build
build/test/faustlens_tests
build/test/faustlens_widget_tests
build/test/faustlens_acceptance
```

The test binaries cover unit, property, and compiler conformance checks, widget interaction, and parser acceptance against tree-sitter-faust.

## Editing

Run `build/app/faustlens_gui path/to/program.dsp`.
Type in the source pane or select a diagram stage to edit it structurally.
Cmd-Z and Cmd-Shift-Z undo and redo text, diagram edits, and control gestures through one history.
Cmd-S saves the active file.
Compilation runs in the background, and incomplete edits preserve the last good audio.
Space previews a selected stage's evaluation, and M materializes it into editable source.

## Reference compiler

The pinned `lib/faust` submodule defines reference behavior and generates comparison outputs.
FaustLens uses its standard-library sources as data and implements its compiler independently.

Build the reference compiler and regenerate the oracle:

```sh
cd lib/faust/build
cmake -C backends/regular.cmake -DFIR_BACKEND=COMPILER -B faustdir -G Ninja .
ninja -C faustdir faust
cd ../../..
test/conformance/regenerate_oracle.sh all
test/conformance/regenerate_oracle.sh compare
```

## Sanitizers

Use a separate build directory and reuse the generated oracle:

```sh
cmake -S . -B build-san -DFAUSTLENS_SANITIZE=ON -DFAUSTLENS_GUI=OFF
cmake --build build-san
FAUSTLENS_ORACLE_DIR="$PWD/build/oracle" build-san/test/faustlens_tests
```
