#pragma once
/// @file widgets/image_editor.h
/// @brief ImageEditor —— 裁剪框（8 把手 + 比例锁定）/ 90° 步进旋转 / 水平·垂直翻转 /
///        像素级出图 / PNG 落盘（header-only）。对齐 pub.dev `extended_image` 的
///        editor 层：`lib/src/editor/editor.dart` + `crop_layer.dart` +
///        `editor_utils.dart`（本机已下到 build-*/tmp/ei_*.dart，v3.10.0）。
///
/// 结构（上游是 ExtendedRawImage + Positioned.fill(CustomPaint + 8 个把手)，本仓一条
/// 自绘层全包）：
///   LayoutBuilder → [Listener] → CustomPaint(图 + 蒙层 + 网格线 + 角标)
///
/// 坐标系：全部用「盒子局部坐标」，`layoutRect = cropRectPadding.deflate(0,0,W,H)`。
/// 上游的 `EditActionDetails.layoutTopLeft` 恒等于 (0,0)（绘制空间与层空间同源），
/// 所以 `screenCropRect == cropRect`、`layerDestinationRect == screenDestinationRect`；
/// 本仓把这两层 shift 直接省掉，只保留 `cropRect`（盒子坐标）。
///
/// 有意偏离 / 未移植（勿当 bug 排查）：
///   1. **输入走 Listener**：上游用 8 个 `Positioned + GestureDetector` 当把手热区。
///      本仓把同样的 8 个矩形（同一条 left/top/size 公式）搬进 `handleDown` 自己比，
///      命中即锁定把手；没命中把手就落在裁剪框内/外都算「拖图」（上游外层
///      GestureDetector 的行为）。
///   2. **旋转/翻转「烘进位图」，绘制侧不做画布变换**：本仓先把图翻/转成新位图
///      （`transformedSource`），再画进 `screenDestinationRect`，出图 = 翻转 → 旋转 →
///      按同一张图上的裁剪框取像素 ⇒ 屏幕所见就是落盘的像素。上游相反：原图 +
///      `canvas.transform(T(c)·Rz·Ry·Rx·T(-c))`，裁剪框用 `paintRect` 反变换回**原图**
///      空间再交给平台侧编辑管线，所以它没有「烘焙图」这一层，`rawDestinationRect`
///      也恒按原图尺寸算。烘进位图带来两条自己的口径：
///        · 转 90/270° 后烘焙图宽高互换 ⇒ `rawDestinationRect` 变、图像矩形作废重算，
///          裁剪框按新的长短边重铺，`totalScale` 不动（新框与新图同比例）。
///        · 上游 `flipX/flipY` 与轴名是反的（`flipY` 做水平镜像、`flipX` 做垂直镜像）。
///          本仓改名成 `flipHorizontal/flipVertical`（都指**图像空间**），并按同一个
///          口径修正 `flip()` 里目标框的镜像轴（上游无论翻哪轴都只镜像 dx）。
///   3. **`flip()` 的单键语义保留**：未旋转(0/180°) ⇒ 水平翻，转过 90/270° ⇒ 垂直翻。
///      在屏幕上看两者都是「左右镜像」，与上游一致。
///   4. **自动归中**是一条定长补间（`tickerMs` 延迟 + `animationMs` 线性），
///      对应上游 `Timer.periodic(tickerDuration)` + `AnimationController`（默认
///      curve 就是 linear）。RectTween 的逐边线性插值照抄；每帧的缩放/平移都按
///      上游那样以「上一帧的 cropRect」为基准做**增量**。框大到跑出布局时不起补间，
///      而是同一条公式一次到位（= 上游 `moveUpdate` 里直接调
///      `_doCropAutoCenterAnimation(newScreenCropRect:)` 那一路）。
///   5. **把手最小尺寸沿用上游 `moveUpdate` 里的 `cornerSize.width × 2`**（不是
///      `hitTestSize`）⇒ 默认 30 ⇒ 最小框 60×60。上游这里两个量本来就不一致。
///   6. **删掉上游 editor_utils.dart:305 那句 `if (raw.isSame(raw)) totalScale = 1.0`**
///      —— 自比恒成立，写坏了：它让「图比裁剪框小、需要撑大」那一刻缩放值被强行重置。
///      本仓按语义只保留上面那行 `totalScale /= rect.width / dest.width`。
///   7. **单指拖图只搬运移，不动 `totalScale`**：上游 `_handleScaleUpdate` 把
///      `totalScale = startingScale * details.scale * speed` 无条件写回去，所以
///      `speed != 1` 时单纯拖图也会改缩放值。本仓在 Pan 分支里不写 totalScale。
///   8. 未接：`initEditorConfigHandler`、可替换的 `EditorCropLayerPainter`（颜色/尺寸
///      参数都在，但没有可换的 painter 对象）、`hitTestBehavior`、双指捏合（上游外层
///      GestureDetector 支持，本仓先给滚轮 + 拖拽）、`rawImageData` 重编码保存路径
///      （本仓直接对 SkImage 出图）。动图只编辑首帧。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkRect.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/image_provider.h"
#include "cui/foundation/key.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/widgets/layout.h"
#include "cui/framework/listener.h"
#include "cui/widgets/paint.h"

namespace cui {

// ── 小工具 ──────────────────────────────────────────────────────────────────

/// 上游 `precisionErrorTolerance`（double.epsilon × 10000）在本仓放宽到 1e-9：
/// 浮点几何一路乘除之后，2e-12 已经不是同一个量级了。
constexpr double kImageEditorEps = 1e-9;

inline double clampImageEditorValue(double value, double lower, double upper) {
    return value < lower ? lower : (value > upper ? upper : value);
}
inline bool imageEditorNear(double a, double b) { return std::fabs(a - b) <= kImageEditorEps; }

inline Offset rectCenter(const Rect& r) { return {r.x + r.width / 2.0, r.y + r.height / 2.0}; }
inline Rect rectFromPoints(const Offset& a, const Offset& b) {
    const double left = std::min(a.dx, b.dx);
    const double top = std::min(a.dy, b.dy);
    return Rect(left, top, std::fabs(a.dx - b.dx), std::fabs(a.dy - b.dy));
}
inline Rect rectFromCenter(const Offset& center, double width, double height) {
    return Rect(center.dx - width / 2.0, center.dy - height / 2.0, width, height);
}
inline Rect rectShift(const Rect& r, const Offset& o) {
    return Rect(r.x + o.dx, r.y + o.dy, r.width, r.height);
}
inline bool rectSameEdge(double a, double b) { return imageEditorNear(a, b); }
inline bool rectIsSame(const Rect& a, const Rect& b) {
    return rectSameEdge(a.left(), b.left()) && rectSameEdge(a.top(), b.top()) &&
           rectSameEdge(a.right(), b.right()) && rectSameEdge(a.bottom(), b.bottom());
}
/// 上游 `RectExtension.beyond`：有一条边露到 other 外面。
inline bool rectBeyond(const Rect& r, const Rect& other) {
    return r.left() < other.left() - kImageEditorEps ||
           r.right() > other.right() + kImageEditorEps ||
           r.top() < other.top() - kImageEditorEps ||
           r.bottom() > other.bottom() + kImageEditorEps;
}
/// 上游 `Rect.expandToInclude`（本仓 `Rect::unite` 会把空矩形当无贡献，这里要精确复刻）。
inline Rect rectExpandToInclude(const Rect& a, const Rect& b) {
    const double left = std::min(a.left(), b.left());
    const double top = std::min(a.top(), b.top());
    const double right = std::max(a.right(), b.right());
    const double bottom = std::max(a.bottom(), b.bottom());
    return Rect(left, top, right - left, bottom - top);
}
/// 上游 `EdgeInsets.deflateRect`。
inline Rect deflateRect(const EdgeInsets& insets, const Rect& r) {
    return Rect(r.left() + insets.left, r.top() + insets.top, r.width - insets.horizontal(),
                r.height - insets.vertical());
}

/// 上游 editor_utils.dart 的 `getDestinationRect`（去掉 centerSlice / flipHorizontally
/// 两条本仓用不到的分支）。注意摆位公式是 `halfDelta + alignment·halfDelta`
/// （不是 `inscribe` 的 `halfDelta·(1+alignment)`）—— 默认 center 时两者等价。
inline Rect imageEditorDestinationRect(const Rect& rect, const Size& inputSize, double scale = 1.0,
                                       BoxFit fit = BoxFit::Contain,
                                       const Alignment& alignment = Alignment::center()) {
    const Size scaledInput{inputSize.width / scale, inputSize.height / scale};
    const FittedSizes fitted = applyBoxFit(fit, scaledInput, Size{rect.width, rect.height});
    const double halfWidthDelta = (rect.width - fitted.destination.width) / 2.0;
    const double halfHeightDelta = (rect.height - fitted.destination.height) / 2.0;
    const double dx = halfWidthDelta + alignment.x * halfWidthDelta;
    const double dy = halfHeightDelta + alignment.y * halfHeightDelta;
    return Rect(rect.left() + dx, rect.top() + dy, fitted.destination.width,
                fitted.destination.height);
}

// ── 配置 ────────────────────────────────────────────────────────────────────

/// 上游 `InitCropRectType`：初始裁剪框按图片矩形还是按布局矩形铺。
enum class ImageInitCropRectType { ImageRect, LayoutRect };

/// 上游 `CropAspectRatios` 的常量集（`custom` 在本仓是 `std::nullopt`，
/// `original` 是 0.0 ⇒ 用图片自身的宽高比）。
struct CropAspectRatios {
    static constexpr double kOriginal = 0.0;
    static constexpr double kRatio1_1 = 1.0;
    static constexpr double kRatio3_4 = 3.0 / 4.0;
    static constexpr double kRatio4_3 = 4.0 / 3.0;
    static constexpr double kRatio9_16 = 9.0 / 16.0;
    static constexpr double kRatio16_9 = 16.0 / 9.0;
};

/// 上游 `EditorConfig`（去掉 `hitTestBehavior` 与可替换 painter 两条，见文件头第 7 条）。
struct ImageEditorConfig {
    double maxScale = 5.0;
    EdgeInsets cropRectPadding = EdgeInsets::all(20.0);
    /// 角标形状（上游默认 30×5：宽 30、厚 5）。
    Size cornerSize = Size(30.0, 5.0);
    std::optional<Color> cornerColor;  ///< 缺省 = theme primary
    std::optional<Color> lineColor;    ///< 缺省 = theme background × 0.7
    double lineHeight = 0.6;
    std::optional<Color> maskColor;    ///< 缺省 = theme background × (按下 0.4 / 松开 0.8)
    double hitTestSize = 20.0;         ///< 把手热区半径（热区 = 2×这个值）
    double animationMs = 200.0;        ///< 归中补间时长
    double tickerMs = 400.0;           ///< 抬手后多久起归中（上游 Timer.periodic）
    std::optional<double> cropAspectRatio;       ///< nullopt = 自由比例
    std::optional<double> initialCropAspectRatio; ///< 只影响初始框
    ImageInitCropRectType initCropRectType = ImageInitCropRectType::ImageRect;
    double speed = 1.0;
    bool reverseMousePointerScrollDirection = false;

    bool operator==(const ImageEditorConfig& o) const {
        return maxScale == o.maxScale && cropRectPadding == o.cropRectPadding &&
               cornerSize == o.cornerSize && cornerColor == o.cornerColor &&
               lineColor == o.lineColor && lineHeight == o.lineHeight &&
               maskColor == o.maskColor && hitTestSize == o.hitTestSize &&
               animationMs == o.animationMs && tickerMs == o.tickerMs &&
               cropAspectRatio == o.cropAspectRatio &&
               initialCropAspectRatio == o.initialCropAspectRatio &&
               initCropRectType == o.initCropRectType && speed == o.speed &&
               reverseMousePointerScrollDirection == o.reverseMousePointerScrollDirection;
    }
    bool operator!=(const ImageEditorConfig& o) const { return !(*this == o); }
};

// ── EditActionDetails ───────────────────────────────────────────────────────

/// 上游 `EditActionDetails`：把「图怎么被翻/转、裁剪框在哪、图相对框缩放到几倍」
/// 全部收在一个值对象里，绘制侧每帧调 `getFinalDestinationRect()` 收敛。
class ImageEditAction {
public:
    // ── 公开的缩放/平移累加（上游是公有字段）──────────────────────────────
    double totalScale = 1.0;
    double preTotalScale = 1.0;
    Offset delta{};
    std::optional<Offset> screenFocalPoint;
    EdgeInsets cropRectPadding = EdgeInsets::all(20.0);
    std::optional<Rect> cropRect;  ///< 盒子坐标（= 上游 screenCropRect）

    /// 原始（未缩放）宽高比；`cropAspectRatio` 读它会做半圈翻转。
    std::optional<double> originalAspectRatio;

    void setRawCropAspectRatio(std::optional<double> value) { rawCropAspectRatio_ = value; }

    // ── 状态读数 ──────────────────────────────────────────────────────────
    double rotateRadian() const { return rotateRadian_; }
    bool flipHorizontal() const { return flipHorizontal_; }
    bool flipVertical() const { return flipVertical_; }
    /// 0/±90/±180/±270 的「度数」（上游 `rotateAngle` 用 `~/ (pi/2) * 90`，对负角向下取整）。
    double rotateAngle() const { return std::floor(rotateRadian_ / (kPi / 2.0)) * 90.0; }
    bool isHalfPi() const { return !imageEditorNear(std::fmod(rotateRadian_, kPi), 0.0); }
    bool isTwoPi() const { return imageEditorNear(std::fmod(rotateRadian_, 2.0 * kPi), 0.0); }
    bool isPi() const { return !isHalfPi() && !isTwoPi(); }
    bool hasRotateAngle() const { return !isTwoPi(); }
    bool hasEditAction() const { return hasRotateAngle() || flipHorizontal_ || flipVertical_;
    }
    bool needFlip() const { return flipHorizontal_ || flipVertical_; }
    bool reachCropRectEdge() const { return reachCropRectEdge_; }
    /// 上游 getter：转过 90/270° 时宽高比对调。
    std::optional<double> cropAspectRatio() const {
        if (!rawCropAspectRatio_)
            return std::nullopt;
        const double r = *rawCropAspectRatio_;
        return isHalfPi() ? 1.0 / r : rawCropAspectRatio_;
    }

    const std::optional<Rect>& screenDestinationRect() const { return screenDestinationRect_; }
    void setScreenDestinationRect(const Rect& r) { screenDestinationRect_ = r; }
    Rect layoutRect() const { return layoutRect_; }

    /// 上游 `layerDestinationRect`（layoutTopLeft 恒 0 ⇒ 就是自身）。
    const std::optional<Rect>& layerDestinationRect() const { return screenDestinationRect_; }

    // ── 操作 ──────────────────────────────────────────────────────────────

    /// 上游 `initRect`：布局或原图矩形变了就把已收敛的绘制矩形作废。
    void initRect(const Rect& layoutRect, const Rect& destinationRect) {
        if (!(layoutRect_ == layoutRect)) {
            layoutRect_ = layoutRect;
            screenDestinationRect_.reset();
        }
        if (!rawDestinationRect_ || !rectIsSame(*rawDestinationRect_, destinationRect)) {
            rawDestinationRect_ = destinationRect;
            screenDestinationRect_.reset();
        }
    }

    /// 上游 `rotate(angle, layoutRect, fit)`：角度是**弧度**，正角顺时针。
    /// 本仓把旋转烘进位图（文件头第 2 条），所以宽高互换后：裁剪框按新的长短边重铺、
    /// 图像矩形作废重算（下一帧 `getFinalDestinationRect` 从新尺寸的 contain-fit 出发），
    /// `totalScale` 保持不变 —— 新框与新图同比例，框不会跑出图外。
    void rotate(double angle, const Rect& layoutRect) {
        if (!cropRect)
            return;
        rotateRadian_ = std::fmod(rotateRadian_ + angle, 2.0 * kPi);
        if (flipHorizontal_ && flipVertical_ && isPi()) {
            flipHorizontal_ = flipVertical_ = false;
            rotateRadian_ = 0.0;
        }
        cropRect = imageEditorDestinationRect(layoutRect,
                                             Size{cropRect->height, cropRect->width}, 1.0,
                                             BoxFit::Contain);
        screenDestinationRect_.reset();
    }

    /// 图像空间水平镜像（左右翻）。
    void flipHorizontalNow() { toggleFlip(true); }
    /// 图像空间垂直镜像（上下翻）。
    void flipVerticalNow() { toggleFlip(false); }

    /// 上游 `flip()` 的单键语义（见文件头第 3 条）。
    void flip() {
        if (isHalfPi())
            flipVerticalNow();
        else
            flipHorizontalNow();
    }

    /// 上游 `getFinalDestinationRect()`：每帧收敛「缩放 → 平移 → 夹边界 → 撑满框」。
    Rect getFinalDestinationRect() {
        reachCropRectEdge_ = false;
        if (!screenDestinationRect_) {
            if (!rawDestinationRect_ || !cropRect)
                return Rect();
            screenDestinationRect_ = getRectWithScale(*rawDestinationRect_);
            screenDestinationRect_ = computeBoundary(*screenDestinationRect_, *cropRect);
            return *screenDestinationRect_;
        }

        const double scaleDelta = totalScale / preTotalScale;
        if (!imageEditorNear(scaleDelta, 1.0)) {
            Offset focal = screenFocalPoint ? *screenFocalPoint : rectCenter(*screenDestinationRect_);
            focal = Offset(clampImageEditorValue(focal.dx, screenDestinationRect_->left(),
                                                 screenDestinationRect_->right()),
                           clampImageEditorValue(focal.dy, screenDestinationRect_->top(),
                                                 screenDestinationRect_->bottom()));
            *screenDestinationRect_ = Rect(
                focal.dx - (focal.dx - screenDestinationRect_->left()) * scaleDelta,
                focal.dy - (focal.dy - screenDestinationRect_->top()) * scaleDelta,
                screenDestinationRect_->width * scaleDelta,
                screenDestinationRect_->height * scaleDelta);
            preTotalScale = totalScale;
            delta = Offset();
        } else if (cropRect && !rectIsSame(*screenDestinationRect_, *cropRect)) {
            if (rectSameEdge(screenDestinationRect_->top(), cropRect->top()) &&
                rectSameEdge(screenDestinationRect_->bottom(), cropRect->bottom()))
                delta = Offset(delta.dx, 0.0);
            else if (rectSameEdge(screenDestinationRect_->left(), cropRect->left()) &&
                     rectSameEdge(screenDestinationRect_->right(), cropRect->right()))
                delta = Offset(0.0, delta.dy);
            *screenDestinationRect_ = rectShift(*screenDestinationRect_, delta);
            delta = Offset();
        } else {
            // 上游在这里无条件清一次 delta（框已就位时不搬运移量）。
            delta = Offset();
        }

        if (cropRect)
            *screenDestinationRect_ = computeBoundary(*screenDestinationRect_, *cropRect);

        // 图必须盖满裁剪框：盖不住就把图撑大（并把缩放值同步）。
        if (cropRect && screenDestinationRect_) {
            Rect expanded = rectExpandToInclude(*cropRect, *screenDestinationRect_);
            if (!rectIsSame(expanded, *screenDestinationRect_)) {
                const bool topSame = rectSameEdge(expanded.top(), cropRect->top());
                const bool leftSame = rectSameEdge(expanded.left(), cropRect->left());
                const bool bottomSame = rectSameEdge(expanded.bottom(), cropRect->bottom());
                const bool rightSame = rectSameEdge(expanded.right(), cropRect->right());
                if (topSame && bottomSame) {
                    expanded = rectFromCenter(
                        rectCenter(expanded),
                        expanded.height / screenDestinationRect_->height *
                            screenDestinationRect_->width,
                        expanded.height);
                    reachCropRectEdge_ = true;
                } else if (leftSame && rightSame) {
                    expanded = rectFromCenter(
                        rectCenter(expanded), expanded.width,
                        expanded.width / screenDestinationRect_->width *
                            screenDestinationRect_->height);
                    reachCropRectEdge_ = true;
                }
                // 文件头第 6 条：上游这里还有一句自比恒成立的 totalScale = 1.0。
                totalScale = totalScale / (expanded.width / screenDestinationRect_->width);
                preTotalScale = totalScale;
                *screenDestinationRect_ = expanded;
            }
        }
        return *screenDestinationRect_;
    }

    Rect getRectWithScale(const Rect& rect) const {
        const double width = rect.width * totalScale;
        const double height = rect.height * totalScale;
        const Offset center = rectCenter(rect);
        return Rect(center.dx - width / 2.0, center.dy - height / 2.0, width, height);
    }

    /// 上游 `computeBoundary`：只在「另一个方向也顶到边」时才锁该轴（记住上一次状态）。
    Rect computeBoundary(const Rect& resultIn, const Rect& layoutRect) {
        Rect result = resultIn;
        if (computeHorizontalBoundary_) {
            if (result.left() >= layoutRect.left() - kImageEditorEps)
                result = Rect(layoutRect.left(), result.top(), result.width, result.height);
            if (result.right() <= layoutRect.right() + kImageEditorEps)
                result = Rect(layoutRect.right() - result.width, result.top(), result.width,
                              result.height);
        }
        if (computeVerticalBoundary_) {
            if (result.bottom() <= layoutRect.bottom() + kImageEditorEps)
                result = Rect(result.left(), layoutRect.bottom() - result.height, result.width,
                              result.height);
            if (result.top() >= layoutRect.top() - kImageEditorEps)
                result = Rect(result.left(), layoutRect.top(), result.width, result.height);
        }
        computeHorizontalBoundary_ = result.left() <= layoutRect.left() + kImageEditorEps &&
                                     result.right() >= layoutRect.right() - kImageEditorEps;
        computeVerticalBoundary_ = result.top() <= layoutRect.top() + kImageEditorEps &&
                                   result.bottom() >= layoutRect.bottom() - kImageEditorEps;
        return result;
    }

    /// 归一化到 [0, 2π)，与上游 `% 2*pi` 在负角上的差异（Dart 取模非负，C++ 可负）。
    static constexpr double kPi = 3.14159265358979323846;

    void normalize() {
        rotateRadian_ = std::fmod(rotateRadian_, 2.0 * kPi);
        if (rotateRadian_ < 0.0)
            rotateRadian_ += 2.0 * kPi;
    }

private:
    void toggleFlip(bool horizontal) {
        if (!cropRect || !screenDestinationRect_)
            return;
        const Offset origin = rectCenter(*cropRect);
        if (horizontal) {
            flipHorizontal_ = !flipHorizontal_;
            *screenDestinationRect_ =
                Rect(2.0 * origin.dx - screenDestinationRect_->right(),
                     screenDestinationRect_->top(),
                     screenDestinationRect_->width,
                     screenDestinationRect_->height);
        } else {
            flipVertical_ = !flipVertical_;
            *screenDestinationRect_ =
                Rect(screenDestinationRect_->left(),
                     2.0 * origin.dy - screenDestinationRect_->bottom(),
                     screenDestinationRect_->width,
                     screenDestinationRect_->height);
        }
        if (flipHorizontal_ && flipVertical_ && isPi()) {
            flipHorizontal_ = flipVertical_ = false;
            rotateRadian_ = 0.0;
        }
    }

    double rotateRadian_ = 0.0;
    bool flipHorizontal_ = false;
    bool flipVertical_ = false;
    bool computeHorizontalBoundary_ = false;
    bool computeVerticalBoundary_ = false;
    bool reachCropRectEdge_ = false;
    std::optional<double> rawCropAspectRatio_;
    Rect layoutRect_{};
    std::optional<Rect> screenDestinationRect_;
    std::optional<Rect> rawDestinationRect_;
};

// ── ImageEditor ─────────────────────────────────────────────────────────────

class ImageEditor : public StatefulWidget {
public:
    using EditChangedCallback = std::function<void(const ImageEditAction&)>;
    using LoadingBuilder = std::function<SharedPtr<Widget>(BuildContext&, double progress)>;
    using ErrorBuilder = std::function<SharedPtr<Widget>(BuildContext&, const std::string& error)>;

    /// 参数按「图源 → 配置 → 占位 → 回调」分组，新增参数一律追加在 `key` 之前。
    /// `decoded` 非空时直接用它，不再发起加载（测试与「编辑已有位图」的入口）。
    explicit ImageEditor(std::shared_ptr<ImageProvider> provider = nullptr,
                         ImageEditorConfig config = {}, DecodedImagePtr decoded = nullptr,
                         LoadingBuilder loadingBuilder = nullptr,
                         ErrorBuilder errorBuilder = nullptr,
                         EditChangedCallback onEditChanged = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), provider_(std::move(provider)), config_(config),
          decoded_(std::move(decoded)), loadingBuilder_(std::move(loadingBuilder)),
          errorBuilder_(std::move(errorBuilder)), onEditChanged_(std::move(onEditChanged)) {}

    const std::shared_ptr<ImageProvider>& provider() const { return provider_; }
    void setProvider(std::shared_ptr<ImageProvider> p) { provider_ = std::move(p); }

    const ImageEditorConfig& config() const { return config_; }
    void setConfig(const ImageEditorConfig& c) { config_ = c; }

    const DecodedImagePtr& decoded() const { return decoded_; }
    void setDecoded(DecodedImagePtr d) { decoded_ = std::move(d); }

    const LoadingBuilder& loadingBuilder() const { return loadingBuilder_; }
    void setLoadingBuilder(LoadingBuilder b) { loadingBuilder_ = std::move(b); }
    const ErrorBuilder& errorBuilder() const { return errorBuilder_; }
    void setErrorBuilder(ErrorBuilder b) { errorBuilder_ = std::move(b); }
    const EditChangedCallback& onEditChanged() const { return onEditChanged_; }
    void setOnEditChanged(EditChangedCallback cb) { onEditChanged_ = std::move(cb); }

    std::string loadSignature() const {
        if (decoded_)
            return "decoded:" + std::to_string(decoded_->width()) + "x" +
                   std::to_string(decoded_->height());
        if (!provider_)
            return std::string();
        return provider_->key() + "|" + std::to_string(int(config_.maxScale));
    }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ImageEditor"; }

private:
    std::shared_ptr<ImageProvider> provider_;
    ImageEditorConfig config_;
    DecodedImagePtr decoded_;
    LoadingBuilder loadingBuilder_;
    ErrorBuilder errorBuilder_;
    EditChangedCallback onEditChanged_;
};

// ── 把手类型 ────────────────────────────────────────────────────────────────

/// 上游私有枚举 `_MoveType`（8 个把手）+「拖图」这一档（上游走外层 GestureDetector）。
enum class ImageCropHandle {
    None,
    Pan,
    TopLeft,
    TopRight,
    BottomRight,
    BottomLeft,
    Top,
    Right,
    Bottom,
    Left,
};

// ── ImageEditorState ────────────────────────────────────────────────────────

class ImageEditorState : public State {
public:
    void initState() override {
        auto* w = widget_();
        ticker_ = std::make_unique<Ticker>(
            [this](Duration elapsed) { advanceAutoCenter(elapsed.count()); });
        if (w && (w->provider() || w->decoded()))
            startLoad();
    }

    void dispose() override {
        if (request_) {
            request_->cancel();
            request_.reset();
        }
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    void didUpdateWidget(cui::Widget* oldWidget) override {
        State::didUpdateWidget(oldWidget);
        auto* w = widget_();
        if (!w)
            return;
        const std::string sig = w->loadSignature();
        if (sig != signature_) {
            signature_ = sig;
            startLoad();
        }
    }

    // ── 对外读数（测试与宿主都从这里问）────────────────────────────────────

    ImageEditAction& action() { return action_; }
    const ImageEditAction& action() const { return action_; }
    const DecodedImagePtr& decodedImage() const { return image_; }
    Rect layoutRect() const { return layoutRect_; }
    Size outerSize() const { return outerSize_; }
    std::optional<Rect> cropRect() const { return action_.cropRect; }
    bool pointerDown() const { return pointerDown_; }
    bool isCenterAnimating() const { return centerAnimating_; }
    ImageCropHandle activeHandle() const { return activeHandle_; }
    const std::string& error() const { return error_; }
    double progress() const { return progress_; }

    /// 裁剪框换算到「烘焙后图像素空间」（见文件头第 2 条）。无图为 / 无框 / 零尺寸 ⇒ nullopt。
    /// 会先把图像矩形收敛一次，所以宿主/测试**不需要先走绘制**才能出图。
    std::optional<Rect> cropRectInPixels() {
        converge();
        const sk_sp<SkImage> transformed = transformedSource();
        const std::optional<Rect>& destOpt = action_.screenDestinationRect();
        if (!transformed || !action_.cropRect || !destOpt)
            return std::nullopt;
        const Rect& dest = *destOpt;
        if (dest.width <= 0.0 || dest.height <= 0.0)
            return std::nullopt;
        const Rect& crop = *action_.cropRect;
        const double ratioX = double(transformed->width()) / dest.width;
        const double ratioY = double(transformed->height()) / dest.height;
        Rect out((crop.left() - dest.left()) * ratioX, (crop.top() - dest.top()) * ratioY,
                 crop.width * ratioX, crop.height * ratioY);
        out = clampToPixels(out, transformed->width(), transformed->height());
        if (out.width < 1.0 || out.height < 1.0)
            return std::nullopt;
        return out;
    }

    /// 出图：翻转 → 旋转 → 裁剪。任何一步缺条件就返回当前能给的（无裁剪框 ⇒ 整图变换后）。
    sk_sp<SkImage> produceImage() {
        sk_sp<SkImage> out = transformedSource();
        if (!out)
            return out;
        if (const auto cropPx = cropRectInPixels()) {
            const SkIRect rect = SkIRect::MakeLTRB(int(std::floor(cropPx->left())),
                                                   int(std::floor(cropPx->top())),
                                                   int(std::ceil(cropPx->right())),
                                                   int(std::ceil(cropPx->bottom())));
            if (auto cropped = cropImage(out, rect))
                out = cropped;
        }
        return out;
    }

    /// 出图并编码成 PNG 写盘；失败返回 false（无解码结果 / 无像素 / 写不进）。
    bool savePng(const std::string& path) {
        const sk_sp<SkImage> out = produceImage();
        if (!out)
            return false;
        const sk_sp<SkData> data = encodePng(out);
        if (!data || data->size() == 0)
            return false;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(reinterpret_cast<const char*>(data->data()),
                std::streamsize(data->size()));
        // 立刻 flush 并查状态：共享目录 / 满盘这类写入失败只会在 flush 那一刻暴露，
        // 不 flush 就成了「返回 true 但落盘 0 字节」。
        f.flush();
        return f.good();
    }

    // ── 编辑动作（宿主按钮直接调）──────────────────────────────────────────

    /// 右转（默认）90°；`right=false` 左转。上游走 `_layerKey.currentState.layoutRect`
    /// 也就是**加了 padding 的**布局矩形，这里同口径。
    void rotate(bool right = true) {
        applyEdit([&](ImageEditAction& a) {
            a.rotate(right ? ImageEditAction::kPi / 2.0 : -ImageEditAction::kPi / 2.0,
                     layoutRect_);
            a.normalize();
        });
    }
    void flip() { applyEdit([](ImageEditAction& a) { a.flip(); }); }
    void flipHorizontal() { applyEdit([](ImageEditAction& a) { a.flipHorizontalNow(); }); }
    void flipVertical() { applyEdit([](ImageEditAction& a) { a.flipVerticalNow(); }); }

    /// 换比例锁定：与上游一致 —— 比例变了就把已做的编辑整体作废（裁剪框重新铺）。
    void setCropAspectRatio(std::optional<double> ratio) {
        auto* w = widget_();
        if (!w)
            return;
        ImageEditorConfig c = w->config();
        if (c.cropAspectRatio == ratio)
            return;
        c.cropAspectRatio = ratio;
        w->setConfig(c);
        reset();
    }

    /// 上游 `reset()`：配置回到 widget 当前值、编辑状态全清。
    void reset() {
        stopAutoCenter();
        action_ = ImageEditAction();
        syncConfig();
        if (mounted())
            setState();
    }

    /// 推进归中补间到 `elapsedMs`（从起表算）。Ticker 按真实时钟调它；测试直接传一个
    /// 大于 `tickerMs + animationMs` 的值就能一次性收敛。
    void advanceAutoCenter(double elapsedMs) {
        if (!centerPending_)
            return;
        auto* w = widget_();
        const double waitMs = w ? std::max(0.0, w->config().tickerMs) : 400.0;
        if (!centerStarted_) {
            if (elapsedMs < waitMs)
                return;
            if (!beginCenterTween())
                return;
        }
        const double total = std::max(1.0, w ? w->config().animationMs : 200.0);
        // 上游 RectTween 的时间基是 AnimationController 自己的进度，起表那刻归零，
        // 所以这里从「越过延迟的那一刻」重新计时。
        const double sinceStart = std::max(0.0, elapsedMs - waitMs);
        const double t = clampImageEditorValue(sinceStart / total, 0.0, 1.0);
        applyCenterTween(t);
        if (t < 1.0)
            return;
        centerPending_ = false;
        centerStarted_ = false;
        centerAnimating_ = false;
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        if (mounted())
            setState();
    }

    // ── 指针（Listener 回调；测试用注入事件驱动）────────────────────────────

    void handleDown(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || !action_.cropRect)
            return;
        stopAutoCenter();
        setPointerDown(true);
        activeHandle_ = hitHandle(e.position);
        startingScale_ = action_.totalScale;
        startingFocal_ = e.position;
        detailsScale_ = 1.0;
        action_.screenFocalPoint = e.position;
        if (activeHandle_ == ImageCropHandle::None)
            activeHandle_ = ImageCropHandle::Pan;
    }

    void handleMove(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || activeHandle_ == ImageCropHandle::None || centerAnimating_)
            return;
        const double speed = w->config().speed;
        if (activeHandle_ == ImageCropHandle::Pan) {
            // 上游 _handleScaleUpdate 的单指分支：details.scale 恒 1，只有 focal 位移。
            // 位移公式照抄 `focalPoint * speed - startingOffset`（基准不带 speed 是上游原样）。
            const Offset delta = e.position * speed - startingFocal_;
            startingFocal_ = e.position;
            action_.screenFocalPoint = e.position;
            if (delta.dx == 0.0 && delta.dy == 0.0)
                return;
            action_.delta = action_.delta + delta;
            notifyChanged();
            if (mounted())
                setState();
            return;
        }
        // 把手拖动的增量自己从位置算（= 上游 details.delta），不依赖事件里的 delta 字段。
        const Offset step = (e.position - startingFocal_) * speed;
        if (step.dx == 0.0 && step.dy == 0.0)
            return;
        startingFocal_ = e.position;
        moveUpdate(activeHandle_, step);
    }

    void handleUp(const PointerEvent&) {
        auto* w = widget_();
        setPointerDown(false);
        if (activeHandle_ != ImageCropHandle::None && activeHandle_ != ImageCropHandle::Pan) {
            activeHandle_ = ImageCropHandle::None;
            startAutoCenter();
            return;
        }
        activeHandle_ = ImageCropHandle::None;
        if (w && mounted())
            setState();
    }

    /// 上游 `_handlePointerSignal`：`scale = 1 + delta·speed/1000`，再走一遍
    /// scaleStart + scaleUpdate（位置不变 ⇒ 只有缩放，没有位移）。
    void handleScroll(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || !action_.cropRect || centerAnimating_)
            return;
        const double dy = e.scrollDeltaY;
        const double dx = e.scrollDeltaX;
        double raw = (std::fabs(dy) > std::fabs(dx) ? dy : dx) * w->config().speed / 1000.0;
        if (w->config().reverseMousePointerScrollDirection)
            raw = -raw;
        handleDown(e);
        applyScale(1.0 + raw, e.position);
    }

    SharedPtr<Widget> build(BuildContext& /*context*/) override {
        auto* w = widget_();
        if (!w)
            return nullptr;
        syncConfig();

        return cui::make_shared<LayoutBuilder>(
            [this](BuildContext& ctx, const BoxConstraints& constraints) {
                return buildSized(ctx, constraints);
            });
    }

private:
    ImageEditor* widget_() const { return static_cast<ImageEditor*>(widget()); }

    // ── 加载 ────────────────────────────────────────────────────────────────

    void startLoad() {
        auto* w = widget_();
        if (!w)
            return;
        if (request_) {
            request_->cancel();
            request_.reset();
        }
        if (w->decoded()) {
            image_ = w->decoded();
            signature_ = w->loadSignature();
            loading_ = false;
            failed_ = false;
            error_.clear();
            progress_ = 1.0;
            return;
        }
        if (!w->provider()) {
            image_.reset();
            loading_ = false;
            return;
        }
        ImageRequestOptions opts;
        opts.decodeAllFrames = false;  // 编辑器只处理首帧（见文件头第 7 条）
        opts.useMemoryCache = true;
        opts.useDiskCache = false;
        signature_ = w->loadSignature();
        loading_ = true;
        failed_ = false;
        error_.clear();
        progress_ = -1.0;
        // 缓存命中会在返回前就把结果打过来，这段窗口里不能 setState。
        synchronousDelivery_ = true;
        request_ = loadImage(w->provider(), opts, [this](const ImageLoadResult& r) {
            handleResult(r);
        });
        synchronousDelivery_ = false;
    }

    void handleResult(const ImageLoadResult& result) {
        const bool sync = synchronousDelivery_;
        loading_ = false;
        request_.reset();
        if (result.ok()) {
            image_ = result.image;
            failed_ = false;
            error_.clear();
            progress_ = 1.0;
        } else {
            image_.reset();
            failed_ = true;
            error_ = result.error.empty() ? std::string("image load failed") : result.error;
        }
        if (!sync && mounted())
            setState();
    }

    // ── 配置同步 ────────────────────────────────────────────────────────────

    /// 上游 `_initGestureConfig`：每帧（build）都过一遍，比例换了就作废编辑状态。
    void syncConfig() {
        auto* w = widget_();
        if (!w)
            return;
        ImageEditorConfig config = w->config();
        if (config.cropAspectRatio && *config.cropAspectRatio <= 0.0)
            config.cropAspectRatio = action_.originalAspectRatio;
        if (config_.cropAspectRatio != config.cropAspectRatio)
            action_ = ImageEditAction();
        config_ = config;
        action_.cropRectPadding = config_.cropRectPadding;
        action_.setRawCropAspectRatio(config_.cropAspectRatio);
        if (image_ && image_->valid())
            action_.originalAspectRatio = double(image_->width()) / double(image_->height());
    }

    // ── 布局与绘制 ──────────────────────────────────────────────────────────

    SharedPtr<Widget> buildSized(BuildContext& context, const BoxConstraints& constraints) {
        outerSize_ = constraints.biggest();
        layoutRect_ = deflateRect(config_.cropRectPadding,
                                 Rect(0.0, 0.0, outerSize_.width, outerSize_.height));

        const sk_sp<SkImage> source = image_ ? image_->image() : nullptr;
        // 本仓的 LayoutBuilder 在拿到真实约束之前，挂载期会先用默认（0×0 紧）约束把
        // builder 跑一次 ⇒ 这一帧 `layoutRect_` 是 deflate 出来的负尺寸。几何必须等
        // 正尺寸那一次再建，否则 `cropRect` 被锁成废值、后面再也不会重算（它只认
        // 「还没建过」）。
        if (source && !action_.cropRect && layoutRect_.width > 0.0 && layoutRect_.height > 0.0)
            initGeometry(*source);

        auto* w = widget_();
        if (!source && w) {
            if (failed_ || (!loading_ && !w->provider())) {
                if (const auto& b = w->errorBuilder())
                    return b(context, error_);
                return nullptr;
            }
            if (const auto& b = w->loadingBuilder())
                return b(context, progress_);
            return nullptr;
        }

        CustomPaint::PainterCallback painter = [this](Painter& p, const Size& size) {
            paintLayer(p, size);
        };
        SharedPtr<Widget> box = cui::make_shared<CustomPaint>(std::move(painter), nullptr,
                                                             outerSize_, nullptr);
        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { handleDown(e); },
            [this](const PointerEvent& e) { handleMove(e); },
            [this](const PointerEvent& e) { handleUp(e); }, Listener::PointerCallback{},
            [this](const PointerEvent& e) { handleScroll(e); }, std::move(box));
    }

    /// 上游在 `painting.dart` 里做的 `initRect` + `editor.dart` 里做的初始裁剪框
    /// （`_initCropRect` / `_calculateCropRectFromAspectRatio` / `InitCropRectType`）。
    void initGeometry(const SkImage& source) {
        const Size inputSize{double(source.width()), double(source.height())};
        const Rect raw = imageEditorDestinationRect(layoutRect_, inputSize, 1.0, BoxFit::Contain);
        action_.initRect(layoutRect_, raw);
        if (!action_.cropRect) {
            Rect crop = initCropRect(raw);
            if (config_.initCropRectType == ImageInitCropRectType::LayoutRect &&
                action_.cropAspectRatio() && *action_.cropAspectRatio() > 0.0) {
                // 锁比例时铺满「布局矩形」而不是图片矩形，并把图像撑到同样大小
                // （上游 totalScale/preTotalScale 那一行按图片的长短边取比值）。
                const Rect byLayout = initCropRect(layoutRect_);
                action_.totalScale = action_.preTotalScale =
                    raw.width > raw.height ? byLayout.height / crop.height
                                           : byLayout.width / crop.width;
                crop = byLayout;
            }
            action_.cropRect = crop;
        }
        // 立刻收敛一次：首帧绘制前 `screenDestinationRect` 就得有值，否则宿主先调
        // `produceImage()`/`savePng()` 拿到的是 nullopt。
        converge();
    }

    /// 上游 `_initCropRect`：锁定比例 > 初始比例 > 按当前缩放铺满。
    Rect initCropRect(const Rect& rect) const {
        if (const auto ratio = action_.cropAspectRatio())
            return cropRectFromAspectRatio(rect, *ratio);
        if (config_.initialCropAspectRatio)
            return cropRectFromAspectRatio(rect, *config_.initialCropAspectRatio);
        return action_.getRectWithScale(rect);
    }

    /// 上游 `_calculateCropRectFromAspectRatio`（锚在 `getRectWithScale(rect)` 的中心）。
    Rect cropRectFromAspectRatio(const Rect& rect, double aspectRatio) const {
        const Rect scaled = action_.getRectWithScale(rect);
        const double height = std::min(scaled.height, scaled.width / aspectRatio);
        const double width = height * aspectRatio;
        return rectFromCenter(rectCenter(scaled), width, height);
    }

    /// 把图像矩形按「当前烘焙图 + 当前 layoutRect」收敛一次：`initRect`（布局或图片
    /// 尺寸变了就作废重算）+ `getFinalDestinationRect`（缩放 → 平移 → 夹边界 → 撑满框）。
    /// 绘制侧每帧走它，出图侧（`cropRectInPixels`）也走它 ⇒ 不绘制也能出图。
    void converge();
    void paintLayer(Painter& painter, const Size& size);

    // ── 把手命中与移动 ──────────────────────────────────────────────────────

    ImageCropHandle hitHandle(const Offset& p) const;
    void moveUpdate(ImageCropHandle handle, const Offset& delta);
    Rect handleAspectRatio(ImageCropHandle handle, const Rect& in, const Rect& bounds,
                           const Offset& delta) const;
    std::optional<Rect> doWithMaxScale(const Rect& rect) const;

    // ── 归中动画 ────────────────────────────────────────────────────────────

    void startAutoCenter() {
        if (centerAnimating_ || !action_.cropRect)
            return;
        centerPending_ = true;
        centerStarted_ = false;
        centerFrom_ = Rect();
        centerTo_ = Rect();
        if (ticker_) {
            if (ticker_->isActive())
                ticker_->stop();
            ticker_->start();
        }
    }
    void stopAutoCenter() {
        centerPending_ = false;
        centerStarted_ = false;
        centerAnimating_ = false;
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
    }
    /// 上游 `_startTimer` 里那段「算出目标框」= `getDestinationRect(layoutRect,
    /// cropSize, contain)`。返回 false = 条件不足（没有框 / 框已归位）。
    bool beginCenterTween();
    /// 上游 `_doCropAutoCenterAnimation(newScreenCropRect)`：缩放优先、再平移。
    /// tween 每帧调一次，`old` 就是「上一帧落下的 cropRect」，所以逐帧的
    /// scale/offset 都是**增量**（与上游同形，不是从固定起点做绝对插值）。
    void applyCropRectTo(const Rect& newCropRect);
    void applyCenterTween(double t);
    /// 上游 `moveUpdate` 里「框大到超出布局」那一路：调用方已把 cropRect 跳到大框，
    /// 这里用同一条公式一次性收敛到布局内的居中框（不起补间）。
    void centerOversizedRect(const Rect& targetCropRect);

    // ── 杂项 ────────────────────────────────────────────────────────────────

    void applyScale(double scale, const Offset& focal);
    void applyEdit(const std::function<void(ImageEditAction&)>& fn) {
        if (!action_.cropRect)
            return;
        fn(action_);
        // 编辑动作可能作废图像矩形（rotate 会 reset 它），当场重算，
        // 宿主在同一次调用里读 `cropRectInPixels()`/`produceImage()` 才拿得到新值。
        converge();
        notifyChanged();
        if (mounted())
            setState();
    }
    void notifyChanged() {
        auto* w = widget_();
        if (w && w->onEditChanged())
            w->onEditChanged()(action_);
    }
    void setPointerDown(bool down) {
        if (pointerDown_ == down)
            return;
        pointerDown_ = down;
        if (mounted())
            setState();
    }
    /// 先翻转再旋转，得到「出图空间」的那张图。
    sk_sp<SkImage> transformedSource() const;
    static Rect clampToPixels(const Rect& r, int w, int h);

    ImageEditAction action_;
    ImageEditorConfig config_;
    DecodedImagePtr image_;
    std::shared_ptr<ImageRequest> request_;
    std::string signature_;
    std::string error_;
    bool loading_ = false;
    bool failed_ = false;
    bool synchronousDelivery_ = false;
    double progress_ = -1.0;

    Rect layoutRect_{};
    Size outerSize_{};

    bool pointerDown_ = false;
    ImageCropHandle activeHandle_ = ImageCropHandle::None;
    double startingScale_ = 1.0;
    Offset startingFocal_{};
    double detailsScale_ = 1.0;

    bool centerPending_ = false;
    bool centerStarted_ = false;
    bool centerAnimating_ = false;
    Rect centerFrom_{};
    Rect centerTo_{};
    std::unique_ptr<Ticker> ticker_;
};

inline std::unique_ptr<State> ImageEditor::createState() {
    return std::make_unique<ImageEditorState>();
}

// ── 绘制 ────────────────────────────────────────────────────────────────────

inline Color imageEditorMaskFallback(const Color& background, bool pointerDown) {
    return Color{background.r, background.g, background.b, pointerDown ? 0.4 : 0.8};
}

inline void ImageEditorState::converge() {
    const sk_sp<SkImage> source = transformedSource();
    if (!source || layoutRect_.width <= 0.0 || layoutRect_.height <= 0.0)
        return;
    const Size inputSize{double(source->width()), double(source->height())};
    action_.initRect(layoutRect_, imageEditorDestinationRect(layoutRect_, inputSize));
    action_.getFinalDestinationRect();
}

inline void ImageEditorState::paintLayer(Painter& painter, const Size& size) {
    if (!image_ || !action_.cropRect)
        return;
    converge();
    const sk_sp<SkImage> source = transformedSource();
    const std::optional<Rect>& destOpt = action_.screenDestinationRect();
    if (!source || !destOpt)
        return;
    const Rect dest = *destOpt;
    const Rect crop = *action_.cropRect;
    const Theme& theme = cui::theme();

    // ── 图：直接画到 dest（旋转/翻转已烘进位图，画布不再做变换）─────────────
    painter.save();
    if (rectBeyond(dest, layoutRect_))
        painter.clipRect(layoutRect_);
    painter.drawImageSkia(source, dest.left(), dest.top(), dest.width, dest.height);
    painter.restore();

    // ── 蒙层（四块，不用 BlendMode.clear，与上游 web 兼容那条一致）───────────
    const Color mask = config_.maskColor
                           ? *config_.maskColor
                           : imageEditorMaskFallback(theme.background, pointerDown_);
    const Rect box(0.0, 0.0, size.width, size.height);
    painter.setSourceColor(mask);
    painter.setFillStyle();
    auto fillRect = [&painter](const Rect& r) {
        if (r.width <= 0.0 || r.height <= 0.0)
            return;
        painter.rectangle(r);
        painter.fill();
    };
    fillRect(Rect(box.left(), box.top(), crop.left() - box.left(), box.height));
    fillRect(Rect(crop.left(), box.top(), crop.width, crop.top() - box.top()));
    fillRect(Rect(crop.right(), box.top(), box.right() - crop.right(), box.height));
    fillRect(Rect(crop.left(), crop.bottom(), crop.width, box.bottom() - crop.bottom()));

    // ── 边框 + 按下时的三分线 ───────────────────────────────────────────────
    const Color line = config_.lineColor
                           ? *config_.lineColor
                           : Color{theme.background.r, theme.background.g, theme.background.b,
                                   0.7};
    painter.setSourceColor(line);
    painter.setLineWidth(config_.lineHeight);
    const Rect frame(crop.left(), crop.top(), crop.width, crop.height);
    painter.rectangle(frame);
    painter.stroke();
    if (pointerDown_) {
        const double thirdW = crop.width / 3.0;
        const double thirdH = crop.height / 3.0;
        for (int i = 1; i <= 2; ++i) {
            const double x = crop.left() + thirdW * i;
            painter.moveTo(x, crop.top());
            painter.lineTo(x, crop.bottom());
            painter.stroke();
            const double y = crop.top() + thirdH * i;
            painter.moveTo(crop.left(), y);
            painter.lineTo(crop.right(), y);
            painter.stroke();
        }
    }

    // ── 四角：宽 cornerSize.width、厚 cornerSize.height 的 L 形 ─────────────
    const Color corner = config_.cornerColor ? *config_.cornerColor : theme.primary;
    painter.setSourceColor(corner);
    painter.setLineWidth(1.0);
    painter.setFillStyle();
    const double cw = config_.cornerSize.width;
    const double ch = config_.cornerSize.height;
    auto cornerPath = [&](double px, double py, double sx, double sy) {
        painter.moveTo(px, py);
        painter.lineTo(px + cw * sx, py);
        painter.lineTo(px + cw * sx, py + ch * sy);
        painter.lineTo(px + ch * sx, py + ch * sy);
        painter.lineTo(px + ch * sx, py + cw * sy);
        painter.lineTo(px, py + cw * sy);
        painter.fill();
    };
    cornerPath(crop.left(), crop.top(), 1.0, 1.0);
    cornerPath(crop.left(), crop.bottom(), 1.0, -1.0);
    cornerPath(crop.right(), crop.top(), -1.0, 1.0);
    cornerPath(crop.right(), crop.bottom(), -1.0, -1.0);
}

// ── 变换后的源图 ────────────────────────────────────────────────────────────

inline sk_sp<SkImage> ImageEditorState::transformedSource() const {
    sk_sp<SkImage> out = image_ ? image_->image() : nullptr;
    if (!out)
        return nullptr;
    if (action_.needFlip())
        out = flippedImage(out, action_.flipHorizontal(), action_.flipVertical());
    if (action_.hasRotateAngle()) {
        int deg = int(std::lround(action_.rotateAngle())) % 360;
        if (deg < 0)
            deg += 360;
        out = rotatedImage(out, deg);
    }
    return out;
}

inline Rect ImageEditorState::clampToPixels(const Rect& r, int w, int h) {
    const Rect bounds(0.0, 0.0, double(w), double(h));
    const double left = clampImageEditorValue(r.left(), 0.0, bounds.right());
    const double top = clampImageEditorValue(r.top(), 0.0, bounds.bottom());
    const double right = clampImageEditorValue(r.right(), 0.0, bounds.right());
    const double bottom = clampImageEditorValue(r.bottom(), 0.0, bounds.bottom());
    return Rect(left, top, std::max(0.0, right - left), std::max(0.0, bottom - top));
}

// ── 把手命中 ────────────────────────────────────────────────────────────────

/// 上游 crop_layer.dart 里 8 个 `Positioned + Container` 的矩形，逐个照抄。
/// 命中顺序 = Stack 的**逆绘制序**（后画者优先）：上游的 children 依次是
/// 四个角 → top → left → bottom → right，所以边条压过角的热区重叠带。
inline ImageCropHandle ImageEditorState::hitHandle(const Offset& p) const {
    if (!action_.cropRect)
        return ImageCropHandle::None;
    const Rect crop = *action_.cropRect;
    const double g = config_.hitTestSize;
    struct Region {
        ImageCropHandle handle;
        Rect rect;
    };
    const double edgeW = std::max(crop.width - g * 2.0, g * 2.0);
    const double edgeH = std::max(crop.height - g * 2.0, g * 2.0);
    const Region regions[] = {
        {ImageCropHandle::Right, Rect(crop.right() - g, crop.top() + g, g * 2, edgeH)},
        {ImageCropHandle::Bottom, Rect(crop.left() + g, crop.bottom() - g, edgeW, g * 2)},
        {ImageCropHandle::Left, Rect(crop.left() - g, crop.top() + g, g * 2, edgeH)},
        {ImageCropHandle::Top, Rect(crop.left() + g, crop.top() - g, edgeW, g * 2)},
        {ImageCropHandle::BottomRight, Rect(crop.right() - g, crop.bottom() - g, g * 2, g * 2)},
        {ImageCropHandle::BottomLeft, Rect(crop.left() - g, crop.bottom() - g, g * 2, g * 2)},
        {ImageCropHandle::TopRight, Rect(crop.right() - g, crop.top() - g, g * 2, g * 2)},
        {ImageCropHandle::TopLeft, Rect(crop.left() - g, crop.top() - g, g * 2, g * 2)},
    };
    for (const Region& r : regions) {
        if (r.rect.contains(Point(p.dx, p.dy)))
            return r.handle;
    }
    return ImageCropHandle::None;
}

// ── 把手拖动 ────────────────────────────────────────────────────────────────

inline void ImageEditorState::moveUpdate(ImageCropHandle handle, const Offset& delta) {
    auto* w = widget_();
    if (!w || !action_.cropRect || centerAnimating_)
        return;
    const std::optional<Rect> bounds = action_.layerDestinationRect();
    if (!bounds)
        return;
    // 上游这里用 cornerSize.width 当最小半宽（见文件头第 5 条）。
    const double g = config_.cornerSize.width;
    Rect crop = *action_.cropRect;
    Rect result = crop;

    switch (handle) {
        case ImageCropHandle::TopLeft:
        case ImageCropHandle::Top:
        case ImageCropHandle::Left: {
            Offset topLeft(crop.left() + delta.dx, crop.top() + delta.dy);
            topLeft.dx = std::min(topLeft.dx, crop.right() - g * 2.0);
            topLeft.dy = std::min(topLeft.dy, crop.bottom() - g * 2.0);
            result = rectFromPoints(topLeft, Offset(crop.right(), crop.bottom()));
            break;
        }
        case ImageCropHandle::TopRight: {
            Offset topRight(crop.right() + delta.dx, crop.top() + delta.dy);
            topRight.dx = std::max(topRight.dx, crop.left() + g * 2.0);
            topRight.dy = std::min(topRight.dy, crop.bottom() - g * 2.0);
            result = rectFromPoints(topRight, Offset(crop.left(), crop.bottom()));
            break;
        }
        case ImageCropHandle::BottomRight:
        case ImageCropHandle::Right:
        case ImageCropHandle::Bottom: {
            Offset bottomRight(crop.right() + delta.dx, crop.bottom() + delta.dy);
            bottomRight.dx = std::max(bottomRight.dx, crop.left() + g * 2.0);
            bottomRight.dy = std::max(bottomRight.dy, crop.top() + g * 2.0);
            result = rectFromPoints(Offset(crop.left(), crop.top()), bottomRight);
            break;
        }
        case ImageCropHandle::BottomLeft: {
            Offset bottomLeft(crop.left() + delta.dx, crop.bottom() + delta.dy);
            bottomLeft.dx = std::min(bottomLeft.dx, crop.right() - g * 2.0);
            bottomLeft.dy = std::max(bottomLeft.dy, crop.top() + g * 2.0);
            result = rectFromPoints(bottomLeft, Offset(crop.right(), crop.top()));
            break;
        }
        default:
            return;
    }

    // 裁剪框不许跑出图（上游同一条 min/max）。
    result = rectFromPoints(Offset(std::max(result.left(), bounds->left()),
                                   std::max(result.top(), bounds->top())),
                            Offset(std::min(result.right(), bounds->right()),
                                   std::min(result.bottom(), bounds->bottom())));

    result = handleAspectRatio(handle, result, *bounds, delta);

    if (rectBeyond(result, layoutRect_)) {
        // 框大到超出布局 ⇒ 先把框跳到这个「大框」（上游 setState(cropRect = result)），
        // 再用同一条归中公式把图像和框一起收回布局内。
        action_.cropRect = result;
        const Rect centered =
            imageEditorDestinationRect(layoutRect_, Size{result.width, result.height});
        centerOversizedRect(centered);
        return;
    }

    const std::optional<Rect> allowed = doWithMaxScale(result);
    if (allowed && *allowed != *action_.cropRect) {
        action_.cropRect = *allowed;
        notifyChanged();
        if (mounted())
            setState();
    }
}

/// 上游 `_handleAspectRatio` + `_doAspectRatioH/V`。
inline Rect ImageEditorState::handleAspectRatio(ImageCropHandle handle, const Rect& in,
                                                const Rect& bounds, const Offset& delta) const {
    const auto ratio = action_.cropAspectRatio();
    if (!ratio)
        return in;
    const double minD = config_.cornerSize.width * 2.0;
    const double aspect = *ratio;
    Rect result = in;
    switch (handle) {
        case ImageCropHandle::Top:
        case ImageCropHandle::Bottom: {
            const bool isTop = handle == ImageCropHandle::Top;
            const double width = std::max(minD,
                                          std::min(result.height * aspect, bounds.width));
            const double height = width / aspect;
            const double top = isTop ? result.bottom() - height : result.top();
            const double left = result.left() + result.width / 2.0 - width / 2.0;
            result = Rect(left, top, width, height);
            break;
        }
        case ImageCropHandle::Left:
        case ImageCropHandle::Right: {
            const bool isLeft = handle == ImageCropHandle::Left;
            const double height = std::max(minD,
                                           std::min(result.width / aspect, bounds.height));
            const double width = height * aspect;
            const double left = isLeft ? result.right() - width : result.left();
            const double top = result.top() + result.height / 2.0 - height / 2.0;
            result = Rect(left, top, width, height);
            break;
        }
        case ImageCropHandle::TopLeft:
        case ImageCropHandle::TopRight:
        case ImageCropHandle::BottomRight:
        case ImageCropHandle::BottomLeft: {
            double width = result.width;
            double height = result.height;
            if (std::fabs(delta.dx) >= std::fabs(delta.dy)) {
                height = std::max(minD, std::min(result.width / aspect, bounds.height));
                width = height * aspect;
            } else {
                width = std::max(minD, std::min(result.height * aspect, bounds.width));
                height = width / aspect;
            }
            double top = result.top();
            double left = result.left();
            switch (handle) {
                case ImageCropHandle::TopLeft:
                    top = result.bottom() - height;
                    left = result.right() - width;
                    break;
                case ImageCropHandle::TopRight:
                    top = result.bottom() - height;
                    left = result.left();
                    break;
                case ImageCropHandle::BottomLeft:
                    top = result.top();
                    left = result.right() - width;
                    break;
                default:
                    break;
            }
            result = Rect(left, top, width, height);
            break;
        }
        default:
            break;
    }
    return result;
}

/// 上游 `_doWithMaxScale`：放大到超过 maxScale 时，只在「框还在变大」的那一步放行。
inline std::optional<Rect> ImageEditorState::doWithMaxScale(const Rect& rect) const {
    if (!action_.cropRect)
        return rect;
    const Rect centered = imageEditorDestinationRect(layoutRect_, Size{rect.width, rect.height});
    const Rect& oldRect = *action_.cropRect;
    if (oldRect.width <= 0.0)
        return rect;
    const double scale = centered.width / oldRect.width;
    const double totalScale = action_.totalScale * scale;
    if (totalScale > config_.maxScale + kImageEditorEps) {
        if (rect.width > oldRect.width || rect.height > oldRect.height)
            return rect;
        return std::nullopt;
    }
    return rect;
}

// ── 归中补间 ────────────────────────────────────────────────────────────────

inline bool ImageEditorState::beginCenterTween() {
    if (!action_.cropRect)
        return false;
    const Rect to = imageEditorDestinationRect(
        layoutRect_, Size{action_.cropRect->width, action_.cropRect->height});
    if (rectIsSame(to, *action_.cropRect)) {
        stopAutoCenter();
        return false;
    }
    centerFrom_ = *action_.cropRect;
    centerTo_ = to;
    centerStarted_ = true;
    centerAnimating_ = true;
    return true;
}

/// 上游 `_doCropAutoCenterAnimation`：先按新框/旧框的比例缩放图像矩形，再平移到新中心。
/// `old` 恒取「当前 cropRect」，所以补间的每一帧只做增量的缩放 + 平移。
inline void ImageEditorState::applyCropRectTo(const Rect& newCropRect) {
    if (!action_.cropRect || !action_.screenDestinationRect())
        return;
    const Rect& oldCrop = *action_.cropRect;
    if (oldCrop.width <= 0.0)
        return;
    const Rect& oldDest = *action_.screenDestinationRect();
    const double scale = newCropRect.width / oldCrop.width;
    const Offset offset = rectCenter(newCropRect) - rectCenter(oldCrop);
    const Offset cropCenter = rectCenter(oldCrop);
    /// 先缩放后平移：所以先算「缩放后的图像中心」，再加上框自身的位移。
    const Offset newImageCenter = cropCenter + (rectCenter(oldDest) - cropCenter) * scale;
    const Rect newDest = rectFromCenter(newImageCenter + offset, oldDest.width * scale,
                                        oldDest.height * scale);
    action_.cropRect = newCropRect;
    action_.setScreenDestinationRect(newDest);
    action_.totalScale *= scale;
    action_.preTotalScale = action_.totalScale;
    notifyChanged();
    if (mounted())
        setState();
}

inline void ImageEditorState::applyCenterTween(double t) {
    if (centerFrom_.width <= 0.0)
        return;
    const Rect next(centerFrom_.left() + (centerTo_.left() - centerFrom_.left()) * t,
                    centerFrom_.top() + (centerTo_.top() - centerFrom_.top()) * t,
                    centerFrom_.width + (centerTo_.width - centerFrom_.width) * t,
                    centerFrom_.height + (centerTo_.height - centerFrom_.height) * t);
    applyCropRectTo(next);
}

inline void ImageEditorState::centerOversizedRect(const Rect& targetCropRect) {
    applyCropRectTo(targetCropRect);
}

// ── 滚轮缩放 ────────────────────────────────────────────────────────────────

inline void ImageEditorState::applyScale(double scale, const Offset& focal) {
    auto* w = widget_();
    if (!w)
        return;
    const double maxScale = w->config().maxScale;
    const double scaleDelta = detailsScale_ > 0.0 ? scale / detailsScale_ : scale;
    const bool zoomIn = scaleDelta > 1.0;
    const bool zoomOut = scaleDelta < 1.0;
    detailsScale_ = scale;
    if ((action_.reachCropRectEdge() && zoomOut) ||
        (imageEditorNear(action_.totalScale, maxScale) && zoomIn)) {
        // 上游那句「correct _startingScale」：这一步没生效，所以下一步的基准
        // 要改成「当前值 / 本次请求的倍数」，否则连续外滚会一直顶在同一个值上。
        startingScale_ = scale != 0.0 ? action_.totalScale / scale : action_.totalScale;
        return;
    }
    const double totalScale = std::min(startingScale_ * scale * w->config().speed, maxScale);
    if (imageEditorNear(totalScale, action_.totalScale))
        return;
    action_.totalScale = totalScale;
    action_.screenFocalPoint = focal;
    notifyChanged();
    if (mounted())
        setState();
}

}  // namespace cui_core
