cmake_minimum_required(VERSION 3.20)
project(c6g3-infotainment-services VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find system-installed dependencies
find_package(Boost 1.80 REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
)

# Main executable
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/core/Logger.cpp
    src/app/Config.cpp
    src/app/Application.cpp
    src/platform/windows/ShortcutResolver.cpp
    src/resources/MediaScanner.cpp
    src/resources/ResourceManager.cpp
    src/server/ConnectionManager.cpp
    src/server/WebSocketSession.cpp
    src/server/WebSocketServer.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
    PkgConfig::FFMPEG
    ole32
    oleaut32
    uuid
    ws2_32
    wsock32
    mswsock
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
)
