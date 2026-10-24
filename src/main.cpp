#include "vb/camera/camera_stream.hpp"
#include "vb/detection/ball_detector.hpp"
#include "vb/detection/dataset_recorder.hpp"
#include "vb/detection/detection_worker.hpp"
#include "vb/detection/motion_detector.hpp"
#include "vb/detection/switchable_detector.hpp"
#include "vb/trajectory/throw_service.hpp"
#include "vb/web/web_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <opencv2/core/utils/logger.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr auto kShutdownPollInterval = std::chrono::milliseconds(100);
// Web UI で変えた設定を覚えておくファイル (run.bat の場所 = プロジェクト直下)。
constexpr const char* kSettingsPath = "settings.ini";
// settings.ini に保存するカメラ番号のキー (Web UI で切り替えたときに書く)。
constexpr const char* kCameraSettingKey = "camera";
// settings.ini に保存する検出方法 (yolo / cv) と OpenCV 検出の感度のキー (web_server.cpp と同じ)。
constexpr const char* kDetectorSettingKey = "detector";
constexpr const char* kCvThresholdSettingKey = "cv_threshold";
constexpr const char* kComputeSettingKey = "compute";

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested = true;
}

struct AppOptions {
    camera::CameraConfig camera;
    web::WebConfig web;
    detection::DetectorConfig detector;
    detection::MotionDetectorConfig motion;
    std::optional<detection::DetectorMode> detector_mode;  // --detector で指定したとき
    bool is_cv_threshold_explicit = false;
    bool is_compute_explicit = false;
    trajectory::ThrowServiceConfig throws;
    // --model を明示したかどうか。明示したのに無ければ起動を止める。
    bool is_model_explicit = false;
    // 起動したらすぐ計測を始める (Web UI の「計測開始」を押さなくてよい)。
    bool start_armed = false;
    bool is_camera_explicit = false;
};

void print_usage() {
    std::cout << "使い方: vb2026_utility_tools [オプション]\n"
                 "  --camera N   カメラ番号 (既定: 前回 Web UI で選んだカメラ、無ければ 0)\n"
                 "  --width N    カメラの幅 (既定: カメラ任せ)\n"
                 "  --height N   カメラの高さ (既定: カメラ任せ)\n"
                 "  --port N     Web UI のポート (既定: 8080)\n"
                 "  --host H     待ち受けアドレス (既定: 127.0.0.1。別PCから見るなら 0.0.0.0)\n"
                 "  --model P    YOLO の ONNX ファイル (既定: models/best.onnx)\n"
                 "  --conf F     検出の信頼度しきい値 0.01〜0.99 (既定: 0.25)\n"
                 "  --detector M 検出方法 yolo / cv (cv は YOLO を使わず OpenCV だけ。既定: 前回の選択)\n"
                 "  --cv-threshold N  OpenCV 検出の感度 1〜255 (小さいほど敏感。既定: 30)\n"
                 "  --compute D  YOLO の計算装置 cpu / onnxruntime / opencl / opencl_fp16 (既定: 前回の選択、無ければ cpu)\n"
                 "  --start      起動と同時に計測を開始する (既定: Web UI の「計測開始」を待つ)\n"
                 "  --output P   Unity が読む vector.txt のパス (既定: Web UI で設定した場所)\n"
                 "  --lane-width F   レーンの横幅 [m] (既定: 5、去年と同じ)\n"
                 "  --speed-scale F  Unity に渡す速度の倍率 (既定: 1.0)\n"
                 "  --end-wait F     ボールが止まって/消えてから確定するまでの秒数 (既定: 0.3)\n"
                 "  --min-travel F   投球とみなす縦方向の移動 (画面の高さの割合、既定: 0.5)\n"
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

bool parse_double(const std::string& text, double min_value, double max_value, double& value) {
    float parsed = 0.0f;
    if (!parse_float(text, static_cast<float>(min_value), static_cast<float>(max_value), parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

// コマンドライン引数を UTF-8 で取り出す。
// Windows の argv は Shift_JIS なので、日本語のパス (デスクトップ など) が化けないようにする。
std::vector<std::string> utf8_arguments(int argc, char* argv[]) {
    std::vector<std::string> args;
#ifdef _WIN32
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        for (int i = 0; i < count; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(size > 0 ? size - 1 : 0, '\0');
            if (size > 1) {
                WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
            }
            args.push_back(std::move(text));
        }
        LocalFree(wide);
        return args;
    }
#endif
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

// 成功したら true。--help のときは show_help を立てる。
bool parse_options(
    const std::vector<std::string>& argv,
    AppOptions& options,
    bool& show_help,
    std::string& error
) {
    const int argc = static_cast<int>(argv.size());
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
            options.is_camera_explicit = true;
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
        } else if (arg == "--detector") {
            detection::DetectorMode mode = detection::DetectorMode::Yolo;
            if (!has_value || !detection::parse_detector_mode(argv[++i], mode)) {
                error = "--detector には yolo か cv を指定してください";
                return false;
            }
            options.detector_mode = mode;
        } else if (arg == "--compute") {
            if (!has_value || !detection::parse_compute_target(argv[++i], options.detector.target)) {
                error = "--compute には cpu / onnxruntime / opencl / opencl_fp16 のどれかを指定してください";
                return false;
            }
            options.is_compute_explicit = true;
        } else if (arg == "--cv-threshold") {
            if (!has_value || !parse_int(argv[++i], 1, 255, options.motion.diff_threshold)) {
                error = "--cv-threshold には 1〜255 の数値を指定してください";
                return false;
            }
            options.is_cv_threshold_explicit = true;
        } else if (arg == "--start") {
            options.start_armed = true;
        } else if (arg == "--output") {
            if (!has_value) {
                error = "--output には vector.txt のパスを指定してください";
                return false;
            }
            options.throws.output_path = argv[++i];
        } else if (arg == "--lane-width") {
            if (!has_value || !parse_double(argv[++i], 0.1, 100.0, options.throws.lane_width_m)) {
                error = "--lane-width には 0.1〜100 の数値を指定してください";
                return false;
            }
        } else if (arg == "--speed-scale") {
            if (!has_value || !parse_double(argv[++i], 0.01, 100.0, options.throws.speed_scale)) {
                error = "--speed-scale には 0.01〜100 の数値を指定してください";
                return false;
            }
        } else if (arg == "--end-wait") {
            if (!has_value ||
                !parse_double(argv[++i], 0.05, 5.0, options.throws.tracker.end_wait_s)) {
                error = "--end-wait には 0.05〜5 の数値を指定してください";
                return false;
            }
        } else if (arg == "--min-travel") {
            if (!has_value ||
                !parse_double(argv[++i], 0.0, 1.0, options.throws.tracker.min_travel_ratio)) {
                error = "--min-travel には 0〜1 の数値を指定してください";
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
    if (!parse_options(utf8_arguments(argc, argv), options, show_help, error)) {
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

    // Web UI で変えた設定 (出力先・検出範囲・ボールの大きさ) はここに保存し、次回も使う。
    // 出力先は --output を付けた場合はそちらが優先。
    options.throws.settings_path = kSettingsPath;

    // 投球の判定と Unity (vector.txt) への受け渡し。検出が無効でもテスト送信は使える。
    trajectory::ThrowService throw_service(options.throws);
    if (throw_service.snapshot().output_path.empty()) {
        std::cerr << "[warn][throw] vector.txt の出力先が未設定です "
                     "(Web UI か --output で指定してください)\n";
    } else {
        std::cout << "[throw] 出力先: " << throw_service.snapshot().output_path << "\n";
    }

    if (options.start_armed) {
        throw_service.set_armed(true);
    }

    // カメラ番号は --camera が優先。無ければ前回 Web UI で選んだカメラを使う。
    if (!options.is_camera_explicit) {
        int saved_index = 0;
        if (parse_int(throw_service.setting(kCameraSettingKey), 0, 99, saved_index)) {
            options.camera.index = saved_index;
        }
    }
    // カメラが開けなくても終了しない (Web UI から別のカメラを選べるように)。
    camera::CameraStream camera(options.camera);
    if (camera.start(error)) {
        std::cout << "[camera] opened index=" << options.camera.index << "\n";
    } else {
        std::cerr << "[error][camera] " << error << " (Web UI でカメラを選び直してください)\n";
    }

    // 検出器は2種類。YOLO (モデルが読めたときだけ) と、OpenCV だけの検出 (いつでも使える)。
    // モデルが無い・読めないときは OpenCV だけで動く。
    // (--model を明示したのに無い場合は、間違いに気づけるよう起動を止める)
    // 計算装置 (CPU / OpenCL) は --compute が優先。無ければ前回 Web UI で選んだもの。
    if (!options.is_compute_explicit) {
        detection::parse_compute_target(throw_service.setting(kComputeSettingKey), options.detector.target);
    }
    // モデルは --model が優先。無ければ前回 Web UI で選んだもの (見つからなければ既定の models/best.onnx)。
    const std::string default_model_path = options.detector.model_path;
    if (!options.is_model_explicit) {
        const std::string saved_model = throw_service.setting("model");
        std::error_code saved_error;
        if (!saved_model.empty() && std::filesystem::exists(saved_model, saved_error)) {
            options.detector.model_path = saved_model;
        }
    }
    detection::BallDetector yolo(options.detector);
    std::string detection_message;
    std::error_code file_error;
    bool is_yolo_loaded = false;
    bool is_preloaded = false;  // 前回選んだモデルをもう読み込めたか
    if (!options.is_model_explicit && options.detector.model_path != default_model_path) {
        is_preloaded = yolo.load(error);
    }
    if (!options.is_model_explicit && options.detector.model_path != default_model_path && !is_preloaded) {
        // 前回選んだモデルが読めないときは、既定のモデルで起動する
        std::cerr << "[warn][detector] " << options.detector.model_path << " を読めません (" << error
                  << ")。" << default_model_path << " を使います\n";
        options.detector.model_path = default_model_path;
        yolo = detection::BallDetector(options.detector);
    }
    if (!std::filesystem::exists(options.detector.model_path, file_error)) {
        detection_message = "モデルが見つかりません: " + options.detector.model_path;
        if (options.is_model_explicit) {
            std::cerr << "[error][detector] " << detection_message << "\n";
            return 1;
        }
        std::cerr << "[warn][detector] " << detection_message << " (OpenCV だけで検出します)\n";
    } else if (!is_preloaded && !yolo.load(error)) {
        detection_message = error;
        std::cerr << "[error][detector] " << error << " (OpenCV だけで検出します)\n";
    } else {
        is_yolo_loaded = true;
        std::cout << "[detector] loaded " << yolo.config().model_path << " input=" << yolo.config().input_size
                  << " conf>=" << options.detector.conf_threshold
                  << " compute=" << detection::to_string(yolo.target()) << "\n";
    }

    // 検出方法と OpenCV の感度は、コマンドライン > 前回 Web UI で選んだもの > 既定 (YOLO) の順。
    detection::DetectorMode mode = detection::DetectorMode::Yolo;
    if (options.detector_mode) {
        mode = *options.detector_mode;
    } else {
        detection::parse_detector_mode(throw_service.setting(kDetectorSettingKey), mode);
    }
    if (!options.is_cv_threshold_explicit) {
        parse_int(throw_service.setting(kCvThresholdSettingKey), 1, 255, options.motion.diff_threshold);
    }
    detection::MotionDetector motion(options.motion);
    detection::SwitchableDetector detector(is_yolo_loaded ? &yolo : nullptr, motion, mode);
    std::cout << "[detector] 検出方法: "
              << (detector.mode() == detection::DetectorMode::Yolo ? "YOLO" : "OpenCV だけ")
              << "\n";

    // 来年の学習用に、認識した画像を保存する (管理画面の「データセット保存」で ON/OFF)。
    detection::DatasetRecorder dataset_recorder("dataset");
    dataset_recorder.set_enabled(throw_service.setting("dataset_record") == "1");

    auto detection_worker = std::make_unique<detection::DetectionWorker>(camera, detector);
    detection_worker->set_result_callback([&throw_service](const detection::DetectionResult& r) {
        std::vector<trajectory::BallCandidate> candidates;
        candidates.reserve(r.detections.size());
        for (const detection::Detection& d : r.detections) {
            candidates.push_back(
                {d.box.x + d.box.width / 2.0,
                 d.box.y + d.box.height / 2.0,
                 d.confidence,
                 (d.box.width + d.box.height) / 2.0}
            );
        }
        throw_service.on_frame(r.timestamp_s, r.frame_width, r.frame_height, candidates);
    });
    detection_worker->set_frame_callback(
        [&dataset_recorder, &throw_service, &detector](const cv::Mat& frame, const detection::DetectionResult& r) {
            if (!dataset_recorder.is_enabled()) {
                return;
            }
            dataset_recorder.submit(
                frame,
                r.frame_id,
                r.detections,
                throw_service.snapshot().is_tracking,
                detection::to_string(detector.mode())
            );
        }
    );
    detection_worker->start();

    web::WebServer server(
        options.web,
        camera,
        detection_worker.get(),
        detection_message,
        &throw_service,
        &detector
    );
    server.set_dataset_recorder(&dataset_recorder);
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
