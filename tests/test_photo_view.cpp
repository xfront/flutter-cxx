/// @file test_photo_view.cpp
/// @brief PhotoView / PhotoViewGallery / 滑出关闭页单元测试。对齐 pub.dev
///        `photo_view`（PhotoView + PhotoViewCore + PhotoViewController +
///        PhotoViewScaleStateController + ScaleBoundaries + HitCornersDetector）
///        与 `extended_image` 的 ExtendedImageSlidePage 语义。
///
/// 覆盖：
///   1. 纯函数：contained/covering 尺寸公式、ScaleBoundaries 的三条夹取口径、
///      档位换档循环、`getScaleForScaleState`、滑出比例。
///   2. 控制器契约：setValue/prevValue、invisibly 的 internalWrite 闸门、
///      updateMultiple 只通知一次、reset、等值早退。
///   3. 挂载：LayoutBuilder 取外框、图片自然尺寸（同步送达）、树形（Transform +
///      Center + Align + 一层 ClipRect 都没有）、初始 scale = contained。
///   4. 双击换档：Initial→Covering→OriginalSize→Initial、定长补间逐帧插值、
///      「档位换了但尺寸没变」跳档、双击吞掉第二记 onTapUp。
///   5. 平移：`clampPosition` 的 cornersX/Y、内容比外框小的轴归零、夹到边界后
///      剩余拖拽让给祖先（文件头第 9 条）。
///   6. 捏合与旋转：跨度比 ⇒ 缩放、`enableRotation` 的角度增量、单指降级成平移。
///   7. 滚轮：以光标为锚的精确锚定（场景点不变）、strictScale 整帧不动且不消费、
///      max/min 夹取、缩到 min 之后让给祖先。
///   8. 回调节奏与 disableGestures（树上不再挂 Listener）。
///   9. customChild / tightMode / heroTag / 失败态整体替换 / 异步出图补尺寸。
///  10. 画廊：页窗口 [page-1, page+1]、setPage、拖拽翻页 + 吸附 + onPageChanged、
///      每页控制器保活、滚轮攒满一整页才翻、撞到首末页逃逸。
///  11. 画廊：垂直主轴、reverse 镜像、像素级换页。
///  12. 滑出关闭页：背景淡出、内容缩放、offsetHandler、复位补间、越阈值经
///      Navigator 关页、enableSlideOutPage=false 时不挂 Listener。
///  13. 监听摘除（铁律）：外部 controller 挂载 2 条 / 状态控制器 1 条，卸载归零。
///
/// headless 帧泵；补间一律用 `advanceAnimation/advanceSettle/advanceReset` 直接
/// 推（Ticker 走真实时钟）。唯一真正 sleep 的地方是 `Harness::doubleTap`：双击是
/// 时序判定，成双的一对应从上一记单点之后重新开始算。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/image_provider.h"
#include "cui/foundation/key.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/framework/listener.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/hero.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/pages.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/photo_view.h"

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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
bool near(const Offset& a, const Offset& b, double eps = 1e-6) {
    return near(a.dx, b.dx, eps) && near(a.dy, b.dy, eps);
}

const SkColor kWhite = SK_ColorWHITE;

// ── 素材 ────────────────────────────────────────────────────────────────────

/// 左半红、右半蓝的 PNG。
sk_sp<SkData> makePng(int w, int h) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeLTRB(0.0f, 0.0f, float(w / 2), float(h)), paint);
    paint.setColor(SK_ColorBLUE);
    canvas->drawRect(SkRect::MakeLTRB(float(w / 2), 0.0f, float(w), float(h)), paint);
    return encodePng(surface->makeImageSnapshot());
}

std::vector<uint8_t> toBytes(const sk_sp<SkData>& data) {
    const uint8_t* p = static_cast<const uint8_t*>(data->data());
    return std::vector<uint8_t>(p, p + data->size());
}

/// 纯色块：SizedBox 定尺寸 + DecoratedBox 上色（DecoratedBox 认命中）。
SharedPtr<Widget> block(double w, double h, const Color& color) {
    BoxDecoration dec;
    dec.color = color;
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(dec));
}

const Color kCyan(0.0, 0.6, 0.9, 1.0);
const Color kLime(0.4, 0.9, 0.2, 1.0);
const Color kPink(0.9, 0.3, 0.5, 1.0);

bool isCyanish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), g = uint8_t((c >> 8) & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return b > 150 && g > 120 && r < 120;
}
bool isLimish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), g = uint8_t((c >> 8) & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return g > 150 && r < 150 && b < 120;
}
bool isPinkish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), g = uint8_t((c >> 8) & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return r > 150 && b > 90 && g < 120;
}

// ── 线程接缝（异步出图那一段用）─────────────────────────────────────────────

std::vector<ImageTask>& bgQueue() {
    static std::vector<ImageTask> q;
    return q;
}
std::vector<ImageTask>& uiQueue() {
    static std::vector<ImageTask> q;
    return q;
}
void installSeams(bool bgInline, bool uiInline) {
    if (bgInline)
        setBackgroundExecutor([](ImageTask t) { t(); });
    else
        setBackgroundExecutor([](ImageTask t) { bgQueue().push_back(std::move(t)); });
    if (uiInline)
        setUiDispatcher([](ImageTask t) { t(); });
    else
        setUiDispatcher([](ImageTask t) { uiQueue().push_back(std::move(t)); });
}
void runBg() {
    std::vector<ImageTask> tasks;
    tasks.swap(bgQueue());
    for (ImageTask& t : tasks)
        t();
}
void runUi() {
    std::vector<ImageTask> tasks;
    tasks.swap(uiQueue());
    for (ImageTask& t : tasks)
        t();
}
void resetSeams() {
    setBackgroundExecutor({});
    setUiDispatcher({});
    bgQueue().clear();
    uiQueue().clear();
}

// ── 宿主探针：根恒为 Align，body 可换 ────────────────────────────────────────

class Host : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Host"; }
};

class HostState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<Align>(-1.0, -1.0, body_);
    }
    void setBody(SharedPtr<Widget> body) {
        body_ = std::move(body);
        setState();
    }

private:
    SharedPtr<Widget> body_;
};

inline std::unique_ptr<State> Host::createState() { return std::make_unique<HostState>(); }

State* findStateNamed(Element* e, const char* tag) {
    if (!e)
        return nullptr;
    if (auto* se = dynamic_cast<StatefulElement*>(e)) {
        if (se->widget() && se->widget()->toStringShort() == tag)
            return se->state();
    }
    State* found = nullptr;
    e->visitChildren([&](Element* child) {
        if (!found)
            found = findStateNamed(child, tag);
    });
    return found;
}

template <typename W>
W* findWidget(Element* e) {
    if (!e)
        return nullptr;
    if (auto* w = dynamic_cast<W*>(e->widget()))
        return w;
    W* found = nullptr;
    e->visitChildren([&](Element* child) {
        if (!found)
            found = findWidget<W>(child);
    });
    return found;
}

/// 沿渲染链统计某个渲染类型出现次数（验 Listener / ClipRect / Transform 插没插）。
template <typename T>
int countRenderObjects(RenderObject* node) {
    if (!node)
        return 0;
    int found = dynamic_cast<T*>(node) ? 1 : 0;
    node->visitChildren([&](AbstractNode* child) {
        found += countRenderObjects<T>(static_cast<RenderObject*>(child));
    });
    return found;
}

/// 顺渲染链第一个某类型的节点（拿背景盒的真实尺寸）。
template <typename T>
T* firstRenderObject(RenderObject* node) {
    if (!node)
        return nullptr;
    if (auto* hit = dynamic_cast<T*>(node))
        return hit;
    T* found = nullptr;
    node->visitChildren([&](AbstractNode* child) {
        if (!found)
            found = firstRenderObject<T>(static_cast<RenderObject*>(child));
    });
    return found;
}

/// 画廊摆在窗口里的页：收集「四边都定死的 Positioned」的 ValueKey<int>。
void collectPageKeys(Element* e, std::vector<int>& out) {
    if (!e)
        return;
    if (auto* pos = dynamic_cast<Positioned*>(e->widget())) {
        if (pos->hasLeft && pos->hasTop && pos->hasWidth && pos->hasHeight) {
            if (auto* vk = dynamic_cast<const ValueKey<int>*>(pos->key().get()))
                out.push_back(vk->value());
        }
    }
    e->visitChildren([&](Element* child) { collectPageKeys(child, out); });
}

// ── 祖先探针：数「让给父级」的事件 ──────────────────────────────────────────

struct Probe {
    int downs = 0, moves = 0, ups = 0, scrolls = 0;
    SharedPtr<Widget> wrap(SharedPtr<Widget> inner) {
        return cui::make_shared<Listener>(
            [this](const PointerEvent&) { ++downs; },
            [this](const PointerEvent&) { ++moves; },
            [this](const PointerEvent&) { ++ups; }, Listener::PointerCallback{},
            [this](const PointerEvent&) { ++scrolls; }, std::move(inner));
    }
};

// ── 帧泵 + 指针注入 ─────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    SchedulerBinding sb;
    Element* root = nullptr;
    HostState* host = nullptr;
    sk_sp<SkSurface> surface;

    void attach(SharedPtr<Widget> body, double ww = 400, double hh = 300) {
        rv.setWindowSize({ww, hh});
        owner.setRootRenderObjectHost(&rv);
        auto w = cui::make_shared<Host>();
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        po.setRootNode(&rv);
        sb.setBuildOwner(&owner);
        sb.setPipelineOwner(&po);
        sb.setRenderView(&rv);
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(int(ww), int(hh)));
        pump();
        host = static_cast<HostState*>(findStateNamed(root, "Host"));
        host->setBody(std::move(body));
        // PhotoView / SlidePage / 画廊都是 LayoutBuilder 打头：子树在布局期才建，
        // 同步出图的 onReady 又会在其中一次构建里 setState ⇒ 多泵几帧才到稳态。
        settle();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
    }
    void settle(int frames = 3) {
        for (int i = 0; i < frames; ++i)
            pump();
    }

    void frame() {
        sb.runPipelineAndComputeDamage();
        surface->getCanvas()->clear(kWhite);
        sb.compositeTo(surface->getCanvas());
    }

    void setBody(SharedPtr<Widget> body) {
        host->setBody(std::move(body));
        settle();
    }

    PhotoViewState* view() const {
        return dynamic_cast<PhotoViewState*>(findStateNamed(root, "PhotoView"));
    }
    PhotoViewGalleryState* gallery() const {
        return dynamic_cast<PhotoViewGalleryState*>(findStateNamed(root, "PhotoViewGallery"));
    }
    PhotoViewSlidePageState* slide() const {
        return dynamic_cast<PhotoViewSlidePageState*>(findStateNamed(root, "PhotoViewSlidePage"));
    }

    // 指针事件：position 给全局点，本地坐标由分发器按节点变换。
    void down(int64_t id, double x, double y) {
        PointerDownEvent d;
        d.pointerId = id;
        d.position = {x, y};
        d.globalPosition = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        pump();
    }
    void move(int64_t id, double x, double y) {
        PointerMoveEvent m;
        m.pointerId = id;
        m.position = {x, y};
        m.globalPosition = {x, y};
        m.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(m);
        pump();
    }
    void up(int64_t id, double x, double y) {
        PointerUpEvent u;
        u.pointerId = id;
        u.position = {x, y};
        u.globalPosition = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }
    void tap(double x, double y) {
        down(1, x, y);
        up(1, x, y);
    }
    /// 两记一组的「双击」。成双后那一对整体作废（文件头第 2 条），但作废靠的是
    /// 「第二记吃掉记录」，不是时间 —— 所以这里先睡过 300ms 的双击窗口，让前面
    /// 可能留下的单点不参与配对，这一对的第二记才是唯一一次换档触发。
    void doubleTap(double x, double y) {
        std::this_thread::sleep_for(std::chrono::milliseconds(320));
        tap(x, y);
        tap(x, y);
    }
    void scroll(double x, double y, double dy, double dx = 0.0) {
        PointerScrollEvent s;
        s.pointerId = 9;
        s.position = {x, y};
        s.globalPosition = {x, y};
        s.scrollDeltaY = dy;
        s.scrollDeltaX = dx;
        po.dispatchPointerEvent(s);
        pump();
    }

    uint32_t at(int x, int y) const {
        const SkImageInfo info = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        uint32_t px = 0;
        surface->readPixels(info, &px, 4, x, y);
        return px;
    }
};

// ── PhotoView 配方（每帧新建实例才能走 didUpdateWidget 的更新路径）───────────

struct PVArgs {
    SharedPtr<Widget> customChild;
    std::shared_ptr<ImageProvider> provider;
    std::optional<Size> childSize;
    std::optional<PhotoViewScale> minScale, maxScale, initialScale;
    std::optional<Size> customSize;
    std::optional<Alignment> basePosition;
    PhotoViewControllerPtr controller;
    PhotoViewScaleStateControllerPtr scaleController;
    PhotoViewScaleStateCycle scaleStateCycle;
    bool enableRotation = false;
    bool tightMode = false;
    bool disableGestures = false;
    bool enablePanAlways = false;
    bool strictScale = false;
    bool animate = true;
    double scaleFactor = 200.0;
    double animationMs = 250.0;
    std::string heroTag;
    PhotoViewTapCallback onTapUp, onTapDown;
    PhotoViewScaleEndCallback onScaleEnd;
    PhotoViewScaleStateChangedCallback onScaleState;
    ExtendedImage::LoadingBuilder loadingBuilder;
    ExtendedImage::ErrorBuilder errorBuilder;
};

SharedPtr<PhotoView> mkPhoto(const PVArgs& a) {
    return cui::make_shared<PhotoView>(
        a.customChild, a.provider, a.childSize, a.minScale, a.maxScale, a.initialScale,
        a.customSize, a.basePosition, photoViewDefaultBackground(), a.controller,
        a.scaleController, a.scaleStateCycle, a.onTapUp, a.onTapDown, a.onScaleEnd, a.onScaleState,
        a.loadingBuilder, a.errorBuilder, a.enableRotation, a.tightMode, a.disableGestures,
        a.enablePanAlways, a.strictScale, false, a.animate, a.scaleFactor, a.animationMs,
        Easing::EaseOutCubic, a.heroTag, nullptr);
}

/// 200×150 的定尺盒子（外面套在 body 里，给 PhotoView 一个确定的外框）。
SharedPtr<Widget> pin(double w, double h, SharedPtr<Widget> inner) {
    return cui::make_shared<SizedBox>(w, h, std::move(inner));
}

DecodedImagePtr png200x100() {
    static DecodedImagePtr img = decodeImage(makePng(200, 100));
    return img;
}

}  // namespace

// ── 1. 纯函数：边界与档位 ────────────────────────────────────────────────────

void testBoundariesMath() {
    std::printf("[test] 1. 尺寸公式 / ScaleBoundaries / 换档循环\n");

    const Size outer(400.0, 300.0), child(200.0, 100.0);
    check(near(containedScaleFor(outer, child), 2.0), "1: contained = min(400/200, 300/100)");
    check(near(coveringScaleFor(outer, child), 3.0), "1: covering = max(两个比值)");
    check(near(containedScaleFor(outer, Size(0.0, 0.0)), 1.0) &&
              near(coveringScaleFor(outer, Size(0.0, 0.0)), 1.0),
          "1: 空尺寸退化成 1.0（上游同口径）");

    // 缺省：min 0 / max ∞ / initial = contained。
    const PhotoViewScaleBoundaries d = computePhotoViewScaleBoundaries(outer, child);
    check(near(d.minScale, 0.0) && d.maxScale == BoxConstraints::kInfinity &&
              near(d.initialScale, 2.0),
          "1: 缺省 boundaries = (0, ∞, contained)");

    // min 负值夹成 0（上游是 assert，这里照注释口径夹）。
    const PhotoViewScaleBoundaries neg = computePhotoViewScaleBoundaries(
        outer, child, PhotoViewScale::fixed(-2.0), std::nullopt, std::nullopt);
    check(near(neg.minScale, 0.0), "1: 负 minScale 夹成 0");

    // max 一定 ≥ min：max 写得比 min 小 ⇒ 抬到 min。
    const PhotoViewScaleBoundaries flip = computePhotoViewScaleBoundaries(
        outer, child, PhotoViewScale::fixed(1.0), PhotoViewScale::fixed(0.5), std::nullopt);
    check(near(flip.minScale, 1.0) && near(flip.maxScale, 1.0), "1: max < min ⇒ max 抬到 min");

    // initial 为固定档 ⇒ 夹进 [min,max]；为计算档 ⇒ 不夹。
    const PhotoViewScaleBoundaries fixedHi = computePhotoViewScaleBoundaries(
        outer, child, std::nullopt, PhotoViewScale::fixed(3.0), PhotoViewScale::fixed(9.0));
    check(near(fixedHi.initialScale, 3.0), "1: 固定 initial 越上界 ⇒ 夹到 max");
    const PhotoViewScaleBoundaries calcHi =
        computePhotoViewScaleBoundaries(outer, child, std::nullopt, PhotoViewScale::fixed(3.0),
                                        PhotoViewScale::contained(2.0));
    check(near(calcHi.initialScale, 4.0), "1: 计算档 initial（contained×2=4）不被 max=3 夹住");

    // 倍数：`contained() * 1.5` 与 `contained(1.5)` 等价；== 连倍数一起比。
    check(near((PhotoViewScale::contained() * 1.5).resolve(2.0, 3.0), 3.0),
          "1: operator* 把倍数乘进去");
    check(PhotoViewScale::contained(1.0) != PhotoViewScale::contained(2.0),
          "1: 同基但不同倍数 ⇒ 不相等（本仓刻意，见头注释）");
    check(near(PhotoViewScale::covered().resolve(2.0, 3.0), 3.0), "1: covered 走 covering");

    // 档位读数。
    check(near(getScaleForPhotoViewScaleState(PhotoViewScaleState::Initial, d), 2.0),
          "1: Initial ⇒ clamp(initialScale)");
    check(near(getScaleForPhotoViewScaleState(PhotoViewScaleState::Covering, d), 3.0),
          "1: Covering ⇒ clamp(covering)");
    check(near(getScaleForPhotoViewScaleState(PhotoViewScaleState::OriginalSize, d), 1.0),
          "1: OriginalSize ⇒ clamp(1.0)");
    check(near(getScaleForPhotoViewScaleState(PhotoViewScaleState::ZoomedIn, d), 2.0),
          "1: ZoomedIn/Out 在纯函数口径下回落到 initial（真实值由手势写）");

    // 换档循环。
    check(defaultPhotoViewScaleStateCycle(PhotoViewScaleState::Initial) ==
              PhotoViewScaleState::Covering &&
              defaultPhotoViewScaleStateCycle(PhotoViewScaleState::Covering) ==
                  PhotoViewScaleState::OriginalSize &&
              defaultPhotoViewScaleStateCycle(PhotoViewScaleState::OriginalSize) ==
                  PhotoViewScaleState::Initial,
          "1: 默认循环 Initial→Covering→OriginalSize→Initial");
    check(defaultPhotoViewScaleStateCycle(PhotoViewScaleState::ZoomedIn) ==
              PhotoViewScaleState::Initial,
          "1: 放大态双击直接回 Initial");
    check(isPhotoViewScaleStateZooming(PhotoViewScaleState::ZoomedOut) &&
              !isPhotoViewScaleStateZooming(PhotoViewScaleState::Covering),
          "1: isScaleStateZooming 只认 Zoomed*");

    check(near(clampPhotoViewValue(5.0, 0.0, 3.0), 3.0) && near(clampPhotoViewValue(-1.0, 0.0, 3.0), 0.0),
          "1: clampPhotoViewValue");

    // 滑出比例（三轴）。
    check(near(photoViewSlideRatio(Size(400.0, 300.0), Offset(0.0, 150.0),
                                   PhotoViewSlideAxis::Vertical),
               0.5),
          "1: Vertical ⇒ |dy|/h");
    check(near(photoViewSlideRatio(Size(400.0, 300.0), Offset(200.0, 999.0),
                                   PhotoViewSlideAxis::Horizontal),
               0.5),
          "1: Horizontal 只看 dx");
    check(near(photoViewSlideRatio(Size(400.0, 300.0), Offset(300.0, 400.0),
                                   PhotoViewSlideAxis::Both),
               1.0),
          "1: Both ⇒ 拖距/对角线（500/500）");
    check(near(photoViewSlideRatio(Size(0.0, 0.0), Offset(10.0, 10.0), PhotoViewSlideAxis::Both),
               0.0),
          "1: 零尺寸 ⇒ 比例 0（不除零）");
}

// ── 2. 控制器契约 ───────────────────────────────────────────────────────────

void testControllers() {
    std::printf("[test] 2. PhotoViewController / ScaleStateController\n");

    PhotoViewControllerValue v0;
    v0.scale = 1.0;
    PhotoViewController c(v0);
    int notified = 0;
    int invisibleSeen = 0;
    const int handle = c.addListenerWithHandle([&]() {
        ++notified;
        if (c.internalWrite())
            ++invisibleSeen;
    });
    check(c.prevValue() == v0, "2: 构造时 prevValue 就等于初值（上游同形）");

    c.setPosition(Offset(3.0, 4.0));
    check(near(c.position().dx, 3.0) && notified == 1, "2: setPosition 通知一次");
    c.setPosition(Offset(3.0, 4.0));
    check(notified == 1, "2: 等值写入早退（不通知）");

    c.setScaleInvisibly(2.0);
    check(notified == 2 && invisibleSeen == 1, "2: invisibly 写照样通知，但带 internalWrite 闸门");
    check(c.internalWrite() == false, "2: 通知结束后闸门复位");
    check(near(*c.scale(), 2.0), "2: setScaleInvisibly 落值");

    c.updateMultiple(Offset(0.0, 0.0), 1.5, 0.25);
    check(notified == 3, "2: updateMultiple 一次写三个字段只通知一次");
    check(near(c.position().dx, 0.0) && near(*c.scale(), 1.5) && near(c.rotation(), 0.25),
          "2: updateMultiple 的三个值都落到位");

    c.reset();
    check(near(c.position().dx, 0.0) && near(*c.scale(), 1.0) && c.rotation() == 0.0,
          "2: reset 回到构造时的初值");
    check(notified == 4, "2: reset 也算一次外部写");
    c.removeListenerByHandle(handle);
    check(c.listenerCount() == 0, "2: 句柄摘除后监听清零");

    PhotoViewScaleStateController s;
    int stateSeen = 0;
    const int sh = s.addListenerWithHandle([&]() { ++stateSeen; });
    check(s.scaleState() == PhotoViewScaleState::Initial && !s.hasChanged(),
          "2: 状态控制器初值 Initial 且 hasChanged=false");
    s.setScaleState(PhotoViewScaleState::Covering);
    check(s.prevScaleState() == PhotoViewScaleState::Initial &&
              s.scaleState() == PhotoViewScaleState::Covering && s.hasChanged() && stateSeen == 1,
          "2: setScaleState 记 prev 并通知");
    s.setInvisibly(PhotoViewScaleState::ZoomedIn);
    check(stateSeen == 2 && s.prevScaleState() == PhotoViewScaleState::Covering,
          "2: setInvisibly 也换 prev、也通知（ignorable 由订阅方自己判）");
    check(s.isZooming(), "2: ZoomedIn ⇒ isZooming");
    s.reset();
    check(s.scaleState() == PhotoViewScaleState::Initial &&
              s.prevScaleState() == PhotoViewScaleState::ZoomedIn,
          "2: reset 先把 prev 记成当前再回 Initial");
    s.removeListenerByHandle(sh);
    check(s.listenerCount() == 0, "2: 状态控制器监听清零");
}

// ── 3. 挂载与几何 ───────────────────────────────────────────────────────────

void testMount() {
    std::printf("[test] 3. 挂载：外框 / 自然尺寸 / 树形\n");

    PVArgs args;
    args.provider = std::make_shared<DecodedImageProvider>(png200x100(), "pv3-sync");
    Harness h;
    h.attach(pin(400.0, 300.0, mkPhoto(args)));

    PhotoViewState* st = h.view();
    check(st != nullptr, "3: 找到 PhotoViewState");
    check(st && near(st->outerSize().width, 400.0) && near(st->outerSize().height, 300.0),
          "3: 外框 = LayoutBuilder 的 biggest");
    check(st && st->naturalSize() && near(st->naturalSize()->width, 200.0) &&
              near(st->naturalSize()->height, 100.0),
          "3: decoded provider 同步送达自然尺寸");
    check(st && near(st->boundaries().minScale, 0.0), "3: minScale 缺省 0");
    check(st && st->boundaries().maxScale == BoxConstraints::kInfinity, "3: maxScale 缺省 ∞");
    check(st && near(st->boundaries().initialScale, 2.0), "3: initialScale = contained = 2.0");
    check(st && near(st->scale(), 2.0), "3: 初始 scale 就是 contained");
    check(st && st->scaleState() == PhotoViewScaleState::Initial, "3: 初始档位 Initial");
    check(st && st->error().empty(), "3: 没有错误");
    check(st && !st->isAnimating(), "3: 静息态没在补间");

    // 树形：Transform 恰一条（缩放走 Transform，不做 filterQuality 那条替代路径）、
    // 没有 ClipRect（PhotoView 不裁）、有一层 Listener。
    RenderObject* ro = h.root->findRenderObject();
    check(countRenderObjects<RenderTransform>(ro) == 1, "3: 树里恰好一个 RenderTransform");
    check(countRenderObjects<RenderClipRect>(ro) == 0, "3: 没有 ClipRect（core 不裁）");
    check(countRenderObjects<RenderPointerListener>(ro) == 1, "3: 手势层是 Listener");
    // 两处 RenderAlign：宿主自己的 Align(-1,-1) + PhotoView 的 Center。内容盒那一层
    // 不再是 Align —— 它是 OverflowBox（上游 CustomSingleChildLayout 的等价物）。
    check(countRenderObjects<RenderAlign>(ro) == 2, "3: 只剩宿主 Align + Center");
    check(countRenderObjects<RenderOverflowBox>(ro) == 1, "3: 内容盒是 OverflowBox");

    // 「比外框大的图」：childSize 800×600 在 400×300 的框里 contained = 0.5 ⇒ 铺满。
    // 若内容盒先被外框的 max 夹成 400×300 再乘 0.5，画面就只有中间 200×150 有图。
    PVArgs big;
    big.customChild = block(800.0, 600.0, kCyan);
    big.childSize = Size(800.0, 600.0);
    Harness hBig;
    hBig.attach(pin(400.0, 300.0, mkPhoto(big)));
    hBig.frame();
    check(isCyanish(hBig.at(6, 6)) && isCyanish(hBig.at(393, 293)),
          "3: 缩小看图 ⇒ 内容尺寸是 childSize×scale，没被外框夹成两倍小");

    // customSize 覆盖外框（嵌在非全屏位置时用）。
    PVArgs small = args;
    small.provider = std::make_shared<DecodedImageProvider>(png200x100(), "pv3-small");
    small.customSize = Size(100.0, 100.0);
    h.setBody(pin(400.0, 300.0, mkPhoto(small)));
    check(h.view() && near(h.view()->outerSize().width, 100.0), "3: customSize 顶掉外框");
    check(h.view() && near(h.view()->boundaries().initialScale, 0.5),
          "3: 外框 100×100 ⇒ contained = 0.5");
}

// ── 4. 双击换档与补间 ───────────────────────────────────────────────────────

void testDoubleTapCycle() {
    std::printf("[test] 4. 双击换档 / 定长补间 / 跳档\n");

    PVArgs args;
    args.provider = std::make_shared<DecodedImageProvider>(png200x100(), "pv4-sync");
    Harness h;
    h.attach(pin(400.0, 300.0, mkPhoto(args)));
    PhotoViewState* st = h.view();
    check(st != nullptr, "4: 状态就位");
    if (!st)
        return;

    // 第一记：单点 ⇒ 只发 onTapUp，不换档。
    int taps = 0;
    std::vector<PhotoViewScaleState> states;
    PVArgs withCb = args;
    withCb.provider = args.provider;
    withCb.onTapUp = [&](const PhotoViewTapDetails&) { ++taps; };
    withCb.onScaleState = [&](PhotoViewScaleState s) { states.push_back(s); };
    h.setBody(pin(400.0, 300.0, mkPhoto(withCb)));
    st = h.view();
    h.tap(200.0, 150.0);
    check(taps == 1, "4: 单点发 onTapUp");
    check(st && st->scaleState() == PhotoViewScaleState::Initial, "4: 单点不换档");

    // 第二记落在同一处 ⇒ 成双：这一对里第一记仍发 onTapUp，第二记吞掉 tap 发改档。
    h.doubleTap(200.0, 150.0);
    check(taps == 2, "4: 双击只有第一记发 onTapUp（第二记吞掉，文件头第 2 条）");
    check(st && st->scaleState() == PhotoViewScaleState::Covering, "4: 双击 ⇒ Covering");
    check(st && st->isAnimating(), "4: 换档起补间");
    check(states.size() == 1 && states[0] == PhotoViewScaleState::Covering,
          "4: scaleStateChangedCallback 收到 Covering");
    check(near(st->scale(), 2.0), "4: 补间没推 ⇒ scale 还是上一帧的值");

    // 中段：EaseOutCubic(0.5) = 0.875 ⇒ 2 + (3-2)·0.875 = 2.875。
    st->advanceAnimation(125.0);
    check(near(st->scale(), 2.875), "4: 半程 = EaseOutCubic 插值");
    st->advanceAnimation(1000.0);
    check(near(st->scale(), 3.0) && !st->isAnimating(), "4: 超时一推即收敛到 covering");

    // Covering → OriginalSize（scale 1.0）。
    h.doubleTap(200.0, 150.0);
    check(st && st->scaleState() == PhotoViewScaleState::OriginalSize, "4: 再双击 ⇒ OriginalSize");
    st->advanceAnimation(1000.0);
    check(near(st->scale(), 1.0), "4: OriginalSize ⇒ scale = 1.0");

    // OriginalSize → Initial（回到 contained 2.0）。
    h.doubleTap(200.0, 150.0);
    check(st && st->scaleState() == PhotoViewScaleState::Initial, "4: 三击循环回 Initial");
    st->advanceAnimation(1000.0);
    check(near(st->scale(), 2.0), "4: 回 Initial ⇒ contained");
    check(states.size() == 3, "4: 三次换档都发了回调");

    // 换档时把平移一起归零（上游 animateOnScaleStateUpdate）。
    st->controller()->setPosition(Offset(5.0, 5.0));
    h.pump();
    h.doubleTap(200.0, 150.0);
    st->advanceAnimation(1000.0);
    check(near(st->position(), Offset()), "4: 换档补间把 position 归零");

    // 跳档：contained == covering ⇒ Covering 那一档尺寸没变 ⇒ 直接落 OriginalSize。
    PVArgs wide;
    wide.customChild = block(400.0, 300.0, kCyan);
    wide.childSize = Size(400.0, 300.0);
    wide.controller = std::make_shared<PhotoViewController>();
    h.setBody(pin(200.0, 150.0, mkPhoto(wide)));
    st = h.view();
    check(st && near(st->boundaries().initialScale, 0.5) &&
              near(coveringScaleFor(st->outerSize(), st->boundaries().childSize), 0.5),
          "4: 外框 200×150 / 内容 400×300 ⇒ contained == covering == 0.5");
    h.doubleTap(100.0, 75.0);
    check(st && st->scaleState() == PhotoViewScaleState::OriginalSize,
          "4: 同尺寸档位被跳过 ⇒ 直接 OriginalSize");
    st->advanceAnimation(1000.0);
    check(st && near(st->scale(), 1.0), "4: 跳档后落到 scale 1.0");

    // 三档全等（内容 == 外框）⇒ 双击什么都不发生。起点必须是 Initial，所以另起
    // 一棵树：同一棵里 scaleState 还是上一段留下的 OriginalSize（宿主原地换 body，
    // State 与它内部的控制器都复用）。
    PVArgs same;
    same.customChild = block(400.0, 300.0, kCyan);
    same.childSize = Size(400.0, 300.0);
    Harness hSame;
    hSame.attach(pin(400.0, 300.0, mkPhoto(same)));
    PhotoViewState* stSame = hSame.view();
    hSame.doubleTap(200.0, 150.0);
    check(stSame && stSame->scaleState() == PhotoViewScaleState::Initial &&
              !stSame->isAnimating(),
          "4: 三档尺寸全等 ⇒ 双击不换档（上游同款）");

    // 自定义 cycle：ZoomedIn 之后双击去 OriginalSize（上游的 scaleStateCycle 参数）。
    PVArgs custom;
    custom.customChild = block(400.0, 300.0, kCyan);
    custom.childSize = Size(400.0, 300.0);
    custom.provider = nullptr;
    custom.scaleStateCycle = [](PhotoViewScaleState actual) {
        return actual == PhotoViewScaleState::ZoomedIn ? PhotoViewScaleState::OriginalSize
                                                       : defaultPhotoViewScaleStateCycle(actual);
    };
    Harness hCustom;
    hCustom.attach(pin(200.0, 150.0, mkPhoto(custom)));
    PhotoViewState* stc = hCustom.view();
    // 先外部写一个大于 contained(0.5) 的 scale ⇒ 档位跟成 ZoomedIn，再双击走自定义 cycle。
    // 注意走的是 controller 的「可见」setter：delegate 的 `set scale` 是 invisibly，
    // 上游同样不会跟着改档位。
    stc->controller()->setScale(0.9);
    hCustom.pump();
    check(stc->scaleState() == PhotoViewScaleState::ZoomedIn,
          "4: 外部写 scale ⇒ 档位跟成 ZoomedIn（上游 _blindScaleListener）");
    hCustom.doubleTap(100.0, 75.0);
    check(stc && stc->scaleState() == PhotoViewScaleState::OriginalSize,
          "4: 自定义 cycle ⇒ ZoomedIn 双击去 OriginalSize");
    stc->advanceAnimation(1000.0);
    check(stc && near(stc->scale(), 1.0), "4: 自定义 cycle 那一档就是 scale 1.0");

    // animate=false ⇒ 不起补间，那一档的值立刻落位（文件头第 12 条；上游那里其实
    // 什么都不写，本仓按字面意图补齐）。
    PVArgs noAnim;
    noAnim.customChild = block(400.0, 300.0, kCyan);
    noAnim.childSize = Size(400.0, 300.0);
    noAnim.animate = false;
    Harness hNoAnim;
    hNoAnim.attach(pin(200.0, 150.0, mkPhoto(noAnim)));
    PhotoViewState* stn = hNoAnim.view();
    hNoAnim.doubleTap(100.0, 75.0);
    check(stn && stn->scaleState() == PhotoViewScaleState::OriginalSize,
          "4: animate=false 照样换档");
    check(stn && !stn->isAnimating(), "4: animate=false ⇒ 不起补间");
    check(stn && near(stn->scale(), 1.0), "4: animate=false ⇒ 目标档位立刻落位");
}

// ── 5. 平移、夹取与逃逸 ─────────────────────────────────────────────────────

void testPanAndClamp() {
    std::printf("[test] 5. 平移 / cornersX·Y / 撞到边界让给父级\n");

    Probe probe;
    PVArgs args;
    args.customChild = block(400.0, 300.0, kCyan);
    args.childSize = Size(400.0, 300.0);
    Harness h;
    h.attach(probe.wrap(pin(200.0, 150.0, mkPhoto(args))));
    PhotoViewState* st = h.view();
    check(st != nullptr, "5: 状态就位");
    if (!st)
        return;

    // contained == covering == 0.5 ⇒ 内容刚好铺满 ⇒ 平移不动。
    h.down(1, 100.0, 75.0);
    h.move(1, 130.0, 95.0);
    h.up(1, 130.0, 95.0);
    check(near(st->position(), Offset()), "5: 铺满时平移被夹成 0（上游 clampPosition）");
    check(probe.moves == 1, "5: 没吃掉位移 ⇒ 拖拽让给祖先");

    // 双击到 OriginalSize ⇒ 内容 400×300 在 200×150 里 ⇒ 两轴都能平移。
    h.doubleTap(100.0, 75.0);
    st->advanceAnimation(1000.0);
    h.pump();
    check(near(st->scale(), 1.0), "5: 已放大到 1.0");
    const PhotoViewCornersRange cx = st->cornersX(1.0);
    const PhotoViewCornersRange cy = st->cornersY(1.0);
    check(near(cx.min, -100.0) && near(cx.max, 100.0), "5: cornersX = ±(400-200)/2");
    check(near(cy.min, -75.0) && near(cy.max, 75.0), "5: cornersY = ±(300-150)/2");

    h.down(1, 100.0, 75.0);
    h.move(1, 140.0, 95.0);
    check(near(st->position(), Offset(40.0, 20.0)), "5: 平移跟手（未越界）");
    check(probe.moves == 1, "5: 吃掉了位移 ⇒ 不发给祖先");

    h.move(1, 300.0, 200.0);
    check(near(st->position(), Offset(100.0, 75.0)), "5: 越界 ⇒ 夹到 corners");
    const int movesAfterClamp = probe.moves;
    h.move(1, 310.0, 205.0);
    check(near(st->position(), Offset(100.0, 75.0)) && probe.moves == movesAfterClamp + 1,
          "5: 已夹住 ⇒ 后续拖拽整帧不动 ⇒ 让给祖先");
    h.up(1, 310.0, 205.0);

    // basePosition 挪枢轴 ⇒ corners 跟着不对称（上游公式）。
    PVArgs cornered = args;
    cornered.basePosition = Alignment(-1.0, -1.0);
    h.setBody(probe.wrap(pin(200.0, 150.0, mkPhoto(cornered))));
    st = h.view();
    st->setScale(1.0);
    h.pump();
    const PhotoViewCornersRange tl = st->cornersX(1.0);
    check(near(tl.min, -200.0) && near(tl.max, 0.0),
          "5: basePosition=(-1,-1) ⇒ corners = [-200, 0]（内容贴左上，只许往负方向拖）");
    check(near(st->clampPosition(Offset(-50.0, 0.0), 1.0).dx, -50.0),
          "5: 左上枢轴 ⇒ 负方向平移放行");
    check(near(st->clampPosition(Offset(50.0, 0.0), 1.0).dx, 0.0),
          "5: 左上枢轴 ⇒ 正方向平移夹成 0");

    // enablePanAlways ⇒ 不夹（上游那条「随便拖」的开关）。
    PVArgs freePan = args;
    freePan.enablePanAlways = true;
    h.setBody(probe.wrap(pin(200.0, 150.0, mkPhoto(freePan))));
    st = h.view();
    h.doubleTap(100.0, 75.0);
    st->advanceAnimation(1000.0);
    h.down(1, 100.0, 75.0);
    h.move(1, 300.0, 200.0);
    check(near(st->position(), Offset(200.0, 125.0)), "5: enablePanAlways ⇒ 位移不夹");
    h.up(1, 300.0, 200.0);
}

// ── 6. 捏合与旋转 ───────────────────────────────────────────────────────────

void testPinchAndRotation() {
    std::printf("[test] 6. 双指捏合 / 旋转 / 抬指降级\n");

    Probe probe;
    PVArgs args;
    args.customChild = block(400.0, 300.0, kCyan);
    args.childSize = Size(400.0, 300.0);
    args.enableRotation = true;
    Harness h;
    h.attach(probe.wrap(pin(200.0, 150.0, mkPhoto(args))));
    PhotoViewState* st = h.view();
    if (!st)
        return;
    h.doubleTap(100.0, 75.0);  // → OriginalSize，scale 1.0
    st->advanceAnimation(1000.0);
    h.pump();

    // 两指水平张开 100 → 200 ⇒ ratio 2 ⇒ scale 2.0。
    h.down(1, 50.0, 75.0);
    h.down(2, 150.0, 75.0);
    h.move(2, 250.0, 75.0);
    check(near(st->scale(), 2.0), "6: 跨度比 ⇒ 缩放");
    check(st->scaleState() == PhotoViewScaleState::ZoomedIn, "6: 捏大 ⇒ ZoomedIn");

    // 抬一指 ⇒ 降级成平移（焦点用剩下的那根手指）。
    h.up(2, 250.0, 75.0);
    const double scaleAfterLift = st->scale();
    h.move(1, 60.0, 85.0);
    check(near(st->scale(), scaleAfterLift), "6: 剩单指 ⇒ 只平移不改缩放");

    // 旋转：两指连线从水平转到 +45°，跨度不变 ⇒ scale 不变、rotation = π/4。
    h.up(1, 60.0, 85.0);
    h.pump();
    st->setScale(1.0);
    st->controller()->setPosition(Offset());
    st->controller()->setRotation(0.0);
    h.pump();
    h.down(3, 50.0, 75.0);
    h.down(4, 150.0, 75.0);
    h.move(4, 100.0, 25.0);  // 新连线 (50,75)→(100,25)：atan2(-50,50) = -π/4，跨度 70.71
    const double expectRotation = std::atan2(-50.0, 50.0);
    check(near(st->rotation(), expectRotation, 1e-9), "6: rotation = 角度增量（enableRotation）");
    const double expectScale = 1.0 * std::hypot(50.0, -50.0) / 100.0;
    check(near(st->scale(), expectScale, 1e-9), "6: 同一次移动里跨度也照改缩放");
    h.up(3, 50.0, 75.0);
    h.up(4, 100.0, 25.0);

    // 关掉旋转 ⇒ 只缩放。
    PVArgs noRot = args;
    noRot.enableRotation = false;
    h.setBody(probe.wrap(pin(200.0, 150.0, mkPhoto(noRot))));
    st = h.view();
    st->setScale(1.0);
    st->controller()->setRotation(0.0);  // 上一段把角度转成了 -π/4，同一棵树的控制器要清干净
    h.pump();
    h.down(5, 50.0, 75.0);
    h.down(6, 150.0, 75.0);
    h.move(6, 100.0, 25.0);
    check(near(st->rotation(), 0.0), "6: enableRotation=false ⇒ 角度不改 rotation");
    h.up(5, 50.0, 75.0);
    h.up(6, 100.0, 25.0);

    // 抬手回弹：超过 max ⇒ 补间回 max 并按比例夹位（上游 onScaleEnd 那段）。
    PVArgs capped = args;
    capped.maxScale = PhotoViewScale::fixed(1.2);
    h.setBody(probe.wrap(pin(200.0, 150.0, mkPhoto(capped))));
    st = h.view();
    st->controller()->setScaleInvisibly(1.0);
    h.pump();
    h.down(7, 100.0, 75.0);
    h.down(8, 120.0, 75.0);
    h.move(8, 300.0, 75.0);  // ratio 9 ⇒ 想要 9 倍
    const double during = st->scale();
    h.up(7, 100.0, 75.0);
    h.up(8, 300.0, 75.0);
    check(during > 1.2, "6: 捏合途中可以先越界（strictScale=false 时不拦）");
    check(st->isAnimating(), "6: 抬手发现超界 ⇒ 起回弹补间");
    st->advanceAnimation(1000.0);
    check(near(st->scale(), 1.2), "6: 回弹收敛到 maxScale");
}

// ── 7. 滚轮 ─────────────────────────────────────────────────────────────────

void testWheel() {
    std::printf("[test] 7. 滚轮锚定 / strictScale / 上下限让位\n");

    const Size child(400.0, 300.0);
    const double pivotX = 100.0, pivotY = 75.0;  // 外框 200×150 的中心

    Probe probe;
    PVArgs args;
    args.customChild = block(child.width, child.height, kCyan);
    args.childSize = child;
    Harness h;
    h.attach(probe.wrap(pin(200.0, 150.0, mkPhoto(args))));
    PhotoViewState* st = h.view();
    if (!st)
        return;
    h.doubleTap(100.0, 75.0);  // OriginalSize ⇒ scale 1.0, pos 0
    st->advanceAnimation(1000.0);
    h.pump();
    check(near(st->scale(), 1.0) && near(st->position(), Offset()), "7: 起点 scale 1.0 / pos 0");

    // 滚轮放大：dy = -200·ln2 ⇒ 正好 2 倍。光标 (100,25)，枢轴 (100,75)。
    const double dy = -200.0 * std::log(2.0);
    const int scrollsBefore = probe.scrolls;
    h.scroll(100.0, 25.0, dy);
    check(near(st->scale(), 2.0, 1e-9), "7: 滚轮按 exp(-dy/factor) 缩放");
    // 精确锚定：pos' = k·pos + (1-k)(f-P) = 0 + (-1)(0,-50) = (0,50)。
    check(near(st->position(), Offset(0.0, 50.0), 1e-9), "7: 滚轮锚定写进 position");
    const double sceneBefore = (25.0 - pivotY - 0.0) / 1.0;
    const double sceneAfter = (25.0 - pivotY - st->position().dy) / st->scale();
    check(near(sceneBefore, sceneAfter, 1e-9), "7: 光标下的场景点不变");
    check(probe.scrolls == scrollsBefore, "7: 吃掉了滚轮 ⇒ 不发给祖先");
    check(st->scaleState() == PhotoViewScaleState::ZoomedIn, "7: 滚轮放大 ⇒ ZoomedIn");

    // 缩到 min（contained）⇒ 整帧没改动 ⇒ 滚轮让给祖先（画廊翻页靠这条）。
    PVArgs floored = args;
    floored.minScale = PhotoViewScale::contained();
    Harness h2;
    Probe probe2;
    h2.attach(probe2.wrap(pin(200.0, 150.0, mkPhoto(floored))));
    PhotoViewState* s2 = h2.view();
    check(s2 && near(s2->boundaries().minScale, 0.5), "7: minScale=contained ⇒ 0.5");
    const int before2 = probe2.scrolls;
    h2.scroll(100.0, 75.0, 200.0);  // 想缩到 0.5·e⁻¹
    check(near(s2->scale(), 0.5), "7: 缩到 min 就停住");
    check(probe2.scrolls == before2 + 1, "7: 已到 min ⇒ 滚轮交还祖先");

    // strictScale ⇒ 想越界就整帧不动，而且不消费（与上面同一条口径）。
    PVArgs strict = args;
    strict.maxScale = PhotoViewScale::fixed(0.6);
    strict.strictScale = true;
    Harness h3;
    Probe probe3;
    h3.attach(probe3.wrap(pin(200.0, 150.0, mkPhoto(strict))));
    PhotoViewState* s3 = h3.view();
    const int before3 = probe3.scrolls;
    const double scale3 = s3 ? s3->scale() : 0.0;
    h3.scroll(100.0, 75.0, -100.0);  // 想要 0.5·e^0.5 ≈ 0.82 > max 0.6
    check(near(s3->scale(), scale3), "7: strictScale ⇒ 越界的滚轮整帧不动");
    check(probe3.scrolls == before3 + 1, "7: strictScale ⇒ 不消费 ⇒ 让给祖先");

    // 同一棵树关掉 strict ⇒ 同样的滚轮夹到 max 并消费。
    PVArgs lax = strict;
    lax.strictScale = false;
    h3.setBody(probe3.wrap(pin(200.0, 150.0, mkPhoto(lax))));
    s3 = h3.view();
    const int before4 = probe3.scrolls;
    h3.scroll(100.0, 75.0, -100.0);
    check(near(s3->scale(), 0.6), "7: 非 strict ⇒ 夹到 maxScale");
    check(probe3.scrolls == before4, "7: 夹到位算吃掉 ⇒ 不发祖先");

    // scaleFactor 决定每格滚轮的倍率。
    PVArgs fast = args;
    fast.scaleFactor = 100.0;
    Harness h4;
    h4.attach(pin(200.0, 150.0, mkPhoto(fast)));
    PhotoViewState* s4 = h4.view();
    if (s4) {
        const double from = s4->scale();
        h4.scroll(100.0, 75.0, -100.0);  // exp(1) 倍
        check(near(s4->scale(), from * std::exp(1.0), 1e-9), "7: scaleFactor 进指数");
    }
}

// ── 8. 回调节奏与 disableGestures ───────────────────────────────────────────

void testCallbacks() {
    std::printf("[test] 8. onTapDown/onTapUp/onScaleEnd/disableGestures\n");

    int downs = 0, ups = 0, ends = 0;
    Offset lastUpGlobal, lastUpLocal, lastEndGlobal;
    PVArgs args;
    args.customChild = block(400.0, 300.0, kCyan);
    args.childSize = Size(400.0, 300.0);
    args.onTapDown = [&](const PhotoViewTapDetails& d) { ++downs; (void)d; };
    args.onTapUp = [&](const PhotoViewTapDetails& d) {
        ++ups;
        lastUpGlobal = d.globalPosition;
        lastUpLocal = d.position;
    };
    args.onScaleEnd = [&](const PhotoViewTapDetails& d) {
        ++ends;
        lastEndGlobal = d.globalPosition;
    };
    Probe probe;
    Harness h;
    h.attach(probe.wrap(pin(200.0, 150.0, mkPhoto(args))));
    PhotoViewState* st = h.view();

    h.tap(50.0, 40.0);
    check(downs == 1 && ups == 1, "8: 单点发 down+up 各一次");
    check(near(lastUpGlobal, Offset(50.0, 40.0)), "8: onTapUp 的 globalPosition");
    check(near(lastUpLocal, Offset(50.0, 40.0)), "8: 探针在 (0,0) ⇒ 局部==全局");
    check(ends == 1, "8: 抬手发 onScaleEnd（上游 _onScaleEnd）");

    // 拖超过 slop（44px）⇒ 不算 tap，也不换档，tap 记录作废。
    h.down(1, 50.0, 40.0);
    h.move(1, 120.0, 40.0);
    h.up(1, 120.0, 40.0);
    check(ups == 1, "8: 拖动结束的抬手不发 onTapUp");
    check(st && st->scaleState() == PhotoViewScaleState::Initial, "8: 拖动结束不换档");
    h.tap(120.0, 40.0);
    check(ups == 2, "8: 拖动后重新起算 ⇒ 下一记仍是单点");

    // 双击：第一记发 tap、第二记吞掉。
    h.doubleTap(120.0, 40.0);
    check(ups == 3, "8: 双击的第一记仍发 onTapUp（本仓时序判定，文件头第 2 条）");
    check(st && st->scaleState() != PhotoViewScaleState::Initial, "8: 第二记换档");

    // disableGestures ⇒ 整棵树没有 Listener，事件全给祖先。
    PVArgs disabled = args;
    disabled.disableGestures = true;
    const int upsBeforeDisable = ups, endsBeforeDisable = ends;
    const int downsBefore = probe.downs, scrollsBefore = probe.scrolls;
    h.setBody(probe.wrap(pin(200.0, 150.0, mkPhoto(disabled))));
    check(countRenderObjects<RenderPointerListener>(h.root->findRenderObject()) == 1,
          "8: 只剩祖先探针那一个 Listener");
    h.tap(100.0, 75.0);
    h.scroll(100.0, 75.0, 100.0);
    check(ups == upsBeforeDisable && ends == endsBeforeDisable,
          "8: disableGestures ⇒ 自己一个回调都收不到");
    // 一记 tap = 一个 down ⇒ 探针的 downs 只 +1；滚轮 +1。
    check(probe.downs == downsBefore + 1 && probe.scrolls == scrollsBefore + 1,
          "8: 事件原样交给祖先");
}

// ── 9. customChild / tightMode / hero / 失败态 / 异步出图 ────────────────────

void testContentModes() {
    std::printf("[test] 9. customChild / tightMode / heroTag / 失败与异步\n");

    // tightMode：core 的尺寸就是「内容 × scale」，所以背景盒会短一截。
    Harness h;
    PVArgs tight;
    tight.provider = std::make_shared<DecodedImageProvider>(png200x100(), "pv9-tight");
    tight.tightMode = true;
    // 用 Align 给松约束，tightMode 的 SizedBox 才顶得出真实尺寸。
    h.attach(cui::make_shared<Align>(-1.0, -1.0, mkPhoto(tight)));
    PhotoViewState* st = h.view();
    check(st && near(st->outerSize().height, 300.0), "9: 松约束下外框仍是窗口高");
    check(st && near(st->scale(), 2.0), "9: contained = 2.0");
    h.frame();
    // 背景盒 = 200×100 × 2 = 400×200 ⇒ 盒内是内容本身（contained 恰好铺满，
    // 黑背景被图盖住），(10,250) 落在盒子外面（白）。素材是「左半红右半蓝」，
    // 所以 (10,50) 期望红色通道高。
    check((h.at(10, 50) & 0xFFu) > 150 && (h.at(10, 50) >> 16 & 0xFFu) < 120,
          "9: tightMode ⇒ 内容盒里是图");
    check(!isCyanish(h.at(10, 250)) && h.at(10, 250) != 0xFF000000u,
          "9: tightMode ⇒ 背景盒只盖住内容（下方露白）");
    auto* tightBox = firstRenderObject<RenderDecoratedBox>(h.root->findRenderObject());
    check(tightBox && near(tightBox->size().width, 400.0) && near(tightBox->size().height, 200.0),
          "9: tightMode ⇒ 背景盒尺寸 = childSize × scale");

    // 非 tightMode：背景盒撑满外框。
    Harness h2;
    PVArgs loose;
    loose.provider = std::make_shared<DecodedImageProvider>(png200x100(), "pv9-loose");
    h2.attach(cui::make_shared<Align>(-1.0, -1.0, mkPhoto(loose)));
    h2.frame();
    const uint32_t below = h2.at(10, 250);
    check((below & 0xFFu) < 20 && ((below >> 8) & 0xFFu) < 20 && ((below >> 16) & 0xFFu) < 20,
          "9: 不开 tightMode ⇒ 背景铺满整个外框");
    auto* looseBox = firstRenderObject<RenderDecoratedBox>(h2.root->findRenderObject());
    check(looseBox && near(looseBox->size().width, 400.0) && near(looseBox->size().height, 300.0),
          "9: 非 tightMode ⇒ 背景盒尺寸 = 外框");

    // heroTag ⇒ 树里出现 Hero（在内容之上、SizedBox 之下）。
    PVArgs hero = loose;
    hero.heroTag = "pv-hero";
    h2.setBody(cui::make_shared<Align>(-1.0, -1.0, mkPhoto(hero)));
    check(findWidget<Hero>(h2.root) != nullptr, "9: heroTag 非空 ⇒ 包一层 Hero");
    hero.heroTag = "";
    h2.setBody(cui::make_shared<Align>(-1.0, -1.0, mkPhoto(hero)));
    check(findWidget<Hero>(h2.root) == nullptr, "9: heroTag 清空 ⇒ 不再包 Hero");

    // 失败态：整体替换 core（没有 Transform），errorBuilder 收到原因。
    Harness h3;
    int errorShown = 0;
    std::string why;
    PVArgs failed;
    failed.provider = std::make_shared<MemoryImageProvider>(std::vector<uint8_t>{9, 8, 7}, "pv9-bad");
    failed.errorBuilder = [&](BuildContext&, const std::string& e) {
        ++errorShown;
        why = e;
        return block(20.0, 20.0, kPink);
    };
    h3.attach(pin(200.0, 150.0, mkPhoto(failed)));
    PhotoViewState* s3 = h3.view();
    check(s3 && !s3->error().empty(), "9: 坏数据 ⇒ state.error() 非空");
    check(errorShown >= 1 && !why.empty(), "9: errorBuilder 被调且拿到失败原因");
    check(countRenderObjects<RenderTransform>(h3.root->findRenderObject()) == 0,
          "9: 失败态把 core 整棵树换掉（无 Transform）");
    check(countRenderObjects<RenderPointerListener>(h3.root->findRenderObject()) == 0,
          "9: 失败态不挂手势 Listener（文件头第 8 条）");
    check(s3 && s3->naturalSize() == std::nullopt, "9: 失败时没有自然尺寸");

    // 异步出图：先按「外框」当 childSize 挂完整树，尺寸到了再重算边界。
    installSeams(false, false);
    Harness h4;
    int loadingBuilt = 0;
    PVArgs asyncArgs;
    asyncArgs.provider =
        std::make_shared<MemoryImageProvider>(toBytes(makePng(200, 100)), "pv9-async");
    asyncArgs.loadingBuilder = [&](BuildContext&, double) {
        ++loadingBuilt;
        return block(8.0, 8.0, kLime);
    };
    h4.attach(pin(400.0, 300.0, mkPhoto(asyncArgs)));
    PhotoViewState* s4 = h4.view();
    check(s4 && s4->naturalSize() == std::nullopt, "9: 在飞时还没有自然尺寸");
    check(s4 && near(s4->boundaries().childSize.width, 400.0),
          "9: 在飞时 childSize 暂取外框（文件头第 8 条）");
    check(loadingBuilt >= 1, "9: loadingBuilder 画了占位");
    runBg();
    runUi();
    h4.settle();
    check(s4 && s4->naturalSize() && near(s4->naturalSize()->width, 200.0),
          "9: 异步结果落地 ⇒ 自然尺寸 200×100");
    check(s4 && near(s4->boundaries().initialScale, 2.0), "9: 尺寸到了 ⇒ 边界重算 contained=2");
    check(s4 && near(s4->scale(), 2.0), "9: 边界变了 ⇒ scale 跟着重算");
    resetSeams();
    installSeams(true, true);
}

// ── 10. 画廊：窗口 / 翻页 / 吸附 / 保活 / 滚轮 ──────────────────────────────

PhotoViewGalleryPageOptions pageOption(const Color& color, double w, double h) {
    auto opt = PhotoViewGalleryPageOptions::forChild(block(w, h, color), Size(w, h));
    return opt;
}

void testGallery() {
    std::printf("[test] 10. PhotoViewGallery：页窗口 / 拖拽翻页 / 吸附 / 控制器保活\n");

    std::vector<int> changed;
    std::vector<PhotoViewGalleryPageOptions> pages = {
        pageOption(kCyan, 800.0, 600.0),
        pageOption(kLime, 800.0, 600.0),
        pageOption(kPink, 800.0, 600.0),
    };
    auto gallery = cui::make_shared<PhotoViewGallery>(
        pages, nullptr, 0, 0, Axis::Horizontal, false, true, std::nullopt,
        photoViewDefaultBackground(), false, false, 250.0, Easing::EaseOutCubic,
        [&](int i) { changed.push_back(i); }, nullptr, nullptr, nullptr, nullptr);

    Probe probe;
    Harness h;
    h.attach(probe.wrap(gallery));
    PhotoViewGalleryState* g = h.gallery();
    check(g != nullptr, "10: 找到 PhotoViewGalleryState");
    if (!g)
        return;
    check(g->itemCount() == 3, "10: itemCount = 页表长度");
    check(near(g->pageOffset(), 0.0) && g->currentPage() == 0, "10: 起始在第 0 页");
    check(near(g->outerSize().width, 400.0) && near(g->outerSize().height, 300.0),
          "10: 画廊外框 = 窗口");

    std::vector<int> keys;
    collectPageKeys(h.root, keys);
    check(keys == std::vector<int>({0, 1}), "10: offset=0 ⇒ 只建 [floor-1, ceil+1] = {0,1}");
    check(changed.empty(), "10: 挂载本身不发 onPageChanged");

    // 瞬时跳页。
    g->setPage(2, false);
    h.settle();
    check(near(g->pageOffset(), 2.0) && g->currentPage() == 2, "10: setPage(2, 不过程间)");
    check(changed.size() == 1 && changed[0] == 2, "10: onPageChanged 报 2");
    keys.clear();
    collectPageKeys(h.root, keys);
    check(keys == std::vector<int>({1, 2}), "10: offset=2 ⇒ 窗口 {1,2}（第 0 页已回收）");

    // 带补间跳回第 0 页。
    g->setPage(0, true);
    check(g->isSettling(), "10: setPage(animate) 起吸附");
    g->advanceSettle(125.0);  // EaseOutCubic(0.5) = 0.875 ⇒ 2 + (0-2)·0.875 = 0.25
    check(near(g->pageOffset(), 0.25), "10: 吸附中段按曲线插值");
    g->advanceSettle(1000.0);
    check(near(g->pageOffset(), 0.0) && !g->isSettling(), "10: 吸附收敛到整页");
    check(changed.size() == 2 && changed[1] == 0, "10: 吸附途中也发了 onPageChanged(0)");

    // 拖拽翻页：800×600 内容在 400×300 外框里 contained=0.5 ⇒ 铺满 ⇒ 平移不动 ⇒ 让给画廊。
    g->setPage(0, false);
    h.settle();
    changed.clear();
    h.down(1, 200.0, 150.0);
    h.move(1, 100.0, 150.0);  // 拖走 100px = 1/4 页
    check(near(g->pageOffset(), 0.25), "10: 拖拽按主轴长度换算页偏移");
    check(g->currentPage() == 0 && changed.empty(), "10: round(0.25)=0 ⇒ 还没换页");
    h.move(1, 50.0, 150.0);  // 累计 150px = 0.375 页
    check(near(g->pageOffset(), 0.375), "10: 拖拽以「按下点」为基准（不是逐帧增量）");
    h.move(1, -100.0, 150.0);  // 累计 300px = 0.75 页
    check(near(g->pageOffset(), 0.75) && g->currentPage() == 1 && changed == std::vector<int>({1}),
          "10: 越过半页即换页（round 报，不等吸附）");
    h.up(1, -100.0, 150.0);
    check(g->isSettling(), "10: pageSnapping ⇒ 抬手起吸附");
    g->finishSettle();
    h.settle();
    check(near(g->pageOffset(), 1.0), "10: 吸附到最近整页");

    // 撞到末页 ⇒ 剩余拖拽让给祖先（探针数）。
    g->setPage(2, false);
    h.settle();
    const int movesBefore = probe.moves;
    h.down(1, 200.0, 150.0);
    h.move(1, 100.0, 150.0);  // 想再往前一页 ⇒ 夹在 2.0 ⇒ 画廊也没动
    check(near(g->pageOffset(), 2.0), "10: 末页再拖不改偏移（严格夹取，无橡皮筋）");
    check(probe.moves == movesBefore + 1, "10: 夹到边界 ⇒ 拖拽让给祖先");
    h.up(1, 100.0, 150.0);

    // 滚轮：页面 minScale 缺省 contained ⇒ 缩小整帧无效 ⇒ 交给画廊攒页。
    g->setPage(0, false);
    h.settle();
    changed.clear();
    const int scrollsBefore = probe.scrolls;
    h.scroll(200.0, 150.0, 400.0);  // 一整页
    check(near(g->pageOffset(), 1.0), "10: 滚轮攒满一整页才翻");
    check(probe.scrolls == scrollsBefore, "10: 画廊吃了滚轮 ⇒ 不再上抛");
    h.scroll(200.0, 150.0, 100.0);  // 1/4 页：不够 ⇒ 只累积
    check(near(g->pageOffset(), 1.0), "10: 不足一页的滚轮留在累加里");
    h.scroll(200.0, 150.0, 300.0);  // 加上攒下的 1/4 ⇒ 1.25 ⇒ 翻 1 页，余 0.25
    check(near(g->pageOffset(), 2.0), "10: 累加跨页后翻一页并把余量留下");
    check(changed == std::vector<int>({2}) || changed == std::vector<int>({1, 2}),
          "10: 每次跨页都发了 onPageChanged");

    // 每页控制器由画廊持有 ⇒ 翻出窗口再翻回来，缩放态还在（wantKeepAlive 等价物）。
    PhotoViewControllerPtr page0 = g->controllerFor(0);
    check(page0 != nullptr, "10: 第 0 页有控制器");
    g->setPage(0, false);
    h.settle();
    // 双击当前页放大。`h.view()` 是窗口里第一个 PhotoViewState ⇒ offset=0 时正是第 0 页，
    // 补间也推得动（换到第 2 页就推不到了，那要另开一棵单页树，见 §4/§5）。
    h.doubleTap(200.0, 150.0);
    if (PhotoViewState* page = h.view())
        page->advanceAnimation(1000.0);
    h.settle();
    check(g->pageZoomed(0), "10: 页面把边界写回控制器 ⇒ pageZoomed 认得放大态");
    check(!g->pageZoomed(1), "10: 没动过的第 1 页不算放大");
    g->setPage(2, false);
    h.settle();
    keys.clear();
    collectPageKeys(h.root, keys);
    check(keys == std::vector<int>({1, 2}), "10: 第 0 页已回收出窗口");
    check(g->pageZoomed(0), "10: 翻出窗口的页保住了缩放态");

    // builder 入口 + declaredItemCount。
    int built = 0;
    auto byBuilder = PhotoViewGallery::builder(
        [&](int i) {
            ++built;
            return pageOption(i == 0 ? kCyan : kLime, 800.0, 600.0);
        },
        4);
    Harness h2;
    h2.attach(byBuilder);
    PhotoViewGalleryState* gb = h2.gallery();
    check(gb && gb->itemCount() == 4, "10: builder 入口用声明的 itemCount");
    check(built >= 2, "10: builder 被调过（窗口内每页一次）");
    gb->setPage(3, false);
    h2.settle();
    std::vector<int> bKeys;
    collectPageKeys(h2.root, bKeys);
    check(bKeys == std::vector<int>({2, 3}), "10: builder 模式同样走窗口");
}

// ── 11. 画廊：垂直轴 / reverse / 像素 ───────────────────────────────────────

void testGalleryAxisAndPixels() {
    std::printf("[test] 11. 画廊：垂直主轴 / reverse / 像素换页\n");

    std::vector<PhotoViewGalleryPageOptions> pages = {
        pageOption(kCyan, 800.0, 600.0),
        pageOption(kLime, 800.0, 600.0),
        pageOption(kPink, 800.0, 600.0),
    };

    // 垂直：主轴长度 = 300 ⇒ 拖 150px 就是半页。
    Probe probe;
    auto vertical = cui::make_shared<PhotoViewGallery>(pages, nullptr, 0, 0, Axis::Vertical,
                                                       false, false, std::nullopt,
                                                       photoViewDefaultBackground(), false,
                                                       false, 250.0, Easing::EaseOutCubic,
                                                       nullptr, nullptr, nullptr, nullptr,
                                                       nullptr);
    Harness h;
    h.attach(probe.wrap(vertical));
    PhotoViewGalleryState* g = h.gallery();
    if (!g)
        return;
    check(near(g->outerSize().height, 300.0), "11: 垂直模式读高度");
    h.down(1, 200.0, 250.0);
    h.move(1, 200.0, 100.0);  // 上拖 150px = 半页
    check(near(g->pageOffset(), 0.5), "11: 垂直拖拽按高度换算");
    h.up(1, 200.0, 100.0);
    check(!g->isSettling(), "11: pageSnapping=false ⇒ 抬手不吸附");
    check(near(g->pageOffset(), 0.5), "11: 半页偏移原样留着");
    h.frame();
    check(isCyanish(h.at(200, 30)) || isLimish(h.at(200, 30)),
          "11: 半页处两页都在画（各占一截）");

    // reverse：布局镜像 + 拖拽方向镜像。
    auto reversed = cui::make_shared<PhotoViewGallery>(
        pages, nullptr, 0, 2, Axis::Horizontal, true, false, std::nullopt,
        photoViewDefaultBackground(), false, false, 250.0, Easing::EaseOutCubic, nullptr,
        nullptr, nullptr, nullptr, nullptr);
    Harness h2;
    h2.attach(reversed);
    PhotoViewGalleryState* gr = h2.gallery();
    check(gr && gr->currentPage() == 2, "11: initialIndex=2 生效（夹在末页）");
    h2.frame();
    check(isPinkish(h2.at(200, 150)), "11: reverse 下末页仍铺满窗口（页号定位是相对的）");
    const double before = gr->pageOffset();
    h2.down(1, 200.0, 150.0);
    h2.move(1, 100.0, 150.0);  // 左拖：reverse ⇒ 往页号小的方向走
    check(gr->pageOffset() < before, "11: reverse 把拖拽方向镜像");
    h2.up(1, 100.0, 150.0);

    // 像素：翻到第 1 页后窗口里换颜色。
    auto forward = cui::make_shared<PhotoViewGallery>(pages, nullptr, 0, 0, Axis::Horizontal,
                                                      false, false, std::nullopt,
                                                      photoViewDefaultBackground(), false,
                                                      false, 250.0, Easing::EaseOutCubic,
                                                      nullptr, nullptr, nullptr, nullptr,
                                                      nullptr);
    Harness h3;
    h3.attach(forward);
    PhotoViewGalleryState* gf = h3.gallery();
    h3.frame();
    check(isCyanish(h3.at(50, 50)), "11: 第 0 页画的是青色块");
    gf->setPage(1, false);
    h3.settle();
    h3.frame();
    check(isLimish(h3.at(50, 50)), "11: 翻到第 1 页 ⇒ 换成黄绿块");
    check(!isCyanish(h3.at(50, 50)), "11: 旧页不再盖在新页上面");
    gf->setPage(2, false);
    h3.settle();
    h3.frame();
    check(isPinkish(h3.at(50, 50)), "11: 第 2 页 ⇒ 粉色块");

    // 空画廊不崩。
    auto empty = cui::make_shared<PhotoViewGallery>();
    Harness h4;
    h4.attach(empty);
    PhotoViewGalleryState* ge = h4.gallery();
    check(ge && ge->itemCount() == 0 && near(ge->pageOffset(), 0.0), "11: 零页画廊偏移为 0");
    ge->setPage(3, false);
    check(near(ge->pageOffset(), 0.0), "11: 零页时 setPage 不改偏移");
}

// ── 12. 滑出关闭页 ──────────────────────────────────────────────────────────

struct NavHarness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    NavigatorState* nav = nullptr;

    void attach(double ww = 400, double hh = 300) {
        rv.setWindowSize({ww, hh});
        auto navigator = cui::make_shared<Navigator>();
        auto e = navigator->createElement(navigator);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        if (auto* box = dynamic_cast<RenderBox*>(root->findRenderObject()))
            rv.setChild(box);
        po.setRootNode(&rv);
        frame();
        nav = dynamic_cast<NavigatorState*>(dynamic_cast<StatefulElement*>(root)->state());
    }
    void frame() {
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
        owner.finalizeTree();
        owner.flushPostFrameCallbacks();
    }
    void tick(int sleepMs = 16) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        Ticker::tickAllActive(Duration(sleepMs));
        frame();
    }
    bool until(const std::function<bool()>& pred, int maxTicks = 40) {
        for (int i = 0; i < maxTicks; ++i) {
            if (pred())
                return true;
            tick();
        }
        return pred();
    }
    void pushBase(SharedPtr<Widget> page) {
        nav->push(cui::make_shared<ContentRoute>(
            [page](BuildContext&) { return page; }, true));
        frame();
    }
    void down(int64_t id, double x, double y) {
        PointerDownEvent d;
        d.pointerId = id;
        d.position = {x, y};
        d.globalPosition = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        frame();
    }
    void move(int64_t id, double x, double y) {
        PointerMoveEvent m;
        m.pointerId = id;
        m.position = {x, y};
        m.globalPosition = {x, y};
        m.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(m);
        frame();
    }
    void up(int64_t id, double x, double y) {
        PointerUpEvent u;
        u.pointerId = id;
        u.position = {x, y};
        u.globalPosition = {x, y};
        po.dispatchPointerEvent(u);
        frame();
    }
    PhotoViewSlidePageState* slide() const {
        return dynamic_cast<PhotoViewSlidePageState*>(findStateNamed(root, "PhotoViewSlidePage"));
    }
};

void testSlidePage() {
    std::printf("[test] 12. PhotoViewSlidePage：淡出 / 缩放 / 复位 / 关页\n");

    Offset slid;
    int slidCalls = 0;
    auto slide = cui::make_shared<PhotoViewSlidePage>(
        block(400.0, 300.0, kCyan), Color(0.0, 0.0, 0.0, 1.0), 0.25,
        PhotoViewSlideAxis::Vertical, PhotoViewSlideType::OnlyImage, nullptr, nullptr, nullptr,
        nullptr, 500.0, [&](const Offset& o) { slid = o; ++slidCalls; }, true, nullptr);

    Harness h;
    h.attach(pin(400.0, 300.0, slide));
    PhotoViewSlidePageState* st = h.slide();
    check(st != nullptr, "12: 找到 PhotoViewSlidePageState");
    if (!st)
        return;
    check(near(st->outerSize().width, 400.0) && near(st->outerSize().height, 300.0),
          "12: 滑出页外框 = 约束 biggest");
    check(near(st->ratio(), 0.0) && near(st->currentScale(), 1.0), "12: 静止态比例 0 / 缩放 1");
    check(near(st->currentBackground().a, 1.0), "12: 静止态背景不透明");

    // 用 setOffset 直接验证三条缺省公式。
    st->setOffset(Offset(0.0, 150.0));
    h.pump();
    check(near(st->ratio(), 0.5), "12: Vertical 比例 = |dy|/h");
    check(near(st->currentScale(), 0.925), "12: OnlyImage 缩放 = 1 - 0.15·ratio");
    check(near(st->currentBackground().a, 0.5), "12: 背景 alpha 随 ratio 线性淡出");
    check(slidCalls >= 1 && near(slid, Offset(0.0, 150.0)), "12: onSlidingPage 收到偏移");
    check(st->willCloseOnRelease(), "12: ratio 0.5 ≥ 0.25 ⇒ 抬手该关页");

    st->setOffset(Offset(0.0, 60.0));
    h.pump();
    check(!st->willCloseOnRelease(), "12: ratio 0.2 < 0.25 ⇒ 不关");

    // 拖拽：轴锁定 ⇒ dx 被丢掉。拖 60px（ratio 0.2 < 0.25）才落在「不复位就关页」
    // 之外 —— 拖 80px 就已经越过阈值了。
    st->finishReset();
    st->setOffset(Offset());
    h.pump();
    h.down(1, 200.0, 100.0);
    h.move(1, 260.0, 160.0);
    check(near(st->slideOffset(), Offset(0.0, 60.0)), "12: Vertical 轴锁定（dx 归零）");
    check(st->isDragging(), "12: 拖拽中");
    h.up(1, 260.0, 160.0);
    // 探针在宿主里（无 Navigator）⇒ 只可能复位。
    check(st->isResetting(), "12: 未到阈值 ⇒ 抬手起复位补间");
    st->advanceReset(100.0);  // EaseOutCubic(0.2) = 0.488 ⇒ 60·(1-0.488)
    check(near(st->slideOffset().dy, 60.0 * (1.0 - 0.488), 0.5),
          "12: 复位中段按 EaseOutCubic 收缩");
    st->advanceReset(1e9);
    check(near(st->slideOffset(), Offset()) && !st->isResetting(), "12: 复位收敛回原位");

    // 自定义处理器全都走一遍（背景/缩放/结束/偏移改写）。注意 slideOffsetHandler
    // 只改写「拖出来的」偏移 —— `setOffset` 是外部直接设定，不经过它，所以这里用
    // 真实拖拽序列。
    auto custom = cui::make_shared<PhotoViewSlidePage>(
        block(400.0, 300.0, kCyan), Color(1.0, 1.0, 1.0, 1.0), 0.25, PhotoViewSlideAxis::Both,
        PhotoViewSlideType::WholePage,
        [](const Size&, const Offset& o) { return Color(0.0, o.dy / 1000.0, 0.0, 1.0); },
        [](const Size&, const Offset& o) { return 1.0 + o.dx / 1000.0; },
        [](const Size&, const Offset& o, double) { return o.dy > 110.0; },
        [](const Size&, const Offset& o) { return Offset(o.dx, o.dy * 0.5); }, 500.0, nullptr,
        true, nullptr);
    Harness h2;
    h2.attach(pin(400.0, 300.0, custom));
    PhotoViewSlidePageState* c2 = h2.slide();
    if (!c2)
        return;
    h2.down(1, 50.0, 50.0);
    h2.move(1, 250.0, 250.0);  // 原始位移 (200,200) ⇒ 改写后 (200,100)
    check(near(c2->slideOffset(), Offset(200.0, 100.0)),
          "12: slideOffsetHandler 改写了偏移（dy 阻尼 0.5）");
    check(near(c2->currentBackground().g, 0.1) && near(c2->currentBackground().a, 1.0),
          "12: backgroundHandler 接管了颜色");
    check(near(c2->currentScale(), 1.2), "12: slideScaleHandler 接管了缩放");
    check(!c2->willCloseOnRelease(), "12: slideEndHandler 说了算（100 ≤ 110 ⇒ 不关）");
    h2.move(1, 250.0, 290.0);  // 原始 (200,240) ⇒ 改写后 dy=120
    check(c2->willCloseOnRelease(), "12: 改写后 dy=120 > 110 ⇒ 该关");
    h2.up(1, 250.0, 290.0);
    // WholePage 模式：没给 scaleHandler 时恒 1.0（这里是自定义 handler，仍生效）。
    auto wholePage = cui::make_shared<PhotoViewSlidePage>(
        block(400.0, 300.0, kCyan), Color(0.0, 0.0, 0.0, 1.0), 0.25, PhotoViewSlideAxis::Vertical,
        PhotoViewSlideType::WholePage, nullptr, nullptr, nullptr, nullptr, 500.0, nullptr, true,
        nullptr);
    Harness h3;
    h3.attach(pin(400.0, 300.0, wholePage));
    PhotoViewSlidePageState* w3 = h3.slide();
    w3->setOffset(Offset(0.0, 150.0));
    h3.pump();
    check(near(w3->currentScale(), 1.0), "12: WholePage ⇒ 缺省不缩放内容");
    check(near(w3->ratio(), 0.5), "12: WholePage 也算比例（背景跟着淡）");

    // enableSlideOutPage=false ⇒ 不挂 Listener，拖不动。
    auto frozen = cui::make_shared<PhotoViewSlidePage>(block(400.0, 300.0, kCyan),
                                                       Color(0.0, 0.0, 0.0, 1.0), 0.25,
                                                       PhotoViewSlideAxis::Vertical,
                                                       PhotoViewSlideType::OnlyImage, nullptr,
                                                       nullptr, nullptr, nullptr, 500.0, nullptr,
                                                       false, nullptr);
    Harness h4;
    h4.attach(pin(400.0, 300.0, frozen));
    PhotoViewSlidePageState* f4 = h4.slide();
    check(countRenderObjects<RenderPointerListener>(h4.root->findRenderObject()) == 0,
          "12: enableSlideOutPage=false ⇒ 树里没有 Listener");
    h4.down(1, 200.0, 100.0);
    h4.move(1, 200.0, 250.0);
    h4.up(1, 200.0, 250.0);
    check(near(f4->slideOffset(), Offset()) && !f4->isDragging(), "12: 关掉滑出 ⇒ 拖不动");

    // 真关页：路由里拖过阈值 ⇒ Navigator.pop。
    NavHarness nav;
    nav.attach();
    nav.pushBase(pin(400.0, 300.0, block(400.0, 300.0, kLime)));
    check(nav.nav && nav.nav->history().size() == 1, "12: 基础页已入栈");
    auto route = makePhotoViewSlideRoute(pin(400.0, 300.0, slide), 60.0);
    nav.nav->push(route);
    check(nav.until([&] { return route->controller()->isCompleted(); }), "12: 滑出页入场完成");
    PhotoViewSlidePageState* live = nav.slide();
    check(live != nullptr, "12: 路由里找到滑出页状态");
    const size_t historyBefore = nav.nav->history().size();
    nav.down(1, 200.0, 50.0);
    nav.move(1, 200.0, 200.0);  // dy = 150 ⇒ ratio 0.5
    check(near(live->slideOffset().dy, 150.0), "12: 路由里拖得动（外层就是它）");
    nav.up(1, 200.0, 200.0);
    check(nav.until([&] { return nav.nav->history().size() < historyBefore; }),
          "12: 越过阈值 ⇒ 经 Navigator 关页（history 变短）");
}

// ── 13. 监听摘除（铁律）─────────────────────────────────────────────────────

void testListenerTeardown() {
    std::printf("[test] 13. 监听摘除：挂载 2+1 条、卸载归零\n");

    auto ctrl = std::make_shared<PhotoViewController>();
    auto stateCtrl = std::make_shared<PhotoViewScaleStateController>();
    check(ctrl->listenerCount() == 0 && stateCtrl->listenerCount() == 0,
          "13: 新控制器没有监听");

    PVArgs args;
    args.customChild = block(400.0, 300.0, kCyan);
    args.childSize = Size(400.0, 300.0);
    args.controller = ctrl;
    args.scaleController = stateCtrl;
    Harness h;
    h.attach(pin(200.0, 150.0, mkPhoto(args)));
    check(ctrl->listenerCount() == 2, "13: 挂载后 controller 上两条订阅（重建 + ignorable）");
    check(stateCtrl->listenerCount() == 1, "13: 挂载后状态控制器一条订阅");

    // 外部写控制器 ⇒ 会排重建。上游 `_blindScaleListener` 的第一件事就是
    // `controller.position = clampPosition()`（非 enablePanAlways 时）：这里内容
    // 400×300 在 200×150 的框里 contained = 0.5 ⇒ 恰好铺满 ⇒ 平移夹回 0。
    ctrl->setPosition(Offset(1.0, 1.0));
    h.pump();
    check(h.view() && near(h.view()->position(), Offset()),
          "13: 外部写 position ⇒ 立刻被夹回边界内（上游同款）");

    // enablePanAlways ⇒ 不夹，外部写原样落地。
    PVArgs panAlways = args;
    panAlways.controller = std::make_shared<PhotoViewController>();
    panAlways.scaleController = std::make_shared<PhotoViewScaleStateController>();
    panAlways.enablePanAlways = true;
    Harness hPan;
    hPan.attach(pin(200.0, 150.0, mkPhoto(panAlways)));
    panAlways.controller->setPosition(Offset(1.0, 1.0));
    hPan.pump();
    check(hPan.view() && near(hPan.view()->position(), Offset(1.0, 1.0)),
          "13: enablePanAlways ⇒ 外部写落到位");

    // 换成内部控制器 ⇒ 旧的订阅必须摘干净。
    PVArgs owned = args;
    owned.controller = nullptr;
    owned.scaleController = nullptr;
    h.setBody(pin(200.0, 150.0, mkPhoto(owned)));
    check(ctrl->listenerCount() == 0 && stateCtrl->listenerCount() == 0,
          "13: 摘掉外部控制器后旧订阅归零");

    // 再挂回来，然后整棵子树拆掉 ⇒ 归零（ASan 树的真牙齿）。
    h.setBody(pin(200.0, 150.0, mkPhoto(args)));
    check(ctrl->listenerCount() == 2 && stateCtrl->listenerCount() == 1, "13: 重新认领成功");
    h.setBody(nullptr);
    check(ctrl->listenerCount() == 0 && stateCtrl->listenerCount() == 0,
          "13: 卸载后监听清零（不会打悬挂 State）");

    // 画廊：页面控制器由画廊持有 ⇒ 子树拆掉后也不留监听。
    std::vector<PhotoViewGalleryPageOptions> pages = {
        pageOption(kCyan, 800.0, 600.0), pageOption(kLime, 800.0, 600.0),
    };
    Harness h2;
    h2.attach(cui::make_shared<PhotoViewGallery>(pages));
    PhotoViewGalleryState* g = h2.gallery();
    if (!g)
        return;
    PhotoViewControllerPtr page0 = g->controllerFor(0);
    check(page0->listenerCount() == 2, "13: 画廊第 0 页的控制器被页面订阅");
    h2.setBody(nullptr);
    check(page0->listenerCount() == 0, "13: 画廊卸载后页面控制器不再挂监听");
}

int main() {
    std::printf("== PhotoView / PhotoViewGallery / 滑出关闭页 ==\n");
    installSeams(true, true);

    testBoundariesMath();
    testControllers();
    testMount();
    testDoubleTapCycle();
    testPanAndClamp();
    testPinchAndRotation();
    testWheel();
    testCallbacks();
    testContentModes();
    testGallery();
    testGalleryAxisAndPixels();
    testSlidePage();
    testListenerTeardown();

    resetSeams();
    std::printf("%s: %d\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
