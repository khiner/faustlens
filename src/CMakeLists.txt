set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(STDLIB_CPP "${GEN_DIR}/embedded_stdlib.cpp")
set(NOTICES "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES")

file(GLOB_RECURSE STDLIB_SOURCES CONFIGURE_DEPENDS
     "${FAUSTLENS_FAUSTLIBRARIES}/*.lib")
file(GLOB STDLIB_LICENSES CONFIGURE_DEPENDS
     "${FAUSTLENS_FAUSTLIBRARIES}/licenses/*")

add_custom_command(
    OUTPUT "${STDLIB_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DLIB_DIR=${FAUSTLENS_FAUSTLIBRARIES}
            -DOUT=${STDLIB_CPP}
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedStdlib.cmake"
    DEPENDS ${STDLIB_SOURCES} "${PROJECT_SOURCE_DIR}/cmake/EmbedStdlib.cmake"
    COMMENT "Embedding faustlibraries"
    VERBATIM)

add_custom_command(
    OUTPUT "${NOTICES}"
    COMMAND ${CMAKE_COMMAND}
            -DLIB_DIR=${FAUSTLENS_FAUSTLIBRARIES}
            -DOUT=${NOTICES}
            -P "${PROJECT_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake"
    DEPENDS ${STDLIB_SOURCES} ${STDLIB_LICENSES}
            "${PROJECT_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake"
    COMMENT "Generating THIRD-PARTY-NOTICES"
    VERBATIM)

add_custom_target(faustlens_notices ALL DEPENDS "${NOTICES}")

add_library(faustlens_syntax
    syntax/Token.cpp
    syntax/Lexer.cpp
    syntax/Term.cpp
    syntax/Prec.cpp
    syntax/Parser.cpp
    syntax/Diagnostic.cpp)
target_include_directories(faustlens_syntax PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(faustlens_syntax PUBLIC cxx_std_23)

add_library(faustlens_files
    files/Vfs.cpp
    "${STDLIB_CPP}")
target_include_directories(faustlens_files PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_files PUBLIC faustlens_syntax)
add_dependencies(faustlens_files faustlens_notices)

add_library(faustlens_eval
    box/Box.cpp
    box/Label.cpp
    eval/Env.cpp
    eval/Fold.cpp
    eval/Eval.cpp
    eval/Apply.cpp
    query/Query.cpp)
target_include_directories(faustlens_eval PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_eval PUBLIC faustlens_syntax faustlens_files)

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
    signal/Plan.cpp
    signal/Instruction.cpp)
target_include_directories(faustlens_signal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_signal PUBLIC faustlens_eval)

add_library(faustlens_layout runtime/Layout.cpp)
target_link_libraries(faustlens_layout PUBLIC faustlens_signal)

add_library(faustlens_arm64 arm64/Compile.cpp arm64/Artifact.cpp)
target_link_libraries(faustlens_arm64 PUBLIC faustlens_layout)

add_library(faustlens_runtime
    runtime/Registry.cpp
    runtime/Soundfile.cpp
    runtime/Instance.cpp)
target_include_directories(faustlens_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_runtime PUBLIC faustlens_layout)

add_library(faustlens_compiler INTERFACE)
target_link_libraries(faustlens_compiler INTERFACE faustlens_arm64)

add_library(faustlens_native runtime/Native.cpp runtime/Executable.cpp)
target_link_libraries(faustlens_native PUBLIC faustlens_runtime faustlens_arm64)

add_library(faustlens_interp EXCLUDE_FROM_ALL runtime/Interp.cpp)
target_link_libraries(faustlens_interp PUBLIC faustlens_runtime)

add_library(faustlens_backends EXCLUDE_FROM_ALL runtime/Backend.cpp)
target_link_libraries(faustlens_backends PUBLIC faustlens_native faustlens_interp)

add_library(faustlens_migrate EXCLUDE_FROM_ALL runtime/Migrate.cpp)
target_link_libraries(faustlens_migrate PUBLIC faustlens_runtime)

add_library(faustlens_lens EXCLUDE_FROM_ALL
    syntax/Printer.cpp
    syntax/Splice.cpp
    syntax/Edit.cpp
    eval/Lift.cpp
    query/Snapshot.cpp)
target_link_libraries(faustlens_lens PUBLIC faustlens_eval)

add_library(faustlens INTERFACE)
target_link_libraries(faustlens INTERFACE
    faustlens_compiler faustlens_backends faustlens_migrate faustlens_lens)

# Preserve the numerical contract in host-compiled DSP operations.
target_compile_options(faustlens_eval PRIVATE -ffp-contract=off -fno-fast-math)
target_compile_options(faustlens_signal PRIVATE -ffp-contract=off -fno-fast-math)
target_compile_options(faustlens_runtime PRIVATE -ffp-contract=off -fno-fast-math)
target_compile_options(faustlens_arm64 PRIVATE -ffp-contract=off -fno-fast-math)
target_compile_options(faustlens_native PRIVATE -ffp-contract=off -fno-fast-math)
target_compile_options(faustlens_interp PRIVATE -ffp-contract=off -fno-fast-math)
