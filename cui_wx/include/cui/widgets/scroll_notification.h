#pragma once
/// @file widgets/scroll_notification.h
/// @brief 滚动通知家族。上游的 widgets/scroll_notification.dart +
///        widgets/scroll_metrics.dart。
///
/// 上游 Scrollable 的通知生命周期（本文件与 scroll.h 一起照此实现）：
///   ScrollStartNotification
///     → 若干 ScrollUpdateNotification（混着若干 OverscrollNotification，
///       以及若干 UserScrollNotification 表示方向变化）
///   → ScrollEndNotification
///   → UserScrollNotification(direction: idle)
///
/// 与 上游 的差别：
///   - `ScrollMetrics` 未带 devicePixelRatio / textDirection（本仓无这两个上下文）；
///   - 通知里的 `context` 是裸 `BuildContext*`（上游是 BuildContext?），只在
///     dispatch 期间有效，监听者别存下来；
///   - `dragDetails` / `velocity` 这类手势细节本仓手势层还没有对应物，省略；
///   - `ScrollMetricsNotification` **只有类型、没有派发点**：上游由
///     `ScrollPosition.didUpdateScrollMetrics` 在 `applyNewDimensions` 检测到
///     metrics 变化时用 `scheduleMicrotask` **推后到帧末**发（布局期发已经太晚，
///     见 scroll_position.dart:670-680）。本仓视口在布局期用
///     `ScrollController::applyExtents` 回写尺寸、且刻意不通知 setState 监听者，
///     也没有 microtask 调度 ⇒ 这条通知留待 ScrollPosition 批次接线。
///
/// 方向通知折算（`notifyDirection`）与上游 `_deltaToScrollDirection` 一致：按
/// **请求量**（不是落位量）定 forward/reverse，`axisDirectionIsReversed` 取反；
/// 收尾的 idle 由 `endScroll` 发。

#include <cmath>
#include <optional>
#include <string>

#include "cui/foundation/geometry.h"
#include "cui/rendering/render_flex.h"  // Axis
#include "cui/rendering/sliver.h"       // ScrollDirection
#include "cui/widgets/notification.h"

namespace cui {

/// 滚动位置的一份快照。上游的 `ScrollMetrics`。
struct ScrollMetrics {
    double pixels = 0.0;
    double minScrollExtent = 0.0;
    double maxScrollExtent = 0.0;
    double viewportExtent = 0.0;
    Axis axis = Axis::Vertical;

    /// 上游的 `isScrollable`：内容比视口长才谈得上滚动。
    bool isScrollable() const { return maxScrollExtent > minScrollExtent; }
    /// 上游的 `outOfRange`。
    bool outOfRange() const { return pixels < minScrollExtent || pixels > maxScrollExtent; }
    /// 上游的 `extentInside`：视口内已展示的内容长度。
    double extentInside() const {
        return std::fmin(maxScrollExtent + 1.0 - pixels, viewportExtent);
    }
    /// 上游的 `extentBefore` / `extentAfter`：视口前后仍未展示的内容长度。
    double extentBefore() const {
        return std::fmin(pixels - minScrollExtent, viewportExtent);
    }
    double extentAfter() const {
        return std::fmin(maxScrollExtent - pixels, viewportExtent);
    }

    std::string toString() const {
        return "pixels=" + std::to_string(pixels) +
               " range=[" + std::to_string(minScrollExtent) + "," +
               std::to_string(maxScrollExtent) + "] viewport=" +
               std::to_string(viewportExtent);
    }
};

/// 滚动相关通知的基类。上游的 `ScrollNotification`
/// （extends LayoutChangedNotification with ViewportNotificationMixin）。
class ScrollNotification : public LayoutChangedNotification, public ViewportNotification {
public:
    ScrollNotification(ScrollMetrics m, BuildContext* ctx)
        : metrics(std::move(m)), context(ctx) {}

    ScrollMetrics metrics;
    /// 触发本通知的 Scrollable 的上下文（上游同名 `context` 字段）。
    BuildContext* context = nullptr;

    std::string toString() const override { return "ScrollNotification(" + metrics.toString() + ")"; }
};

/// 开始滚动。上游的 `ScrollStartNotification`。
class ScrollStartNotification : public ScrollNotification {
public:
    using ScrollNotification::ScrollNotification;
    std::string toString() const override { return "ScrollStartNotification"; }
};

/// 滚动位置发生变化。上游的 `ScrollUpdateNotification`。
class ScrollUpdateNotification : public ScrollNotification {
public:
    ScrollUpdateNotification(ScrollMetrics m, BuildContext* ctx,
                             std::optional<double> delta = std::nullopt)
        : ScrollNotification(std::move(m), ctx), scrollDelta(delta) {}

    /// 本次变化推进的像素数（上游同名 `scrollDelta`）。
    std::optional<double> scrollDelta;

    std::string toString() const override { return "ScrollUpdateNotification"; }
};

/// 想滚但被边界挡住。上游的 `OverscrollNotification`。
class OverscrollNotification : public ScrollNotification {
public:
    OverscrollNotification(ScrollMetrics m, BuildContext* ctx, double overscrollAmount)
        : ScrollNotification(std::move(m), ctx), overscroll(overscrollAmount) {}

    /// 因越界而**没有**滚出去的那部分像素。
    double overscroll = 0.0;

    std::string toString() const override { return "OverscrollNotification"; }
};

/// 滚动结束。上游的 `ScrollEndNotification`。
class ScrollEndNotification : public ScrollNotification {
public:
    using ScrollNotification::ScrollNotification;
    std::string toString() const override { return "ScrollEndNotification"; }
};

/// 用户滚动方向变化。上游的 `UserScrollNotification`。
class UserScrollNotification : public ScrollNotification {
public:
    UserScrollNotification(ScrollMetrics m, BuildContext* ctx, ScrollDirection dir)
        : ScrollNotification(std::move(m), ctx), direction(dir) {}

    ScrollDirection direction = ScrollDirection::Idle;

    std::string toString() const override { return "UserScrollNotification"; }
};

/// 可滚动范围（不是滚动位置）发生变化。上游的 `ScrollMetricsNotification`。
/// 本仓只有类型，派发点见文件头偏离说明（待 ScrollPosition 批次）。
class ScrollMetricsNotification : public Notification {
public:
    explicit ScrollMetricsNotification(ScrollMetrics m) : metrics(std::move(m)) {}

    ScrollMetrics metrics;

    std::string toString() const override { return "ScrollMetricsNotification"; }
};

}  // namespace cui_core
