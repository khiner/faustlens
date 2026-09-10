# Architecture

FaustLens combines a Faust compiler, synchronized source and diagram editors, and live audio.
Source bytes are authoritative for the authored program.
Text edits and structural rewrites update those bytes through one Workspace history.
The diagram derives from parsed source occurrences, while audio uses the latest successfully compiled program.

Pinned `lib/faust` supplies standard-library data and the test oracle's reference compiler.

## Major implementation objective

Build a compact backend that generates competitive general DSP code extremely quickly.
Native compilation must be self-contained in the editor, with a small footprint and no major compiler dependencies.

## Representations

| Representation | Contents | Ownership |
|---|---|---|
| Text | Source bytes, including comments, formatting, and incomplete input | Workspace buffers |
| Term | Interned surface-syntax values and per-file occurrence refs | Worker Session and independent publications |
| Box | Evaluated, arity-checked diagrams | Worker Session |
| Signal | Hash-consed signal nodes and analysis data | Compiled artifact |
| Plan | Instructions, state layout, UI, soundfiles, and foreign-symbol descriptors | Compiled artifact |
| Instance | Registers, state fields, controls, and execution lifecycle | Compiled artifact retained through audio use |

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
Preserving surface forms avoids reconstructing syntax after desugaring [R1, R2].

Expansion evaluates the selected source occurrence in its lexical scope and displays a read-only projection.
Enclosing function parameters become symbolic bindings, and selected calls use their own arguments.
Occurrence refs distinguish equal values evaluated in different scopes.

Materialization lifts the evaluated circuit into Term and splices it into source.
It preserves circuit behavior, including enclosing bindings, and can change surface spelling.
Ambient slots require visible source binder names, and symbolic abstractions receive fresh binders.
Errors, environments, and free slots without visible binders are declined.
Both expansion preview and materialization use the same rewrite result.

Evaluated-view updates require a chosen update policy [R3].
FaustLens uses explicit materialization before structural editing.

### Retentive lens contracts

`get` parses text into Term and an occurrence-specific ref tree.
Structural edits attach links from replacement-term paths to the old source refs whose bytes they retain [R4].
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

Quotient lenses define laws modulo chosen equivalences [R5].
Retention additionally specifies preservation of concrete source information.
Standalone parse/print consistency establishes a narrower contract than source-preserving updates [R9, R10].

### Splicing

An edit contains a target ref, replacement term, and source links.
Composition, deletion, retext, and rewiring preserve links while constructing replacements.
New stages have no source link even when their values equal existing stages.
The lower-level value-rewrite API supports heuristic alignment for callers without explicit correspondence [R6, R11].

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
This recovery policy keeps incomplete source editable [R12].

Tree-sitter provides an independent acceptance and token-boundary oracle, with recovery-control discussions in [R7, R8].
Derived parser/printer systems [R9, R10] address consistency, while this editor also requires recovery, source refs, and retained fragments.

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

### Worker publication and lifetimes

One worker owns the single-threaded Session and query engine.
Requests contain immutable buffer copies and coalesce before compilation.
Superseded results are discarded at worker publication checks.

Session pools and evaluation memos remain append-only for its lifetime.
Each publication owns copied syntax pools, file text, refs, tokens, diagnostics, and lifted expansions.
The UI interns replacement terms in its publication's syntax pools.

Structural edits require buffer bytes matching the snapshot.
Materialization also requires the selection's document revision, including dependency changes.
The UI verifies publication tickets before adoption and audio submission.
Stale source highlights are hidden, and document edits clear expansion refs.

Compiled artifacts retain Signal storage, Plan, UI data, and the DSP together.
Requests retain the artifact used to prepare state matching.
The UI rejects preparation against a replaced base.
Retired artifacts and obsolete publications return to the worker for destruction after audio use ends.
Shutdown stops callbacks before destroying artifacts.

## Compilation

Evaluation performs desugaring, application, pattern matching, lexical scoping, iteration, label substitution, and metadata collection.
Box construction validates known arities and propagates errors without duplicate enclosing diagnostics.
Partially applied cases retain per-rule environments and match one argument at a time [R14].

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
The host converts device samples at the boundary.
The audio thread enables denormal flushing, while the reference-comparison harness preserves the oracle's floating-point mode.
UI controls and bargraphs use aligned, relaxed atomic 64-bit accesses between threads.

The UI descriptor preserves group paths, widget bounds, and metadata.
Labels support group prefixes, parent paths, and evaluated iteration substitutions.

The host resolves soundfile URLs and caches both successful and failed decodes across recompiles.
Missing audio uses the reference silent defaults and reports diagnostics.

### DSP state transfer

State fields match first by content hash and then by shape hash with numeric literals normalized away.
Shape matches use greedy source-offset proximity with lower-offset tie breaking [R11].
Unmatched fields use initialized state.
Matched delay buffers copy their common history relative to the write head and zero additional slots.

Workspace retains control values by label path even when the control is absent.
Restoration requires compatible control classes and applies current bounds.
Tables, waveforms, soundfile pointers, and controls are excluded from delay-state matching.

The worker compiles, initializes, decodes soundfiles, and prepares field matches using immutable Plan data and source offsets.
The UI submits a prepared voice through an atomic pointer.
At the next callback boundary, the audio thread copies matched state from the current instance before executing the replacement.
Copying allocates nothing and scales with the transferred history size.
A pending transfer must complete before another is accepted.

The host runs old and new instances during a linear crossfade with weights summing to one.
Equal Plan hashes skip replacement when the backend is unchanged.
Failed compilation preserves the last good audio while the editors continue displaying incomplete source.

## Library boundaries

`faustlens_compiler` provides parsing, file resolution, evaluation, signal analysis, Plan lowering, and ARM64 compilation.
`faustlens_arm64` generates code and serializes artifacts using the shared layout in `faustlens_layout`.
`faustlens_runtime` provides shared instance state, controls, foreign bindings, and soundfile storage.
`faustlens_native` binds and publishes ARM64 artifacts and creates instances sharing executable code.
`faustlens_interp` executes Plan instructions directly.
`faustlens_migrate` provides optional DSP state transfer.
`faustlens_lens` contains printing, source splicing, structural edits, evaluation lifting, and source snapshots.
Standalone native builds include the compiler and shared runtime and require only the embedded Faust libraries as third-party source data.

## Application

SDL3 and SDL_GPU provide platform and rendering support, Dear ImGui provides widgets, and miniaudio provides audio I/O and decoding.

Workspace history records all open buffers, selections, and control values.
Immutable text storage shares unchanged files across history entries.
The multiline widget commits one byte-range replacement per changed frame and uses Workspace undo and redo.
Save writes the active buffer, placing edited embedded-library files beside the root program.

Diagram geometry is derived from node kind and children.
Expansion layout is keyed by source occurrence, and selection tracks a ref chain with a byte anchor for reparsing.
Structural edits use the same rewrite and Workspace splice path.

Source comments and formatting are information retained outside the diagram view [R13].
Persistent diagram coordinates would require additional document state and synchronization rules.

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

Editor properties check token coverage, print/parse consistency, identity splices, retained correspondence, and bounded full-program reparsing.
Additional tests compare incremental and fresh compilation, audio continuity, control responsiveness, publication lifetimes, and widget history.
ASan and UBSan builds exercise the same suites with memory and undefined-behavior checks.
Build and oracle commands are in [README.md](README.md).

## Repository layout

| Directory | Responsibility |
|---|---|
| src/syntax | Lexer, parser, terms, refs, diagnostics, printer, and splices |
| src/files | Overlay VFS and embedded libraries |
| src/eval, src/box | Evaluation, lexical environments, and Box graphs |
| src/signal | Signal graphs, analysis, normalization, Plan, and UI descriptors |
| src/runtime | Native code generation, interpreter, shared state, DSP state transfer, foreign symbols, and soundfiles |
| src/query | Revisions, dependencies, and source snapshots |
| app | Compiler worker, Workspace, diagram, controls, and audio host |
| test | Unit, property, and reference-conformance checks |
| lib | Pinned dependency submodules |

Embedded standard-library data preserves upstream bytes and records its revision with third-party notices.

## References

- **[R1]** Justin Pombrio, Shriram Krishnamurthi.
  *Resugaring: Lifting Evaluation Sequences through Syntactic Sugar.*
  PLDI 2014.
- **[R2]** Zhichao Guan, Yiyuan Cao, Tailai Yu, Ziheng Wang, Di Wang, Zhenjiang Hu.
  *Semantics Lifting for Syntactic Sugar.*
  OOPSLA 2024.
- **[R3]** Mikaël Mayer, Viktor Kunčak, Ravi Chugh.
  *Bidirectional Evaluation with Direct Manipulation.*
  OOPSLA 2018, arXiv:1809.04209.
  [Paper](https://arxiv.org/html/1809.04209v1).
- **[R4]** Zirun Zhu, Zhixuan Yang, Hsiang-Shang Ko, Zhenjiang Hu.
  *Retentive Lenses.*
  2020, arXiv:2001.02031.
  [Paper](https://arxiv.org/pdf/2001.02031).
- **[R5]** J. Nathan Foster, Alexandre Pilkiewicz, Benjamin C. Pierce.
  *Quotient Lenses.*
  ICFP 2008.
  [Paper](https://www.cis.upenn.edu/~bcpierce/papers/quotient-lenses.pdf).
- **[R6]** Sebastian Erdweg, Tamás Szabó, André Pacak.
  *Concise, Type-Safe, and Efficient Structural Diffing* (truediff).
  PLDI 2021.
- **[R7]** tree-sitter issue #1870, *How does one improve the error recovery of a grammar?*
- **[R8]** tree-sitter discussion #1205, *Is there any way to give hints to the error recovery process?*
- **[R9]** Tillmann Rendel, Klaus Ostermann.
  *Invertible Syntax Descriptions: Unifying Parsing and Pretty Printing.*
  Haskell Symposium 2010.
- **[R10]** Kazutaka Matsuda, Meng Wang.
  *FliPpr: A Prettier Invertible Printing System.*
  ESOP 2013; *A System for Deriving Parsers from Pretty-Printers*, New Generation Computing 2018.
  [Paper](https://research-information.bris.ac.uk/ws/files/160992789/Meng_Wang_FliPpr_A_System_for_Deriving_Parsers_from_Pretty_Printers.pdf).
- **[R11]** Davi M. J. Barbosa, Julien Cretin, Nate Foster, Michael Greenberg, Benjamin C. Pierce.
  *Matching Lenses: Alignment and View Update.*
  ICFP 2010.
  [Paper](https://www.cis.upenn.edu/~bcpierce/papers/alignment.pdf).
- **[R12]** Cyrus Omar et al.
  *Total Type Error Localization and Recovery with Holes*, POPL 2024.
  *Live Functional Programming with Typed Holes*, POPL 2019.
- **[R13]** Martin Hofmann, Benjamin C. Pierce, Daniel Wagner.
  *Symmetric Lenses.*
  POPL 2011.
  [Paper](https://www.cis.upenn.edu/~bcpierce/papers/symmetric.pdf).
- **[R14]** Albert Gräf.
  *Left-to-Right Tree Pattern Matching.*
  RTA 1991, LNCS 488.
