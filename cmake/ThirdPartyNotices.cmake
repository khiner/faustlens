# Emits THIRD-PARTY-NOTICES for the embedded standard library.
#
# The obligation is discharged for the files that state terms: everything in
# licenses/, every `declare license` line, the pinned SHA and the upstream URL.
# The files that state nothing are recorded as stating nothing, which is the
# accurate thing to say about them.

if(NOT LIB_DIR OR NOT OUT)
    message(FATAL_ERROR "ThirdPartyNotices.cmake needs -DLIB_DIR= and -DOUT=")
endif()

execute_process(COMMAND git -C "${LIB_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE SHA OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE GIT_RC)
if(NOT GIT_RC EQUAL 0)
    set(SHA "unknown")
endif()

set(N "THIRD-PARTY NOTICES\n===================\n\n")
string(APPEND N
  "This binary embeds the Faust standard library (faustlibraries), unmodified,\n"
  "byte for byte as checked out at:\n\n"
  "    https://github.com/grame-cncm/faustlibraries\n"
  "    commit ${SHA}\n\n"
  "Modifying an embedded library is possible only by ejecting it into the\n"
  "user's workspace, so no distributed copy differs from the upstream bytes.\n\n")

file(GLOB_RECURSE FILES RELATIVE "${LIB_DIR}" "${LIB_DIR}/*.lib")
list(SORT FILES)

string(APPEND N "Declared licenses\n-----------------\n\n")
set(SILENT "")
foreach(REL IN LISTS FILES)
    file(STRINGS "${LIB_DIR}/${REL}" DECLS REGEX "declare +license")
    if(DECLS)
        foreach(D IN LISTS DECLS)
            string(STRIP "${D}" D)
            string(APPEND N "  ${REL}: ${D}\n")
        endforeach()
    else()
        list(APPEND SILENT "${REL}")
    endif()
endforeach()

string(APPEND N "\nFiles carrying no license declaration\n")
string(APPEND N "-------------------------------------\n\n")
string(APPEND N
  "These state no terms in the repository. They are redistributed unmodified\n"
  "under the same provenance as the rest of faustlibraries.\n\n")
foreach(REL IN LISTS SILENT)
    string(APPEND N "  ${REL}\n")
endforeach()

file(GLOB LICENSES "${LIB_DIR}/licenses/*")
list(SORT LICENSES)
foreach(L IN LISTS LICENSES)
    get_filename_component(BASE "${L}" NAME)
    file(READ "${L}" TEXT)
    string(APPEND N "\n\nlicenses/${BASE}\n")
    string(REPEAT "-" 20 RULE)
    string(APPEND N "${RULE}\n\n${TEXT}\n")
endforeach()

file(WRITE "${OUT}" "${N}")
