#pragma once

#include "vb/detection/detection.hpp"
#include "vb/detection/ort_engine.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <memory>
#include <string>
#include <vector>

namespace detection {

// YOLO の計算に使う装置。
enum class ComputeTarget {
    Cpu,         // CPU (既定。どの PC でも動く)
    OpenCl,      // GPU (OpenCL)。NVIDIA / Intel / AMD のドライバーに入っている OpenCL を使う
    OpenClFp16,  // GPU (OpenCL, 16bit 浮動小数)。対応 GPU ならさらに速いことがある
    OnnxRuntime, // CPU (ONNX Runtime)。OpenCV より速いことが多い。exe の隣に onnxruntime.dll が必要
};

const char* to_string(ComputeTarget target);
bool parse_compute_target(const std::string& text, ComputeTarget& target);

// ONNX ファイルの入力の大きさ (例: 640x640 なら 640) を読む。読めない・正方形でないときは -1 と理由。
// path は UTF-8。onnx ライブラリは使わず、ファイルの中身 (protobuf) を直接読む。
int read_onnx_input_size(const std::string& path, std::string& error_message);

struct DetectorConfig {
    std::string model_path = "models/best.onnx";
    // 学習・書き出し時の画像サイズ。load() のときにモデルから自動で読み取る (読めなければこの値)。
    int input_size = 640;
    // 信頼度のしきい値。ボールの色・照明で調整する。まだ実機映像で調整していない初期値。
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;
    int max_detections = 10;
    ComputeTarget target = ComputeTarget::Cpu;
};

// YOLO (Ultralytics で ONNX に書き出した [1, 4+クラス数, 候補数] 形式) のボール検出器。
// スレッドセーフではない。1つのスレッドからだけ呼ぶこと。
class BallDetector : public IBallDetector {
public:
    explicit BallDetector(DetectorConfig config);

    bool load(std::string& error_message);

    // 計算に使う装置を変える。モデルを読み直して試しに1回推論するので、数秒かかることがある。
    // 失敗したら元の装置に戻して false を返す。detect() と同じスレッドから呼ぶこと。
    bool set_target(ComputeTarget target, std::string& message);
    ComputeTarget target() const {
        return config_.target;
    }
    // 使うモデル (ONNX) を変える。読み直して試しに推論するので数秒かかる。
    // 失敗したら今のモデルのまま false を返す。detect() と同じスレッドから呼ぶこと。
    bool set_model(const std::string& model_path, std::string& message);
    // 信頼度のしきい値。detect() と同じスレッドから呼ぶこと。
    void set_conf_threshold(float threshold) {
        config_.conf_threshold = threshold;
    }
    std::vector<Detection> detect(const cv::Mat& bgr_frame) override;

    const DetectorConfig& config() const {
        return config_;
    }

private:
    // target 用にモデルを読み込んで net に入れる。
    bool load_net(ComputeTarget target, cv::dnn::Net& net, std::string& error_message) const;
    // ONNX Runtime 用にモデルを読み込み、試しに1回推論する。
    std::unique_ptr<OrtEngine> load_ort(std::string& error_message) const;

    DetectorConfig config_;
    cv::dnn::Net net_;
    std::unique_ptr<OrtEngine> ort_;  // target が OnnxRuntime のときだけ使う
    bool is_loaded_ = false;
    bool has_warned_output_ = false;
    bool has_warned_score_ = false;
    bool has_warned_forward_ = false;
};

}  // namespace detection
