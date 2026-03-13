/// @file file_dialog.cpp
/// @brief 原生文件对话框实现（见 file_dialog.h）。桌面各平台调系统原生选择器，
///        其余平台返回 std::nullopt。

#include "cui/platform/services/file_dialog.h"

#include <cerrno>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

namespace cui {
namespace services {
namespace {

/// 去掉子进程输出末尾的换行。
std::string trimTrailingNewlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

#if !defined(_WIN32)
/// 运行 argv（不经 shell）并捕获其 stdout。
/// 返回退出码；-1 表示 fork/pipe/wait 失败。exec 失败时子进程以 127 退出，
/// 上层据此判断「工具不存在」从而换后备。stderr 保持继承父进程（诊断可见）。
int runCapture(const std::vector<std::string>& argv, std::string& out) {
    int fds[2];
    if (::pipe(fds) != 0)
        return -1;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv)
            cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    char buf[512];
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/// zenity 的 --file-filter 语法："Videos | *.mp4 *.mkv"。
#if !defined(_WIN32) && !defined(__APPLE__)
std::string zenityFilter(const FileFilter& f) {
    std::string s = f.name.empty() ? std::string("All files") : f.name;
    s += " |";
    for (const auto& ext : f.extensions)
        s += " *." + ext;
    return s;
}
#endif  // !_WIN32 && !__APPLE__

/// kdialog 的过滤串："Videos (*.mp4 *.mkv)"，多个用 ";;" 连接。
#if !defined(_WIN32) && !defined(__APPLE__)
std::string kdialogFilters(const std::vector<FileFilter>& filters) {
    std::string s;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i)
            s += ";;";
        const auto& f = filters[i];
        s += (f.name.empty() ? std::string("Files") : f.name) + " (";
        for (size_t j = 0; j < f.extensions.size(); ++j)
            s += (j ? " " : "") + ("*." + f.extensions[j]);
        s += ")";
    }
    return s;
}
#endif  // !_WIN32 && !__APPLE__

/// AppleScript 字符串字面量里的双引号与反斜杠转义。
#if defined(__APPLE__)
std::string escapeAppleScript(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 4);
    for (const char c : s) {
        if (c == '\\' || c == '"')
            r += '\\';
        r += c;
    }
    return r;
}
#endif  // __APPLE__
#endif  // !_WIN32

}  // namespace

std::optional<std::string> openFileDialog(const FileDialogOptions& options) {
    const std::string title = options.title.empty() ? std::string("Open File") : options.title;

#if defined(_WIN32)
    // UTF-8 <-> UTF-16 转换 + GetOpenFileNameW（系统原生打开对话框）。
    auto toWide = [](const std::string& s) {
        if (s.empty())
            return std::wstring();
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    };
    std::vector<wchar_t> fileBuf(4096, L'\0');
    // 过滤串格式："Name\0*.mp4;*.mkv\0Name2\0pat2\0\0"（空模式=全部）。
    std::wstring filterW;
    if (!options.filters.empty()) {
        for (const auto& f : options.filters) {
            std::string pat;
            for (size_t i = 0; i < f.extensions.size(); ++i) {
                if (i)
                    pat += ";";
                pat += "*." + f.extensions[i];
            }
            filterW += toWide(f.name.empty() ? "Files" : f.name);
            filterW += L'\0';
            filterW += toWide(pat);
            filterW += L'\0';
        }
    }
    filterW += L'\0';  // 结尾双空

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filterW.empty() ? nullptr : filterW.data();
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = DWORD(fileBuf.size());
    std::wstring titleWide = toWide(title);
    std::wstring initDir = toWide(options.initialDirectory);
    ofn.lpstrTitle = titleWide.c_str();
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.Flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetOpenFileNameW(&ofn))
        return std::nullopt;  // 取消或出错
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, -1, nullptr, 0, nullptr,
                                          nullptr);
    if (len <= 0)
        return std::nullopt;
    std::string utf8(size_t(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, -1, utf8.data(), len, nullptr, nullptr);
    return utf8;

#elif defined(__APPLE__)
    // macOS：osascript 的 choose file 唤起 Finder 原生打开面板。
    std::string script = "POSIX path of (choose file with prompt \"" + escapeAppleScript(title) +
                         "\"";
    if (!options.initialDirectory.empty())
        script += " in POSIX file \"" + escapeAppleScript(options.initialDirectory) + "\"";
    script += ")";
    std::string out;
    const int code = runCapture({"osascript", "-e", script}, out);
    out = trimTrailingNewlines(out);
    if (code == 0 && !out.empty())
        return out;
    return std::nullopt;  // 用户取消（osascript 非零退出）或不支持

#else
    // Linux / BSD：先试 zenity，起不来（exec 失败=127）再退 kdialog。
    std::vector<std::string> zargs = {"zenity", "--file-selection", "--title=" + title};
    if (!options.initialDirectory.empty())
        zargs.push_back("--filename=" + options.initialDirectory + "/");
    for (const auto& f : options.filters)
        zargs.push_back("--file-filter=" + zenityFilter(f));

    std::string out;
    int code = runCapture(zargs, out);
    if (code != 127) {  // zenity 存在：0=选中，其它=取消，都不再试后备
        out = trimTrailingNewlines(out);
        if (code == 0 && !out.empty())
            return out;
        return std::nullopt;
    }

    std::vector<std::string> kargs = {"kdialog", "--title", title, "--getopenfilename",
                                      options.initialDirectory};
    const std::string kf = kdialogFilters(options.filters);
    if (!kf.empty())
        kargs.push_back(kf);
    out.clear();
    code = runCapture(kargs, out);
    out = trimTrailingNewlines(out);
    if (code == 0 && !out.empty())
        return out;
    return std::nullopt;
#endif
}

std::optional<std::string> saveFileDialog(const FileDialogOptions& options) {
    const std::string title = options.title.empty() ? std::string("Save File") : options.title;

#if defined(_WIN32)
    auto toWide = [](const std::string& s) {
        if (s.empty())
            return std::wstring();
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    };
    std::vector<wchar_t> fileBuf(4096, L'\0');
    // 预填建议文件名（GetSaveFileNameW 会在 lpstrFile 里看到它）。
    const std::wstring nameW = toWide(options.suggestedName);
    if (!nameW.empty() && nameW.size() < fileBuf.size())
        ::lstrcpynW(fileBuf.data(), nameW.c_str(), int(fileBuf.size()));

    std::wstring filterW;
    for (const auto& f : options.filters) {
        std::string pat;
        for (size_t i = 0; i < f.extensions.size(); ++i) {
            if (i)
                pat += ";";
            pat += "*." + f.extensions[i];
        }
        filterW += toWide(f.name.empty() ? "Files" : f.name);
        filterW += L'\0';
        filterW += toWide(pat);
        filterW += L'\0';
    }
    filterW += L'\0';

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filterW.empty() ? nullptr : filterW.data();
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = DWORD(fileBuf.size());
    const std::wstring titleWide = toWide(title);
    const std::wstring initDir = toWide(options.initialDirectory);
    ofn.lpstrTitle = titleWide.c_str();
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetSaveFileNameW(&ofn))
        return std::nullopt;
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, -1, nullptr, 0, nullptr,
                                          nullptr);
    if (len <= 0)
        return std::nullopt;
    std::string utf8(size_t(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, -1, utf8.data(), len, nullptr, nullptr);
    return utf8;

#elif defined(__APPLE__)
    std::string script =
            "POSIX path of (choose file name with prompt \"" + escapeAppleScript(title) + "\"";
    if (!options.suggestedName.empty())
        script += " default name \"" + escapeAppleScript(options.suggestedName) + "\"";
    script += ")";
    std::string out;
    const int code = runCapture({"osascript", "-e", script}, out);
    out = trimTrailingNewlines(out);
    if (code == 0 && !out.empty())
        return out;
    return std::nullopt;

#else
    // 拼一个「目录/建议名」给 zenity --filename / kdialog 作为起始路径。
    std::string startPath = options.initialDirectory;
    if (!startPath.empty() && startPath.back() != '/')
        startPath += '/';
    startPath += options.suggestedName;

    std::vector<std::string> zargs = {"zenity", "--file-selection", "--save",
                                      "--confirm-overwrite", "--title=" + title};
    if (!startPath.empty())
        zargs.push_back("--filename=" + startPath);
    for (const auto& f : options.filters)
        zargs.push_back("--file-filter=" + zenityFilter(f));

    std::string out;
    int code = runCapture(zargs, out);
    if (code != 127) {
        out = trimTrailingNewlines(out);
        if (code == 0 && !out.empty())
            return out;
        return std::nullopt;
    }

    std::vector<std::string> kargs = {"kdialog", "--title", title, "--getsavefilename",
                                      startPath};
    const std::string kf = kdialogFilters(options.filters);
    if (!kf.empty())
        kargs.push_back(kf);
    out.clear();
    code = runCapture(kargs, out);
    out = trimTrailingNewlines(out);
    if (code == 0 && !out.empty())
        return out;
    return std::nullopt;
#endif
}

}  // namespace services
}  // namespace cui_core
