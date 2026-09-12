# Compiler benchmarks

The optional JIT benchmarks compare FaustLens with the pinned Faust LLVM backend on Apple Silicon.
Live-reload measurements are maintained in [FaustEditor](../../FaustEditor/README.md#validation).
The C++ reference benchmarks and acceptance budgets are documented in [README.md](../README.md#benchmarks-and-acceptance-budgets).

## Build and run

Build the [reference compiler](../README.md#reference-compiler) for the scalar/vector C++ references.
Build the pinned Faust library with LLVM 21:

```sh
brew install llvm@21
cmake -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -S lib/faust/build -B build/faust-llvm -G Ninja \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_CONFIG="$(brew --prefix llvm@21)/bin/llvm-config" \
    -DINCLUDE_LLVM=ON -DLLVM_BACKEND=DYNAMIC -DLINK_LLVM_STATIC=OFF \
    -DINCLUDE_DYNAMIC=ON -DINCLUDE_EXECUTABLE=OFF \
    -DINCLUDE_OSC=OFF -DINCLUDE_HTTP=OFF \
    -DINCLUDE_EMCC=OFF -DINCLUDE_WASM_GLUE=OFF
cmake --build build/faust-llvm --target dynamiclib
cmake -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -S . -B build -DCMAKE_BUILD_TYPE=Release -DFAUSTLENS_BENCHMARKS=ON \
    -DFAUSTLENS_LLVM_LIBRARY="$PWD/lib/faust/build/lib/libfaust.dylib"
cmake --build build
python3 benchmark/run.py --jit --output build/jit-reference.json
python3 benchmark/check.py build/jit-reference.json
```

Use `--jit-baseline build/jit-reference.json --output build/jit-current.json` to measure native performance with saved LLVM results.
The runner checks source, reference binary, library, and measurement settings and records the reused report's date and hash.

`FAUSTLENS_LLVM_LIBRARY` enables the LLVM comparison executables.
Only the `_llvm` executables link libfaust and LLVM.
Their benchmark-specific entitlement permits loading the separately built libraries.
The native and LLVM executables use Hardened Runtime and permit JIT execution.
LLVM 21 builds the pinned source without patches.

## Impulse corpus comparison

`corpus.py` compares FaustLens with LLVM scalar across all 94 programs in the pinned [Faust impulse corpus](../lib/faust/tests/impulse-tests/dsp).
After configuring the build above:

```sh
cmake --build build --target faustlens_source_native faustlens_source_llvm \
    faustlens_memory_native faustlens_memory_llvm -j 2
python3 benchmark/corpus.py --llvm --output build/impulse-benchmark.json
python3 benchmark/corpus.py --check build/impulse-benchmark.json
```

The JSON report and Markdown tables cover compilation, initialization, DSP execution at 64/256 frames, generated code size, and memory.
Each program/backend has one fresh-process timing run, with memory and allocation probes in separate processes.
Compilation measures loaded source through executable publication.
Instance creation and initialization are timed separately.
DSP uses binary64 at 48 kHz with the rendering batches and floating-point settings described below.
Aggregate DSP ratios are geometric means with equal weight per program.
DSP p95 is the maximum of nine batch-average timings.

Every program must compile and produce finite output matching LLVM scalar over one second at each block size.
The tolerance is `abs(actual - reference) <= 1e-10 * max(1, abs(reference))`.
Inputs are deterministic, sliders and checkboxes use defaults, buttons remain at one, and soundfiles use synthetic sine data.
The impulse conformance tests separately validate upstream inputs and button transitions.

| Size or memory metric | Scope |
|---|---|
| Generated code | Instructions and constants, excluding metadata, unwind data, and allocation padding |
| Peak memory | Whole-process peak through compilation or rendering |
| Factory heap | Retained compiler data, excluding executable mappings |
| Instance heap | Initialized state, controls, and sound fixtures |
| Runtime resident memory | Initialized process, including libraries and allocator caches |
| DSP allocations | Heap allocation calls during the first block and next 32 blocks at each block size |
| Compiler/runtime executable + required libraries | On-disk stripped benchmark executable and non-system shared libraries |

LLVM shared static tables are reported separately.
Native tables occupy instance storage.

Reuse saved LLVM timings and traces for subsequent native runs, or add footprint measurements without repeating timings:

```sh
python3 benchmark/corpus.py --llvm-baseline build/impulse-benchmark.json \
    --output build/impulse-current.json
python3 benchmark/corpus.py --footprint build/impulse-benchmark.json --output build/impulse-comparison.json
```

Keep the JSON report and adjacent `.traces` directory together.
Reuse requires matching sources, libraries, hardware, OS, LLVM binaries, and measurement settings.
Omit `--llvm` for native-only measurements.
`--llvm-vector` also tests vector compilation.
The pinned Faust backend rejects `osc_enable`, which requires scalar mode.

## Twelve-program JIT measurement boundaries

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

## JIT execution settings and validation

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
Each program exposes the `gain` slider used by the control checks.

| Math workload | Behavior | Calls per sample |
|---|---|---:|
| `waveshaper` | Sine waveshaping | 1 |
| `mathchain` | Combine sine, cosine, and exponential results | 3 |
| `power` | Compute a power with a signal-dependent exponent | 2 |
| `mathwide` | Sum 16 weighted sine harmonics under register pressure | 16 |

`check.py` verifies complete coverage, numerical accuracy, timing boundaries, and the native acceptance budgets.
JIT speed ratios are measurements rather than pass/fail thresholds.
Keep generated reports in the build directory.

## Forward differentiation

Build and run the forward differentiation benchmark in Release mode:

```sh
cmake --build build --target faustlens_differentiate
python3 benchmark/differentiate.py --output build/differentiation.json
python3 benchmark/differentiate.py --corpus --output build/differentiation-corpus.json
```

Each measurement runs in a fresh process and records executable/source hashes, compiler, flags, hardware, library revision, and worktree status.
The DSP fixtures compare plain compilation, all control columns, and one weighted direction.
`ad/filterbank.dsp` also measures 0, 1, 4, and 16 columns on the same sixteen-pole filter bank.
`ad/mutable_table.dsp` exercises tangent writes and delayed reads.
Controls keep their source defaults.
Deterministic corpus inputs use a 0.25 offset to keep the math fixture within its real-valued domains.

The report times normalization from source, differentiation, lowering/emission/publication, instance creation, initialization, and rendering separately.
Rendering uses `Bench.h`: binary64 at 48 kHz, 64/256-frame blocks, 100 ms warm-up, and nine batches of at least 65,536 frames.
The default floating-point environment includes gradual underflow.
Results include instruction counts, code/state/persistent/scratch sizes, and node/child vector capacities.
Peak process RSS also includes interning maps, strings, other compiler allocations, and validation instances.
Reachable tangent nodes include shared primal coefficients; allocated tangent nodes measure arena growth.

Each successful measurement verifies bitwise primal equality against a plain program and finite primal/tangent samples over one second.
`--corpus` runs every pinned impulse program with per-control activity and absent/barrier/unsupported diagnostics.
Analytic and finite-difference derivative checks are in [DifferentiateTest.cpp](../test/unit/DifferentiateTest.cpp).
Unsupported transformations are reported, and build/runtime/validation failures produce a nonzero exit status.
Nonfinite tangents with finite, unchanged primals report `invalid_derivative`, as with the spatializer's default square-root singularities.
