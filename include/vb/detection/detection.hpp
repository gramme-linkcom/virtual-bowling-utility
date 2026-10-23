#pragma once

#include <opencv2/core.hpp>

#include <vector>

namespace detection {

// 1フレームの中で見つかったボール候補。
// 「検出」であって「投球の確定」ではない。確定は追跡・投球判定の責務。
struct Detection {
    cv::Rect2f box;  // 元のフレーム上のピクセル座標 (左上 x, y と 幅, 高さ)
    float confidence = 0.0f;  // 0〜1
    int class_id = 0;
};

// 検出器の共通の形 (YOLO と OpenCV だけの検出を同じように扱う)。
// 1つのスレッド (DetectionWorker) からだけ呼ぶ。
class IBallDetector {
public:
    virtual ~IBallDetector() = default;
    virtual std::vector<Detection> detect(const cv::Mat& bgr_frame) = 0;
};

}  // namespace detection
