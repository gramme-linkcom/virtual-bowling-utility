cmake_minimum_required(VERSION 3.20)

project(vb2026_utility_tools CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# このツールでは TLS・圧縮を使わない。環境差で勝手にリンクされないよう無効化する。
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.15.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(cpp_httplib)

# OpenCV は toolchain (MSYS2 ucrt64) 同梱のものを使う。
# highgui (Qt に依存する) は使わず、画面は Web UI に出す。
# dnn は YOLO (ONNX) の推論に使う。
# geometry は OpenCV だけの検出 (輪郭の面積・外接円) に使う (OpenCV 5 で imgproc から分かれた)。
find_package(OpenCV REQUIRED COMPONENTS core imgproc geometry videoio imgcodecs dnn)

add_executable(vb2026_utility_tools
    src/main.cpp
    src/camera/camera_stream.cpp
    src/detection/ball_detector.cpp
    src/detection/detection_worker.cpp
    src/detection/motion_detector.cpp
    src/detection/switchable_detector.cpp
    src/web/web_server.cpp
    src/trajectory/throw_tracker.cpp
    src/trajectory/throw_service.cpp
    src/transport/vector_file_writer.cpp
)

target_include_directories(vb2026_utility_tools PRIVATE
    include
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(vb2026_utility_tools PRIVATE
    httplib::httplib
    ${OpenCV_LIBS}
)

if(WIN32)
    # ws2_32: cpp-httplib / shell32: 日本語のコマンドライン引数を読む (CommandLineToArgvW)
    target_link_libraries(vb2026_utility_tools PRIVATE ws2_32 shell32)
endif()
