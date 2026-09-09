cmake_minimum_required(VERSION 3.24)
project(faustlens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(FAUSTLENS_TESTS "Build the test binaries" ${PROJECT_IS_TOP_LEVEL})

option(FAUSTLENS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

option(FAUSTLENS_GUI "Build the SDL3 + Dear ImGui front end" ${PROJECT_IS_TOP_LEVEL})

if(FAUSTLENS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Use the same faustlibraries revision for embedding and oracle generation.
set(FAUSTLENS_FAUSTLIBRARIES "${PROJECT_SOURCE_DIR}/lib/faust/libraries"
    CACHE PATH "faustlibraries checkout to embed")

if(NOT EXISTS "${FAUSTLENS_FAUSTLIBRARIES}/stdfaust.lib")
    message(FATAL_ERROR
        "faustlibraries is not checked out. Run:\n"
        "  git -C lib/faust submodule update --init libraries")
endif()

if(FAUSTLENS_GUI OR FAUSTLENS_TESTS)
    enable_language(C)
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

add_subdirectory(src EXCLUDE_FROM_ALL)
if(FAUSTLENS_GUI OR FAUSTLENS_TESTS)
    add_subdirectory(app)
endif()

if(FAUSTLENS_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
