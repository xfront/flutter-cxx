/// @file test_widgets.cpp
/// @brief 控件层 Widget 单元测试：Text / Opacity / ClipRRect /
///        AspectRatio / FractionallySizedBox / DecoratedBox / Container /
///        IgnorePointer & AbsorbPointer。
///
/// 覆盖：
///   1. Text（RenderParagraph）：单行文本尺寸 = 文本度量（松约束下）
///   2. Opacity（RenderOpacity）：布局透传子节点尺寸，opacity 字段正确
///   3. ClipRRect（RenderClipRRect）：布局透传子节点尺寸，圆角字段正确
///   4. AspectRatio：松约束按 maxWidth 反推高度；紧约束取 smallest（上游语义）
///   5. FractionallySizedBox：按父约束比例缩放
///   6. DecoratedBox：装饰字段正确、有子节点时尺寸 = 子节点尺寸
///   7. Container：width/height 含 padding 不含 margin（上游语义）
///   8. IgnorePointer / AbsorbPointer：命中链屏蔽 / 截断
///   9. 组合：Column{ Text, Opacity(ClipRRect(Text)) } 整树布局无崩溃
///
/// 全部 headless（无窗口）。只验证布局/尺寸/命中（确定性），不验证像素绘制——
/// 真实绘制由生产环境的 FrameScheduler（pump 含 flushPaint）在持有画布时驱动。
///
/// 注意：RenderView 给子节点的是**紧约束**（窗口尺寸），所以凡是要测「自然
/// 尺寸」的用例都要先套一层 Center（RenderAlign 会把约束放松为 0..max）。

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/painter.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
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

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner，并布局好。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 必须最后声明，保证其成员 FocusManager 销毁时仍存活
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    /// 深度优先取树中第一个指定类型的 RenderObject（root 自身优先）。
    template <typename T>
    T* findRenderObject() {
        RenderObject* ro = root ? root->findRenderObject() : nullptr;
        if (auto* hit = dynamic_cast<T*>(ro))
            return hit;
        if (!ro)
            return nullptr;
        T* found = nullptr;
        // 迭代式先序遍历，避免深层树递归。
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

// ── 测试 1：Text 单行尺寸 ───────────────────────────────────────────────────
void testTextLayout() {
    std::printf("[test] Text 单行尺寸\n");

    std::string s = "Hello, Cui!";
    auto t = cui::make_shared<Text>(s, TextStyle{}.size(20).bold());
    Harness h;
    // RenderView 给的是紧约束，套 Center 放松为 0..max 才能测自然尺寸。
    h.attach(cui::make_shared<Center>(t), 1000, 1000);

    RenderParagraph* para = h.findRenderObject<RenderParagraph>();
    check(para != nullptr, "RenderParagraph 已创建");

    // 自然宽度：文本度量；在 0..1000 松约束下不被 clamp。
    double expectW = Painter::measureTextWidth(nullptr, s, 20, 700, false);
    double expectH = Painter::textHeight(nullptr, 20, 700, false);
    check(para->size().width > 0, "文本宽度 > 0");
    check(std::abs(para->size().width - expectW) < 1.0, "宽度 ≈ 文本度量");
    check(std::abs(para->size().height - expectH) < 1.0, "高度 ≈ 文本高度");

    // 空串：宽 0，高仍为非 0 的文本高度。
    auto t2 = cui::make_shared<Text>(std::string(""), TextStyle{}.size(16));
    Harness h2;
    h2.attach(cui::make_shared<Center>(t2), 1000, 1000);
    RenderParagraph* para2 = h2.findRenderObject<RenderParagraph>();
    check(para2 && para2->size().width == 0.0, "空串宽度 = 0");
    check(para2 && para2->size().height > 0, "空串高度 > 0");
}

// ── 测试 2：Opacity 布局透传 ────────────────────────────────────────────────
void testOpacityPassThrough() {
    std::printf("[test] Opacity 布局透传\n");

    auto child = cui::make_shared<SizedBox>(120, 40);
    auto op = cui::make_shared<Opacity>(0.5, child);
    // RenderView 给紧约束（=窗口尺寸），上游 下紧约束会顶掉 SizedBox 的
    // 指定尺寸；套 Center 放松约束以验证「尺寸透传」语义本身。
    Harness h;
    h.attach(cui::make_shared<Center>(op), 500, 500);

    RenderOpacity* ro = h.findRenderObject<RenderOpacity>();
    check(ro != nullptr, "RenderOpacity 已创建");
    check(std::abs(ro->size().width - 120.0) < 1e-9, "尺寸透传 = 子节点宽 120");
    check(std::abs(ro->size().height - 40.0) < 1e-9, "尺寸透传 = 子节点高 40");
    check(std::abs(ro->opacity() - 0.5) < 1e-9, "opacity 字段 = 0.5");
}

// ── 测试 3：ClipRRect 布局透传 ─────────────────────────────────────────────
void testClipRRectPassThrough() {
    std::printf("[test] ClipRRect 布局透传\n");

    auto child = cui::make_shared<SizedBox>(200, 80);
    auto clip = cui::make_shared<ClipRRect>(12.0, child);
    Harness h;
    h.attach(cui::make_shared<Center>(clip), 500, 500);

    RenderClipRRect* rc = h.findRenderObject<RenderClipRRect>();
    check(rc != nullptr, "RenderClipRRect 已创建");
    check(std::abs(rc->size().width - 200.0) < 1e-9, "尺寸透传 = 子节点宽 200");
    check(std::abs(rc->size().height - 80.0) < 1e-9, "尺寸透传 = 子节点高 80");
    check(std::abs(rc->borderRadius() - 12.0) < 1e-9, "圆角字段 = 12");
}

// ── 测试 4：AspectRatio ────────────────────────────────────────────────────
void testAspectRatio() {
    std::printf("[test] AspectRatio\n");

    // 松约束：按 maxWidth 反推高度（ratio = 宽/高 = 2）。
    auto ar = cui::make_shared<AspectRatio>(2.0, cui::make_shared<SizedBox>(10, 10));
    Harness h;
    h.attach(cui::make_shared<Center>(ar), 400, 400);
    RenderAspectRatio* r = h.findRenderObject<RenderAspectRatio>();
    check(r != nullptr, "RenderAspectRatio 已创建");
    check(r && std::abs(r->size().width - 400.0) < 1e-6, "松约束：宽取 maxWidth 400");
    check(r && std::abs(r->size().height - 200.0) < 1e-6, "松约束：高 = 宽 / ratio = 200");

    // 紧约束：上游语义——直接取 constraints.smallest()，不再按 ratio 反推。
    // （外层套 Center 放松约束，否则 RenderView 的紧约束会经 enforce 覆盖 100×60。）
    auto arTight = cui::make_shared<AspectRatio>(
            2.0, cui::make_shared<SizedBox>(10, 10));
    auto box = cui::make_shared<SizedBox>(100, 60, arTight);
    Harness h2;
    h2.attach(cui::make_shared<Center>(box), 800, 800);
    RenderAspectRatio* r2 = h2.findRenderObject<RenderAspectRatio>();
    check(r2 && std::abs(r2->size().width - 100.0) < 1e-6, "紧约束：宽 = 100");
    check(r2 && std::abs(r2->size().height - 60.0) < 1e-6, "紧约束：高 = 60");
}

// ── 测试 5：FractionallySizedBox ───────────────────────────────────────────
void testFractionallySizedBox() {
    std::printf("[test] FractionallySizedBox\n");

    // 无子节点：自身尺寸 = 父约束 × 因子（0..400 × 0.5 = 200）。
    auto f = cui::make_shared<FractionallySizedBox>(0.5, 0.5);
    Harness h;
    h.attach(cui::make_shared<Center>(f), 400, 400);
    RenderFractionallySizedBox* r = h.findRenderObject<RenderFractionallySizedBox>();
    check(r != nullptr, "RenderFractionallySizedBox 已创建");
    check(r && std::abs(r->size().width - 200.0) < 1e-6, "宽 = 父 maxWidth × 0.5 = 200");
    check(r && std::abs(r->size().height - 200.0) < 1e-6, "高 = 父 maxHeight × 0.5 = 200");

    // 只给 widthFactor：子节点拿到缩放后的宽约束，高约束原样透传。
    auto f2 = cui::make_shared<FractionallySizedBox>(0.5, std::nullopt,
                                                     cui::make_shared<SizedBox>(300, 300));
    Harness h2;
    h2.attach(cui::make_shared<Center>(f2), 400, 400);
    RenderFractionallySizedBox* r2 = h2.findRenderObject<RenderFractionallySizedBox>();
    check(r2 && r2->child() && std::abs(r2->child()->constraints().maxWidth - 200.0) < 1e-6,
          "子节点宽约束 = 200（0..400 × 0.5）");
    check(r2 && r2->child() && std::abs(r2->child()->constraints().maxHeight - 400.0) < 1e-6,
          "未设 heightFactor：子节点高约束原样 400");
}

// ── 测试 6：DecoratedBox ───────────────────────────────────────────────────
void testDecoratedBox() {
    std::printf("[test] DecoratedBox\n");

    BoxDecoration dec;
    dec.color = Color::fromRGB(1.0, 0.0, 0.0);
    dec.borderRadius = 8.0;
    dec.borderWidth = 2.0;
    dec.borderColor = Color::fromRGB(0.0, 0.0, 1.0);

    auto d = cui::make_shared<DecoratedBox>(dec, cui::make_shared<SizedBox>(120, 40));
    Harness h;
    h.attach(cui::make_shared<Center>(d), 500, 500);
    RenderDecoratedBox* r = h.findRenderObject<RenderDecoratedBox>();
    check(r != nullptr, "RenderDecoratedBox 已创建");
    check(r && std::abs(r->size().width - 120.0) < 1e-6, "有子节点：宽 = 子节点宽 120");
    check(r && std::abs(r->size().height - 40.0) < 1e-6, "有子节点：高 = 子节点高 40");
    check(r && std::abs(r->borderRadius() - 8.0) < 1e-9, "圆角字段 = 8");
    check(r && std::abs(r->borderWidth() - 2.0) < 1e-9, "边框宽度字段 = 2");
    check(r && r->borderColor().b == 1.0, "边框颜色字段 = 蓝");
}

// ── 测试 7：Container 组合语义 ─────────────────────────────────────────────
void testContainer() {
    std::printf("[test] Container 组合语义\n");

    // A. width/height + margin（无 padding）：100×50 + 外边距 8 = 108×58。
    auto c = cui::make_shared<Container>(cui::make_shared<SizedBox>(100, 50));
    Container& cc = *c;  // 链式配置返回 Container&，用引用调用
    cc.width(100).height(50).margin(EdgeInsets::all(4));

    Harness h;
    // 套 Center 放松约束：RenderView 的紧约束会经 enforce 把 width/height 顶掉。
    h.attach(cui::make_shared<Center>(c), 600, 600);
    RenderPadding* margin = h.findRenderObject<RenderPadding>();
    check(margin != nullptr, "最外层是 Padding(margin)");
    check(margin && std::abs(margin->size().width - 108.0) < 1e-6, "总宽 = 100 + margin 8 = 108");
    check(margin && std::abs(margin->size().height - 58.0) < 1e-6, "总高 = 50 + margin 8 = 58");

    // B. padding 包住子节点：子节点 40×20 + 内边距 16 = 56×36。
    auto c2 = cui::make_shared<Container>(cui::make_shared<SizedBox>(40, 20));
    c2->padding(EdgeInsets::all(8));
    Harness h2;
    h2.attach(cui::make_shared<Center>(c2), 600, 600);
    RenderPadding* pad = h2.findRenderObject<RenderPadding>();
    check(pad && std::abs(pad->size().width - 56.0) < 1e-6, "padding 后宽 = 40 + 16 = 56");
    check(pad && std::abs(pad->size().height - 36.0) < 1e-6, "padding 后高 = 20 + 16 = 36");

    // C. decoration：子树里插入 RenderDecoratedBox，且尺寸 = 子节点尺寸。
    auto c3 = cui::make_shared<Container>(cui::make_shared<SizedBox>(60, 30));
    BoxDecoration dec;
    dec.color = Color::fromRGB(0.0, 0.5, 1.0);
    dec.borderRadius = 6.0;
    c3->decoration(dec);
    Harness h3;
    h3.attach(cui::make_shared<Center>(c3), 600, 600);
    RenderDecoratedBox* deco = h3.findRenderObject<RenderDecoratedBox>();
    check(deco != nullptr, "子树含 RenderDecoratedBox");
    check(deco && std::abs(deco->size().width - 60.0) < 1e-6, "装饰盒宽 = 子节点宽 60");
    check(deco && std::abs(deco->borderRadius() - 6.0) < 1e-9, "装饰圆角 = 6");

    // D. 无子节点：只有约束生效（套 Center 放松紧约束）。
    auto empty = cui::make_shared<Container>();
    empty->width(80).height(30);
    Harness h4;
    h4.attach(cui::make_shared<Center>(empty), 400, 400);
    // Center 本身是 RenderObjectElement（尺寸=窗口），用 DFS 找内层约束盒。
    RenderConstrainedBox* ro = h4.findRenderObject<RenderConstrainedBox>();
    check(ro != nullptr, "空 Container 也能挂载");
    check(ro && std::abs(ro->size().width - 80.0) < 1e-6, "空 Container 宽 = 80");
}

// ── 测试 8：IgnorePointer / AbsorbPointer 命中 ─────────────────────────────
void testPointerBlocking() {
    std::printf("[test] IgnorePointer / AbsorbPointer\n");

    // 可命中的叶子：ColoredBox 的 RenderDecoratedBox hitTestSelf 恒命中。
    auto leaf = cui::make_shared<ColoredBox>(Color::fromRGB(1.0, 1.0, 1.0));
    auto sized = cui::make_shared<SizedBox>(100, 50, leaf);

    auto ignoring = cui::make_shared<IgnorePointer>(true, sized);
    Harness h;
    h.attach(ignoring, 400, 400);
    RenderIgnorePointer* ri = h.findRenderObject<RenderIgnorePointer>();
    check(ri != nullptr, "RenderIgnorePointer 已创建");
    std::vector<RenderObject*> hits;
    bool hit = ri && ri->hitTest(Offset(50, 25), hits);
    check(ri && !hit && hits.empty(), "ignoring：整棵子树不进入命中链");

    // 同一棵树改为不屏蔽 → 命中链非空。
    ri->setIgnoring(false);
    hits.clear();
    bool hit2 = ri->hitTest(Offset(50, 25), hits);
    check(hit2 && !hits.empty(), "不屏蔽：命中链非空");

    // AbsorbPointer：自身留在链上，子节点被截断。
    auto absorbing = cui::make_shared<AbsorbPointer>(
            true, cui::make_shared<SizedBox>(100, 50, leaf));
    Harness h2;
    h2.attach(absorbing, 400, 400);
    RenderIgnorePointer* ra = h2.findRenderObject<RenderIgnorePointer>();
    check(ra != nullptr, "AbsorbPointer → RenderIgnorePointer");
    std::vector<RenderObject*> hits2;
    bool hit3 = ra && ra->hitTest(Offset(50, 25), hits2);
    check(hit3 && hits2.size() == 1 && hits2[0] == ra, "absorbing：只有自身留在命中链");
}

// ── 测试 9：组合树布局（含 SingleChild 协调）──────────────────────────────
void testComposition() {
    std::printf("[test] 组合树布局\n");

    // Column{ Text("A"), Opacity(0.5, ClipRRect(8, Text("B"))) }
    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("A", TextStyle{}.size(16)),
        cui::make_shared<Opacity>(0.5,
            cui::make_shared<ClipRRect>(8.0,
                cui::make_shared<Text>("B", TextStyle{}.size(16)))),
    });
    Harness h;
    h.attach(col, 400, 400);

    RenderFlex* flex = h.findRenderObject<RenderFlex>();
    check(flex != nullptr, "Column → RenderFlex 已创建");
    check(flex->size().width <= 400.0 + 1e-6, "Column 宽不超过约束");
    check(flex->size().height > 0, "Column 高 > 0");

    // 子节点树被正确协调：Opacity / ClipRRect / RenderParagraph 都应存在。
    // 用一个独立的 Text 验证 RenderParagraph 在组合场景下也被正确实例化。
    bool sawParagraph = false, sawOpacity = false, sawClip = false;
    std::function<void(RenderObject*)> walk = [&](RenderObject* n) {
        if (!n) return;
        if (dynamic_cast<RenderParagraph*>(n)) sawParagraph = true;
        if (dynamic_cast<RenderOpacity*>(n)) sawOpacity = true;
        if (dynamic_cast<RenderClipRRect*>(n)) sawClip = true;
        n->visitChildren([&](AbstractNode* c) { walk(static_cast<RenderObject*>(c)); });
    };
    walk(flex);
    check(sawParagraph, "组合树内含 RenderParagraph");
    check(sawOpacity, "组合树内含 RenderOpacity");
    check(sawClip, "组合树内含 RenderClipRRect");
}

}  // namespace

int main() {
    std::printf("=== test_widgets ===\n");
    testTextLayout();
    testOpacityPassThrough();
    testClipRRectPassThrough();
    testAspectRatio();
    testFractionallySizedBox();
    testDecoratedBox();
    testContainer();
    testPointerBlocking();
    testComposition();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
