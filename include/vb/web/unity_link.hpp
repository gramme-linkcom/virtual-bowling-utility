#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace web {

// Unity の状態 (Unity が定期的に送ってくる)。
struct UnityStatus {
    bool is_connected = false;  // 最近 Unity から連絡があったか
    std::string scene;          // 今のシーン名
    std::string state;          // "StartMenu" / "Playing" / "Menu" など
    std::optional<bool> is_gutter_wall;  // ガーターの壁が上がっているか (分からなければ無し)
    bool is_waiting_name = false;        // ハイスコアの名前入力欄が出ているか
    int score = -1;                      // 今のスコア (分からなければ -1)
    long long last_seen_ms = -1;         // 最後の連絡から何ミリ秒たったか
};

// Web UI → Unity の操作を中継する。
// Web UI がボタンで命令を積み、Unity (RemoteControl.cs) が定期的に取りに来て実行する。
// Web サーバーの複数のスレッドから呼ばれるので、中でロックする。
class UnityLink {
public:
    // 受け付ける命令。Unity 側の RemoteControl.cs と同じ名前。
    static bool is_valid_command(const std::string& command);
    // ハイスコアの名前入力の命令 ("name:" + 名前)。名前が不正なら false と理由。
    static bool make_name_command(const std::string& name, std::string& command, std::string& error);

    void push(const std::string& command);

    // Unity から呼ばれる。状態を記録し、たまっている命令を渡して空にする。
    std::vector<std::string> poll(
        const std::string& scene,
        const std::string& state,
        std::optional<bool> is_gutter_wall,
        bool is_waiting_name,
        int score
    );

    UnityStatus status() const;

private:
    using Clock = std::chrono::steady_clock;
    struct Pending {
        std::string command;
        Clock::time_point queued_at;
    };

    mutable std::mutex mutex_;
    std::deque<Pending> pending_;
    std::optional<Clock::time_point> last_seen_;
    std::string scene_;
    std::string state_;
    std::optional<bool> is_gutter_wall_;
    bool is_waiting_name_ = false;
    int score_ = -1;
};

}  // namespace web
