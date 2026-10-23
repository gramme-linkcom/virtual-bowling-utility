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
    BallDetector* yolo,
    MotionDetector& motion,
    DetectorMode mode
)
    : yolo_(yolo),
      motion_(motion),
      mode_(static_cast<int>(yolo == nullptr ? DetectorMode::Cv : mode)),
      cv_threshold_(motion.config().diff_threshold),
      compute_(static_cast<int>(yolo == nullptr ? ComputeTarget::Cpu : yolo->target())) {}

bool SwitchableDetector::request_compute(ComputeTarget target, std::string& message) {
    if (yolo_ == nullptr) {
        message = "YOLO のモデルが読み込めていないので切り替えられません";
        return false;
    }
    requested_compute_ = static_cast<int>(target);
    message = "切り替え中です (数秒かかることがあります)";
    return true;
}

std::string SwitchableDetector::compute_message() const {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return compute_message_;
}

std::vector<Detection> SwitchableDetector::detect(const cv::Mat& bgr_frame) {
    // 計算装置の切り替えは、推論と同じこのスレッドで行う (BallDetector はスレッドセーフでないため)。
    const int requested = requested_compute_.load();
    if (requested >= 0 && yolo_ != nullptr) {
        std::string message;
        const bool ok = yolo_->set_target(static_cast<ComputeTarget>(requested), message);
        compute_ = static_cast<int>(yolo_->target());
        std::cout << "[detector] " << message << (ok ? "" : " (元の装置のまま)") << "\n";
        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            compute_message_ = ok ? message : message + " (元の装置のまま)";
        }
        // 切り替え中に次の依頼が来ていたら、それは次のフレームで処理する。
        int expected = requested;
        requested_compute_.compare_exchange_strong(expected, -1);
    }

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
