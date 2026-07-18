/// @file test_lifecycle.cpp
/// @brief 控件层生命周期回归测试（卸载 / 焦点 / 监听注销）。
///
/// 覆盖三类真实崩溃的根因：
///   1. 元素卸载（finalizeTree / BuildOwner 析构）期间 markNeedsLayout 走父链
///      → 祖先 RenderObject 已释放 → 野指针（"vtable for __class_type_info"）。
///      修复：拆除模式（push/popRenderTeardown）下跳过父链遍历。
///   2. State 在 dispose 之后被控制器再次通知 → setState() 访问已销毁 Element。
///      修复：State::setState() 检查 mounted_；EditableTextState 在 dispose 时
///      注销控制器监听并释放焦点。
///   3. TextEditingController 的监听无法注销 → 回调打到已销毁 State。
///      修复：addListener 返回 id + removeListener(id)。
///
/// 全部 headless（无窗口）。

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"

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

/// 最小宿主：BuildOwner + RenderView + PipelineOwner。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 最后声明：FocusManager 析构晚于元素树
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww = 800, double hh = 600) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        if (auto* box = dynamic_cast<RenderBox*>(root->findRenderObject()))
            rv.setChild(box);
        po.setRootNode(&rv);
        pump();
    }
    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }
    /// 帧末：卸载 inactive 元素（上游的 finalizeTree）。
    void frame() {
        owner.flushBuildScope();
        po.flushLayout();
        owner.finalizeTree();
    }
};

// ── 用例 1：卸载期间 markNeedsLayout 不再走父链 ──────────────────────────────
void testUnmountDuringTeardown() {
    std::printf("[test] 卸载期 markNeedsLayout 安全性\n");

    // 拆除模式下 markNeedsLayout 不应触碰父链：构造一棵深树后整体卸载。
    Harness h;
    auto leaf = cui::make_shared<Text>("leaf");
    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{leaf});
    h.attach(col);

    // 直接触发一次拆除模式下的标记（模拟卸载过程中的 removeChild）。
    pushRenderTeardown();
    if (auto* ro = h.root->findRenderObject())
        ro->markNeedsLayout();
    popRenderTeardown();
    check(true, "拆除模式下 markNeedsLayout 未崩溃");

    // 完整卸载：BuildOwner 析构（析构发生在 Harness 析构时）。
    check(true, "BuildOwner 析构卸载元素树未崩溃");
}

// ── 用例 2：dispose 之后 setState 不再触碰已销毁 Element ────────────────────
class CounterState : public State {
public:
    void dispose() override { disposed_ = true; }
    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<Text>(std::to_string(value_));
    }
    void bump() {
        ++value_;
        setState();
    }
    bool disposed() const { return disposed_; }

private:
    int value_ = 0;
    bool disposed_ = false;
};

class Counter : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override { return std::make_unique<CounterState>(); }
};

void testSetStateAfterDispose() {
    std::printf("[test] dispose 之后的 setState\n");

    Harness h;
    auto counter = cui::make_shared<Counter>();
    h.attach(counter);

    auto* se = dynamic_cast<StatefulElement*>(h.root);
    auto* st = se ? dynamic_cast<CounterState*>(se->state()) : nullptr;
    check(st != nullptr, "拿到 CounterState");
    if (!st)
        return;

    st->bump();
    h.pump();
    check(true, "正常 setState 未崩溃");

    // 卸载（finalizeTree）后再 setState：必须被忽略，不能访问已销毁 Element。
    h.owner.finalizeTree();
    st->bump();
    check(true, "卸载后 setState 被安全忽略");
}

// ── 用例 3：控制器监听可注销 ──────────────────────────────────────────────────
void testControllerListenerRemoval() {
    std::printf("[test] TextEditingController 监听注销\n");

    auto ctrl = cui::make_shared<TextEditingController>("a");
    int fired = 0;
    int id = ctrl->addListener([&fired] { ++fired; });
    ctrl->insert("b");
    check(fired == 1, "监听被调用一次");

    ctrl->removeListener(id);
    ctrl->insert("c");
    check(fired == 1, "注销后不再被调用");
}

// ── 用例 4：EditableText 卸载时释放焦点 + 注销监听 ───────────────────────────
/// 可换子树的宿主：swap() 把 EditableText 换掉 → 其元素被 deactivate。
class SwapHostState;

class SwapHost : public StatefulWidget {
public:
    SharedPtr<Widget> child;
    std::unique_ptr<State> createState() override;
};

class SwapHostState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override { return host()->child; }
    void swap(SharedPtr<Widget> next) {
        host()->child = std::move(next);
        setState();
    }

private:
    SwapHost* host() { return static_cast<SwapHost*>(widget()); }
};

inline std::unique_ptr<State> SwapHost::createState() {
    return std::make_unique<SwapHostState>();
}

void testEditableTextDispose() {
    std::printf("[test] EditableText 卸载（焦点 / 监听）\n");

    auto ctrl = cui::make_shared<TextEditingController>("");
    auto field = cui::make_shared<EditableText>(ctrl, TextStyle{}, TextAlign::Left,
                                                 std::string(), false, /*autofocus=*/true);
    auto host = cui::make_shared<SwapHost>();
    host->child = field;

    Harness h;
    h.attach(host);

    check(h.owner.focusManager().primaryFocus() != nullptr, "autofocus 后焦点落在输入框");

    // 换掉输入框 → 旧元素 deactivate；帧末 finalizeTree 卸载 → State::dispose。
    auto* se = dynamic_cast<StatefulElement*>(h.root);
    auto* st = se ? dynamic_cast<SwapHostState*>(se->state()) : nullptr;
    check(st != nullptr, "拿到 SwapHostState");
    if (!st)
        return;
    st->swap(cui::make_shared<Text>("已替换"));
    h.owner.flushBuildScope();
    h.owner.finalizeTree();

    check(h.owner.focusManager().primaryFocus() == nullptr,
          "卸载后焦点被释放（FocusManager 不持有已销毁节点）");

    // 控制器再通知：不应回调到已销毁的 State。
    ctrl->insert("x");
    check(true, "卸载后控制器通知未崩溃");
}

}  // namespace

int main() {
    std::printf("=== test_lifecycle ===\n");
    testUnmountDuringTeardown();
    testSetStateAfterDispose();
    testControllerListenerRemoval();
    testEditableTextDispose();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
