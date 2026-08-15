cmake_minimum_required(VERSION 3.24)
# C is only for the tree-sitter acceptance oracle under test/.
project(faustlens CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(FAUSTLENS_TESTS "Build the test binaries" ON)

# MemorySanitizer does not exist on macOS, so a clean run is evidence, not proof:
# no uninitialized reads are caught. Configure a second tree rather than switching
# this one, and reuse the oracle the regular tree generated:
#   cmake -B build-san -DFAUSTLENS_SANITIZE=ON -DFAUSTLENS_GUI=OFF
#   FAUSTLENS_ORACLE_DIR=$PWD/build/oracle ./build-san/test/faustlens_tests
option(FAUSTLENS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Layout geometry is a pure function of Term, so `faustlens_boxview` is testable
# without a window and the front end can be dropped from the sanitizer tree.
option(FAUSTLENS_GUI "Build the SDL3 + Dear ImGui front end" ON)

if(FAUSTLENS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# The standard library is embedded from this checkout and the oracle is generated
# against it, so both must come from the same commit.
set(FAUSTLENS_FAUSTLIBRARIES "${CMAKE_SOURCE_DIR}/lib/faust/libraries"
    CACHE PATH "faustlibraries checkout to embed")

if(NOT EXISTS "${FAUSTLENS_FAUSTLIBRARIES}/stdfaust.lib")
    message(FATAL_ERROR
        "faustlibraries is not checked out. Run:\n"
        "  git -C lib/faust submodule update --init libraries")
endif()

# Audio host and soundfile decoding. No window, so it sits outside `FAUSTLENS_GUI`
# and the sanitizer tree builds it too. miniaudio's own CMake builds the
# high-level engine and offers no way to drop it.
add_library(miniaudio STATIC lib/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC lib/miniaudio)
# The audio callback is the mixer, so the high-level API is dead weight. Decoding
# stays, since it is half the reason miniaudio was chosen.
target_compile_definitions(miniaudio PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING)
if(APPLE)
    target_link_libraries(miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

if(FAUSTLENS_GUI)
    # The app is one executable, so static keeps the link simple.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(lib/sdl3 EXCLUDE_FROM_ALL)

    # Dear ImGui ships no CMake of its own. Core units plus the two backends,
    # with `imgui_demo.cpp` deliberately left out.
    add_library(imgui STATIC
        lib/imgui/imgui.cpp
        lib/imgui/imgui_draw.cpp
        lib/imgui/imgui_tables.cpp
        lib/imgui/imgui_widgets.cpp
        lib/imgui/backends/imgui_impl_sdl3.cpp
        lib/imgui/backends/imgui_impl_sdlgpu3.cpp)
    target_include_directories(imgui PUBLIC lib/imgui lib/imgui/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
endif()

add_subdirectory(src)
add_subdirectory(app)

if(FAUSTLENS_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
