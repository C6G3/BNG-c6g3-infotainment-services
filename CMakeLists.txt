cmake_minimum_required(VERSION 3.20)
project(c6g3-infotainment-services VERSION 0.1.0 LANGUAGES CXX RC)

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
    libswresample
    libavutil
)

# sherpa-onnx C API (Kokoro TTS). Prebuilt MSVC DLLs; the plain C interface links from MinGW.
# Binaries are not in git: see third_party/sherpa-onnx/README.md
set(SHERPA_ONNX_DIR ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx)
set(SHERPA_ONNX_RUNTIME_DLLS
    ${SHERPA_ONNX_DIR}/lib/sherpa-onnx-c-api.dll
    ${SHERPA_ONNX_DIR}/lib/onnxruntime.dll
)
if(NOT EXISTS ${SHERPA_ONNX_DIR}/lib/sherpa-onnx-c-api.lib)
    message(FATAL_ERROR "sherpa-onnx binaries not found in ${SHERPA_ONNX_DIR}/lib — see third_party/sherpa-onnx/README.md")
endif()
add_library(sherpa_onnx_c_api SHARED IMPORTED)
set_target_properties(sherpa_onnx_c_api PROPERTIES
    IMPORTED_LOCATION             ${SHERPA_ONNX_DIR}/lib/sherpa-onnx-c-api.dll
    IMPORTED_IMPLIB               ${SHERPA_ONNX_DIR}/lib/sherpa-onnx-c-api.lib
    INTERFACE_INCLUDE_DIRECTORIES ${SHERPA_ONNX_DIR}/include
)

# Main executable
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/core/Logger.cpp
    src/app/Config.cpp
    src/app/Application.cpp
    src/platform/windows/ProcessPriority.cpp
    src/platform/windows/ShortcutResolver.cpp
    src/resources/MediaScanner.cpp
    src/resources/ResourceManager.cpp
    src/resources/VideoTranscoder.cpp
    src/resources/VideoStreamer.cpp
    src/resources/RadioStreamer.cpp
    src/resources/RadioDiscovery.cpp
    src/resources/WebRtcProbe.cpp
    src/server/ConnectionManager.cpp
    src/server/WebSocketSession.cpp
    src/server/WebSocketServer.cpp
    src/tts/KokoroEngine.cpp
    src/tts/TextSplitter.cpp
    src/tts/TtsService.cpp
    src/tts/VoiceLibrary.cpp
    resources/c6g3.rc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/resources
    C:/msys64/ucrt64/include/stb
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
    PkgConfig::FFMPEG
    sherpa_onnx_c_api
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

# ─── Post-build: Copy TTS runtime DLLs next to the executable ───────────────
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHERPA_ONNX_RUNTIME_DLLS}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMENT "Copying sherpa-onnx runtime DLLs to build output directory"
)

# ─── Dist Target: Package standalone release into dist/ ─────────────────────
set(DIST_DIR ${CMAKE_SOURCE_DIR}/dist/${PROJECT_NAME})

if(WIN32)
    # Bundles every runtime DLL the exe needs, followed recursively (FFmpeg and everything it links: libvpx,
    # libopus, ... about 100 DLLs), so the release runs without MSYS2 on PATH; see cmake/BundleRuntimeDlls.cmake.
    # Compiler runtimes are statically linked into the exe itself. assistant/ ships with whatever models are
    # present locally; model weights are not in git and are added to the release package by hand.
    get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    add_custom_target(dist
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/config ${DIST_DIR}/config
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:${PROJECT_NAME}>
            -DDEST=${DIST_DIR}
            "-DSEARCH_DIRS=$<TARGET_FILE_DIR:${PROJECT_NAME}>|${MINGW_BIN_DIR}"
            -P ${CMAKE_SOURCE_DIR}/cmake/BundleRuntimeDlls.cmake
        COMMAND ${CMAKE_COMMAND} -E copy ${SHERPA_ONNX_RUNTIME_DLLS} ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assistant ${DIST_DIR}/assistant
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${DIST_DIR}/assistant/.cache
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
