set(FAUSTLENS_SIGN_IDENTITY "-" CACHE STRING "Code signing identity for hardened macOS executables")

function(faustlens_sign_jit target)
    if(APPLE AND NOT FAUSTLENS_SANITIZE)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Jit.entitlements")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND /usr/bin/codesign --force --sign "${FAUSTLENS_SIGN_IDENTITY}" --options runtime
                    --entitlements "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Jit.entitlements" "$<TARGET_FILE:${target}>"
            VERBATIM)
    endif()
endfunction()
