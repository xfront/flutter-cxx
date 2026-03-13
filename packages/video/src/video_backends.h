#pragma once
/// @file video/video_backends.h
/// @brief 包内私有：后端表（系统端 + FFmpeg 端）与注册表。
///
/// 为什么要一张表而不是 `#if` 链条：现在同一个库里并存两条路线（各端系统
/// 媒体栈 / FFmpeg），两条都可能在同一台机器上可用，也都可能其中一条根本没
/// 编进来。挑选规则（默认系统端优先）、运行期探测结果、以及「实际会试哪几个」
/// 必须只有一个地方说了算 —— 否则能力申报和真正打开会走歪。
///
/// 顺序：注册表（应用扩展 / 单测假件）→ 系统端 → FFmpeg，`preference` 可调。
/// 每一项都要过 `probe`：编进来了不等于这台机器用得起来（缺 SDK、API level
/// 太低、浏览器没有 HTMLVideoElement……）。探测结果进程内缓存一次。

#include <string>
#include <vector>

#include "cui/video/video_decoder.h"

namespace cui {

/// 一个编进来的后端。`reg` 是对外契约（名字 / 出声 / 工厂），`kind` 决定
/// 它在 preference 里的分组，`probe` 是运行期可用性。
struct BuiltinBackend {
    VideoBackendRegistration reg;
    VideoBackendKind kind = VideoBackendKind::System;
    /// 运行期探测；返回 false ⇒ 这一项跳过，`note` 给原因。可为空（= 恒可用）。
    bool (*probe)(std::string& note) = nullptr;
    /// 后端自己出声（浏览器 `<video>`、系统播放器那一路），不经过本仓的
    /// `AudioSink`。能力申报据此认「这一路有声」。
    bool selfAudio = false;
};

/// 编译期决定的一张内置表（按「系统端在前、FFmpeg 在后」的原始顺序）。
const std::vector<BuiltinBackend>& builtinVideoBackends();

/// 按当前 `videoBackendPreference()` 排好的完整尝试序列（含注册表项）。
std::vector<BuiltinBackend> orderedVideoBackends();

/// 探测（带进程内缓存）：不可用时把原因写进 `note`。
bool backendAvailable(const BuiltinBackend& backend, std::string& note);

}  // namespace cui
