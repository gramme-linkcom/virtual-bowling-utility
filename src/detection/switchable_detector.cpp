#include "vb/detection/switchable_detector.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

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

const char* to_string(Enhance enhance) {
    switch (enhance) {
        case Enhance::GammaWeak:
            return "gamma_weak";
        case Enhance::GammaStrong:
            return "gamma_strong";
        case Enhance::Clahe:
            return "clahe";
        case Enhance::None:
        default:
            return "none";
    }
}

bool parse_enhance(const std::string& text, Enhance& enhance) {
    if (text == "none") {
        enhance = Enhance::None;
    } else if (text == "gamma_weak") {
        enhance = Enhance::GammaWeak;
    } else if (text == "gamma_strong") {
        enhance = Enhance::GammaStrong;
    } else if (text == "clahe") {
        enhance = Enhance::Clahe;
    } else {
        return false;
    }
    return true;
}

void SwitchableDetector::set_conf_threshold(float threshold) {
    conf_threshold_ = std::clamp(threshold, 0.01f, 0.99f);
}

const cv::Mat& SwitchableDetector::enhance_frame(const cv::Mat& frame) {
    const Enhance enhance = this->enhance();
    if (enhance == Enhance::None || frame.empty() || frame.type() != CV_8UC3) {
        return frame;
    }
    if (enhance == Enhance::Clahe) {
        // 明るさ (Lab の L) だけコントラストを上げる。色は変えない。
        cv::Mat lab;
        cv::cvtColor(frame, lab, cv::COLOR_BGR2Lab);
        std::vector<cv::Mat> channels;
        cv::split(lab, channels);
        static thread_local cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
        clahe->apply(channels[0], channels[0]);
        cv::merge(channels, lab);
        cv::cvtColor(lab, enhanced_, cv::COLOR_Lab2BGR);
        return enhanced_;
    }
    // ガンマ補正: 出力 = 255 × (入力 / 255)^γ。γ < 1 で暗い所ほど明るくなる。
    const int kind = static_cast<int>(enhance);
    if (gamma_lut_for_ != kind) {
        const double gamma = enhance == Enhance::GammaStrong ? 0.55 : 0.75;
        gamma_lut_.create(1, 256, CV_8U);
        for (int i = 0; i < 256; ++i) {
            gamma_lut_.at<unsigned char>(i) =
                cv::saturate_cast<unsigned char>(std::pow(i / 255.0, gamma) * 255.0 + 0.5);
        }
        gamma_lut_for_ = kind;
    }
    cv::LUT(frame, gamma_lut_, enhanced_);
    return enhanced_;
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
      compute_(static_cast<int>(yolo == nullptr ? ComputeTarget::Cpu : yolo->target())) {
    if (yolo_ != nullptr) {
        model_path_ = yolo_->config().model_path;
        input_size_ = yolo_->config().input_size;
        conf_threshold_ = yolo_->config().conf_threshold;
    }
}

bool SwitchableDetector::request_model(const std::string& model_path, std::string& message) {
    if (yolo_ == nullptr) {
        message = "YOLO のモデルが読み込めていないので切り替えられません";
        return false;
    }
    std::lock_guard<std::mutex> lock(message_mutex_);
    requested_model_ = model_path;
    message = "モデルを切り替え中です (数秒かかることがあります)";
    return true;
}

bool SwitchableDetector::is_model_switching() const {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return !requested_model_.empty();
}

std::string SwitchableDetector::model_path() const {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return model_path_;
}

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

    // モデルの切り替えも、推論と同じこのスレッドで行う。
    std::string model_request;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        model_request = requested_model_;
    }
    if (!model_request.empty() && yolo_ != nullptr) {
        std::string message;
        const bool ok = yolo_->set_model(model_request, message);
        compute_ = static_cast<int>(yolo_->target());
        std::cout << "[detector] " << message << (ok ? "" : " (元のモデルのまま)") << "\n";
        std::lock_guard<std::mutex> lock(message_mutex_);
        compute_message_ = ok ? message : message + " (元のモデルのまま)";
        if (ok) {
            model_path_ = yolo_->config().model_path;
            input_size_ = yolo_->config().input_size;
        }
        if (requested_model_ == model_request) {
            requested_model_.clear();  // 切り替え中に別の依頼が来ていたら、次のフレームで処理する
        }
    }

    if (mode() == DetectorMode::Yolo && yolo_ != nullptr) {
        // 次に OpenCV に切り替えたとき、古い背景を使わないようにする。
        needs_motion_reset_ = true;
        yolo_->set_conf_threshold(conf_threshold_.load());
        return yolo_->detect(enhance_frame(bgr_frame));
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
