#pragma once

#include "vb/detection/detection.hpp"

#include <opencv2/core.hpp>

#include <vector>

namespace detection {

struct MotionDetectorConfig {
    // 背景との明るさの差がこれ以上の画素を「動いた物」とみなす (0〜255)。
    // 小さいほど敏感 (影や照明のちらつきも拾いやすい)。
    int diff_threshold = 30;
    // 背景を更新する速さ (0〜1)。大きいほど、止まった物がすぐ背景に溶け込む。
    double background_rate = 0.02;
    // ボールとみなす大きさ (直径。画面の高さに対する割合)。
    double min_diameter_ratio = 0.02;
    double max_diameter_ratio = 0.25;
    // 丸さ (1 が真円)。これ未満の形は無視する。
    double min_circularity = 0.4;
    // 処理を軽くするため、この幅に縮小してから調べる。
    int process_width = 640;
    int max_detections = 10;
};

// YOLO を使わず、OpenCV だけで「動いている丸い物」を探す (去年の Python と同じ背景差分の考え方)。
// 止まっているボールは背景に溶け込むので検出されない (投げる前のボールを拾わないので都合がよい)。
class MotionDetector : public IBallDetector {
public:
    explicit MotionDetector(MotionDetectorConfig config);

    std::vector<Detection> detect(const cv::Mat& bgr_frame) override;

    // 背景を取り直す (カメラを切り替えたときなど)。
    void reset();

    void set_diff_threshold(int threshold) {
        config_.diff_threshold = threshold;
    }
    const MotionDetectorConfig& config() const {
        return config_;
    }

private:
    MotionDetectorConfig config_;
    cv::Mat background_;  // CV_32F の背景 (縮小後のグレー画像)
    cv::Size frame_size_;
};

}  // namespace detection
