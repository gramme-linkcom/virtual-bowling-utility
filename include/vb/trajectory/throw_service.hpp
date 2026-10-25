#pragma once

#include "vb/trajectory/throw_tracker.hpp"
#include "vb/transport/vector_file_writer.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trajectory {

struct ThrowServiceConfig {
    // Unity が読む vector.txt のパス (UTF-8)。空なら書かない (判定と表示だけする)。
    // 空でなければ、設定ファイルに保存された出力先より優先する (--output)。
    std::string output_path;
    // Web UI で変えた設定 (出力先・検出範囲・ボールの大きさ) を保存するファイル。空なら保存しない。
    std::string settings_path;
    // 実際のレーンの横幅 [m]。去年の constant.py の LANE_WIDTH と同じ。
    // 検出範囲を設定したときは、その範囲の (レーンと直角な方向の) 幅がこの長さに対応する。
    double lane_width_m = 5.0;
    // 速度にかける倍率。会場でボールが強すぎる・弱すぎるときに調整する。
    double speed_scale = 1.0;
    TrackerConfig tracker;
};

// 検出範囲。画面に対する割合 (0〜1) で持つので、解像度を変えても使える。
struct Roi {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 1.0;
    double y1 = 1.0;
};

// 投球1回分の記録 (Web UI 表示用)。
struct ThrowRecord {
    int id = 0;
    bool is_test = false;  // Web UI のテスト送信なら true
    bool is_accepted = false;
    bool is_written = false;  // vector.txt に書けたか
    std::string message;
    transport::UnityMotion motion;
    double duration_s = 0.0;
    LaneAxis axis = LaneAxis::Vertical;  // この投球で使ったレーンの向き
    std::vector<TrackPoint> points;  // 画面全体のピクセル座標
};

struct ThrowSnapshot {
    std::string output_path;
    int throw_count = 0;  // vector.txt に書いた回数
    bool is_armed = false;  // 計測中か (Web UI の「計測開始」)
    bool is_tracking = false;
    int frame_width = 0;
    int frame_height = 0;
    std::vector<TrackPoint> tracking_points;  // 画面全体のピクセル座標
    std::optional<Roi> roi;
    // 登録したボールの大きさ (画面の高さに対する割合)。未登録なら大きさで絞り込まない。
    std::optional<double> ball_size_ratio;
    LaneAxis axis = LaneAxis::Auto;
    bool is_mirrored = false;
    BowlerSide bowler_side = BowlerSide::Auto;
    std::optional<ThrowRecord> last;
};

// 検出結果 → 投球判定 → Unity 向けの値に変換 → vector.txt に書く、をまとめて行う。
// 検出スレッドと Web サーバーの両方から呼ばれるので、中でロックする。
class ThrowService {
public:
    explicit ThrowService(ThrowServiceConfig config);

    ThrowService(const ThrowService&) = delete;
    ThrowService& operator=(const ThrowService&) = delete;

    // 検出スレッドから 1フレームごとに呼ぶ。座標は画面全体のピクセル。
    void on_frame(
        double t_s,
        int frame_width,
        int frame_height,
        const std::vector<BallCandidate>& candidates
    );

    // 去年の Python の流れ (ボール登録 → 計測モード、R でリセット) に合わせた操作。
    void set_armed(bool is_armed);
    void reset_tracking();

    // 今映っているボールの大きさを登録する (候補がちょうど1個のときだけ)。
    // 以後、大きさが 0.5〜1.5 倍の候補だけを使う (去年の camera.py と同じ幅)。
    bool register_ball_size(std::string& message);
    void clear_ball_size();

    // 検出範囲を設定する。nullopt なら画面全体。
    bool set_roi(const std::optional<Roi>& roi, std::string& message);

    // レーンの向き (自動・縦・横) と左右反転を設定する。
    void set_orientation(LaneAxis axis, bool is_mirrored, BowlerSide bowler_side);

    // 出力先を変える。フォルダを指定したら中の vector.txt にする。空文字なら書き込みを止める。
    bool set_output_path(const std::string& path, std::string& message);

    // ボールを転がさずに Unity との連携を確かめる。
    bool send_test(const transport::UnityMotion& motion, std::string& message);

    ThrowSnapshot snapshot() const;

    // 投球判定以外の設定 (カメラ番号など) も同じ settings.ini に保存する。無ければ空文字。
    std::string setting(const std::string& key) const;
    void set_setting(const std::string& key, const std::string& value);

    // ピクセル単位の結果を Unity 向けに変換する (去年の main.py の calcBallMotionForUnity)。
    static transport::UnityMotion to_unity(
        const ThrowResult& result,
        double lane_width_m,
        double speed_scale
    );

private:
    // 以下はロックを取った状態で呼ぶ。
    bool write_locked(ThrowRecord& record);
    void load_settings_locked();
    void save_settings_locked();
    // 検出範囲のピクセル座標 (左上 x, y, 幅, 高さ)。
    void roi_pixels_locked(int& x, int& y, int& width, int& height) const;

    ThrowServiceConfig config_;
    mutable std::mutex mutex_;
    ThrowTracker tracker_;
    bool is_armed_ = false;
    std::optional<Roi> roi_;
    std::optional<double> ball_size_ratio_;
    std::vector<BallCandidate> last_candidates_;
    std::map<std::string, std::string> extra_settings_;
    std::optional<ThrowRecord> last_;
    int next_id_ = 1;
    int throw_count_ = 0;
    int frame_width_ = 0;
    int frame_height_ = 0;
    std::optional<std::chrono::steady_clock::time_point> last_write_;
};

}  // namespace trajectory
