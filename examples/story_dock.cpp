/// @file gallery_dock.cpp — story 与 IDE 合并后的停靠层（实现，对应薄头
/// gallery_dock.h）。
///
/// 从旧 `examples/ide.cpp` 移植进 `cui_core::story` 命名空间。本 .cpp 持有原
/// 头里全部内联实现；对外类型（GalleryDockPanel / IdeLogView / IdeTextBuffer）
/// 的方法以 out-of-line 形式给出，其余内部类/助手仍在本 TU 内联。

#include "story_dock.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/dock/dock.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/theme.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/data.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/popup_menu.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/text.h"

namespace cui {
namespace story {

// ── IdeTextBuffer ───────────────────────────────────────────────────────────

void IdeTextBuffer::setText(std::string s) {
    value = std::move(s);
    notifyListeners();
}
const std::string& IdeTextBuffer::text() const { return value; }
void IdeTextBuffer::append(const std::string& line) {
    value += line;
    value += '\n';
    notifyListeners();
}

SharedPtr<IdeTextBuffer> buildOutput() {
    auto out = cui::make_shared<IdeTextBuffer>();
    out->setText(
        "1>------ Build started: Project: cui_core, Configuration: Debug x64 ------\n"
        "1>  application.cpp\n"
        "1>  docking.cpp\n"
        "1>  dock_layout.cpp\n"
        "1>  widget.cpp\n"
        "1>  painter.cpp\n"
        "1>  theme.cpp\n"
        "1>  Generating Code...\n"
        "1>  cui_core -> build/cui_core\n"
        "1>  story.cpp\n"
        "1>  gallery_shell.h\n"
        "1>  Generating Code...\n"
        "1>  gallery_example -> build/gallery_example\n"
        "========== Build: 1 succeeded, 0 failed, 0 up-to-date, 0 skipped ==========\n");
    return out;
}

// ── GalleryDockPanel ────────────────────────────────────────────────────────

GalleryDockPanel::GalleryDockPanel(const Config& cfg, SharedPtr<Widget> content)
    : cfg_(cfg), content_(std::move(content)) {
    Docking::registerDockable(this);
}
GalleryDockPanel::~GalleryDockPanel() { Docking::unregisterDockable(this); }

std::string GalleryDockPanel::persistentID() const { return cfg_.id; }
std::string GalleryDockPanel::tabText() const { return cfg_.tab; }
SharedPtr<Widget> GalleryDockPanel::content() const { return content_; }
std::string GalleryDockPanel::titleText() const { return cfg_.title; }
std::string GalleryDockPanel::tabTooltip() const { return cfg_.tooltip; }
DockableStyle GalleryDockPanel::style() const { return cfg_.style; }
DockableStyle GalleryDockPanel::autoHideStyle() const { return cfg_.style; }
bool GalleryDockPanel::isClosable() const { return cfg_.closable; }
bool GalleryDockPanel::isAutoHideAllowed() const { return cfg_.autoHide; }
bool GalleryDockPanel::isMinMaxAllowed() const { return cfg_.minMax; }
int GalleryDockPanel::panelType() const { return static_cast<int>(cfg_.type); }

SharedPtr<GalleryDockPanel> makePanel(const GalleryDockPanel::Config& cfg,
                                      SharedPtr<Widget> content) {
    return cui::make_shared<GalleryDockPanel>(cfg, std::move(content));
}

// ── IdeLogView ──────────────────────────────────────────────────────────────

IdeLogView::IdeLogView(SharedPtr<IdeTextBuffer> buffer, KeyPtr key)
    : StatefulWidget(std::move(key)), buffer_(std::move(buffer)) {}

const SharedPtr<IdeTextBuffer>& IdeLogView::buffer() const { return buffer_; }

std::string IdeLogView::toStringShort() const { return "IdeLogView"; }

// ── 面板通用小助手 ───────────────────────────────────────────────────────────

SharedPtr<Widget> ideText(const std::string& s, double size, const Color& color,
                          bool bold = false) {
    TextStyle st = TextStyle().size(size).fgColor(color);
    if (bold)
        st.bold();
    return cui::make_shared<Text>(s, st);
}

SharedPtr<Widget> ideDivider(double height, const Color& color) {
    auto c = cui::make_shared<Container>();
    c->height(height).decoration(BoxDecoration{color});
    return c;
}

/// 面板内滚动容器：SingleChildScrollView 需要一个生命周期等于内容树的
/// ScrollController，所以由这个 StatelessWidget 自己持有。
/// 面板通用滚动容器：垂直 SingleChildScrollView（桌面端自带 Scrollbar）。
///
/// 关键：ScrollController 必须在 State 中持久，不能放在 StatelessWidget 字段里——
/// dock 在指针移动/布局变化时反复重建面板内容 widget，每次重建都会 new 出一个
/// 全新 controller，导致滚动位置每帧被重置为 0（表现为「怎么拖都不滚动」）。
class IdeScrollPaneState;  // 前置声明：createState 需返回其 unique_ptr

class IdeScrollPane : public StatefulWidget {
public:
    explicit IdeScrollPane(SharedPtr<Widget> child, EdgeInsets padding = {},
                            KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)),
          child_(std::move(child)),
          padding_(padding) {}

    std::unique_ptr<State> createState() override;

    const SharedPtr<Widget>& child() const { return child_; }
    const EdgeInsets& padding() const { return padding_; }

    std::string toStringShort() const override { return "IdeScrollPane"; }

private:
    SharedPtr<Widget> child_;
    EdgeInsets padding_;
};

class IdeScrollPaneState : public State {
public:
    SharedPtr<ScrollController> controller_ = cui::make_shared<ScrollController>();

    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = static_cast<const IdeScrollPane*>(widget());
        // SingleChildScrollView 桌面端自带 Scrollbar(controller)，无需再包。
        return cui::make_shared<SingleChildScrollView>(
            controller_.get(), self->child(), Axis::Vertical,
            /*keyboardScroll=*/false, /*reverse=*/false, self->padding());
    }
};

std::unique_ptr<State> IdeScrollPane::createState() {
    return std::make_unique<IdeScrollPaneState>();
}

/// 一列日志文本（每行一个 Text——本仓文本不换行，整段 \n 会画成一行）。
SharedPtr<Widget> buildLogBody(const std::string& text) {
    std::vector<SharedPtr<Widget>> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto t = ideText(line.empty() ? std::string(" ") : line, 12.0,
                         theme().textPrimary);
        lines.push_back(cui::make_shared<Padding>(EdgeInsets::symmetric(0.0, 1.5), t));
    }
    auto col = cui::make_shared<Column>(std::move(lines));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

class IdeLogViewState : public State {
public:
    void initState() override {
        auto* self = this;
        handle_ = logView()->buffer()->addListenerWithHandle([self] { self->setState(); });
    }

    void dispose() override { logView()->buffer()->removeListenerByHandle(handle_); }

    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<SingleChildScrollView>(
            controller_.get(), buildLogBody(logView()->buffer()->text()),
            Axis::Vertical, /*keyboardScroll=*/false);
    }

private:
    IdeLogView* logView() { return static_cast<IdeLogView*>(widget()); }

    int handle_ = 0;
    SharedPtr<ScrollController> controller_ = cui::make_shared<ScrollController>();
};

std::unique_ptr<State> IdeLogView::createState() {
    return std::make_unique<IdeLogViewState>();
}

// ── 主题化表格（对齐 VS Error List 的整宽 + 染色表头）─────────────────────────

struct IdeTableColumn {
    std::string label;
    double width = 100.0;  ///< <= 0 → 弹性列（填满剩余宽度）
};

constexpr double kTableCellPadH = 7.0;
/// 弹性列（Description / Text / Value）在面板变窄时的最小宽度。
constexpr double kTableFlexMinWidth = 120.0;

/// 把列宽从「偏好值」解成「实际值」。面板够宽时定宽列原样、弹性列吃掉剩余；
/// 不够宽时定宽列按比例让位，弹性列保住 kTableFlexMinWidth。
std::vector<double> resolveIdeTableWidths(const std::vector<IdeTableColumn>& cols,
                                          double available) {
    std::vector<double> widths(cols.size());
    double fixedSum = 0.0;
    int flex = -1;
    for (size_t i = 0; i < cols.size(); ++i) {
        widths[i] = cols[i].width;
        if (cols[i].width > 0.0)
            fixedSum += cols[i].width;
        else if (flex < 0)
            flex = static_cast<int>(i);
    }
    if (flex < 0)
        return widths;
    if (available - fixedSum < kTableFlexMinWidth && fixedSum > 0.0) {
        const double budget = std::max(available - kTableFlexMinWidth, 0.0);
        const double scale = budget / fixedSum;
        fixedSum = 0.0;
        for (size_t i = 0; i < cols.size(); ++i) {
            if (cols[i].width <= 0.0)
                continue;
            widths[i] = std::max(cols[i].width * scale, 2.0 * kTableCellPadH + 16.0);
            fixedSum += widths[i];
        }
    }
    widths[flex] = std::max(available - fixedSum, 0.0);
    return widths;
}

SharedPtr<Widget> ideTableCell(const std::string& value, bool header) {
    auto t = ideText(value, header ? 11.5 : 12.0,
                     header ? theme().textSecondary : theme().textPrimary, header);
    auto align = cui::make_shared<Align>(-1.0, 0.0, t);
    auto padded = cui::make_shared<Padding>(EdgeInsets::symmetric(kTableCellPadH, 0.0), align);
    return cui::make_shared<ClipRect>(padded, /*antiAlias=*/false);
}

SharedPtr<Widget> ideTableRow(const std::vector<std::string>& cells,
                              const std::vector<double>& widths, double height,
                              bool header, bool selected = false,
                              std::function<void()> onTap = nullptr) {
    std::vector<SharedPtr<Widget>> kids;
    for (size_t i = 0; i < widths.size(); ++i) {
        SharedPtr<Widget> cell =
            ideTableCell(i < cells.size() ? cells[i] : std::string(), header);
        auto box = cui::make_shared<Container>(cell);
        box->width(widths[i]);
        kids.push_back(box);
    }
    auto row = cui::make_shared<Row>(std::move(kids));
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto box = cui::make_shared<Container>(row);
    box->height(height);
    if (header)
        box->decoration(BoxDecoration{theme().surfaceVariant});
    else if (selected)
        box->decoration(BoxDecoration{theme().primaryLight});
    if (onTap)
        return cui::make_shared<GestureDetector>(box, std::move(onTap));
    return box;
}

/// 表格体（表头 + 数据行）；selected 行高亮，onSelect(rowIdx) 非空时数据行可点击。
SharedPtr<Widget> ideTableBody(const std::vector<IdeTableColumn>& cols,
                               const std::vector<std::vector<std::string>>& rows,
                               int selected,
                               const std::function<void(int)>& onSelect) {
    return cui::make_shared<LayoutBuilder>(
        [cols, rows, selected, onSelect](BuildContext&, const BoxConstraints& c) {
            double available = c.hasBoundedWidth() ? c.maxWidth : 0.0;
            if (available <= 0.0) {
                available = 0.0;
                for (const auto& col : cols)
                    available += col.width > 0.0 ? col.width : 160.0;
            }
            const std::vector<double> widths = resolveIdeTableWidths(cols, available);

            std::vector<SharedPtr<Widget>> kids;
            std::vector<std::string> headers;
            headers.reserve(cols.size());
            for (const auto& col : cols)
                headers.push_back(col.label);
            kids.push_back(ideTableRow(headers, widths, 24.0, /*header=*/true));
            kids.push_back(ideDivider(1.0, theme().border));
            for (size_t i = 0; i < rows.size(); ++i) {
                std::function<void()> tap;
                if (onSelect) {
                    int idx = static_cast<int>(i);
                    auto sel = onSelect;
                    tap = [sel, idx] { sel(idx); };
                }
                kids.push_back(ideTableRow(rows[i], widths, 22.0, /*header=*/false,
                                           static_cast<int>(i) == selected, std::move(tap)));
                kids.push_back(ideDivider(0.5, theme().borderLight));
            }
            auto col = cui::make_shared<Column>(std::move(kids));
            col->setMainAxisSize(MainAxisSize::Min);
            col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
            return col;
        });
}

/// 可点击选中行的 IDE 表格（Watch / Error List / Properties / Find Results 共用）。
class IdeSelectTable : public StatefulWidget {
public:
    IdeSelectTable(std::vector<IdeTableColumn> cols,
                   std::vector<std::vector<std::string>> rows, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), cols_(std::move(cols)), rows_(std::move(rows)) {}

    const std::vector<IdeTableColumn>& cols() const { return cols_; }
    const std::vector<std::vector<std::string>>& rows() const { return rows_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "IdeSelectTable"; }

private:
    std::vector<IdeTableColumn> cols_;
    std::vector<std::vector<std::string>> rows_;
};

class IdeSelectTableState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = this;
        return ideTableBody(table()->cols(), table()->rows(), selected_,
                            [self](int i) { self->selected_ = i; self->setState(); });
    }

private:
    IdeSelectTable* table() { return static_cast<IdeSelectTable*>(widget()); }

    int selected_ = -1;
};

std::unique_ptr<State> IdeSelectTable::createState() {
    return std::make_unique<IdeSelectTableState>();
}

SharedPtr<Widget> ideTable(std::vector<IdeTableColumn> cols,
                           std::vector<std::vector<std::string>> rows) {
    return cui::make_shared<IdeSelectTable>(std::move(cols), std::move(rows));
}

// ── 面板内容（对齐 rui ide.rs builders）──────────────────────────────────────

// 注意：TreeItem 取自 cui_core/widgets/data.h 的 cui_core::TreeItem（在本命名空间内可见），
// 此处不再重定义，避免与 TreeView 的构造签名冲突。

TreeItem ti(std::string id, std::string label, std::vector<TreeItem> children = {}) {
    TreeItem t;
    t.id = std::move(id);
    t.label = std::move(label);
    t.children = std::move(children);
    return t;
}

SharedPtr<Widget> buildSolutionExplorer() {
    std::vector<TreeItem> roots = {ti(
        "sol", "Solution 'cui_core' (1 of 1)",
        {ti("inc", "include/",
            {ti("inc-cui_core", "cui_core/",
                {ti("inc-found", "foundation/",
                    {ti("inc-found-app", "application.h"),
                     ti("inc-found-theme", "theme.h"),
                     ti("inc-found-events", "events.h"),
                     ti("inc-found-notifier", "change_notifier.h")}),
                 ti("inc-wid", "widgets/",
                    {ti("inc-wid-dock", "dock.h"),
                     ti("inc-wid-controls", "controls.h"),
                     ti("inc-wid-data", "data.h"),
                     ti("inc-wid-scroll", "scroll.h"),
                     ti("inc-wid-input", "text_input.h")}),
                 ti("inc-rend", "rendering/",
                    {ti("inc-rend-box", "render_box.h"),
                     ti("inc-rend-sliver", "sliver.h"),
                     ti("inc-rend-icon", "render_icon.h")})})}),
         ti("src", "src/",
            {ti("src-found", "foundation/",
                {ti("src-found-app", "application.cpp"), ti("src-found-theme", "theme.cpp")}),
             ti("src-wid", "widgets/",
                {ti("src-wid-dock", "dock.cpp"), ti("src-wid-sliver", "sliver.cpp"),
                 ti("src-wid-anim", "implicit_animation.cpp")}),
             ti("src-rend", "rendering/",
                {ti("src-rend-box", "render_box.cpp"),
                 ti("src-rend-grid", "sliver_grid.cpp")})}),
         ti("ex", "examples/",
            {ti("ex-story", "story.cpp"), ti("ex-shell", "gallery_shell.h"),
             ti("ex-dock", "gallery_dock.h")}),
         ti("tests", "tests/",
            {ti("tests-dock", "test_dock.cpp"), ti("tests-sliver", "test_sliver.cpp"),
             ti("tests-intrinsic", "test_intrinsic.cpp")}),
         ti("cmake", "CMakeLists.txt"), ti("vcpkg", "vcpkg.json")})};
    return cui::make_shared<IdeScrollPane>(
        cui::make_shared<TreeView>(std::move(roots), /*defaultExpanded=*/true),
        EdgeInsets::symmetric(6.0, 6.0));
}

/// 可点击选中项的 IDE 列表（Toolbox 用）。
class IdeSelectList : public StatefulWidget {
public:
    explicit IdeSelectList(std::vector<std::string> items, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)) {}

    const std::vector<std::string>& items() const { return items_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "IdeSelectList"; }

private:
    std::vector<std::string> items_;
};

class IdeSelectListState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> rows;
        for (size_t i = 0; i < list()->items().size(); ++i) {
            auto t = ideText(list()->items()[i], 12.5, theme().textPrimary);
            auto row = cui::make_shared<Container>(t);
            row->height(22.0)
                .alignment(-1.0, 0.0)
                .padding(EdgeInsets::symmetric(10.0, 0.0));
            if (static_cast<int>(i) == selected_)
                row->decoration(BoxDecoration{theme().primaryLight});
            auto* self = this;
            int idx = static_cast<int>(i);
            rows.push_back(cui::make_shared<GestureDetector>(
                row, [self, idx] { self->selected_ = idx; self->setState(); }));
        }
        auto col = cui::make_shared<Column>(std::move(rows));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        return col;
    }

private:
    IdeSelectList* list() { return static_cast<IdeSelectList*>(widget()); }

    int selected_ = -1;
};

std::unique_ptr<State> IdeSelectList::createState() {
    return std::make_unique<IdeSelectListState>();
}

SharedPtr<Widget> buildToolbox() {
    std::vector<std::string> names = {"Cursor",        "Button",     "Text",
                                      "TextEdit",      "Checkbox",   "Radio",
                                      "Slider",        "DropdownButton", "TreeView",
                                      "Table",         "DockArea",   "Tabs",
                                      "Splitter",      "SingleChildScrollView",
                                      "MenuBar",       "ToolBar"};
    return cui::make_shared<IdeScrollPane>(
        cui::make_shared<IdeSelectList>(std::move(names)), EdgeInsets::symmetric(0.0, 6.0));
}

SharedPtr<Widget> buildProperties() {
    std::vector<IdeTableColumn> cols = {{"Property", 105.0}, {"Value", 0.0}};
    std::vector<std::vector<std::string>> rows = {
        {"Name", "main.cpp"},
        {"Text", "#include <cui_core/..."},
        {"Location", "0, 0"},
        {"Size", "800, 600"},
        {"Font", "Consolas 11"},
        {"ForeColor", "#D4D4D4"},
        {"BackColor", "#1E1E1E"},
        {"Anchor", "Top, Left"},
        {"ReadOnly", "False"},
        {"TabStop", "True"},
    };
    return cui::make_shared<IdeScrollPane>(ideTable(std::move(cols), std::move(rows)));
}

SharedPtr<Widget> buildClassView() {
    std::vector<TreeItem> roots = {ti(
        "ns", "cui_core",
        {ti("cls-dockmgr", "DockManager",
            {ti("dm-dock", "dock(id, region, ratio)"), ti("dm-undock", "undock(id)"),
             ti("dm-float", "floatPanel(id)"), ti("dm-autohide", "autoHide(id, edge)"),
             ti("dm-max", "maximize(id) / restore()"),
             ti("dm-split", "setSplitRatioByPath(path, ratio)"),
             ti("dm-reorder", "reorderTabItem(tabId, from, to)")}),
         ti("cls-docklayout", "DockPanel",
            {ti("dl-paint", "paint(Painter&)"),
             ti("dl-hit", "hitTest(const Point&) : Widget*"),
             ti("dl-mouse", "onMousePress/Move/Release(Point)"),
             ti("dl-mgr", "manager() : DockManager*")}),
         ti("cls-widget", "Widget",
            {ti("w-paint", "paint(Painter&)"), ti("w-arrange", "arrange(const Rect&)"),
             ti("w-hit", "hitTest(const Point&) : Widget*"),
             ti("w-measure", "measure(const Size&) : Size"), ti("w-repaint", "repaint()")}),
         ti("cls-app", "Application",
            {ti("app-run", "run() : int"), ti("app-frame", "renderFrame()"),
             ti("app-create", "createWindow(title, w, h)"),
             ti("app-instance", "instance() : Application*")})})};
    return cui::make_shared<IdeScrollPane>(
        cui::make_shared<TreeView>(std::move(roots), /*defaultExpanded=*/true),
        EdgeInsets::symmetric(6.0, 6.0));
}

SharedPtr<Widget> buildWatch() {
    std::vector<IdeTableColumn> cols = {
        {"Name", 95.0}, {"Value", 95.0}, {"Type", 0.0}};
    std::vector<std::vector<std::string>> rows = {
        {"dock", "0x00007ff...{...}", "shared_ptr<RootDockingPanel>"},
        {"output", "0x00007ff...{...}", "shared_ptr<Widget>"},
        {"panelSolution", "0x00007ff...{...}", "Dockable*"},
        {"panelProps", "0x00007ff...{...}", "Dockable*"},
        {"panelOutput", "0x00007ff...{...}", "Dockable*"},
        {"theme()", "{primary=#F28026...}", "Theme&"},
    };
    return cui::make_shared<IdeScrollPane>(ideTable(std::move(cols), std::move(rows)));
}

SharedPtr<Widget> buildErrorList() {
    std::vector<IdeTableColumn> cols = {
        {"#", 30.0}, {"Code", 64.0}, {"Description", 0.0}, {"File", 150.0}, {"Line", 50.0}};
    std::vector<std::vector<std::string>> rows = {
        {"1", "C2039", "'setTabbed': is not a member of 'DockPanel'", "dock_area.h", "56"},
        {"2", "C4189", "'panelInsp': initialized but not referenced",
         "widget_logic_test.cpp", "222"},
        {"3", "C26495", "Variable 'Node::onTap' is uninitialized.", "modifier.h", "115"},
        {"4", "LNK1105", "cannot close file 'gallery_example.exe'", "gallery_example.exe",
         "0"},
        {"5", "C5046", "'kTabBarH': symbol with internal linkage not defined",
         "dock.cpp", "101"},
    };
    return cui::make_shared<IdeScrollPane>(ideTable(std::move(cols), std::move(rows)));
}

SharedPtr<Widget> buildTerminal() {
    return cui::make_shared<IdeScrollPane>(buildLogBody(
        "$ cmake --preset x64-linux-debug\n"
        "-- Configuring done (1.5s)\n"
        "-- Generating done (0.0s)\n"
        "$ cmake --build build-x64-linux-debug -j8\n"
        "[43/86] Building CXX object CMakeFiles/cui_core.dir/src/dock.cpp.o\n"
        "[86/86] Linking CXX executable gallery_example\n"
        "$ ctest --test-dir build-x64-linux-debug\n"
        "100% tests passed, 0 tests failed out of 4\n"
        "$ ./build-x64-linux-debug/examples/story\n"));
}

SharedPtr<Widget> buildFindResults() {
    std::vector<IdeTableColumn> cols = {
        {"File", 135.0}, {"Line", 46.0}, {"Column", 60.0}, {"Text", 0.0}};
    std::vector<std::vector<std::string>> rows = {
        {"dock.cpp", "174", "1", "Widget* DockPanel::hitTest(const Point& pt) {"},
        {"dock.cpp", "368", "1", "void DockPanel::arrange(const Rect& rect) {"},
        {"dock.h", "24", "1", "class DockManager {"},
        {"gallery_shell.h", "502", "10", "auto dock = make_shared<RootDockingPanel>();"},
    };
    return cui::make_shared<IdeScrollPane>(ideTable(std::move(cols), std::move(rows)));
}

SharedPtr<Widget> buildBuildOutput() {
    return cui::make_shared<IdeScrollPane>(buildLogBody(
        "Build Output (tab in the bottom group)\n"
        "=======================================\n"
        "This panel shares the bottom tab group with Output / Error List /\n"
        "Terminal / Find Results - all managed by the DockManager tree.\n"));
}

// ── 停靠布局（页面列表 + 页面内容 + IDE 面板）────────────────────────────────
//
// 页面内容（content）是中心面板，页面列表（pages）贴在它左侧；再叠上原 IDE
// 的左栈（solution/toolbox）、底部标签组（output/error_list/...）、右栈
// （properties/class_view/watch）。
WindowLayoutBuilder dockGalleryLayoutBuilder() {
    WindowLayoutBuilder b;
    b.dock("content", DockingRegion::Center);
    b.dockRelative("pages", "content", DockingRegion::Left, 0.22);
    b.dockRelative("solution", "pages", DockingRegion::Bottom, 0.35);
    b.dockRelative("toolbox", "solution", DockingRegion::Center, 0.5);
    b.dockRelative("output", "content", DockingRegion::Bottom, 0.35);
    b.dockRelative("error_list", "output", DockingRegion::Center, 0.5);
    b.dockRelative("terminal", "output", DockingRegion::Center, 0.5);
    b.dockRelative("find_results", "output", DockingRegion::Center, 0.5);
    b.dockRelative("build_output", "output", DockingRegion::Center, 0.5);
    b.dockRelative("properties", "content", DockingRegion::Right, 0.25);
    b.dockRelative("class_view", "properties", DockingRegion::Center, 0.5);
    b.dockRelative("watch", "properties", DockingRegion::Center, 0.5);
    return b;
}

void applyInitialDockGalleryLayout() {
    Docking::applyLayout(dockGalleryLayoutBuilder());
    Docking::bringToFront("error_list");
    Docking::bringToFront("properties");
}

/// 持久化文件里必须仍包含全部面板：面板析构会逐个 unregister，退出路径若
/// 还在写盘会把「只剩最后注销的那个面板」的中间态存进去。读到残缺布局时
/// 回退默认布局。
bool dockGalleryLayoutComplete() {
    const char* ids[] = {"content",    "pages",        "solution",     "toolbox",
                         "properties", "class_view",   "watch",        "output",
                         "error_list", "terminal",     "find_results", "build_output"};
    for (const char* id : ids)
        if (!Docking::isDocked(id))
            return false;
    return true;
}

// ── IDE 风格外壳构件（菜单 / 工具条 / 状态条）────────────────────────────────

/// 单个菜单的下拉体（Popup 负责加面板外壳）。
SharedPtr<Widget> buildIdeMenuContent(const std::vector<MenuAction>& actions) {
    std::vector<SharedPtr<Widget>> kids;
    for (const auto& a : actions) {
        if (a.separator) {
            auto sep = cui::make_shared<Container>();
            sep->height(1.0)
                .margin(EdgeInsets::symmetric(3.0, 0.0))
                .decoration(BoxDecoration{theme().borderLight});
            kids.push_back(sep);
            continue;
        }
        auto t = cui::make_shared<Text>(a.label,
                                        TextStyle().size(13.0).fgColor(theme().textPrimary));
        auto c = cui::make_shared<Container>(t);
        c->alignment(-1.0, 0.0).padding(EdgeInsets::symmetric(5.0, 8.0));
        kids.push_back(cui::make_shared<GestureDetector>(c, a.cb));
    }
    auto col = cui::make_shared<Column>(kids);
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    return col;
}

/// 菜单标签宽度（须与 buildIdeMenuBar 里 trigger 的水平内边距一致：9×2=18）。
double ideMenuLabelWidth(const std::string& title) {
    return Painter::measureTextWidth(nullptr, title, 13.0) + 18.0;
}

/// 第 openIndex 个菜单标签相对菜单条左缘的水平偏移，用于把下拉浮层左缘
/// 对齐到标签左缘（Row MainAxisSize::Min、无左内边距，故从 0 累加即可）。
double ideMenuOpenLeft(const std::vector<MenuDef>& defs, int openIndex) {
    double x = 0.0;
    for (int i = 0; i < openIndex && i < static_cast<int>(defs.size()); ++i)
        x += ideMenuLabelWidth(defs[i].title);
    return x;
}

/// 菜单栏：每枚标签一个点击手势。点击后**由调用方（外壳）在覆盖整窗的顶层
/// Stack 里渲染下拉浮层**，而不是用 Popup——Popup 的下拉是自身小 Stack 内的
/// Positioned，会溢出到标签框之下、被后续不透明兄弟（停靠区）绘制覆盖，且落在
/// Popup 盒 size 之外命中不到，顶栏点下去就是「毫无反应」。
SharedPtr<Widget> buildIdeMenuBar(const std::vector<MenuDef>& defs, int openIndex,
                                  const std::function<void(int)>& onLabelTap) {
    std::vector<SharedPtr<Widget>> menus;
    for (int i = 0; i < static_cast<int>(defs.size()); ++i) {
        auto t = cui::make_shared<Text>(defs[i].title,
                                        TextStyle().size(13.0).fgColor(theme().textPrimary));
        auto trigger = cui::make_shared<Container>(t);
        trigger->padding(EdgeInsets::symmetric(9.0, 4.0));
        if (i == openIndex)
            trigger->decoration(BoxDecoration{theme().surface, 4.0});
        int idx = i;
        menus.push_back(cui::make_shared<GestureDetector>(trigger, [onLabelTap, idx] {
            if (onLabelTap) onLabelTap(idx);
        }));
    }
    auto row = cui::make_shared<Row>(menus);
    row->setMainAxisSize(MainAxisSize::Min);
    auto bar = cui::make_shared<Container>(row);
    bar->height(28.0)
        .alignment(-1.0, 0.0)
        .decoration(BoxDecoration{theme().surfaceVariant});
    return bar;
}

/// 下拉浮层：定位到展开标签下方（top=barH）。每个动作执行后回调
/// onItemSelected 让外壳收起浮层。返回已 Positioned 的顶层浮层（越界返回 null）。
SharedPtr<Widget> buildIdeMenuDropdown(const std::vector<MenuDef>& defs, int openIndex,
                                       double barH,
                                       const std::function<void()>& onItemSelected) {
    if (openIndex < 0 || openIndex >= static_cast<int>(defs.size()))
        return nullptr;
    std::vector<SharedPtr<Widget>> rows;
    for (const auto& a : defs[openIndex].actions) {
        if (a.separator) {
            auto sep = cui::make_shared<Container>();
            sep->height(1.0)
                .margin(EdgeInsets::symmetric(3.0, 0.0))
                .decoration(BoxDecoration{theme().borderLight});
            rows.push_back(sep);
            continue;
        }
        auto t = cui::make_shared<Text>(a.label,
                                        TextStyle().size(12.5).fgColor(theme().textPrimary));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::symmetric(6.0, 12.0));
        auto cb = a.cb;
        rows.push_back(cui::make_shared<GestureDetector>(c, [cb, onItemSelected] {
            if (cb) cb();
            if (onItemSelected) onItemSelected();
        }));
    }
    auto list = cui::make_shared<Column>(std::move(rows));
    list->setMainAxisSize(MainAxisSize::Min);
    list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto panel = cui::make_shared<Container>(list);
    panel->width(180.0)
        .decoration(BoxDecoration{theme().surface, 6.0, 1.0, theme().border, 6.0,
                                  Color::fromRGBA(0, 0, 0, 0.2)});
    auto pos = cui::make_shared<Positioned>(panel);
    pos->hasLeft = true;
    pos->left = ideMenuOpenLeft(defs, openIndex);
    pos->hasTop = true;
    pos->top = barH;
    return pos;
}

/// 工具条右侧的解决方案配置下拉（VS 风格 Debug | x64）。
/// 旧实现是一段纯装饰的静态标签（无任何手势），点下去没反应；这里换成
/// PopupMenuButton：点击弹出配置列表，选中后刷新标签并写一行到 Output。
class IdeConfigCombo : public StatefulWidget {
public:
    explicit IdeConfigCombo(std::function<void(const std::string&)> logToOutput,
                            KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), logToOutput_(std::move(logToOutput)) {}

    const std::function<void(const std::string&)>& logger() const { return logToOutput_; }

    /// 候选配置（下拉项与当前标签都取自这里，value = 下标）。
    static const std::vector<std::string>& configs() {
        static const std::vector<std::string> k = {
            "Debug | x64", "Release | x64", "Debug | ARM64", "Release | ARM64"};
        return k;
    }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "IdeConfigCombo"; }

private:
    std::function<void(const std::string&)> logToOutput_;
};

class IdeConfigComboState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        const auto& cfgs = IdeConfigCombo::configs();
        std::vector<PopupMenuItem> items;
        for (size_t i = 0; i < cfgs.size(); ++i)
            items.push_back(PopupMenuItem{cfgs[i], static_cast<int>(i)});

        auto label = cui::make_shared<Text>(cfgs[config_],
                                            TextStyle().size(12.0).fgColor(theme().textPrimary));
        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            label,
            cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)),
            cui::make_shared<Icon>(IconCode::ExpandMore, 14.0, theme().textSecondary)});
        auto box = cui::make_shared<Container>(row);
        box->width(130.0)
            .height(24.0)
            .padding(EdgeInsets::only(8.0, 0.0, 6.0, 0.0))
            .decoration(BoxDecoration{theme().surface, 4.0, 1.0, theme().border});

        auto* self = this;
        auto popup = cui::make_shared<PopupMenuButton>(
            std::move(items),
            [self, this](int v) {
                self->config_ = v;
                if (combo()->logger())
                    combo()->logger()("1> Solution configuration: " +
                                      IdeConfigCombo::configs()[v]);
                self->setState();
            },
            box);
        auto wrap = cui::make_shared<Container>(popup);
        wrap->margin(EdgeInsets::only(0.0, 0.0, 4.0, 0.0));
        return wrap;
    }

private:
    IdeConfigCombo* combo() { return static_cast<IdeConfigCombo*>(widget()); }

    int config_ = 0;
};

std::unique_ptr<State> IdeConfigCombo::createState() {
    return std::make_unique<IdeConfigComboState>();
}

/// 工具条：紧凑 26px 按钮，右侧 Debug 配置组合框。
SharedPtr<Widget> buildIdeToolBar(const std::function<void(const std::string&)>& logToOutput,
                                  const std::function<void()>& onBuild) {
    struct Item {
        const char* label;
        std::function<void()> cb;
        bool separator;
    };
    const std::vector<Item> items = {
        {"New", [logToOutput] { if (logToOutput) logToOutput("1> New document"); }, false},
        {"Open", [logToOutput] { if (logToOutput) logToOutput("1> Open file"); }, false},
        {"Save", [logToOutput] { if (logToOutput) logToOutput("1> Saved"); }, false},
        {nullptr, nullptr, true},
        {"Undo", [logToOutput] { if (logToOutput) logToOutput("1> Undo"); }, false},
        {"Redo", [logToOutput] { if (logToOutput) logToOutput("1> Redo"); }, false},
        {nullptr, nullptr, true},
        {"Build", [onBuild] { if (onBuild) onBuild(); }, false},
        {"Run", [logToOutput] { if (logToOutput) logToOutput("1> Run started: Project: cui_core"); }, false},
    };
    std::vector<SharedPtr<Widget>> kids;
    for (const auto& it : items) {
        if (it.separator) {
            auto s = cui::make_shared<Container>();
            s->width(1.0)
                .height(20.0)
                .margin(EdgeInsets::symmetric(3.0, 0.0))
                .decoration(BoxDecoration{theme().border});
            kids.push_back(s);
            continue;
        }
        auto t = cui::make_shared<Text>(it.label,
                                        TextStyle().size(10.0).fgColor(theme().textPrimary));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->width(32.0)
            .height(26.0)
            .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.05), 3.0});
        auto btn = cui::make_shared<GestureDetector>(c, it.cb);
        auto wrap = cui::make_shared<Container>(btn);
        wrap->margin(EdgeInsets::only(0.0, 0.0, 3.0, 0.0));
        kids.push_back(wrap);
    }
    kids.push_back(cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)));

    // 右侧解决方案配置下拉（原为纯装饰的静态标签，现换成真正的 PopupMenuButton）。
    kids.push_back(buildIdeConfigCombo(logToOutput));

    auto row = cui::make_shared<Row>(kids);
    auto bar = cui::make_shared<Container>(row);
    bar->height(32.0)
        .padding(EdgeInsets::symmetric(4.0, 0.0))
        .decoration(BoxDecoration{theme().surface});
    return bar;
}

SharedPtr<Widget> buildIdeConfigCombo(const std::function<void(const std::string&)>& logToOutput) {
    return cui::make_shared<IdeConfigCombo>(logToOutput);
}

/// 状态条：主色底 + 白色左对齐分段。
SharedPtr<Widget> buildIdeStatusBar(const std::vector<std::string>& segments) {
    std::vector<SharedPtr<Widget>> kids;
    for (size_t i = 0; i < segments.size(); ++i) {
        auto t = cui::make_shared<Text>(segments[i],
                                        TextStyle().size(11.0).fgColor(theme().textOnPrimary));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::symmetric(10.0, 0.0));
        kids.push_back(c);
        if (i + 1 < segments.size()) {
            auto sep = cui::make_shared<Container>();
            sep->width(1.0)
                .height(14.0)
                .decoration(BoxDecoration{Color::fromRGBA(1.0, 1.0, 1.0, 0.25)});
            kids.push_back(sep);
        }
    }
    auto row = cui::make_shared<Row>(kids);
    row->setMainAxisSize(MainAxisSize::Min);
    auto bar = cui::make_shared<Container>(row);
    bar->height(24.0)
        .alignment(-1.0, 0.0)
        .decoration(BoxDecoration{theme().primary});
    return bar;
}

/// Build 动作：往 Output 面板追一段构建日志并前置 Output 标签组。
void ideBuildSolution(SharedPtr<IdeTextBuffer> outputText) {
    if (!outputText)
        return;
    outputText->append("");
    outputText->append("1>------ Build started: Project: cui_core ------");
    outputText->append("1>  Building...");
    outputText->append("1>  cui_core.lib");
    outputText->append("========== Build: 1 succeeded ==========");
    Docking::bringToFront("output");
    std::fprintf(stderr, "[ide] Build > Build Solution\n");
}

}  // namespace story
}  // namespace cui_core
