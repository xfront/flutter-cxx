#pragma once
/// @file widgets/scroll_physics.h
/// @brief ScrollPhysics 体系（上游的 scrolling 物理子集）。
///
/// 对应 上游源码：
///   - ClampingScrollPhysics（Android 默认：越界 clamp）
///   - BouncingScrollPhysics（iOS 默认：越界阻尼，回弹）
///   - NeverScrollableScrollPhysics（禁止用户滚动）
///
/// 简化说明：不建 ScrollActivity/ScrollPosition 体系，物理只作用在
/// 「用户输入的目标偏移 → 实际偏移」一步（Scrollable 的 pan/wheel 处理）；
/// 松手回弹由 Scrollable 在 panEnd 用 jumpTo 的边界 clamp 一步到位
/// （上游 是弹簧动画，这里已知简化）。

#include <algorithm>

namespace cui {

/// 滚动物理基类。上游的 `ScrollPhysics`。
class ScrollPhysics {
public:
    virtual ~ScrollPhysics() = default;

    /// 把用户输入的目标偏移换算为实际偏移。
    /// @param target   本次输入直接投影出的目标偏移
    /// @param current  当前偏移
    /// @param min      最小偏移（本移植恒为 0）
    /// @param max      maxScrollExtent
    virtual double applyTo(double target, double current, double min,
                           double max) const {
        (void)current;
        return std::clamp(target, min, max);
    }

    /// 上游的 `ScrollPhysics.shouldAcceptUserOffset`：这份用户输入归不归本物理
    /// 模型管。控件层的滚轮先问它再决定「认领不认领」⇒ NeverScrollable 的内层
    /// 不会把增量吃掉，嵌套时外层照样能滚。
    /// 偏离：上游入参是 `ScrollMetrics`（子类可按位置否决），本仓没有
    /// ScrollPosition，故无参。
    virtual bool shouldAcceptUserOffset() const { return true; }
};

/// Android 风格：越界一律钳制（本移植的历史默认行为）。
class ClampingScrollPhysics : public ScrollPhysics {};

/// iOS 风格：越界阻尼（超出边界的位移打对折），松手回弹到边界。
class BouncingScrollPhysics : public ScrollPhysics {
public:
    double applyTo(double target, double current, double min,
                   double max) const override {
        // 边界内：正常跟手，越界部分阻尼减半。
        if (current >= min && current <= max) {
            if (target > max)
                return max + (target - max) * 0.5;
            if (target < min)
                return min + (target - min) * 0.5;
            return target;
        }
        // 已在界外：界外区摩擦加大（delta 减半），拖回边界内后恢复正常。
        return current + (target - current) * 0.5;
    }
};

/// 禁止用户滚动（pan/wheel 不动；jumpTo 仍可用——对齐 上游语义）。
class NeverScrollableScrollPhysics : public ScrollPhysics {
public:
    double applyTo(double target, double current, double min,
                   double max) const override {
        (void)target;
        (void)min;
        (void)max;
        return current;
    }

    /// 上游 `_receivedPointerSignal` 的第一道闸：不接受用户滚动 ⇒ 控件层
    /// 既不滚也不认领，滚轮增量继续向祖先冒泡（嵌套时外层接手）。
    bool shouldAcceptUserOffset() const override { return false; }
};

}  // namespace cui_core
