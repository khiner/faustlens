add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${CMAKE_SOURCE_DIR}/lib/doctest/doctest")

set(FAUST_DIR "${CMAKE_SOURCE_DIR}/lib/faust")

# --- unit and property suites ----------------------------------------------

add_executable(faustlens_tests
    main.cpp
    unit/LexerTest.cpp
    unit/ParserTest.cpp
    unit/PrinterTest.cpp
    unit/SpliceTest.cpp
    unit/CatalogueTest.cpp
    unit/VfsTest.cpp
    unit/MdocTest.cpp
    unit/ConstructsTest.cpp
    unit/SelectTest.cpp
    unit/EditsTest.cpp
    unit/ExpandTest.cpp
    unit/TraceTest.cpp
    property/Corpus.cpp
    property/Retentive.cpp
    property/CorpusTest.cpp
    property/EditTest.cpp
    property/HoleExtentTest.cpp
    property/IncrementalTest.cpp
    property/LatencyTest.cpp
    conformance/BoxCompare.cpp
    conformance/BoxTest.cpp
    conformance/LiftTest.cpp
    conformance/SigParse.cpp
    conformance/FirParse.cpp
    conformance/FirTest.cpp
    conformance/UiTest.cpp
    conformance/ScheduleTest.cpp
    conformance/PlanTest.cpp
    conformance/ImpulseTest.cpp
    conformance/ControlTest.cpp
    conformance/SemanticsTest.cpp
    conformance/TypeParse.cpp
    conformance/TypeTest.cpp
    conformance/SigTest.cpp
    conformance/SigCompare.cpp
    conformance/PropagateTest.cpp
    conformance/ProbeTest.cpp
    unit/EvalTest.cpp
    unit/QueryTest.cpp
    unit/PaneTest.cpp
    unit/LayoutTest.cpp
    unit/AudioTest.cpp
    unit/ControlsTest.cpp
    unit/StoreTest.cpp
    unit/WorkspaceTest.cpp
    unit/MigrateTest.cpp
    unit/ReloadTest.cpp)
target_link_libraries(faustlens_tests PRIVATE faustlens faustlens_app doctest)
target_include_directories(faustlens_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(faustlens_tests PRIVATE
    FAUSTLENS_FAUST_DIR="${FAUST_DIR}"
    FAUSTLENS_BUILD_DIR="${CMAKE_BINARY_DIR}"
    # The soundfile decoder tests want an audio file of known shape, and miniaudio ships one.
    FAUSTLENS_MINIAUDIO_DIR="${CMAKE_SOURCE_DIR}/lib/miniaudio")

add_test(NAME unit COMMAND faustlens_tests)

# --- differential acceptance oracle against tree-sitter-faust ---------------
# An independently written grammar, so accept/reject parity over the corpora
# catches a lexer or layering mistake our own suites cannot see.

set(TS_DIR "${CMAKE_SOURCE_DIR}/lib/tree-sitter")
set(TSF_DIR "${CMAKE_SOURCE_DIR}/lib/tree-sitter-faust")

if(EXISTS "${TSF_DIR}/src/parser.c" AND EXISTS "${TS_DIR}/lib/src/lib.c")
    add_library(tree_sitter_faust STATIC
        "${TS_DIR}/lib/src/lib.c"
        "${TSF_DIR}/src/parser.c")
    if(EXISTS "${TSF_DIR}/src/scanner.c")
        target_sources(tree_sitter_faust PRIVATE "${TSF_DIR}/src/scanner.c")
    endif()
    target_include_directories(tree_sitter_faust PUBLIC
        "${TS_DIR}/lib/include" "${TSF_DIR}/src")
    target_include_directories(tree_sitter_faust PRIVATE "${TS_DIR}/lib/src")

    add_executable(faustlens_acceptance
        main.cpp
        property/Corpus.cpp
        conformance/AcceptanceTest.cpp)
    target_link_libraries(faustlens_acceptance PRIVATE faustlens doctest tree_sitter_faust)
    target_include_directories(faustlens_acceptance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(faustlens_acceptance PRIVATE
        FAUSTLENS_FAUST_DIR="${FAUST_DIR}")
    add_test(NAME acceptance COMMAND faustlens_acceptance)
else()
    message(WARNING "tree-sitter submodules missing; acceptance oracle not built")
endif()
