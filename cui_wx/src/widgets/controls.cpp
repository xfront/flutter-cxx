// ── cui_widgets · controls 实现（从 controls.h 拆分）────────────────
// 本文件承载 controls.h 中各控件的非内联成员实现（createState / build /
// initState / dispose 等），头文件仅保留声明。
#include "cui/widgets/controls.h"

namespace cui {

SharedPtr<Widget> ZoomableState::build(BuildContext&) {
    auto* self = zm();
    auto inner = Transform::translate(Offset(dx_, dy_), Transform::scale(scale_, self->child()));

    auto area = cui::make_shared<Container>(inner);
    area->width(self->width())
            .height(self->height())
            .decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.03), 4.0, 1.0, Color::fromRGBA(0, 0, 0, 0.18)});

    GestureDetector::PointerCallback onDown = [this](const PointerEvent& ev) {
        startX_ = ev.globalPosition.dx;
        startY_ = ev.globalPosition.dy;
        anchorDx_ = dx_;
        anchorDy_ = dy_;
    };
    GestureDetector::PointerCallback onMove = [this](const PointerEvent& ev) {
        dx_ = anchorDx_ + (ev.globalPosition.dx - startX_);
        dy_ = anchorDy_ + (ev.globalPosition.dy - startY_);
        setState();
    };
    auto panArea = cui::make_shared<GestureDetector>(area, nullptr, nullptr, nullptr, onDown, onMove, nullptr);

    auto btn = [this](const std::string& label, std::function<void()> fn) {
        auto t = cui::make_shared<Text>(label, TextStyle().size(13.0).bold());
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(36.0).height(28.0).decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.06), 4.0});
        return cui::make_shared<GestureDetector>(c, std::move(fn));
    };
    auto zoomOut = btn("-", [this] {
        scale_ = std::max(0.5, scale_ - 0.25);
        setState();
    });
    auto zoomIn = btn("+", [this] {
        scale_ = std::min(3.0, scale_ + 0.25);
        setState();
    });
    auto reset = btn("1:1", [this] {
        scale_ = 1.0;
        dx_ = dy_ = 0.0;
        setState();
    });
    auto label = cui::make_shared<Text>("scale " + std::to_string(static_cast<int>(scale_ * 100)) + "%",
                                        TextStyle().size(11.5).fgColor(theme().textSecondary));
    auto labelBox = cui::make_shared<Container>(cui::make_shared<Center>(label));
    labelBox->padding(EdgeInsets::symmetric(0.0, 8.0));

    auto ctrl = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{zoomOut, zoomIn, reset, labelBox});
    ctrl->setMainAxisSize(MainAxisSize::Min);
    ctrl->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{panArea, cui::make_shared<SizedBox>(0.0, 6.0), ctrl});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

SharedPtr<Widget> DockPanelState::build(BuildContext&) {
    auto* self = dp();
    const Color kBorderLight = theme().borderLight;

    auto t = cui::make_shared<Text>(self->title(), TextStyle().size(13.0).bold().fgColor(theme().textPrimary));
    auto pin = cui::make_shared<Text>(pinned_ ? "◉" : "○", TextStyle().size(11.0).fgColor(theme().textSecondary));
    auto pinBox = cui::make_shared<Container>(cui::make_shared<Center>(pin));
    pinBox->width(20.0).height(20.0);
    auto fold = cui::make_shared<Text>(collapsed_ ? "▶" : "▼", TextStyle().size(10.0).fgColor(theme().textSecondary));
    auto foldBox = cui::make_shared<Container>(cui::make_shared<Center>(fold));
    foldBox->width(20.0).height(20.0);

    auto headRow = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{t, cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)),
                                           cui::make_shared<GestureDetector>(pinBox,
                                                                             [this] {
                                                                                 pinned_ = !pinned_;
                                                                                 setState();
                                                                             }),
                                           cui::make_shared<SizedBox>(4.0, 0.0),
                                           cui::make_shared<GestureDetector>(foldBox, [this] { toggleCollapse(); })});
    headRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto head = cui::make_shared<Container>(headRow);
    // 头部：浅 surface + 底部 1px 分隔线（干净的工具面板头）。
    head->height(32.0)
            .padding(EdgeInsets::symmetric(0.0, 10.0))
            .decoration(BoxDecoration{theme().surface, 0.0, 1.0, kBorderLight});

    std::vector<SharedPtr<Widget>> kids{head};
    if (collapse_ > 0.01 && self->body()) {
        auto bodyBox = cui::make_shared<Container>(self->body());
        bodyBox->padding(EdgeInsets::all(10.0));
        kids.push_back(cui::make_shared<Opacity>(collapse_, bodyBox));
    }

    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto box = cui::make_shared<Container>(col);
    box->width(self->width())
            .decoration(BoxDecoration{theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.16), 8.0,
                                      Color::fromRGBA(0, 0, 0, 0.18)});
    return box;
}

std::unique_ptr<State> DockPanel::createState() {
    return std::make_unique<DockPanelState>();
}

SharedPtr<Widget> MaterialBannerState::build(BuildContext&) {
    auto* self = bn();
    if (dismissed_)
        return cui::make_shared<SizedBox>(0.0, 0.0);

    // 对齐 legacy Banner（src/widgets/dialogs/banner.cpp）的几何：
    //   高 ≥52、消息 14px、右侧 24x24 关闭按钮；配色改由主题/入参决定，
    //   默认 surface + textPrimary（暗色主题下不再是浅琥珀底 + 深棕字）。
    const Color kBg = self->backgroundColor();
    const Color kFg = self->foregroundColor();
    const bool customFg = !(kFg == theme().textPrimary);
    const Color kAction = customFg ? kFg : theme().primary;

    std::vector<SharedPtr<Widget>> kids;
    if (!self->leading().empty()) {
        auto icon = cui::make_shared<Text>(self->leading(), TextStyle().size(13.0).bold().fgColor(kFg));
        auto iconBox = cui::make_shared<Container>(icon);
        iconBox->width(24.0).height(24.0).alignment(0.0, 0.0).decoration(BoxDecoration{theme().hoverOverlay, 12.0});
        kids.push_back(iconBox);
        kids.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
    }

    auto msg = cui::make_shared<Text>(self->message(), TextStyle().size(14.0).fgColor(kFg));
    std::vector<SharedPtr<Widget>> mid{msg};
    if (!self->actions().empty()) {
        std::vector<SharedPtr<Widget>> acts;
        for (const auto& a : self->actions()) {
            auto t = cui::make_shared<Text>(a.first, TextStyle().size(13.0).bold().fgColor(kAction));
            std::function<void()> cb = a.second;
            std::string text = a.first;
            acts.push_back(cui::make_shared<TextButton>(t, [this, cb, text] {
                lastAction_ = text;
                if (cb)
                    cb();
                setState();
            }));
            acts.push_back(cui::make_shared<SizedBox>(10.0, 0.0));
        }
        acts.pop_back();
        auto actRow = cui::make_shared<Row>(std::move(acts));
        actRow->setMainAxisSize(MainAxisSize::Min);
        actRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto actBox = cui::make_shared<Container>(actRow);
        actBox->padding(EdgeInsets(0.0, 6.0, 0.0, 0.0));
        mid.push_back(actBox);
    }
    if (!lastAction_.empty()) {
        auto hint = cui::make_shared<Text>("已执行：" + lastAction_, TextStyle().size(11.0).fgColor(kFg));
        mid.push_back(hint);
    }
    auto midCol = cui::make_shared<Column>(std::move(mid));
    midCol->setMainAxisSize(MainAxisSize::Min);
    midCol->setCrossAxisAlignment(CrossAxisAlignment::Start);
    kids.push_back(cui::make_shared<Expanded>(1, midCol));

    // 关闭按钮（legacy：24x24，命中区靠右 8px）。
    auto close = cui::make_shared<Text>("×", TextStyle().size(16.0).fgColor(kFg));
    auto closeBox = cui::make_shared<Container>(close);
    closeBox->width(24.0).height(24.0).alignment(0.0, 0.0).decoration(BoxDecoration{theme().hoverOverlay, 12.0});
    kids.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
    kids.push_back(cui::make_shared<GestureDetector>(closeBox, [this] {
        dismissed_ = true;
        setState();
        if (bn()->onDismiss())
            bn()->onDismiss()();
    }));

    auto row = cui::make_shared<Row>(std::move(kids));
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto box = cui::make_shared<Container>(row);
    // 上游 MaterialBanner 从不锁死整条高度：Column(min) + 操作条 minHeight 52，
    // 卡片高度随内容长。这里同口径 —— 只给 52 的下限，别再用 height() 塞紧约束
    // （消息 + 操作 + 「已执行」提示三行时，紧 52 会把内容顶出卡片外）。
    // 另：本仓 EdgeInsets::symmetric 的形参是 (horizontal, vertical)，与上游
    // `symmetric(vertical:, horizontal:)` 相反 —— 这里要的是左右 16、上下 6。
    box->width(self->width())
            .constraints(BoxConstraints(0.0, BoxConstraints::kInfinity, 52.0, BoxConstraints::kInfinity))
            .padding(EdgeInsets::symmetric(16.0, 6.0))
            .decoration(BoxDecoration{kBg, 4.0, 1.0, theme().border, 6.0, Color::fromRGBA(0, 0, 0, 0.06)});
    return box;
}

std::unique_ptr<State> MaterialBanner::createState() {
    return std::make_unique<MaterialBannerState>();
}

SharedPtr<Widget> RibbonState::build(BuildContext&) {
    auto* self = rb();
    const auto& tabs = self->tabs();

    // 配色与尺寸对齐 legacy（src/widgets/navigation/ribbon.cpp）：
    //   kTabH = 32（标签条）、kBodyH = 92（命令区）、大按钮 70x58、小按钮 80x22。
    const Color kPrimary = theme().primary;
    const Color kSurface = theme().surface;
    const Color kTextPrimary = theme().textPrimary;
    const Color kTextSecondary = theme().textSecondary;
    const Color kBorder = theme().border;

    // 玻璃模式：面全部换成半透明白（数值对齐壳里 StatusBar / 菜单面板的 alpha 口径），
    // 描边换成同族的高亮白；文字仍走主题色，因为底下的壁纸已被磨砂提亮。
    const bool glass = self->glass();
    auto glassFill = [glass](Color normal, double alpha) {
        return glass ? Color::fromRGBA(1.0, 1.0, 1.0, alpha) : normal;
    };
    const Color kStripBg = glassFill(theme().surfaceVariant, 0.28);
    const Color kBodyBg = glassFill(kSurface, 0.16);
    const Color kCardBg = glassFill(kSurface, 0.50);
    const Color kCardBorder = glassFill(kBorder, 0.70);
    const Color kHairline = glassFill(theme().divider, 0.45);

    // ── 选项卡条 ──────────────────────────────────────────────────────────────
    std::vector<SharedPtr<Widget>> strip;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        const bool active = (i == active_) && !collapsed_;
        auto label =
                cui::make_shared<Text>(tabs[i].name, TextStyle().size(13.0).fgColor(active ? kPrimary : kTextPrimary));
        std::vector<SharedPtr<Widget>> colKids{label};
        if (active) {
            // 激活标签底部的主色条（legacy：4px 高，左右各留 12px）。
            auto bar = cui::make_shared<Container>(cui::make_shared<SizedBox>(24.0, 3.0));
            bar->decoration(BoxDecoration{kPrimary, 1.5});
            colKids.push_back(cui::make_shared<SizedBox>(0.0, 4.0));
            colKids.push_back(bar);
        }
        auto tabCol = cui::make_shared<Column>(std::move(colKids));
        tabCol->setMainAxisSize(MainAxisSize::Min);
        tabCol->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(tabCol);
        box->padding(EdgeInsets::symmetric(6.0, 14.0))
                .decoration(BoxDecoration{active ? kCardBg : Color::fromRGBA(0, 0, 0, 0), 4.0, active ? 1.0 : 0.0,
                                          kCardBorder, active ? 8.0 : 0.0,
                                          active ? Color::fromRGBA(0, 0, 0, 0.10) : Color::fromRGBA(0, 0, 0, 0)});
        int idx = i;
        strip.push_back(cui::make_shared<GestureDetector>(box, [this, idx] {
            // 点击当前标签折叠/展开（legacy 是双击，这里单击更顺手）。
            if (active_ == idx)
                collapsed_ = !collapsed_;
            else {
                active_ = idx;
                collapsed_ = false;
            }
            setState();
            if (rb()->onTabChanged())
                rb()->onTabChanged()(active_);
        }));
    }

    // 折叠按钮：白色圆钮（legacy：半径 11 + 阴影）。
    auto chevGlyph = cui::make_shared<Text>(collapsed_ ? "▼" : "▲", TextStyle().size(8.0).fgColor(kTextSecondary));
    auto chevBox = cui::make_shared<Container>(chevGlyph);
    chevBox->width(22.0).height(22.0).alignment(0.0, 0.0).decoration(
            BoxDecoration{kCardBg, 11.0, 1.0, kCardBorder, 6.0, Color::fromRGBA(0, 0, 0, 0.12)});
    auto chevBtn = cui::make_shared<GestureDetector>(chevBox, [this] {
        collapsed_ = !collapsed_;
        setState();
    });

    auto tabsRow = cui::make_shared<Row>(std::move(strip));
    tabsRow->setMainAxisSize(MainAxisSize::Min);
    tabsRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto stripRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            tabsRow, cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)), chevBtn});
    stripRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto stripBox = cui::make_shared<Container>(stripRow);
    stripBox->padding(EdgeInsets::symmetric(4.0, 6.0)).decoration(BoxDecoration{kStripBg, 0.0, 1.0, kHairline});

    std::vector<SharedPtr<Widget>> col{stripBox};

    // ── 命令区 ────────────────────────────────────────────────────────────────
    if (!collapsed_ && active_ >= 0 && active_ < static_cast<int>(tabs.size())) {
        const auto& groups = tabs[active_].groups;
        auto mkButton = [this, &kCardBg, &kCardBorder, &kTextPrimary](const RibbonItem& item, bool large) {
            auto label =
                    cui::make_shared<Text>(item.label, TextStyle().size(large ? 12.5 : 12.0).fgColor(kTextPrimary));
            auto c = cui::make_shared<Container>(label);
            if (large) {
                // 垂直内边距留 0：固定高度 + 垂直内边距会让文字基线偏移（被裁切）。
                c->width(70.0).height(58.0).padding(EdgeInsets::symmetric(0.0, 6.0)).alignment(0.0, 0.0);
            } else {
                // 小按钮：只固定宽度，高度由文字 + 内边距决定
                //（固定高度 + 文字会让基线贴到下边框上）。
                c->width(80.0).padding(EdgeInsets::symmetric(3.0, 10.0)).alignment(-1.0, 0.0);
            }
            c->decoration(BoxDecoration{kCardBg, 4.0, 1.0, kCardBorder, 6.0, Color::fromRGBA(0, 0, 0, 0.06)});
            std::function<void()> cb = item.onTap;
            std::string text = item.label;
            return cui::static_pointer_cast<Widget>(cui::make_shared<GestureDetector>(c, [this, cb, text] {
                lastAction_ = text;
                if (cb)
                    cb();
                setState();
            }));
        };

        std::vector<SharedPtr<Widget>> groupWidgets;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            std::vector<SharedPtr<Widget>> slots;
            size_t i = 0;
            while (i < g.items.size()) {
                if (g.items[i].large) {
                    slots.push_back(mkButton(g.items[i], true));
                    ++i;
                } else {
                    // 小按钮：两枚一行（legacy 的 small button 布局）。
                    std::vector<SharedPtr<Widget>> pair{mkButton(g.items[i], false)};
                    if (i + 1 < g.items.size() && !g.items[i + 1].large) {
                        pair.push_back(mkButton(g.items[i + 1], false));
                        i += 2;
                    } else {
                        ++i;
                    }
                    auto pairCol = cui::make_shared<Column>(std::move(pair));
                    pairCol->setMainAxisSize(MainAxisSize::Min);
                    pairCol->setCrossAxisAlignment(CrossAxisAlignment::Start);
                    slots.push_back(pairCol);
                }
                slots.push_back(cui::make_shared<SizedBox>(4.0, 0.0));
            }
            if (!slots.empty())
                slots.pop_back();

            auto bodyRow = cui::make_shared<Row>(std::move(slots));
            bodyRow->setMainAxisSize(MainAxisSize::Min);
            bodyRow->setCrossAxisAlignment(CrossAxisAlignment::Start);

            // 组标题（居中放在按钮下方，legacy 11px secondary）。
            auto gTitle = cui::make_shared<Text>(g.title, TextStyle().size(11.0).fgColor(kTextSecondary));
            auto gCol = cui::make_shared<Column>(
                    std::vector<SharedPtr<Widget>>{bodyRow, cui::make_shared<SizedBox>(0.0, 8.0), gTitle});
            gCol->setMainAxisSize(MainAxisSize::Min);
            gCol->setCrossAxisAlignment(CrossAxisAlignment::Center);
            groupWidgets.push_back(gCol);

            // 组间分隔线（legacy：1px 竖线）。
            if (gi + 1 < groups.size()) {
                auto sep = cui::make_shared<Container>(cui::make_shared<SizedBox>(1.0, 62.0));
                sep->decoration(BoxDecoration{kHairline});
                groupWidgets.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
                groupWidgets.push_back(sep);
            }
        }

        auto groupsRow = cui::make_shared<Row>(std::move(groupWidgets));
        groupsRow->setMainAxisSize(MainAxisSize::Min);
        groupsRow->setCrossAxisAlignment(CrossAxisAlignment::Start);

        // 尾部挂件（如配置下拉）靠右放置，不挤占分组区。
        SharedPtr<Widget> bodyInner = groupsRow;
        if (auto tr = self->trailing()) {
            auto trRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                    cui::make_shared<Expanded>(1, groupsRow), cui::make_shared<SizedBox>(10.0, 0.0), tr});
            trRow->setCrossAxisAlignment(CrossAxisAlignment::Start);
            bodyInner = trRow;
        }
        auto bodyBox = cui::make_shared<Container>(bodyInner);
        bodyBox->padding(EdgeInsets::symmetric(8.0, 8.0)).decoration(BoxDecoration{kBodyBg, 0.0, 1.0, kHairline});
        col.push_back(bodyBox);
    }

    if (!lastAction_.empty()) {
        auto hint = cui::make_shared<Text>("最近点击：" + lastAction_,
                                           TextStyle().size(11.0).fgColor(theme().textSecondary));
        auto hintBox = cui::make_shared<Container>(hint);
        hintBox->padding(EdgeInsets(8.0, 0.0, 0.0, 4.0));
        col.push_back(hintBox);
    }

    auto root = cui::make_shared<Column>(std::move(col));
    root->setMainAxisSize(MainAxisSize::Min);
    root->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto wrap = cui::make_shared<Container>(root);
    // width <= 0 表示不约束宽度（由父级拉伸决定），便于放进窗口外壳顶部。
    if (self->width() > 0.0)
        wrap->width(self->width());
    return wrap;
}

std::unique_ptr<State> Ribbon::createState() {
    return std::make_unique<RibbonState>();
}

SharedPtr<Widget> PullToRefreshState::build(BuildContext&) {
    auto* self = pr();
    const double trigger = self->triggerExtent();

    std::vector<SharedPtr<Widget>> col;
    if (refreshing_ || pull_ > 0.0) {
        std::string label;
        if (refreshing_)
            label = "刷新中… " + std::to_string(static_cast<int>(holdProgress_ * 100)) + "%";
        else
            label = (pull_ >= trigger) ? "松手刷新" : "下拉刷新";
        auto hint = cui::make_shared<Text>(label, TextStyle().size(11.5).fgColor(theme().textSecondary));
        auto hintBox = cui::make_shared<Container>(cui::make_shared<Center>(hint));
        hintBox->height(refreshing_ ? 26.0 : std::max(18.0, pull_ * 0.5));
        col.push_back(hintBox);
    }
    col.push_back(self->child());

    auto column = cui::make_shared<Column>(std::move(col));
    column->setMainAxisSize(MainAxisSize::Min);
    column->setCrossAxisAlignment(CrossAxisAlignment::Start);

    GestureDetector::PointerCallback onDown = [this](const PointerEvent& e) { dragStartY_ = e.globalPosition.dy; };
    GestureDetector::PointerCallback onMove = [this, trigger](const PointerEvent& e) {
        if (refreshing_)
            return;
        double dy = e.globalPosition.dy - dragStartY_;
        pull_ = std::max(0.0, dy);
        if (pull_ > trigger * 2.0)
            pull_ = trigger * 2.0;
        setState();
    };
    GestureDetector::PointerCallback onUp = [this, trigger](const PointerEvent&) {
        if (refreshing_ || pull_ <= 0.0)
            return;
        if (pull_ >= trigger) {
            refreshing_ = true;
            holdStartMs_ = 0.0;  // ticker elpased 从 0 重新计时
            holdProgress_ = 0.0;
            if (ticker_ && !ticker_->isActive())
                ticker_->start();
            setState();
            if (pr()->onRefresh())
                pr()->onRefresh()();
        } else {
            pull_ = 0.0;
            setState();
        }
    };

    return cui::make_shared<GestureDetector>(column, nullptr, nullptr, nullptr, onDown, onMove, onUp);
}

std::unique_ptr<State> PullToRefresh::createState() {
    return std::make_unique<PullToRefreshState>();
}

std::unique_ptr<State> SpinBox::createState() {
    return std::make_unique<SpinBoxState>();
}

std::unique_ptr<State> ButtonGroup::createState() {
    return std::make_unique<ButtonGroupState>();
}

SharedPtr<Widget> TitleBarState::build(BuildContext&) {
    auto* self = static_cast<TitleBar*>(widget());
    auto* st = this;
    // 对齐 legacy TitleBar（src/widgets/navigation/title_bar.cpp）：
    //   surface 底、0.5px 下边框、16px 图标、13px 标题、
    //   右侧 46px 宽的窗口按钮（"—" / "□" / "×"）。新增 hover 高亮；
    //   关闭键（“×”）hover 时变红（桌面端通用行为）。
    const Color kSurface = theme().surface;
    const Color kTextPrimary = theme().textPrimary;
    const Color kTextSecondary = theme().textSecondary;

    std::vector<SharedPtr<Widget>> kids;
    if (self->icon() != IconCode::None) {
        auto ic = cui::make_shared<Icon>(self->icon(), 16.0, kTextPrimary);
        auto icBox = cui::make_shared<Container>(ic);
        icBox->width(16.0).height(16.0).alignment(0.0, 0.0);
        kids.push_back(icBox);
        kids.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
    }
    kids.push_back(cui::make_shared<Text>(self->title(), TextStyle().size(13.0).fgColor(kTextPrimary)));

    if (!self->actions().empty()) {
        std::vector<SharedPtr<Widget>> tail;
        int n = static_cast<int>(self->actions().size());
        for (int i = 0; i < n; ++i) {
            const std::string& a = self->actions()[i];
            bool isClose = (a == "×");
            bool hov = (st->hovered_ == i);
            bool closeHov = hov && isClose;
            Color bg = closeHov ? Color::fromRGB(0.90, 0.30, 0.30) :
                                  (hov ? Color::fromRGBA(0, 0, 0, 0.08) : Color::fromRGBA(0, 0, 0, 0.04));
            Color fg = closeHov ? Color::fromRGB(1, 1, 1) : kTextSecondary;
            auto g = cui::make_shared<Text>(a, TextStyle().size(14.0).fgColor(fg));
            auto c = cui::make_shared<Container>(cui::make_shared<Center>(g));
            c->width(46.0).height(32.0).alignment(0.0, 0.0).decoration(BoxDecoration{bg, 0.0});
            int idx = i;
            auto cell = cui::make_shared<Listener>(
                    nullptr, nullptr, nullptr,
                    [st, idx](const PointerEvent&) {
                        if (st->hovered_ != idx) {
                            st->hovered_ = idx;
                            st->setState();
                        }
                    },
                    nullptr, c);
            tail.push_back(cell);
        }
        auto tailRow = cui::make_shared<Row>(std::move(tail));
        tailRow->setMainAxisSize(MainAxisSize::Min);
        tailRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
        kids.push_back(cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)));
        kids.push_back(tailRow);
    }

    auto row = cui::make_shared<Row>(std::move(kids));
    row->setMainAxisAlignment(MainAxisAlignment::Start);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto box = cui::make_shared<Container>(row);
    box->width(self->width())
            .height(32.0)
            .padding(EdgeInsets::symmetric(0.0, 8.0))
            .decoration(BoxDecoration{kSurface, 0.0, 1.0, Color::fromRGBA(0, 0, 0, 0.10)});
    return box;
}

std::unique_ptr<State> TitleBar::createState() {
    return std::make_unique<TitleBarState>();
}

SharedPtr<Widget> ExpandableFabState::build(BuildContext&) {
    const double bw = bodyW(), bh = bodyH(), radius = borderRadius();
    const IconCode code = w()->icon();
    const Color fg = resolveFg();

    // 内容：Extended = 图标 + 文字标签的横向胶囊；其余 = 居中图标。
    SharedPtr<Widget> content;
    if (w()->size() == FabSize::Extended) {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Icon>(code, iconSize(), fg));
        if (!w()->label().empty()) {
            kids.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
            kids.push_back(cui::make_shared<Text>(w()->label(), TextStyle().size(14.0).bold().fgColor(fg)));
        }
        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        content = row;
    } else {
        content = cui::make_shared<Icon>(code, iconSize(), fg);
    }

    // 本体容器：M3 圆角矩形 + 双层投影。
    auto box = cui::make_shared<Container>(content);
    box->width(bw).height(bh).alignment(0.0, 0.0);
    if (w()->size() == FabSize::Extended)
        box->padding(EdgeInsets::only(16.0, 0.0, 20.0, 0.0));
    box->decoration(BoxDecoration{resolveBg(), radius, 0.0, Color(), 6.0, Color::fromRGBA(0, 0, 0, 0.28), 2.0,
                                  Color::fromRGBA(0, 0, 0, 0.15)});

    // 状态覆盖层（hover/focus/pressed/涟漪/焦点环/禁用）画在本体之上，裁到圆角矩形。
    auto painted = cui::make_shared<CustomPaint>(nullptr, makeStatePainter(), Size(bw, bh), box);

    // 交互：Focus（点击/Tab 上焦，驱动焦点环）+ MouseRegion（hover 染色）+ 点击。
    auto tappable = cui::make_shared<GestureDetector>(
            painted,
            [this] {
                pressed_ = true;
                setState();
                activate();
            },
            nullptr,
            [this](const PointerEvent&) {
                pressed_ = false;
                setState();
            });
    auto hover = cui::make_shared<MouseRegion>(
            tappable,
            [this](const PointerEvent&) {
                hovered_ = true;
                setState();
            },
            [this](const PointerEvent&) {
                hovered_ = false;
                pressed_ = false;
                setState();
            });
    return cui::make_shared<Focus>(focusNode_, hover, [this](bool f) {
        focused_ = f;
        setState();
    });
}

std::unique_ptr<State> ExpandableFab::createState() {
    return std::make_unique<ExpandableFabState>();
}

SharedPtr<Widget> PopupState::build(BuildContext&) {
    auto* self = w();
    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(cui::make_shared<GestureDetector>(self->trigger(), [this] {
        open_ = !open_;
        setState();
    }));

    if (open_) {
        std::vector<SharedPtr<Widget>> col;
        if (!self->title().empty()) {
            auto t = cui::make_shared<Text>(self->title(), TextStyle().size(12.5).bold());
            col.push_back(t);
        }
        if (self->content())
            col.push_back(self->content());
        auto panel = cui::make_shared<Container>(cui::make_shared<Column>(std::move(col)));
        // 对齐 legacy PopupMenuButton::paintPopup：surface 底 + borderLight 边框 +
        //   radiusMedium(6) + 0.12 黑色阴影（偏移 2,2）。
        panel->width(220.0)
                .padding(EdgeInsets::all(12.0))
                .decoration(BoxDecoration{theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.10), 12.0,
                                          Color::fromRGBA(0, 0, 0, 0.18)});
        auto pos = cui::make_shared<Positioned>(panel);
        pos->hasTop = true;
        pos->top = 30.0;
        pos->hasLeft = true;
        pos->left = 0.0;
        kids.push_back(pos);
    }

    auto stack = cui::make_shared<Stack>(std::move(kids));
    stack->setFit(RenderStack::Fit::Loose);
    return stack;
}

std::unique_ptr<State> Popup::createState() {
    return std::make_unique<PopupState>();
}

SharedPtr<Widget> AnimatedState::build(BuildContext&) {
    auto* self = an();
    double w = self->smallW() + (self->bigW() - self->smallW()) * t_;
    bool on = t_ > 0.5;
    auto box = cui::make_shared<Container>(cui::make_shared<SizedBox>(w, self->height()));
    box->decoration(BoxDecoration{on ? Color::fromRGB(0.95, 0.55, 0.15) : theme().primary, 8.0});

    auto hint = cui::make_shared<Text>("点击切换（t = " + std::to_string(t_).substr(0, 4) + "）",
                                       TextStyle().size(11.5).fgColor(theme().textSecondary));
    auto col =
            cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{cui::make_shared<GestureDetector>(box,
                                                                                                      [this] {
                                                                                                          big_ = !big_;
                                                                                                          setState();
                                                                                                      }),
                                                                    cui::make_shared<SizedBox>(0.0, 6.0), hint});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

std::unique_ptr<State> Animated::createState() {
    return std::make_unique<AnimatedState>();
}

SharedPtr<Widget> SettingsState::build(BuildContext&) {
    auto* self = st();
    std::vector<SharedPtr<Widget>> rows;
    for (size_t i = 0; i < self->items().size(); ++i) {
        const auto& it = self->items()[i];
        auto title = cui::make_shared<Text>(it.title, TextStyle().size(13.0).bold());
        auto sub = cui::make_shared<Text>(it.subtitle, TextStyle().size(11.5).fgColor(theme().textSecondary));
        auto left = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{title, sub});
        left->setMainAxisSize(MainAxisSize::Min);
        left->setCrossAxisAlignment(CrossAxisAlignment::Start);

        SharedPtr<Widget> trailing;
        if (it.kind == 0) {
            size_t idx = i;
            trailing = cui::make_shared<Switch>(values_[i], [this, idx](bool v) {
                values_[idx] = v;
                setState();
            });
        } else if (it.kind == 1) {
            size_t idx = i;
            trailing = cui::make_shared<Checkbox>(values_[i], [this, idx](bool v) {
                values_[idx] = v;
                setState();
            });
        } else {
            trailing = cui::make_shared<Text>(it.valueText, TextStyle().size(12.0).fgColor(theme().textSecondary));
        }

        auto row = cui::make_shared<Row>(
                std::vector<SharedPtr<Widget>>{left, cui::make_shared<SizedBox>(16.0, 0.0), trailing});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(row);
        box->width(360.0).padding(EdgeInsets::symmetric(10.0, 12.0));
        rows.push_back(box);
        rows.push_back(cui::make_shared<Container>(cui::make_shared<SizedBox>(360.0, 1.0), nullptr));
    }
    auto col = cui::make_shared<Column>(std::move(rows));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto outer = cui::make_shared<Container>(col);
    outer->decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.12)});
    return outer;
}

std::unique_ptr<State> Settings::createState() {
    return std::make_unique<SettingsState>();
}

SharedPtr<Widget> GifState::build(BuildContext&) {
    auto* self = gif();
    const auto& frames = self->frames();
    SharedPtr<Widget> current = frames.empty() ? cui::make_shared<SizedBox>(self->width(), self->height()) :
                                                 frames[static_cast<size_t>(frame_) % frames.size()];
    auto stage = cui::make_shared<Container>(cui::make_shared<Center>(current));
    stage->width(self->width())
            .height(self->height())
            .decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.05), 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.12)});

    auto toggle =
            cui::make_shared<Text>(playing_ ? "|| 暂停" : "▶ 播放", TextStyle().size(12.0).fgColor(theme().primary));
    auto toggleBox = cui::make_shared<Container>(toggle);
    toggleBox->padding(EdgeInsets::symmetric(5.0, 10.0))
            .decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.06), 12.0});
    auto toggleBtn = cui::make_shared<GestureDetector>(toggleBox, [this] {
        playing_ = !playing_;
        setState();
    });

    auto info = cui::make_shared<Text>("frame " + std::to_string(frame_ + 1) + " / " + std::to_string(frames.size()) +
                                               "  ·  " + std::to_string(static_cast<int>(self->fps())) + " fps",
                                       TextStyle().size(11.0).fgColor(theme().textSecondary));

    auto ctrl = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{toggleBtn, cui::make_shared<SizedBox>(8.0, 0.0), info});
    ctrl->setMainAxisSize(MainAxisSize::Min);
    ctrl->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto col =
            cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{stage, cui::make_shared<SizedBox>(0.0, 8.0), ctrl});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Center);
    return col;
}

std::unique_ptr<State> Gif::createState() {
    return std::make_unique<GifState>();
}

SharedPtr<Widget> DatePickerState::build(BuildContext&) {
    auto pad2 = [](int v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    std::string text = std::to_string(selY_) + "-" + pad2(selM_ + 1) + "-" + pad2(selD_);
    auto head = cui::make_shared<Text>("selected: " + text, TextStyle().size(14.0).bold().fgColor(theme().primary));
    auto headBox = cui::make_shared<Container>(head);
    headBox->padding(EdgeInsets(0.0, 0.0, 0.0, 10.0));

    // 选中态由 Calendar 的 State 持有，这里只把它镜像成回显文字 + 初始选中日。
    auto cal = cui::make_shared<Calendar>(
            0, 0,
            [this](int y, int m, int d) {
                selY_ = y;
                selM_ = m;
                selD_ = d;
                setState();
            },
            nullptr, selY_, selM_ + 1, selD_);

    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{headBox, cal});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

std::unique_ptr<State> DatePicker::createState() {
    return std::make_unique<DatePickerState>();
}

SharedPtr<Widget> TimePickerState::build(BuildContext&) {
    auto pad2 = [](int v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    auto smallBtn = [this](const std::string& sym, std::function<void()> fn) {
        auto t = cui::make_shared<Text>(sym, TextStyle().size(11.0).fgColor(theme().textSecondary));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(44.0).height(20.0).decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.06), 4.0});
        return cui::make_shared<GestureDetector>(c, std::move(fn));
    };
    auto valueBox = [&pad2](int v) {
        auto t = cui::make_shared<Text>(pad2(v), TextStyle().size(22.0).bold());
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(56.0).height(36.0).decoration(
                BoxDecoration{theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.18)});
        return c;
    };

    auto hourCol = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{smallBtn("▲",
                                                                                    [this] {
                                                                                        hour_ = (hour_ + 1) % 24;
                                                                                        setState();
                                                                                    }),
                                                                           valueBox(hour_), smallBtn("▼", [this] {
                                                                               hour_ = (hour_ + 23) % 24;
                                                                               setState();
                                                                           })});
    hourCol->setMainAxisSize(MainAxisSize::Min);

    auto minCol = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{smallBtn("▲",
                                                                                   [this] {
                                                                                       minute_ = (minute_ + 1) % 60;
                                                                                       setState();
                                                                                   }),
                                                                          valueBox(minute_), smallBtn("▼", [this] {
                                                                              minute_ = (minute_ + 59) % 60;
                                                                              setState();
                                                                          })});
    minCol->setMainAxisSize(MainAxisSize::Min);

    auto colon = cui::make_shared<Text>(":", TextStyle().size(22.0).bold());
    auto colonBox = cui::make_shared<Container>(cui::make_shared<Center>(colon));
    colonBox->padding(EdgeInsets::symmetric(4.0, 0.0));

    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{hourCol, colonBox, minCol});
    row->setMainAxisSize(MainAxisSize::Min);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto summary = cui::make_shared<Text>("selected: " + pad2(hour_) + ":" + pad2(minute_),
                                          TextStyle().size(11.5).fgColor(theme().textSecondary));
    auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{row, cui::make_shared<SizedBox>(0.0, 8.0), summary});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto outer = cui::make_shared<Container>(col);
    outer->padding(EdgeInsets::all(12.0))
            .decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.12)});
    return outer;
}

std::unique_ptr<State> TimePicker::createState() {
    return std::make_unique<TimePickerState>();
}

SharedPtr<Widget> HoverCardState::build(BuildContext&) {
    auto* self = w();
    auto* me = this;

    std::vector<SharedPtr<Widget>> kids;
    if (self->trigger())
        kids.push_back(self->trigger());

    // 悬停/移动 → 显示（顺带取消关闭延时）。
    Listener::PointerCallback onHover = [me](const PointerEvent& e) {
        if (e.pointerKind == PointerDeviceKind::mouse)
            me->showNow();
    };
    Listener::PointerCallback onMove = [me](const PointerEvent&) { me->showNow(); };
    Listener::PointerCallback onEnter = [me](const PointerEvent&) { me->showNow(); };
    // 移出 → 延时关闭。
    Listener::PointerCallback onExit = [me](const PointerEvent&) { me->scheduleClose(); };
    Listener::PointerCallback onUp = [me](const PointerEvent&) { me->hideNow(); };

    if (visible_ && self->card()) {
        auto panel = cui::make_shared<Container>(self->card());
        panel->width(220.0)
                .padding(EdgeInsets::all(12.0))
                .decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.14), 10.0,
                                          Color::fromRGBA(0, 0, 0, 0.25)});
        // 不在此处再包一层 Listener：外层 Listener 的命中区域已覆盖
        // trigger+卡片（Stack 包含卡片），鼠标移到卡片上仍算「在内部」，
        // 不会触发 Exit；若这里再包一层，卡片出现/消失会改变 hover 路径，
        // 触发框架每帧重新合成的 onEnter → showNow → cancelClose 振荡。
        auto pos = cui::make_shared<Positioned>(panel);
        pos->hasTop = true;
        pos->top = self->cardTop();
        pos->hasLeft = true;
        pos->left = 0.0;
        kids.push_back(pos);
    }

    auto stack = cui::make_shared<Stack>(std::move(kids));
    stack->setFit(RenderStack::Fit::Loose);
    return cui::make_shared<Listener>(nullptr, onMove, onUp, onHover, nullptr, stack, onEnter, onExit);
}

std::unique_ptr<State> HoverCard::createState() {
    return std::make_unique<HoverCardState>();
}

SharedPtr<Widget> CommandPaletteState::build(BuildContext&) {
    auto* self = w();
    rebuildMatched();
    const Color kAccent = theme().primary;
    const std::string query = controller_->text();

    // 输入行：真的输入框（点它即上焦打字），前面保留面板的 ">" 提示符。
    auto field = cui::make_shared<EditableText>(controller_, TextStyle().size(13.0).fgColor(theme().textPrimary),
                                                TextAlign::Left, std::string("Type a command…"),
                                                /*obscureText=*/false, /*autofocus=*/false, nullptr, focusNode_,
                                                nullptr, nullptr);
    auto inputRow = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{cui::make_shared<Text>(">", TextStyle().size(13.0).bold().fgColor(kAccent)),
                                           cui::make_shared<SizedBox>(6.0, 0.0), cui::make_shared<Expanded>(1, field)});
    inputRow->setMainAxisSize(MainAxisSize::Min);
    inputRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto inputBox = cui::make_shared<Container>(inputRow);
    // 显式给到卡片内容宽（340 - 2·12）：Min + Expanded 在松约束下只按非 flex 子算尺寸，
    // 296 宽的输入盒会溢出到 Row 自身 20 宽的命中盒之外 ⇒ RenderBox::hitTest 的 size
    // 判定直接把 EditableText 挡在命中链外（点不动，也就上不了焦）。
    inputBox->width(316.0)
            .padding(EdgeInsets::symmetric(10.0, 12.0))
            .decoration(BoxDecoration{theme().surface, 0.0, 0.0, Color::fromRGBA(0, 0, 0, 0), 1.0,
                                      Color::fromRGBA(0, 0, 0, 0.12)});

    // 首字母筛选（鼠标用户的快捷入口：等价于把那个字母写进查询框）
    std::vector<SharedPtr<Widget>> chips;
    auto chip = [&](const std::string& label, const std::string& value) {
        bool on = (query == value);
        auto t = cui::make_shared<Text>(
                label, TextStyle().size(11.5).bold().fgColor(on ? theme().textOnPrimary : theme().textPrimary));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->padding(EdgeInsets::symmetric(5.0, 9.0))
                .decoration(BoxDecoration{on ? theme().primary : Color::fromRGBA(0, 0, 0, 0.06), 10.0});
        return cui::make_shared<GestureDetector>(c, [this, value] {
            controller_->setText(value);  // 走监听 ⇒ 高亮回 0 + 重建
        });
    };
    chips.push_back(chip("全部", ""));
    std::set<char> seen;
    for (const auto& c : self->commands()) {
        if (c.title.empty())
            continue;
        char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(c.title[0])));
        if (seen.insert(ch).second)
            chips.push_back(chip(std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))),
                                 std::string(1, ch)));
    }
    auto chipRow = cui::make_shared<Row>(std::move(chips));
    chipRow->setMainAxisSize(MainAxisSize::Min);

    // 命令列表（按当前查询过滤；高亮项是键盘选中项）
    std::vector<SharedPtr<Widget>> rows;
    for (int slot = 0; slot < static_cast<int>(matched_.size()); ++slot) {
        const auto& cmd = self->commands()[static_cast<size_t>(matched_[static_cast<size_t>(slot)])];
        const bool sel = (slot == highlight_);
        auto titleStyle = TextStyle().size(13.0).bold();
        auto subStyle = TextStyle().size(11.0).fgColor(theme().textSecondary);
        auto scStyle = TextStyle().size(11.0).fgColor(theme().textDisabled);
        if (sel) {
            // 高亮行整条铺主色 ⇒ 文字统一用「主色上的前景色」，深色主题下自动反色。
            titleStyle.fgColor(theme().textOnPrimary);
            subStyle.fgColor(theme().textOnPrimary);
            scStyle.fgColor(theme().textOnPrimary);
        }
        auto title = cui::make_shared<Text>(cmd.title, titleStyle);
        auto sub = cui::make_shared<Text>(cmd.subtitle, subStyle);
        auto sc = cui::make_shared<Text>(cmd.shortcut, scStyle);
        auto left = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{title, sub});
        left->setMainAxisSize(MainAxisSize::Min);
        left->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{left, sc});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(row);
        box->width(300.0)
                .padding(EdgeInsets::symmetric(8.0, 10.0))
                .decoration(BoxDecoration{sel ? kAccent : Color::fromRGBA(0, 0, 0, 0.02), 6.0});
        rows.push_back(cui::make_shared<GestureDetector>(box, [this, slot] { runMatched(slot); }));
        rows.push_back(cui::make_shared<SizedBox>(0.0, 4.0));
    }
    if (matched_.empty())
        rows.push_back(cui::make_shared<Text>("（无匹配命令）", TextStyle().size(12.0).fgColor(theme().textSecondary)));
    auto list = cui::make_shared<Column>(std::move(rows));
    list->setMainAxisSize(MainAxisSize::Min);
    list->setCrossAxisAlignment(CrossAxisAlignment::Start);

    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            inputBox, cui::make_shared<SizedBox>(0.0, 8.0), chipRow, cui::make_shared<SizedBox>(0.0, 10.0), list});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto outer = cui::make_shared<Container>(col);
    outer->width(340.0)
            .padding(EdgeInsets::all(12.0))
            .decoration(BoxDecoration{theme().surface, 10.0, 1.0, Color::fromRGBA(0, 0, 0, 0.15), 12.0,
                                      Color::fromRGBA(0, 0, 0, 0.3)});
    return outer;
}

KeyEventResult CommandPaletteState::handleKey(const KeyEvent& ev) {
    switch (static_cast<KeyCode>(ev.key)) {
        case KeyCode::Up:
        case KeyCode::Down: {
            rebuildMatched();  // 两次按键之间可能只重建过旧列表
            if (matched_.empty())
                return KeyEventResult::Ignored;  // 没东西可移动 ⇒ 放行给祖先
            moveHighlight(static_cast<KeyCode>(ev.key) == KeyCode::Up ? -1 : 1);
            return KeyEventResult::Handled;
        }
        case KeyCode::Return:
            rebuildMatched();
            if (highlight_ < 0)
                return KeyEventResult::Ignored;
            runMatched(highlight_);
            return KeyEventResult::Handled;
        case KeyCode::Escape:
            if (controller_->text().empty())
                return KeyEventResult::Ignored;
            controller_->setText("");  // 走监听 ⇒ 高亮回 0 + 重建
            return KeyEventResult::Handled;
        default:
            return KeyEventResult::Ignored;
    }
}

void CommandPaletteState::runMatched(int slot) {
    if (slot < 0 || slot >= static_cast<int>(matched_.size()))
        return;
    auto* self = w();
    if (self->onRun())
        self->onRun()(matched_[static_cast<size_t>(slot)]);
}

void CommandPaletteState::moveHighlight(int delta) {
    if (matched_.empty())
        return;
    const int n = static_cast<int>(matched_.size());
    const int h = (highlight_ < 0 ? 0 : highlight_ + delta);
    highlight_ = std::max(0, std::min(n - 1, h));  // 到顶/到底停住（不环绕）
    setState();
}

void CommandPaletteState::rebuildMatched() {
    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string lq = lower(controller_->text());
    matched_.clear();
    const auto& cmds = w()->commands();
    for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
        const std::string title = lower(cmds[static_cast<size_t>(i)].title);
        if (lq.empty() || title.find(lq) != std::string::npos)
            matched_.push_back(i);
    }
    if (highlight_ < 0 || highlight_ >= static_cast<int>(matched_.size()))
        highlight_ = matched_.empty() ? -1 : 0;
}

void CommandPaletteState::dispose() {
    // 控制器/焦点节点可能被外部持有得比本 State 久，不摘就会在后续通知上
    // 回调已销毁的 State。
    if (keyHandlerId_ > 0) {
        focusNode_->removeKeyHandler(keyHandlerId_);
        keyHandlerId_ = 0;
    }
    if (listenerId_ > 0) {
        controller_->removeListener(listenerId_);
        listenerId_ = 0;
    }
    State::dispose();
}

void CommandPaletteState::initState() {
    controller_ = cui::make_shared<TextEditingController>();
    focusNode_ = cui::make_shared<FocusNode>();
    // 导航键处理器与 EditableText 共用一个节点，注册在其单槽之后
    // （见 FocusManager::sendKeyEvent）⇒ 字符键先被输入框吃掉，它 Ignored
    // 的键才轮到我们。
    keyHandlerId_ = focusNode_->addKeyHandler([this](const KeyEvent& ev) { return handleKey(ev); });
    listenerId_ = controller_->addListener([this] {
        highlight_ = 0;  // 查询变了 ⇒ 高亮回到第一条匹配（rebuildMatched 负责夹）
        setState();
    });
}

std::unique_ptr<State> CommandPalette::createState() {
    return std::make_unique<CommandPaletteState>();
}

SharedPtr<Widget> CarouselState::build(BuildContext&) {
    auto* self = w();
    int n = static_cast<int>(self->items().size());
    if (n == 0)
        return cui::make_shared<SizedBox>(0.0, 0.0);
    int cur = self->index();

    // 严格 1:1：一次只渲染「当前页」（上游 CarouselView 的
    // viewportFraction = 1）。视口里只有一张卡片，切换靠左右箭头 /
    // 指示点，不再显示左右邻项的「窥视」窗口。
    std::vector<SharedPtr<Widget>> slides;
    {
        auto inner = cui::make_shared<Container>(cui::make_shared<Center>(self->items()[cur]));
        inner->width(self->itemWidth())
                .height(self->itemHeight())
                .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.12), 8.0, 2.0, theme().primary});
        auto padded = cui::make_shared<Container>(inner);
        padded->padding(EdgeInsets::symmetric(0.0, 5.0));
        slides.push_back(padded);
    }


    auto arrowBtn = [&](const std::string& sym, int delta) {
        auto t = cui::make_shared<Text>(sym, TextStyle().size(18.0).bold().fgColor(theme().primary));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(28.0).height(28.0).decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.05), 14.0});
        return cui::make_shared<GestureDetector>(c, [this, delta] {
            auto* s = w();
            s->setIndex(s->index() + delta);
            setState();
        });
    };

    std::vector<SharedPtr<Widget>> rowKids;
    rowKids.push_back(arrowBtn("‹", -1));
    rowKids.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
    for (auto& s : slides)
        rowKids.push_back(s);
    rowKids.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
    rowKids.push_back(arrowBtn("›", 1));
    auto row = cui::make_shared<Row>(std::move(rowKids));
    row->setMainAxisSize(MainAxisSize::Min);
    row->setMainAxisAlignment(MainAxisAlignment::Center);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    std::vector<SharedPtr<Widget>> dots;
    for (int i = 0; i < n; ++i) {
        bool on = i == cur;
        auto d = cui::make_shared<Container>(cui::make_shared<SizedBox>(on ? 14.0 : 8.0, 8.0));
        d->decoration(BoxDecoration{on ? theme().primary : Color::fromRGBA(0, 0, 0, 0.2), 4.0});
        int idx = i;
        dots.push_back(cui::make_shared<GestureDetector>(d, [this, idx] {
            auto* s = w();
            s->setIndex(idx);
            setState();
        }));
        dots.push_back(cui::make_shared<SizedBox>(4.0, 0.0));
    }
    auto dotRow = cui::make_shared<Row>(std::move(dots));
    dotRow->setMainAxisSize(MainAxisSize::Min);

    auto counter = cui::make_shared<Text>(std::to_string(cur + 1) + " / " + std::to_string(n),
                                          TextStyle().size(11.5).fgColor(theme().textSecondary));

    auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{row, cui::make_shared<SizedBox>(0.0, 8.0), dotRow, counter});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Center);
    return col;
}

std::unique_ptr<State> Carousel::createState() {
    return std::make_unique<CarouselState>();
}

SharedPtr<Widget> AccordionState::build(BuildContext&) {
    auto* self = w();
    std::vector<SharedPtr<Widget>> kids;
    for (int i = 0; i < static_cast<int>(self->items().size()); ++i) {
        bool open = i == self->expandedIndex();
        auto title = cui::make_shared<Text>(
                self->items()[i].title,
                TextStyle().size(13.0).bold().fgColor(open ? theme().primary : theme().textPrimary));
        auto arrow = cui::make_shared<Text>(open ? "▼" : "▶", TextStyle().size(12.0));
        auto row = cui::make_shared<Row>(
                std::vector<SharedPtr<Widget>>{title, cui::make_shared<SizedBox>(8.0, 0.0), arrow});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto header = cui::make_shared<Container>(row);
        header->padding(EdgeInsets::symmetric(10.0, 12.0))
                .decoration(BoxDecoration{
                        open ? Color::fromRGBA(0.0, 0.45, 0.95, 0.10) : Color::fromRGBA(0, 0, 0, 0.03), 6.0});
        int idx = i;
        kids.push_back(cui::make_shared<GestureDetector>(header, [this, idx] {
            auto* s = w();
            s->setExpandedIndex(s->expandedIndex() == idx ? -1 : idx);  // 单开
            setState();
        }));
        if (open && self->items()[i].content) {
            auto body = cui::make_shared<Container>(self->items()[i].content);
            body->padding(EdgeInsets::symmetric(10.0, 12.0))
                    .decoration(BoxDecoration{theme().surface, 0.0, 1.0, Color::fromRGBA(0, 0, 0, 0.08)});
            kids.push_back(body);
        }
        kids.push_back(cui::make_shared<SizedBox>(0.0, 4.0));
    }
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto box = cui::make_shared<Container>(col);
    box->width(320.0).decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.12)});
    return box;
}

std::unique_ptr<State> Accordion::createState() {
    return std::make_unique<AccordionState>();
}

std::unique_ptr<State> Zoomable::createState() {
    return std::make_unique<ZoomableState>();
}

SharedPtr<Widget> SplitterState::build(BuildContext&) {
    auto* self = sp();
    bool horiz = (self->axis() == Axis::Horizontal);
    double th = self->thickness();
    auto first = self->first();
    auto second = self->second();

    return cui::make_shared<LayoutBuilder>(
            [this, horiz, th, first, second](BuildContext&, const BoxConstraints& cons) mutable -> SharedPtr<Widget> {
                double total = horiz ? cons.maxWidth : cons.maxHeight;
                if (!(total > 0.0) || total > 1e17)
                    total = horiz ? 560.0 : 260.0;
                double firstSize = std::max(24.0, total * ratio_ - th / 2.0);

                auto pane1 = cui::make_shared<Container>(first);
                if (horiz)
                    pane1->width(firstSize);
                else
                    pane1->height(firstSize);
                pane1->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.04), 4.0});

                auto handle = cui::make_shared<Container>(cui::make_shared<SizedBox>(th, th));
                handle->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.22), th / 2.0});

                GestureDetector::PointerCallback onDown = [this, horiz](const PointerEvent& ev) {
                    dragStart_ = horiz ? ev.globalPosition.dx : ev.globalPosition.dy;
                    startRatio_ = ratio_;
                };
                GestureDetector::PointerCallback onMove = [this, horiz, total](const PointerEvent& ev) {
                    double cur = horiz ? ev.globalPosition.dx : ev.globalPosition.dy;
                    double delta = cur - dragStart_;
                    ratio_ = std::clamp(startRatio_ + delta / total, 0.15, 0.85);
                    setState();
                };
                auto gd = cui::make_shared<GestureDetector>(handle, nullptr, nullptr, nullptr, onDown, onMove, nullptr);

                auto pane2Box = cui::make_shared<Container>(second);
                pane2Box->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.02), 4.0});
                auto pane2 = cui::make_shared<Expanded>(1, pane2Box);

                if (horiz) {
                    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{pane1, gd, pane2});
                    row->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
                    auto box = cui::make_shared<Container>(row);
                    box->height(200.0);
                    return box;
                } else {
                    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{pane1, gd, pane2});
                    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
                    auto box = cui::make_shared<Container>(col);
                    box->height(260.0);
                    return box;
                }
            });
}

std::unique_ptr<State> Splitter::createState() {
    return std::make_unique<SplitterState>();
}

SharedPtr<Widget> TokenInputState::build(BuildContext&) {
    auto* self = ti();
    std::vector<SharedPtr<Widget>> chips;
    for (int i = 0; i < static_cast<int>(tokens_.size()); ++i) {
        auto label = cui::make_shared<Text>(tokens_[i], TextStyle().size(12.0));
        auto x = cui::make_shared<Text>(" ×", TextStyle().size(12.0).bold().fgColor(theme().textDisabled));
        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{label, x});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto c = cui::make_shared<Container>(row);
        c->padding(EdgeInsets::symmetric(6.0, 8.0))
                .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.12), 12.0});
        int idx = i;
        chips.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
            if (idx < static_cast<int>(tokens_.size())) {
                tokens_.erase(tokens_.begin() + idx);
                setState();
            }
        }));
    }
    if (tokens_.empty()) {
        auto t = cui::make_shared<Text>("（无标签，点下方候选添加）",
                                        TextStyle().size(12.0).fgColor(theme().textSecondary));
        chips.push_back(t);
    }
    auto chipWrap = cui::make_shared<Wrap>(Axis::Horizontal, std::move(chips), WrapAlignment::Start, 6.0,
                                           WrapAlignment::Start, 6.0);

    std::vector<SharedPtr<Widget>> sugg;
    for (const auto& s : self->suggestions()) {
        auto t = cui::make_shared<Text>("+ " + s, TextStyle().size(12.0).fgColor(theme().primary));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::symmetric(5.0, 8.0))
                .decoration(
                        BoxDecoration{Color::fromRGBA(0, 0, 0, 0), 12.0, 1.0, Color::fromRGBA(0.0, 0.45, 0.95, 0.5)});
        std::string val = s;
        sugg.push_back(cui::make_shared<GestureDetector>(c, [this, val] {
            if (std::find(tokens_.begin(), tokens_.end(), val) == tokens_.end()) {
                tokens_.push_back(val);
                setState();
            }
        }));
    }
    auto suggWrap = cui::make_shared<Wrap>(Axis::Horizontal, std::move(sugg), WrapAlignment::Start, 6.0,
                                           WrapAlignment::Start, 6.0);

    auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{chipWrap, cui::make_shared<SizedBox>(0.0, 10.0), suggWrap});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto box = cui::make_shared<Container>(col);
    box->width(360.0)
            .padding(EdgeInsets::all(12.0))
            .decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.15)});
    return box;
}

std::unique_ptr<State> TokenInput::createState() {
    return std::make_unique<TokenInputState>();
}

SharedPtr<Widget> OtpInputState::build(BuildContext&) {
    auto* self = otp();
    const Color kAccent = theme().primary;
    std::vector<SharedPtr<Widget>> boxes;
    for (int i = 0; i < self->length(); ++i) {
        const bool filled = i < static_cast<int>(digits_.size()) && digits_[i] >= 0;
        const bool hot = i < static_cast<int>(nodes_.size()) && nodes_[static_cast<size_t>(i)]->hasFocus();
        auto t = cui::make_shared<Text>(filled ? std::to_string(digits_[i]) : std::string(),
                                        TextStyle().size(16.0).bold());
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(38.0).height(44.0).decoration(
                BoxDecoration{theme().surface, 6.0, 1.5, hot ? kAccent : Color::fromRGBA(0, 0, 0, 0.25)});
        // 节点由状态持有（跨重建持久），所以走「外部节点」那个构造；
        // onFocusChange 必须回到 setState，否则边框高亮停在旧帧。
        boxes.push_back(cui::make_shared<Focus>(
                nodes_[static_cast<size_t>(i)], std::move(c), [this](bool) { setState(); }, false, nullptr,
                [this, i](const KeyEvent& ev) { return handleKey(i, ev); }));
        boxes.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
    }
    auto row = cui::make_shared<Row>(std::move(boxes));
    row->setMainAxisSize(MainAxisSize::Min);

    auto hint = cui::make_shared<Text>("code: " + code(), TextStyle().size(11.5).fgColor(theme().textSecondary));
    auto col =
            cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{row, cui::make_shared<SizedBox>(0.0, 6.0), hint});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

KeyEventResult OtpInputState::handleKey(int index, const KeyEvent& ev) {
    if (index < 0 || index >= static_cast<int>(digits_.size()))
        return KeyEventResult::Ignored;

    if (ev.key == 0 && !ev.text.empty() && !(ev.modifiers & (ModCtrl | ModMeta))) {
        const char c = ev.text.front();
        if (c < '0' || c > '9')
            return KeyEventResult::Ignored;
        digits_[index] = c - '0';
        setState();
        focusCell(index + 1);
        return KeyEventResult::Handled;
    }

    switch (static_cast<KeyCode>(ev.key)) {
        case KeyCode::Backspace:
            if (digits_[index] >= 0) {
                digits_[index] = -1;
            } else if (index > 0) {
                focusCell(index - 1);
                digits_[static_cast<size_t>(index) - 1] = -1;
            }
            setState();
            return KeyEventResult::Handled;
        case KeyCode::Delete:
            digits_[index] = -1;
            setState();
            return KeyEventResult::Handled;
        case KeyCode::Left:
            focusCell(index - 1);
            return KeyEventResult::Handled;
        case KeyCode::Right:
            focusCell(index + 1);
            return KeyEventResult::Handled;
        default:
            return KeyEventResult::Ignored;
    }
}

void OtpInputState::focusCell(int index) {
    if (index < 0 || index >= static_cast<int>(nodes_.size()))
        return;
    nodes_[index]->requestFocus();
}

std::string OtpInputState::code() const {
    std::string out;
    for (int d : digits_)
        out += (d >= 0) ? std::to_string(d) : "-";
    return out;
}

void OtpInputState::initState() {
    const int n = otp()->length() > 0 ? otp()->length() : 0;
    digits_.assign(static_cast<size_t>(n), -1);
    for (int i = 0; i < n; ++i)
        nodes_.push_back(cui::make_shared<FocusNode>());
}

std::unique_ptr<State> OtpInput::createState() {
    return std::make_unique<OtpInputState>();
}

SharedPtr<Widget> MenuBarState::build(BuildContext&) {
    auto* self = w();
    // 菜单条高度（下拉浮层由上层以 Stack 顶层渲染，紧贴条底部）。
    const double kBarH = 30.0;
    const double kPadX = 12.0;  // 标签左右内边距（壳层定位公式依赖它）
    const double kJPadY = 6.0;  // 标签上下内边距
    const double kGap = 4.0;    // 标签间距
    std::vector<SharedPtr<Widget>> labels;
    for (int i = 0; i < static_cast<int>(self->menus().size()); ++i) {
        bool on = i == self->openIndex();
        const std::string& label = self->menus()[i].label;
        auto t = cui::make_shared<Text>(label,
                                        TextStyle().size(13.0).fgColor(on ? theme().primary : theme().textPrimary));
        auto c = cui::make_shared<Container>(t);
        // kJPadY 是上下内边距；左右用 kPadX —— 壳层按下拉面板左缘 = 标签左缘
        // 的位置公式（2*kPadX + kGap）依赖这两个口径，别调换。
        c->padding(EdgeInsets::symmetric(kPadX, kJPadY))
                .decoration(
                        BoxDecoration{on ? Color::fromRGBA(0.0, 0.45, 0.95, 0.12) : Color::fromRGBA(0, 0, 0, 0), 4.0});
        int idx = i;
        // 标签只负责触发「点击标签」回调；展开/收起/切换由上层维护。
        // hover 只在已展开时转发 ⇒ 菜单打开后左右移到相邻标签即自动切换下拉。
        auto tap = cui::make_shared<GestureDetector>(c, [self, idx] {
            if (auto f = self->onLabelTap())
                f(idx);
        });
        labels.push_back(
                cui::make_shared<MouseRegion>(std::move(tap), nullptr, nullptr, [self, idx](const PointerEvent&) {
                    // 用 onHover 而不是 onEnter：Enter/Exit 是「命中集合差分」，而按下/抬起
                    // 不更新悬停链 ⇒ 只接 onEnter 时，「先扫过 B、再点 A、再回到 B」这条
                    // 最常见的路径会被当成「没变化」而漏掉。hover 每次移动都发，等值由
                    // 上层早退。
                    if (self->openIndex() < 0)
                        return;
                    if (auto f = self->onLabelHover())
                        f(idx);
                }));
        labels.push_back(cui::make_shared<SizedBox>(kGap, 0.0));
    }
    auto bar = cui::make_shared<Row>(std::move(labels));
    bar->setMainAxisSize(MainAxisSize::Min);
    auto barBox = cui::make_shared<Container>(bar);
    barBox->height(kBarH).decoration(
            BoxDecoration{Color::fromRGBA(0, 0, 0, 0.04), 0.0, 1.0, Color::fromRGBA(0, 0, 0, 0.1)});
    return barBox;
}

std::unique_ptr<State> MenuBar::createState() {
    return std::make_unique<MenuBarState>();
}

std::unique_ptr<State> ContextMenu::createState() {
    return std::make_unique<ContextMenuState>();
}

std::unique_ptr<State> Tabs::createState() {
    return std::make_unique<TabsState>();
}

SharedPtr<Widget> ColorPickerState::build(BuildContext&) {
    auto* self = w();
    std::vector<SharedPtr<Widget>> kids;
    for (int i = 0; i < static_cast<int>(self->swatches().size()); ++i) {
        bool sel = i == self->selected();
        auto c = cui::make_shared<Container>(cui::make_shared<SizedBox>(28.0, 28.0));
        c->decoration(BoxDecoration{self->swatches()[i], 6.0, sel ? 3.0 : 0.0,
                                    sel ? Color::fromRGB(0.0, 0.0, 0.0) : Color::fromRGBA(0, 0, 0, 0)});
        int idx = i;
        kids.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
            auto* s = w();
            s->setSelected(idx);
            if (s->onChanged())
                s->onChanged()(s->swatches()[idx]);
            setState();
        }));
        kids.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
    }
    auto row = cui::make_shared<Row>(std::move(kids));
    row->setMainAxisSize(MainAxisSize::Min);
    return row;
}

std::unique_ptr<State> ColorPicker::createState() {
    return std::make_unique<ColorPickerState>();
}

SharedPtr<Widget> PaginationState::build(BuildContext&) {
    auto* self = w();
    std::vector<SharedPtr<Widget>> kids;
    for (int i = 1; i <= self->count(); ++i) {
        bool cur = i == self->current();
        auto t = cui::make_shared<Text>(std::to_string(i), TextStyle().size(12.5).bold().fgColor(
                                                                   cur ? theme().textOnPrimary : theme().textPrimary));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(28.0).height(28.0).decoration(
                BoxDecoration{cur ? theme().primary : Color::fromRGBA(0, 0, 0, 0.05), 6.0});
        int idx = i;
        kids.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
            auto* s = w();
            if (idx != s->current()) {
                s->setCurrent(idx);
                if (s->onPage())
                    s->onPage()(idx);
                setState();
            }
        }));
        kids.push_back(cui::make_shared<SizedBox>(4.0, 0.0));
    }
    auto row = cui::make_shared<Row>(std::move(kids));
    row->setMainAxisSize(MainAxisSize::Min);
    return row;
}

std::unique_ptr<State> Pagination::createState() {
    return std::make_unique<PaginationState>();
}

SharedPtr<Widget> PageViewState::build(BuildContext&) {
    auto* self = w();
    int n = static_cast<int>(self->pages().size());
    auto page = cui::make_shared<Container>(self->pages()[self->index()]);
    page->width(320.0).height(150.0).decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.06), 8.0});

    // 左右箭头
    auto prev = cui::make_shared<GestureDetector>(cui::make_shared<Container>(cui::make_shared<Center>(
                                                          cui::make_shared<Text>("‹", TextStyle().size(18.0).bold()))),
                                                  [this] {
                                                      auto* s = w();
                                                      s->setIndex(s->index() - 1);
                                                      setState();
                                                  });
    auto next = cui::make_shared<GestureDetector>(cui::make_shared<Container>(cui::make_shared<Center>(
                                                          cui::make_shared<Text>("›", TextStyle().size(18.0).bold()))),
                                                  [this] {
                                                      auto* s = w();
                                                      s->setIndex(s->index() + 1);
                                                      setState();
                                                  });

    // 指示点
    std::vector<SharedPtr<Widget>> dots;
    for (int i = 0; i < n; ++i) {
        bool on = i == self->index();
        auto d = cui::make_shared<Container>(cui::make_shared<SizedBox>(on ? 14.0 : 8.0, 8.0));
        d->decoration(BoxDecoration{on ? theme().primary : Color::fromRGBA(0, 0, 0, 0.2), 4.0});
        dots.push_back(d);
    }
    auto dotRow = cui::make_shared<Row>(std::move(dots));
    dotRow->setMainAxisSize(MainAxisSize::Min);

    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{prev, page, next});
    row->setMainAxisSize(MainAxisSize::Min);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{row, dotRow});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Center);
    return col;
}

std::unique_ptr<State> PageView::createState() {
    return std::make_unique<PageViewState>();
}

SharedPtr<Widget> NavigationRailState::build(BuildContext&) {
    auto* self = w();
    std::vector<SharedPtr<Widget>> kids;
    for (int i = 0; i < static_cast<int>(self->destinations().size()); ++i) {
        bool sel = i == self->selected();
        const Color iconColor = sel ? theme().primary : theme().textSecondary;
        SharedPtr<Widget> ic;
        if (self->destinations()[i].iconCode != IconCode::None) {
            ic = cui::make_shared<Icon>(self->destinations()[i].iconCode, 16.0, iconColor);
        } else {
            ic = cui::make_shared<Text>(self->destinations()[i].icon, TextStyle().size(16.0).fgColor(iconColor));
        }
        auto lb = cui::make_shared<Text>(self->destinations()[i].label,
                                         TextStyle().size(12.0).fgColor(sel ? theme().primary : theme().textSecondary));
        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{ic, lb});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto c = cui::make_shared<Container>(row);
        c->padding(EdgeInsets::symmetric(10.0, 12.0))
                .decoration(
                        BoxDecoration{sel ? Color::fromRGBA(0.0, 0.45, 0.95, 0.1) : Color::fromRGBA(0, 0, 0, 0), 8.0});
        int idx = i;
        kids.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
            auto* s = w();
            if (idx != s->selected()) {
                s->setSelected(idx);
                if (s->onDestination())
                    s->onDestination()(idx);
                setState();
            }
        }));
        kids.push_back(cui::make_shared<SizedBox>(0.0, 4.0));
    }
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto box = cui::make_shared<Container>(col);
    box->width(170.0)
            .padding(EdgeInsets::all(8.0))
            .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.03), 0.0, 1.0, Color::fromRGBA(0, 0, 0, 0.06)});
    return box;
}

std::unique_ptr<State> NavigationRail::createState() {
    return std::make_unique<NavigationRailState>();
}

SharedPtr<Widget> RatingBarState::build(BuildContext&) {
    auto* self = w();
    std::vector<SharedPtr<Widget>> stars;
    for (int i = 0; i < self->max(); ++i) {
        bool on = i < self->value();
        auto s = cui::make_shared<Text>(
                on ? "★" : "☆",
                TextStyle().size(20.0).fgColor(on ? Color::fromRGB(1.0, 0.75, 0.0) : Color::fromRGB(0.7, 0.7, 0.7)));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(s));
        c->padding(EdgeInsets::symmetric(2.0, 0.0));
        int idx = i + 1;
        stars.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
            auto* s2 = w();
            s2->setValue(idx);
            if (s2->onChanged())
                s2->onChanged()(idx);
            setState();
        }));
    }
    auto row = cui::make_shared<Row>(std::move(stars));
    row->setMainAxisSize(MainAxisSize::Min);
    return row;
}

std::unique_ptr<State> RatingBar::createState() {
    return std::make_unique<RatingBarState>();
}

SharedPtr<Widget> DropdownButtonState::build(BuildContext& context) {
    auto* self = w();
    auto label = cui::make_shared<Text>(self->items().empty() ? "—" : self->items()[self->selected()],
                                        TextStyle().size(13.0).fgColor(theme().textPrimary));
    auto arrow = cui::make_shared<Text>(open_ ? "▲" : "▼", TextStyle().size(12.0));
    auto row =
            cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{label, cui::make_shared<SizedBox>(6.0, 0.0), arrow});
    row->setMainAxisSize(MainAxisSize::Min);
    auto btn = cui::make_shared<Container>(row, buttonKey_);
    btn->padding(EdgeInsets::symmetric(8.0, 12.0))
            .decoration(BoxDecoration{theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.2)});

    std::vector<SharedPtr<Widget>> col;
    col.push_back(cui::make_shared<GestureDetector>(btn, [this] {
        open_ = !open_;
        setState();
    }));
    if (open_ && !self->items().empty()) {
        const TextStyle itemStyle = TextStyle().size(13.0);
        std::vector<SharedPtr<Widget>> opts;
        for (int i = 0; i < static_cast<int>(self->items().size()); ++i) {
            bool sel = i == self->selected();
            TextStyle s = itemStyle;
            s.fgColor(sel ? theme().primary : theme().textPrimary);
            auto t = cui::make_shared<Text>(self->items()[i], s);
            auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
            c->padding(EdgeInsets::symmetric(6.0, 12.0))
                    .decoration(
                            BoxDecoration{sel ? Color::fromRGBA(0.0, 0.45, 0.95, 0.1) : Color::fromRGBA(0, 0, 0, 0)});
            int idx = i;
            opts.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
                auto* s = w();
                s->setSelected(idx);
                if (s->onChanged())
                    s->onChanged()(idx);
                open_ = false;
                setState();
            }));
        }
        auto list = cui::make_shared<Column>(std::move(opts));
        list->setMainAxisSize(MainAxisSize::Min);
        list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto card = cui::make_shared<Container>(list);
        card->width(menuWidth(context, itemStyle))
                .margin(EdgeInsets(0.0, 4.0, 0.0, 0.0))
                .decoration(BoxDecoration{theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.2), 6.0,
                                          Color::fromRGBA(0, 0, 0, 0.2)});
        col.push_back(card);
    }
    auto box = cui::make_shared<Column>(std::move(col));
    box->setMainAxisSize(MainAxisSize::Min);
    box->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return box;
}

std::unique_ptr<State> DropdownButton::createState() {
    return std::make_unique<DropdownButtonState>();
}

SharedPtr<Widget> SearchBarState::build(BuildContext&) {
    auto* self = w();
    const Color kText = theme().textPrimary;
    const Color kMuted = theme().textSecondary;

    auto icon = cui::make_shared<Icon>(IconCode::Search, 15.0, kMuted);
    auto field = cui::make_shared<EditableText>(
            controller_, TextStyle().size(13.0).fgColor(kText), TextAlign::Left, self->placeholder(),
            /*obscureText=*/false, /*autofocus=*/false, nullptr, focusNode_,
            [this](const std::string& text) { onTextChanged(text); }, self->onSubmitted());

    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(std::move(icon));
    kids.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
    kids.push_back(cui::make_shared<Expanded>(1, std::move(field)));
    if (!controller_->text().empty()) {
        // 图标本身不参与命中（RenderIcon 没有 hitTestSelf），所以外包一层有尺寸的
        // 装饰盒子把命中区撑出来 —— 与 ComboBox 的下拉箭头同一口径。
        auto clear = cui::make_shared<Icon>(IconCode::Close, 14.0, kMuted);
        auto clearBox = cui::make_shared<Container>(cui::make_shared<Center>(clear));
        clearBox->width(20.0).height(20.0).decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.06), 10.0});
        kids.push_back(cui::make_shared<GestureDetector>(clearBox, [this] {
            controller_->setText("");
            focusNode_->requestFocus();
        }));
    }
    auto row = cui::make_shared<Row>(std::move(kids));
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto box = cui::make_shared<Container>(row);
    box->padding(EdgeInsets::symmetric(12.0, 8.0))
            .decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.05), 20.0, 1.0, Color::fromRGBA(0, 0, 0, 0.15)});
    return box;
}

void SearchBarState::onTextChanged(const std::string& text) {
    auto* self = w();
    self->setValue(text);
    if (self->onChanged())
        self->onChanged()(text);
    // 清除按钮的显隐跟着文本走。
    setState();
}

void SearchBarState::initState() {
    controller_ = cui::make_shared<TextEditingController>();
    focusNode_ = cui::make_shared<FocusNode>();
    // widget 上预置的 value_ 是初值（挂载前由宿主写的那一种）。
    controller_->setText(w()->value());
}

std::unique_ptr<State> SearchBar::createState() {
    return std::make_unique<SearchBarState>();
}

SharedPtr<Widget> SegmentedControlState::build(BuildContext&) {
    auto* self = w();
    auto* st = this;
    std::vector<SharedPtr<Widget>> kids;
    for (int i = 0; i < static_cast<int>(self->options().size()); ++i) {
        bool sel = i == self->selected();
        bool hov = i == hovered_;
        // 对齐 legacy SegmentedControl::paint：选中白字、否则次要灰 (textSecondary)。
        auto t = cui::make_shared<Text>(
                self->options()[i],
                TextStyle().size(12.5).bold().fgColor(sel ? theme().textOnPrimary : theme().textSecondary));
        // 药丸：选中 primary、悬停 hoverOverlay、否则透明；radiusSmall(4) 圆角。
        auto pill = cui::make_shared<Container>(cui::make_shared<Center>(t));
        pill->padding(EdgeInsets::symmetric(12.0, 4.0));
        Color bg = sel ? Color::fromRGB(0.13, 0.59, 0.95) : (hov ? Color::fromRGBA(0, 0, 0, 0.04) : Color{0, 0, 0, 0});
        pill->decoration(BoxDecoration{bg, 4.0});
        int idx = i;
        auto hoverWrap = cui::make_shared<Listener>(
                nullptr, nullptr, nullptr,
                [st, idx](const PointerEvent&) {
                    if (st->hovered_ != idx) {
                        st->hovered_ = idx;
                        st->setState();
                    }
                },
                nullptr, pill);
        kids.push_back(cui::make_shared<GestureDetector>(hoverWrap, [this, idx] {
            auto* s = w();
            if (idx != s->selected()) {
                s->setSelected(idx);
                if (s->onChanged())
                    s->onChanged()(idx);
                setState();
            }
        }));
    }
    auto row = cui::make_shared<Row>(std::move(kids));
    row->setMainAxisSize(MainAxisSize::Min);
    // 轨道：surfaceVariant 圆角(radiusMedium 6)+2px 内边距，选中药丸天然内缩 2px。
    auto box = cui::make_shared<Container>(row);
    box->padding(EdgeInsets::all(2.0)).decoration(BoxDecoration{theme().surfaceVariant, 6.0});
    return box;
}

std::unique_ptr<State> SegmentedControl::createState() {
    return std::make_unique<SegmentedControlState>();
}

}  // namespace cui_core
