#pragma once

#include "vb/detection/ball_detector.hpp"
#include "vb/detection/detection.hpp"
#include "vb/detection/motion_detector.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace detection {

enum class DetectorMode {
    Yolo,  // YOLO (ONNX) で検出する
    Cv,    // YOLO を使わず OpenCV だけで検出する (動いている丸い物)
};

const char* to_string(DetectorMode mode);
bool parse_detector_mode(const std::string& text, DetectorMode& mode);

// YOLO と OpenCV だけの検出を、動かしたまま切り替えられるようにする。
// detect() は検出スレッドから、set_*() は Web サーバーから呼ばれる。
class SwitchableDetector : public IBallDetector {
public:
    // yolo は読み込めなかったときは nullptr (OpenCV だけで動く)。
    SwitchableDetector(BallDetector* yolo, MotionDetector& motion, DetectorMode mode);

    std::vector<Detection> detect(const cv::Mat& bgr_frame) override;

    // YOLO が使えないのに Yolo を選んだら false。
    bool set_mode(DetectorMode mode, std::string& message);
    DetectorMode mode() const {
        return static_cast<DetectorMode>(mode_.load());
    }
    bool has_yolo() const {
        return yolo_ != nullptr;
    }

    // YOLO の計算に使う装置 (CPU / OpenCL) の切り替えを頼む。実際の切り替えは検出スレッドで行う
    // (モデルの読み直しに数秒かかる)。結果は compute_message() で分かる。
    bool request_compute(ComputeTarget target, std::string& message);
    ComputeTarget compute() const {
        return static_cast<ComputeTarget>(compute_.load());
    }
    bool is_compute_switching() const {
        return requested_compute_.load() >= 0;
    }
    std::string compute_message() const;

    // 使う YOLO のモデル (ONNX) の切り替えを頼む。実際の切り替えは検出スレッドで行う。結果は compute_message()。
    bool request_model(const std::string& model_path, std::string& message);
    bool is_model_switching() const;
    // 今使っているモデルのパス (UTF-8) と入力の大きさ。
    std::string model_path() const;
    int input_size() const {
        return input_size_.load();
    }

    // OpenCV 検出の感度 (背景との差のしきい値 1〜255)。
    void set_cv_threshold(int threshold);
    int cv_threshold() const {
        return cv_threshold_.load();
    }

private:
    BallDetector* yolo_;
    MotionDetector& motion_;
    std::atomic<int> mode_;
    std::atomic<int> cv_threshold_;
    std::atomic<bool> needs_motion_reset_{true};
    std::atomic<int> compute_;
    std::atomic<int> requested_compute_{-1};  // -1: 切り替え待ちなし
    mutable std::mutex message_mutex_;
    std::string compute_message_;
    // モデルの切り替え (message_mutex_ で守る)
    std::string requested_model_;  // 空なら依頼なし
    std::string model_path_;
    std::atomic<int> input_size_{0};
};

}  // namespace detection
