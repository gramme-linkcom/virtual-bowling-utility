#pragma once

#include "vb/detection/detection.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <string>
#include <vector>

namespace detection {

struct DetectorConfig {
    std::string model_path = "models/best.onnx";
    // 学習・書き出し時の画像サイズ (2026-10-16: imgsz=640 で ONNX に書き出し)
    int input_size = 640;
    // 信頼度のしきい値。ボールの色・照明で調整する。まだ実機映像で調整していない初期値。
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;
    int max_detections = 10;
};

// YOLO (Ultralytics で ONNX に書き出した [1, 4+クラス数, 候補数] 形式) のボール検出器。
// スレッドセーフではない。1つのスレッドからだけ呼ぶこと。
class BallDetector : public IBallDetector {
public:
    explicit BallDetector(DetectorConfig config);

    bool load(std::string& error_message);
    std::vector<Detection> detect(const cv::Mat& bgr_frame) override;

    const DetectorConfig& config() const {
        return config_;
    }

private:
    DetectorConfig config_;
    cv::dnn::Net net_;
    bool is_loaded_ = false;
    bool has_warned_output_ = false;
    bool has_warned_score_ = false;
};

}  // namespace detection
