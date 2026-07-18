#pragma once
/// @file widgets/media.h
/// @brief 上游的 `MediaQuery` / `MediaQueryData` / `SafeArea` 移植。
///
/// 对应 上游源码：
///   上游 widgets/media_query.dart
///
/// `MediaQuery` 是 InheritedWidget：子树通过 `MediaQuery.of(context)` 读取
/// 设备信息（尺寸 / 安全区内边距 / 方向等），并在其变化时重建依赖者。
/// `SafeArea` 是 StatelessWidget：按 `MediaQuery` 提供的安全区 padding
/// 给 child 包一层 `Padding`，避免内容被刘海 / 系统栏遮挡。

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/framework.h"

namespace cui {

/// 设备方向。上游的 `Orientation`。
enum class Orientation { Portrait, Landscape };

/// 设备媒体信息。上游的 `MediaQueryData`（media_query.dart）。
///
/// 本移植保留最常用的字段；其余以 0 / 默认值填充（headless 布局测试足够）。
struct MediaQueryData {
    Size size;                                 ///< 逻辑像素尺寸。
    EdgeInsets padding;                        ///< 安全区内边距（刘海 / 系统栏）。
    EdgeInsets viewInsets;                     ///< 被系统 UI 完全遮挡的部分。
    EdgeInsets viewPadding;                    ///< 安全区 + 可能被遮挡部分。
    Orientation orientation = Orientation::Portrait;
    double devicePixelRatio = 1.0;

    bool operator==(const MediaQueryData& o) const {
        return size == o.size && padding == o.padding && viewInsets == o.viewInsets &&
               viewPadding == o.viewPadding && orientation == o.orientation &&
               devicePixelRatio == o.devicePixelRatio;
    }
    bool operator!=(const MediaQueryData& o) const { return !(*this == o); }
};

/// 媒体查询。上游的 `MediaQuery`（media_query.dart）。
///
/// 子树通过 `MediaQuery::of(context)` 读取并订阅 `data_`。
class MediaQuery : public InheritedWidget<MediaQueryData> {
public:
    MediaQuery(MediaQueryData data, SharedPtr<Widget> child, KeyPtr key = nullptr)
        : InheritedWidget<MediaQueryData>(key, std::move(child)), data_(data) {}

    SharedPtr<const MediaQueryData> data() const override {
        return cui::make_shared<MediaQueryData>(data_);
    }

    /// 读取最近的 MediaQuery 并登记依赖（上游的 `of`）。
    static const MediaQueryData& of(BuildContext& context) {
        static const MediaQueryData kDefault;
        auto* w = context.dependOnInheritedWidgetOfExactType<MediaQuery>();
        return w ? w->data_ : kDefault;
    }

private:
    MediaQueryData data_;
};

/// 安全区。上游的 `SafeArea`（media_query.dart）。
///
/// 按 `MediaQuery` 的安全区 padding，仅对 `edges` 中选中的边包 `Padding`。
class SafeArea : public StatelessWidget {
public:
    explicit SafeArea(SharedPtr<Widget> child,
                      bool top = true, bool bottom = true,
                      bool left = true, bool right = true, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          child_(std::move(child)),
          edges_{left, top, right, bottom} {}

    SharedPtr<Widget> build(BuildContext& context) override {
        const MediaQueryData& mq = MediaQuery::of(context);
        const EdgeInsets& p = mq.padding;
        EdgeInsets insets{
            edges_[0] ? p.left : 0.0,
            edges_[1] ? p.top : 0.0,
            edges_[2] ? p.right : 0.0,
            edges_[3] ? p.bottom : 0.0,
        };
        return cui::make_shared<Padding>(insets, child_);
    }

private:
    SharedPtr<Widget> child_;
    bool edges_[4] = {true, true, true, true};  ///< left, top, right, bottom
};

}  // namespace cui_core
