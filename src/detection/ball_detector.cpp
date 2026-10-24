#include "vb/detection/ball_detector.hpp"

#include <opencv2/core/ocl.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>

namespace detection {

namespace {

// YOLO の学習時に使われる、余白の標準の灰色。
constexpr int kLetterboxPadValue = 114;
constexpr double kInputScale = 1.0 / 255.0;
// 出力の先頭4チャンネルは cx, cy, w, h。その後ろがクラスごとの信頼度。
constexpr int kBoxChannels = 4;

}  // namespace

// ---- ONNX ファイルから入力の大きさを読む (protobuf を最小限だけ読む。onnx ライブラリ不要) ----
// ModelProto.graph(7) → GraphProto.input(11) の最初 → ValueInfoProto.type(2) → TypeProto.tensor_type(1)
// → Tensor.shape(2) → TensorShapeProto.dim(1) の並び → Dimension.dim_value(1)
namespace onnx_shape {

struct Span {
    const unsigned char* p = nullptr;
    const unsigned char* end = nullptr;
};

bool read_varint(Span& s, std::uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64 && s.p < s.end; shift += 7) {
        const unsigned char b = *s.p++;
        value |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

// 次のフィールドを読む。長さ付き (wire 2) なら sub に中身、varint (wire 0) なら value に値。
bool next_field(Span& s, std::uint32_t& field, std::uint32_t& wire, Span& sub, std::uint64_t& value) {
    std::uint64_t key = 0;
    if (s.p >= s.end || !read_varint(s, key)) {
        return false;
    }
    field = static_cast<std::uint32_t>(key >> 3);
    wire = static_cast<std::uint32_t>(key & 7);
    switch (wire) {
        case 0:
            return read_varint(s, value);
        case 1:
            if (s.end - s.p < 8) return false;
            s.p += 8;
            return true;
        case 5:
            if (s.end - s.p < 4) return false;
            s.p += 4;
            return true;
        case 2: {
            std::uint64_t length = 0;
            if (!read_varint(s, length) || length > static_cast<std::uint64_t>(s.end - s.p)) return false;
            sub = {s.p, s.p + length};
            s.p += length;
            return true;
        }
        default:
            return false;  // 古い group 形式などは使われていない
    }
}

// 中から field 番号の長さ付きフィールドを最初の1つだけ探す。
bool find_message(Span s, std::uint32_t wanted, Span& out) {
    std::uint32_t field = 0, wire = 0;
    Span sub;
    std::uint64_t value = 0;
    while (next_field(s, field, wire, sub, value)) {
        if (field == wanted && wire == 2) {
            out = sub;
            return true;
        }
    }
    return false;
}

// 最初の入力の形 (例: [1, 3, 640, 640])。決まっていない次元は -1。
bool input_dims(const std::vector<unsigned char>& bytes, std::vector<long long>& dims) {
    Span model{bytes.data(), bytes.data() + bytes.size()};
    Span graph, input, type, tensor, shape;
    if (!find_message(model, 7, graph) || !find_message(graph, 11, input) || !find_message(input, 2, type) ||
        !find_message(type, 1, tensor) || !find_message(tensor, 2, shape)) {
        return false;
    }
    dims.clear();
    std::uint32_t field = 0, wire = 0;
    Span dim;
    std::uint64_t value = 0;
    while (next_field(shape, field, wire, dim, value)) {
        if (field != 1 || wire != 2) continue;
        long long size = -1;
        std::uint32_t f = 0, w = 0;
        Span unused;
        std::uint64_t v = 0;
        while (next_field(dim, f, w, unused, v)) {
            if (f == 1 && w == 0) size = static_cast<long long>(v);
        }
        dims.push_back(size);
    }
    return !dims.empty();
}

}  // namespace onnx_shape

int read_onnx_input_size(const std::string& path, std::string& error_message) {
    const std::filesystem::path file(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error_message = "ファイルを開けません: " + path;
        return -1;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<long long> dims;
    if (!onnx_shape::input_dims(bytes, dims) || dims.size() != 4) {
        error_message = "入力の形を読み取れません";
        return -1;
    }
    if (dims[2] <= 0 || dims[2] != dims[3]) {
        error_message = "入力が正方形の固定サイズではありません (" + std::to_string(dims[2]) + "x" +
                        std::to_string(dims[3]) + ")";
        return -1;
    }
    return static_cast<int>(dims[2]);
}

BallDetector::BallDetector(DetectorConfig config) : config_(std::move(config)) {}

const char* to_string(ComputeTarget target) {
    switch (target) {
        case ComputeTarget::OpenCl:
            return "opencl";
        case ComputeTarget::OpenClFp16:
            return "opencl_fp16";
        case ComputeTarget::OnnxRuntime:
            return "onnxruntime";
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
    } else if (text == "onnxruntime") {
        target = ComputeTarget::OnnxRuntime;
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

std::unique_ptr<OrtEngine> BallDetector::load_ort(std::string& error_message) const {
    auto engine = std::make_unique<OrtEngine>();
    if (!engine->load(config_.model_path, 0, error_message)) {
        return nullptr;
    }
    // 試しに1回推論して、モデルと ONNX Runtime の組み合わせで動くことを確かめる。
    const int size = config_.input_size;
    const cv::Mat dummy(size, size, CV_8UC3, cv::Scalar(kLetterboxPadValue, kLetterboxPadValue, kLetterboxPadValue));
    const cv::Mat blob =
        cv::dnn::blobFromImage(dummy, kInputScale, cv::Size(size, size), cv::Scalar(), true, false);
    cv::Mat output;
    if (!engine->run(blob, output, error_message)) {
        return nullptr;
    }
    return engine;
}

bool BallDetector::load(std::string& error_message) {
    // 入力の大きさはモデルから読む (320 で書き出したモデルなら 320)。読めなければ設定の値のまま。
    std::string size_error;
    const int size = read_onnx_input_size(config_.model_path, size_error);
    if (size > 0) {
        config_.input_size = size;
    } else {
        std::cerr << "[detector] " << size_error << "。入力 " << config_.input_size << " として扱います\n";
    }
    if (config_.target == ComputeTarget::OnnxRuntime) {
        ort_ = load_ort(error_message);
        if (ort_) {
            is_loaded_ = true;
            return true;
        }
        // ONNX Runtime が使えなければ OpenCV (CPU) で読み直す (起動できないよりはまし)。
        std::cerr << "[detector] " << error_message << "。OpenCV (CPU) で動かします\n";
        config_.target = ComputeTarget::Cpu;
    }
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
    if (target == ComputeTarget::OnnxRuntime) {
        std::unique_ptr<OrtEngine> engine = load_ort(message);
        if (!engine) {
            return false;  // 今の装置のまま動き続ける
        }
        ort_ = std::move(engine);
        config_.target = target;
        has_warned_forward_ = false;
        message = "ONNX Runtime (CPU) で推論します";
        return true;
    }
    cv::dnn::Net net;
    if (!load_net(target, net, message)) {
        return false;  // 今の net_ はそのまま (元の装置で動き続ける)
    }
    net_ = net;
    ort_.reset();
    config_.target = target;
    has_warned_forward_ = false;
    message = target == ComputeTarget::Cpu ? "CPU で推論します" : "GPU (" + std::string(to_string(target)) + ") で推論します";
    if (target != ComputeTarget::Cpu) {
        message += " — " + cv::ocl::Device::getDefault().name();
    }
    return true;
}

bool BallDetector::set_model(const std::string& model_path, std::string& message) {
    DetectorConfig next = config_;
    next.model_path = model_path;
    next.input_size = 640;  // load() でモデルから読み直す
    BallDetector candidate(next);
    if (!candidate.load(message)) {
        return false;  // 今のモデルのまま動き続ける
    }
    const ComputeTarget wanted = config_.target;
    *this = std::move(candidate);
    const std::string name =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(model_path.data()), model_path.size()))
            .filename()
            .string();
    message = "モデルを切り替えました: " + name + " (入力 " + std::to_string(config_.input_size) + ")";
    if (config_.target != wanted) {
        message += std::string(" — ") + to_string(wanted) + " で読めなかったので " + to_string(config_.target) + " で動かします";
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
        if (config_.target == ComputeTarget::OnnxRuntime && ort_) {
            std::string error;
            if (!ort_->run(blob, output, error)) {
                if (!has_warned_forward_) {
                    std::cerr << "[detector] " << error << "\n";
                    has_warned_forward_ = true;
                }
                return results;
            }
        } else {
            net_.setInput(blob);
            output = net_.forward();
        }
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
