#pragma once
/// @file rendering/render_scroll.h
/// @brief 滚动系列的渲染对象。
///
/// 对应 上游源码：
///   - RenderSingleChildViewport（widgets/single_child_scroll_view.dart 的私有
///     `_RenderSingleChildViewport`：单子节点、单方向、clamp 滚动，不做 sliver
///     分片。上游的 sliver 版视口 `RenderViewport` 在 rendering/sliver.h）
///   - RenderScrollbar（Scrollbar 覆盖层：内容之上叠加右侧滚动指示条）
///
/// 约定：
///   - RenderSingleChildViewport 的子节点按「主轴无界、交叉轴紧锁 viewport
///     尺寸」布局，尺寸即 viewport 约束的 biggest()；滚动偏移只在 paint/hitTest
///     时平移，不触发重新布局（与 上游的「滚动不重排，只重绘」一致）。
///   - 内容超出 viewport 时通过 ExtentCallback 把 maxScrollExtent /
///     viewportExtent 回写给 ScrollController（layout 期间静默回写，不通知）。

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_flex.h"  // Axis
#include "cui/rendering/sliver.h"       // AxisDirection + axisDirectionToAxis 等方向助手
namespace cui {

class RenderScrollbar;  // 定义见下方（ScrollController 的绘制观察者）

// ── ScrollController ───────────────────────────────────────────────────────
/// 滚动位置控制器。上游的 `ScrollController` 子集：
/// offset 存在控制器里（合并所有 position 的简化），jumpTo 带边界 clamp。
/// 两类观察者：
///   - setState 监听器（widget 层，jumpTo 时通知——不能在 layout 期触发）；
///   - 绘制观察者（RenderScrollbar，任何变化只 markNeedsPaint，layout 期安全）。
class ScrollController {
public:
    using Listener = std::function<void()>;
    /// 监听器句柄。removeListener 时用它精确注销，避免按值比较
    /// std::function（不可比较）。注销后该槽位置空，遍历时跳过。
    using ListenerHandle = size_t;
    static constexpr ListenerHandle kInvalidListener = static_cast<ListenerHandle>(-1);

    double offset() const { return offset_; }
    double maxScrollExtent() const { return maxExtent_; }
    double viewportExtent() const { return viewportExtent_; }
    bool hasClients() const { return measured_; }

    /// 跳到指定偏移（clamp 到 [0, maxScrollExtent]）并通知观察者。
    void jumpTo(double value) {
        double v = std::clamp(value, 0.0, maxExtent_);
        if (v == offset_)
            return;
        offset_ = v;
        notifyPaintObservers();
        for (auto& l : listeners_)
            if (l) l();
    }

    /// 不经边界 clamp 直接设置偏移（BouncingScrollPhysics 越界用）。
    /// 通知节奏与 jumpTo 一致。
    void forceOffset(double value) {
        if (value == offset_)
            return;
        offset_ = value;
        notifyPaintObservers();
        for (auto& l : listeners_)
            if (l) l();
    }

    /// 注册 setState 监听器；返回句柄，供 removeListener 精确注销。
    /// 必须在宿主 State 卸载时注销，否则回调会悬空指向已释放的 State。
    ListenerHandle addListener(Listener l) {
        listeners_.push_back(std::move(l));
        return listeners_.size() - 1;
    }

    /// 注销监听器（置空槽位，不搬移向量，句柄始终保持有效）。
    void removeListener(ListenerHandle h) {
        if (h < listeners_.size())
            listeners_[h] = Listener{};
    }

    /// 绘制观察者（RenderScrollbar）：任何 offset/extents 变化只触发
    /// markNeedsPaint，不做 setState——因此 layout 期间回调也安全
    /// （对齐上游：ScrollPosition 的监听者只做 markNeeds*）。
    void addPaintObserver(RenderScrollbar* ro) { paintObservers_.push_back(ro); }
    void removePaintObserver(RenderScrollbar* ro) {
        paintObservers_.erase(std::remove(paintObservers_.begin(), paintObservers_.end(),
                                          ro),
                              paintObservers_.end());
    }

    /// SingleChildViewport 布局完成后回写测量结果。layout 期间调用：不通知 setState
    /// 监听器，只刷新绘制观察者；若当前 offset 超出新上界则同步 clamp。
    void applyExtents(double maxExtent, double viewportExtent) {
        bool changed = maxExtent != maxExtent_ || viewportExtent != viewportExtent_;
        measured_ = true;
        maxExtent_ = maxExtent;
        viewportExtent_ = viewportExtent;
        if (offset_ > maxExtent_) {
            offset_ = maxExtent_;
            changed = true;  // 位置被夹回来也要刷滚动条 thumb
        }
        if (changed)
            notifyPaintObservers();
    }

private:
    void notifyPaintObservers();  // 定义在文件尾（需 RenderScrollbar 完整类型）

    double offset_ = 0.0;
    double maxExtent_ = 0.0;
    double viewportExtent_ = 0.0;
    bool measured_ = false;
    std::vector<Listener> listeners_;
    std::vector<RenderScrollbar*> paintObservers_;
};

// ── RenderSingleChildViewport ───────────────────────────────────────────────
/// 视口：裁剪并平移子节点内容。上游的 `_RenderSingleChildViewport`
/// （widgets/single_child_scroll_view.dart）—— 上游 SingleChildScrollView
/// 不走 sliver，就是这个盒子版视口。
/// 继承 RenderProxyBox 以获得单子节点槽位（insertChild / child()）。
///
/// 与上游的差别：上游 `offset` 是个 `ViewportOffset` 且自己 addListener
/// （滚动只 markNeedsPaint）；本仓由 Scrollable 的 State 监听 controller 后
/// setState，把 `setOffset(double)` 推下来 ⇒ 滚动会重建子树，绘制偏移语义相同。
class RenderSingleChildViewport : public RenderProxyBox {
public:
    /// 内容尺寸测量完成后回调 (maxScrollExtent, viewportExtent)。
    using ExtentCallback = std::function<void(double /*maxExtent*/, double /*viewportExtent*/)>;

    explicit RenderSingleChildViewport(AxisDirection axisDirection = AxisDirection::Down)
        : axisDirection_(axisDirection) {}

    /// 上游的 `axisDirection`（`axis` 由它推导，和 上游 一样）。
    AxisDirection axisDirection() const { return axisDirection_; }
    void setAxisDirection(AxisDirection d) {
        if (d != axisDirection_) {
            axisDirection_ = d;
            markNeedsLayout();
        }
    }

    Axis axis() const { return axisDirectionToAxis(axisDirection_); }
    /// 上游的 `reverse`：Up / Left 即反向。
    bool reverse() const { return axisDirectionIsReversed(axisDirection_); }

    double offset() const { return offset_; }
    void setOffset(double v) {
        if (v != offset_) {
            offset_ = v;
            markNeedsPaint();
        }
    }

    void setOnMaxExtentChanged(ExtentCallback cb) { onMaxExtentChanged_ = std::move(cb); }

    bool hitTestSelf(const Offset&) const override { return false; }

    /// 命中测试把 viewport 本地坐标换算到内容坐标（回补滚动偏移），
    /// 使滚动后的子控件仍可点。
    bool hitTestChildren(const Offset& position, std::vector<RenderObject*>& result) override;

    /// 滚动偏移只在 paint/hitTest 施加（`parentData->offset` 恒为 0），所以
    /// `getTransformTo`/`localToGlobal` 默认拿不到它。这里把 `contentOffset()`
    /// 补进绘制变换，令坐标换算与 `performPaint` 一致 —— 浮层（Overlay）锚点
    /// （ExpandableFab / ContextMenu 等）据此落在滚动后的真实屏幕位置。
    void applyPaintTransform(const RenderObject* child, Matrix4& transform) const override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    /// 上游的 `_paintOffsetForPosition`：四个朝向各自的绘制平移公式。
    /// Down/Right 是「偏移取负」；Up/Left 还要加回「内容长 - 视口长」。
    Offset contentOffset() const;

    AxisDirection axisDirection_;
    double offset_ = 0;
    ExtentCallback onMaxExtentChanged_;
};

// ── RenderScrollbar ────────────────────────────────────────────────────────
/// 代理盒：先画子节点内容，再在右缘叠加滚动指示条（轨道 + thumb）。
/// 直接持有 ScrollController（弱协作：controller 变化 → markNeedsPaint，
/// paint 时现读 controller 的 offset/extents，不缓存快照）。
///
/// 几何逐条对齐 上游 widgets/scrollbar.dart 的 `ScrollbarPainter`（右缘
/// `ScrollbarOrientation.right` 那一支，本仓 Scrollbar 不接 padding，等价于
/// padding 恒 0）：
///   - 轨道宽 = thickness + 2·crossAxisMargin，绘制区间从 x = W - 轨道宽 起，
///     所以 crossAxisMargin=0 时整条贴住控件右边缘；轨道全长 = 视口高
///     （上游 `_trackExtent = viewportDimension - padding.mainAxis`，不含 margin）。
///   - thumb 在轨道宽度方向居中（x = W - thickness - crossAxisMargin），长度按
///     「视口/内容」比例，行走区间 = 视口高 - 2·mainAxisMargin。
///   - 两者都画成胶囊（radius = thickness/2，对齐 上游 material/scrollbar.dart
///     的 `_kScrollbarRadius = 8.0` 配 8px 厚度）。
class RenderScrollbar : public RenderProxyBox {
public:
    /// thumb 长度的下限（上游 `_kMinThumbExtent` 为 18.0、Material 侧
    /// `_kScrollbarMinLength` 为 48.0；本仓沿用原实现的 24.0）。
    static constexpr double kMinThumbExtent = 24.0;

    RenderScrollbar() = default;
    ~RenderScrollbar() override;

    /// 绑定 controller（内部注册 paint observer；重复绑定先解绑旧的）。
    void setController(class ScrollController* c);

    ScrollController* controller() const { return controller_; }

    void setThickness(double v) {
        if (v == thickness_)
            return;
        thickness_ = v;
        markNeedsPaint();
    }
    void setMainAxisMargin(double v) {
        if (v == mainAxisMargin_)
            return;
        mainAxisMargin_ = v;
        markNeedsPaint();
    }
    void setCrossAxisMargin(double v) {
        if (v == crossAxisMargin_)
            return;
        crossAxisMargin_ = v;
        markNeedsPaint();
    }
    /// 传 std::nullopt = 跟随主题（thumb 用 `theme().scrollbarThumb`，
    /// 轨道用 `theme().outlineVariant`），绘制时现读，换主题立刻生效。
    void setThumbColor(const std::optional<Color>& v) {
        if (v == thumbColor_)
            return;
        thumbColor_ = v;
        markNeedsPaint();
    }
    void setTrackColor(const std::optional<Color>& v) {
        if (v == trackColor_)
            return;
        trackColor_ = v;
        markNeedsPaint();
    }

    double thickness() const { return thickness_; }
    double mainAxisMargin() const { return mainAxisMargin_; }
    double crossAxisMargin() const { return crossAxisMargin_; }

    /// 供测试/headless 读取当前 thumb/轨道几何（本地坐标）。按 controller
    /// 现值推导，不依赖 paint 期间写入的缓存。
    Rect thumbRect() const;
    Rect trackRect() const;

    /// 轨道区（右缘条带）在可滚动时接管按下 —— 上游 `ScrollbarPainter.hitTest`
    /// 同样把整条 `_trackRect` 当作命中区（滚动条是覆盖层，不做这一步的话
    /// 按在 thumb 上的拖动会被下面的内容当成内容拖拽，方向正好相反）。
    bool hitTestSelf(const Offset& position) const override;
    /// 轨道区内的点不再落到内容子树：一是避免内容拖拽与 thumb 拖拽抢同一段
    /// 手势，二是让最外层 Scrollbar 独占该条带（嵌套滚动视图的多个滚动条
    /// 不会各自处理同一次按下）。
    bool hitTestChildren(const Offset& position,
                         std::vector<RenderObject*>& result) override;
    /// 按下 thumb ⇒ 拖动 controller（对齐上游 `handleThumbPress` 的
    /// `_getPrimaryDelta` 换算：thumb 在轨道里移动多少像素，滚动偏移就按
    /// 「可滚距离 / 可行走距离」等比放大）；按在轨道空白处 ⇒ 向点击方向
    /// 翻一页（上游 `handleTrackTapDown`，本仓无补间，直接 jumpTo）。
    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    Color resolveThumbColor() const;
    Color resolveTrackColor() const;

    /// 上游 `ScrollbarPainter.getTrackToScroll`：thumb 顶端在轨道里的位置
    /// → 滚动偏移（本地坐标，未 clamp；由 `jumpTo` 负责夹边界）。
    double trackToScroll(double thumbTop) const;

    ScrollController* controller_ = nullptr;
    double thickness_ = 8.0;        // 上游 material/scrollbar.dart _kScrollbarThickness
    double mainAxisMargin_ = 0.0;   // 上游 ScrollbarPainter.mainAxisMargin
    double crossAxisMargin_ = 0.0;  // 上游 ScrollbarPainter.crossAxisMargin
    std::optional<Color> thumbColor_;
    std::optional<Color> trackColor_;

    /// thumb 拖拽会话（按下时记录，Up/Cancel 清空）。thumb 顶端起点用按下
    /// 时的几何快照：拖动过程中 offset 变化会挪动 thumb，不能用现算值。
    bool dragging_ = false;
    int64_t dragPointer_ = -1;
    double dragStartThumbTop_ = 0.0;
    double dragStartY_ = 0.0;
};

inline void ScrollController::notifyPaintObservers() {
    for (auto* ro : paintObservers_)
        ro->markNeedsPaint();
}

}  // namespace cui_core
