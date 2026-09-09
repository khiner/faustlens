# FaustLens: a bidirectional editor and compiler for Faust

FaustLens is a from-scratch Faust compiler with synchronized text and diagram editing and live audio.
Both editors update one source document through shared undo history.
Structural edits preserve linked source occurrences, including comments and formatting.
On Apple Silicon, the editor generates native DSP code with its own compact compiler backend.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the representations, lens contracts, compilation, and runtime.
[NATIVE.md](NATIVE.md) defines native execution and performance budgets.

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

## Standalone compiler and native runtime

Build and run the compile-only CLI:

```sh
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFAUSTLENS_GUI=OFF -DFAUSTLENS_TESTS=OFF
cmake --build build-native --target faustlens_cli
build-native/cli/faustlens-compile program.dsp -o program.f64
```

`-I directory` adds an import search path.
The `.f64` file contains ARM64 code, symbolic relocations, Plan data, and controls for loading in another process.
Compilation allocates no DSP state or executable memory and requires no JIT entitlement.

Link from another CMake project:

```cmake
add_subdirectory(path/to/faustlens)
target_link_libraries(my_dsp_host PRIVATE faustlens_native)
faustlens_sign_jit(my_dsp_host)
```

Use `faustlens_compiler` instead for source-to-ARM64 compilation without the runtime.
Embedded builds compile only the requested targets and their dependencies.

Use `Session` and `Graph` to lower source to a `Plan`, then `arm64::Program::Compile` to produce an immutable artifact.
`Program::Encode` and `Program::Decode` save and load its bytes.
`NativeCode::Publish` resolves host bindings and publishes executable code.
`Native::Create` creates independent DSP instances sharing the code and immutable layout.
`Native::Compile` combines these steps for a single instance.

Artifacts own their Plan and UI data, and instances retain their published code.
Keep the Registry and its bindings valid and unchanged while published code is in use.
Values supplied through registered variable addresses may change.
Call `Init` before `Compute`.
The host supplies audio buffers and optional soundfile decoding through `SoundfileReader`.
See [library boundaries](ARCHITECTURE.md#library-boundaries) for optional execution, DSP state transfer, and lens targets.

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
