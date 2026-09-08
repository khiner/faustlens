cmake_minimum_required(VERSION 3.24)
# C is used by the tree-sitter acceptance oracle.
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

option(FAUSTLENS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

option(FAUSTLENS_GUI "Build the SDL3 + Dear ImGui front end" ON)

if(FAUSTLENS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Use the same faustlibraries revision for embedding and oracle generation.
set(FAUSTLENS_FAUSTLIBRARIES "${CMAKE_SOURCE_DIR}/lib/faust/libraries"
    CACHE PATH "faustlibraries checkout to embed")

if(NOT EXISTS "${FAUSTLENS_FAUSTLIBRARIES}/stdfaust.lib")
    message(FATAL_ERROR
        "faustlibraries is not checked out. Run:\n"
        "  git -C lib/faust submodule update --init libraries")
endif()

# Build audio hosting and decoding independently of the GUI for headless tests.
add_library(miniaudio STATIC lib/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC lib/miniaudio)
# Use miniaudio device I/O and decoding without the high-level engine.
target_compile_definitions(miniaudio PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING)
if(APPLE)
    target_link_libraries(miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

if(FAUSTLENS_GUI)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(lib/sdl3 EXCLUDE_FROM_ALL)

    add_library(imgui STATIC
        lib/imgui/imgui.cpp
        lib/imgui/imgui_draw.cpp
        lib/imgui/imgui_tables.cpp
        lib/imgui/imgui_widgets.cpp
        lib/imgui/misc/cpp/imgui_stdlib.cpp
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
