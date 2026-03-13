/// @file test_extended_image.cpp
/// @brief ExtendedImage 控件单元测试（占位 / 进度 / 失败兜底 / 缓存 / 动画 / 手势）。
///
/// 绘制链复用 `cui_core::Image`，所以本测试只钉控件层这五件事：
///   1. 同步出图（decoded provider）：第一次 build 就有画面，
///      `wasSynchronouslyLoaded=true`，且**不会**因此多排一次重建。
///   2. 异步出图：loadingBuilder 先在飞 → 进度 → 结果落地；errorBuilder 接住失败原因。
///   3. 换源与缓存：`cacheWidth` 进签名 ⇒ 真的重解；`gaplessPlayback` 决定
///      换源时旧画面留不留。
///   4. 动图播放：Ticker 推帧、`animate=false` 停表、`repeatCount` 播完停末帧。
///   5. 拆除安全：dispose 取消在飞请求，结果闭包再也不打悬挂 State（ASan 树的真牙齿）。
///
/// 宿主驱动方式：每次 `setBody` 都交一棵**新建的**控件。本仓 `updateChild` 对
/// 「同一个 Widget 实例」直接短路（不 update 也不重建，见 element.cpp:246），
/// 原地改配置的对象永远走不到 `didUpdateWidget` —— 上游宿主本来也是每帧新建配置
/// 对象，这里保持同一口径。
///
/// 线程接缝换成可手动排空的队列，除动图那段（Ticker 走真实时钟）外全部确定。

#include <chrono>
#include <cmath>
#include <cstdio>
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

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/image_provider.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/extended_image.h"

#ifndef CUI_SAMPLE_GIF
#define CUI_SAMPLE_GIF ""
#endif

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

constexpr SkColor kWhite = 0xFFFFFFFF;

// ── 素材 ────────────────────────────────────────────────────────────────────

/// 左半红、右半蓝的 PNG（与 test_image_core 同一张）。
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

// 读像素按 RGBA8888，字节序与平台无关：低字节 = R。
bool isRedish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return r > b;
}
bool isBlueish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return b > r;
}

// ── 线程接缝：可手动排空 ────────────────────────────────────────────────────

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

int runBg() {
    std::vector<ImageTask> tasks;
    tasks.swap(bgQueue());
    for (ImageTask& t : tasks)
        t();
    return int(tasks.size());
}

/// 排空 UI 队列；`oneOnly` 时只跑队首一条（用于逐帧观察进度）。
int runUi(bool oneOnly = false) {
    int ran = 0;
    while (!uiQueue().empty()) {
        ImageTask t = uiQueue().front();
        uiQueue().erase(uiQueue().begin());
        t();
        ++ran;
        if (oneOnly)
            break;
    }
    return ran;
}

void resetSeams() {
    setBackgroundExecutor({});
    setUiDispatcher({});
    setHttpTransport({});
    bgQueue().clear();
    uiQueue().clear();
}

// ── 被测控件的构造配方 ──────────────────────────────────────────────────────

struct Spec {
    std::shared_ptr<ImageProvider> provider;
    BoxFit fit = BoxFit::Contain;
    std::optional<double> width, height;
    std::optional<int> cacheWidth;
    bool gapless = false;
    bool animate = true;
    int repeatCount = -1;
    bool useMemoryCache = true;
    bool useDiskCache = true;
};

SharedPtr<ExtendedImage> mk(const Spec& spec) {
    return cui::make_shared<ExtendedImage>(spec.provider, spec.fit, Alignment::center(),
                                           spec.width, spec.height, spec.cacheWidth, std::nullopt,
                                           /*decodeAllFrames=*/true, spec.useMemoryCache,
                                           spec.useDiskCache, spec.gapless, spec.animate,
                                           spec.repeatCount);
}

/// 换一张不同 name 的内存图：全局解码缓存按 key 存，唯一 name 才能保证走异步路径。
Spec memorySpec(const sk_sp<SkData>& png, const std::string& name) {
    Spec s;
    s.provider = std::make_shared<MemoryImageProvider>(toBytes(png), name);
    return s;
}

// ── 宿主探针：根恒为 Align，换 body 会走子树的 didUpdateWidget ───────────────

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

// ── 帧泵 ────────────────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    SchedulerBinding sb;
    Element* root = nullptr;
    HostState* host = nullptr;
    sk_sp<SkSurface> surface;

    void attach(double ww = 400, double hh = 400) {
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
    }

    /// 只跑到布局：状态类断言用，省掉每帧软件光栅。
    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
    }

    /// 跑完整帧（含绘制）并合成到离屏画布，之后可以读像素。
    void frame() {
        sb.runPipelineAndComputeDamage();
        surface->getCanvas()->clear(kWhite);
        sb.compositeTo(surface->getCanvas());
    }

    void setBody(SharedPtr<Widget> body) {
        host->setBody(std::move(body));
        pump();
    }

    void setSpec(const Spec& spec) { setBody(mk(spec)); }

    ExtendedImageState* image() const {
        return static_cast<ExtendedImageState*>(findStateNamed(root, "ExtendedImage"));
    }

    /// 真实时钟推进一步并让所有活动 Ticker 出 tick（等价 handleBeginFrame）。
    void advance(double ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds((long)ms));
        Ticker::tickAllActive(Duration(0));
    }

    void down(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        pump();
    }
    void up(double x, double y) {
        PointerUpEvent u;
        u.position = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }
    void tap(double x, double y) {
        down(x, y);
        up(x, y);
    }

    uint32_t at(int x, int y) const {
        const SkImageInfo info = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        uint32_t px = 0;
        surface->readPixels(info, &px, 4, x, y);
        return px;
    }
};

}  // namespace

int main() {
    std::printf("== ExtendedImage ==\n");

    const sk_sp<SkData> png = makePng(200, 100);
    const sk_sp<SkData> png2 = makePng(120, 120);

    // ── 1. 同步出图（decoded provider）──────────────────────────────────────
    installSeams(true, true);
    {
        int ready = 0, builds = 0, syncFlags = 0;
        Spec spec;
        spec.provider = std::make_shared<DecodedImageProvider>(decodeImage(png), "s1");
        auto img = mk(spec);
        img->setFrameBuilder([&](BuildContext&, const SharedPtr<Widget>& child, int, bool wasSync) {
            ++builds;
            if (wasSync)
                ++syncFlags;
            return child;
        });
        img->setOnReady([&ready](const ImageLoadResult&) { ++ready; });

        Harness h;
        h.attach();
        h.setBody(img);
        h.frame();

        ExtendedImageState* st = h.image();
        check(st != nullptr, "1: 找到了 ExtendedImageState");
        check(st && st->loadState() == ExtendedImageLoadState::Completed, "1: 首次 build 即 Completed");
        check(builds == 1, "1: 同步结果没有额外排一次重建");
        check(syncFlags == 1, "1: wasSynchronouslyLoaded=true");
        check(ready == 1, "1: onReady 恰好一次");
        check(st && st->frameImage() != nullptr, "1: 当前帧有位图");
        check(st && st->error().empty(), "1: 没有错误信息");
        // Align(-1,-1) 把图钉在左上角：200x100。
        check(isRedish(h.at(25, 50)), "1: 左半画的是红色");
        check(isBlueish(h.at(175, 50)), "1: 右半画的是蓝色");
        check(h.at(300, 300) == 0xFFFFFFFFu, "1: 图外的像素没被画");
    }

    // ── 2. 异步出图：loading → completed ────────────────────────────────────
    installSeams(false, false);
    {
        int loadingSeen = 0, ready = 0;
        Spec spec = memorySpec(png, "s2");
        auto img = mk(spec);
        img->setLoadingBuilder([&](BuildContext&, double) {
            ++loadingSeen;
            return cui::make_shared<SizedBox>(8.0, 8.0);
        });
        img->setOnReady([&ready](const ImageLoadResult&) { ++ready; });

        Harness h;
        h.attach();
        h.setBody(img);
        ExtendedImageState* st = h.image();
        check(st && st->loadState() == ExtendedImageLoadState::Loading, "2: 后台没跑 ⇒ Loading");
        check(loadingSeen == 1, "2: loadingBuilder 出过一次画面");
        check(st && st->progress() < 0.0, "2: 未知进度报 -1");

        check(runBg() == 1, "2: 后台任务排了一条");
        check(st && st->loadState() == ExtendedImageLoadState::Loading,
              "2: 后台跑完但没投递 UI 仍是 Loading");
        check(runUi() >= 1, "2: UI 投递了结果");
        h.pump();
        check(st && st->loadState() == ExtendedImageLoadState::Completed, "2: 投递后 Completed");
        check(ready == 1, "2: onReady 在异步路径上被调");
        check(st && st->frameImage() && st->frameImage()->width() == 200, "2: 出图尺寸是原图 200");
    }

    // ── 3. 进度分档送达 ─────────────────────────────────────────────────────
    installSeams(false, false);
    {
        std::vector<double> seen;
        int calls = 0;
        const std::vector<uint8_t> bytes = toBytes(png);
        setHttpTransport([&, bytes](const std::string&, const HttpHeaders&,
                                   std::vector<uint8_t>& out,
                                   const std::function<void(const HttpProgress&)>& onProgress,
                                   const std::shared_ptr<ImageCancellation>&, std::string&) {
            ++calls;
            onProgress(HttpProgress{40000, 100000});
            onProgress(HttpProgress{80000, 100000});
            onProgress(HttpProgress{100000, 100000});
            out = bytes;
            return true;
        });

        Spec spec;
        spec.provider = std::make_shared<NetworkImageProvider>("https://invalid.invalid/s3.png");
        spec.useMemoryCache = false;
        spec.useDiskCache = false;
        auto img = mk(spec);
        img->setLoadingBuilder([&](BuildContext&, double progress) {
            seen.push_back(progress);
            return cui::make_shared<SizedBox>(8.0, 8.0);
        });

        Harness h;
        h.attach();
        h.setBody(img);
        ExtendedImageState* st = h.image();
        check(st && st->loadState() == ExtendedImageLoadState::Loading, "3: 下载中");
        runBg();
        // 队列里此刻是 3 条进度 + 1 条结果；逐条排空并各自重建一次。
        while (runUi(true) > 0)
            h.pump();
        check(calls == 1, "3: 传输层只被调一次");
        check(seen.size() >= 4, "3: loadingBuilder 见过 -1 与三档进度");
        bool saw04 = false, saw08 = false, sawNeg = false;
        for (double v : seen) {
            if (v < 0.0)
                sawNeg = true;
            if (std::fabs(v - 0.4) < 1e-9)
                saw04 = true;
            if (std::fabs(v - 0.8) < 1e-9)
                saw08 = true;
        }
        check(sawNeg && saw04 && saw08, "3: -1 / 0.4 / 0.8 都送达了（进度不只是「有/没有」）");
        check(st && st->loadState() == ExtendedImageLoadState::Completed, "3: 最终 Completed");
        resetSeams();
    }

    // ── 4. 失败兜底 ─────────────────────────────────────────────────────────
    installSeams(false, false);
    {
        std::string err;
        int errCalls = 0, shown = 0;
        Spec spec;
        spec.provider =
            std::make_shared<MemoryImageProvider>(std::vector<uint8_t>{1, 2, 3, 4, 5}, "s4-bad");
        auto img = mk(spec);
        img->setErrorBuilder([&](BuildContext&, const std::string& e) {
            ++shown;
            err = e;
            return cui::make_shared<SizedBox>(6.0, 6.0);
        });
        img->setOnError([&](const std::string&) { ++errCalls; });

        Harness h;
        h.attach();
        h.setBody(img);
        ExtendedImageState* st = h.image();
        check(st && st->loadState() == ExtendedImageLoadState::Loading, "4: 坏数据也先在飞");
        runBg();
        runUi();
        h.pump();
        check(st && st->loadState() == ExtendedImageLoadState::Failed, "4: 解码失败 ⇒ Failed");
        check(st && st->frameImage() == nullptr, "4: 失败时没有画面");
        check(shown >= 1, "4: errorBuilder 被调用");
        check(!err.empty(), "4: errorBuilder 收到了失败原因");
        check(errCalls == 1, "4: onError 恰好一次");
    }

    // ── 5. gaplessPlayback：换源时旧画面留不留 ──────────────────────────────
    installSeams(false, false);
    {
        Spec a = memorySpec(png, "s5-a");
        Spec b = memorySpec(png2, "s5-b");
        b.gapless = true;
        Harness h;
        h.attach();
        h.setSpec(a);
        runBg();
        runUi();
        h.pump();
        ExtendedImageState* st = h.image();
        sk_sp<SkImage> first = st ? st->frameImage() : nullptr;
        check(st && first && first->width() == 200, "5: 第一张图已出");

        h.setSpec(b);
        check(st && st->loadState() == ExtendedImageLoadState::Loading, "5: 换源后仍在飞");
        check(st && st->frameImage() == first, "5: gaplessPlayback=true ⇒ 旧画面还在");
        h.frame();
        check(isRedish(h.at(25, 50)), "5: 旧画面真的还在画（没闪白）");
        runBg();
        runUi();
        h.pump();
        check(st && st->frameImage() != first, "5: 新图落地后换成新画面");
        check(st && st->frameImage() && st->frameImage()->width() == 120, "5: 新图是 120 宽");
    }
    {
        Spec c = memorySpec(png, "s5-c");
        Spec d = memorySpec(png2, "s5-d");
        Harness h;
        h.attach();
        h.setSpec(c);
        runBg();
        runUi();
        h.pump();
        ExtendedImageState* st = h.image();
        check(st && st->frameImage() != nullptr, "5: 对照组的图也出了");
        h.setSpec(d);
        check(st && st->frameImage() == nullptr, "5: 默认（非 gapless）⇒ 旧画面立刻清空");
        // 把这条在飞请求排空：残留的后台任务会漏进 §8，把「取消后还剩几条」读错。
        runBg();
        runUi();
        h.pump();
    }

    // ── 6. cacheWidth 进签名 ⇒ 真的重解，且是另一条缓存条目 ─────────────────
    installSeams(true, true);
    {
        Spec small = memorySpec(png, "s6");
        small.cacheWidth = 100;
        Spec full = memorySpec(png, "s6");
        Harness h;
        h.attach();
        h.setSpec(small);
        ExtendedImageState* st = h.image();
        check(st && st->frameImage() && st->frameImage()->width() == 100,
              "6: cacheWidth=100 解出 100 宽");
        h.setSpec(full);
        check(st && st->frameImage() && st->frameImage()->width() == 200,
              "6: 去掉 cacheWidth 后重解成 200 宽");
    }

    // ── 7. 动图播放 ─────────────────────────────────────────────────────────
    installSeams(true, true);
    {
        if (std::string(CUI_SAMPLE_GIF).empty()) {
            std::printf("  [SKIP] 7: 没有 sample.gif\n");
        } else {
            auto gif = decodeImageFromFile(CUI_SAMPLE_GIF);
            check(gif && gif->animated && gif->frames.size() > 1, "7: 素材确实是多帧动画");
            const int frames = gif ? int(gif->frames.size()) : 0;

            Spec spec;
            spec.provider = std::make_shared<DecodedImageProvider>(gif, "s7");
            int frameEvents = 0;
            auto img = mk(spec);
            img->setOnFrameChanged([&](int) { ++frameEvents; });

            Harness h;
            h.attach();
            h.setBody(img);
            ExtendedImageState* st = h.image();
            check(st && st->isAnimating(), "7: 挂载后在播");
            check(st && st->frame() == 0, "7: 初始停在第 0 帧");

            int guard = 0;
            while (st && st->frame() == 0 && guard++ < 40) {
                h.advance(50);
                h.pump();
            }
            check(st && st->frame() != 0, "7: 真实时钟推进后换了帧");
            check(frameEvents > 0, "7: onFrameChanged 跟着帧号发");

            Spec paused = spec;
            paused.animate = false;
            h.setSpec(paused);
            check(st && !st->isAnimating(), "7: animate=false ⇒ 停表");
            const int held = st ? st->frame() : -1;
            for (int i = 0; i < 4 && st; ++i) {
                h.advance(80);
                h.pump();
            }
            check(st && st->frame() == held, "7: 停表后帧号不动");

            Spec once = spec;
            once.repeatCount = 1;
            h.setSpec(once);
            check(st && st->isAnimating(), "7: 恢复播放");
            guard = 0;
            while (st && st->isAnimating() && guard++ < 200) {
                h.advance(50);
                h.pump();
            }
            check(st && !st->isAnimating(), "7: repeatCount=1 播完自动停");
            check(st && st->frame() == frames - 1, "7: 停在末帧");
        }
    }

    // ── 8. 拆除安全：dispose 取消在飞请求 ───────────────────────────────────
    installSeams(false, false);
    {
        Spec spec = memorySpec(png, "s8");
        {
            Harness h;
            h.attach();
            h.setSpec(spec);
            check(h.image() && h.image()->loadState() == ExtendedImageLoadState::Loading,
                  "8: 卸载前仍在飞");
            // h 在此析构：Element 树卸载 ⇒ State::dispose ⇒ request->cancel()
        }
        check(runBg() == 1, "8: 后台任务照跑（取消不砍已排定的取字节）");
        check(runUi() >= 1, "8: UI 投递照排");
        check(inflightImageRequests() == 0, "8: 分组已从在飞表摘掉");
    }

    // ── 9. onTap / onLongPress 接线 ─────────────────────────────────────────
    installSeams(true, true);
    {
        int taps = 0, longs = 0;
        Spec spec = memorySpec(png, "s9");
        auto img = mk(spec);
        img->setOnTap([&] { ++taps; });
        img->setOnLongPress([&] { ++longs; });
        Harness h;
        h.attach();
        h.setBody(img);

        h.tap(25, 50);
        check(taps == 1, "9: 快按出 tap");
        check(longs == 0, "9: 快按不出 longPress");

        h.down(25, 50);
        h.advance(600);
        h.pump();
        h.up(25, 50);
        check(longs == 1, "9: 按住 600ms 出 longPress");
        check(taps == 1, "9: 这一次按下不再出 tap");

        // 图外（300,300）没有可命中节点 ⇒ 两个回调都不该动
        h.tap(300, 300);
        check(taps == 1 && longs == 1, "9: 图外点击不触发回调");
    }

    resetSeams();
    std::printf(g_failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
