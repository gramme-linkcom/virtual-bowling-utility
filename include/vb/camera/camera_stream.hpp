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

    // 別のカメラに切り替える。新しいカメラが使えなければ、元のカメラに戻して false を返す。
    bool switch_to(int index, std::string& error_message);
    // 0〜max_index のうち、映像が取れるカメラ番号を返す (使用中のカメラも含む)。数秒かかる。
    std::vector<int> find_cameras(int max_index);
    int index() const {
        return index_.load();
    }

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
    // control_mutex_ を取った状態で呼ぶ。
    bool start_locked(std::string& error_message);
    void stop_locked();

    CameraConfig config_;
    cv::VideoCapture capture_;
    std::thread thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<int> index_{0};
    // 開始・停止・切り替え・カメラ探しを同時に行わないためのロック。
    std::mutex control_mutex_;

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
