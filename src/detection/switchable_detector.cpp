#include "vb/detection/switchable_detector.hpp"

#include <algorithm>
#include <iostream>

namespace detection {

const char* to_string(DetectorMode mode) {
    return mode == DetectorMode::Cv ? "cv" : "yolo";
}

bool parse_detector_mode(const std::string& text, DetectorMode& mode) {
    if (text == "yolo") {
        mode = DetectorMode::Yolo;
    } else if (text == "cv") {
        mode = DetectorMode::Cv;
    } else {
        return false;
    }
    return true;
}

SwitchableDetector::SwitchableDetector(
    IBallDetector* yolo,
    MotionDetector& motion,
    DetectorMode mode
)
    : yolo_(yolo),
      motion_(motion),
      mode_(static_cast<int>(yolo == nullptr ? DetectorMode::Cv : mode)),
      cv_threshold_(motion.config().diff_threshold) {}

std::vector<Detection> SwitchableDetector::detect(const cv::Mat& bgr_frame) {
    if (mode() == DetectorMode::Yolo && yolo_ != nullptr) {
        // 次に OpenCV に切り替えたとき、古い背景を使わないようにする。
        needs_motion_reset_ = true;
        return yolo_->detect(bgr_frame);
    }

    // MotionDetector の設定変更・リセットは、検出スレッドの中でだけ行う。
    motion_.set_diff_threshold(cv_threshold_.load());
    if (needs_motion_reset_.exchange(false)) {
        motion_.reset();
    }
    return motion_.detect(bgr_frame);
}

bool SwitchableDetector::set_mode(DetectorMode mode, std::string& message) {
    if (mode == DetectorMode::Yolo && yolo_ == nullptr) {
        message = "YOLO のモデルが読み込めていないので使えません";
        return false;
    }
    mode_ = static_cast<int>(mode);
    message = mode == DetectorMode::Yolo ? "YOLO で検出します" : "OpenCV だけで検出します (YOLO は使いません)";
    std::cout << "[detector] " << message << "\n";
    return true;
}

void SwitchableDetector::set_cv_threshold(int threshold) {
    cv_threshold_ = std::clamp(threshold, 1, 255);
}

}  // namespace detection
