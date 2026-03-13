/// @file tabs.cpp
/// @brief TabController / TabBar / TabBarView / ExpansionTile / Stepper 实现。

#include "cui/widgets/tabs.h"

#include <algorithm>
#include <numeric>

#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/widgets/implicit_animation.h"

namespace cui {

std::unique_ptr<State> TabBar::createState() { return std::make_unique<TabBarState>(); }
std::unique_ptr<State> TabBarView::createState() { return std::make_unique<TabBarViewState>(); }
std::unique_ptr<State> ExpansionTile::createState() {
    return std::make_unique<ExpansionTileState>();
}

namespace {

/// 取标签文本（TabBar 的 tab 通常是 Text；非 Text 时回退占位）。
std::string tabLabelOf(const SharedPtr<Widget>& w) {
    if (auto* t = dynamic_cast<Text*>(w.get()))
        return t->data();
    return "Tab";
}

}  // namespace

// ── TabBarState ──────────────────────────────────────────────────────────
SharedPtr<Widget> TabBarState::build(BuildContext&) {
    auto* c = tab()->controller();
    int sel = c ? c->index() : 0;
    const Color ind = tab()->indicatorColor();  // 拷贝：lambda 会跨帧持有
    const auto& tabs = tab()->tabs();

    // 估算每个标签宽度：与 cellAt 实际渲染保持一致（常规字重、左右 padding 各 16 =
    // 共 32；选中项字号 14，其余 13）。此前按粗体估算并用错 padding，导致每项多算
    // 十余像素，标签栏尾部留下可再容纳 1~2 个 tab 的空白。
    std::vector<double> widths(tabs.size(), 88.0);
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (auto* t = dynamic_cast<Text*>(tabs[i].get())) {
            const double fs = (static_cast<int>(i) == sel) ? 14.0 : 13.0;
            widths[i] = Painter::measureTextWidth(nullptr, t->data(), fs, 400.0) + 32.0;
        }
    }
    const double total = std::accumulate(widths.begin(), widths.end(), 0.0);

    // 可见标签排一行；放不下的收进右侧「»」下拉列表（对齐 DropdownButton 的内联卡片）。
    return cui::make_shared<LayoutBuilder>(
        [this, sel, ind, widths, total](BuildContext&, const BoxConstraints& con)
            -> SharedPtr<Widget> {
            const bool bounded = std::isfinite(con.maxWidth);
            const double avail = bounded ? con.maxWidth : total;
            const auto& tabs = tab()->tabs();

            const Color primary = ind;  // 选中主色（对齐旧 theme().primary）
            const Color inactiveTxt = theme().textSecondary;
            auto cellAt = [this, sel, ind, primary, inactiveTxt, &tabs](int i)
                             -> SharedPtr<Widget> {
                bool active = i == sel;
                // 文字：选中=primary 且略大（fontSizeBody+1），未选中=textSecondary。
                auto label = cui::make_shared<Text>(
                    tabLabelOf(tabs[i]),
                    TextStyle().size(active ? 14.0 : 13.0)
                        .fgColor(active ? primary : inactiveTxt));
                auto inner = cui::make_shared<Container>(label);
                // symmetric(horizontal, vertical)：左右各 16（共 32，与宽度估算一致），
                // 上下各 10，40 高内文字可正常居中（此前参数顺序写反，垂直方向被压扁）。
                inner->height(40.0).padding(EdgeInsets::symmetric(16.0, 10.0));
                if (active)
                    inner->decoration(BoxDecoration{theme().surface, 6.0});  // 选中卡片
                // 底部指示线（primary，每边缩 8，对齐旧 Active indicator line）。
                std::vector<SharedPtr<Widget>> cellKids{inner};
                if (active) {
                    auto indLine = cui::make_shared<Container>(nullptr);
                    indLine->height(3.0).decoration(BoxDecoration{primary});
                    auto pind = cui::make_shared<Positioned>(indLine);
                    pind->hasBottom = true; pind->bottom = 0.0;
                    pind->hasLeft = true; pind->left = 8.0;
                    pind->hasRight = true; pind->right = 8.0;
                    cellKids.push_back(pind);
                }
                auto box = cui::make_shared<Stack>(cellKids);
                box->setFit(RenderStack::Fit::Loose);
                return cui::make_shared<InkWell>(box, [this, i]() {
                    if (auto* cc = tab()->controller())
                        cc->setIndex(i);
                });
            };

            // 可见窗口（滑动窗口）：
            // 选中项始终落在窗口内；窗口右侧溢出为 rightOverflow（菜单上方），
            // 窗口左侧溢出为 leftOverflow（菜单下方「滚出的 tab」）。
            const double kBtnW = 20.0;  // 下拉按钮宽度（▼ 三角居中，24 足够）；尽量少占用标签空间
            double available = avail - kBtnW;
            int n = static_cast<int>(tabs.size());
            auto fitCount = [&](int start) -> int {
                double acc = 0.0;
                int c = 0;
                for (int i = start; i < n; ++i) {
                    acc += widths[i];
                    if (acc > available) break;
                    ++c;
                }
                return c;
            };
            std::vector<int> vis, leftOverflow, rightOverflow;
            bool hasOverflow;
            int vs, vc;
            if (!bounded || total <= available) {
                vs = 0; vc = n; hasOverflow = false;
            } else {
                int maxFit = fitCount(0);
                if (maxFit == 0) {
                    vs = 0; vc = 0; hasOverflow = true;
                } else if (sel < maxFit) {
                    vs = 0; vc = maxFit; hasOverflow = true;
                } else {
                    // 选中项在后部：滑动窗口，找能包含选中项且尽可能多的窗口。
                    int bestStart = sel, bestCount = 1;
                    for (int s = 0; s <= sel; ++s) {
                        int c = fitCount(s);
                        if (c == 0) continue;
                        if (s + c > sel && c > bestCount) {
                            bestCount = c;
                            bestStart = s;
                        }
                    }
                    vs = bestStart; vc = bestCount; hasOverflow = true;
                }
            }
            for (int j = 0; j < vc; ++j) vis.push_back(vs + j);
            for (int i = 0; i < vs; ++i) leftOverflow.push_back(i);
            for (int i = vs + vc; i < n; ++i) rightOverflow.push_back(i);

            std::vector<SharedPtr<Widget>> row;
            for (int i : vis)
                row.push_back(cellAt(i));
            auto r = cui::make_shared<Row>(row);
            r->setMainAxisSize(MainAxisSize::Min);

            SharedPtr<Widget> content = r;  // 未溢出时即单行标签

            if (hasOverflow) {
            // 折叠项交给上层 Overlay 的浮层菜单（见 buildMenu）：内联画在 40 高的
            // 标签栏 Stack 里会落在所有祖先盒子之外，RenderBox::hitTest 的 size
            // 判定把整棵面板子树挡在命中链之外 ⇒ 菜单项点不到。
            menuRightOverflow_ = rightOverflow;
            menuLeftOverflow_ = leftOverflow;

            // 「»」溢出按钮，接在已显示标签之后。
            auto arrow = cui::make_shared<Text>(
                std::string("\xe2\x96\xbc"),  // ▼ 向下三角形
                TextStyle().size(14.0).fgColor(theme().textSecondary));
            auto btn = cui::make_shared<Container>(cui::make_shared<Center>(arrow));
            btn->width(20.0).height(40.0);  // 三角居中，20 宽即可（减少对标签空间的占用）
            std::vector<SharedPtr<Widget>> rowWithBtn;
            rowWithBtn.push_back(r);
            rowWithBtn.push_back(cui::make_shared<GestureDetector>(btn, [this]() {
                setMenuOpen(!overflowOpen_);
            }));
            auto r2 = cui::make_shared<Row>(rowWithBtn);
            r2->setMainAxisSize(MainAxisSize::Max);  // 撑满标签栏宽度，使 ▼ 落在最右
            r2->setMainAxisAlignment(MainAxisAlignment::SpaceBetween);  // ▼ 推到最右
            content = r2;
            }

            // 标签栏背景（对齐旧：浅灰底 + 底部浅灰分隔线）。
            auto barBg = cui::make_shared<Container>(content);
            barBg->decoration(BoxDecoration{theme().surfaceVariant, 0.0, 1.0,
                                            theme().divider});
            if (bounded)
                barBg->width(con.maxWidth);
            return barBg;
        });
}

void TabBarState::onControllerChanged() {
    if (menuEntry_ && menuEntry_->mounted())
        menuEntry_->markNeedsBuild();  // ✓ 跟着选中项走
    setState();
}

void TabBarState::setMenuOpen(bool open) {
    if (overflowOpen_ == open)
        return;
    overflowOpen_ = open;
    if (overflowOpen_) {
        if (insertMenu())
            menuScroll_->jumpTo(0.0);  // 每次打开都从顶部开始
        else
            overflowOpen_ = false;  // 树上没有上层 Overlay ⇒ 弹不出来，别留下假状态
    } else {
        removeMenu();
    }
    setState();
}

bool TabBarState::insertMenu() {
    if (menuEntry_ && menuEntry_->mounted())
        return true;
    OverlayState* overlay = OverlayState::maybeOf(*context());
    if (!overlay)
        return false;
    if (!menuScroll_)
        menuScroll_ = cui::make_shared<ScrollController>();
    auto* self = this;
    // 捕获一份 controller：菜单子树的生命周期比本 State 长（见 tabs.h 里
    // menuScroll_ 的注释），条目活着 = controller 活着。
    auto scrollKeeper = menuScroll_;
    menuEntry_ = cui::make_shared<OverlayEntry>(
        [self, scrollKeeper](BuildContext& ctx) {
            return self->buildMenu(ctx, *scrollKeeper);
        },
        /*opaque=*/false, /*maintainState=*/false);
    overlay->insert(menuEntry_);
    return true;
}

void TabBarState::removeMenu() {
    if (menuEntry_ && menuEntry_->mounted())
        menuEntry_->remove();
    menuEntry_.reset();
}

Rect TabBarState::barRectInOverlay(BuildContext& entryContext) const {
    Element* e = context() ? context()->element() : nullptr;
    auto* box = e ? dynamic_cast<RenderBox*>(e->findRenderObject()) : nullptr;
    if (!box)
        return Rect{};
    Offset origin = box->localToGlobal(Offset{});
    Element* host = entryContext.element();
    if (auto* roHost = host ? host->ancestorRenderObjectElement() : nullptr) {
        if (auto* stack = dynamic_cast<RenderBox*>(roHost->findRenderObject()))
            origin = stack->globalToLocal(origin);
    }
    return Rect{origin.dx, origin.dy, box->size().width, box->size().height};
}

SharedPtr<Widget> TabBarState::buildMenu(BuildContext& entryContext,
                                         ScrollController& scroll) {
    const Rect bar = barRectInOverlay(entryContext);
    const Color ind = tab()->indicatorColor();
    const auto& tabs = tab()->tabs();
    const int sel = tab()->controller() ? tab()->controller()->index() : 0;
    const double kPanelW = 180.0;

    // 下拉菜单：分隔线「上方」为窗口右侧溢出的 tab，「下方」为左侧滚出的 tab。
    auto optionItem = [this, sel, ind, &tabs](int idx) -> SharedPtr<Widget> {
        bool active = idx == sel;
        Color fg = active ? Color::fromRGB(ind.r, ind.g, ind.b)
                          : theme().textPrimary;
        auto t = cui::make_shared<Text>(
            tabLabelOf(tabs[idx]),
            TextStyle().size(13.0).fgColor(fg));
        // 用 Row(mainAxisSize=Max) 撑满整行；选中项右侧显示 ✓（对齐旧 DropdownMenu 的 checkmark）。
        std::vector<SharedPtr<Widget>> itemKids{t};
        if (active) {
            auto chk = cui::make_shared<Text>(
                std::string("\xe2\x9c\x93"),  // ✓
                TextStyle().size(13.0).fgColor(Color::fromRGB(ind.r, ind.g, ind.b)));
            itemKids.push_back(chk);
        }
        auto row = cui::make_shared<Row>(itemKids);
        row->setMainAxisSize(MainAxisSize::Max);
        row->setMainAxisAlignment(MainAxisAlignment::SpaceBetween);  // 文字左、✓ 右
        auto oc = cui::make_shared<Container>(row);
        oc->padding(EdgeInsets::symmetric(6.0, 12.0))
            .decoration(BoxDecoration{active ? Color::fromRGBA(ind.r, ind.g, ind.b, 0.1)
                                             : Color::fromRGBA(0, 0, 0, 0)});
        return cui::make_shared<GestureDetector>(oc, [this, idx]() {
            if (auto* cc = tab()->controller())
                cc->setIndex(idx);
            setMenuOpen(false);
        });
    };

    std::vector<SharedPtr<Widget>> opts;
    for (int idx : menuRightOverflow_)
        opts.push_back(optionItem(idx));
    // 两侧都溢出时插入分隔线（上=右侧 tab，下=左侧滚出的 tab）。
    if (!menuLeftOverflow_.empty() && !menuRightOverflow_.empty()) {
        // 空 Container 无子节点时宽会塌成 0（RenderDecoratedBox 取
        // constraints.smallest()），用空 Row 撑满宽度才能画出线。
        auto div = cui::make_shared<Container>(
            cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{}));
        div->height(1.0).margin(EdgeInsets::symmetric(4.0, 8.0))
            .decoration(BoxDecoration{theme().divider});
        opts.push_back(div);
    }
    for (int idx : menuLeftOverflow_)
        opts.push_back(optionItem(idx));

    auto list = cui::make_shared<Column>(std::move(opts));
    list->setMainAxisSize(MainAxisSize::Min);
    list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);  // 让分隔线/项撑满宽度
    // 菜单过长时滚动：限制最大高度，超出部分可滚动查看最下方项。
    // scroll 由条目 builder 的捕获保活（见 insertMenu）。
    auto sv = cui::make_shared<SingleChildScrollView>(&scroll, list, Axis::Vertical);
    auto cb = cui::make_shared<ConstrainedBox>(
        BoxConstraints(0.0, BoxConstraints::kInfinity, 0.0, 360.0), sv);
    auto panel = cui::make_shared<Container>(cb);
    // 浮动下拉卡片：固定宽度、白底、细边框 + 轻阴影（与 Popup 一致）。
    panel->width(kPanelW).padding(EdgeInsets::all(4.0))
        .decoration(BoxDecoration{theme().surface, 6.0, 1.0,
                                  theme().border, 12.0,
                                  Color::fromRGBA(0, 0, 0, 0.18)});

    std::vector<SharedPtr<Widget>> layers;
    // 全屏透明屏障：点击面板外任意处收起（上游的 TextFieldTapRegion）。
    auto barrierBox = cui::make_shared<Container>();
    barrierBox->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0)});
    auto barrier = cui::make_shared<GestureDetector>(barrierBox, [this] { setMenuOpen(false); });
    auto fill = cui::make_shared<Positioned>(barrier);
    fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
    layers.push_back(fill);

    auto pos = cui::make_shared<Positioned>(panel);
    pos->hasLeft = true;
    pos->left = std::max(0.0, bar.x + bar.width - kPanelW);  // 右对齐到标签栏右缘
    pos->hasTop = true;
    pos->top = bar.y + bar.height;  // 从标签栏下沿开始，浮在 ▼ 正下方
    layers.push_back(pos);

    return cui::make_shared<Stack>(std::move(layers));
}

// ── ExpansionTileState ───────────────────────────────────────────────────
namespace {
/// 上游 Expansible 的默认 duration（200ms）。
constexpr double kExpansionDurationMs = 200.0;
}  // namespace

SharedPtr<Widget> ExpansionTileState::build(BuildContext&) {
    // 头部：上游是 ListTile —— title 进 Expanded、箭头进 trailing，于是箭头永远
    // 贴在瓦片的右边缘（转 0.5 圈 = 展开）。
    auto chevron = cui::make_shared<AnimatedRotation>(
            expanded_ ? 0.5 : 0.0,
            cui::make_shared<Icon>(IconCode::ExpandMore, 20.0,
                                   theme().textSecondary),
            kExpansionDurationMs);
    auto headerChild = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<Expanded>(1, tile()->title()), std::move(chevron)});
    auto headerBox = cui::make_shared<Container>(headerChild, nullptr);
    headerBox->padding(EdgeInsets::symmetric(16.0, 12.0));
    auto header = cui::make_shared<InkWell>(headerBox, [this] { toggle(); });

    // 主体：Align → Padding → Column（上游 _buildBody）。折叠且动画已收尾时不挂载
    // 任何东西（上游 shouldRemoveBody），所以子项在收起状态下既不占位也不吃命中。
    SharedPtr<Widget> body = cui::make_shared<SizedBox>(0.0, 0.0);
    if (bodyMounted_) {
        auto col = cui::make_shared<Column>(tile()->children());
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(tile()->expandedCrossAxisAlignment());
        Alignment a = tile()->expandedAlignment();
        body = cui::make_shared<Align>(
                a.x, a.y,
                cui::make_shared<Padding>(tile()->childrenPadding(), std::move(col)));
    }
    auto crossFade = cui::make_shared<AnimatedCrossFade>(
            cui::make_shared<SizedBox>(0.0, 0.0), std::move(body),
            expanded_ ? CrossFadeState::ShowSecond : CrossFadeState::ShowFirst,
            kExpansionDurationMs, Easing::Linear, Easing::Linear,
            Easing::EaseInOutCubic, Alignment::topLeft(), Clip::HardEdge, nullptr,
            [this] { onFadeSettled(); });

    std::vector<SharedPtr<Widget>> col;
    col.push_back(header);
    col.push_back(std::move(crossFade));
    auto root = cui::make_shared<Column>(std::move(col));
    // Stretch：头部要铺满整行才有可点的水波纹区域，主体才能按 expandedAlignment
    // 在瓦片宽度内对齐。
    root->setMainAxisSize(MainAxisSize::Min);
    root->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    return root;
}

void ExpansionTileState::toggle() {
    expanded_ = !expanded_;
    if (expanded_)
        bodyMounted_ = true;  // 展开：先把主体挂上，crossFade 才有东西可淡入
    setState();
}

void ExpansionTileState::onFadeSettled() {
    if (expanded_ || !bodyMounted_)
        return;
    bodyMounted_ = false;  // 收起动画结束 ⇒ 摘掉主体（maintainState=false）
    setState();
}

// ── Stepper ──────────────────────────────────────────────────────────────
SharedPtr<Widget> Stepper::build(BuildContext&) {
    std::vector<SharedPtr<Widget>> col;
    for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
        bool done = i < currentStep_;
        bool active = i == currentStep_;
        Color circleColor = (done || active) ? theme().primary
                                            : theme().textDisabled;
        auto circleChild = cui::make_shared<Center>(cui::make_shared<Text>(
            done ? std::string("\xE2\x9C\x93")  // ✓
                 : std::to_string(i + 1),
            TextStyle().fgColor((done || active) ? theme().textOnPrimary
                                                : theme().textPrimary)));
        auto circle = cui::make_shared<Container>(circleChild, nullptr);
        circle->width(28.0);
        circle->height(28.0);
        circle->decoration(BoxDecoration{circleColor, 14.0});

        auto label = steps_[i].title;
        auto row =
                cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{circle, label});
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        col.push_back(row);

        if (active && steps_[i].content)
            col.push_back(steps_[i].content);
    }
    return cui::make_shared<Column>(col);
}

}  // namespace cui_core
