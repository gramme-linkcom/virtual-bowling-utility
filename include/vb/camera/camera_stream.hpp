#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace camera {

struct CameraConfig {
    int index = 0;
    int width = 0;   // 0 ならカメラの既定値
    int height = 0;  // 0 ならカメラの既定値
    int jpeg_quality = 80;  // 0〜100。Web 表示用の圧縮率
};

struct CameraStatus {
    bool is_running = false;
    double fps = 0.0;
    int width = 0;
    int height = 0;
    std::uint64_t frame_id = 0;  // 取得したフレームの通し番号
};

// 専用スレッドでカメラを読み続け、最新フレームを JPEG にして保持する。
class CameraStream {
public:
    explicit CameraStream(CameraConfig config);
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    // カメラを開き、取得スレッドを開始する。失敗したら理由を error_message に入れる。
    bool start(std::string& error_message);
    void stop();

    // last_frame_id より新しいフレームが来るまで待つ。来なければ false。
    bool wait_for_jpeg(
        std::uint64_t last_frame_id,
        std::chrono::milliseconds timeout,
        std::vector<unsigned char>& jpeg,
        std::uint64_t& frame_id
    );
    bool latest_jpeg(std::vector<unsigned char>& jpeg) const;

    // 検出など、生の画像 (BGR) が必要な処理向け。last_frame_id より新しいフレームを待つ。
    bool wait_for_frame(
        std::uint64_t last_frame_id,
        std::chrono::milliseconds timeout,
        cv::Mat& frame,
        std::uint64_t& frame_id
    );

    CameraStatus status() const;
    bool is_running() const {
        return is_running_.load();
    }

private:
    void capture_loop();

    CameraConfig config_;
    cv::VideoCapture capture_;
    std::thread thread_;
    std::atomic<bool> is_running_{false};

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<unsigned char> jpeg_;
    cv::Mat latest_frame_;
    std::uint64_t frame_id_ = 0;
    double fps_ = 0.0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace camera
