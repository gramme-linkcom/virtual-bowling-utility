#pragma once

#include "vb/camera/camera_stream.hpp"
#include "vb/detection/ball_detector.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace detection {

struct DetectionResult {
    std::uint64_t frame_id = 0;  // どのフレームの結果か (CameraStream の frame_id)
    int frame_width = 0;
    int frame_height = 0;
    double inference_ms = 0.0;  // 1フレームあたりの推論時間 (平滑化済み)
    std::vector<Detection> detections;
};

// カメラの最新フレームを取り出して検出を繰り返す。
// 推論が遅れたときは古いフレームを捨てる (溜めない)。
class DetectionWorker {
public:
    DetectionWorker(camera::CameraStream& camera, BallDetector& detector);
    ~DetectionWorker();

    DetectionWorker(const DetectionWorker&) = delete;
    DetectionWorker& operator=(const DetectionWorker&) = delete;

    void start();
    void stop();

    DetectionResult latest() const;

private:
    void run();

    camera::CameraStream& camera_;
    BallDetector& detector_;
    std::thread thread_;
    std::atomic<bool> is_running_{false};

    mutable std::mutex mutex_;
    DetectionResult latest_;
};

}  // namespace detection
