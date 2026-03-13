/// @file test_image_painter.cpp
/// @brief ImagePainterLayer + ImagePainterController（widgets/image_painter.h）单元测试。
///
/// 覆盖：
///   1. 控制器纯数据口径：addStroke 拒绝空笔、通知次数、撤销栈存「操作」（一笔 / 一次
///      清空）、clear 可撤销、reset 不可撤销、失败操作不通知、bounds 含半个笔宽。
///   2. 挂载：树形（Listener + 前景 CustomPaint 各一个）、layerSize = 约束最大值、
///      订阅句柄 mount=1 / 换控制器重绑 / 拆除后 =0、没传控制器时自建。
///   3. 采点：minPointDistance 过滤、liveStroke/isPainting、抬手提交、onStrokeEnd、
///      颜色/宽度/模式的落笔快照。
///   4. 单指守卫 + commitLiveStroke 手动结笔。
///   5. 像素：笔迹盖在子节点之上、橡皮只擦笔迹层（底图透出来）、单点 = 实心圆。
///   6. compose：铺满映射 vs imageRect 映射、空笔迹/空底图、橡皮进合成、输出尺寸。
///   7. savePng：落盘再解码（本地临时目录）、坏路径返回 false。
///
/// headless 帧泵 + flushPaint 录进 SkPicture 后 composite 读像素（不需要窗口）。
/// 图层钉 400×200，底图 200×100 ⇒ 铺满映射的比例是 0.5，好核对。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/key.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/image_painter.h"
#include "cui/widgets/layout.h"
#include "cui/framework/listener.h"
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
bool sameRect(const Rect& a, const Rect& b, double eps = 1e-6) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.width, b.width, eps) &&
           near(a.height, b.height, eps);
}
std::string desc(const Rect& r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.4f,%.4f,%.4f,%.4f)", r.x, r.y, r.width, r.height);
    return buf;
}
std::string desc(size_t v) { return std::to_string(v); }
std::string desc(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const SkColor kWhite = SK_ColorWHITE;
const Color kGreen(0.0, 1.0, 0.0, 1.0);
const Color kBlue(0.0, 0.0, 1.0, 1.0);
const Color kYellow(1.0, 1.0, 0.0, 1.0);

// ── 素材 ────────────────────────────────────────────────────────────────────

/// 200×100 左红右蓝（合成出来的底图，不需要 PNG 编解码）。
sk_sp<SkImage> baseLR(int w = 200, int h = 100) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeXYWH(0.0f, 0.0f, float(w / 2), float(h)), paint);
    paint.setColor(SK_ColorBLUE);
    canvas->drawRect(SkRect::MakeXYWH(float(w / 2), 0.0f, float(w - w / 2), float(h)), paint);
    return surface->makeImageSnapshot();
}

ImagePainterStroke mkStroke(std::initializer_list<Offset> pts, const Color& color = kGreen,
                            double width = 4.0, bool erase = false) {
    ImagePainterStroke s;
    s.points = std::vector<Offset>(pts);
    s.color = color;
    s.width = width;
    s.erase = erase;
    return s;
}

/// 两点一笔，用宽度当「这一笔是谁」的标记。
ImagePainterStroke twoPoints(double x, double y, double width) {
    return mkStroke({Offset{x, y}, Offset{x + 20.0, y}}, kGreen, width);
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

/// 纯色块（图层里的「被涂对象」）。
SharedPtr<Widget> block(double w, double h, const Color& color) {
    BoxDecoration dec;
    dec.color = color;
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(dec));
}

/// 定尺外框：给图层一个确切的盒子。
SharedPtr<Widget> pin(double w, double h, SharedPtr<Widget> inner) {
    return cui::make_shared<SizedBox>(w, h, std::move(inner));
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
        // LayoutBuilder 打头：子树在布局期才建，多泵几帧到稳态。
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

    ImagePainterLayerState* painter() const {
        return dynamic_cast<ImagePainterLayerState*>(findStateNamed(root, "ImagePainterLayer"));
    }

    // 图层钉在窗口原点 ⇒ 全局点与本地点数值相同（分发器按节点做 globalToLocal）。
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

    /// 一划：水平线（步进 40 ⇒ 全部大于 minPointDistance）。
    void drawH(int64_t id, double y, double x0, double x1) {
        down(id, x0, y);
        for (double x = x0 + 40.0; x < x1; x += 40.0)
            move(id, x, y);
        up(id, x1, y);
    }
    void drawV(int64_t id, double x, double y0, double y1) {
        down(id, x, y0);
        for (double y = y0 + 40.0; y < y1; y += 40.0)
            move(id, x, y);
        up(id, x, y1);
    }

    uint32_t at(int x, int y) const {
        const SkImageInfo info = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        uint32_t px = 0;
        surface->readPixels(info, &px, 4, x, y);
        return px;
    }
};

// ── 配方 ────────────────────────────────────────────────────────────────────

struct IPArgs {
    std::shared_ptr<ImagePainterController> controller;
    ImagePainterMode mode = ImagePainterMode::Draw;
    Color color = kGreen;
    double width = 20.0;
    double minDistance = 2.0;
    ImagePainterLayer::StrokeCallback onStrokeEnd;
    Color childColor = kBlue;
};

/// 每帧新建实例：宿主是原地改同一个 Widget 对象的，测试要走 didUpdateWidget 的更新
/// 路径就必须换实例（同一个实例会被短路）。
SharedPtr<Widget> mkPainter(const IPArgs& a) {
    return cui::make_shared<ImagePainterLayer>(block(400.0, 200.0, a.childColor), a.controller,
                                              a.mode, a.color, a.width, a.minDistance,
                                              a.onStrokeEnd, nullptr);
}

/// 挂一个 400×200 的图层（底下一块纯蓝的 400×200 子节点）。
ImagePainterLayerState* mountPainter(Harness& h, const IPArgs& a) {
    h.attach(pin(400.0, 200.0, mkPainter(a)));
    return h.painter();
}

// ── 1. 控制器：仓库 / 撤销栈 / 通知 / bounds ─────────────────────────────────

void testControllerModel() {
    std::printf("[test] 1. 控制器：笔迹仓库 / 操作级撤销栈 / 通知 / bounds\n");

    auto ctl = std::make_shared<ImagePainterController>();
    int notified = 0;
    const int handle = ctl->addListenerWithHandle([&] { ++notified; });

    check(ctl->strokeCount() == 0 && !ctl->canUndo() && !ctl->canRedo(),
          "1: 空仓库 ⇒ 三个都空");
    check(sameRect(ctl->bounds(), Rect()), "1: 没有笔迹 ⇒ bounds 是空矩形");
    check(!ctl->addStroke(mkStroke({})), "1: 空点序列被拒");
    check(notified == 0 && ctl->strokeCount() == 0, "1: 被拒的一笔不通知、不进栈");
    check(!ctl->undo() && !ctl->redo() && !ctl->clear(), "1: 空栈三个操作都返回 false");
    check(notified == 0, "1: 失败操作不通知");

    ctl->addStroke(twoPoints(10.0, 10.0, 4.0));
    ctl->addStroke(twoPoints(20.0, 20.0, 5.0));
    ctl->addStroke(twoPoints(30.0, 30.0, 6.0));
    check(ctl->strokeCount() == 3 && notified == 3 && ctl->historyCount() == 3,
          "1: 三笔 = 三次通知 = 三个操作 ⇒ " + desc(ctl->historyCount()));
    check(ctl->canUndo() && !ctl->canRedo(), "1: 有历史可撤、无可重做");

    check(ctl->undo(), "1: undo 掉最后一笔");
    check(ctl->strokeCount() == 2 && ctl->redoCount() == 1 && notified == 4,
          "1: 撤一笔 ⇒ 笔迹 2 / 重做栈 1");
    check(near(ctl->strokes()[1].width, 5.0), "1: 撤的确实是最早那笔之后的 5.0");
    ctl->undo();
    ctl->undo();
    check(ctl->strokeCount() == 0 && ctl->redoCount() == 3 && !ctl->canUndo(),
          "1: 连撤三笔 ⇒ 空笔迹 + 三个可重做");

    // 重做按原序回来（栈的后进先出正好抵消一次）。
    ctl->redo();
    ctl->redo();
    ctl->redo();
    check(ctl->strokeCount() == 3 && ctl->redoCount() == 0, "1: 连重做三次 ⇒ 三笔全回来");
    check(near(ctl->strokes()[0].width, 4.0) && near(ctl->strokes()[2].width, 6.0),
          "1: 原序 4/5/6 ⇒ " + desc(ctl->strokes()[0].width) + "," +
              desc(ctl->strokes()[1].width) + "," + desc(ctl->strokes()[2].width));

    // clear 是一个「操作」：撤销它 = 整摞原序回来。
    check(ctl->clear(), "1: clear 成功");
    check(ctl->strokeCount() == 0 && ctl->canUndo() && !ctl->canRedo(),
          "1: clear 进撤销栈、作废重做栈 ⇒ " + desc(ctl->historyCount()));
    check(near(ctl->bounds().width, 0.0), "1: clear 后 bounds 空");
    check(ctl->undo(), "1: clear 之后可以 undo");
    check(ctl->strokeCount() == 3 && near(ctl->strokes()[0].width, 4.0) &&
              near(ctl->strokes()[2].width, 6.0),
          "1: 一次 undo 把整摞按原序拿回来");

    // 时序：清空 → 新笔 → 先撤新笔，再撤清空。
    ctl->clear();
    ctl->addStroke(twoPoints(40.0, 40.0, 9.0));
    check(ctl->strokeCount() == 1, "1: 清空后又能画");
    ctl->undo();
    check(ctl->strokeCount() == 0, "1: 先撤掉新那笔（不是回滚清空）");
    ctl->undo();
    check(ctl->strokeCount() == 3, "1: 再撤才回滚清空");
    ctl->redo();
    check(ctl->strokeCount() == 0, "1: redo 先进清空（时序在前的先重做）");
    ctl->redo();
    check(ctl->strokeCount() == 1 && near(ctl->strokes()[0].width, 9.0),
          "1: 再 redo 拿回那笔 9.0");

    // reset 不进栈。
    ctl->reset();
    check(ctl->strokeCount() == 0 && !ctl->canUndo() && !ctl->canRedo(),
          "1: reset 把笔迹和两个栈一起废");
    const int before = notified;
    ctl->reset();
    check(notified == before, "1: 空仓库的 reset 不通知");

    // bounds 含半个笔宽，跨所有笔迹。
    ctl->addStroke(mkStroke({Offset{20.0, 30.0}, Offset{100.0, 80.0}}, kGreen, 10.0));
    check(sameRect(ctl->bounds(), Rect(15.0, 25.0, 90.0, 60.0)),
          "1: bounds = 点框各外扩半笔宽 ⇒ " + desc(ctl->bounds()));

    ctl->removeListenerByHandle(handle);
    check(ctl->listenerCount() == 0, "1: 句柄摘得干净");
}

// ── 2. 挂载 / 订阅 / 树形 ────────────────────────────────────────────────────

void testMount() {
    std::printf("[test] 2. 挂载：树形 / layerSize / 订阅句柄 / 换控制器重绑\n");

    auto ctl = std::make_shared<ImagePainterController>();
    auto ctl2 = std::make_shared<ImagePainterController>();
    {
        Harness h;
        IPArgs a;
        a.controller = ctl;
        ImagePainterLayerState* st = mountPainter(h, a);
        check(st != nullptr, "2: 挂上了 ImagePainterLayerState");
        check(st && st->controller() == ctl, "2: State 绑的是宿主给的控制器");
        check(st && near(st->layerSize().width, 400.0) && near(st->layerSize().height, 200.0),
              "2: layerSize = 约束最大值 400×200");
        check(ctl->listenerCount() == 1, "2: 挂载期恰好一个订阅 ⇒ " + desc(ctl->listenerCount()));
        check(st && !st->isPainting() && st->liveStroke().empty(), "2: 没按下 ⇒ 不在画中");

        RenderObject* ro = h.root->findRenderObject();
        check(countRenderObjects<RenderPointerListener>(ro) == 1, "2: 树里恰好一个 Listener");
        check(countRenderObjects<RenderCustomPaint>(ro) == 1, "2: 前景自绘层恰好一个");

        ctl2->addStroke(twoPoints(1.0, 2.0, 3.0));
        IPArgs b;
        b.controller = ctl2;
        h.setBody(pin(400.0, 200.0, mkPainter(b)));
        st = h.painter();
        check(st && st->controller() == ctl2, "2: didUpdateWidget 换了控制器 ⇒ 重绑");
        check(ctl->listenerCount() == 0 && ctl2->listenerCount() == 1,
              "2: 旧控制器退订、新的订阅 ⇒ " + desc(ctl->listenerCount()) + "/" +
                  desc(ctl2->listenerCount()));
        check(st && st->controller()->strokeCount() == 1, "2: 新控制器的笔迹立刻可读");
    }
    check(ctl->listenerCount() == 0 && ctl2->listenerCount() == 0,
          "2: 拆除后两个都退订（铁律：别靠 ASan 才发现）");

    {
        Harness h;
        IPArgs a;  // controller 为空 ⇒ 控件自建
        ImagePainterLayerState* st = mountPainter(h, a);
        check(st && st->controller() != nullptr, "2: 没传控制器 ⇒ 控件自建一个");
        check(st && st->controller()->listenerCount() == 1,
              "2: 自建的那个也被订阅一次");
    }
}

// ── 3. 采点与快照 ───────────────────────────────────────────────────────────

void testStrokeCapture() {
    std::printf("[test] 3. 采点：minPointDistance / liveStroke / 提交 / 快照\n");

    auto ctl = std::make_shared<ImagePainterController>();
    int ends = 0;
    ImagePainterStroke last;
    IPArgs a;
    a.controller = ctl;
    a.width = 6.0;
    a.minDistance = 2.0;
    a.onStrokeEnd = [&](const ImagePainterStroke& s) {
        ++ends;
        last = s;
    };
    Harness h;
    ImagePainterLayerState* st = mountPainter(h, a);
    if (!st)
        return;

    h.down(1, 50.0, 50.0);
    check(st->isPainting(), "3: 按下 ⇒ 进入画中");
    check(st->liveStroke().points.size() == 1, "3: down 落第一个点");
    check(ctl->strokeCount() == 0, "3: 没抬手 ⇒ 控制器里还没这一笔");
    check(st->liveStroke().color == kGreen && near(st->liveStroke().width, 6.0) &&
              !st->liveStroke().erase,
          "3: live 带上 widget 的颜色/宽度/模式");

    h.move(1, 51.0, 51.0);
    check(st->liveStroke().points.size() == 1, "3: 位移 1.41 < minPointDistance 2 ⇒ 丢弃");
    h.move(1, 60.0, 50.0);
    h.move(1, 61.0, 50.0);
    check(st->liveStroke().points.size() == 2, "3: 距离按「最后一个已记录的点」比");
    h.move(1, 90.0, 50.0);
    check(st->liveStroke().points.size() == 3, "3: 采到三个点 ⇒ " +
                                                   desc(st->liveStroke().points.size()));

    h.up(1, 90.0, 50.0);
    check(!st->isPainting() && st->liveStroke().empty(), "3: 抬手清空 live");
    check(ctl->strokeCount() == 1, "3: 抬手提交一笔");
    check(ends == 1 && last.points.size() == 3, "3: onStrokeEnd 恰好一次、拿到那一笔");
    check(near(ctl->strokes()[0].points.front().dx, 50.0) &&
              near(ctl->strokes()[0].points.back().dy, 50.0),
          "3: 点存的是图层本地坐标");
    check(near(ctl->strokes()[0].width, 6.0), "3: 宽度按落笔时快照");

    // 落笔快照：中途改 widget 不影响已经画好的那笔。
    IPArgs b = a;
    b.color = kYellow;
    b.width = 30.0;
    h.setBody(pin(400.0, 200.0, mkPainter(b)));
    st = h.painter();
    h.drawH(1, 150.0, 60.0, 300.0);
    check(ctl->strokeCount() == 2, "3: 第二笔提交");
    check(ctl->strokes()[0].color == kGreen && ctl->strokes()[1].color == kYellow,
          "3: 两笔各自的颜色快照");
    check(near(ctl->strokes()[1].width, 30.0) && near(ctl->strokes()[0].width, 6.0),
          "3: 两笔各自的宽度快照");
    check(ctl->strokes()[1].points.size() == 6,
          "3: 60→300 的 drawH = 1 个 down + 步进 40 的 5 个 move（up 不落点）⇒ " +
              desc(ctl->strokes()[1].points.size()));

    // 橡皮模式也走快照。
    IPArgs c = b;
    c.mode = ImagePainterMode::Eraser;
    h.setBody(pin(400.0, 200.0, mkPainter(c)));
    st = h.painter();
    h.drawV(1, 200.0, 20.0, 180.0);
    check(ctl->strokeCount() == 3 && ctl->strokes()[2].erase, "3: 橡皮模式 ⇒ erase=true 的那一笔");
    check(!ctl->strokes()[1].erase, "3: 之前那笔仍是画笔");

    IPArgs d = c;
    d.mode = ImagePainterMode::Draw;
    h.setBody(pin(400.0, 200.0, mkPainter(d)));
    h.down(1, 350.0, 30.0);
    h.up(1, 350.0, 30.0);
    check(ctl->strokeCount() == 4 && !ctl->strokes()[3].erase, "3: 换回画笔 ⇒ 第四笔不是橡皮");
    check(ctl->strokes()[3].points.size() == 1, "3: 点一下 = 单点一笔");
    check(ends == 4, "3: 四笔 ⇒ 四次 onStrokeEnd");
}

// ── 4. 单指守卫与手动结笔 ───────────────────────────────────────────────────

void testSinglePointerAndCommit() {
    std::printf("[test] 4. 单指守卫 / commitLiveStroke\n");

    auto ctl = std::make_shared<ImagePainterController>();
    IPArgs a;
    a.controller = ctl;
    Harness h;
    ImagePainterLayerState* st = mountPainter(h, a);
    if (!st)
        return;

    h.down(1, 20.0, 20.0);
    h.down(2, 300.0, 150.0);
    check(st->liveStroke().points.size() == 1 &&
              near(st->liveStroke().points[0].dx, 20.0),
          "4: 第二根手指的 down 被忽略");
    h.move(2, 310.0, 150.0);
    check(st->liveStroke().points.size() == 1, "4: 非活动 pointerId 的 move 被忽略");
    h.up(2, 310.0, 150.0);
    check(st->isPainting() && ctl->strokeCount() == 0, "4: 非活动 pointerId 的 up 不结笔");

    h.move(1, 60.0, 20.0);
    st->commitLiveStroke();
    check(ctl->strokeCount() == 1 && !st->isPainting(), "4: commitLiveStroke 手动结笔");
    check(st->liveStroke().empty(), "4: 手动结笔后 live 清空");
    h.move(1, 90.0, 20.0);
    check(ctl->strokeCount() == 1 && st->liveStroke().empty(),
          "4: 结笔后同一手指的 move 不再续画（等下一次 down）");
    h.up(1, 90.0, 20.0);
    check(ctl->strokeCount() == 1, "4: 空 live 的 up 不产生第二笔");

    h.down(3, 100.0, 100.0);
    h.move(3, 140.0, 100.0);
    h.up(3, 140.0, 100.0);
    check(ctl->strokeCount() == 2, "4: 下一根手指重新起笔");

    // 图层外的点：Listener 只有 400×200，y=250 命中不到 ⇒ 不收事件。
    h.down(4, 100.0, 250.0);
    check(!st->isPainting(), "4: 盒子外的按下不进入画中（hit-test 没命中 Listener）");
    check(ctl->strokeCount() == 2, "4: 也没有新笔迹");
}

// ── 5. 像素：笔迹在上、橡皮只擦笔迹层 ───────────────────────────────────────

void testPixels() {
    std::printf("[test] 5. 像素：笔迹盖住子节点 / 橡皮只擦笔迹层 / 单点=圆\n");

    auto ctl = std::make_shared<ImagePainterController>();
    IPArgs a;
    a.controller = ctl;
    a.width = 20.0;
    a.color = kGreen;
    Harness h;
    ImagePainterLayerState* st = mountPainter(h, a);
    if (!st)
        return;

    h.frame();
    check(isBluePx(h.at(200, 100)) && isBluePx(h.at(50, 50)), "5: 起笔前整块是子节点的蓝");

    h.drawH(1, 100.0, 20.0, 380.0);
    h.frame();
    check(isGreenPx(h.at(200, 100)), "5: 笔迹盖在子节点之上（前景层）");
    check(isGreenPx(h.at(100, 100)) && isGreenPx(h.at(300, 100)), "5: 一整条横线都在");
    check(isBluePx(h.at(200, 150)) && isBluePx(h.at(200, 30)), "5: 线外仍是子节点（半笔宽 10）");
    check(ctl->strokeCount() == 1, "5: 屏幕上有的一笔 = 仓库里的一笔");

    // 橡皮：只擦笔迹层，底下蓝块重新透出来。
    IPArgs e = a;
    e.mode = ImagePainterMode::Eraser;
    e.width = 60.0;
    h.setBody(pin(400.0, 200.0, mkPainter(e)));
    h.drawV(1, 200.0, 40.0, 160.0);
    h.frame();
    check(isBluePx(h.at(200, 100)), "5: 橡皮擦过的位置透出子节点（不是黑/白）");
    check(isGreenPx(h.at(120, 100)) && isGreenPx(h.at(300, 100)), "5: 擦不到的两段笔迹还在");
    check(isBluePx(h.at(200, 170)), "5: 橡皮本身不往子节点上留痕");

    // 单点 = 实心圆（半径 = 半个笔宽）。
    ctl->reset();
    IPArgs g = a;
    h.setBody(pin(400.0, 200.0, mkPainter(g)));
    h.down(1, 200.0, 100.0);
    h.up(1, 200.0, 100.0);
    h.frame();
    check(ctl->strokeCount() == 1 && ctl->strokes()[0].points.size() == 1, "5: 一笔单点");
    check(isGreenPx(h.at(200, 100)), "5: 点一下看得见（圆心）");
    check(isGreenPx(h.at(200, 105)), "5: 半径 10 内 ⇒ y=105 还是绿的");
    check(isBluePx(h.at(200, 116)), "5: 半径外 ⇒ y=116 回到子节点");

    // 宽度 <= 0 的一笔什么都不画。
    ctl->reset();
    IPArgs z = a;
    z.width = 0.0;
    h.setBody(pin(400.0, 200.0, mkPainter(z)));
    h.drawH(1, 100.0, 20.0, 380.0);
    h.frame();
    check(ctl->strokeCount() == 1, "5: width=0 的一笔仍进仓库（点是采到的）");
    check(isBluePx(h.at(200, 100)), "5: 但 width<=0 不画 ⇒ 屏幕上没有它");
}

// ── 6. compose：图层坐标 → 像素 ─────────────────────────────────────────────

void testCompose() {
    std::printf("[test] 6. compose：铺满映射 / imageRect 映射 / 橡皮 / 边界\n");

    const sk_sp<SkImage> base = baseLR();
    const Size layer(400.0, 200.0);
    auto ctl = std::make_shared<ImagePainterController>();

    check(ctl->compose(base, layer).get() == base.get(), "6: 没有笔迹 ⇒ 原样返回底图指针");
    check(!ctl->compose(nullptr, layer), "6: 没有底图 ⇒ 返回空");

    ctl->addStroke(mkStroke({Offset{100.0, 100.0}, Offset{300.0, 100.0}}, kGreen, 20.0));
    const sk_sp<SkImage> out = ctl->compose(base, layer);
    check(out && out->width() == 200 && out->height() == 100,
          "6: 输出尺寸 = 底图尺寸（不是图层尺寸）");
    check(out && isGreenPx(pixelAt(out, 100, 50)), "6: 图层 (200,100) ⇒ 像素 (100,50) 是笔迹");
    check(out && isGreenPx(pixelAt(out, 50, 50)), "6: 图层 (100,100) ⇒ 像素 (50,50)");
    check(out && isRedPx(pixelAt(out, 30, 30)), "6: 底图左半没被改");
    check(out && isBluePx(pixelAt(out, 170, 30)), "6: 底图右半没被改");
    check(out && isRedPx(pixelAt(out, 30, 50)), "6: 圆头只到像素 x=40 ⇒ x=30 还是底图");
    check(out && isRedPx(pixelAt(out, 10, 10)), "6: 不改 base 本身（新位图）");

    // imageRect：底图只占图层里那块 ⇒ 1:1 映射
    auto ctl2 = std::make_shared<ImagePainterController>();
    ctl2->addStroke(mkStroke({Offset{150.0, 50.0}}, kGreen, 20.0));
    const sk_sp<SkImage> mapped = ctl2->compose(base, layer, Rect(100.0, 0.0, 200.0, 100.0));
    check(mapped && isGreenPx(pixelAt(mapped, 50, 50)),
          "6: imageRect(100,0,200,100) ⇒ 图层 (150,50) 落在像素 (50,50)");
    check(mapped && !isGreenPx(pixelAt(mapped, 75, 25)), "6: 铺满映射的那个位置是空的");
    const sk_sp<SkImage> tiled = ctl2->compose(base, layer);
    check(tiled && isGreenPx(pixelAt(tiled, 75, 25)),
          "6: 同一笔迹按铺满（0.5 倍）落到 (75,25)");
    check(tiled && isRedPx(pixelAt(tiled, 5, 50)), "6: 铺满映射的圆头不越界");

    // 合成里的橡皮：擦掉笔迹 ⇒ 底图重新透出来。
    ctl->addStroke(mkStroke({Offset{200.0, 40.0}, Offset{200.0, 160.0}}, kGreen, 60.0, true));
    const sk_sp<SkImage> erased = ctl->compose(base, layer);
    check(erased && erased->width() == 200 && erased->height() == 100, "6: 橡皮合成后尺寸不变");
    check(erased && isBluePx(pixelAt(erased, 100, 50)),
          "6: 擦到处回到像素 (100,50) 的底图色（蓝）");
    check(erased && isGreenPx(pixelAt(erased, 60, 50)), "6: 擦不到的左段笔迹还在");
    check(erased && isRedPx(pixelAt(erased, 30, 30)), "6: 底图始终完好");

    // 只有一个点的橡皮 = 一个透明洞。
    auto ctl3 = std::make_shared<ImagePainterController>();
    ctl3->addStroke(mkStroke({Offset{100.0, 100.0}, Offset{300.0, 100.0}}, kGreen, 20.0));
    ctl3->addStroke(mkStroke({Offset{200.0, 100.0}}, kGreen, 40.0, true));
    const sk_sp<SkImage> dotErase = ctl3->compose(base, layer);
    check(dotErase && isBluePx(pixelAt(dotErase, 100, 50)), "6: 单点橡皮 = 半径 20 的洞");
    check(dotErase && isGreenPx(pixelAt(dotErase, 80, 50)), "6: 洞外（像素 x=80）还在");

    // 空的 imageRect（宽高 0）按铺满处理：走 1:1 那条映射的话，这两笔全落在
    // 像素 y=100（图外），(60,50) 会是底图的红。
    const sk_sp<SkImage> zeroRect = ctl3->compose(base, layer, Rect());
    check(zeroRect && isGreenPx(pixelAt(zeroRect, 60, 50)),
          "6: imageRect 宽高 0 ⇒ 退回铺满映射（0.5 倍）而不是 1:1");
}

// ── 7. savePng ──────────────────────────────────────────────────────────────

void testSavePng() {
    std::printf("[test] 7. savePng 落盘再解码 / 坏路径\n");

    const sk_sp<SkImage> base = baseLR();
    auto ctl = std::make_shared<ImagePainterController>();
    ctl->addStroke(mkStroke({Offset{100.0, 100.0}, Offset{300.0, 100.0}}, kGreen, 20.0));

    // 落盘用本地临时目录：VMware 共享目录的写回时机不定，紧接着回读会拿到 0 字节。
    const char* tmpDir = std::getenv("TEMP");
    if (!tmpDir)
        tmpDir = std::getenv("TMPDIR");
    const std::string path =
        (tmpDir ? std::string(tmpDir) + "/" : std::string()) + "cui_test_image_painter_out.png";

    check(ctl->savePng(path, base, Size(400.0, 200.0)), "7: savePng 返回 true");
    const DecodedImagePtr reread = decodeImageFromFile(path);
    check(reread && reread->valid() && reread->width() == 200 && reread->height() == 100,
          "7: 重读回 200×100（" + (reread ? reread->error : std::string("null")) + "）");
    if (reread && reread->valid()) {
        const sk_sp<SkImage> img = reread->image();
        check(img && isGreenPx(pixelAt(img, 100, 50)), "7: 重读后笔迹还在");
        check(img && isRedPx(pixelAt(img, 30, 30)) && isBluePx(pixelAt(img, 170, 30)),
              "7: 重读后底图两半颜色一致");
    }
    std::remove(path.c_str());

    // 没有笔迹 ⇒ compose 原样返回底图 ⇒ 也能落盘（就是底图的 PNG）。
    auto empty = std::make_shared<ImagePainterController>();
    const std::string plain =
        (tmpDir ? std::string(tmpDir) + "/" : std::string()) +
        "cui_test_image_painter_plain.png";
    check(empty->savePng(plain, base, Size(400.0, 200.0)), "7: 空笔迹也能出图");
    const DecodedImagePtr back = decodeImageFromFile(plain);
    check(back && back->valid() && isRedPx(pixelAt(back->image(), 30, 50)),
          "7: 空笔迹出图 = 底图（" + (back ? back->error : std::string("null")) + "）");
    std::remove(plain.c_str());

    check(!empty->savePng("no_such_dir_cui/__out.png", base, Size(400.0, 200.0)),
          "7: 目录不存在 ⇒ false");
    check(!ctl->savePng(plain, nullptr, Size(400.0, 200.0)), "7: 没有底图 ⇒ false");
}

}  // namespace

int main() {
    std::printf("== ImagePainterLayer / ImagePainterController ==\n");

    testControllerModel();
    testMount();
    testStrokeCapture();
    testSinglePointerAndCommit();
    testPixels();
    testCompose();
    testSavePng();

    std::printf("%s: %d\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
