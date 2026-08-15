# The application layer, and the only place the unbounded dependencies live.
#
# `faustlens_app` is the half with semantics: the buffer contract, the ref-tree
# walks, box-view *layout*, the audio host, and what a widget's metadata asks
# for. None of it links ImGui, so all of it is covered by tests rather than by
# screenshots. Only `faustlens_gui` draws, and all it does is render that
# output and read the mouse.
#
# The split is load-bearing, not tidiness: box-view geometry is claimed to be a
# pure function of Term, and that claim is only a property if it can be checked
# without opening a window.
add_library(faustlens_app
    editor/Buffer.cpp
    editor/Workspace.cpp
    boxview/Layout.cpp
    boxview/Select.cpp
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
        boxview/Draw.cpp
        controls/Draw.cpp)
    target_link_libraries(faustlens_gui PRIVATE faustlens_app imgui)
endif()
