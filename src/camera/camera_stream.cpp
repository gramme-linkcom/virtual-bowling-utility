#include "vb/camera/camera_stream.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgcodecs.hpp>

#include <iostream>
#include <utility>

namespace camera {

namespace {

// この回数だけ連続でフレームを取れなければ、カメラ切断とみなす。
constexpr int kMaxConsecutiveReadFailures = 30;
constexpr auto kReadRetryWait = std::chrono::milliseconds(10);
// FPS 表示の急な揺れをならす指数移動平均の係数。
constexpr double kFpsSmoothing = 0.1;

}  // namespace

CameraStream::CameraStream(CameraConfig config) : config_(config) {}

CameraStream::~CameraStream() {
    stop();
}

bool CameraStream::start(std::string& error_message) {
    if (is_running_) {
        error_message = "カメラはすでに動作中です";
        return false;
    }
    if (!capture_.open(config_.index)) {
        error_message = "カメラを開けません (番号: " + std::to_string(config_.index) + ")";
        return false;
    }
    if (config_.width > 0) {
        capture_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
    }
    if (config_.height > 0) {
        capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
    }

    // 開いただけでは映像が来ないカメラがあるので、1枚読めることを確認する。
    cv::Mat probe;
    if (!capture_.read(probe) || probe.empty()) {
        capture_.release();
        error_message = "カメラから最初のフレームを取得できません (番号: " +
                        std::to_string(config_.index) + ")";
        return false;
    }

    is_running_ = true;
    thread_ = std::thread(&CameraStream::capture_loop, this);
    return true;
}

void CameraStream::stop() {
    is_running_ = false;
    condition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    capture_.release();
}

void CameraStream::capture_loop() {
    cv::Mat frame;
    std::vector<unsigned char> encoded;
    const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, config_.jpeg_quality};

    int failures = 0;
    double fps = 0.0;
    std::int64_t last_tick = cv::getTickCount();

    while (is_running_) {
        if (!capture_.read(frame) || frame.empty()) {
            ++failures;
            if (failures >= kMaxConsecutiveReadFailures) {
                std::cerr << "[camera] フレームを取得できません。カメラが切断された可能性があります\n";
                break;
            }
            std::this_thread::sleep_for(kReadRetryWait);
            continue;
        }
        failures = 0;

        const std::int64_t now_tick = cv::getTickCount();
        const double elapsed_s = static_cast<double>(now_tick - last_tick) / cv::getTickFrequency();
        last_tick = now_tick;
        if (elapsed_s > 0.0) {
            const double instant_fps = 1.0 / elapsed_s;
            fps = (fps == 0.0) ? instant_fps : (1.0 - kFpsSmoothing) * fps + kFpsSmoothing * instant_fps;
        }

        if (!cv::imencode(".jpg", frame, encoded, params)) {
            std::cerr << "[camera] JPEG への変換に失敗しました\n";
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            jpeg_ = encoded;
            frame.copyTo(latest_frame_);
            ++frame_id_;
            fps_ = fps;
            width_ = frame.cols;
            height_ = frame.rows;
        }
        condition_.notify_all();
    }

    is_running_ = false;
    condition_.notify_all();
}

bool CameraStream::wait_for_jpeg(
    std::uint64_t last_frame_id,
    std::chrono::milliseconds timeout,
    std::vector<unsigned char>& jpeg,
    std::uint64_t& frame_id
) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [&] { return frame_id_ > last_frame_id || !is_running_; });
    if (frame_id_ <= last_frame_id) {
        return false;
    }
    jpeg = jpeg_;
    frame_id = frame_id_;
    return true;
}

bool CameraStream::wait_for_frame(
    std::uint64_t last_frame_id,
    std::chrono::milliseconds timeout,
    cv::Mat& frame,
    std::uint64_t& frame_id
) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [&] { return frame_id_ > last_frame_id || !is_running_; });
    if (frame_id_ <= last_frame_id || latest_frame_.empty()) {
        return false;
    }
    latest_frame_.copyTo(frame);
    frame_id = frame_id_;
    return true;
}

bool CameraStream::latest_jpeg(std::vector<unsigned char>& jpeg) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jpeg_.empty()) {
        return false;
    }
    jpeg = jpeg_;
    return true;
}

CameraStatus CameraStream::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CameraStatus status;
    status.is_running = is_running_.load();
    status.fps = fps_;
    status.width = width_;
    status.height = height_;
    status.frame_id = frame_id_;
    return status;
}

}  // namespace camera
