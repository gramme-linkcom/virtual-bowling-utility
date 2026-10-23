#pragma once

#include <filesystem>
#include <string>

namespace transport {

// Unity (GameContoller.cs) が読む vector.txt の中身。去年の Python (main.py) と同じ3行。
//   1行目: 投げ始めの横位置 [m]
//   2行目: 横方向の速度
//   3行目: 前方向の速度 (負の値)
// Unity 側はファイルの更新時刻が変わったら読み込んでボールを投げる。
struct UnityMotion {
    double position = 0.0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
};

// UTF-8 の文字列をパスにする (日本語のフォルダ名でも正しく開けるように)。
std::filesystem::path path_from_utf8(const std::string& text);
std::string path_to_utf8(const std::filesystem::path& path);

// 一時ファイルに書いてから置き換えるので、Unity が書きかけの内容を読むことはない。
// 失敗したら理由を error_message に入れて false を返す。
bool write_vector_file(const std::string& path, const UnityMotion& motion, std::string& error_message);

}  // namespace transport
