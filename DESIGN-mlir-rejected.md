# A Faust compiler built on MLIR

Design for a complete, correct, independent, from-scratch reimplementation of the Faust compiler,
structured as a stack of MLIR dialects.

Reference implementation surveyed: `lib/faust` @ `515dc515c` (2.85.9-25).
Target MLIR/LLVM: 22.x (`/opt/homebrew/opt/llvm`, `mlir-opt`/`mlir-tblgen` present).

---

## 1. Scope and success criteria

Three words in the goal each mean something testable.

**Independent** — the shipped compiler links no libfaust code. Reference Faust appears only in CI,
built from the `lib/faust` submodule, as a test oracle. No source is ported verbatim, its algorithms
are reimplemented from a reading of the reference plus the published papers.

**Complete** — the language surface of Faust 2.85, specifically:

- Full definition language: definitions, `with`, `letrec`, `environment`, abstraction/application,
  pattern-matching `case`, `import`, `component`, `library`, `declare`, metadata.
- Full block-diagram algebra: `: , <: :> ~`, `route`, iterations (`par seq sum prod`),
  `_ ! ` and all primitives.
- Full signal layer: delays, `rdtable`/`rwtable`, `select2`/`select3`, `waveform`, `soundfile`,
  `attach`, `enable`/`control`, foreign functions/constants/variables, all UI widgets.
- Full type system: nature x variability x computability x vectorability x boolean, plus interval
  and resolution inference.
- Codegen: scalar, vector and scheduler modes, `-single`/`-double`, delay-line and table memory
  allocation, JSON/UI metadata.

**Correct** — defined by oracles, not by assertion. See §9. The bar is: every `.dsp` in
`lib/faust/tests`, `lib/faust/examples` and `faustlibraries` compiles, and its impulse response
matches reference Faust within the tolerance reference Faust already applies between its own
backends (`2e-06`, `tests/impulse-tests/tools/filesCompare.cpp:174`).

Explicitly **out of scope** for this design: the block-diagram SVG drawer (`compiler/draw`), the
mathdoc generator (`compiler/documentator`), and the long tail of text backends (Rust, Julia, D,
C#, Cmajor, JSFX, VHDL, ...). Those are additive and independent of the architecture below.

---

## 2. What we are replacing

Grounding numbers, because they set the scale of the work. Core compiler, excluding backends,
drawing, and doc generation:

| Reference component | Path | Lines | Role |
|---|---|---:|---|
| Parser (bison/flex) | `compiler/parser` | 9.1k (1.0k grammar) | source -> box tree |
| Box algebra + arity | `compiler/boxes` | 3.1k | block-diagram terms, `boxtype` arity inference |
| Evaluator | `compiler/evaluate` | 2.4k | beta-reduction, environments, `with`/`letrec` |
| Pattern matcher | `compiler/patternmatcher` | 0.9k | `case` rule automaton |
| Propagation | `compiler/propagate` | 1.2k | box -> signal |
| Signals + typing | `compiler/signals` | 7.2k | signal terms, type rules, sharing, recursiveness |
| Interval arithmetic | `compiler/interval` | 5.9k | per-primitive interval and resolution rules |
| Normalization | `compiler/normalize` | 2.3k | `aterm`/`mterm` arithmetic normal form, simplify |
| Signal transforms | `compiler/transform` | 8.0k | promotion, constant prop, retiming, visitors |
| Instruction gen + FIR | `compiler/generator/*.cpp` | ~15k | signal -> FIR, memory allocation, loops |
| Loop building | `compiler/parallelize` | 1.1k | loop graph, fusion, task scheduling |

The table above is a subset. Counting all hand-written core code — excluding the generated
lexer/parser (6.9k), `compiler/draw`, `compiler/documentator`, and the 20 backend subdirectories —
the reference core is **~81k lines**: 42.5k across the analysis/IR directories, 2.9k hand-written
parser, 27.9k in `compiler/generator` top-level, and 7.6k in `libcode.cpp`/`global.cpp`/`main.cpp`.

These are measurements of the reference, recorded to show where its complexity actually lives —
the interval arithmetic, the signal transforms, and the instruction generator dominate, and the
plan below is ordered accordingly.

They are not a target to beat. This design should not be sold as a code-size reduction:
`thoughts.md` expects MLIR to "dramatically reduce the amount of code," and on the core it will
not. MLIR does eliminate pure infrastructure — `tlib`'s hash-consed Tree library, `DirectedGraph`,
the FIR IR machinery, the hand-written pretty-printers for each IR, the global-state singleton —
but it charges most of that back in TableGen definitions, conversion-pattern boilerplate, and
region ceremony.

The real reductions are elsewhere, and they are the actual argument for this architecture:

- **What we never write.** 18 of 20 backends, `compiler/draw`, `compiler/documentator` — because
  lowering to standard dialects makes a new target additive rather than another full text emitter.
- **What upstream does for us.** Canonicalization, CSE, LICM, loop unrolling, vectorization,
  register allocation, and the entire LLVM optimization pipeline — all of which the reference
  either reimplements weakly or forgoes.
- **What becomes checkable.** Arity errors move into the verifier; causality becomes structural;
  every pass gets a diffable checked-in input/output pair instead of a debugger session.

The pitch is structure, testability and extensibility, not size.

The reference pipeline, as it actually runs (`compiler/libcode.cpp`, `compiler/generator/compile_scal.cpp:99`):

```
source -> [bison] -> box tree -> [eval] -> evaluated box -> [propagate] -> signals
       -> simplifyToNormalForm -> newConstantPropagation -> conditionAnnotation
       -> recursivnessAnnotation -> typeAnnotation -> sharingAnalysis -> occurrences
       -> [instructions_compiler] -> FIR -> [backend] -> C++/LLVM/wasm/...
```

---

## 3. Architecture: the dialect stack

Three dialects, each a strict lowering of the one above, sitting on top of a bespoke front-end
representation.

```
  .dsp source
      |  tree-sitter CST  (text-preserving, incremental)  <- source fidelity lives here
      v
  Term     hand-rolled AST: the definition language. NOT MLIR. See §3.1.
      |  box evaluation  (beta-reduction, pattern matching, environments)
      v
  fbox     Block-diagram algebra. Arity carried in the type -> verifier-checked.
      |  propagation
      v
  fsig     Signal graph. Per-sample dataflow, recursion as regions.
      |  normalization, typing, interval inference, scheduling, memory allocation
      v
  fdsp     DSP module structure (state fields, UI tree, lifecycle functions)
   + scf / arith / math / memref / vector   <- bodies are *standard* MLIR
      |
      v
  llvm dialect -> LLVM IR -> native / JIT / wasm
  emitc        -> C / C++
  (later) linalg / stablehlo -> autograd, ONNX
```

### 3.1 The front end is not a dialect

The definition language — definitions, abstraction/application, pattern-matching `case`,
`with`/`letrec`/`environment`, `import`/`component`/`library` — is represented by an ordinary
hand-rolled C++ AST (`Term`), evaluated by an ordinary memoized recursive evaluator that emits
`fbox` through an `OpBuilder`. **MLIR starts at `fbox`.**

This is deliberate, and it is the one place where the obvious "put everything in MLIR" instinct is
wrong:

- **It is what every comparable compiler does, including MLIR's own front ends.** Flang parses to a
  bespoke `parser::Program`, runs semantic analysis on that, and only then lowers to FIR, its MLIR
  dialect. ClangIR is built from the Clang AST. rustc runs AST -> HIR -> THIR before reaching MIR.
  Front-end concerns — names, scopes, sugar, source fidelity, error recovery — are language-specific
  and gain nothing from a generic IR.
- **Faust has first-class environments, so MLIR's scoping infrastructure is unusable.**
  `boxEnvironment()` and `boxAccess(exp, id)` make an environment a *value produced by evaluation*,
  not a static scope. `SymbolTable` — the main thing a definition-language dialect would draw on —
  models statically resolvable symbols, so it would have to go unused. Pattern matching gets
  nothing from MLIR either. There is no leverage here, only ceremony.
- **The CST is load-bearing for round-tripping, which makes a term dialect a redundant third
  representation.** MLIR has no notion of comments or formatting, so source fidelity has to live in
  the tree-sitter CST regardless (§8). A term dialect would sit between the representation that can
  hold trivia and the representation that matters, needing to be kept in sync with both and
  invalidated on every keystroke.

The one genuine argument for a term dialect was testability, since the evaluator is the
highest-risk and least-specified part of the project (§12). That argument survives the removal and
in fact improves: evaluator tests are `faust-opt --evaluate %s.dsp | FileCheck %s` over checked-in
**Faust source**, which is more readable than checked-in term IR and exercises the actual
user-facing surface.

What is given up is intermediate dumps between parsing and evaluation. The mitigation is free: the
printer needed there emits Faust source, which the round-tripping goal requires building anyway —
it is the `faust -e` equivalent.

**Revisit this if** the design grows genuine term-to-term rewrite passes: staged desugaring, or a
partial-evaluation/specialization pass feeding the incremental story. A pass *pipeline* over the
term layer would make the pass manager and verifier worth their cost. As specified, evaluation is a
single memoized recursive traversal, so there is no pipeline to manage.

### 3.2 `fbox` — the block-diagram dialect

The output of evaluation. All abstractions are gone, all iterations unrolled, all names resolved.

The key design decision: **arity lives in the type**.

```mlir
!fbox.diagram<2, 1>   // 2 inputs, 1 output
```

`fbox.seq`, `fbox.par`, `fbox.split`, `fbox.merge`, `fbox.rec` get verifiers derived directly from
Faust's composition rules:

| op | constraint | result |
|---|---|---|
| `seq(A: <n,k>, B: <k',m>)` | `k == k'` | `<n,m>` |
| `par(A: <n,m>, B: <p,q>)` | — | `<n+p, m+q>` |
| `split(A: <n,m>, B: <p,q>)` | `p % m == 0` | `<n,q>` |
| `merge(A: <n,m>, B: <p,q>)` | `m % p == 0` | `<n,q>` |
| `rec(A: <n,m>, B: <p,q>)` | `q <= n`, `p <= m` | `<n-q, m>` |

This turns `compiler/boxes/boxtype.cpp` — a separate inference pass in the reference — into
*verification*, running automatically after every pass, with a source location attached for free.
Faust's most common user-facing error class ("cannot connect 3 outputs to 2 inputs") becomes an
MLIR diagnostic pointing at the exact source range. This is the single clearest win of the MLIR
framing at the box level and justifies `fbox` existing as a dialect rather than a plain graph.

Remaining ops mirror the reference constructors in `compiler/boxes/boxes.hh`: `fbox.route`,
`fbox.wire`/`fbox.cut`, `fbox.prim0..5`, `fbox.int`/`fbox.real`, `fbox.waveform`, `fbox.delay1`,
the UI family, `fbox.ffun`/`fconst`/`fvar`, `fbox.soundfile`, `fbox.attach`, `fbox.enable`/
`fbox.control`, `fbox.metadata`.

### 3.3 `fsig` — the signal dialect

Per-sample dataflow. One value type, `!fsig.sig`, deliberately opaque (see §4 for why nature is
*not* in the type).

Ops mirror `compiler/signals/signals.hh`: `fsig.input`, `fsig.int`/`fsig.real`, `fsig.delay`,
`fsig.delay1`, `fsig.prefix`, `fsig.binop<op>`, the math family (`fsig.sin`, `fsig.pow`, ...),
`fsig.select2`/`select3`, `fsig.rdtable`/`fsig.wrtable`/`fsig.gen`, `fsig.intcast`/`floatcast`/
`bitcast`, `fsig.fconst`/`fvar`/`ffun`, `fsig.waveform`, `fsig.soundfile*`, the UI family,
`fsig.attach`, `fsig.enable`, `fsig.control`, `fsig.assertbounds`, `fsig.lowest`/`highest`,
`fsig.register`.

**Recursion is a region, not a cycle.** Faust's `sigRec`/`sigProj` pair is inherently cyclic and
SSA is not. Encode a recursive group as a single op whose block arguments *are* the projections,
implicitly delayed one sample:

```mlir
%y:2 = fsig.rec -> (!fsig.sig, !fsig.sig) {
^bb0(%r0: !fsig.sig, %r1: !fsig.sig):    // %ri == projection i, delayed by 1
  %a = fsig.binop<mul> %r0, %c
  %b = ...
  fsig.yield %a, %b : !fsig.sig, !fsig.sig
}
// %y#0, %y#1 == the same signals, undelayed, visible to the rest of the graph
```

This is legal SSA, verifiable (the one-sample delay on the back edge *is* Faust's causality
guarantee, so causality becomes structural rather than a separate check), naturally nestable, and
gives the recursiveness analysis a ready-made scope. Uses of a projection outside the group are
just uses of the op's results.

The one thing to watch: `fbox.rec` is the only source of cycles in Faust, and we build `fsig.rec`
regions directly during propagation, so an SCC never needs to be discovered after the fact.
Any future pass that could create a cross-region cycle would have to merge the regions; the
verifier should reject cross-region back edges so this fails loudly rather than silently.

Table initialization gets its own scope for the same reason: `fsig.gen` is a region whose body is
evaluated at init time, which is exactly the distinction the reference draws with `sigGen`.

### 3.4 `fdsp` + standard dialects — and why there is no FIR dialect

`thoughts.md` proposes a Faust FIR dialect. **Recommendation: do not build one.**

Faust's FIR (`compiler/generator/instructions.hh`, 3.9k lines) is a general-purpose imperative IR —
declarations, loops, arithmetic, casts, arrays, functions — reinvented inside Faust because in 2010
there was nothing to reuse. MLIR *is* that thing, and it is better: `scf` for loops, `arith`/`math`
for computation, `memref` for state, `func` for the lifecycle entry points, `vector` for SIMD. If
we lower into standard dialects we inherit canonicalization, CSE, LICM, loop unrolling,
vectorization, `--convert-to-llvm`, and `emitc`, none of which we then have to write. Rebuilding
FIR would mean reimplementing all of that badly and would forfeit the main reason to be on MLIR at
all.

What FIR genuinely carries that standard dialects do not is *DSP module structure*. That is what
`fdsp` is for, and it is small:

```mlir
fdsp.dsp @mydsp {
  fdsp.field @fRec0 : memref<2xf64>            // delay line
  fdsp.field @fHslider0 : memref<f64>          // UI-bound control
  fdsp.table  @ftbl0 : memref<65536xf64>       // read-only table
  fdsp.ui {
    fdsp.group "vgroup" "osc" {
      fdsp.hslider @fHslider0 "freq" init 440.0 min 20.0 max 20000.0 step 0.1
        {unit = "Hz", scale = "log"}
    }
  }
  func.func @instanceConstants(%sr: i32) { ... }
  func.func @instanceClear() { ... }
  func.func @compute(%count: i32, %in: memref<?xmemref<?xf64>>, %out: ...) {
    scf.for %i = %c0 to %count step %c1 { ... arith/math/memref ... }
  }
}
```

`fdsp.ui` is a faithful model of the UI tree, so `buildUserInterface` emission and the JSON
description are two straightforward walks over the same op — replacing `uitree.cpp` and
`json_instructions.hh` with one source of truth.

---

## 4. The type system

Faust's type is seven-dimensional (`compiler/signals/sigtype.hh:36-72`):

```
nature        : int | real | any
variability   : konst | block | samp
computability : comp | init | exec
vectorability : vect | scal | truescal
boolean       : num | bool
interval      : [lo, hi]
resolution    : fixed-point precision
```

Only *nature* looks like an MLIR type. Encoding the other six in the type would mean either six
type parameters churned on every rewrite, or six parallel type systems.

**Decision: `!fsig.sig` is opaque; the full type is a dataflow analysis.**

Implement `FaustTypeAnalysis` on MLIR's `DataFlowFramework` as a sparse forward analysis over the
signal graph, with a lattice that is the product of the seven components. `fsig.rec` regions are
exactly where the analysis iterates to a fixed point, starting from bottom on the block arguments —
matching what `sigtyperules.cpp` does with its recursive-group memoization, but expressed in the
framework rather than by hand. MLIR's own `IntegerRangeAnalysis` is the structural model.

Two supporting pieces:

- **Materialization pass.** `--fsig-annotate-types` writes results back as a `#fsig.type<...>`
  attribute per op. This exists for debugging and, critically, so the analysis can be diffed
  against reference Faust's `.type` golden files (§9).
- **Promotion pass.** `--fsig-promote` mirrors `sigPromotion.cpp`: once nature is known, insert
  explicit `fsig.intcast`/`fsig.floatcast` so that no downstream pass has to re-derive implicit
  coercions. After this pass the graph is explicitly typed and nature *could* be reflected into the
  type if it proves useful.

**Interval and resolution inference is a sub-project, not a detail.** `compiler/interval` is 5.9k
lines: a correctly-rounded interval rule for each of ~35 primitives, including the awkward cases
(`fmod`, `remainder`, division by an interval spanning zero, `pow` with negative bases, trig range
reduction). It drives real decisions — table sizing, `select` branch elimination, fixed-point
resolution — so it cannot be stubbed as "top" without changing generated code. Budget it as its own
phase with its own test corpus (§11 phase 4), and consider property-testing each rule against
randomized sampling of the primitive.

---

## 5. Pass pipeline

Stages 1-6 are ordinary front-end code (§3.1), not MLIR passes. From stage 7 on, every entry is a
named MLIR pass, separately testable with lit/FileCheck.

**Front end** (hand-rolled, driven by the query engine of §8.3)
1. `tree-sitter` parse -> CST (kept, not discarded — see §8)
2. CST -> `Term` AST, attaching source ranges
3. `import`/`component`/`library` resolution, file search path
4. name resolution — identifier -> definition or parameter binding
5. well-formedness — unbound names, duplicate definitions, malformed patterns
6. **evaluation** — beta-reduction, pattern matching, `with`/`letrec`/`environment` scoping,
   iteration unrolling, metadata propagation. Memoized (§8.3). Emits `fbox` via `OpBuilder`.

**Entering MLIR**
7. verifier — arity checking, automatic (§3.2). This is the first point at which the framework
   carries any weight, and it catches the largest user-facing error class immediately.

**Propagation: `fbox -> fsig`**
8. `--fbox-to-fsig` — symbolic propagation of input signals through the diagram; `fbox.rec`
   becomes an `fsig.rec` region

**Signal-level**
9.  `--fsig-normalize` — arithmetic normal form (`aterm`/`mterm` equivalent) + simplification
10. `--fsig-constant-fold` — constant propagation
11. `--fsig-promote` — explicit casts
12. `--fsig-annotate-conditions` — `enable`/`control` condition propagation
13. `--fsig-annotate-types` — the §4 analysis, materialized
14. `--fsig-check-causality` — delays non-negative, `rdtable`/`rwtable` index bounds vs. interval
15. `--fsig-sharing` — occurrence counts driving materialization decisions
16. `--fsig-schedule` — assign each subexpression a level (init / block-rate / sample-rate) from
    its variability, and compute max-delay per signal

**Lowering: `fsig -> fdsp + scf/arith/math/memref`**
17. `--fsig-to-fdsp` — allocate state (delay lines, tables, UI-bound controls), emit lifecycle
    functions, build the sample loop, place each expression at its scheduled level
18. `--fdsp-optimize-delays` — choose shift-register vs. ring-buffer-with-mask vs.
    ring-buffer-with-wrap per delay line, based on max delay and access pattern
19. upstream: `--canonicalize --cse --licm`
20. `--fdsp-vectorize` (optional) — loop splitting into vector-size chunks with intermediate
    buffers, then upstream `affine`/`vector` passes
21. `--fdsp-parallelize` (optional) — task-graph mode

**Backends**
22. `--fdsp-to-llvm` -> LLVM IR -> native / JIT / wasm32
23. `--fdsp-to-emitc` + C++ class emitter -> `.cpp`/`.h`
24. `--fdsp-emit-json` -> the JSON description architecture files consume

Note on 9-11 ordering: it matches the reference (`compile_scal.cpp:102-117`) and the ordering is
load-bearing, see §9's tolerance discussion.

---

## 6. Representation decisions worth calling out

**Delays.** `fsig.delay(%s, %d)` keeps the delay symbolic through the signal layer. Max delay per
signal is computed in pass 16 from the interval of `%d` — which is precisely why interval analysis
must be real and not stubbed: a delay whose index interval is `[0, 4]` becomes five registers, one
whose interval is `top` becomes a heap-allocated ring buffer and a much worse inner loop.

**Tables.** `fsig.wrtable(%size, %gen, %widx, %wsig) -> !fsig.table` and
`fsig.rdtable(%tbl, %ridx)`. Read-only tables are the case where `%widx`/`%wsig` are absent and
`%gen`'s region is init-time-computable; those lower to a `fdsp.table` filled in
`instanceConstants`. Read-write tables lower to a `fdsp.field` written in the sample loop.

**`enable` / `control`.** Reference Faust propagates a boolean condition per signal and wraps the
generated code in `if`. In MLIR this is more natural: the condition annotation pass computes the
condition lattice, and lowering emits `scf.if` around the guarded statements. The condition is a
signal like any other, so it participates in normalization and CSE.

**Foreign functions.** `fsig.ffun` carries a signature and a header name; it lowers to
`func.func` declarations plus `func.call`, and the C++ backend emits the `#include`. Nothing
special is needed — this is a place where MLIR's existing machinery is simply better than FIR's.

**Waveforms and soundfiles.** `fsig.waveform` carries a `DenseElementsAttr`, which is exactly the
right MLIR construct and gives us constant folding of `waveform` reads at fixed indices for free.
`fsig.soundfile` lowers to the reference's soundfile struct layout, which must match byte-for-byte
because architecture files depend on it.

---

## 7. Front end

**Use tree-sitter**, as `thoughts.md` proposes. Justification, since it is not free:

- Faust's grammar has genuine ambiguity around infix operators and pattern-matching rules; the
  reference resolves it with bison precedence declarations and 175 token/type declarations.
  tree-sitter's GLR handles it without hand-tuned precedence tables.
- Error recovery is a requirement, not a nicety, if the compiler is to back an editor.
- Incremental reparse of a changed range is the foundation of §8.3.
- The CST retains every byte, including comments and whitespace, which is what makes
  source-preserving edits possible.

There is no usable tree-sitter Faust grammar to adopt — `lib/faust/syntax-highlighting/` contains
only regex-based editor modes. Writing `tree-sitter-faust` is a real deliverable: budget it as its
own repository/subdirectory with its own corpus tests, validated by parsing all 341 test `.dsp`
files plus all 296 examples plus faustlibraries without error.

The CST is **kept alive** after parsing rather than discarded. `Term` nodes reference CST node ids;
the CST references source byte ranges. That two-level indirection is what survives edits, and it is
also why the term layer stays out of MLIR (§3.1) — MLIR cannot carry trivia, so the CST would have
to exist alongside it regardless.

---

## 8. Round-tripping and incrementality

This is `thoughts.md`'s stated motivation and it deserves a precise statement of what is and is not
achievable, because the goal as written is partly unreachable.

### 8.1 What is invertible

- **`Term` <-> source: yes, exactly.** The CST preserves all text; a term edit is applied as a
  text splice over the corresponding byte range, leaving every other byte untouched (the
  rust-analyzer model). No reformatting, no lost comments.
- **`fbox` -> `Term`: partially.** Evaluation is lossy. `par(i, 10, osc(i))` unrolls to ten
  independent subgraphs, and there is no general way to re-roll ten edited subgraphs into a `par`.
  Reference Faust already exposes the lossy direction: `faust -e` prints the evaluated diagram as
  Faust source (this is how `tests/impulse-tests/reference/*.box` is produced) and the output has
  lost every name and every iteration.
- **`fsig` -> `fbox`: no.** Normalization dissolves the diagram structure entirely. Reordering a
  sum, folding constants, and merging shared subexpressions destroy any correspondence to boxes.
- **`fdsp` -> anything: no.**

### 8.2 The design that follows

Support **`Term` <-> `fbox` round-tripping only**, and make the boundary explicit rather than
discovering it later:

- Chain `Location` through every lowering. `fsig` op -> `fbox` op -> term id -> CST node -> byte
  range. The term-id hop is an `OpaqueLoc` (or a small custom location attribute) since the term
  layer is not MLIR; everything above it is ordinary MLIR location machinery, and `FusedLoc`
  naturally handles the many-to-one collapse caused by CSE and normalization — so a normalized
  signal still points at *all* the source it came from, enough for editor highlighting even though
  it is not enough for editing.
- Each `fbox` op additionally carries a `provenance` attribute: the term it was produced from, plus
  a *path* through the evaluation (e.g. "iteration index 3 of the `par` at line 12").
- An edit to an `fbox` node is legal iff its provenance path is invertible — the node came from a
  term with no intervening unrolling or pattern-match specialization. Otherwise the editor rejects
  the edit and offers to "expand" the definition (materialize the unrolled source, then edit).
  This is the same trade-off spreadsheet formula editors and IDE refactorings make, and stating it
  up front is better than shipping an editor that silently corrupts source.

### 8.3 Incremental compilation

Structure the compiler as a **demand-driven query engine** (the salsa/rust-analyzer model), not a
batch pipeline:

- Queries: `parse(file)`, `defs(file)`, `evaluate(def, env)`, `propagate(box)`, `types(sig)`, ...
- Each query result is memoized under a hash of its inputs. `evaluate(def, env)` keyed on
  (definition content hash, environment hash) is the important one, since it is the expensive step
  and Faust code is heavily reused across a program.
- A text edit invalidates the tree-sitter subtree, which invalidates only the definitions whose
  ranges overlap, which invalidates only their transitive dependents.

This is what makes `thoughts.md`'s "real-time updates to subsections of very large Faust programs"
actually work, and it is a structural decision — it cannot be retrofitted onto a batch compiler,
so the query layer belongs in phase 1 even though it pays off in phase 8.

---

## 9. Correctness strategy

The reference repository ships an unusually good oracle and the plan should exploit it fully.
`lib/faust/tests/impulse-tests/reference/` holds, for each of 99 test programs:

| file | content | what it validates |
|---|---|---|
| `.box` | evaluated diagram, printed as Faust source | our evaluator (`Term -> fbox`) |
| `.sig` | normalized signal graph in SSA form (`ID_0 = max(1.0f, ID_1);`) | propagation + normalization |
| `.type` | per-signal type + interval (`RBEVN interval(-1,1,-259)`) | the §4 analysis |
| `.ir` | 60000-frame impulse response | end-to-end numerics |
| `.fir`, `.cpp1` | reference FIR and C++ | informational only, we do not target these |

### Comparison method

Do **not** try to byte-match reference Faust's printers — that couples us to their formatting
forever. Instead:

- **`.box`**: parse the reference `.box` with *our own* parser, evaluate it to `fbox`, and check
  graph isomorphism against the `fbox` we produced from the original `.dsp`. This validates
  evaluation without validating pretty-printing.
- **`.sig`**: same trick — parse the reference `.sig` into `fsig` and check isomorphism up to
  ID renaming.
- **`.type`**: compare our analysis results as a multiset of type tuples, since their emission
  order is a traversal detail.
- **`.ir`**: reuse `tests/impulse-tests/tools/filesCompare.cpp` unmodified.

### Tolerance policy

Reference Faust does **not** require bit-exactness between its own backends — `filesCompare`
defaults to `tolerance = 2e-06`
(`tests/impulse-tests/tools/filesCompare.cpp:174`). So bit-exactness is not the standard and should
not be the goal.

But there is a trap here, and it is the sharpest correctness risk in the project. Floating-point
addition and multiplication are not associative, so **the arithmetic normal form is
semantically observable**. Reference Faust's `aterm`/`mterm` normalization
(`compiler/normalize/`) imposes a specific ordering on sums and products. Implement a *different*
ordering and results drift — usually within `2e-06`, but recursive filters accumulate, and a
resonant filter or a long feedback network will diverge past tolerance. The failures will look
random and appear late.

**Therefore: reproduce `aterm`/`mterm` ordering faithfully, and treat it as a phase-3 deliverable
with its own targeted tests** (`norm1.dsp`, `norm2.dsp`, `norm3.dsp`, `math_simp.dsp` in the
reference set exist precisely for this). Do not treat normalization as "some canonicalization we
can design ourselves."

### Corpus, in order of increasing breadth

1. 99 reference-file test programs — all four levels above.
2. 341 `.dsp` files across `tests/` — compile + impulse.
3. 296 files in `examples/` — compile + impulse.
4. `faustlibraries` — note that `lib/faust/libraries/` is an **uninitialized nested submodule**
   and `git submodule update --init --recursive` is a prerequisite. The standard library is the
   densest real-world exercise of the language.
5. `tests/error-tests` and `tests/warning-tests` — diagnostic parity. Do not target identical
   message strings; MLIR diagnostics will be better than the reference's. Define an error *class*
   taxonomy and assert that the same programs are rejected with the same class. That is the
   property that matters.
6. **Grammar-directed differential fuzzing.** Generate random arity-correct terms, compile
   with both compilers, compare impulse responses. This is what finds the evaluator and
   normalization bugs that a curated corpus misses, and it is cheap once the oracle harness exists.

### Oracle hygiene

The `faust` on this machine's `PATH` is 2.66.9 while the submodule is 2.85.9 — different enough to
produce spurious diffs. CI must build the oracle from `lib/faust` and never use a system binary.
Also note the reference files were generated with 2.81.0 (`declare compile_options` in each `.box`),
so regenerate them from the pinned submodule rather than trusting them as-is.

---

## 10. Repository layout and build

Out-of-tree MLIR project, standard structure:

```
faust_mlir/
  CMakeLists.txt                  # find_package(MLIR REQUIRED CONFIG)
  include/faust/Syntax/                              # tree-sitter binding, CST, Term AST, evaluator
  include/faust/Dialect/{FBox,FSig,FDSP}/*.td        # TableGen op definitions
  include/faust/Analysis/                            # FaustTypeAnalysis, interval
  include/faust/Conversion/                          # the §5 passes
  lib/                                               # implementations, mirroring include/
  tools/
    faustc/         # the user-facing driver, argv-compatible with `faust` where it makes sense
    faust-opt/      # mlir-opt with our dialects registered  -> lit tests
    faust-translate/# IR -> C++/LLVM/wasm/JSON
    faust-lsp/      # phase 8
  grammar/tree-sitter-faust/
  test/
    Dialect/        # lit + FileCheck, per dialect, round-trip and verifier tests
    Conversion/     # lit + FileCheck, per pass
    Conformance/    # driven by lib/faust/tests corpora, four-level oracle
    Fuzz/
  lib/faust/        # submodule, oracle only, never linked
```

Pin LLVM. MLIR's C++ API breaks every release; `main` breaks weekly. Track a release branch
(22.x is what is installed locally) and pin the exact commit in CI, building llvm-project from
source there for reproducibility while allowing `brew llvm@22` for local development.

`faust-opt` plus lit/FileCheck is the primary development loop and should exist on day one — it is
the reason MLIR pays for itself on a project this size, since every pass gets a readable, diffable,
checked-in input/output pair instead of a debugger session.

---

## 11. Phased plan

Each phase has an exit criterion that is a passing test suite, not a subjective judgment.

**Phase 0 — Foundations.**
CMake skeleton against pinned MLIR. `faust-opt` with four empty dialects registered. lit harness.
Oracle harness: build reference Faust from the submodule, initialize `faustlibraries`, wrap
`filesCompare`, and stand up the four-level comparison scripts against the existing reference files.
*Exit:* `ninja check-faust` runs, oracle reproduces reference-vs-reference comparisons green.

**Phase 1 — Vertical slice.**
Deliberately narrow language subset: numeric literals, `+ - * /`, `: , ~`, `@`, `_`, `!`, and
`hslider`. Full stack, tree-sitter through LLVM JIT. Query engine skeleton in place.
*Exit:* `process = +;`, `process = _ ~ (0.5 * _);`, and `process = os.osc`-shaped hand-written
equivalents match reference impulse responses.

**Phase 2 — Complete the box layer.**
Full grammar. Full evaluator: abstraction/application, pattern matching, `with`/`letrec`/
`environment`, `component`/`library`/`import`, iterations, `route`, `waveform`, metadata, foreign
declarations. Full `fbox` op set and verifiers.
*Exit:* `.box` isomorphism on all 99 reference tests; all 296 examples and all of faustlibraries
evaluate without error.

**Phase 3 — Complete the signal layer.**
Full propagation. Normalization with faithful `aterm`/`mterm` ordering (§9). Constant propagation,
promotion, condition annotation. Tables, soundfile, `enable`/`control`, `select`, `prefix`.
*Exit:* `.sig` isomorphism on all 99 reference tests. Targeted `norm*.dsp`/`math_simp.dsp` pass.

**Phase 4 — Type and interval system.**
`FaustTypeAnalysis` on the dataflow framework. Interval arithmetic for all primitives, with
property tests per rule. Resolution/fixed-point inference. Causality and bounds checks.
*Exit:* `.type` parity on all 99 reference tests; error-test class parity.

**Phase 5 — Scalar codegen.**
Sharing/occurrence analysis, level scheduling, delay-line and table allocation, loop construction,
`fsig -> fdsp`, `fdsp -> llvm`.
*Exit:* `.ir` impulse parity on all 99 reference tests, all 341 test `.dsp`, all 296 examples, and
faustlibraries — in both `-single` and `-double`.

**Phase 6 — C/C++ backend and metadata.**
`fdsp -> emitc` plus a C++ class emitter producing an architecture-file-compatible class. JSON
description generation. `buildUserInterface`.
*Exit:* `tests/ui-export` parity; generated C++ compiles against `lib/faust/architecture` and
passes impulse tests; JSON diffs clean.

**Phase 7 — Performance modes.**
Vectorization (loop splitting + `vector` dialect), scheduler/parallel mode, wasm target.
*Exit:* `tests/benchmark` shows parity-or-better throughput vs. reference in scalar and vector
modes. This is where the "MLIR gives better optimization" claim gets tested rather than asserted.

**Phase 8 — Round-tripping and tooling.**
Provenance attributes, `fbox -> Term` edit application, source-preserving printer, incremental
recompile through the query engine, `faust-lsp`.
*Exit:* an editing harness that applies a box-level edit to each of the 99 reference programs and
reproduces byte-identical source for identity edits, with correct localized diffs for real ones.

**Phase 9 — Stretch: differentiable signals.**
`fsig -> linalg`/StableHLO lowering, enabling autograd at the signal level — `thoughts.md`'s ONNX/
PyTorch goal. This is the payoff that only exists because `fsig` is a real dialect in a real
compiler framework, and it is worth stating as the strategic reason to pay MLIR's costs.

---

## 12. Risks

**The reference implementation is the specification.** Faust's published semantics cover the
algebra well but not the evaluator's corner cases — pattern-match ordering, `with` scoping
interactions, `environment` capture, name shadowing in `letrec`. Expect to resolve these by reading
`compiler/evaluate/eval.cpp` and by differential testing, not from documentation. Phase 2 should
budget for this explicitly.

**Normalization ordering** (§9). Highest-severity, latest-surfacing risk in the project.

**Interval arithmetic scale** (§4). 5.9k lines of the reference; easy to underestimate as "just
min/max propagation".

**MLIR API churn.** Mitigated by pinning, but it is a real recurring tax — budget periodic
upgrade work.

**`emitc` coverage.** Upstream `emitc` conversions for `scf`/`arith`/`memref`/`math` are
incomplete in places. The C++ backend is heavily used by Faust's community, so phase 6 may require
upstream contributions or a hand-written emitter for the gaps. Validate `emitc` coverage against a
representative `fdsp` module early — during phase 5, not phase 6 — so the answer is known before
it is on the critical path.

**Byte-level compatibility surfaces.** The soundfile struct layout, the JSON schema, and the
generated C++ class shape are consumed by `lib/faust/architecture` and by every downstream
`faust2*` tool. These are compatibility contracts, not implementation details.

**Scope discipline.** The reference ships 20 backends. Shipping four (LLVM, C, C++, wasm) that are
correct is worth more than twenty that are approximate, and the dialect stack means adding the
others later is additive.

---

## 13. Summary of the decisions

1. Three dialects — `fbox`, `fsig`, `fdsp` — on top of a hand-rolled front-end AST. MLIR starts
   at `fbox`. The definition language stays out of MLIR because Faust's first-class environments
   make `SymbolTable` unusable, the CST has to hold source fidelity anyway, and every comparable
   compiler (Flang, ClangIR, rustc) keeps a bespoke parse tree ahead of its IR.
2. **No FIR dialect.** Lower to `scf`/`arith`/`math`/`memref`/`vector` and inherit upstream
   optimization and backends. This is the largest single simplification versus the reference and
   the main reason MLIR is the right substrate.
3. Arity in the `fbox` type, so Faust's most common error class becomes verifier-checked with
   precise locations.
4. Recursion as regions with implicitly-delayed block arguments, making causality structural.
5. The seven-dimensional Faust type is a dataflow analysis, not an MLIR type; nature-only promotion
   is materialized explicitly.
6. tree-sitter front end with a retained CST, and a demand-driven query engine from phase 1, since
   incrementality cannot be retrofitted.
7. Round-tripping is `Term <-> fbox` only, with provenance paths determining which box edits are
   legal. `fsig -> source` is not achievable and the design says so.
8. Correctness is defined by the reference repository's own four-level golden files plus impulse
   comparison at `2e-06`, extended with differential fuzzing — and normalization ordering is
   treated as observable semantics.
