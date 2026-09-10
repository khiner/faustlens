# FaustLens: a lightweight ARM64 Faust compiler and bidirectional editor

FaustLens compiles Faust to native ARM64 DSP code with a compact custom backend and no LLVM dependency.
It provides a [standalone compiler library and CLI](#standalone-compiler-and-native-runtime), and a runtime for executing generated DSP.

FaustLens also includes a bidirectional editor with synchronized text and diagram editing, shared undo history, and live audio.
For audio processing on Apple Silicon, the editor compiles Faust programs to ARM64 machine code.
On other platforms, it lowers Faust programs to a `Plan` containing DSP instructions and a state layout.
A C++ interpreter, compiled for the host CPU when FaustLens is built, executes each instruction through a switch on its opcode.
[ARCHITECTURE.md](ARCHITECTURE.md) describes the compiler, lens contracts, and runtime.

Measured across all Faust impulse programs on Apple M5 Max, with binary64 DSP at 48 kHz (2026-09-10):

| Measurement | FaustLens ARM64 | Faust LLVM scalar | FaustLens / LLVM |
|---|---:|---:|---:|
| Total compilation time | 397 ms | 8,264 ms | 0.048× |
| Median compilation time | 1.01 ms | 12.92 ms | 0.078× |
| DSP execution time, 64 frames¹ | 0.362 µs | 0.314 µs | 1.154× |
| DSP execution time, 256 frames¹ | 1.429 µs | 1.255 µs | 1.138× |
| Median peak process memory usage | 3.78 MiB | 41.98 MiB | 0.090× |
| Maximum peak process memory usage | 34.05 MiB | 261.23 MiB | 0.130× |
| Total generated code and constants size | 487.35 KiB | 332.96 KiB | 1.464× |
| Median initialized instance heap size | 3.41 KiB | 7.70 KiB | 0.442× |
| Compiler/runtime disk size² | 2.96 MiB | 135.51 MiB | 0.022× |

¹ Geometric mean across programs of median DSP time per block.
² On-disk stripped benchmark executable and non-system shared libraries.
See [detailed benchmark results](benchmark/RESULTS.md) and [methodology and reproduction commands](benchmark/README.md#impulse-corpus-comparison).

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

These binaries cover unit, property, compiler conformance, widget, and tree-sitter-faust parser acceptance tests.

## Editing

Run `build/app/faustlens_gui path/to/program.dsp`.
Type in the source pane or select a diagram stage to edit it structurally.
Cmd-Z and Cmd-Shift-Z undo and redo text, diagram edits, and control gestures.
Cmd-S saves the active file.
Compilation runs in the background, and incomplete edits preserve the last good audio.
Space previews a selected stage's evaluation, and M materializes it into editable source.

## Standalone compiler and native runtime

Build and run the compile-only CLI:

```sh
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFAUSTLENS_GUI=OFF -DFAUSTLENS_TESTS=OFF
cmake --build build-native --target faustlens_cli
build-native/cli/faustlens-compile program.dsp -o program.f64
```

`-I directory` adds an import search path.
The `.f64` artifact contains ARM64 code, relocations, Plan data, controls, and instance layout for loading without source or recompilation.
It uses the FaustLens runtime ABI and requires the same trust as a native library.
Compilation allocates no DSP state or executable memory and requires no JIT entitlement.

Link from another CMake project:

```cmake
add_subdirectory(path/to/faustlens)
target_link_libraries(my_dsp_host PRIVATE faustlens_native)
faustlens_sign_jit(my_dsp_host)
```

Use `faustlens_compiler` for compilation without the runtime.
Embedded builds compile only requested targets and their dependencies.
See [library boundaries](ARCHITECTURE.md#library-boundaries) for interpreter, DSP state transfer, and lens targets.

Use `Session` and `Graph` to lower source to a `Plan`, then `arm64::Program::Compile` to produce an immutable artifact.
`Program::Encode` and `Program::Decode` save and load its bytes.
`NativeCode::Publish` resolves host bindings and publishes executable code.
`Native::Create` creates independent instances sharing the code and layout.
`Native::Compile` combines compilation, publication, and creation for a single instance.

### Execution and ownership

Instances retain their published code and layout until destruction.
Keep the Registry and its bindings valid and unchanged while published code is in use.
Values supplied through registered variable addresses may change.
The host supplies audio buffers and optional soundfile decoding through `SoundfileReader`.

Call `Init` before `Compute`.
Controls execute once per block, including empty blocks.
Null input arrays or channels produce silence, and null output arrays or channels discard output.
Channel pointers must remain stable during a call, with sample buffers separate from instance state.
Both executors share the [instance lifecycle and DSP state transfer](ARCHITECTURE.md#runtime-and-reload).

macOS executables require Hardened Runtime and the `allow-jit` entitlement to publish code.
The build signs the editor, native tests, and benchmarks with `FAUSTLENS_SIGN_IDENTITY`, which defaults to ad hoc.
See [Apple's JIT requirements](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon).

### Numerical behavior

Real arithmetic and state use binary64, with floating-point reassociation and implicit FMA contraction disabled.
Execution preserves the calling thread's rounding and denormal settings.
Integer arithmetic uses signed 32-bit values with wrapping add, subtract, and multiply.
Float-to-int conversion truncates, saturates outside the integer range, and maps NaN to zero.
Integer shift counts use their low five bits.
Integer division by zero produces zero, and remainder by zero preserves the dividend.
Dividing `INT32_MIN` by -1 produces `INT32_MIN` with zero remainder.
Field accesses clamp unsigned indices to the last slot and treat a zero extent as one slot.
Explicit Plan bitcasts produce a compilation diagnostic.
The frontend omits upstream's internal denormal-handling bitcasts.

Tests round-trip artifacts for all impulse programs and compare output and final state with the interpreter, including split blocks and reinitialization.
Finite bits, signed zero, and infinity signs must match, with NaN payloads unspecified.
Arithmetic tests cover all four rounding modes with gradual underflow and denormal flushing.

## Reference compiler

The pinned `lib/faust` submodule supplies standard-library sources and the reference compiler for conformance checks.
Build it and regenerate the oracle:

```sh
cd lib/faust/build
cmake -C backends/regular.cmake -DFIR_BACKEND=COMPILER -B faustdir -G Ninja .
ninja -C faustdir faust
cd ../../..
test/conformance/regenerate_oracle.sh all
test/conformance/regenerate_oracle.sh compare
```

## Benchmarks and acceptance budgets

Build the [reference compiler](#reference-compiler), then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFAUSTLENS_BENCHMARKS=ON
cmake --build build
python3 benchmark/run.py --output build/backend-current.json
python3 benchmark/check.py build/backend-current.json
```

C++ references use the pinned Faust libraries, binary64, `-O3`, and disabled fast math and FMA contraction, with scalar and 32-sample vector variants.
Each program/backend/block combination runs in a fresh process at 48 kHz.
Rendering uses a 100 ms warm-up and nine batches of at least 65,536 frames.
Compilation uses eleven samples and edit latency uses nine, with nearest-rank percentiles.
Unrounded traces require maximum error at most `1e-10 * max(1, reference_peak)`.
Scaling checks cover arithmetic chains, simultaneous live values, and math calls at 1,000 and 10,000 instructions.

| Measure | Budget |
|---|---|
| Coverage | Compile every valid Plan in the conformance and benchmark corpora |
| Correctness | Pass semantic, lifecycle, control, state transfer, and full-precision checks |
| Native compilation p95 | At most 1 ms for 1,000 instructions and 10 ms for 10,000 instructions |
| Native footprint | Add at most 1 MiB to the stripped interpreter executable |
| Preparation memory | Add at most 32 MiB peak RSS over equivalent fresh-process interpreter preparation |
| Edit preparation p95 | At most 10 ms on the benchmark corpus |
| Paced edit-to-output p95 | At most 10 ms plus one block period plus 1 ms scheduling allowance |
| DSP throughput | At most the greater of 1.5 times the faster reference block time or that time plus 0.25 microseconds, per case |

Native compilation includes executable publication, and edit preparation includes replacement initialization.
Edit-to-output ends at the first paced headless callback using the replacement and excludes audio-device latency.
Native preparation peak RSS includes the initial interpreter instance, while process peak RSS includes the entire benchmark.

The [LLVM comparisons](benchmark/README.md) measure compilation, startup, DSP execution, code size, and memory.
The [impulse corpus comparison](benchmark/README.md#impulse-corpus-comparison) covers all pinned programs and supports reusing saved LLVM measurements.

## Sanitizers

Use a separate build directory and reuse the generated oracle:

```sh
cmake -S . -B build-san -DFAUSTLENS_SANITIZE=ON -DFAUSTLENS_GUI=OFF
cmake --build build-san
FAUSTLENS_ORACLE_DIR="$PWD/build/oracle" build-san/test/faustlens_tests
```
