# Faust LLVM JIT comparison

The optional JIT benchmarks compare FaustLens with the pinned Faust LLVM backend on Apple Silicon.
The editor, standalone compiler, and runtime retain their existing dependencies.
The C++ reference benchmarks and acceptance budgets are documented in [NATIVE.md](../NATIVE.md#benchmarks-and-acceptance-budgets).

## Build and run

Build the [reference compiler](../README.md#reference-compiler) for the scalar/vector C++ references.
Build the pinned Faust library with LLVM 21:

```sh
brew install llvm@21
cmake -S lib/faust/build -B build/faust-llvm -G Ninja \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_CONFIG="$(brew --prefix llvm@21)/bin/llvm-config" \
    -DINCLUDE_LLVM=ON -DLLVM_BACKEND=DYNAMIC -DLINK_LLVM_STATIC=OFF \
    -DINCLUDE_DYNAMIC=ON -DINCLUDE_EXECUTABLE=OFF \
    -DINCLUDE_OSC=OFF -DINCLUDE_HTTP=OFF \
    -DINCLUDE_EMCC=OFF -DINCLUDE_WASM_GLUE=OFF
cmake --build build/faust-llvm --target dynamiclib
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFAUSTLENS_BENCHMARKS=ON \
    -DFAUSTLENS_LLVM_LIBRARY="$PWD/lib/faust/build/lib/libfaust.dylib"
cmake --build build
python3 benchmark/run.py --jit --output build/jit-reference.json
python3 benchmark/check.py build/jit-reference.json
```

Use `--jit-baseline build/jit-reference.json --output build/jit-current.json` to measure native performance with saved LLVM results.
The runner checks source, reference binary, library, and measurement settings and records the reused report's date and hash.

`FAUSTLENS_LLVM_LIBRARY` enables two standalone benchmark executables.
Only `faustlens_jit_llvm` links libfaust and LLVM.
Its benchmark-specific entitlement permits loading the separately built libraries.
Both executables use Hardened Runtime and permit JIT execution.
LLVM 21 builds the pinned source without patches.

## Measurement boundaries

Both compilers receive identical source strings with the root file already loaded.
FaustLens creates a fresh Session, lowers the source, emits ARM64, and publishes executable code.
Faust creates an LLVM DSP factory with eager code generation and symbol resolution.
FaustLens resolves standard libraries from embedded data, and Faust reads the same pinned library sources from disk.
OS filesystem caches remain enabled.

| Measurement | Included work |
|---|---|
| `compile_first_ms` | First source-to-executable compilation in a fresh process |
| `compile_p50_ms`, `compile_p95_ms` | Eleven subsequent source-to-executable compilations with each previous factory released |
| `cache_hit_*` | Eleven repeated Faust compile calls while retaining the original factory |
| `create_*` | Allocate an independent instance and build its control interface from retained compiled code |
| `init_*` | Initialize the instance at 48 kHz, including tables and state |
| `first_block_ms` | Execute the first 64-frame block after initialization |
| `process_to_first_block_ms` | Parent-observed process launch through receipt of the flushed first-block result, including loader startup and source loading |
| `render_*` | Warmed DSP execution through the common reference interface |

Uncached Faust runs require an empty factory cache before compilation.
Cache-hit runs require the returned factory pointer to match the retained factory.
FaustLens has no source-to-factory lookup cache, so its cache-hit timings are null.
Instance creation retains compiled code in both backends.
Factory and instance destruction occur outside their timed stages.

Startup measurements use eleven fresh processes per program/backend at 64 frames and retain every sample.
They exclude process teardown and include parent scheduling and result transport.
Compilation/render measurements use one fresh process per program/backend/block combination.
Creation and initialization each use eleven samples, including the first instance.
Percentiles use the nearest-rank convention.

## Execution settings and validation

The LLVM variants use binary64, scalar or 32-sample vector Faust output, and the maximum LLVM optimization setting.
The report records the loaded Faust/LLVM version, detected target, effective Faust options, and binary/library hashes and sizes.
`FAUST_OPT=FAUST_LLVM_NO_FM` disables Faust's LLVM fast-math IR flags and aggressive floating-point fusion.
LLVM still enables its target-level signed-zero relaxation, so reference validation uses finite traces and the existing numerical tolerance.
Native/interpreter bitwise conformance remains a separate test suite.

All variants use the same deterministic inputs, gain change, one-second accuracy comparison, and scalar C++ oracle.
The native and LLVM instances use the same rendering harness as the existing benchmarks.
The benchmark thread uses user-initiated QoS on macOS.
Rendering uses a 100 ms warm-up, then nine batches of at least 65,536 frames, with initialization and 32 warm-up blocks outside each timed batch.
Accuracy checks use the default floating-point environment, and timed rendering flushes denormals.
The throughput measurements include the public runtime calls and common function-pointer dispatch.

The twelve programs cover gain, delay, table access, oscillation, filtering, feedback, arithmetic chains, reverb, and per-sample math calls.
Programs in `dsp/` are included automatically.
Each program exposes the `gain` slider used by the control and edit checks.

| Math workload | Behavior | Calls per sample |
|---|---|---:|
| `waveshaper` | Sine waveshaping | 1 |
| `mathchain` | Combine sine, cosine, and exponential results | 3 |
| `power` | Compute a power with a signal-dependent exponent | 2 |
| `mathwide` | Sum 16 weighted sine harmonics under register pressure | 16 |

`check.py` verifies complete coverage, numerical accuracy, timing boundaries, and the native acceptance budgets.
JIT speed ratios are measurements rather than pass/fail thresholds.
Keep generated reports in the build directory.
