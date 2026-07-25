# FaustLens: a bidirectional editor and compiler for Faust

A from-scratch Faust compiler built around bidirectional editing: a box graph and a text editor
showing the same program, either one editable, updating each other as you type, with audio running
throughout.

Named for the *lens* of bidirectional programming: the text and the diagram are two views of one
program, related by a `get` that renders and a `put` that splices edits back into the source. It is
a *retentive* lens specifically — an edit keeps the bytes of everything it did not change, comments
and formatting included ([DESIGN.md](DESIGN.md) §4.4).

Design stage, nothing implemented yet. [DESIGN.md](DESIGN.md) is the plan.
