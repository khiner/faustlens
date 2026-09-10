set(FAUST_REFERENCE "${CMAKE_SOURCE_DIR}/lib/faust/build/bin/faust")
if(NOT EXISTS "${FAUST_REFERENCE}")
    message(FATAL_ERROR "Build the pinned reference Faust compiler before enabling FAUSTLENS_BENCHMARKS; see README.md")
endif()

file(GLOB_RECURSE REFERENCE_LIBRARIES CONFIGURE_DEPENDS "${FAUSTLENS_FAUSTLIBRARIES}/*.lib")

file(GLOB CASE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dsp/*.dsp")
set(DECLARATIONS "")
set(FACTORIES "")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED}")
add_executable(faustlens_benchmark Main.cpp)
add_library(faustlens_references STATIC)
target_link_libraries(faustlens_benchmark PRIVATE faustlens_app faustlens_references)
target_include_directories(faustlens_references PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${GENERATED}" "${CMAKE_SOURCE_DIR}/lib/faust/architecture")
target_compile_options(faustlens_references PUBLIC -O3 -ffp-contract=off -fno-fast-math)
target_compile_definitions(faustlens_benchmark PRIVATE BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dsp")

foreach(source IN LISTS CASE_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    foreach(mode scalar vector)
        set(id "${name}_${mode}")
        if(mode STREQUAL "vector")
            set(FLAGS -vec -vs 32)
        else()
            set(FLAGS -scal)
        endif()
        set(output "${GENERATED}/${id}.cpp")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${FAUST_REFERENCE}" -double ${FLAGS} -cn "Dsp_${id}"
                    -I "${FAUSTLENS_FAUSTLIBRARIES}" -a "${CMAKE_CURRENT_SOURCE_DIR}/Reference.cpp.in"
                    "${source}" -o "${output}"
            DEPENDS "${FAUST_REFERENCE}" "${source}" Reference.cpp.in Reference.h ${REFERENCE_LIBRARIES}
            VERBATIM)
        set_source_files_properties("${output}" PROPERTIES COMPILE_DEFINITIONS "BENCH_CLASS=Dsp_${id};BENCH_FACTORY=Reference_${id}")
        target_sources(faustlens_references PRIVATE "${output}")
        string(APPEND DECLARATIONS "Reference Reference_${id}();\n")
    endforeach()
    string(APPEND FACTORIES "{\"${name}\", Reference_${name}_scalar, Reference_${name}_vector},\n")
endforeach()
file(WRITE "${GENERATED}/Cases.h" "#pragma once
#include \"Reference.h\"

${DECLARATIONS}\nstruct Case { const char *Name; Reference (*Scalar)(); Reference (*Vector)(); };\ninline const Case Cases[] = {\n${FACTORIES}};\n")
faustlens_sign_jit(faustlens_benchmark)

foreach(mode interp native)
    add_executable(faustlens_footprint_${mode} Footprint.cpp)
    target_link_libraries(faustlens_footprint_${mode} PRIVATE faustlens_interp)
    if(mode STREQUAL "native")
        target_compile_definitions(faustlens_footprint_${mode} PRIVATE BENCH_NATIVE=1)
        target_link_libraries(faustlens_footprint_${mode} PRIVATE faustlens_native)
    endif()
    faustlens_sign_jit(faustlens_footprint_${mode})
endforeach()

add_executable(faustlens_compile Compile.cpp)
target_link_libraries(faustlens_compile PRIVATE faustlens_native faustlens_interp)
faustlens_sign_jit(faustlens_compile)

set(FAUSTLENS_LLVM_LIBRARY "" CACHE FILEPATH "Pinned libfaust with LLVM for JIT benchmarks")
if(FAUSTLENS_LLVM_LIBRARY)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "JIT comparison requires Apple Silicon")
    endif()
    foreach(mode native llvm)
        add_executable(faustlens_jit_${mode} Jit.cpp)
        target_link_libraries(faustlens_jit_${mode} PRIVATE faustlens_references)
        target_compile_definitions(faustlens_jit_${mode} PRIVATE
            BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dsp"
            BENCH_LIBRARY_DIR="${FAUSTLENS_FAUSTLIBRARIES}")
    endforeach()
    faustlens_sign_jit(faustlens_jit_native)
    set_property(TARGET faustlens_jit_llvm APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Llvm.entitlements")
    add_custom_command(TARGET faustlens_jit_llvm POST_BUILD
        COMMAND /usr/bin/codesign --force --sign "${FAUSTLENS_SIGN_IDENTITY}" --options runtime
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Llvm.entitlements" "$<TARGET_FILE:faustlens_jit_llvm>"
        VERBATIM)
    target_link_libraries(faustlens_jit_native PRIVATE faustlens_native)
    target_link_libraries(faustlens_jit_llvm PRIVATE "${FAUSTLENS_LLVM_LIBRARY}")
    get_filename_component(LLVM_LIBRARY_DIR "${FAUSTLENS_LLVM_LIBRARY}" DIRECTORY)
    set_target_properties(faustlens_jit_llvm PROPERTIES BUILD_RPATH "${LLVM_LIBRARY_DIR}")
    target_compile_definitions(faustlens_jit_llvm PRIVATE BENCH_LLVM=1)
endif()
