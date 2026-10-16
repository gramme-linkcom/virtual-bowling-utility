#include "vb/detection/ball_detector.hpp"

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

bool BallDetector::load(std::string& error_message) {
    try {
        net_ = cv::dnn::readNetFromONNX(config_.model_path);
    } catch (const cv::Exception& e) {
        error_message = "ONNX を読み込めません (" + config_.model_path + "): " + e.what();
        return false;
    }
    if (net_.empty()) {
        error_message = "ONNX を読み込めません (" + config_.model_path + "): 空のネットワークです";
        return false;
    }
    net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    is_loaded_ = true;
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
    net_.setInput(blob);
    const cv::Mat output = net_.forward();

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
