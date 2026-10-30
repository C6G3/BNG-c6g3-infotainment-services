# Sets "logging.level" in the dist copy of the config (the repo config stays DEBUG for development).
# Usage: cmake -DCONFIG=<path to config.default.json> -DLEVEL=INFO -P SetDistLogLevel.cmake
if(NOT CONFIG OR NOT EXISTS "${CONFIG}")
    message(FATAL_ERROR "SetDistLogLevel: config not found: ${CONFIG}")
endif()
if(NOT LEVEL)
    set(LEVEL INFO)
endif()

file(READ "${CONFIG}" content)
string(REGEX REPLACE "(\"level\"[ \t]*:[ \t]*)\"[A-Za-z]+\"" "\\1\"${LEVEL}\"" updated "${content}")
if(updated STREQUAL content AND NOT content MATCHES "\"level\"[ \t]*:[ \t]*\"${LEVEL}\"")
    message(FATAL_ERROR "SetDistLogLevel: no logging level found in ${CONFIG}")
endif()
file(WRITE "${CONFIG}" "${updated}")
message(STATUS "Dist log level: ${LEVEL}")
