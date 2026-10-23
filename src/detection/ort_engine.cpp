#include "vb/detection/ort_engine.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(_WIN32) && !defined(_stdcall)
// MinGW の GCC は -std=c++20 (GNU 拡張なし) だと _stdcall を定義しないが、ONNX Runtime のヘッダーが使う。
#define _stdcall __stdcall
#endif
#include <onnxruntime_c_api.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace detection {

namespace {

#ifdef _WIN32
constexpr const wchar_t* kLibraryName = L"onnxruntime.dll";
#else
constexpr const char* kLibraryName = "libonnxruntime.so";
#endif

using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();

// exe と同じフォルダのパス。
std::filesystem::path executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

}  // namespace

struct OrtEngine::Impl {
    void* library = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory_info = nullptr;
    std::string input_name;
    std::string output_name;

    ~Impl() {
        if (api != nullptr) {
            if (memory_info != nullptr) {
                api->ReleaseMemoryInfo(memory_info);
            }
            if (session != nullptr) {
                api->ReleaseSession(session);
            }
            if (options != nullptr) {
                api->ReleaseSessionOptions(options);
            }
            if (env != nullptr) {
                api->ReleaseEnv(env);
            }
        }
        // DLL は解放しない (プロセス終了まで読み込んだままにする方が安全)。
    }

    // OrtStatus を確認する。失敗なら error_message に理由を入れて false。
    bool check(OrtStatus* status, const char* what, std::string& error_message) const {
        if (status == nullptr) {
            return true;
        }
        error_message = std::string("ONNX Runtime: ") + what + " に失敗: " + api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        return false;
    }
};

OrtEngine::OrtEngine() : impl_(std::make_unique<Impl>()) {}

OrtEngine::~OrtEngine() = default;

bool OrtEngine::load(const std::string& model_path, int threads, std::string& error_message) {
    Impl& m = *impl_;
    const std::filesystem::path library_path = executable_dir() / kLibraryName;

#ifdef _WIN32
    // exe の隣の DLL をフルパスで読む (Windows 11 の System32 にある別バージョンを拾わないように)。
    HMODULE module = LoadLibraryExW(library_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        error_message = "onnxruntime.dll が見つかりません (exe と同じフォルダに置いてください)";
        return false;
    }
    m.library = module;
    auto get_api_base = reinterpret_cast<GetApiBaseFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "OrtGetApiBase"))
    );
#else
    m.library = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (m.library == nullptr) {
        error_message = std::string("libonnxruntime.so を読み込めません: ") + dlerror();
        return false;
    }
    auto get_api_base = reinterpret_cast<GetApiBaseFn>(dlsym(m.library, "OrtGetApiBase"));
#endif
    if (get_api_base == nullptr) {
        error_message = "onnxruntime.dll が壊れています (OrtGetApiBase がありません)";
        return false;
    }
    m.api = get_api_base()->GetApi(ORT_API_VERSION);
    if (m.api == nullptr) {
        error_message = "onnxruntime.dll のバージョンが古すぎます (1.20 以降が必要)";
        return false;
    }

    if (!m.check(m.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "vb2026", &m.env), "初期化", error_message) ||
        !m.check(m.api->CreateSessionOptions(&m.options), "設定の作成", error_message) ||
        !m.check(
            m.api->SetSessionGraphOptimizationLevel(m.options, ORT_ENABLE_ALL),
            "最適化の設定",
            error_message
        ) ||
        !m.check(m.api->SetIntraOpNumThreads(m.options, threads), "スレッド数の設定", error_message)) {
        return false;
    }

#ifdef _WIN32
    // ORTCHAR_T は wchar_t。UTF-8 のパスを変換する (日本語のフォルダでも読めるように)。
    const std::filesystem::path model(
        std::u8string(reinterpret_cast<const char8_t*>(model_path.data()), model_path.size())
    );
    const std::wstring model_native = model.wstring();
    const ORTCHAR_T* model_arg = model_native.c_str();
#else
    const ORTCHAR_T* model_arg = model_path.c_str();
#endif
    if (!m.check(
            m.api->CreateSession(m.env, model_arg, m.options, &m.session),
            "モデルの読み込み",
            error_message
        )) {
        return false;
    }

    OrtAllocator* allocator = nullptr;
    if (!m.check(m.api->GetAllocatorWithDefaultOptions(&allocator), "アロケーター", error_message)) {
        return false;
    }
    char* name = nullptr;
    if (!m.check(m.api->SessionGetInputName(m.session, 0, allocator, &name), "入力名", error_message)) {
        return false;
    }
    m.input_name = name;
    std::string ignored;
    m.check(m.api->AllocatorFree(allocator, name), "解放", ignored);
    if (!m.check(m.api->SessionGetOutputName(m.session, 0, allocator, &name), "出力名", error_message)) {
        return false;
    }
    m.output_name = name;
    m.check(m.api->AllocatorFree(allocator, name), "解放", ignored);

    return m.check(
        m.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m.memory_info),
        "メモリ情報",
        error_message
    );
}

bool OrtEngine::run(const cv::Mat& blob, cv::Mat& output, std::string& error_message) {
    Impl& m = *impl_;
    if (m.session == nullptr) {
        error_message = "ONNX Runtime: モデルが読み込まれていません";
        return false;
    }
    if (blob.type() != CV_32F || !blob.isContinuous() || blob.dims < 1) {
        error_message = "ONNX Runtime: 入力の形式が想定外です";
        return false;
    }

    std::vector<int64_t> shape(blob.dims);
    for (int i = 0; i < blob.dims; ++i) {
        shape[i] = blob.size[i];
    }

    OrtValue* input = nullptr;
    if (!m.check(
            m.api->CreateTensorWithDataAsOrtValue(
                m.memory_info,
                const_cast<float*>(blob.ptr<float>()),
                blob.total() * sizeof(float),
                shape.data(),
                shape.size(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                &input
            ),
            "入力の作成",
            error_message
        )) {
        return false;
    }

    const char* input_names[] = {m.input_name.c_str()};
    const char* output_names[] = {m.output_name.c_str()};
    OrtValue* result = nullptr;
    const bool is_run_ok = m.check(
        m.api->Run(m.session, nullptr, input_names, &input, 1, output_names, 1, &result),
        "推論",
        error_message
    );
    m.api->ReleaseValue(input);
    if (!is_run_ok) {
        return false;
    }

    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t dim_count = 0;
    float* data = nullptr;
    bool ok = m.check(m.api->GetTensorTypeAndShape(result, &info), "出力の形", error_message) &&
              m.check(m.api->GetDimensionsCount(info, &dim_count), "出力の次元", error_message);
    std::vector<int64_t> dims(dim_count);
    if (ok) {
        ok = m.check(m.api->GetDimensions(info, dims.data(), dim_count), "出力の大きさ", error_message) &&
             m.check(
                 m.api->GetTensorMutableData(result, reinterpret_cast<void**>(&data)),
                 "出力の取り出し",
                 error_message
             );
    }
    if (info != nullptr) {
        m.api->ReleaseTensorTypeAndShapeInfo(info);
    }
    if (ok) {
        // OpenCV DNN の forward() と同じ形の cv::Mat にする (後ろの処理を共通にするため)。
        std::vector<int> sizes(dims.begin(), dims.end());
        output = cv::Mat(static_cast<int>(sizes.size()), sizes.data(), CV_32F, data).clone();
    }
    m.api->ReleaseValue(result);
    return ok;
}

}  // namespace detection
