/// @file test_build_scheduling.cpp
/// @brief `BuildOwner::onBuildScheduled` 上升沿契约。
///
/// 上游 `BuildOwner.scheduleBuildFor` 在脏列表从空转入有待办时回调
/// `onBuildScheduled`，`WidgetsBinding` 借此 `ensureVisualUpdate → scheduleFrame`。
/// 本仓缺这条线时 `setState()` 只标记脏、不请求帧 —— 界面要等下一个输入事件
/// （点击 / 滚动）才刷新（Async 页进度条「不点不动」的根因）。
///
/// 覆盖：
///   1. 第一次标脏 ⇒ 回调一次（请求一帧）。
///   2. 同一批内再标脏别的 Element ⇒ 不再回调（合并成一帧）。
///   3. flushBuildScope 之后 ⇒ 重新走上升沿。
///   4. 构建期 setState ⇒ 不回调（并入同一批），但元素在同一批里被重建。
///   5. 没接回调时 setState/flush 一切照常（默认空回调）。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok)
        std::printf("  [PASS] %s\n", what.c_str());
    else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

class ProbeState;

/// 极简探针：记录 build 次数；可选在 build 里给兄弟再标一次脏。
class Probe : public StatefulWidget {
public:
    explicit Probe(int index) : index_(index) {}
    std::unique_ptr<State> createState() override;
    int index() const { return index_; }

private:
    int index_;
};

class ProbeState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext&) override;

    void bump() { setState(); }

    int builds = 0;
    bool triggerSiblingOnBuild = false;
};

ProbeState* g_probes[2] = {nullptr, nullptr};

inline std::unique_ptr<State> Probe::createState() {
    return std::make_unique<ProbeState>();
}

void ProbeState::initState() {
    auto* p = static_cast<Probe*>(widget());
    g_probes[p->index()] = this;
}

void ProbeState::dispose() {
    auto* p = static_cast<Probe*>(widget());
    if (g_probes[p->index()] == this)
        g_probes[p->index()] = nullptr;
    State::dispose();
}

SharedPtr<Widget> ProbeState::build(BuildContext&) {
    ++builds;
    auto* p = static_cast<Probe*>(widget());
    if (p->index() == 0 && triggerSiblingOnBuild) {
        triggerSiblingOnBuild = false;
        if (g_probes[1])
            g_probes[1]->bump();  // 构建期给兄弟标脏
    }
    return cui::make_shared<SizedBox>(20.0, 10.0);
}

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;

    void attach(SharedPtr<Widget> w) {
        rv.setWindowSize({300, 300});
        owner.setRootRenderObjectHost(&rv);
        auto e = w->createElement(w);
        owner.adoptRootElement(std::move(e));
        po.setRootNode(&rv);
    }
};

SharedPtr<Widget> probeRow() {
    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(cui::make_shared<Probe>(0));
    kids.push_back(cui::make_shared<Probe>(1));
    return cui::make_shared<Column>(std::move(kids));
}

void testRisingEdge() {
    std::printf("--- 上升沿：标脏即请求一帧，同批合并，flush 后复位 ---\n");
    Harness h;
    h.attach(probeRow());
    ProbeState* a = g_probes[0];
    ProbeState* b = g_probes[1];
    check(a != nullptr && b != nullptr, "两个探针都已挂载");
    if (!a || !b)
        return;

    int scheduled = 0;
    h.owner.onBuildScheduled = [&scheduled] { ++scheduled; };

    a->bump();
    check(scheduled == 1, "首次标脏回调一次");
    check(h.owner.hasScheduledBuilds(), "脏列表里已有待办");

    b->bump();
    check(scheduled == 1, "同一批再标脏不再回调（合并为一帧）");

    const int aBefore = a->builds;
    const int bBefore = b->builds;
    h.owner.flushBuildScope();
    check(a->builds == aBefore + 1, "A 被重建");
    check(b->builds == bBefore + 1, "B 被重建");
    check(!h.owner.hasScheduledBuilds(), "flush 后脏列表空");

    a->bump();
    check(scheduled == 2, "flush 之后再标脏重新走上升沿");
    h.owner.flushBuildScope();
}

void testMarkDuringBuild() {
    std::printf("--- 构建期标脏：并入同一批，不额外请求帧 ---\n");
    Harness h;
    h.attach(probeRow());
    ProbeState* a = g_probes[0];
    ProbeState* b = g_probes[1];
    if (!a || !b)
        return;

    int scheduled = 0;
    h.owner.onBuildScheduled = [&scheduled] { ++scheduled; };

    a->triggerSiblingOnBuild = true;
    a->bump();
    check(scheduled == 1, "A 标脏请求一帧");

    const int bBefore = b->builds;
    h.owner.flushBuildScope();
    check(scheduled == 1, "A 构建期间标脏 B 不再请求帧");
    check(b->builds == bBefore + 1, "B 在同一批里被重建");
    check(!h.owner.hasScheduledBuilds(), "同一批处理完脏列表为空");
}

void testNoCallback() {
    std::printf("--- 未接回调：setState/flush 照常 ---\n");
    Harness h;
    h.attach(probeRow());
    ProbeState* a = g_probes[0];
    if (!a)
        return;

    const int before = a->builds;
    a->bump();
    h.owner.flushBuildScope();
    check(a->builds == before + 1, "没有 onBuildScheduled 也能正常重建");
    check(!h.owner.hasScheduledBuilds(), "脏列表清空");
}

}  // namespace

int main() {
    std::printf("=== test_build_scheduling ===\n");
    testRisingEdge();
    testMarkDuringBuild();
    testNoCallback();
    if (g_failures == 0) {
        std::printf("test_build_scheduling: 全部通过\n");
        return 0;
    }
    std::printf("test_build_scheduling: %d 项失败\n", g_failures);
    return 1;
}
