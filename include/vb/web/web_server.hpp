#pragma once

#include "vb/camera/camera_stream.hpp"
#include "vb/detection/detection_worker.hpp"

#include <httplib.h>

#include <string>

namespace web {

struct WebConfig {
    // 既定は自分の PC からだけ見られる設定。別の PC から見るときは 0.0.0.0 にする。
    std::string host = "127.0.0.1";
    int port = 8080;
};

// ツール自体を操作・確認するための Web サーバー。
// Unity への送信(HTTP POST)は別の責務なので、ここには置かない。
class WebServer {
public:
    // detection が nullptr のときは「検出なし」で動く。理由を detection_message に入れる。
    WebServer(
        WebConfig config,
        camera::CameraStream& camera,
        const detection::DetectionWorker* detection,
        std::string detection_message
    );

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // ポートを確保する。使用中などで失敗したら理由を error_message に入れる。
    bool bind(std::string& error_message);
    // stop() が呼ばれるまで処理を返さない。
    void run();
    void stop();

    const WebConfig& config() const {
        return config_;
    }

private:
    void register_routes();

    WebConfig config_;
    camera::CameraStream& camera_;
    const detection::DetectionWorker* detection_;
    std::string detection_message_;
    httplib::Server server_;
};

}  // namespace web
