#pragma once
/// @file widgets/icons.h
/// @brief Icon（矢量图标）与 IconButton。
///
/// 对应 上游源码：
///   - widgets/icon.dart 的 `Icon`（图标字体 → 这里换成程序化矢量路径，
///     见 rendering/render_icon.h 的 IconCode/RenderIcon）；
///   - material/icon_button.dart 的 `IconButton`。
///
/// IconButton 约定：
///   - onPressed 为 null 时禁用（图标置灰、不响应点击）；
///   - 点击区域为 (iconSize + 16)²，与 上游的 48dp 逻辑近似；
///   - 可选 tooltip 包裹（依赖 tooltip.h）。

#include <functional>
#include <memory>
#include <string>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_icon.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/tooltip.h"

namespace cui {

/// 矢量图标。上游的 `Icon`（icon.dart）。
class Icon : public LeafRenderObjectWidget {
public:
    explicit Icon(IconCode code, double size = 24.0,
                  Color color = Color::fromRGBA(0, 0, 0, 0.87), KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), code_(code), size_(size),
          color_(color) {}

    IconCode code() const { return code_; }
    double size() const { return size_; }
    Color color() const { return color_; }

    RenderObject* createRenderObject() override {
        return new RenderIcon(code_, size_, color_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderIcon*>(ro);
        if (!r)
            return;
        r->setCode(code_);
        r->setIconSize(size_);
        r->setColor(color_);
    }

    std::string toStringShort() const override { return "Icon"; }

private:
    IconCode code_;
    double size_;
    Color color_;
};

/// 图标按钮。上游的 `IconButton`（icon_button.dart）。
class IconButton : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit IconButton(IconCode icon, VoidCallback onPressed = {},
                        double iconSize = 24.0,
                        Color color = Color::fromRGBA(0, 0, 0, 0.87),
                        Color disabledColor = Color::fromRGBA(0, 0, 0, 0.38),
                        std::string tooltip = {}, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), icon_(icon), onPressed_(std::move(onPressed)),
          iconSize_(iconSize), color_(color), disabledColor_(disabledColor),
          tooltip_(std::move(tooltip)) {}

    const VoidCallback& onPressed() const { return onPressed_; }
    IconCode code() const { return icon_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        bool enabled = static_cast<bool>(onPressed_);
        auto ic = cui::make_shared<Icon>(icon_, iconSize_, enabled ? color_
                                                                   : disabledColor_);
        auto box = cui::make_shared<SizedBox>(iconSize_ + 16.0, iconSize_ + 16.0,
                                              cui::make_shared<Center>(ic));
        SharedPtr<Widget> w = cui::make_shared<GestureDetector>(box, onPressed_);
        if (!tooltip_.empty())
            w = cui::make_shared<Tooltip>(tooltip_, w);
        return w;
    }

    std::string toStringShort() const override { return "IconButton"; }

private:
    IconCode icon_;
    VoidCallback onPressed_;
    double iconSize_;
    Color color_, disabledColor_;
    std::string tooltip_;
};

}  // namespace cui_core
