add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${CMAKE_SOURCE_DIR}/lib/doctest/doctest")

set(FAUST_DIR "${CMAKE_SOURCE_DIR}/lib/faust")

add_executable(faustlens_tests
    main.cpp
    unit/LexerTest.cpp
    unit/ParserTest.cpp
    unit/PrinterTest.cpp
    unit/SpliceTest.cpp
    unit/ExpandTest.cpp
    unit/OriginsTest.cpp
    unit/CatalogueTest.cpp
    unit/VfsTest.cpp
    unit/MdocTest.cpp
    unit/ConstructsTest.cpp
    property/Corpus.cpp
    property/Retentive.cpp
    property/CorpusTest.cpp
    property/EditTest.cpp
    property/HoleExtentTest.cpp
    property/IncrementalTest.cpp
    conformance/BoxCompare.cpp
    conformance/BoxTest.cpp
    conformance/LiftTest.cpp
    conformance/SigParse.cpp
    conformance/FirParse.cpp
    conformance/FirTest.cpp
    conformance/UiTest.cpp
    conformance/PlanTest.cpp
    conformance/ImpulseTest.cpp
    conformance/ControlTest.cpp
    conformance/SemanticsTest.cpp
    conformance/TypeParse.cpp
    conformance/TypeTest.cpp
    conformance/SigTest.cpp
    conformance/SigCompare.cpp
    conformance/PropagateTest.cpp
    unit/EvalTest.cpp
    unit/QueryTest.cpp
    unit/MigrateTest.cpp
    unit/NativeTest.cpp
    unit/DifferentiateTest.cpp
    unit/ArtifactTest.cpp)
target_link_libraries(faustlens_tests PRIVATE faustlens doctest)
target_include_directories(faustlens_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(faustlens_tests PRIVATE
    FAUSTLENS_FAUST_DIR="${FAUST_DIR}"
    FAUSTLENS_BUILD_DIR="${CMAKE_BINARY_DIR}")

faustlens_sign_jit(faustlens_tests)
add_test(NAME unit COMMAND faustlens_tests)

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

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    add_executable(faustlens_artifact_test runtime/ArtifactRunner.cpp)
    target_link_libraries(faustlens_artifact_test PRIVATE faustlens_native)
    add_dependencies(faustlens_artifact_test faustlens_cli)
    faustlens_sign_jit(faustlens_artifact_test)
    add_test(NAME artifact_compile COMMAND faustlens_cli
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/artifact.dsp" -o "${CMAKE_CURRENT_BINARY_DIR}/artifact.f64")
    set_tests_properties(artifact_compile PROPERTIES FIXTURES_SETUP artifact)
    add_test(NAME artifact_run COMMAND faustlens_artifact_test "${CMAKE_CURRENT_BINARY_DIR}/artifact.f64")
    set_tests_properties(artifact_run PROPERTIES FIXTURES_REQUIRED artifact)
endif()
