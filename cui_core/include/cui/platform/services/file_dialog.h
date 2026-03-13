#pragma once
/// @file file_dialog.h
/// @brief 原生文件对话框（打开）——跨平台系统级文件选择能力。
///
/// 对齐 rui 的 `platform::services::file_dialog`（那里用 `rfd`，Linux 底层正是
/// zenity/kdialog）。cui_core 是纯 C++，按同样的「调各平台原生选择器」策略自持实现：
///   - **Linux / BSD**：`zenity`，缺失时退 `kdialog`（POSIX fork+execvp，无 shell）。
///   - **macOS**：`osascript` 的 `choose file`（Finder 原生打开面板）。
///   - **Windows**：`GetOpenFileNameW`（comdlg32，系统 IFileOpenDialog 家族）。
///   - **Android / iOS / Web / OHOS**：本进程内无原生选择器，直接返回
///     `std::nullopt`；将来经宿主方法通道（对齐 rui 的 `rui.file.open`）补上。
///
/// 用法（同步，直接在按钮回调里调用）：
///   auto path = services::openFileDialog("选择视频文件",
///             {{services::FileFilter{"Videos", {"mp4", "mkv", "mov"}}}});
///   if (path) { /* 打开 *path */ }
///
/// 阻塞语义：调用会一直阻塞到原生对话框关闭并返回所选路径。原生模态窗口本就在
/// 最上层、由系统负责绘制，所以桌面端在 UI 线程直接调用即可（其间框架不重绘，
/// 属正常模态表现）。若要在后台线程跑再回投结果，由调用方自行编排。

#include <optional>
#include <string>
#include <vector>

namespace cui {
namespace services {

/// 文件过滤器。`extensions` 不含前导点，例如 {"mp4", "mkv", "mov"}。
struct FileFilter {
    std::string name;                     ///< 显示名，如 "Videos"
    std::vector<std::string> extensions;  ///< 扩展名（不含点）
};

/// 打开对话框的参数（全部可留空走默认）。
struct FileDialogOptions {
    std::string title;                     ///< 对话框标题
    std::string initialDirectory;          ///< 起始目录（可选）
    std::vector<FileFilter> filters;       ///< 类型过滤（可选）
    std::string suggestedName;             ///< 保存对话框的建议文件名（可选）
};

/// 选择单个已存在的文件。取消 / 平台不支持时返回 `std::nullopt`。
std::optional<std::string> openFileDialog(const FileDialogOptions& options);

/// 便捷重载：只给标题。
inline std::optional<std::string> openFileDialog(const std::string& title) {
    FileDialogOptions options;
    options.title = title;
    return openFileDialog(options);
}

/// 选择保存路径（文件不必已存在）。取消 / 平台不支持时返回 `std::nullopt`。
std::optional<std::string> saveFileDialog(const FileDialogOptions& options);

}  // namespace services
}  // namespace cui_core
