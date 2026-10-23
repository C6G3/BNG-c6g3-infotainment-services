# Copies every DLL an executable needs at run time (followed recursively: FFmpeg -> libvpx, libopus, ...)
# next to it, so the release runs on PCs without MSYS2. Windows' own DLLs (System32, api-ms-*) are skipped.
#
#   cmake -DEXE=<path to exe> -DDEST=<dist dir> "-DSEARCH_DIRS=<dir|dir>" -P BundleRuntimeDlls.cmake
#
# SEARCH_DIRS is '|'-separated: a ';' list does not survive the build tool's command line on Windows.
#
# DLLs from an earlier bundle are removed first, so the folder never keeps DLLs the exe no longer uses.
# A DLL that cannot be found stops the build instead of producing a release that fails to start.

foreach(var EXE DEST SEARCH_DIRS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "BundleRuntimeDlls.cmake: -D${var}=... is required")
    endif()
endforeach()
string(REPLACE "|" ";" SEARCH_DIRS "${SEARCH_DIRS}")

find_program(OBJDUMP_EXE objdump HINTS ${SEARCH_DIRS} REQUIRED)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP_EXE}")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES ${SEARCH_DIRS}
    PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-"
    POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]"
)

if(unresolved)
    list(JOIN unresolved "\n  " unresolvedText)
    message(FATAL_ERROR "These DLLs needed by ${EXE} were not found in ${SEARCH_DIRS}:\n  ${unresolvedText}")
endif()

file(GLOB oldDlls "${DEST}/*.dll")
if(oldDlls)
    file(REMOVE ${oldDlls})
endif()

set(totalBytes 0)
list(LENGTH resolved count)
foreach(dll IN LISTS resolved)
    file(COPY "${dll}" DESTINATION "${DEST}")
    file(SIZE "${dll}" size)
    math(EXPR totalBytes "${totalBytes} + ${size}")
endforeach()
math(EXPR totalMb "${totalBytes} / 1048576")
message(STATUS "Bundled ${count} runtime DLLs (${totalMb} MB) into ${DEST}")
