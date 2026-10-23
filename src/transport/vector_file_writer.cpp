#include "vb/transport/vector_file_writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace transport {

namespace {

// Unity がちょうどファイルを開いている間は置き換えに失敗するので、少し待って再試行する。
constexpr int kReplaceRetries = 20;
constexpr auto kReplaceRetryWait = std::chrono::milliseconds(10);

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return !error;
#endif
}

}  // namespace

std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())
    );
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool write_vector_file(const std::string& path, const UnityMotion& motion, std::string& error_message) {
    const std::filesystem::path target = path_from_utf8(path);
    std::filesystem::path temp = target;
    temp += ".tmp";

    std::ostringstream text;
    text.imbue(std::locale::classic());  // 小数点は必ず "." (Unity の float.Parse 向け)
    text.precision(9);
    text << motion.position << "\n" << motion.velocity_x << "\n" << motion.velocity_y;

    {
        // binary: Windows でも改行を "\n" のままにする (Unity は '\n' で分割している)。
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error_message = "書き込み用に開けません: " + path + ".tmp (フォルダがあるか確認)";
            return false;
        }
        file << text.str();
        if (!file) {
            error_message = "書き込みに失敗しました: " + path + ".tmp";
            return false;
        }
    }

    for (int i = 0; i < kReplaceRetries; ++i) {
        if (replace_file(temp, target)) {
            return true;
        }
        std::this_thread::sleep_for(kReplaceRetryWait);
    }
    error_message = "置き換えに失敗しました: " + path + " (Unity などが開いたままの可能性)";
    return false;
}

}  // namespace transport
