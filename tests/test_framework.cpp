/// @file test_framework.cpp
/// @brief 上游 架构移植层（cui_core）单元测试。
///
/// 覆盖 上游官方语义的核心断言：
///   1. Element 生命周期（initState → didChangeDependencies → build → dispose）
///   2. State::setState 触发重建
///   3. InheritedWidget 依赖通知（数据变化才通知，依赖者局部重建）
///   4. ParentDataWidget（Expanded/Flexible）写入 FlexParentData
///   5. Flex 布局尺寸与偏移（flex 权重分配、mainAxisAlignment）
///   6. Key 驱动的 reconcile（跨位置重排保持 State）
///
/// 无窗口依赖，全部 headless。运行：./test_framework

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"
#include "cui/widgets/basic.h"

using namespace cui;
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

bool nearly(double a, double b) { return (a > b ? a - b : b - a) < 0.001; }

/// 记录生命周期的 StatefulWidget。
class LifecycleWidget : public StatefulWidget {
public:
    struct Trace {
        int initState = 0;
        int didChangeDependencies = 0;
        int build = 0;
        int didUpdateWidget = 0;
        int dispose = 0;
    };
    static Trace* trace;

    explicit LifecycleWidget(SharedPtr<Widget> child = nullptr) : child_(std::move(child)) {}

    std::unique_ptr<State> createState() override;

    SharedPtr<Widget> child_;
};

LifecycleWidget::Trace* LifecycleWidget::trace = nullptr;

class LifecycleState : public State {
public:
    void initState() override { ++LifecycleWidget::trace->initState; }
    void didChangeDependencies() override { ++LifecycleWidget::trace->didChangeDependencies; }
    void didUpdateWidget(Widget*) override { ++LifecycleWidget::trace->didUpdateWidget; }
    void dispose() override { ++LifecycleWidget::trace->dispose; }

    SharedPtr<Widget> build(BuildContext& context) override {
        ++LifecycleWidget::trace->build;
        return lifecycleWidget()->child_;
    }

private:
    LifecycleWidget* lifecycleWidget() {
        return static_cast<LifecycleWidget*>(widget());
    }
};

std::unique_ptr<State> LifecycleWidget::createState() {
    return std::make_unique<LifecycleState>();
}

/// 可自增的 StatefulWidget。
class CounterState;

class CounterWidget : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
};

class CounterState : public State {
public:
    int counter = 0;
    int buildCount = 0;

    void increment() {
        setState([this]() { counter++; });
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        ++buildCount;
        return cui::make_shared<ColoredBox>(Color::fromRGB(0.2, 0.4, 0.8));
    }
};

std::unique_ptr<State> CounterWidget::createState() {
    return std::make_unique<CounterState>();
}

/// 简单的数据 InheritedWidget。
struct ThemeData {
    int value = 0;
    bool operator==(const ThemeData& o) const { return value == o.value; }
};

class ThemeInherited : public InheritedWidget<ThemeData> {
public:
    explicit ThemeInherited(ThemeData data, SharedPtr<Widget> child = nullptr)
        : InheritedWidget<ThemeData>(nullptr, std::move(child)), data_(data) {}

    SharedPtr<const ThemeData> data() const override {
        return cui::make_shared<ThemeData>(data_);
    }

    ThemeData data_;
};

/// 依赖 ThemeInherited 的无状态 Widget。
class ThemeConsumer : public StatelessWidget {
public:
    explicit ThemeConsumer(SharedPtr<Widget> child = nullptr) : child_(std::move(child)) {}

    int* counter = nullptr;

    SharedPtr<Widget> build(BuildContext& context) override {
        auto* theme = context.dependOnInheritedWidgetOfExactType<ThemeInherited>();
        if (theme && counter)
            (*counter)++;
        return child_;
    }

    SharedPtr<Widget> child_;
};

// ── 测试 1：Element 生命周期 ───────────────────────────────────────────────

void testLifecycle() {
    std::printf("[test] Element 生命周期\n");
    LifecycleWidget::Trace trace;
    LifecycleWidget::trace = &trace;

    BuildOwner owner;
    auto widget = cui::make_shared<LifecycleWidget>();
    auto element = widget->createElement(widget);
    owner.adoptRootElement(std::move(element));

    check(trace.initState == 1, "initState 调用一次");
    check(trace.didChangeDependencies == 1, "didChangeDependencies 在首次 build 前调用一次");
    check(trace.build == 1, "build 调用一次");

    // 重建不应重复触发 initState。
    owner.flushBuildScope();
    check(trace.initState == 1, "重建不重复触发 initState");
}

// ── 测试 2：setState 触发重建 ─────────────────────────────────────────────

void testSetState() {
    std::printf("[test] State::setState 触发重建\n");

    BuildOwner owner;
    auto widget = cui::make_shared<CounterWidget>();
    auto element = widget->createElement(widget);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    auto* state = static_cast<CounterState*>(static_cast<StatefulElement*>(root)->state());
    int buildsBefore = state->buildCount;
    check(buildsBefore == 1, "首次 build 一次");

    state->increment();
    check(owner.hasScheduledBuilds(), "setState 之后存在待重建 Element");

    owner.flushBuildScope();
    check(state->buildCount == buildsBefore + 1, "flush 后发生一次重建");
    check(state->counter == 1, "状态值已更新");
}

// ── 测试 3：InheritedWidget 依赖通知 ──────────────────────────────────────

void testInheritedWidget() {
    std::printf("[test] InheritedWidget 依赖通知\n");

    int consumerBuilds = 0;
    auto consumer = cui::make_shared<ThemeConsumer>();
    consumer->counter = &consumerBuilds;

    auto theme = cui::make_shared<ThemeInherited>(ThemeData{1}, consumer);

    BuildOwner owner;
    auto element = theme->createElement(theme);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    check(consumerBuilds == 1, "依赖者首次 build 一次");

    // 数据不变 → 不应通知依赖者。
    auto sameTheme = cui::make_shared<ThemeInherited>(ThemeData{1}, consumer);
    root->update(sameTheme);
    owner.flushBuildScope();
    check(consumerBuilds == 1, "数据未变化时不通知依赖者");

    // 数据变化 → 通知依赖者，依赖者局部重建。
    auto newTheme = cui::make_shared<ThemeInherited>(ThemeData{2}, consumer);
    root->update(newTheme);
    owner.flushBuildScope();
    check(consumerBuilds == 2, "数据变化时依赖者被重建");
}

// ── 测试 4：ParentDataWidget 写入 FlexParentData ──────────────────────────

void testParentDataWidget() {
    std::printf("[test] ParentDataWidget（Expanded/Flexible）\n");

    // 构造：Column 的子节点中含 Expanded。
    auto leaf = cui::make_shared<ColoredBox>(Color::fromRGB(1, 0, 0));
    auto expanded = cui::make_shared<Expanded>(2, leaf);
    auto column = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{expanded});

    BuildOwner owner;
    auto element = column->createElement(column);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    // 找到 Column 的 RenderObject。
    auto* roElement = static_cast<RenderObjectElement*>(root);
    auto* flex = dynamic_cast<RenderFlex*>(roElement->renderObject());
    check(flex != nullptr, "Column 的 RenderObject 是 RenderFlex");
    if (!flex)
        return;

    check(flex->childCount() == 1, "Expanded 本身不产生 RenderObject，只有其 child 挂入 Flex");
    check(flex->flexOf(0) == 2, "flex 系数经 ParentDataWidget 写入 FlexParentData");
    check(flex->fitOf(0) == FlexFit::Tight, "Expanded 的 fit 为 Tight");
}

// ── 测试 5：Flex 布局 ──────────────────────────────────────────────────────

void testFlexLayout() {
    std::printf("[test] Flex 布局尺寸与偏移\n");

    // Row：两个固定宽度的子节点 + 一个 Expanded。
    auto a = cui::make_shared<SizedBox>(30, 20);
    auto b = cui::make_shared<SizedBox>(30, 20);
    auto c = cui::make_shared<ColoredBox>(Color::fromRGB(0, 1, 0));
    auto expanded = cui::make_shared<Expanded>(1, c);

    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{a, b, expanded});

    BuildOwner owner;
    auto element = row->createElement(row);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    auto* flex = dynamic_cast<RenderFlex*>(static_cast<RenderObjectElement*>(root)->renderObject());
    check(flex != nullptr, "Row 的 RenderObject 是 RenderFlex");
    if (!flex)
        return;

    // 总宽 100：两个 30 固定，Expanded 拿到 40。
    flex->layout(cui::BoxConstraints::tight(100, 50));
    check(nearly(flex->size().width, 100), "Row 占满主轴可用宽度");
    // 上游的 RenderFlex 在交叉轴上最终走 constraints.constrain：
    // 这里传入的是紧约束 tight(100, 50)，因此高度被强制为 50
    // （RenderFlex.performLayout：size = constraints.constrain(...)）。
    check(nearly(flex->size().height, 50), "Row 高度受交叉轴紧约束约束为 50");

    const auto& kids = flex->children();
    check(kids.size() == 3, "Row 有 3 个 RenderObject 子节点");
    if (kids.size() != 3)
        return;

    check(nearly(kids[0]->size().width, 30), "第 1 个子节点保持自然宽度 30");
    check(nearly(kids[1]->size().width, 30), "第 2 个子节点保持自然宽度 30");
    check(nearly(kids[2]->size().width, 40), "Expanded 子节点按 flex 权重获得剩余 40");
    check(nearly(kids[0]->parentData()->offset.dx, 0), "第 1 个子节点偏移为 0");
    check(nearly(kids[1]->parentData()->offset.dx, 30), "第 2 个子节点偏移为 30");
    check(nearly(kids[2]->parentData()->offset.dx, 60), "Expanded 子节点偏移为 60");
}

// ── 测试 6：Key 驱动的 reconcile ──────────────────────────────────────────

class KeyedState;

class KeyedWidget : public StatefulWidget {
public:
    explicit KeyedWidget(KeyPtr key, const std::string& label)
        : StatefulWidget(std::move(key)), label_(label) {}

    std::unique_ptr<State> createState() override;
    std::string label_;
};

class KeyedState : public State {
public:
    int initCount = 0;
    void initState() override { ++initCount; }
    SharedPtr<Widget> build(BuildContext& context) override {
        return cui::make_shared<ColoredBox>(Color::fromRGB(0, 0, 0));
    }
};

std::unique_ptr<State> KeyedWidget::createState() {
    return std::make_unique<KeyedState>();
}

void testKeyedReconcile() {
    std::printf("[test] Key 驱动的 reconcile\n");

    auto keyA = cui::make_shared<ValueKey<int>>(1);
    auto keyB = cui::make_shared<ValueKey<int>>(2);

    auto widgetA = cui::make_shared<KeyedWidget>(keyA, "A");
    auto widgetB = cui::make_shared<KeyedWidget>(keyB, "B");

    auto column = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{widgetA, widgetB});
    BuildOwner owner;
    auto element = column->createElement(column);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    auto* multi = dynamic_cast<MultiChildRenderObjectElement*>(root);
    check(multi != nullptr, "Column 的 Element 是 MultiChildRenderObjectElement");
    if (!multi)
        return;

    check(multi->children().size() == 2, "初始有 2 个子 Element");

    // 拿到 B 对应的 State，稍后验证重排后它仍是同一个 State。
    auto* stateB = static_cast<KeyedState*>(
            static_cast<StatefulElement*>(multi->children()[1].get())->state());
    check(stateB->initCount == 1, "B 首次 initState 一次");

    // 交换顺序：Key 相同 → State 复用（不重新 initState）。
    auto swapped = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{widgetB, widgetA});
    root->update(swapped);
    owner.flushBuildScope();

    auto* stateBAfter = static_cast<KeyedState*>(
            static_cast<StatefulElement*>(multi->children()[0].get())->state());
    check(stateBAfter == stateB, "Key 匹配使 B 的 State 跨位置保留");
    check(stateBAfter->initCount == 1, "重排后不重复 initState");
}

}  // namespace

int main() {
    testLifecycle();
    testSetState();
    testInheritedWidget();
    testParentDataWidget();
    testFlexLayout();
    testKeyedReconcile();

    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
