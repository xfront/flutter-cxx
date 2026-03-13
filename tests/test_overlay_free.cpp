/// @file test_overlay_free.cpp
/// @brief 回归测试：Overlay 条目被移除时，其宿主 Element 必须先从
///        BuildOwner::dirtyElements_ 摘除，否则会作为悬空指针残留在脏列表，
///        下一帧 flushBuildScope 对其 rebuild()→markNeedsBuild() 触发 UAF
///        （崩溃栈顶为 `cui_core::Element::markNeedsBuild`）。
///
/// 复现顺序（与 gallery 等实际使用一致）：
///   1. 插入条目并 pump —— 宿主 OverlayEntryWidget 挂载，hostElement 就绪；
///   2. entry->remove()  —— 调度 Overlay 元素（脏列表：overlay）；
///   3. entry->markNeedsBuild() —— 调度宿主元素（脏列表：overlay, host），
///        此时 host 仍有效（dispose 尚未发生）；
///   4. pump() —— flushBuildScope 先处理 overlay（重建后条目被移除→宿主
///        被卸载并释放），再处理 host：修复前对悬空指针 rebuild()→UAF，
///        修复后 forgetElement 将其从脏列表摘除并在本帧标记跳过。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/overlay.h"

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

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    OverlayState* overlay = nullptr;

    void attach() {
        rv.setWindowSize({400, 400});
        auto overlayWidget = cui::make_shared<Overlay>();
        auto e = overlayWidget->createElement(overlayWidget);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        overlay = static_cast<OverlayState*>(
            static_cast<StatefulElement*>(root)->state());
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }
};

}  // namespace

int main() {
    std::printf("=== test_overlay_free ===\n");

    Harness h;
    h.attach();
    check(h.overlay != nullptr, "Overlay mounted, OverlayState resolved");

    // 1. 插入一个条目并 pump（宿主挂载）。
    auto entry = cui::make_shared<OverlayEntry>(
        [](BuildContext&) { return cui::make_shared<SizedBox>(120.0, 40.0); });
    h.overlay->insert(entry);
    h.pump();
    check(entry->mounted(), "entry mounted after insert");
    check(h.overlay->entries().size() == 1, "overlay has 1 entry");

    // 2+3. 先 remove（调度 overlay 元素），再 markNeedsBuild（调度宿主元素）。
    //     此顺序使脏列表为 [overlay, host]，flush 时 overlay 先重建并释放
    //     host，随后处理 host —— 这正是触发 UAF 的危险顺序。
    entry->remove();
    entry->markNeedsBuild();

    // 4. flush：修复前此处对悬空 host 调 rebuild()→markNeedsBuild() 触发 ASan
    //     UAF（进程非零退出，ctest 失败）；修复后干净通过。
    h.pump();

    check(!entry->mounted(), "entry unmounted after remove+flush");
    check(h.overlay->entries().empty(), "overlay has 0 entries after remove");
    check(h.owner.dirtyCount() == 0, "no stale dirty elements remain");

    if (g_failures == 0)
        std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
