#pragma once
/// @file widgets/photo_view.h
/// @brief PhotoView / PhotoViewGallery / 滑出关闭页（header-only）。对齐
///        pub.dev `photo_view`（bluefireteam/photo_view master）的
///        `photo_view.dart + core/photo_view_core.dart +
///        core/photo_view_controller_delegate.dart +
///        controller/photo_view_controller.dart +
///        controller/photo_view_scalestate_controller.dart + utils/*.dart`，
///        以及 gallery 的 `photo_view_gallery.dart`。
///
/// 结构（与上游同一棵树）：
///   LayoutBuilder → [Listener] → DecoratedBox(background)
///     → [tightMode: SizedBox(childSize·scale)] → Center
///       → Transform(matrix, alignment: basePosition)
///         → OverflowBox(tight childSize, alignment: basePosition, Hero? → content)
///
///   `matrix = T(position) · S(scale) · Rz(rotation)`，枢轴由 `Transform` 的
///   alignment 折成 `T(P)·matrix·T(-P)`，`P = ((basePosition.x+1)/2·W,
///   (basePosition.y+1)/2·H)`。上游用 `_CenterWithOriginalSizeDelegate` 把子节点
///   紧约束到「图片自然尺寸」并按 `halfDiff·(basePosition+1)` 摆放 —— 本仓没有
///   CustomSingleChildLayout，等价物是 `OverflowBox(min=max=childSize,
///   alignment: basePosition)`：自身撑满外框，子节点拿到**外框之外的紧尺寸**
///   （Align+SizedBox 不行 —— Align 只 loosen()，max 还在，「比外框大的图」会被夹
///   成外框尺寸，缩放后小一倍）。偏移公式与上游逐字一致。
///
/// 有意偏离 / 未移植（勿当 bug 排查）：
///   1. **输入走 Listener + 自带多指针表**，上游用 `ScaleGestureRecognizer`
///      （`photo_view_gesture_detector.dart`）；本仓 GestureDetector 没有 scale。
///      单指 = 平移、双指 = 捏合（+ 旋转），逐条复刻 `_onScaleStart/_onScaleUpdate/
///      _onScaleEnd` 的判定顺序。
///   2. **双击是时序判定**（`kDoubleTapWindowMs` 内、`kDoubleTapSlopPx` 半径内的
///      第二次抬手）。上游靠 TapGestureRecognizer 与 DoubleTapGestureRecognizer
///      的竞技场互斥；本仓没有双击识别器，所以第一次抬手立刻发 `onTapUp`，
///      第二次抬手发 `nextScaleState()` 并**吞掉** `onTapUp`；成双后这一对整体
///      作废，第三记重新按单点算（否则连点会变成「每记都换档」）。
///   3. **无甩动惯性**：本仓指针事件不带速度 ⇒ 上游 `onScaleEnd` 里
///      `magnitude >= 400` 那段直接不写；超出 min/max 的回弹动画保留。
///   4. **动画是定长补间**（`animationMs` + `applyEasing(curve)` + 一个 Ticker），
///      上游是三个 `AnimationController..fling(velocity: 0.4)`（时长=0.25s，
///      曲线=Curves.fling 的等价物）。本仓每帧都重建（RenderObject 不订阅响应流，
///      与 widgets/transition.h、AnimatedSwitcher 同一口径），上游在 fling 期间
///      只写 `setScaleInvisibly`、不重建子树。
///   5. **捏合的 position 用上游的近似式** `clampPosition(delta · ratio)`
///      （把「拖了多少」当「在哪」，越界就夹），但夹的时候用**本帧的目标 scale**
///      （上游 `_blindScaleListener` 读的是上一帧的）。滚轮缩放本仓另做精确锚定：
///      `pos' = k·pos + (1-k)·(f - P)`，`k = s'/s` —— 上游没有滚轮。
///   6. **焦点用局部坐标**（上游 `ScaleStartDetails.focalPoint` 是全局点）。
///      全屏时两者等价，嵌在滚动视图里时局部坐标才是对的。
///   7. `rotationFocusPoint` 未移植（上游存进 controller 但矩阵里从未使用）；
///      `filterQuality != none` 那条「让 Image 自己缩放」的路径未移植（恒走
///      Transform）；`wantKeepAlive` / `semanticLabel` / `gestureDetectorBehavior`
///      / `HeroAttributes.createRectTween` 等未接（hero 只认 tag）。
///   8. **loading 期间仍挂完整 core 树**（`childSize` 暂取 outerSize，占位由
///      ExtendedImage 自己画）；上游是用占位控件整体替换 `PhotoViewCore`。
///      这样尺寸到达时树形不变、ExtendedImage 不会被拆装重解一遍。失败态仍照上游
///      整体替换（没有手势可言）。
///   9. **PhotoViewGallery 自带横向翻页**（本仓 `PageView` 是装饰性卡片演示，
///      没有分页语义）：只构建 `[page-1, page+1]` 窗口、每页 `ClipRect` +
///      `ValueKey<int>`、每页的 controller 由 gallery 持有（所以翻回来缩放态还在，
///      等价于上游的 wantKeepAlive）。上游的 `PhotoViewGestureDetectorScope` +
///      `HitCornersDetector`「撞到角就把拖拽让给父级」在这里换成更直接的口径：
///      PhotoView 只在**本帧真的吃掉了位移**时才 `stopPropagation()`，于是夹到
///      边界之后剩余拖拽自动归 gallery。`scrollPhysics`/`allowImplicitScrolling`
///      /`PageController` 未接。
///  10. **翻页是严格夹取**（上游 `PageView` 默认 `ClampingScrollPhysics` ⇒ 没有
///      橡皮筋）；`pageSnapping` 抬手后吸附到 `lround(offset)`，补间时长
///      `pageSnapMs`。滚轮按页累积（`wheelAccum_`），攒满一整页才翻 ——
///      上游是 Scrollable 自己处理，本仓没有 ScrollPosition。
///      `onPageChanged` 按 **round** 报（含拖拽途中越界那一刻，与上游
///      `PageView.onPageChanged` 同口径），不是等吸附结束才报。
///      页面的 `minScale` 缺省给 `contained`（上游是 0）：否则「滚轮缩小」永远
///      算有效果、滚轮永远被页面吃掉，画廊翻不了页 —— 与拖拽那条「撞到边界才让
///      给父级」的口径对齐：**缩小到铺满为止，之后的滚轮归画廊**。
///  11. **滑出关闭页**（`PhotoViewSlidePage`）对应 `extended_image` 的
///      `ExtendedImageSlidePage`：源码在本机取不到（jsdelivr 的 tag 只到
///      3.10.0，pub storage 被 TLS 吊销检查挡住），所以参数名/背景透明度公式
///      按 README 实现，**三条缺省公式**（背景淡出、缩放 85%、
///      `ratio >= slideEndRatio` 才关）是有意的自选实现，见
///      `photoViewSlideRatio` 与类注释。`slideAxis` 上游是 `Axis?`（null = 双向），
///      这里换成显式三值枚举。
///  12. **`animate=false` 时换档立刻落位**（写 `nextScale` + 平移归零 + 旋转归零）。
///      上游 `_blindScaleStateListener` 在这种情况下走 `setScaleInvisibly(scale)`
///      —— 那里的 `scale` getter 因为 `markNeedsScaleRecalc` 已复位，读回来还是
///      **换档前**的旧值，等于「什么都不写」。本仓按字面意图补齐，否则非补间换档
///      永远看不到缩放变化。
///      （`PhotoView::animate` 在 picture 侧还兼作 ExtendedImage 的 GIF 播放开关，
///      与上游「animate 只管缩放补间」不同 —— 本仓的动图播放需要它。）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_provider.h"
#include "cui/foundation/key.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/extended_image.h"
#include "cui/widgets/hero.h"
#include "cui/widgets/layout.h"
#include "cui/framework/listener.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/pages.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/transition.h"

namespace cui {

/// 本文件自带的夹取（`clampDoubleValue` 住在 widgets/interactive_viewer.h，
/// 拉进来只为一个夹取函数会让两个手势控件头文件互相牵连）。
inline double clampPhotoViewValue(double value, double lower, double upper) {
    return value < lower ? lower : (value > upper ? upper : value);
}

// ── ScaleState 枚举与 cycle ─────────────────────────────────────────────────

/// 上游的 `PhotoViewScaleState`：双击循环走到哪一步。
enum class PhotoViewScaleState {
    Initial,      ///< 初始（= initialScale）
    Covering,     ///< 铺满外框（可能裁掉一边）
    OriginalSize, ///< 图片像素尺寸（scale = 1）
    ZoomedIn,     ///< 用户捏大（不属于循环档位）
    ZoomedOut,    ///< 用户捏小（不属于循环档位）
};

/// 上游的 `PhotoViewScaleStateIZoomingExtension.isScaleStateZooming`。
inline bool isPhotoViewScaleStateZooming(PhotoViewScaleState s) {
    return s == PhotoViewScaleState::ZoomedIn || s == PhotoViewScaleState::ZoomedOut;
}

/// 上游的 `defaultScaleStateCycle`。
inline PhotoViewScaleState defaultPhotoViewScaleStateCycle(PhotoViewScaleState actual) {
    switch (actual) {
        case PhotoViewScaleState::Initial:
            return PhotoViewScaleState::Covering;
        case PhotoViewScaleState::Covering:
            return PhotoViewScaleState::OriginalSize;
        case PhotoViewScaleState::OriginalSize:
            return PhotoViewScaleState::Initial;
        case PhotoViewScaleState::ZoomedIn:
        case PhotoViewScaleState::ZoomedOut:
            return PhotoViewScaleState::Initial;
    }
    return PhotoViewScaleState::Initial;
}

/// 上游的 `ScaleStateCycle`。
using PhotoViewScaleStateCycle =
    std::function<PhotoViewScaleState(PhotoViewScaleState actual)>;

// ── PhotoViewScale（上游的 PhotoViewComputedScale + 裸 double）──────────────

/// min/max/initial 三档的「值类型枚举」：一个固定倍数，或者「按外框/图片算出来」
/// 的 contained / covered。上游 `dynamic` 只能是 double 或
/// PhotoViewComputedScale，这里合成一个类型。
class PhotoViewScale {
public:
    enum class Basis { Fixed, Contained, Covered };

    static PhotoViewScale fixed(double value) {
        PhotoViewScale s;
        s.basis_ = Basis::Fixed;
        s.value_ = value;
        return s;
    }
    static PhotoViewScale contained(double multiplier = 1.0) {
        PhotoViewScale s;
        s.basis_ = Basis::Contained;
        s.multiplier_ = multiplier;
        return s;
    }
    static PhotoViewScale covered(double multiplier = 1.0) {
        PhotoViewScale s;
        s.basis_ = Basis::Covered;
        s.multiplier_ = multiplier;
        return s;
    }

    /// 上游的 `PhotoViewComputedScale * multiplier` / `/ divider`。
    PhotoViewScale operator*(double multiplier) const {
        PhotoViewScale s = *this;
        s.multiplier_ *= multiplier;
        return s;
    }
    PhotoViewScale operator/(double divider) const {
        PhotoViewScale s = *this;
        s.multiplier_ = divider != 0.0 ? s.multiplier_ / divider : s.multiplier_;
        return s;
    }

    /// 上游的 `==` 只比 `_value`（倍数被忽略），那会让 `contained*1 → contained*2`
    /// 这种改动漏掉 markNeedsScaleRecalc；这里连倍数一起比。
    bool operator==(const PhotoViewScale& o) const {
        return basis_ == o.basis_ && value_ == o.value_ && multiplier_ == o.multiplier_;
    }
    bool operator!=(const PhotoViewScale& o) const { return !(*this == o); }

    Basis basis() const { return basis_; }
    double value() const { return value_; }
    double multiplier() const { return multiplier_; }

    /// 把这一档换算成具体倍数。`contained` / `covered` 由调用方按当前两个尺寸算好
    /// 再递进来（上游是在 ScaleBoundaries 的 getter 里现算的）。
    double resolve(double containedScale, double coveringScale) const {
        switch (basis_) {
            case Basis::Contained:
                return containedScale * multiplier_;
            case Basis::Covered:
                return coveringScale * multiplier_;
            case Basis::Fixed:
                return value_;
        }
        return value_;
    }

private:
    Basis basis_ = Basis::Fixed;
    double value_ = 0.0;
    double multiplier_ = 1.0;
};

// ── ScaleBoundaries ─────────────────────────────────────────────────────────

/// 上游的 `ScaleBoundaries`：两个尺寸 + 三条缩放边界。
/// **`childSize` 是图片的自然像素尺寸**（不是画出来的尺寸）。
struct PhotoViewScaleBoundaries {
    Size outerSize{};
    Size childSize{};
    double minScale = 0.0;
    double maxScale = BoxConstraints::kInfinity;
    double initialScale = 1.0;

    bool operator==(const PhotoViewScaleBoundaries& o) const {
        return outerSize == o.outerSize && childSize == o.childSize &&
               minScale == o.minScale && maxScale == o.maxScale &&
               initialScale == o.initialScale;
    }
    bool operator!=(const PhotoViewScaleBoundaries& o) const { return !(*this == o); }
};

/// 上游的 `_scaleForContained`。
inline double containedScaleFor(const Size& outer, const Size& child) {
    if (child.width <= 0.0 || child.height <= 0.0)
        return 1.0;
    return std::min(outer.width / child.width, outer.height / child.height);
}

/// 上游的 `_scaleForCovering`。
inline double coveringScaleFor(const Size& outer, const Size& child) {
    if (child.width <= 0.0 || child.height <= 0.0)
        return 1.0;
    return std::max(outer.width / child.width, outer.height / child.height);
}

/// 上游 `ScaleBoundaries` 三个 getter 的合并版。
/// 口径逐条照抄：min 不夹（负值夹成 0，上游是 assert）；max 夹进 [min, ∞)；
/// initial 为计算档时**不夹**，为固定值时夹进 [min, max]；缺省 initial = contained。
inline PhotoViewScaleBoundaries computePhotoViewScaleBoundaries(
    const Size& outerSize, const Size& childSize,
    const std::optional<PhotoViewScale>& minScale = std::nullopt,
    const std::optional<PhotoViewScale>& maxScale = std::nullopt,
    const std::optional<PhotoViewScale>& initialScale = std::nullopt) {
    const double contained = containedScaleFor(outerSize, childSize);
    const double covering = coveringScaleFor(outerSize, childSize);
    const double infinity = BoxConstraints::kInfinity;

    PhotoViewScaleBoundaries b;
    b.outerSize = outerSize;
    b.childSize = childSize;

    b.minScale = minScale ? std::max(0.0, minScale->resolve(contained, covering)) : 0.0;
    if (maxScale) {
        b.maxScale = std::max(b.minScale, maxScale->resolve(contained, covering));
    } else {
        b.maxScale = infinity;
    }
    if (initialScale) {
        const double resolved = initialScale->resolve(contained, covering);
        b.initialScale = initialScale->basis() == PhotoViewScale::Basis::Fixed
                             ? clampPhotoViewValue(resolved, b.minScale, b.maxScale)
                             : resolved;
    } else {
        b.initialScale = contained;
    }
    return b;
}

/// 上游的 `getScaleForScaleState`。
inline double getScaleForPhotoViewScaleState(PhotoViewScaleState scaleState,
                                             const PhotoViewScaleBoundaries& b) {
    switch (scaleState) {
        case PhotoViewScaleState::Initial:
        case PhotoViewScaleState::ZoomedIn:
        case PhotoViewScaleState::ZoomedOut:
            return clampPhotoViewValue(b.initialScale, b.minScale, b.maxScale);
        case PhotoViewScaleState::Covering:
            return clampPhotoViewValue(coveringScaleFor(b.outerSize, b.childSize), b.minScale,
                                    b.maxScale);
        case PhotoViewScaleState::OriginalSize:
            return clampPhotoViewValue(1.0, b.minScale, b.maxScale);
    }
    return b.initialScale;
}

/// 上游的 `CornersRange`。
struct PhotoViewCornersRange {
    double min = 0.0;
    double max = 0.0;
};

// ── PhotoViewControllerValue / PhotoViewController ──────────────────────────

/// 上游的 `PhotoViewControllerValue`（`rotationFocusPoint` 未移植，见文件头第 7 条）。
struct PhotoViewControllerValue {
    Offset position{};
    std::optional<double> scale;  ///< 空 = 「还没被算过」，见 delegate 的 scale getter
    double rotation = 0.0;

    bool operator==(const PhotoViewControllerValue& o) const {
        return position == o.position && scale == o.scale && rotation == o.rotation;
    }
    bool operator!=(const PhotoViewControllerValue& o) const { return !(*this == o); }
};

/// 上游的 `PhotoViewController`。
///
/// 「ignorable」语义靠 `internalWrite_` 复刻：内部写入（`*Invisibly`）照样通知
/// 全部监听器，但监听器能读到 `internalWrite()` 从而跳过自己 —— 上游正是用
/// `IgnorableValueNotifier` 区分「外部改的」和「框架自己改的」。
class PhotoViewController : public ChangeNotifier {
public:
    PhotoViewController() = default;
    explicit PhotoViewController(PhotoViewControllerValue value)
        : value_(value), initial_(value), prevValue_(value) {}

    const PhotoViewControllerValue& value() const { return value_; }
    const PhotoViewControllerValue& prevValue() const { return prevValue_; }

    /// 上游的 `value=`（通知 ignorable 监听器）。
    void setValue(const PhotoViewControllerValue& v) {
        if (value_ == v)
            return;
        prevValue_ = value_;
        value_ = v;
        notifyListeners();
    }
    /// 上游的 `updateIgnoring`：通知，但 ignorable 监听器会跳过自己。
    void setValueInvisibly(const PhotoViewControllerValue& v) {
        if (value_ == v)
            return;
        prevValue_ = value_;
        value_ = v;
        internalWrite_ = true;
        notifyListeners();
        internalWrite_ = false;
    }

    Offset position() const { return value_.position; }
    void setPosition(const Offset& p) {
        if (value_.position == p)
            return;
        PhotoViewControllerValue v = value_;
        v.position = p;
        setValue(v);
    }
    void setPositionInvisibly(const Offset& p) {
        if (value_.position == p)
            return;
        PhotoViewControllerValue v = value_;
        v.position = p;
        setValueInvisibly(v);
    }

    const std::optional<double>& scale() const { return value_.scale; }
    void setScale(std::optional<double> s) {
        if (value_.scale == s)
            return;
        PhotoViewControllerValue v = value_;
        v.scale = s;
        setValue(v);
    }
    /// 上游的 `setScaleInvisibly`。
    void setScaleInvisibly(std::optional<double> s) {
        if (value_.scale == s)
            return;
        PhotoViewControllerValue v = value_;
        v.scale = s;
        setValueInvisibly(v);
    }

    double rotation() const { return value_.rotation; }
    void setRotation(double r) {
        if (value_.rotation == r)
            return;
        PhotoViewControllerValue v = value_;
        v.rotation = r;
        setValue(v);
    }
    void setRotationInvisibly(double r) {
        if (value_.rotation == r)
            return;
        PhotoViewControllerValue v = value_;
        v.rotation = r;
        setValueInvisibly(v);
    }

    /// 上游的 `updateMultiple`：一次写多个字段、只通知一次。
    void updateMultiple(std::optional<Offset> position = std::nullopt,
                        std::optional<double> scale = std::nullopt,
                        std::optional<double> rotation = std::nullopt) {
        PhotoViewControllerValue v = value_;
        if (position)
            v.position = *position;
        if (scale)
            v.scale = scale;
        if (rotation)
            v.rotation = *rotation;
        setValue(v);
    }

    /// 上游的 `reset()`：回到构造时的初值。
    void reset() { setValue(initial_); }

    bool internalWrite() const { return internalWrite_; }

    /// 本仓新增：把当前边界存进 controller，方便宿主（PhotoViewGallery）在不碰
    /// State 的情况下问「这一页放大过了吗」。写入是静默的（不参与通知与比较）。
    void setBoundariesInvisibly(const PhotoViewScaleBoundaries& b) { boundaries_ = b; }
    const PhotoViewScaleBoundaries& boundaries() const { return boundaries_; }
    /// 当前 scale 是否超过 initialScale（= 上游 gallery 允许翻页的反面条件）。
    bool zoomedBeyondInitial() const {
        return bool(value_.scale) && *value_.scale > boundaries_.initialScale + 1e-6;
    }

private:
    PhotoViewControllerValue value_{};
    PhotoViewControllerValue initial_{};
    PhotoViewControllerValue prevValue_{};
    PhotoViewScaleBoundaries boundaries_{};
    bool internalWrite_ = false;
};

using PhotoViewControllerPtr = std::shared_ptr<PhotoViewController>;

// ── PhotoViewScaleStateController ───────────────────────────────────────────

/// 上游的 `PhotoViewScaleStateController`（只管 scaleState 那一份状态）。
class PhotoViewScaleStateController : public ChangeNotifier {
public:
    PhotoViewScaleState scaleState() const { return value_; }
    PhotoViewScaleState prevScaleState() const { return prevValue_; }

    /// 上游的 `set scaleState`：换值前记下 prevScaleState，并通知。
    void setScaleState(PhotoViewScaleState v) {
        if (value_ == v)
            return;
        prevValue_ = value_;
        value_ = v;
        notifyListeners();
    }
    /// 上游的 `setInvisibly`：换值但 ignorable 监听器（delegate）收不到。
    void setInvisibly(PhotoViewScaleState v) {
        if (value_ == v)
            return;
        prevValue_ = value_;
        value_ = v;
        internalWrite_ = true;
        notifyListeners();
        internalWrite_ = false;
    }

    /// 上游的 `hasChanged`。
    bool hasChanged() const { return prevValue_ != value_; }
    bool isZooming() const { return isPhotoViewScaleStateZooming(value_); }

    /// 上游的 `reset()`：先把 prev 记成当前，再回 Initial。
    void reset() {
        prevValue_ = value_;
        setScaleState(PhotoViewScaleState::Initial);
    }

    bool internalWrite() const { return internalWrite_; }

private:
    PhotoViewScaleState value_ = PhotoViewScaleState::Initial;
    PhotoViewScaleState prevValue_ = PhotoViewScaleState::Initial;
    bool internalWrite_ = false;
};

using PhotoViewScaleStateControllerPtr = std::shared_ptr<PhotoViewScaleStateController>;

// ── 回调细节 ────────────────────────────────────────────────────────────────

/// 上游 `PhotoViewImageTapUpCallback / TapDown / ScaleEnd` 的合并细节：
/// 位置（局部 + 全局）与当时的 controller 值。
struct PhotoViewTapDetails {
    Offset position{};
    Offset globalPosition{};
    PhotoViewControllerValue value{};
};

using PhotoViewTapCallback = std::function<void(const PhotoViewTapDetails&)>;
using PhotoViewScaleEndCallback = std::function<void(const PhotoViewTapDetails&)>;
using PhotoViewScaleStateChangedCallback = std::function<void(PhotoViewScaleState)>;

inline BoxDecoration photoViewDefaultBackground() {
    BoxDecoration d;
    d.color = Color(0.0, 0.0, 0.0, 1.0);
    return d;
}

// ── PhotoView ───────────────────────────────────────────────────────────────

/// 可缩放 / 可平移 / 可双击换档的看图控件。上游的 `PhotoView`
/// （`PhotoView` + `PhotoView.customChild` 两个构造合成一个：`customChild`
/// 非空即 customChild 模式）。
class PhotoView : public StatefulWidget {
public:
    /// 参数按「内容 → 边界 → 控制器 → 回调 → 占位 → 开关 → 动画」分组，
    /// 新增参数一律追加在 `key` 之前。
    explicit PhotoView(SharedPtr<Widget> customChild = nullptr,
                       std::shared_ptr<ImageProvider> provider = nullptr,
                       std::optional<Size> childSize = std::nullopt,
                       std::optional<PhotoViewScale> minScale = std::nullopt,
                       std::optional<PhotoViewScale> maxScale = std::nullopt,
                       std::optional<PhotoViewScale> initialScale = std::nullopt,
                       std::optional<Size> customSize = std::nullopt,
                       std::optional<Alignment> basePosition = std::nullopt,
                       BoxDecoration background = photoViewDefaultBackground(),
                       PhotoViewControllerPtr controller = nullptr,
                       PhotoViewScaleStateControllerPtr scaleStateController = nullptr,
                       PhotoViewScaleStateCycle scaleStateCycle = nullptr,
                       PhotoViewTapCallback onTapUp = nullptr,
                       PhotoViewTapCallback onTapDown = nullptr,
                       PhotoViewScaleEndCallback onScaleEnd = nullptr,
                       PhotoViewScaleStateChangedCallback scaleStateChangedCallback = nullptr,
                       ExtendedImage::LoadingBuilder loadingBuilder = nullptr,
                       ExtendedImage::ErrorBuilder errorBuilder = nullptr,
                       bool enableRotation = false, bool tightMode = false,
                       bool disableGestures = false, bool enablePanAlways = false,
                       bool strictScale = false, bool gaplessPlayback = false,
                       bool animate = true, double scaleFactor = 200.0,
                       double animationMs = 250.0,
                       Easing animationCurve = Easing::EaseOutCubic,
                       std::string heroTag = std::string(), KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), customChild_(std::move(customChild)),
          provider_(std::move(provider)), childSize_(childSize), minScale_(minScale),
          maxScale_(maxScale), initialScale_(initialScale), customSize_(customSize),
          basePosition_(basePosition), background_(background),
          controller_(std::move(controller)),
          scaleStateController_(std::move(scaleStateController)),
          scaleStateCycle_(std::move(scaleStateCycle)), onTapUp_(std::move(onTapUp)),
          onTapDown_(std::move(onTapDown)), onScaleEnd_(std::move(onScaleEnd)),
          scaleStateChangedCallback_(std::move(scaleStateChangedCallback)),
          loadingBuilder_(std::move(loadingBuilder)), errorBuilder_(std::move(errorBuilder)),
          enableRotation_(enableRotation), tightMode_(tightMode),
          disableGestures_(disableGestures), enablePanAlways_(enablePanAlways),
          strictScale_(strictScale), gaplessPlayback_(gaplessPlayback), animate_(animate),
          scaleFactor_(scaleFactor), animationMs_(animationMs), animationCurve_(animationCurve),
          heroTag_(std::move(heroTag)) {}

    // ── 常用入口（对齐 ExtendedImage 那几个工厂）────────────────────────────

    static SharedPtr<PhotoView> make(std::shared_ptr<ImageProvider> provider,
                                     KeyPtr key = nullptr) {
        return cui::make_shared<PhotoView>(nullptr, std::move(provider), std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt,
                                           photoViewDefaultBackground(), nullptr, nullptr,
                                           nullptr, nullptr, nullptr, nullptr, nullptr,
                                           nullptr, nullptr, false, false, false, false,
                                           false, false, true, 200.0, 250.0,
                                           Easing::EaseOutCubic, std::string(),
                                           std::move(key));
    }
    static SharedPtr<PhotoView> file(const std::string& path, KeyPtr key = nullptr) {
        return make(std::make_shared<FileImageProvider>(path), std::move(key));
    }
    static SharedPtr<PhotoView> network(const std::string& url, HttpHeaders headers = {},
                                        KeyPtr key = nullptr) {
        return make(std::make_shared<NetworkImageProvider>(std::string(url),
                                                           std::move(headers)),
                    std::move(key));
    }
    static SharedPtr<PhotoView> memory(std::vector<uint8_t> bytes,
                                       std::string name = "memory", KeyPtr key = nullptr) {
        return make(std::make_shared<MemoryImageProvider>(std::move(bytes), std::move(name)),
                    std::move(key));
    }
    static SharedPtr<PhotoView> decoded(DecodedImagePtr image,
                                        std::string name = "decoded", KeyPtr key = nullptr) {
        return make(std::make_shared<DecodedImageProvider>(std::move(image), std::move(name)),
                    std::move(key));
    }
    /// 上游的 `PhotoView.customChild`：缩放一个非图片内容（`childSize` 缺省用外框）。
    static SharedPtr<PhotoView> custom(SharedPtr<Widget> child, Size childSize,
                                       KeyPtr key = nullptr) {
        return cui::make_shared<PhotoView>(std::move(child), nullptr, childSize,
                                           std::nullopt, std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt,
                                           photoViewDefaultBackground(), nullptr, nullptr, nullptr,
                                           nullptr, nullptr, nullptr, nullptr, nullptr,
                                           nullptr, false, false, false, false, false,
                                           false, true, 200.0, 250.0, Easing::EaseOutCubic,
                                           std::string(), std::move(key));
    }

    /// 上游的 `PhotoView.hasCustomChild`。
    bool isCustomChild() const { return bool(customChild_); }

    // ── 访问器 / 就地 setter ────────────────────────────────────────────────

    const SharedPtr<Widget>& customChild() const { return customChild_; }
    void setCustomChild(SharedPtr<Widget> c) { customChild_ = std::move(c); }

    const std::shared_ptr<ImageProvider>& provider() const { return provider_; }
    void setProvider(std::shared_ptr<ImageProvider> p) { provider_ = std::move(p); }

    const std::optional<Size>& childSize() const { return childSize_; }
    void setChildSize(std::optional<Size> s) { childSize_ = s; }

    const std::optional<PhotoViewScale>& minScale() const { return minScale_; }
    void setMinScale(std::optional<PhotoViewScale> v) { minScale_ = v; }
    const std::optional<PhotoViewScale>& maxScale() const { return maxScale_; }
    void setMaxScale(std::optional<PhotoViewScale> v) { maxScale_ = v; }
    const std::optional<PhotoViewScale>& initialScale() const { return initialScale_; }
    void setInitialScale(std::optional<PhotoViewScale> v) { initialScale_ = v; }

    const std::optional<Size>& customSize() const { return customSize_; }
    void setCustomSize(std::optional<Size> s) { customSize_ = s; }

    const std::optional<Alignment>& basePosition() const { return basePosition_; }
    void setBasePosition(std::optional<Alignment> a) { basePosition_ = a; }

    const BoxDecoration& background() const { return background_; }
    void setBackground(const BoxDecoration& b) { background_ = b; }

    const PhotoViewControllerPtr& controller() const { return controller_; }
    void setController(PhotoViewControllerPtr c) { controller_ = std::move(c); }
    const PhotoViewScaleStateControllerPtr& scaleStateController() const {
        return scaleStateController_;
    }
    void setScaleStateController(PhotoViewScaleStateControllerPtr c) {
        scaleStateController_ = std::move(c);
    }

    const PhotoViewScaleStateCycle& scaleStateCycle() const { return scaleStateCycle_; }
    void setScaleStateCycle(PhotoViewScaleStateCycle c) { scaleStateCycle_ = std::move(c); }

    const PhotoViewTapCallback& onTapUp() const { return onTapUp_; }
    void setOnTapUp(PhotoViewTapCallback cb) { onTapUp_ = std::move(cb); }
    const PhotoViewTapCallback& onTapDown() const { return onTapDown_; }
    void setOnTapDown(PhotoViewTapCallback cb) { onTapDown_ = std::move(cb); }
    const PhotoViewScaleEndCallback& onScaleEnd() const { return onScaleEnd_; }
    void setOnScaleEnd(PhotoViewScaleEndCallback cb) { onScaleEnd_ = std::move(cb); }
    const PhotoViewScaleStateChangedCallback& scaleStateChangedCallback() const {
        return scaleStateChangedCallback_;
    }
    void setScaleStateChangedCallback(PhotoViewScaleStateChangedCallback cb) {
        scaleStateChangedCallback_ = std::move(cb);
    }

    const ExtendedImage::LoadingBuilder& loadingBuilder() const { return loadingBuilder_; }
    void setLoadingBuilder(ExtendedImage::LoadingBuilder b) {
        loadingBuilder_ = std::move(b);
    }
    const ExtendedImage::ErrorBuilder& errorBuilder() const { return errorBuilder_; }
    void setErrorBuilder(ExtendedImage::ErrorBuilder b) { errorBuilder_ = std::move(b); }

    bool enableRotation() const { return enableRotation_; }
    void setEnableRotation(bool v) { enableRotation_ = v; }
    bool tightMode() const { return tightMode_; }
    void setTightMode(bool v) { tightMode_ = v; }
    bool disableGestures() const { return disableGestures_; }
    void setDisableGestures(bool v) { disableGestures_ = v; }
    bool enablePanAlways() const { return enablePanAlways_; }
    void setEnablePanAlways(bool v) { enablePanAlways_ = v; }
    bool strictScale() const { return strictScale_; }
    void setStrictScale(bool v) { strictScale_ = v; }
    bool gaplessPlayback() const { return gaplessPlayback_; }
    void setGaplessPlayback(bool v) { gaplessPlayback_ = v; }
    bool animate() const { return animate_; }
    void setAnimate(bool v) { animate_ = v; }

    double scaleFactor() const { return scaleFactor_; }
    void setScaleFactor(double v) { scaleFactor_ = v; }
    double animationMs() const { return animationMs_; }
    void setAnimationMs(double v) { animationMs_ = v; }
    Easing animationCurve() const { return animationCurve_; }
    void setAnimationCurve(Easing v) { animationCurve_ = v; }

    const std::string& heroTag() const { return heroTag_; }
    void setHeroTag(std::string t) { heroTag_ = std::move(t); }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "PhotoView"; }

private:
    SharedPtr<Widget> customChild_;
    std::shared_ptr<ImageProvider> provider_;
    std::optional<Size> childSize_;
    std::optional<PhotoViewScale> minScale_, maxScale_, initialScale_;
    std::optional<Size> customSize_;
    std::optional<Alignment> basePosition_;
    BoxDecoration background_;
    PhotoViewControllerPtr controller_;
    PhotoViewScaleStateControllerPtr scaleStateController_;
    PhotoViewScaleStateCycle scaleStateCycle_;
    PhotoViewTapCallback onTapUp_, onTapDown_;
    PhotoViewScaleEndCallback onScaleEnd_;
    PhotoViewScaleStateChangedCallback scaleStateChangedCallback_;
    ExtendedImage::LoadingBuilder loadingBuilder_;
    ExtendedImage::ErrorBuilder errorBuilder_;
    bool enableRotation_ = false;
    bool tightMode_ = false;
    bool disableGestures_ = false;
    bool enablePanAlways_ = false;
    bool strictScale_ = false;
    bool gaplessPlayback_ = false;
    bool animate_ = true;
    double scaleFactor_ = 200.0;
    double animationMs_ = 250.0;
    Easing animationCurve_ = Easing::EaseOutCubic;
    std::string heroTag_;
};

// ── PhotoViewState（上游 _PhotoViewState + ImageWrapper + PhotoViewCoreState
//    + PhotoViewControllerDelegate + HitCornersDetector 的合成）───────────────

class PhotoViewState : public State {
public:
    static constexpr double kDoubleTapWindowMs = 300.0;
    static constexpr double kDoubleTapSlopPx = 44.0;

    void initState() override {
        auto* w = widget_();
        controller_ = w && w->controller() ? w->controller()
                                           : std::make_shared<PhotoViewController>();
        controllerExternal_ = bool(w && w->controller());
        scaleStateController_ = w && w->scaleStateController()
                                    ? w->scaleStateController()
                                    : std::make_shared<PhotoViewScaleStateController>();
        scaleStateExternal_ = bool(w && w->scaleStateController());
        subscribe();
        ticker_ = std::make_unique<Ticker>([this](Duration elapsed) {
            advanceAnimation(elapsed.count());
        });
    }

    void dispose() override {
        // 铁律：捕获 this 的 lambda 只能按句柄摘。
        unsubscribe();
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    const PhotoViewControllerPtr& controller() const { return controller_; }
    const PhotoViewScaleStateControllerPtr& scaleStateController() const {
        return scaleStateController_;
    }

    // ── 对外的读数（测试与宿主都从这里问）───────────────────────────────────

    const PhotoViewScaleBoundaries& boundaries() const { return boundaries_; }
    Size outerSize() const { return outerSize_; }
    /// 图片自然尺寸；customChild 模式就是 widget 给的 childSize。
    const std::optional<Size>& naturalSize() const { return naturalSize_; }
    const std::string& error() const { return error_; }
    bool isAnimating() const { return animating_; }

    /// 上游 delegate 的 `scale` getter：需要重算或 controller 里还没有值时，
    /// 按当前 scaleState 现算一个并** invisibly 写回** controller。
    double scale() {
        const bool needsRecalc =
            markNeedsScaleRecalc_ && !isPhotoViewScaleStateZooming(scaleStateController_->scaleState());
        if (needsRecalc || !controller_->scale()) {
            const double newScale = getScaleForPhotoViewScaleState(
                scaleStateController_->scaleState(), boundaries_);
            markNeedsScaleRecalc_ = false;
            suppressRebuild_ = true;
            controller_->setScaleInvisibly(newScale);
            suppressRebuild_ = false;
            return newScale;
        }
        return *controller_->scale();
    }
    /// 上游 delegate 的 `set scale`。
    void setScale(double s) { controller_->setScaleInvisibly(s); }

    Offset position() const { return controller_->position(); }
    double rotation() const { return controller_->rotation(); }
    PhotoViewScaleState scaleState() const { return scaleStateController_->scaleState(); }

    /// 上游 delegate 的 `cornersX`。
    /// 注意 `(positionX - 1).abs()` 那个 `abs` 是必需的：`basePosition` 在
    /// `[-1, 1]`，`(positionX - 1)` 恒为负，少了 abs 就会算出 `min > max`
    /// （夹取直接把平移压成一个点）。
    PhotoViewCornersRange cornersX(double atScale) const {
        const double computedWidth = boundaries_.childSize.width * atScale;
        const double screenWidth = boundaries_.outerSize.width;
        const double positionX = basePosition().x;
        const double widthDiff = computedWidth - screenWidth;
        PhotoViewCornersRange r;
        r.min = (std::abs(positionX - 1.0) / 2.0) * widthDiff * -1.0;
        r.max = (std::abs(positionX + 1.0) / 2.0) * widthDiff;
        return r;
    }
    /// 上游 delegate 的 `cornersY`。
    PhotoViewCornersRange cornersY(double atScale) const {
        const double computedHeight = boundaries_.childSize.height * atScale;
        const double screenHeight = boundaries_.outerSize.height;
        const double positionY = basePosition().y;
        const double heightDiff = computedHeight - screenHeight;
        PhotoViewCornersRange r;
        r.min = (std::abs(positionY - 1.0) / 2.0) * heightDiff * -1.0;
        r.max = (std::abs(positionY + 1.0) / 2.0) * heightDiff;
        return r;
    }

    /// 上游 delegate 的 `clampPosition`：内容比外框小的那个轴**归零**
    /// （所以图片没放大到位时平移不动，是上游行为）。
    Offset clampPosition(std::optional<Offset> position = std::nullopt,
                         std::optional<double> atScale = std::nullopt) {
        const double s = atScale ? *atScale : scale();
        const Offset p = position ? *position : controller_->position();

        const double computedWidth = boundaries_.childSize.width * s;
        const double computedHeight = boundaries_.childSize.height * s;
        const double screenWidth = boundaries_.outerSize.width;
        const double screenHeight = boundaries_.outerSize.height;

        double finalX = 0.0;
        if (screenWidth < computedWidth) {
            const PhotoViewCornersRange cx = cornersX(s);
            finalX = clampPhotoViewValue(p.dx, cx.min, cx.max);
        }
        double finalY = 0.0;
        if (screenHeight < computedHeight) {
            const PhotoViewCornersRange cy = cornersY(s);
            finalY = clampPhotoViewValue(p.dy, cy.min, cy.max);
        }
        return Offset(finalX, finalY);
    }

    /// 上游 delegate 的 `updateScaleStateFromNewScale`。
    void updateScaleStateFromNewScale(double newScale) {
        PhotoViewScaleState newState = PhotoViewScaleState::Initial;
        if (scale() != boundaries_.initialScale) {
            newState = newScale > boundaries_.initialScale ? PhotoViewScaleState::ZoomedIn
                                                           : PhotoViewScaleState::ZoomedOut;
        }
        scaleStateController_->setInvisibly(newState);
    }

    /// 上游 delegate 的 `nextScaleState`：沿 cycle 走档，跳过「换了档但尺寸没变」。
    void nextScaleState() {
        const PhotoViewScaleState state = scaleStateController_->scaleState();
        if (state == PhotoViewScaleState::ZoomedIn || state == PhotoViewScaleState::ZoomedOut) {
            scaleStateController_->setScaleState(cycle()(state));
            return;
        }
        const double originalScale = getScaleForPhotoViewScaleState(state, boundaries_);
        double prevScale = originalScale;
        PhotoViewScaleState prev = state;
        double nextScale = originalScale;
        PhotoViewScaleState next = state;
        do {
            prevScale = nextScale;
            prev = next;
            next = cycle()(prev);
            nextScale = getScaleForPhotoViewScaleState(next, boundaries_);
        } while (prevScale == nextScale && state != next);

        if (originalScale == nextScale)
            return;
        scaleStateController_->setScaleState(next);
    }

    // ── 动画（上游的三个 AnimationController.fling 换成定长补间，见文件头第 4 条）─

    void animateScale(double from, double to) {
        animFromScale_ = from;
        animToScale_ = to;
        startAnimation();
    }
    void animatePosition(const Offset& from, const Offset& to) {
        animFromPosition_ = from;
        animToPosition_ = to;
        startAnimation();
    }
    void animateRotation(double from, double to) {
        animFromRotation_ = from;
        animToRotation_ = to;
        startAnimation();
    }
    /// 上游的 `animateOnScaleStateUpdate`。
    void animateOnScaleStateUpdate(double prevScale, double nextScale) {
        animateScale(prevScale, nextScale);
        animatePosition(controller_->position(), Offset());
        animateRotation(controller_->rotation(), 0.0);
    }
    /// 推进到 `elapsedMs` 那一刻的插值。Ticker 按真实时钟调它；测试直接传一个
    /// 大于 `animationMs` 的值就能一次性收敛（不必 sleep）。
    void advanceAnimation(double elapsedMs) {
        auto* w = widget_();
        const double total = w ? std::max(1.0, w->animationMs()) : 250.0;
        double t = total > 0.0 ? elapsedMs / total : 1.0;
        if (t > 1.0)
            t = 1.0;
        const double eased = applyEasing(clampPhotoViewValue(t, 0.0, 1.0),
                                         w ? w->animationCurve() : Easing::EaseOutCubic);
        if (animToScale_)
            controller_->setScaleInvisibly(*animFromScale_ +
                                           (*animToScale_ - *animFromScale_) * eased);
        if (animToPosition_) {
            const Offset to = *animToPosition_;
            const Offset from = *animFromPosition_;
            controller_->setPosition(Offset(from.dx + (to.dx - from.dx) * eased,
                                            from.dy + (to.dy - from.dy) * eased));
        }
        if (animToRotation_)
            controller_->setRotationInvisibly(*animFromRotation_ +
                                              (*animToRotation_ - *animFromRotation_) * eased);
        if (t < 1.0)
            return;
        finishAnimation();
    }
    void stopAnimation() {
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        animating_ = false;
        clearAnimationTargets();
    }

    // ── 构建 ────────────────────────────────────────────────────────────────

    SharedPtr<Widget> build(BuildContext& context) override {
        auto* w = widget_();
        if (!w)
            return nullptr;
        adoptControllers(w);

        // builder 在**布局期**才被调，所以 context 只能从回调形参拿（不能捕获
        // build 的引用 —— 那时它早就出栈了）。
        return cui::make_shared<LayoutBuilder>([this](BuildContext& ctx,
                                                      const BoxConstraints& constraints) {
            return buildSized(ctx, constraints);
        });
    }

private:
    PhotoView* widget_() const { return static_cast<PhotoView*>(widget()); }

    SharedPtr<Widget> buildSized(BuildContext& context, const BoxConstraints& constraints) {
        auto* w = widget_();
        const Size outer = w->customSize().value_or(constraints.biggest());
        outerSize_ = outer;

        // ── 内容：图片走 ExtendedImage，customChild 走调用方给的控件 ──────────
        SharedPtr<Widget> content;
        std::optional<Size> natural;
        if (w->isCustomChild()) {
            content = w->customChild();
            natural = w->childSize();
        } else {
            auto img = cui::make_shared<ExtendedImage>(
                w->provider(), BoxFit::Contain, Alignment::center(), std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                /*decodeAllFrames=*/true, /*useMemoryCache=*/true, /*useDiskCache=*/true,
                w->gaplessPlayback(), w->animate(), -1, w->loadingBuilder(), w->errorBuilder(),
                nullptr, nullptr, nullptr);
            img->setOnReady([this](const ImageLoadResult& result) { handleImageReady(result); });
            img->setOnError([this](const std::string& err) { handleImageError(err); });
            content = std::move(img);
            natural = naturalSize_;
        }

        if (!w->isCustomChild() && !error_.empty()) {
            // 上游：失败时整体替换掉 core（没有可缩放的东西）。
            SharedPtr<Widget> placeholder =
                w->errorBuilder() ? w->errorBuilder()(context, error_) : content;
            return cui::make_shared<DecoratedBox>(w->background(),
                                                  cui::make_shared<SizedBox>(
                                                      outer.width, outer.height, placeholder));
        }

        const Size childSize = natural.value_or(outer);
        boundaries_ = computePhotoViewScaleBoundaries(outer, childSize, w->minScale(),
                                                      w->maxScale(), w->initialScale());
        if (!(boundaries_ == cachedBoundaries_)) {
            markNeedsScaleRecalc_ = true;
            cachedBoundaries_ = boundaries_;
        }
        // 存进 controller，供宿主（gallery）在不碰 State 的情况下问「放大过了吗」。
        controller_->setBoundariesInvisibly(boundaries_);

        const double computedScale = scale();
        Matrix4 matrix = Matrix4::identity();
        matrix.translate(position().dx, position().dy, 0.0);
        matrix.scaleBy(computedScale, computedScale, computedScale);
        if (rotation() != 0.0)
            matrix.rotateZ(rotation());

        const Alignment base = basePosition();
        // 上游的 `_buildHero()`：Hero 包在最内层内容外（在 CustomSingleChildLayout
        // 之下、Image/customChild 之上），两种模式都认 heroAttributes。
        if (!w->heroTag().empty())
            content = cui::make_shared<Hero>(w->heroTag(), std::move(content));
        // 上游是 `CustomSingleChildLayout(_CenterWithOriginalSizeDelegate)`：自身
        // 占满外框，子节点拿到 `BoxConstraints.tight(childSize)` —— **不被外框的 max
        // 夹住**，再按 `halfDiff·(basePosition+1)` 摆位。本仓没有
        // CustomSingleChildLayout，`OverflowBox` 的四条 min/max + alignment 就是同
        // 一个形状。用 `Align(basePosition, SizedBox(childSize))` 复刻时 Align 只
        // loosen()（max 还留着），「比外框大的图」会先被夹成外框尺寸、再乘 scale
        // ⇒ 缩小看图小了一倍；摆位公式虽然一致，尺寸来源错了。
        SharedPtr<Widget> inner = cui::make_shared<OverflowBox>(
            boundaries_.childSize.width, boundaries_.childSize.width,
            boundaries_.childSize.height, boundaries_.childSize.height, base, std::move(content));
        inner = cui::make_shared<Transform>(
            matrix, std::move(inner), Offset(), true, nullptr, base);
        inner = cui::make_shared<Center>(std::move(inner));
        if (w->tightMode())
            inner = cui::make_shared<SizedBox>(boundaries_.childSize.width * computedScale,
                                               boundaries_.childSize.height * computedScale,
                                               std::move(inner));
        SharedPtr<Widget> child =
            cui::make_shared<DecoratedBox>(w->background(), std::move(inner));

        if (w->disableGestures())
            return child;

        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { handleDown(e); },
            [this](const PointerEvent& e) { handleMove(e); },
            [this](const PointerEvent& e) { handleUp(e); },
            Listener::PointerCallback{},  // hover 不参与手势
            [this](const PointerEvent& e) { handleScroll(e); },
            std::move(child));
    }

    Alignment basePosition() const {
        auto* w = widget_();
        return w && w->basePosition() ? *w->basePosition() : Alignment::center();
    }

    const PhotoViewScaleStateCycle& cycle() const {
        static const PhotoViewScaleStateCycle kDefault = defaultPhotoViewScaleStateCycle;
        auto* w = widget_();
        return w && w->scaleStateCycle() ? w->scaleStateCycle() : kDefault;
    }

    // ── 图片结果 ────────────────────────────────────────────────────────────

    void handleImageReady(const ImageLoadResult& result) {
        if (!result.image || !result.image->valid())
            return;
        const Size size(double(result.image->width()), double(result.image->height()));
        if (size == naturalSize_ && error_.empty())
            return;
        naturalSize_ = size;
        error_.clear();
        rebuildNow();
    }
    void handleImageError(const std::string& err) {
        if (error_ == err)
            return;
        error_ = err;
        rebuildNow();
    }

    /// build 里 `scale()` 的 invisibly 写回**不**再排一次重建：值已经在这一帧里
    /// 生效，重排只会把同一遍 build 再跑一次（下一次写回相等就自然收敛，但白搭
    /// 一帧）。图片结果的回调不受这个闸门管（它可能正好在 `buildSized` 挂
    /// ExtendedImage 的途中同步送达，那一帧的树里还没有新尺寸，必须排重建）。
    void requestRebuild() {
        if (suppressRebuild_)
            return;
        rebuildNow();
    }
    void rebuildNow() {
        if (mounted())
            setState();
    }

    // ── 控制器接线 ──────────────────────────────────────────────────────────

    void subscribe() {
        // 上游的四个订阅：StreamBuilder（重建）、outputScaleStateStream
        // （发 scaleStateChangedCallback）、两个 ignorable（delegate 逻辑）。
        ctrlHandle_ =
            controller_->addListenerWithHandle([this] { requestRebuild(); });
        // delegate 的 ignorable 监听与「重建」是两个独立订阅，分开写清楚：
        ctrlIgnorableHandle_ = controller_->addListenerWithHandle(
            [this] { handleControllerChangedIgnorable(); });
        stateHandle_ = scaleStateController_->addListenerWithHandle([this] {
            handleScaleStateChangedIgnorable();
            auto* w = widget_();
            if (w && w->scaleStateChangedCallback())
                w->scaleStateChangedCallback()(scaleStateController_->scaleState());
        });
    }
    void unsubscribe() {
        if (controller_ && ctrlHandle_ >= 0)
            controller_->removeListenerByHandle(ctrlHandle_);
        if (controller_ && ctrlIgnorableHandle_ >= 0)
            controller_->removeListenerByHandle(ctrlIgnorableHandle_);
        if (scaleStateController_ && stateHandle_ >= 0)
            scaleStateController_->removeListenerByHandle(stateHandle_);
        ctrlHandle_ = ctrlIgnorableHandle_ = stateHandle_ = -1;
    }

    /// 上游 `_blindScaleListener`（ignorable：内部 setScaleInvisibly 不打进来）。
    void handleControllerChangedIgnorable() {
        if (controller_->internalWrite())
            return;
        auto* w = widget_();
        if (!w)
            return;
        if (!w->enablePanAlways())
            controller_->setPosition(clampPosition());
        if (controller_->scale() == controller_->prevValue().scale)
            return;
        const PhotoViewScaleState newState =
            scale() > boundaries_.initialScale ? PhotoViewScaleState::ZoomedIn
                                               : PhotoViewScaleState::ZoomedOut;
        scaleStateController_->setInvisibly(newState);
    }
    /// 上游 `_blindScaleStateListener`（ignorable）。
    void handleScaleStateChangedIgnorable() {
        if (scaleStateController_->internalWrite())
            return;
        if (!scaleStateController_->hasChanged())
            return;
        if (scaleStateController_->isZooming()) {
            controller_->setScaleInvisibly(scale());
            return;
        }
        const double prevScale = controller_->scale()
                                     ? *controller_->scale()
                                     : getScaleForPhotoViewScaleState(
                                           scaleStateController_->prevScaleState(), boundaries_);
        const double nextScale =
            getScaleForPhotoViewScaleState(scaleStateController_->scaleState(), boundaries_);
        // 上游的 `_animateScale` 只在 `widget.animate` 时才注册，没注册就走上面那条
        // 「 invisibly 写回当前值」的短路。本仓没有那条函数指针，直接看 animate；且
        // 刻意写 nextScale 而不是当前 scale —— 上游那里 markNeedsScaleRecalc 已复位，
        // 写回的是换档前的旧值（等于什么都不做），于是 animate=false 时换档永远不落位。
        auto* w = widget_();
        if (!w || !w->animate()) {
            controller_->setScaleInvisibly(nextScale);
            controller_->setPositionInvisibly(Offset());
            controller_->setRotationInvisibly(0.0);
            return;
        }
        animateOnScaleStateUpdate(prevScale, nextScale);
    }

    void adoptControllers(PhotoView* w) {
        const PhotoViewControllerPtr& wantedController = w->controller();
        if (wantedController && wantedController != controller_) {
            unsubscribe();
            controller_ = wantedController;
            controllerExternal_ = true;
            subscribe();
        } else if (!wantedController && controllerExternal_) {
            unsubscribe();
            controller_ = std::make_shared<PhotoViewController>();
            controllerExternal_ = false;
            subscribe();
        }
        const PhotoViewScaleStateControllerPtr& wantedState = w->scaleStateController();
        if (wantedState && wantedState != scaleStateController_) {
            unsubscribe();
            scaleStateController_ = wantedState;
            scaleStateExternal_ = true;
            subscribe();
        } else if (!wantedState && scaleStateExternal_) {
            unsubscribe();
            scaleStateController_ = std::make_shared<PhotoViewScaleStateController>();
            scaleStateExternal_ = false;
            subscribe();
        }
    }

    // ── 指针表（上游的 ScaleGestureRecognizer，见文件头第 1 条）──────────────

    struct TrackedPointer {
        Offset global;
        Offset local;
        /// 按下点：`handleMove` 每帧刷新 `global/local`，所以「是不是 tap」必须拿
        /// 这个基准比 —— 否则拖了一大段的抬手，起点已经是最后一次移动的位置，
        /// 距离恒为 0 ⇒ 每次拖动都被当成 tap（进而被当成双击）。
        Offset downGlobal;
    };

    void focalPoint(Offset* global, Offset* local) const {
        double gx = 0.0, gy = 0.0, lx = 0.0, ly = 0.0;
        for (const auto& kv : pointers_) {
            gx += kv.second.global.dx;
            gy += kv.second.global.dy;
            lx += kv.second.local.dx;
            ly += kv.second.local.dy;
        }
        const double n = double(std::max<size_t>(pointers_.size(), 1));
        *global = {gx / n, gy / n};
        *local = {lx / n, ly / n};
    }

    double span() const {
        if (pointers_.size() < 2)
            return 0.0;
        auto it = pointers_.begin();
        const Offset a = it->second.global;
        const Offset b = (++it)->second.global;
        return (b - a).distance();
    }

    /// 两指连线与水平线的夹角；单指恒 0。
    double angle() const {
        if (pointers_.size() < 2)
            return 0.0;
        auto it = pointers_.begin();
        const Offset a = it->second.global;
        const Offset b = (++it)->second.global;
        return std::atan2(b.dy - a.dy, b.dx - a.dx);
    }

    void handleDown(const PointerEvent& e) {
        if (pointers_.count(e.pointerId))
            return;
        pointers_[e.pointerId] = {e.globalPosition, e.position, e.globalPosition};
        if (pointers_.size() == 1) {
            // 上游的 TapDown：按下立刻发。
            auto* w = widget_();
            if (w && w->onTapDown())
                w->onTapDown()(tapDetails(e.position, e.globalPosition));
            doubleTapCandidate_ = withinDoubleTapWindow(e.globalPosition);
            if (doubleTapCandidate_)
                clearTapRecord();
        }
        beginGesture();
    }

    void handleMove(const PointerEvent& e) {
        auto it = pointers_.find(e.pointerId);
        if (it == pointers_.end())
            return;
        const Offset moved = it->second.global - e.globalPosition;
        it->second = {e.globalPosition, e.position, it->second.downGlobal};
        if (moved.distance() > kDoubleTapSlopPx)
            clearTapRecord();
        updateGesture(e);
    }

    void handleUp(const PointerEvent& e) {
        auto it = pointers_.find(e.pointerId);
        if (it == pointers_.end())
            return;
        const Offset startGlobal = it->second.downGlobal;
        pointers_.erase(it);

        if (pointers_.empty()) {
            const bool isTap = !tapMoved(startGlobal, e.globalPosition);
            endGesture(e);
            if (!isTap) {
                // 拖动结束的抬手：既不发 tap，也不换档（记录一并作废，下一轮重新判）。
                doubleTapCandidate_ = false;
                clearTapRecord();
            } else if (doubleTapCandidate_) {
                // 成双的那一记把「这一对」整体消费掉：记录作废 ⇒ 第三记重新按单点
                // 算（上游的 DoubleTapGestureRecognizer 也是两记一组）。
                doubleTapCandidate_ = false;
                clearTapRecord();
                nextScaleState();
            } else {
                fireTapUp(e);
            }
            return;
        }
        // 还剩手指：以剩余手指为新的基准继续（上游 ScaleGestureRecognizer 同形）。
        beginGesture();
    }

    void handleScroll(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || e.scrollDeltaY == 0.0)
            return;  // 上游同样忽略纯横向滚轮
        const double current = scale();
        if (current <= 0.0)
            return;
        const double desired = current * std::exp(-e.scrollDeltaY / w->scaleFactor());
        // strictScale 的口径与上游 `onScaleUpdate` 同一条：本帧想要越界就整帧不动
        // （不 clamp、不消费事件 ⇒ 滚轮交还给祖先滚动视图）。
        if (w->strictScale() && (desired > boundaries_.maxScale || desired < boundaries_.minScale))
            return;
        const double target =
            clampPhotoViewValue(desired, boundaries_.minScale, boundaries_.maxScale);
        // 精确锚定（文件头第 5 条）：滚轮落在 (f-P) 上的场景点保持不动。
        const double k = target / current;
        const Offset pivot(((basePosition().x + 1.0) / 2.0) * boundaries_.outerSize.width,
                           ((basePosition().y + 1.0) / 2.0) * boundaries_.outerSize.height);
        const Offset focal = e.position;
        const Offset pos = position();
        const Offset anchored = {k * pos.dx + (1.0 - k) * (focal.dx - pivot.dx),
                                 k * pos.dy + (1.0 - k) * (focal.dy - pivot.dy)};
        updateScaleStateFromNewScale(target);
        const PhotoViewControllerValue before = controller_->value();
        controller_->updateMultiple(clampPosition(anchored, target), target);
        const PhotoViewControllerValue after = controller_->value();
        // 与 `updateGesture` 同一条口径：已经夹到 min/max 且平移也动不了 ⇒ 本帧没
        // 吃掉任何东西 ⇒ 不拦事件，滚轮交给祖先（gallery 翻页 / 外层滚动视图）。
        if (after.position != before.position || after.scale != before.scale)
            e.stopPropagation();
    }

    void beginGesture() {
        stopAnimation();
        scaleBefore_ = scale();
        rotationBefore_ = controller_->rotation();
        Offset global, local;
        focalPoint(&global, &local);
        // 上游用全局 focalPoint；本仓用局部（文件头第 6 条）。
        normalizedPosition_ = local - controller_->position();
        startSpan_ = span();
        startAngle_ = angle();
        gestureStarted_ = true;
    }

    void updateGesture(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || !gestureStarted_)
            return;
        Offset global, local;
        focalPoint(&global, &local);
        const double ratio = pointers_.size() >= 2 && startSpan_ > 0.0
                                 ? span() / startSpan_
                                 : 1.0;
        const double newScale = scaleBefore_ * ratio;
        const Offset delta = local - normalizedPosition_;

        if (w->strictScale() && (newScale > boundaries_.maxScale || newScale < boundaries_.minScale))
            return;

        updateScaleStateFromNewScale(newScale);

        const Offset nextPosition =
            w->enablePanAlways() ? delta : clampPosition(delta * ratio, newScale);
        std::optional<double> nextRotation;
        if (w->enableRotation() && pointers_.size() >= 2)
            nextRotation = rotationBefore_ + (angle() - startAngle_);

        const PhotoViewControllerValue before = controller_->value();
        controller_->updateMultiple(nextPosition, newScale, nextRotation);
        const PhotoViewControllerValue after = controller_->value();
        // 「撞到角就把拖拽让给父级」（文件头第 9 条）：本帧没吃掉任何位移/缩放/旋转
        // 就不拦事件 —— 于是夹到边界之后的剩余拖拽自动归祖先（gallery/滚动视图）。
        if (after.position != before.position || after.scale != before.scale ||
            after.rotation != before.rotation)
            e.stopPropagation();
    }

    void endGesture(const PointerEvent& e) {
        auto* w = widget_();
        gestureStarted_ = false;
        scaleBefore_ = 1.0;
        startSpan_ = 0.0;
        startAngle_ = 0.0;
        normalizedPosition_ = Offset();
        if (!w)
            return;

        const double current = scale();
        const Offset pos = controller_->position();
        const double maxScale = boundaries_.maxScale;
        const double minScale = boundaries_.minScale;

        if (w->onScaleEnd())
            w->onScaleEnd()(tapDetails(e.position, e.globalPosition));

        if (current > maxScale) {
            const double ratio = maxScale / current;
            animateScale(current, maxScale);
            animatePosition(pos, clampPosition(Offset(pos.dx * ratio, pos.dy * ratio), maxScale));
            return;
        }
        if (current < minScale) {
            const double ratio = minScale / current;
            animateScale(current, minScale);
            animatePosition(pos, clampPosition(Offset(pos.dx * ratio, pos.dy * ratio), minScale));
        }
        // 上游这里还有一段「速度 ≥ 400px/s 就再甩 100px」——本仓拿不到速度，
        // 见文件头第 3 条。
    }

    PhotoViewTapDetails tapDetails(const Offset& local, const Offset& global) const {
        PhotoViewTapDetails d;
        d.position = local;
        d.globalPosition = global;
        d.value = controller_->value();
        return d;
    }

    void fireTapUp(const PointerEvent& e) {
        recordTapUp(e);
        auto* w = widget_();
        if (w && w->onTapUp())
            w->onTapUp()(tapDetails(e.position, e.globalPosition));
    }
    /// 记下「这一记 tap 何时落在哪」，供下一记按下判定是否成双。
    void recordTapUp(const PointerEvent& e) {
        lastTapUpTime_ = nowMs();
        lastTapUpGlobal_ = e.globalPosition;
    }

    static double nowMs() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    bool withinDoubleTapWindow(const Offset& global) const {
        if (lastTapUpTime_ <= 0.0)
            return false;
        if (nowMs() - lastTapUpTime_ > kDoubleTapWindowMs)
            return false;
        return (global - lastTapUpGlobal_).distance() <= kDoubleTapSlopPx;
    }
    void clearTapRecord() {
        lastTapUpTime_ = 0.0;
        lastTapUpGlobal_ = Offset();
    }
    static bool tapMoved(const Offset& a, const Offset& b) {
        return (b - a).distance() > kDoubleTapSlopPx;
    }

    void startAnimation() {
        animating_ = true;
        if (ticker_) {
            // 上游每次 fling 前把 controller 的 value 归零 ⇒ 这里重开表重置时间线。
            if (ticker_->isActive())
                ticker_->stop();
            ticker_->start();
        }
    }
    void finishAnimation() {
        animating_ = false;
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        // 上游的 onAnimationStatusCompleted。
        if (scaleStateController_->scaleState() != PhotoViewScaleState::Initial &&
            controller_->scale().value_or(0.0) == boundaries_.initialScale)
            scaleStateController_->setInvisibly(PhotoViewScaleState::Initial);
        clearAnimationTargets();
    }
    void clearAnimationTargets() {
        animFromScale_ = animToScale_ = std::nullopt;
        animFromPosition_ = animToPosition_ = std::nullopt;
        animFromRotation_ = animToRotation_ = std::nullopt;
    }

    PhotoViewControllerPtr controller_;
    PhotoViewScaleStateControllerPtr scaleStateController_;
    bool controllerExternal_ = false;
    bool scaleStateExternal_ = false;
    int ctrlHandle_ = -1;
    int ctrlIgnorableHandle_ = -1;
    int stateHandle_ = -1;

    Size outerSize_{};
    PhotoViewScaleBoundaries boundaries_{};
    PhotoViewScaleBoundaries cachedBoundaries_{};
    bool markNeedsScaleRecalc_ = true;

    std::optional<Size> naturalSize_;
    std::string error_;
    bool suppressRebuild_ = false;

    std::map<int64_t, TrackedPointer> pointers_;
    bool gestureStarted_ = false;
    double scaleBefore_ = 1.0;
    double rotationBefore_ = 0.0;
    Offset normalizedPosition_;
    double startSpan_ = 0.0;
    double startAngle_ = 0.0;

    double lastTapUpTime_ = 0.0;
    Offset lastTapUpGlobal_;
    bool doubleTapCandidate_ = false;

    std::unique_ptr<Ticker> ticker_;
    bool animating_ = false;
    std::optional<double> animFromScale_, animToScale_;
    std::optional<Offset> animFromPosition_, animToPosition_;
    std::optional<double> animFromRotation_, animToRotation_;
};

inline std::unique_ptr<State> PhotoView::createState() {
    return std::make_unique<PhotoViewState>();
}

// ── PhotoViewGalleryPageOptions ─────────────────────────────────────────────

/// 上游的 `PhotoViewGalleryPageOptions` 与 `PhotoViewGalleryPageOptionsCustomChild`
/// 合成一个值类型：`customChild` 非空即 customChild 模式（上游靠两个子类区分）。
///
/// 字段全公开：既走 `forImage` / `forChild` 两个工厂，也能用指定初始化器
/// （**必须按声明序**）。`enableRotation` / `gaplessPlayback` 是 `optional<bool>`
/// ——这两条 gallery 自己也有一份，页面没给就用 gallery 的（上游同款）。
struct PhotoViewGalleryPageOptions {
    // ── 内容 ──
    SharedPtr<Widget> customChild;
    std::shared_ptr<ImageProvider> imageProvider;
    std::optional<Size> childSize;  ///< customChild 模式的自然尺寸
    ExtendedImage::LoadingBuilder loadingBuilder;
    ExtendedImage::ErrorBuilder errorBuilder;

    // ── 边界 ──
    std::optional<PhotoViewScale> minScale, maxScale, initialScale;
    std::optional<Size> customSize;
    std::optional<Alignment> basePosition;

    // ── 回调 ──
    PhotoViewTapCallback onTapUp, onTapDown;
    PhotoViewScaleEndCallback onScaleEnd;
    PhotoViewScaleStateChangedCallback scaleStateChangedCallback;

    // ── 开关 ──
    std::optional<bool> enableRotation;   ///< 缺省跟 gallery
    std::optional<bool> gaplessPlayback;  ///< 缺省跟 gallery
    bool tightMode = false;
    bool disableGestures = false;
    bool enablePanAlways = false;
    bool strictScale = false;

    // ── 动画 ──
    bool animate = true;
    double scaleFactor = 200.0;
    double animationMs = 250.0;
    Easing animationCurve = Easing::EaseOutCubic;

    // ── 其他 ──
    std::string heroTag;

    static PhotoViewGalleryPageOptions forImage(std::shared_ptr<ImageProvider> provider) {
        PhotoViewGalleryPageOptions o;
        o.imageProvider = std::move(provider);
        return o;
    }
    static PhotoViewGalleryPageOptions forChild(SharedPtr<Widget> child,
                                                std::optional<Size> childSize = std::nullopt) {
        PhotoViewGalleryPageOptions o;
        o.customChild = std::move(child);
        o.childSize = childSize;
        return o;
    }

    /// 上游靠两个构造分别置 `_child` / `_image`，这里看 `customChild` 非空。
    bool isCustomChild() const { return bool(customChild); }
};

// ── PhotoViewGallery ────────────────────────────────────────────────────────

/// 上游的 `PhotoViewGalleryPage`（`PhotoViewGalleryPageOptions Function(int index)`）。
using PhotoViewGalleryPageBuilder =
    std::function<PhotoViewGalleryPageOptions(int index)>;
/// 上游的 `PhotoViewGalleryPageChangedCallback`。
using PhotoViewGalleryPageChangedCallback = std::function<void(int index)>;

/// 分页看图。上游的 `PhotoViewGallery`。
///
/// 上游建在 `PageView` 上；本仓的 `PageView` 是装饰性卡片演示、没有分页语义，
/// 所以这里自己维护 `offset_`（以「页」为单位的浮点数）＋ `[page-1, page+1]`
/// 窗口（见文件头第 9 条）。
class PhotoViewGallery : public StatefulWidget {
public:
    explicit PhotoViewGallery(std::vector<PhotoViewGalleryPageOptions> pageOptions = {},
                              PhotoViewGalleryPageBuilder pageBuilder = nullptr,
                              int itemCount = 0, int initialIndex = 0,
                              Axis scrollDirection = Axis::Horizontal, bool reverse = false,
                              bool pageSnapping = true,
                              std::optional<Size> customSize = std::nullopt,
                              BoxDecoration background = photoViewDefaultBackground(),
                              bool enableRotation = false, bool gaplessPlayback = false,
                              double pageSnapMs = 250.0,
                              Easing pageSnapCurve = Easing::EaseOutCubic,
                              PhotoViewGalleryPageChangedCallback onPageChanged = nullptr,
                              PhotoViewScaleStateChangedCallback scaleStateChangedCallback = nullptr,
                              ExtendedImage::LoadingBuilder loadingBuilder = nullptr,
                              ExtendedImage::ErrorBuilder errorBuilder = nullptr,
                              KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), pageOptions_(std::move(pageOptions)),
          pageBuilder_(std::move(pageBuilder)), itemCount_(itemCount),
          initialIndex_(initialIndex), scrollDirection_(scrollDirection), reverse_(reverse),
          pageSnapping_(pageSnapping), customSize_(customSize), background_(background),
          enableRotation_(enableRotation), gaplessPlayback_(gaplessPlayback),
          pageSnapMs_(pageSnapMs), pageSnapCurve_(pageSnapCurve),
          onPageChanged_(std::move(onPageChanged)),
          scaleStateChangedCallback_(std::move(scaleStateChangedCallback)),
          loadingBuilder_(std::move(loadingBuilder)), errorBuilder_(std::move(errorBuilder)) {}

    /// 上游的 `PhotoViewGallery.builder`。
    static SharedPtr<PhotoViewGallery> builder(PhotoViewGalleryPageBuilder pageBuilder,
                                               int itemCount, KeyPtr key = nullptr) {
        return cui::make_shared<PhotoViewGallery>(std::vector<PhotoViewGalleryPageOptions>{},
                                                  std::move(pageBuilder), itemCount, 0,
                                                  Axis::Horizontal, false, true, std::nullopt,
                                                  photoViewDefaultBackground(), false, false,
                                                  250.0, Easing::EaseOutCubic, nullptr, nullptr,
                                                  nullptr, nullptr, std::move(key));
    }

    /// 有 `pageBuilder` 时用 `itemCount`，否则用列表长度（上游也是这个口径）。
    bool hasPageBuilder() const { return bool(pageBuilder_); }
    const std::vector<PhotoViewGalleryPageOptions>& pageOptions() const { return pageOptions_; }
    const PhotoViewGalleryPageBuilder& pageBuilder() const { return pageBuilder_; }
    PhotoViewGalleryPageOptions optionAt(int index) const {
        return pageBuilder_ ? pageBuilder_(index)
                            : (index >= 0 && index < int(pageOptions_.size())
                                   ? pageOptions_[size_t(index)]
                                   : PhotoViewGalleryPageOptions{});
    }

    int declaredItemCount() const { return itemCount_; }
    void setDeclaredItemCount(int v) { itemCount_ = v; }

    int initialIndex() const { return initialIndex_; }
    Axis scrollDirection() const { return scrollDirection_; }
    void setScrollDirection(Axis a) { scrollDirection_ = a; }
    bool reverse() const { return reverse_; }
    void setReverse(bool v) { reverse_ = v; }
    bool pageSnapping() const { return pageSnapping_; }
    void setPageSnapping(bool v) { pageSnapping_ = v; }
    const std::optional<Size>& customSize() const { return customSize_; }
    void setCustomSize(std::optional<Size> s) { customSize_ = s; }

    const BoxDecoration& background() const { return background_; }
    void setBackground(const BoxDecoration& b) { background_ = b; }
    bool enableRotation() const { return enableRotation_; }
    void setEnableRotation(bool v) { enableRotation_ = v; }
    bool gaplessPlayback() const { return gaplessPlayback_; }
    void setGaplessPlayback(bool v) { gaplessPlayback_ = v; }
    double pageSnapMs() const { return pageSnapMs_; }
    void setPageSnapMs(double v) { pageSnapMs_ = v; }
    Easing pageSnapCurve() const { return pageSnapCurve_; }
    void setPageSnapCurve(Easing v) { pageSnapCurve_ = v; }

    const PhotoViewGalleryPageChangedCallback& onPageChanged() const { return onPageChanged_; }
    void setOnPageChanged(PhotoViewGalleryPageChangedCallback cb) {
        onPageChanged_ = std::move(cb);
    }
    const PhotoViewScaleStateChangedCallback& scaleStateChangedCallback() const {
        return scaleStateChangedCallback_;
    }
    void setScaleStateChangedCallback(PhotoViewScaleStateChangedCallback cb) {
        scaleStateChangedCallback_ = std::move(cb);
    }
    const ExtendedImage::LoadingBuilder& loadingBuilder() const { return loadingBuilder_; }
    void setLoadingBuilder(ExtendedImage::LoadingBuilder b) { loadingBuilder_ = std::move(b); }
    const ExtendedImage::ErrorBuilder& errorBuilder() const { return errorBuilder_; }
    void setErrorBuilder(ExtendedImage::ErrorBuilder b) { errorBuilder_ = std::move(b); }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "PhotoViewGallery"; }

private:
    std::vector<PhotoViewGalleryPageOptions> pageOptions_;
    PhotoViewGalleryPageBuilder pageBuilder_;
    int itemCount_ = 0;
    int initialIndex_ = 0;
    Axis scrollDirection_ = Axis::Horizontal;
    bool reverse_ = false;
    bool pageSnapping_ = true;
    std::optional<Size> customSize_;
    BoxDecoration background_;
    bool enableRotation_ = false;
    bool gaplessPlayback_ = false;
    double pageSnapMs_ = 250.0;
    Easing pageSnapCurve_ = Easing::EaseOutCubic;
    PhotoViewGalleryPageChangedCallback onPageChanged_;
    PhotoViewScaleStateChangedCallback scaleStateChangedCallback_;
    ExtendedImage::LoadingBuilder loadingBuilder_;
    ExtendedImage::ErrorBuilder errorBuilder_;
};

// ── PhotoViewGalleryState ───────────────────────────────────────────────────

class PhotoViewGalleryState : public State {
public:
    void initState() override {
        auto* w = widget_();
        offset_ = double(clampIndex(w ? w->initialIndex() : 0));
        currentPage_ = int(std::lround(offset_));
        ticker_ = std::make_unique<Ticker>(
            [this](Duration elapsed) { advanceSettle(elapsed.count()); });
    }
    void dispose() override {
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    // ── 对外读数（宿主与测试都从这里问）─────────────────────────────────────

    int itemCount() const {
        auto* w = widget_();
        if (!w)
            return 0;
        return w->hasPageBuilder() ? std::max(0, w->declaredItemCount())
                                   : int(w->pageOptions().size());
    }
    double pageOffset() const { return offset_; }
    int currentPage() const { return currentPage_; }
    Size outerSize() const { return outerSize_; }
    bool isSettling() const { return settling_; }
    /// 第 index 页的平移/缩放控制器（画廊持有 ⇒ 翻出窗口再翻回来，缩放态还在，
    /// 等价于上游的 `wantKeepAlive`）。
    const PhotoViewControllerPtr& controllerFor(int index) { return ensureController(index); }
    const PhotoViewScaleStateControllerPtr& scaleStateFor(int index) {
        return ensureScaleState(index);
    }
    /// 这一页当前是否放大到超过 initialScale（PhotoView 会把边界写回控制器）。
    bool pageZoomed(int index) { return ensureController(index)->zoomedBeyondInitial(); }

    /// 跳到某页。`animate` 走 `pageSnapMs` 的补间；否则瞬时到位。
    void setPage(int index, bool animate = true) {
        const double target = double(clampIndex(index));
        if (animate && target != offset_) {
            startSettle(target);
            return;
        }
        stopSettle();
        applyOffset(target);
    }

    /// 推进补间到 `elapsedMs`。Ticker 按真实时钟调它；测试直接传一个大值即可
    /// 一次收敛（不必 sleep），或者调 finishSettle()。
    void advanceSettle(double elapsedMs) {
        if (!settling_)
            return;
        auto* w = widget_();
        const double total = std::max(1.0, w ? w->pageSnapMs() : 250.0);
        double t = elapsedMs / total;
        if (t > 1.0)
            t = 1.0;
        const double eased = applyEasing(clampPhotoViewValue(t, 0.0, 1.0),
                                         w ? w->pageSnapCurve() : Easing::EaseOutCubic);
        applyOffset(settleFrom_ + (settleTo_ - settleFrom_) * eased);
        if (t < 1.0)
            return;
        finishSettle();
    }
    /// 直接把补间收掉（吸附到目标页）。
    void finishSettle() {
        const double to = settleTo_;
        stopSettle();
        applyOffset(to);
    }

    // ── 构建 ────────────────────────────────────────────────────────────────

    SharedPtr<Widget> build(BuildContext& context) override {
        // 画廊没有「外部控制器」要重新认领，但 itemCount 可能在 update 之后变了，
        // 所以照样在 build 开头做一次同步（夹回 offset_、缩控制器表）。
        syncItemCount();
        return cui::make_shared<LayoutBuilder>(
            [this](BuildContext& ctx, const BoxConstraints& constraints) {
                return buildSized(ctx, constraints);
            });
    }

private:
    PhotoViewGallery* widget_() const { return static_cast<PhotoViewGallery*>(widget()); }

    int clampIndex(int index) const {
        const int count = itemCount();
        if (count <= 0)
            return 0;
        return index < 0 ? 0 : (index > count - 1 ? count - 1 : index);
    }
    void syncItemCount() {
        const int count = itemCount();
        if (count == lastItemCount_)
            return;
        lastItemCount_ = count;
        if (count <= 0) {
            stopSettle();
            offset_ = 0.0;
            currentPage_ = 0;
            return;
        }
        const double maxOffset = double(count - 1);
        if (offset_ < 0.0 || offset_ > maxOffset) {
            offset_ = clampPhotoViewValue(offset_, 0.0, maxOffset);
            stopSettle();
        }
        currentPage_ = int(std::lround(offset_));
    }

    bool isHorizontal() const {
        auto* w = widget_();
        return !w || w->scrollDirection() == Axis::Horizontal;
    }
    /// 主轴长度（一页占多少像素）。
    double mainExtent() const { return isHorizontal() ? outerSize_.width : outerSize_.height; }

    SharedPtr<Widget> buildSized(BuildContext& context, const BoxConstraints& constraints) {
        auto* w = widget_();
        outerSize_ = w->customSize().value_or(constraints.biggest());
        const int count = itemCount();

        std::vector<SharedPtr<Widget>> layers;
        if (count > 0) {
            const int lo = std::max(0, int(std::floor(offset_)) - 1);
            const int hi = std::min(count - 1, int(std::ceil(offset_)) + 1);
            layers.reserve(size_t(std::max(0, hi - lo + 1)));
            for (int i = lo; i <= hi; ++i) {
                SharedPtr<Widget> page =
                    cui::make_shared<ClipRect>(makePagePhotoView(w->optionAt(i), i));
                // 每页按页号挂 key：窗口两端进出时 Element 按 key 复用，
                // 不会被相邻页的位移「顺位接管」（上游 PageView 靠 PageStorageKey）。
                auto pos = cui::make_shared<Positioned>(
                    std::move(page), cui::make_shared<const ValueKey<int>>(i));
                const double shift = (double(i) - offset_) * mainExtent();
                const double signedShift = w->reverse() ? -shift : shift;
                if (isHorizontal()) {
                    pos->left = signedShift;
                    pos->top = 0.0;
                    pos->width = outerSize_.width;
                    pos->height = outerSize_.height;
                    pos->hasLeft = pos->hasTop = pos->hasWidth = pos->hasHeight = true;
                } else {
                    pos->left = 0.0;
                    pos->top = signedShift;
                    pos->width = outerSize_.width;
                    pos->height = outerSize_.height;
                    pos->hasLeft = pos->hasTop = pos->hasWidth = pos->hasHeight = true;
                }
                layers.push_back(std::move(pos));
            }
        }

        // Stack 的子节点全是 positioned ⇒ 尺寸取父约束的 biggest；外层 SizedBox
        // 只是把「窗口尺寸」钉死（RenderView 给的是紧约束时这一步是冗余的，
        // 嵌在别处时才是必需的）。
        SharedPtr<Widget> stack = cui::make_shared<Stack>(std::move(layers));
        SharedPtr<Widget> box = cui::make_shared<SizedBox>(outerSize_.width, outerSize_.height,
                                                          std::move(stack));
        SharedPtr<Widget> page = cui::make_shared<DecoratedBox>(w->background(), std::move(box));
        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { handleDown(e); },
            [this](const PointerEvent& e) { handleMove(e); },
            [this](const PointerEvent& e) { handleUp(e); },
            Listener::PointerCallback{},  // hover 不参与翻页
            [this](const PointerEvent& e) { handleScroll(e); },
            std::move(page));
    }

    SharedPtr<PhotoView> makePagePhotoView(const PhotoViewGalleryPageOptions& opt, int index) {
        auto* w = widget_();
        PhotoViewControllerPtr ctrl = ensureController(index);
        PhotoViewScaleStateControllerPtr stateCtrl = ensureScaleState(index);

        // 页面级回调与画廊级回调都要跑（上游是两条各自订阅流）。
        const auto pageCb = opt.scaleStateChangedCallback;
        const auto galleryCb = w->scaleStateChangedCallback();
        PhotoViewScaleStateChangedCallback cb;
        if (pageCb || galleryCb)
            cb = [pageCb, galleryCb](PhotoViewScaleState s) {
                if (pageCb)
                    pageCb(s);
                if (galleryCb)
                    galleryCb(s);
            };

        return cui::make_shared<PhotoView>(
            opt.customChild, opt.imageProvider, opt.childSize,
            // 页面没给 minScale 时用 contained（上游的缺省是 0 ⇒ 滚轮缩小永远「有
            // 效果」，于是画廊的翻页滚轮永远被页面吃掉）。口径：缩到「刚好铺满」就
            // 把滚轮让给画廊 ⇒ 滚轮缩小到尽头才能连着翻页。
            opt.minScale.has_value() ? opt.minScale
                                     : std::optional<PhotoViewScale>(PhotoViewScale::contained()),
            opt.maxScale, opt.initialScale,
            opt.customSize.has_value() ? opt.customSize : w->customSize(),
            opt.basePosition, w->background(), std::move(ctrl), std::move(stateCtrl), nullptr,
            opt.onTapUp, opt.onTapDown, opt.onScaleEnd, std::move(cb),
            opt.loadingBuilder ? opt.loadingBuilder : w->loadingBuilder(),
            opt.errorBuilder ? opt.errorBuilder : w->errorBuilder(),
            opt.enableRotation.value_or(w->enableRotation()), opt.tightMode,
            opt.disableGestures, opt.enablePanAlways, opt.strictScale,
            opt.gaplessPlayback.value_or(w->gaplessPlayback()), opt.animate, opt.scaleFactor,
            opt.animationMs, opt.animationCurve, opt.heroTag, nullptr);
    }

    // ── 控制器表（按页号持有，不随窗口增减而销毁）────────────────────────────

    PhotoViewControllerPtr& ensureController(int index) {
        if (index < 0)
            index = 0;
        if (size_t(index) >= controllers_.size())
            controllers_.resize(size_t(index) + 1);
        if (!controllers_[size_t(index)])
            controllers_[size_t(index)] = std::make_shared<PhotoViewController>();
        return controllers_[size_t(index)];
    }
    PhotoViewScaleStateControllerPtr& ensureScaleState(int index) {
        if (index < 0)
            index = 0;
        if (size_t(index) >= scaleStates_.size())
            scaleStates_.resize(size_t(index) + 1);
        if (!scaleStates_[size_t(index)])
            scaleStates_[size_t(index)] = std::make_shared<PhotoViewScaleStateController>();
        return scaleStates_[size_t(index)];
    }

    // ── 翻页手势 ────────────────────────────────────────────────────────────

    void handleDown(const PointerEvent& e) {
        if (pointers_.empty()) {
            dragActive_ = true;
            dragPointer_ = e.pointerId;
            startGlobal_ = e.globalPosition;
            offsetAtStart_ = offset_;
            stopSettle();
            wheelAccum_ = 0.0;
        }
        pointers_[e.pointerId] = e.globalPosition;
    }

    void handleMove(const PointerEvent& e) {
        auto it = pointers_.find(e.pointerId);
        if (it == pointers_.end())
            return;
        it->second = e.globalPosition;
        // 多指（捏合）交给页面里的 PhotoView，画廊不参与。
        if (!dragActive_ || e.pointerId != dragPointer_ || pointers_.size() != 1)
            return;
        const double extent = mainExtent();
        if (extent <= 0.0 || itemCount() <= 1)
            return;
        const Offset delta = Offset(e.globalPosition.dx - startGlobal_.dx,
                                    e.globalPosition.dy - startGlobal_.dy);
        const double along = alongAxis(delta);
        // 与 PhotoView 同一条口径：本帧真的改到了 offset 才算「画廊吃掉了拖拽」，
        // 于是夹到首/末页之后剩余的拖拽自动让给祖先。
        const double before = offset_;
        applyOffset(offsetAtStart_ - along / extent);
        if (offset_ != before)
            e.stopPropagation();
    }

    void handleUp(const PointerEvent& e) {
        pointers_.erase(e.pointerId);
        if (!pointers_.empty())
            return;
        if (!dragActive_)
            return;
        dragActive_ = false;
        auto* w = widget_();
        if (w && w->pageSnapping())
            startSettle(double(std::lround(offset_)));
    }

    /// 滚轮没有「抬手」，所以按页累积：攒满一整页就翻一整页（结果仍是整数偏移，
    /// 不需要吸附）。夹到首/末页时 `applyOffset` 不改值 ⇒ 这一帧不认领事件，
    /// 剩余滚轮自动让给祖先（与文件头第 9 条同一条口径）。
    void handleScroll(const PointerEvent& e) {
        auto* w = widget_();
        const int count = itemCount();
        if (!w || count <= 1)
            return;
        const double extent = mainExtent();
        if (extent <= 0.0)
            return;
        const double raw = isHorizontal() ? (e.scrollDeltaX + e.scrollDeltaY) : e.scrollDeltaY;
        if (raw == 0.0)
            return;
        wheelAccum_ += (w->reverse() ? -raw : raw) / extent;
        const int steps = int(std::trunc(wheelAccum_));
        if (steps == 0)
            return;
        wheelAccum_ -= double(steps);
        const double before = offset_;
        applyOffset(offset_ + double(steps));
        if (offset_ != before)
            e.stopPropagation();
    }

    /// 拖拽向量在主轴上的投影（含 reverse 的镜像）。
    double alongAxis(const Offset& delta) const {
        auto* w = widget_();
        const double main = isHorizontal() ? delta.dx : delta.dy;
        return (w && w->reverse()) ? -main : main;
    }

    void applyOffset(double next) {
        const int count = itemCount();
        // 严格夹取（上游 PageView 默认 ClampingScrollPhysics ⇒ 没有橡皮筋）。
        const double clamped =
            count <= 0 ? 0.0 : clampPhotoViewValue(next, 0.0, double(count - 1));
        if (clamped == offset_)
            return;
        offset_ = clamped;
        const int rounded = int(std::lround(offset_));
        if (rounded != currentPage_) {
            currentPage_ = rounded;
            auto* w = widget_();
            if (w && w->onPageChanged())
                w->onPageChanged()(currentPage_);
        }
        rebuildNow();
    }

    void startSettle(double target) {
        const int count = itemCount();
        if (count > 0)
            target = clampPhotoViewValue(target, 0.0, double(count - 1));
        if (target == offset_) {
            settling_ = false;
            if (ticker_ && ticker_->isActive())
                ticker_->stop();
            return;
        }
        settleFrom_ = offset_;
        settleTo_ = target;
        settling_ = true;
        if (ticker_) {
            if (ticker_->isActive())
                ticker_->stop();
            ticker_->start();
        }
        rebuildNow();
    }
    void stopSettle() {
        settling_ = false;
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
    }

    void rebuildNow() {
        if (mounted())
            setState();
    }

    double offset_ = 0.0;
    int currentPage_ = 0;
    int lastItemCount_ = -1;
    Size outerSize_{};
    std::vector<PhotoViewControllerPtr> controllers_;
    std::vector<PhotoViewScaleStateControllerPtr> scaleStates_;

    std::map<int64_t, Offset> pointers_;
    bool dragActive_ = false;
    int64_t dragPointer_ = -1;
    Offset startGlobal_;
    double offsetAtStart_ = 0.0;
    double wheelAccum_ = 0.0;

    std::unique_ptr<Ticker> ticker_;
    bool settling_ = false;
    double settleFrom_ = 0.0;
    double settleTo_ = 0.0;
};

inline std::unique_ptr<State> PhotoViewGallery::createState() {
    return std::make_unique<PhotoViewGalleryState>();
}

// ── PhotoViewSlidePage（滑出关闭）───────────────────────────────────────────

/// 拖拽方向。上游 `ExtendedImageSlidePage.slideAxis` 用的是 `Axis?`（null = 两个方向
/// 都行），本仓没有 nullable 枚举的习惯，换成显式的三值枚举。
enum class PhotoViewSlideAxis { Both, Horizontal, Vertical };

/// 滑动的作用范围。上游的 `SlideType`。
enum class PhotoViewSlideType {
    WholePage, ///< 整页（含背景）一起走
    OnlyImage  ///< 只有内容走 + 缩放，背景按透明度淡出
};

/// 上游的 `SlidePageBackgroundHandler`：由「画布尺寸 + 当前拖拽偏移」出背景色。
using PhotoViewSlideBackgroundHandler =
    std::function<Color(const Size& size, const Offset& offset)>;
/// 上游的 `SlideScaleHandler`：内容缩放系数。
using PhotoViewSlideScaleHandler =
    std::function<double(const Size& size, const Offset& offset)>;
/// 上游的 `SlideEndHandler`：抬手时是否真的关闭页面。
using PhotoViewSlideEndHandler =
    std::function<bool(const Size& size, const Offset& offset, double scale)>;
/// 上游的 `SlideOffsetHandler`：改写拖拽偏移（例如限制只能向下、加阻尼）。
using PhotoViewSlideOffsetHandler =
    std::function<Offset(const Size& size, const Offset& offset)>;
/// 上游的 `OnSlidingPage`：拖拽过程中把当前偏移报给宿主。
using PhotoViewSlidingPageCallback = std::function<void(const Offset& offset)>;

/// 缺省的比例：`Both` 用「拖拽距离 / 对角线长」，单向用「拖拽距离 / 该边长」。
/// 上游 `defaultSlidePageBackgroundHandler` 就是这个口径（README 里写明的两条）。
inline double photoViewSlideRatio(const Size& size, const Offset& offset,
                                  PhotoViewSlideAxis axis) {
    const double diag = std::hypot(size.width, size.height);
    switch (axis) {
        case PhotoViewSlideAxis::Horizontal:
            return size.width > 0.0 ? std::abs(offset.dx) / size.width : 0.0;
        case PhotoViewSlideAxis::Vertical:
            return size.height > 0.0 ? std::abs(offset.dy) / size.height : 0.0;
        case PhotoViewSlideAxis::Both:
            return diag > 0.0 ? std::hypot(offset.dx, offset.dy) / diag : 0.0;
    }
    return 0.0;
}

/// 上游 `ExtendedImageSlidePage` 的缺省滑出关闭页。
///
/// 三条缺省公式（extended_image 的源码在本机取不到，README 只给了
/// `slideEndRatio`/`resetPageDuration` 的默认值与背景透明度公式，所以这三条
/// 是按 README 的语义实现的，写在类注释里，不当 bug 排查）：
///   - 背景：`colorWithOpacity(background, a * (1 - min(1, ratio)))`
///   - 缩放：`1 - 0.15 * min(1, ratio)`（拖到底收到 85%，`OnlyImage` 才生效）
///   - 结束判定：`ratio >= slideEndRatio`（默认 0.25）
class PhotoViewSlidePage : public StatefulWidget {
public:
    explicit PhotoViewSlidePage(SharedPtr<Widget> child = nullptr,
                                Color background = Color(0.0, 0.0, 0.0, 1.0),
                                double slideEndRatio = 0.25,
                                PhotoViewSlideAxis slideAxis = PhotoViewSlideAxis::Vertical,
                                PhotoViewSlideType slideType = PhotoViewSlideType::OnlyImage,
                                PhotoViewSlideBackgroundHandler slidePageBackgroundHandler = nullptr,
                                PhotoViewSlideScaleHandler slideScaleHandler = nullptr,
                                PhotoViewSlideEndHandler slideEndHandler = nullptr,
                                PhotoViewSlideOffsetHandler slideOffsetHandler = nullptr,
                                double resetPageDurationMs = 500.0,
                                PhotoViewSlidingPageCallback onSlidingPage = nullptr,
                                bool enableSlideOutPage = true, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), background_(background),
          slideEndRatio_(slideEndRatio), slideAxis_(slideAxis), slideType_(slideType),
          backgroundHandler_(std::move(slidePageBackgroundHandler)),
          scaleHandler_(std::move(slideScaleHandler)), endHandler_(std::move(slideEndHandler)),
          offsetHandler_(std::move(slideOffsetHandler)),
          resetPageDurationMs_(resetPageDurationMs), onSlidingPage_(std::move(onSlidingPage)),
          enableSlideOutPage_(enableSlideOutPage) {}

    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    Color background() const { return background_; }
    void setBackground(const Color& c) { background_ = c; }
    double slideEndRatio() const { return slideEndRatio_; }
    void setSlideEndRatio(double v) { slideEndRatio_ = v; }
    PhotoViewSlideAxis slideAxis() const { return slideAxis_; }
    void setSlideAxis(PhotoViewSlideAxis v) { slideAxis_ = v; }
    PhotoViewSlideType slideType() const { return slideType_; }
    void setSlideType(PhotoViewSlideType v) { slideType_ = v; }

    const PhotoViewSlideBackgroundHandler& slidePageBackgroundHandler() const {
        return backgroundHandler_;
    }
    const PhotoViewSlideScaleHandler& slideScaleHandler() const { return scaleHandler_; }
    const PhotoViewSlideEndHandler& slideEndHandler() const { return endHandler_; }
    const PhotoViewSlideOffsetHandler& slideOffsetHandler() const { return offsetHandler_; }
    const PhotoViewSlidingPageCallback& onSlidingPage() const { return onSlidingPage_; }

    double resetPageDurationMs() const { return resetPageDurationMs_; }
    void setResetPageDurationMs(double v) { resetPageDurationMs_ = v; }
    bool enableSlideOutPage() const { return enableSlideOutPage_; }
    void setEnableSlideOutPage(bool v) { enableSlideOutPage_ = v; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "PhotoViewSlidePage"; }

private:
    SharedPtr<Widget> child_;
    Color background_ = Color(0.0, 0.0, 0.0, 1.0);
    double slideEndRatio_ = 0.25;
    PhotoViewSlideAxis slideAxis_ = PhotoViewSlideAxis::Vertical;
    PhotoViewSlideType slideType_ = PhotoViewSlideType::OnlyImage;
    PhotoViewSlideBackgroundHandler backgroundHandler_;
    PhotoViewSlideScaleHandler scaleHandler_;
    PhotoViewSlideEndHandler endHandler_;
    PhotoViewSlideOffsetHandler offsetHandler_;
    double resetPageDurationMs_ = 500.0;
    PhotoViewSlidingPageCallback onSlidingPage_;
    bool enableSlideOutPage_ = true;
};

class PhotoViewSlidePageState : public State {
public:
    void initState() override {
        ticker_ = std::make_unique<Ticker>(
            [this](Duration elapsed) { advanceReset(elapsed.count()); });
    }
    void dispose() override {
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    // ── 对外读数 ────────────────────────────────────────────────────────────

    Offset slideOffset() const { return offset_; }
    Size outerSize() const { return outerSize_; }
    bool isDragging() const { return dragActive_; }
    bool isResetting() const { return resetting_; }
    /// 当前内容缩放系数（`WholePage` 模式恒 1）。
    double currentScale() const {
        auto* w = widget_();
        const Size size = sizeOrZero();
        if (!w)
            return 1.0;
        const double t = std::min(1.0, ratio());
        if (w->slideScaleHandler())
            return w->slideScaleHandler()(size, offset_);
        return w->slideType() == PhotoViewSlideType::OnlyImage ? 1.0 - 0.15 * t : 1.0;
    }
    /// 当前背景色（含淡出的 alpha）。
    Color currentBackground() const {
        auto* w = widget_();
        if (!w)
            return Color(0.0, 0.0, 0.0, 1.0);
        const Size size = sizeOrZero();
        if (w->slidePageBackgroundHandler())
            return w->slidePageBackgroundHandler()(size, offset_);
        const double t = std::min(1.0, ratio());
        return colorWithOpacity(w->background(), w->background().a * (1.0 - t));
    }
    double ratio() const {
        auto* w = widget_();
        return w ? photoViewSlideRatio(outerSize_, offset_, w->slideAxis()) : 0.0;
    }
    /// 抬手时会不会真的关掉这一页。
    bool willCloseOnRelease() const {
        auto* w = widget_();
        if (!w || !w->enableSlideOutPage())
            return false;
        if (w->slideEndHandler())
            return w->slideEndHandler()(sizeOrZero(), offset_, currentScale());
        return ratio() >= w->slideEndRatio();
    }

    /// 复位补间的推进。Ticker 按真实时钟调它；测试传一个大值即可一次收敛。
    void advanceReset(double elapsedMs) {
        if (!resetting_)
            return;
        auto* w = widget_();
        const double total = std::max(1.0, w ? w->resetPageDurationMs() : 500.0);
        double t = elapsedMs / total;
        if (t >= 1.0) {
            finishReset();
            return;
        }
        const double eased = applyEasing(clampPhotoViewValue(t, 0.0, 1.0), Easing::EaseOutCubic);
        setOffset(Offset(resetFrom_.dx * (1.0 - eased), resetFrom_.dy * (1.0 - eased)));
    }
    /// 直接回到原位（停表 + 归零偏移）。
    void finishReset() {
        resetting_ = false;
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        setOffset(Offset());
    }
    /// 外部（或测试）直接设定滑出偏移。
    void setOffset(const Offset& next) {
        if (next == offset_)
            return;
        offset_ = next;
        auto* w = widget_();
        if (w && w->onSlidingPage())
            w->onSlidingPage()(offset_);
        if (mounted())
            setState();
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        return cui::make_shared<LayoutBuilder>(
            [this](BuildContext& ctx, const BoxConstraints& constraints) {
                return buildSized(ctx, constraints);
            });
    }

private:
    PhotoViewSlidePage* widget_() const {
        return static_cast<PhotoViewSlidePage*>(widget());
    }
    Size sizeOrZero() const { return outerSize_; }

    SharedPtr<Widget> buildSized(BuildContext& context, const BoxConstraints& constraints) {
        auto* w = widget_();
        outerSize_ = constraints.biggest();

        BoxDecoration decor;
        decor.color = currentBackground();
        SharedPtr<Widget> content = w->child();
        const double scale = currentScale();

        Matrix4 matrix = Matrix4::identity();
        matrix.translate(offset_.dx, offset_.dy, 0.0);
        if (scale != 1.0)
            matrix.scaleBy(scale, scale, scale);

        if (w->slideType() == PhotoViewSlideType::OnlyImage) {
            content = cui::make_shared<Transform>(matrix, std::move(content), Offset(), true,
                                                 nullptr, Alignment::center());
        }
        SharedPtr<Widget> page =
            cui::make_shared<DecoratedBox>(decor, cui::make_shared<SizedBox>(
                                                      outerSize_.width, outerSize_.height,
                                                      std::move(content)));
        if (w->slideType() == PhotoViewSlideType::WholePage) {
            // 整页位移：背景跟着走，下面那条路由从 opaque=false 的缺口里透出来。
            Matrix4 moved = Matrix4::translation(offset_.dx, offset_.dy);
            page = cui::make_shared<Transform>(moved, std::move(page));
        }
        if (!w->enableSlideOutPage())
            return page;
        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { handleDown(e); },
            [this](const PointerEvent& e) { handleMove(e); },
            [this](const PointerEvent& e) { handleUp(e); },
            Listener::PointerCallback{}, Listener::PointerCallback{}, std::move(page));
    }

    // ── 拖拽 ────────────────────────────────────────────────────────────────

    void handleDown(const PointerEvent& e) {
        if (pointers_.empty()) {
            dragActive_ = true;
            dragPointer_ = e.pointerId;
            startGlobal_ = e.globalPosition;
            offsetAtStart_ = offset_;
            resetting_ = false;
            if (ticker_ && ticker_->isActive())
                ticker_->stop();
        }
        pointers_[e.pointerId] = e.globalPosition;
    }

    void handleMove(const PointerEvent& e) {
        auto it = pointers_.find(e.pointerId);
        if (it == pointers_.end())
            return;
        it->second = e.globalPosition;
        if (!dragActive_ || e.pointerId != dragPointer_)
            return;
        auto* w = widget_();
        const Offset raw = Offset(e.globalPosition.dx - startGlobal_.dx,
                                  e.globalPosition.dy - startGlobal_.dy);
        Offset next = offsetAtStart_ + raw;
        // 轴锁定（上游用 Axis 决定，本仓多一个 Both）。
        switch (w->slideAxis()) {
            case PhotoViewSlideAxis::Horizontal:
                next = Offset(next.dx, 0.0);
                break;
            case PhotoViewSlideAxis::Vertical:
                next = Offset(0.0, next.dy);
                break;
            case PhotoViewSlideAxis::Both:
                break;
        }
        if (w->slideOffsetHandler())
            next = w->slideOffsetHandler()(outerSize_, next);
        setOffset(next);
        e.stopPropagation();
    }

    void handleUp(const PointerEvent& e) {
        pointers_.erase(e.pointerId);
        if (!pointers_.empty() || !dragActive_)
            return;
        dragActive_ = false;
        if (willCloseOnRelease()) {
            auto* nav = NavigatorState::maybeOf(*context());
            if (nav)
                nav->pop();
            return;
        }
        if (offset_ == Offset())
            return;
        resetFrom_ = offset_;
        resetting_ = true;
        if (ticker_) {
            if (ticker_->isActive())
                ticker_->stop();
            ticker_->start();
        }
    }

    std::map<int64_t, Offset> pointers_;
    bool dragActive_ = false;
    int64_t dragPointer_ = -1;
    Offset startGlobal_;
    Offset offsetAtStart_;
    Offset offset_;
    Size outerSize_{};

    std::unique_ptr<Ticker> ticker_;
    bool resetting_ = false;
    Offset resetFrom_;
};

inline std::unique_ptr<State> PhotoViewSlidePage::createState() {
    return std::make_unique<PhotoViewSlidePageState>();
}

/// 把滑出关闭页包成一条淡入淡出的透明路由（上游是 `PageRouteBuilder(opaque: false)`
/// + `ExtendedImageSlidePage`）。`opaque=false` 让下面那条路由继续绘制，
/// `WholePage` 模式滑开时才能透出原页面。
inline SharedPtr<PageRouteBuilder> makePhotoViewSlideRoute(SharedPtr<Widget> page,
                                                           double transitionMs = 300.0) {
    return cui::make_shared<PageRouteBuilder>(
        [page](BuildContext&, SharedPtr<Animation<double>>, SharedPtr<Animation<double>>) {
            return page;
        },
        [](BuildContext&, SharedPtr<Animation<double>> animation, SharedPtr<Animation<double>>,
           SharedPtr<Widget> child) {
            return cui::make_shared<FadeTransition>(std::move(animation), std::move(child));
        },
        transitionMs, std::nullopt, /*opaque=*/false);
}

}  // namespace cui_core
