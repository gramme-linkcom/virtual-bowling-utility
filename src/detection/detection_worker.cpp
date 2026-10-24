#include "vb/detection/detection_worker.hpp"

#include <chrono>
#include <utility>

namespace detection {

namespace {

constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(500);
constexpr auto kIdleWait = std::chrono::milliseconds(100);
// 推論時間の表示の揺れをならす指数移動平均の係数。
constexpr double kTimeSmoothing = 0.1;

}  // namespace

DetectionWorker::DetectionWorker(camera::CameraStream& camera, IBallDetector& detector)
    : camera_(camera), detector_(detector) {}

DetectionWorker::~DetectionWorker() {
    stop();
}

void DetectionWorker::set_result_callback(ResultCallback callback) {
    on_result_ = std::move(callback);
}

void DetectionWorker::set_frame_callback(FrameCallback callback) {
    on_frame_ = std::move(callback);
}

void DetectionWorker::start() {
    if (is_running_) {
        return;
    }
    is_running_ = true;
    thread_ = std::thread(&DetectionWorker::run, this);
}

void DetectionWorker::stop() {
    is_running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

DetectionResult DetectionWorker::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void DetectionWorker::run() {
    cv::Mat frame;
    std::uint64_t last_frame_id = 0;
    double smoothed_ms = 0.0;

    while (is_running_) {
        if (!camera_.is_running()) {
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        std::uint64_t frame_id = 0;
        if (!camera_.wait_for_frame(last_frame_id, kFrameWaitTimeout, frame, frame_id)) {
            continue;
        }
        last_frame_id = frame_id;

        const auto started = std::chrono::steady_clock::now();
        std::vector<Detection> detections = detector_.detect(frame);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        smoothed_ms = (smoothed_ms == 0.0) ? elapsed_ms
                                           : (1.0 - kTimeSmoothing) * smoothed_ms + kTimeSmoothing * elapsed_ms;

        DetectionResult result;
        result.frame_id = frame_id;
        result.frame_width = frame.cols;
        result.frame_height = frame.rows;
        result.inference_ms = smoothed_ms;
        result.timestamp_s =
            std::chrono::duration<double>(started.time_since_epoch()).count();
        result.detections = std::move(detections);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = result;
        }
        // ロックの外で呼ぶ (Web サーバーが latest() で待たされないように)。
        if (on_result_) {
            on_result_(result);
        }
        if (on_frame_) {
            on_frame_(frame, result);
        }
    }
}

}  // namespace detection
