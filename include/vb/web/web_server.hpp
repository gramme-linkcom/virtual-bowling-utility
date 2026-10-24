#pragma once

#include "vb/camera/camera_stream.hpp"
#include "vb/detection/dataset_recorder.hpp"
#include "vb/detection/detection_worker.hpp"
#include "vb/detection/switchable_detector.hpp"
#include "vb/trajectory/throw_service.hpp"
#include "vb/web/unity_link.hpp"

#include <httplib.h>

#include <mutex>
#include <string>

namespace web {

struct WebConfig {
    // 既定は自分の PC からだけ見られる設定。別の PC から見るときは 0.0.0.0 にする。
    std::string host = "127.0.0.1";
    int port = 8080;
};

// ツール自体を操作・確認するための Web サーバー。
// Unity への受け渡し (vector.txt) は ThrowService の責務。ここは表示とテスト送信の窓口だけ。
class WebServer {
public:
    // detection が nullptr のときは「検出なし」で動く。理由を detection_message に入れる。
    WebServer(
        WebConfig config,
        camera::CameraStream& camera,
        const detection::DetectionWorker* detection,
        std::string detection_message,
        trajectory::ThrowService* throws,
        detection::SwitchableDetector* detector
    );

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // ポートを確保する。使用中などで失敗したら理由を error_message に入れる。
    bool bind(std::string& error_message);
    // stop() が呼ばれるまで処理を返さない。
    void run();
    void stop();

    // 学習用の画像保存 (管理画面の「データセット保存」)。run() より前に設定する。
    void set_dataset_recorder(detection::DatasetRecorder* recorder) {
        dataset_ = recorder;
    }

    const WebConfig& config() const {
        return config_;
    }

private:
    void register_routes();
    // 投球判定が無効なら 503 を返して false。
    bool require_throws(httplib::Response& res) const;

    WebConfig config_;
    camera::CameraStream& camera_;
    const detection::DetectionWorker* detection_;
    std::string detection_message_;
    trajectory::ThrowService* throws_;
    detection::SwitchableDetector* detector_;
    httplib::Server server_;
    UnityLink unity_;  // Web UI の「ゲーム操作」→ Unity (RemoteControl.cs) の中継

    // 休止中 (管理画面の「休止」ボタン)。お客さん用画面に「休止中」を出し、その間は計測を止める。
    std::mutex pause_mutex_;
    bool is_paused_ = false;
    bool was_armed_before_pause_ = false;  // 再開したら計測を元に戻すため

    // Unity でボールを出す位置 (管理画面の「Unity 連携」)。Unity (RemoteControl.cs) が問い合わせのたびに受け取る。
    //   use_throw_position_ … true: カメラで取った投げ始めの位置から出す / false: いつもレーンの真ん中から
    //   invert_throw_position_ … 左右を反転する
    detection::DatasetRecorder* dataset_ = nullptr;

    std::mutex position_mutex_;
    bool use_throw_position_ = true;
    bool invert_throw_position_ = false;
};

}  // namespace web
