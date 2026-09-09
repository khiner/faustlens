set(FAUST_REFERENCE "${CMAKE_SOURCE_DIR}/lib/faust/build/bin/faust")
if(NOT EXISTS "${FAUST_REFERENCE}")
    message(FATAL_ERROR "Build the pinned reference Faust compiler before enabling FAUSTLENS_BENCHMARKS; see README.md")
endif()

file(GLOB_RECURSE REFERENCE_LIBRARIES CONFIGURE_DEPENDS "${FAUSTLENS_FAUSTLIBRARIES}/*.lib")

set(CASES gain feedback delay table oscillator filter chain reverb)
set(DECLARATIONS "")
set(FACTORIES "")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED}")
add_executable(faustlens_benchmark Main.cpp)
target_link_libraries(faustlens_benchmark PRIVATE faustlens_app)
target_include_directories(faustlens_benchmark PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${GENERATED}" "${CMAKE_SOURCE_DIR}/lib/faust/architecture")
target_compile_options(faustlens_benchmark PRIVATE -O3 -ffp-contract=off -fno-fast-math)
target_compile_definitions(faustlens_benchmark PRIVATE BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dsp")

foreach(name IN LISTS CASES)
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
                    "${CMAKE_CURRENT_SOURCE_DIR}/dsp/${name}.dsp" -o "${output}"
            DEPENDS "${FAUST_REFERENCE}" "dsp/${name}.dsp" Reference.cpp.in Reference.h ${REFERENCE_LIBRARIES}
            VERBATIM)
        set_source_files_properties("${output}" PROPERTIES COMPILE_DEFINITIONS "BENCH_CLASS=Dsp_${id};BENCH_FACTORY=Reference_${id}")
        target_sources(faustlens_benchmark PRIVATE "${output}")
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
