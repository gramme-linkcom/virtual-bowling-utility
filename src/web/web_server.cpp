#include "vb/web/web_server.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace web {

namespace {

// 新しいフレームが来ない間も、この間隔で接続の状態を確認する。
constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(1000);

const char* const kIndexHtml = R"HTML(<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VB2026 Utility Tools</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 0; background: #111; color: #eee; }
  header { padding: 12px 16px; background: #1b1b1b; border-bottom: 1px solid #333; }
  main { padding: 16px; display: grid; gap: 12px; max-width: 960px; margin: 0 auto; }
  .view { position: relative; }
  .view img { width: 100%; display: block; background: #000; border-radius: 8px; }
  .view canvas { position: absolute; left: 0; top: 0; width: 100%; height: 100%; pointer-events: none; }
  .stat { display: flex; gap: 16px; flex-wrap: wrap; font-variant-numeric: tabular-nums; }
  .bad { color: #ff6b6b; }
</style>
</head>
<body>
<header><strong>VB2026 Utility Tools</strong></header>
<main>
  <div class="view">
    <img id="cam" src="/stream" alt="camera">
    <canvas id="overlay"></canvas>
  </div>
  <div class="stat" id="stat">接続中...</div>
  <div class="stat" id="det">検出: -</div>
</main>
<script>
  const cam = document.getElementById('cam');
  const overlay = document.getElementById('overlay');
  const stat = document.getElementById('stat');
  const detStat = document.getElementById('det');

  async function pollStatus() {
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      const s = await r.json();
      stat.className = 'stat' + (s.camera_running ? '' : ' bad');
      stat.textContent = s.camera_running
        ? `FPS ${s.fps.toFixed(1)} / ${s.width}x${s.height} / frame ${s.frame_id}`
        : 'カメラが停止しています';
    } catch (e) {
      stat.className = 'stat bad';
      stat.textContent = 'サーバーに接続できません';
    }
  }

  async function pollDetections() {
    try {
      const r = await fetch('/api/detections', { cache: 'no-store' });
      const d = await r.json();
      overlay.width = cam.clientWidth;
      overlay.height = cam.clientHeight;
      const ctx = overlay.getContext('2d');
      ctx.clearRect(0, 0, overlay.width, overlay.height);

      if (!d.enabled) {
        detStat.className = 'stat bad';
        detStat.textContent = '検出: 無効 (' + d.message + ')';
        return;
      }
      detStat.className = 'stat';
      if (!d.frame_width) {
        detStat.textContent = '検出: 待機中';
        return;
      }

      const sx = overlay.width / d.frame_width;
      const sy = overlay.height / d.frame_height;
      ctx.lineWidth = 2;
      ctx.strokeStyle = '#00ff66';
      ctx.fillStyle = '#00ff66';
      ctx.font = '14px sans-serif';
      for (const b of d.detections) {
        ctx.strokeRect(b.x * sx, b.y * sy, b.w * sx, b.h * sy);
        ctx.fillText('ball ' + b.confidence.toFixed(2), b.x * sx, Math.max(14, b.y * sy - 4));
      }
      detStat.textContent = `検出 ${d.detections.length} 件 / 推論 ${d.inference_ms.toFixed(1)} ms`;
    } catch (e) {
      detStat.className = 'stat bad';
      detStat.textContent = '検出: 取得できません';
    }
  }

  pollStatus();
  pollDetections();
  setInterval(pollStatus, 1000);
  setInterval(pollDetections, 100);
</script>
</body>
</html>
)HTML";

// JSON の文字列に入れても壊れないようにする。
std::string json_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) {
                    escaped += c;
                }
                break;
        }
    }
    return escaped;
}

}  // namespace

WebServer::WebServer(
    WebConfig config,
    camera::CameraStream& camera,
    const detection::DetectionWorker* detection,
    std::string detection_message
)
    : config_(std::move(config)),
      camera_(camera),
      detection_(detection),
      detection_message_(std::move(detection_message)) {
    register_routes();
}

bool WebServer::bind(std::string& error_message) {
    if (!server_.bind_to_port(config_.host, config_.port)) {
        error_message = "ポートを開けません (" + config_.host + ":" + std::to_string(config_.port) +
                        ")。すでに使われている可能性があります";
        return false;
    }
    return true;
}

void WebServer::run() {
    server_.listen_after_bind();
}

void WebServer::stop() {
    server_.stop();
}

void WebServer::register_routes() {
    server_.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kIndexHtml, "text/html; charset=utf-8");
    });

    server_.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        const camera::CameraStatus status = camera_.status();
        std::ostringstream json;
        json << "{\"camera_running\":" << (status.is_running ? "true" : "false")
             << ",\"fps\":" << status.fps << ",\"width\":" << status.width
             << ",\"height\":" << status.height << ",\"frame_id\":" << status.frame_id << "}";
        res.set_header("Cache-Control", "no-store");
        res.set_content(json.str(), "application/json");
    });

    // 最新の検出結果。座標は元のカメラ映像上のピクセル。
    server_.Get("/api/detections", [this](const httplib::Request&, httplib::Response& res) {
        std::ostringstream json;
        if (detection_ == nullptr) {
            json << "{\"enabled\":false,\"message\":\"" << json_escape(detection_message_) << "\"}";
        } else {
            const detection::DetectionResult result = detection_->latest();
            json << std::fixed << std::setprecision(1);
            json << "{\"enabled\":true,\"frame_id\":" << result.frame_id
                 << ",\"frame_width\":" << result.frame_width
                 << ",\"frame_height\":" << result.frame_height
                 << ",\"inference_ms\":" << result.inference_ms << ",\"detections\":[";
            bool is_first = true;
            for (const detection::Detection& d : result.detections) {
                if (!is_first) {
                    json << ",";
                }
                is_first = false;
                json << "{\"x\":" << d.box.x << ",\"y\":" << d.box.y << ",\"w\":" << d.box.width
                     << ",\"h\":" << d.box.height << ",\"confidence\":" << std::setprecision(3)
                     << d.confidence << std::setprecision(1) << "}";
            }
            json << "]}";
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json.str(), "application/json");
    });

    // 最新の1枚だけ欲しいとき用 (確認・デバッグ向け)。
    server_.Get("/snapshot.jpg", [this](const httplib::Request&, httplib::Response& res) {
        std::vector<unsigned char> jpeg;
        if (!camera_.latest_jpeg(jpeg)) {
            res.status = 503;
            res.set_content("まだフレームがありません", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), "image/jpeg");
    });

    // MJPEG: <img src="/stream"> にそのまま流せる連続 JPEG。
    server_.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
        // 接続ごとに「最後に送ったフレーム番号」を持つ。
        auto last_frame_id = std::make_shared<std::uint64_t>(0);
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=frame",
            [this, last_frame_id](std::size_t, httplib::DataSink& sink) {
                std::vector<unsigned char> jpeg;
                std::uint64_t frame_id = 0;
                if (!camera_.wait_for_jpeg(*last_frame_id, kFrameWaitTimeout, jpeg, frame_id)) {
                    if (!camera_.is_running()) {
                        sink.done();
                    }
                    return true;
                }
                *last_frame_id = frame_id;

                const std::string header =
                    "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    std::to_string(jpeg.size()) + "\r\n\r\n";
                // 書き込みに失敗したら、ブラウザーが切断したということ。
                if (!sink.write(header.data(), header.size())) {
                    return false;
                }
                if (!sink.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) {
                    return false;
                }
                return sink.write("\r\n", 2);
            }
        );
    });
}

}  // namespace web
