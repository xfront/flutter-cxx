/// @file test_auto_hide_toolbar.cpp
/// @brief AutoHideToolbar（widgets/auto_hide_toolbar.h）单元测试。
///
/// 覆盖：
///   1. 初始收缩态：可见条带 = peekSize，热区 = 触发容差；条带底色可命中，
///      溢出到条带之外的内容按钮**点不到**。
///   2. 悬停进入 ⇒ 展开到 expandedSize，内容可命中，进度收敛到 1。
///   3. 盒子尺寸闸门（本批框架改动的回归）：中途帧同样只放行条带内的点。
///   4. 离开 ⇒ 先「按住」（hideDelay 内保持展开），到期 ⇒ 收缩回 peek。
///   5. 已展开时鼠标出/入热区不重置补间（rui 的
///      reenter_does_not_reset_expanded_animation 同款）。
///   6. pin 按钮：收缩态点不到、展开态点得到 ⇒ 钉住后离开不收缩；取消钉住时指针
///      已不在条带上（点击不产生悬停事件，指针还停在前面 hover 带出去的位置），
///      故直接进入按住态，到期收回。
///   7. 水平吸附（Left）：条带在主轴（宽）上逐帧显露，交叉轴（高）按上游 Align
///      无因子 + 有界的口径撑满父级。
///
/// 条带/hot 的交叉轴尺寸 = 父级给的最大值（本用例里就是窗口宽），不是内容自然
/// 宽：`RenderAlign` 在交叉轴没有因子且约束有界时取 `biggest()`。
///
/// headless 帧泵（flushBuildScope + flushLayout，不碰 paint）。隐式动画与自动隐藏
/// 定时都读真实时钟，故按「每帧 sleep 8ms」推进，断言只钉收敛值与中途单调性。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/auto_hide_toolbar.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/paint.h"

using namespace cui;

namespace {

int g_failures = 0;

bool near(double a, double b, double eps = 0.5) { return std::fabs(a - b) <= eps; }

/// 进度断言用紧容差（补间收敛是精确值，0.5 像素容差不适用）。
bool progressIs(double a, double b) { return std::fabs(a - b) <= 0.01; }

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

void checkSize(const RenderObject* ro, double w, double h, const std::string& what,
               double eps = 0.5) {
    auto* box = dynamic_cast<const RenderBox*>(ro);
    const bool ok = box && std::fabs(box->size().width - w) <= eps &&
                    std::fabs(box->size().height - h) <= eps;
    if (!ok)
        std::printf("      实际: %.2fx%.2f  期望: %.2fx%.2f\n",
                    box ? box->size().width : -1.0, box ? box->size().height : -1.0, w, h);
    check(ok, what);
}

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    /// 一帧真实时间：睡 8ms 再打 tick（Ticker::tick 用真实时钟算 elapsed）。
    void frame() {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        Ticker::tickAllActive(Duration(8));
        pump();
    }

    /// 把动画跑到收敛（隐式动画的 ticker 只在还有动画时出帧）。
    void settle(double maxMs = 1200.0) {
        double spent = 0.0;
        while (spent < maxMs && Ticker::hasActiveTickers()) {
            frame();
            spent += 8.0;
        }
    }

    void hover(double x, double y) {
        PointerHoverEvent e;
        e.position = {x, y};
        po.dispatchPointerEvent(e);
        pump();
    }
    void tap(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        PointerUpEvent u;
        u.position = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }

    /// 从根渲染对象做一次命中测试，返回命中链（根 → … → target）。
    std::vector<RenderObject*> chain(double x, double y) {
        std::vector<RenderObject*> out;
        rv.hitTest(Offset{x, y}, out);
        return out;
    }
};

bool chainHas(const std::vector<RenderObject*>& c, const RenderObject* ro) {
    return std::find(c.begin(), c.end(), ro) != c.end();
}

/// 深度优先取第一个指定类型的 RenderObject。
template <typename T>
T* findRo(Element* root) {
    RenderObject* ro = root ? root->findRenderObject() : nullptr;
    if (!ro)
        return nullptr;
    std::vector<RenderObject*> stack{ro};
    while (!stack.empty()) {
        RenderObject* n = stack.back();
        stack.pop_back();
        if (auto* hit = dynamic_cast<T*>(n))
            return hit;
        n->visitChildren([&](AbstractNode* c) {
            if (c)
                stack.push_back(static_cast<RenderObject*>(c));
        });
    }
    return nullptr;
}

Element* findElementFor(Element* root, const Widget* w) {
    if (!root)
        return nullptr;
    Element* found = nullptr;
    std::function<void(Element*)> walk = [&](Element* e) {
        if (found || !e)
            return;
        if (e->widget() == w) {
            found = e;
            return;
        }
        e->visitChildren(walk);
    };
    walk(root);
    return found;
}

/// 钉到左上角（RenderView 给窗口紧约束，代理盒子会一路 biggest 撑满）。
SharedPtr<Widget> pinned(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

AutoHideToolbarState* stateOf(Element* root, const Widget* w) {
    auto* e = findElementFor(root, w);
    if (!e)
        return nullptr;
    return static_cast<AutoHideToolbarState*>(static_cast<StatefulElement*>(e)->state());
}

constexpr double kExpanded = 56.0;
constexpr double kPeek = 2.0;
constexpr double kMargin = auto_hide_detail::kCollapsedTriggerMargin;
constexpr double kDelay = 60.0;
constexpr double kDuration = 120.0;
constexpr double kContentW = 300.0;
/// 窗口尺寸（= 条带交叉轴撑满的值：Align 无因子 + 有界 ⇒ 撑满父级）。
constexpr double kWindowW = 400.0;
constexpr double kWindowH = 300.0;

/// 造一棵带稳定内容指针的工具栏；`content` 用于取它的渲染盒做命中判定。
SharedPtr<AutoHideToolbar> makeToolbar(SharedPtr<Widget>& content,
                                       AutoHideDirection dir = AutoHideDirection::Top,
                                       bool pinnable = true) {
    content = cui::make_shared<DecoratedBox>(BoxDecoration{Color::fromRGB(0.2, 0.4, 0.9)});
    auto body = cui::make_shared<SizedBox>(kContentW, 40.0, content);
    return cui::make_shared<AutoHideToolbar>(std::move(body), dir, kExpanded, kPeek, kDelay,
                                             pinnable, Color::fromRGB(0.98, 0.98, 1.0),
                                             nullptr, kDuration);
}

// ── 1. 初始收缩态 ───────────────────────────────────────────────────────────
void testCollapsed(Harness& h, SharedPtr<AutoHideToolbar> tb, SharedPtr<Widget> content) {
    std::printf("[test] 初始收缩态\n");

    auto* band = findRo<RenderClipRect>(h.root);
    auto* hot = findRo<RenderMouseRegion>(h.root);
    auto* contentRo = findRo<RenderDecoratedBox>(findElementFor(h.root, content.get()));
    check(band != nullptr, "条带 ClipRect 在场");
    check(hot != nullptr, "热区 MouseRegion 在场");
    check(contentRo != nullptr, "内容渲染盒在场");
    if (!band || !hot || !contentRo)
        return;

    checkSize(band, kWindowW, kPeek, "条带 = 交叉轴撑满 × peekSize");
    checkSize(hot, kWindowW, kMargin, "热区 = 交叉轴撑满 × 触发容差");
    check(progressIs(stateOf(h.root, tb.get())->progress(), 0.0), "进度 0");

    // 条带内（y < 2）可命中；溢出到条带外的内容点不到。
    check(chainHas(h.chain(10.0, 1.0), contentRo), "条带内的内容仍可命中");
    check(!chainHas(h.chain(10.0, 30.0), contentRo), "被裁掉的内容点不到（盒子尺寸闸门）");
    check(!chainHas(h.chain(10.0, 12.0), contentRo), "容差区（条带外）点不到内容");
    // 条带外、容差内：热区在场但身后页面照样能收事件（opaque=false）。
    check(!chainHas(h.chain(10.0, 12.0), band), "容差区不属于条带");
}

// ── 2. 悬停展开 ─────────────────────────────────────────────────────────────
void testExpandOnHover(Harness& h, SharedPtr<AutoHideToolbar> tb, SharedPtr<Widget> content,
                       double& midProgress) {
    std::printf("[test] 悬停展开\n");

    h.hover(10.0, 10.0);
    h.frame();
    midProgress = stateOf(h.root, tb.get())->progress();
    check(midProgress > 0.0 && midProgress < 1.0, "中途帧进度在 (0,1)");

    auto* band = findRo<RenderClipRect>(h.root);
    check(band && band->size().height > kPeek && band->size().height < kExpanded,
          "中途帧条带逐帧变高");
    auto* contentRo = findRo<RenderDecoratedBox>(findElementFor(h.root, content.get()));
    if (band && contentRo) {
        const double inside = band->size().height * 0.5;
        const double outside = band->size().height + 8.0;
        check(chainHas(h.chain(10.0, inside), contentRo), "条带内的内容可命中");
        check(!chainHas(h.chain(10.0, outside), contentRo),
              "条带外的溢出内容仍点不到（中途帧）");
    }

    h.settle();
    check(progressIs(stateOf(h.root, tb.get())->progress(), 1.0), "收敛后进度 1");
    check(band && near(band->size().height, kExpanded), "条带展开到 expandedSize");
}

// ── 3. 离开后延迟收回 ───────────────────────────────────────────────────────
void testHoldThenCollapse(Harness& h, SharedPtr<AutoHideToolbar> tb) {
    std::printf("[test] 离开 → 按住 → 到期收缩\n");

    auto* st = stateOf(h.root, tb.get());
    check(st != nullptr && st->isHovering(), "仍在热区时 hovering=true");

    // 离开到条带 + 容差之外。
    h.hover(10.0, 200.0);
    check(st && st->isHolding(), "离开后进入「按住」态");
    for (int i = 0; i < 4 && st && st->isHolding(); ++i)
        h.frame();
    check(st && near(st->progress(), 1.0, 0.02), "hideDelay 未到期内保持展开（rui 同款）");

    for (int i = 0; i < 60 && st && st->isHolding(); ++i)
        h.frame();
    check(st && !st->isHolding(), "到期后解除按住态");
    h.settle();
    check(st && progressIs(st->progress(), 0.0), "到期后收缩到 0");
    auto* band = findRo<RenderClipRect>(h.root);
    check(band && near(band->size().height, kPeek), "条带回落到 peekSize");
}

// ── 4. 重新进入不重置补间 ───────────────────────────────────────────────────
void testReenterDoesNotReset(Harness& h, SharedPtr<AutoHideToolbar> tb, double midProgress) {
    std::printf("[test] 重新进入不重置补间\n");

    auto* st = stateOf(h.root, tb.get());
    if (!st)
        return;
    // 从收缩态重新展开，跑到中途。
    h.hover(10.0, 10.0);
    h.frame();
    h.frame();
    const double before = st->progress();
    check(before > 0.0, "再次悬停后开始展开");

    // 中途离开（进入按住态）再立刻回来：目标恒为 1 ⇒ 补间不重启。
    h.hover(10.0, 200.0);
    check(st->isHolding(), "离开进入按住态");
    const double afterExit = st->progress();
    h.hover(10.0, 12.0);
    check(!st->isHolding() && st->isHovering(), "回到热区解除按住态");
    check(st->progress() >= afterExit - 1e-9, "重新进入没有把进度清零");
    (void)midProgress;
    h.settle();
    check(progressIs(st->progress(), 1.0), "重新进入后仍收敛到 1");
}

// ── 5. pin 按钮 ─────────────────────────────────────────────────────────────
void testPin(Harness& h, SharedPtr<AutoHideToolbar> tb) {
    std::printf("[test] pin 按钮\n");

    auto* st = stateOf(h.root, tb.get());
    auto* band = findRo<RenderClipRect>(h.root);
    if (!st || !band) {
        check(false, "pin 用例：状态与条带在场");
        return;
    }
    // pin 在条带右上角内缩处（交叉轴 = 条带实际宽，即父级给的值）。
    const double pinX = band->size().width - auto_hide_detail::kPinInset -
                        auto_hide_detail::kPinSize / 2.0;
    const double pinY = auto_hide_detail::kPinInset + auto_hide_detail::kPinSize / 2.0;

    // 收缩态点不到 pin（条带只有 2 像素）。
    h.hover(10.0, 200.0);   // 确保离开热区
    h.settle();
    check(progressIs(st->progress(), 0.0), "收缩完成");
    h.tap(pinX, pinY);
    check(!st->isPinned(), "收缩态点不到 pin");

    // 展开态点得到 pin。
    h.hover(10.0, 10.0);
    h.settle();
    check(progressIs(st->progress(), 1.0), "悬停展开完成");
    h.tap(pinX, pinY);
    check(st->isPinned(), "展开态点击 pin 生效");

    // 钉住后离开不收缩。
    h.hover(10.0, 200.0);
    for (int i = 0; i < 20; ++i)
        h.frame();
    check(!st->isHolding(), "钉住态不挂隐藏定时");
    check(progressIs(st->progress(), 1.0), "钉住后离开仍保持展开");

    // 取消钉住。此时（虚拟）指针还停在 200 像素外（前面那步 hover 带出去的，
    // 点击本身不产生悬停事件）⇒ 取消钉住直接进入按住态，到期才收缩。
    h.tap(pinX, pinY);
    check(!st->isPinned(), "再次点击取消钉住");
    check(!st->isHovering(), "取消钉住时指针不在热区");
    check(st->isHolding(), "取消钉住即进入按住态");
    check(progressIs(st->progress(), 1.0), "按住态未到期前保持展开");

    for (int i = 0; i < 60 && st->isHolding(); ++i)
        h.frame();
    h.settle();
    check(progressIs(st->progress(), 0.0), "取消钉住后到期收缩");
}

// ── 6. 水平吸附 ─────────────────────────────────────────────────────────────
void testHorizontal(Harness& h) {
    std::printf("[test] 水平吸附（Left）\n");

    SharedPtr<Widget> content;
    auto tb = makeToolbar(content, AutoHideDirection::Left);
    h.attach(pinned(tb), 400.0, 300.0);

    auto* band = findRo<RenderClipRect>(h.root);
    auto* hot = findRo<RenderMouseRegion>(h.root);
    if (!band || !hot) {
        check(false, "Left 方向条带/热区在场");
        return;
    }
    check(true, "Left 方向条带/热区在场");
    checkSize(band, kPeek, kWindowH, "收缩态条带 = peekSize × 交叉轴撑满");
    checkSize(hot, kMargin, kWindowH, "热区宽度 = 触发容差");

    h.hover(10.0, 10.0);
    h.settle();
    checkSize(band, kExpanded, kWindowH, "展开后条带宽度 = expandedSize");
}

}  // namespace

int main() {
    {
        Harness h;
        SharedPtr<Widget> content;
        auto tb = makeToolbar(content);
        h.attach(pinned(tb), 400.0, 300.0);
        double mid = 0.0;
        testCollapsed(h, tb, content);
        testExpandOnHover(h, tb, content, mid);
        testHoldThenCollapse(h, tb);
        testReenterDoesNotReset(h, tb, mid);
        testPin(h, tb);
    }
    {
        Harness h;
        testHorizontal(h);
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILURES: %d\n", g_failures);
    return 1;
}
