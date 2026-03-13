#pragma once
/// @file enums.h
/// @brief 与控件体系无关的公共枚举。
///
/// 这些枚举原先寄居在 cui_core/foundation/widget.h（ 控件基类所在的头）。
///  体系被移除后，上游 体系（framework/focus.h 的 KeyEventResult、
/// rendering/render_editable.h 与 widgets/text_input.h 的 TextAlign）
/// 仍需要它们，故独立成头，避免把已删除的 legacy 头重新拉回来。

namespace cui {

/// Three-valued keyboard-event result (mirrors the upstream's KeyEventResult).
enum class KeyEventResult {
    Handled,              ///< Stop bubbling — this widget consumed the key.
    Ignored,              ///< Continue bubbling to the parent (default).
    SkipRemainingHandlers ///< Stop bubbling but do NOT consume (let platform/IME handle).
};

/// Horizontal text alignment.
enum class TextAlign { Left, Center, Right };

}  // namespace cui_core
