#pragma once

#include <opencv2/core.hpp>

#include <memory>
#include <string>

namespace detection {

// ONNX Runtime (CPU) で ONNX モデルを推論する。
// onnxruntime.dll は exe と同じフォルダから実行時に読み込む (無い・古いときは load() が失敗するだけ)。
// スレッドセーフではない。1つのスレッドからだけ呼ぶこと。
class OrtEngine {
public:
    OrtEngine();
    ~OrtEngine();

    OrtEngine(const OrtEngine&) = delete;
    OrtEngine& operator=(const OrtEngine&) = delete;

    // model_path は UTF-8。threads が 0 なら ONNX Runtime 任せ (物理コア数)。
    bool load(const std::string& model_path, int threads, std::string& error_message);

    // blob は NCHW の float32 (cv::dnn::blobFromImage の結果)。output に推論結果を入れる。
    bool run(const cv::Mat& blob, cv::Mat& output, std::string& error_message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace detection
