#include "vb/web/unity_link.hpp"

#include <algorithm>
#include <array>

namespace web {

namespace {

// Unity が取りに来なかった命令は、この時間で捨てる
// (Unity を起動していない間に押したボタンが、あとで勝手に実行されないように)。
constexpr auto kCommandLifetime = std::chrono::seconds(3);
// この時間 Unity から連絡がなければ「未接続」とみなす。
constexpr auto kConnectionTimeout = std::chrono::seconds(2);
constexpr std::size_t kMaxPending = 16;

constexpr std::array<const char*, 6> kCommands = {
    "start",       // スペースキー (スタート画面・メニュー画面で開始)
    "reset",       // Esc キー (1投目からやり直し)
    "gutter_on",   // ガーターの壁を上げる (G キー)
    "gutter_off",  // ガーターの壁を下げる (G キー)
    "mode_prev",   // ← キー (メニュー画面のモード選択)
    "mode_next",   // → キー (メニュー画面のモード選択)
};

}  // namespace

bool UnityLink::is_valid_command(const std::string& command) {
    return std::find(kCommands.begin(), kCommands.end(), command) != kCommands.end();
}

bool UnityLink::make_name_command(
    const std::string& name,
    std::string& command,
    std::string& error
) {
    // 改行やタブは消し、前後の空白を外す。
    std::string cleaned;
    for (const char c : name) {
        if (c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        cleaned += c;
    }
    const auto first = cleaned.find_first_not_of(' ');
    const auto last = cleaned.find_last_not_of(' ');
    cleaned = first == std::string::npos ? "" : cleaned.substr(first, last - first + 1);
    if (cleaned.empty()) {
        error = "名前を入れてください";
        return false;
    }
    // 日本語 (UTF-8 で1文字3バイト) でも 20 文字くらいまで。
    constexpr std::size_t kMaxNameBytes = 60;
    if (cleaned.size() > kMaxNameBytes) {
        error = "名前が長すぎます (20 文字くらいまで)";
        return false;
    }
    command = "name:" + cleaned;
    return true;
}

void UnityLink::set_name_draft(const std::string& text) {
    // 制御文字は消し、長すぎる分は UTF-8 の文字の途中で切らないように落とす。
    constexpr std::size_t kMaxDraftBytes = 90;
    std::string cleaned;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            continue;
        }
        cleaned += c;
    }
    if (cleaned.size() > kMaxDraftBytes) {
        std::size_t end = kMaxDraftBytes;
        while (end > 0 && (static_cast<unsigned char>(cleaned[end]) & 0xC0) == 0x80) {
            --end;
        }
        cleaned.resize(end);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    name_draft_ = cleaned;
}

void UnityLink::push(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() >= kMaxPending) {
        pending_.pop_front();
    }
    pending_.push_back({command, Clock::now()});
}

std::vector<std::string> UnityLink::poll(
    const std::string& scene,
    const std::string& state,
    std::optional<bool> is_gutter_wall,
    bool is_waiting_name,
    int score
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    last_seen_ = now;
    scene_ = scene;
    state_ = state;
    is_gutter_wall_ = is_gutter_wall;
    if (is_waiting_name_ && !is_waiting_name) {
        // 名前入力が終わったら、入力中の名前も消す (次のハイスコアに残さない)。
        name_draft_.clear();
    }
    is_waiting_name_ = is_waiting_name;
    score_ = score;

    std::vector<std::string> commands;
    for (const Pending& p : pending_) {
        if (now - p.queued_at <= kCommandLifetime) {
            commands.push_back(p.command);
        }
    }
    pending_.clear();
    return commands;
}

UnityStatus UnityLink::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    UnityStatus status;
    if (last_seen_) {
        const auto elapsed = Clock::now() - *last_seen_;
        status.last_seen_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        status.is_connected = elapsed <= kConnectionTimeout;
    }
    status.scene = scene_;
    status.state = state_;
    status.is_gutter_wall = is_gutter_wall_;
    status.is_waiting_name = is_waiting_name_;
    status.score = score_;
    status.name_draft = name_draft_;
    return status;
}

}  // namespace web
