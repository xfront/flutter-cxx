/// @file tests/pipeline_test.cpp
/// @brief Unit tests for the paint pipeline:
///   1. flushPaint really records the frame (no longer a stub).
///   2. markNeedsPaint propagates to the nearest repaint boundary and the
///      frame is re-recorded; clean frames do not re-record.
///   3. paintTree populates the LayerTree and composite() reproduces it.
///   4. PaintingContext's layer helpers (clip / repaint boundary / plain
///      child) record the child's output at the right position and the
///      clip actually applies at raster time.

#include <cassert>
#include <cstdio>
#include <include/core/SkCanvas.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include "cui/foundation/geometry.h"
#include "cui/foundation/layer.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/painting_context.h"
#include "cui/rendering/repaint_boundary.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"

using namespace cui;

namespace {

constexpr SkColor kRed = 0xFFFF0000;
constexpr SkColor kGreen = 0xFF00CC00;

/// A leaf render box that counts performPaint calls and fills its size.
class CountingBox : public RenderBox {
public:
    explicit CountingBox(SkColor color) : color_(color) {}

    int paintCount() const { return paintCount_; }

    void performLayout() override {
        size_ = constraints_.constrain({60, 60});
        paintBounds_ = {0, 0, size_.width, size_.height};
    }

    void performPaint(PaintingContext& context, const Offset& /*offset*/) override {
        // The recording canvas is already translated to our position.
        ++paintCount_;
        SkPaint p;
        p.setColor(color_);
        p.setAntiAlias(false);
        context.canvas()->drawRect(
            SkRect::MakeXYWH(0, 0, static_cast<SkScalar>(size_.width),
                             static_cast<SkScalar>(size_.height)),
            p);
    }

private:
    SkColor color_;
    int paintCount_ = 0;
};

sk_sp<SkSurface> makeSurface(int w, int h) {
    return SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
}

SkColor pixelAt(const sk_sp<SkSurface>& surface, int x, int y) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(1, 1);
    uint32_t px = 0;
    if (!surface->readPixels(info, &px, 4, x, y))
        return 0;
    return px;
}

// 1. flushPaint drives the real painting: the dirty root boundary is
//    re-recorded through PaintingContext; clean frames are not.
void test_flush_paint_drives_painting() {
    RenderView rv;
    rv.setWindowSize({100, 100});
    CountingBox box(kRed);
    rv.setChild(&box);

    PipelineOwner po;
    po.setRootNode(&rv);  // attaches the owner; the root seeds the dirty lists

    po.flushPaint();
    assert(box.paintCount() == 1);

    // Clean frame: nothing re-records.
    po.flushPaint();
    assert(box.paintCount() == 1);

    // A leaf mark resolves to the nearest boundary (the RenderView) and the
    // frame is re-recorded.
    box.markNeedsPaint();
    po.flushPaint();
    assert(box.paintCount() == 2);
}

// 2. paintTree records the frame into a PictureLayer under the root layer
//    and composite() reproduces the recorded content.
void test_paint_tree_layer_structure() {
    RenderView rv;
    rv.setWindowSize({100, 100});
    CountingBox box(kGreen);
    rv.setChild(&box);
    // paintTree replays performPaint, which reads the laid-out size — run a
    // layout pass first.  Loose constraints keep the box at its 60x60
    // natural size inside the 100x100 window.
    box.layout(BoxConstraints(0, 100, 0, 100), true);
    rv.paintTree();

    const auto& children = rv.layerTree().rootLayer()->children();
    assert(children.size() == 1);
    auto* pic = dynamic_cast<PictureLayer*>(children[0].get());
    assert(pic && pic->picture() != nullptr);

    auto surface = makeSurface(100, 100);
    surface->getCanvas()->clear(SK_ColorWHITE);
    rv.composite(surface->getCanvas());
    assert(pixelAt(surface, 30, 30) == kGreen);    // inside the 60x60 box
    assert(pixelAt(surface, 90, 90) == SK_ColorWHITE);
}

// 3. pushClip records the child into a PictureLayer UNDER the ClipRectLayer
//    so the clip applies at raster time.
void test_push_clip_records_into_layer_and_clips() {
    auto surface = makeSurface(100, 100);
    surface->getCanvas()->clear(SK_ColorWHITE);

    ContainerLayer root;
    PaintingContext ctx(&root, surface->getCanvas(), {0, 0, 100, 100});

    CountingBox box(kGreen);
    box.layout(BoxConstraints(60, 60, 60, 60), true);

    ctx.pushClip({0, 0, 25, 60}, &box, {0, 0});
    assert(box.paintCount() == 1);

    const auto& children = root.children();
    assert(children.size() == 1);
    auto* clip = dynamic_cast<ClipRectLayer*>(children[0].get());
    assert(clip && clip->children().size() == 1);

    root.rasterize(surface->getCanvas());
    assert(pixelAt(surface, 10, 30) == kGreen);        // inside the clip
    assert(pixelAt(surface, 50, 30) == SK_ColorWHITE); // clipped away
}

// 4. pushRepaintBoundary positions the picture at the given offset.
void test_push_repaint_boundary_positions_picture() {
    auto surface = makeSurface(100, 100);
    surface->getCanvas()->clear(SK_ColorWHITE);

    ContainerLayer root;
    PaintingContext ctx(&root, surface->getCanvas(), {0, 0, 100, 100});

    CountingBox box(kRed);
    box.layout(BoxConstraints(60, 60, 60, 60), true);

    ctx.pushRepaintBoundary(&box, {30, 20});
    assert(box.paintCount() == 1);

    const auto& children = root.children();
    assert(children.size() == 1);
    auto* pic = dynamic_cast<PictureLayer*>(children[0].get());
    assert(pic && pic->picture() != nullptr);
    const Rect& b = pic->bounds();
    assert(b.x == 30 && b.y == 20 && b.width == 60 && b.height == 60);

    root.rasterize(surface->getCanvas());
    assert(pixelAt(surface, 50, 40) == kRed);          // inside the offset box
    assert(pixelAt(surface, 10, 10) == SK_ColorWHITE); // outside
}

// 5. paintChild on a non-boundary child translates the recording canvas
//    (paintDirect) so the output lands at the requested offset.
void test_paint_direct_offset() {
    auto surface = makeSurface(120, 120);
    surface->getCanvas()->clear(SK_ColorWHITE);

    ContainerLayer root;
    PaintingContext ctx(&root, surface->getCanvas(), {0, 0, 120, 120});

    CountingBox box(kGreen);
    box.layout(BoxConstraints(60, 60, 60, 60), true);

    ctx.paintChild(&box, {40, 30});
    root.rasterize(surface->getCanvas());
    assert(pixelAt(surface, 70, 60) == kGreen);          // 40..100 x 30..90
    assert(pixelAt(surface, 10, 10) == SK_ColorWHITE);
}

// 6. Damage region (frameDamage): drives the Application frame-skip decision.
//    - Re-recording the ROOT boundary dirties the whole screen (damageAll).
//    - An isolated repaint boundary under the root only dirties its own small
//      rect (not the full window) and leaves damageAll false.
//    - A clean frame (nothing re-recorded) yields no damage at all → skippable.
void test_frame_damage_region() {
    // (a) root boundary re-record → whole-screen damage.
    {
        RenderView rv;
        rv.setWindowSize({400, 400});
        CountingBox box(kRed);
        rv.setChild(&box);
        PipelineOwner po;
        po.setRootNode(&rv);
        box.markNeedsPaint();  // no intermediate boundary → resolves to the root
        po.flushPaint();
        assert(po.takeDamageAll());
    }

    // (b) isolated repaint boundary → only its small rect is dirty.
    {
        RenderView rv;
        rv.setWindowSize({400, 400});
        RenderRepaintBoundary rb;
        CountingBox box(kRed);
        rv.setChild(&rb);
        rb.insertChild(&box, nullptr);  // RenderProxyBox 的单槽位子节点挂载
        PipelineOwner po;
        po.setRootNode(&rv);
        // RenderView tight-constrains its child, so drive the boundary's layout
        // directly to keep it at the box's 60x60 natural size.
        rb.layout(BoxConstraints(0, 400, 0, 400), true);
        // adoptChild seeds the root as paint-dirty; flush + consume that first
        // frame so the isolated-boundary assertion below measures ONLY rb.
        po.flushPaint();
        (void)po.takeDamageAll();
        (void)po.takeFrameDamage();
        box.markNeedsPaint();  // stops at rb, the nearest repaint boundary
        po.flushPaint();
        assert(!po.takeDamageAll());
        Rect dmg = po.takeFrameDamage();
        assert(dmg.width == 60 && dmg.height == 60);  // small, not 400x400
    }

    // (c) clean frame → no damage → the frame can be skipped.
    {
        RenderView rv;
        rv.setWindowSize({400, 400});
        RenderRepaintBoundary rb;
        CountingBox box(kRed);
        rv.setChild(&rb);
        rb.insertChild(&box, nullptr);  // RenderProxyBox 的单槽位子节点挂载
        PipelineOwner po;
        po.setRootNode(&rv);
        rb.layout(BoxConstraints(0, 400, 0, 400), true);
        box.markNeedsPaint();
        po.flushPaint();
        (void)po.takeDamageAll();
        (void)po.takeFrameDamage();
        // Nothing re-marked: the next flush records nothing → zero damage.
        po.flushPaint();
        SchedulerBinding::FrameDamage fd;
        fd.all = po.takeDamageAll();
        fd.rect = po.takeFrameDamage();
        assert(!fd.any());
    }
}

}  // namespace

int main() {
    test_flush_paint_drives_painting();
    test_paint_tree_layer_structure();
    test_push_clip_records_into_layer_and_clips();
    test_push_repaint_boundary_positions_picture();
    test_paint_direct_offset();
    test_frame_damage_region();
    std::printf("pipeline_test: all passed\n");
    return 0;
}
