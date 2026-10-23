#include "vb/detection/motion_detector.hpp"

// OpenCV 5 では輪郭の面積・外接円などが imgproc から geometry モジュールに移った。
#include <opencv2/geometry.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace detection {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

MotionDetector::MotionDetector(MotionDetectorConfig config) : config_(std::move(config)) {}

void MotionDetector::reset() {
    background_.release();
}

std::vector<Detection> MotionDetector::detect(const cv::Mat& bgr_frame) {
    std::vector<Detection> detections;
    if (bgr_frame.empty()) {
        return detections;
    }
    if (bgr_frame.size() != frame_size_) {
        // 解像度が変わった (カメラを切り替えたなど) ので背景を取り直す。
        frame_size_ = bgr_frame.size();
        background_.release();
    }

    // 縮小 → グレー → ぼかし (細かいノイズを消す)。
    const double scale = bgr_frame.cols > config_.process_width
                             ? static_cast<double>(config_.process_width) / bgr_frame.cols
                             : 1.0;
    cv::Mat small;
    if (scale < 1.0) {
        cv::resize(bgr_frame, small, cv::Size(), scale, scale, cv::INTER_AREA);
    } else {
        small = bgr_frame;
    }
    cv::Mat gray;
    cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0);

    if (background_.empty()) {
        gray.convertTo(background_, CV_32F);
        return detections;  // 最初のフレームは背景を作るだけ
    }

    // 背景との差 → 2値化 → 穴埋め・ノイズ除去。
    cv::Mat background_8u;
    background_.convertTo(background_8u, CV_8U);
    cv::Mat diff;
    cv::absdiff(gray, background_8u, diff);
    cv::Mat mask;
    cv::threshold(diff, mask, config_.diff_threshold, 255, cv::THRESH_BINARY);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 2);

    // 背景をゆっくり今の画像に近づける (照明の変化や、止まった物を背景にする)。
    cv::accumulateWeighted(gray, background_, config_.background_rate);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const double height = static_cast<double>(gray.rows);
    const double min_diameter = config_.min_diameter_ratio * height;
    const double max_diameter = config_.max_diameter_ratio * height;

    for (const std::vector<cv::Point>& contour : contours) {
        const double area = cv::contourArea(contour);
        const double perimeter = cv::arcLength(contour, true);
        if (area <= 0.0 || perimeter <= 0.0) {
            continue;
        }
        cv::Point2f center;
        float radius = 0.0f;
        cv::minEnclosingCircle(contour, center, radius);
        const double diameter = radius * 2.0;
        if (diameter < min_diameter || diameter > max_diameter) {
            continue;
        }
        // 丸さ: 円なら 1。細長い影や人の腕などは小さくなる。
        const double circularity = std::min(1.0, 4.0 * kPi * area / (perimeter * perimeter));
        if (circularity < config_.min_circularity) {
            continue;
        }

        // 元の解像度に戻す。
        const cv::Rect box = cv::boundingRect(contour);
        Detection d;
        d.box = cv::Rect2f(
            static_cast<float>(box.x / scale),
            static_cast<float>(box.y / scale),
            static_cast<float>(box.width / scale),
            static_cast<float>(box.height / scale)
        );
        d.confidence = static_cast<float>(circularity);
        detections.push_back(d);
    }

    // 丸い順に並べ、多すぎる分は捨てる。
    std::sort(detections.begin(), detections.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    if (static_cast<int>(detections.size()) > config_.max_detections) {
        detections.resize(config_.max_detections);
    }
    return detections;
}

}  // namespace detection
