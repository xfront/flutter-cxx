/// @file foundation/layer.cpp
/// @brief Layer hierarchy rasterization — each layer type knows how to
///        replay itself onto an SkCanvas.

#include "cui/foundation/layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <typeinfo>
#include <unordered_set>

#include <include/core/SkCanvas.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

namespace cui {

// 全局图层「代」计数（Layer::markLive / advanceGeneration 共享）。
unsigned Layer::s_gen = 0;

namespace {

/// 存活图层登记表。GUI 单线程（所有绘制都在主线程），无需加锁。
std::unordered_set<const Layer*>& liveLayers() {
    static std::unordered_set<const Layer*> set;
    return set;
}

}  // namespace

Layer::Layer() {
    liveLayers().insert(this);
}

Layer::~Layer() {
    liveLayers().erase(this);
}

bool Layer::isAlive(const Layer* layer) {
    return layer && liveLayers().count(layer) != 0;
}

// ── ContainerLayer ──────────────────────────────────────────────────────────

ContainerLayer::~ContainerLayer() = default;

Layer* ContainerLayer::appendChild(std::unique_ptr<Layer> child) {
    child->parent_ = this;
    auto* raw = child.get();
    children_.push_back(std::move(child));
    return raw;
}

void ContainerLayer::removeAllChildren() {
    for (auto& c : children_)
        c->parent_ = nullptr;
    children_.clear();
}

void ContainerLayer::bringToFront(Layer* child) {
    if (!child || children_.empty() || children_.back().get() == child)
        return;
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            auto owned = std::move(*it);
            children_.erase(it);
            children_.push_back(std::move(owned));
            return;
        }
    }
}

std::unique_ptr<Layer> ContainerLayer::detachChild(Layer* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            child->parent_ = nullptr;
            auto owned = std::move(*it);
            children_.erase(it);
            return owned;
        }
    }
    return nullptr;
}

void ContainerLayer::rasterize(SkCanvas* canvas) const {
    for (const auto& child : children_)
        child->rasterize(canvas);
}

void ContainerLayer::pruneOrphans() {
    // 先递归清扫容器型子层（自底向上），使嵌套的孤儿在其父层被删前先自行清理；
    // 未标 live 的子层整体即将销毁，其子树随之释放，故递归只对保留下来的层有意义。
    for (auto& c : children_)
        if (auto* cl = dynamic_cast<ContainerLayer*>(c.get()))
            cl->pruneOrphans();
    // 删除本代未被 markLive 命中的子层（owner 已销毁 = 孤儿），unique_ptr 随之析构。
    children_.erase(
        std::remove_if(children_.begin(), children_.end(),
                       [](const std::unique_ptr<Layer>& l) { return l->liveGen_ != s_gen; }),
        children_.end());
}

// ── PictureLayer ────────────────────────────────────────────────────────────

void PictureLayer::rasterize(SkCanvas* canvas) const {
    if (!picture_)
        return;

    // 只对足够大、且连续多帧未被重录的 picture 做光栅缓存。小 picture（动画
    // 画布）或每帧都变的 picture（stable_ 达不到阈值）走直接重放。
    const SkRect cull = picture_->cullRect();
    const double area = static_cast<double>(cull.width()) * cull.height();
    constexpr double kMinCacheArea = 40000.0;    // ~200x200 以下不缓存
    constexpr double kMaxCacheArea = 4096.0 * 4096.0;
    const bool cacheable = area >= kMinCacheArea && area <= kMaxCacheArea;

    if (!cacheable) {
        canvas->drawPicture(picture_);
        return;
    }

    if (!cache_) {
        // 需连续稳定若干帧才烘焙，避免为瞬变 picture 白白分配/光栅。
        if (++stable_ < 2) {
            canvas->drawPicture(picture_);
            return;
        }
        const int w = static_cast<int>(std::ceil(cull.width()));
        const int h = static_cast<int>(std::ceil(cull.height()));
        auto surf = canvas->makeSurface(SkImageInfo::MakeN32Premul(w, h));
        if (!surf) {  // 后端不支持：退回直接重放（不重试计数）。
            --stable_;
            canvas->drawPicture(picture_);
            return;
        }
        SkCanvas* sc = surf->getCanvas();
        sc->translate(-cull.x(), -cull.y());  // picture 内容映到 [0,0,w,h]
        sc->drawPicture(picture_);
        cache_ = surf->makeImageSnapshot();
        if (!cache_) {
            canvas->drawPicture(picture_);
            return;
        }
    }

    // blit 缓存位图到 picture 原窗口位置（受调用方 clip 约束，仅脏区参
    // 与光栅）。canvas 已处于 picture 录制时的坐标系。
    canvas->save();
    canvas->translate(cull.x(), cull.y());
    canvas->drawImage(cache_, 0, 0);
    canvas->restore();
}

// ── TransformLayer ──────────────────────────────────────────────────────────

void TransformLayer::rasterize(SkCanvas* canvas) const {
    canvas->save();
    canvas->translate(static_cast<SkScalar>(offset_.dx),
                      static_cast<SkScalar>(offset_.dy));
    ContainerLayer::rasterize(canvas);
    canvas->restore();
}

// ── ClipRectLayer ───────────────────────────────────────────────────────────

void ClipRectLayer::rasterize(SkCanvas* canvas) const {
    canvas->save();
    // Clamp negative dimensions to prevent Skia crash
    const double w = std::max(0.0, clipRect_.width);
    const double h = std::max(0.0, clipRect_.height);
    canvas->clipRect(SkRect::MakeXYWH(
        static_cast<SkScalar>(clipRect_.x),
        static_cast<SkScalar>(clipRect_.y),
        static_cast<SkScalar>(w),
        static_cast<SkScalar>(h)));
    ContainerLayer::rasterize(canvas);
    canvas->restore();
}

// ── OpacityLayer ────────────────────────────────────────────────────────────

void OpacityLayer::rasterize(SkCanvas* canvas) const {
    canvas->save();
    canvas->saveLayerAlpha(nullptr, static_cast<U8CPU>(alpha_ * 255));
    ContainerLayer::rasterize(canvas);
    canvas->restore();
    canvas->restore();
}

// ── LayerTree ───────────────────────────────────────────────────────────────

LayerTree::LayerTree() : root_(std::make_unique<ContainerLayer>()) {}

void LayerTree::rasterize(SkCanvas* canvas) const {
    if (root_)
        root_->rasterize(canvas);
}

}  // namespace cui_core
