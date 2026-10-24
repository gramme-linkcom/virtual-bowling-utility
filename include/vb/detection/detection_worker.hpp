#pragma once

#include "vb/camera/camera_stream.hpp"
#include "vb/detection/detection.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace detection {

struct DetectionResult {
    std::uint64_t frame_id = 0;  // どのフレームの結果か (CameraStream の frame_id)
    int frame_width = 0;
    int frame_height = 0;
    double inference_ms = 0.0;  // 1フレームあたりの推論時間 (平滑化済み)
    double timestamp_s = 0.0;  // フレームを受け取った時刻 (秒。steady_clock 基準)
    std::vector<Detection> detections;
};

// カメラの最新フレームを取り出して検出を繰り返す。
// 推論が遅れたときは古いフレームを捨てる (溜めない)。
class DetectionWorker {
public:
    DetectionWorker(camera::CameraStream& camera, IBallDetector& detector);
    ~DetectionWorker();

    DetectionWorker(const DetectionWorker&) = delete;
    DetectionWorker& operator=(const DetectionWorker&) = delete;

    // 1フレーム検出するたびに (検出スレッドから) 呼ばれる。start() より前に設定すること。
    using ResultCallback = std::function<void(const DetectionResult&)>;
    void set_result_callback(ResultCallback callback);
    // 結果と一緒に、検出に使ったフレームも渡す (学習用の画像保存など)。結果のコールバックの後に呼ばれる。
    // frame は次のフレームで上書きされるので、残すならコピーすること。start() より前に設定すること。
    using FrameCallback = std::function<void(const cv::Mat& frame, const DetectionResult&)>;
    void set_frame_callback(FrameCallback callback);

    void start();
    void stop();

    DetectionResult latest() const;

private:
    void run();

    camera::CameraStream& camera_;
    IBallDetector& detector_;
    std::thread thread_;
    std::atomic<bool> is_running_{false};
    ResultCallback on_result_;
    FrameCallback on_frame_;

    mutable std::mutex mutex_;
    DetectionResult latest_;
};

}  // namespace detection
