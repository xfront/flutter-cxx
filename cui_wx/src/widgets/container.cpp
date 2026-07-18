/// @file container.cpp
/// @brief 上游 `Container`（container.dart）的组合实现。
///
/// Container 是 StatelessWidget：build() 里按 上游的顺序把配置翻译为
/// 一棵由 Align / Padding / DecoratedBox / ConstrainedBox 组成的子树。

#include "cui/widgets/container.h"

#include "../../../cui_core/include/cui/utils/shared_ptr.h"

namespace cui {

std::optional<BoxConstraints> Container::mergedConstraints() const {
    if (!width_ && !height_ && !constraints_)
        return std::nullopt;

    // 以显式 constraints 为底（缺省无界），再让 width/height 收紧对应轴。
    BoxConstraints base = constraints_.value_or(BoxConstraints::unbounded());
    return base.tighten(width_, height_);
}

SharedPtr<Widget> Container::build(BuildContext& context) {
    (void)context;

    SharedPtr<Widget> current = child_;

    // 1. 对齐（最内层，紧贴子节点）。
    if (alignment_ && current)
        current = cui::make_shared<Align>(alignment_->first, alignment_->second, current);

    // 2. 内边距（在装饰之内）。
    if (padding_)
        current = cui::make_shared<Padding>(*padding_, current);

    // 3. 装饰（绘制在 padding 之外）。
    if (decoration_ && !decoration_->isEmpty())
        current = cui::make_shared<DecoratedBox>(*decoration_, current);

    // 4. 附加约束（width/height + constraints）。
    if (auto c = mergedConstraints())
        current = cui::make_shared<ConstrainedBox>(*c, current);

    // 5. 外边距（最外层）。
    if (margin_)
        current = cui::make_shared<Padding>(*margin_, current);

    return current;
}

}  // namespace cui_core
