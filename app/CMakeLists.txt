add_library(miniaudio STATIC ../lib/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC ../lib/miniaudio)
target_compile_definitions(miniaudio PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING)
if(APPLE)
    target_link_libraries(miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

add_library(faustlens_app
    editor/Buffer.cpp
    editor/Text.cpp
    editor/Workspace.cpp
    boxview/Layout.cpp
    boxview/Select.cpp
    Compiler.cpp
    Edits.cpp
    Expand.cpp
    Trace.cpp
    audio/Host.cpp
    audio/Decode.cpp
    controls/Style.cpp
    controls/Store.cpp
    Live.cpp)
target_include_directories(faustlens_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faustlens_app PUBLIC faustlens PRIVATE miniaudio)

if(FAUSTLENS_GUI)
    add_executable(faustlens_gui
        main.cpp
        editor/Draw.cpp
        boxview/Draw.cpp
        controls/Draw.cpp)
    target_link_libraries(faustlens_gui PRIVATE faustlens_app imgui)
    faustlens_sign_jit(faustlens_gui)
endif()
