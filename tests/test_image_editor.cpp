/// @file test_image_editor.cpp
/// @brief ImageEditor（对齐 extended_image v3.10.0 的 editor 层：editor.dart +
///        crop_layer.dart + editor_utils.dart）单元测试。
///
/// 覆盖：
///   1. 几何小工具与 EditActionDetails 口径：contain 铺图公式、getRectWithScale、
///      computeBoundary 的「上一帧标志」两帧语义、cropAspectRatio 的半 π 对调、
///      旋转角度归一化、两个翻转 = 180° 那条等价。
///   2. 挂载：outerSize / layoutRect / 初始裁剪框 / 图像矩形 / totalScale=1 / 树形
///      （LayoutBuilder→Listener→CustomPaint）/ onEditChanged 计数。
///   3. 八个把手的命中热区（真实指针注入走 hit-test）。
///   4. 把手拖动：TopLeft 内拖、最小框 60×60、裁剪框跑不出图、pointerDown/activeHandle。
///   5. 比例锁定：1:1 初始框与拖角、original(0.0)、换比例作废旧编辑。
///   6. 归中补间：延迟帧不起表、t=0/0.5/1 逐帧插值、**像素框不变量（所见即所得）**。
///   7. 拖图平移：delta 搬运 + 撞边界夹取 + totalScale 不动（文件头第 7 条）。
///   8. 滚轮缩放：系数公式 1+dy·speed/1000、maxScale 上限、reverse 开关。
///   9. 90° 步进旋转：裁剪框按长短边重铺、烘焙图宽高互换、出图上下半颜色。
///  10. 翻转：水平/垂直镜像的像素证据、flip() 的单键语义。
///  11. 出图：cropRectInPixels 换算、produceImage 尺寸与颜色、savePng 落盘再解码。
///  12. 绘制：蒙层/图像/旋转后的像素位置。
///  13. 占位与拆除：loading/error builder、异步出图、拆除后 Ticker 停表。
///
/// headless 帧泵；归中补间一律用 `advanceAutoCenter(ms)` 直推（Ticker 走真实时钟）。
/// 出图侧（`cropRectInPixels`/`produceImage`）自己会收敛图像矩形，所以不绘制也能出图。

#include <algorithm>
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
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/image_provider.h"
#include "cui/foundation/key.h"
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
#include "cui/widgets/image_editor.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"

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
bool sameRect(const Rect& a, const Rect& b, double eps = 1e-6) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.width, b.width, eps) &&
           near(a.height, b.height, eps);
}
std::string desc(const Rect& r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.5f,%.5f,%.5f,%.5f)", r.x, r.y, r.width, r.height);
    return buf;
}
std::string desc(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const SkColor kWhite = SK_ColorWHITE;

// ── 素材 ────────────────────────────────────────────────────────────────────

/// 左半红、右半蓝的 PNG（旋转/水平镜像看得出来的那种）。
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

/// 上红下蓝（垂直镜像专属：左右分布的图上下翻是看不出来的）。
sk_sp<SkData> makePngTB(int w, int h) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeLTRB(0.0f, 0.0f, float(w), float(h / 2)), paint);
    paint.setColor(SK_ColorBLUE);
    canvas->drawRect(SkRect::MakeLTRB(0.0f, float(h / 2), float(w), float(h)), paint);
    return encodePng(surface->makeImageSnapshot());
}

/// 四象限：左上红 / 右上绿 / 左下蓝 / 右下黄（翻转 + 旋转的组合唯一确定）。
sk_sp<SkData> makePngQuad(int w, int h) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeXYWH(0.0f, 0.0f, float(w / 2), float(h / 2)), paint);
    paint.setColor(SK_ColorGREEN);
    canvas->drawRect(SkRect::MakeXYWH(float(w / 2), 0.0f, float(w / 2), float(h / 2)), paint);
    paint.setColor(SK_ColorBLUE);
    canvas->drawRect(SkRect::MakeXYWH(0.0f, float(h / 2), float(w / 2), float(h / 2)), paint);
    paint.setColor(SK_ColorYELLOW);
    canvas->drawRect(SkRect::MakeXYWH(float(w / 2), float(h / 2), float(w / 2), float(h / 2)),
                     paint);
    return encodePng(surface->makeImageSnapshot());
}

std::vector<uint8_t> toBytes(const sk_sp<SkData>& data) {
    const uint8_t* p = static_cast<const uint8_t*>(data->data());
    return std::vector<uint8_t>(p, p + data->size());
}

DecodedImagePtr pngLR() {
    static DecodedImagePtr img = decodeImage(makePng(200, 100));
    return img;
}
DecodedImagePtr pngTB() {
    static DecodedImagePtr img = decodeImage(makePngTB(200, 100));
    return img;
}
DecodedImagePtr pngQuad() {
    static DecodedImagePtr img = decodeImage(makePngQuad(200, 100));
    return img;
}

uint32_t pixelAt(const sk_sp<SkImage>& img, int x, int y) {
    const SkImageInfo info = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    uint32_t px = 0;
    if (img)
        img->readPixels(info, &px, 4, x, y);
    return px;
}
uint8_t chR(uint32_t c) { return uint8_t(c & 0xFF); }
uint8_t chG(uint32_t c) { return uint8_t((c >> 8) & 0xFF); }
uint8_t chB(uint32_t c) { return uint8_t((c >> 16) & 0xFF); }
bool isRedPx(uint32_t c) { return chR(c) > 200 && chG(c) < 60 && chB(c) < 60; }
bool isGreenPx(uint32_t c) { return chG(c) > 200 && chR(c) < 60 && chB(c) < 60; }
bool isBluePx(uint32_t c) { return chB(c) > 200 && chR(c) < 60 && chG(c) < 60; }
bool isYellowPx(uint32_t c) { return chR(c) > 200 && chG(c) > 200 && chB(c) < 60; }
bool isDarkPx(uint32_t c) { return chR(c) < 40 && chG(c) < 40 && chB(c) < 40; }

/// 纯色块（占位 builder 用）。
SharedPtr<Widget> block(double w, double h, const Color& color) {
    BoxDecoration dec;
    dec.color = color;
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(dec));
}
const Color kLime(0.4, 0.9, 0.2, 1.0);

/// 定尺外框：给编辑器一个确切的盒子。
SharedPtr<Widget> pin(double w, double h, SharedPtr<Widget> inner) {
    return cui::make_shared<SizedBox>(w, h, std::move(inner));
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

/// 沿渲染链统计某个渲染类型出现次数（验自绘层/Listener 插没插）。
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
        // LayoutBuilder 打头：子树在布局期才建；同步出图的 initGeometry 也在那一次
        // 构建里跑，多泵几帧到稳态。
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

    ImageEditorState* editor() const {
        return dynamic_cast<ImageEditorState*>(findStateNamed(root, "ImageEditor"));
    }

    // 指针事件：position 给全局点，本地坐标由分发器按节点变换（编辑器钉在窗口左上，
    // 两者数值相同）。
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

// ── 配方 ────────────────────────────────────────────────────────────────────

struct IEArgs {
    std::shared_ptr<ImageProvider> provider;
    DecodedImagePtr decoded;
    ImageEditorConfig config;
    ImageEditor::LoadingBuilder loadingBuilder;
    ImageEditor::ErrorBuilder errorBuilder;
    ImageEditor::EditChangedCallback onEditChanged;
};

/// 每帧新建实例：宿主是原地改同一个 Widget 对象的，测试要走 didUpdateWidget 的更新
/// 路径就必须换实例（同一个实例会被短路）。
SharedPtr<Widget> mkEditor(const IEArgs& a) {
    return cui::make_shared<ImageEditor>(a.provider, a.config, a.decoded, a.loadingBuilder,
                                        a.errorBuilder, a.onEditChanged, nullptr);
}

int nextIndex() {
    static int i = 0;
    return ++i;
}
const char* nextKey() {
    static std::string buf;
    buf = "ie-" + std::to_string(nextIndex());
    return buf.c_str();
}

/// 挂一个「200×100 左红右蓝」的编辑器（默认配置：padding 20 ⇒ layoutRect 20,20,360,260）。
ImageEditorState* mountEditor(Harness& h, const IEArgs& base, DecodedImagePtr image) {
    IEArgs a = base;
    a.provider = std::make_shared<DecodedImageProvider>(std::move(image), nextKey());
    h.attach(pin(400.0, 300.0, mkEditor(a)));
    return h.editor();
}

const char* kHandleNames[] = {"None", "Pan",     "TopLeft", "TopRight", "BottomRight",
                              "BottomLeft", "Top", "Right",   "Bottom",   "Left"};
const char* handleName(ImageCropHandle h) { return kHandleNames[int(h)]; }

// ── 1. 几何小工具与 EditActionDetails 口径 ──────────────────────────────────

void testGeometryAndAction() {
    std::printf("[test] 1. 几何小工具与 EditActionDetails 口径\n");

    check(near(clampImageEditorValue(5.0, 0.0, 3.0), 3.0) &&
              near(clampImageEditorValue(-1.0, 0.0, 3.0), 0.0) &&
              near(clampImageEditorValue(2.0, 0.0, 3.0), 2.0),
          "1: clampImageEditorValue 三段");
    check(imageEditorNear(1e-10, 0.0) && !imageEditorNear(1e-8, 0.0),
          "1: 误差容差是 1e-9（不是上游的 2e-12）");

    check(sameRect(rectFromPoints({10.0, 20.0}, {30.0, 5.0}), Rect(10.0, 5.0, 20.0, 15.0)),
          "1: rectFromPoints 对角点无序 ⇒ " + desc(rectFromPoints({10.0, 20.0}, {30.0, 5.0})));
    check(sameRect(rectFromCenter({10.0, 10.0}, 4.0, 6.0), Rect(8.0, 7.0, 4.0, 6.0)),
          "1: rectFromCenter");
    check(sameRect(rectShift(Rect(1.0, 2.0, 3.0, 4.0), {5.0, -1.0}), Rect(6.0, 1.0, 3.0, 4.0)),
          "1: rectShift");
    check(rectIsSame(Rect(0.0, 0.0, 10.0, 10.0), Rect(1e-12, 0.0, 10.0, 10.0 + 5e-10)),
          "1: rectIsSame 容差内算同一个框");
    check(rectBeyond(Rect(5.0, 5.0, 10.0, 10.0), Rect(0.0, 0.0, 12.0, 12.0)) &&
              !rectBeyond(Rect(1.0, 1.0, 5.0, 5.0), Rect(0.0, 0.0, 12.0, 12.0)),
          "1: rectBeyond 只要有一条边露出去");
    check(sameRect(rectExpandToInclude(Rect(0.0, 0.0, 10.0, 10.0), Rect(5.0, 5.0, 10.0, 10.0)),
                   Rect(0.0, 0.0, 15.0, 15.0)),
          "1: rectExpandToInclude 外包矩形");
    check(sameRect(deflateRect(EdgeInsets::all(20.0), Rect(0.0, 0.0, 400.0, 300.0)),
                   Rect(20.0, 20.0, 360.0, 260.0)),
          "1: deflateRect 内缩 padding ⇒ " +
              desc(deflateRect(EdgeInsets::all(20.0), Rect(0.0, 0.0, 400.0, 300.0))));

    const Rect layout(20.0, 20.0, 360.0, 260.0);
    const Rect fit = imageEditorDestinationRect(layout, Size(200.0, 100.0));
    check(sameRect(fit, Rect(20.0, 60.0, 360.0, 180.0)),
          "1: contain 铺图（长边贴满、短边居中）⇒ " + desc(fit));
    const Rect tl = imageEditorDestinationRect(layout, Size(200.0, 100.0), 1.0, BoxFit::Contain,
                                               Alignment(-1.0, -1.0));
    check(sameRect(tl, Rect(20.0, 20.0, 360.0, 180.0)),
          "1: alignment=topLeft ⇒ 贴左上（halfDelta + a·halfDelta 公式）" + desc(tl));
    const Rect scaled = imageEditorDestinationRect(layout, Size(200.0, 100.0), 2.0);
    check(sameRect(scaled, Rect(20.0, 60.0, 360.0, 180.0)),
          "1: scale=2 只把源尺寸除一半，Contain 又按短边撑回贴满 ⇒ 同一个矩形 " + desc(scaled));

    ImageEditAction a;
    a.totalScale = 2.0;
    check(sameRect(a.getRectWithScale(Rect(20.0, 60.0, 360.0, 180.0)),
                   Rect(-160.0, -30.0, 720.0, 360.0)),
          "1: getRectWithScale 以中心放大 ⇒ " + desc(a.getRectWithScale(Rect(20.0, 60.0, 360.0, 180.0))));

    // computeBoundary 的「上一次状态」语义：第一帧只记标志，第二帧才夹。
    ImageEditAction b;
    const Rect frame(0.0, 0.0, 100.0, 100.0);
    const Rect r1 = b.computeBoundary(Rect(-10.0, 10.0, 120.0, 20.0), frame);
    check(sameRect(r1, Rect(-10.0, 10.0, 120.0, 20.0)),
          "1: computeBoundary 首帧不动（标志位还没立起来）" + desc(r1));
    const Rect r2 = b.computeBoundary(Rect(10.0, 10.0, 120.0, 20.0), frame);
    check(sameRect(r2, Rect(0.0, 10.0, 120.0, 20.0)) && !near(r1.x, r2.x),
          "1: 次帧横向锁定生效 ⇒ left 夹到 0（纵向仍自由）" + desc(r2));

    // 旋转角度与比例对调。
    ImageEditAction c;
    c.cropRect = Rect(10.0, 20.0, 60.0, 20.0);
    c.setRawCropAspectRatio(2.0);
    check(near(*c.cropAspectRatio(), 2.0), "1: 未旋转 ⇒ 比例原样");
    c.rotate(ImageEditAction::kPi / 2.0, Rect(0.0, 0.0, 100.0, 100.0));
    check(c.isHalfPi() && !c.isTwoPi() && !c.isPi(), "1: 90° ⇒ isHalfPi");
    check(near(c.rotateAngle(), 90.0), "1: rotateAngle = 90 ⇒ " + desc(c.rotateAngle()));
    check(near(*c.cropAspectRatio(), 0.5), "1: 转过 90° ⇒ 长短边对调（2 → 0.5）");
    check(sameRect(*c.cropRect, Rect(100.0 / 3.0, 0.0, 100.0 / 3.0, 100.0), 1e-5),
          "1: 旋转后按对调的长短边（20×60 → 高边贴满 100）重铺 ⇒ " + desc(*c.cropRect));
    check(!c.screenDestinationRect().has_value(), "1: 旋转作废图像矩形（下一帧重算）");
    c.rotate(ImageEditAction::kPi / 2.0, Rect(0.0, 0.0, 100.0, 100.0));
    check(c.isPi() && near(c.rotateAngle(), 180.0) && near(*c.cropAspectRatio(), 2.0),
          "1: 180° ⇒ isPi、比例换回来");
    c.rotate(-ImageEditAction::kPi / 2.0, Rect(0.0, 0.0, 100.0, 100.0));
    c.normalize();
    check(c.isHalfPi() && near(c.rotateRadian(), ImageEditAction::kPi / 2.0),
          "1: 累加和 180-90=90 已经是正角 ⇒ normalize 不改值");
    check(near(c.rotateAngle(), 90.0), "1: rotateAngle 回到 90 ⇒ " + desc(c.rotateAngle()));

    // 真正的负角只有从 0 起转才留得下符号（fmod 保留被除数符号，Dart 的 % 恒非负）。
    ImageEditAction neg;
    neg.cropRect = Rect(10.0, 20.0, 60.0, 20.0);
    neg.rotate(-ImageEditAction::kPi / 2.0, Rect(0.0, 0.0, 100.0, 100.0));
    check(near(neg.rotateRadian(), -ImageEditAction::kPi / 2.0),
          "1: 未 normalize ⇒ rotateRadian = -90°");
    check(near(neg.rotateAngle(), -90.0),
          "1: 上游 `~/ (pi/2) * 90` 在 C++ 里是 floor ⇒ 负角得 -90 ⇒ " + desc(neg.rotateAngle()));
    neg.normalize();
    check(neg.isHalfPi() && !neg.isPi() && !neg.isTwoPi() &&
              near(neg.rotateRadian(), 3.0 * ImageEditAction::kPi / 2.0),
          "1: 负角 normalize 到 [0,2π) ⇒ 270°");
    check(near(neg.rotateAngle(), 270.0), "1: rotateAngle 跟着到 270");

    // 翻转：两个翻转 + 180° = 什么都没做（上游那条等价）。
    ImageEditAction d;
    d.cropRect = Rect(0.0, 0.0, 40.0, 20.0);
    d.rotate(ImageEditAction::kPi, Rect(0.0, 0.0, 100.0, 100.0));
    d.setScreenDestinationRect(Rect(0.0, 0.0, 40.0, 20.0));
    d.flipHorizontalNow();
    check(d.flipHorizontal() && !d.flipVertical() && d.hasEditAction() && d.needFlip(),
          "1: 水平镜像只置水平位");
    d.flipVerticalNow();
    check(!d.flipHorizontal() && !d.flipVertical() && near(d.rotateRadian(), 0.0),
          "1: 180° 时两个镜像互相抵消（回到无编辑）");
    ImageEditAction e;
    e.cropRect = Rect(0.0, 0.0, 40.0, 20.0);
    e.setScreenDestinationRect(Rect(0.0, 0.0, 40.0, 20.0));
    e.flip();
    check(e.flipHorizontal() && !e.flipVertical(), "1: flip() 未旋转 ⇒ 水平翻（文件头第 3 条）");
    e.rotate(ImageEditAction::kPi / 2.0, Rect(0.0, 0.0, 100.0, 100.0));
    e.setScreenDestinationRect(Rect(0.0, 0.0, 40.0, 20.0));
    e.flip();
    check(e.flipVertical() , "1: flip() 转过 90° ⇒ 改翻垂直");
}

// ── 2. 挂载与初始几何 ───────────────────────────────────────────────────────

void testMount() {
    std::printf("[test] 2. 挂载：layoutRect / 初始裁剪框 / 图像矩形 / 树形\n");

    int changed = 0;
    IEArgs args;
    args.onEditChanged = [&](const ImageEditAction&) { ++changed; };
    Harness h;
    ImageEditorState* st = mountEditor(h, args, pngLR());
    check(st != nullptr, "2: 找到 ImageEditorState");
    if (!st)
        return;

    check(near(st->outerSize().width, 400.0) && near(st->outerSize().height, 300.0),
          "2: outerSize = 外框 400×300");
    check(sameRect(st->layoutRect(), Rect(20.0, 20.0, 360.0, 260.0)),
          "2: layoutRect = cropRectPadding.deflate(盒子) ⇒ " + desc(st->layoutRect()));
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "2: 初始裁剪框 = contain 铺图（200×100 在 360×260 里）⇒ " +
              desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(st->action().screenDestinationRect() &&
              sameRect(*st->action().screenDestinationRect(), Rect(20.0, 60.0, 360.0, 180.0)),
          "2: 首帧就收敛出图像矩形（不用先绘制）⇒ " +
              desc(st->action().screenDestinationRect() ? *st->action().screenDestinationRect()
                                                       : Rect()));
    check(near(st->action().totalScale, 1.0) && near(st->action().preTotalScale, 1.0),
          "2: totalScale/preTotalScale 起始 1");
    check(!st->action().hasEditAction() && !st->action().needFlip(), "2: 起始没有任何编辑");
    check(near(st->progress(), 1.0) && st->error().empty(), "2: 同步送达 ⇒ progress=1、无错误");
    check(st->decodedImage() && st->decodedImage()->width() == 200, "2: 图已就位 200×100");
    check(changed == 0, "2: 挂载本身不发 onEditChanged ⇒ " + desc(double(changed)));

    RenderObject* ro = h.root->findRenderObject();
    check(countRenderObjects<RenderPointerListener>(ro) == 1, "2: 树里恰好一个 Listener");
    check(countRenderObjects<RenderCustomPaint>(ro) == 1, "2: 树里恰好一个自绘层");

    // 出图侧不依赖绘制：没有 frame() 也能拿到像素框。
    const auto px = st->cropRectInPixels();
    check(px && sameRect(*px, Rect(0.0, 0.0, 200.0, 100.0), 1e-4),
          "2: 裁剪框 → 像素框 = 整图 ⇒ " + desc(px ? *px : Rect()));
}

// ── 3. 八个把手的命中热区 ───────────────────────────────────────────────────

void probeHandle(Harness& h, ImageEditorState* st, double x, double y, ImageCropHandle want,
                 const std::string& label) {
    h.down(1, x, y);
    check(st->activeHandle() == want,
          label + " @" + std::to_string(int(x)) + "," + std::to_string(int(y)) + " ⇒ " +
              handleName(want) + "（实收 " + handleName(st->activeHandle()) + "）");
    check(st->pointerDown(), label + ": 按下态");
    h.up(1, x, y);
    check(st->activeHandle() == ImageCropHandle::None, label + ": 抬手后把手归零");
}

void testHandleHits() {
    std::printf("[test] 3. 八个把手的热区（hitTestSize=20 ⇒ 热区 40×40）\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;
    // 裁剪框 (20,60)-(380,240)。
    probeHandle(h, st, 10.0, 50.0, ImageCropHandle::TopLeft, "3: 左上角");
    probeHandle(h, st, 390.0, 50.0, ImageCropHandle::TopRight, "3: 右上角");
    probeHandle(h, st, 390.0, 250.0, ImageCropHandle::BottomRight, "3: 右下角");
    probeHandle(h, st, 10.0, 250.0, ImageCropHandle::BottomLeft, "3: 左下角");
    probeHandle(h, st, 10.0, 150.0, ImageCropHandle::Left, "3: 左边条");
    probeHandle(h, st, 390.0, 150.0, ImageCropHandle::Right, "3: 右边条");
    probeHandle(h, st, 200.0, 50.0, ImageCropHandle::Top, "3: 上边条");
    probeHandle(h, st, 200.0, 250.0, ImageCropHandle::Bottom, "3: 下边条");
    probeHandle(h, st, 200.0, 150.0, ImageCropHandle::Pan, "3: 框内拖图");
    probeHandle(h, st, 5.0, 5.0, ImageCropHandle::Pan, "3: 框外也拖图（文件头第 1 条）");

    // 边条与角的分界：热区半开区间 ⇒ 360 这条线归角。
    h.down(1, 359.0, 60.0);
    check(st->activeHandle() == ImageCropHandle::Top,
          std::string("3: 边条/角分界 (359,60) ⇒ Top（实收 ") + handleName(st->activeHandle()) + "）");
    h.up(1, 359.0, 60.0);
    h.down(1, 361.0, 60.0);
    check(st->activeHandle() == ImageCropHandle::TopRight,
          std::string("3: 边条/角分界 (361,60) ⇒ TopRight（实收 ") +
              handleName(st->activeHandle()) + "）");
    h.up(1, 361.0, 60.0);
}

// ── 4. 把手拖动 / 最小框 / 不许跑出图 ───────────────────────────────────────

void testHandleDrag() {
    std::printf("[test] 4. 把手拖动：TopLeft / 最小 60×60 / 夹在图内\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;

    // TopLeft 往里拖：热区起点 (10,50) ⇒ 位移 (50,60)。
    h.down(1, 10.0, 50.0);
    h.move(1, 60.0, 110.0);
    check(st->activeHandle() == ImageCropHandle::TopLeft, "4: 拖的是 TopLeft");
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(70.0, 120.0, 310.0, 120.0)),
          "4: 裁剪框跟着角走 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(near(st->action().totalScale, 1.0),
          "4: 单纯改框不动 totalScale ⇒ " + desc(st->action().totalScale));
    h.up(1, 60.0, 110.0);
    check(st->isCenterAnimating() == false, "4: 抬手只「排队」归中（补间还没起）");

    // 出界：往左上猛拖，只能拖到图的边界（= 图像矩形）。
    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, IEArgs{}, pngLR());
    if (!s2)
        return;
    h2.down(1, 10.0, 50.0);
    h2.move(1, 0.0, 40.0);
    check(s2->cropRect() && sameRect(*s2->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "4: 裁剪框跑不出图（夹回图像矩形）⇒ " + desc(s2->cropRect() ? *s2->action().cropRect : Rect()));
    h2.up(1, 0.0, 40.0);

    // 最小框：cornerSize.width × 2 = 60（文件头第 5 条，不是 hitTestSize）。
    Harness h3;
    ImageEditorState* s3 = mountEditor(h3, IEArgs{}, pngLR());
    if (!s3)
        return;
    h3.down(1, 10.0, 50.0);
    h3.move(1, 350.0, 230.0);
    check(s3->cropRect() && sameRect(*s3->action().cropRect, Rect(320.0, 180.0, 60.0, 60.0)),
          "4: 拖过界只留最小框 60×60 ⇒ " + desc(s3->cropRect() ? *s3->action().cropRect : Rect()));
    // 小框时热区互相重叠 ⇒ 命中顺序（= 上游逆绘制序）让边条压过角。
    h3.down(2, 380.0, 230.0);
    check(s3->activeHandle() == ImageCropHandle::Right,
          std::string("4: 小框重叠带 (380,230) ⇒ Right 压过 BottomRight（实收 ") +
              handleName(s3->activeHandle()) + "）");
    h3.up(2, 380.0, 230.0);
    h3.down(2, 370.0, 170.0);
    check(s3->activeHandle() == ImageCropHandle::Top,
          std::string("4: 小框重叠带 (370,170) ⇒ Top 压过 TopRight（实收 ") +
              handleName(s3->activeHandle()) + "）");
    h3.up(2, 370.0, 170.0);
    h3.up(1, 350.0, 230.0);
}

// ── 5. 比例锁定 ─────────────────────────────────────────────────────────────

void testAspectRatio() {
    std::printf("[test] 5. 比例锁定：1:1 / original / 换比例作废旧编辑\n");

    IEArgs square;
    square.config.cropAspectRatio = CropAspectRatios::kRatio1_1;
    Harness h;
    ImageEditorState* st = mountEditor(h, square, pngLR());
    if (!st)
        return;
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(110.0, 60.0, 180.0, 180.0)),
          "5: 1:1 初始框在铺图矩形里居中 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));

    // 铺图矩形只有 360×180 ⇒ 1:1 的框已经吃满图高（180×180），往外拖顶不动；
    // 往里拖才是真的缩，且永远是正方形（TopRight 锚在左下角）。
    h.down(1, 290.0, 55.0);
    check(st->activeHandle() == ImageCropHandle::TopRight, "5: 拖的是 TopRight");
    h.move(1, 250.0, 95.0);
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(110.0, 100.0, 140.0, 140.0)),
          "5: 锁 1:1 ⇒ 拖角只出正方形 " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    h.move(1, 330.0, 15.0);
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(110.0, 60.0, 180.0, 180.0)),
          "5: 想拖成 220×220 被图高 180 夹回 ⇒ 停在最大正方形 " +
              desc(st->cropRect() ? *st->action().cropRect : Rect()));
    h.up(1, 330.0, 15.0);

    // 换比例 ⇒ 已做的编辑整体作废，按新比例重铺。
    st->setCropAspectRatio(CropAspectRatios::kRatio4_3);
    h.settle();
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(80.0, 60.0, 240.0, 180.0)),
          "5: 换 4:3 ⇒ 作废重铺 " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(near(st->action().totalScale, 1.0), "5: 作废后 totalScale 回 1");

    st->setCropAspectRatio(std::nullopt);
    h.settle();
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "5: 取消锁定 ⇒ 铺满铺图矩形 " + desc(st->cropRect() ? *st->action().cropRect : Rect()));

    // original（0.0）= 图片自身宽高比 = 2 ⇒ 恰好铺满（图就是 2:1）。
    IEArgs orig;
    orig.config.cropAspectRatio = CropAspectRatios::kOriginal;
    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, orig, pngLR());
    if (!s2)
        return;
    check(s2->cropRect() && sameRect(*s2->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "5: original 比例用图片宽高比 2 ⇒ " + desc(s2->cropRect() ? *s2->action().cropRect : Rect()));
    check(near(*s2->action().originalAspectRatio, 2.0),
          "5: originalAspectRatio 记的是图的自然比 ⇒ " + desc(*s2->action().originalAspectRatio));

    // initCropRectType=LayoutRect + 锁比例：框铺满布局矩形，图像撑到同样大小。
    IEArgs byLayout;
    byLayout.config.cropAspectRatio = CropAspectRatios::kRatio1_1;
    byLayout.config.initCropRectType = ImageInitCropRectType::LayoutRect;
    Harness h3;
    ImageEditorState* s3 = mountEditor(h3, byLayout, pngLR());
    if (!s3)
        return;
    // 上游那条走 `_calculateCropRectFromAspectRatio(layoutRect)`：边长夹成 260 后
    // **按布局矩形的居中**放 ⇒ x = 20 + (360-260)/2 = 70，不是贴左。
    check(s3->cropRect() && sameRect(*s3->action().cropRect, Rect(70.0, 20.0, 260.0, 260.0)),
          "5: LayoutRect + 1:1 ⇒ 布局矩形里的最大正方形（居中） " +
              desc(s3->cropRect() ? *s3->action().cropRect : Rect()));
    check(near(s3->action().totalScale, 260.0 / 180.0, 1e-9) &&
              near(s3->action().preTotalScale, s3->action().totalScale),
          "5: 那条 totalScale 用铺图矩形的长短边比 ⇒ " + desc(s3->action().totalScale));
}

// ── 6. 归中补间 + 像素框不变量 ──────────────────────────────────────────────

void testAutoCenter() {
    std::printf("[test] 6. 归中补间：延迟 / 逐帧插值 / 像素框不变量\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;
    const auto beforeUp = st->cropRectInPixels();
    check(beforeUp.has_value(), "6: 抬手前拿得到像素框");

    h.down(1, 10.0, 50.0);
    h.move(1, 60.0, 110.0);
    const std::optional<Rect> dragged = st->cropRect();
    const std::optional<Rect> pxDragged = st->cropRectInPixels();
    check(dragged && sameRect(*dragged, Rect(70.0, 120.0, 310.0, 120.0)),
          "6: 拖动后的框 " + desc(dragged ? *dragged : Rect()));
    h.up(1, 60.0, 110.0);

    st->advanceAutoCenter(0.0);
    st->advanceAutoCenter(399.0);
    check(st->cropRect() && sameRect(*st->action().cropRect, *dragged),
          "6: tickerMs(400) 之前一动不动 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(!st->isCenterAnimating(), "6: 延迟没到 ⇒ 还没起表");
    st->advanceAutoCenter(400.0);  // t = 0：起表那一帧就是起点
    check(st->isCenterAnimating(), "6: 到 tickerMs 那帧起表");
    check(st->cropRect() && sameRect(*st->action().cropRect, *dragged, 1e-4),
          "6: t=0 仍是拖动的框 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));

    st->advanceAutoCenter(500.0);  // t = 0.5
    const Rect mid(45.0, 100.1612903225806, 335.0, 129.6774193548387);
    check(st->cropRect() && sameRect(*st->action().cropRect, mid, 1e-4),
          "6: 中段 = 两端线性插值 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(near(st->action().totalScale, 335.0 / 310.0, 1e-6),
          "6: 框放大多少，图就跟着缩放多少 ⇒ " + desc(st->action().totalScale));

    st->advanceAutoCenter(600.0);  // t = 1
    const Rect target(20.0, 80.3225806451613, 360.0, 139.3548387096774);
    check(st->cropRect() && sameRect(*st->action().cropRect, target, 1e-4),
          "6: 收敛到「保持比例的居中最大框」⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(near(st->action().totalScale, 360.0 / 310.0, 1e-6),
          "6: 最终 totalScale = 360/310 ⇒ " + desc(st->action().totalScale));
    check(!st->isCenterAnimating(), "6: 收敛后停表");

    // 所见即所得：框在屏幕上又搬又放大，选中的像素区域一个字没变。
    const std::optional<Rect> pxAfter = st->cropRectInPixels();
    check(pxDragged && pxAfter && sameRect(*pxDragged, *pxAfter, 1e-4),
          "6: 归中前后像素框不变（" + desc(pxDragged ? *pxDragged : Rect()) + " vs " +
              desc(pxAfter ? *pxAfter : Rect()) + "）");

    // 框大到跑出布局：不起补间，同一条归中公式一次到位（上游 moveUpdate 里那条
    // `if (result.beyond(layoutRect)) { cropRect = result; _doCropAutoCenterAnimation…}`）。
    // 前提是图像比布局大 ⇒ 先用滚轮把图放大，让 clamp 的上下界（= 图像矩形）超出布局。
    Harness h3;
    ImageEditorState* s3 = mountEditor(h3, IEArgs{}, pngLR());
    if (!s3)
        return;
    h3.scroll(200.0, 150.0, 1000.0);
    check(near(s3->action().totalScale, 2.0), "6: 先放大到 2 倍");
    // headless 没有绘制帧 ⇒ 图像矩形只有出图侧会收敛，手动补一次（真实宿主每帧
    // paintLayer 都会收敛，这里只是测试口径）。
    const std::optional<Rect> zoomed = s3->cropRectInPixels();
    check(zoomed && sameRect(*zoomed, Rect(50.0, 25.0, 100.0, 50.0), 1e-3),
          "6: 放大后像素框 ⇒ " + desc(zoomed ? *zoomed : Rect()));
    h3.down(1, 380.0, 240.0);
    check(s3->activeHandle() == ImageCropHandle::BottomRight,
          std::string("6: 角点收到 ") + handleName(s3->activeHandle()));
    h3.move(1, 500.0, 300.0);  // 右下角往外拽 ⇒ 框 480×240，跑出布局
    check(s3->cropRect() && sameRect(*s3->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "6: 大框当场收回布局内的居中框 ⇒ " + desc(s3->cropRect() ? *s3->action().cropRect : Rect()));
    check(near(s3->action().totalScale, 1.5, 1e-9),
          "6: 居中把 480 宽压回 360 ⇒ totalScale 2×0.75 ⇒ " + desc(s3->action().totalScale));
    const auto& d3 = s3->action().screenDestinationRect();
    check(d3 && sameRect(*d3, Rect(-115.0, -7.5, 540.0, 270.0), 1e-6),
          "6: 图像跟着同一公式走 ⇒ " + desc(d3 ? *d3 : Rect()));
    const std::optional<Rect> px3 = s3->cropRectInPixels();
    check(px3 && sameRect(*px3, Rect(50.0, 25.0, 133.3333, 66.6667), 1e-3),
          "6: 拽大的那截确实多选了像素（框本来就变大）⇒ " + desc(px3 ? *px3 : Rect()));
    h3.up(1, 500.0, 300.0);
}

// ── 7. 拖图平移 ─────────────────────────────────────────────────────────────

void testPan() {
    std::printf("[test] 7. 拖图：搬运移 / 撞边界夹取 / 不动缩放（文件头第 7 条）\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;
    // 框 == 图 ⇒ 平移会被夹死，先滚轮放大让图比框大。
    h.scroll(200.0, 150.0, 1000.0);
    check(near(st->action().totalScale, 2.0), "7: 滚轮 1000 ⇒ totalScale 2");
    // headless 没有绘制帧，图像矩形只有出图侧（cropRectInPixels）会收敛一次。
    const auto pxZoomed = st->cropRectInPixels();
    check(pxZoomed && sameRect(*pxZoomed, Rect(50.0, 25.0, 100.0, 50.0), 1e-3),
          "7: 放大后像素框（光标 200,150 为锚）⇒ " + desc(pxZoomed ? *pxZoomed : Rect()));

    h.down(1, 200.0, 150.0);
    check(st->activeHandle() == ImageCropHandle::Pan, "7: 框内按下 = 拖图");
    h.move(1, 240.0, 150.0);
    check(near(st->action().delta.dx, 40.0) && near(st->action().delta.dy, 0.0),
          "7: 位移先记在 delta 上 ⇒ " + desc(st->action().delta.dx) + "," +
              desc(st->action().delta.dy));
    st->cropRectInPixels();  // 收敛：delta 搬运到图像矩形上
    const auto& dest = st->action().screenDestinationRect();
    check(dest && near(dest->left(), -120.0) && near(dest->top(), -30.0),
          "7: 图像矩形跟着搬 40px ⇒ " + desc(dest ? *dest : Rect()));
    check(near(st->action().totalScale, 2.0),
          "7: 拖图不改 totalScale（上游 speed≠1 时会错）⇒ " + desc(st->action().totalScale));
    check(near(st->action().delta.dx, 0.0) && near(st->action().delta.dy, 0.0),
          "7: 搬运完的 delta 当场清空");

    // 一路向右拖到撞边：图比框宽 ⇒ 左边界贴在框左边。
    h.move(1, 395.0, 150.0);
    st->cropRectInPixels();
    check(dest && near(dest->left(), 20.0),
          "7: 撞右边界 ⇒ 图像左边夹在裁剪框左边 ⇒ " + desc(dest ? *dest : Rect()));
    check(near(st->action().delta.dx, 0.0),
          "7: 夹完把 delta 清空 ⇒ " + desc(st->action().delta.dx));
    h.up(1, 395.0, 150.0);
}

// ── 8. 滚轮缩放 ─────────────────────────────────────────────────────────────

void testWheelZoom() {
    std::printf("[test] 8. 滚轮：1+dy·speed/1000 / maxScale 上限 / reverse\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;
    h.scroll(200.0, 150.0, 500.0);
    check(near(st->action().totalScale, 1.5),
          "8: dy=500 ⇒ scale 1.5 ⇒ " + desc(st->action().totalScale));
    h.scroll(200.0, 150.0, -200.0);
    // 两次滚轮各自走 scaleStart+scaleUpdate：startingScale 每次都取当前值。
    check(near(st->action().totalScale, 1.5 * 0.8, 1e-9),
          "8: 再外滚 -200 ⇒ 0.8 倍 ⇒ " + desc(st->action().totalScale));

    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, IEArgs{}, pngLR());
    if (!s2)
        return;
    h2.scroll(200.0, 150.0, 5000.0);
    check(near(s2->action().totalScale, 5.0), "8: maxScale=5 夹住 ⇒ " + desc(s2->action().totalScale));
    const double capped = s2->action().totalScale;
    h2.scroll(200.0, 150.0, 1000.0);
    check(near(s2->action().totalScale, capped),
          "8: 顶到上限后再内滚一动不动 ⇒ " + desc(s2->action().totalScale));
    // 到顶后基准被改写（correct _startingScale），下一次外滚立刻生效。
    // 注意上游只用 dy·speed/1000 换算倍数、没有下限：dy=-1000 ⇒ scale=0 ⇒ totalScale=0，
    // 这个退化值本仓也原样保留（不断言）。
    h2.scroll(200.0, 150.0, -500.0);
    check(near(s2->action().totalScale, 2.5),
          "8: 到顶后内滚 -500 从当前 5 起算 ⇒ " + desc(s2->action().totalScale));

    IEArgs reversed;
    reversed.config.reverseMousePointerScrollDirection = true;
    Harness h3;
    ImageEditorState* s3 = mountEditor(h3, reversed, pngLR());
    if (!s3)
        return;
    h3.scroll(200.0, 150.0, 500.0);
    check(near(s3->action().totalScale, 0.5),
          "8: reverse ⇒ 同一下滚改成缩小 " + desc(s3->action().totalScale));

    IEArgs fast;
    fast.config.speed = 2.0;
    Harness h4;
    ImageEditorState* s4 = mountEditor(h4, fast, pngLR());
    if (!s4)
        return;
    h4.scroll(200.0, 150.0, 500.0);
    // speed 被乘了两遍（上游 ei_editor.dart:229/235 原样）：滚轮先把 dy 换算成
    // `1 + dy·speed/1000 = 2`，_handleScaleUpdate 又 `startingScale·scale·speed`。
    check(near(s4->action().totalScale, 4.0),
          "8: speed=2 的滚轮是 2 的两倍（上游双重乘 speed）⇒ " + desc(s4->action().totalScale));
}

// ── 9. 旋转 ─────────────────────────────────────────────────────────────────

void testRotate() {
    std::printf("[test] 9. 90° 步进旋转：长短边重铺 / 烘焙互换 / 出图颜色\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;
    st->rotate();  // 右转正 90°
    h.settle();
    check(near(st->action().rotateAngle(), 90.0), "9: rotateAngle = 90");
    check(st->action().hasEditAction() && st->action().hasRotateAngle(), "9: 有编辑动作");
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(135.0, 20.0, 130.0, 260.0)),
          "9: 裁剪框按新的长短边重铺 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    check(st->action().screenDestinationRect() &&
              sameRect(*st->action().screenDestinationRect(), Rect(135.0, 20.0, 130.0, 260.0)),
          "9: 图像矩形立刻重算（applyEdit 里收敛一次）⇒ " +
              desc(st->action().screenDestinationRect() ? *st->action().screenDestinationRect()
                                                        : Rect()));
    const auto px = st->cropRectInPixels();
    check(px && sameRect(*px, Rect(0.0, 0.0, 100.0, 200.0), 1e-4),
          "9: 像素框跟着变成 100×200 ⇒ " + desc(px ? *px : Rect()));
    const sk_sp<SkImage> out = st->produceImage();
    check(out && out->width() == 100 && out->height() == 200,
          "9: 出图尺寸互换（200×100 → 100×200）");
    if (out) {
        check(isRedPx(pixelAt(out, 50, 10)) && isRedPx(pixelAt(out, 50, 90)),
              "9: 顺时针 90° ⇒ 原图左半边跑到上边");
        check(isBluePx(pixelAt(out, 50, 110)) && isBluePx(pixelAt(out, 50, 190)),
              "9: 原图右半边跑到下边");
    }

    st->rotate();
    h.settle();
    check(st->action().isPi() && near(st->action().rotateAngle(), 180.0), "9: 两转 = 180°");
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "9: 180° 后盒子回到原方向 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    const sk_sp<SkImage> half = st->produceImage();
    check(half && half->width() == 200 && half->height() == 100, "9: 180° 出图 200×100");
    if (half) {
        check(isBluePx(pixelAt(half, 5, 50)) && isRedPx(pixelAt(half, 195, 50)),
              "9: 180° ⇒ 红蓝左右对调");
    }

    st->rotate();
    st->rotate();
    h.settle();
    check(st->action().isTwoPi() && !st->action().hasRotateAngle(),
          "9: 四转回到 0° ⇒ 不再需要旋转");
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "9: 四次旋转后框复原 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    const sk_sp<SkImage> full = st->produceImage();
    check(full && isRedPx(pixelAt(full, 5, 50)) && isBluePx(pixelAt(full, 195, 50)),
          "9: 转满一圈 = 原图");

    // 左转 + 连续步进。
    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, IEArgs{}, pngLR());
    if (!s2)
        return;
    s2->rotate(false);
    h2.settle();
    check(near(s2->action().rotateAngle(), 270.0),
          "9: 左转 90° normalize 后 rotateAngle = 270 ⇒ " + desc(s2->action().rotateAngle()));
    check(s2->action().isHalfPi(), "9: 270° 也算 isHalfPi（比例对调）");
    const sk_sp<SkImage> ccw = s2->produceImage();
    check(ccw && ccw->width() == 100 && ccw->height() == 200, "9: 左转同样宽高互换");
    if (ccw) {
        check(isBluePx(pixelAt(ccw, 50, 10)) && isRedPx(pixelAt(ccw, 50, 190)),
              "9: 逆时针 90° ⇒ 原图右半边跑到上边");
    }
}

// ── 10. 翻转 ────────────────────────────────────────────────────────────────

void testFlip() {
    std::printf("[test] 10. 水平/垂直翻转（烘进位图后的所见即所得）\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngQuad());
    if (!st)
        return;
    const sk_sp<SkImage> raw = st->produceImage();
    check(raw && isRedPx(pixelAt(raw, 10, 10)) && isGreenPx(pixelAt(raw, 190, 10)) &&
              isBluePx(pixelAt(raw, 10, 90)) && isYellowPx(pixelAt(raw, 190, 90)),
          "10: 原图四象限 红/绿/蓝/黄");

    st->flipHorizontal();
    h.settle();
    check(st->action().flipHorizontal() && !st->action().flipVertical(), "10: 只置水平位");
    const sk_sp<SkImage> fh = st->produceImage();
    check(fh && isGreenPx(pixelAt(fh, 10, 10)) && isRedPx(pixelAt(fh, 190, 10)) &&
              isYellowPx(pixelAt(fh, 10, 90)) && isBluePx(pixelAt(fh, 190, 90)),
          "10: 水平镜像 ⇒ 左右象限成对调");
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(20.0, 60.0, 360.0, 180.0)),
          "10: 翻转不改裁剪框 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));

    st->flipHorizontal();
    h.settle();
    check(!st->action().needFlip(), "10: 再翻一次回到无翻转");
    const sk_sp<SkImage> back = st->produceImage();
    check(back && isRedPx(pixelAt(back, 10, 10)), "10: 两次镜像互相抵消");

    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, IEArgs{}, pngQuad());
    if (!s2)
        return;
    s2->flipVertical();
    h2.settle();
    const sk_sp<SkImage> fv = s2->produceImage();
    check(fv && isBluePx(pixelAt(fv, 10, 10)) && isYellowPx(pixelAt(fv, 190, 10)) &&
              isRedPx(pixelAt(fv, 10, 90)) && isGreenPx(pixelAt(fv, 190, 90)),
          "10: 垂直镜像 ⇒ 上下象限对调");

    // 上红下蓝的图：只有垂直翻才看得出变化。
    Harness h3;
    ImageEditorState* s3 = mountEditor(h3, IEArgs{}, pngTB());
    if (!s3)
        return;
    s3->flipHorizontal();
    h3.settle();
    const sk_sp<SkImage> lr = s3->produceImage();
    check(lr && isRedPx(pixelAt(lr, 100, 5)) && isBluePx(pixelAt(lr, 100, 95)),
          "10: 上红下蓝的图水平翻 ⇒ 上下不变（翻转轴各自独立）");
    s3->flipVertical();
    h3.settle();
    const sk_sp<SkImage> ud = s3->produceImage();
    check(ud && isBluePx(pixelAt(ud, 100, 5)) && isRedPx(pixelAt(ud, 100, 95)),
          "10: 再垂直翻 ⇒ 上下对调（两个翻转同时生效）");

    // 旋转 + 翻转的组合：翻转先烘、再旋转（与上游矩阵顺序一致）。
    Harness h4;
    ImageEditorState* s4 = mountEditor(h4, IEArgs{}, pngQuad());
    if (!s4)
        return;
    s4->rotate();
    s4->flip();  // 转过 90° ⇒ flip() 走垂直翻（图像空间）
    h4.settle();
    check(s4->action().flipVertical() && near(s4->action().rotateAngle(), 90.0),
          "10: 旋转态下 flip() 翻的是图像空间的垂直轴");
    const sk_sp<SkImage> combo = s4->produceImage();
    check(combo && combo->width() == 100 && combo->height() == 200, "10: 组合出图 100×200");
    if (combo) {
        check(isRedPx(pixelAt(combo, 10, 10)) && isBluePx(pixelAt(combo, 90, 10)),
              "10: 组合后左上=红、右上=蓝");
        check(isGreenPx(pixelAt(combo, 10, 190)) && isYellowPx(pixelAt(combo, 90, 190)),
              "10: 组合后左下=绿、右下=黄");
    }
}

// ── 11. 出图与落盘 ──────────────────────────────────────────────────────────

void testProduceAndSave() {
    std::printf("[test] 11. cropRectInPixels / produceImage / savePng\n");

    Harness h;
    ImageEditorState* st = mountEditor(h, IEArgs{}, pngLR());
    if (!st)
        return;
    // 下边条往里拖 45px：裁剪框 (20,60,360,135) ⇒ 像素框 (0,0,200,75)。
    h.down(1, 200.0, 230.0);
    check(st->activeHandle() == ImageCropHandle::Bottom, "11: 拖的是下边条");
    h.move(1, 200.0, 185.0);
    check(st->cropRect() && sameRect(*st->action().cropRect, Rect(20.0, 60.0, 360.0, 135.0)),
          "11: 框缩到 360×135 ⇒ " + desc(st->cropRect() ? *st->action().cropRect : Rect()));
    const auto px = st->cropRectInPixels();
    check(px && sameRect(*px, Rect(0.0, 0.0, 200.0, 75.0), 1e-4),
          "11: 像素框 (0,0,200,75) ⇒ " + desc(px ? *px : Rect()));
    const sk_sp<SkImage> out = st->produceImage();
    check(out && out->width() == 200 && (out->height() == 75 || out->height() == 76),
          "11: 出图 200×75（ceil 之后 intersect 到图内）");
    if (out) {
        check(isRedPx(pixelAt(out, 10, 10)) && isBluePx(pixelAt(out, 190, 10)),
              "11: 出图像素就是屏幕上那块");
    }

    // 落盘用本地临时目录：VMware 共享目录的写回时机不定，紧接着回读会拿到 0 字节
    // （Skia 对空文件直接报 cannot read file），那和本仓代码无关。
    const char* tmpDir = std::getenv("TEMP");
    if (!tmpDir)
        tmpDir = std::getenv("TMPDIR");
    const std::string path =
        (tmpDir ? std::string(tmpDir) + "/" : std::string()) + "cui_test_image_editor_out.png";
    check(st->savePng(path), "11: savePng 写盘成功");
    const DecodedImagePtr reread = decodeImageFromFile(path);
    check(reread && reread->valid() && reread->width() == 200,
          "11: 落盘的 PNG 能重新解码回 200 宽（" + (reread ? reread->error : std::string("null")) +
              "）");
    if (reread && reread->valid()) {
        check(reread->height() >= 75 && reread->height() <= 76,
              "11: 高度 75~76 ⇒ " + std::to_string(reread->height()));
        const sk_sp<SkImage> img = reread->image();
        check(img && isRedPx(pixelAt(img, 10, 10)) && isBluePx(pixelAt(img, 190, 10)),
              "11: 重读后颜色分布一致");
    }
    std::remove(path.c_str());

    // 旋转后的出图走「烘焙图」坐标：裁剪框换算用的是烘焙后的尺寸（文件头第 2 条）。
    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, IEArgs{}, pngLR());
    if (!s2)
        return;
    s2->rotate();
    h2.settle();
    h2.down(1, 200.0, 270.0);  // Bottom 热区（框 (135,20,130,260) ⇒ 下热区 y∈[260,300)）
    check(s2->activeHandle() == ImageCropHandle::Bottom,
          std::string("11: 旋转后拖的是 ") + handleName(s2->activeHandle()));
    h2.move(1, 200.0, 250.0);
    const auto px2 = s2->cropRectInPixels();
    // 框 (135,20,130,240) 在图像矩形 (135,20,130,260) 里 ⇒ 100×200 的烘焙图取满宽、
    // 高度 240/260 ⇒ 184.6154。
    check(px2 && sameRect(*px2, Rect(0.0, 0.0, 100.0, 184.6154), 1e-3),
          "11: 旋转后拖 Bottom 的像素框 ⇒ " + desc(px2 ? *px2 : Rect()));
    const sk_sp<SkImage> rotated = s2->produceImage();
    check(rotated && rotated->width() == 100 && rotated->height() == 185,
          "11: 旋转 + 裁剪出图 100×185（floor/ceil 取整）");
    if (rotated) {
        check(isRedPx(pixelAt(rotated, 50, 5)), "11: 出图顶部仍是原图左半（红）");
    }
}

// ── 12. 绘制 ────────────────────────────────────────────────────────────────

void testPaint() {
    std::printf("[test] 12. 绘制：蒙层遮住框外 / 图按图像矩形落位 / 旋转后跟着走\n");

    IEArgs args;
    args.config.maskColor = Color(0.0, 0.0, 0.0, 1.0);  // 不透明黑，好和图区分
    Harness h;
    ImageEditorState* st = mountEditor(h, args, pngLR());
    if (!st)
        return;
    h.frame();
    check(isDarkPx(h.at(30, 30)), "12: 框外（padding 带）被蒙层盖住");
    check(isDarkPx(h.at(10, 150)), "12: 框左边一条蒙层（裁剪框从 x=20 起）");
    check(isRedPx(h.at(100, 150)), "12: 框内左侧是原图左半（红）");
    check(isBluePx(h.at(300, 150)), "12: 框内右侧是原图右半（蓝）");

    st->rotate();
    h.settle();
    h.frame();
    check(isDarkPx(h.at(30, 30)), "12: 旋转后左侧仍是蒙层（框缩到 135..265）");
    check(isRedPx(h.at(200, 80)), "12: 旋转后框内上段是红（原图左半转上来）");
    check(isBluePx(h.at(200, 200)), "12: 旋转后框内下段是蓝");

    // 按下才画三分线；这里只验绘制不炸 + 蒙层颜色随按下变淡（默认 maskColor 那条）。
    IEArgs plain;
    Harness h2;
    ImageEditorState* s2 = mountEditor(h2, plain, pngLR());
    if (!s2)
        return;
    h2.frame();
    const uint32_t idle = h2.at(30, 30);
    h2.down(1, 200.0, 150.0);
    h2.frame();
    const uint32_t pressed = h2.at(30, 30);
    check(idle != pressed,
          "12: 按下后蒙层透明度 0.8 → 0.4（同一像素变了）");
    h2.up(1, 200.0, 150.0);
}

// ── 13. 占位与拆除 ──────────────────────────────────────────────────────────

void testPlaceholders() {
    std::printf("[test] 13. loading/error 占位 / 异步出图 / 拆除停表\n");

    int errors = 0;
    std::string why;
    IEArgs bad;
    bad.provider = std::make_shared<MemoryImageProvider>(std::vector<uint8_t>{9, 8, 7}, "ie13-bad");
    bad.errorBuilder = [&](BuildContext&, const std::string& e) {
        ++errors;
        why = e;
        return block(20.0, 20.0, kLime);
    };
    Harness h;
    h.attach(pin(400.0, 300.0, mkEditor(bad)));
    ImageEditorState* st = h.editor();
    check(st != nullptr, "13: 失败态照样挂载");
    check(st && !st->error().empty(), "13: state.error() 有原因");
    check(errors >= 1 && !why.empty(), "13: errorBuilder 被调并拿到原因");
    check(st && !st->cropRect().has_value(), "13: 没有图 ⇒ 没有裁剪框");
    check(st && !st->produceImage(), "13: 出图给空");
    check(countRenderObjects<RenderCustomPaint>(h.root->findRenderObject()) == 0,
          "13: 失败态整条自绘层被占位替换");

    // 在飞：loading 占位；结果落地后再初始化几何。
    installSeams(false, false);
    int loadings = 0;
    IEArgs pending;
    pending.provider = std::make_shared<MemoryImageProvider>(toBytes(makePng(200, 100)), "ie13-async");
    pending.loadingBuilder = [&](BuildContext&, double) {
        ++loadings;
        return block(8.0, 8.0, kLime);
    };
    Harness h2;
    h2.attach(pin(400.0, 300.0, mkEditor(pending)));
    ImageEditorState* s2 = h2.editor();
    check(s2 && !s2->cropRect().has_value(), "13: 在飞时没有裁剪框");
    check(loadings >= 1, "13: loadingBuilder 画了占位");
    check(countRenderObjects<RenderCustomPaint>(h2.root->findRenderObject()) == 0,
          "13: 在飞时不挂自绘层");
    runBg();
    runUi();
    h2.settle();
    s2 = h2.editor();
    const std::optional<Rect> asyncCrop = s2 ? s2->cropRect() : std::optional<Rect>();
    check(s2 && asyncCrop.has_value(), "13: 异步结果落地 ⇒ 裁剪框初始化");
    check(s2 && near(s2->action().totalScale, 1.0), "13: 异步出图的 totalScale = 1");
    check(asyncCrop && sameRect(*asyncCrop, Rect(20.0, 60.0, 360.0, 180.0)),
          "13: 异步出图的铺图和同步一致 ⇒ " + desc(asyncCrop ? *asyncCrop : Rect()));
    resetSeams();
    installSeams(true, true);

    // 归中补间的 Ticker 必须随树销毁（铁律：别靠 ASan 才发现）。
    check(!Ticker::hasActiveTickers(), "13: 段首没有活动中的 Ticker");
    {
        Harness h3;
        IEArgs a;
        a.provider = std::make_shared<DecodedImageProvider>(pngLR(), "ie13-ticker");
        h3.attach(pin(400.0, 300.0, mkEditor(a)));
        ImageEditorState* s3 = h3.editor();
        if (s3) {
            h3.down(1, 10.0, 50.0);
            h3.move(1, 60.0, 110.0);
            h3.up(1, 60.0, 110.0);
            check(Ticker::hasActiveTickers(), "13: 抬手后归中起表");
        }
    }
    check(!Ticker::hasActiveTickers(), "13: 拆除后 Ticker 停表");
}

}  // namespace

int main() {
    std::printf("== ImageEditor（extended_image editor 层）==\n");
    installSeams(true, true);

    testGeometryAndAction();
    testMount();
    testHandleHits();
    testHandleDrag();
    testAspectRatio();
    testAutoCenter();
    testPan();
    testWheelZoom();
    testRotate();
    testFlip();
    testProduceAndSave();
    testPaint();
    testPlaceholders();

    resetSeams();
    std::printf("%s: %d\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
