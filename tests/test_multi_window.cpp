/// @file test_multi_window.cpp
/// @brief 多窗口：Application 持有窗口列表，每个 AppWindow 一条独立流水线。
///
/// 钉住多窗口改造的契约：
///   1. window() 在没有窗口时懒建一个离屏窗口（旧单窗口 API 的兜底），
///      之后稳定返回主窗口；
///   2. createOffscreenWindow 逐个追加，windowCount / windowAt 顺序正确，
///      越界返回 nullptr，第一个创建的是主窗口；
///   3. 每个窗口有自己的 Element 树、RenderView 与 Skia surface：根盒尺寸
///      取各自窗口尺寸，画面像素互不相干；
///   4. 输入按来源窗口派发 —— 打给 A 的按下不会进 B 的命中链；
///   5. closeWindow 关闭后从列表移除（主窗口指针改指向下一个存活窗口）；
///   6. 旧 API（setRoot / renderFrame / windowWidth）落在当前主窗口上。

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/element.h"
#include "cui/foundation/application.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/window.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/skia_renderer.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/framework/listener.h"

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

SharedPtr<Widget> pinned(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

SharedPtr<Widget> colorBox(double w, double h, const Color& color) {
    BoxDecoration d;
    d.color = color;
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(d));
}

RenderBox* rootBox(AppWindow* w) {
    if (!w || !w->rootElement())
        return nullptr;
    return dynamic_cast<RenderBox*>(w->rootElement()->findRenderObject());
}

/// 读回窗口 surface 上一点的 4 个字节（N32 的字节序随平台，测试只比较通道）。
bool samplePixel(AppWindow* w, int x, int y, uint8_t out[4]) {
    if (!w || !w->renderer())
        return false;
    const int rw = w->renderer()->width();
    const int rh = w->renderer()->height();
    if (x < 0 || y < 0 || x >= rw || y >= rh)
        return false;
    std::vector<uint8_t> buf(static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4);
    if (!w->renderer()->readPixels(buf.data(), rw, rh))
        return false;
    const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(rw) +
                      static_cast<size_t>(x)) * 4;
    for (int i = 0; i < 4; ++i)
        out[i] = buf[o + static_cast<size_t>(i)];
    return true;
}

/// 最强的那个颜色通道下标（0/1/2），用于不依赖 BGRA/RGBA 顺序地比较纯色。
int dominantChannel(const uint8_t px[4]) {
    int best = -1;
    int bestV = 0;
    for (int i = 0; i < 3; ++i) {
        if (px[i] > bestV) {
            bestV = px[i];
            best = i;
        }
    }
    return bestV > 200 ? best : -1;
}

void sendPointer(AppWindow* w, EventType type, double x, double y) {
    PlatformEvent ev;
    ev.type = type;
    ev.pos = Point{x, y};
    ev.button = 1;
    w->dispatchInput(ev);
}

}  // namespace

int main() {
    // ── 1. 无窗口时 window() 懒建离屏窗口（旧单窗口 API 的兜底）──────
    {
        Application app;
        check(app.windowCount() == 0, "新 Application 没有窗口");
        AppWindow* w = app.window();
        check(w != nullptr, "window() 懒建一个窗口");
        check(app.windowCount() == 1, "懒建后窗口数为 1");
        check(w->isOffscreen(), "懒建窗口是离屏窗口");
        check(w == app.window(), "window() 稳定返回同一个主窗口");
        check(!w->hasRoot(), "懒建窗口还没有根");
    }

    Application app;

    // ── 2. 追加窗口与枚举 ────────────────────────────────────────────
    AppWindow* a = app.createOffscreenWindow(200, 120);
    AppWindow* b = app.createOffscreenWindow(160, 90);
    check(a != nullptr && b != nullptr, "createOffscreenWindow 建出两个窗口");
    check(app.windowCount() == 2, "windowCount == 2");
    check(app.windowAt(0) == a && app.windowAt(1) == b, "windowAt 按创建顺序");
    check(app.windowAt(2) == nullptr, "越界 windowAt 返回 nullptr");
    check(app.window() == a, "第一个窗口是主窗口");
    check(a->width() == 200 && a->height() == 120, "A 的尺寸 200x120");
    check(b->width() == 160 && b->height() == 90, "B 的尺寸 160x90");
    check(a->isOpen() && b->isOpen(), "两个离屏窗口创建后都处于打开状态");

    // ── 3. 各自的树 / 尺寸 / 画面 ────────────────────────────────────
    auto clicksA = std::make_shared<int>(0);
    auto clicksB = std::make_shared<int>(0);
    auto makeBody = [](std::shared_ptr<int> counter, const Color& color) {
        auto hit = cui::make_shared<Listener>(
                [counter](const PointerEvent&) { ++*counter; },
                RenderPointerListener::PointerCallback{},
                RenderPointerListener::PointerCallback{},
                RenderPointerListener::PointerCallback{},
                RenderPointerListener::PointerCallback{},
                colorBox(50.0, 40.0, color));
        return pinned(std::move(hit));
    };
    const Color kRed(1.0, 0.0, 0.0, 1.0);
    const Color kBlue(0.0, 0.0, 1.0, 1.0);
    a->setRoot(makeBody(clicksA, kRed));
    b->setRoot(makeBody(clicksB, kBlue));
    check(a->hasRoot() && b->hasRoot(), "两个窗口各自挂上了根");
    check(a->rootElement() != b->rootElement(), "两棵树是不同的 Element");

    a->renderFrame(/*forceFull=*/true);
    b->renderFrame(/*forceFull=*/true);

    RenderBox* boxA = rootBox(a);
    RenderBox* boxB = rootBox(b);
    check(boxA != nullptr && boxA->size().width == 200.0 &&
                  boxA->size().height == 120.0,
          "A 的根盒按 A 的窗口尺寸布局（200x120）");
    check(boxB != nullptr && boxB->size().width == 160.0 &&
                  boxB->size().height == 90.0,
          "B 的根盒按 B 的窗口尺寸布局（160x90）");

    uint8_t pxA[4] = {}, pxB[4] = {};
    check(samplePixel(a, 10, 10, pxA), "A 能读回像素");
    check(samplePixel(b, 10, 10, pxB), "B 能读回像素");
    const int chA = dominantChannel(pxA);
    const int chB = dominantChannel(pxB);
    check(chA >= 0 && chB >= 0 && chA != chB,
          "A/B 的左上角分别是两种纯色（各画各的 surface）");

    // ── 4. 输入按来源窗口派发 ────────────────────────────────────────
    sendPointer(a, EventType::MouseDown, 10.0, 10.0);
    sendPointer(a, EventType::MouseUp, 10.0, 10.0);
    check(*clicksA == 1, "A 收到自己的按下");
    check(*clicksB == 0, "打给 A 的输入没有进 B 的命中链");

    sendPointer(b, EventType::MouseDown, 10.0, 10.0);
    sendPointer(b, EventType::MouseUp, 10.0, 10.0);
    check(*clicksB == 1, "B 收到自己的按下");
    check(*clicksA == 1, "A 的计数不受 B 的输入影响");

    // ── 5. 关闭与主窗口切换 ──────────────────────────────────────────
    // closeWindow 在事件派发之外会立刻销毁窗口（指针随之失效），
    // 所以这之后只通过 Application 的枚举接口观察，不再解引用 b。
    app.closeWindow(b);
    b = nullptr;
    check(app.windowCount() == 1, "关闭的窗口从列表移除");
    check(app.windowAt(0) == a && app.windowAt(1) == nullptr, "A 仍在列表首位");

    AppWindow* c = app.createOffscreenWindow(80, 60);
    check(c != nullptr && app.windowCount() == 2, "新建窗口追加到列表");
    check(app.window() == a, "主窗口仍是 A");

    app.closeWindow(a);
    check(app.windowCount() == 1, "关闭主窗口后只剩 C");
    check(app.window() == c, "主窗口指针切到下一个存活窗口");
    check(app.windowWidth() == 80 && app.windowHeight() == 60,
          "windowWidth/Height 跟随主窗口");

    // ── 6. 旧单窗口 API 落在当前主窗口 ──────────────────────────────
    app.setRoot(colorBox(40.0, 30.0, Color(0.2, 0.8, 0.2, 1.0)));
    check(c->hasRoot(), "legacy setRoot 挂到当前主窗口");
    app.renderFrame(/*forceFull=*/true);
    RenderBox* boxC = rootBox(c);
    check(boxC != nullptr && boxC->size().width == 80.0 &&
                  boxC->size().height == 60.0,
          "legacy renderFrame 渲染主窗口");

    // 主窗口上的输入同样只进它自己的树（这里没有监听者，验证不崩且保持可用）
    sendPointer(c, EventType::MouseDown, 5.0, 5.0);
    sendPointer(c, EventType::MouseUp, 5.0, 5.0);
    check(c->hasRoot(), "主窗口收到输入后仍完好");

    if (g_failures == 0)
        std::printf("\nOK (test_multi_window)\n");
    else
        std::printf("\nFAILED: %d checks\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
