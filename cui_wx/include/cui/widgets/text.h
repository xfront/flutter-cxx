#pragma once
/// @file widgets/text.h
/// @brief 上游的 `Text` / `RichText` / `DefaultTextStyle` 移植。
///
/// 对应 上游源码：
///   上游 widgets/text.dart
///   上游 widgets/default_text_style.dart
///
/// 层级映射：
///   RichText → LeafRenderObjectWidget → RenderParagraph（叶子 RenderBox）
///   Text     → StatelessWidget，build 时读取 DefaultTextStyle 合并样式
///             后产出 RichText（与 上游的 Text.build 一致）
///   DefaultTextStyle → InheritedWidget<TextStyle>，子树通过 of(context) 订阅

#include <memory>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/text_style.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/widgets/basic.h"

namespace cui {

/// 富文本。上游的 `RichText`（text.dart）。
///
/// 持有 `TextSpan` 根（可含嵌套 children）；`RenderParagraph` 负责单行
/// 布局与逐 run 绘制。`textAlign` 暂保留为字段（本移植单行不换行，左对齐）。
class RichText : public LeafRenderObjectWidget {
public:
    explicit RichText(SharedPtr<TextSpan> text, KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), text_(std::move(text)) {
        if (!text_)
            text_ = cui::make_shared<TextSpan>();
    }

    const SharedPtr<TextSpan>& text() const { return text_; }
    void setText(SharedPtr<TextSpan> t) { text_ = std::move(t); }

    RenderObject* createRenderObject() override {
        return new RenderParagraph(text_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* para = dynamic_cast<RenderParagraph*>(ro);
        if (para)
            para->setText(text_);
    }

private:
    SharedPtr<TextSpan> text_;
};

/// 默认文本样式。上游的 `DefaultTextStyle`（default_text_style.dart）。
///
/// InheritedWidget：子树通过 `DefaultTextStyle::of(context)` 读取并订阅；
/// 数据（`style_`）变化时仅重建真正依赖它的 `Text`。
class DefaultTextStyle : public InheritedWidget<TextStyle> {
public:
    DefaultTextStyle(TextStyle style, SharedPtr<Widget> child, KeyPtr key = nullptr)
        : InheritedWidget<TextStyle>(key, std::move(child)), style_(style) {}

    const TextStyle& style() const { return style_; }

    SharedPtr<const TextStyle> data() const override {
        return cui::make_shared<TextStyle>(style_);
    }

    /// 读取最近的 DefaultTextStyle 并登记依赖（上游的 `of`）。
    static const TextStyle& of(BuildContext& context) {
        static const TextStyle kDefault;
        auto* w = context.dependOnInheritedWidgetOfExactType<DefaultTextStyle>();
        return w ? w->style() : kDefault;
    }

private:
    TextStyle style_;
};

/// 文本。上游的 `Text`（text.dart）的单行版。
///
/// `build()` 读取 `DefaultTextStyle::of(context)` 与自身 `style_` 合并，
/// 产出 `RichText(TextSpan(data, merged))` —— 与 上游 把 `Text` 实现为
/// 包 `RichText` 的 `StatelessWidget` 完全一致。
class Text : public StatelessWidget {
public:
    explicit Text(std::string data, TextStyle style = TextStyle::unset(), KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), style_(style) {}

    const std::string& data() const { return data_; }
    void setData(const std::string& d) { data_ = d; }

    const TextStyle& style() const { return style_; }
    void setStyle(const TextStyle& s) { style_ = s; }

    SharedPtr<Widget> build(BuildContext& context) override {
        TextStyle merged = DefaultTextStyle::of(context);
        merged.mergeFrom(style_);
        if (data_.find('\n') == std::string::npos) {
            auto span = cui::make_shared<TextSpan>(data_, merged);
            return cui::make_shared<RichText>(std::move(span));
        }
        // 多行文本：RenderParagraph 只做单行排版（'\n' 会被当成缺字形 → 豆腐块），
        // 这里按行拆成多个段落竖排，效果等同 上游的多行 Text。
        std::vector<SharedPtr<Widget>> lines;
        std::string cur;
        for (char ch : data_) {
            if (ch == '\n') {
                lines.push_back(cui::make_shared<RichText>(
                    cui::make_shared<TextSpan>(cur, merged)));
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        lines.push_back(
            cui::make_shared<RichText>(cui::make_shared<TextSpan>(cur, merged)));
        auto col = cui::make_shared<Column>(std::move(lines));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    std::string data_;
    TextStyle style_;
};

}  // namespace cui_core
