#pragma once
/// @file widgets/virtual_list.h
/// @brief 真正的虚拟列表：零逐行节点，只按滚动偏移绘制可见行。
///
/// 与 `widgets/scroll.h` 的 ListView 区别：
///   - sliver 版会为窗口内的每一行创建 Widget/Element/RenderObject，滚出窗口
///     即 unmount、滚回来重新 build；
///   - 本控件把「行」降级为数据模型 + Painter 回调，整棵树只有一个叶子
///     RenderObject；10^6 行也只占用 O(可见行数) 的绘制调用。
///
/// 变高行用惰性前缀和（offsets[i] = 前 i 项高度和）+ 二分定位，首次滚到深处
/// 只计算经过的前缀，不会预先遍历全部数据。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_scroll.h"

namespace cui {

class RenderVirtualList;

/// 滚动条条带宽度（与 RenderSingleChildViewport 同一口径）。
inline constexpr double kVirtualListScrollbarWidth = 10.0;
inline constexpr double kVirtualListMinThumb = 24.0;
/// 空串 = 走 Painter 默认字体（fontconfig / 各平台 fallback 会选带 CJK 字形的字体）。
/// 不能写死 "DejaVu Sans"：它没有中文字形，行文本里的中文会显示成方框。
inline constexpr const char* kVirtualListFontFamily = "";

/// 虚拟列表数据模型：项数、每项高度、绘制一行的回调。
///
/// `paintItem` 收到的 rect 已是内容坐标系（减去滚动偏移）中的行矩形，
/// 宽度已让出右侧滚动条条带；斑马纹 / hover / 选中背景由控件先画好。
class VirtualListModel {
public:
    virtual ~VirtualListModel() = default;
    virtual std::size_t itemCount() const = 0;
    /// 行高，必须 >= 1（前缀和严格递增）。
    virtual double itemHeight(std::size_t index) const = 0;
    /// 固定行高模型返回该高度：总长与下标换算走 O(1)，不建前缀和缓存。
    virtual std::optional<double> fixedRowHeight() const { return std::nullopt; }
    virtual void paintItem(Painter& painter, std::size_t index, const Rect& rect,
                           bool selected, bool hovered) const = 0;
};

namespace detail {

/// 行文本的统一绘制口径：先设字体/颜色，再把画布平移到基线位置。
inline void paintVirtualRowText(Painter& p, const Rect& rect, const std::string& text) {
    if (text.empty())
        return;
    p.setFontFamily(kVirtualListFontFamily);
    p.setFontSize(Theme::fontSizeBody);
    p.setSourceColor(theme().textPrimary);
    SkCanvas* c = p.canvas();
    if (!c)
        return;
    c->save();
    c->translate(static_cast<SkScalar>(rect.x + 10.0),
                 static_cast<SkScalar>(rect.y + Painter::centeredBaselineY(
                                                   rect.height, kVirtualListFontFamily,
                                                   Theme::fontSizeBody)));
    p.showText(text.c_str());
    c->restore();
}

}  // namespace detail

/// 固定行高模型：标签按需生成，100 万行也不会分配 100 万个字符串。
class FixedListModel : public VirtualListModel {
public:
    using LabelBuilder = std::function<std::string(std::size_t)>;

    FixedListModel(std::size_t count, double rowHeight, LabelBuilder label)
        : count_(count), rowHeight_(std::max(1.0, rowHeight)), label_(std::move(label)) {}

    std::size_t itemCount() const override { return count_; }
    double itemHeight(std::size_t) const override { return rowHeight_; }
    std::optional<double> fixedRowHeight() const override { return rowHeight_; }
    void paintItem(Painter& p, std::size_t index, const Rect& rect, bool, bool) const override {
        detail::paintVirtualRowText(p, rect, label_ ? label_(index) : std::string());
    }

private:
    std::size_t count_;
    double rowHeight_;
    LabelBuilder label_;
};

/// 变高模型：高度与标签都由闭包按 index 惰性给出。
class VariableListModel : public VirtualListModel {
public:
    using HeightBuilder = std::function<double(std::size_t)>;
    using LabelBuilder = std::function<std::string(std::size_t)>;

    VariableListModel(std::size_t count, HeightBuilder height, LabelBuilder label)
        : count_(count), height_(std::move(height)), label_(std::move(label)) {}

    std::size_t itemCount() const override { return count_; }
    double itemHeight(std::size_t index) const override {
        return std::max(1.0, height_ ? height_(index) : 1.0);
    }
    void paintItem(Painter& p, std::size_t index, const Rect& rect, bool, bool) const override {
        detail::paintVirtualRowText(p, rect, label_ ? label_(index) : std::string());
    }

private:
    std::size_t count_;
    HeightBuilder height_;
    LabelBuilder label_;
};

/// 现成字符串模型（小数据集用；大数据集请用 FixedListModel）。
class StringListModel : public VirtualListModel {
public:
    StringListModel(std::vector<std::string> items, double rowHeight)
        : items_(std::move(items)), rowHeight_(std::max(1.0, rowHeight)) {}

    std::size_t itemCount() const override { return items_.size(); }
    double itemHeight(std::size_t) const override { return rowHeight_; }
    std::optional<double> fixedRowHeight() const override { return rowHeight_; }
    void paintItem(Painter& p, std::size_t index, const Rect& rect, bool, bool) const override {
        if (index < items_.size())
            detail::paintVirtualRowText(p, rect, items_[index]);
    }

private:
    std::vector<std::string> items_;
    double rowHeight_;
};

/// Widget 与 RenderObject 共享的状态：模型 + 惰性前缀和 + 选择/hover/拖拽。
///
/// 滚动偏移由 ScrollController 持有（外部可直接 jumpTo）；其余状态在这里。
class VirtualListState {
public:
    struct DragState {
        int64_t pointerId = -1;
        double startY = 0.0;
        double startThumbTop = 0.0;
        bool active = false;
    };

    explicit VirtualListState(SharedPtr<VirtualListModel> model) : model_(std::move(model)) {}

    void setModel(SharedPtr<VirtualListModel> model) {
        model_ = std::move(model);
        offsets_.clear();
        selected_ = hovered_ = -1;
        invalidateLayout();
    }
    const SharedPtr<VirtualListModel>& model() const { return model_; }

    void setOwner(RenderVirtualList* owner) { owner_ = owner; }
    std::size_t itemCount() const { return model_ ? model_->itemCount() : 0; }

    /// offsets_[0] = 0；offsets_[i+1] = offsets_[i] + itemHeight(i)。
    double offsetAt(std::size_t index) {
        if (!model_)
            return 0.0;
        const std::size_t n = model_->itemCount();
        const std::size_t target = std::min(index, n);
        if (auto h = model_->fixedRowHeight())
            return static_cast<double>(target) * *h;
        ensurePrefixToIndex(target);
        return offsets_[target];
    }

    double totalHeight() {
        if (!model_)
            return 0.0;
        if (auto h = model_->fixedRowHeight())
            return static_cast<double>(model_->itemCount()) * *h;
        return offsetAt(itemCount());
    }

    /// 内容 y → 项下标（前缀和补齐到覆盖 y 后二分）。
    std::size_t indexAtY(double y) {
        const std::size_t n = itemCount();
        if (n == 0)
            return 0;
        if (auto h = model_->fixedRowHeight()) {
            const auto idx = static_cast<std::size_t>(std::max(0.0, y / *h));
            return std::min(idx, n - 1);
        }
        ensurePrefixToY(y, n);
        std::size_t lo = 0, hi = offsets_.size() - 1;
        while (lo + 1 < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (offsets_[mid] <= y)
                lo = mid;
            else
                hi = mid;
        }
        return std::min(lo, n - 1);
    }

    /// [first, last)：覆盖 [scrollY, scrollY + viewportHeight) 的行号。
    std::pair<std::size_t, std::size_t> visibleRange(double scrollY, double viewportHeight) {
        const std::size_t n = itemCount();
        if (n == 0)
            return {0, 0};
        const std::size_t first = indexAtY(scrollY);
        double y = offsetAt(first) - scrollY;
        std::size_t idx = first;
        while (idx < n && y < viewportHeight) {
            y += std::max(1.0, model_->itemHeight(idx));
            ++idx;
        }
        return {first, idx};
    }

    int64_t selected() const { return selected_; }
    void setSelected(int64_t index) {
        if (selected_ == index)
            return;
        selected_ = index;
        invalidatePaint();
    }
    int64_t hovered() const { return hovered_; }
    void setHovered(int64_t index) {
        if (hovered_ == index)
            return;
        hovered_ = index;
        invalidatePaint();
    }

    DragState& drag() { return drag_; }
    const DragState& drag() const { return drag_; }

private:
    /// 补到 offsets_[target] 可用（offsetAt 用）。
    void ensurePrefixToIndex(std::size_t target) {
        if (!model_)
            return;
        if (offsets_.empty())
            offsets_.push_back(0.0);
        while (offsets_.size() <= target) {
            const std::size_t index = offsets_.size() - 1;
            offsets_.push_back(offsets_.back() + std::max(1.0, model_->itemHeight(index)));
        }
    }

    /// 只补到覆盖 y（indexAtY 用）——滚到深处不会预先遍历全部数据。
    void ensurePrefixToY(double y, std::size_t n) {
        if (!model_)
            return;
        if (offsets_.empty())
            offsets_.push_back(0.0);
        while (offsets_.size() < n + 1 && offsets_.back() <= y) {
            const std::size_t index = offsets_.size() - 1;
            offsets_.push_back(offsets_.back() + std::max(1.0, model_->itemHeight(index)));
        }
    }

    /// 定义见 virtual_list.cpp（RenderVirtualList 在头文件里只是前置声明）。
    void invalidatePaint();
    void invalidateLayout();

    SharedPtr<VirtualListModel> model_;
    std::vector<double> offsets_;
    int64_t selected_ = -1;
    int64_t hovered_ = -1;
    DragState drag_;
    /// RenderObject 析构时清空，避免 Widget 仍持有 state 时的悬挂回调。
    RenderVirtualList* owner_ = nullptr;
};

/// 虚拟列表的叶渲染节点：不 adopt 任何子节点，直接把行录进 Painter。
class RenderVirtualList : public RenderBox {
public:
    RenderVirtualList(SharedPtr<VirtualListState> state, SharedPtr<ScrollController> controller);
    ~RenderVirtualList() override;

    const SharedPtr<VirtualListState>& state() const { return state_; }
    const SharedPtr<ScrollController>& controller() const { return controller_; }
    double scrollOffset() const { return controller_->offset(); }
    /// 当前可见行区间（需先完成一次 layout）。
    std::pair<std::size_t, std::size_t> visibleRange() const;

    bool hitTestSelf(const Offset& position) const override;
    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    double maxScroll() const;
    std::optional<Rect> thumbRect() const;
    void scrollToTrack(double thumbTop);
    /// 滚动一个像素增量；返回是否真的改变（到边界时让事件继续冒泡）。
    bool scrollBy(double delta);
    std::optional<std::size_t> rowAt(double localY) const;

    SharedPtr<VirtualListState> state_;
    SharedPtr<ScrollController> controller_;
    ScrollController::ListenerHandle listener_ = ScrollController::kInvalidListener;
};

/// 虚拟列表控件（叶节点）。
class VirtualList : public LeafRenderObjectWidget {
public:
    explicit VirtualList(SharedPtr<VirtualListModel> model,
                         SharedPtr<ScrollController> controller = nullptr,
                         KeyPtr key = nullptr);
    /// 直接接管已构造的共享状态（外部需持有 state 做探针时更方便）。
    explicit VirtualList(SharedPtr<VirtualListState> state,
                         SharedPtr<ScrollController> controller = nullptr,
                         KeyPtr key = nullptr);

    const SharedPtr<VirtualListState>& state() const { return state_; }
    const SharedPtr<ScrollController>& controller() const { return controller_; }

    /// 让指定行进入视口顶部。首次 layout 前 extents 尚未建立，此时用 forceOffset，
    /// layout 里的 applyExtents 会把它夹回合法范围。
    void scrollToIndex(std::size_t index);
    void scrollToOffset(double offset);

    RenderObject* createRenderObject() override;
    void updateRenderObject(RenderObject* ro) override;

private:
    SharedPtr<VirtualListState> state_;
    SharedPtr<ScrollController> controller_;
};

}  // namespace cui
