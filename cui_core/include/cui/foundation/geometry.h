#pragma once
/// @file geometry.h
/// @brief Basic geometry types used throughout the framework.

#include <algorithm>
#include <cmath>
#include <optional>

namespace cui {

struct Point {
    double x = 0;
    double y = 0;

    // 同 Size/Color：`Point(x, y)` 这种带括号的聚合初始化属于 C++20，
    // OHOS NDK 26.0.0 的 clang 15 没实现（本仓所有调用点都是 0 参或 2 参）。
    constexpr Point() = default;
    constexpr Point(double x_, double y_) : x(x_), y(y_) {}
};

/// A 2-D displacement, mirroring the upstream `Offset`.  Distinct from `Point`
/// (a location) in intent: parentData offsets, deltas and velocity are
/// Offsets and can be negative / added to a Point to yield a Point.
struct Offset {
    double dx = 0;
    double dy = 0;

    constexpr Offset() = default;
    constexpr Offset(double x, double y) : dx(x), dy(y) {}

    constexpr Offset operator+(const Offset& o) const { return {dx + o.dx, dy + o.dy}; }
    constexpr Offset operator-(const Offset& o) const { return {dx - o.dx, dy - o.dy}; }
    constexpr Offset operator*(double s) const { return {dx * s, dy * s}; }
    constexpr bool operator==(const Offset& o) const { return dx == o.dx && dy == o.dy; }

    double distance() const { return std::sqrt(dx * dx + dy * dy); }
};

/// Point + Offset -> Point (the upstream's `Offset + Offset`/`point.translate`).
constexpr Point operator+(const Point& p, const Offset& o) { return {p.x + o.dx, p.y + o.dy}; }
constexpr Point operator-(const Point& p, const Offset& o) { return {p.x - o.dx, p.y - o.dy}; }

/// 指针速度。上游的 `Velocity`（geometry/velocity.dart）。
///
/// 只移植「像素/秒」这一个量（上游 `Draggable.onDraggableCanceled`、
/// `DraggableDetails.velocity` 用它）；`angleInRadians` 构造、`direction`、
/// `asAngle()` 未移植——本仓还没有速度估计器，这些值恒为 0。
struct Velocity {
    Offset pixelsPerSecond{};

    constexpr Velocity() = default;
    constexpr Velocity(double dx, double dy) : pixelsPerSecond(dx, dy) {}

    constexpr bool operator==(const Velocity& o) const {
        return pixelsPerSecond == o.pixelsPerSecond;
    }
    constexpr bool operator!=(const Velocity& o) const { return !(*this == o); }

    /// 上游的 `Velocity.zero`。
    static constexpr Velocity zero() { return {}; }

    double speed() const { return pixelsPerSecond.distance(); }
};

struct Size {
    double width = 0;
    double height = 0;

    // 构造函数是必需的，不是糖：`Size(w, h)` 这种带括号的聚合初始化属于 C++20，
    // OHOS NDK 26.0.0 的 clang 15 没实现（同文件 Rect/Offset 早就有自己的 ctor）。
    constexpr Size() = default;
    constexpr Size(double w, double h) : width(w), height(h) {}

    bool operator==(const Size& o) const { return width == o.width && height == o.height; }
    bool operator!=(const Size& o) const { return !(*this == o); }

    /// 上游 的 `Size.contains(Offset)`：局部坐标是否落在尺寸内（半开区间，
    /// 右/下边不含）。`Rect.contains` 的 `Offset.zero & size` 特例，命中测试用。
    constexpr bool contains(const Offset& o) const {
        return o.dx >= 0 && o.dy >= 0 && o.dx < width && o.dy < height;
    }
};

struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;

    constexpr Rect() = default;
    constexpr Rect(double x_, double y_, double w_, double h_)
        : x(x_), y(y_), width(w_), height(h_) {}

    double left() const { return x; }
    double top() const { return y; }
    double right() const { return x + width; }
    double bottom() const { return y + height; }

    bool contains(const Point& p) const { return p.x >= x && p.x < x + width && p.y >= y && p.y < y + height; }

    /// True when a 1-pixel-wide area of this rect overlaps `o`.  Empty rects
    /// (zero width/height, the sentinel for "no damage") never intersect.
    bool intersects(const Rect& o) const {
        if (width <= 0 || height <= 0 || o.width <= 0 || o.height <= 0)
            return false;
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }

    /// Smallest rect covering both.  An empty (zero-size) rect contributes
    /// nothing, matching how damage unions ignore cleared sentinels.
    Rect unite(const Rect& o) const;

    /// Overlap rect (empty when disjoint).
    Rect intersect(const Rect& o) const;

    /// Grow every edge outward by `by` (the damage overdraw padding).
    Rect inflate(double by) const { return {x - by, y - by, width + 2 * by, height + 2 * by}; }

    /// Clamp inside `bounds`, keeping the origin (used to window-clip damage).
    Rect intersectWith(const Rect& bounds) const { return intersect(bounds); }

    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

/// Edge insets for margin and padding (left/top/right/bottom).
struct EdgeInsets;

/// Axis-aligned box constraints, mirroring the upstream `BoxConstraints`
/// (上游 rendering/box.dart).
///
/// A parent hands a set of these to a child's `RenderObject::layout`; the
/// child must return a size that satisfies them.  `min == max` on an axis is
/// a *tight* constraint (size forced); `min < max` is *loose*.
///
/// Key semantics (aligned with 上游):
///   - `tighten` 收紧、`loosen` 放松、`deflate` 内缩、`enforce` 取交集。
///   - `tightFor` / `expand` 接受 `std::optional<double>`，nullopt 表示
///     该轴不约束。
struct BoxConstraints {
    double minWidth = 0;
    double maxWidth = 0;
    double minHeight = 0;
    double maxHeight = 0;

    static constexpr double kInfinity = 1e18;

    constexpr BoxConstraints() = default;
    constexpr BoxConstraints(double minW, double maxW, double minH, double maxH)
        : minWidth(minW), maxWidth(maxW), minHeight(minH), maxHeight(maxH) {}

    // ── Factories ─────────────────────────────────────────────────────────

    /// 紧约束：强制为给定尺寸。
    static constexpr BoxConstraints tight(double w, double h) { return {w, w, h, h}; }
    static constexpr BoxConstraints tightSize(const Size& s) {
        return {s.width, s.width, s.height, s.height};
    }
    static constexpr BoxConstraints tight(const Size& s) { return tightSize(s); }

    /// 紧约束但只锁给定轴。上游的 `BoxConstraints.tightFor`。
    /// 传入 nullopt 表示该轴不约束（0 .. kInfinity）。
    static BoxConstraints tightFor(const std::optional<double>& width,
                                   const std::optional<double>& height) {
        return {width.value_or(0.0), width.value_or(kInfinity),
                height.value_or(0.0), height.value_or(kInfinity)};
    }

    /// 松约束：尺寸在 [0, max]。
    static constexpr BoxConstraints loose(double w, double h) { return {0, w, 0, h}; }
    static constexpr BoxConstraints loose(const Size& s) { return loose(s.width, s.height); }

    /// 无限约束：子节点可自由决定尺寸。
    static constexpr BoxConstraints unbounded() { return {0, kInfinity, 0, kInfinity}; }

    /// 展开为给定尺寸的紧约束。上游的 `BoxConstraints.expand`。
    static BoxConstraints expand(const std::optional<double>& width,
                                 const std::optional<double>& height) {
        return {width.value_or(kInfinity), width.value_or(kInfinity),
                height.value_or(kInfinity), height.value_or(kInfinity)};
    }

    /// 把 value 夹到 [lo, hi] 区间内。上游的 `clampDouble`。
    /// 区间非法（lo > hi）时退化为返回 lo，避免产生非法约束。
    static constexpr double clampTo(double value, double lo, double hi) {
        if (lo > hi) return lo;
        if (value < lo) return lo;
        if (value > hi) return hi;
        return value;
    }

    // ── Queries ───────────────────────────────────────────────────────────

    bool isTight() const { return minWidth == maxWidth && minHeight == maxHeight; }
    bool hasTightWidth() const { return minWidth >= maxWidth; }
    bool hasTightHeight() const { return minHeight >= maxHeight; }
    bool hasBoundedWidth() const { return maxWidth < kInfinity; }
    bool hasBoundedHeight() const { return maxHeight < kInfinity; }
    bool hasInfiniteWidth() const { return minWidth >= kInfinity; }
    bool hasInfiniteHeight() const { return minHeight >= kInfinity; }

    /// 尺寸是否满足约束。
    bool isSatisfiedBy(const Size& size) const {
        return size.width >= minWidth && size.width <= maxWidth &&
               size.height >= minHeight && size.height <= maxHeight;
    }

    // ── Constriction ──────────────────────────────────────────────────────

    /// Clamp a width into [minWidth, maxWidth].
    double constrainWidth(double w = kInfinity) const {
        if (w < minWidth) return minWidth;
        if (w > maxWidth) return maxWidth;
        return w;
    }
    /// Clamp a height into [minHeight, maxHeight].
    double constrainHeight(double h = kInfinity) const {
        if (h < minHeight) return minHeight;
        if (h > maxHeight) return maxHeight;
        return h;
    }
    Size constrain(const Size& s) const {
        return {constrainWidth(s.width), constrainHeight(s.height)};
    }

    /// 可选的最大尺寸（无界时返回 kInfinity）。
    Size biggest() const { return {constrainWidth(), constrainHeight()}; }
    /// 最小的合法尺寸。
    Size smallest() const { return {constrainWidth(0.0), constrainHeight(0.0)}; }

    /// A tight copy forcing the given size (clamp 语义：夹进当前范围)。
    BoxConstraints tighten(double w, double h) const {
        return {constrainWidth(w), constrainWidth(w), constrainHeight(h), constrainHeight(h)};
    }
    /// 收紧为指定尺寸（上游的 `tighten`，不做范围收缩，只把该轴
    /// min/max 同时设为给定值；nullopt 轴保持不变）。
    BoxConstraints tighten(const std::optional<double>& width,
                           const std::optional<double>& height) const {
        return {width ? *width : minWidth, width ? *width : maxWidth,
                height ? *height : minHeight, height ? *height : maxHeight};
    }
    /// Relax to [0, current max] — the constraints a scrollable hands down so
    /// the child is not forced to fill the viewport cross/main extent.
    BoxConstraints loosen() const { return {0, maxWidth, 0, maxHeight}; }

    /// 按内缩量收缩约束（上游的 `deflate`）。
    BoxConstraints deflate(double horizontal, double vertical) const {
        return {std::max(0.0, minWidth - horizontal), std::max(0.0, maxWidth - horizontal),
                std::max(0.0, minHeight - vertical), std::max(0.0, maxHeight - vertical)};
    }
    BoxConstraints deflate(const EdgeInsets& e) const;

    /// 与另一组约束取交集（上游的 `enforce`，clamp 语义：把本约束的
    /// min / max 各自夹进对方允许的区间——当本约束是紧约束而对方更宽时，
    /// 紧约束被放宽到对方的区间，而不是产生 min > max 的非法约束）。
    BoxConstraints enforce(const BoxConstraints& other) const {
        return {clampTo(minWidth, other.minWidth, other.maxWidth),
                clampTo(maxWidth, other.minWidth, other.maxWidth),
                clampTo(minHeight, other.minHeight, other.maxHeight),
                clampTo(maxHeight, other.minHeight, other.maxHeight)};
    }

    /// 在保持宽高比的前提下把 `size` 夹进约束（上游的
    /// `BoxConstraints.constrainSizeAndAttemptToPreserveAspectRatio`）。
    ///
    /// 紧约束直接返回 smallest；否则先按 maxWidth / maxHeight、再按
    /// minWidth / minHeight 依次修正，另一轴随宽高比联动，最后 clamp。
    /// 宽或高 ≤ 0 时退化为普通 constrain（无法谈比例）。
    Size constrainSizeAndAttemptToPreserveAspectRatio(const Size& size) const {
        if (isTight())
            return smallest();
        if (size.width <= 0.0 || size.height <= 0.0)
            return constrain(size);
        double width = size.width;
        double height = size.height;
        const double aspectRatio = width / height;
        if (width > maxWidth) {
            width = maxWidth;
            height = width / aspectRatio;
        }
        if (height > maxHeight) {
            height = maxHeight;
            width = height * aspectRatio;
        }
        if (width < minWidth) {
            width = minWidth;
            height = width / aspectRatio;
        }
        if (height < minHeight) {
            height = minHeight;
            width = height * aspectRatio;
        }
        return {clampTo(width, minWidth, maxWidth), clampTo(height, minHeight, maxHeight)};
    }

    /// 交换宽高。
    BoxConstraints flipped() const { return {minHeight, maxHeight, minWidth, maxWidth}; }
    /// 仅宽度约束。
    BoxConstraints widthConstraints() const { return {minWidth, maxWidth, 0.0, kInfinity}; }
    /// 仅高度约束。
    BoxConstraints heightConstraints() const { return {0.0, kInfinity, minHeight, maxHeight}; }

    bool operator==(const BoxConstraints& o) const {
        return minWidth == o.minWidth && maxWidth == o.maxWidth &&
               minHeight == o.minHeight && maxHeight == o.maxHeight;
    }
    bool operator!=(const BoxConstraints& o) const { return !(*this == o); }
};

/// Edge insets for margin and padding (left/top/right/bottom).
struct EdgeInsets {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;

    // 同 Size/Color：`EdgeInsets(l, t, r, b)` 要真 ctor（clang 15 没有 C++20 的
    // 带括号聚合初始化）；本仓的 `EdgeInsets{...}` 只有 0 参和 4 参两种。
    constexpr EdgeInsets() = default;
    constexpr EdgeInsets(double l, double t, double r, double b)
        : left(l), top(t), right(r), bottom(b) {}

    static constexpr EdgeInsets all(double value) { return {value, value, value, value}; }
    static constexpr EdgeInsets symmetric(double horizontal, double vertical) {
        return {horizontal, vertical, horizontal, vertical};
    }
    static constexpr EdgeInsets only(double left = 0, double top = 0, double right = 0, double bottom = 0) {
        return {left, top, right, bottom};
    }

    double horizontal() const { return left + right; }
    double vertical() const { return top + bottom; }

    bool operator==(const EdgeInsets& o) const {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
    bool operator!=(const EdgeInsets& o) const { return !(*this == o); }
};

inline BoxConstraints BoxConstraints::deflate(const EdgeInsets& e) const {
    return deflate(e.horizontal(), e.vertical());
}

struct Color {
    double r = 0, g = 0, b = 0, a = 1.0;

    /// 同 `Size`：`Color(r, g, b, a)` 的带括号写法要真 ctor，聚合初始化那套 C++20
    /// 规则在 OHOS NDK 的 clang 15 上不存在。
    constexpr Color() = default;
    constexpr Color(double r_, double g_, double b_, double a_ = 1.0)
        : r(r_), g(g_), b(b_), a(a_) {}

    static constexpr Color fromRGB(double r, double g, double b) { return {r, g, b, 1.0}; }
    static constexpr Color fromRGBA(double r, double g, double b, double a) { return {r, g, b, a}; }

    bool operator==(const Color& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Color& o) const { return !(*this == o); }
    Color withOpacity(double fill_alpha) const {
        return {r, g, b, fill_alpha};
    }
};

// ── Alignment ───────────────────────────────────────────────────────────────

/// 归一化对齐。上游的 `Alignment`（painting/alignment.dart）。
///
/// x / y ∈ [-1, 1]：(0, 0) 是中心，(-1, -1) 左上，(1, 1) 右下。
struct Alignment {
    double x = 0.0;
    double y = 0.0;

    constexpr Alignment() = default;
    constexpr Alignment(double x_, double y_) : x(x_), y(y_) {}

    static constexpr Alignment topLeft() { return {-1.0, -1.0}; }
    static constexpr Alignment topCenter() { return {0.0, -1.0}; }
    static constexpr Alignment topRight() { return {1.0, -1.0}; }
    static constexpr Alignment centerLeft() { return {-1.0, 0.0}; }
    static constexpr Alignment center() { return {0.0, 0.0}; }
    static constexpr Alignment centerRight() { return {1.0, 0.0}; }
    static constexpr Alignment bottomLeft() { return {-1.0, 1.0}; }
    static constexpr Alignment bottomCenter() { return {0.0, 1.0}; }
    static constexpr Alignment bottomRight() { return {1.0, 1.0}; }

    /// 线性插值（上游的 `Alignment.lerp`）。
    static Alignment lerp(const Alignment& a, const Alignment& b, double t) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }

    /// 把 `size` 大小的矩形按本对齐放进 `rect` 内（上游的 `inscribe`）。
    Rect inscribe(const Size& size, const Rect& rect) const {
        const double halfWidthDelta = (rect.width - size.width) / 2.0;
        const double halfHeightDelta = (rect.height - size.height) / 2.0;
        return {rect.x + halfWidthDelta + x * halfWidthDelta,
                rect.y + halfHeightDelta + y * halfHeightDelta, size.width, size.height};
    }

    /// 在 `other` 描述的尺寸内定位（上游的 `alongOffset`）。
    Offset alongOffset(const Offset& other) const {
        const double centerX = other.dx / 2.0;
        const double centerY = other.dy / 2.0;
        return {centerX + x * centerX, centerY + y * centerY};
    }

    /// 在 `other` 尺寸内定位（上游的 `alongSize`）。
    Offset alongSize(const Size& other) const {
        const double centerX = other.width / 2.0;
        const double centerY = other.height / 2.0;
        return {centerX + x * centerX, centerY + y * centerY};
    }

    bool operator==(const Alignment& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Alignment& o) const { return !(*this == o); }
};

// ── BoxFit ──────────────────────────────────────────────────────────────────

/// 缩放适配模式。上游的 `BoxFit`（painting/box_fit.dart）。
enum class BoxFit {
    Fill,       ///< 拉伸填满（不保持比例）。
    Contain,    ///< 等比缩放至完整可见。
    Cover,      ///< 等比缩放至铺满（可能裁切）。
    FitWidth,   ///< 以宽度为准等比缩放。
    FitHeight,  ///< 以高度为准等比缩放。
    None,       ///< 不缩放（按输入输出取交集裁切）。
    ScaleDown,  ///< 只在输入大于输出时按 Contain 缩小。
};

/// 一次 fit 计算的结果。上游的 `FittedSizes`。
struct FittedSizes {
    Size source;       ///< 从输入尺寸中取出的源区域大小。
    Size destination;  ///< 画到输出尺寸中的目标区域大小。
};

/// 上游的 `applyBoxFit`（painting/box_fit.dart:63-128）。
///
/// 输入或输出任一边 ≤ 0 时返回零尺寸（上游 同样短路）。
inline FittedSizes applyBoxFit(BoxFit fit, const Size& input, const Size& output) {
    if (input.width <= 0.0 || input.height <= 0.0 || output.width <= 0.0 || output.height <= 0.0)
        return {Size(), Size()};

    Size source = input;
    Size destination = output;
    switch (fit) {
        case BoxFit::Fill:
            source = input;
            destination = output;
            break;
        case BoxFit::Contain:
            source = input;
            if (output.width / output.height > source.width / source.height)
                destination = {source.width * output.height / source.height, output.height};
            else
                destination = {output.width, source.height * output.width / source.width};
            break;
        case BoxFit::Cover:
            if (output.width / output.height > input.width / input.height)
                source = {input.width, input.width * output.height / output.width};
            else
                source = {input.height * output.width / output.height, input.height};
            destination = output;
            break;
        case BoxFit::FitWidth:
            source = {input.width, input.width * output.height / output.width};
            destination = output;
            break;
        case BoxFit::FitHeight:
            source = {input.height * output.width / output.height, input.height};
            destination = output;
            break;
        case BoxFit::None:
            source = {std::min(input.width, output.width), std::min(input.height, output.height)};
            destination = source;
            break;
        case BoxFit::ScaleDown:
            source = input;
            destination = input;
            {
                const double aspectRatio = input.width / input.height;
                if (destination.height > output.height)
                    destination = {output.height * aspectRatio, output.height};
                if (destination.width > output.width)
                    destination = {output.width, output.width / aspectRatio};
            }
            break;
    }
    return {source, destination};
}

}  // namespace cui_core
