#pragma once
/// @file widgets/dialog.h
/// @brief 模态对话框（上游的 material 对话框子集）。
///
/// 对应 上游源码：
///   - material/dialog.dart 的 `AlertDialog` / `SimpleDialog` /
///     `SimpleDialogOption`
///   - material/dialog.dart 的 `DialogRoute`（TransitionsRoute 子集）
///   - showDialog（route.dart 的 `showGeneralDialog` 简化）
///
/// 移植约定：
///   - DialogRoute 是不透明度 0 的 Overlay 条目（opaque=false，不遮挡
///     下方路由的构建）：内容 = 全屏 Stack[ barrier(Positioned.fill 半透明
///     遮罩 + 点击关闭) + Center(dialog) ]；
///   - barrier 点击 → navigator->pop(result)（上游的
///     barrierDismissible；false 时不挂点击手势）；
///   - 转场动画省略（与 Navigator 层约定一致，push 后立即在场）。

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/theme.h"
#include "cui/framework/framework.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/pages.h"  // PageRouteBuilder：showModalBottomSheet
#include "cui/widgets/text.h"
#include "cui/widgets/transition.h"  // SlideTransition / CurvedAnimation

namespace cui {

// ── AlertDialog ────────────────────────────────────────────────────────────

/// 提示对话框。上游的 `AlertDialog`（dialog.dart）。
///
/// 布局：Container(padding:24, 白底圆角 elevation) → Column[
///   title(加粗 16) · 12 · content? · 12 · actions 行(End 对齐) ]。
class AlertDialog : public StatelessWidget {
public:
    explicit AlertDialog(std::string title = {}, SharedPtr<Widget> content = nullptr,
                         std::vector<SharedPtr<Widget>> actions = {},
                         KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)),
          content_(std::move(content)), actions_(std::move(actions)) {}

    const std::string& title() const { return title_; }
    const SharedPtr<Widget>& content() const { return content_; }
    const std::vector<SharedPtr<Widget>>& actions() const { return actions_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        std::vector<SharedPtr<Widget>> col;
        if (!title_.empty()) {
            TextStyle ts = TextStyle::unset();
            ts.fontSize = 16.0;
            ts.fontWeight = 700.0;
            col.push_back(cui::make_shared<Text>(title_, ts));
            col.push_back(cui::make_shared<SizedBox>(0.0, 12.0));
        }
        if (content_) {
            col.push_back(content_);
            col.push_back(cui::make_shared<SizedBox>(0.0, 12.0));
        }
        if (!actions_.empty()) {
            auto row = cui::make_shared<Row>(actions_);
            row->setMainAxisAlignment(MainAxisAlignment::End);
            row->setMainAxisSize(MainAxisSize::Min);
            col.push_back(row);
        }
        auto inner = cui::make_shared<Column>(std::move(col));
        inner->setMainAxisSize(MainAxisSize::Min);
        auto c = cui::make_shared<Container>(inner);
        c->padding(EdgeInsets::all(24.0))
            .decoration(BoxDecoration{theme().surface, 8.0, 0.0,
                                      Color{0, 0, 0, 0}, 24.0,
                                      Color::fromRGBA(0, 0, 0, 0.35)});
        return c;
    }

    std::string toStringShort() const override { return "AlertDialog"; }

private:
    std::string title_;
    SharedPtr<Widget> content_;
    std::vector<SharedPtr<Widget>> actions_;
};

// ── SimpleDialog ───────────────────────────────────────────────────────────

/// SimpleDialog 的一个选项。上游的 `SimpleDialogOption`。
struct SimpleDialogOption {
    std::string label;
    int value = 0;
};

/// 选择对话框。上游的 `SimpleDialog`（dialog.dart）。
///
/// 选项列表：每项一行（padding 16·12，hover 语义省略），点击触发
/// onSelected(value)。onSelected 由宿主注入（Widget 层不可变回调）。
class SimpleDialog : public StatelessWidget {
public:
    using OptionCallback = std::function<void(int)>;

    explicit SimpleDialog(std::string title = {},
                          std::vector<SimpleDialogOption> options = {},
                          OptionCallback onSelected = {}, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)),
          options_(std::move(options)), onSelected_(std::move(onSelected)) {}

    const std::string& title() const { return title_; }
    const std::vector<SimpleDialogOption>& options() const { return options_; }
    const OptionCallback& onSelected() const { return onSelected_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        std::vector<SharedPtr<Widget>> col;
        if (!title_.empty()) {
            TextStyle ts = TextStyle::unset();
            ts.fontSize = 16.0;
            ts.fontWeight = 700.0;
            auto t = cui::make_shared<Container>(
                cui::make_shared<Text>(title_, ts));
            t->padding(EdgeInsets::only(16.0, 16.0, 16.0, 8.0));
            col.push_back(t);
        }
        for (const auto& opt : options_) {
            auto* self = this;
            int value = opt.value;
            std::string label = opt.label;
            auto item = cui::make_shared<Container>(cui::make_shared<Text>(label));
            item->padding(EdgeInsets::symmetric(12.0, 16.0));
            col.push_back(cui::make_shared<GestureDetector>(
                item, [self, value]() {
                    if (self->onSelected_)
                        self->onSelected_(value);
                }));
        }
        auto inner = cui::make_shared<Column>(std::move(col));
        inner->setMainAxisSize(MainAxisSize::Min);
        auto c = cui::make_shared<Container>(inner);
        c->padding(EdgeInsets::symmetric(0.0, 8.0))
            .decoration(BoxDecoration{theme().surface, 8.0, 0.0,
                                      Color{0, 0, 0, 0}, 24.0,
                                      Color::fromRGBA(0, 0, 0, 0.35)});
        return c;
    }

    std::string toStringShort() const override { return "SimpleDialog"; }

private:
    std::string title_;
    std::vector<SimpleDialogOption> options_;
    OptionCallback onSelected_;
};

// ── DialogRoute ────────────────────────────────────────────────────────────

/// 模态对话框路由。上游的 `DialogRoute`（dialog.dart）。
///
/// install 填一个 opaque=false 的 OverlayEntry（半透明 barrier + 居中
/// dialog）。barrierDismissible 时 barrier 点击 → navigator->pop(result)。
class DialogRoute : public Route {
public:
    explicit DialogRoute(SharedPtr<Widget> child,
                         bool barrierDismissible = true,
                         Color barrierColor = Color::fromRGBA(0, 0, 0, 0.4),
                         SharedPtr<const RouteSettings> settings = nullptr)
        : Route(std::move(settings)), child_(std::move(child)),
          barrierDismissible_(barrierDismissible), barrierColor_(barrierColor) {}

    const SharedPtr<Widget>& child() const { return child_; }
    bool barrierDismissible() const { return barrierDismissible_; }
    Color barrierColor() const { return barrierColor_; }

    void install() override {
        auto* self = this;
        overlayEntries_.push_back(cui::make_shared<OverlayEntry>(
            [self](BuildContext& context) {
                (void)context;
                return self->buildOverlay();
            },
            /*opaque=*/false, /*maintainState=*/true));
    }

private:
    SharedPtr<Widget> buildOverlay() {
        std::vector<SharedPtr<Widget>> stack;
        if (barrierDismissible_) {
            auto barrierBox = cui::make_shared<Container>();
            barrierBox->decoration(BoxDecoration{barrierColor_});
            auto barrier = cui::make_shared<GestureDetector>(
                barrierBox, [this]() { navigator()->pop(); });
            auto fill = cui::make_shared<Positioned>(barrier);
            fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
            stack.push_back(fill);
        } else {
            auto barrierBox = cui::make_shared<Container>();
            barrierBox->decoration(BoxDecoration{barrierColor_});
            auto fill = cui::make_shared<Positioned>(barrierBox);
            fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
            stack.push_back(fill);
        }
        stack.push_back(cui::make_shared<Center>(child_));
        return cui::make_shared<Stack>(std::move(stack));
    }

    SharedPtr<Widget> child_;
    bool barrierDismissible_;
    Color barrierColor_;
};

// ── showDialog ─────────────────────────────────────────────────────────────

/// 推入一个对话框路由。上游的 `showDialog`（dialog.dart）。
///
/// 返回压入的 DialogRoute（结果经 `Route::whenPopped` 回调获取——
/// 上游的 `showDialog<T>().then` 的 C++ 表达）。
inline SharedPtr<DialogRoute> showDialog(
    BuildContext& context, SharedPtr<Widget> dialog,
    bool barrierDismissible = true,
    Color barrierColor = Color::fromRGBA(0, 0, 0, 0.4)) {
    auto* nav = NavigatorState::of(context);
    auto route = cui::make_shared<DialogRoute>(std::move(dialog), barrierDismissible,
                                               barrierColor);
    nav->push(route);
    return route;
}

// ── showModalBottomSheet ───────────────────────────────────────────────────

/// 推入一个模态底部面板。上游的 `showModalBottomSheet`（material/bottom_sheet.dart）。
///
/// 上游的 `_ModalBottomSheetRoute` 由 `BottomSheet.createAnimationController`
/// （进场 250ms / 出场 200ms）驱动，面板本体靠私有渲染盒
/// `_BottomSheetLayoutWithSizeListener` 按动画值做**布局偏移**
/// `Offset(0, (1 - t) * 面板高)`，即「从屏幕下沿滑上来」。本仓没有那条渲染盒，
/// 改用 `SlideTransition`（平移绘制与命中，视觉效果与布局偏移等价），并且曲线
/// 施加在 page 里而不是 `buildTransitions` 里：转场件平移的是它自己那棵子树，
/// 罩在整屏 page 外面时 `Offset(0, 1)` 等于一整屏的位移，面板要到最后 1/6 才
/// 进视野。
///
/// 有意未接（上游同名函数的其余参数）：`enableDrag` 下滑关闭手势、
/// `isScrollControlled` 与 `scrollControlDisabledMaxHeightRatio`、`useSafeArea`、
/// `backgroundColor` / `elevation` / `shape` / `clipBehavior` / `constraints`
/// （面板外观交给 `BottomSheet` 控件自己）、`sheetAnimationStyle`、`anchorPoint`
/// （折叠屏）、`showDragHandle`。
inline SharedPtr<PageRouteBuilder> showModalBottomSheet(
    BuildContext& context, SharedPtr<Widget> sheet,
    bool barrierDismissible = true,
    Color barrierColor = Color::fromRGBA(0.0, 0.0, 0.0, 0.54),
    double transitionMs = 250.0, double reverseTransitionMs = 200.0) {
    auto* nav = NavigatorState::of(context);
    auto route = cui::make_shared<PageRouteBuilder>(
        [sheet](BuildContext&, SharedPtr<Animation<double>> animation,
                SharedPtr<Animation<double>>) {
            // 上游的 _kModalBottomSheetCurve = Easing.legacyDecelerate
            // （cubic(0, 0, 0, 1)）；本仓 Easing 枚举没有 decelerate，
            // 取 EaseOutCubic 近似（同为「起步快、收尾缓」）。
            auto curved = cui::make_shared<CurvedAnimation>(std::move(animation),
                                                            Easing::EaseOutCubic);
            auto slide = cui::make_shared<SlideTransition>(
                std::move(curved), Offset(0.0, 1.0), Offset(0.0, 0.0), sheet);
            return cui::make_shared<Align>(0.0, 1.0, std::move(slide));
        },
        nullptr, transitionMs, reverseTransitionMs,
        /*opaque=*/false, /*maintainState=*/true, std::optional<Color>(barrierColor),
        barrierDismissible);
    nav->push(route);
    return route;
}

}  // namespace cui_core
