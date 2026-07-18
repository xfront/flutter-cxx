#pragma once
/// @file widgets/paint.h
/// @brief 上游 绘制类 Widget 移植（第二批）。
///
/// 对应 上游源码：
///   上游 widgets/basic.dart       — ClipRect / ClipOval / Image
///   上游 widgets/custom_paint.dart — CustomPaint
///
/// 层级映射严格对齐上游：
///   ClipRect / ClipOval / CustomPaint → SingleChildRenderObjectWidget
///   Image                             → LeafRenderObjectWidget

#include <functional>
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <memory>
#include <optional>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/painter.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box_widgets.h"

namespace cui {

// ── ClipRect ────────────────────────────────────────────────────────────────

/// 矩形裁剪。上游的 `ClipRect`（basic.dart）。
///
/// 不改变布局：裁剪区域即自身盒子。
class ClipRect : public SingleChildRenderObjectWidget {
public:
    explicit ClipRect(SharedPtr<Widget> child = nullptr, bool antiAlias = true,
                      KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), antiAlias_(antiAlias) {}

    bool antiAlias() const { return antiAlias_; }
    void setAntiAlias(bool v) { antiAlias_ = v; }

    RenderObject* createRenderObject() override { return new RenderClipRect(antiAlias_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderClipRect*>(ro))
            r->setAntiAlias(antiAlias_);
    }

private:
    bool antiAlias_ = true;
};

// ── ClipOval ────────────────────────────────────────────────────────────────

/// 椭圆裁剪。上游的 `ClipOval`（basic.dart）。
class ClipOval : public SingleChildRenderObjectWidget {
public:
    explicit ClipOval(SharedPtr<Widget> child = nullptr, bool antiAlias = true,
                      KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), antiAlias_(antiAlias) {}

    bool antiAlias() const { return antiAlias_; }
    void setAntiAlias(bool v) { antiAlias_ = v; }

    RenderObject* createRenderObject() override { return new RenderClipOval(antiAlias_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderClipOval*>(ro))
            r->setAntiAlias(antiAlias_);
    }

private:
    bool antiAlias_ = true;
};

// ── Image ───────────────────────────────────────────────────────────────────

/// 图像。上游的 `Image`（basic.dart 的 `Image` widget 的渲染部分）。
///
/// 只负责「把一张已解码的 SkImage 按 BoxFit + Alignment 画出来」——
/// 解码 / 缓存 / Provider 属于更高层，不在本移植范围内。
class Image : public LeafRenderObjectWidget {
public:
    explicit Image(sk_sp<SkImage> image = nullptr, KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), image_(std::move(image)) {}

    const sk_sp<SkImage>& image() const { return image_; }
    void setImage(sk_sp<SkImage> img) { image_ = std::move(img); }

    /// 显式宽度（nullopt = 由图像与约束决定）。
    const std::optional<double>& width() const { return width_; }
    void setWidth(std::optional<double> w) { width_ = w; }

    /// 显式高度（nullopt = 由图像与约束决定）。
    const std::optional<double>& height() const { return height_; }
    void setHeight(std::optional<double> h) { height_ = h; }

    /// 像素 → 逻辑尺寸换算（上游的 `Image.scale`）。
    double scale() const { return scale_; }
    void setScale(double s) { scale_ = s; }

    BoxFit fit() const { return fit_; }
    void setFit(BoxFit f) { fit_ = f; }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { alignment_ = a; }

    RenderObject* createRenderObject() override {
        auto* r = new RenderImage();
        r->setImage(image_);
        r->setWidth(width_);
        r->setHeight(height_);
        r->setScale(scale_);
        r->setFit(fit_);
        r->setAlignment(alignment_);
        return r;
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderImage*>(ro);
        if (!r)
            return;
        r->setImage(image_);
        r->setWidth(width_);
        r->setHeight(height_);
        r->setScale(scale_);
        r->setFit(fit_);
        r->setAlignment(alignment_);
    }

private:
    sk_sp<SkImage> image_;
    std::optional<double> width_;
    std::optional<double> height_;
    double scale_ = 1.0;
    BoxFit fit_ = BoxFit::Contain;
    Alignment alignment_ = Alignment::center();
};

// ── CustomPaint ─────────────────────────────────────────────────────────────

/// 自绘。上游的 `CustomPaint`（custom_paint.dart）。
///
/// `painter` 在子节点之前绘制，`foregroundPainter` 在子节点之后；
/// 画布已平移到盒子原点，回调拿到 `(Painter&, size)`。
/// 无子节点时尺寸 = `preferredSize`（受父约束 clamp）。
class CustomPaint : public SingleChildRenderObjectWidget {
public:
    using PainterCallback = RenderCustomPaint::PainterCallback;

    explicit CustomPaint(PainterCallback painter = nullptr,
                         PainterCallback foregroundPainter = nullptr, Size preferredSize = Size(),
                         SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          painter_(std::move(painter)), foregroundPainter_(std::move(foregroundPainter)),
          preferredSize_(preferredSize) {}

    void setPainter(PainterCallback p) { painter_ = std::move(p); }
    void setForegroundPainter(PainterCallback p) { foregroundPainter_ = std::move(p); }

    const Size& preferredSize() const { return preferredSize_; }
    void setPreferredSize(const Size& s) { preferredSize_ = s; }

    RenderObject* createRenderObject() override {
        return new RenderCustomPaint(painter_, foregroundPainter_, preferredSize_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderCustomPaint*>(ro)) {
            r->setPainter(painter_);
            r->setForegroundPainter(foregroundPainter_);
            r->setPreferredSize(preferredSize_);
        }
    }

private:
    PainterCallback painter_;
    PainterCallback foregroundPainter_;
    Size preferredSize_;
};

}  // namespace cui_core
