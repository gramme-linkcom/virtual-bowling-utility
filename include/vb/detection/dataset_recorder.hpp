#pragma once

#include "vb/detection/detection.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace detection {

// 来年以降の学習用に、カメラ画像と検出結果 (YOLO 形式のラベル) を保存する。
//   <root>/<開始日時>/images/<名前>.jpg   元のカメラ画像 (線や枠は描かない)
//   <root>/<開始日時>/labels/<名前>.txt   YOLO 形式 "class cx cy w h" (0〜1)。検出なしなら空
//   <root>/<開始日時>/meta.csv            名前・投球中か・信頼度など (あとで選ぶため)
//   <root>/<開始日時>/classes.txt         "ball"
// 保存するフレーム:
//   - 投球を追っている間: ボールが写っていなくても保存 (取りこぼしたフレームこそ学習に使える)。最大 15 枚/秒
//   - それ以外: ボールを検出したときだけ、idle_interval_s 秒に1枚
// 書き込みは別スレッドで行うので、検出は遅くならない (間に合わないときは捨てる)。
class DatasetRecorder {
public:
    struct Status {
        bool is_enabled = false;
        std::uint64_t saved = 0;    // この起動で保存した枚数
        std::uint64_t dropped = 0;  // 書き込みが間に合わず捨てた枚数
        std::string folder;         // 今保存しているフォルダ (UTF-8)
        double free_gb = -1.0;      // 保存先の空き容量
        std::string message;        // 止まった理由など
    };

    explicit DatasetRecorder(std::filesystem::path root);
    ~DatasetRecorder();

    DatasetRecorder(const DatasetRecorder&) = delete;
    DatasetRecorder& operator=(const DatasetRecorder&) = delete;

    void set_enabled(bool is_enabled);
    bool is_enabled() const {
        return is_enabled_.load();
    }
    Status status() const;

    // 検出スレッドから毎フレーム呼ぶ。保存するかをここで決め、するなら画像をコピーして書き込み待ちに積む。
    void submit(
        const cv::Mat& bgr_frame,
        std::uint64_t frame_id,
        const std::vector<Detection>& detections,
        bool is_tracking,
        const std::string& detector_name
    );

private:
    struct Job {
        cv::Mat frame;
        std::uint64_t frame_id = 0;
        std::vector<Detection> detections;
        bool is_tracking = false;
        std::string detector_name;
    };

    void run();
    // 新しい保存フォルダを用意する (writer スレッドから)。失敗したら false。
    bool open_session_locked(std::string& error);
    void write_job(const Job& job);

    std::filesystem::path root_;
    std::atomic<bool> is_enabled_{false};

    // 保存するかの判断 (検出スレッドだけが触る)
    double last_tracking_save_s_ = -1e9;
    double last_idle_save_s_ = -1e9;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool is_stopping_ = false;
    std::thread thread_;

    // writer スレッドが使う (status() のために mutex_ で守る)
    std::filesystem::path session_dir_;
    std::ofstream meta_;
    std::uint64_t saved_ = 0;
    std::uint64_t dropped_ = 0;
    double free_gb_ = -1.0;
    std::string message_;
};

}  // namespace detection
