# FaustLens: a bidirectional editor and compiler for Faust

A from-scratch Faust compiler built around bidirectional editing: a box graph and a text editor
showing the same program, either one editable, updating each other as you type, with audio running
throughout.

Reference implementation surveyed for semantics and used as a test oracle: `lib/faust` @ `515dc515c`
(2.85.9-25). No libfaust code is linked or ported. An earlier MLIR-based design is preserved in
`DESIGN-mlir-rejected.md`; §16 records why it was rejected.

---

## 1. What this actually is

This is not primarily a compiler project. It is a **live programming environment** whose compiler
happens to need to be correct and fast. That ordering decides nearly every question below: where
compiler instinct and editing experience conflict, editing wins.

Three properties define the product:

1. **Bidirectional.** Selecting a box highlights its source range. Editing a box rewrites that
   source range, leaving every other byte — including comments and formatting — untouched.
2. **Live.** Every keystroke produces a new running DSP, fast enough to feel immediate, without
   dropping the reverb tail or clicking.
3. **Simple.** Small enough that one person holds the whole thing in their head. A hard constraint,
   and §10 spends scope to buy it.

Faust's reference compiler is none of these, for good reasons: it is a batch compiler targeting
twenty backends with fixed-point support and a decade of accumulated completeness, and reusing its
architecture imports all of that. Reusing its *semantics* is mandatory; reusing its structure is
not.

---

## 2. Goals and non-goals

**Goals**

- The Faust definition language and block-diagram algebra, complete: definitions, `with`, `letrec`,
  `environment`, abstraction/application, pattern-matching `case`, `import`/`component`/`library`,
  `declare`, metadata, the five composition operators, `route`, iterations, all primitives. Three
  constructs the papers omit are in scope: local definition modification `expr[defs]`
  (`parser/faustparser.y:514`), modulation `[a -> b]` (`:609`), and `any` as a
  foreign-function argument type (`:780`). The `minput` spelling of modulation is not: its production
  is commented out (`:605-606`), so the token lexes but no rule reaches it. Lexically: `<mdoc>` blocks
  (`parser/faustlexer.l:53`),
  `::`-qualified names, and the `+>` spelling of `:>`. Mathdoc *generation* is a non-goal (§10), but
  an unlexed `<mdoc>` block is a parse failure on a file reference Faust accepts.
- The signal layer: delays (`@`, `'`, `mem`, `prefix`), `rdtable`/`rwtable`, `select2`/`select3`,
  `waveform`, `soundfile`, `attach`, `enable`/`control`, `inputs`/`outputs`, the
  `assertbounds`/`lowest`/`highest` bound hints, foreign functions/constants/variables, and all UI
  widgets.
- Numerical fidelity to reference Faust (§11).
- Two execution tiers: an interpreter for the edit loop, LLVM for release.
- Lossless bidirectional editing between the box graph and the text.
- State-preserving hot reload while audio runs.
- A complete runtime contract (§7): controllable UI, soundfile loading, foreign symbol resolution,
  a defined init/block/channel contract — enough that a compiled program is *playable*, not merely
  correct.

Internal precision is a build option defaulting to **f64**, matching the `-double` that generated
the reference `.ir` files (`tests/impulse-tests/Make.ref:20`) and removing a class of divergence
noise from the oracle (§11). The `.box`, `.sig`, `.type` and `.fir` files in the same directory are
`-single`, since those four targets replace the option list wholesale with `-I dsp`
(`tests/impulse-tests/Makefile:473-491`), discarding the `-double` — which §11.1 has to account for.

**Non-goals**

- Fixed-point arithmetic and resolution inference.
- Vectorized and multi-threaded scheduler codegen modes.
- The eighteen text backends (Rust, Julia, D, C#, Cmajor, JSFX, VHDL, wasm, ...).
- The block-diagram SVG drawer and the mathdoc generator.
- Being a drop-in `libfaust` replacement.
- **MIDI and OSC control.** The metadata is parsed and exposed (§7.2) so a host can implement them;
  no implementation ships here.
- **Polyphony** (`[nvoices:...]`, the `process`/`effect` convention). Voice allocation over N
  instances is a host concern, so it can be added later without touching anything below §7.
- **Compiling C.** Foreign functions are *resolved*, not built (§7.4).
- **Non-default precision *modes*.** The four precision prefixes are statement prefixes rather than
  per-expression annotations, and §4.5 honours them as the reference does. What is cut is the
  arithmetic behind two: `quadprecision` and `fixedpointprecision` select nothing here, so a name
  defined only behind one resolves to an undefined-symbol diagnostic. Zero files across `libraries`,
  `tests` and `examples` use any of the four.

Each non-goal buys simplicity deliberately rather than by oversight. §10 prices them.

---

## 3. The representation stack

```
   Text                    source of truth, user-owned
    | ^
    | |  exact, byte-preserving splices                        §4
    v |
   CST                     tree-sitter. Trivia, error recovery, byte ranges.
    | ^
    | |  exact                                                 §4
    v |
   Term                    definition language AST, in two layers:             §4.6
    |                      interned Merkle-hashed *values*, and per-file
    |                      *refs* carrying CST ids.
    |                      *** the editable semantic surface ***
    |  evaluate  (memoized on value id x environment id)       §5
    v
   Box                     evaluated diagram. Arity-checked. Read-only view.
    |  propagate
    v
   Signal                  flat hash-consed DAG. The optimization IR.          §6
    |  analyze, schedule, allocate state
    v
   Plan                    linear three-address instructions + state layout.   §6
    |                      + descriptors: UI tree, soundfiles, foreign syms    §7
    |                      = the compiled artifact
    |
    +--> Bytecode          edit loop. Sub-millisecond.                         §6.4
    +--> LLVM IR           release. Ahead-of-time or ORC JIT.                  §6.5
              |
              v
   Instance                state block + lifecycle. Hot-swapped, state-preserving.  §7.1, §8
```

Six representations sounds like a lot: three are front-end views of the same text, two are the
compiler proper. Three structural claims carry the design.

**Term is the editable surface, Box is a read-only view.** The single decision that makes
bidirectional editing tractable, developed in §4. Term is two layers: an interned, provenance-free
*value* graph that everything downstream consumes, and a per-file *ref* tree holding the byte
ranges (§4.6).

**Signal is a flat, hash-consed DAG, not a pointer graph.** `std::vector<Node>` where `Node` is a
POD of `{opcode, operand indices, payload}`, uniqued through a hash map at construction. It replaces
the reference's entire `tlib` hash-consing layer, gives structural sharing and CSE for free, makes
the incremental story work (§5.3), and traverses far faster than a graph of heap nodes.
Construction-site arity checking makes malformed nodes unrepresentable, so there is no verifier to
write.

**Plan is a linear instruction list — simultaneously a bytecode and a trivial LLVM emission
source.** The design's best simplification. The reference maintains FIR, a full imperative IR with
expression trees, statements and types, plus a distinct code emitter per backend. Produce
three-address code over virtual registers in the shared lowering instead and both backends go thin:
the interpreter is a dispatch loop, the LLVM emitter a one-pass walk mapping each instruction to one
or two IRBuilder calls. One IR, two nearly trivial consumers.

---

## 4. The editing model

### 4.1 The key decision

An evaluated Faust diagram cannot be edited back into source. Evaluation beta-reduces, unrolls
iterations and specializes pattern matches: `par(i, 10, osc(i))` becomes ten independent subgraphs,
with no general way to re-roll ten edited subgraphs into a `par`. `faust -e` exposes exactly this
lossiness, printing an evaluated diagram as source with every name and every iteration gone.

So do not invert evaluation. **Render the Term graph as boxes and edit that.**

`par(i, 10, osc(i))` displays as a single `par` node with a multiplicity badge, because that is what
the source says. Editing it rewrites the term, and the mapping back to text is exact because nothing
was evaluated in between. The hardest problem in the system becomes a non-problem: no
provenance-inversion machinery, no "is this edit legal" predicate, no partial round-tripping.

**Term is pre-desugaring; Box is post-desugaring**, which is the opposite of the reference and what
§4.4 rests on. Faust's *grammar* rewrites `a + b` into `boxSeq(boxPar(a, b), boxAdd())`
and `x'` into `boxSeq(x, boxDelay1())` (`parser/faustparser.y:489`, `:496`), along with `@`, the
comparisons and the bitwise operators. After that there is no way back — `(a, b) : +` cannot be
reprinted as `a + b` in general — so §4.4's printer soundness and splice locality are lost on the
first arithmetic expression in the file.

Term therefore keeps the surface forms, and desugaring happens during evaluation on the way to Box.
The payoff is not merely mechanical: the structural view shows `a + b` as an infix expression node
rather than a parallel-then-sequential composition, which is what a reader of the source expects.
The box UI consequently renders two shapes of node, diagram-shaped (`:`, `,`, `<:`, `:>`, `~`) and
expression-shaped (`+`, `*`, `@`, `'`) — a feature of the view, not an inconsistency in it.

Surface form means *spelling*, not just shape. `:>` and `+>`, `x^y` and `pow(x, y)`, `mem` and `'`,
`-x` (rewritten to `0 - x`, `parser/faustparser.y:600`), and the exact text of a numeric literal are
all distinctions the reference erases at parse time and §4.4's printer has to reproduce. Mirroring
the grammar recovers most of them for free, since the two spellings are usually two productions and
so two nodes; §4.5 inventories the nodes and the three cases that do need an explicit form tag.

### 4.2 The two views

- **Structural view (editable).** The Term graph. What the source says.
- **Evaluated view (read-only).** The Box graph. What the program means. Reachable by expanding any
  node: evaluate that subterm and display the result.

To edit inside an expanded view, the user invokes **materialize**: the Term subtree is replaced by a
printed form of its evaluated Box graph, the source is rewritten to match, and the result is
editable structurally. Lossy — names and the iteration are gone — but explicit and user-initiated,
which is the difference between a tool with a sharp edge and one that corrupts your file.

### 4.3 Mechanics

*Source to box.* Every Term *ref* carries the CST node id it came from; every CST node carries a
byte range. Selection and hover in either direction are a lookup. The interned value a ref points at
carries no range (§4.6).

*Box to source.* An edit is a Term rewrite plus a splice: print only the rewritten subtree, replace
exactly its byte range, leave the rest of the file alone. Comments and formatting outside the edit
survive because they were never parsed away — they are still in the CST.

The structural edits worth supporting first, each a small rewrite plus a splice: insert into a
sequence (`a : b` → `a : x : b`), delete a stage, change a literal or a UI parameter, wrap a
selection in a composition, rewire a `route`.

*Cursor to box.* Given a byte offset, find the innermost CST node, map to its Term ref, highlight
the box. The "linking" that makes the two panes feel like one program.

*Undo.* Text is the source of truth and everything below it is derived, so undo is a text-level
operation and needs no counterpart in Term, Box or the box view. Restoring a previous text state
re-derives the rest, and §8 treats the result as it would any other edit.

### 4.4 The printer is load-bearing

Term-to-source printing appears in three places: splicing edits, materializing evaluated subtrees,
and the `faust -e` equivalent. One structure covers all three.

**Text and Term form a quotient lens** (Foster, Pilkiewicz & Pierce, *Quotient Lenses*, 2008). `get`
parses text into Term; `put` is a term rewrite plus a splice taking the *original text* as well as
the new subtree — the CST is that second argument, and it is why bytes outside the edit survive. It
is a *quotient* lens because `get` is lossy: Term carries no trivia (§5.1), so `print(parse(src))`
cannot reproduce a file's comments and indentation. The equivalence is already named in §4.6: **two
texts are equivalent when they parse to the same interned value id**, one relation quotienting out
both trivia and redundant parentheses.

Four obligations, checked on the whole corpus:

> **CST fidelity.** `text(cst(source)) == source`, byte for byte. Outside the lens: a property of
> tree-sitter, and the guarantee that trivia survives a parse at all.
>
> **PutGet, printer soundness.** `value(parse(print(t))) == value(t)`, compared as interned ids and
> so exact (§4.6). A printed Term re-reads as the same Term, form tags included (§4.1). Quantified
> over **hole-free** terms (§4.5), since error recovery is context-dependent and a `Hole`'s bytes
> reparsed in isolation need not recover to the same shape.
>
> **GetPut, splice locality.** Rewriting a subtree to an equal subtree leaves the file
> byte-identical, and any other edit changes only the bytes inside that subtree's range. Holes are
> *not* excepted: `print(Hole(s)) == s` verbatim, so a splice across broken text preserves it.
>
> **The canonizer law.** Reprinting normalizes redundant parentheses away (§4.5), so GetPut is
> byte-exact where a subtree is already canonically parenthesized (§4.7) and converges in one step
> everywhere else: `splice(splice(f)) == splice(f)`.

**Printing a subtree is context-dependent, and plain `print` breaks PutGet.** A ref's range excludes
the parentheses around it — grouping parens are anonymous in both grammars (§4.7) — so the `Seq` in
`(a : b) : c` has the range `a : b`. Splicing a lower-precedence subtree over one changes how the
surrounding bytes parse: `x , y` over the `b : c` of `a : b : c` gives `a : x , y`, reparsing as
`(a : x) , y`. So the splice primitive is `print_in_context(term, parent_kind, side)`, parenthesizing
by §4.7's rule before replacing the range — which is why phase 1 builds it alongside the printer.

**Only the top of §3's stack is a lens**, and the boundary is §4.1's. Term to Box has a `get` and no
lawful `put`: evaluation beta-reduces and unrolls, so `par(i, 10, osc(i))` becomes ten subgraphs with
nothing to invert. Box is a projection, and `materialize` (§4.2) is the one operation that leaves the
lawful region, which is why it is user-initiated and announced.

GetPut is what keeps a user's comments and formatting in the file across an edit.

### 4.5 The Term inventory

Term is the one structure the whole design leans on, so it is written down rather than left to be
derived. The rule generating it is short: **one Term node per surface production in
`parser/faustparser.y`, no node the grammar does not write, and exactly one node for what the grammar
could not read.** Everything below follows from that.

**Statements**, the file level. `Import(spec)`; `Declare(key, value)` and its three-token variant
`DeclareDef(name, key, value)` (`:401-402`), which attaches metadata to a named definition and is
easy to miss; `Definition(name, clauses)`; and `MdocBlock(parts)`. An mdoc block is not opaque text
and has **six** part kinds (`docelem`, `:384-391`): prose, `<equation>` and `<diagram>` — which
switch the lexer back and enclose real expressions (`:423-427`) — plus `<notice/>`, `<listing/>` with
its three boolean attributes (`:412-416`), and `<metadata>name</metadata>`. Generating mathdoc is a
non-goal, parsing all six is not (§2).

**The precision prefix is a set, and it is spelled out in full.** The four spellings are
`singleprecision`, `doubleprecision`, `quadprecision` and `fixedpointprecision`
(`parser/faustlexer.l:226-229`) — *not* `float`/`double`, which lex as the cast primitives
(`:141-142`). A statement's `variantlist` accumulates by bitwise or (`:369-370`), and
`acceptdefinition` (`:46-50`) admits the statement when the set is empty or contains the build mode.
Term keeps the set and evaluation applies that filter unchanged, which is what lets one file define a
name once per mode; each filtered statement emits an **info** diagnostic (§9.1) naming what was
dropped. Zero files in `libraries`, `tests` and `examples` use any of the four.

**Expressions.** Five composition operators, binary: `Seq`, `Par`, `Split`, `Merge`, `Rec`. Scoping:
`With(expr, defs)`, `LetRec(expr, recdefs, wheredefs)` — the `where` clause optional (`:480`) — and
`ModifLocalDef(expr, defs)` for `expr[defs]`. A `recdef` is its own clause shape, not a `Definition`:
the name is primed and parameters are forbidden (`recinition`/`recname`, `:456-464`), which is §6.2's
mandatory prime as a grammar rule. Then `Apply(fn, args)`, `Access(expr, name)`,
`Lambda(params, body)`, `Case(rules)`, `Modulation(entries, expr)` for `[a -> b]`,
`Iterate(kind, name, count, body)` with kind in `{par, seq, sum, prod}`, `Inputs(e)`, `Outputs(e)`,
`Environment(stmts)`, `Component(path)`, `Library(path)`, `Waveform(numbers)`, and
`Route(ins, outs, entries)`. Three surface-only shapes complete it: `BinOp(op, lhs, rhs)` for the
eighteen infix operators, `Delay1(expr)` for `x'`, and `NegIdent(name)` for prefix `-`.

**`NegIdent` takes a name, not an expression.** The only prefix-minus productions are `SUB INT`,
`SUB FLOAT` and `SUB ident` (`:525-526`, `:600`), so `-x` is negation, `-3` is a literal, and
`-(1+2)` is a partial application of the bare subtraction primitive. Probed: `process = -(1+2);`
evaluates to `_, 3 : -`, a one-input box subtracting three, while `-x with { x = 3; }` evaluates to
`-3`. The adopted grammar spells the second `negate_id: seq('-', $.identifier)`.

**Leaves.** `Int(lexeme)` and `Real(lexeme)`; `Ident(lexeme)` — `NSID` is a lexer *definition*
(`faustlexer.l:35`), not a token: it and a leading-`::` form both return **`IDENT`** (`:232-233`), so
a qualified name is one token and `::foo` is legal; the nullary primitives (`_`,
`!`, `mem`, `prefix`, `int`, `float`, each operator used bare, `attach`, `enable`, `control`, the
math functions, `rdtable`, `rwtable`, `select2`, `select3`, `assertbounds`, `lowest`, `highest`); the
eleven UI constructors with their labels and bounds; and the three foreign forms `FFun`, `FConst`,
`FVar`. `FFun`'s signature holds **one to four names**, `|`-separated (`:748-751`), and the reference
picks by build precision — `nth(namelist, gFloatSize - 1)` (`signals/prim2.cpp:57-61`). So
`float sinf|sin|sinl(float)` resolves to `sin` at this design's default f64, and §7.4's registry key
is that selected name, not the declaration.

**`Hole(lexeme, children)` — the one node no production writes.** §9 needs Term to survive a partial
parse and §5.1 grades the grammar on it, so the exception is written down rather than improvised at
the first `ERROR` node.

- **Payload.** The source bytes tree-sitter could not parse, empty for a `MISSING` node, which is
  zero-width.
- **Children.** The Term nodes of the hole's own recognizable named children, so a hole at an
  `ERROR` node still renders `a` and `b` out of half-typed `a : b : `. They keep the box view
  populated through the incomplete expressions that are the common state of the system (§8.3).
- **Hash.** Over lexeme *and* child ids, since recovery depends on surrounding context and the
  children are not a function of the lexeme. Two recoveries of the same bytes intern apart.
- **Printer.** `print(Hole(s, _)) == s`, verbatim, ignoring the children — they are inside `s`. This
  is what keeps §4.4's splice locality unrestricted.
- **Downstream.** A `Hole` is itself a syntax diagnostic and evaluates to the `Error` box of §9,
  whose arity rule §6.1 gives. Nothing else in the pipeline needs a case for it.

**"Recognizable named children" is a predicate.** A hole is built at an `ERROR` or `MISSING` CST
node, scanning that node's *direct* children in source order:

1. **Anonymous node** (a bare token — `:`, `(`, `=`) → skipped; its bytes stay in the payload.
2. **Named node, not `ERROR`/`MISSING`** → lowered by the ordinary rules and kept. Lowering recurses
   arbitrarily deep; only the *scan* is shallow.
3. **Named `ERROR`/`MISSING`** → a nested `Hole`, kept. Nesting is preserved, so two recoveries of
   the same bytes into different shapes hash apart.
4. **Named node whose lowering has no case** → a `Hole` over its bytes, which keeps the lowering
   total. On the corpus this is a **hard test failure**: every corpus file parses cleanly, so an
   unlowerable named node means the grammar and §4.8 have drifted apart. Holes stay a runtime concept
   for user text.

One level is enough: it already yields `a` and `b` from `a : b : `, and each kept child carries its
own subtree. The child list may be empty — the ordinary case for a `MISSING` token, printing as the
empty string — and every child's range lies inside the payload, which is what makes the verbatim
printer rule consistent with keeping children at all.

**Three form tags, not one per surface choice.** §4.1 asks for a tag "wherever the grammar offers a
choice", which overstates the cost: because Term mirrors productions, most of those choices are
*already* distinct nodes. `a + b` is `BinOp`, `(a, b) : +` is `Seq(Par(...), Prim)` and `+(a, b)` is
`Apply(Prim, ...)` — three shapes, no tag. `x'` is `Delay1`, `x : mem` is `Seq`. What genuinely needs
a tag is where two spellings produce one node:

| Tag | Choices | Why it cannot be inferred |
|---|---|---|
| Merge spelling | `:>` / `+>` | One lexer token, `MIX` (`faustlexer.l:94-95`) — the only true alias |
| Power spelling | `^` / `pow` used as a bare primitive | Both yield `gPowPrim` (`faustparser.y:573-574`) |
| Literal lexeme | `3` `3f` `3.` `.5` `3e2` `+3` | `atof`/`str2int` collapse them, and a leading `+` vanishes |

The literal tag is the load-bearing one: it also carries nature, since `3` lexes as `INT` and `3f` as
`FLOAT` (`faustlexer.l:77-79`). Keep the lexeme and both problems are one problem.

**What Term must refuse to do.** The reference desugars at parse time, and each case below is a place
where copying it costs §4.4's splice locality. Listed with what Term keeps instead:

- **Definition shape.** `makeDefinition` (`parser/sourcereader.cpp:135-157`) turns `f = e;` into a
  bare body, `f(x, y) = e;` into a lambda, a pattern clause into a one-rule `case`, and *several
  clauses of one name into a single `case`*. Term keeps `Definition(name, clauses)` with the clauses
  in source order.
- **Definition order and uniqueness.** `formatDefinitions` (`:435-460`) collects into a `map` keyed
  by name, so source order is gone and same-name clauses merge no matter how far apart they are.
  Term is a list, ordered, which §4.3's splices need and §5.5 requires anyway.
- **`environment { … }`** becomes `boxWithLocalDef(boxEnvironment(), defs)` (`faustparser.y:619`) —
  indistinguishable from a `with` afterwards.
- **Prefix minus** splits: `-x` becomes `Seq(Par(Int(0), x), Sub)` (`:600`) while `-3` becomes a
  negative literal (`:525-526`). One surface operator, two desugarings, neither reversible.
- **`route(n, m)`** gains a third argument, `boxPar(boxInt(0), boxInt(0))` — the "fake route" of
  `:621`. The two-argument spelling is not recoverable from the three-argument node.
- **Parentheses** are dropped outright: `( e )` yields `e` (`:602`). Term does the same, so grouping
  is *not* a node and the printer re-derives it from precedence. That is sound for §4.4's second
  property but not its third — reprinting a subtree normalizes the user's redundant parens away.
  Splices only reprint what was edited, so the blast radius is the edited subtree.

**What Term does not carry**: trivia, which is the CST's (§5.1); types and intervals, which are
Signal's (§6.3); arity, which is Box's (§6.1). Each is derived downstream and re-derived on demand,
so an edit invalidates nothing held in Term.

### 4.6 Values and refs

Term is interned (§5.2) and every occurrence carries a byte range (§4.3). The two live in separate
structures.

**The Merkle hash covers structure alone**, and carries no CST node id, since a node id encodes
position. A blank line inserted at the top of a file therefore leaves every hash below it unchanged,
along with every Signal node id and state field key derived from them — which is how §8.1's state
identity survives unrelated edits elsewhere in the file, and how a reverb tail survives a whitespace
edit.

**Two structures:**

- **`TermValue`** — interned, hash-consed, Merkle-hashed, provenance-free:
  `{kind, form_tag, payload, child value-ids}`. Deduplicated globally across every open and imported
  file. This is what §5.2 memoizes on, what §8.1 keys state on, and what §4.4's printer soundness
  quantifies over.
- **`TermRef`** — a per-file positional tree, one entry per syntactic node, `{value_id, cst_node_id}`,
  isomorphic to the parse. Rebuilt whenever its file is reparsed. Never interned, never hashed.

Evaluation, Box, Signal and Plan consume value ids only. Splices, selection linking, the box view and
diagnostics work on refs. That is the precise sense of "Term is the editable semantic surface": you
edit an *occurrence*, and the value is what makes two occurrences the same program fragment.

**Provenance lives in side tables, keyed by interned id, written first-writer-wins in deterministic
traversal order (§5.5).** One rule, applied at every layer: `signal id -> value id` is a side table
too, which is what keeps the originating term out of a Signal node's hash and the free CSE of §3
intact.

Both directions that need provenance resolve by walking the ref tree:

- **Diagnostics.** A diagnostic names a value id. Rendering the open file walks that file's ref tree
  and tests each `value_id` against the diagnostic set, `O(nodes in the file)`. Every occurrence of a
  broken fragment is marked.
- **State attribution.** Field -> signal -> value -> ref tree -> byte range, which is what §8.1's
  source-proximity pairing consumes. Where a memo hit gives one shared state field several source
  locations, the attribution is the first in deterministic order (file resolution index, then byte
  offset) — a UI affordance and a migration heuristic.

**Identity is the interned id.** The `uint32_t` value id *is* the identity and id equality is exact by
construction; the hash serves the intern table. So: a 64-bit hash with a fixed seed, since §5.5 wants
reproducible builds, and structural comparison on an intern-table bucket hit. That comparison is
`O(arity)` rather than `O(subtree)`, because children are already ids and it compares a fixed-size
tuple. This is what makes §5.2's diff exact.

**The shape hash of §8.1 stays a hash.** It is many-to-one by design — §8.1 pairs same-shape fields by
source proximity — so it is a plain 64-bit hash with no interned counterpart. The field population per
program is small, and a collision resolves to a delay line migrated from a same-shaped neighbour,
which §8.1's length-mismatch rule reduces to a transient click.

### 4.7 Precedence, and where parentheses come from

§4.5 drops grouping parens, so the printer re-derives them, and that derivation is what §4.4's third
property means by "canonical".

**The grammar is layered, and the layers do more work than the precedence table.** Four nonterminals
nest, each admitting strictly less than the last:

| Level | Nonterminal | Admits | Reached from |
|---|---|---|---|
| 1 | `expression` | `with`, `letrec`, the five compositions, and everything below | definition bodies, rule right-hand sides, group/iteration/`inputs`/`outputs` bodies, `<equation>`/`<diagram>`, and `( … )` |
| 2 | `argument` | `:`, `<:`, `:>`, `~` and everything below — **not** `,`, `with` or `letrec` (`:654-660`) | every comma-separated position: `arglist`, iteration counts, widget bounds, `route` arities, soundfile channels, modulator values |
| 3 | `infixexp` | the eighteen infix operators, application, `.`, `'`, `[defs]` | operands of any of those |
| 4 | `primitive` | leaves, and `( expression )`, which re-enters level 1 (`:602`) | operand of `.`, callee of an application |

Level 2 is the one the corpus exercises constantly: `,` is the argument separator, so a parallel
composition passed as an argument is parenthesized. The adopted grammar reaches the same restriction
by aliasing a separate `_argument` rule set (`grammar.js:155-171`).

**Within a level, precedence and associativity are the reference's, lowest first**
(`parser/faustparser.y:114-134`, `:204`, `:208`):

| Prec | Operators | Assoc | Term |
|---|---|---|---|
| 1 | `with` | left | `With` |
| 2 | `letrec` | left | `LetRec` |
| 3 | `<:` `:>` `+>` | right | `Split`, `Merge` |
| 4 | `:` | right | `Seq` |
| 5 | `,` | right | `Par` |
| 6 | `~` | left | `Rec` |
| 7 | `<` `<=` `>` `>=` `==` `!=` | left | `BinOp` |
| 8 | `+` `-` `\|` | left | `BinOp` |
| 9 | `*` `/` `%` `&` `xor` `<<` `>>` | left | `BinOp` |
| 10 | `^` | left | `BinOp` |
| 11 | `@` | left | `BinOp` |
| 12 | `'` | postfix | `Delay1` |
| 13 | `.` | left | `Access` |
| 14 | `(` — application | left | `Apply` |
| 15 | `[` — `expr[defs]` | left | `ModifLocalDef` |

The adopted grammar's `PREC` block (`grammar.js:1-20`) reproduces every level and associativity —
two independent derivations agreeing.

**The rule.** Printing child `C` at side `S` of parent `P`, parenthesize when `prec(C) < prec(P)`;
when `prec(C) == prec(P)` and `S` is the side associativity does not favour — the left of a
right-associative operator, the right of a left-associative one; or when `C` is `Par`, `With` or
`LetRec` in a level-2 position. Otherwise not.

Canonical is exactly that, and nowhere else. A canonical subtree splices back byte-identically, and
one carrying redundant parens converges in a single reprint (§4.4). `print_in_context` is this rule
plus the byte-range replacement, taking (`prec(P)`, `S`) and the position's level.

### 4.8 The Term node table

Every node carries §4.6's interned-value fields; every *statement* also carries `variants`, §4.5's
precision bitmask.

**Statements.**

| Node | Children | Payload | Prints as |
|---|---|---|---|
| `Import` | — | spec lexeme | `import("spec");` |
| `Declare` | value: string | key lexeme | `declare key "v";` |
| `DeclareDef` | value: string | name, key lexemes | `declare name key "v";` |
| `Definition` | clauses: `Clause`* | name lexeme | one line per clause |
| `Clause` | params: expr* (level 2, may be empty), body: expr | — | `name(p, q) = body;` / `name = body;` |
| `RecDef` | body: expr | name lexeme | `'name = body;` |
| `MdocBlock` | parts: mdoc-part* | — | `<mdoc>…</mdoc>` |
| `MdocProse` | — | text | verbatim |
| `MdocEquation`, `MdocDiagram` | expr | — | `<equation>e</equation>` |
| `MdocMetadata` | — | name lexeme | `<metadata>name</metadata>` |
| `MdocListing` | — | three booleans | `<listing k="v" … />` |
| `MdocNotice` | — | — | `<notice/>` |

`Clause` params are an `arglist` — patterns, not identifiers (`:475`) — which is what makes several
clauses of one name a pattern-matching definition. `RecDef` is separate: its name is primed and it
takes no parameters (`:456-464`).

**Expressions.**

| Node | Children | Payload | Prec |
|---|---|---|---|
| `Seq`, `Par`, `Split`, `Merge`, `Rec` | lhs, rhs | `Merge` carries the `:>`/`+>` tag | 4, 5, 3, 3, 6 |
| `BinOp` | lhs, rhs | operator, incl. the `^`/`pow` tag | 7–11 |
| `Delay1` | expr | — | 12 |
| `NegIdent` | — | name lexeme | primitive (`SUB ident`, `:600`) |
| `With` | expr, defs: stmt* | — | 1 |
| `LetRec` | expr, recdefs: `RecDef`*, wheredefs: stmt* | — | 2 |
| `ModifLocalDef` | expr, defs: stmt* | — | 15 |
| `Apply` | fn, args: expr* (level 2) | — | 14 |
| `Access` | expr, — | name lexeme | 13 |
| `Lambda` | body | param name lexemes | primitive |
| `Case` | rules: `Rule`* | — | primitive |
| `Rule` | patterns: expr* (level 2), body: expr | — | — |
| `Modulation` | entries: `Modulator`*, expr | — | primitive |
| `Modulator` | value: expr (level 2, optional) | name string | — |
| `Iterate` | count: expr (level 2), body: expr | kind ∈ `{par, seq, sum, prod}`, var lexeme | primitive |
| `Inputs`, `Outputs` | expr | — | primitive |
| `Environment` | stmts: stmt* | — | primitive |
| `Component`, `Library` | — | path string | primitive |
| `Waveform` | — | number lexemes | primitive |
| `Route` | ins, outs (level 2), entries: expr (optional) | — | primitive |
| `Hole` | children (§4.5) | source bytes | verbatim |

`Lambda` parameters are plain identifiers (`params`, `:466-469`), unlike `Clause`'s. `Route`'s third
child is absent in the two-argument spelling, the distinction §4.5 keeps against the fake route.

**Leaves.**

| Node | Payload |
|---|---|
| `Int`, `Real` | the literal lexeme verbatim, sign and all (§4.5) |
| `Ident` | lexeme, `::`-qualification included |
| `Prim` | which primitive — the nullary set of §4.5, each infix operator used bare, `mem`, `prefix`, `int`, `float`, the math functions, `rdtable`, `rwtable`, `select2`, `select3`, `attach`, `enable`, `control`, `assertbounds`, `lowest`, `highest`, `_`, `!` |
| `Button`, `Checkbox` | label string |
| `NumericWidget` | kind ∈ `{vslider, hslider, nentry}`, label; children `init`, `min`, `max`, `step` at level 2 |
| `Bargraph` | kind ∈ `{vbargraph, hbargraph}`, label; children `min`, `max` at level 2 |
| `Group` | kind ∈ `{vgroup, hgroup, tgroup}`, label; child expr at level 1 |
| `Soundfile` | label; child channel count at level 2 |
| `FFun` | one to four `\|`-separated names, return type, argument types incl. `any`, include file, library string |
| `FConst`, `FVar` | type, name, include file |

---

## 5. Incrementality

### 5.1 What tree-sitter is and is not for

tree-sitter is the right parser, but incremental reparse is not why. Parsing a Faust file from
scratch is nowhere near the bottleneck, and a reparsed tree gives changed ranges — not a semantic
diff, and not stable node identity across edits.

It earns its place for two other things, both mandatory here:

- **Error recovery.** A live editor parses syntactically incomplete text on every keystroke and must
  still produce a usable tree.
- **Trivia preservation.** Comments and whitespace are retained, which is what makes §4's splices
  lossless.

**Those two are also why the printer is a separate artifact rather than a derived one.** Invertible
syntax descriptions generate a parser and a printer from one description (Rendel & Ostermann, 2010),
making §4.4's PutGet true by construction instead of by property test — but they generate parser
*combinators*, which have neither property above. The trade is error recovery against derivability,
and a live editor takes error recovery. The shared specification is `faustparser.y`: `grammar.js` and
the printer are two derivations from it, which is what lets §4.7 check one against the other.

**The grammar is adopted.** `khiner/tree-sitter-faust` is published and vendors as a
submodule under `lib/`. Measured, it parses **296/296 of `lib/faust/examples`** and 319/341 of
`lib/faust/tests`, where the 22 remaining files are correct rejections: twenty-one use multirate
syntax (`vectorize`, `serialize`, `#`, `[](0)`) that appears nowhere in `faustlexer.l` or
`faustparser.y`, so reference Faust 2.85.9 rejects them too — residue of an abandoned branch — and the
twenty-second is `error-tests/error24.dsp`, which is `process = :`.

It covers the corners this design leans on — the `+>` alias, `expr[defs]`, `[a -> b]`, `any`, the
precision prefixes, `declare name key value`, `::`-qualified names, `letrec … where`, both `route`
arities, and `<mdoc>` with its nested `<equation>`/`<diagram>`/`<metadata>` — and keeps comments in
`extras`, which is the trivia requirement above. It is pure `grammar.js`: each of flex's four start
conditions is a distinct parse context, mdoc prose is a `/[^<]+/` token, strings are `"[^"]*"`
(`faustlexer.l:235`), and block comments terminate at the first `*/`.

Three things remain:

- **CST fidelity** — `text(cst(source)) == source` byte for byte over both corpora (§4.4). This, not
  the absence of an `ERROR` node, is what §4's splices rest on.
- **Error-recovery quality on half-typed input**, which §9 leans on and a highlighting-oriented
  grammar has never been graded on. The corpus is every corpus file truncated at a random byte, and
  §4.5's `Hole` makes the grade concrete: **a hole's extent must stay inside the `;`-delimited
  statement containing the truncation**, so an error in one statement cannot turn a later complete one
  into a hole. Against `;` rather than the statement list because a *missing* `;` legitimately fuses
  two statements — that fusion is the recovery working. Failures need an error-recovery rule in the
  grammar, so they are phase-1 work.
- **Node-granularity audit against §4.5** — that every Term node has a CST node to hang a ref on.
  Run against the published grammar, it returns one gap and four facts to write the lowering around:

  - **`<equation>` and `<diagram>` hold prose, not expressions** — `repeat($._doc_char)` with
    `_doc_char` as `/[^<]+/` (`grammar.js:403-404`), where the reference parses a real `expression`
    between the tags (`faustparser.y:423-427`). §4.8 needs an expression child, so this is the
    grammar change phase 1 makes.
  - **Grouping parens are anonymous** — `seq('(', $._expression, ')')` sits inline in the hidden
    `_primitive` rule (`grammar.js:120`), so a parenthesized expression has no node and a ref's range
    stops inside the parens. That suits Term, and §4.4's `print_in_context` is what splices under it.
  - **The composition operator is an unnamed token.** `define_binary_comp` fields only `left` and
    `right` (`grammar.js:24-25`), so §4.5's merge tag comes from the operator's text. The `^`/`pow`
    tag does not: `^` is the named `$.pow` and bare `pow` an anonymous keyword in `_prim2`, so those
    two differ by node type already.
  - **Numeric literals carry their sign in the token** (`grammar.js:386-398`), where the reference
    reaches a negative through `SUB INT` (`faustparser.y:525-526`), and `- 2` with a space goes
    through the separate `unary_number` rule. All three lower to one `Int`/`Real` whose payload is
    the verbatim span — what §4.5's literal tag already asks for.
  - **`with` accepts a statement list.** `environment: seq('{', repeat($._statement), '}')` serves
    both `with` and `environment{}` (`grammar.js:190`), where the reference gives `with` a `deflist`
    of definitions only. The extra admissions are caught by evaluation rather than by the parse.

### 5.2 The mechanism is Merkle hashing

Hash the Term value graph bottom-up: every value carries the hash of its subtree. Intern values by
hash, so structurally identical terms are the same object and the same id (§4.6). On an edit, hashes
are recomputed only along the path from the changed node to the root.

Evaluation is memoized on **(value id, environment id)**. Id equality *is* the diff — exact by
construction, since interning resolves the hash to an id where the value is built (§4.6) — so no
tree-diffing algorithm is needed anywhere.

The environment component is easy to miss and not optional: the same definition evaluates differently
under different `with` scopes and parameter bindings, so environments are interned and hashed too.
This is why the reference hash-conses its `Tree` layer, and the same discipline applies here at the
Term and environment level.

**The environment hash must be O(1) — a constraint on its representation, not an optimization.**
Environments are persistent: an extension is a new interned node holding `(name, value, parent)`,
hashed once at construction from the child hash and the parent's. That keeps a memo probe `O(1)`
against an environment whose large case is the standard library's — the case this mechanism exists
to make fast.

The job it does is **the standard library**. `import("stdfaust.lib")` drags in a large body of Faust,
and this cache is what keeps re-evaluating it off the per-keystroke path — work for the *evaluator's*
cache, not the parser's.

### 5.3 Downstream falls out for free

Because Signal is hash-consed, re-propagating a changed definition produces identical node ids for
every unchanged subtree. Type and interval results memoize per node id, so only genuinely new nodes
are analyzed — minimal recomputation through the middle of the compiler, with no diffing logic
written.

### 5.4 Where it stops, and why that is fine

Codegen is whole-function. The compute loop is one block in a topological order over a shared state
layout, so one changed node means a new function. Every scheme to avoid this is worse than the
problem: compiling each definition into a separately relinkable unit with its own state would
destroy global CSE, shared delay lines and loop fusion, which is most of what makes Faust fast.

The answer is not to make codegen incremental but to make it **cheap**. Emitting bytecode is a
topological sort plus linear instruction emission over a flat array; regenerating all of it per
keystroke is not a problem. LLVM is the only expensive stage, and it runs only for a release build.
Which is why the two-tier backend (§6.4, §6.5) matters more to the edit loop than incrementality
does.

### 5.5 Determinism

Anything whose iteration order can affect output must be deterministic — sorted or
insertion-ordered, never raw hash-map order. Two reasons: reproducible builds, and §11.5's
incrementality property, which asserts an incremental result is *identical* to a from-scratch
compile. Non-determinism turns that invariant into flaky noise.

### 5.6 File inputs and library resolution

A Faust program is not one file. `import("stdfaust.lib")` opens nearly every one, so file reading
sits on the critical path of every compile and belongs inside the incremental model, not underneath
it.

**Files are input queries.** `file_text(path)` is a *source* in the query engine's sense: no
dependencies, a revision, and everything downstream depending on it. Bumping a file's revision
invalidates exactly its dependents, which is what carries an edit to a library through to every
program importing it (§5.2).

Resolution is itself a query, `resolve(spec, importing_file)`, for two reasons: the search path can
change at runtime, and a *failed* resolution must be retried when the missing file appears. Caching
a failure permanently is the obvious bug here.

**Resolution is an overlay, in this order:**

1. **Open editor buffers.** An unsaved buffer shadows everything below it. This is what makes live
   editing of a library work at all — without it, `.lib` edits take effect only on save.
2. **The importing file's own directory.** Easy to omit, and the corpus does not survive omitting it
   (§11.4). The reference arrives here by side effect: `fopenSearch` (`parser/enrobage.cpp:329-347`)
   opens the path against the process's working directory and then **pushes that file's dirname onto
   the global import list**, so loading `dsp/echo.dsp` is what makes `dsp/music.lib` findable. No `-I`
   is involved.
3. **Search path on disk.** The session's workspace, then any configured directories.
4. **The embedded standard library.** `faustlibraries` is compiled into the binary.

**Layer 2 is per-import, not accumulated.** The reference's list grows as files load, so a program
can resolve an import only because an unrelated earlier one widened the path — order-dependent, and
so against §5.5. Taking the *importing* file's own directory is deterministic and resolves the
corpus identically. A program that needs the accumulating behaviour gets a resolution diagnostic and
gets recorded, which beats an order-dependent compiler.

Embedding the standard library removes install steps and version skew, lets the environment run with
no filesystem at all (relevant to a browser build), and guarantees the version we test against ships.
The overlay keeps it from being a cage: to modify a stdlib file the user **ejects** it, copying it
into the workspace where it shadows the embedded copy and becomes editable.

Import cycles are detected and reported as a diagnostic on the offending `import`; §5.8 gives the
mechanism, which is not the same one §9 uses for evaluation loops. Remote imports
over HTTP, which the reference supports, are out of scope.

### 5.7 Working across files

The session holds a set of open buffers. The box view renders the **root** file — the one defining
`process` — and imported definitions are navigable: opening one shows that file's Term graph in the
same editable view, since nothing about §4 is specific to the root file.

Editing any open buffer bumps its revision (§5.6) and recompiles whatever depends on it, which for a
stdlib file may be the entire program and for a leaf file nothing at all. The machinery is identical
either way; only the size of the invalidated set differs.

### 5.8 The query engine

§15 names incremental-equivalence bugs as a risk and §11.5 property 2 as the defence. Both are
statements about this component, so it is specified rather than left as "a memoized query engine".

**The engine spans only the path-addressed layers, and that is why it is small.** A query is needed
exactly where a fact is named by something other than its own content — a path. Everything below
Term is *content-addressed*: `eval` keyed on `(value id, environment id)`, propagation on box ids,
analyses on signal ids, Plan on its root signal ids plus the build configuration. Those memos need
no invalidation at all, since an edit produces different ids and therefore different keys — a stale
entry is unreachable rather than wrong. So the invalidating engine covers five query kinds and
stops:

| Query | Kind | Depends on |
|---|---|---|
| `file_text(path)` | input | — |
| `vfs_revision()` | input | — |
| `resolve(spec, importer)` | derived | `vfs_revision`, and the buffer set and search path it reads |
| `cst(path)` | derived | `file_text(path)` |
| `terms(path)` | derived | `cst(path)` |
| `file_env(path)` | derived | `terms(path)`, `resolve(...)`, `file_env(...)` of each import |

`vfs_revision` is one global input bumped when a buffer opens or closes, the search path changes, or
a watcher reports a create or delete. Making `resolve` depend on it retries a *failed* resolution
exactly when something could have changed, which is how §5.6's permanent-failure-caching bug is
avoided without per-path negative caching.

**`import`, `component` and `library` are resolved by this layer, not by the evaluator, and the
content-addressed claim above depends on it.** All three name a *path*, and resolution belongs with the
other path-addressed queries so that its results carry revisions. `file_env(path)` resolves every
spec in the file and the environment it
produces carries an interned **resolution map**, `{spec -> file_env id}`, alongside its bindings.
The evaluator reads the map and never touches the VFS. Editing a library changes its `file_env` id,
hence the map, hence the environment id, hence the memo key — invalidation by key change. The map is
interned once per file, so §5.2's O(1) environment hash is unaffected.

**Mechanism: revision stamping with early cutoff.** A global monotonic revision counter, bumped by
any input write. Each cache entry holds `{result, changed_at, verified_at, deps}`. `get(q)`:

1. If `verified_at == current revision`, return the result.
2. Otherwise `get` each dep. If no dep's `changed_at` exceeds this entry's `verified_at`, set
   `verified_at = current revision` and **return the cached result without recomputing**.
3. Otherwise recompute, setting `verified_at` to the current revision either way, and `changed_at`
   only if the new result differs from the old.

That is around two hundred lines. Step 3's equality check is the early cutoff, and interning is what
makes it free: every derived result is an interned id or a small per-file structure, so "equal" is
integer comparison (§4.6). A whitespace edit changes `file_text` and `cst`, re-derives the same
`terms` value ids, and `file_env` never recomputes — nothing below the query layer runs at all. That
is §8.2's "when the new Plan hashes equal to the old, skip the swap", reached several stages earlier
and for free.

**Two cycle detectors at two layers, deliberately not unified.** An import cycle is a query cycle:
an in-flight stack detects re-entry, the re-entrant call returns an empty environment plus §5.6's
diagnostic on the offending `import`, and **no query on the cycle is cached** — its entry is
volatile and recomputed each revision. A cycle's result depends on which file was queried first, so
leaving it uncached is what keeps output independent of entry order (§5.5). Cycles are an error and
are rare, so recomputing costs nothing. The other detector is §9's evaluation loop
guard, in the content-addressed `eval` memo, marking `(value id, environment id)` keys in flight.
One is about paths, the other about values.

**Single-threaded, with cancellation only at phase boundaries.** §8.2 gives the compile thread sole
ownership, so the engine needs no locking. Edits arrive on a queue; the thread drains it, bumps
revisions, recompiles. A pending edit is checked *between* phases — evaluate, propagate, analyze,
lower — and never inside a query, because §5.4 already makes a full recompile cheap and in-query
cancellation leaves partial results that must not be cached under the current revision.

**Determinism.** Dep lists are recorded in call order and re-recorded on recompute. No iteration
over a hash map reaches a result (§5.5).

### 5.9 Lifetimes and memory

Interning is append-only by nature and the environment runs for hours, so what is never reclaimed
has to be a decision rather than an accident. There are two lifetimes, split along the same boundary
§5.8 draws.

**Permanent — process lifetime, append-only, ids never move:** interned strings, `TermValue`s,
environments. These are the id spaces quoted by things that outlive a compile: diagnostics name
value ids (§9), `TermRef` entries hold them (§4.6), and every content-addressed memo key is built
from them. Growth is bounded by *distinct terms ever typed*, `O(depth)` values per keystroke, so a
long session adds tens of megabytes on top of the one-time cost of interning the standard library
(56 files, 2.4 MB). Not collected: §8.1's state identity depends on ids staying put, which is what
an append-only intern table provides.

**Droppable arenas — rebuildable caches whose ids are valid only within one generation:**

- the **eval arena** — Box nodes and the `(value id, environment id) -> box id` memo;
- the **compile arena** — Signal nodes, per-node type and interval results, the max-delay map, the
  `signal id -> value id` side table (§4.6), and Plan.

Each is dropped and rebuilt whole above a byte budget, **on idle only** — never with an edit pending
— so the cost never lands on a keystroke. A drop costs one cold compile including a cold
standard-library evaluation, which is why the two drop independently: reclaiming signal memory
should not throw away the eval cache §5.2 exists to fill.

**One invariant makes this safe: nothing outside an arena holds an arena id.** Query results hold
text, CSTs, Term value ids, ref trees and environment ids. Diagnostics hold value ids. The running
`Instance` holds a state block keyed by §8.1's exact and shape **hashes**, not ids, so state
migration survives a generation boundary untouched. "Query" versus "content-addressed memo" turns
out to be the same line as "permanent" versus "droppable".

Budgets can be generous, because the graphs are small: the 94 reference `.sig` files run one signal
node per line, median 52 against 6337 for the largest, `reverb_designer`. An edit invalidating a
graph that size costs a few hundred kilobytes.

---

## 6. Compilation

### 6.1 Evaluation and propagation

Evaluation is a memoized recursive traversal of Term producing Box: desugaring the infix and postfix
forms (§4.1), beta-reduction, pattern matching, `with`/`letrec`/`environment` scoping, iteration
unrolling, metadata collection, label substitution (§7.2). Box carries arity, checked at
construction, so the language's most common error — mismatched counts — is caught with a precise
source range.

**`Error` has unconstrained arity, and composing anything with it yields `Error`.** §4.5's `Hole`
evaluates to `Error`, and arity is checked at construction everywhere else. So `Error` satisfies
whatever count is asked of it and absorbs its neighbours, keeping the diagnostic set the size of the
mistake — §9's locality rule, enforced where it is cheapest.

File-level `declare` is namespaced by its file: bare in the root, keyed `<file>/<key>` in any import
(§11.1 gives the spelling), with a missing root `declare name` synthesized from the basename.

**The observability boundary is `.box` plus accept/reject.** Two evaluators agreeing on `.box`
isomorphism for every program, and on which programs are rejected, are indistinguishable. That turns
"which of the reference's behaviour is semantics and which is 2005" from a judgement into a test:
write a probe `.dsp`, diff the `.box`. This section is built on that rule, and it is why evaluation
can be designed rather than transcribed.

**The model, which is ours.** A closure is a pair `(term id, environment id)`, and since
environments are interned nodes hashed in O(1) (§5.2), that is two integers and closure equality is
integer equality. Evaluation is strict and recursive, memoized on `(value id, environment id)`;
marking a key in-flight is both the loop detector and the memo probe (§9). The reference instead
threads a four-part closure with its own `visited` set through every call, unioning on entry — the
same guarantee, bought again.

**Desugaring, also ours, because Term kept the surface forms (§4.1).** One rule per surface-only node
of §4.5, applied on the way to Box:

| Term | Box |
|---|---|
| `BinOp(op, l, r)` | `Seq(Par(l, r), Prim(op))` |
| `Delay1(x)` | `Seq(x, Prim(mem))` |
| `Neg(x)` | `Seq(Par(Int 0, x), Prim(-))`, or a negative literal when `x` is one (§4.5) |
| `Definition(name, clauses)` | body, lambda, or `case` — `makeDefinition`'s four shapes (§4.5) |
| `LetRec(body, recdefs, wheredefs)` | a `with` holding a recursive body definition and one projection per name |
| `Route(i, o, ∅)` | `Route(i, o, (0, 0))` |
| `Environment(stmts)` | a closure over the statements' environment |

`letrec` is the one that looks like a scoping rule and is not: `boxWithRecDef`
(`boxes/boxes.cpp:520-532`) rewrites it into `with` plus `~` projections outright, so the evaluator
never sees a `letrec` form. Its visibility rule falls out of that rewrite, asymmetrically — the
letrec-bound names escape, the `where` definitions do not.

**Semantics to match, each with a probe.** These are what `.box` can see, so each is pinned here and
gets a purpose-built `.dsp` the way §11.3's do:

- **`component(f)` and `library(f)` evaluate in a fresh, empty environment** (`eval.cpp:463`, `:471`)
  — neither sees the importing scope. `component` evaluates that file's `process`, `library` yields
  the file as an environment value. The distinction is visible only in programs that shadow a stdlib
  name, which is what the probe targets. Both reach the file through
  §5.8's resolution map rather than through the VFS, which is what keeps evaluation a pure function of
  interned ids.
- **`.` resolves in the captured environment, not the current one** (`:435-452`). Lexical, and the
  reason `environment{}` and `library()` are usable as first-class values at all.
- **`with` sees the enclosing scope**, and `import` inside a `with` block is expanded before the
  block is pushed (`:565-567`).
- **Patterns are evaluated; right-hand sides are not** (`evalRule`). Rule bodies stay unevaluated
  until a match selects one.
- **Pattern matching is incremental over applied arguments**, so a `case` may be partially applied
  and matching resumes as further arguments arrive.
- **Evaluation constant-folds numeric tuples through primitives**: `2, 3 : +` evaluates to `5`,
  propagated and simplified mid-evaluation (`:395-418`). So Box is *not* purely structural — easy to
  miss, and visible directly in `.box`.
- **The entry point is a name, not a keyword.** `process` is `gProcessName`, settable by `-pn`.

**What we do not copy.** The pattern-matching automaton of `patternmatcher/` — rule *selection* is
semantics, the automaton is one way to get it, and direct structural matching over interned terms is
another. `Tree` as a universal representation. Parse-time desugaring, which §4.5 shows is actively
harmful here. The `gLoopDetector(1024, 400)` constants, since §9 bounds divergence its own way.

**Three the papers leave open, read out of the source.** Each keeps its phase-2 `.box` probe, which
now confirms rather than decides.

- **Rule priority is first-viable in source order.** `make_pattern_matcher` numbers rules `0..n-1`,
  un-reversing the list the parser hands it (`patternmatcher/patternmatcher.cpp:588-601`); a state's
  rule list stays sorted ascending (`:136-137`, `:457-461`); and at a final state
  `apply_pattern_matcher` returns the first rule still viable (`:844-857`).
- **Viability is where non-linear patterns are decided.** A variable appearing twice binds twice, and
  if the two subtrees differ that rule's environment is marked `boxError()` and skipped, leaving the
  rules below it to match (`:812-830`). So `f(x, x) = …;` matches only structurally equal arguments.
  Construction separately warns when an earlier rule matches a later rule's left-hand side
  (`:640-663`), reproduced as a §9.1 diagnostic.
- **`letrec` shadows like any `with`, because it becomes one.** `boxWithRecDef`
  (`boxes/boxes.cpp:483-532`) rewrites `e letrec { 'x = ex; 'y = ey; where W }` into
  `e with { B = \(x, y).((ex, ey) with W) ~ bus(2); x = B : select(2,0); y = B : select(2,1); }`
  before evaluation, `B` being a reserved internal name. The projections land in the `with` layer, so
  the letrec names shadow an enclosing `with` by the ordinary rule — `pushMultiClosureDefs` pushes a
  layer, `evalIdDef` walks outward (`evaluate/eval.cpp:1421-1432`) — while the `where` definitions
  sit inside the abstraction body, which is the visibility asymmetry above.
- **A nested `environment` captures its enclosing one, by being a closure.** `boxEnvironment`
  evaluates to `closure(exp, nil, visited, localValEnv)` over the current environment
  (`eval.cpp:576-578`), and `environment{ defs }` is `boxWithLocalDef(boxEnvironment(), defs)`
  (`faustparser.y:619`), so the defs layer sits on whatever was in scope. `.` then evaluates its name
  in the captured environment (`eval.cpp:435-442`). Nesting needs no rule of its own.

Two facts fall out of the same reading. **Redefining a name within one layer is an error** —
`addLayerDef` throws unless the definitions are identical (`evaluate/environment.cpp:84-100`) — so it
belongs to §11.4's accept/reject comparison. And **the environment barrier is narrow**:
`pushEnvBarrier` stops `searchIdDef`, which only the pattern matcher calls, and `evalIdDef`'s
ordinary lookup walks past it (`environment.cpp:62-76`, `:154-168`).

Propagation walks Box, threading input signals through the composition operators, producing Signal.

`inputs(e)` and `outputs(e)` report a box's arity as a number, so evaluation must be able to ask for
arity without propagating; arity-at-construction already provides it. They appear in the standard
library, so they are reachable from the first `import`.

### 6.2 Recursion in a flat hash-consed DAG

`~` introduces cycles, and cycles cannot be hash-consed bottom-up. Settle it up front rather than
discover it.

Represent a recursive group as a `Rec` node owning N body node ids, with `Proj(rec, i)` nodes for
its outputs; the body may reference projections of its own group. Build it in two phases: reserve
the `Rec` id, build the body against that reserved id, then hash the group *as a unit*, with
self-references replaced by a canonical positional marker. The group hashes and interns like any
other node; only its construction is special.

**`Proj(rec, i)` reads output `i` at the current sample.** Folding the feedback delay into `Proj` is
tempting and wrong: `A ~ B` returns A's outputs *undelayed*, and the one-sample delay sits only on
the path back into the body. The reference makes both explicit — group outputs are
`sigDelay0(sigProj(p, g))`, in-body feedback is `sigDelay1(sigProj(i, ref))`
(`propagate/propagate.cpp:715-724` and `:339-346`). Keeping the two separate is what puts recursive
outputs on the current sample, which is nearly every audio path in the language.

So the delay stays an ordinary node, and causality comes from the rule that builds the group rather
than from the representation: `~` propagation inserts the `mem` itself, and `letrec`'s mandatory
prime is the surface equivalent. It becomes a construction invariant — **a `Rec` body may reference
its own projections only through a delay of at least one** — checked where the group is built.
Matching the reference here also means `.sig` comparison needs no recursion normalization at all,
beyond reading its explicit `@0` as identity.

One more rule for that comparison: a body branch with no self-reference is emitted directly rather
than as a projection (`propagate.cpp:715-724`), so a group mixing recursive and non-recursive
branches has fewer projections than outputs.

### 6.3 Analyses, then Plan

Over the Signal DAG, memoized per node id:

- **Type inference** — nature (int/real) and variability (constant / block-rate / sample-rate), a
  fixpoint over `Rec` groups starting from bottom. The promotion rules are not C's and are pinned in
  §11.3 — most importantly, division always yields a float even for two integer operands.

  Faust's type carries three further dimensions, deliberately absent here. *Computability* and
  *vectorability* serve codegen modes this design does not have (§10). *Boolean-ness* is safer to
  drop than it looks: the type rules never consult it, and its only readers
  (`extended/maxprim.hh:141-142`, `extended/minprim.hh:141-142`) sit inside `generateCode(Klass*,
  ...)`, the deprecated `ocpp` backend, where all it buys is an `int(...)` around an already-int
  operand. Nature plus an interval of `[0,1]` carries what downstream needs.
- **Interval inference** — bounds per node. Kept because it is load-bearing, not for completeness: it
  sizes delay lines, decides which table and soundfile accesses need clamping, and rejects programs
  whose delays cannot be bounded (§10 explains the reduced scope). `assertbounds`, `lowest` and
  `highest` are the user's overrides and feed straight into it — and they matter *more* here than in
  the reference, being the escape hatch when a deliberately weaker engine sizes something pessimally.

  Two of those consumers make interval precision *observable* rather than a quality knob, which
  bounds how lazy the rules may be. A delay index whose interval is unbounded or possibly negative is
  a hard error in the reference, so a weaker engine rejects programs reference Faust accepts —
  though not where it looks. The causality check at `signals/sigtyperules.cpp:513-542` is dead code,
  gated on a `gCausality` that `global.cpp:546` sets `false` and nothing sets back. The live check is
  `checkDelayInterval` (`signals/sigtype.cpp:336-347`), called unconditionally from the max-delay
  pass below, so the rejection belongs to lowering rather than typing.
  And a clamp is inserted only where the interval fails to prove
  the access in range (§11.3), so a weaker engine adds a `min` and a `max` per table access inside
  the sample loop and changes the very graph §11.1 compares. Both say the same thing: widen anywhere
  *except* on the arithmetic that reaches a delay index or a table index.

  **That subset is computed, not assumed — it is the backward slice from every index position.**
  Nothing bounds it syntactically; `_@(int(100 * sin(x)))` puts a transcendental on the slice. But it
  is measurable. Slicing back from every `@`, table and soundfile index across the 93 reference
  `.sig` files gives, by program count out of 94:

  | Reach | Operations |
  |---|---|
  | 20+ programs | `int` `+` `*` `max` `min` `@` `float` `letrec`/`proj` `hslider` |
  | 13–19 | `-` `/` `floor` `&` |
  | under 10 | `%` `<=` `pow` `>` `exp` `fmod` `select2` `sigRDTbl` `sin` `vslider` `abs` `nentry` `<` `>=` `button` `cos` `length` `log` |

  So **thirteen operations cover the slice in nearly every program**, and they are the cheap ones —
  affine arithmetic, `min`/`max`, masking, truncation. Those get rules at least as tight as the
  reference's; everything else, the whole transcendental tail included, may widen to `⊤`. That is
  §10's cut made concrete.

  Three entries are to be designed against rather than skimmed. `letrec`/`proj` reaching 24 programs
  means **a recursive signal commonly feeds a delay index**, so the fixpoint must yield a usable
  bound rather than give up — the hard case, and not an exotic one. `sigRDTbl` means a table read can
  feed an index, so the rules compose across a table. And the comparisons plus `length` are the
  *clamps' own* arithmetic: §11.3's `max(0, min(i, length-1))` puts a soundfile length and a
  comparison on the slice, so the rules have to survive being applied to their own output.

  **Widening is safe because it degrades to a question, not a wrong answer.** It can only add an
  unneeded clamp or fail `checkDelayInterval`; it cannot silently under-size a delay line. That
  failure is a diagnostic naming `assertbounds`, which is why §10 calls it an escape hatch. So ship
  the thirteen, let the corpus find the rest, and add rules on demand with a test attached to each.

There is deliberately **no occurrence or sharing analysis**, with one carve-out. The reference needs
one because it compiles signals into nested C++ *expressions* and must decide whether to emit
`float fTemp0 = ...` or inline. Under a three-address Plan there are no nested expressions and every
node already has a virtual register, so that consumer disappears. If interpreter register pressure
ever becomes real, the answer is liveness for register reuse, not occurrence counts — and one
register per node may never be worth improving on.

The carve-out is **maximum delay per signal**, which the reference computes in that same pass
(`generator/occurrences.cpp:150-186`, consumed at `generator/instructions_compiler.cpp:2080`) and
which is needed here regardless of how expressions are emitted. It is what makes `x'`, `x@3` and
`x@5` share *one* delay line sized to the largest use, rather than three lines holding three copies
of the same history. One traversal accumulating a `node id -> max delay` map — `int(hi + 0.5)` of
each delay index's interval, round-to-nearest rather than ceiling, and 1 for each `mem` and `prefix`
— not an analysis framework. It is also where an unbounded or negative delay index is rejected
(`generator/occurrences.cpp:169`), on the condition `isValid() && lo() >= 0 && hi() < INT_MAX`.

Between typing and lowering sits **promotion**, a graph rewrite that is easy to mistake for a typing
rule. Faust's implicit conversions are *materialized as cast nodes on the operands*: both sides of
`/` to float, both sides of the bitwise and shift operators to int, delay and table indices to int,
and so on. The graph that gets scheduled is not the graph propagation produced, and the promoted one
is what `.sig` records; §11.3 lists the placements. Promotion needs types and invalidates them, so it
runs as rewrite-then-retype — the reference does this twice around its simplifier
(`normalize/normalform.cpp:88-112`).

Lowering to Plan then does three things:

1. **Schedule by rate.** Partition nodes into init-time, control-rate (once per block) and
   sample-rate (inside the loop) from their variability. Most of the runtime win lives here, in
   hoisting slider arithmetic out of the sample loop.
2. **Allocate state.** One delay line per *delayed signal*, sized from that signal's maximum delay,
   in the reference's three cases (`instructions_compiler.cpp:2060-2102`, threshold
   `gMaxCopyDelay = 16` at `global.cpp:435`): a maximum delay of 0 gets no array at all, just a
   scalar; under 16 gets a copy-shifted register of `max + 1` slots; 16 or more gets a ring buffer of
   the next power of two at least `max + 1`. Match the constants, because §8.1 migrates on shape and
   an edit that crosses the threshold changes the representation rather than the length. (A fourth
   case, a non-power-of-two ring, sits behind `-dlt`, whose `gMaskDelayLineThreshold` defaults to
   `INT_MAX` at `global.cpp:644` and so never fires.) Since §6.2 keeps the feedback delay as an
   ordinary node, a recursive register is not a separate field class but the delay line of a
   projection, allocated by the same rule. Tables, UI-bound controls and guarded values (below) get
   fields, though the reference keeps read-only tables outside the instance entirely (§11.1). Every
   field gets a **stable identity** (§8).
3. **Emit three-address instructions** over virtual registers, in topological order, per rate band.

Plan is *mostly* straight-line. The exceptions need naming, so that both backends implement the
same thing:

- **Three bands.** `init` (runs once, at a known sample rate), `control` (once per `compute` call),
  `sample` (once per frame, inside the loop).
- **Bounded loops, in the init band only.** `rdtable` and `waveform` are filled by running a
  generator subgraph over `0..size-1`. This is the one place Plan contains a loop other than the
  frame loop, and it is always bounded by a compile-time constant. The generator is its own little
  program: it may contain delays and recursions, and their state belongs to the fill loop rather
  than to the instance, which is why the reference walks generator subtrees separately from the main
  graph (`generator/occurrences.cpp:178`). Note that `waveform` is a box of *two* outputs, its size
  as an int constant and the wave itself (`propagate/propagate.cpp:472-476`), so a bare
  `waveform{...}` feeding a one-input expression is an arity error rather than a wave.
- **Guards.** `enable` and `control` make a range of instructions conditional. Each instruction
  carries an optional guard register; the interpreter branches over the range, the LLVM emitter
  produces a basic block. Guards nest, so the representation is a range with a guard stack depth
  rather than a flag.

  **The two are not the same primitive, and the difference is audible.** Propagation rewrites both
  into a single guarded node, but `enable` also *scales*: `enable(x, c)` becomes
  `control(x * c, c != 0)` while `control(x, c)` becomes `control(x, c != 0)`
  (`propagate/propagate.cpp:534-553`). So `enable(osc, 0.5)` is half-amplitude, not merely computed.
  Both forms compare the condition against zero explicitly, and that comparison is a node `.sig`
  records. Treating `enable` as a bare guard drops the multiply and changes the output. (Both
  rewrites sit behind `gEnableFlag`, `true` by default at `global.cpp:647`; under `-es 0` the
  reference degrades `enable` to a bare multiply and `control` to identity. Not implemented, like
  `-strict-select` below.)

  **A guarded instruction writes to a state field, not to a register.** When the guard is false the
  value has to keep whatever it last computed, possibly from an earlier block, which a virtual
  register cannot do — the reference promotes exactly these temporaries to struct fields, cleared at
  init and carried across `compute` calls (`instructions_compiler.cpp:1199-1230`). The state field is
  what makes the two tiers agree here, LLVM's registers being SSA values, and §11.5 property 3 is the
  check on it.

  **The reference reaches the same result through a fourth band.** Writing the field directly is the
  `-ec` shape; by default (`gExtControl = false`, `global.cpp:507`) it instead loads the field into a
  stack temporary in *Compute control*, stores under the guard in *Compute DSP*, and copies back in
  *Post compute DSP* (`instructions_compiler.cpp:1228-1239`). Same semantics, three statements where
  we emit one, so §11.1's projection maps that load and that store back onto our single guarded
  store.

So the instruction set is three-address code plus `loop_begin`/`loop_end` and
`guard_begin`/`guard_end`. That is all of it, and it is what keeps both backends thin.

**`select2`/`select3` are plain instructions, not guards.** Faust evaluates *both* branches
unconditionally and then selects (`instructions_compiler.cpp:1978-1983`), so `select2(c, 1/x, 0)`
divides regardless of `c`. A frequent source of surprise, and it must be preserved: turning a select
into a branch changes which programs produce infinities and NaNs, and therefore the audio. The
reference's `-strict-select` option, which does emit a branch, is not implemented.

**Tables are values, and that is what orders reads against writes.** `wrtable` produces a table
value, `rdtable` consumes one, so a read of an `rwtable` *depends* on the write and topological
emission places the write first within the sample with no ordering rule of its own — exactly the
reference's semantics (§11.3). Table writes are placed by variability like everything else, so a
constant-rate write lands in the init band rather than the loop. Index clamping for tables and
soundfiles belongs here too, as a graph rewrite rather than backend code, which is what keeps the two
tiers and the `.sig` comparison honest; §11.3 gives the forms and the insertion condition.

**Delay lines initialize to zero, except under `prefix`.** `prefix(x, y)` yields `x` at time 0 and
`y` delayed by one sample thereafter — a one-sample delay with a caller-supplied initial value. Plan
carries that value on the state field, and §8.1's migration treats it as part of the field's shape.

**Dead code elimination is implicit, and `attach` is how it is controlled.** Emission walks backward
from the roots — `process`'s outputs — so anything unreachable is never emitted, and no DCE pass is
needed.

That makes the root set load-bearing. A `vbargraph` computes a value nobody reads, so naive
reachability drops it and the meter goes dead; `attach(x, y)` exists for exactly this. The rule:
**`attach` forwards `x` and widens the root set with `y`** — it computes nothing of its own, which is
literally all the reference does with it (`CS(y); return CS(x)`,
`generator/instructions_compiler.cpp:1520-1524`). Guard conditions need the same care, being
reachable through the instructions they guard rather than through any data edge.

### 6.4 The interpreter tier

A register machine over a flat register file plus the state block. Opcodes: load constant, read
input, binop, math call, int/float cast, read/write state, delay read/write, table read/write,
select, soundfile length/rate/read, foreign call, foreign variable read, and the loop/guard bracket
pairs of §6.3. A switch or computed-goto dispatch loop.

This exists so the edit loop never waits on LLVM. Reference Faust validates the approach, shipping
an interpreter backend (`compiler/generator/interpreter`, `tests/interp-tests`) for the same reason.

*If per-sample interpretation proves too slow for large programs*, the escalation is block-wise
interpretation: partition the graph into feedback-free regions that can run one instruction across a
whole buffer, leaving only cycles per-sample. A significant speedup and a significant complication —
build it when a measured program demands it, not before.

### 6.5 The LLVM tier

Emit LLVM IR directly from Plan; no intermediate framework. Faust's output shape is a loop around
straight-line float math, so LLVM's own optimizer does nearly all the work, including vectorizing
the feedback-free parts — which is why §2 drops explicit vector codegen modes.

**LLVM is an optional dependency.** An interpreter-only build has essentially none, keeping the edit
loop's build fast and a browser/wasm deployment cheap.

**Ahead-of-time export is the same artifact, written down**: an object file from this tier plus the
§7 descriptors, which are already data and need no serializer beyond the obvious one. Nothing in the
export path is a second code generator.

---

## 7. The runtime contract

Everything above produces a Plan. This section defines what a *running instance* is, because
"compiles correctly" and "makes sound you can control" are different claims — a program whose
sliders are frozen at their init values is not runnable in any useful sense.

A **compiled artifact** is a Plan plus four descriptors: the UI tree (§7.2), the soundfile
requirements (§7.3), the foreign symbol requirements (§7.4), and the file's `declare` metadata.
Descriptors are data, not code, and are shared unchanged by both backends.

### 7.1 Instance lifecycle and the block contract

```
create(artifact)                  allocate the state block; nothing computed yet
constants(sampleRate)             run the init band: rate-dependent constants, table fills
resetControls()                   UI fields to their declared init values
clear()                           delay lines, registers and guarded values to zero
init(sampleRate)                  all three, in that order
compute(frames, in[], out[])      run the control band once, then the sample band `frames` times
destroy()
```

Start-up splits three ways because its parts are needed separately, which the reference's `Init` /
`ResetUI` / `Clear` sections confirm. A sample-rate change needs `constants` alone; a live reload
(§8) needs `constants` plus a partial `clear` of whatever failed to migrate; `init` is the batch case
that runs all three. Only `constants` executes the init band — resetting controls and zeroing state
are not computations in it.

`compute` has no post-loop step, deliberately: §6.3's guarded instructions store straight to their
state fields, which is what the reference's *Post compute DSP* achieves by a longer route. Table
fills land in `constants` rather than the class-level entry point the reference uses (§11.1).

- **Sample rate** enters exclusively through `constants`. Faust's `fSamplingFreq` is not an external
  C symbol here (§7.4) — it is a runtime-provided value feeding the init band, which is what makes
  `ma.SR` work.
- **A sample-rate change is a re-`constants`, not a recompile.** `de.delay(SR/10, x)` looks like a
  rate-dependent allocation, but delay lines are sized from the *interval* of the delay index (§6.3),
  and the standard library clamps `ma.SR` to `[1, 192000]` — which is why `max(1.0f, fSamplingFreq)`
  and `min(1.92e+05f, ...)` open nearly every reference `.sig`. The line is sized for 192 kHz
  whatever the rate: compiling `process = _@(int(ma.SR/10));` allocates `fVec0[32768]` and puts the
  actual delay in a constant computed at init. Table sizes cannot depend on `SR` at all, since they
  must reduce to a literal. Nothing in the layout moves, so a rate change recomputes constants and
  refills tables while delay lines and registers stay put — no rebuild, no migration, no click.
- **Control rate is the host's block rate.** The control band runs once per `compute` call, so a
  slider takes effect at the next block boundary. This matches reference Faust exactly, including
  the consequence that control rate is `sampleRate / framesPerBlock` — a property of the host, not
  the program.
- **Block size is unconstrained.** With no vectorization there is no per-block scratch: the register
  file is per-sample and sized by node count, and table fills happen in `constants`. So `compute`
  accepts any frame count and the audio thread never allocates. The one per-block buffer in the
  system belongs to the crossfade (§8.2), and it lives in the live layer, not the instance.

**Channel mapping.** `process`'s arity fixes the DSP's input and output counts. The host maps device
channels positionally: missing inputs are fed silence, surplus outputs dropped, surplus device inputs
ignored. Stated as policy so the UI can show the mismatch rather than silently producing quiet.

**The buffer sample type is a separate axis from internal precision.** `compute`'s `in[]`/`out[]`
are the host's format, which need not be the f64 of §2 — the reference keeps the two independent
(`FAUSTFLOAT` versus the internal float, `global.cpp:493`) and converts at the boundary. The
artifact declares its buffer type, conversion happens on read and write, and the conformance harness
uses double for both because the oracle does.

**Floating-point environment.** The audio thread runs with flush-to-zero and denormals-are-zero
enabled. Recursive filters decay into denormal territory constantly, and on most hardware that costs
one to two orders of magnitude in throughput.
Both are *per-thread* control-register settings, established on the audio thread at startup, not in
`main`.

The testing consequence runs the other way. The reference emits no flush-to-zero code by default
(`-ftz 0`, `global.cpp:457`) and its impulse harness is plain C++, so the oracle runs *without* FTZ
and cannot be asked to do otherwise. The conformance harness (§11) therefore runs with FTZ and DAZ
**off** to match, making the setting live-audio-only. A mismatch here produces divergence that looks
like a compiler bug and is not.

### 7.2 The UI tree

Faust's UI structure is carried in *labels*: `vgroup`/`hgroup`/`tgroup` boxes prefix a path onto the
labels of the widgets beneath them, and each label may carry bracketed metadata. Propagation threads
a group path alongside the signal, and a pass over the Signal DAG collects widgets into a tree.

**A label is not a string, it is a path expression.**
The grammar is small and total (`propagate/labels.cpp:98-127`): `v:name`, `h:name` and `t:name` open
a vertical, horizontal or tab group *from within the label itself*, `/` separates segments, a leading
`/` anchors at the root, `./` stays, `../` walks up one level. So `hslider("v:Reverb/Wet", ...)`
creates a group with no `vgroup` anywhere in the source, and `"../Bypass"` escapes the group it is
written inside. Bracketed metadata is deliberately *not* stripped when the path is built. The
standard library uses these forms constantly, so reading only the group boxes fails on ordinary
programs, not exotic ones.

The group prefix is looser than `v`/`h`/`t` suggests. `label2path` branches on `label[1] == ':'` for
*any* first character, and `encodeName` (`propagate/labels.cpp:75-93`) maps `v`/`V`, `h`/`H` and
`t`/`T` to vertical, horizontal and tab — and **every other character to vertical**. So `"a:Gain"`
silently opens a vgroup named `Gain`. Match the case-insensitivity and the fall-through both.

**Labels are also evaluated, not copied.** `"Gain %i"` inside `par(i, 8, ...)` becomes eight distinct
labels: the `%ident`, `%{ident}` and width-padded `%2ident` forms are substituted during
*evaluation*, the identifier evaluated as an integer in the enclosing environment
(`evaluate/eval.cpp:934-1022`).
The padding is `printf` field width, not zero-fill: the table is `{"%d", "%1d", "%2d", "%3d", "%4d"}`
indexed by the digit in `%<n>ident` clamped to `0..4`, so `%2i` at `i = 3` yields `" 3"`, with a
leading space. That makes it §6.1's job, not propagation's, and it is load-bearing twice — it is how
iterated widgets get distinct paths at all, and §8.1 keys UI state migration on those paths, so a
wrong pad character breaks both silently.

The extracted **UI tree** is part of the artifact:

- **Groups** — vertical, horizontal, or tab, nested, each with a label.
- **Input widgets** — button, checkbox, vslider, hslider, nentry. Each carries its label, full path,
  the state field it writes, and `init`/`min`/`max`/`step`.
- **Output widgets** — vbargraph, hbargraph. Each carries a state field the DSP writes and the host
  reads, plus its range.
- **Metadata** — `[unit:Hz]`, `[style:knob]`, `[scale:log]`, `[tooltip:...]`, `[hidden:1]`,
  `[midi:ctrl 7]`, `[acc:...]` — parsed out of the label into a key/value map, with the cleaned
  label kept for display. Keys this project does not act on are carried anyway, at no cost, so a host
  can implement `midi` or `acc` on top of them later (§2).

The host renders controls from this tree and writes values directly into the named state fields. No
callback machinery, no `buildUserInterface` visitor: the tree already *is* the description. Values
are taken as written, since the reference does not clamp a widget to its declared range unless asked
(`-rui`, off by default, `global.cpp:460`) — `min`/`max` are a UI contract the host enforces, not a
guarantee the DSP re-checks. Reference Faust's JSON description for external architectures is a
straightforward serialization of this structure if it is ever wanted.

### 7.3 Soundfiles

`soundfile("label[url:{'kick.wav'}]", n)` is a box of two inputs — a part index and a read index —
and `n + 2` outputs: the selected part's length and rate, then one buffer-read signal per declared
channel (`propagate/propagate.cpp:621-635`).

- The artifact lists each soundfile's label, URL set, and declared channel count.
- The **host** resolves URLs and decodes audio; the compiler never touches a file. Decoding stays out
  of the compiler, and the host applies its own search paths and formats.
- Loaded data is per-channel buffers plus per-part length, rate and offset, referenced by a pointer
  in the state block.
- **A missing or undecodable file is not an error.** Substitute the reference's behavior and report a
  diagnostic. That behavior is specific and the corpus depends on it: a soundfile always has 256
  parts, an unfilled part is 1024 frames of silence at the declared channel count, and per-part
  offsets accumulate across the whole set (`architecture/faust/gui/Soundfile.h:36-39, 144, 285-313`).
  A typo'd filename must not take down the edit loop.
- **Loaded data is cached by URL across recompiles.** Editing a line of code must not re-decode a
  large sample — a live-loop requirement, not an optimization.

### 7.4 Foreign symbols

This is the one place where the two-tier decision (§6.4, §6.5) creates a real problem: `ffunction`
calls arbitrary external C, which an LLVM backend links trivially and a bytecode interpreter cannot
call at all.

The problem is narrower than it looks. Faust's foreign constructs split into two populations that
behave completely differently:

- **Runtime-provided constants**, above all `fconstant(int fSamplingFreq, "<math.h>")`. The
  C-flavored syntax is a disguise: this is how `ma.SR` is defined in the standard library, so it
  appears in essentially every non-trivial program. These resolve against a small fixed table the
  runtime owns (§7.1), trivially, in both tiers.
- **Genuine external calls** — `ffunction(float clip(float, float), "clip.h", "")`. Rare, and
  concentrated in code that is already platform-specific.

Resolve both with **one symbol registry shared by both tiers**: a map from (name, signature) to a
native function pointer, populated from built-in math primitives, host-registered natives,
`dlopen`ed libraries, and — for the LLVM tier only — the process symbol table. The interpreter calls
through a small thunk; Faust restricts foreign signatures to scalar `int`/`float`/`double` arguments
and return, so a handful of thunk shapes covers the entire space.

**The name in that key is chosen, not given.** An `ffunction` declares one to four `|`-separated
names and the reference selects by build precision (§4.5), so `float sinf|sin|sinl(float)` is a
lookup of `sin` under this design's default f64 and of `sinf` under `-single`. Resolving the wrong
slot fails only on the programs that bothered to spell out per-precision names — which is exactly the
platform-specific code §7.4 exists for.

Two shapes are not plain calls. `fvariable` is a *read of an external variable*, so the registry
holds addresses as well as function pointers and the read is scheduled at block rate. And an argument
declared `any` (`parser/faustparser.y:780`) takes no cast, so the thunk for that position is chosen
from the argument's inferred nature rather than from the signature.

**An unresolved symbol poisons its subgraph rather than failing the compile** (§9). The program
still runs, the broken region is marked in both the editor and the box view, and audio continues if
`process` does not depend on it.

Because the registry serves the interpreter as well as it serves LLVM — the tiers differ only in
that LLVM additionally sees process symbols — nothing here makes LLVM non-optional. The honest limit
is at the other end: foreign code that is neither registered nor `dlopen`able works in neither tier.
This environment loads native functions, it does not compile C.

---

## 8. Live reload

Recompiling on every keystroke is only usable if the running audio survives it. Two problems.

### 8.1 State preservation

Editing a gain constant must not silence a reverb tail, and moving a slider then editing code must
not reset the slider.

Give every state field a **stable identity derived from the Signal node that owns it** — not from
allocation order, which shifts on every edit. Because Signal nodes are hash-consed from
Merkle-hashed terms, an unchanged filter keeps its identity automatically, and that identity survives
unrelated edits elsewhere in the file. That last clause is what forces §4.6's split: it holds only
because the hash is provenance-free.

The node hash alone is not enough, and taking it alone defeats the example above. State for a
feedback network lives on its `Rec` node, whose Merkle hash covers the whole body, so editing a gain
constant *inside* the reverb changes that hash, the lookup finds nothing, and the tail dies. Merkle
hashing propagates changes upward; state sits at the top.

So every field carries a second key: a **shape hash**, the same Merkle hash computed with numeric
literal payloads normalized away. Editing a constant leaves it alone; editing structure changes both.

**This is the alignment problem**, which the literature splits into *keyed* alignment, on a stable
identifier, and *similarity* alignment, on resemblance (Barbosa, Cretin, Foster, Pierce et al.,
*Matching Lenses*, 2010). The three passes below are keyed, similarity, fresh. Similarity is right
for the second: an anonymous subterm carries no stable key, and once an edit restructures the graph
there is no recoverable fact about which old delay line a new one should inherit, so the pass decides
a behaviour rather than approximating an answer. Editing needs none of it — every edit carries the
ref it applies to (§4.3) — and migration's worst case is one field holding a same-shaped neighbour's
history, truncated by the length rule below and gone within the filter's own decay.

On reload, build the new state layout, then match new fields against old ones in three passes:

1. **Exact hash match** — an untouched subgraph. Copy.
2. **Shape hash match** — same shape, different constants. Copy. This is the pass that keeps the
   reverb tail alive across a gain edit.

   Fields sharing a shape hash are paired **by source proximity**, not allocation order. Order alone
   is a trap: in a program with eight identically shaped filters, deleting the third shifts every
   later one and five filters inherit their neighbour's state. Every field traces back through its
   Signal node to a Term value and, through that file's ref tree, to a byte range (§4.6), and pairing
   on that range makes migration follow the user's edit instead of an array index.

   **Proximity is the byte distance between range starts, paired greedily over the closest remaining
   candidate.** Byte rather than tree distance, since the ref tree is rebuilt on every reparse and
   its shape shifts with the edit while offsets do not. Ties break toward the lower offset, making
   the result independent of traversal order (§5.5).
3. **No match** — initialize fresh.

Within a match, a field of a different length (a delay line resized) copies the common window
relative to the write head and zeros the remainder.

Three field classes need a rule beyond node identity:

- **UI values** key on their label path (§7.2), not their node hash. A slider the user has moved
  should stay moved when the surrounding expression is edited, and the path is what survives that.
- **Soundfile pointers** key on URL and are cached across reloads (§7.3), so an edit never
  re-decodes audio.
- **Tables and waveforms** are recomputed rather than migrated: their contents are a pure function
  of the init band, so migrating them buys a correctness risk and no perceptual payoff.

None of this is bespoke machinery: both keys come out of the same bottom-up pass §5 already needs,
and the provenance the shape pass pairs on is the side table §4.6 already carries.

### 8.2 Real-time hand-off

Compile off the audio thread. Publish the new instance by atomic pointer swap; the audio thread picks
it up at a block boundary and the old instance is freed on the compile thread. Never allocate or lock
on the audio thread.

State preservation alone still clicks when the graph changes structurally, so cross-fade over a few
milliseconds between old and new instances, running both during the overlap. Without it, "live" is
unpleasant regardless of latency. The fade is **linear, with weights summing to one**, so two
instances producing the same signal sum to exactly that signal — which is what §11.5 property 4
asserts, and what an equal-power fade would raise by as much as 3 dB. And when the new Plan hashes equal to the old,
skip the swap entirely: an edit that does not change the compiled program costs nothing and stays
exactly sample-identical.

Three threads total: audio (RT), compile (query engine), UI.

**Control writes cross a thread boundary too**, and the instance swap does not cover them. UI-bound
control fields are written by the UI thread and read by the audio thread once per block; bargraph
fields go the other way. Both are single scalars, so make them relaxed atomics — no locks, no fences,
no cost on the target architectures. Faust's own architectures write these as plain pointers, which
works in practice and is a data race on paper; no reason to inherit that when correctness is free.

Tearing is not a concern beyond this. Faust has no multi-word control values, and the control band
reads each field once into a register at the top of `compute`, so a value changing mid-block is seen
at the next one.

### 8.3 What happens when the program does not compile

Most keystrokes leave the program transiently invalid, so this is not an edge case but the common
state of the system, and the rule below is what the environment does in it.

A compile produces either an artifact or a diagnostic set. The swap rule:

- **`process` resolves and its reachable subgraph is error-free** → build the instance, migrate
  state, crossfade, swap.
- **`process` is missing, unresolvable, or depends on an `Error` node (§9)** → **do not swap.** The
  last good instance keeps playing. Diagnostics appear in the editor and on the affected boxes.

So the environment deliberately lets the two views diverge: **you see the broken program and hear the
last good one.** The box view still updates, because it renders Term, which survives a partial parse.
That divergence is correct — the alternative is silence or NaN on every incomplete keystroke — but
it has to be visible in the UI, or the user wonders why an edit had no audible effect.

Before the first successful compile there is no last-good instance, and the output is silence.

---

## 9. Error resilience

A batch compiler may throw on the first error. A live editor may not — the transiently invalid
program of §8.3 is the common case, and it has to stay responsive rather than abandon the compile.

- **Errors are values, not exceptions.** Every phase produces diagnostics attached to Term *value*
  ids. They resolve to source ranges by walking the open file's ref tree (§4.6), which marks every
  occurrence of a broken fragment.
- **Errors are local.** An explicit `Error` node in Box and Signal propagates through the graph. A
  broken definition poisons itself and its dependents; everything else still compiles. Whether audio
  keeps running depends on whether `process` reaches the poisoned region, and §8.3 gives the rule.
- **Partial trees still work.** tree-sitter's error recovery turns a half-typed expression into a
  tree with an error node rather than a failure, and the rest of the file still renders as boxes. The
  representation is §4.5's `Hole`: it keeps its recognizable children, so recovery inside a broken
  expression still produces boxes, and it prints back verbatim. Its `Error` box has unconstrained arity
  (§6.1), which is what keeps one typo from producing a diagnostic per enclosing composition.
- **Divergence is bounded too.** `foo = foo;` and its subtler relatives make evaluation
  loop forever, and a batch compiler can afford to spin until someone kills it. A live one cannot:
  the compile thread stops answering and every later keystroke queues behind it, which reads as the
  editor freezing rather than as a broken program. The reference guards evaluation with a loop
  detector over (expression, environment) pairs and a stack-depth limit (`evaluate/eval.cpp:324-325`,
  `gLoopDetector(1024, 400)`). Here the same guard is nearly free, because evaluation is already
  memoized on (value id, environment id) — marking a key in-flight detects re-entry, and a depth
  counter bounds the rest. Hitting either limit is a diagnostic like any other, and §8.3 then
  declines the swap. This is the value-level detector; the path-level one for import cycles lives in
  the query engine and works differently, for reasons §5.8 gives.

A real departure from the reference compiler, which throws a `faustexception` and abandons
compilation. Not an optional refinement — it is what "live" means.

### 9.1 The diagnostic record

Every phase emits into one set, so the record is one shape:

```
{ severity, code, subject, related[], payload }
```

- **`severity`** — `error`, `warning` or `info` — is what §8.3's swap rule reads: only an `error`
  reachable from `process` declines the swap. `info` carries what the compiler did correctly and
  invisibly, like a precision-filtered statement (§4.5) or a substituted soundfile (§7.3).
- **`code`** is a stable enum prefixed by the phase raising it: `syn` for parse and `Hole`, `res` for
  resolution and import cycles, `eval` for scope, arity, pattern and loop bounds, `type` for nature,
  interval and delay bounds, `plan` for scheduling and allocation, `link` for foreign symbols. The
  prefix is what lets §11.4 compare error *classes* against `tests/error-tests` rather than strings.
- **`subject`** is an interned Term value id, never a byte range; ranges come from walking the open
  file's ref tree (§4.6), which marks every occurrence of a shared fragment.
- **`related[]`** carries further value ids where a diagnostic is about a pair — a shadowed rule and
  its shadower (§6.1), a redefinition and its collision, an import cycle and its closing edge.
- **`payload`** is the typed data the message renders from: an arity mismatch's two counts, an
  unbound name, the interval that failed to bound a delay.

Two properties come from sections above. Diagnostics hold **permanent ids only** (§5.9), so a set
outlives an arena drop. And the set is **deterministically ordered** (§5.5) — file resolution index,
first byte offset, then code. Deduplication keys on `(code, subject, payload)`, so a memoized subterm
raises once and renders at every occurrence.

---

## 10. What is cut, and what it costs

Simplicity is bought. Naming the price is what lets the trades be revisited knowingly.

| Cut | Buys | Costs |
|---|---|---|
| Fixed-point / resolution inference | The large majority of the reference's interval library | No fixed-point targets (embedded DSP without an FPU) |
| Vector and scheduler codegen modes | Two whole code containers and a task-graph scheduler | Relies on LLVM auto-vectorization; no multicore DSP |
| Eighteen text backends | A backend abstraction layer and eighteen emitters | No Rust/Julia/wasm-text/etc. output |
| SVG drawer, mathdoc | Two independent subsystems | The box UI supersedes the drawer; mathdoc has no replacement |
| `libfaust` API compatibility | The `global` singleton and its threading contortions | Not a drop-in for existing tooling |
| MIDI/OSC implementation | Two protocol stacks and their threading | Metadata is preserved (§7.2), so a host can add them without compiler changes |
| Polyphony | Voice allocation, note routing, the `effect` convention | Addable purely above §7, since it is N instances plus a scheduler — but it costs two of the four sections of every reference `.ir` (§11.1) |
| Compiling C for `ffunction` | A C toolchain dependency | Foreign code must be registered or `dlopen`able (§7.4) |

Interval analysis is the one cut that is partial rather than total, so it deserves a note. The
reference covers every primitive to a high standard because fixed-point support demands it. Without
fixed-point, intervals matter only where they change generated code — delay line sizing, and the
clamping of table and soundfile indices — so elsewhere the rules can widen freely, costing at most
code quality and improvable later on demand. Only elsewhere, though: on the arithmetic that reaches
a delay or table index a widened answer is not pessimal, it rejects working programs or changes the
emitted graph, so there the rules stay at least as tight as the reference's. §6.3 measures that
slice across the corpus and puts thirteen operations in it, against the reference's ~50 files.

What is emphatically *not* cut is the definition language, the algebra, or numeric fidelity. Those
are the language.

---

## 11. Correctness

The reference repository ships an unusually good oracle and this plan leans on it hard.
`lib/faust/tests/impulse-tests/reference/` holds `.box` (evaluated diagram as source), `.sig`
(normalized signal graph), `.type` (per-node type and interval), `.fir` (state layout and band split,
§11.1) and `.ir` (a 60000-frame impulse response) — 93/93/92/93 of the first four and 94 usable of
the last, over 94 `.dsp` inputs. The levels cover different sets of programs, so each is checked over
what it has: `select2` is the one program missing a `.type`.

The directory holds 99 `.ir`, but `clarinet`, `midi`, `organ`, `simulated_control` and `sitar` have
no `dsp/*.dsp` and `Make.ref` derives its targets from `$(wildcard dsp/*.dsp)` — orphans of deleted
inputs, unregenerable, and excluded from the counts above.

### 11.1 Comparison method

Compare graphs, not bytes. Isomorphism validates evaluation and propagation while leaving the
project independent of the reference's pretty-printing.

`.box` is ordinary Faust source — a list of definitions ending in `process = ID_n;` — so it parses
with our own front end and the check is isomorphism against what we produced from the original
`.dsp`. Its leading `declare` lines are compiler-generated rather than part of the program
(`version`, `compile_options`, `filename`, and one `library_path` per resolved import, carrying
absolute paths from whichever machine generated the file), so the comparison drops them.

**The `declare` lines that remain encode a namespacing rule, and most of them are it.** A `declare`
keeps its key verbatim only in the master document; in an imported file the key becomes
`<file>/<key>` (`parser/sourcereader.cpp:488-500`), and the printer maps `.`, `:` and `/` to `_`
(`global.cpp:2006-2015`), so `math.lib`'s `author` emerges as `math_lib_author` — 14 of `echo.box`'s
20 header lines. Two printer quirks ride along: `author` is exempt from the replacement and is the
only key whose duplicate values all print, every other key printing just the first. A missing master
`declare name` is synthesized from the basename, and `filename` is always set (`:195-204`).

**`.sig` is not source.** It is an SSA dump in a notation of its own (`ID_5 = letrec(W0 = (ID_4));`,
`proj0`, `ID_7 = ID_6@0`, `buffer`, `length`) and needs a small dedicated parser — a real
deliverable, a few hundred lines. Two properties make it easier than it looks: every right-hand side
is exactly *one* operation over `ID` references and literals, never a nested expression, and its only
normalization is reading the reference's explicit `@0` as identity (§6.2).

**Write that parser against regenerated files, not the shipped ones.** The two notations already
differ: `signals/ppsig.cpp:276-279` separates `sigWRTbl4p`'s four arguments with `;`, while the
shipped `table1.sig` and `table2.sig` use `,`. The files were generated by 2.81.0 and the submodule
is 2.85.9, so a parser written from the current printer fails on the shipped corpus and one written
from the shipped corpus fails on anything regenerated. This is §11.6's "regenerate rather than
trust", with a concrete instance attached.

Compare `.type` as a multiset, since emission order is a traversal detail, and as a **projection**,
which is worth saying so it does not look like a broken test. The reference encodes five dimensions
per entry — `NKCVN` is nature, variability, computability, vectorability, boolean
(`signals/sigtype.cpp:103`) — of which this design computes two, and its `interval(lo, hi, lsb)`
carries a third field for fixed-point resolution (`signals/interval.hh:158`). Compare nature,
variability and the interval's bounds; ignore computability, vectorability, boolean and the lsb.
That the ignored fields are exactly the ones tied to fixed-point and to codegen modes we do not have
(§10) is itself a check: any dropped dimension that turns out to be needed surfaces as a `.type`
field we cannot explain.

**`.fir` is the oracle for Plan.** Everything above checks the front end, leaving scheduling and
state allocation to the impulse run two phases later. The
`.fir` dumps close that gap for free: each prints the DSP struct with every field's type and array
shape, then the code split into `Init`, `ResetUI`, `Clear`, `Compute control`, `Compute DSP` (the
frame loop) and `Post compute DSP`. Their FIR is an expression tree and ours is three-address, so the
comparison is again a projection: the set of state fields with their shapes, and which band each
computation landed in. That checks rate scheduling and state layout at the phase that
introduces them, instead of dynamically and much later through §11.5 property 6.

Three of those sections do not line up with §6.3's three bands, and the mapping has to be written
down or the projection reports mismatches that are not:

- `Init` / `ResetUI` / `Clear` are §7.1's `constants` / `resetControls` / `clear`, only the first of
  which is the init band.
- `Post compute DSP`, with its paired load in `Compute control`, is bookkeeping for one guarded
  store in our sample band (§6.3) — not two more scheduled computations.
- **Read-only tables are not instance state in the reference.** `gInlineTable` is `false`
  (`global.cpp:508`), so `generateRDTbl` emits an `rdtable` array as `Address::kStaticStruct`
  (`instructions_compiler.cpp:1843-1848`): a static member outside the DSP struct, filled by
  `classInit`, shared by every instance. `table.fir`'s struct is
  `(fSampleRate)(fConst0)(fHslider0)(float[2] fRec2)` with no table in it. The generator subgraph
  gets its own **sub container** with its own struct, `Clear` and `Compute` — §6.3's "its own little
  program", confirmed. We allocate tables per instance and fill them in `constants`, identical
  behaviour since their contents are a pure function of the init band, so the projection excludes
  table storage on both sides.

The `.fir` files carry three more oracles, all free. The **`User Interface`** section —
`OpenVerticalBox` / `AddHorizontalSlider("feedback", fHslider0, 98.4f, 0.0f, 1e+02f, 0.1f)`, nested
— is a complete checked-in oracle for §11.5 property 5, needing no `-json` run and available the
moment phase 4 emits a UI tree. `Object memory footprint` and `Variable access in {Control, compute
control, compute DSP}` are secondary checks on state layout.

Two properties of the shipped files shape the harness. The structural files are `-single` while
`.ir` is `-double` (§2), so they are regenerated at our own precision rather than compared as
shipped. And `.sig` ships normalized, with a variant from the same Makefile (`FAUST_SIG_NO_NORM`,
`tests/impulse-tests/Makefile:477-479`) that suppresses sum reassociation and nothing else (§11.2);
comparing against that one first separates "did we associate sums the same way" from every other
graph question, which is §11.2's.

**So the harness regenerates all five levels itself, and the recipes are worth writing down**, being
spread across two makefiles with the option lists fighting each other.

Build, once: `cmake -C lib/faust/build/backends/regular.cmake -DFIR_BACKEND=COMPILER`.
`regular.cmake` is the preset that avoids LLVM, but it sets `FIR_BACKEND OFF` and `.fir` needs
`-lang fir`; no shipped preset gives FIR without LLVM. Only the `faust` binary is needed —
`filesCompare` is standalone C++ and links nothing (`tests/impulse-tests/Makefile:553-554`), so the
oracle never builds `libfaust`.

Then, per `dsp/X.dsp`, from `tests/impulse-tests/` and at our own `-double` (`Make.ref:77-95`,
`:115`):

| Level | Command |
|---|---|
| `.box` | `faust -double -e dsp/X.dsp -o reference/X.box` |
| `.sig` | `faust -double -norm1 dsp/X.dsp` |
| `.sig` unnormalized | the same, with `FAUST_OPT=FAUST_SIG_NO_NORM` in the environment |
| `.type` | `faust -double -norm2 dsp/X.dsp` |
| `.fir` | `faust -lang fir -double dsp/X.dsp` |
| `.ir` | `faust -double -i -a archs/impulsearch.cpp dsp/X.dsp -o dsp/X.cpp`, then `c++ -O3 -I../../architecture -Iarchs -pthread -std=c++11`, then run with `-n 60000` |

Two rows are load-bearing beyond their content. `-norm1` and `-norm2` are the only way to reach the
`.sig` and `.type` dumps, and `-norm2` is undocumented: `--help` lists `-norm` and `-norm1` then
skips on (`global.cpp:2559-2566`) though `-norm2` and `-norm3` both parse (`:1504-1512`). And the
`.ir` recipe carries **no `-I dsp`**, which is how §11.4's pinned libraries actually get found — the
resolution side effect of §5.6 layer 2, not a flag. A harness that "fixes" that by adding `-I`
everywhere regenerates a corpus resolved against the wrong libraries.

Compare `.ir` with the reference's own `filesCompare` (§11.2).

**The `.ir` files are a protocol, not just a buffer of numbers**, and reproducing them means
reproducing the harness (`tests/impulse-tests/archs/impulsearch.cpp`,
`tests/impulse-tests/archs/controlTools.h`):

- Four concatenated sections of 15000 frames each: impulse response, the same run with each block
  split at a random point, then two *polyphonic* runs at 4 and 1 voices. Polyphony is a non-goal
  (§10), so the harness compares the first two sections with `filesCompare -part` and says so, rather
  than appearing to pass a check it never ran. Section two earns its place: for a correct scalar
  compiler its output must equal section one's, making it a block-size invariance test that needs
  nothing from the reference's `rand()` sequence — the block counter advances per loop iteration,
  not per `compute`, so the impulse still lands at frame 0 and buttons stay held across both halves.
- **`-part` relaxes exactly one check**, the `number_of_frames` equality; the comparison loop is then
  driven by the *test* file's count (`filesCompare.cpp:122-130`). So our runner emits a 30000-frame
  header and 30000 lines, compared against the reference's first 30000, and the outer loop re-reads
  the reference per further response so a short file terminates cleanly. Tolerance is absolute
  (`fabs(s1 - s2) > tol`), with a hard exit after ten mismatches.
- 44100 Hz, blocks of 64 frames. The first block gets an impulse on every input and **all buttons
  held at 1**; every later block gets silence and buttons at 0.
- Output is printed per frame at `%8.6f`, through a filter that zeroes anything under `1e-06` and
  aborts the run on NaN or infinity. That print granularity is where §11.2's `2e-06` tolerance comes
  from, and it also keeps denormals out of the comparison — §7.1's FTZ rule is about what they do to
  a feedback loop before it prints. Our runner replicates the abort, since a diverging program leaves
  a file shorter than its own header claims and a runner that keeps printing compares misaligned
  frames. `filesCompare` also exits before comparing anything if the two headers disagree on channel
  counts.
- Soundfiles are not read from disk: the harness installs a reader synthesizing every part as
  `sin(part + 2*pi*sample/4096)`, 2 channels, 4096 frames, 44100 Hz. Matching `sound.dsp` means
  feeding our §7.3 host interface the same buffers.
- MIDI messages are pushed through the DSP before the run, so a program with `[midi:...]` metadata
  reacts to them. Exactly one program in the impulse corpus does, so it is documented and excluded
  rather than implemented.

### 11.2 Numeric fidelity and the normalization trap

Bit-exactness is not the bar: reference Faust does not require it even between its own backends,
defaulting to a tolerance of `2e-06` (`tests/impulse-tests/tools/filesCompare.cpp:174`).

But floating-point addition and multiplication are not associative, which makes **arithmetic normal
form semantically observable**. The reference's `aterm`/`mterm` normalization imposes a specific
ordering on sums and products, and a different but equally reasonable ordering usually stays inside
tolerance — until a resonant filter or long feedback network accumulates and diverges. The failures
look random and surface late.

Ordering is not all of it. `normalizeDelayTerm` (`normalize/normalize.cpp:130-136`) rewrites
`(x@n)@m` to `x@(n+m)` whenever the inner delay's signal order is below 2, which is why `echo.sig`
shows the `mem` that `~` inserts absorbed into the user's delay as `ID_3@((... & 65535) + 1)` rather
than as two `@` nodes. Reordering and constant folding alone will not produce that.

Porting the reference's normalizer wholesale is the largest single complexity import still available
to this design, and it is paid up front, on a theory rather than on evidence. Note that
`FAUST_SIG_NO_NORM` gates only `normalizeAddTerm` (`normalize/simplify.cpp:218`),
so the "unnormalized" dump still carries constant folding, product normalization and the delay fusion
above. It isolates sum association — which is exactly the question at issue, but only that one.

**Build the simple thing first and let the corpus decide.** A canonical ordering plus constant
folding is a small amount of code. Run the whole corpus against `filesCompare` and port fidelity only
where the comparison actually fails — the corpus carries `norm1`, `norm2`, `norm3` and `math_simp`
to exercise exactly this, so the feedback is immediate and specific.

The risk is real but bounded. For a *stable* filter a differently-associated sum produces an error
that stays bounded rather than growing, which is why most programs will pass; those that diverge name
themselves, and porting the reference's ordering for those cases is a targeted job rather than a
subsystem. What must not happen is shipping without running the comparison and assuming tolerance
covers it — which is why §15 files this as the sharpest correctness risk in the project.

### 11.3 Semantics that must match exactly

A different class of risk from normalization: details that pass every structural check — `.box`,
`.sig` and `.type` all agree — then fail the impulse comparison, or pass on the corpus and fail on a
user's program. Each is pinned against the reference here rather than left to be inferred, and each
gets a purpose-built `.dsp` probe.

**Arithmetic and promotion** (`signals/sigtyperules.cpp:548-560`, `signals/binop.cpp`):

- **Division always yields a float, even for two integers.** `3/2` is `1.5`, not `1` — an explicit
  rule in the reference (`floatCast` on `kDiv`), and the single easiest way to get a whole program
  subtly wrong.
- **Comparisons yield an int** — the reference additionally tags it boolean, a dimension we drop
  (§6.3), so for us it is an int with interval `[0,1]`. **Shifts and bitwise operators yield an
  int.** Every other binary operator yields the join of its operand types.
- **`%` is C signed remainder** — the sign follows the dividend, `srem` rather than a modulo
  (`signals/binop.cpp:69`) — and **`>>` is arithmetic**, `ashift_right` (`:78`). Logical right shift
  is a separate primitive with no surface syntax: `boxLRightShift` exists but no token produces it,
  so it is reachable only from the signal API.
- **`^` yields the join, so `int ^ int` is an int** (`extended/powprim.hh:45-58`). The exact mirror
  of the division rule above, and pinned next to it because the two look symmetric and are not.
- **`^` rewrites at two layers, and only the first reaches `.sig`.** `computeSigOutput`
  (`extended/powprim.hh:89-119`) folds during normalization, so phase 3 compares these as graph
  nodes: two constants give `ipow` when both are int-natured **and the exponent is strictly
  positive**, otherwise a double `pow`; a constant exponent alone gives `x^0` → the real `1.0`,
  `x^1` → `x`, `x^0.5` → `sqrt(x)`, `x^0.25` → `sqrt(sqrt(x))`. The `x^10` → `exp10(x)` arm sits
  behind `gHasExp10`, `false` unless `-exp10` is passed (`global.cpp:496`, `:1534`), and is not
  implemented. `pow.dsp` exercises the list directly, with exponents `0.5`, `0.25`, `0.125`,
  `0.0625`, `2`, `2.0`, `0`, `1`, `0.0` and `1.0`.
- **The repeated-multiplication expansion is codegen, not a graph rewrite.** `generateCode`
  (`powprim.hh:151-193`, under `gNeedManualPow` at `global.cpp:503`) emits a `faustpower<n>_i`/`_f`
  helper for a constant, computable exponent in range, in both tiers. Probed, `process = _ ^ 3;`
  dumps `pow(IN[0], 3.0f)` at `-norm1` while the generated C++ calls `faustpower3_f` — so `.sig`
  compares the `pow` node and §11.5 property 3 compares the expansion. On int operands the helper
  multiplies in int and wraps.
- **A constant negative int exponent becomes a reciprocal, during normalization.** `mterm` collects
  `x^n` factors with integer exponents (`normalize/mterm.cpp:114-128`) and `normalizedTree` puts the
  negative ones in the denominator (`:493-511`), so phase 3 compares it as a graph rewrite. Probed,
  `int(_) ^ (-2)` dumps `pow(ID_0, 2)` under `1.0f/float(…)` and compiles to
  `1.0f / float(faustpower2_i(…))`. That also makes `isIntPowArg`'s asymmetry unreachable: it bounds
  a *float*-natured integral constant with `>= 0 && <= 8` and an **int**-natured one with `<= 8`
  alone (`powprim.hh:122-149`), which read on its own admits a negative exponent into an expansion
  returning the base — but normalization consumes it first, so `powprim.hh` without `mterm.cpp`
  suggests a bug that never fires. Probe: `pow_negint.dsp`,
  `process = int(_) ^ (-2), int(_) ^ 3, _ ^ (-2), 2 ^ (-2);`, taking reciprocal-by-normalization, the
  int expansion, a float base holding the exponent at `-2.0f`, and a constant arm folding to `0.25`.
- **Division by zero is unguarded.** Floats go to ±inf or NaN per IEEE; integer division by zero is
  largely unreachable precisely because `/` promotes to float.

**Promotion places casts in the graph, and the placement is what `.sig` records**
(`transform/sigPromotion.cpp:295-436`). The rules above give the result types; these give the
operands, and the two are separate claims:

- Both operands of `/` are float-cast, both operands of `&`, `|`, `xor` and the shifts are int-cast.
- Delay indices and table read/write indices are int-cast; an `rwtable`'s written value is cast to
  the table's content nature.
- `select2`'s selector is int-cast, and its two branches are float-promoted *only* when their natures
  differ — matching natures are left alone.
- Foreign call arguments are cast per the declared signature, and left alone where it says `any`.
- `prefix`'s two arguments are float-promoted only when their natures differ.
- Every one of these is a *smart* cast: omitted when the operand already has that nature. Emitting a
  redundant cast is as much an isomorphism failure as omitting a needed one.

**Composition wiring** (`propagate/propagate.cpp:299-326`, `:734-760`). The algebra's arity rules are
well documented. What happens to the signals is left implicit:

- **`:>` sums.** Output bus `b` is the sum of inputs `b`, `b + n`, `b + 2n`, ...; a bus with no input
  reads integer 0.
- **`<:` replicates modulo** the input count, so it is defined even where the arity check would not
  suggest an obvious pairing.
- **`route` is 1-based, silently partial, and additive.** An entry whose source or destination is out
  of range is dropped without a diagnostic, several sources landing on one destination are summed,
  and an unconnected output is integer 0.

**Casts:**

- **`int(x)` truncates toward zero**, matching a C cast. Rounding is never implicit — `rint`,
  `floor`, `ceil` and `round` are separate primitives.
- **Float-to-int conversion out of range, or of NaN, is undefined in the reference by default** (C++
  `static_cast` and LLVM `fptosi` both, with `-cir` off, `global.cpp:514`). We **define** it, and not
  by invention: the definition is what `-cir` itself generates,
  `int(min(2147483647.0, max(x, -2147483648.0)))` (`transform/sigPromotion.cpp:671`), plus NaN to 0,
  which `-cir` leaves open. Both tiers implement that one definition, which is what §11.5 property 3
  checks; the reference's own backends diverge here.

  **It lives in both backends' emission, not in the graph.** `-cir` implements it as a signal
  rewrite, but `gCheckIntRange` is `false` by default, so the graph we compare against has no clamp
  nodes and putting ours there fails `.sig` isomorphism on every conversion. The one semantic in
  §11.3 deliberately *not* a graph rewrite; the two tiers agreeing is what keeps both facts true.

**Tables:**

- **Within a sample, an `rwtable` write happens before the read** — no rule needed, it falls out of
  the data model. In the reference, `generateRDTbl` compiles the write node to obtain the table it
  reads from (`generator/instructions_compiler.cpp:1850`), so the read *consumes the table value
  produced by the write*. Model `wrtable` the same way and topological emission (§6.3) orders them
  correctly with no special case.
- **Table writes are placed by variability**, not always in the sample loop: constant to the init
  band, block-rate to the control band, sample-rate inside the loop under its guard
  (`instructions_compiler.cpp:1813-1823`). The same three-band scheduling as everything else, which
  confirms it generalizes.
- **Table accesses are clamped by default, and the clamp is conditional.** `-ct` is on
  (`global.cpp:475`), and it is a signal rewrite rather than backend code, so it shows up in `.sig`:
  an index the interval cannot prove in range becomes `max(0, min(i, size-1))`, for reads
  (`transform/sigPromotion.cpp:601`) and for `rwtable` write indices (`:633`). An index that *is*
  provably in range is left alone, which is why §6.3 ties interval precision to generated code.
  Soundfile reads are clamped unconditionally at propagation, to `max(0, min(i, length-1))`
  (`propagate/propagate.cpp:630`). Together these are what hold every access inside the state block.

**Already pinned elsewhere:** `select2`/`select3` evaluate both branches (§6.3); delay lines
initialize to zero except under `prefix`, whose initial value must itself be init-time computable
(§6.3, `signals/sigtyperules.cpp:507-511`); an unbounded or negative delay index is a compile error
rather than a widened allocation (§6.3); a recursive group's outputs are read undelayed and only its
in-body feedback carries the `mem` (§6.2); a guarded value keeps its last computed result across
blocks where the guard is false, so it lives in state (§6.3); a missing soundfile is 256 parts of
1024 silent frames (§7.3); UI labels are path expressions and are substituted during evaluation
(§7.2).

### 11.4 Corpus

The reference programs first, at whichever of the five levels each one has (§11); then 341 `.dsp`
across `lib/faust/tests`; then 296 in `lib/faust/examples`; then faustlibraries, the densest real
exercise of the language. `lib/faust/libraries/` is a nested submodule, checked out at `ecf2fdc23` —
the SHA §12 embeds. A fresh clone initializes it with
`git -C lib/faust submodule update --init libraries`, naming the path rather than `--recursive`,
which also pulls oboe, the CLAP SDK, py2max, faust2ck and spectra.

**The impulse corpus does not use faustlibraries at all, and cannot.** `tests/impulse-tests/dsp/`
ships pinned pre-namespace copies of `music.lib`, `math.lib`, `filter.lib`, `oscillator.lib`,
`effect.lib` and `maxmsp.lib`. They define bare names that no longer exist in modern faustlibraries —
`music.lib` has `delay(n,d,x) = x@(int(d)&(n-1))` and `millisec` — so `echo.dsp` and its neighbours
fail to resolve without them. Those files are part of the oracle, and the harness puts that directory
on §5.6's search path ahead of the embedded standard library.

**Nothing on the command line makes this work.** The `.box`, `.sig`, `.type` and `.fir` targets pass
`-I dsp` (and thereby drop `-double`, §2), but the `.ir` targets pass no `-I` at all: they resolve
because `fopenSearch` opened `dsp/echo.dsp` relative to the working directory and pushed `dsp/` onto
the import path (§5.6 layer 2). That layer is what selects the pinned copies over the modern
library, which satisfies the same imports and compiles.

The failure mode is numeric divergence, not a resolution error, and the shipped files say how far:
`libraries/old/` carries same-named descendants of all five, every one differing from the pinned
copy — `oscillator.lib` by 8 diff lines, `filter.lib` by 26, `math.lib` by 162, `music.lib` by 381,
`effect.lib` by 430. `maxmsp.lib` is worse, existing as both `libraries/maxmsp.lib` and
`tests/impulse-tests/dsp/maxmsp.lib` under one resolution key, separated only by precedence.

For error behavior, use `tests/error-tests` and `tests/warning-tests`, comparing error *classes*
rather than message strings. Which programs are rejected is the property that matters; §9 makes our
diagnostics differ from the reference's by design.

### 11.5 Properties specific to this design

Six invariants the reference compiler has no analogue for, each cheap to test and each protecting
something the product depends on:

1. **Printer fidelity — the quotient-lens laws of §4.4** over the whole corpus, each with its stated
   quantifier: CST fidelity, `text(cst(src)) == src`, on every file; PutGet,
   `value(parse(print(t))) == value(t)`, for every hole-free term; GetPut, byte-identity on the
   canonically parenthesized subset; and the canonizer law, `splice` idempotent byte-exactly over
   every ref. Needs the splice primitive, which is why §14 puts it in phase 1.
2. **Incremental equivalence.** Apply a random edit, then compare the incrementally recompiled
   result against a from-scratch compile of the edited text. They must be identical — the invariant
   a memoizing compiler most easily breaks.
3. **Tier agreement.** Interpreter and LLVM outputs must match within tolerance across the corpus.
   Two independent backends checking each other is the cheapest real bug-finder available.
4. **State-preservation continuity.** A no-op edit while running must leave the output sample-
   identical across the reload; a gain edit inside a feedback network must not cost the tail. The
   first tests §8.2's fade and swap shortcut, the second §8.1's shape pass.
5. **UI completeness.** For every impulse-corpus program, the extracted UI tree must match the
   `User Interface` section of the shipped `.fir` — same widgets, nesting, labels and
   `init`/`min`/`max`/`step` (§11.1). Already checked in, so no second tool; reference Faust's JSON
   extends it to the wider corpora if wanted. The corpus exercises every §7.2 label mechanism:
   labels that open their own groups, `../` escapes, non-`v`/`h`/`t` prefixes, iterated widgets.
6. **Control responsiveness.** Writing a UI field must change the output within one block, and
   change *only* the outputs that depend on it. Catches rate-scheduling bugs that put a control read
   in the wrong band, otherwise nearly invisible.

### 11.6 Fuzzing and oracle hygiene

Grammar-directed differential fuzzing — random arity-correct terms compiled by both compilers,
impulse responses compared — finds the evaluator and normalization bugs a curated corpus misses, and
is cheap once the harness exists. Extend it with random *edit sequences* to attack property 2.

Build the oracle from the submodule. The `faust` on this machine's `PATH` is 2.66.9 against the
submodule's 2.85.9, and the checked-in reference files were generated with 2.81.0 — regenerate from
the pinned submodule rather than trusting them. The submodule's version strings disagree with its tag —
`git describe` gives 2.85.9-25-g515dc515c while `build/CMakeLists.txt:5` and `COPYING.txt` say 2.87.2 —
so a regenerated `.box` carries `declare version "2.87.2"`, a line §11.1 drops.

---

## 12. Implementation

**C++23.** The parts that matter for this design:

- **Data-oriented IR.** Flat arrays, `uint32_t` indices instead of pointers, structure-of-arrays for
  the Signal DAG. Opcode switches instead of virtual dispatch. Both the simplicity and the speed come
  from here, and it is the opposite of the reference's visitor-heavy style.
- **`std::expected`** for diagnostics, matching §9's errors-as-values rule.
- **Interning everywhere it matters**: term values, environments, signal nodes, strings — with
  identity carried as the interned id, never as the hash (§4.6).
- **Deterministic containers** on any path that affects output (§5.5).

**Build: CMake.** Decided by LLVM, which ships `find_package(LLVM)` and expects it; every alternative
means hand-rolling discovery for the one dependency that is hardest to discover.

**Dependencies, deliberately few, and every one a submodule under `lib/`:** tree-sitter's C runtime,
the `tree-sitter-faust` grammar (§5.1), and doctest for unit tests, chosen on compile speed. The
application layer adds three more — SDL3, Dear ImGui and miniaudio (§13.1) — and nothing under `src/`
links any of them. The conformance and property suites are their own binary, with flags for which
corpus, which level, and regenerate or compare. Fuzzing (§11.6) uses libFuzzer, built into clang.
LLVM is optional and confined to the release backend behind an interface the interpreter also
implements.

**The embedded standard library is a generated translation unit.** A CMake step walks
`lib/faust/libraries/` **recursively** and emits one `.cpp` of `std::string_view` constants plus a
sorted table keyed by path *relative to that directory*, which §5.6's overlay reads as its bottom
layer. Recursive and path-keyed rather than `*.lib` flat, because top-level libraries import by
subdirectory path — `"dx7/operator.lib"`, `"old/music.lib"` — so a flat walk of the 43 top-level files
misses 13 and breaks `dx7` and the deprecated aliases. The full set is 56 files, 2.4 MB of source. The
step stamps the `faustlibraries` submodule SHA into the generated file, since §11.6's "regenerate
rather than trust" holds only if the embedded library and the oracle came from the same commit.

**Embedding is redistribution, and faustlibraries' terms are not uniformly stated.** The facts at
the pinned commit `ecf2fdc23`: the Faust *compiler* is **LGPL 2.1 or later**
(`lib/faust/COPYING.txt`), which constrains nothing here, since §1 neither links nor ports it and
running it as an oracle is ordinary use. The *libraries* are a separate repository with **no
top-level license file**: `libraries/licenses/` holds only `stk-4.3.0.md`, six of the 43 top-level
`.lib` files carry a `declare license` line (five `"LGPL with exception"`, one `"LGPL"`),
`filters.lib` and `wdmodels.lib` embed MIT text inline, around ten credit STK 4.3, and the rest
state nothing.

The decision is **embed, unmodified, and ship the provenance**:

- Every embedded file is byte-identical to the submodule, and §5.6's *eject* is the only path to a
  modified library — it writes into the user's workspace, so the distribution never contains a
  changed copy. That is the condition the "LGPL with exception" wording turns on.
- A build step emits `THIRD-PARTY-NOTICES` from `libraries/licenses/*`, every `declare license`
  line, the pinned SHA and the upstream URL — the whole obligation for the files that do state
  terms.
- The files that state nothing stay an open question upstream. Asking GRAME for a repository-level
  license is the fix, worth doing before any commercial distribution, and not something this design
  can settle unilaterally.

Not embedding is the alternative, and it costs filesystem-free operation, the absent install step
and version-skew immunity — three of the four reasons embedding is here (§5.6). Not worth paying to
avoid a notice file.

```
faustlens/
  src/
    syntax/      CST wrapper, Term values + refs, interning, Merkle
                 hashing, provenance side tables, printer               (§4.6)
    files/       overlay VFS, import resolution, embedded stdlib          (§5.6)
    eval/        evaluator, pattern matching, environments, memo cache
    box/         Box graph, arity
    signal/      flat DAG, hash-consing, normalization
    analysis/    type, interval
    plan/        scheduling, state allocation, three-address emission
    runtime/     artifact + descriptors, instance lifecycle, UI tree,
                 soundfile loading, foreign symbol registry            (§7)
    backend/
      interp/    bytecode + dispatch loop
      llvm/      optional; LLVM IR emission + ORC JIT
    query/       memoized query engine, revisions, invalidation,
                 arena lifetimes                                    (§5.8, §5.9)
    live/        instance hand-off, state migration, crossfade
  app/           the program; the only unbounded dependencies                  (§13)
    editor/      text pane: buffer, highlighting, cursor-preserving splices  (§13.2)
    boxview/     derived layout, wires, selection, structural edits          (§13.4)
    controls/    UI tree -> ImGui widgets                                    (§13.5)
    host/        audio device, soundfile decoding, snapshot plumbing  (§13.3, §13.5)
  test/
    conformance/ driven by lib/faust corpora, including the parsers for the
                 reference `.sig` and `.fir` dumps and the `.ir` harness
                 protocol                                                   (§11.1)
    property/    the six invariants of §11.5
    fuzz/
  lib/           every submodule
    faust/               oracle only, never linked                            (§11)
    tree-sitter/         C runtime
    tree-sitter-faust/   adopted upstream grammar                           (§5.1)
    sdl3/ imgui/ miniaudio/   app layer only                                (§13.1)
    doctest/             unit tests
```

---

## 13. The application layer

Everything above is a library; this is the program that uses it, and the only place the unbounded
dependencies live. Nothing under `src/` links them (§12), so the conformance and property suites
never see this section.

### 13.1 The stack

**SDL3 for platform, SDL_GPU natively, WebGPU on web.** SDL_GPU is in-tree in SDL3 and dispatches to
Metal, Vulkan and D3D12, so one submodule covers windowing, input and a modern native renderer across
all three desktops. The platform half is identical on both targets, so the web build swaps one file:

| | Platform backend | Renderer backend |
|---|---|---|
| Native | `imgui_impl_sdl3` | `imgui_impl_sdlgpu3` |
| Web | `imgui_impl_sdl3` | `imgui_impl_wgpu` |

**Dear ImGui**, docking-capable branch — the layout is three panes plus a control surface, and
hand-rolling that is work the toolkit has done. **miniaudio** for device I/O, which §13.5 shows pays
twice.

Two rejected alternatives. **OpenGL** is dead on macOS, capped at 4.1, and was the only thing making
a GLFW pairing attractive. **WebGPU everywhere via Dawn** is tidier — one API native and web, with no
header skew, emdawnwebgpu being the same implementation family — and costs a Dawn submodule that
would be the largest and slowest-building dependency in the project. A bad trade for a renderer that
draws rectangles and text.

The escape hatch, should SDL3's emscripten port disappoint for being newer than SDL2's:
`imgui_impl_metal` natively and WebGPU on web. No native GPU dependency at all, since Metal is in the
SDK, macOS-only native, and no shared platform layer.

### 13.2 The text pane is ours, not a widget

The one app-layer decision with consequences, and §4.3 already forced it. Text is the source of
truth, undo is a text-level operation, the buffer *is* §5.6's layer-1 open buffer, and the compiler
is a co-author of it through splices. An editor widget — ImGuiColorTextEdit and its forks — owns the
buffer, owns the undo stack, and speaks line and column: three mismatches against a design where all
three belong to someone else. Draw the pane against `ImDrawList` and own the buffer.

The contract is small and set entirely by sections above: byte offset to screen position and back
(§4.3's cursor linking), arbitrary byte-range highlight sets (selection, diagnostics, hole extents),
splices that preserve the cursor and push one undo entry, and highlighting driven from the CST rather
than from a second lexer.

**Storage is a flat `std::string` plus a line-start index.** No rope, no piece table: the largest
corpus file is well under a megabyte, edits are a `memmove`, and a contiguous buffer is exactly what
`ts_parser_parse_string` wants — which §5.1 permits, having already declined to depend on incremental
reparse.

### 13.3 The view snapshot

§8.2 gives three threads and specifies two. The UI thread's half:

- It owns the buffers and all ImGui state. It never blocks on the compile thread and never calls a
  query.
- Edits post to the queue §5.8's engine already drains; the UI does not wait for the result.
- The compile thread publishes an immutable **view snapshot** after each drain:
  `{revision, per-file ref trees, diagnostic set, UI tree}`. The UI renders the newest one, which may
  lag the buffer by one compile. At sub-millisecond parse and evaluate that is invisible, and it is
  the same divergence §8.3 already makes visible for audio.
- Syntax highlighting comes from the snapshot too, so one source of truth says what the text means.

**A snapshot may hold only permanent ids.** Term value ids and ref trees are (§5.9), so the
structural view is safe by construction — it renders Term. §4.2's evaluated view is not: expanding a
node reads the eval arena, and arenas are dropped whole on idle. So expansion copies a materialized
render list out at publish time rather than holding box ids across frames, which is §5.9's "nothing
outside an arena holds an arena id" holding for the UI as it does for the running instance.

### 13.4 The box view

**Position is derived, never user-owned.** The whole design follows from it. Term is a tree, so each
node lays its children out in a fixed pattern by kind — left to right for `:`, stacked for `,`, a fan
for `<:` and `:>`, a return path for `~` — and computes its bounding box bottom-up in one recursive
pass, memoized per value id. Wires only ever connect siblings inside one composition node, so there
is no edge routing, no crossing minimization, and no layout solver.

**The deeper reason is that it keeps §4.4's lens asymmetric.** Everything on screen is a function of
the term, so the diagram holds no private information. A transformation whose two sides both hold
private state is a **symmetric lens** (Hofmann, Pierce & Wagner, POPL 2011), needing a persistent
**complement** for what each side knows that the other does not, kept consistent under every edit.
Derived layout removes the complement rather than managing it.

So there is **no node-editor library.** `imgui-node-editor` exists to manage free-positioned graphs
with user-owned coordinates and persisted layout, exactly the state this refuses to have: nothing to
save, nothing to hand-place, and no way for the diagram to drift from the source. `ImDrawList` plus a
few hundred lines of shape primitives covers it.

Interaction, mapping onto §4.3's edit catalogue:

- **Selection is a ref**, and it drives both panes (§4.3). Arrow keys walk the term structurally —
  parent, child, sibling — which is the reason to have a structural view at all.
- **Editing is selection plus a key.** Delete removes a stage. Typing `:` `,` `<:` `:>` `~` wraps the
  selection in that composition. Enter on a literal or a UI parameter opens an inline field scoped to
  that node, committing on blur. A click in the gap between two stages opens a completion popup and
  inserts. Rewiring a `route` is the one genuine drag, between port endpoints inside the node.
- **Every one of these is a term rewrite plus a splice**, so the text pane visibly changes on each and
  the box view has no private edit path. Keyboard-first, with the mouse for navigation and selection,
  which is what derived layout affords.
- **Expansion is in-place.** A node expands to show its evaluated form inline, badged read-only, and
  collapses back, reusing the structural view's selection model and linking rules for a view entered
  to answer one question and then left.

### 13.5 Control surface and audio host

miniaudio's data callback maps onto §7.1's `compute` with no adapter, the frame count being
unconstrained there already, and §7.1's FTZ and DAZ settings are established at the top of that
callback thread rather than in `main`.

It pays twice: `ma_decoder` handles wav, flac and mp3 in-tree, so §7.3's decoding requirement needs
no further dependency, and the host holds the decoded buffers in the per-URL cache §7.3 asks for
across recompiles.

The control surface renders §7.2's UI tree directly — groups, sliders, buttons and bargraphs from a
descriptor tree is close to a transcription in an immediate-mode toolkit, writing through the relaxed
atomics of §8.2. It is also where the metadata the compiler carries but does not act on earns its
keep: `style:knob`, `scale:log`, `unit:Hz`, `hidden:1`. §7.1's channel mismatch surfaces here rather
than silently producing quiet.

### 13.6 The web build

The renderer swap of §13.1 is the easy part. What decides whether a browser build is real:

- **Threads.** §8.2 wants three real ones, which means `-pthread`, which means `SharedArrayBuffer`,
  which means serving under COOP/COEP cross-origin isolation. A deployment constraint rather than a
  code one, and it rules out a static host that will not set headers.
- **Audio.** miniaudio has an emscripten backend over Web Audio, so §7.1's contract carries across
  unchanged. Under cross-origin isolation it is an AudioWorklet on its own thread — the same
  real-time discipline §8.2 already demands. **FTZ and DAZ have no wasm equivalent**, so the web
  build's denormal behaviour differs from native. This affects live audio only: §7.1 already runs the
  conformance harness with both off to match the oracle.
- **The interpreter tier only.** §6.5 already makes LLVM optional, so this is free, and it is why
  §6.4's throughput risk lands hardest here.
- **No filesystem.** §5.6's embedded standard library was justified partly on this, and it pays off
  exactly here: the browser build resolves every `import` with nothing behind it. The workspace layer
  becomes browser storage, or nothing.

---

## 14. Phases

Each exits on a passing test suite, not a judgment call.

**Phase 1 — Skeleton and oracle.** Vendor `tree-sitter-faust` and close its three gaps (§5.1): CST
fidelity over the corpora, an error-recovery corpus of mid-keystroke fragments graded by hole extent,
and the node-granularity audit against §4.5. The §4.5 Term inventory — `Hole` included — as §4.6's
value and ref layers, with interning, Merkle hashing, the provenance side tables, the printer, and the
**splice primitive** (`print_in_context`, then replace a ref's byte range) — a dozen lines over the
printer, and property 1's GetPut and canonizer laws cannot be stated without it. The overlay VFS and
embedded standard library (§5.6, §12) with its notice step, since nothing in the corpus parses
without resolving `import`. Oracle harness: build reference Faust at `regular.cmake` + `FIR_BACKEND`
and regenerate all five levels at `-double` per §11.1, against the `faustlibraries` checkout of
§11.4.
*Exit:* §11.5 property 1 over the 319 test files reference Faust accepts, all 296 examples, and
faustlibraries. The other 22 are pinned as *failures* (§5.1), where a parse signals grammar drift.
Plus the hole-extent property over the truncation corpus, and a regenerated reference set matching
the shipped files wherever precision does not differ. Two cheap drift tripwires land here too: a test
asserting `grammar.js`'s `PREC` block and operator spellings against §4.7's table row for row — the
one fact written down in three places — and §4.5's `Hole` rule 4 failing the build on any corpus
file.

**Phase 2 — Evaluation.** The §6.1 evaluator: abstraction, pattern matching, `with`/`letrec`/
`environment`, `import`/`component`/`library`, iterations, metadata, label substitution (§7.2). Box
graph with arity checking. Memoization and the query engine of §5.8 — the five queries, early cutoff,
the resolution map, the import-cycle rule. The §6.1 probes land here too, including the ones for its
open questions — they are how those get settled, so they are phase work rather than follow-up. Then
the first app work: the §13.2 text pane and §13.3 snapshot, buildable the moment ref trees and
diagnostics exist, and the debugging instrument for every phase after.
*Exit:* `.box` isomorphism on all 93 reference `.box` files; whole corpus evaluates; editing a
library file invalidates exactly its dependents. Plus, in the pane, a diagnostic marked at *every*
occurrence of its value id and a cursor that resolves to the innermost ref — §4.6's two ref-tree
walks, which nothing before this exercises.

**Phase 3 — Signal and numerics.** Propagation, the flat hash-consed DAG with `Rec` groups,
normalization (canonical ordering plus constant folding — the simple version, per §11.2), promotion,
the soundfile read clamp. The parser for the reference `.sig` notation (§11.1) lands here, since
nothing in this phase can be checked without it. Also the **read-only box view** (§13.4): derived
layout and selection linking, no editing. It is tooling — reading the graph being propagated pays for
itself here and in phase 4 — and it tests §13.4's premise against the live backend, which is what
§15 asks for.
*Exit:* `.sig` isomorphism on all 93 reference `.sig` files, against the unnormalized dump first and
the normalized one second, plus the `norm*`/`math_simp` probes. Where isomorphism fails only by
association order, record it and defer to phase 5, where `filesCompare` decides whether it matters.
Layout is total: every Term node kind has a shape rule and every corpus program renders, with
selection linking both ways.

**Phase 4 — Analysis, Plan, and descriptors.** Type and interval analyses, including the
`assertbounds`/`lowest`/`highest` overrides and the table index clamping that depends on them. Rate
scheduling, max-delay-per-signal and state allocation with both identities (§8.1), three-address
emission including the loop and guard brackets. UI tree extraction (§7.2) and the soundfile/foreign
descriptors, since a Plan without them is not yet an artifact.
*Exit:* `.type` parity on all 92 reference `.type` files (modulo the projection of §11.1), `.fir`
parity on all 93 — same state fields with the same shapes, same band per computation, under §11.1's
band mapping and with table storage excluded on both sides — no program in the corpus rejected for
an unbounded delay that reference Faust accepts, and §11.5 property 5 against the `.fir` UI sections.

**Phase 5 — Interpreter and first sound.** Bytecode backend including guards, bounded init loops,
soundfile reads and foreign thunks. The §7.1 lifecycle, channel mapping, symbol registry, and
host-side soundfile loading. The §13.5 audio host and control surface, and end-to-end playback.
*Exit:* `.ir` impulse parity across all four corpora — the first two sections of each of the 94
usable reference files, per §11.1 — plus §11.5 property 6 and the §11.3 semantic probes. Phase 3's
deferred normalization question settles here: any program that fails `filesCompare` gets the
reference's ordering ported for its case, and nothing more. Interpreter throughput measured on a
realistic program, since §15 depends on the answer.

**Phase 6 — Live loop.** Query-driven recompile on edit, state migration, crossfaded hand-off,
atomic control writes, and the §8.3 failure policy — which matters more than the rest, since a live
editor spends most of its time not compiling.
*Exit:* §11.5 properties 2 and 4. Measured edit-to-audio latency on a `zita_rev1`-scale program, with
the profile broken out by phase to confirm where time actually goes.

**Phase 7 — Bidirectional editing.** The §4.3 edit catalogue as term rewrites over phase 1's splice
primitive, driven by §13.4's interaction model, plus expand and materialize. Everything that
*chooses* a rewrite; the mechanism that applies one already exists and is already under property 1,
and the view that displays one has been up since phase 3.
*Exit:* every structural edit on every reference program produces correct localized diffs, and
identity edits produce byte-identical source.

**Phase 8 — LLVM tier.** LLVM IR emission, ORC JIT, ahead-of-time export.
*Exit:* §11.5 property 3 across the corpus; performance measured against reference Faust's C++
backend.

Phase 5 is the first point at which the system is *playable* — a batch compiler with a runtime,
worth having on its own. Phases 6 and 7 are the product.

---

## 15. Risks

**Normalization ordering** (§11.2). Highest severity, latest surfacing. Mitigated by measuring rather
than by a speculative port, which makes the phase 5 impulse run load-bearing: it is the only thing
standing between a simple normalizer and silent numeric drift.

**The reference implementation is the specification.** Faust's published papers cover the algebra but
not the evaluator's corner cases — pattern-match ordering, `with` scoping interactions, first-class
`environment` capture, shadowing in `letrec`. Those get resolved by reading
`compiler/evaluate/eval.cpp` and by differential fuzzing, not from documentation.

**Interpreter throughput.** If per-sample dispatch cannot sustain a realistic program in real time,
§6.4's block-wise escalation is a significant complication. Measure early, in phase 5, so the answer
is known before phase 6 depends on it.

**Incremental equivalence bugs.** Memoizing compilers silently serve stale results. Property 2 and
edit-sequence fuzzing are the defense, and both need to exist from phase 2 rather than be added
later. §5.8 narrows the exposure structurally — only the five path-addressed queries can go stale,
since everything below Term is keyed on content — so the surface property 2 has to cover is small
enough to reason about directly.

**Label-path threading.** Faust encodes its UI hierarchy in labels rather than in the graph, through
three mechanisms that all thread past code with nothing else to do with the UI (§7.2). Getting any
of them wrong produces a tree that looks plausible and nests incorrectly. §11.5 property 5 exists to
catch this and should be in place as soon as phase 4 emits a UI tree.

**Editing UX is unproven.** Whether a Term-graph-as-boxes view is pleasant to edit is a design
question no amount of compiler correctness answers, and phase 7 is where the product's central
premise gets tested. The mitigation is to meet it early and for real: §14 puts the text pane in phase
2 and the read-only box view in phase 3, both against the live backend, so the layout and the linking
are known to be pleasant or not long before anything depends on them.

---

## 16. Decisions, and why

1. **No MLIR.** It contributes nothing to bidirectional editing (the primary goal), its per-op
   allocation and post-pass verification are overhead on the edit loop, and its affine/vector
   machinery does not apply to Faust's output shape — a basic block in a 1-D loop with a serial
   dependence. A large framework dependency bought for a testing methodology and a verifier that
   construction-site checking provides for free. Defensible for the parity-focused design in
   `DESIGN-mlir-rejected.md`; not for this one.
2. **Term is editable, Box is a read-only view.** Dissolves the round-tripping problem instead of
   solving it.
3. **Flat hash-consed DAG, not a pointer graph.** Simplicity, speed, free CSE, and the foundation of
   both incrementality and state identity.
4. **Three-address Plan.** One shared lowering makes both backends nearly trivial, replacing the
   reference's FIR plus per-backend emitters.
5. **Two tiers, LLVM optional.** The edit loop must never wait on LLVM; a release build must not
   compromise on speed. Different problems, different answers.
6. **Merkle hashing, not tree diffing — over interned values, with provenance beside them.** Id
   equality is an exact diff. Terms split into provenance-free interned values and per-file refs
   holding the byte ranges, so the hash covers structure alone and every filter's state survives a
   stray newline (§4.6). tree-sitter is kept for error recovery and trivia, and the grammar is
   adopted (§5.1).
7. **Errors are local values, from the parse down.** Every phase emits diagnostics and continues.
   Unparsable text is a `Hole` in Term that keeps its recognizable children and prints back verbatim
   (§4.5), and it evaluates to an `Error` box of unconstrained arity (§6.1) so one typo yields one
   diagnostic rather than one per enclosing composition.
8. **State identity from node hashes — two of them.** State-preserving reload falls out of the
   hashing incrementality already requires. Merkle hashes propagate upward and state lives at the
   top, so a literal-normalized **shape hash** is the second key, holding a reverb tail across an
   edit to a constant inside it (§8.1). It is the one key left as a hash rather than an interned id,
   being many-to-one by design.
9. **One symbol registry for both tiers.** Faust's foreign constructs split into runtime-provided
   constants (`fSamplingFreq`, which every program using `ma.SR` needs) and genuine external calls
   (rare). A single (name, signature) -> pointer registry serves both, so the interpreter can call
   native code and LLVM stays optional. Unresolved symbols poison a subgraph rather than failing the
   compile.
10. **The UI tree is a descriptor, not a visitor.** Extracting groups, widgets and metadata into data
    at compile time removes the reference's `buildUserInterface` callback machinery, and the JSON
    description becomes a serialization of it rather than a parallel implementation.
11. **The host owns file I/O.** The compiler emits soundfile *requirements*; decoding, search paths
    and caching live in the host. Missing files degrade to silence rather than failing the build.
12. **Files are input queries, behind an overlay VFS.** Open buffers shadow the importing file's own
    directory, which shadows the search path, which shadows an embedded standard library. That makes
    libraries live-editable, removes install steps and version skew, and lets the environment run with
    no filesystem at all. The importing-file layer is not decoration — the impulse corpus does not
    resolve without it (§11.4). Embedded copies are byte-identical to the pinned submodule and *eject*
    is the only way to modify one, which is also what settles redistribution (§12).
13. **The invalidating engine covers only what is named by a path.** Text, CSTs, terms, resolution
    and file environments are queries with revisions; everything below Term is keyed on interned
    content, so a changed program produces different keys rather than wrong answers at the same key
    (§5.8). Revision stamping with early cutoff, and interning is what makes the cutoff free.
14. **Two lifetimes, and the boundary is the same one.** Term values, environments and strings are
    permanent and append-only, because diagnostics and state identity quote their ids. Box, Signal,
    analyses and Plan live in arenas dropped whole on idle. Nothing outside an arena holds an arena id —
    the running instance keys state on hashes, not ids — so a drop costs one cold compile and nothing
    else (§5.9).
15. **On a failed compile, keep the last good instance.** It keeps playing while the editor shows
    the broken program (§8.3). Most keystrokes are invalid states, so this is the common path, not an
    error path.
16. **Match the reference where it has one behaviour; define one only where its backends genuinely
    disagree.** The first half is §11.3 throughout, odd results reproduced rather than corrected —
    `"a:Gain"` opening a *vertical* group, since `encodeName` maps every unrecognized prefix
    character to vertical (§7.2), is the sharpest. The second half has one case: out-of-range and NaN
    float-to-int conversion, UB by default and evaluated differently by the reference's own C++ and
    LLVM backends. It is specified once and implemented identically in both tiers, which is what
    makes §11.5 property 3 a real check, and the specification is the reference's own `-cir` rewrite
    — an existing option made mandatory, not a new semantic (§11.3).
17. **Cut fixed-point, vector modes, and eighteen backends.** Simplicity is the constraint, and §10
    prices each cut.
18. **Numeric fidelity to reference Faust is not cut** — but it is pursued by measurement (§11.2)
    and by pinning semantics (§11.3), not by porting the reference's implementation.
19. **The box view has no coordinates, and the text pane is ours.** Layout is derived from the term
    on every frame, so there is nothing to save, nothing to hand-place, and no way for the diagram to
    drift from the source — which is why `ImDrawList` and a few hundred lines of shape primitives
    cover it in place of a node-editor library. It also keeps §4.4's lens asymmetric: saved
    coordinates would be private view state, obliging a symmetric lens and its complement (§13.4).
    The editor is ours for the mirror-image reason: buffer, undo stack and splices all belong to
    sections above, so the pane owns them (§13.2, §13.4).
