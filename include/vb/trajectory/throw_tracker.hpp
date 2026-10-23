#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace trajectory {

// 1フレーム分のボール候補 (検出枠の中心)。座標はカメラ映像上のピクセル。
struct BallCandidate {
    double x = 0.0;
    double y = 0.0;
    double confidence = 0.0;
    double size = 0.0;  // 検出枠の大きさ (幅と高さの平均、ピクセル)
};

struct TrackPoint {
    double t_s = 0.0;  // 秒 (単調増加する時計)
    double x = 0.0;
    double y = 0.0;
};

// 画面の中でボールが転がる向き。
enum class LaneAxis {
    Auto,        // 投球ごとに、縦と横で大きく動いた方を自動で選ぶ
    Vertical,    // 上下に転がる (投げる人は画面の上か下)
    Horizontal,  // 左右に転がる (投げる人は画面の左か右。カメラを 90 度回して付けたとき)
};

const char* to_string(LaneAxis axis);
// "auto" / "vertical" / "horizontal" を読む。それ以外なら false。
bool parse_lane_axis(const std::string& text, LaneAxis& axis);

// 割合は画面の短い辺 (レーンの長さ方向の判定だけはレーンの長さ) に対する割合。
struct TrackerConfig {
    LaneAxis axis = LaneAxis::Auto;
    // 左右を反転する (カメラの映像が鏡像になっているとき)。
    bool is_mirrored = false;
    // ボールが動かなくなって (または見えなくなって) から投球を確定するまでの時間。
    // 去年の Python (camera.py) は 0.2 秒。YOLO の取りこぼしを考えて少し長めにする。
    double end_wait_s = 0.3;
    // 投げる前に手で持っている・置いているボールを「止まっている」とみなす移動量。
    double still_ratio = 0.03;
    // 投球中、これ以上動いたら軌跡に点を追加する (細かい揺れで点を増やさない)。
    double step_ratio = 0.01;
    // 1回でこれ以上飛んだら別物 (誤検出) とみなし、軌跡をそこから取り直す。
    double max_jump_ratio = 0.35;
    // レーンの長さ方向の移動がこれ未満なら投球として扱わない (去年の Python と同じ 0.5)。
    double min_travel_ratio = 0.5;
    // 投球を確定してから次の投球を受け付けるまでの時間。
    double cooldown_s = 2.0;
    std::size_t max_points = 600;
};

// 投球1回分の結果。速度はピクセル/秒で、向きは去年の Python と同じ決め方。
struct ThrowResult {
    bool is_accepted = false;
    std::string reject_reason;  // is_accepted が false のときの理由
    LaneAxis axis = LaneAxis::Vertical;  // この投球で使った向き (Auto にはならない)

    // レーンを縦向きに直したときの大きさ (横向きのレーンなら幅と高さが入れ替わる)。
    int frame_width = 0;
    int frame_height = 0;
    double duration_s = 0.0;
    std::vector<TrackPoint> points;  // 元の画面の座標 (表示用)

    // 去年の Motion と同じ意味:
    //   position_px … 投げ始めの横位置 (画面中央を 0。投げる側から見て右が正)
    //   velocity_x_px … 横方向の速度、velocity_y_px … 前方向の速度 (常に負)
    double position_px = 0.0;
    double velocity_x_px = 0.0;
    double velocity_y_px = 0.0;
};

// フレームごとの検出結果から、投球の始まりと終わりを判定する。
// スレッドセーフではない (呼び出し側でロックする)。
class ThrowTracker {
public:
    explicit ThrowTracker(TrackerConfig config);

    // 1フレームごとに呼ぶ。投球が終わったと判断したときだけ結果を返す (却下も含む)。
    std::optional<ThrowResult> update(
        double t_s,
        int frame_width,
        int frame_height,
        const std::vector<BallCandidate>& candidates
    );

    // 追跡中の軌跡 (表示用)。
    const std::vector<TrackPoint>& current_points() const {
        return points_;
    }
    bool is_tracking() const {
        return has_moved_;
    }

    const TrackerConfig& config() const {
        return config_;
    }
    void set_orientation(LaneAxis axis, bool is_mirrored) {
        config_.axis = axis;
        config_.is_mirrored = is_mirrored;
    }

    // 追跡中の軌跡を捨てる (去年の R キー)。
    void reset();

private:
    ThrowResult finish() const;

    TrackerConfig config_;
    std::vector<TrackPoint> points_;
    bool has_moved_ = false;
    double last_change_s_ = 0.0;
    double cooldown_until_s_ = 0.0;
    int frame_width_ = 0;
    int frame_height_ = 0;
};

}  // namespace trajectory
