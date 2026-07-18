/// @file test_material.cpp
/// @brief 上游 Material 装饰/进度组件单元测试。
///
/// 覆盖：
///   1. Divider / VerticalDivider：占位尺寸 + 着色线
///   2. Card：外 margin + 内圆角底
///   3. Chip：圆角底色 + label 文本
///   4. LinearProgressIndicator：value 百分比 / indeterminate
///   5. CircularProgressIndicator：value 百分比 / 尺寸约束
///   6. Material：canvas/card/circle/transparency 的底色、圆角、裁剪与 surfaceTint
///
/// 全部 headless（无窗口）。只验证布局 / 尺寸 / 命中（确定性），不验证像素。

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_material.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
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

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner，并布局好。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    template <typename T>
    T* findRenderObject() {
        RenderObject* ro = root ? root->findRenderObject() : nullptr;
        if (!ro)
            return nullptr;
        if (auto* hit = dynamic_cast<T*>(ro))
            return hit;
        T* found = nullptr;
        std::vector<RenderObject*> stack{ro};
        while (!stack.empty() && !found) {
            RenderObject* n = stack.back();
            stack.pop_back();
            if (auto* hit = dynamic_cast<T*>(n)) {
                found = hit;
                break;
            }
            n->visitChildren([&](AbstractNode* c) {
                if (c)
                    stack.push_back(static_cast<RenderObject*>(c));
            });
        }
        return found;
    }
};

// ── 1. Divider / VerticalDivider ────────────────────────────────────────────
void testDivider() {
    std::printf("[test] Divider / VerticalDivider\n");

    // 水平：占 16 高，内联一条 1px 红线。
    Harness h;
    h.attach(cui::make_shared<Divider>(16.0, 1.0, colors::red()), 200, 20);

    auto* outer = h.findRenderObject<RenderConstrainedBox>();
    check(outer != nullptr, "Divider 生成 RenderConstrainedBox");
    if (outer)
        // 紧约束窗口（200x20）下高度被撑满为 20；Divider 的 height=16 只在
        // 父给出宽松主轴约束时（如 Column 内）才精确生效（与 上游 一致）。
        check(near(outer->size().height, 20.0), "紧约束下高度撑满 = 20");

    auto* box = h.findRenderObject<RenderDecoratedBox>();
    check(box != nullptr, "Divider 内联 RenderDecoratedBox");
    if (box) {
        check(near(box->size().height, 1.0), "线高 = 1");
        check(box->color() == colors::red(), "线色 = 红");
    }

    // 竖直：占 16 宽，内联一条 1px 蓝线。
    Harness h2;
    h2.attach(cui::make_shared<VerticalDivider>(16.0, 1.0, colors::blue()), 20, 200);
    auto* vbox = h2.findRenderObject<RenderDecoratedBox>();
    check(vbox != nullptr, "VerticalDivider 生成 RenderDecoratedBox");
    if (vbox) {
        check(near(vbox->size().width, 1.0), "竖线宽 = 1");
        check(vbox->color() == colors::blue(), "竖线色 = 蓝");
    }
}

// ── 2. Card ─────────────────────────────────────────────────────────────────
void testCard() {
    std::printf("[test] Card\n");

    auto card = cui::make_shared<Card>(cui::make_shared<SizedBox>(80.0, 40.0),
                                       EdgeInsets::all(4.0), colors::white(), 4.0);
    // RenderView 紧约束会一路顶掉子节点指定尺寸（deflate 后仍是紧约束），
    // 套 Center 放松约束以验证 Card 自身的 margin/decoration 语义。
    Harness h;
    h.attach(cui::make_shared<Center>(card), 200, 200);

    auto* pad = h.findRenderObject<RenderPadding>();
    check(pad != nullptr, "Card 外层是 RenderPadding(margin)");
    if (pad && pad->child() && pad->child()->parentData())
        check(near(pad->child()->parentData()->offset.dx, 4.0) &&
                  near(pad->child()->parentData()->offset.dy, 4.0),
              "外边距偏移 = (4,4)");

    auto* box = h.findRenderObject<RenderDecoratedBox>();
    check(box != nullptr, "Card 内层是 RenderDecoratedBox");
    if (box) {
        check(box->color() == colors::white(), "底色 = 白");
        check(near(box->borderRadius(), 4.0), "圆角 = 4");
        check(near(box->size().width, 96.0) && near(box->size().height, 56.0),
              "装饰盒尺寸 = 子节点 80x40 + 内边距 8*2");
    }
}

// ── 3. Chip ─────────────────────────────────────────────────────────────────
void testChip() {
    std::printf("[test] Chip\n");

    auto chip = cui::make_shared<Chip>(cui::make_shared<Text>("hi"),
                                       Color::fromRGBA(0, 0, 0, 0.12), 16.0,
                                       EdgeInsets::symmetric(4.0, 8.0));
    Harness h;
    h.attach(chip, 200, 80);

    auto* box = h.findRenderObject<RenderDecoratedBox>();
    check(box != nullptr, "Chip 生成 RenderDecoratedBox");
    if (box) {
        check(near(box->borderRadius(), 16.0), "圆角 = 16（胶囊）");
        check(box->color() == Color::fromRGBA(0, 0, 0, 0.12), "底色 = 半透明黑");
    }
    auto* para = h.findRenderObject<RenderParagraph>();
    check(para != nullptr, "Chip 子树含 RenderParagraph(label)");
}

// ── 4. LinearProgressIndicator ──────────────────────────────────────────────
void testLinearProgress() {
    std::printf("[test] LinearProgressIndicator\n");

    // 有 value：0.5 → 填满 50%。
    Harness h;
    h.attach(cui::make_shared<LinearProgressIndicator>(std::optional<double>(0.5),
                                                       Color::fromRGBA(0, 0, 0, 0.12),
                                                       colors::blue(), 4.0),
             200, 30);
    auto* p = h.findRenderObject<RenderLinearProgressIndicator>();
    check(p != nullptr, "LinearProgressIndicator 生成 RenderLinearProgressIndicator");
    if (p) {
        check(near(p->value(), 0.5), "value = 0.5");
        check(near(p->minHeight(), 4.0), "minHeight = 4");
        check(near(p->size().width, 200.0), "横向撑满宽 = 200");
        // 紧约束下高度被父约束撑满（200x30 窗口）→ 30；minHeight 是下限，
        // 仅当约束允许更小时才生效（与 上游 一致）。
        check(near(p->size().height, 30.0), "紧约束下高度撑满 = 30");
    }

    // indeterminate（无 value）：value() = -1。
    Harness h2;
    h2.attach(cui::make_shared<LinearProgressIndicator>(std::nullopt), 200, 30);
    auto* p2 = h2.findRenderObject<RenderLinearProgressIndicator>();
    check(p2 != nullptr, "indeterminate 也生成 RenderLinearProgressIndicator");
    if (p2)
        check(near(p2->value(), -1.0), "无 value → value() = -1（indeterminate）");
}

// ── 5. CircularProgressIndicator ────────────────────────────────────────────
void testCircularProgress() {
    std::printf("[test] CircularProgressIndicator\n");

    Harness h;
    h.attach(cui::make_shared<CircularProgressIndicator>(std::optional<double>(0.25),
                                                         Color::fromRGBA(0, 0, 0, 0.12),
                                                         colors::blue(), 4.0),
             100, 100);
    auto* p = h.findRenderObject<RenderCircularProgressIndicator>();
    check(p != nullptr, "CircularProgressIndicator 生成 RenderCircularProgressIndicator");
    if (p) {
        check(near(p->value(), 0.25), "value = 0.25");
        check(near(p->strokeWidth(), 4.0), "strokeWidth = 4");
        check(near(p->size().width, 100.0) && near(p->size().height, 100.0),
              "有界约束下尺寸 = 100x100");
    }

    // 无约束（紧约束下仍取约束尺寸，与 上游 一致）：这里用 50x50 窗口。
    Harness h2;
    h2.attach(cui::make_shared<CircularProgressIndicator>(std::nullopt), 50, 50);
    auto* p2 = h2.findRenderObject<RenderCircularProgressIndicator>();
    check(p2 != nullptr, "indeterminate 也生成 RenderCircularProgressIndicator");
    if (p2) {
        check(near(p2->size().width, 50.0) && near(p2->size().height, 50.0),
              "紧约束下尺寸 = 50x50");
        check(near(p2->value(), -1.0), "无 value → value() = -1");
    }
}

// ── 6. Material ────────────────────────────────────────────────────────────
void testMaterial() {
    std::printf("[test] Material\n");

    // surface tint 查表（上游 elevation_overlay.dart:171-178）。
    check(near(materialSurfaceTintOpacity(0.0), 0.0), "tint opacity @0 = 0");
    check(near(materialSurfaceTintOpacity(1.0), 0.05), "tint opacity @1 = 0.05");
    check(near(materialSurfaceTintOpacity(2.0), 0.065), "tint opacity @2 = 0.065（线性插值）");
    check(near(materialSurfaceTintOpacity(20.0), 0.14), "tint opacity @20 = 0.14（封顶）");

    // canvas：默认底色 = theme().surface、无圆角、不裁剪。
    Harness h;
    h.attach(cui::make_shared<Material>(cui::make_shared<SizedBox>(40.0, 20.0)), 200, 100);
    auto* deco = h.findRenderObject<RenderDecoratedBox>();
    check(deco != nullptr, "Material 生成 RenderDecoratedBox");
    if (deco) {
        check(deco->color() == theme().surface, "canvas 默认色 = theme().surface");
        check(near(deco->borderRadius(), 0.0), "canvas 默认无圆角");
        check(near(deco->size().width, 200.0) && near(deco->size().height, 100.0),
              "紧约束下铺满 200x100");
    }
    check(h.findRenderObject<RenderClipRRect>() == nullptr, "Clip.none 不插裁剪层");

    // card + 显式底色 / 圆角 / clipBehavior。
    Harness h2;
    h2.attach(cui::make_shared<Material>(cui::make_shared<SizedBox>(40.0, 20.0),
                                         MaterialType::Card, colors::white(), 2.0,
                                         Color::fromRGBA(0.0, 0.0, 0.0, 0.3), std::nullopt,
                                         std::optional<double>(12.0), Clip::AntiAlias),
              200, 100);
    auto* clip = h2.findRenderObject<RenderClipRRect>();
    check(clip != nullptr, "clipBehavior != none 时子节点外包 ClipRRect");
    if (clip)
        check(near(clip->borderRadius(), 12.0), "裁剪半径 = 12");
    if (auto* deco2 = h2.findRenderObject<RenderDecoratedBox>()) {
        check(deco2->color() == colors::white(), "显式 color 生效");
        check(near(deco2->borderRadius(), 12.0), "填充半径 = 12");
    }

    // circle：裁剪用 ClipOval，填充按胶囊圆角（正方形盒子上等同圆形）。
    Harness h3;
    h3.attach(cui::make_shared<Material>(cui::make_shared<SizedBox>(20.0, 20.0),
                                         MaterialType::Circle, std::nullopt, 0.0,
                                         Color::fromRGBA(0.0, 0.0, 0.0, 0.35), std::nullopt,
                                         std::nullopt, Clip::HardEdge),
              100, 100);
    check(h3.findRenderObject<RenderClipOval>() != nullptr, "circle 裁剪用 ClipOval");
    if (auto* deco3 = h3.findRenderObject<RenderDecoratedBox>())
        check(near(deco3->borderRadius(), Theme::radiusPill), "circle 填充按胶囊圆角");

    // transparency：不上色、不投影。
    Harness h4;
    h4.attach(cui::make_shared<Material>(cui::make_shared<SizedBox>(40.0, 20.0),
                                         MaterialType::Transparency, std::nullopt, 8.0),
              100, 100);
    if (auto* deco4 = h4.findRenderObject<RenderDecoratedBox>()) {
        check(deco4->color().a == 0.0, "transparency 不填充颜色");
        check(near(deco4->elevation(), 0.0), "transparency 不投影");
    }

    // surfaceTint：M3 的 elevation 叠加色（elevation 6 → 11%）。
    Harness h5;
    h5.attach(cui::make_shared<Material>(cui::make_shared<SizedBox>(40.0, 20.0),
                                         MaterialType::Canvas, colors::white(), 6.0,
                                         Color::fromRGBA(0.0, 0.0, 0.0, 0.35), colors::blue()),
              100, 100);
    if (auto* deco5 = h5.findRenderObject<RenderDecoratedBox>()) {
        const Color expected =
                materialAlphaBlend(Color::fromRGBA(0.0, 0.0, 1.0, 0.11), colors::white());
        check(deco5->color() == expected, "elevation 6 叠加 11% surfaceTint");
    }
}

}  // namespace

int main() {
    std::printf("=== test_material ===\n");
    testDivider();
    testCard();
    testChip();
    testLinearProgress();
    testCircularProgress();
    testMaterial();

    if (g_failures == 0) {
        std::printf("\nALL PASS (%d checks)\n", 0);
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
