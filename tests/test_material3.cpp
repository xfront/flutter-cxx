/// @file test_material3.cpp
/// @brief Material 第三批单元测试（Chip avatar/deleteIcon/selected、
///        InkWell 水波纹、Card onTap、Slider 标签气泡、MaterialBanner 随内容长高、
///        StatusBar 文字居中）。
///
/// 覆盖：
///   1. Chip：avatar + label 布局；onDeleted 追加删除按钮并响应点击；
///      selected 底色切换。
///   2. InkWell：点击触发 onTap；按下启动水波（真实时间推进到完成）；
///      鼠标 hover 高亮；按住拖出边界取消。
///   3. Card：onTap 非 null 时内层包 InkWell 并响应点击。
///   4. Slider 标签：拖拽时气泡可见（淡入）、几何在 thumb 上方；
///      抬起后淡出。
///   5. MaterialBanner：卡片高度随内容长（不再锁死 52），消息/操作/关闭全部
///      落在卡片矩形内，点操作后新增提示行卡片继续长高。
///   6. StatusBar：固定 22 高里每段文字的垂直中心与条体对齐，且有横向内边距
///      （钉 `EdgeInsets::symmetric` 的形参序 = (horizontal, vertical)）。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_inkwell.h"
#include "cui/rendering/render_interactive.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/interactive.h"
#include "cui/widgets/material.h"
#include "cui/widgets/text.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    RenderObject* rootRO = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rootRO = root->findRenderObject();
        rv.setChild(static_cast<RenderBox*>(rootRO));
        po.setRootNode(&rv);
        pump();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    template <typename T>
    T* findRenderObject() {
        struct Visitor {
            T* found = nullptr;
            void visit(RenderObject* ro) {
                if (!ro || found)
                    return;
                if (dynamic_cast<T*>(ro))
                    found = static_cast<T*>(ro);
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v;
        v.visit(rootRO);
        return v.found;
    }
};

PointerDownEvent downAt(double x, double y) {
    PointerDownEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerMoveEvent moveAt(double x, double y) {
    PointerMoveEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerUpEvent upAt(double x, double y) {
    PointerUpEvent e;
    e.position = {x, y};
    return e;
}
PointerHoverEvent hoverAt(double x, double y) {
    PointerHoverEvent e;
    e.position = {x, y};
    e.pointerKind = PointerDeviceKind::mouse;
    return e;
}

// ── 测试 1：Chip avatar / deleteIcon / selected ────────────────────────────
void testChipExtended() {
    std::printf("[test] Chip avatar / deleteIcon / selected\n");

    // avatar + label：布局成一行。
    {
        auto chip = cui::make_shared<Chip>(
            cui::make_shared<Text>("标签"),
            Color::fromRGBA(0, 0, 0, 0.12), 16.0, EdgeInsets::symmetric(4.0, 8.0),
            cui::make_shared<SizedBox>(20.0, 20.0));
        Harness h;
        h.attach(chip, 300, 60);
        auto* box = h.findRenderObject<RenderDecoratedBox>();
        check(box != nullptr, "Chip 生成 RenderDecoratedBox");
        check(box->color() == Color::fromRGBA(0, 0, 0, 0.12), "未选中底色");
        check(box->size().width > 40.0, "avatar + label 撑开宽度");
    }

    // onDeleted：尾部追加删除按钮，点击触发。
    {
        int deleted = 0;
        auto chip = cui::make_shared<Chip>(
            cui::make_shared<Text>("可删除"), Color::fromRGBA(0, 0, 0, 0.12), 16.0,
            EdgeInsets::symmetric(4.0, 8.0), nullptr, IconCode::Close,
            [&]() { ++deleted; });
        Harness h;
        h.attach(chip, 300, 60);
        auto* ri = h.findRenderObject<RenderIcon>();
        check(ri != nullptr && ri->code() == IconCode::Close,
              "onDeleted 追加 Close 图标按钮");
        // 点击删除按钮：用 RenderIcon 的全局中心派发（Chip 在紧约束下
        // 撑满窗口宽，手算行内偏移不可靠）。
        auto* dri = h.findRenderObject<RenderIcon>();
        Offset dic = dri->localToGlobal(Offset(dri->size().width / 2, dri->size().height / 2));
        h.dispatch(downAt(dic.dx, dic.dy));
        h.dispatch(upAt(dic.dx, dic.dy));
        check(deleted == 1, "点击删除按钮触发 onDeleted");
    }

    // selected：底色换 selectedColor。
    {
        auto chip = cui::make_shared<Chip>(
            cui::make_shared<Text>("选中"), Color::fromRGBA(0, 0, 0, 0.12), 16.0,
            EdgeInsets::symmetric(4.0, 8.0), nullptr, IconCode::None, nullptr,
            true, Color::fromRGB(0.0, 0.45, 0.95));
        Harness h;
        h.attach(chip, 300, 60);
        auto* box = h.findRenderObject<RenderDecoratedBox>();
        check(box != nullptr &&
                  box->color() == Color::fromRGB(0.0, 0.45, 0.95),
              "selected 时底色 = selectedColor");
    }
}

// ── 测试 2：InkWell ─────────────────────────────────────────────────────────
void testInkWell() {
    std::printf("[test] InkWell\n");

    // 点击触发 onTap + 水波扩散（真实时间推进到完成）。
    {
        int taps = 0;
        auto ink = cui::make_shared<InkWell>(cui::make_shared<SizedBox>(80.0, 40.0),
                                             [&]() { ++taps; });
        Harness h;
        h.attach(cui::make_shared<Center>(ink), 200, 200);
        auto* rink = h.findRenderObject<RenderInkWell>();
        check(rink != nullptr, "InkWell 生成 RenderInkWell");

        h.dispatch(downAt(100, 100));
        check(rink->splashActive(), "按下启动水波动画");
        check(near(rink->splashCenter().dx, 40.0, 0.5) &&
                  near(rink->splashCenter().dy, 20.0, 0.5),
              "水波圆心 = 按下点（本地坐标）");

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Ticker::tickAllActive(Duration(0));
        check(near(rink->splashT(), 1.0, 1e-3), "400ms 后水波扩散完成 t=1");

        h.dispatch(upAt(100, 100));
        check(taps == 1, "抬起触发 onTap");
    }

    // hover（鼠标）点亮高亮。
    {
        auto ink = cui::make_shared<InkWell>(cui::make_shared<SizedBox>(80.0, 40.0));
        Harness h;
        h.attach(cui::make_shared<Center>(ink), 200, 200);
        auto* rink = h.findRenderObject<RenderInkWell>();
        check(!rink->hovered() && near(rink->hoverT(), 0.0), "初始无 hover");
        h.dispatch(hoverAt(100, 100));
        check(rink->hovered() && rink->hoverT() > 0.0, "鼠标 hover 点亮高亮");
    }

    // 按住拖出边界 → 取消，不触发 onTap。
    {
        int taps = 0, cancels = 0;
        auto ink = cui::make_shared<InkWell>(cui::make_shared<SizedBox>(80.0, 40.0),
                                             [&]() { ++taps; }, nullptr,
                                             [&]() { ++cancels; });
        Harness h;
        h.attach(cui::make_shared<Center>(ink), 200, 200);
        h.dispatch(downAt(100, 100));
        h.dispatch(moveAt(190, 190));  // 远超边界（本地 130 > 80）
        h.dispatch(upAt(190, 190));
        check(cancels == 1, "拖出边界触发 onTapCancel");
        check(taps == 0, "取消后抬起不触发 onTap");
    }
}

// ── 测试 3：Card onTap → InkWell ────────────────────────────────────────────
void testCardInkWell() {
    std::printf("[test] Card onTap\n");

    int taps = 0;
    auto card = cui::make_shared<Card>(cui::make_shared<SizedBox>(80.0, 40.0),
                                       EdgeInsets::all(4.0), Color::fromRGB(1, 1, 1),
                                       4.0, 4.0, [&]() { ++taps; });
    Harness h;
    h.attach(cui::make_shared<Center>(card), 200, 200);
    auto* rink = h.findRenderObject<RenderInkWell>();
    check(rink != nullptr, "onTap 非 null 时 Card 内层包 InkWell");
    h.dispatch(downAt(100, 100));
    h.dispatch(upAt(100, 100));
    check(taps == 1, "Card 点击触发 onTap");

    // 无 onTap 时不含 InkWell。
    auto plain = cui::make_shared<Card>(cui::make_shared<SizedBox>(80.0, 40.0));
    Harness h2;
    h2.attach(cui::make_shared<Center>(plain), 200, 200);
    check(h2.findRenderObject<RenderInkWell>() == nullptr, "无 onTap 不包 InkWell");
}

// ── 测试 4：Slider 标签气泡 ─────────────────────────────────────────────────
void testSliderLabel() {
    std::printf("[test] Slider label\n");

    auto slider = cui::make_shared<Slider>(0.5, nullptr, 0.0, 1.0,
                                           Color::fromRGB(0.0, 0.45, 0.95),
                                           Color::fromRGBA(0, 0, 0, 0.24),
                                           Color::fromRGB(0.0, 0.45, 0.95), 20.0,
                                           10.0, "50");
    Harness h;
    h.attach(cui::make_shared<Center>(slider), 200, 100);
    auto* rs = h.findRenderObject<RenderSlider>();
    check(rs != nullptr && rs->hasLabel(), "label 传入 RenderSlider");
    check(!rs->labelVisible(), "初始气泡不可见");

    // 按下 → 气泡淡入；几何在 thumb 上方、水平居中于 thumb。
    h.dispatch(downAt(100, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Ticker::tickAllActive(Duration(0));
    check(rs->labelVisible(), "按下后气泡可见（淡入）");
    Rect b = rs->bubbleRect();
    check(near(b.left() + b.width / 2.0, rs->thumbCenterX(), 0.5),
          "气泡水平中心 = thumb 中心");
    check(b.bottom() <= rs->size().height / 2.0 - 10.0 + 1e-6,
          "气泡底边在 thumb 上方 8px 之上");
    check(b.width > 16.0, "气泡宽度含文本 + 内边距");

    // 抬起 → 气泡淡出。
    h.dispatch(upAt(100, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    Ticker::tickAllActive(Duration(0));
    check(!rs->labelVisible(), "抬起后气泡淡出");
}

// ── 几何工具：把渲染盒换成全局矩形，供 §5/§6 共用 ─────────────────────────
Rect rectOf(RenderBox* b) {
    const Offset o = b->localToGlobal({0, 0});
    return Rect{o.dx, o.dy, b->size().width, b->size().height};
}
bool insideRect(const Rect& outer, const Rect& inner, double eps = 0.5) {
    return inner.left() >= outer.left() - eps && inner.top() >= outer.top() - eps &&
           inner.right() <= outer.right() + eps && inner.bottom() <= outer.bottom() + eps;
}

/// DFS 收集子树里全部文本段（DFS 序，第一个就是最靠前的那个）。
std::vector<RenderParagraph*> collectParas(RenderObject* root) {
    std::vector<RenderParagraph*> out;
    struct V {
        std::vector<RenderParagraph*>& out;
        void visit(RenderObject* ro) {
            if (!ro)
                return;
            if (dynamic_cast<RenderParagraph*>(ro))
                out.push_back(static_cast<RenderParagraph*>(ro));
            ro->visitChildren([this](AbstractNode* n) {
                visit(static_cast<RenderObject*>(n));
            });
        }
    } v{out};
    v.visit(root);
    return out;
}

// ── 测试 5：MaterialBanner 卡片随内容长高 ─────────────────────────────────
// 回归点：卡片以前用 `height(52)` 塞紧约束，两行内容（消息 + 操作）被顶出卡片
// 外；`EdgeInsets::symmetric` 的形参又是 (horizontal, vertical)，写成 (0, 16)
// 等于「上下 16、左右 0」，把溢出放大成一整行。上游 MaterialBanner 从不锁死整条
// 高度：Column(min) + 操作条 minHeight 52，卡片高度随内容长。
void testMaterialBanner() {
    std::printf("[test] MaterialBanner 内容不得超出卡片\n");

    auto banner = cui::make_shared<MaterialBanner>(
        "有 3 项更改尚未保存，关闭前请先同步。", "!",
        std::vector<MaterialBanner::BannerAction>{{"立即同步", [] {}}, {"稍后", [] {}}},
        [] {});

    // Align 负责松约束：RenderView 给的是窗口大小的紧约束，紧高度会一路穿透，
    // 卡片就永远测不出「随内容长高」这条。
    Harness h;
    h.attach(cui::make_shared<Align>(0.0, -1.0, banner), 600.0, 200.0);

    auto* card = h.findRenderObject<RenderDecoratedBox>();
    check(card != nullptr, "找到卡片渲染盒");
    if (!card)
        return;

    Rect cardRect = rectOf(card);
    check(cardRect.height >= 52.0, "卡片不低于 52 的下限");
    check(cardRect.height > 56.0, "两行内容把卡片撑到 52 以上（不再锁死）");

    auto paras = collectParas(h.rootRO);
    check(paras.size() >= 4, "消息 / 两个操作 / leading 与关闭都在树上");
    if (paras.empty())
        return;
    int outside = 0;
    for (auto* p : paras)
        if (!insideRect(cardRect, rectOf(p)))
            ++outside;
    check(outside == 0, "所有文本段都落在卡片矩形内");
    // DFS 序第一个文本段就是 leading「!」，它左边到卡片左边的距离 = 横向内边距。
    // 写成 `symmetric(0.0, 16.0)`（本仓形参是 horizontal, vertical）时这里是 0，
    // 消息会贴住卡片左边缘 —— 正是这次截图里「!」顶着边框的那个 bug。
    const double leadLeft = rectOf(paras.front()).left();
    check(leadLeft >= cardRect.left() + 16.0 - 1.0, "leading 左侧有 16px 内边距（横向）");

    // 点操作 → 多一行「已执行：…」，卡片还要再长一截且照样包住内容。
    RenderParagraph* action = nullptr;
    for (auto* p : paras)
        if (p->text()->toPlainText() == "立即同步")
            action = p;
    check(action != nullptr, "找到「立即同步」操作");
    if (!action)
        return;

    const Rect aRect = rectOf(action);
    const double ax = (aRect.left() + aRect.right()) / 2.0;
    const double ay = (aRect.top() + aRect.bottom()) / 2.0;
    h.dispatch(downAt(ax, ay));
    h.dispatch(upAt(ax, ay));
    h.pump();

    card = h.findRenderObject<RenderDecoratedBox>();
    const Rect grownRect = rectOf(card);
    bool hasHint = false;
    int stillOutside = 0;
    for (auto* p : collectParas(h.rootRO)) {
        if (p->text()->toPlainText().find("已执行") != std::string::npos)
            hasHint = true;
        if (!insideRect(grownRect, rectOf(p)))
            ++stillOutside;
    }
    check(hasHint, "点击操作后出现「已执行」提示行");
    check(grownRect.height > cardRect.height, "提示行让卡片继续长高");
    check(stillOutside == 0, "三行内容仍然全部在卡片内");
}

/// 状态条每一段文字的垂直中心都应与条体垂直中心对齐（容差内）。
/// `width > 0` 是状态条的真实用法（窗口底部 = 有界宽度）；无界宽度下 Row 的
/// `Expanded` 撑开项拿不到份额，右侧分段会跑到条体外面，那是上游同款退化，
/// 不是本条要钉的东西。
void checkBarCentered(const char* what, Harness& h, double tol) {
    auto* bar = h.findRenderObject<RenderDecoratedBox>();
    check(bar != nullptr, "找到状态条渲染盒");
    if (!bar)
        return;
    const Rect barRect = rectOf(bar);
    auto paras = collectParas(h.rootRO);
    check(paras.size() >= 2, "分段文字都在树上");
    if (paras.empty())
        return;

    int offCenter = 0, outside = 0, squashed = 0;
    for (auto* p : paras) {
        const Rect r = rectOf(p);
        const double mid = (r.top() + r.bottom()) / 2.0;
        const double barMid = (barRect.top() + barRect.bottom()) / 2.0;
        if (std::fabs(mid - barMid) > tol)
            ++offCenter;
        if (!insideRect(barRect, r))
            ++outside;
        // 关键：文字盒被压扁时盒子中心照样对齐，但墨迹是从盒子顶边往下画的 ⇒
        // 视觉上贴到条子上沿（截图就是这个）。所以必须钉「盒子高 >= 自然行高」。
        if (r.height + 0.5 < p->getMaxIntrinsicHeight(r.width))
            ++squashed;
    }
    std::printf("       %s：条 %0.f×%0.f y=%0.f，段数 %zu，偏心 %d 段，出界 %d 段，压扁 %d 段\n",
                what, barRect.width, barRect.height, barRect.top(), paras.size(), offCenter,
                outside, squashed);
    check(offCenter == 0, "所有文字垂直居中于状态条");
    check(squashed == 0, "文字盒未被内边距压扁（墨迹才真的高在中间）");
    check(outside == 0, "所有文字都在状态条矩形内");
    check(rectOf(paras.front()).left() >= barRect.left() + 10.0 - 1.0,
          "左侧文案有 10px 横向内边距");
}

// ── 测试 6：StatusBar 文字垂直居中 ────────────────────────────────────────
// 回归点：`height(22.0)` + `EdgeInsets::symmetric(0.0, 10.0)`。本仓 symmetric 的
// 形参是 (horizontal, vertical)，那条写法实际是「上下各 10」，22 的条高只剩 2px
// 给内容 ⇒ 11px 文字被压到条体上半截（截图里「就绪 / 100% / UTF-8」明显偏上）。
void testStatusBar() {
    std::printf("[test] StatusBar 文字垂直居中\n");

    // ① 固定 22 高、内容居中。
    {
        Harness h;
        h.attach(cui::make_shared<Align>(
                     0.0, -1.0,
                     cui::make_shared<StatusBar>(
                         "就绪",
                         std::vector<std::string>{"100%", "UTF-8", "行 1, 列 1"},
                         std::nullopt, 420.0)),
                 600.0, 200.0);
        checkBarCentered("无状态点", h, 1.5);
    }

    // ② 带状态点：圆点与文字同一条 Row，仍要一起居中。
    {
        Harness h;
        h.attach(cui::make_shared<Align>(
                     0.0, -1.0,
                     cui::make_shared<StatusBar>(
                         "就绪", std::vector<std::string>{"100%"},
                         Color::fromRGB(0.2, 0.7, 0.3), 420.0)),
                 600.0, 200.0);
        checkBarCentered("带状态点", h, 1.5);
    }
}

}  // namespace

int main() {
    testChipExtended();
    testInkWell();
    testCardInkWell();
    testSliderLabel();
    testMaterialBanner();
    testStatusBar();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILURES: %d\n", g_failures);
    return 1;
}
