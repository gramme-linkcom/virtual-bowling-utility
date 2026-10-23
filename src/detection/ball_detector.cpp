#include "vb/detection/ball_detector.hpp"

#include <opencv2/core/ocl.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace detection {

namespace {

// YOLO の学習時に使われる、余白の標準の灰色。
constexpr int kLetterboxPadValue = 114;
constexpr double kInputScale = 1.0 / 255.0;
// 出力の先頭4チャンネルは cx, cy, w, h。その後ろがクラスごとの信頼度。
constexpr int kBoxChannels = 4;

}  // namespace

BallDetector::BallDetector(DetectorConfig config) : config_(std::move(config)) {}

const char* to_string(ComputeTarget target) {
    switch (target) {
        case ComputeTarget::OpenCl:
            return "opencl";
        case ComputeTarget::OpenClFp16:
            return "opencl_fp16";
        case ComputeTarget::Cpu:
        default:
            return "cpu";
    }
}

bool parse_compute_target(const std::string& text, ComputeTarget& target) {
    if (text == "cpu") {
        target = ComputeTarget::Cpu;
    } else if (text == "opencl") {
        target = ComputeTarget::OpenCl;
    } else if (text == "opencl_fp16") {
        target = ComputeTarget::OpenClFp16;
    } else {
        return false;
    }
    return true;
}

bool BallDetector::load_net(ComputeTarget target, cv::dnn::Net& net, std::string& error_message)
    const {
    // OpenCV 5 の新しい DNN エンジンは CPU 専用なので、GPU (OpenCL) のときは従来のエンジンで読む。
    const bool is_gpu = target != ComputeTarget::Cpu;
    if (is_gpu) {
        if (!cv::ocl::haveOpenCL()) {
            error_message = "この PC では OpenCL が使えません (GPU ドライバーを確認してください)";
            return false;
        }
        cv::ocl::setUseOpenCL(true);
    }
    try {
        net = cv::dnn::readNetFromONNX(
            config_.model_path, is_gpu ? cv::dnn::ENGINE_CLASSIC : cv::dnn::ENGINE_AUTO
        );
    } catch (const cv::Exception& e) {
        error_message = "ONNX を読み込めません (" + config_.model_path + "): " + e.what();
        return false;
    }
    if (net.empty()) {
        error_message = "ONNX を読み込めません (" + config_.model_path + "): 空のネットワークです";
        return false;
    }
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    switch (target) {
        case ComputeTarget::OpenCl:
            net.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL);
            break;
        case ComputeTarget::OpenClFp16:
            net.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL_FP16);
            break;
        case ComputeTarget::Cpu:
        default:
            net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            break;
    }
    if (!is_gpu) {
        return true;
    }

    // GPU は最初の推論で準備 (カーネルのコンパイル) が走るので、ここで1回試して失敗を拾う。
    try {
        const int size = config_.input_size;
        const cv::Mat dummy(size, size, CV_8UC3, cv::Scalar(kLetterboxPadValue, kLetterboxPadValue, kLetterboxPadValue));
        net.setInput(cv::dnn::blobFromImage(dummy, kInputScale, cv::Size(size, size), cv::Scalar(), true, false));
        net.forward();
    } catch (const cv::Exception& e) {
        error_message = std::string("GPU (OpenCL) で推論できません: ") + e.what();
        return false;
    }
    return true;
}

bool BallDetector::load(std::string& error_message) {
    cv::dnn::Net net;
    if (!load_net(config_.target, net, error_message)) {
        if (config_.target == ComputeTarget::Cpu) {
            return false;
        }
        // GPU で読めなければ CPU で読み直す (起動できないよりはまし)。
        std::cerr << "[detector] " << error_message << "。CPU で動かします\n";
        config_.target = ComputeTarget::Cpu;
        if (!load_net(ComputeTarget::Cpu, net, error_message)) {
            return false;
        }
    }
    net_ = net;
    is_loaded_ = true;
    return true;
}

bool BallDetector::set_target(ComputeTarget target, std::string& message) {
    if (!is_loaded_) {
        message = "モデルが読み込めていません";
        return false;
    }
    if (target == config_.target) {
        message = std::string("すでに ") + to_string(target) + " で動いています";
        return true;
    }
    cv::dnn::Net net;
    if (!load_net(target, net, message)) {
        return false;  // 今の net_ はそのまま (元の装置で動き続ける)
    }
    net_ = net;
    config_.target = target;
    has_warned_forward_ = false;
    message = target == ComputeTarget::Cpu ? "CPU で推論します" : "GPU (" + std::string(to_string(target)) + ") で推論します";
    if (target != ComputeTarget::Cpu) {
        message += " — " + cv::ocl::Device::getDefault().name();
    }
    return true;
}

std::vector<Detection> BallDetector::detect(const cv::Mat& bgr_frame) {
    std::vector<Detection> results;
    if (!is_loaded_ || bgr_frame.empty()) {
        return results;
    }

    // 縦横比を保って縮小し、余白を足して正方形にする (letterbox)。
    const int size = config_.input_size;
    const float scale =
        std::min(static_cast<float>(size) / bgr_frame.cols, static_cast<float>(size) / bgr_frame.rows);
    const int resized_w = static_cast<int>(std::round(bgr_frame.cols * scale));
    const int resized_h = static_cast<int>(std::round(bgr_frame.rows * scale));
    const int pad_x = (size - resized_w) / 2;
    const int pad_y = (size - resized_h) / 2;

    cv::Mat resized;
    cv::resize(bgr_frame, resized, cv::Size(resized_w, resized_h));
    cv::Mat letterboxed(
        size, size, bgr_frame.type(), cv::Scalar(kLetterboxPadValue, kLetterboxPadValue, kLetterboxPadValue)
    );
    resized.copyTo(letterboxed(cv::Rect(pad_x, pad_y, resized_w, resized_h)));

    // BGR -> RGB、0〜1 に正規化。
    const cv::Mat blob = cv::dnn::blobFromImage(
        letterboxed, kInputScale, cv::Size(size, size), cv::Scalar(), true, false
    );
    cv::Mat output;
    try {
        net_.setInput(blob);
        output = net_.forward();
    } catch (const cv::Exception& e) {
        // 推論の失敗で検出スレッドごと落ちないようにする (GPU のドライバー不調など)。
        if (!has_warned_forward_) {
            std::cerr << "[detector] 推論に失敗しました: " << e.what() << "\n";
            has_warned_forward_ = true;
        }
        return results;
    }

    // 想定する出力は [1, 4+クラス数, 候補数] (例: [1, 5, 8400])。
    // 軸の入れ替わった [1, 候補数, 4+クラス数] も受け付ける。
    if (output.dims != 3 || output.size[0] != 1 || output.type() != CV_32F || !output.isContinuous()) {
        if (!has_warned_output_) {
            std::cerr << "[detector] 想定外の出力形式です (dims=" << output.dims << ")\n";
            has_warned_output_ = true;
        }
        return results;
    }
    const int dim1 = output.size[1];
    const int dim2 = output.size[2];
    const bool is_channels_first = dim1 < dim2;
    const int channels = is_channels_first ? dim1 : dim2;
    const int count = is_channels_first ? dim2 : dim1;
    if (channels <= kBoxChannels) {
        if (!has_warned_output_) {
            std::cerr << "[detector] 出力のチャンネル数が足りません (" << channels << ")\n";
            has_warned_output_ = true;
        }
        return results;
    }

    const float* data = output.ptr<float>();
    const auto value = [&](int channel, int index) {
        return is_channels_first ? data[channel * count + index] : data[index * channels + channel];
    };

    std::vector<cv::Rect2d> boxes;
    std::vector<float> scores;
    std::vector<int> class_ids;

    for (int i = 0; i < count; ++i) {
        int best_class = 0;
        float best_score = value(kBoxChannels, i);
        for (int c = 1; c < channels - kBoxChannels; ++c) {
            const float score = value(kBoxChannels + c, i);
            if (score > best_score) {
                best_score = score;
                best_class = c;
            }
        }
        if (best_score < config_.conf_threshold) {
            continue;
        }
        if (best_score > 1.0f && !has_warned_score_) {
            // 信頼度が 0〜1 に収まっていない。書き出しの設定を確認する必要がある。
            std::cerr << "[detector] 信頼度が 1 を超えています (" << best_score
                      << ")。ONNX の書き出し設定を確認してください\n";
            has_warned_score_ = true;
        }

        // 640x640 (letterbox 後) の座標を、元のフレームの座標に戻す。
        const float cx = value(0, i);
        const float cy = value(1, i);
        const float w = value(2, i);
        const float h = value(3, i);
        const float x1 = std::max(0.0f, (cx - w / 2.0f - pad_x) / scale);
        const float y1 = std::max(0.0f, (cy - h / 2.0f - pad_y) / scale);
        const float x2 = std::min(static_cast<float>(bgr_frame.cols), (cx + w / 2.0f - pad_x) / scale);
        const float y2 = std::min(static_cast<float>(bgr_frame.rows), (cy + h / 2.0f - pad_y) / scale);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        boxes.emplace_back(x1, y1, x2 - x1, y2 - y1);
        scores.push_back(best_score);
        class_ids.push_back(best_class);
    }

    std::vector<int> kept;
    cv::dnn::NMSBoxes(
        boxes, scores, config_.conf_threshold, config_.nms_threshold, kept, 1.0f, config_.max_detections
    );

    results.reserve(kept.size());
    for (const int index : kept) {
        Detection detection;
        detection.box = cv::Rect2f(
            static_cast<float>(boxes[index].x),
            static_cast<float>(boxes[index].y),
            static_cast<float>(boxes[index].width),
            static_cast<float>(boxes[index].height)
        );
        detection.confidence = scores[index];
        detection.class_id = class_ids[index];
        results.push_back(detection);
    }
    return results;
}

}  // namespace detection
