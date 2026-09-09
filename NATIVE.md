# Native backend

On Apple Silicon, the macOS editor compiles Plan instructions to ARM64 machine code.
Other platforms execute Plan instructions through the interpreter.
Generated code contains host addresses and runs in the compiling process.

## Execution contract

Both executors share the [instance lifecycle and DSP state transfer](ARCHITECTURE.md#runtime-and-reload).

Native entries use the host C ABI:

```cpp
void band(Scalar *values, Scalar *state,
          const double *const *inputs, double *const *outputs, int32_t frames,
          const Parameters *parameters);
```

Parameters supply the sample rate, actual block length, temporary storage, and instance pointer.
Initialization and control entries receive `frames = 1`.
Control entries execute even for an empty block.
The sample entry receives the block length.
Null input arrays or channels produce silence.
Null output arrays or channels discard output.
Channel pointers must remain stable during a call, with sample buffers separate from instance state.

Explicit Plan bitcasts produce a compilation diagnostic.
The frontend omits upstream's internal denormal-handling bitcasts.

## Numerical contract

Real arithmetic and state use binary64.
Integer arithmetic uses signed 32-bit values with wrapping add, subtract, and multiply.
Float-to-int conversion truncates, saturates outside the integer range, and maps NaN to zero.
Integer shift counts use their low five bits.
Exceptional integer division and remainder use ARM64 results.
Arithmetic dependencies, table read/write dependencies, guards, and delay timing are preserved.
Floating-point reassociation and implicit FMA contraction are disabled.

Execution preserves the calling thread's rounding and denormal settings.
Arithmetic tests cover all four rounding modes with gradual underflow and denormal flushing.
The 94-program corpus compares full-precision output and final state with the interpreter, including split blocks and reinitialization.
Finite bits, signed zero, and infinity signs must match.
NaN payloads are unspecified.
Shared tests cover semantic rules, controls, lifecycle, state transfer, and typed foreign calls.
Unrounded upstream benchmark traces supplement the six-decimal oracle and require maximum error at most `1e-10 * max(1, reference_peak)`.

## Platform and ownership

Executable storage uses one 8 MiB virtual `MAP_JIT` arena, with physical pages committed on use.
Code is immutable and reused only after instance destruction.
Writes use thread-local JIT write protection and instruction-cache invalidation before publication.

macOS builds sign the editor, native tests, and benchmark executables with Hardened Runtime and the `allow-jit` entitlement.
`FAUSTLENS_SIGN_IDENTITY` selects a signing identity and defaults to ad hoc.
Local verification covers ad-hoc hardened execution.
Notarization requires release-packaging validation.
See [Apple's JIT requirements](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon).

## Benchmarks and acceptance budgets

Build the [pinned reference compiler](README.md#reference-compiler), then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFAUSTLENS_BENCHMARKS=ON
cmake --build build
python3 benchmark/run.py --output build/backend-current.json
python3 benchmark/check.py build/backend-current.json
```

Scalar and vector C++ references use the pinned Faust and library revisions, binary64, `-O3`, and disabled fast math and FMA contraction.
The vector reference uses 32-sample vectors.
Each program/backend/block combination runs in a fresh process at 48 kHz.
Nine warmed render batches contain at least 65,536 frames each.
Compilation uses eleven samples and edit latency uses nine, with nearest-rank percentiles.
These samples measure throughput and edit latency, with audio-device latency excluded.
Unrounded comparison runs for at least one second and changes gain during the trace.
Compilation scaling covers 1,000 and 10,000 instructions in arithmetic chains, simultaneous live values, and math calls.
Scaling checks exact execution, large spill offsets, and distant branches.

Cold preparation ends with an initialized interpreter instance.
Native compilation includes executable-memory publication and reports the first allocation separately.
Native edits create and initialize the replacement before publication.
Edit-to-output ends at the first paced headless callback block using the replacement.
Scalar/vector runs measure interpreter edit preparation.
Native preparation peak RSS includes the retained initial interpreter.
Process peak RSS includes rendering, references, edits, and callback buffers.
Stripped executables measure the interpreter alone and with native code generation.

| Measure | Budget |
|---|---|
| General coverage | Compile every valid Plan in the existing conformance corpus and benchmark corpus |
| Numerical behavior | Pass shared semantic, lifecycle, control, state transfer, and full-precision comparison checks |
| Native compilation p95 | At most 1 ms for 1,000 instructions and 10 ms for 10,000 instructions |
| Native footprint | Add at most 1 MiB to the stripped interpreter executable |
| Preparation memory | Add at most 32 MiB peak RSS over equivalent fresh-process interpreter preparation |
| Edit preparation p95 | At most 10 ms on the benchmark corpus |
| Paced edit-to-output p95 | At most 10 ms plus one block period plus 1 ms scheduling allowance |
| DSP throughput | Per case, at most the greater of 1.5 times the faster reference block time or that reference time plus 0.25 microseconds |

## Recorded validation

[benchmark/native.json](benchmark/native.json) records 96 isolated runs on an Apple M5 Max with macOS 26.5.2 and Homebrew Clang 23.1.0.
All 24 DSP cases, six compilation cases, and footprint, memory, and edit-latency budgets passed.
The initial subset report is preserved in [benchmark/baseline.json](benchmark/baseline.json).

Release and ASan/UBSan validation passed 301 unit/property/conformance cases and two parser acceptance cases.
Release validation also passed the widget test and hardened signature checks for the editor, tests, and benchmarks.
