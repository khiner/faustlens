# Architecture

FaustLens provides Faust compilation, DSP execution, and source-preserving transformations.
Source bytes are authoritative; consumers own documents, editing history, and device orchestration.
Pinned `lib/faust` supplies standard-library data and the test oracle's reference compiler.

## Representations

| Representation | Contents | Ownership |
|---|---|---|
| Text | Source bytes, including comments, formatting, and incomplete input | Consumer buffers |
| Term | Interned surface-syntax values and per-file occurrence refs | Session or consumer-owned syntax pools |
| Box | Evaluated, arity-checked diagrams | Session |
| Signal | Hash-consed signal nodes and analysis data | Compiled artifact |
| Plan | Instructions, state layout, UI, soundfiles, and foreign-symbol descriptors | Compiled artifact |
| Instance | Registers, state fields, controls, and execution lifecycle | Caller-owned instance |

Term preserves source forms such as `a+b`, `(a,b) : +`, numeric lexemes, and operator spellings.
Evaluation desugars Term into Box, then propagation constructs Signal.
Analysis and lowering produce a Plan shared by both execution paths.
The native backend compiles Plan instructions to an immutable ARM64 artifact, and the interpreter executes Plan instructions directly.
Both use the same instance state, lifecycle, controls, and DSP state transfer.
[README.md](README.md#execution-and-ownership) defines native execution and acceptance budgets.

Equivalent terms share an interned id across files, while each source occurrence has its own ref and byte spans.
Reparsing rebuilds per-file refs and tokens covering every source byte, including whitespace and comments.
Refs and tokens preserve concrete syntax.
Semantic hashes exclude source positions.
Diagnostics and state-field origins recover source ranges through side tables and ref traversal.

## Editing

### Structural and evaluated views

The editable diagram renders Term before evaluation.
For example, `par(i, 10, osc(i))` remains one structural stage while its evaluated Box contains unrolled circuits.

Expansion evaluates the selected source occurrence in its lexical scope and displays a read-only projection.
Enclosing function parameters become symbolic bindings, and selected calls use their own arguments.
Occurrence refs distinguish equal values evaluated in different scopes.

Materialization lifts the evaluated circuit into Term and splices it into source.
It preserves circuit behavior, including enclosing bindings, and can change surface spelling.
Ambient slots require visible source binder names, and symbolic abstractions receive fresh binders.
Errors, environments, and free slots without visible binders are declined.
Both expansion preview and materialization use the same rewrite result.

### Retentive lens contracts

`get` parses text into Term and an occurrence-specific ref tree.
Structural edits attach links from replacement-term paths to the old source refs whose bytes they retain.
`put` applies the resulting splice to the original source.
These links distinguish retained, relocated, and copied occurrences even when their values are equal.

- **Token coverage:** every source byte belongs to one token.
- **Printer round trip:** parsing a printed surface term preserves its value and spelling within the parser's image.
- **PutGet:** parsing an accepted splice produces the intended rewritten program, including surrounding syntax.
- **Hippocraticness:** writing an unchanged view with unchanged correspondence preserves the source byte-for-byte.
- **Retentiveness:** each explicit link preserves its source occurrence's outer-span bytes at the destination, with grammar-required grouping.
- **Edit boundary:** bytes outside the target span remain unchanged.

Swapping equal-valued occurrences can change correspondence and source bytes.
Materialization has an additional semantic contract: lift, splice, parse, and evaluation preserve the circuit.
These contracts are checked by properties and regressions within the tested domains.

### Splicing

An edit contains a target ref, replacement term, and source links.
Composition, deletion, retext, and rewiring preserve links while constructing replacements.
New stages have no source link even when their values equal existing stages.
The lower-level value-rewrite API supports heuristic alignment for callers without explicit correspondence.

Rendering emits retained source fragments and newly printed text.
A monotonic source cursor produces disjoint replacements in source order.
A fragment preceding the cursor is copied into its destination.
All retained regions are collected before comment processing so relocated and copied comments remain associated with their fragments.

- Links remain within the target's outer span.
- Retained fragments receive grouping required by precedence and argument grammar, including commas below the fragment root.
- Comments in replaced gaps are re-emitted once, except where explicit copying duplicates their containing fragment.
- Adjacent fragments receive spacing when concatenation would change token boundaries, such as `3` followed by `.name`.

For `a : b` to `a : x : b`, links retain `a` and `b`, and the printer emits `x` and the new composition punctuation.
Only the gap between retained stages changes.

### Parsing and printing

A mode-stacked lexer and recursive-descent parser construct values and refs in one pass.
Expression parsing uses shared operator binding powers also used by the printer.
Grouping parentheses remain in ref spans, and the printer derives required grouping from precedence, associativity, and grammar position.

Recovery frames preserve enclosing synchronization tokens and match stop tokens at their entry bracket depth.
A failed frame emits a diagnostic and a Hole containing its source bytes and completed children.
Holes print verbatim and evaluate to Error.
This recovery policy keeps incomplete source editable.

Tree-sitter provides an independent acceptance and token-boundary oracle.

Each edited file is reparsed in full.
The printer emits single-line expressions and line-separated statements, preserving existing formatting through retained spans.

## Incremental compilation

Evaluation memoizes by `(value id, environment id)`.
Interned environments preserve binding order and lexical scope.
Signal construction shares unchanged nodes, and analyses index results by node id.
Output-sensitive iteration uses sorted or insertion order for deterministic recompilation.

The path-addressed query layer tracks file text, VFS revisions, resolution, terms, and file environments.
Each entry records its result, dependencies, and changed/verified revisions.
Dependency verification reuses results until an input changes, and recomputation advances the changed revision only when the result differs.
Term equality uses the value component so whitespace changes can update refs without invalidating semantic dependents.

File resolution uses these layers in order:

1. Open editor buffers.
2. The importing file's directory.
3. Disk search paths.
4. Embedded standard-library sources.

Failed resolution depends on the VFS revision and retries after relevant changes.
Imported definitions merge into file environments; component and library targets resolve lazily.
Import cycles remain uncached because their results depend on query entry order.
Evaluation cycles are detected independently by in-flight memo keys and a depth bound.

### Snapshots and lifetimes

A Session and its query engine require external synchronization.
Session pools and evaluation memos remain append-only for its lifetime.
Published snapshots own file text, refs, tokens, and diagnostics independently of subsequent queries.
Consumers retain syntax pools for terms referenced by their snapshots.
Structural edits require buffer bytes matching the snapshot.
Consumers must reject stale source or dependency revisions before applying materialization.

## Compilation

Evaluation performs desugaring, application, pattern matching, lexical scoping, iteration, label substitution, and metadata collection.
Box construction validates known arities and propagates errors without duplicate enclosing diagnostics.
Partially applied cases retain per-rule environments and match one argument at a time.

Propagation connects Box inputs through the composition operators to construct Signal.
Recursive groups reserve an id before constructing branches and intern the completed group afterward.
Group hashing represents self-references with canonical binding positions.
Projections expose current-sample outputs, and feedback reads pass through explicit delays.

Type analysis computes integer/real nature and constant, block, or sample variability.
Interval analysis bounds delays and indices, controls buffer sizing and clamping, and reports unbounded delay requirements.
Promotion inserts casts around simplification to preserve Faust conversion behavior.
Ordered arithmetic normalization preserves reproducible association, which affects numerical results in feedback networks.

Plan lowering schedules init, control, and sample instructions with numbered values.
Instance storage retains values read across bands or defined under guards.
Each executor owns its temporary storage.
Each delayed signal uses one history buffer sized for its maximum delay.
Bounded init loops fill tables and waveforms.
Guards preserve inactive outputs in state fields across frames.
Select instructions evaluate all branches before selecting a result.
Table dependencies order reads after writes, and attach retains the attached operand's effects.

The instance resolves scalar foreign symbols through a host registry.
Unresolved symbols produce diagnostics and zero-valued reads.

## Runtime and reload

### Instance lifecycle

| Operation | Effect |
|---|---|
| Constants(sampleRate) | Run rate-dependent initialization and table fills |
| ResetControls() | Restore declared UI initial values |
| Clear() | Reset runtime history while preserving init-band writes |
| Init(sampleRate) | Run Constants, ResetControls, and Clear in order |
| Compute(frames, in, out) | Run the control band once and the sample band per frame |

Compilation, allocation, and destruction occur outside the audio callback.
Execution preserves the calling thread's rounding and denormal settings.
UI controls and bargraphs use aligned, relaxed atomic 64-bit accesses between threads.

The UI descriptor preserves group paths, widget bounds, and metadata.
Labels support group prefixes, parent paths, and evaluated iteration substitutions.

The host supplies a decoding callback for soundfile URLs.
Missing audio uses the reference silent defaults and reports diagnostics.

### DSP state transfer

State fields match first by content hash and then by shape hash with numeric literals normalized away.
Shape matches use greedy source-offset proximity with lower-offset tie breaking.
Unmatched fields use initialized state.
Matched delay buffers copy their common history relative to the write head and zero additional slots.

Tables, waveforms, soundfile pointers, and controls are excluded from delay-state matching.

Prepare field matches from immutable Plans and source offsets before publication.
Apply the transfer while both instances are idle, or at a callback boundary before executing the replacement.
Copying allocates nothing and scales with the transferred history size.
The consumer owns publication, crossfading, and instance retirement.

## Library boundaries

`faustlens_compiler` provides parsing, file resolution, evaluation, signal analysis, Plan lowering, and ARM64 compilation.
`faustlens_arm64` generates code and serializes artifacts using the shared layout in `faustlens_layout`.
`faustlens_runtime` provides shared instance state, controls, foreign bindings, and soundfile storage.
`faustlens_native` binds and publishes ARM64 artifacts and creates instances sharing executable code.
`faustlens_interp` executes Plan instructions directly.
`faustlens_migrate` provides optional DSP state transfer.
`faustlens_lens` provides source snapshots, origin queries, printing, splicing, structural edits, and scope-aware materialization.
Standalone native builds include the compiler and shared runtime and require only the embedded Faust libraries as third-party source data.

## Consumers

[FaustEditor](../FaustEditor/README.md) owns documents, history, diagram geometry, selection, widgets, compilation scheduling, and audio-device lifecycle.
It translates user commands into FaustLens edits and applies their source replacements through its history.
[AudioGraphEstimation](../AudioGraphEstimation/README.md) owns objectives, fitting, prediction assessment, and topology search policy.
Both consumers use the same pinned compiler and runtime.

`FindControlOrigins` returns candidate declarations across parsed files and identifies ambiguous matches from equal interned terms.
The consumer chooses which matching file to display.

## Scope and validation

The compiler covers Faust's definition language, block-diagram algebra, signal processing, and native and interpreted execution.
Scope includes lexical environments, pattern matching, imports, iterations, metadata, route, local-definition modification, and modulation.
Fixed-point code generation, additional text backends, reference API compatibility, MIDI/OSC, polyphony, and compiling foreign C are outside the current scope.

The oracle covers 94 programs at five levels:

| Output | Comparison |
|---|---|
| .box | Declared metadata, channel counts, and numerical behavior |
| .nonorm.sig | Unnormalized signal graph structure and literals |
| .type | Supported nature, variability, and interval projections |
| .fir | Reference parser and UI structure |
| .ir | Reference impulse protocol and numerical tolerance |

Regeneration uses the pinned reference compiler, explicit corpus search paths, and double precision.
Tests require every corpus program to compile and compare numerical results independently.
Focused probes cover conversion, remainder, shifts, composition, routing, and index clamping.

Source-editing properties check token coverage, print/parse consistency, identity splices, retained correspondence, and bounded full-program reparsing.
Additional tests compare incremental and fresh compilation, DSP state transfer, and native publication lifetimes.
FaustEditor tests audio continuity, widget history, and application publication.
ASan and UBSan builds exercise the same suites with memory and undefined-behavior checks.
Build and oracle commands are in [README.md](README.md).

## Repository layout

| Directory | Responsibility |
|---|---|
| src/syntax | Lexer, parser, terms, refs, diagnostics, printer, and splices |
| src/files | Overlay VFS and embedded libraries |
| src/eval, src/box | Evaluation, lexical environments, and Box graphs |
| src/signal | Signal graphs, analysis, normalization, Plan, and UI descriptors |
| src/arm64 | Native code generation and artifact serialization |
| src/runtime | Native publication, interpreter, shared state, DSP state transfer, foreign symbols, and soundfiles |
| src/query | Revisions, dependencies, source snapshots, and candidate control origins |
| test | Unit, property, and reference-conformance checks |
| lib | Pinned dependency submodules |

Embedded standard-library data preserves upstream bytes and records its revision with third-party notices.
