# The compiler proper. Nothing here links an unbounded dependency, so the
# conformance and property suites never pull in a window or an audio device.

# --- Generated translation units -------------------------------------------
# The embedded standard library and the third-party notice file, both keyed on
# the faustlibraries checkout so the shipped library and the oracle provably
# come from one commit.

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(STDLIB_CPP "${GEN_DIR}/embedded_stdlib.cpp")
set(NOTICES "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES")

file(GLOB_RECURSE STDLIB_SOURCES CONFIGURE_DEPENDS
     "${FAUSTLENS_FAUSTLIBRARIES}/*.lib")
file(GLOB STDLIB_LICENSES CONFIGURE_DEPENDS
     "${FAUSTLENS_FAUSTLIBRARIES}/licenses/*")

add_custom_command(
    OUTPUT "${STDLIB_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DLIB_DIR=${FAUSTLENS_FAUSTLIBRARIES}
            -DOUT=${STDLIB_CPP}
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedStdlib.cmake"
    DEPENDS ${STDLIB_SOURCES} "${CMAKE_SOURCE_DIR}/cmake/EmbedStdlib.cmake"
    COMMENT "Embedding faustlibraries"
    VERBATIM)

add_custom_command(
    OUTPUT "${NOTICES}"
    COMMAND ${CMAKE_COMMAND}
            -DLIB_DIR=${FAUSTLENS_FAUSTLIBRARIES}
            -DOUT=${NOTICES}
            -P "${CMAKE_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake"
    DEPENDS ${STDLIB_SOURCES} ${STDLIB_LICENSES}
            "${CMAKE_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake"
    COMMENT "Generating THIRD-PARTY-NOTICES"
    VERBATIM)

add_custom_target(faustlens_notices ALL DEPENDS "${NOTICES}")

# --- syntax: lexer, parser, Term, printer, splice --------------------------

add_library(faustlens_syntax
    syntax/Token.cpp
    syntax/Lexer.cpp
    syntax/Term.cpp
    syntax/Prec.cpp
    syntax/Parser.cpp
    syntax/Printer.cpp
    syntax/Splice.cpp
    syntax/Edit.cpp
    syntax/Diagnostic.cpp)
target_include_directories(faustlens_syntax PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# --- files: overlay VFS, import resolution, embedded stdlib ----------------

add_library(faustlens_files
    files/Vfs.cpp
    "${STDLIB_CPP}")
target_include_directories(faustlens_files PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_files PUBLIC faustlens_syntax)

# --- box, eval, query: the evaluator and the memoized query engine ---------

add_library(faustlens_eval
    box/Box.cpp
    box/Label.cpp
    eval/Env.cpp
    eval/Fold.cpp
    eval/Eval.cpp
    eval/Apply.cpp
    eval/Lift.cpp
    query/Query.cpp
    query/Snapshot.cpp)
target_include_directories(faustlens_eval PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_eval PUBLIC faustlens_syntax faustlens_files)

# --- signal: propagation, the flat hash-consed DAG, analyses, Plan ---------

add_library(faustlens_signal
    signal/Signal.cpp
    signal/Simplify.cpp
    signal/Promote.cpp
    signal/Interval.cpp
    signal/Type.cpp
    signal/NormalForm.cpp
    signal/Propagate.cpp
    signal/Ui.cpp
    signal/Schedule.cpp
    signal/Plan.cpp)
target_include_directories(faustlens_signal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_signal PUBLIC faustlens_eval)

# --- runtime: the interpreter tier and the instance contract ---------------

add_library(faustlens_runtime
    runtime/Registry.cpp
    runtime/Soundfile.cpp
    runtime/Interp.cpp
    runtime/Migrate.cpp)
target_include_directories(faustlens_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_runtime PUBLIC faustlens_signal)

add_library(faustlens INTERFACE)
target_link_libraries(faustlens INTERFACE
    faustlens_syntax faustlens_files faustlens_eval faustlens_signal faustlens_runtime)
