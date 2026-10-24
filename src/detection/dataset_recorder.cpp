#include "vb/detection/dataset_recorder.hpp"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <system_error>
#include <utility>

namespace detection {

namespace {

constexpr double kTrackingIntervalS = 1.0 / 15.0;  // 投球中は最大 15 枚/秒
constexpr double kIdleIntervalS = 5.0;              // 投球中でないときは 5 秒に1枚
constexpr std::size_t kMaxQueue = 30;               // 書き込み待ちがこれを超えたら捨てる
constexpr double kMinFreeGb = 2.0;                  // 空きがこれを切ったら止める
constexpr int kJpegQuality = 92;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// 今の日時を format (strftime 形式) で。ミリ秒が要るときは with_ms。
std::string local_time_text(const char* format, bool with_ms) {
    static std::mutex time_mutex;  // std::localtime はスレッドセーフでないため
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buffer[64] = {};
    {
        std::lock_guard<std::mutex> lock(time_mutex);
        const std::tm* tm = std::localtime(&t);
        if (tm != nullptr) {
            std::strftime(buffer, sizeof(buffer), format, tm);
        }
    }
    std::string text = buffer;
    if (with_ms) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        char ms_text[8] = {};
        std::snprintf(ms_text, sizeof(ms_text), "_%03d", static_cast<int>(ms));
        text += ms_text;
    }
    return text;
}

double free_space_gb(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::space_info info = std::filesystem::space(path, error);
    if (error) {
        return -1.0;
    }
    return static_cast<double>(info.available) / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace

DatasetRecorder::DatasetRecorder(std::filesystem::path root) : root_(std::move(root)) {
    thread_ = std::thread([this] { run(); });
}

DatasetRecorder::~DatasetRecorder() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DatasetRecorder::set_enabled(bool is_enabled) {
    if (is_enabled_.exchange(is_enabled) == is_enabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_enabled) {
        message_.clear();
    }
    std::cout << "[dataset] " << (is_enabled ? "画像の保存を始めました" : "画像の保存を止めました") << "\n";
}

DatasetRecorder::Status DatasetRecorder::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Status status;
    status.is_enabled = is_enabled_.load();
    status.saved = saved_;
    status.dropped = dropped_;
    status.folder = session_dir_.empty() ? to_utf8(root_) : to_utf8(session_dir_);
    status.free_gb = free_gb_;
    status.message = message_;
    return status;
}

void DatasetRecorder::submit(
    const cv::Mat& bgr_frame,
    std::uint64_t frame_id,
    const std::vector<Detection>& detections,
    bool is_tracking,
    const std::string& detector_name
) {
    if (!is_enabled_.load() || bgr_frame.empty()) {
        return;
    }
    const double t = now_s();
    if (is_tracking) {
        if (t - last_tracking_save_s_ < kTrackingIntervalS) {
            return;
        }
        last_tracking_save_s_ = t;
    } else {
        if (detections.empty() || t - last_idle_save_s_ < kIdleIntervalS) {
            return;
        }
        last_idle_save_s_ = t;
    }

    Job job;
    job.frame = bgr_frame.clone();  // 検出スレッドのフレームは次で上書きされるのでコピーする
    job.frame_id = frame_id;
    job.detections = detections;
    job.is_tracking = is_tracking;
    job.detector_name = detector_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueue) {
            ++dropped_;
            return;
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

bool DatasetRecorder::open_session_locked(std::string& error) {
    const std::filesystem::path dir = root_ / local_time_text("%Y%m%d_%H%M%S", false);
    std::error_code ec;
    std::filesystem::create_directories(dir / "images", ec);
    if (!ec) {
        std::filesystem::create_directories(dir / "labels", ec);
    }
    if (ec) {
        error = "保存フォルダを作れません: " + to_utf8(dir) + " (" + ec.message() + ")";
        return false;
    }
    std::ofstream(dir / "classes.txt") << "ball\n";
    meta_.open(dir / "meta.csv", std::ios::app);
    meta_ << "file,frame_id,tracking,detector,count,confidences\n";
    session_dir_ = dir;
    std::cout << "[dataset] 保存先: " << to_utf8(dir) << "\n";
    return true;
}

void DatasetRecorder::run() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return is_stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;  // 止める (残りは書き終えてから)
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            if (session_dir_.empty()) {
                std::string error;
                if (!open_session_locked(error)) {
                    message_ = error;
                    std::cerr << "[dataset] " << error << "\n";
                    is_enabled_ = false;
                    queue_.clear();
                    continue;
                }
            }
        }
        write_job(job);
    }
}

void DatasetRecorder::write_job(const Job& job) {
    const std::string name = local_time_text("%Y%m%d_%H%M%S", true) + "_f" + std::to_string(job.frame_id);

    // 画像。日本語のフォルダでも書けるように、メモリで JPEG にしてから std::filesystem::path で書く。
    std::vector<unsigned char> jpeg;
    if (!cv::imencode(".jpg", job.frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, kJpegQuality})) {
        return;
    }
    {
        std::ofstream image(session_dir_ / "images" / (name + ".jpg"), std::ios::binary);
        image.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        if (!image) {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = "画像を書き込めません (容量不足かもしれません)";
            is_enabled_ = false;
            return;
        }
    }

    // ラベル (YOLO 形式)。検出が無ければ空のファイル (「ボールなし」の画像として使える)。
    const double w = static_cast<double>(job.frame.cols);
    const double h = static_cast<double>(job.frame.rows);
    std::ofstream label(session_dir_ / "labels" / (name + ".txt"));
    std::string confidences;
    for (const Detection& d : job.detections) {
        const double x0 = std::clamp(static_cast<double>(d.box.x), 0.0, w);
        const double y0 = std::clamp(static_cast<double>(d.box.y), 0.0, h);
        const double x1 = std::clamp(static_cast<double>(d.box.x + d.box.width), 0.0, w);
        const double y1 = std::clamp(static_cast<double>(d.box.y + d.box.height), 0.0, h);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        char line[128] = {};
        std::snprintf(
            line, sizeof(line), "%d %.6f %.6f %.6f %.6f\n", d.class_id,
            (x0 + x1) / 2.0 / w, (y0 + y1) / 2.0 / h, (x1 - x0) / w, (y1 - y0) / h
        );
        label << line;
        char conf[16] = {};
        std::snprintf(conf, sizeof(conf), "%.3f", d.confidence);
        confidences += (confidences.empty() ? "" : ";") + std::string(conf);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    meta_ << name << "," << job.frame_id << "," << (job.is_tracking ? 1 : 0) << "," << job.detector_name
          << "," << job.detections.size() << "," << confidences << "\n";
    ++saved_;
    if (saved_ % 50 == 1) {
        meta_.flush();
        free_gb_ = free_space_gb(session_dir_);
        if (free_gb_ >= 0.0 && free_gb_ < kMinFreeGb) {
            message_ = "空き容量が少ないので保存を止めました";
            is_enabled_ = false;
            std::cerr << "[dataset] " << message_ << "\n";
        }
    }
}

}  // namespace detection
