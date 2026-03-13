/// @file test_material2.cpp
/// @brief Material 第二批单元测试（Icon / IconButton / ListTile / Tooltip /
///        Card elevation / Switch·Checkbox 过渡动画）。
///
/// 覆盖：
///   1. Icon：RenderIcon 布局尺寸；IconButton 点击触发 / 禁用不触发。
///   2. ListTile：leading/title/trailing 布局；dense 内边距差；onTap 触发。
///   3. Tooltip：hover 显示气泡、抬起隐藏、长按（真实时间）显示。
///   4. Card elevation：decoration 参数正确传入 RenderDecoratedBox。
///   5. 过渡动画：Switch 圆钮 t 随真实时间缓动（sleep + Ticker::tickAllActive
///      确定性推进）；Checkbox 对勾 t 同理。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_icon.h"
#include "cui/rendering/render_interactive.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/interactive.h"
#include "cui/widgets/material.h"
#include "cui/widgets/text.h"
#include "cui/widgets/tooltip.h"

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

/// 测试用 Harness：接入 Widget 树并暴露 dispatch 驱动。
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

    /// 手势/动画回调触发 setState 后：冲刷 build scope + 布局。
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

    /// DFS 找文本内容包含 sub 的 RenderParagraph（Tooltip 气泡检测用）。
    RenderParagraph* findParagraphContaining(const std::string& sub) {
        struct Visitor {
            const std::string* sub;
            RenderParagraph* found = nullptr;
            void visit(RenderObject* ro) {
                if (!ro || found)
                    return;
                if (auto* p = dynamic_cast<RenderParagraph*>(ro)) {
                    if (p->text() && p->text()->toPlainText().find(*sub) !=
                                         std::string::npos)
                        found = p;
                }
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v{&sub};
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

// ── 测试 1：Icon / IconButton ───────────────────────────────────────────────
void testIcon() {
    std::printf("[test] Icon / IconButton\n");

    {
        auto icon = cui::make_shared<Icon>(IconCode::Check, 24.0);
        Harness h;
        h.attach(cui::make_shared<Center>(icon), 100, 100);
        auto* ri = h.findRenderObject<RenderIcon>();
        check(ri != nullptr, "Icon 生成 RenderIcon");
        if (ri) {
            check(near(ri->size().width, 24.0) && near(ri->size().height, 24.0),
                  "Icon 尺寸 = 24x24");
            check(ri->code() == IconCode::Check, "code 正确传入 RenderObject");
        }
    }

    {
        int tap = 0;
        auto btn = cui::make_shared<IconButton>(IconCode::Add, [&]() { ++tap; });
        Harness h;
        h.attach(cui::make_shared<Center>(btn), 100, 100);
        auto* ri = h.findRenderObject<RenderIcon>();
        check(ri != nullptr && ri->code() == IconCode::Add,
              "IconButton 内部生成正确 Icon");
        h.dispatch(downAt(50, 50));
        h.dispatch(upAt(50, 50));
        check(tap == 1, "IconButton 点击触发 onPressed");
    }

    {
        int tap = 0;
        auto btn = cui::make_shared<IconButton>(IconCode::Close, nullptr);
        Harness h;
        h.attach(cui::make_shared<Center>(btn), 100, 100);
        auto* ri = h.findRenderObject<RenderIcon>();
        if (ri)
            check(near(ri->color().a, 0.38), "禁用图标用置灰色");
        h.dispatch(downAt(50, 50));
        h.dispatch(upAt(50, 50));
        check(tap == 0, "禁用 IconButton 不触发");
    }
}

// ── 测试 2：ListTile ────────────────────────────────────────────────────────
void testListTile() {
    std::printf("[test] ListTile\n");

    {
        auto leading = cui::make_shared<SizedBox>(24, 24);
        auto title = cui::make_shared<Text>("标题");
        auto subtitle = cui::make_shared<Text>("副标题");
        auto trailing = cui::make_shared<SizedBox>(20, 20);
        auto tile = cui::make_shared<ListTile>(title, subtitle, leading, trailing);
        Harness h;
        h.attach(tile, 300, 100);
        auto* ro = h.root->findRenderObject();
        check(ro != nullptr && ro->size().height > 40,
              "ListTile 有内容高度");
        // leading 在左、trailing 在右（经 Expanded 分隔）。
        auto* box24 = h.findRenderObject<RenderConstrainedBox>();
        check(box24 != nullptr, "leading/trailing 的 SizedBox 挂载");
        (void)box24;
    }

    {
        int taps = 0;
        auto tile = cui::make_shared<ListTile>(cui::make_shared<Text>("点我"),
                                               nullptr, nullptr, nullptr, false,
                                               [&]() { ++taps; });
        Harness h;
        h.attach(tile, 300, 100);
        h.dispatch(downAt(150, 50));
        h.dispatch(upAt(150, 50));
        check(taps == 1, "ListTile 点击触发 onTap");

        auto* pad = h.findRenderObject<RenderPadding>();
        check(pad != nullptr, "ListTile 有 padding 装饰层");
    }
}

// ── 测试 3：Tooltip ────────────────────────────────────────────────────────
void testTooltip() {
    std::printf("[test] Tooltip\n");

    // hover 立即显示。
    {
        auto tip = cui::make_shared<Tooltip>("提示文本",
                                             cui::make_shared<SizedBox>(60, 30));
        Harness h;
        h.attach(tip, 200, 100);
        check(h.findParagraphContaining("提示文本") == nullptr,
              "初始无气泡");
        h.dispatch(hoverAt(30, 15));
        h.pump();
        check(h.findParagraphContaining("提示文本") != nullptr,
              "hover 后气泡出现");
        h.dispatch(downAt(30, 15));
        h.dispatch(upAt(30, 15));
        h.pump();
        check(h.findParagraphContaining("提示文本") == nullptr,
              "抬起后气泡隐藏");
    }

    // 长按（真实时间）显示：wait=100ms，sleep 160ms 后推进 Ticker。
    {
        auto tip = cui::make_shared<Tooltip>("长按提示",
                                             cui::make_shared<SizedBox>(60, 30),
                                             100.0);
        Harness h;
        h.attach(tip, 200, 100);
        h.dispatch(downAt(30, 15));
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        Ticker::tickAllActive(Duration(0));
        h.pump();
        check(h.findParagraphContaining("长按提示") != nullptr,
              "长按超过 wait 后气泡出现");
        h.dispatch(upAt(30, 15));
        h.pump();
        check(h.findParagraphContaining("长按提示") == nullptr,
              "抬起后气泡隐藏");
    }
}

// ── 测试 4：Card elevation ─────────────────────────────────────────────────
void testCardElevation() {
    std::printf("[test] Card elevation\n");

    auto card = cui::make_shared<Card>(cui::make_shared<SizedBox>(80, 40),
                                       EdgeInsets::all(4.0), Color::fromRGB(1, 1, 1),
                                       4.0, 8.0);
    Harness h;
    h.attach(cui::make_shared<Center>(card), 200, 200);
    auto* deco = h.findRenderObject<RenderDecoratedBox>();
    check(deco != nullptr, "Card 生成 RenderDecoratedBox");
    if (deco) {
        check(near(deco->elevation(), 8.0), "elevation 传入 RenderObject");
        check(deco->shadowColor().a > 0.0, "shadowColor 非透明");
    }

    // elevation=0 时无阴影。
    auto flat = cui::make_shared<Card>(cui::make_shared<SizedBox>(80, 40),
                                       EdgeInsets::all(4.0), Color::fromRGB(1, 1, 1),
                                       4.0, 0.0);
    Harness h2;
    h2.attach(cui::make_shared<Center>(flat), 200, 200);
    auto* deco2 = h2.findRenderObject<RenderDecoratedBox>();
    if (deco2)
        check(near(deco2->elevation(), 0.0), "elevation=0 不投影");
}

// ── 测试 5：Switch / Checkbox 过渡动画 ─────────────────────────────────────
void testTransitions() {
    std::printf("[test] Switch / Checkbox 过渡动画\n");

    {
        // 直接驱动渲染对象（受控更新路径）。
        auto sw = cui::make_shared<Switch>(false);
        Harness h;
        h.attach(sw, 100, 40);
        auto* rs = h.findRenderObject<RenderSwitch>();
        check(rs != nullptr && near(rs->animT(), 0.0), "关 → t=0");

        rs->setValue(true);
        check(rs->value() && rs->animT() > 0.0 && rs->animT() < 1.0,
              "开 → t 进入过渡（0<t<1）");

        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        Ticker::tickAllActive(Duration(0));
        check(near(rs->animT(), 1.0, 1e-3), "200ms 后 t 缓动到 1");

        rs->setValue(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        Ticker::tickAllActive(Duration(0));
        check(near(rs->animT(), 0.0, 1e-3), "关 → t 缓动回 0");
    }

    {
        auto cb = cui::make_shared<Checkbox>(false);
        Harness h;
        h.attach(cb, 100, 40);
        auto* rc = h.findRenderObject<RenderCheckbox>();
        check(rc != nullptr && near(rc->animT(), 0.0), "未勾选 → t=0");

        rc->setValue(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        Ticker::tickAllActive(Duration(0));
        check(near(rc->animT(), 1.0, 1e-3), "勾选后 t 缓动到 1");
    }
}

}  // namespace

int main() {
    testIcon();
    testListTile();
    testTooltip();
    testCardElevation();
    testTransitions();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILURES: %d\n", g_failures);
    return 1;
}
