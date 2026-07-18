#pragma once
/// @file widgets/image_painter.h
/// @brief ImagePainterLayer + ImagePainterController —— 在图像（或任意子节点）之上
///        自由涂抹：画笔 / 橡皮 / 撤销·重做 / 清空 / 把笔迹合成到位图上出图（header-only）。
///
/// 与参考库的关系（写清楚，别当 bug 排查）：
///   pub.dev `extended_image` v3.10.0 的「painting」是 `ExtendedImage.painting` 回调
///   （lib/src/image/painting.dart 的 `PaintType::DrawHeart/ClipHeart`），也就是
///   「在图之前/之后画自定义内容」，**并没有自由手绘层**；lib/ 下也没有 painter_layer.dart
///   （本机已核对整份文件清单）。所以本层是按同一控件族（ExtendedImage / ImageEditor）
///   的用法自建的一套，命名与「控制器 + 控件 + State」的分工对齐本仓 InteractiveViewer
///   （`TransformationController`）那条，而不是照抄某个上游文件。
///
/// 结构：LayoutBuilder → [Listener] → CustomPaint(foregroundPainter = 笔迹层, child)
///   笔迹画在子节点**之后**（RenderCustomPaint 的前景观），所以盖在图上。
///
/// 口径 / 有意偏离：
///   1. **单指**：一次只跟一个 pointer（`activePointerId_`），后来的手指事件忽略。
///      多点触控手绘要的是「同色连笔」，本仓先用简单那条。
///   2. **橡皮 = 图层级 kClear**：所有笔迹（含橡皮）画进同一个 `saveLayer`，橡皮用
///      `SkBlendMode::kClear` ⇒ 只擦掉笔迹层，**底下的图像不受影响**。要擦图请走
///      ImageEditor（裁剪/翻转）或直接对位图做像素处理。
///   3. **笔迹存图层坐标**（盒子本地），`compose()` 按 `imageRect` 映射到像素：
///      默认整个图层铺到位图上（`layerSize → 位图尺寸`），图在图层里只占一块时
///      传 `imageRect` 指明那块。
///   4. **落点 = 实心圆**（单点一笔）、折线用圆头圆角 ⇒ 不依赖 Painter 暴露 cap 设置，
///      直接走 `skPaint()`。
///   5. **撤销栈存的是「操作」**（一笔 / 一次清空），不是笔迹列表本身 ⇒ `clear()`
///      之后一次 `undo()` 就把整摞笔迹原序拿回来；`reset()` 不进栈（清空 + 两个栈
///      一起废）。都是笔迹粒度，不是像素粒度。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <include/core/SkBlendMode.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypes.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/key.h"
#include "cui/foundation/painter.h"
#include "cui/framework/framework.h"
#include "cui/framework/listener.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"

namespace cui {

// ── 笔迹模型 ────────────────────────────────────────────────────────────────

/// 画笔 / 橡皮。
enum class ImagePainterMode {
    Draw,
    Eraser,
};

/// 一笔：图层坐标系下的点序列 + 落笔时的颜色/宽度/模式快照。
/// 快照是刻意的 —— 宿主中途换颜色/粗细不该改已画好的笔迹。
struct ImagePainterStroke {
    std::vector<Offset> points;
    Color color{1.0, 0.0, 0.0, 1.0};
    double width = 4.0;
    bool erase = false;

    bool empty() const { return points.empty(); }
};

/// 把笔迹画进 `bounds` 描述的那一层（调用方负责已经处于目标坐标系）。
/// 全部笔迹共用一个 `saveLayer` ⇒ 橡皮的 kClear 只作用在本层内。
inline void paintImagePainterStrokes(Painter& p, const std::vector<ImagePainterStroke>& strokes,
                                     const Rect& bounds) {
    if (strokes.empty())
        return;
    p.save();
    p.saveLayer(bounds);
    for (const ImagePainterStroke& s : strokes) {
        if (s.points.empty() || s.width <= 0.0)
            continue;
        p.skPaint().setBlendMode(s.erase ? SkBlendMode::kClear : SkBlendMode::kSrcOver);
        p.skPaint().setStrokeCap(SkPaint::kRound_Cap);
        p.skPaint().setStrokeJoin(SkPaint::kRound_Join);
        p.skPaint().setAntiAlias(true);
        p.setSourceColor(s.color);
        if (s.points.size() == 1) {
            // 单点 = 落一个实心圆（半径 = 半个笔宽），不然点一下什么都不看见。
            p.setFillStyle();
            p.circle(s.points.front().dx, s.points.front().dy, s.width / 2.0);
            p.fill();
            continue;
        }
        p.setLineWidth(s.width);
        p.moveTo(s.points.front().dx, s.points.front().dy);
        for (size_t i = 1; i < s.points.size(); ++i)
            p.lineTo(s.points[i].dx, s.points[i].dy);
        p.stroke();
    }
    p.restore();
    p.restore();
}

// ── ImagePainterController ──────────────────────────────────────────────────

/// 笔迹仓库 + 撤销/重做栈 + 出图。宿主自己 new 一个 shared_ptr 传进控件，
/// 就能在控件外面按撤销/重做、读笔迹、合成出图（对齐本仓 `TransformationController`）。
class ImagePainterController : public ChangeNotifier {
public:
    const std::vector<ImagePainterStroke>& strokes() const { return strokes_; }
    size_t strokeCount() const { return strokes_.size(); }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    size_t historyCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }

    /// 追加一笔（会通知监听者）。空点序列直接忽略、返回 false。
    bool addStroke(const ImagePainterStroke& stroke) {
        if (stroke.points.empty())
            return false;
        strokes_.push_back(stroke);
        undo_.push_back(Op{false, stroke, {}});
        redo_.clear();
        notifyListeners();
        return true;
    }

    /// 撤销最后一个操作：一笔 ⇒ 摘掉那条笔迹；一次清空 ⇒ 整摞原样回来。
    bool undo() {
        if (undo_.empty())
            return false;
        Op op = std::move(undo_.back());
        undo_.pop_back();
        if (op.isClear) {
            strokes_ = op.before;
        } else if (!strokes_.empty()) {
            strokes_.pop_back();
        }
        redo_.push_back(std::move(op));
        notifyListeners();
        return true;
    }

    bool redo() {
        if (redo_.empty())
            return false;
        Op op = std::move(redo_.back());
        redo_.pop_back();
        if (op.isClear)
            strokes_.clear();
        else
            strokes_.push_back(op.stroke);
        undo_.push_back(std::move(op));
        notifyListeners();
        return true;
    }

    /// 清空，但作为一个「操作」进栈 ⇒ 一次 undo() 全部原序拿回来。
    bool clear() {
        if (strokes_.empty())
            return false;
        undo_.push_back(Op{true, ImagePainterStroke{}, strokes_});
        strokes_.clear();
        redo_.clear();
        notifyListeners();
        return true;
    }

    /// 清空且不可撤销（画笔工具「重新开始」）：两个栈一起废。
    void reset() {
        if (strokes_.empty() && undo_.empty() && redo_.empty())
            return;
        strokes_.clear();
        undo_.clear();
        redo_.clear();
        notifyListeners();
    }

    /// 笔迹在图层坐标里覆盖的外接框（含半个笔宽）；没有笔迹时返回空矩形。
    Rect bounds() const {
        Rect out;
        double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
        bool any = false;
        for (const ImagePainterStroke& s : strokes_) {
            for (const Offset& o : s.points) {
                const double half = s.width / 2.0;
                const double l = o.dx - half, t = o.dy - half;
                const double r = o.dx + half, b = o.dy + half;
                if (!any) {
                    left = l;
                    top = t;
                    right = r;
                    bottom = b;
                    any = true;
                } else {
                    left = std::min(left, l);
                    top = std::min(top, t);
                    right = std::max(right, r);
                    bottom = std::max(bottom, b);
                }
            }
        }
        if (!any)
            return out;
        return Rect(left, top, right - left, bottom - top);
    }

    /// 在 `base` 之上画一层笔迹，返回新位图（不改 base）。
    /// `layerSize` = 笔迹所在图层的尺寸；`imageRect` = base 在图层里占的那块
    /// （宽或高 ≤0 时按「铺满图层」）。橡皮擦掉的是笔迹层 ⇒ base 会重新透出来。
    sk_sp<SkImage> compose(const sk_sp<SkImage>& base, const Size& layerSize,
                           const Rect& imageRect = Rect()) const {
        if (!base)
            return base;
        const bool useRect = imageRect.width > 0.0 && imageRect.height > 0.0;
        const double rectX = useRect ? imageRect.x : 0.0;
        const double rectY = useRect ? imageRect.y : 0.0;
        const double rectW = useRect ? imageRect.width : layerSize.width;
        const double rectH = useRect ? imageRect.height : layerSize.height;
        if (strokes_.empty() || rectW <= 0.0 || rectH <= 0.0)
            return base;
        const int w = base->width();
        const int h = base->height();
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
        if (!surface)
            return base;
        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        Painter p(canvas);
        p.drawImageSkia(base, 0.0, 0.0);
        const double sx = double(w) / rectW;
        const double sy = double(h) / rectH;
        p.save();
        // CTM 是后乘：先 scale 再 translate ⇒ 设备 = s·(p − rect.leftTop)，正是图层→像素。
        p.scale(sx, sy);
        p.translate(-rectX, -rectY);
        paintImagePainterStrokes(p, strokes_, Rect(0.0, 0.0, layerSize.width, layerSize.height));
        p.restore();
        return surface->makeImageSnapshot();
    }

    /// compose + encodePng + 写盘；失败返回 false（无位图 / 写不进）。
    bool savePng(const std::string& path, const sk_sp<SkImage>& base, const Size& layerSize,
                 const Rect& imageRect = Rect()) const {
        const sk_sp<SkImage> out = compose(base, layerSize, imageRect);
        if (!out)
            return false;
        const sk_sp<SkData> data = encodePng(out);
        if (!data || data->size() == 0)
            return false;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(reinterpret_cast<const char*>(data->data()), std::streamsize(data->size()));
        // 立刻 flush 查状态：不 flush 就是「返回 true 但落盘 0 字节」。
        f.flush();
        return f.good();
    }

private:
    /// 历史条目：一笔，或一次「清空」（`before` 记住清空前的全部笔迹）。
    struct Op {
        bool isClear = false;
        ImagePainterStroke stroke;
        std::vector<ImagePainterStroke> before;
    };

    std::vector<ImagePainterStroke> strokes_;
    std::vector<Op> undo_;
    std::vector<Op> redo_;
};

// ── ImagePainterLayer ───────────────────────────────────────────────────────

class ImagePainterLayer : public StatefulWidget {
public:
    using StrokeCallback = std::function<void(const ImagePainterStroke&)>;

    /// 参数按「被涂的子节点 → 控制器 → 笔刷 → 回调」分组，新参数一律追加在 `key` 之前。
    /// `controller` 为空时控件自建一个（只想用控件、不想管控制器的调用法）。
    explicit ImagePainterLayer(SharedPtr<Widget> child = nullptr,
                               std::shared_ptr<ImagePainterController> controller = nullptr,
                               ImagePainterMode mode = ImagePainterMode::Draw,
                               const Color& strokeColor = Color(1.0, 0.0, 0.0, 1.0),
                               double strokeWidth = 4.0, double minPointDistance = 2.0,
                               StrokeCallback onStrokeEnd = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)),
          controller_(controller ? std::move(controller)
                                : std::make_shared<ImagePainterController>()),
          mode_(mode), strokeColor_(strokeColor), strokeWidth_(strokeWidth),
          minPointDistance_(minPointDistance), onStrokeEnd_(std::move(onStrokeEnd)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    const std::shared_ptr<ImagePainterController>& controller() const { return controller_; }
    void setController(std::shared_ptr<ImagePainterController> c) { controller_ = std::move(c); }

    ImagePainterMode mode() const { return mode_; }
    void setMode(ImagePainterMode m) { mode_ = m; }
    const Color& strokeColor() const { return strokeColor_; }
    void setStrokeColor(const Color& c) { strokeColor_ = c; }
    double strokeWidth() const { return strokeWidth_; }
    void setStrokeWidth(double w) { strokeWidth_ = w; }
    double minPointDistance() const { return minPointDistance_; }
    void setMinPointDistance(double d) { minPointDistance_ = d; }
    const StrokeCallback& onStrokeEnd() const { return onStrokeEnd_; }
    void setOnStrokeEnd(StrokeCallback cb) { onStrokeEnd_ = std::move(cb); }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ImagePainterLayer"; }

private:
    SharedPtr<Widget> child_;
    std::shared_ptr<ImagePainterController> controller_;
    ImagePainterMode mode_ = ImagePainterMode::Draw;
    Color strokeColor_{1.0, 0.0, 0.0, 1.0};
    double strokeWidth_ = 4.0;
    double minPointDistance_ = 2.0;
    StrokeCallback onStrokeEnd_;
};

// ── ImagePainterLayerState ──────────────────────────────────────────────────

class ImagePainterLayerState : public State {
public:
    void initState() override { bindController(); }

    void dispose() override { unbindController(); }

    void didUpdateWidget(cui::Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = widget_();
        if (w && w->controller() != controller_) {
            unbindController();
            bindController();
        }
    }

    /// 当前控制器（未挂载时也拿得到：widget 自己建的那一个）。
    const std::shared_ptr<ImagePainterController>& controller() const { return controller_; }

    /// 图层尺寸 = 最后一次 build 收到的约束最大值（compose 的映射基准）。
    const Size& layerSize() const { return outerSize_; }

    /// 正在画的那一笔（还没提交到控制器）；抬手后为空。
    const ImagePainterStroke& liveStroke() const { return live_; }
    bool isPainting() const { return activePointerId_ >= 0; }

    /// 提交当前正在画的一笔（宿主想「一划一存」时手动切）。
    void commitLiveStroke() { endStroke(); }

    SharedPtr<Widget> build(BuildContext& context) override {
        auto* w = widget_();
        if (!w)
            return nullptr;
        return cui::make_shared<LayoutBuilder>([this, w](BuildContext& ctx,
                                                        const BoxConstraints& constraints) {
            return buildSized(ctx, constraints, w);
        });
    }

private:
    ImagePainterLayer* widget_() const { return static_cast<ImagePainterLayer*>(widget()); }

    // ── 控制器订阅（句柄式：捕获 this 的 lambda 摘不掉，见 AGENTS.md 铁律）────
    void bindController() {
        auto* w = widget_();
        if (!w || !w->controller())
            return;
        controller_ = w->controller();
        controllerHandle_ = controller_->addListenerWithHandle([this] {
            if (mounted())
                setState();
        });
    }
    void unbindController() {
        if (controller_ && controllerHandle_ >= 0)
            controller_->removeListenerByHandle(controllerHandle_);
        controllerHandle_ = -1;
    }

    SharedPtr<Widget> buildSized(BuildContext& /*context*/, const BoxConstraints& constraints,
                                 ImagePainterLayer* w) {
        outerSize_ = constraints.biggest();
        CustomPaint::PainterCallback foreground = [this](Painter& p, const Size& size) {
            std::vector<ImagePainterStroke> all =
                controller_ ? controller_->strokes() : std::vector<ImagePainterStroke>();
            if (!live_.empty())
                all.push_back(live_);
            paintImagePainterStrokes(p, all, Rect(0.0, 0.0, size.width, size.height));
        };
        SharedPtr<Widget> box =
            cui::make_shared<CustomPaint>(CustomPaint::PainterCallback{}, std::move(foreground),
                                          outerSize_, w->child());
        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { handleDown(e); },
            [this](const PointerEvent& e) { handleMove(e); },
            [this](const PointerEvent& e) { handleUp(e); }, Listener::PointerCallback{},
            Listener::PointerCallback{}, std::move(box));
    }

    void handleDown(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || activePointerId_ >= 0)
            return;
        activePointerId_ = e.pointerId;
        live_ = ImagePainterStroke{};
        live_.color = w->strokeColor();
        live_.width = w->strokeWidth();
        live_.erase = w->mode() == ImagePainterMode::Eraser;
        live_.points.push_back(e.position);
        if (mounted())
            setState();
    }

    void handleMove(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || e.pointerId != activePointerId_ || live_.points.empty())
            return;
        const Offset& last = live_.points.back();
        const double dx = e.position.dx - last.dx;
        const double dy = e.position.dy - last.dy;
        if (std::hypot(dx, dy) < std::max(0.0, w->minPointDistance()))
            return;
        live_.points.push_back(e.position);
        if (mounted())
            setState();
    }

    void handleUp(const PointerEvent& e) {
        if (e.pointerId != activePointerId_)
            return;
        endStroke();
    }

    void endStroke() {
        activePointerId_ = -1;
        auto* w = widget_();
        if (live_.empty())
            return;
        ImagePainterStroke stroke = std::move(live_);
        live_ = ImagePainterStroke{};
        if (controller_)
            controller_->addStroke(stroke);  // 监听回调里已经 setState
        if (w && w->onStrokeEnd())
            w->onStrokeEnd()(stroke);
    }

    std::shared_ptr<ImagePainterController> controller_;
    int controllerHandle_ = -1;
    int64_t activePointerId_ = -1;
    ImagePainterStroke live_;
    Size outerSize_{};
};

inline std::unique_ptr<State> ImagePainterLayer::createState() {
    return std::make_unique<ImagePainterLayerState>();
}

}  // namespace cui_core
