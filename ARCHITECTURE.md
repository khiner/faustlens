# Architecture

FaustLens is a from-scratch Faust compiler built around bidirectional editing: a box graph and a text editor showing the same program, either one editable, updating each other as you type, with audio running throughout.

`lib/faust` (@ `515dc515c`, 2.85.9-25) is the **test oracle and definition of correct behaviour**.
No libfaust code is linked or ported.
Internal precision is a build option defaulting to **f64**, matching the `-double` the reference impulse responses were generated with.

Three properties define the product and decide every trade-off below:

1. **Bidirectional.**
   Selecting a box highlights its source range.
   Editing a box rewrites exactly the part of the source that changed, leaving every other byte — including comments and formatting *inside* the edited region — untouched.
2. **Live.**
   Every keystroke produces a new running DSP, fast enough to feel immediate, without dropping the reverb tail or clicking.
3. **Simple.**
   Small enough that one person holds the whole thing in their head.

Reference Faust is none of these, for good reasons: it is a batch compiler targeting twenty backends with fixed-point support.
Reusing its *semantics* is mandatory; reusing its *structure* is not.

---

## The representation stack

```
   Text                    source of truth, host-owned. Beside it, a token
    | ^                    vector that tiles every byte.
    | |  get:  own lexer + recursive descent, error-recovering
    | |  put:  a retentive edit script over byte spans
    v |
   Term                    definition language AST, in two layers: interned
    |                      Merkle-hashed *values*, and per-file *refs*
    |                      carrying byte spans.
    |                      *** the editable semantic surface ***
    |  evaluate  (memoized on value id x environment id)
    v
   Box                     evaluated diagram. Arity-checked. Read-only view.
    |  propagate
    v
   Signal                  flat hash-consed DAG. The optimization IR.
    |  analyze, schedule, allocate state
    v
   Plan                    linear three-address instructions + state layout,
    |                      plus descriptors: UI tree, soundfiles, foreign
    |                      symbols, metadata = the compiled artifact
    |
    +--> Bytecode          edit loop. Sub-millisecond.
    +--> LLVM IR           release. Ahead-of-time or ORC JIT.
              |
              v
   Instance                state block + lifecycle. Hot-swapped, state-preserving.
```

Text and Term are two views of one source; Signal and Plan are the compiler proper; Box is the evaluated view joining them.
Three structural decisions shape the design.

**Term is the editable surface; Box is a read-only projection.**
This is what makes bidirectional editing tractable — see below.

**Signal is a flat, hash-consed DAG, not a pointer graph.**
`std::vector<Node>` where `Node` is a POD of `{opcode, operand indices, payload}`, uniqued through a hash map at construction.
This replaces the reference's `tlib` hash-consing layer, gives structural sharing and CSE for free, supports incremental recompilation, and traverses faster than heap nodes.
Arity checking at the construction site makes malformed nodes unrepresentable, so there is no verifier.

**Plan is a linear instruction list serving as both bytecode and LLVM emission source.**
The reference maintains FIR — a full imperative IR with expression trees, statements and types — plus a distinct emitter per backend.
Three-address code over virtual registers in the shared lowering keeps both backends thin: the interpreter is a dispatch loop, the LLVM emitter a one-pass walk mapping each instruction to one or two IRBuilder calls.

---

## Editing

### Term is pre-desugaring; Box is post-desugaring

An evaluated Faust diagram cannot be edited back into source.
Evaluation beta-reduces, unrolls iterations and specializes pattern matches: `par(i, 10, osc(i))` becomes ten independent subgraphs with no general way to re-roll them.
So do not invert evaluation — **render the Term graph as boxes and edit that**.
`par(i, 10, osc(i))` displays as a single `par` node with a multiplicity badge, because that is what the source says.

Faust's *grammar* desugars: it rewrites `a + b` into `boxSeq(boxPar(a, b), boxAdd())` and `x'` into `boxSeq(x, boxDelay1())`, along with `@`, the comparisons and the bitwise operators.
Desugaring loses which spelling the author chose: `(a, b) : +` and `a + b` can denote the same circuit.
Reconstructing that choice would require retained source correspondence or a resugaring policy.
Term therefore keeps the surface forms and desugars on the way to Box, the opposite of the reference.
This is the **resugaring** problem [R1, R2], *avoided* rather than solved: keeping the surface forms leaves nothing to recover.

Surface form means *spelling*, not just shape: `:>` versus `+>`, `x^y` versus `pow(x,y)`, `mem` versus `'`, and the exact text of a numeric literal.
Mirroring the grammar preserves most of this for free — two spellings are usually two productions and so two nodes — and only three cases need an explicit form tag (merge spelling, power spelling, literal lexeme).

So there are two views: the **structural view** (the Term graph, editable) and the **evaluated view** (the Box graph, read-only, reachable by expanding any node).
To edit inside an expanded view the user invokes **materialize** [R3]: the Box subgraph is lifted back into Term, the source is rewritten to match, and the result is editable structurally.
Expansion evaluates the selected source occurrence in its lexical scope, including symbolic bindings for enclosing function parameters.
It never chooses an environment from another occurrence with an equal value id.
Closures are normalized to symbolic circuits before lifting.
The lift takes the desugared spelling and is **partial by design**: errors, environments, and free slots with no visible source binder are declined.
A `Symbolic` introduces a fresh lambda binder, while an ambient `Slot` uses its visible source parameter name.
Materializing is lossy and user-initiated.

### Text and Term form a retentive lens

`get` parses text into Term and an occurrence-specific ref tree.
Structural edits carry **links** from paths in the replacement term to the old source refs whose bytes they preserve [R4, §5].
These updated links distinguish survival, movement, and copying even when several occurrences have equal values.
`put` applies the resulting splice to the original source.
Text remains authoritative, so comments, spelling, formatting, and incomplete input have one owner.
Both the text pane and diagram operate on this source-backed document.

The contracts are:

- **Token coverage.** Every byte belongs to exactly one token, including whitespace and comments.
- **Printer round trip.** `parse(print(t)) == t` for well-formed surface terms in the parser's image, with equality including spelling.
  This is a printer contract, distinct from the editor's update law.
- **PutGet.** Parsing the source after an accepted structural splice produces the intended rewritten program, including its surrounding syntax.
- **Hippocraticness.** Writing back the same view with unchanged correspondence leaves the source byte-identical.
  Swapping equal-valued occurrences can change correspondence and therefore change bytes.
- **Retentiveness.** Every explicit link retains that source occurrence's outer-span bytes at its destination occurrence, with additional grouping permitted by the destination grammar.
  Bytes outside the edit's target span remain unchanged.

The tests check token coverage, printer round trips, identity splices, generated edits, and source retention across the corpus.
Full-program reparsing and destination-link checks use a bounded, distributed sample of catalogue edits per file.
Focused regressions cover repeated equal values, moved comments, argument boundaries, and lexical validation.
These are executable checks, not a formal proof for every possible edit.
Materialization has a separate contract: lifting, splicing, parsing, and evaluating preserve the circuit, including its enclosing bindings.
Lifted terms need not retain the original surface spelling.

Quotient lenses [R5] describe laws modulo chosen equivalences.
They do not require discarding comments, and quotienting and retention address different concerns.
In the parse/print analogy, parsing chooses the abstract representative and printing chooses concrete syntax.
The printer alone is not a lens `put`.

### The splice

An edit contains a target ref, a replacement Term value, and links to surviving source occurrences.
Composition, deletion, retext, and rewiring preserve these links while constructing the replacement.
Unchanged subtrees retain their whole source region.
A new stage has no source link, even if its value equals another stage.
The lower-level value-rewrite API also supports equality-based alignment for transformations that supply no correspondence.
That heuristic does not express occurrence-specific editing intent [R11].

Rendering produces a sequence of retained source fragments and printed text.
Before reconciling it with the source, the splicer gathers all retained regions so comment salvage knows which comments will travel with a moved or copied fragment.
A reconcile cursor advances monotonically, producing disjoint source-ordered replacements.
Fragments behind the cursor are copied into their new position.

Four details are essential:

- **Links stay inside the target span.** A replacement cannot claim source bytes outside the region it owns.
- **Retention respects destination grammar.** Precedence and the distinction between expression and argument positions determine whether a fragment needs grouping.
  A retained sequence can contain a comma below its root, so checking only the root operator is insufficient.
- **Comment salvage excludes retained regions.** Comments in replaced gaps are re-emitted once, unless explicit copying intentionally duplicates them.
  Whitespace in those gaps is supplied by the printer.
- **Seams must not fuse tokens.** Joining `3` and `.name` would lex as `3.` followed by `name`.
  Emission inserts spacing where lexing a junction would move a token boundary.

For `a : b` → `a : x : b`, the new tree is `Seq(a, Seq(x, b))`.
Links retain the original `a` and `b`; the printer emits the new connectives and `x`.
Only the gap between the retained stages changes.

The evaluated Box graph is a projection in this product, with explicit materialization before structural editing.
Evaluation has no unique inverse, but a chosen update policy can obey laws on a suitable domain [R3].
The choice to materialize does not imply that a lawful evaluated-view update is mathematically impossible.

### Values and refs

Term lives in two structures:

- **`TermValue`** — interned, hash-consed, Merkle-hashed, provenance-free: `{kind, form, variants, payload, child value-ids}`, 16 bytes, children in a shared pool.
  Deduplicated globally across every open and imported file.
- **`TermRef`** — a per-file positional tree, `{value_id, span, outer_span?}`, isomorphic to the parse, rebuilt whenever its file is reparsed, never interned and never hashed.

**The Merkle hash covers structure alone and carries no span.**
A blank line inserted at the top of a file therefore leaves every hash below it unchanged, along with every Signal node id and state field key derived from them.
That is how state identity — and the reverb tail — survives an unrelated edit.

**Identity is the interned id**, exact by construction; the hash only serves the intern table.
Both directions that need provenance resolve by walking the ref tree — a diagnostic names a value id and is marked at every occurrence; a state field traces field → signal → value → ref → byte range.
All provenance lives in side tables keyed by interned id, so the originating term never enters a Signal node's hash and CSE stays free.

There is **no CST**.
The token vector and the ref tree carry everything a CST would: spans at every syntactic node, trivia in the gaps between sibling spans, and token kinds for highlighting.

### Precedence and parentheses

Grouping parens are dropped at parse time, as in the reference, so the printer re-derives them.
The grammar is layered — four nonterminals, each admitting strictly less than the last — and the layers carry more of the disambiguation than the precedence table; in the parser they collapse to a single minimum-binding-power argument.

**Precedence is one runtime artifact, keyed on the operator token, read by both the parser and the printer.**
The parser reads it forward as `token -> (left_bp, right_bp)`; the printer reads it backward to decide parenthesization.
Split across a grammar's precedence block, a printer and a prose table, any two can agree while the third drifts.
Keying on the token rather than the Term node is what makes it one table: `BinOp` is a single node covering eighteen infix operators.

The rule: printing child `C` at side `S` of parent `P`, parenthesize when `prec(C) < prec(P)`; when `prec(C) == prec(P)` and `S` is the side associativity does not favour; or when `C` is `Par`, `With` or `LetRec` in an argument position.
That rule defines the canonical form.

**Layout is deterministic but minimal.**
Diagram-shaped operators (`:` `<:` `:>` `+>` `~`) take one space each side; expression-shaped operators take none.
**Expressions never contain a newline**, at any length — splice output lands in text the user has already formatted, and a wrapped fragment would clash with it.
Only statement lists break lines.
The printer is not a formatter: it emits bytes only for nodes the alignment left unmatched.

---

## Front end and incrementality

### The parser is ours

A hand-written, mode-stacked lexer emitting a flat `vector<Token>{kind, span}` that tiles the file, plus recursive descent with a Pratt core for expressions.
It builds interned values and refs **in one pass** — recursive descent produces children before parents, which is the order bottom-up Merkle hashing needs.

A generated grammar was the obvious alternative and is rejected for two reasons.
**tree-sitter gives the grammar author no control over error recovery**: recovery is a runtime cost heuristic, with no `error` production, no synchronization tokens and no cost hints [R7, R8].
The property this design must hit — a hole's extent stays inside the `;`-delimited statement containing the failure — would belong to a black box, measurable but not fixable.
And **a generated grammar is a second structural derivation** of `faustparser.y` alongside the Term node inventory, and the two must be kept in agreement.

Recovery is panic-mode and per-frame: each frame carries its own synchronization set, a frame never consumes a token in an enclosing frame's stop set, and a sync token counts only at the frame's entry bracket depth.
On failure the frame records a diagnostic, consumes to its sync token, and emits a **`Hole`** carrying the source bytes plus the children it had already built — so `a : b : ` still renders `a` and `b` in the box view.
A hole prints back verbatim and evaluates to an `Error` box.

Derived printers and parsers were also considered: invertible syntax descriptions generate both from one description [R9], and FliPpr derives a parser from a pretty-printer [R10], which derive consistency properties for their parser/printer relation.
Those properties would not by themselves establish this editor's splice law.
This editor would still need source references, retained-fragment emission, and its chosen recovery behavior.
The hand-written parser and printer share a precedence table and are checked against each other.

**A generated grammar is still useful as a test oracle.**
`khiner/tree-sitter-faust` is independently written, so it serves as a *differential acceptance oracle*: parse both corpora with both parsers and compare accept/reject plus agreement on leaf token boundaries.
It is an acceptance oracle, not a structural one — the two trees differ by design in at least five ways.

**No incremental parsing.**
Parsing from scratch is nowhere near the bottleneck, and reparse cost is `O(edited file)` rather than `O(program)`.
Early cutoff in the query engine then stops propagation wherever the value ids come out unchanged.

### Merkle hashing is the incremental mechanism

Hash the Term value graph bottom-up and intern by hash, so structurally identical terms are the same object and the same id.
Evaluation is memoized on **(value id, environment id)**, and **id equality *is* the diff** — no tree-diffing algorithm decides what changed.

One mechanism serves three consumers: incremental evaluation, state identity on reload, and subtree matching in the splice.
Environments are interned too, since the same definition evaluates differently under different scopes: an extension is a new interned node holding `(name, value, parent)`, hashed once at construction, which keeps a memo probe O(1) even against an environment as large as the standard library's.

The same applies downstream: Signal is hash-consed, so re-propagating a changed definition produces identical node ids for every unchanged subtree, and per-node analyses memoize on those ids.

Incrementality stops at codegen, deliberately.
The compute loop is one block in a topological order over a shared state layout, so one changed node means a new function; avoiding that means compiling each definition into a separately relinkable unit, destroying global CSE, shared delay lines and loop fusion.
The answer is to make codegen **cheap** rather than incremental — which is what the two-tier backend provides.

**Determinism is a hard rule.**
Anything whose iteration order can affect output is sorted or insertion-ordered, never raw hash-map order, because an incremental result must be *identical* to a from-scratch compile.

### Files, and the query engine

A Faust program is not one file: `import("stdfaust.lib")` opens nearly every one, so file reading sits on the critical path of every compile and belongs inside the incremental model.
Files are **input queries**; resolution is itself a query, so a *failed* resolution retries when the missing file appears.

Resolution is an overlay, in this order:

1. **Open editor buffers** — an unsaved buffer shadows everything below, so live editing of a library works.
2. **The importing file's own directory** — deterministic, unlike the reference's globally accumulating import list.
3. **The search path on disk.**
4. **The embedded standard library** — `faustlibraries` compiled into the binary, which removes install steps and version skew, lets the environment run with no filesystem at all, and guarantees the tested version ships.
   To modify a stdlib file the user **ejects** it into the workspace, where it shadows the embedded copy.

**The invalidating engine spans only the path-addressed layers, which is why it is small.**
A query is needed exactly where a fact is named by something other than its own content.
Everything below Term is *content-addressed* — evaluation on `(value id, environment id)`, propagation on box ids, analyses on signal ids — so those memos need no invalidation: an edit produces different keys, making a stale entry unreachable rather than wrong.
That leaves five query kinds: `file_text`, a global `vfs_revision`, `resolve`, `terms` and `file_env`.
`import`, `component` and `library` are resolved by this layer rather than by the evaluator, which is what keeps evaluation a pure function of interned ids.

The mechanism is **revision stamping with early cutoff**: each entry holds `{result, changed_at, verified_at, deps}`; a query re-verifies its deps and returns the cached result without recomputing when none changed, and advances `changed_at` on recompute only if the result differs.
Interning makes that equality check free.
One hand-written rule: `terms(path)` returns a value id, a ref tree and a token vector, and **equality is the value component alone** — ref spans and token offsets shift on every whitespace edit, so comparing the whole result would defeat the cutoff.

Two cycle detectors sit at two layers, deliberately not unified: import cycles are query cycles, detected by an in-flight stack, with nothing on the cycle cached so the result stays independent of entry order; evaluation loops are detected by marking a `(value id, environment id)` key in flight.
One is about paths, the other about values.

One worker owns the single-threaded query engine.
Requests coalesce before compilation; superseded results are discarded after preparation and before publication.
An in-progress compile runs to its next worker check rather than being interrupted inside evaluation.

### Lifetimes

The worker Session retains its interned strings, terms, environments, boxes, and evaluation memo for its lifetime.
They are append-only; idle arena reclamation is not implemented.
Each publication copies the syntax pools and open-file source data into storage owned independently of the worker.
Published term ids refer to those copied pools; the UI may intern edit terms locally without affecting the Session.
Expanded views contain lifted terms, not worker Box ids.

Compiled artifacts own their signal pools, Plan, UI tree, and DSP instance together.
Requests hold the artifact against which state matching was prepared.
The UI rejects a prepared instance if that base has changed.
The audio host retains old instances through the crossfade; the UI collects retired voices and returns their artifacts and obsolete publications to the worker for destruction.
Shutdown stops the callback before destroying artifacts.

---

## Compilation

### Evaluation and propagation

Evaluation is a memoized recursive traversal of Term producing Box: desugaring the surface forms, beta-reduction, pattern matching, `with`/`letrec`/`environment` scoping, iteration unrolling, metadata collection and label substitution.
Box carries arity, checked at construction, so the language's most common error is caught with a precise source range.
A closure is a pair `(term id, environment id)` — two integers — so closure equality is integer equality.

**`Error` has unconstrained arity and absorbs its neighbours**, so one typo yields one diagnostic rather than one per enclosing composition.
Error locality comes from the arity check rather than from a dedicated pass.

**The observability boundary is `.box` plus accept/reject.**
Two evaluators agreeing on `.box` isomorphism for every program, and on which programs are rejected, are indistinguishable.
That turns "which of the reference's behaviour is semantics and which is incidental" from a judgement into a test: write a probe `.dsp`, diff the `.box`.
Evaluation can therefore be *designed* rather than transcribed — the reference's pattern-matching automaton [R14], its `Tree` universal representation and its parse-time desugaring are all left out, while its observable behaviour is matched.

Pattern matching is per-`case` rather than per-automaton: a vector of per-rule environments behind an environment barrier, plus a live-rule set, matched one applied argument at a time.
The automaton shares only the prefix of arguments already tested, which a live-rule set carries just as well.
Equality in matching is Box identity, so **Box is hash-consed** as Signal is.

Propagation then walks Box, threading input signals through the composition operators, producing Signal.

### Recursion in a flat DAG

`~` introduces cycles, and cycles cannot be hash-consed bottom-up.
A recursive group is a `Rec` node owning N body node ids, with `Proj(rec, i)` nodes for its outputs.
Build it in two phases: reserve the `Rec` id, build the body against it, then hash the group *as a unit* with self-references replaced by a canonical positional marker.
The group interns like any other node; only its construction is special.

**`Proj(rec, i)` reads output `i` at the current sample.**
Folding the feedback delay into `Proj` is tempting and wrong: `A ~ B` returns A's outputs *undelayed*, and the one-sample delay sits only on the path back into the body — which is nearly every audio path in the language.
So the delay stays an ordinary node, and causality comes from a construction invariant: a `Rec` body may reference its own projections only through a delay of at least one.

### Analyses

Over the Signal DAG, memoized per node id:

- **Type inference** — nature (int/real) and variability (constant / block-rate / sample-rate), a fixpoint over `Rec` groups.
  Faust's type carries three further dimensions, all omitted: computability and vectorability serve codegen modes this design does not have, and boolean-ness is used only by a deprecated backend.
  Variability decides which band a computation lands in, so its lattice is matched exactly — including that any delay forces sample-rate and a nullary `ffunction` is sample-rate by assumption.
- **Interval inference** — bounds per node, kept because generated code depends on it: it sizes delay lines, decides which table and soundfile accesses need clamping, and rejects programs whose delays cannot be bounded.
  `assertbounds`, `lowest` and `highest` are the user's overrides and feed straight into it.

Interval precision is *observable*, not an internal quality setting, which limits how imprecise the rules may be: a widened bound can reject a program the reference accepts, or add a clamp the reference omitted.
Measured over the corpus, the backward slice from every index position is dominated by thirteen cheap operations — affine arithmetic, `min`/`max`, masking, truncation — so those get rules at least as tight as the reference's and everything else may widen.
Widening degrades to a diagnostic, never to a silently under-sized delay line.

There is **no occurrence or sharing analysis**.
The reference needs one to decide whether to emit a temporary or inline an expression; under a three-address Plan every node already has a virtual register, so that consumer disappears.
The one exception is **maximum delay per signal**, which makes `x'`, `x@3` and `x@5` share one delay line sized to the largest use.

Between typing and lowering sits **promotion**: Faust's implicit conversions are materialized as cast nodes on the operands.
Promotion needs types and invalidates them, so it runs as rewrite-then-retype, twice around the simplifier.
The ordering affects results, not just form: a simplifier rule that *reads* a literal sees it promoted, while a rule that *writes* one emits it unpromoted.

### Plan

Lowering does three things: **schedule by rate** (init / control / sample bands, from variability — most of the runtime gain comes from hoisting slider arithmetic out of the sample loop), **allocate state** (one delay line per delayed signal, in the reference's three size cases, each field with a stable identity), and **emit three-address instructions** over virtual registers in topological order per band.

Plan is mostly straight-line.
The exceptions are named so both backends implement the same thing: bounded loops in the init band only (table and waveform fills, whose generator subgraph is its own little program), and **guards** — `enable` and `control` make a range of instructions conditional.
A guarded instruction writes to a *state field*, not a register: when the guard is false the value must keep whatever it last computed, possibly from an earlier block.
So the instruction set is three-address code plus `loop_begin`/`loop_end` and `guard_begin`/`guard_end`.

`select2`/`select3` are plain instructions, **not** guards: Faust evaluates both branches unconditionally and then selects, so `select2(c, 1/x, 0)` divides regardless of `c`.
Turning a select into a branch changes which programs produce infinities and NaNs, and therefore the audio.

**Tables are values, which orders reads against writes.**
`wrtable` produces a table value and `rdtable` consumes one, so a read *depends* on the write and topological emission places them correctly with no ordering rule of its own.

**Dead code elimination is implicit**: emission walks backward from `process`'s outputs, so anything unreachable is never emitted.
That makes the root set part of the semantics — `attach(x, y)` forwards `x` and widens the root set with `y`, which is how a `vbargraph` nobody reads stays alive.

### Two backends

**Interpreter** — a register machine over a flat register file plus the state block, with a switch or computed-goto dispatch loop.
This exists so the edit loop never waits on LLVM.

**LLVM** — IR emitted directly from Plan, no intermediate framework.
Faust's output shape is a loop around straight-line float math, so LLVM's optimizer does nearly all the work including auto-vectorization.
**LLVM is an optional dependency**, which keeps the edit loop's build fast and a browser/wasm deployment cheap.
Ahead-of-time export is the same artifact serialized — an object file plus the descriptors, which are already data.

---

## The runtime contract

A **compiled artifact** is a Plan plus four descriptors: the UI tree, the soundfile requirements, the foreign symbol requirements, and the file's `declare` metadata.
Descriptors are data, not code, and are shared unchanged by both backends.

```
create(artifact)                  allocate the state block; nothing computed yet
constants(sampleRate)             run the init band: rate-dependent constants, table fills
resetControls()                   UI fields to their declared init values
clear()                           delay lines, registers and guarded values to zero
init(sampleRate)                  all three, in that order
compute(frames, in[], out[])      run the control band once, then the sample band `frames` times
destroy()
```

Start-up splits three ways because its parts are needed separately: a sample-rate change needs `constants` alone, and a live reload needs `constants` plus a partial `clear` of whatever failed to migrate.
**A sample-rate change is a re-`constants`, not a recompile** — delay lines are sized from the *interval* of the delay index, and the standard library clamps `ma.SR` to `[1, 192000]`, so nothing in the layout moves.

Control rate is the host's block rate: a slider takes effect at the next block boundary.
Block size is unconstrained — with no vectorization there is no per-block scratch, so `compute` accepts any frame count and the audio thread never allocates.
The buffer sample type is independent of internal precision and converted at the boundary.
The audio thread runs with flush-to-zero and denormals-are-zero enabled (recursive filters decay into denormal territory constantly); the conformance harness runs with both *off*, because the reference oracle does.

**The UI tree is a descriptor, not a visitor.**
Faust carries its UI structure in *labels*: group boxes prefix a path onto the labels beneath them, and a label is not a string but a path expression — `v:name`/`h:name`/`t:name` open a group from within the label itself, `/` separates, `../` walks up.
Labels are also *evaluated*, so `"Gain %i"` inside `par(i, 8, ...)` becomes eight distinct labels.
The extracted tree holds groups, input widgets with their state field and `init`/`min`/`max`/`step`, output widgets, and parsed metadata — including keys this project does not act on, so a host can implement `midi` or `acc` on top of them.
The host renders from the tree and writes values directly into the named state fields: no callback machinery, and reference Faust's JSON description would be a straightforward serialization of this structure.

**The host owns file I/O.**
The artifact lists each soundfile's label, URL set and channel count; the host resolves URLs and decodes audio, and caches decoded data by URL across recompiles so an edit never re-decodes a large sample.
A missing or undecodable file is *not* an error — it substitutes the reference's behaviour (256 parts of 1024 silent frames) and reports a diagnostic, because a typo'd filename must not take down the edit loop.

**One symbol registry serves both tiers.**
Faust's foreign constructs split into runtime-provided constants — above all `fconstant(int fSamplingFreq, ...)`, which is how `ma.SR` is defined and so appears in essentially every program — and genuine external calls, which are rare.
A map from (name, signature) to a native pointer covers both, so the interpreter can call native code and LLVM stays optional.
Foreign signatures are restricted to scalar arguments and returns, so a handful of thunk shapes covers every case.
An unresolved symbol poisons its subgraph rather than failing the compile.
This environment *loads* native functions; it does not compile C.

---

## Live reload

### State preservation

Editing a gain constant must not silence a reverb tail, and moving a slider then editing code must not reset the slider.

Every state field gets a **stable identity derived from the Signal node that owns it**, not from allocation order.
Because Signal nodes are hash-consed from Merkle-hashed terms, an unchanged filter keeps its identity automatically — and that identity survives unrelated edits elsewhere in the file, which holds only because the Term hash is provenance-free.

The node hash alone is not enough: state for a feedback network lives on its `Rec` node, whose hash covers the whole body, so editing a gain constant *inside* the reverb changes that hash and the tail dies.
Merkle hashing propagates changes upward; state sits at the top.
So every field carries a second key — a **shape hash**, the same Merkle hash with numeric literal payloads normalized away.

This is the **alignment problem**, which the literature splits into *keyed* alignment on a stable identifier and *similarity* alignment on resemblance [R11].
The three passes are keyed, similarity, fresh:

1. **Exact hash match** — an untouched subgraph.
   Copy.
2. **Shape hash match** — same shape, different constants.
   Copy.
   This is what keeps the reverb tail alive across a gain edit.
   Fields sharing a shape hash are paired **by source proximity**, not allocation order: in a program with eight identically shaped filters, deleting the third would otherwise shift every later one.
   Proximity is byte distance between ref ranges, paired greedily, ties breaking toward the lower offset.
   (A structural diff [R6] is the other answer, rejected here: the splice has both trees at once, while migration compares a layout built a revision ago against a ref tree rebuilt since.
   Byte offsets survive that; node identity does not.)
3. **No match** — initialize fresh.

Within a match, a field of different length copies the common window relative to the write head and zeros the remainder.

Three field classes need a rule beyond node identity.
**UI values** key on their label path and are *not* migrated by the passes above: those passes are pairwise between adjacent revisions, so a value carried that way dies the first time a revision omits its path — too fragile for "a moved slider stays moved".
A UI value needs a home that outlives any instance, so it lives beside the text as editable session state, layered over `resetControls`.
**Soundfile pointers** key on URL.
**Tables and waveforms** are recomputed rather than migrated: their contents are a pure function of the init band.

### Hand-off

Compile, initialize, decode soundfiles, and match state fields on the worker.
Preparation reads the old Plan and source offsets, never the running DSP state.
The UI publishes a prepared voice by atomic pointer hand-off.
At the next callback boundary, the audio thread copies the matched state from the current instance before running the replacement.
This copy uses precomputed field pairs and allocates nothing, but its cost scales with the carried delay history.
A pending hand-off must be consumed before another is accepted, so the migration base is the instance at that boundary.
Retired artifacts are freed on the worker after the callback has finished with them.
Never allocate or lock on the audio thread.
The first device open occurs on the UI thread and reinitializes the first instance at the device sample rate before starting callbacks.
State preservation alone still clicks when the graph changes structurally, so **cross-fade** over a few milliseconds, running both instances during the overlap.
The fade is *linear, with weights summing to one*, so two instances producing the same signal sum to that signal — an equal-power fade would raise it by as much as 3 dB.
When the new Plan hashes equal to the old, skip the swap entirely.

Three threads: audio (RT), compile (query engine), UI.
Control writes cross a thread boundary too, and the instance swap does not cover them: UI-bound control fields and bargraph fields are single scalars, so they are relaxed atomics.
Faust's own architectures write these as plain pointers, which works in practice and is a data race on paper.

### When the program does not compile

Most keystrokes leave the program transiently invalid, making this the common state of the system rather than an edge case.

- **`process` resolves and its reachable subgraph is error-free** → build, migrate, crossfade, swap.
- **`process` is missing, unresolvable, or depends on an `Error` node** → **do not swap.**
  The last good instance keeps playing.

So the environment deliberately lets the two views diverge: **you see the broken program and hear the last good one.**
The box view still updates, since it renders Term, which survives a partial parse.
The alternative is silence or NaN on every incomplete keystroke — but the divergence must be visible in the UI, or the user wonders why an edit had no audible effect.

---

## Error resilience

A batch compiler may throw on the first error.
A live editor may not.

- **Errors are values, not exceptions.**
  Every phase emits diagnostics attached to Term *value* ids, which resolve to source ranges by walking the open file's ref tree — marking every occurrence of a broken fragment.
- **Errors are local.**
  An explicit `Error` node propagates through Box and Signal: a broken definition poisons itself and its dependents, and everything else still compiles.
- **Partial trees still work.**
  A half-typed expression becomes a `Hole`, so the rest of the file still renders as boxes and the hole prints back verbatim.
  This is a recovery policy, not Hazel's formal guarantee of meaningful typed-hole states [R12].
- **The program with a hole in it does not run.**
  Hazel evaluates *around* holes; not here, because running around a hole means choosing a signal for it, and every choice is a program the user did not write, played into their monitors at full gain.
  Hearing the last good program is the better answer for a DSP.
- **Divergence is bounded.**
  `foo = foo;` makes evaluation loop forever, which reads as the editor freezing rather than as a broken program.
  The guard is nearly free, since evaluation is already memoized on `(value id, environment id)`: marking a key in-flight detects re-entry, and a depth counter bounds the rest.

A diagnostic is `{severity, code, subject, related[], payload}`.
`severity` is what the swap rule reads — only an `error` reachable from `process` blocks the swap.
`code` is a stable enum prefixed by the phase raising it (`syn`, `res`, `eval`, `type`, `plan`, `link`), which lets error *classes* be compared against the reference's error corpus rather than message strings.
`subject` is an interned value id, never a byte range.
Diagnostics hold permanent ids only, so a set outlives an arena drop, and the set is deterministically ordered.

---

## Scope

**In scope**: the Faust definition language and block-diagram algebra complete — definitions, `with`, `letrec`, `environment`, abstraction/application, pattern-matching `case`, `import`/`component`/`library`, `declare`, metadata, the five composition operators, `route`, iterations, all primitives, plus three constructs the papers omit (`expr[defs]`, modulation `[a -> b]`, and `any` as a foreign argument type).
The full signal layer, numerical fidelity to reference Faust, two execution tiers, lossless bidirectional editing, state-preserving hot reload, and a complete runtime contract.

**Cut, and what each cut costs:**

| Cut | What it saves | What it costs |
|---|---|---|
| Fixed-point / resolution inference | The large majority of the reference's interval library | No fixed-point targets |
| Vector and scheduler codegen modes | Two code containers and a task-graph scheduler | Relies on LLVM auto-vectorization; no multicore DSP |
| Eighteen text backends | A backend abstraction layer and eighteen emitters | No Rust/Julia/wasm-text output |
| SVG drawer, mathdoc generation | Two independent subsystems | The box UI supersedes the drawer |
| `libfaust` API compatibility | The `global` singleton and its threading contortions | Not a drop-in for existing tooling |
| MIDI/OSC implementation | Two protocol stacks and their threading | Metadata is preserved, so a host can add them |
| Polyphony | Voice allocation, note routing, the `effect` convention | Addable above the runtime contract |
| Compiling C for `ffunction` | A C toolchain dependency | Foreign code must be registered or `dlopen`able |

Interval analysis is the one *partial* cut: the reference covers every primitive to a high standard because fixed-point demands it, but without fixed-point, intervals matter only where they change generated code.
What is **not** cut is the definition language, the algebra, or numeric fidelity — those are the language.

---

## Correctness

The reference repository ships an oracle at five levels over 94 programs, and this project uses all of them: `.box` (evaluated diagram as source), `.sig` (normalized signal graph), `.type` (per-node type and interval), `.fir` (state layout and band split) and `.ir` (a 60000-frame impulse response).

**Compare graphs, not bytes.**
Isomorphism validates evaluation and propagation while leaving the project independent of the reference's pretty-printing.
`.box` is ordinary Faust source, so it parses with our own front end and both sides run through *our* evaluator — making the comparison a check on the evaluator rather than on the reference's printer.
`.sig` and `.fir` need parsers of their own, which live under `test/conformance/`.
`.type` is compared as a multiset and as a *projection*, since the reference encodes five dimensions per entry of which this design computes two.

**The harness regenerates the oracle itself**, from the pinned submodule at this project's own precision, rather than trusting the checked-in files — the `faust` on a developer's `PATH`, the submodule and the shipped reference files are three different versions.

**Numeric fidelity is the biggest risk, and it is handled by measurement.**
Floating-point addition and multiplication are not associative, which makes arithmetic normal form *semantically observable*: a different but equally reasonable ordering usually stays inside tolerance until a resonant filter or long feedback network accumulates and diverges.
Rather than preemptively port the reference's normalizer wholesale, build the simple thing, run the whole corpus against the reference's own `filesCompare`, and port the reference's behaviour where the comparison fails.

A second class of risk is semantics that pass every structural check and then fail the impulse comparison.
Each is pinned with a purpose-built probe: division always yields a float even for two integers; `%` is C signed remainder and `>>` is arithmetic; `int(x)` truncates toward zero; out-of-range and NaN float-to-int conversion is *defined* here (using the reference's own `-cir` rewrite) because the reference's backends disagree; `:>` sums and `<:` replicates modulo; `route` is 1-based, silently partial and additive; table accesses are clamped only where the interval cannot prove them in range.

**Six properties have no analogue in the reference compiler**, each protecting something the product depends on:

1. **Splice fidelity** — corpus-wide token, printer, identity, and generated-edit checks, with bounded full-program reparsing and destination-correspondence checks.
2. **Incremental equivalence** — an incrementally recompiled result must be *identical* to a from-scratch compile of the edited text.
   The invariant a memoizing compiler most easily breaks.
3. **Tier agreement** — interpreter and LLVM outputs match within tolerance.
4. **State-preservation continuity** — a no-op edit while running leaves the output sample-identical; a gain edit inside a feedback network does not lose the tail.
5. **UI completeness** — the extracted UI tree matches the `User Interface` section of the shipped `.fir`.
6. **Control responsiveness** — writing a UI field changes the output within one block, and changes *only* the outputs that depend on it.

Corpus sweeps also run under ASan and UBSan (`FAUSTLENS_SANITIZE=ON` configures a second build tree).
They are the only thing exercising the evaluator at scale, and a memory bug there surfaces as a *wrong value* — the failure mode a graph comparison is least able to localize.

---

## Layout

```
faustlens/
  src/
    syntax/      lexer, recursive-descent + Pratt parser, Term values + refs,
                 interning, Merkle hashing, provenance side tables, printer, splice
    files/       overlay VFS, import resolution, embedded stdlib
    eval/        evaluator, pattern matching, interned environments, folding, memo cache
    box/         Box graph, arity
    signal/      flat DAG, hash-consing, normalization
    runtime/     interpreter, state migration, soundfiles, foreign symbol registry
    query/       memoized query engine, revisions, invalidation, snapshots
  app/           worker publication, live artifacts; application dependencies
    editor/      text widget, buffers, shared history, cursor-preserving splices
    boxview/     derived layout, wires, occurrence selection
    controls/    UI tree -> ImGui widgets
    audio/       audio device, soundfile decoding, callback hand-off
  test/
    conformance/ driven by lib/faust corpora, including parsers for the reference
                 `.sig` and `.fir` dumps and the `.ir` harness protocol
    property/    the six invariants above
  lib/           every dependency, as a submodule
    faust/               oracle only, never linked
    tree-sitter-faust/   test only: acceptance oracle
    sdl3/ imgui/ miniaudio/   app layer only
    doctest/             unit tests
```

**C++23**, built with CMake — decided by LLVM, which ships `find_package(LLVM)` and expects it.
Data oriented throughout: flat arrays, `uint32_t` indices instead of pointers, opcode switches instead of virtual dispatch — the opposite of the reference's visitor-heavy style.
One failure convention: `std::expected<T, std::string>` wherever a call fails for a reason worth reporting, `std::optional` where absence is an ordinary outcome and not a failure, and an accumulated `std::vector<Diagnostic>` where a phase has many problems to report rather than one.
Interning everywhere identity matters, deterministic containers on any path that affects output.

Dependencies are deliberately few and every one is a submodule.
Nothing under `src/` links an unbounded dependency; the application layer adds SDL3, Dear ImGui and miniaudio.
The embedded standard library is a generated translation unit — a CMake step walks `lib/faust/libraries/` recursively and emits `std::string_view` constants keyed by relative path, stamped with the submodule SHA so the embedded library and the oracle provably come from the same commit.
Every embedded file is byte-identical to the submodule, and *eject* is the only path to a modified library, so the distribution never contains a changed copy.

---

## The application layer

**SDL3** for platform, with SDL_GPU natively (it dispatches to Metal, Vulkan and D3D12) and WebGPU on web, so the web build swaps one file.
**Dear ImGui**, docking branch.
**miniaudio** for device I/O, chosen for two reasons: its data callback maps onto `compute` with no adapter, and `ma_decoder` handles wav, flac and mp3 in-tree, so soundfile decoding needs no further dependency.

**The text buffer is a contract, not a widget.**
Source bytes are authoritative for the authored program.
The UI owns the buffers and a Workspace history over text, selection, and control values; immutable buffer copies form the worker VFS overlay.
Structural edits and expansion return the same rewrite type.
The UI previews its replacement term or splices it through Workspace, using the same source check and history for every structural edit.
Six requirements: byte-addressed splices applied as one undoable unit preserving the cursor; undo over a previous *state*, not a delta; byte-offset cursor mapping both directions; readable buffers; an undo unit that **spans files** (per-file stacks are not a weaker version of this but a wrong one — undo in one file can silently discard a newer edit in another); and an undo unit that carries **non-text state**, since a control value is neither text nor derived from it.
Entries stay cheap because they share their unchanged parts: an untouched file is a refcount, not a copy.
An inverse can be wrong; a shared pointer to an immutable value cannot.

The multiline ImGui widget edits a draft and commits one byte-range replacement per changed frame through Workspace.
Its private undo shortcuts are disabled; global undo and redo restore Workspace state, including cursor and selection, into the active widget.
Incomplete source remains editable while the last good audio continues.
Save writes the active buffer; embedded library buffers save as a local file beside the root program.

**The worker publishes owned view snapshots** containing request and document revisions, independent syntax pools, file text, ref trees, token vectors, diagnostics, and lifted expansions.
The UI never calls a query and does not wait for compilation during a frame; it renders the newest completed snapshot while showing current buffer bytes.
Only the latest request can publish, and the UI checks its ticket again before adoption and audio hand-off.
Structural edits require source bytes matching the snapshot.
Materialization also requires the selection's document revision, so a dependency edit invalidates a pending evaluation even if the selected file's text is unchanged.
Occurrence highlights use the snapshot's source spans and are hidden while its text is stale.
Opening files, tracing controls, and requesting expansions also go through the worker.

**The box view has no coordinates.**
Position is derived: each node lays its children out in a fixed pattern by kind and computes its bounding box bottom-up.
Expanded layout is keyed by source occurrence as well as value, so opening one repeated stage does not open all of them.
Wires only ever connect siblings inside one composition node, so there is no edge routing and no layout solver.

Derived layout adds no independent layout data to the synchronized document.
An asymmetric lens can still preserve information hidden from its view: source comments and formatting already provide such information [R13].
Persisting coordinates would be a document-model choice, not by itself a change to a symmetric lens.
Selection carries a chain of source refs and is re-resolved from its byte offset after a reparse.
Expansion refs belong to their source snapshot and are cleared on document edits; same-source publications retain them.
The derived layout needs no free-positioned node-editor library.

Interaction is keyboard-first, with the mouse for navigation.
Arrow keys walk the term structurally.
Delete removes a stage; typing a composition operator wraps the selection; Enter on a literal opens an inline field; a click between two stages opens a completion popup.
**Every one is a term rewrite plus a splice**, so the text pane visibly changes on each and the box view has no private edit path.

---

## References

**Bidirectional transformation**

- **[R4]** Zirun Zhu, Zhixuan Yang, Hsiang-Shang Ko, Zhenjiang Hu.
  *Retentive Lenses.*
  2020, arXiv:2001.02031. — The Text/Term framing and edit-updated correspondence links.
  [Paper](https://arxiv.org/pdf/2001.02031).
  Adds **Retentiveness** to Correctness and Hippocraticness, and enriches `get` to return links that `put` consumes.
  Its motivating example is a comment-carrying CST with a `Paren` constructor against a paren-free AST.
- **[R5]** J. Nathan Foster, Alexandre Pilkiewicz, Benjamin C. Pierce.
  *Quotient Lenses.*
  ICFP 2008. — Laws modulo chosen equivalences, with canonizers and representative selection.
  [Paper](https://www.cis.upenn.edu/~bcpierce/papers/quotient-lenses.pdf).
- **[R11]** Davi M. J. Barbosa, Julien Cretin, Nate Foster, Michael Greenberg, Benjamin C. Pierce.
  *Matching Lenses: Alignment and View Update.*
  ICFP 2010. — Alignment policies and their separation from view update.
  [Paper](https://www.cis.upenn.edu/~bcpierce/papers/alignment.pdf).
- **[R13]** Martin Hofmann, Benjamin C. Pierce, Daniel Wagner.
  *Symmetric Lenses.*
  POPL 2011. — Complements and synchronization when both sides contain private information.
  [Paper](https://www.cis.upenn.edu/~bcpierce/papers/symmetric.pdf).
- **[R3]** Mikaël Mayer, Viktor Kunčak, Ravi Chugh.
  *Bidirectional Evaluation with Direct Manipulation.*
  OOPSLA 2018, arXiv:1809.04209. — Update semantics for evaluated views, with correctness depending on the merge policy.
  FaustLens uses explicit materialization.
  [Paper](https://arxiv.org/html/1809.04209v1).

**Syntax, sugar and printing**

- **[R1]** Justin Pombrio, Shriram Krishnamurthi.
  *Resugaring: Lifting Evaluation Sequences through Syntactic Sugar.*
  PLDI 2014.
- **[R2]** Zhichao Guan, Yiyuan Cao, Tailai Yu, Ziheng Wang, Di Wang, Zhenjiang Hu.
  *Semantics Lifting for Syntactic Sugar.*
  OOPSLA 2024. — The problem avoided by never desugaring in the editable layer.
- **[R9]** Tillmann Rendel, Klaus Ostermann.
  *Invertible Syntax Descriptions: Unifying Parsing and Pretty Printing.*
  Haskell Symposium 2010.
- **[R10]** Kazutaka Matsuda, Meng Wang.
  *FliPpr: A Prettier Invertible Printing System.*
  ESOP 2013; *A System for Deriving Parsers from Pretty-Printers*, New Generation Computing 2018. — Derives a consistent CFG parser from a printer.
  Integrating recovery, source references, and retained-fragment edits would require additional work.
  [Paper](https://research-information.bris.ac.uk/ws/files/160992789/Meng_Wang_FliPpr_A_System_for_Deriving_Parsers_from_Pretty_Printers.pdf).
- **[R6]** Sebastian Erdweg, Tamás Szabó, André Pacak.
  *Concise, Type-Safe, and Efficient Structural Diffing* (truediff).
  PLDI 2021. — Heuristic alignment for splice callers without explicit occurrence links; not used for state migration, which has no tree identity surviving a reparse.

**Live programming with incomplete programs**

- **[R12]** Cyrus Omar et al. *Total Type Error Localization and Recovery with Holes.*
  POPL 2024; *Live Functional Programming with Typed Holes*, POPL 2019. — Typed-hole semantics as a reference for partial programs, not a theorem established by this parser's recovery nodes.
  Evaluation *around* holes is not adopted here.

**Error recovery in generated parsers**

- **[R7]** tree-sitter issue #1870, *How does one improve the error recovery of a grammar?*
  Open.
- **[R8]** tree-sitter discussion #1205, *Is there any way to give hints to the error recovery process?* — Together, the evidence for a hand-written parser: no `error` production, no synchronization tokens, no cost hints.

**Pattern matching**

- **[R14]** Albert Gräf.
  *Left-to-Right Tree Pattern Matching.*
  RTA 1991, LNCS 488. — The construction behind the reference's `patternmatcher/`.
  Not used: the automaton shares only the prefix of arguments already tested, which a live-rule set carries directly.
