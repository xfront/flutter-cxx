/// @file ide_main.cpp
/// @brief CUI Visual Designer — VS 2026-style IDE with drag-and-drop widget
///        design, property editing, hierarchy tree, and live code generation.
///
/// 全部跑在 单流水线上：`Application::setRoot(IdeShell)`。
///
/// Layout:
///   ┌──────────────────────────────────────────────────────┐
///   │ Menu: File Edit View Design Build Help               │
///   │ ToolBar: New Open Save SaveAs Undo Redo Preview      │
///   ├──────────┬──────────────────────────┬────────────────┤
///   │ Palette  │  Design Canvas           │ Properties     │
///   │          │                          │                │
///   ├──────────┤                          ├────────────────┤
///   │ Hierarchy│  Generated Code          │                │
///   │ Solution │                          │                │
///   ├──────────┴──────────────────────────┴────────────────┤
///   │ Output │ Error List │ Terminal                        │
///   ├──────────────────────────────────────────────────────┤
///   │ Status: Ready | ...                                  │
///   └──────────────────────────────────────────────────────┘

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cui/code_editor/code_editor.h>
#include <cui/dock/dock.h>
#include <cui/foundation/application.h>
#include <cui/foundation/theme.h>
#include <cui/rendering/render_scroll.h>
#include <cui/widgets/basic.h>
#include <cui/widgets/container.h>
#include <cui/widgets/controls.h>
#include <cui/widgets/data.h>
#include <cui/widgets/gestures.h>
#include <cui/widgets/layout.h>
#include <cui/widgets/scroll.h>
#include <cui/widgets/text.h>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "design_codegen.h"
#include "design_codeparser.h"
#include "design_model.h"
#include "design_project.h"
#include "ide/design_canvas.h"
#include "ide/design_properties.h"
#include "ide/design_toolbox.h"
#include "ide/ide_controller.h"
#include "ide/ide_shell.h"

namespace {

using namespace cui;
using namespace cui::designer;

// ── Global designer state ──────────────────────────────────────────────────

static SharedPtr<DesignModel> g_model;
static SharedPtr<IdeController> g_ctl;
static SharedPtr<IdeShellStatus> g_status;
static SharedPtr<IdeShellDialog> g_dialog;

static CodeGenerator g_codegen;
static CodeParser g_parser;
static ProjectManager g_projectMgr;

// ── Panel content buffers ──────────────────────────────────────────────────
// 控件是不可变的：面板内容放进共享 buffer，State 注册 onChanged
// （= setState），任何一处改写 buffer 都会让对应面板重建。

/// 追加式只读文本（Output / Terminal）。
struct LogBuffer {
    std::vector<std::string> lines;
    std::function<void()> onChanged;
    void append(const std::string& s) {
        lines.push_back(s);
        if (onChanged) onChanged();
    }
};

/// 生成代码（按行保存）。
struct CodeBuffer {
    std::vector<std::string> lines;
    std::function<void()> onChanged;
    void setText(const std::string& text) {
        lines.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        if (onChanged) onChanged();
    }
    std::string text() const {
        std::string out;
        for (const auto& l : lines) {
            out += l;
            out += '\n';
        }
        return out;
    }
};

/// 错误列表。
struct ErrorBuffer {
    std::vector<std::vector<std::string>> rows;
    std::function<void()> onChanged;
    void setRows(std::vector<std::vector<std::string>> r) {
        rows = std::move(r);
        if (onChanged) onChanged();
    }
};

/// 解决方案资源管理器（目录树）。
struct TreeBuffer {
    std::vector<TreeItem> roots;
    std::function<void()> onChanged;
    void setRoots(std::vector<TreeItem> r) {
        roots = std::move(r);
        if (onChanged) onChanged();
    }
};

/// 控件层级（扁平行：节点 + 缩进层级）。
struct HierarchyBuffer {
    std::vector<std::pair<DesignNode*, int>> rows;
    std::function<void()> onChanged;
};

static SharedPtr<LogBuffer> g_output;
static SharedPtr<LogBuffer> g_terminal;
static SharedPtr<CodeBuffer> g_code;
static SharedPtr<ErrorBuffer> g_errors;
static SharedPtr<TreeBuffer> g_solution;
static SharedPtr<HierarchyBuffer> g_hierarchy;

static bool g_statusModified = false;
static std::string g_statusFile = "Generated Code";

/// 追加一行到 Output 面板。
static void log(const std::string& line) {
    if (g_output)
        g_output->append(line);
}

static void refreshStatus() {
    if (!g_status) return;
    g_status->left = g_statusModified ? "Modified" : "Ready";
    g_status->segments = {g_statusFile + (g_statusModified ? " [Modified]" : ""),
                          "Ln 1, Col 1", "UTF-8", "C++", "Design"};
    g_status->dot = theme().success;
}

// ── Model → panels ─────────────────────────────────────────────────────────

static void syncModelToCode() {
    if (!g_model || !g_code) return;
    g_code->setText(g_codegen.generate(*g_model));
}

static void syncHierarchy() {
    if (!g_model || !g_hierarchy) return;
    std::vector<std::pair<DesignNode*, int>> rows;
    std::function<void(DesignNode*, int)> walk = [&](DesignNode* node, int depth) {
        if (!node) return;
        if (node != g_model->root())
            rows.push_back({node, depth});
        for (auto& child : node->children)
            walk(child.get(), depth + 1);
    };
    walk(g_model->root(), 0);
    g_hierarchy->rows = std::move(rows);
    if (g_hierarchy->onChanged) g_hierarchy->onChanged();
}

static void onModelChanged() {
    g_statusModified = true;
    syncModelToCode();
    syncHierarchy();
    refreshStatus();
    if (g_ctl)
        g_ctl->syncAll();  // 画布 / 属性面板标脏 + 外壳重建
}

/// 把编辑框里的代码反向解析回模型。
static void syncCodeToModel() {
    if (!g_code || !g_model) return;
    std::vector<CodeParser::ParseError> errors;
    if (g_parser.parse(g_code->text(), *g_model, errors)) {
        if (g_errors) g_errors->setRows({});
        log("[Designer] Code parsed successfully.");
    } else {
        std::vector<std::vector<std::string>> rows;
        log("[Designer] Code parse errors:");
        for (auto& e : errors) {
            rows.push_back({"PARSE", e.message, "Generated Code", std::to_string(e.line)});
            log("  Line " + std::to_string(e.line) + ": " + e.message);
        }
        if (g_errors) g_errors->setRows(std::move(rows));
    }
    if (g_ctl) g_ctl->syncAll();
}

/// 从项目目录刷新解决方案资源管理器。
static void refreshSolutionExplorer() {
    if (!g_solution) return;
    std::vector<TreeItem> roots;
    if (g_projectMgr.hasProject()) {
        auto root = g_projectMgr.browseDirectory(g_projectMgr.projectDir(), 3);
        if (root) {
            std::function<TreeItem(const FileEntry&)> conv =
                [&](const FileEntry& e) -> TreeItem {
                TreeItem it;
                it.id = e.path;
                it.label = e.name;
                for (auto& c : e.children)
                    it.children.push_back(conv(*c));
                return it;
            };
            roots.push_back(conv(*root));
        }
    }
    g_solution->setRoots(std::move(roots));
}

// ── Panel views ────────────────────────────────────────────────────────────

/// 只读日志面板（Output / Terminal）。
class LogView : public StatefulWidget {
public:
    explicit LogView(SharedPtr<LogBuffer> buf, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), buf_(std::move(buf)) {}
    const SharedPtr<LogBuffer>& buffer() const { return buf_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "LogView"; }

private:
    SharedPtr<LogBuffer> buf_;
};

class LogViewState : public State {
public:
    void initState() override {
        view()->buffer()->onChanged = [this] { setState(); };
    }
    void dispose() override { view()->buffer()->onChanged = nullptr; }

    SharedPtr<Widget> build(BuildContext&) override {
        auto buf = view()->buffer();
        // 只保留末尾若干行，避免长日志拖慢重建。
        const size_t kKeep = 400;
        size_t from = buf->lines.size() > kKeep ? buf->lines.size() - kKeep : 0;
        std::string all;
        for (size_t i = from; i < buf->lines.size(); ++i) {
            all += buf->lines[i];
            all += '\n';
        }
        auto t = cui::make_shared<Text>(all, TextStyle().size(12.0).fgColor(theme().textPrimary));
        auto box = cui::make_shared<Container>(t);
        box->padding(EdgeInsets::all(6.0)).alignment(-1.0, -1.0);
        return cui::make_shared<SingleChildScrollView>(&scroll_, box, Axis::Vertical, false);
    }

private:
    LogView* view() { return static_cast<LogView*>(widget()); }
    ScrollController scroll_;
};

inline std::unique_ptr<State> LogView::createState() {
    return std::make_unique<LogViewState>();
}

/// 生成代码面板。
class CodeView : public StatefulWidget {
public:
    explicit CodeView(SharedPtr<CodeBuffer> buf, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), buf_(std::move(buf)) {}
    const SharedPtr<CodeBuffer>& buffer() const { return buf_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "CodeView"; }

private:
    SharedPtr<CodeBuffer> buf_;
};

class CodeViewState : public State {
public:
    void initState() override {
        view()->buffer()->onChanged = [this] { setState(); };
    }
    void dispose() override { view()->buffer()->onChanged = nullptr; }

    SharedPtr<Widget> build(BuildContext&) override {
        auto editor = cui::make_shared<CodeEditor>(view()->buffer()->lines,
                                                            "generated.cpp");
        return cui::make_shared<SingleChildScrollView>(&scroll_, editor, Axis::Vertical, false);
    }

private:
    CodeView* view() { return static_cast<CodeView*>(widget()); }
    ScrollController scroll_;
};

inline std::unique_ptr<State> CodeView::createState() {
    return std::make_unique<CodeViewState>();
}

/// 错误列表面板。
class ErrorView : public StatefulWidget {
public:
    explicit ErrorView(SharedPtr<ErrorBuffer> buf, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), buf_(std::move(buf)) {}
    const SharedPtr<ErrorBuffer>& buffer() const { return buf_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ErrorView"; }

private:
    SharedPtr<ErrorBuffer> buf_;
};

class ErrorViewState : public State {
public:
    void initState() override {
        view()->buffer()->onChanged = [this] { setState(); };
    }
    void dispose() override { view()->buffer()->onChanged = nullptr; }

    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<DataColumn> cols = {
            {"Code", 72.0, false},
            {"Description", 300.0, false},
            {"File", 160.0, false},
            {"Line", 48.0, true},
        };
        return cui::make_shared<DataTable>(std::move(cols),
                                                    view()->buffer()->rows, true);
    }

private:
    ErrorView* view() { return static_cast<ErrorView*>(widget()); }
};

inline std::unique_ptr<State> ErrorView::createState() {
    return std::make_unique<ErrorViewState>();
}

/// 解决方案资源管理器面板。
class TreePanelView : public StatefulWidget {
public:
    explicit TreePanelView(SharedPtr<TreeBuffer> buf, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), buf_(std::move(buf)) {}
    const SharedPtr<TreeBuffer>& buffer() const { return buf_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "TreePanelView"; }

private:
    SharedPtr<TreeBuffer> buf_;
};

class TreePanelViewState : public State {
public:
    void initState() override {
        view()->buffer()->onChanged = [this] { setState(); };
    }
    void dispose() override { view()->buffer()->onChanged = nullptr; }

    SharedPtr<Widget> build(BuildContext&) override {
        auto tree = cui::make_shared<TreeView>(view()->buffer()->roots, true);
        auto box = cui::make_shared<Container>(tree);
        box->padding(EdgeInsets::all(6.0)).alignment(-1.0, -1.0);
        return cui::make_shared<SingleChildScrollView>(&scroll_, box, Axis::Vertical, false);
    }

private:
    TreePanelView* view() { return static_cast<TreePanelView*>(widget()); }
    ScrollController scroll_;
};

inline std::unique_ptr<State> TreePanelView::createState() {
    return std::make_unique<TreePanelViewState>();
}

/// 控件层级面板：点击行 → 选中该节点。
class HierarchyView : public StatefulWidget {
public:
    explicit HierarchyView(SharedPtr<HierarchyBuffer> buf, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), buf_(std::move(buf)) {}
    const SharedPtr<HierarchyBuffer>& buffer() const { return buf_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "HierarchyView"; }

private:
    SharedPtr<HierarchyBuffer> buf_;
};

class HierarchyViewState : public State {
public:
    void initState() override {
        view()->buffer()->onChanged = [this] { setState(); };
    }
    void dispose() override { view()->buffer()->onChanged = nullptr; }

    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> rows;
        for (auto& entry : view()->buffer()->rows) {
            DesignNode* node = entry.first;
            int depth = entry.second;
            std::string label = node->widgetType + " (" + node->id + ")";
            auto t = cui::make_shared<Text>(label,
                                            TextStyle().size(12.0).fgColor(theme().textPrimary));
            auto c = cui::make_shared<Container>(t);
            c->padding(EdgeInsets::symmetric(3.0, 6.0 + depth * 14.0)).alignment(-1.0, 0.0);
            if (g_model && g_model->isSelected(node))
                c->decoration(BoxDecoration{theme().primaryLight});
            rows.push_back(cui::make_shared<GestureDetector>(c, [node] {
                if (g_model) g_model->select(node);
            }));
        }
        auto col = cui::make_shared<Column>(std::move(rows));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        return cui::make_shared<SingleChildScrollView>(&scroll_, col, Axis::Vertical, false);
    }

private:
    HierarchyView* view() { return static_cast<HierarchyView*>(widget()); }
    ScrollController scroll_;
};

inline std::unique_ptr<State> HierarchyView::createState() {
    return std::make_unique<HierarchyViewState>();
}

// ── IdePanel — dockable wrapper ────────────────────────────────────────────

class IdePanel : public Dockable {
public:
    struct Config {
        std::string id;
        std::string tab;
        std::string title;
        std::string tooltip;
        DockableStyle style = DockableStyle::Both;
        bool closable = true;
        bool autoHide = true;
        bool minMax = true;
        int type = 0;
    };

    IdePanel(const Config& cfg, SharedPtr<Widget> content)
        : cfg_(cfg), content_(std::move(content)) {
        Docking::registerDockable(this);
    }
    ~IdePanel() override { Docking::unregisterDockable(this); }

    std::string persistentID() const override { return cfg_.id; }
    std::string tabText() const override { return cfg_.tab; }
    SharedPtr<Widget> content() const override { return content_; }
    std::string titleText() const { return cfg_.title; }
    std::string tabTooltip() const { return cfg_.tooltip; }
    DockableStyle style() const override { return cfg_.style; }
    DockableStyle autoHideStyle() const override { return cfg_.style; }
    bool isClosable() const override { return cfg_.closable; }
    bool isAutoHideAllowed() const override { return cfg_.autoHide; }
    bool isMinMaxAllowed() const override { return cfg_.minMax; }
    int type() const { return cfg_.type; }

private:
    Config cfg_;
    SharedPtr<Widget> content_;
};

SharedPtr<IdePanel> makePanel(const IdePanel::Config& cfg,
                                    SharedPtr<Widget> content) {
    return cui::make_shared<IdePanel>(cfg, std::move(content));
}

void applyInitialLayout() {
    WindowLayoutBuilder b;
    b.dock("design", DockingRegion::Center);
    b.dock("properties", "design", DockingRegion::Right, 0.2);
    b.dock("code", "design", DockingRegion::Bottom, 0.25);
    b.dock("palette", "design", DockingRegion::Left, 0.15);
    b.dock("hierarchy", "palette", DockingRegion::Bottom, 0.5);
    b.dock("solution", "hierarchy", DockingRegion::Bottom, 0.5);
    b.dock("output", "code", DockingRegion::Center);
    b.dock("error_list", "output", DockingRegion::Center);
    b.dock("terminal", "output", DockingRegion::Center);
    Docking::applyLayout(b);
    Docking::bringToFront("properties");
    Docking::bringToFront("output");
    Docking::bringToFront("solution");
}

// ── Path input dialog (modal, replaces legacy Dialog + TextField) ────

using PathCallback = std::function<void(const std::string&)>;

/// 回调签名与 legacy 一致：取消时回传空串。
static void showPathDialog(const std::string& title, const std::string& prompt,
                           const std::string& defaultPath, PathCallback callback) {
    if (g_dialog)
        g_dialog->show(title, prompt, defaultPath, std::move(callback));
}

// ── Menu bar ───────────────────────────────────────────────────────────────

struct MenuAction {
    std::string label;
    std::function<void()> cb;
    bool separator = false;
};

struct MenuDef {
    std::string title;
    std::vector<MenuAction> actions;
};

/// 单个菜单的下拉内容（Popup 负责外框）。
SharedPtr<Widget> buildMenuContent(const std::vector<MenuAction>& actions) {
    std::vector<SharedPtr<Widget>> kids;
    for (const auto& a : actions) {
        if (a.separator) {
            auto sep = cui::make_shared<Container>();
            sep->height(1.0)
                .margin(EdgeInsets::symmetric(3.0, 0.0))
                .decoration(BoxDecoration{theme().divider});
            kids.push_back(sep);
            continue;
        }
        auto t = cui::make_shared<Text>(a.label,
                                        TextStyle().size(12.5).fgColor(theme().textPrimary));
        auto row = cui::make_shared<Container>(t);
        row->padding(EdgeInsets::symmetric(5.0, 12.0)).alignment(-1.0, 0.0);
        kids.push_back(cui::make_shared<GestureDetector>(row, a.cb));
    }
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    return col;
}

SharedPtr<Widget> buildMenuBar(const std::vector<MenuDef>& defs) {
    std::vector<SharedPtr<Widget>> menus;
    for (const auto& d : defs) {
        auto t = cui::make_shared<Text>(d.title,
                                        TextStyle().size(13.0).fgColor(theme().textPrimary));
        auto trigger = cui::make_shared<Container>(t);
        trigger->padding(EdgeInsets::symmetric(4.0, 10.0));
        menus.push_back(
            cui::make_shared<Popup>(trigger, buildMenuContent(d.actions), d.title));
    }
    auto row = cui::make_shared<Row>(std::move(menus));
    row->setMainAxisSize(MainAxisSize::Min);
    auto bar = cui::make_shared<Container>(row);
    bar->height(30.0).alignment(-1.0, 0.0).decoration(BoxDecoration{theme().surfaceVariant});
    return bar;
}

std::vector<MenuDef> buildMenuDefs(Application& app) {
    std::vector<MenuDef> defs;

    // ── File ────────────────────────────────────────────────────────────
    MenuDef file;
    file.title = "File";
    file.actions = {
        {"New Design", [] {
            if (!g_model) return;
            while (!g_model->root()->children.empty())
                g_model->removeNode(g_model->root()->children.back().get());
            g_model->clearSelection();
            log("[Designer] New design");
        }},
        {"New Project...", [] {
            showPathDialog("New Project", "Project directory:", "/tmp/myproject",
                           [](const std::string& path) {
                               if (path.empty()) return;
                               g_projectMgr.setModel(g_model.get());
                               if (g_projectMgr.newProject(path)) {
                                   refreshSolutionExplorer();
                                   log("[Project] New project: " + path);
                               }
                           });
        }},
        {"Open Project...", [] {
            showPathDialog("Open Project", "Project directory:", "/tmp",
                           [](const std::string& path) {
                               if (path.empty()) return;
                               g_projectMgr.setModel(g_model.get());
                               if (g_projectMgr.openProject(path)) {
                                   refreshSolutionExplorer();
                                   syncHierarchy();
                                   syncModelToCode();
                                   log("[Project] Opened: " + path);
                               } else {
                                   refreshSolutionExplorer();
                                   log("[Project] Opened directory (no design file): " + path);
                               }
                           });
        }},
        {"", {}, true},
        {"Save", [] {
            g_projectMgr.setModel(g_model.get());
            if (g_projectMgr.hasProject()) {
                if (g_projectMgr.saveProject()) {
                    g_statusModified = false;
                    refreshStatus();
                    log("[Project] Saved: " + g_projectMgr.projectFile());
                }
            } else {
                showPathDialog("Save Project As", "Save to file:",
                               "/tmp/myproject/myproject.cuidesign",
                               [](const std::string& path) {
                                   if (path.empty()) return;
                                   g_projectMgr.setModel(g_model.get());
                                   if (g_projectMgr.saveProjectAs(path)) {
                                       g_statusModified = false;
                                       refreshStatus();
                                       log("[Project] Saved: " + path);
                                   }
                               });
            }
        }},
        {"Save As...", [] {
            showPathDialog("Save Project As", "Save to file:",
                           "/tmp/myproject/myproject.cuidesign",
                           [](const std::string& path) {
                               if (path.empty()) return;
                               g_projectMgr.setModel(g_model.get());
                               if (g_projectMgr.saveProjectAs(path)) {
                                   g_statusModified = false;
                                   refreshStatus();
                                   log("[Project] Saved: " + path);
                               }
                           });
        }},
        {"", {}, true},
        {"Export Code...", [] {
            if (!g_model) return;
            std::string code = g_codegen.generate(*g_model);
            std::string defaultPath = g_projectMgr.hasProject()
                                          ? g_projectMgr.projectDir() + "/main.cpp"
                                          : "/tmp/main.cpp";
            showPathDialog("Export Code", "Export to file:", defaultPath,
                           [](const std::string& path) {
                               if (path.empty() || !g_model) return;
                               g_projectMgr.setModel(g_model.get());
                               if (g_projectMgr.exportCode(g_codegen.generate(*g_model), path)) {
                                   refreshSolutionExplorer();
                                   log("[Designer] Code exported to: " + path);
                               }
                           });
        }},
        {"", {}, true},
        {"Exit", [&app] { app.quit(); }},
    };
    defs.push_back(std::move(file));

    // ── Edit ────────────────────────────────────────────────────────────
    MenuDef edit;
    edit.title = "Edit";
    edit.actions = {
        {"Undo", [] { log("[Designer] Undo"); }},
        {"Redo", [] { log("[Designer] Redo"); }},
        {"", {}, true},
        {"Delete Selected", [] {
            if (g_ctl) g_ctl->deleteSelected();
        }},
        {"Select All", [] {
            if (!g_model) return;
            for (auto* node : g_model->flatten())
                if (node != g_model->root()) g_model->addToSelection(node);
        }},
    };
    defs.push_back(std::move(edit));

    // ── View ────────────────────────────────────────────────────────────
    MenuDef view;
    view.title = "View";
    view.actions = {
        {"Widget Palette", [] { Docking::display("palette"); }},
        {"Properties", [] { Docking::display("properties"); }},
        {"Widget Tree", [] { Docking::display("hierarchy"); }},
        {"", {}, true},
        {"Design Canvas", [] { Docking::display("design"); }},
        {"Code Editor", [] { Docking::display("code"); }},
        {"", {}, true},
        {"Output", [] { Docking::display("output"); }},
        {"Error List", [] { Docking::display("error_list"); }},
        {"Terminal", [] { Docking::display("terminal"); }},
        {"Solution Explorer", [] { Docking::display("solution"); }},
        {"", {}, true},
        {"Reset Layout", [] {
            applyInitialLayout();
            log("[Layout] Reset to default");
        }},
    };
    defs.push_back(std::move(view));

    // ── Design ──────────────────────────────────────────────────────────
    MenuDef design;
    design.title = "Design";
    design.actions = {
        {"Parse Code → Canvas", [] { syncCodeToModel(); }},
        {"Canvas → Generate Code", [] { syncModelToCode(); }},
        {"", {}, true},
        {"Toggle Preview", [] {
            if (!g_ctl) return;
            bool on = !g_ctl->previewMode();
            g_ctl->setPreviewMode(on);
            log(std::string("[Designer] Preview ") + (on ? "ON" : "OFF"));
        }},
        {"", {}, true},
        // 主题子菜单展平成一组动作（Popup 不支持嵌套）。
        {"Theme: IDE (VS 2026)",
         [&app] { theme() = Theme::ide(); app.scheduleRender(); }},
        {"Theme: Ocean", [&app] { theme() = Theme::ocean(); app.scheduleRender(); }},
        {"Theme: Forest", [&app] { theme() = Theme::forest(); app.scheduleRender(); }},
        {"Theme: Dark", [&app] { theme() = Theme::dark(); app.scheduleRender(); }},
        {"Theme: Light", [&app] { theme() = Theme::light(); app.scheduleRender(); }},
    };
    defs.push_back(std::move(design));

    // ── Build ───────────────────────────────────────────────────────────
    MenuDef build;
    build.title = "Build";
    build.actions = {
        {"Build Solution", [] {
            log("1>------ Build started: Project: design, Configuration: Debug x64 ------");
            log("1>  codegen.cpp");
            log("1>  design linked.");
            log("1>  1 succeeded.");
            Docking::bringToFront("output");
        }},
    };
    defs.push_back(std::move(build));

    // ── Help ────────────────────────────────────────────────────────────
    MenuDef help;
    help.title = "Help";
    help.actions = {
        {"About CUI Designer", [] { log("[Designer] CUI Visual Designer v1.0"); }},
    };
    defs.push_back(std::move(help));

    return defs;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    using namespace cui;
    using namespace cui::designer;

    Application app;
    app.createWindow("CUI Visual Designer", 1400, 900);

    theme() = Theme::ide();

    // ── Model + controller ──────────────────────────────────────────────
    g_model = cui::make_shared<DesignModel>();
    g_ctl = cui::make_shared<IdeController>(g_model);
    g_model->setOnChanged([] { onModelChanged(); });

    // ── Panel buffers ───────────────────────────────────────────────────
    g_output = cui::make_shared<LogBuffer>();
    g_output->append("[Designer] CUI Visual Designer ready.");
    g_output->append("[Designer] Drag widgets from the palette onto the canvas.");

    g_terminal = cui::make_shared<LogBuffer>();
    g_terminal->append("$ cmake --preset x64-linux-debug");
    g_terminal->append("-- Configuring done (0.8s)");
    g_terminal->append("$ cmake --build build-x64-linux-debug -j8");
    g_terminal->append("[100%] Linking CXX executable ide_example");
    g_terminal->append("$ ./build-x64-linux-debug/ide_example");

    g_code = cui::make_shared<CodeBuffer>();
    g_errors = cui::make_shared<ErrorBuffer>();
    g_solution = cui::make_shared<TreeBuffer>();
    g_hierarchy = cui::make_shared<HierarchyBuffer>();
    syncModelToCode();
    syncHierarchy();

    // ── Docking ─────────────────────────────────────────────────────────
    Docking::initialize();
    auto dock = cui::make_shared<RootDockingPanel>();

    auto panelDesign = makePanel(
        {"design", "Design", "Design Canvas", "Visual widget designer",
         DockableStyle::Both, false, false, true, 0},
        cui::make_shared<DesignCanvas>(g_ctl));

    auto panelPalette = makePanel(
        {"palette", "Palette", "Widget Palette", "Drag widgets to canvas",
         DockableStyle::Horizontal, true, true, true, 1},
        cui::make_shared<DesignToolbox>(g_ctl));

    auto panelProperties = makePanel(
        {"properties", "Properties", "Properties", "Widget properties",
         DockableStyle::Horizontal, true, true, true, 2},
        cui::make_shared<DesignProperties>(g_ctl));

    auto panelCode = makePanel(
        {"code", "Code", "Generated Code", "Live C++ code output",
         DockableStyle::Both, true, true, true, 0},
        cui::make_shared<CodeView>(g_code));

    auto panelHierarchy = makePanel(
        {"hierarchy", "Hierarchy", "Widget Tree", "Widget hierarchy",
         DockableStyle::Horizontal, true, true, true, 1},
        cui::make_shared<HierarchyView>(g_hierarchy));

    auto panelSolution = makePanel(
        {"solution", "Solution", "Solution Explorer", "Project files",
         DockableStyle::Horizontal, true, true, true, 1},
        cui::make_shared<TreePanelView>(g_solution));

    auto panelOutput = makePanel(
        {"output", "Output", "Output", "Designer output",
         DockableStyle::Vertical, true, true, true, 3},
        cui::make_shared<LogView>(g_output));

    auto panelErrors = makePanel(
        {"error_list", "Error List", "Error List", "Parse errors",
         DockableStyle::Vertical, true, true, true, 3},
        cui::make_shared<ErrorView>(g_errors));

    auto panelTerminal = makePanel(
        {"terminal", "Terminal", "Terminal", "Integrated terminal",
         DockableStyle::Vertical, true, true, true, 3},
        cui::make_shared<LogView>(g_terminal));

    // Keep the panels alive for the whole application lifetime.
    std::vector<SharedPtr<IdePanel>> panels = {
        panelDesign, panelPalette, panelProperties, panelCode, panelHierarchy,
        panelSolution, panelOutput, panelErrors, panelTerminal};

    applyInitialLayout();

    // ── Shell: menu bar + dock + status bar ─────────────────────────────
    auto menuBar = buildMenuBar(buildMenuDefs(app));

    IdeShellActions actions;
    actions.onNew = [] {
        if (!g_model) return;
        while (!g_model->root()->children.empty())
            g_model->removeNode(g_model->root()->children.back().get());
        g_model->clearSelection();
        log("[Designer] New design");
    };
    actions.onOpen = [] {
        showPathDialog("Open Project", "Project directory:", "/tmp",
                       [](const std::string& path) {
                           if (path.empty()) return;
                           g_projectMgr.setModel(g_model.get());
                           g_projectMgr.openProject(path);
                           refreshSolutionExplorer();
                           syncHierarchy();
                           syncModelToCode();
                           log("[Project] Opened: " + path);
                       });
    };
    actions.onSave = [] {
        g_projectMgr.setModel(g_model.get());
        if (g_projectMgr.hasProject()) {
            if (g_projectMgr.saveProject()) {
                g_statusModified = false;
                refreshStatus();
                log("[Project] Saved: " + g_projectMgr.projectFile());
            }
        } else {
            showPathDialog("Save Project As", "Save to file:",
                           "/tmp/myproject/myproject.cuidesign",
                           [](const std::string& path) {
                               if (path.empty()) return;
                               g_projectMgr.setModel(g_model.get());
                               g_projectMgr.saveProjectAs(path);
                               g_statusModified = false;
                               refreshStatus();
                               log("[Project] Saved: " + path);
                           });
        }
    };
    actions.onSaveAs = [] {
        showPathDialog("Save Project As", "Save to file:",
                       "/tmp/myproject/myproject.cuidesign",
                       [](const std::string& path) {
                           if (path.empty()) return;
                           g_projectMgr.setModel(g_model.get());
                           g_projectMgr.saveProjectAs(path);
                           g_statusModified = false;
                           refreshStatus();
                           log("[Project] Saved: " + path);
                       });
    };
    actions.onUndo = [] { log("[Designer] Undo"); };
    actions.onRedo = [] { log("[Designer] Redo"); };

    auto body = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        menuBar, cui::make_shared<Expanded>(1, dock)});
    body->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

    g_status = cui::make_shared<IdeShellStatus>();
    refreshStatus();

    auto shell = cui::make_shared<IdeShell>(g_ctl, body, actions);
    shell->setStatus(g_status);
    g_dialog = shell->dialog();

    app.setRoot(shell);

    std::fprintf(stderr, "[designer] starting — CUI Visual Designer\n");
    app.renderFrame();

    app.runAsync([]() { return 0; },
                 [&](int) { app.saveScreenshot("/tmp/designer-shot.png"); });

    return app.run();
}
