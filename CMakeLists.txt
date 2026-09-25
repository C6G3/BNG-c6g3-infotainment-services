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
    libswscale
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
    C:/msys64/ucrt64/include/stb
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
    PkgConfig::FFMPEG
    -Wl,-Bstatic -lwebp -lsharpyuv -Wl,-Bdynamic
    ole32
    oleaut32
    uuid
    ws2_32
    wsock32
    mswsock
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
    -fno-var-tracking
    -fno-var-tracking-assignments
    --param=ggc-min-expand=20
    --param=ggc-min-heapsize=32768
)

if(WIN32 AND MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic
    )
endif()

# ─── Post-build: Copy config/ into build output directory ───────────────────
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/config
    COMMENT "Bundling config/ to build output directory"
)

# ─── Dist Target: Package standalone release into dist/ ─────────────────────
set(DIST_DIR ${CMAKE_SOURCE_DIR}/dist/${PROJECT_NAME})

if(WIN32)
    # Collect only FFmpeg runtime DLLs for portable distribution (compiler runtimes are statically linked)
    add_custom_target(dist
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/config ${DIST_DIR}/config
        COMMAND ${CMAKE_COMMAND} -E copy
            C:/msys64/ucrt64/bin/avformat-62.dll
            C:/msys64/ucrt64/bin/avcodec-62.dll
            C:/msys64/ucrt64/bin/swscale-9.dll
            C:/msys64/ucrt64/bin/avutil-60.dll
            ${DIST_DIR}
        COMMENT "Bundling standalone distribution in dist/${PROJECT_NAME}"
        DEPENDS ${PROJECT_NAME}
    )
else()
    add_custom_target(dist
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/config ${DIST_DIR}/config
        COMMENT "Bundling standalone distribution in dist/${PROJECT_NAME}"
        DEPENDS ${PROJECT_NAME}
    )
endif()
