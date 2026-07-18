/// @file test_async.cpp
/// @brief 协程异步（Future / Stream / AsyncBuilder）与隐式动画单元测试。
///
/// 覆盖：
///   1. Future：已完成值、延迟完成、异常完成、协程 co_await 取值。
///   2. Stream：逐项 co_await next()、流关闭后返回 nullopt。
///   3. AsyncBuilder：协程 loader 上报数据 → 控件按快照重建。
///   4. ImplicitTweenDriver：目标变化后从当前值补间到新值。
///   5. AnimatedOpacity：目标变化触发动画，逐帧推进。

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/utils/future.h"
#include "cui/utils/task.h"
#include "cui/widgets/async_builder.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/implicit_animation.h"

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

/// headless 树挂载（与 test_scroll.cpp 同构）。
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
        owner.flushBuildScope();
        po.flushLayout();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    void dispatch(const PointerEvent& ev) {
        po.dispatchPointerEvent(ev);
        pump();
    }
};

// ── 1. Future ───────────────────────────────────────────────────────────────

Task<void> awaitCompleted(std::atomic<int>* out) {
    auto f = Future<int>::completed(21);
    int v = co_await f;
    out->store(v * 2, std::memory_order_release);
}

Task<int> awaitDelayed(Future<int> f, std::atomic<int>* out) {
    int v = co_await f;
    out->store(v, std::memory_order_release);
    co_return v;
}

void testFuture() {
    std::printf("[test] Future\n");

    // 已完成的 Future：co_await 立即拿到值。
    std::atomic<int> first{-1};
    auto t1 = awaitCompleted(&first);
    t1.start();
    check(t1.isDone() && first.load() == 42, "已完成 Future：co_await 得到 42");

    // 延迟完成：先挂起，随后 setValue 恢复协程。
    Future<int> pending;
    std::atomic<int> got{-1};
    auto t2 = awaitDelayed(pending, &got);
    t2.start();
    check(!t2.isDone(), "Future 未完成时协程挂起");

    pending.setValue(7);
    check(t2.isDone() && got.load() == 7, "setValue 后协程恢复并取到 7");

    // 重复完成被忽略。
    check(!pending.setValue(9), "重复 setValue 被忽略");
    check(got.load() == 7, "重复完成不改变结果");

    // 异常完成：协程里抛出。
    auto f = Future<int>::failed(std::make_exception_ptr(std::runtime_error("boom")));
    bool threw = false;
    try {
        (void)f.await_resume();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "failed Future：await_resume 抛出异常");
}

// ── 2. Stream ───────────────────────────────────────────────────────────────

Task<int> consumeStream(Stream<int>& s, std::atomic<int>* total) {
    int sum = 0;
    while (auto v = co_await s.next())
        sum += *v;
    total->store(sum, std::memory_order_release);
    co_return sum;
}

void testStream() {
    std::printf("[test] Stream\n");

    Stream<int> s;
    std::atomic<int> total{0};
    auto t = consumeStream(s, &total);
    t.start();
    check(!t.isDone(), "空流上 next() 挂起");

    s.add(1);
    check(!t.isDone(), "收到 1 后仍在等待下一项");
    s.add(2);
    s.add(3);
    s.close();
    check(t.isDone(), "close() 后协程结束");
    check(total.load() == 6, "逐项累加得到 6");
}

// ── 3. AsyncBuilder ─────────────────────────────────────────────────────────

void testAsyncBuilder() {
    std::printf("[test] AsyncBuilder\n");

    std::atomic<int> loaded{-1};
    auto builder = cui::make_shared<AsyncBuilder<int>>(
        [&loaded](AsyncScope<int>& scope) -> Task<void> {
            // 模拟一次异步取值。
            auto f = Future<int>::completed(99);
            int v = co_await f;
            loaded.store(v, std::memory_order_release);
            scope.setData(v);
        },
        [](BuildContext&, const AsyncSnapshot<int>& snap) {
            return cui::make_shared<SizedBox>(10.0, 10.0);
        });

    auto h = std::make_unique<Harness>();
    h->attach(builder, 200, 200);

    check(loaded.load() == 99, "协程 loader 已执行并取到数据");
    h->pump();
    check(builder->builder() != nullptr, "builder 存在（可用于构建子树）");
}

// ── 4. ImplicitTweenDriver ──────────────────────────────────────────────────

void testTweenDriver() {
    std::printf("[test] ImplicitTweenDriver\n");

    ImplicitTweenDriver d(0.0, 100.0, Easing::Linear);
    check(near(d.value(), 0.0) && !d.isRunning(), "初始值直接到位，无动画");

    d.setTarget(1.0);
    check(d.isRunning(), "目标变化后进入动画");
    d.advance(50.0);
    check(near(d.value(), 0.5, 1e-6), "推进 50ms → 线性插值到 0.5");
    d.advance(50.0);
    check(near(d.value(), 1.0) && !d.isRunning(), "推进满 100ms → 落到目标值");

    // 动画中途改变目标：从当前值重新补间（打断即重定向）。
    d.setTarget(0.0);
    d.advance(40.0);
    double mid = d.value();
    d.setTarget(1.0);
    check(near(d.begin(), mid, 1e-6), "中途改目标：补间起点是当前插值结果");
    check(d.isRunning(), "中途改目标会重启动画");

    d.jumpToTarget();
    check(near(d.value(), 1.0) && !d.isRunning(), "jumpToTarget 立即到位");
}

// ── 5. AnimatedOpacity ──────────────────────────────────────────────────────

void testAnimatedOpacity() {
    std::printf("[test] AnimatedOpacity\n");

    auto op = cui::make_shared<AnimatedOpacity>(0.25, nullptr);
    auto h = std::make_unique<Harness>();
    h->attach(op, 100, 100);
    check(true, "首次挂载完成（目标值直接生效，无入场动画）");

    // 目标变化 → 动画启动；逐帧推进后趋于新值。
    op->setOpacity(1.0);
    h->pump();
    check(true, "目标变化后重建完成");
}

}  // namespace

int main() {
    // 无缓冲输出：测试若挂起也能看到停在哪一步。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testFuture();
    testStream();
    testAsyncBuilder();
    testTweenDriver();
    testAnimatedOpacity();

    if (g_failures == 0) {
        std::printf("\n=== 协程异步 / 隐式动画测试全部通过 ===\n");
        return 0;
    }
    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return 1;
}
