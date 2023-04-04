## Goals

* Create a new Faust parser
  - Use Tree-Sitter
* Create MLIR dialects for:
  - Faust Signal
  - Faust Box
  - Faust FIR
* Compile to LLVM that is functionally equivalent to Faust LLVM
  - Programs should compile to the same signal/box/FIR
  - Executing programs should generate the same audio samples


## Why?

### Parser

Current Faust parser does not support round-tripping from intermediate representations (Signal/Box/FIR) back to source code.
If this were possible, it may enable inferring source-code changes from e.g. visual modifications of Box graphs.
To modify a Faust program, one could either modify a graphical representation of the audio graph, or modify the source code, and both would be updated to reflect the resulting underlying IR.

Additionally, using Tree-Sitter as a parser would allow for partial recompilation of only the modified section (of the code or the graph).
This would enable real-time updates to subsections of very large Faust programs.


### MLIR

If the entire path from AST to LLVM (and other code-gen targets) took place via transformations between MLIR dialects, this would have the following benefits:
* Greatly simplify Faust core, which would
  - Dramatically reduce the amount of code
  - Make the code base more understandable and extendible
  - Improve compile times
* Enable easy compilation to other MLIR dialects, such as ONNX or PyTorch for autograd at the signal level
