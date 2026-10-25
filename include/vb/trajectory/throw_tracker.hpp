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

// 投げる人がいる側 (レーンを縦向きに直したときの上下)。
enum class BowlerSide {
    Auto,  // 投球ごとに、ボールが転がった向きで判定する
    Near,  // 画面の下 (横向きのレーンなら左)
    Far,   // 画面の上 (横向きのレーンなら右)
};
const char* to_string(BowlerSide side);
bool parse_bowler_side(const std::string& text, BowlerSide& side);

const char* to_string(LaneAxis axis);
// "auto" / "vertical" / "horizontal" を読む。それ以外なら false。
bool parse_lane_axis(const std::string& text, LaneAxis& axis);

// 割合は画面の短い辺 (レーンの長さ方向の判定だけはレーンの長さ) に対する割合。
struct TrackerConfig {
    LaneAxis axis = LaneAxis::Auto;
    // 左右を反転する (カメラの映像が鏡像になっているとき)。
    bool is_mirrored = false;
    // 投げる人がいる側。固定すると、逆向き (投げる人の方へ戻る) の動きは投球として扱わない。
    BowlerSide bowler_side = BowlerSide::Auto;
    // ボールが動かなくなって (または見えなくなって) から投球を確定するまでの時間。
    // 去年の Python (camera.py) は 0.2 秒。YOLO の取りこぼしを考えて少し長めにする。
    double end_wait_s = 0.3;
    // 投げる前に手で持っている・置いているボールを「止まっている」とみなす移動量。
    double still_ratio = 0.03;
    // 投球中、これ以上動いたら軌跡に点を追加する (細かい揺れで点を増やさない)。
    double step_ratio = 0.01;
    // 追跡中、直前の点からこれより遠い候補は別物 (誤検出) とみなして使わない。
    // 許す距離 = max_jump_ratio × 長い辺 + max_speed_ratio × 長い辺 × 前の点からの経過秒数
    // (速いボールで検出が数フレーム抜けても、追跡が途切れないように時間に応じて広げる)。
    double max_jump_ratio = 0.35;
    double max_speed_ratio = 4.0;
    // 同じ場所にこの秒数以上ある候補は「止まっている物」とみなし、投球の始まりに使わない
    // (ボールに似た置物や、ピンの誤検出に追跡が引っ張られないように)。
    double static_after_s = 1.0;
    // レーン方向の移動が足りない軌跡は、すぐには確定せずこの秒数まで続き (検出の抜け) を待つ。
    double short_track_wait_s = 0.8;
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
    void set_bowler_side(BowlerSide side) {
        config_.bowler_side = side;
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

    // 止まっている物の場所 (投球の始まりの候補から外す)。
    struct StaticSpot {
        double x = 0.0;
        double y = 0.0;
        double first_seen_s = 0.0;
        double last_seen_s = 0.0;
    };
    std::vector<StaticSpot> spots_;
    // 候補 c が止まっている物か (spots_ を更新したあとに呼ぶ)。
    bool is_static(const BallCandidate& c, double t_s, double radius_px) const;
    void update_spots(double t_s, const std::vector<BallCandidate>& candidates, double radius_px);
    // レーン方向に動いた量 (画面の長さに対する割合)。向きの設定に合わせて縦・横を選ぶ。
    double travel_ratio() const;
    int frame_width_ = 0;
    int frame_height_ = 0;
};

}  // namespace trajectory
