/// @file test_paint_position.cpp
/// @brief 「绘制位置」回归：推图层换录制画布后，后续兄弟节点的绘制基准。
///
/// 钉住本仓曾经的一个整帧级缺陷：`PaintingContext::paintDirect` 用
/// `canvas_ == canvas` 判断「子树里有没有换过录制画布」。分段录制换画布时，
/// 旧画布随 recorder 析构，新画布常常落在同一个堆地址上 ⇒ 指针比较误判成
/// 「没换」⇒ 该做的 CTM 补偿没有执行，新画布保留着上一段残留的基准偏移。
/// 于是边界之后的兄弟节点被整体画偏（本仓表现为 story 顶栏下拉菜单跑到
/// 页面中间、状态条被画到屏幕外）。
///
/// 修复：用「录制代」计数器（directEpoch_）代替画布指针比较。
///
/// 覆盖：
///   1. 边界（RepaintBoundary）之后的兄弟节点画在自己的位置上，不偏移。
///   2. 偏移量恰好等于边界父节点的绝对位置（复现旧缺陷的漂移量）。
///   3. 边界自身的内容位置正确。

#include <cstdio>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/framework.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"

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

constexpr SkColor kRed = 0xFFFF0000;
constexpr SkColor kGreen = 0xFF00FF00;
constexpr SkColor kWhite = 0xFFFFFFFF;

SharedPtr<Widget> colorBox(double w, double h, SkColor c) {
    BoxDecoration d;
    // SkColor 是 0xAARRGGBB。
    d.color = Color{static_cast<double>((c >> 16) & 0xFF) / 255.0,
                    static_cast<double>((c >> 8) & 0xFF) / 255.0,
                    static_cast<double>(c & 0xFF) / 255.0,
                    static_cast<double>((c >> 24) & 0xFF) / 255.0};
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(d));
}

/// 定位到 (x, y) 的定尺寸色块。
SharedPtr<Widget> positioned(double x, double y, SharedPtr<Widget> child) {
    auto pos = cui::make_shared<Positioned>(std::move(child));
    pos->hasLeft = true;
    pos->left = x;
    pos->hasTop = true;
    pos->top = y;
    return pos;
}

// ── 宿主 ─────────────────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 晚于 rv/po 声明：析构时元素树先散
    SchedulerBinding sb;
    Element* root = nullptr;
    sk_sp<SkSurface> surface;

    void attach(SharedPtr<Widget> w, double ww = 400, double hh = 400) {
        rv.setWindowSize({ww, hh});
        owner.setRootRenderObjectHost(&rv);
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        po.setRootNode(&rv);
        sb.setBuildOwner(&owner);
        sb.setPipelineOwner(&po);
        sb.setRenderView(&rv);
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int)ww, (int)hh));
        surface->getCanvas()->clear(SK_ColorWHITE);
        sb.runPipelineAndComputeDamage();
        sb.compositeTo(surface->getCanvas());
    }

    SkColor at(int x, int y) const {
        uint32_t px = 0;
        surface->readPixels(SkImageInfo::MakeN32Premul(1, 1), &px, 4, x, y);
        return px;
    }
};

// ── 用例 ─────────────────────────────────────────────────────────────────────

/// 边界之后的兄弟节点必须画在自己的位置上。
///
/// 树形（窗口 400×400）：
///   Stack
///     ├─ Positioned(left=200, top=200)
///     │     └─ Container                ← 边界的父节点，绝对位置 (200,200)
///     │          └─ RepaintBoundary
///     │               └─ 40×40 红块      ← 推图层 ⇒ 分段录制换画布，基准 (200,200)
///     └─ Positioned(left=20, top=20)
///          └─ 40×40 绿块                 ← 旧缺陷：被画到 (220,220)
void testSiblingAfterBoundaryKeepsPosition() {
    std::printf("--- 边界之后的兄弟节点位置不能漂移 ---\n");
    Harness h;

    auto boundary = cui::make_shared<RepaintBoundary>(colorBox(40, 40, kRed));
    auto holder = cui::make_shared<Container>(std::move(boundary));
    holder->width(40.0).height(40.0);

    std::vector<SharedPtr<Widget>> layers;
    layers.push_back(positioned(200, 200, std::move(holder)));
    layers.push_back(positioned(20, 20, colorBox(40, 40, kGreen)));
    auto stack = cui::make_shared<Stack>(std::move(layers));
    stack->setFit(RenderStack::Fit::Loose);

    h.attach(stack);

    check(h.at(30, 30) == kGreen, "绿块画在自己的位置 (20,20)");
    check(h.at(210, 210) == kRed, "边界里的红块画在自己的位置 (200,200)");
    // 旧缺陷：绿块被整体画偏 (200,200)（= 边界父节点的绝对位置）。
    check(h.at(220, 220) != kGreen, "绿块没有被画偏到 (220,220)");
    check(h.at(30, 220) != kGreen, "绿块也没有被画偏到 (30,220)");
    check(h.at(120, 120) == kWhite, "漂移落点没有多出一块绿");
}

}  // namespace

int main() {
    std::printf("=== test_paint_position ===\n");
    testSiblingAfterBoundaryKeepsPosition();
    if (g_failures == 0) {
        std::printf("=== ALL PASS ===\n");
        return 0;
    }
    std::printf("=== %d FAILURES ===\n", g_failures);
    return 1;
}
