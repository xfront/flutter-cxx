/// @file audio_sink.cpp
/// @brief `createPlatformAudioSink()` 的兜底实现：没有音频设备的平台返回空。
///
/// 各端真正的实现（`audio_sink_android.cpp` 的 AAudio、`audio_sink_darwin.cpp` 的
/// AudioQueue、`audio_sink_alsa.cpp` 的 ALSA、`audio_sink_ohos.cpp` 的
/// OH_AudioRenderer）由 `packages/video/CMakeLists.txt` 按平台选一个编进来，
/// 并给 `CUI_HAVE_AUDIO_SINK=1`。这个文件永远在源列表里，靠同一个宏把自己关掉
/// —— 这样桌面（Windows / Linux / macOS）不必在 CMake 里做排除，也保证
/// `video_decoder.cpp` 里那句无条件调用永远能链接过。
///
/// 返回 nullptr 的语义不是错误：`VideoPlayback` 会退回「无声 + 墙钟节拍」，
/// 与 Windows Media Foundation 后端一直以来的行为一致。

#include "cui/video/audio_sink.h"

#if !defined(CUI_HAVE_AUDIO_SINK)

namespace cui {

std::unique_ptr<AudioSink> createPlatformAudioSink() { return nullptr; }

}  // namespace cui_core

#endif  // !CUI_HAVE_AUDIO_SINK
