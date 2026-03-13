#pragma once
/// @file ide/ide_shell.h
/// @brief 可视化设计器的 上游 根控件（IdeShell）。
///
/// 组成：工具条 + 主体（由外部传入，通常是 RootDockingPanel）+ 状态栏，
/// 并接好 IdeController 的：
///   - canvas / properties 注册（由各 RenderBox::setController 自行完成）；
///   - 画布 / 属性面板重绘钩子 + 模型变更钩子（后者触发外壳重建）；
///   - 选中变化钩子（刷新状态栏 / 依赖选中的面板）；
///   - 工具箱 → 画布的跨面板拖放：外壳用一层覆盖全窗口的 Listener 接管
///     move / up，无需额外遮罩，也无需拖拽期间重建整树；
///   - 键盘路由：Focus(FocusNode) 的 onKeyEvent 转发给 IdeController::handleKey
///     （属性面板编辑时喂给面板，否则 Delete/Backspace 删除选中节点）；
///   - 模态路径输入对话框（替代 legacy Dialog + TextField）。
///
/// 本头只依赖控件层与 designer 逻辑层，绝不引入 legacy 控件头。

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/controls.h"
#include "cui/framework/focus.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/layout.h"
#include "cui/framework/listener.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"
#include "ide/design_canvas.h"
#include "ide/design_properties.h"
#include "ide/design_toolbox.h"
#include "ide/ide_controller.h"

namespace cui {
namespace designer {

/// 外壳工具条暴露给上层（ide_main）的项目级动作。
struct IdeShellActions {
    std::function<void()> onNew;
    std::function<void()> onOpen;
    std::function<void()> onSave;
    std::function<void()> onSaveAs;
    std::function<void()> onUndo;
    std::function<void()> onRedo;
};

/// 状态栏数据。由 ide_main 持有并就地改写，外壳重建时读取最新值。
struct IdeShellStatus {
    std::string left = "Ready";
    std::vector<std::string> segments;
    std::optional<Color> dot = std::nullopt;
};

/// 模态路径输入对话框的状态（替代 legacy Dialog + TextField）。
///
/// ide_main 调用 show() 打开；外壳 build() 发现 open 时独占渲染对话框；
/// OK / Cancel 经 close() 把结果回传给回调。
class IdeShellDialog {
public:
    bool open = false;
    std::string title;
    std::string prompt;
    SharedPtr<TextEditingController> editor;
    std::function<void(const std::string&)> onResult;

    /// 由外壳 State 注入：触发外壳重建（打开 / 关闭都要重绘）。
    std::function<void()> onChanged;

    void show(std::string title, std::string prompt, std::string initial,
              std::function<void(const std::string&)> cb) {
        this->title = std::move(title);
        this->prompt = std::move(prompt);
        onResult = std::move(cb);
        editor = cui::make_shared<TextEditingController>(std::move(initial));
        open = true;
        if (onChanged) onChanged();
    }

    /// @param accept true → 回传编辑框内容；false → 回传空串（取消）。
    void close(bool accept) {
        if (!open) return;
        open = false;
        auto cb = onResult;
        onResult = nullptr;
        std::string value = editor ? editor->text() : std::string();
        if (onChanged) onChanged();
        if (cb) cb(accept ? value : std::string());
    }
};

/// 设计器外壳：工具条 + 主体 + 状态栏（+ 模态对话框）。
class IdeShell : public StatefulWidget {
public:
    IdeShell(SharedPtr<IdeController> controller, SharedPtr<Widget> body,
             IdeShellActions actions = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), controller_(std::move(controller)),
          body_(std::move(body)), actions_(std::move(actions)) {}

    SharedPtr<IdeController> controller() const { return controller_; }
    const SharedPtr<Widget>& body() const { return body_; }
    const IdeShellActions& actions() const { return actions_; }

    /// 绑定状态栏数据（由上层持有，外壳每次 build 读取最新值）。
    void setStatus(SharedPtr<IdeShellStatus> s) { status_ = std::move(s); }
    const SharedPtr<IdeShellStatus>& status() const { return status_; }

    /// 模态对话框（始终非空；上层调用 show() 打开）。
    const SharedPtr<IdeShellDialog>& dialog() const { return dialog_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "IdeShell"; }

private:
    SharedPtr<IdeController> controller_;
    SharedPtr<Widget> body_;
    IdeShellActions actions_;
    SharedPtr<IdeShellStatus> status_;
    SharedPtr<IdeShellDialog> dialog_ = cui::make_shared<IdeShellDialog>();
};

class IdeShellState : public State {
public:
    void initState() override {
        auto ctl = w()->controller();
        focusNode_ = cui::make_shared<FocusNode>();
        focusNode_->onKeyEvent = [ctl](const KeyEvent& ev) {
            ctl->handleKey(ev);
            return KeyEventResult::Handled;
        };
        // 画布 / 属性面板重绘：拖拽/移动中高频调用，只标脏不重建。
        ctl->setOnCanvasDirty([ctl] {
            if (auto* c = ctl->canvas())
                c->markDirty();
            if (auto* p = ctl->properties())
                p->markDirty();
        });
        // 模型变更：重建整棵外壳（选中 / 删除 / 属性变更后）。
        ctl->setOnModelChanged([this] { setState(); });
        // 选中变化：属性面板标脏 + 外壳重建（状态栏随之刷新）。
        ctl->setOnSelectionChanged([this, ctl] {
            if (auto* p = ctl->properties())
                p->markDirty();
            setState();
        });
        // 对话框开关 → 外壳重建。
        if (auto d = w()->dialog())
            d->onChanged = [this] { setState(); };
    }

    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = w();
        auto ctl = self->controller();

        // ── 模态对话框：打开时独占整棵子树 ──────────────────────────────
        if (auto d = self->dialog()) {
            if (d->open)
                return buildDialog(d);
        }

        // ── 工具条 ──────────────────────────────────────────────────────
        std::vector<SharedPtr<Widget>> btns;
        auto addBtn = [&](const std::string& label, std::function<void()> cb) {
            if (!cb)
                return;
            btns.push_back(toolButton(label, std::move(cb)));
        };
        const auto& acts = self->actions();
        addBtn("New", acts.onNew);
        addBtn("Open", acts.onOpen);
        addBtn("Save", acts.onSave);
        addBtn("Save As", acts.onSaveAs);
        addBtn("Undo", acts.onUndo);
        addBtn("Redo", acts.onRedo);
        addBtn(ctl->previewMode() ? "Edit" : "Preview", [ctl] { ctl->togglePreview(); });

        auto toolRow = cui::make_shared<Row>(btns);
        toolRow->setMainAxisSize(MainAxisSize::Min);
        auto toolBar = cui::make_shared<Container>(toolRow);
        toolBar->height(34.0)
            .alignment(-1.0, 0.0)
            .padding(EdgeInsets::symmetric(0.0, 4.0))
            .decoration(BoxDecoration{theme().surface});

        // ── 主体（通常是停靠面板树）────────────────────────────────────
        auto body = self->body();
        if (!body)
            body = cui::make_shared<SizedBox>(0.0, 0.0);

        // ── 状态栏 ──────────────────────────────────────────────────────
        SharedPtr<Widget> statusBar;
        if (auto st = self->status())
            statusBar = cui::make_shared<StatusBar>(st->left, st->segments, st->dot);

        auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{toolBar, cui::make_shared<Expanded>(1, body),
                                                 statusBar});
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

        // ── 覆盖全窗口的 Listener：接管工具拖放的 move / up ─────────────
        auto listener = cui::make_shared<Listener>(
            /*onDown*/ nullptr,
            /*onMove*/ [ctl](const PointerEvent& e) {
                if (ctl->toolDragging())
                    ctl->updateToolDrag(e.globalPosition);
            },
            /*onUp*/ [ctl](const PointerEvent& e) {
                if (ctl->toolDragging())
                    ctl->endToolDrag(e.globalPosition);
            },
            /*onHover*/ nullptr,
            /*onScroll*/ nullptr,
            /*child*/ col);

        // autofocus：键盘（Delete / 属性编辑）开箱即用。
        return cui::make_shared<Focus>(focusNode_, listener, std::function<void(bool)>{}, true);
    }

private:
    IdeShell* w() { return static_cast<IdeShell*>(widget()); }

    static SharedPtr<Widget> toolButton(const std::string& label,
                                              std::function<void()> cb) {
        auto t = cui::make_shared<Text>(label, TextStyle().size(12.0).fgColor(theme().textPrimary));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::symmetric(4.0, 10.0))
            .decoration(BoxDecoration{theme().surfaceVariant});
        return cui::make_shared<GestureDetector>(c, std::move(cb));
    }

    SharedPtr<Widget> buildDialog(const SharedPtr<IdeShellDialog>& d) {
        auto title = cui::make_shared<Text>(
            d->title, TextStyle().size(14.0).bold().fgColor(theme().textPrimary));
        auto prompt = cui::make_shared<Text>(
            d->prompt, TextStyle().size(12.0).fgColor(theme().textSecondary));

        InputDecoration deco;
        deco.filled = true;
        deco.fillColor = theme().surface;
        auto field = cui::make_shared<TextField>(d->editor, deco, TextStyle().size(12.0),
                                                 TextAlign::Left, false, /*autofocus*/ true);
        auto fieldBox = cui::make_shared<Container>(field);
        fieldBox->height(28.0);

        auto ok = toolButton("OK", [d] { d->close(true); });
        auto cancel = toolButton("Cancel", [d] { d->close(false); });
        auto btnRow = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{ok, cui::make_shared<SizedBox>(8.0, 0.0), cancel});
        btnRow->setMainAxisSize(MainAxisSize::Min);

        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            title, cui::make_shared<SizedBox>(0.0, 8.0), prompt,
            cui::make_shared<SizedBox>(0.0, 6.0), fieldBox,
            cui::make_shared<SizedBox>(0.0, 12.0), btnRow});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);

        auto card = cui::make_shared<Container>(col);
        card->width(420.0)
            .padding(EdgeInsets::all(16.0))
            .decoration(BoxDecoration{Color::fromRGB(1, 1, 1), 6.0, 1.0,
                                      Color::fromRGBA(0, 0, 0, 0.12), 4.0,
                                      Color::fromRGBA(0, 0, 0, 0.2)});

        return cui::make_shared<Center>(card);
    }

    SharedPtr<FocusNode> focusNode_;
};

inline std::unique_ptr<State> IdeShell::createState() {
    return std::make_unique<IdeShellState>();
}

}  // namespace designer
}  // namespace cui
