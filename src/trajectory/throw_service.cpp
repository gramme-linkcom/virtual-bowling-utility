#include "vb/trajectory/throw_service.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>

namespace trajectory {

namespace {

// Unity は更新時刻を秒単位の文字列で比べているので、同じ秒に2回書くと2回目が無視される。
constexpr auto kMinWriteInterval = std::chrono::milliseconds(1100);

// 去年のカメラは 640x480。縦方向の速度は「画面の高さに対する割合」で去年と同じ値になるよう、
// 高さで割ってから 480/640 をかける (= 去年の「幅 640 で割る」と同じ)。
constexpr double kLegacyAspect = 480.0 / 640.0;
// 去年の main.py で前方向の速度だけにかけていた補正。
constexpr double kLegacyForwardBoost = 1.2;

// 去年の camera.py: 直径の 0.5〜1.5 倍の物だけをボールとみなす。
constexpr double kBallSizeMin = 0.5;
constexpr double kBallSizeMax = 1.5;

// 検出範囲の最小の大きさ (画面に対する割合)。小さすぎる範囲は誤操作とみなす。
constexpr double kMinRoiSize = 0.05;

std::string format_motion(const transport::UnityMotion& m) {
    std::ostringstream text;
    text.precision(3);
    text << std::fixed << "位置 " << m.position << " / 横 " << m.velocity_x << " / 前 "
         << m.velocity_y;
    return text.str();
}

std::string trim(const std::string& text) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

ThrowService::ThrowService(ThrowServiceConfig config)
    : config_(std::move(config)), tracker_(config_.tracker) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_settings_locked();
}

// ---- 設定ファイル (key=value の1行ずつ) ----

void ThrowService::load_settings_locked() {
    if (config_.settings_path.empty()) {
        return;
    }
    std::ifstream file(transport::path_from_utf8(config_.settings_path), std::ios::binary);
    if (!file) {
        return;  // まだ保存していない
    }
    const bool has_cli_output = !config_.output_path.empty();
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        std::istringstream in(value);
        in.imbue(std::locale::classic());

        if (key == "output_path") {
            if (!has_cli_output) {
                config_.output_path = value;
            }
        } else if (key == "roi") {
            Roi roi;
            char c1 = 0;
            char c2 = 0;
            char c3 = 0;
            if (in >> roi.x0 >> c1 >> roi.y0 >> c2 >> roi.x1 >> c3 >> roi.y1 && c1 == ',' &&
                c2 == ',' && c3 == ',') {
                roi_ = roi;
            }
        } else if (key == "lane_axis") {
            LaneAxis axis = LaneAxis::Auto;
            if (parse_lane_axis(value, axis)) {
                tracker_.set_orientation(axis, tracker_.config().is_mirrored);
            }
        } else if (key == "mirror") {
            tracker_.set_orientation(tracker_.config().axis, value == "1");
        } else if (key == "bowler_side") {
            BowlerSide side = BowlerSide::Auto;
            if (parse_bowler_side(value, side)) {
                tracker_.set_bowler_side(side);
            }
        } else if (key == "ball_size") {
            double ratio = 0.0;
            if (in >> ratio && ratio > 0.0 && ratio < 1.0) {
                ball_size_ratio_ = ratio;
            }
        } else {
            extra_settings_[key] = value;
        }
    }
    std::cout << "[settings] " << config_.settings_path << " を読み込みました\n";
}

void ThrowService::save_settings_locked() {
    if (config_.settings_path.empty()) {
        return;
    }
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(6);
    text << "# vb2026_utility_tools の設定 (Web UI で変更すると上書きされます)\n";
    text << "output_path=" << config_.output_path << "\n";
    if (roi_) {
        text << "roi=" << roi_->x0 << "," << roi_->y0 << "," << roi_->x1 << "," << roi_->y1 << "\n";
    }
    if (ball_size_ratio_) {
        text << "ball_size=" << *ball_size_ratio_ << "\n";
    }
    text << "lane_axis=" << to_string(tracker_.config().axis) << "\n";
    text << "mirror=" << (tracker_.config().is_mirrored ? 1 : 0) << "\n";
    text << "bowler_side=" << to_string(tracker_.config().bowler_side) << "\n";
    for (const auto& [key, value] : extra_settings_) {
        text << key << "=" << value << "\n";
    }

    std::ofstream file(
        transport::path_from_utf8(config_.settings_path), std::ios::binary | std::ios::trunc
    );
    file << text.str();
    if (!file) {
        std::cerr << "[warn][settings] 設定を保存できません: " << config_.settings_path << "\n";
    }
}

// ---- 変換 ----

transport::UnityMotion ThrowService::to_unity(
    const ThrowResult& result,
    double lane_width_m,
    double speed_scale
) {
    transport::UnityMotion motion;
    if (result.frame_width <= 0 || result.frame_height <= 0) {
        return motion;
    }
    const double width = static_cast<double>(result.frame_width);
    const double height = static_cast<double>(result.frame_height);
    motion.position = result.position_px / width * lane_width_m;
    motion.velocity_x = result.velocity_x_px / width * lane_width_m * speed_scale;
    motion.velocity_y = result.velocity_y_px / height * kLegacyAspect * lane_width_m *
                        kLegacyForwardBoost * speed_scale;
    return motion;
}

void ThrowService::roi_pixels_locked(int& x, int& y, int& width, int& height) const {
    if (!roi_) {
        x = 0;
        y = 0;
        width = frame_width_;
        height = frame_height_;
        return;
    }
    const auto to_px = [](double ratio, int size) {
        return static_cast<int>(std::lround(std::clamp(ratio, 0.0, 1.0) * size));
    };
    x = to_px(roi_->x0, frame_width_);
    y = to_px(roi_->y0, frame_height_);
    width = std::max(1, to_px(roi_->x1, frame_width_) - x);
    height = std::max(1, to_px(roi_->y1, frame_height_) - y);
}

// ---- フレームごとの処理 ----

void ThrowService::on_frame(
    double t_s,
    int frame_width,
    int frame_height,
    const std::vector<BallCandidate>& candidates
) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_width_ = frame_width;
    frame_height_ = frame_height;
    last_candidates_ = candidates;

    int roi_x = 0;
    int roi_y = 0;
    int roi_w = 0;
    int roi_h = 0;
    roi_pixels_locked(roi_x, roi_y, roi_w, roi_h);

    // 検出範囲の中で、登録した大きさに近いものだけを残す。座標は検出範囲の左上を原点にする。
    // (検出範囲 = レーン。範囲の横幅が lane_width_m に、縦がレーンの長さ方向に対応する)
    std::vector<BallCandidate> usable;
    for (const BallCandidate& c : candidates) {
        if (c.x < roi_x || c.x >= roi_x + roi_w || c.y < roi_y || c.y >= roi_y + roi_h) {
            continue;
        }
        if (ball_size_ratio_) {
            const double expected = *ball_size_ratio_ * frame_height;
            if (c.size < expected * kBallSizeMin || c.size > expected * kBallSizeMax) {
                continue;
            }
        }
        BallCandidate local = c;
        local.x -= roi_x;
        local.y -= roi_y;
        usable.push_back(local);
    }

    if (!is_armed_) {
        tracker_.reset();
        return;
    }

    std::optional<ThrowResult> result = tracker_.update(t_s, roi_w, roi_h, usable);
    if (!result) {
        return;
    }

    ThrowRecord record;
    record.id = next_id_++;
    record.is_accepted = result->is_accepted;
    record.duration_s = result->duration_s;
    record.axis = result->axis;
    record.points = std::move(result->points);
    for (TrackPoint& p : record.points) {
        p.x += roi_x;
        p.y += roi_y;
    }

    if (!result->is_accepted) {
        if (result->reject_reason.find("短すぎます") != std::string::npos) {
            // 速すぎて途中の検出が取れず、軌跡が短くなったときによく出る。投げた人に伝わる言い方にする。
            record.message = "早すぎます！少しゆっくり投げてください！ (" + result->reject_reason + ")";
        } else {
            record.message = "投球として扱いませんでした: " + result->reject_reason;
        }
        std::cout << "[throw] #" << record.id << " " << record.message << "\n";
        last_ = std::move(record);
        return;
    }

    record.motion = to_unity(*result, config_.lane_width_m, config_.speed_scale);
    std::cout << "[throw] #" << record.id << " 計測完了 " << format_motion(record.motion) << " ("
              << record.duration_s << " 秒, " << record.points.size() << " 点)\n";
    write_locked(record);
    last_ = std::move(record);
}

// ---- Web UI からの操作 ----

void ThrowService::set_armed(bool is_armed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_armed_ != is_armed) {
        std::cout << "[throw] " << (is_armed ? "計測を開始しました" : "計測を停止しました") << "\n";
    }
    is_armed_ = is_armed;
    tracker_.reset();
}

void ThrowService::reset_tracking() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracker_.reset();
    std::cout << "[throw] 追跡中の軌跡をリセットしました\n";
}

void ThrowService::set_orientation(LaneAxis axis, bool is_mirrored, BowlerSide bowler_side) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracker_.set_orientation(axis, is_mirrored);
    tracker_.set_bowler_side(bowler_side);
    tracker_.reset();
    save_settings_locked();
    std::cout << "[throw] レーンの向き: " << to_string(axis) << (is_mirrored ? " (左右反転)" : "")
              << " / 投げる人: " << to_string(bowler_side) << "\n";
}

bool ThrowService::register_ball_size(std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_height_ <= 0) {
        message = "まだ検出結果がありません";
        return false;
    }
    int roi_x = 0;
    int roi_y = 0;
    int roi_w = 0;
    int roi_h = 0;
    roi_pixels_locked(roi_x, roi_y, roi_w, roi_h);

    // 去年と同じく「ちょうど1個だけ見えているとき」に登録する。
    std::vector<const BallCandidate*> inside;
    for (const BallCandidate& c : last_candidates_) {
        if (c.x >= roi_x && c.x < roi_x + roi_w && c.y >= roi_y && c.y < roi_y + roi_h) {
            inside.push_back(&c);
        }
    }
    if (inside.size() != 1) {
        message = "検出範囲の中にボールが " + std::to_string(inside.size()) +
                  " 個見えています。1個だけ映してください";
        return false;
    }

    ball_size_ratio_ = inside.front()->size / frame_height_;
    save_settings_locked();
    message = "ボールの大きさを登録しました (直径 約 " +
              std::to_string(static_cast<int>(inside.front()->size)) + " px)";
    std::cout << "[throw] " << message << "\n";
    return true;
}

void ThrowService::clear_ball_size() {
    std::lock_guard<std::mutex> lock(mutex_);
    ball_size_ratio_.reset();
    save_settings_locked();
    std::cout << "[throw] ボールの大きさの登録を解除しました\n";
}

bool ThrowService::set_roi(const std::optional<Roi>& roi, std::string& message) {
    std::optional<Roi> normalized;
    if (roi) {
        Roi r;
        r.x0 = std::clamp(std::min(roi->x0, roi->x1), 0.0, 1.0);
        r.x1 = std::clamp(std::max(roi->x0, roi->x1), 0.0, 1.0);
        r.y0 = std::clamp(std::min(roi->y0, roi->y1), 0.0, 1.0);
        r.y1 = std::clamp(std::max(roi->y0, roi->y1), 0.0, 1.0);
        if (!std::isfinite(r.x0) || !std::isfinite(r.y0) || !std::isfinite(r.x1) ||
            !std::isfinite(r.y1) || r.x1 - r.x0 < kMinRoiSize || r.y1 - r.y0 < kMinRoiSize) {
            message = "検出範囲が小さすぎます。もう少し大きく囲んでください";
            return false;
        }
        normalized = r;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    roi_ = normalized;
    tracker_.reset();
    save_settings_locked();
    message = roi_ ? "検出範囲を設定しました" : "検出範囲を解除しました (画面全体)";
    std::cout << "[throw] " << message << "\n";
    return true;
}

bool ThrowService::set_output_path(const std::string& path, std::string& message) {
    // エクスプローラーの「パスのコピー」は "..." で囲まれるので、前後の空白と引用符を外す。
    std::string text = path;
    const auto is_trim = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'';
    };
    while (!text.empty() && is_trim(text.front())) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_trim(text.back())) {
        text.pop_back();
    }

    if (!text.empty()) {
        std::error_code error;
        std::filesystem::path target = transport::path_from_utf8(text);
        if (std::filesystem::is_directory(target, error)) {
            target /= "vector.txt";
        }
        const std::filesystem::path folder = target.parent_path();
        if (!folder.empty() && !std::filesystem::is_directory(folder, error)) {
            message = "フォルダが見つかりません: " + transport::path_to_utf8(folder);
            return false;
        }
        text = transport::path_to_utf8(target);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    config_.output_path = text;
    save_settings_locked();
    message = text.empty() ? "出力先を解除しました (vector.txt を書きません)" : "出力先: " + text;
    std::cout << "[throw] " << message << "\n";
    return true;
}

bool ThrowService::send_test(const transport::UnityMotion& motion, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    ThrowRecord record;
    record.id = next_id_++;
    record.is_test = true;
    record.is_accepted = true;
    record.motion = motion;
    std::cout << "[throw] #" << record.id << " テスト送信 " << format_motion(motion) << "\n";
    const bool is_written = write_locked(record);
    message = record.message;
    last_ = std::move(record);
    return is_written;
}

bool ThrowService::write_locked(ThrowRecord& record) {
    if (config_.output_path.empty()) {
        record.message = "出力先が未設定です (Web UI の「出力先」で指定)";
        std::cout << "[warn][throw] " << record.message << "\n";
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_write_ && now - *last_write_ < kMinWriteInterval) {
        record.message = "前回の書き込みから1秒たっていないので書きませんでした";
        std::cout << "[warn][throw] " << record.message << "\n";
        return false;
    }

    std::string error;
    if (!transport::write_vector_file(config_.output_path, record.motion, error)) {
        record.message = error;
        std::cerr << "[error][throw] " << error << "\n";
        return false;
    }
    last_write_ = now;
    ++throw_count_;
    record.is_written = true;
    record.message = "書き込みました: " + config_.output_path;
    std::cout << "[throw] " << record.message << "\n";
    return true;
}

std::string ThrowService::setting(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = extra_settings_.find(key);
    return it == extra_settings_.end() ? std::string() : it->second;
}

void ThrowService::set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    extra_settings_[key] = value;
    save_settings_locked();
}

ThrowSnapshot ThrowService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ThrowSnapshot snapshot;
    snapshot.output_path = config_.output_path;
    snapshot.throw_count = throw_count_;
    snapshot.is_armed = is_armed_;
    snapshot.is_tracking = tracker_.is_tracking();
    snapshot.frame_width = frame_width_;
    snapshot.frame_height = frame_height_;
    snapshot.roi = roi_;
    snapshot.ball_size_ratio = ball_size_ratio_;
    snapshot.axis = tracker_.config().axis;
    snapshot.is_mirrored = tracker_.config().is_mirrored;
    snapshot.bowler_side = tracker_.config().bowler_side;
    snapshot.last = last_;

    int roi_x = 0;
    int roi_y = 0;
    int roi_w = 0;
    int roi_h = 0;
    roi_pixels_locked(roi_x, roi_y, roi_w, roi_h);
    snapshot.tracking_points = tracker_.current_points();
    for (TrackPoint& p : snapshot.tracking_points) {
        p.x += roi_x;
        p.y += roi_y;
    }
    return snapshot;
}

}  // namespace trajectory
