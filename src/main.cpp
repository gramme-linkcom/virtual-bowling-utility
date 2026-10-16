#include "vb/camera/camera_stream.hpp"
#include "vb/detection/ball_detector.hpp"
#include "vb/detection/detection_worker.hpp"
#include "vb/web/web_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <opencv2/core/utils/logger.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr auto kShutdownPollInterval = std::chrono::milliseconds(100);

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested = true;
}

struct AppOptions {
    camera::CameraConfig camera;
    web::WebConfig web;
    detection::DetectorConfig detector;
    // --model を明示したかどうか。明示したのに無ければ起動を止める。
    bool is_model_explicit = false;
};

void print_usage() {
    std::cout << "使い方: vb2026_utility_tools [オプション]\n"
                 "  --camera N   カメラ番号 (既定: 0)\n"
                 "  --width N    カメラの幅 (既定: カメラ任せ)\n"
                 "  --height N   カメラの高さ (既定: カメラ任せ)\n"
                 "  --port N     Web UI のポート (既定: 8080)\n"
                 "  --host H     待ち受けアドレス (既定: 127.0.0.1。別PCから見るなら 0.0.0.0)\n"
                 "  --model P    YOLO の ONNX ファイル (既定: models/best.onnx)\n"
                 "  --conf F     検出の信頼度しきい値 0.01〜0.99 (既定: 0.25)\n"
                 "  --help       この表示\n";
}

bool parse_int(const std::string& text, int min_value, int max_value, int& value) {
    try {
        std::size_t pos = 0;
        const int parsed = std::stoi(text, &pos);
        if (pos != text.size() || parsed < min_value || parsed > max_value) {
            return false;
        }
        value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_float(const std::string& text, float min_value, float max_value, float& value) {
    try {
        std::size_t pos = 0;
        const float parsed = std::stof(text, &pos);
        if (pos != text.size() || parsed < min_value || parsed > max_value) {
            return false;
        }
        value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// 成功したら true。--help のときは show_help を立てる。
bool parse_options(int argc, char* argv[], AppOptions& options, bool& show_help, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1 < argc);

        if (arg == "--help" || arg == "-h") {
            show_help = true;
            return true;
        }
        if (arg == "--camera") {
            if (!has_value || !parse_int(argv[++i], 0, 99, options.camera.index)) {
                error = "--camera には 0〜99 の数値を指定してください";
                return false;
            }
        } else if (arg == "--width") {
            if (!has_value || !parse_int(argv[++i], 1, 7680, options.camera.width)) {
                error = "--width には 1〜7680 の数値を指定してください";
                return false;
            }
        } else if (arg == "--height") {
            if (!has_value || !parse_int(argv[++i], 1, 4320, options.camera.height)) {
                error = "--height には 1〜4320 の数値を指定してください";
                return false;
            }
        } else if (arg == "--port") {
            if (!has_value || !parse_int(argv[++i], 1, 65535, options.web.port)) {
                error = "--port には 1〜65535 の数値を指定してください";
                return false;
            }
        } else if (arg == "--host") {
            if (!has_value) {
                error = "--host にはアドレスを指定してください";
                return false;
            }
            options.web.host = argv[++i];
        } else if (arg == "--model") {
            if (!has_value) {
                error = "--model には ONNX ファイルのパスを指定してください";
                return false;
            }
            options.detector.model_path = argv[++i];
            options.is_model_explicit = true;
        } else if (arg == "--conf") {
            if (!has_value || !parse_float(argv[++i], 0.01f, 0.99f, options.detector.conf_threshold)) {
                error = "--conf には 0.01〜0.99 の数値を指定してください";
                return false;
            }
        } else {
            error = "不明なオプションです: " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 日本語のログが文字化けしないよう、コンソールの出力を UTF-8 にする。
    SetConsoleOutputCP(CP_UTF8);
#endif
    // OpenCV 内部の警告 (GStreamer が使えない旨など) は動作に影響しないので出さない。
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

    AppOptions options;
    bool show_help = false;
    std::string error;
    if (!parse_options(argc, argv, options, show_help, error)) {
        std::cerr << "[error] " << error << "\n";
        print_usage();
        return 1;
    }
    if (show_help) {
        print_usage();
        return 0;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    camera::CameraStream camera(options.camera);
    if (!camera.start(error)) {
        std::cerr << "[error][camera] " << error << "\n";
        return 1;
    }
    std::cout << "[camera] opened index=" << options.camera.index << "\n";

    // 検出器。モデルが無い・読めないときも、映像の確認だけはできるように起動を続ける。
    // (--model を明示した場合は、間違いに気づけるよう起動を止める)
    detection::BallDetector detector(options.detector);
    std::unique_ptr<detection::DetectionWorker> detection_worker;
    std::string detection_message;
    std::error_code file_error;
    if (!std::filesystem::exists(options.detector.model_path, file_error)) {
        detection_message = "モデルが見つかりません: " + options.detector.model_path;
        if (options.is_model_explicit) {
            std::cerr << "[error][detector] " << detection_message << "\n";
            return 1;
        }
        std::cerr << "[warn][detector] " << detection_message << " (検出なしで起動します)\n";
    } else if (!detector.load(error)) {
        detection_message = error;
        std::cerr << "[error][detector] " << error << " (検出なしで起動します)\n";
    } else {
        detection_worker = std::make_unique<detection::DetectionWorker>(camera, detector);
        detection_worker->start();
        std::cout << "[detector] loaded " << options.detector.model_path
                  << " conf>=" << options.detector.conf_threshold << "\n";
    }

    web::WebServer server(options.web, camera, detection_worker.get(), detection_message);
    if (!server.bind(error)) {
        std::cerr << "[error][web] " << error << "\n";
        return 1;
    }

    std::thread server_thread([&server] { server.run(); });
    std::cout << "[web] http://" << options.web.host << ":" << options.web.port
              << "/  (Ctrl+C で終了)\n";

    while (!g_stop_requested) {
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    std::cout << "[app] 終了します\n";
    server.stop();
    server_thread.join();
    // 検出はカメラを参照しているので、カメラより先に止める。
    if (detection_worker) {
        detection_worker->stop();
    }
    camera.stop();
    return 0;
}
