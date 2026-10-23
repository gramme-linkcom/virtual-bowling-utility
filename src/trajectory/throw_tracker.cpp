#include "vb/trajectory/throw_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace trajectory {

namespace {

double distance(double ax, double ay, double bx, double by) {
    return std::hypot(ax - bx, ay - by);
}

// 去年の getCode と同じ (0 は正として扱う)。
double sign_of(double value) {
    return value >= 0.0 ? 1.0 : -1.0;
}

}  // namespace

const char* to_string(LaneAxis axis) {
    switch (axis) {
        case LaneAxis::Vertical:
            return "vertical";
        case LaneAxis::Horizontal:
            return "horizontal";
        case LaneAxis::Auto:
        default:
            return "auto";
    }
}

bool parse_lane_axis(const std::string& text, LaneAxis& axis) {
    if (text == "auto") {
        axis = LaneAxis::Auto;
    } else if (text == "vertical") {
        axis = LaneAxis::Vertical;
    } else if (text == "horizontal") {
        axis = LaneAxis::Horizontal;
    } else {
        return false;
    }
    return true;
}

ThrowTracker::ThrowTracker(TrackerConfig config) : config_(std::move(config)) {}

void ThrowTracker::reset() {
    points_.clear();
    has_moved_ = false;
}

void ThrowTracker::update_spots(
    double t_s,
    const std::vector<BallCandidate>& candidates,
    double radius_px
) {
    // しばらく見えない場所は忘れる。
    constexpr double kForgetAfterS = 0.5;
    spots_.erase(
        std::remove_if(
            spots_.begin(),
            spots_.end(),
            [&](const StaticSpot& spot) { return t_s - spot.last_seen_s > kForgetAfterS; }
        ),
        spots_.end()
    );
    for (const BallCandidate& c : candidates) {
        bool is_matched = false;
        for (StaticSpot& spot : spots_) {
            if (distance(c.x, c.y, spot.x, spot.y) < radius_px) {
                spot.last_seen_s = t_s;
                is_matched = true;
                break;
            }
        }
        if (!is_matched && spots_.size() < 64) {
            spots_.push_back({c.x, c.y, t_s, t_s});
        }
    }
}

bool ThrowTracker::is_static(const BallCandidate& c, double t_s, double radius_px) const {
    for (const StaticSpot& spot : spots_) {
        if (distance(c.x, c.y, spot.x, spot.y) < radius_px &&
            spot.last_seen_s - spot.first_seen_s >= config_.static_after_s &&
            t_s - spot.last_seen_s <= 0.0) {
            return true;
        }
    }
    return false;
}

double ThrowTracker::travel_ratio() const {
    if (points_.size() < 2 || frame_width_ <= 0 || frame_height_ <= 0) {
        return 0.0;
    }
    const auto [min_x, max_x] = std::minmax_element(
        points_.begin(), points_.end(), [](const TrackPoint& a, const TrackPoint& b) {
            return a.x < b.x;
        }
    );
    const auto [min_y, max_y] = std::minmax_element(
        points_.begin(), points_.end(), [](const TrackPoint& a, const TrackPoint& b) {
            return a.y < b.y;
        }
    );
    const double travel_x = (max_x->x - min_x->x) / frame_width_;
    const double travel_y = (max_y->y - min_y->y) / frame_height_;
    switch (config_.axis) {
        case LaneAxis::Vertical:
            return travel_y;
        case LaneAxis::Horizontal:
            return travel_x;
        case LaneAxis::Auto:
        default:
            return std::max(travel_x, travel_y);
    }
}

std::optional<ThrowResult> ThrowTracker::update(
    double t_s,
    int frame_width,
    int frame_height,
    const std::vector<BallCandidate>& candidates
) {
    if (frame_width <= 0 || frame_height <= 0) {
        return std::nullopt;
    }
    if (frame_width != frame_width_ || frame_height != frame_height_) {
        // 解像度が変わったら座標の意味が変わるので取り直す。
        reset();
        spots_.clear();
        frame_width_ = frame_width;
        frame_height_ = frame_height;
    }

    // 向き (縦・横) に左右されないよう、短い辺・長い辺を基準にする。
    const double short_side = static_cast<double>(std::min(frame_width, frame_height));
    const double long_side = static_cast<double>(std::max(frame_width, frame_height));
    const double still_px = config_.still_ratio * short_side;
    const double step_px = config_.step_ratio * short_side;

    update_spots(t_s, candidates, still_px);

    if (t_s < cooldown_until_s_) {
        reset();
        return std::nullopt;
    }

    // 使う候補を1つ選ぶ。
    //  - 投球中: 直前の点に一番近い候補。ただし時間に応じた距離より遠いものは別物とみなして使わない。
    //  - 投球前: 止まっている物を除いた中で、(始点があれば) 始点に一番近い、なければ一番自信のある候補。
    const BallCandidate* ball = nullptr;
    if (has_moved_) {
        const TrackPoint& last = points_.back();
        const double elapsed = std::max(0.0, t_s - last.t_s);
        const double gate_px =
            config_.max_jump_ratio * long_side + config_.max_speed_ratio * long_side * elapsed;
        double best = gate_px;
        for (const BallCandidate& c : candidates) {
            const double d = distance(c.x, c.y, last.x, last.y);
            if (d <= best) {
                best = d;
                ball = &c;
            }
        }
    } else {
        for (const BallCandidate& c : candidates) {
            if (is_static(c, t_s, still_px)) {
                continue;
            }
            if (ball == nullptr) {
                ball = &c;
            } else if (!points_.empty()) {
                const TrackPoint& anchor = points_.front();
                if (distance(c.x, c.y, anchor.x, anchor.y) <
                    distance(ball->x, ball->y, anchor.x, anchor.y)) {
                    ball = &c;
                }
            } else if (c.confidence > ball->confidence) {
                ball = &c;
            }
        }
    }

    if (ball != nullptr) {
        const TrackPoint point{t_s, ball->x, ball->y};
        if (points_.empty()) {
            points_.push_back(point);
            last_change_s_ = t_s;
        } else if (!has_moved_) {
            const TrackPoint& anchor = points_.front();
            const double moved = distance(point.x, point.y, anchor.x, anchor.y);
            if (moved < still_px) {
                // まだ投げていない (持っている・置いてある)。開始点の時刻だけ進める。
                // (位置まで追従させると、ゆっくり転がるボールを「止まっている」と誤判定する)
                points_.front().t_s = t_s;
                last_change_s_ = t_s;
            } else if (moved > config_.max_jump_ratio * long_side) {
                // 始点から遠すぎる = 別の物。そこを新しい始点にする。
                points_.front() = point;
                last_change_s_ = t_s;
            } else {
                points_.push_back(point);
                has_moved_ = true;
                last_change_s_ = t_s;
            }
        } else {
            const TrackPoint& last = points_.back();
            if (distance(point.x, point.y, last.x, last.y) >= step_px) {
                if (points_.size() < config_.max_points) {
                    points_.push_back(point);
                }
                last_change_s_ = t_s;
            }
        }
    }

    const double idle_s = t_s - last_change_s_;
    if (points_.empty() || idle_s < config_.end_wait_s) {
        return std::nullopt;
    }
    if (!has_moved_) {
        // 動かないまま見えなくなった・止まっているだけ。投球ではない。
        if (ball == nullptr) {
            reset();
        }
        return std::nullopt;
    }
    // レーン方向の移動がまだ足りないときは、検出が抜けているだけかもしれないので少し長く待つ。
    if (travel_ratio() <= config_.min_travel_ratio && idle_s < config_.short_track_wait_s) {
        return std::nullopt;
    }

    ThrowResult result = finish();
    reset();
    if (result.is_accepted) {
        // 投球として扱わなかったときは待たずに次を受け付ける。
        cooldown_until_s_ = t_s + config_.cooldown_s;
    }
    return result;
}

// 去年の camera.py (detectBallMotion) と同じ計算。
// 横向きのレーンは、縦向き (投げる人が下) と同じ形になるよう 90 度回してから計算する。
ThrowResult ThrowTracker::finish() const {
    ThrowResult result;
    result.points = points_;

    const TrackPoint& first_raw = points_.front();
    const TrackPoint& last_raw = points_.back();
    result.duration_s = last_raw.t_s - first_raw.t_s;

    // 向きを決める。自動なら、画面の大きさに対して大きく動いた方をレーンの向きとする。
    LaneAxis axis = config_.axis;
    if (axis == LaneAxis::Auto) {
        const auto [min_x, max_x] = std::minmax_element(
            points_.begin(), points_.end(), [](const TrackPoint& a, const TrackPoint& b) {
                return a.x < b.x;
            }
        );
        const auto [min_y, max_y] = std::minmax_element(
            points_.begin(), points_.end(), [](const TrackPoint& a, const TrackPoint& b) {
                return a.y < b.y;
            }
        );
        const double travel_x = (max_x->x - min_x->x) / frame_width_;
        const double travel_y = (max_y->y - min_y->y) / frame_height_;
        axis = travel_x > travel_y ? LaneAxis::Horizontal : LaneAxis::Vertical;
    }
    result.axis = axis;

    // 縦向きの座標に直す。横向きは (x, y) → (y, W - x) と 90 度回す
    // (左から右へ転がる投球が「下から上」になり、投げる人から見た左右も保たれる)。
    double width = static_cast<double>(frame_width_);
    double height = static_cast<double>(frame_height_);
    std::vector<TrackPoint> points = points_;
    if (axis == LaneAxis::Horizontal) {
        for (TrackPoint& p : points) {
            const double x = p.x;
            p.x = p.y;
            p.y = width - x;
        }
        std::swap(width, height);
    }
    if (config_.is_mirrored) {
        for (TrackPoint& p : points) {
            p.x = width - p.x;
        }
    }
    result.frame_width = static_cast<int>(width);
    result.frame_height = static_cast<int>(height);

    if (result.duration_s <= 1e-3) {
        result.reject_reason = "移動時間が短すぎます";
        return result;
    }

    const TrackPoint& first = points.front();
    const TrackPoint& last = points.back();
    const auto [min_it, max_it] = std::minmax_element(
        points.begin(), points.end(), [](const TrackPoint& a, const TrackPoint& b) {
            return a.y < b.y;
        }
    );
    const double displacement_y = max_it->y - min_it->y;
    if (displacement_y <= config_.min_travel_ratio * height) {
        std::ostringstream reason;
        reason << "レーン方向 (" << (axis == LaneAxis::Horizontal ? "横" : "縦")
               << ") の移動が短すぎます (" << static_cast<int>(displacement_y / height * 100)
               << "%、必要 " << static_cast<int>(config_.min_travel_ratio * 100) << "%超)";
        result.reject_reason = reason.str();
        return result;
    }

    double direction_x = 1.0;
    double first_position = first.x;
    if (first.y > height / 2.0) {
        // 下から上へ転がった。投げる側から見た左右に合わせて反転する。
        direction_x = -1.0;
        first_position = width - first.x;
    }
    direction_x *= sign_of(last.x - first.x);

    const double speed_x = std::abs(last.x - first.x) / result.duration_s;
    const double speed_y = displacement_y / result.duration_s;

    result.is_accepted = true;
    result.position_px = first_position - width / 2.0;
    result.velocity_x_px = speed_x * direction_x;
    result.velocity_y_px = -speed_y;
    return result;
}

}  // namespace trajectory
