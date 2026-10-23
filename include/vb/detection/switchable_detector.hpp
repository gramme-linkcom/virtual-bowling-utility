#pragma once

#include "vb/detection/detection.hpp"
#include "vb/detection/motion_detector.hpp"

#include <atomic>
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
    SwitchableDetector(IBallDetector* yolo, MotionDetector& motion, DetectorMode mode);

    std::vector<Detection> detect(const cv::Mat& bgr_frame) override;

    // YOLO が使えないのに Yolo を選んだら false。
    bool set_mode(DetectorMode mode, std::string& message);
    DetectorMode mode() const {
        return static_cast<DetectorMode>(mode_.load());
    }
    bool has_yolo() const {
        return yolo_ != nullptr;
    }

    // OpenCV 検出の感度 (背景との差のしきい値 1〜255)。
    void set_cv_threshold(int threshold);
    int cv_threshold() const {
        return cv_threshold_.load();
    }

private:
    IBallDetector* yolo_;
    MotionDetector& motion_;
    std::atomic<int> mode_;
    std::atomic<int> cv_threshold_;
    std::atomic<bool> needs_motion_reset_{true};
};

}  // namespace detection
