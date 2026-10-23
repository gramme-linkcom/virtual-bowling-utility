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
    src/detection/ort_engine.cpp
    src/web/web_server.cpp
    src/trajectory/throw_tracker.cpp
    src/trajectory/throw_service.cpp
    src/transport/vector_file_writer.cpp
)

target_include_directories(vb2026_utility_tools PRIVATE
    include
    # ONNX Runtime の C API のヘッダー (DLL は実行時に読み込むのでリンク不要)
    third_party/onnxruntime/include
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

# ONNX Runtime (CPU) の DLL を exe の隣にコピーする (Web UI の「計算: CPU (ONNX Runtime)」で使う)。
# 無くてもビルド・起動はでき、ONNX Runtime を選んだときにエラーになるだけ。
set(VB_ORT_DLL "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/onnxruntime.dll")
if(WIN32 AND EXISTS "${VB_ORT_DLL}")
    add_custom_command(TARGET vb2026_utility_tools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VB_ORT_DLL}" "$<TARGET_FILE_DIR:vb2026_utility_tools>"
    )
endif()
if(NOT WIN32)
    target_link_libraries(vb2026_utility_tools PRIVATE ${CMAKE_DL_LIBS})
endif()
