#pragma once
/// @file gallery_dock.h — gallery 与 IDE 合并后的停靠层（对外**声明**）。
///
/// 全部实现已迁到 gallery_dock.cpp。本头只保留 `gallery_shell.cpp` 需要的
/// 类型完整定义（GalleryDockPanel / IdeLogView / IdeTextBuffer / 菜单结构体）
/// 与各工厂/布局函数的**声明**，不再把 ~930 行内联实现拉进每个 TU。
///
/// 历史：从旧 `examples/ide.cpp` 移植进 `cui::gallery` 命名空间，供
/// `gallery_shell.cpp` 复用。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/dock/dock.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/theme.h"
#include "cui/framework/framework.h"

namespace cui {
namespace story {

// ── Output 面板缓冲（菜单/工具条与 Output 面板共享）─────────────────────────

/// 继承 ChangeNotifier，面板里的 IdeLogView 订阅它，Build 等动作 append 后
/// 视图即时刷新。
struct IdeTextBuffer : public ChangeNotifier {
    std::string value;
    void setText(std::string s);
    const std::string& text() const;
    void append(const std::string& line);
};

// ── GalleryDockPanel — 任意内容控件的可停靠包装 ──────────────────────────────

class GalleryDockPanel : public Dockable {
public:
    /// 面板分组类型（用于「同类型才能并组」的停靠约束，对应 Dockable::panelType）。
    enum class PanelType {
        Generic = 0,    ///< 通用 / 中心内容
        Explorer = 1,   ///< 方案管理类（Solution Explorer / Toolbox）
        Inspector = 2,  ///< 检视类（Properties / Class View / Watch）
        Output = 3,     ///< 输出类（Output / Error List / Terminal / 查找结果）
        PageList = 4,   ///< 页面列表
    };

    struct Config {
        std::string id;
        std::string tab;
        std::string title;
        std::string tooltip;
        DockableStyle style = DockableStyle::Both;
        bool closable = true;
        bool autoHide = true;
        bool minMax = true;
        PanelType type = PanelType::Generic;
    };

    GalleryDockPanel(const Config& cfg, SharedPtr<Widget> content);
    ~GalleryDockPanel() override;

    std::string persistentID() const override;
    std::string tabText() const override;
    SharedPtr<Widget> content() const override;
    std::string titleText() const;
    std::string tabTooltip() const;
    DockableStyle style() const override;
    DockableStyle autoHideStyle() const override;
    bool isClosable() const override;
    bool isAutoHideAllowed() const override;
    bool isMinMaxAllowed() const override;
    int panelType() const override;

private:
    Config cfg_;
    SharedPtr<Widget> content_;
};

SharedPtr<GalleryDockPanel> makePanel(const GalleryDockPanel::Config& cfg,
                                      SharedPtr<Widget> content);

// ── IDE 日志视图（订阅 IdeTextBuffer）───────────────────────────────────────

class IdeLogView : public StatefulWidget {
public:
    explicit IdeLogView(SharedPtr<IdeTextBuffer> buffer, KeyPtr key = nullptr);
    const SharedPtr<IdeTextBuffer>& buffer() const;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override;

private:
    SharedPtr<IdeTextBuffer> buffer_;
};

// ── 菜单结构体（buildIdeMenuBar / buildIdeMenuDropdown 入参）─────────────────

/// 一个菜单项：带回调的标签，或分隔线。
struct MenuAction {
    std::string label;
    std::function<void()> cb;
    bool separator = false;
};

/// 菜单栏的一个顶级菜单。
struct MenuDef {
    std::string title;
    std::vector<MenuAction> actions;
};

// ── 工厂 / 布局声明 ─────────────────────────────────────────────────────────

SharedPtr<IdeTextBuffer> buildOutput();
SharedPtr<Widget> buildSolutionExplorer();
SharedPtr<Widget> buildToolbox();
SharedPtr<Widget> buildProperties();
SharedPtr<Widget> buildClassView();
SharedPtr<Widget> buildWatch();
SharedPtr<Widget> buildErrorList();
SharedPtr<Widget> buildTerminal();
SharedPtr<Widget> buildFindResults();
SharedPtr<Widget> buildBuildOutput();
SharedPtr<Widget> buildIdeMenuBar(const std::vector<MenuDef>& defs, int openIndex,
                                  const std::function<void(int)>& onLabelTap);
SharedPtr<Widget> buildIdeMenuDropdown(const std::vector<MenuDef>& defs, int openIndex,
                                       double barH,
                                       const std::function<void()>& onItemSelected);
SharedPtr<Widget> buildIdeToolBar(const std::function<void(const std::string&)>& logToOutput,
                                  const std::function<void()>& onBuild);
/// 工具栏右端的「解决方案配置」下拉（PopupMenuButton）。抽成工厂便于把它挂到
/// Ribbon 尾部（工具栏合并进 Ribbon 时使用）。
SharedPtr<Widget> buildIdeConfigCombo(const std::function<void(const std::string&)>& logToOutput);
SharedPtr<Widget> buildIdeStatusBar(const std::vector<std::string>& segments);
WindowLayoutBuilder dockGalleryLayoutBuilder();
void applyInitialDockGalleryLayout();
bool dockGalleryLayoutComplete();
void ideBuildSolution(SharedPtr<IdeTextBuffer> outputText);

}  // namespace gallery
}  // namespace cui
