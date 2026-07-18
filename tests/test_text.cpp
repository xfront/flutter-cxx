/// @file test_text.cpp
/// @brief 上游 富文本 / 继承式 Widget / LayoutBuilder 单元测试。
///
/// 覆盖：
///   1. TextSpan / RichText：多 span 拼接宽度、基线、纯文本
///   2. DefaultTextStyle：子树 Text 继承样式（颜色）
///   3. LayoutBuilder：约束驱动的重建 + 无死循环
///   4. MediaQuery / SafeArea：安全区内边距包 Padding
///
/// 全部 headless（无窗口）。只验证布局 / 尺寸 / 命中（确定性），不验证像素。

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/media.h"
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
        if (auto* hit = dynamic_cast<T*>(ro))
            return hit;
        if (!ro)
            return nullptr;
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

// ── 1. TextSpan / RichText ──────────────────────────────────────────────────
void testRichText() {
    std::printf("[test] RichText / TextSpan\n");

    auto root = cui::make_shared<TextSpan>();
    root->children.push_back(TextSpan::make("Hello ",
        TextStyle{}.size(20).fgColor(colors::blue())));
    root->children.push_back(TextSpan::make("World",
        TextStyle{}.size(24).bold().fgColor(colors::red())));

    Harness h;
    h.attach(cui::make_shared<RichText>(root), 400, 100);

    auto* para = h.findRenderObject<RenderParagraph>();
    check(para != nullptr, "RichText 生成 RenderParagraph");

    if (para) {
        check(para->text()->toPlainText() == "Hello World", "纯文本拼接正确");
        check(para->contentWidth() > 0, "内容宽度 > 0");
        check(para->baselineOffset() > 0, "基线偏移 > 0");

        // 多 run：宽度应近似两 run 之和（各自字号不同）。
        double w1 = Painter::measureTextWidth(nullptr, "Hello ", 20, 400, false);
        double w2 = Painter::measureTextWidth(nullptr, "World", 24, 700, false);
        check(near(para->contentWidth(), w1 + w2, 1.0),
              "拼接宽度 ≈ 两 run 宽度之和");
    }
}

// ── 2. DefaultTextStyle ─────────────────────────────────────────────────────
void testDefaultTextStyle() {
    std::printf("[test] DefaultTextStyle\n");

    TextStyle inherited = TextStyle{}.size(22).fgColor(colors::green());
    auto root = cui::make_shared<DefaultTextStyle>(
        inherited, cui::make_shared<Text>("styled"));

    Harness h;
    h.attach(root, 300, 80);

    auto* para = h.findRenderObject<RenderParagraph>();
    check(para != nullptr, "DefaultTextStyle 子树 Text 生成 RenderParagraph");
    if (para) {
        check(para->text()->style.color == colors::green(),
              "Text 继承 DefaultTextStyle 的颜色");
        check(near(para->text()->style.fontSize, 22.0),
              "Text 继承 DefaultTextStyle 的字号");
    }
}

// ── 3. LayoutBuilder ────────────────────────────────────────────────────────
void testLayoutBuilder() {
    std::printf("[test] LayoutBuilder\n");

    BoxConstraints seen;
    auto builder = [&](BuildContext&, const BoxConstraints& c) -> SharedPtr<Widget> {
        seen = c;
        if (c.maxWidth >= 200.0)
            return cui::make_shared<SizedBox>(100.0, 50.0);
        return cui::make_shared<SizedBox>(50.0, 50.0);
    };

    Harness h;
    h.attach(cui::make_shared<LayoutBuilder>(
                 [builder](BuildContext& c, const BoxConstraints& cons) {
                     return cui::make_shared<Center>(builder(c, cons));
                 }),
             200, 100);

    // 约束应等于 RenderView 的紧约束。
    check(near(seen.minWidth, 200.0) && near(seen.maxWidth, 200.0) &&
          near(seen.minHeight, 100.0) && near(seen.maxHeight, 100.0),
          "传给 builder 的约束 = 父紧约束(200x100)");

    // SizedBox(100,50) 直接挂在紧约束下会被顶成 200×100（上游 enforce
    // 语义）；套 Center 放松约束以验证 builder 选择的子节点尺寸本身。
    auto* inner = h.findRenderObject<RenderConstrainedBox>();
    check(inner != nullptr, "LayoutBuilder 用约束选择正确的子节点");
    if (inner) {
        check(near(inner->size().width, 100.0) && near(inner->size().height, 50.0),
              "宽空间下子节点为 100x50");
    }
}

// ── 4. MediaQuery / SafeArea ────────────────────────────────────────────────
void testMediaQuerySafeArea() {
    std::printf("[test] MediaQuery / SafeArea\n");

    MediaQueryData d;
    d.size = {400, 800};
    d.padding = EdgeInsets{10, 20, 30, 40};  // left, top, right, bottom

    auto root = cui::make_shared<MediaQuery>(
        d, cui::make_shared<SafeArea>(cui::make_shared<SizedBox>(100.0, 100.0)));

    Harness h;
    h.attach(root, 400, 800);

    auto* pad = h.findRenderObject<RenderPadding>();
    check(pad != nullptr, "SafeArea 生成 RenderPadding");

    if (pad && pad->child() && pad->child()->parentData()) {
        const Offset& off = pad->child()->parentData()->offset;
        check(near(off.dx, 10.0) && near(off.dy, 20.0),
              "子节点按安全区 padding 偏移(10,20)");
    }
}

}  // namespace

int main() {
    std::printf("=== test_text ===\n");
    testRichText();
    testDefaultTextStyle();
    testLayoutBuilder();
    testMediaQuerySafeArea();

    if (g_failures == 0) {
        std::printf("\nALL PASS (%d checks)\n", 0);
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
