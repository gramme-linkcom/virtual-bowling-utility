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
        frame_width_ = frame_width;
        frame_height_ = frame_height;
    }
    if (t_s < cooldown_until_s_) {
        reset();
        return std::nullopt;
    }

    // 向き (縦・横) に左右されないよう、短い辺・長い辺を基準にする。
    const double short_side = static_cast<double>(std::min(frame_width, frame_height));
    const double long_side = static_cast<double>(std::max(frame_width, frame_height));
    const double still_px = config_.still_ratio * short_side;
    const double step_px = config_.step_ratio * short_side;
    const double max_jump_px = config_.max_jump_ratio * long_side;

    // 追跡中なら直前の位置に近い候補、そうでなければ一番自信のある候補を使う。
    const BallCandidate* ball = nullptr;
    for (const BallCandidate& c : candidates) {
        if (ball == nullptr) {
            ball = &c;
            continue;
        }
        if (!points_.empty()) {
            const TrackPoint& last = points_.back();
            if (distance(c.x, c.y, last.x, last.y) < distance(ball->x, ball->y, last.x, last.y)) {
                ball = &c;
            }
        } else if (c.confidence > ball->confidence) {
            ball = &c;
        }
    }

    if (ball != nullptr) {
        const TrackPoint point{t_s, ball->x, ball->y};
        if (points_.empty()) {
            points_.push_back(point);
            last_change_s_ = t_s;
        } else {
            const TrackPoint& last = points_.back();
            const double moved = distance(point.x, point.y, last.x, last.y);
            if (moved > max_jump_px) {
                // 急に遠くへ飛んだ = 別の物を検出した可能性が高い。そこから取り直す。
                reset();
                points_.push_back(point);
                last_change_s_ = t_s;
            } else if (!has_moved_) {
                if (moved < still_px) {
                    // まだ投げていない (持っている・置いてある)。開始点の時刻だけ進める。
                    // (位置まで追従させると、ゆっくり転がるボールを「止まっている」と誤判定する)
                    points_.front().t_s = t_s;
                    last_change_s_ = t_s;
                } else {
                    points_.push_back(point);
                    has_moved_ = true;
                    last_change_s_ = t_s;
                }
            } else if (moved >= step_px) {
                if (points_.size() < config_.max_points) {
                    points_.push_back(point);
                }
                last_change_s_ = t_s;
            }
        }
    }

    if (points_.empty() || t_s - last_change_s_ < config_.end_wait_s) {
        return std::nullopt;
    }
    if (!has_moved_) {
        // 動かないまま見えなくなった・止まっているだけ。投球ではない。
        if (ball == nullptr) {
            reset();
        }
        return std::nullopt;
    }

    ThrowResult result = finish();
    reset();
    cooldown_until_s_ = t_s + config_.cooldown_s;
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
    if (last.y < first.y) {
        // 下から上へ転がった (投げる人が画面の下側)。投げる側から見た左右に合わせて反転する。
        // 去年の Python は「投げ始めが画面の下半分か」で判定していたが、動いた向きで判定する方が確実。
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
