#pragma once
/// @file widgets/sliver.h
/// @brief Sliver 控件层：把 rendering/sliver.h 的 sliver 协议接进 Widget/Element 三层。
///
/// 对应 上游源码：
///   - widgets/sliver.dart（Sliver 基类、SliverToBoxAdapter 之外的通用约定）
///   - widgets/sliver_padding.dart（SliverPadding）
///   - widgets/sliver_list.dart（SliverList）
///   - widgets/sliver_fixed_extent_list.dart（SliverFixedExtentList）
///   - widgets/viewport.dart（Viewport）
///   - widgets/scroll_view.dart（CustomScrollView / BoxScrollView）
///
/// 层级映射严格对齐 上游：
///   SliverToBoxAdapter / SliverPadding → SingleChildRenderObjectWidget
///   Sliver / SliverList / SliverFixedExtentList / Viewport
///       → MultiChildRenderObjectWidget（上游的 Sliver 正是它的子类）
///   CustomScrollView → StatelessWidget（组合 Scrollable + Viewport，
///       对应 上游 BoxScrollView 组合 Scrollable + Viewport）
///
/// 命名（2026-11-02 对齐完成）：sliver 版视口就叫 `Viewport`，与上游一致；
/// widgets/scroll.h 里那个单子节点盒子版视口是 上游 的私有
/// `_SingleChildViewport`（SingleChildScrollView 用），本仓对应物叫
/// `SingleChildViewport`，不占用 `Viewport` 这个名字。
///
/// 本批把 上游 的懒加载链路透到控件层：`SliverChildDelegate`（builder / 列表
/// 两种实现）+ `SliverMultiBoxAdaptorElement`（实现 RenderSliverBoxChildManager），
/// `SliverList` / `SliverFixedExtentList` 由 delegate 供子，窗口外的子节点在
/// 布局期间被回收。四点简化（前两条详见 rendering/sliver.h 文件头偏离 3）：
///   - 子节点不再被包 RepaintBoundary / AutomaticKeepAlive / IndexedSemantics
///     （本仓还没有这三件套），故 delegate 直接返回 builder 的 Widget；
///   - keepAlive 桶未移植，`addAutomaticKeepAlives` 一类参数没有对应物；
///   - 上游 `SliverChildBuilderDelegate` 的 `findChildIndexCallback` 走
///     `_SaltedValueKey` 拆封；本仓 builder 直接给出带 Key 的 Widget，
///     `findIndexByKey` 因此不必加盐；
///   - 上游用 `slot`（= 子序号）驱动 attachRenderObject 重排被搬动的子节点；
///     本仓 Element 无 slot（见 framework/element.h 顶部），改由 performRebuild
///     里对搬来的子节点显式 `insertChild(RO, currentBeforeChild)` 复位。
///     `SliverMultiBoxAdaptorWidget::replaceMovedChildren()` 是 上游 同名构造
///     参数的等价物（本仓由 Widget 虚函数给出，不经 Element 构造参数）。
///
/// `reverse` / `AxisDirection::Up` / `Left` 只部分可用，见 rendering/sliver.h
/// 文件头偏离 4（非居中视口的反向绘制偏移反算）。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <typeinfo>
#include <utility>
#include <vector>

#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_scroll.h"  // ScrollController
#include "cui/rendering/sliver.h"
#include "cui/widgets/notification.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

// ── Sliver 基类 ─────────────────────────────────────────────────────────────

/// 子节点为 sliver 的渲染对象控件基类。上游的 `Sliver`
/// （widgets/sliver.dart:「abstract class Sliver extends
/// MultiChildRenderObjectWidget」）。
class Sliver : public MultiChildRenderObjectWidget {
public:
    using MultiChildRenderObjectWidget::MultiChildRenderObjectWidget;
};

// ── SliverToBoxAdapter ──────────────────────────────────────────────────────

/// 把一个普通（盒子）控件接入 sliver 序列。上游的 `SliverToBoxAdapter`。
class SliverToBoxAdapter : public SingleChildRenderObjectWidget {
public:
    explicit SliverToBoxAdapter(SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)) {}

    RenderObject* createRenderObject() override { return new RenderSliverToBoxAdapter(); }

    std::string toStringShort() const override { return "SliverToBoxAdapter"; }
};

// ── SliverPadding ───────────────────────────────────────────────────────────

/// 给子 sliver 加内边距。上游的 `SliverPadding`。
class SliverPadding : public SingleChildRenderObjectWidget {
public:
    explicit SliverPadding(EdgeInsets padding, SharedPtr<Widget> sliver = nullptr,
                           KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(sliver)), padding_(padding) {}

    const EdgeInsets& padding() const { return padding_; }

    RenderObject* createRenderObject() override { return new RenderSliverPadding(padding_); }

    void updateRenderObject(RenderObject* ro) override {
        if (auto* p = dynamic_cast<RenderSliverPadding*>(ro))
            p->setPadding(padding_);
    }

    std::string toStringShort() const override { return "SliverPadding"; }

private:
    EdgeInsets padding_;
};

// ── SliverChildDelegate 家族 ────────────────────────────────────────────────

/// 懒加载子节点构造器。上游的 `NullableIndexedWidgetBuilder`：返回空表示该
/// index 没有子节点（列表到此为止）。
using NullableIndexedWidgetBuilder =
    std::function<SharedPtr<Widget>(BuildContext& context, int index)>;

/// 按 Key 找回子节点的新序号。上游的 `ChildIndexGetter`。
using ChildIndexGetter = std::function<std::optional<int>(const KeyPtr& key)>;

/// 多盒子 sliver 的子节点供给者。上游的 `SliverChildDelegate`
/// （widgets/scroll_delegate.dart）。
///
/// 约定与 上游 一致：`build` 一旦对某个 index 返回空，`estimatedChildCount`
/// 此后必须给出精确值（托管人靠它实现 `childCount`）。子 Widget 会被缓存，
/// 只有换了 delegate 实例且新 delegate 的 `shouldRebuild` 为真才重新构建。
///
/// 这些方法都不设 const：本仓的列表型 delegate 会惰性填 key→序号表（上游
/// 的 `_keyToIndex` 同款），`build` 也要把 context 交给 builder。
class SliverChildDelegate {
public:
    virtual ~SliverChildDelegate() = default;

    /// 上游的 `build(context, index)`。
    virtual SharedPtr<Widget> build(BuildContext& context, int index) = 0;

    /// 上游的 `estimatedChildCount`：空 = 未知（子节点无限或多）。
    virtual std::optional<int> estimatedChildCount() { return std::nullopt; }

    /// 上游的 `estimateMaxScrollOffset`：空 = 让调用方按已布局子节点均值外推。
    virtual std::optional<double> estimateMaxScrollOffset(int firstIndex, int lastIndex,
                                                          double leadingScrollOffset,
                                                          double trailingScrollOffset) {
        (void)firstIndex;
        (void)lastIndex;
        (void)leadingScrollOffset;
        (void)trailingScrollOffset;
        return std::nullopt;
    }

    /// 上游的 `didFinishLayout`：本轮布局首尾序号。
    virtual void didFinishLayout(int firstIndex, int lastIndex) {
        (void)firstIndex;
        (void)lastIndex;
    }

    /// 上游的 `shouldRebuild`：新实例是否代表了不同的内容。
    virtual bool shouldRebuild(const SliverChildDelegate& oldDelegate) = 0;

    /// 上游的 `findIndexByKey`：重排时找回某 Key 现在的序号。
    virtual std::optional<int> findIndexByKey(const KeyPtr& key) {
        (void)key;
        return std::nullopt;
    }
};

/// 显式子节点列表供给者。上游的 `SliverChildListDelegate`。
///
/// 偏离：上游 用 `_keyToIndex == nullptr` 区分 const 实例（不做 key 查找）；
/// 本仓没有 const 构造，统一按「只有带 Key 的子节点参与重排定位」惰性建表。
class SliverChildListDelegate : public SliverChildDelegate {
public:
    explicit SliverChildListDelegate(std::vector<SharedPtr<Widget>> children)
        : children_(std::move(children)) {}

    const std::vector<SharedPtr<Widget>>& children() const { return children_; }

    SharedPtr<Widget> build(BuildContext&, int index) override {
        if (index < 0 || index >= static_cast<int>(children_.size()))
            return nullptr;
        return children_[static_cast<size_t>(index)];
    }

    std::optional<int> estimatedChildCount() override {
        return static_cast<int>(children_.size());
    }

    /// 上游：逐元素比身份（Dart 的 List == 是引用比较，元素级 deep equals）。
    bool shouldRebuild(const SliverChildDelegate& oldDelegate) override {
        auto* old = dynamic_cast<const SliverChildListDelegate*>(&oldDelegate);
        if (!old)
            return true;
        return children_ != old->children_;
    }

    /// 上游的 `_findChildIndex`：惰性填 key → 序号表，命中即记游标。
    std::optional<int> findIndexByKey(const KeyPtr& key) override {
        if (!key)
            return std::nullopt;
        auto cached = keyToIndex_.find(key.get());
        if (cached != keyToIndex_.end())
            return cached->second;
        int index = cursor_;
        while (index < static_cast<int>(children_.size())) {
            const KeyPtr& childKey = children_[static_cast<size_t>(index)]->key();
            if (childKey)
                keyToIndex_.emplace(childKey.get(), index);
            if (childKey && *childKey == *key) {
                cursor_ = index + 1;
                return index;
            }
            ++index;
        }
        cursor_ = index;
        return std::nullopt;
    }

private:
    std::vector<SharedPtr<Widget>> children_;
    std::map<const Key*, int> keyToIndex_;
    int cursor_ = 0;
};

/// builder 供给者。上游的 `SliverChildBuilderDelegate`。
///
/// 偏离：上游 会把子节点包一层 `KeyedSubtree(key: _SaltedValueKey(child.key))`
/// 以便在 RepaintBoundary / AutomaticKeepAlive / IndexedSemantics 之外仍然保留
/// Key；本仓这三件套还没有（见文件头简化说明），builder 的 Widget 自己带
/// Key，故 `findIndexByKey` 直接用它，不必加盐。
class SliverChildBuilderDelegate : public SliverChildDelegate {
public:
    explicit SliverChildBuilderDelegate(NullableIndexedWidgetBuilder builder,
                                        ChildIndexGetter findChildIndexCallback = nullptr,
                                        std::optional<int> childCount = std::nullopt)
        : builder_(std::move(builder)),
          findChildIndexCallback_(std::move(findChildIndexCallback)),
          childCount_(childCount) {}

    SharedPtr<Widget> build(BuildContext& context, int index) override {
        if (!builder_ || index < 0)
            return nullptr;
        if (childCount_ && index >= *childCount_)
            return nullptr;
        return builder_(context, index);
    }

    std::optional<int> estimatedChildCount() override { return childCount_; }

    /// 上游：builder 内容不可比较，换 delegate 即重建。
    bool shouldRebuild(const SliverChildDelegate&) override { return true; }

    std::optional<int> findIndexByKey(const KeyPtr& key) override {
        if (!findChildIndexCallback_ || !key)
            return std::nullopt;
        return findChildIndexCallback_(key);
    }

private:
    NullableIndexedWidgetBuilder builder_;
    ChildIndexGetter findChildIndexCallback_;
    std::optional<int> childCount_;
};

// ── SliverMultiBoxAdaptorWidget ─────────────────────────────────────────────

/// 多个盒子子节点排成一线的 sliver 控件基类。上游的
/// `SliverMultiBoxAdaptorWidget`。
///
/// 上游 的父链是 `SliverWithKeepAliveWidget → RenderObjectWidget`；本仓
/// KeepAlive 体系未移植（文件头简化说明），直接挂在自有 `Sliver` 基类下。
/// 注意它虽然继承 MultiChildRenderObjectWidget，**子节点不走 children_**：
/// 全部由 SliverMultiBoxAdaptorElement 按序号懒建（上游 的 Sliver 同样把
/// children 恒置为空列表）。
class SliverMultiBoxAdaptorWidget : public Sliver {
public:
    explicit SliverMultiBoxAdaptorWidget(SharedPtr<SliverChildDelegate> delegate,
                                         KeyPtr key = nullptr)
        : Sliver(std::move(key), {}), delegate_(std::move(delegate)) {}

    /// 上游的 `delegate`。
    SliverChildDelegate* delegate() const { return delegate_.get(); }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;

    /// 上游的 `estimateMaxScrollOffset`：转交 delegate；空 = 让托管人外推。
    std::optional<double> estimateMaxScrollOffset(int firstIndex, int lastIndex,
                                                  double leadingScrollOffset,
                                                  double trailingScrollOffset) {
        return delegate_ ? delegate_->estimateMaxScrollOffset(firstIndex, lastIndex,
                                                              leadingScrollOffset,
                                                              trailingScrollOffset)
                         : std::nullopt;
    }

    /// 上游的 `_replaceMovedChildren`：布局依赖既有子节点布局偏移的
    /// RenderObject（RenderSliverList）为 true；按序号算偏移的
    /// （RenderSliverFixedExtentList）为 false，可少造子节点。
    virtual bool replaceMovedChildren() const { return false; }

    std::string toStringShort() const override { return "SliverMultiBoxAdaptorWidget"; }

protected:
    SharedPtr<SliverChildDelegate> delegate_;
};

// ── SliverList / SliverFixedExtentList ──────────────────────────────────────

/// 变高子节点列表。上游的 `SliverList`。
class SliverList : public SliverMultiBoxAdaptorWidget {
public:
    explicit SliverList(SharedPtr<SliverChildDelegate> delegate, KeyPtr key = nullptr)
        : SliverMultiBoxAdaptorWidget(std::move(delegate), std::move(key)) {}

    /// 上游的 `SliverList(children: [...])`（等价 SliverList.list）。
    explicit SliverList(std::vector<SharedPtr<Widget>> children, KeyPtr key = nullptr)
        : SliverMultiBoxAdaptorWidget(
              cui::make_shared<SliverChildListDelegate>(std::move(children)), std::move(key)) {}

    /// 上游的 `SliverList.builder`。
    static SharedPtr<SliverList> builder(NullableIndexedWidgetBuilder itemBuilder,
                                               std::optional<int> itemCount = std::nullopt,
                                               ChildIndexGetter findChildIndexCallback = nullptr,
                                               KeyPtr key = nullptr) {
        return cui::make_shared<SliverList>(
            cui::make_shared<SliverChildBuilderDelegate>(std::move(itemBuilder),
                                                         std::move(findChildIndexCallback),
                                                         itemCount),
            std::move(key));
    }

    RenderObject* createRenderObject() override { return new RenderSliverList(nullptr); }

    /// RenderSliverList 靠既有子节点的布局偏移定位，搬动后原槽要补一个新子。
    bool replaceMovedChildren() const override { return true; }

    std::string toStringShort() const override { return "SliverList"; }
};

/// 定高子节点列表。上游的 `SliverFixedExtentList`。
class SliverFixedExtentList : public SliverMultiBoxAdaptorWidget {
public:
    SliverFixedExtentList(double itemExtent, SharedPtr<SliverChildDelegate> delegate,
                          KeyPtr key = nullptr)
        : SliverMultiBoxAdaptorWidget(std::move(delegate), std::move(key)),
          itemExtent_(itemExtent) {}

    /// 上游的 `SliverFixedExtentList.list`。
    SliverFixedExtentList(double itemExtent, std::vector<SharedPtr<Widget>> children,
                          KeyPtr key = nullptr)
        : SliverMultiBoxAdaptorWidget(
              cui::make_shared<SliverChildListDelegate>(std::move(children)), std::move(key)),
          itemExtent_(itemExtent) {}

    /// 上游的 `SliverFixedExtentList.builder`。
    static SharedPtr<SliverFixedExtentList> builder(
        double itemExtent, NullableIndexedWidgetBuilder itemBuilder,
        std::optional<int> itemCount = std::nullopt,
        ChildIndexGetter findChildIndexCallback = nullptr, KeyPtr key = nullptr) {
        return cui::make_shared<SliverFixedExtentList>(
            itemExtent,
            cui::make_shared<SliverChildBuilderDelegate>(std::move(itemBuilder),
                                                         std::move(findChildIndexCallback),
                                                         itemCount),
            std::move(key));
    }

    double itemExtent() const { return itemExtent_; }

    RenderObject* createRenderObject() override {
        return new RenderSliverFixedExtentList(nullptr, itemExtent_);
    }

    void updateRenderObject(RenderObject* ro) override {
        if (auto* l = dynamic_cast<RenderSliverFixedExtentList*>(ro))
            l->setItemExtent(itemExtent_);
    }

    std::string toStringShort() const override { return "SliverFixedExtentList"; }

private:
    double itemExtent_;
};

// ── Viewport ────────────────────────────────────────────────────────────────

/// 多 sliver 视口。上游的 `Viewport`（widgets/viewport.dart）。
///
/// 与 ScrollController 的接线沿用本仓既有简化（位置存在控制器里、无
/// ScrollPosition 抽象）：
///   - build/updateRenderObject 时把 `controller->offset()` 推给渲染对象；
///   - 渲染对象布局完把内容区间回写 `controller->applyExtents()`
///     （只刷新绘制观察者，不在布局期触发 setState）。
class Viewport : public Sliver {
public:
    explicit Viewport(ScrollController* controller = nullptr,
                            AxisDirection axisDirection = AxisDirection::Down,
                            AxisDirection crossAxisDirection = AxisDirection::Right,
                            double cacheExtent = 250.0,
                            std::vector<SharedPtr<Widget>> children = {},
                            KeyPtr key = nullptr)
        : Sliver(std::move(key), std::move(children)),
          controller_(controller),
          axisDirection_(axisDirection),
          crossAxisDirection_(crossAxisDirection),
          cacheExtent_(cacheExtent) {}

    ScrollController* controller() const { return controller_; }
    AxisDirection axisDirection() const { return axisDirection_; }
    AxisDirection crossAxisDirection() const { return crossAxisDirection_; }
    double cacheExtent() const { return cacheExtent_; }

    RenderObject* createRenderObject() override {
        auto* vp = new RenderViewport(axisDirection_);
        apply(vp);
        return vp;
    }

    void updateRenderObject(RenderObject* ro) override {
        if (auto* vp = dynamic_cast<RenderViewport*>(ro))
            apply(vp);
    }

    /// 上游的 `_ViewportElement`（`MultiChildRenderObjectElement` +
    /// `ViewportElementMixin`）：冒泡经过本视口的 `ViewportNotification`
    /// 子类 depth 加一，外层 `NotificationListener` 据此区分「我最近的」和
    /// 「嵌套在里面的」滚动视图。
    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override {
        return std::make_unique<ViewportElement>(std::move(self));
    }

    std::string toStringShort() const override { return "Viewport"; }

private:
    void apply(RenderViewport* vp) const {
        vp->setAxisDirection(axisDirection_);
        vp->setCrossAxisDirection(crossAxisDirection_);
        vp->setCacheExtent(cacheExtent_);
        vp->setOffset(controller_ ? controller_->offset() : 0.0);
        ScrollController* c = controller_;
        vp->setOnExtentsChanged([c](double /*minExtent*/, double maxExtent,
                                    double viewportExtent) {
            if (c)
                c->applyExtents(maxExtent, viewportExtent);
        });
    }

    ScrollController* controller_;
    AxisDirection axisDirection_;
    AxisDirection crossAxisDirection_;
    double cacheExtent_;
};

// ── SliverMultiBoxAdaptorElement ────────────────────────────────────────────

/// 多盒子 sliver 的 Element，同时充当渲染层的子节点托管人。上游的
/// `SliverMultiBoxAdaptorElement`（implements RenderSliverBoxChildManager）。
///
/// 与 MultiChildRenderObjectElement 的区别：子节点**不在 Widget 里**，而是
/// 布局期间按序号向 delegate 索取，窗口外的按序号回收。上游用 `slot`
/// （= 子序号）驱动 attachRenderObject 重排；本仓 Element 没有 slot 概念
/// （见 framework/element.h 顶部说明），故重排由 performRebuild 里显式
/// `insertChild` 复位，新建/卸载仍走挂载/卸载钩子。
class SliverMultiBoxAdaptorElement : public RenderObjectElement,
                                     public RenderSliverBoxChildManager {
public:
    explicit SliverMultiBoxAdaptorElement(SharedPtr<Widget> widget);

    /// 本 Element 的渲染对象（多盒子 sliver）。上游的 `renderObject` getter。
    RenderSliverMultiBoxAdaptor* adaptor() const;

    /// 按序号挂载的子 Element 表（测试口径：窗口内实际存在的子）。
    const std::map<int, std::unique_ptr<Element>>& childElements() const {
        return childElements_;
    }

    void update(SharedPtr<Widget> newWidget) override;
    void visitChildren(const std::function<void(Element*)>& visitor) override;
    void unmount() override;
    void deactivate() override;
    void activate() override;

    // ── RenderSliverBoxChildManager ─────────────────────────────────────────
    void createChild(int index, RenderBox* after) override;
    void removeChild(RenderBox* child) override;
    double estimateMaxScrollOffset(const SliverConstraints& constraints, int firstIndex,
                                   int lastIndex, double leadingScrollOffset,
                                   double trailingScrollOffset) override;
    int childCount() override;
    void didAdoptChild(RenderBox* child) override;
    void setDidUnderflow(bool value) override { didUnderflow_ = value; }
    void didStartLayout() override {}
    void didFinishLayout() override;

protected:
    /// 上游的 `createRenderObject`：本仓 widget->createRenderObject() 拿不到
    /// Element，故在此回填托管人（渲染层的装配口）。
    std::unique_ptr<RenderObject> createRenderObject() override;

    void performRebuild() override;

    /// 上游的 `insertRenderObjectChild(child, slot)`：按 currentBeforeChild_
    /// 定位插入序号；序号本身由 didAdoptChild 写入。
    void insertRenderObjectChild(RenderObject* child, Element* owner) override;
    void removeRenderObjectChild(RenderObject* child) override;

private:
    SliverMultiBoxAdaptorWidget* adaptorWidget() const;

    /// 上游的 `delegate.build(this, index)`。
    SharedPtr<Widget> buildAt(int index);

    /// 上游覆写的 `updateChild`：换了 RenderObject 时把旧布局偏移带过去。
    std::unique_ptr<Element> updateChildPreserving(std::unique_ptr<Element> child,
                                                   const SharedPtr<Widget>& newWidget);

    static SliverMultiBoxAdaptorParentData* parentDataOf(Element* element);
    static SliverMultiBoxAdaptorParentData* parentDataOfBox(RenderBox* box);
    static RenderBox* boxOfElement(Element* element);

    /// 上游的 `_childElements`（SplayTreeMap<int, Element?>）。
    std::map<int, std::unique_ptr<Element>> childElements_;
    /// 上游的 `_currentBeforeChild`：下一个新子节点插在它之后。
    RenderBox* currentBeforeChild_ = nullptr;
    /// 上游的 `_currentlyUpdatingChildIndex`：didAdoptChild 写序号的依据。
    std::optional<int> currentlyUpdatingChildIndex_;
    /// 上游的 `_didUnderflow`：本轮布局子节点不够填满绘制窗口。
    bool didUnderflow_ = false;
};

}  // namespace cui_core
