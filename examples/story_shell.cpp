/// @file gallery_shell.cpp — story 壳 + 页面（实现，对应薄头 gallery_shell.h）。
///

///
/// 全部由 cui_core 三层（Widget/Element/RenderObject）构成，经
/// story.cpp 通过 Application::setRoot() 直接挂载并接收指针事件。
/// 这是「story 宿主与页面切到 上游 流水线」的第一批：
///   - 壳：顶部标题栏 + 侧边栏页面列表 + 内容区（滚动）+ 底部状态条；
///   - 页面：概览 / 按钮 / 选择 / 滑块 / 图标 / Material / 布局 / 滚动。
///
/// 已知降级（后续批次补齐）：
///   - 文本为单行 RenderParagraph（长文本截断显示）；
///   - 键盘/焦点路由已接通（键盘事件经 FocusManager 派发）。

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkImageFilters.h>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "animated_demo.h"
#include "cui/charts/chart.h"
#include "cui/code_editor/code_editor.h"     // 独立包：高级页的 CodeEditor
#include "cui/dock/dock.h"                   // 停靠框架（RootDockingPanel / Dockable / Docking）
#include "cui/foundation/application.h"      // Async 页：runAsync + 线程池
#include "cui/foundation/change_notifier.h"  // GallerySelection：侧栏停靠面板的选中态模型
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_cache.h"     // 图像页：解码与编码缓存的读数
#include "cui/foundation/image_data.h"      // 图像页：解码 / PNG 编码
#include "cui/foundation/image_provider.h"  // 图像页：Memory / File / Network / Decoded 来源
#include "cui/foundation/key.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/foundation/translator.h"
#include "cui/foundation/window.h"  // 多窗口页：AppWindow
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/platform/event.h"                      // KeyCode
#include "cui/platform/services/app_lifecycle.h"     // 系统服务页：生命周期
#include "cui/platform/services/bluetooth.h"         // 系统服务页：BLE
#include "cui/platform/services/camera.h"            // 系统服务页：Camera2 通道
#include "cui/platform/services/file_dialog.h"       // 视频页：原生文件选择对话框
#include "cui/platform/services/haptic_feedback.h"   // 系统服务页：触感 / 提示音
#include "cui/platform/services/method_channel.h"    // 系统服务页：通道探活
#include "cui/platform/services/permission.h"        // 系统服务页：运行时权限
#include "cui/platform/services/system_chrome.h"     // 系统服务页：系统 UI
#include "cui/platform/services/system_navigator.h"  // 系统服务页：返回键
#include "cui/utils/store.h"                         // 值输入页：MVI Store（State/Action/Reducer）
#include "cui/video/video_player.h"                  // 视频包：controller / VideoPlayer / 进度条
#include "cui/widgets/animated_size.h"
#include "cui/widgets/auto_hide_toolbar.h"  // AutoHideToolbar：story 的自动隐藏工具栏页
#include "cui/widgets/basic.h"
#include "cui/widgets/combobox.h"
#include "cui/widgets/container.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/data.h"
#include "cui/widgets/dialog.h"
#include "cui/widgets/drag_target.h"     // Draggable / DragTarget：拖放页
#include "cui/widgets/extended_image.h"  // ExtendedImage：占位 / 失败兜底 / 缓存
#include "cui/widgets/gestures.h"
#include "cui/widgets/hero.h"  // Hero / HeroController：story 的 Hero Animation 页
#include "cui/widgets/icons.h"
#include "cui/widgets/image_editor.h"   // ImageEditor：裁剪 / 旋转 / 翻转
#include "cui/widgets/image_painter.h"  // ImagePainterLayer：画笔涂抹层
#include "cui/widgets/implicit_animation.h"
#include "cui/widgets/interactive.h"
#include "cui/widgets/interactive_viewer.h"  // InteractiveViewer：缩放平移
#include "cui/widgets/layout.h"
#include "cui/widgets/liquid_glass.h"  // 液态玻璃外观（壁纸 + 磨砂玻璃片 + 面板卡片）
#include "cui/widgets/material.h"
#include "cui/widgets/material_motion.h"  // story 11–15：Material Motion 转场族
#include "cui/widgets/navigator.h"
#include "cui/widgets/pages.h"       // PageRouteBuilder：Hero 演示的页内 Navigator
#include "cui/widgets/paint.h"       // CustomPaint：Custom Widget 故事页
#include "cui/widgets/photo_view.h"  // PhotoView / PhotoViewSlidePage：看图 + 下滑退出
#include "cui/widgets/popup_menu.h"
#include "cui/widgets/scaffold.h"  // Scaffold + openDrawer：Drawer 段的按钮弹出演示
#include "cui/widgets/scroll.h"
#include "cui/widgets/tabs.h"  // TabBar / TabBarView：story 的 TabBar 页
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"
#include "cui/widgets/tooltip.h"
#include "cui/widgets/transition.h"    // FadeTransition：路由转场
#include "cui/widgets/virtual_list.h"  // VirtualList：零逐行节点虚拟列表
#include "story_dock.h"                // 合并后的停靠层（DockPanel + IDE 面板 / 外壳）
#include "video_demo.h"                // 视频页：运行时生成的 MJPEG AVI 素材

namespace cui {
namespace story {

// ── 控件层控件 ─────────────────────────────────────────────────────────────
// 控件全部定义在 cui_core 命名空间，页面可直接按原名书写。

// ── 主题常量（跟随全局 theme()，暗色主题下自动反色）────────────────────────
inline Color primaryColor() { return theme().primary; }
// 侧栏/页脚是**叠在背景上的半透明淡染**：用前景色的低透明度，浅色主题得到
// 黑 4~7%，深色主题得到白 4~7%，玻璃模式下也不会把磨砂盖实。
inline Color sidebarBg() {
    return Color(theme().textPrimary.r, theme().textPrimary.g, theme().textPrimary.b, 0.045);
}
inline Color itemHoverBg() {
    return Color(theme().primary.r, theme().primary.g, theme().primary.b, 0.10);
}
inline Color footerBg() {
    return Color(theme().textPrimary.r, theme().textPrimary.g, theme().textPrimary.b, 0.07);
}
inline Color captionColor() { return theme().textSecondary; }

// ── 液态玻璃（Liquid Glass）外观 ───────────────────────────────────────────
// 与 rui 的 `Appearance::Glass` 同款思路：关掉是普通实心外观，打开时在整棵
// 界面下垫「彩色壁纸 + 整窗磨砂玻璃片」，侧栏/内容区再各叠一张玻璃卡片。
// 玻璃片画在内容**之下**（Stack 先玻璃后内容），文字不会被霜色糊住。

/// 壁纸：斜向蓝紫渐变 + 几团亮色光斑。纯色底上磨砂看不出效果，必须有色差。
inline void paintGlassWallpaper(Painter& p, const Size& size) {
    p.setLinearGradient(Point{0.0, 0.0}, Point{size.width, size.height},
                        Color::fromRGB(0.16, 0.32, 0.72),
                        Color::fromRGB(0.42, 0.24, 0.68));
    p.rectangle(Rect(0.0, 0.0, size.width, size.height));
    p.fill();

    struct Blob {
        double x, y, r;
        Color color;
    };
    const Blob blobs[] = {
        {0.22, 0.18, 420.0, Color::fromRGB(0.30, 0.85, 1.00)},
        {0.82, 0.30, 380.0, Color::fromRGB(1.00, 0.55, 0.85)},
        {0.55, 0.95, 460.0, Color::fromRGB(0.30, 1.00, 0.75)},
    };
    for (const auto& b : blobs) {
        const Point c{b.x * size.width, b.y * size.height};
        p.setRadialGradient(c, b.r, Color(b.color.r, b.color.g, b.color.b, 0.72),
                            Color(b.color.r, b.color.g, b.color.b, 0.0));
        p.circle(c.x, c.y, b.r);
        p.fill();
    }
    p.resetGradient();
}

/// 壁纸的「已模糊」版本，按窗口尺寸烘一次复用。
///
/// 整窗玻璃片的 backdrop 只有这张壁纸，而软件光栅下每帧一次全屏 backdrop blur
/// 要 5 秒一帧（实测 sigma 3/7/13 的耗时几乎一样，钱花在 saveLayer 的
/// backdrop 快照上，不是卷积本身）—— 界面因此点一下要等好几秒才响应，看上去
/// 就是「点了没反应」。壁纸是静态渐变，模糊结果恒定，烘一次当普通图 blit 即可
/// （1:1 烘焙 + 逐帧模糊的出图与逐帧 backdrop 模糊逐字节相同）。
///
/// 烘焙在 1/4 分辨率上做，放大时用最近邻：壁纸只有大尺度渐变，实测与 1:1 烘焙
/// + 线性放大的出图最大只差 1/255，而一帧的耗时从 7.3 秒降到 6.1 秒（1:1 烘焙
/// 反而更贵：8.6 秒）。尺寸按烘焙分辨率的 32px 分桶复用，拖窗口时不会每变一个
/// 像素就重烘一次。
inline sk_sp<SkImage> frostedWallpaper(double w, double h, double blur) {
    constexpr double kScale = 0.25;
    static sk_sp<SkImage> cached;
    static int cw = -1, ch = -1;
    static double cBlur = -1.0;

    auto bakeDim = [](double side) {
        const double small = side > 0 ? side * kScale : 1.0;
        return static_cast<int>(std::ceil(small / 32.0) * 32.0);
    };
    const int bw = bakeDim(w), bh = bakeDim(h);
    if (cached && cw == bw && ch == bh && cBlur == blur)
        return cached;

    const SkImageInfo info = SkImageInfo::MakeN32Premul(bw, bh);
    auto src = SkSurfaces::Raster(info);
    if (!src)
        return nullptr;
    {
        // 壁纸按最终窗口尺寸画进低分辨率图层，等效于整体缩放绘制。
        Painter p(src->getCanvas());
        p.save();
        p.scale(static_cast<double>(bw) / w, static_cast<double>(bh) / h);
        paintGlassWallpaper(p, Size{w, h});
        p.restore();
    }
    cached = src->makeImageSnapshot();

    if (blur > 0.5) {
        auto dst = SkSurfaces::Raster(info);
        if (dst) {
            // sigma 随分辨率同比缩小，模糊半径在最终尺寸上保持不变。
            const double k = static_cast<double>(bw) / w;
            SkPaint fp;
            fp.setImageFilter(SkImageFilters::Blur(static_cast<SkScalar>(blur * 0.5 * k),
                                                   static_cast<SkScalar>(blur * 0.5 * k),
                                                   SkTileMode::kClamp, nullptr));
            dst->getCanvas()->drawImage(cached.get(), 0, 0, SkSamplingOptions(), &fp);
            if (auto baked = dst->makeImageSnapshot())
                cached = std::move(baked);
        }
    }
    cw = bw;
    ch = bh;
    cBlur = blur;
    return cached;
}

/// 整窗玻璃片：薄霜色 + 顶部高光 + 真·背景模糊，无圆角无边（铺满窗口）。
/// 模糊量只用于烘壁纸（见 frostedWallpaper），玻璃片自身不再做逐帧 backdrop。
inline double glassSheetBlur() {
    return 26.0;
}

inline GlassConfig glassSheetConfig() {
    GlassConfig c = GlassConfig::standard();
    c.cornerRadius = 0.0;
    c.tint = Color::fromRGB(0.94, 0.96, 1.0);
    c.opacity = 0.46;
    c.specularIntensity = 0.5;
    c.rimStrength = 0.0;
    c.borderWidth = 0.0;
    c.blur = 0.0;
    return c;
}

/// 面板卡片玻璃（侧栏 / 内容区）：一层更薄的霜色 + 白色边缘光，勾出圆角卡片。
/// 底下的整窗玻璃片已经模糊过，这里再做背景模糊没有可见收益，故 blur = 0。
inline GlassConfig glassPanelConfig() {
    GlassConfig c = GlassConfig::standard();
    c.cornerRadius = 14.0;
    c.tint = Color::fromRGB(1.0, 1.0, 1.0);
    c.opacity = 0.18;
    c.specularIntensity = 0.45;
    c.rimStrength = 0.55;
    c.borderWidth = 1.0;
    c.borderColor = Color::fromRGB(1.0, 1.0, 1.0);
    c.blur = 0.0;
    return c;
}

// ── 通用小助手 ──────────────────────────────────────────────────────────────

/// 区块标题（页面内小节头）。
inline SharedPtr<Widget> sectionTitle(const std::string& s) {
    auto t = cui::make_shared<Text>(tr(s), TextStyle().size(15.0).bold());
    auto c = cui::make_shared<Container>(t);
    c->padding(EdgeInsets(0.0, 0.0, 0.0, 0.0));
    c->margin(EdgeInsets(4.0, 0.0, 0.0, 10.0));
    return c;
}

/// 说明文字。
inline SharedPtr<Widget> caption(const std::string& s) {
    return cui::make_shared<Text>(s, TextStyle().size(12.0).fgColor(captionColor()));
}

/// 竖直间距。
inline SharedPtr<Widget> vgap(double h) {
    return cui::make_shared<SizedBox>(0.0, h);
}

/// 横向一行：默认垂直居中、主轴收缩到内容。
inline SharedPtr<Widget> hrow(std::vector<SharedPtr<Widget>> kids,
                              CrossAxisAlignment align = CrossAxisAlignment::Center) {
    auto row = cui::make_shared<Row>(std::move(kids));
    row->setMainAxisSize(MainAxisSize::Min);
    row->setCrossAxisAlignment(align);
    return row;
}

/// 纵向一列：主轴收缩到内容、交叉轴左对齐。
inline SharedPtr<Widget> vcol(std::vector<SharedPtr<Widget>> kids) {
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

/// 贴身控件（图标按钮、Chip、导航栏…）：pageBody 是 Stretch 交叉轴，单独摆一个
/// 会连盒子一起被拉到整行宽（内容居中或右侧空一长条），这类控件没有「占满」语义，
/// 用 Align 放松约束后按自身尺寸贴在行首。
inline SharedPtr<Widget> lead(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

/// 页面内容外层：留白 + 纵向排布（不滚动，滚动由壳的 SingleChildScrollView 负责）。
///
/// 交叉轴用 Stretch（不是 Start）：页面上「一行只有一个控件」的槽位——文本框、
/// 下拉框、进度条、代码编辑器、分组框等——一律横向拉满内容区，不再缩在左侧
/// 一小截。多控件行本来就是 Row（主轴 Min、内容靠左），被拉宽后观感不变；
/// 标题/说明文字拉宽也只是盒子变宽，绘制仍在行首。
inline SharedPtr<Widget> pageBody(std::vector<SharedPtr<Widget>> kids) {
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto c = cui::make_shared<Container>(col);
    c->padding(EdgeInsets::symmetric(12.0, 20.0));
    return c;
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 1：概览
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageOverview() {
    auto card1 = cui::make_shared<Container>(cui::make_shared<Text>(
        "整棵 Widget/Element/RenderObject 树运行在 cui_core 流水线上：\n"
        "BuildOwner 驱动重建，PipelineOwner 驱动布局/绘制，\n"
        "整棵树挂载在 Application 的单条流水线上。"));
    card1->padding(EdgeInsets::all(12.0));
    card1->margin(EdgeInsets(0.0, 0.0, 0.0, 10.0));
    card1->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.08), 8.0});

    // 色板行：DecoratedBox 展示。
    std::vector<SharedPtr<Widget>> swatches;
    const char* names[] = {"primary", "sidebar", "footer", "caption"};
    Color colors[] = {primaryColor(), sidebarBg(), footerBg(), captionColor()};
    for (int i = 0; i < 4; ++i) {
        auto box = cui::make_shared<Container>(
            cui::make_shared<SizedBox>(64.0, 40.0));
        box->decoration(BoxDecoration{colors[i], 6.0});
        auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{box, caption(names[i])});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto padded = cui::make_shared<Container>(col);
        padded->padding(EdgeInsets::symmetric(4.0, 0.0));
        padded->margin(EdgeInsets(0.0, 0.0, 8.0, 0.0));
        swatches.push_back(padded);
    }
    auto row = cui::make_shared<Row>(std::move(swatches));
    row->setMainAxisSize(MainAxisSize::Min);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    return pageBody({
        sectionTitle("控件架构（cui_core）"),
        caption("Widgets: basic/container/text/layout/scroll/material/interactive/icons"),
        vgap(10.0),
        card1,
        sectionTitle("色板"),
        row,
        vgap(10.0),
        sectionTitle("页面清单"),
        caption("概览 / 按钮 / 选择 / 滑块 / 图标 / Material / 布局 / 滚动 —— 更多页面持续补齐。"),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 2：按钮
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageButtons() {
    auto e = [] {};
    auto elevated = cui::make_shared<ElevatedButton>(
        cui::make_shared<Text>("Elevated"), e);
    auto outlined = cui::make_shared<OutlinedButton>(
        cui::make_shared<Text>("Outlined"), e);
    auto textBtn =
        cui::make_shared<TextButton>(cui::make_shared<Text>("Text"), e);
    auto disabled = cui::make_shared<ElevatedButton>(cui::make_shared<Text>("Disabled"));

    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
        elevated, vgap(0.0), outlined, vgap(0.0), textBtn, vgap(0.0), disabled});
    row->setMainAxisSize(MainAxisSize::Min);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    return pageBody({
        sectionTitle("Button 系列"),
        caption("Elevated / Outlined / Text，禁用态置灰。"),
        vgap(12.0),
        row,
        vgap(16.0),
        cui::make_shared<Divider>(),
        vgap(12.0),
        sectionTitle("带图标"),
        lead(cui::make_shared<IconButton>(IconCode::Favorite, nullptr)),
        caption("IconButton：矢量图标（SkPath，24 网格缩放），无字体依赖。"),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 3：选择控件（StatefulWidget 演示）
// ══════════════════════════════════════════════════════════════════════════════

class SelectionPage : public StatefulWidget {
public:
    SelectionPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SelectionPage"; }
};

class SelectionPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        auto hrow = [](std::vector<SharedPtr<Widget>> kids) {
            auto r = cui::make_shared<Row>(std::move(kids));
            r->setMainAxisSize(MainAxisSize::Min);
            r->setCrossAxisAlignment(CrossAxisAlignment::Center);
            return r;
        };

        auto switchRow = hrow({
            cui::make_shared<Switch>(switchOn_, [this](bool) {
                switchOn_ = !switchOn_;
                setState();
            }),
            cui::make_shared<SizedBox>(12.0, 0.0),
            cui::make_shared<Text>("Switch: " + std::string(switchOn_ ? "on" : "off")),
        });
        auto checkRow = hrow({
            cui::make_shared<Checkbox>(checkOn_, [this](bool) {
                checkOn_ = !checkOn_;
                setState();
            }),
            cui::make_shared<SizedBox>(12.0, 0.0),
            cui::make_shared<Text>("Checkbox: " +
                                   std::string(checkOn_ ? "checked" : "unchecked")),
        });

        std::vector<SharedPtr<Widget>> radios;
        const char* names[] = {"A", "B", "C"};
        for (int i = 0; i < 3; ++i) {
            bool sel = (radioGroup_ == i);
            radios.push_back(cui::make_shared<Radio>(sel, [this, i](bool) {
                radioGroup_ = i;
                setState();
            }));
            radios.push_back(cui::make_shared<Text>(names[i]));
            radios.push_back(cui::make_shared<SizedBox>(10.0, 0.0));
        }

        return pageBody({
            sectionTitle("Switch / Checkbox / Radio"),
            caption("受控模式：State 持有值，onChanged → setState 重建。"),
            vgap(12.0),
            switchRow,
            vgap(8.0),
            checkRow,
            vgap(8.0),
            hrow(std::move(radios)),
        });
    }

private:
    bool switchOn_ = true;
    bool checkOn_ = false;
    int radioGroup_ = 1;
};

std::unique_ptr<State> SelectionPage::createState() {
    return std::make_unique<SelectionPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 4：滑块（StatefulWidget，带标签气泡）
// ══════════════════════════════════════════════════════════════════════════════

class SliderPage : public StatefulWidget {
public:
    SliderPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SliderPage"; }
};

class SliderPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f%%", value_ * 100.0);
        auto slider = cui::make_shared<Slider>(value_, [this](double v) {
            value_ = v;
            setState();
        }, 0.0, 1.0, primaryColor(), Color::fromRGBA(0, 0, 0, 0.24),
               primaryColor(), 20.0, 10.0, buf);
        auto slider2 = cui::make_shared<Slider>(value2_, [this](double v) {
            value2_ = v;
            setState();
        }, 0.0, 100.0);

        return pageBody({
            sectionTitle("Slider"),
            caption("拖动时 thumb 上方显示标签气泡（150ms 淡入淡出）。"),
            vgap(12.0),
            slider,
            vgap(8.0),
            cui::make_shared<Text>(std::string("value: ") + buf),
            vgap(16.0),
            sectionTitle("无标签"),
            slider2,
            vgap(8.0),
            cui::make_shared<Text>("value2: " + std::to_string((int)value2_)),
        });
    }

private:
    double value_ = 0.4;
    double value2_ = 42.0;
};

std::unique_ptr<State> SliderPage::createState() {
    return std::make_unique<SliderPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 5：图标（StatefulWidget，点击计数）
// ══════════════════════════════════════════════════════════════════════════════

class IconPage : public StatefulWidget {
public:
    IconPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "IconPage"; }
};

class IconPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        std::vector<SharedPtr<Widget>> icons;
        const IconCode codes[] = {
            IconCode::Home,      IconCode::Search,    IconCode::Settings,
            IconCode::Person,    IconCode::Favorite,  IconCode::Star,
            IconCode::Play,      IconCode::Pause,     IconCode::Menu,
            IconCode::Delete,    IconCode::Check,     IconCode::Close,
            IconCode::Add,       IconCode::Remove,    IconCode::ArrowBack,
            IconCode::ArrowForward, IconCode::ArrowUpward, IconCode::ArrowDownward,
            IconCode::MoreVert,  IconCode::MoreHoriz,
            // 语义图标
            IconCode::Button,    IconCode::Toggle,    IconCode::Radio,
            IconCode::Checkbox,  IconCode::Dropdown,  IconCode::Slider,
            IconCode::Text,      IconCode::Input,     IconCode::Table,
            IconCode::List,      IconCode::Grid,      IconCode::Chart,
            IconCode::Image,     IconCode::Dialog,    IconCode::Bell,
            IconCode::Palette,   IconCode::Layers,    IconCode::Refresh,
            IconCode::Tag,       IconCode::Gesture,   IconCode::Folder,
            IconCode::Clock};
        for (auto c : codes) {
            auto box = cui::make_shared<Container>(cui::make_shared<Icon>(c, 26.0));
            box->padding(EdgeInsets::all(7.0));
            icons.push_back(box);
        }
        // 每行 12 个，自动换行。
        std::vector<SharedPtr<Widget>> rows;
        std::vector<SharedPtr<Widget>> line;
        for (auto& ic : icons) {
            line.push_back(ic);
            if (line.size() % 12 == 0) {
                auto row = cui::make_shared<Row>(line);
                row->setMainAxisSize(MainAxisSize::Min);
                row->setCrossAxisAlignment(CrossAxisAlignment::Center);
                rows.push_back(row);
                line.clear();
            }
        }
        if (!line.empty()) {
            auto row = cui::make_shared<Row>(line);
            row->setMainAxisSize(MainAxisSize::Min);
            row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            rows.push_back(row);
        }
        auto grid = cui::make_shared<Column>(std::move(rows));
        grid->setMainAxisSize(MainAxisSize::Min);
        grid->setCrossAxisAlignment(CrossAxisAlignment::Start);

        auto btn = cui::make_shared<IconButton>(IconCode::Add, [this] {
            ++taps_;
            setState();
        });

        return pageBody({
            sectionTitle("Icon 矢量集"),
            caption("IconCode → SkPathBuilder 双路径（fill/stroke），按 size/24 缩放。"),
            vgap(12.0),
            grid,
            vgap(16.0),
            sectionTitle("IconButton"),
            lead(btn),
            vgap(6.0),
            cui::make_shared<Text>("taps: " + std::to_string(taps_)),
        });
    }

private:
    int taps_ = 0;
};

std::unique_ptr<State> IconPage::createState() {
    return std::make_unique<IconPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 6：Material（ListTile / Chip / Tooltip / Card）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageMaterial() {
    auto listCard = cui::make_shared<Card>(cui::make_shared<Column>(
        std::vector<SharedPtr<Widget>>{
            cui::make_shared<ListTile>(cui::make_shared<Text>("列表项一"),
                                       cui::make_shared<Text>("subtitle")),
            cui::make_shared<ListTile>(cui::make_shared<Text>("列表项二"),
                                       cui::make_shared<Text>("dense"),
                                       nullptr, nullptr, true),
            cui::make_shared<ListTile>(cui::make_shared<Text>("可点击项"), nullptr,
                                       cui::make_shared<Icon>(IconCode::Play, 20.0),
                                       cui::make_shared<Icon>(IconCode::MoreVert, 20.0),
                                       false, nullptr),
        }));
    // Column 默认 Max 主轴 —— 收紧为 Min 并 stretch 交叉轴。
    // （Card 内部是普通容器，直接用默认即可。）

    auto chip1 = cui::make_shared<Chip>(cui::make_shared<Text>("普通"));
    auto chip2 = cui::make_shared<Chip>(cui::make_shared<Text>("已选"),
                                        theme().surfaceVariant, 16.0,
                                        EdgeInsets::symmetric(4.0, 8.0), nullptr,
                                        IconCode::None, nullptr, true, primaryColor());
    auto chip3 = cui::make_shared<Chip>(cui::make_shared<Text>("可删除"),
                                        theme().surfaceVariant, 16.0,
                                        EdgeInsets::symmetric(4.0, 8.0), nullptr,
                                        IconCode::Close, nullptr, false,
                                        primaryColor());

    auto chipRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
        chip1, cui::make_shared<SizedBox>(8.0, 0.0), chip2,
        cui::make_shared<SizedBox>(8.0, 0.0), chip3});
    chipRow->setMainAxisSize(MainAxisSize::Min);
    chipRow->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto tip = cui::make_shared<Tooltip>(
        "hover 或长按显示", cui::make_shared<ElevatedButton>(cui::make_shared<Text>("Tooltip")));

    return pageBody({
        sectionTitle("ListTile + Card（elevation 4）"),
        vgap(8.0),
        listCard,
        vgap(16.0),
        sectionTitle("Chip"),
        chipRow,
        vgap(16.0),
        sectionTitle("Tooltip"),
        tip,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 7：布局（Row / Stack / Positioned / 装饰）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> box(double w, double h, const Color& c,
                                   const std::string& label = {}) {
    SharedPtr<Widget> inner =
            label.empty() ? cui::make_shared<SizedBox>(w, h)
                          : cui::make_shared<SizedBox>(
                                w, h, cui::make_shared<Text>(
                                          label, TextStyle().size(11.0).fgColor(Color::fromRGB(1, 1, 1))));
    auto b = cui::make_shared<Container>(inner);
    b->decoration(BoxDecoration{c, 4.0});
    return b;
}

inline SharedPtr<Widget> pageLayout() {
    auto stack = cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{
        box(220.0, 90.0, Color::fromRGBA(0.0, 0.45, 0.95, 0.25)),
        [&] {
            auto p = cui::make_shared<Positioned>(
                box(90.0, 40.0, primaryColor(), "Positioned"));
            p->left = 12.0;
            p->top = 12.0;
            p->hasLeft = p->hasTop = true;
            return p;
        }(),
        [&] {
            auto p = cui::make_shared<Positioned>(
                box(70.0, 30.0, Color::fromRGB(0.95, 0.55, 0.1), "右下"));
            p->right = 12.0;
            p->bottom = 12.0;
            p->hasRight = p->hasBottom = true;
            return p;
        }(),
    });
    stack->setFit(RenderStack::Fit::Loose);

    return pageBody({
        sectionTitle("Row（start 对齐）"),
        [&] {
            auto r = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                box(60.0, 36.0, primaryColor()), box(90.0, 24.0, Color::fromRGBA(0, 0, 0, 0.3)),
                box(40.0, 40.0, Color::fromRGB(0.95, 0.55, 0.1))});
            r->setMainAxisSize(MainAxisSize::Min);
            r->setCrossAxisAlignment(CrossAxisAlignment::Center);
            return r;
        }(),
        vgap(16.0),
        sectionTitle("Stack + Positioned"),
        stack,
        vgap(16.0),
        sectionTitle("Center 对齐"),
        [&] {
            auto c = cui::make_shared<Container>(
                cui::make_shared<Center>(box(80.0, 36.0, primaryColor())));
            c->height(80.0);
            c->decoration(BoxDecoration{theme().surfaceVariant, 6.0});
            return c;
        }(),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 8：滚动（Scrollbar + ListView.builder + GridView.builder）
// ══════════════════════════════════════════════════════════════════════════════

class ScrollPage : public StatefulWidget {
public:
    ScrollPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ScrollPage"; }
};

class ScrollPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        auto list = ListView::builder(
            &listCtrl_,
            [](BuildContext&, int i) {
                auto t = cui::make_shared<Text>("row " + std::to_string(i));
                auto c = cui::make_shared<Container>(t);
                c->padding(EdgeInsets::symmetric(4.0, 0.0));
                c->decoration(BoxDecoration{
                        Color::fromRGBA(0.0, 0.45, 0.95, (i % 2) ? 0.04 : 0.10), 0.0});
                return c;
            },
            60, 36.0);
        auto listArea = cui::make_shared<SizedBox>(
            1e18, 220.0, cui::make_shared<Scrollbar>(&listCtrl_, list));

        auto grid = GridView::builder(
            &gridCtrl_,
            cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(4, 4.0, 0.0, 1.0),
            [](BuildContext&, int i) {
                auto t = cui::make_shared<Text>(std::to_string(i),
                                                TextStyle().size(12.0).fgColor(Color::fromRGB(1, 1, 1)));
                auto c = cui::make_shared<Container>(t);
                c->padding(EdgeInsets::all(4.0));
                c->decoration(BoxDecoration{primaryColor(), 4.0});
                return cui::make_shared<Center>(c);
            },
            48);
        auto gridArea = cui::make_shared<SizedBox>(
            1e18, 180.0, cui::make_shared<Scrollbar>(&gridCtrl_, grid));

        return pageBody({
            sectionTitle("ListView.builder（itemExtent 定高懒加载）"),
            caption("只构建可见窗口 ±cache 行；右侧 Scrollbar 实时联动。"),
            vgap(8.0),
            listArea,
            vgap(16.0),
            sectionTitle("GridView.builder（crossAxisCount=4）"),
            gridArea,
        });
    }

private:
    ScrollController listCtrl_;
    ScrollController gridCtrl_;
};

std::unique_ptr<State> ScrollPage::createState() {
    return std::make_unique<ScrollPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 9：对话框（AlertDialog / SimpleDialog / showDialog / PopupMenuButton）
// ══════════════════════════════════════════════════════════════════════════════

class DialogsPage : public StatefulWidget {
public:
    DialogsPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DialogsPage"; }
};

class DialogsPageState : public State {
public:
    DialogsPageState() : alive_(cui::make_shared<bool>(true)) {}
    ~DialogsPageState() override { *alive_ = false; }
    SharedPtr<Widget> build(BuildContext& context) override {
        // 页面内容挂在 ShellContent 的 Navigator 里 —— 本 context 在
        // Navigator 子树内，showDialog / NavigatorState::of 可用。
        BuildContext* ctx = &context;

        auto alertBtn = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("AlertDialog"), [this, ctx] {
                auto route = showDialog(
                    *ctx, cui::make_shared<AlertDialog>(
                              "删除确认",
                              cui::make_shared<Text>(
                                  "确定要删除这条记录吗？此操作不可撤销。"),
                              std::vector<SharedPtr<Widget>>{
                                  cui::make_shared<TextButton>(
                                      cui::make_shared<Text>("取消"), [ctx] {
                                          NavigatorState::of(*ctx)->pop();
                                      }),
                                  cui::make_shared<ElevatedButton>(
                                      cui::make_shared<Text>("删除"), [this, ctx] {
                                          NavigatorState::of(*ctx)->pop(
                                              cui::make_shared<std::string>("已删除"));
                                      })}));
                route->whenPopped([this, alive = alive_](SharedPtr<void> r) {
                    if (!*alive)
                        return;  // 页面 State 已销毁（切页弹栈）
                    result_ = r ? *cui::static_pointer_cast<std::string>(r) : "已取消";
                    setState();
                });
            });

        auto simpleBtn = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("SimpleDialog"), [this, ctx] {
                showDialog(*ctx, cui::make_shared<SimpleDialog>(
                                     "选择一个音效",
                                     std::vector<SimpleDialogOption>{
                                         {"ding", 0}, {"dong", 1}, {"purr", 2}},
                                     [this, ctx, alive = alive_](int v) {
                                         NavigatorState::of(*ctx)->pop();
                                         if (!*alive)
                                             return;
                                         result_ = std::string("音效 #") +
                                                   std::to_string(v);
                                         setState();
                                     }));
            });

        auto popup = cui::make_shared<PopupMenuButton>(
            std::vector<PopupMenuItem>{{"复制", 0}, {"粘贴", 1}, {"剪切", 2}},
            [this, alive = alive_](int v) {
                if (!*alive)
                    return;
                const char* names[] = {"复制", "粘贴", "剪切"};
                result_ = names[v];
                setState();
            },
            cui::make_shared<OutlinedButton>(cui::make_shared<Text>("菜单 ▼")));

        return pageBody({
            sectionTitle("showDialog + AlertDialog"),
            caption("barrier 点击可关闭；actions 回调经 NavigatorState::of(context)->pop(result) 返回。"),
            vgap(12.0),
            cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                alertBtn, cui::make_shared<SizedBox>(12.0, 0.0), simpleBtn}),
            vgap(16.0),
            sectionTitle("PopupMenuButton"),
            popup,
            vgap(16.0),
            sectionTitle("结果"),
            cui::make_shared<Text>("result: " + result_),
        });
    }

private:
    SharedPtr<bool> alive_;
    std::string result_ = "（尚未操作）";
};

std::unique_ptr<State> DialogsPage::createState() {
    return std::make_unique<DialogsPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 10：手势（GestureDetector / InkWell）
// ══════════════════════════════════════════════════════════════════════════════

class GesturesPage : public StatefulWidget {
public:
    GesturesPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "GesturesPage"; }
};

class GesturesPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        auto tapBox = cui::make_shared<Container>(
            cui::make_shared<Center>(cui::make_shared<Text>(
                "点我 " + std::to_string(taps_),
                TextStyle().size(14.0).fgColor(Color::fromRGB(1, 1, 1)))));
        tapBox->width(160.0).height(64.0);
        tapBox->decoration(BoxDecoration{primaryColor(), 8.0});
        auto tapInner = cui::make_shared<Center>(tapBox);
        auto tapDemo = cui::make_shared<GestureDetector>(tapInner, [this] {
            ++taps_;
            setState();
        });

        auto ink = cui::make_shared<Container>(
            cui::make_shared<Center>(cui::make_shared<Text>("InkWell 水波纹")));
        ink->padding(EdgeInsets::symmetric(12.0, 18.0));
        auto inkDemo = cui::make_shared<InkWell>(
            ink, [this] {
                inkTaps_ += 10;
                setState();
            });

        auto dragBox = cui::make_shared<Container>(
            cui::make_shared<Center>(cui::make_shared<Text>(
                "拖我", TextStyle().size(13.0).fgColor(Color::fromRGB(1, 1, 1)))));
        dragBox->width(90.0).height(90.0);
        dragBox->decoration(BoxDecoration{Color::fromRGBA(0.95, 0.55, 0.1, 1.0), 8.0});
        auto dragInner = cui::make_shared<Center>(dragBox);
        auto dragDemo = cui::make_shared<GestureDetector>(
            dragInner, nullptr, nullptr, nullptr, nullptr,
            [this](const PointerEvent& ev) {
                dragDx_ += ev.delta.dx;
                dragDy_ += ev.delta.dy;
                setState();
            });

        return pageBody({
            sectionTitle("GestureDetector.onTap"),
            vgap(10.0),
            tapDemo,
            vgap(16.0),
            sectionTitle("InkWell（Material 水波纹）"),
            vgap(10.0),
            inkDemo,
            vgap(16.0),
            sectionTitle("onPanUpdate 拖拽"),
            vgap(10.0),
            dragDemo,
            vgap(10.0),
            cui::make_shared<Text>("taps: " + std::to_string(taps_) +
                                   " · ink: " + std::to_string(inkTaps_) +
                                   " · drag Δ(" + std::to_string((int)dragDx_) + ", " +
                                   std::to_string((int)dragDy_) + ")"),
        });
    }

private:
    int taps_ = 0;
    int inkTaps_ = 0;
    double dragDx_ = 0.0;
    double dragDy_ = 0.0;
};

std::unique_ptr<State> GesturesPage::createState() {
    return std::make_unique<GesturesPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 11：文本进阶（RichText / 样式）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageTextAdv() {
    auto root = cui::make_shared<TextSpan>("富文本：");
    root->children.push_back(TextSpan::make(
        "红色", TextStyle().size(14.0).bold().fgColor(Color::fromRGB(0.9, 0.2, 0.2))));
    root->children.push_back(
        TextSpan::make(" · ", TextStyle().size(14.0).fgColor(captionColor())));
    root->children.push_back(TextSpan::make(
        "蓝色", TextStyle().size(14.0).bold().fgColor(primaryColor())));
    root->children.push_back(
        TextSpan::make(" · 普通", TextStyle().size(14.0)));

    auto styleRow = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("14pt 常规", TextStyle().size(14.0)),
        cui::make_shared<Text>("14pt 加粗", TextStyle().size(14.0).bold()),
        cui::make_shared<Text>("18pt 大字", TextStyle().size(18.0).bold()),
        cui::make_shared<Text>("12pt 弱化", TextStyle().size(12.0).fgColor(captionColor())),
        cui::make_shared<Text>("主题色", TextStyle().size(14.0).bold().fgColor(primaryColor()))});
    styleRow->setMainAxisSize(MainAxisSize::Min);
    styleRow->setCrossAxisAlignment(CrossAxisAlignment::Start);

    return pageBody({
        sectionTitle("RichText（TextSpan 嵌套）"),
        vgap(10.0),
        cui::make_shared<RichText>(root),
        vgap(16.0),
        sectionTitle("TextStyle 阶梯"),
        vgap(6.0),
        styleRow,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 12：文本输入（TextField / EditableText / 键盘路由 / IME）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageTextInput() {
    auto nameCtrl = cui::make_shared<TextEditingController>("");
    auto pwdCtrl = cui::make_shared<TextEditingController>("");
    auto descCtrl = cui::make_shared<TextEditingController>("初始内容示例");

    auto nameField = cui::make_shared<TextField>(
        nameCtrl, InputDecoration{.labelText = "用户名", .hintText = "请输入用户名",
                                  .borderColor = primaryColor(), .borderRadius = 6.0,
                                  .borderWidth = 1.0});
    // autofocus 在密码框：启动后 CUI_KEY 可直接键入。
    auto pwdField = cui::make_shared<TextField>(
        pwdCtrl,
        InputDecoration{.labelText = "密码", .hintText = "请输入密码",
                       .borderColor = primaryColor(), .borderRadius = 6.0, .borderWidth = 1.0},
        TextStyle{}, TextAlign::Left, /*obscureText=*/true, /*autofocus=*/true);
    auto descField = cui::make_shared<TextField>(
        descCtrl, InputDecoration{.labelText = "备注", .hintText = "单行文本输入",
                                  .borderColor = primaryColor(), .borderRadius = 6.0,
                                  .borderWidth = 1.0});

    auto wrap = [](SharedPtr<Widget> f) {
        auto c = cui::make_shared<Container>(f);
        c->constraints(BoxConstraints(340.0, 340.0, 0.0, 1e18));
        return c;
    };

    return pageBody({
        sectionTitle("TextField（文本输入层）"),
        vgap(8.0),
        cui::make_shared<Text>(
            "键盘经 FocusManager::primaryFocus 路由；"
            "autofocus 在密码框。冒烟用 CUI_KEY 注入键入。",
            TextStyle().size(12.0).fgColor(captionColor())),
        vgap(14.0),
        wrap(nameField),
        vgap(10.0),
        wrap(pwdField),
        vgap(10.0),
        wrap(descField),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 12b：布局进阶（Transform / FittedBox / Wrap）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageLayoutAdv() {
    auto tile = [](const Color& c, const std::string& label) {
        auto t = cui::make_shared<Text>(
            label, TextStyle().size(12.0).fgColor(Color::fromRGB(1, 1, 1)));
        auto box = cui::make_shared<Container>(cui::make_shared<Center>(t));
        box->width(72.0).height(48.0);
        box->decoration(BoxDecoration{c, 6.0});
        return box;
    };

    auto scaled = cui::make_shared<Transform>(
        Matrix4::scale(1.5), tile(Color::fromRGBA(0.0, 0.45, 0.95, 0.9), "scale 1.5"));
    auto rotated = cui::make_shared<Transform>(
        Matrix4::rotationZ(0.35), tile(Color::fromRGBA(0.2, 0.7, 0.4, 1.0), "rotZ 20°"));
    auto translated = Transform::translate(
        Offset(18.0, 10.0), tile(Color::fromRGBA(0.95, 0.55, 0.1, 1.0), "translate"));

    auto bigText = cui::make_shared<Text>(
        "FittedBox 大文本塞小盒子",
        TextStyle().size(28.0).bold());
    auto fittedArea = cui::make_shared<Container>(cui::make_shared<FittedBox>(
        BoxFit::ScaleDown, Alignment::center(), bigText));
    fittedArea->width(240.0).height(56.0);
    fittedArea->decoration(BoxDecoration{theme().surfaceVariant, 6.0});
    auto fittedArea2 = cui::make_shared<Center>(fittedArea);

    std::vector<SharedPtr<Widget>> chips;
    for (int i = 0; i < 12; ++i)
        chips.push_back(cui::make_shared<Chip>(
            cui::make_shared<Text>("标签 " + std::to_string(i))));
    auto wrap = cui::make_shared<Wrap>(Axis::Horizontal, std::move(chips),
                                       WrapAlignment::Start, 8.0,
                                       WrapAlignment::Start, 8.0);

    return pageBody({
        sectionTitle("Transform"),
        vgap(10.0),
        cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            scaled, cui::make_shared<SizedBox>(16.0, 0.0), rotated,
            cui::make_shared<SizedBox>(16.0, 0.0), translated}),
        vgap(16.0),
        sectionTitle("FittedBox（ScaleDown）"),
        vgap(10.0),
        fittedArea2,
        vgap(16.0),
        sectionTitle("Wrap（spacing 8 / runSpacing 8）"),
        vgap(10.0),
        wrap,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：数据展示（Card / Badge / CircleAvatar / Calendar / DataTable / TreeView）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageData() {
    // ── Cards ──
    auto mkCard = [](const std::string& title, const std::string& desc, double elev) {
        auto inner = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<Text>(title, TextStyle().size(14.0).bold()),
            cui::make_shared<Text>(desc, TextStyle().size(12.0).fgColor(captionColor()))});
        inner->setMainAxisSize(MainAxisSize::Min);
        inner->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return cui::make_shared<Card>(inner, EdgeInsets::all(4.0), theme().surface, 8.0, elev);
    };
    auto cardRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
        mkCard("Card Title", "This is a card with elevation=2.", 2.0),
        cui::make_shared<SizedBox>(12.0, 0.0),
        mkCard("High Elevation", "Shadow depth = 6.", 6.0)});
    cardRow->setMainAxisSize(MainAxisSize::Min);
    cardRow->setCrossAxisAlignment(CrossAxisAlignment::Start);

    // ── Badges & Avatars ──
    const char initials[] = {'A', 'B', 'C', 'Z'};
    Color avatarColors[] = {Color::fromRGB(0.13, 0.59, 0.95), Color::fromRGB(0.95, 0.36, 0.36),
                            Color::fromRGB(0.26, 0.78, 0.54), Color::fromRGB(0.6, 0.4, 0.9)};
    int counts[] = {5, 128, 0, 3};
    bool showZero[] = {false, false, true, false};
    std::vector<SharedPtr<Widget>> badges;
    for (int i = 0; i < 4; ++i) {
        auto av = cui::make_shared<CircleAvatar>(std::string(1, initials[i]), 22.0, avatarColors[i]);
        badges.push_back(cui::make_shared<Badge>(av, counts[i], showZero[i]));
    }
    auto badgeRow = cui::make_shared<Row>(std::move(badges));
    badgeRow->setMainAxisSize(MainAxisSize::Min);
    badgeRow->setCrossAxisAlignment(CrossAxisAlignment::Center);

    // ── Calendar ──
    auto calendar = cui::make_shared<Calendar>();

    // ── DataTable ──
    std::vector<DataColumn> cols = {
        {"ID", 50, true}, {"Name", 140, false}, {"Department", 120, false},
        {"Position", 150, false}, {"Salary", 100, true}, {"Status", 90, false}};
    std::vector<std::vector<std::string>> rows = {
        {"1", "Alice Wang", "Engineering", "Senior Engineer", "28,500", "Active"},
        {"2", "Bob Chen", "Product", "Product Manager", "32,000", "Active"},
        {"3", "Carol Li", "Design", "UI Designer", "22,000", "Active"},
        {"4", "David Zhang", "Engineering", "Tech Lead", "38,000", "Active"},
        {"5", "Eva Liu", "Marketing", "Marketing Specialist", "18,500", "On Leave"},
        {"6", "Frank Wu", "Engineering", "Junior Engineer", "15,000", "Active"},
        {"7", "Grace Xu", "HR", "HR Manager", "26,000", "Active"},
        {"8", "Henry Zhao", "Finance", "Financial Analyst", "24,000", "Active"}};
    auto dataTable = cui::make_shared<DataTable>(std::move(cols), std::move(rows), true);

    // ── TreeView ──
    std::vector<TreeItem> tree = {
        {"src", "src", {
            {"platform", "platform", {
                {"x11_window.cpp", "x11_window.cpp", {}},
                {"win32_window.cpp", "win32_window.cpp", {}},
                {"cocoa_window.m", "cocoa_window.m", {}},
            }},
            {"application.cpp", "application.cpp", {}},
            {"widget.cpp", "widget.cpp", {}},
            {"ribbon.cpp", "ribbon.cpp", {}},
        }},
        {"include/cui_core", "include/cui_core", {
            {"widget.h", "widget.h", {}},
            {"painter.h", "painter.h", {}},
            {"theme.h", "theme.h", {}},
        }},
        {"examples", "examples", {
            {"story.cpp", "story.cpp", {}},
        }},
    };
    auto treeView = cui::make_shared<TreeView>(std::move(tree), true);
    auto treeCard = cui::make_shared<Container>(treeView);
    treeCard->padding(EdgeInsets::all(8.0)).decoration(BoxDecoration{
        theme().surface, 8.0, 1.0, theme().border, 4.0,
        Color::fromRGBA(0, 0, 0, 0.2)});

    return pageBody({
        sectionTitle("Cards"),
        caption("Card — 圆角 + elevation 阴影（2 / 6）。"),
        vgap(8.0),
        cardRow,
        vgap(16.0),
        sectionTitle("Badges & Avatars"),
        caption("CircleAvatar 显示首字母；Badge 在右上角叠加计数圆点（count=0 且 showZero 时隐藏）。"),
        vgap(10.0),
        badgeRow,
        vgap(16.0),
        sectionTitle("Calendar"),
        caption("月历：点击日期即选中（主色实心），描边圈标出今天，‹ / › 切换月份。"),
        vgap(10.0),
        calendar,
        vgap(16.0),
        sectionTitle("DataTable"),
        caption("点击表头排序（再次点击反向）；数字列右对齐；斑马纹。"),
        vgap(8.0),
        dataTable,
        vgap(16.0),
        sectionTitle("TreeView"),
        caption("点击行展开/折叠子树。"),
        vgap(8.0),
        treeCard,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 动画页：基于本框架动画系统实现（纯控件自绘）
// ══════════════════════════════════════════════════════════════════════════════

class AnimationPage;  // 前向声明（State 与 Widget 互相引用）

/// 动画页的 State（成员函数在 AnimationPage 定义之后实现，避免循环依赖）。
class AnimationPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    /// 开关生效：调整每个 demo 的循环设置并启停动画。
    void setContinuous(bool v);

    SharedPtr<AnimPlayback> playback_ = cui::make_shared<AnimPlayback>();
};

class AnimationPage : public StatefulWidget {
public:
    explicit AnimationPage(std::vector<AnimDemoSpec> demos, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), demos_(std::move(demos)) {}

    const std::vector<AnimDemoSpec>& demos() const { return demos_; }
    std::unique_ptr<State> createState() override {
        return std::make_unique<AnimationPageState>();
    }

private:
    std::vector<AnimDemoSpec> demos_;
    friend class AnimationPageState;
};

inline SharedPtr<Widget> AnimationPageState::build(BuildContext&) {
    auto* w = static_cast<AnimationPage*>(widget());
    auto& demos = w->demos();
    bool on = playback_->continuous;

    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(sectionTitle("Animation"));
    kids.push_back(caption("基于本框架 动画系统（Animation / Tween / SpringSimulation / "
                           "CurvedAnimation / Interval / TweenSequence / AnimatedValue / "
                           "AnimatedBuilder）重写的动画演示。"));
    kids.push_back(vgap(10.0));

    // ── 连续播放总开关 ─────────────────────────────────────────────────
    std::vector<SharedPtr<Widget>> switchKids;
    switchKids.push_back(cui::make_shared<Switch>(on, [this](bool v) { setContinuous(v); }));
    switchKids.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
    switchKids.push_back(
        cui::make_shared<Text>(std::string("连续播放：") + (on ? "开（不停循环）" : "关")));
    switchKids.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
    switchKids.push_back(cui::make_shared<Text>(
        std::string(on ? "所有演示循环运行" : "点击各演示的按钮单独播放"),
        TextStyle().size(12.0).fgColor(captionColor())));
    auto switchRow = cui::make_shared<Row>(std::move(switchKids));
    switchRow->setMainAxisSize(MainAxisSize::Min);
    switchRow->setCrossAxisAlignment(CrossAxisAlignment::Center);

    kids.push_back(switchRow);
    kids.push_back(vgap(16.0));

    for (auto& s : demos) {
        kids.push_back(cui::make_shared<AnimDemo>(s));
        kids.push_back(vgap(16.0));
    }
    return pageBody(std::move(kids));
}

inline void AnimationPageState::setContinuous(bool v) {
    playback_->continuous = v;
    auto* w = static_cast<AnimationPage*>(widget());
    for (auto& s : w->demos()) {
        if (!s.clk || !s.clk->anim)
            continue;  // 子控件尚未挂载（anim 在其 initState 里创建）
        auto* anim = s.clk->anim.get();
        if (v) {
            // 连续播放：单向循环，播完自动继续。
            anim->setLoop(true);
            anim->setAutoReverse(false);
            if (s.clk->run)
                s.clk->run();
        } else {
            // 恢复各 demo 自己的循环设置，并停下动画。
            anim->setLoop(s.loop);
            anim->setAutoReverse(s.loop);
            if (s.clk->halt)
                s.clk->halt();
        }
    }
    if (mounted())
        setState();
}

/// 动画页：带「连续播放」总开关。
///
/// 开关打开后所有 demo 强制单向循环播放（播完自动从头再来），关闭则停下。

/// 构建动画页的全部 demo（每个 demo 共享同一个播放控制对象）。
inline std::vector<AnimDemoSpec> buildAnimationDemos(SharedPtr<AnimPlayback> playback) {
    using cui::AnimClock;
    using cui::AnimDemoSpec;

    std::vector<AnimDemoSpec> demos;

    // ── 1 · Tween ──
    {
        auto clk = cui::make_shared<AnimClock>();
        Tween<Color> colorTween(Color::fromRGB(0.9, 0.3, 0.3), Color::fromRGB(0.2, 0.5, 0.95));
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "1 · Tween";
        s.caption = "一个控制器同时驱动 位置 + 颜色 + 尺寸";
        s.durationMs = 1500.0;
        s.easing = Easing::EaseInOutCubic;
        s.paint = [colorTween](Painter& p, const Size& sz, AnimationController& a) {
            double pr = a.progress();
            double x = 20.0 + (sz.width - 80.0) * pr;
            double d = 30.0 + 50.0 * pr;
            p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.12));
            p.setLineWidth(2.0);
            p.moveTo(20.0, sz.height / 2.0);
            p.lineTo(sz.width - 60.0, sz.height / 2.0);
            p.stroke();
            p.setSourceColor(colorTween.evaluate(a));
            p.fillRoundedRect(Rect{x, sz.height / 2.0 - d / 2.0, d, d}, 8.0);
        };
        s.buttons = {{"Start Tween", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 2 · SpringSimulation ──
    {
        auto clk = cui::make_shared<AnimClock>();
        std::vector<SpringSimulation> springs;
        springs.emplace_back(1.0, 120.0, 4.0);    // 欠阻尼（振荡/过冲）
        springs.emplace_back(1.0, 120.0, 21.9);   // 临界阻尼
        springs.emplace_back(1.0, 120.0, 45.0);   // 过阻尼
        for (auto& sp : springs) { sp.setStart(0.0); sp.setEnd(1.0); }
        std::vector<Color> cols = {Color::fromRGB(0.9, 0.3, 0.3), Color::fromRGB(0.2, 0.7, 0.4),
                                   Color::fromRGB(0.3, 0.5, 0.95)};
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "2 · SpringSimulation";
        s.caption = "欠阻尼 / 临界 / 过阻尼（物理弹簧）";
        s.durationMs = 3000.0;
        s.paint = [springs, cols](Painter& p, const Size& sz, AnimationController& a) {
            double t = a.elapsed() / 1000.0;
            for (int i = 0; i < 3; ++i) {
                double y = sz.height * (0.25 + 0.25 * i);
                p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.1));
                p.setLineWidth(2.0);
                p.moveTo(20.0, y);
                p.lineTo(sz.width - 40.0, y);
                p.stroke();
            }
            for (int i = 0; i < 3; ++i) {
                double v = std::clamp(springs[i].value(t), 0.0, 1.2);
                double x = 20.0 + (sz.width - 60.0) * std::min(v, 1.0);
                double y = sz.height * (0.25 + 0.25 * i);
                p.setSourceColor(cols[i]);
                p.circle(x, y, 8.0);
                p.fill();
            }
        };
        s.buttons = {{"Release Spring", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 3 · CurvedAnimation ──
    {
        auto clk = cui::make_shared<AnimClock>();
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "3 · CurvedAnimation";
        s.caption = "同一控制器，四条不同曲线";
        s.durationMs = 1600.0;
        s.loop = true;
        s.paint = [clk](Painter& p, const Size& sz, AnimationController&) {
            Easing es[] = {Easing::EaseInOutCubic, Easing::EaseOutBack, Easing::EaseInOutBack,
                           Easing::EaseOutCubic};
            for (int i = 0; i < 4; ++i) {
                CurvedAnimation ca(clk->anim, es[i]);
                double v = ca.progress();
                double y = sz.height * (0.2 + 0.2 * i);
                p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.1));
                p.setLineWidth(2.0);
                p.moveTo(20.0, y);
                p.lineTo(sz.width - 40.0, y);
                p.stroke();
                p.setSourceColor(Color::fromRGB(0.2, 0.5, 0.95));
                double x = 20.0 + (sz.width - 60.0) * v;
                p.circle(x, y, 8.0);
                p.fill();
            }
        };
        s.buttons = {{"Start Curves", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 4 · AnimatedValue（隐式动画）──
    {
        auto clk = cui::make_shared<AnimClock>();
        auto av = cui::make_shared<AnimatedValue<double>>(0.0, 600.0, Easing::EaseInOutCubic);
        auto idx = cui::make_shared<int>(0);
        std::vector<double> targets = {0.15, 0.55, 0.85, 0.35, 0.7, 0.95, 0.25};
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "4 · AnimatedValue（隐式动画）";
        s.caption = "点击 Randomize，自动过渡到新目标";
        s.durationMs = 1e9;
        s.onTick = [av](Duration) {
            av->tick(Duration(0));
            return av->isAnimating();
        };
        s.paint = [av](Painter& p, const Size& sz, AnimationController&) {
            double v = av->value();
            double x = 20.0 + (sz.width - 60.0) * v;
            p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.1));
            p.setLineWidth(2.0);
            p.moveTo(20.0, sz.height / 2.0);
            p.lineTo(sz.width - 40.0, sz.height / 2.0);
            p.stroke();
            p.setSourceColor(Color::fromRGB(0.95, 0.65, 0.15));
            p.circle(x, sz.height / 2.0, 14.0);
            p.fill();
            adDrawText(p, std::to_string((int)(v * 100)) + "%", 20.0, sz.height - 8.0, 11.0,
                       theme().textSecondary);
        };
        s.buttons = {{"Randomize Target", [clk, av, idx, targets]() {
                         av->setValue(targets[(*idx) % targets.size()]);
                         ++(*idx);
                         clk->run();
                     }}};
        demos.push_back(std::move(s));
    }

    // ── 5 · fling（惯性运动）──
    {
        auto clk = cui::make_shared<AnimClock>();
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "5 · AnimationController::fling（惯性运动）";
        s.caption = "不同初速度的减速滑行";
        s.durationMs = 4000.0;
        s.paint = [clk](Painter& p, const Size& sz, AnimationController& a) {
            double v = a.progress();
            double x = 20.0 + (sz.width - 60.0) * std::clamp(v, 0.0, 1.0);
            p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.1));
            p.setLineWidth(2.0);
            p.moveTo(20.0, sz.height / 2.0);
            p.lineTo(sz.width - 40.0, sz.height / 2.0);
            p.stroke();
            p.setSourceColor(Color::fromRGB(0.9, 0.3, 0.3));
            p.circle(x, sz.height / 2.0, 12.0);
            p.fill();
        };
        s.buttons = {
            {"Slow Fling", [clk]() { clk->anim->fling(0.5); clk->resume(); }},
            {"Medium Fling", [clk]() { clk->anim->fling(1.5); clk->resume(); }},
            {"Fast Fling", [clk]() { clk->anim->fling(3.0); clk->resume(); }},
        };
        demos.push_back(std::move(s));
    }

    // ── 6 · TweenSequence（方形路径）──
    {
        auto clk = cui::make_shared<AnimClock>();
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "6 · TweenSequence";
        s.caption = "多段路径：右 → 下 → 左 → 上";
        s.durationMs = 2400.0;
        s.loop = true;
        s.paint = [clk](Painter& p, const Size& sz, AnimationController& a) {
            double t = a.rawProgress();
            double m = 24.0;
            TweenSequence<Point> seq;
            seq.add(Tween<Point>(Point{m, m}, Point{sz.width - m, m}), 1.0, Easing::EaseInOutSine);
            seq.add(Tween<Point>(Point{sz.width - m, m}, Point{sz.width - m, sz.height - m}), 1.0,
                    Easing::EaseInOutSine);
            seq.add(Tween<Point>(Point{sz.width - m, sz.height - m}, Point{m, sz.height - m}), 1.0,
                    Easing::EaseInOutSine);
            seq.add(Tween<Point>(Point{m, sz.height - m}, Point{m, m}), 1.0, Easing::EaseInOutSine);
            Point pos = seq.valueAt(t);
            p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.12));
            p.setLineWidth(2.0);
            p.rectangle(Rect{m, m, sz.width - 2 * m, sz.height - 2 * m});
            p.stroke();
            p.setSourceColor(Color::fromRGB(0.2, 0.5, 0.95));
            p.circle(pos.x, pos.y, 10.0);
            p.fill();
        };
        s.buttons = {{"Start Sequence", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 7 · Interval + ReverseAnimation（错峰）──
    {
        auto clk = cui::make_shared<AnimClock>();
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "7 · Interval + ReverseAnimation";
        s.caption = "一个控制器的错峰属性";
        s.durationMs = 1800.0;
        s.loop = true;
        s.paint = [clk](Painter& p, const Size& sz, AnimationController& a) {
            double t = a.rawProgress();
            double baseY = sz.height - 20.0;
            double maxH = sz.height - 50.0;
            double starts[] = {0.0, 0.2, 0.4, 0.6};
            Color cols[] = {Color::fromRGB(0.9, 0.3, 0.3), Color::fromRGB(0.2, 0.7, 0.4),
                            Color::fromRGB(0.3, 0.5, 0.95), Color::fromRGB(0.95, 0.65, 0.15)};
            for (int i = 0; i < 4; ++i) {
                Interval iv(starts[i], starts[i] + 0.4);
                double h = maxH * iv.map(t);
                p.setSourceColor(cols[i]);
                p.rectangle(Rect{30.0 + i * 70.0, baseY - h, 40.0, h});
                p.fill();
            }
            ReverseAnimation ra(clk->anim);
            double rp = ra.progress();
            p.setSourceColor(Color::fromRGB(0.6, 0.4, 0.9));
            p.circle(sz.width - 50.0, sz.height / 2.0, 6.0 + 20.0 * rp);
            p.fill();
        };
        s.buttons = {{"Start Staggered", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 8 · Spring Presets ──
    {
        auto clk = cui::make_shared<AnimClock>();
        std::vector<std::string> names = {"Default", "Stiff", "Bouncy", "Smooth", "Gentle"};
        SpringPreset presets[] = {springs::kDefaultSpring, springs::kStiffSpring, springs::kBouncySpring,
                                  springs::kSmoothSpring, springs::kGentleSpring};
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "8 · Spring Presets";
        s.caption = "Default / Stiff / Bouncy / Smooth / Gentle";
        s.durationMs = 2600.0;
        s.paint = [clk, names, presets](Painter& p, const Size& sz, AnimationController& a) {
            double t = a.elapsed() / 1000.0;
            for (int i = 0; i < 5; ++i) {
                auto sim = presets[i].toSimulation(0.0, 1.0);
                double v = std::clamp(sim.value(t), 0.0, 1.2);
                double x = 20.0 + (sz.width - 60.0) * std::min(v, 1.0);
                double y = sz.height * (0.18 + 0.16 * i);
                p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.08));
                p.setLineWidth(1.0);
                p.moveTo(20.0, y);
                p.lineTo(sz.width - 40.0, y);
                p.stroke();
                p.setSourceColor(Color::fromRGB(0.3, 0.5, 0.95));
                p.circle(x, y, 7.0);
                p.fill();
                adDrawText(p, names[i], sz.width - 92.0, y + 4.0, 11.0, theme().textSecondary);
            }
        };
        s.buttons = {{"Release Springs", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 9 · Easing Showcase（全部 25 条曲线）──
    {
        auto clk = cui::make_shared<AnimClock>();
        static const Easing easings[] = {
            Easing::Linear,       Easing::EaseInQuad,    Easing::EaseOutQuad,   Easing::EaseInOutQuad,
            Easing::EaseInCubic,  Easing::EaseOutCubic,  Easing::EaseInOutCubic, Easing::EaseInQuart,
            Easing::EaseOutQuart, Easing::EaseInOutQuart, Easing::EaseInQuint,  Easing::EaseOutQuint,
            Easing::EaseInOutQuint, Easing::EaseInSine,  Easing::EaseOutSine,  Easing::EaseInOutSine,
            Easing::EaseInExpo,   Easing::EaseOutExpo,   Easing::EaseInOutExpo, Easing::EaseInCirc,
            Easing::EaseOutCirc,  Easing::EaseInOutCirc, Easing::EaseInBack,    Easing::EaseOutBack,
            Easing::EaseInOutBack, Easing::Spring};
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "9 · Easing Showcase";
        s.caption = "全部 25 条缓动曲线";
        s.durationMs = 2000.0;
        s.loop = true;
        s.canvasHeight = 384.0;
        s.paint = [clk](Painter& p, const Size& sz, AnimationController& a) {
            double t = a.rawProgress();
            double rowH = (sz.height - 10.0) / 25.0;
            for (int i = 0; i < 25; ++i) {
                double v = std::clamp(applyEasing(t, easings[i]), 0.0, 1.0);
                double x = 20.0 + (sz.width - 60.0) * v;
                double y = 6.0 + rowH * i + rowH / 2.0;
                p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.08));
                p.setLineWidth(1.0);
                p.moveTo(20.0, y);
                p.lineTo(sz.width - 40.0, y);
                p.stroke();
                p.setSourceColor(Color::fromRGB(0.25, 0.55, 0.95));
                p.circle(x, y, 4.0);
                p.fill();
            }
        };
        s.buttons = {{"Start Easing", [clk]() { clk->run(); }}};
        demos.push_back(std::move(s));
    }

    // ── 10 · Animation Control（暂停 / 继续 / 重置）──
    {
        auto clk = cui::make_shared<AnimClock>();
        AnimDemoSpec s;
        s.clk = clk;
        s.title = "10 · Animation Control";
        s.caption = "暂停 / 继续 / 重置";
        s.durationMs = 2000.0;
        s.loop = true;
        s.paint = [clk](Painter& p, const Size& sz, AnimationController& a) {
            double v = a.progress();
            double x = 20.0 + (sz.width - 60.0) * v;
            p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.1));
            p.setLineWidth(2.0);
            p.moveTo(20.0, sz.height / 2.0);
            p.lineTo(sz.width - 40.0, sz.height / 2.0);
            p.stroke();
            p.setSourceColor(Color::fromRGB(0.2, 0.7, 0.4));
            p.circle(x, sz.height / 2.0, 12.0);
            p.fill();
        };
        s.buttons = {
            {"Pause", [clk]() { clk->anim->pause(); clk->halt(); }},
            {"Resume", [clk]() { clk->anim->resume(); clk->resume(); }},
            {"Reset", [clk]() { clk->anim->reset(); clk->resume(); }},
        };
        demos.push_back(std::move(s));
    }

    // 统一挂上播放控制对象（连续播放总开关）。
    for (auto& s : demos)
        s.playback = playback;
    return demos;
}

/// 动画页入口：demos 共享缓存，保证页面重建时动画状态与开关状态不丢失。
inline SharedPtr<Widget> pageFlAnimation() {
    static SharedPtr<AnimPlayback> playback = cui::make_shared<AnimPlayback>();
    static std::vector<AnimDemoSpec> demos = buildAnimationDemos(playback);
    return cui::make_shared<AnimationPage>(demos);
}

// ══════════════════════════════════════════════════════════════════════════════
// 图表页：基于 chart 控件集实现（纯控件自绘）
// ══════════════════════════════════════════════════════════════════════════════

/// 图表目录：每种图表一个 (标签, 控件)，供下拉选择页取用。
inline std::vector<std::pair<std::string, SharedPtr<Widget>>> flChartCatalog() {
    using namespace chart;
    // ── Line ──
    LineChartData line;
    line.titlesData.bottom.getTitles = [](double v) { return std::to_string((int)std::round(v)); };
    line.titlesData.left.getTitles = [](double v) { return std::to_string((int)std::round(v)); };
    {
        LineChartBarData online;
        online.color = Color::fromRGB(0.25, 0.55, 0.95);
        online.isCurved = true;
        online.dotData = false;
        online.showBelow = true;
        online.belowColor = Color::fromRGBA(0.25, 0.55, 0.95, 0.12);
        double ys[] = {32, 41, 38, 52, 48, 61, 55, 72, 68, 80, 75, 92};
        for (int i = 0; i < 12; ++i) online.spots.push_back(ChartSpot(i + 1, ys[i]));
        LineChartBarData offline;
        offline.color = Color::fromRGB(0.9, 0.35, 0.3);
        offline.isCurved = true;
        offline.dotData = false;
        double ys2[] = {45, 42, 50, 47, 53, 49, 58, 55, 60, 57, 63, 60};
        for (int i = 0; i < 12; ++i) offline.spots.push_back(ChartSpot(i + 1, ys2[i]));
        line.lineBarsData = {online, offline};
    }
    auto lineWidget = cui::make_shared<LineChart>(std::move(line), Size(360.0, 220.0));

    // ── Bar（分组 · 圆角）──
    BarChartData bar;
    {
        Color c1 = Color::fromRGB(0.25, 0.55, 0.95);
        Color c2 = Color::fromRGB(0.2, 0.75, 0.45);
        Color c3 = Color::fromRGB(0.95, 0.65, 0.15);
        double d1[] = {40, 52, 44, 60}, d2[] = {30, 26, 34, 28}, d3[] = {15, 18, 12, 20};
        for (int q = 0; q < 4; ++q) {
            BarChartGroupData g;
            g.x = q + 1;
            g.barRods = {BarChartRodData{d1[q], c1, 14.0, 3.0},
                         BarChartRodData{d2[q], c2, 14.0, 3.0},
                         BarChartRodData{d3[q], c3, 14.0, 3.0}};
            bar.groups.push_back(g);
        }
        bar.titlesData.bottom.getTitles = [](double v) { return "Q" + chartDefaultLabel(v); };
    }
    auto barWidget = cui::make_shared<chart::BarChart>(std::move(bar), Size(360.0, 220.0));

    // ── Pie（甜甜圈）──
    PieChartData pie;
    pie.centerSpaceRadius = 28.0;
    pie.sections = {
        PieChartSectionData{35.0, Color::fromRGB(0.25, 0.55, 0.95), 50.0, "A", 12.0, Color::fromRGB(1.0, 1.0, 1.0)},
        PieChartSectionData{25.0, Color::fromRGB(0.9, 0.35, 0.3), 50.0, "B"},
        PieChartSectionData{18.0, Color::fromRGB(0.2, 0.75, 0.45), 50.0, "C"},
        PieChartSectionData{12.0, Color::fromRGB(0.95, 0.65, 0.15), 50.0, "D"},
        PieChartSectionData{10.0, Color::fromRGB(0.6, 0.35, 0.85), 50.0, "E"},
    };
    auto pieWidget = cui::make_shared<PieChart>(std::move(pie), Size(220.0, 220.0));

    // ── Scatter ──
    ScatterChartData scatter;
    scatter.titlesData.bottom.getTitles = [](double v) { return std::to_string((int)std::round(v)); };
    scatter.titlesData.left.getTitles = [](double v) { return std::to_string((int)std::round(v)); };
    {
        Color cm = Color::fromRGB(0.25, 0.55, 0.95), cf = Color::fromRGB(0.9, 0.35, 0.3);
        double mx[] = {165, 170, 175, 180, 168, 172, 178, 183, 166, 174};
        double my[] = {65, 70, 75, 80, 68, 72, 78, 85, 63, 74};
        double fx[] = {155, 160, 165, 170, 158, 163, 168, 173, 157, 166};
        double fy[] = {50, 55, 58, 62, 52, 57, 60, 65, 49, 59};
        for (int i = 0; i < 10; ++i) scatter.spots.push_back(ScatterChartSpot{mx[i], my[i], 5.0, cm});
        for (int i = 0; i < 10; ++i) scatter.spots.push_back(ScatterChartSpot{fx[i], fy[i], 5.0, cf});
    }
    auto scatterWidget = cui::make_shared<ScatterChart>(std::move(scatter), Size(360.0, 220.0));

    // ── Radar ──
    RadarChartData radar;
    radar.features = {{"Attack", 100.0}, {"Defense", 100.0}, {"Speed", 100.0},
                      {"Magic", 100.0}, {"Stamina", 100.0}, {"Luck", 100.0}};
    {
        RadarDataSet hero;
        hero.name = "Hero";
        hero.color = Color::fromRGB(0.25, 0.55, 0.95);
        hero.values = {80, 55, 90, 40, 70, 60};
        RadarDataSet rival;
        rival.name = "Rival";
        rival.color = Color::fromRGB(0.9, 0.35, 0.3);
        rival.values = {60, 85, 50, 75, 55, 80};
        radar.dataSets = {hero, rival};
    }
    auto radarWidget = cui::make_shared<RadarChart>(std::move(radar), Size(320.0, 320.0));

    // ── StepLine（阶梯线）──
    LineChartData step;
    {
        LineChartBarData price;
        price.color = Color::fromRGB(0.20, 0.75, 0.45);
        price.isStep = true;
        price.dotData = true;
        double ys[] = {20, 35, 28, 50, 42, 60, 55, 70};
        for (int i = 0; i < 8; ++i) price.spots.push_back(ChartSpot(i + 1, ys[i]));
        step.lineBarsData = {price};
    }
    auto stepWidget = cui::make_shared<LineChart>(std::move(step), Size(360.0, 200.0));

    // ── StepArea（阶梯面积）──
    LineChartData stepArea;
    {
        LineChartBarData price;
        price.color = Color::fromRGB(0.20, 0.75, 0.45);
        price.isStep = true;
        price.showBelow = true;
        price.belowColor = Color::fromRGBA(0.20, 0.75, 0.45, 0.12);
        price.dotData = false;
        double ys[] = {20, 35, 28, 50, 42, 60, 55, 70};
        for (int i = 0; i < 8; ++i) price.spots.push_back(ChartSpot(i + 1, ys[i]));
        stepArea.lineBarsData = {price};
    }
    auto stepAreaWidget = cui::make_shared<LineChart>(std::move(stepArea), Size(360.0, 200.0));

    // ── SplineArea（平滑面积）──
    LineChartData splineArea;
    {
        LineChartBarData t;
        t.color = Color::fromRGB(0.60, 0.35, 0.85);
        t.isCurved = true;
        t.showBelow = true;
        t.belowColor = Color::fromRGBA(0.60, 0.35, 0.85, 0.12);
        t.dotData = false;
        double ysa[] = {25, 40, 32, 55, 48, 70, 62, 80, 75, 88};
        for (int i = 0; i < 10; ++i) t.spots.push_back(ChartSpot(i + 1, ysa[i]));
        splineArea.lineBarsData = {t};
    }
    auto splineAreaWidget = cui::make_shared<LineChart>(std::move(splineArea), Size(360.0, 200.0));

    // ── Bubble（气泡）──
    BubbleChartData bubble;
    {
        Color bc = Color::fromRGB(0.25, 0.55, 0.95);
        bubble.spots = {
            BubbleSpot{20, 30, 2.5, bc}, BubbleSpot{35, 50, 3.5, bc},
            BubbleSpot{50, 25, 1.5, bc}, BubbleSpot{65, 65, 4.0, bc},
            BubbleSpot{80, 45, 2.0, bc}, BubbleSpot{45, 80, 3.0, bc}};
        bubble.maxBubbleRadius = 30.0;
    }
    auto bubbleWidget = cui::make_shared<BubbleChart>(std::move(bubble), Size(360.0, 220.0));

    // ── Waterfall（瀑布）──
    WaterfallChartData waterfall;
    waterfall.items = {
        WaterfallItem{"Revenue", 100, true},
        WaterfallItem{"COGS", -40, false},
        WaterfallItem{"OpEx", -25, false},
        WaterfallItem{"Tax", -10, false},
        WaterfallItem{"Net", 25, true},
    };
    auto waterfallWidget = cui::make_shared<WaterfallChart>(std::move(waterfall), Size(360.0, 240.0));

    // ── Histogram（直方图）──
    HistogramChartData hist;
    hist.binCount = 10;
    hist.samples = {72,85,90,68,75,82,95,78,88,92,70,80,85,77,90,83,76,89,94,71,
                    84,79,87,91,73,86,81,93,74,88,79,85,90,77,82,86,91,75,83,89};
    auto histWidget = cui::make_shared<HistogramChart>(std::move(hist), Size(360.0, 220.0));

    // ── Candlestick（K 线）──
    CandlestickChartData candle;
    candle.candles = {
        CandleData{0, 100, 110, 98, 108}, CandleData{1, 108, 112, 102, 104},
        CandleData{2, 104, 118, 103, 115}, CandleData{3, 115, 119, 108, 110},
        CandleData{4, 110, 125, 109, 122}, CandleData{5, 122, 126, 116, 118},
        CandleData{6, 118, 133, 117, 130}, CandleData{7, 130, 134, 124, 126},
    };
    auto candleWidget = cui::make_shared<CandlestickChart>(std::move(candle), Size(360.0, 220.0));

    // ── Sankey（桑基）──
    SankeyChartData sankey;
    sankey.nodes = {
        SankeyNodeData{"Solar", 30}, SankeyNodeData{"Wind", 25}, SankeyNodeData{"Gas", 40},
        SankeyNodeData{"Grid", 70}, SankeyNodeData{"Storage", 25},
        SankeyNodeData{"Industry", 45}, SankeyNodeData{"Homes", 30}};
    sankey.links = {
        SankeyLinkData{0, 3, 22}, SankeyLinkData{0, 4, 8},
        SankeyLinkData{1, 3, 18}, SankeyLinkData{1, 4, 7},
        SankeyLinkData{2, 3, 30}, SankeyLinkData{2, 5, 10},
        SankeyLinkData{3, 5, 42}, SankeyLinkData{3, 6, 28},
        SankeyLinkData{4, 6, 15}};
    auto sankeyWidget = cui::make_shared<SankeyChart>(std::move(sankey), Size(440.0, 260.0));

    // ── Polar（极坐标）──
    PolarChartData polar;
    polar.categories = {"Jan", "Feb", "Mar", "Apr", "May", "Jun"};
    {
        PolarDataSet ds;
        ds.name = "2025";
        ds.color = Color::fromRGB(0.25, 0.55, 0.95);
        ds.values = {60, 75, 50, 80, 65, 90};
        polar.dataSets = {ds};
    }
    auto polarWidget = cui::make_shared<PolarChart>(std::move(polar), Size(340.0, 320.0));

    // ── RadialBar（环形进度）──
    RadialBarChartData radial;
    radial.dataSets = {
        RadialBarDataSet{"Sales", 85, 100, Color::fromRGB(0.25, 0.55, 0.95)},
        RadialBarDataSet{"Marketing", 72, 100, Color::fromRGB(0.20, 0.75, 0.45)},
        RadialBarDataSet{"Support", 68, 100, Color::fromRGB(0.95, 0.65, 0.15)},
        RadialBarDataSet{"Dev", 90, 100, Color::fromRGB(0.60, 0.35, 0.85)},
        RadialBarDataSet{"Design", 78, 100, Color::fromRGB(0.90, 0.35, 0.30)},
    };
    auto radialWidget = cui::make_shared<RadialBarChart>(std::move(radial), Size(300.0, 300.0));

    // ── Funnel（漏斗）──
    FunnelChartData funnel;
    funnel.slices = {
        FunnelSlice{"Visitors", 1000, chartPalette(0)},
        FunnelSlice{"Leads", 650, chartPalette(1)},
        FunnelSlice{"Qualified", 420, chartPalette(2)},
        FunnelSlice{"Proposals", 280, chartPalette(3)},
        FunnelSlice{"Negotiation", 150, chartPalette(4)},
        FunnelSlice{"Closed", 85, chartPalette(5)},
    };
    auto funnelWidget = cui::make_shared<FunnelChart>(std::move(funnel), Size(320.0, 260.0));

    // ── Pyramid（金字塔）──
    FunnelChartData pyramid;
    pyramid.slices = {
        FunnelSlice{"0-18", 25, chartPalette(0)},
        FunnelSlice{"19-35", 35, chartPalette(1)},
        FunnelSlice{"36-50", 30, chartPalette(2)},
        FunnelSlice{"51-65", 20, chartPalette(3)},
        FunnelSlice{"65+", 15, chartPalette(4)},
    };
    auto pyramidWidget = cui::make_shared<PyramidChart>(std::move(pyramid), Size(320.0, 260.0));

    // ── Area（面积：折线 + 下方填充，不平滑）──
    LineChartData area;
    {
        LineChartBarData v;
        v.color = Color::fromRGB(0.25, 0.55, 0.95);
        v.showBelow = true;
        v.belowColor = Color::fromRGBA(0.25, 0.55, 0.95, 0.12);
        v.dotData = false;
        double ys[] = {20, 35, 30, 55, 48, 72, 64, 88, 80, 96};
        for (int i = 0; i < 10; ++i) v.spots.push_back(ChartSpot(i + 1, ys[i]));
        area.lineBarsData = {v};
    }
    auto areaWidget = cui::make_shared<LineChart>(std::move(area), Size(360.0, 200.0));

    // ── Spline（平滑曲线，无填充）──
    LineChartData spline;
    {
        LineChartBarData t;
        t.color = Color::fromRGB(0.60, 0.35, 0.85);
        t.isCurved = true;
        t.dotData = true;
        double ys[] = {25, 40, 32, 55, 48, 70, 62, 80, 75, 88};
        for (int i = 0; i < 10; ++i) t.spots.push_back(ChartSpot(i + 1, ys[i]));
        spline.lineBarsData = {t};
    }
    auto splineWidget = cui::make_shared<LineChart>(std::move(spline), Size(360.0, 200.0));

    // ── Stacked（堆叠柱）──
    BarChartData stacked;
    {
        Color c1 = Color::fromRGB(0.25, 0.55, 0.95);
        Color c2 = Color::fromRGB(0.20, 0.75, 0.45);
        Color c3 = Color::fromRGB(0.95, 0.65, 0.15);
        double d1[] = {40, 52, 44, 60}, d2[] = {30, 26, 34, 28}, d3[] = {15, 18, 12, 20};
        for (int q = 0; q < 4; ++q) {
            BarChartGroupData g;
            g.x = q + 1;
            g.barRods = {BarChartRodData{d1[q], c1, 16.0, 0.0},
                         BarChartRodData{d2[q], c2, 16.0, 0.0},
                         BarChartRodData{d3[q], c3, 16.0, 0.0}};
            stacked.groups.push_back(g);
        }
        stacked.stacked = true;
        stacked.titlesData.bottom.getTitles = [](double v) { return "Q" + chartDefaultLabel(v); };
    }
    auto stackedWidget = cui::make_shared<chart::BarChart>(std::move(stacked), Size(360.0, 220.0));

    // 图表标签页顺序（Line…Pyramid）。
    return {
        {"Line", lineWidget},
        {"Area", areaWidget},
        {"Stacked", stackedWidget},
        {"Grouped", barWidget},
        {"Donut", cui::make_shared<Center>(pieWidget)},
        {"Radar", cui::make_shared<Center>(radarWidget)},
        {"Candle", candleWidget},
        {"Sankey", sankeyWidget},
        {"Scatter", scatterWidget},
        {"Bubble", bubbleWidget},
        {"StepLine", stepWidget},
        {"Spline", splineWidget},
        {"StepArea", stepAreaWidget},
        {"SplineArea", splineAreaWidget},
        {"Waterfall", waterfallWidget},
        {"Histogram", histWidget},
        {"Polar", cui::make_shared<Center>(polarWidget)},
        {"RadialBar", cui::make_shared<Center>(radialWidget)},
        {"Funnel", cui::make_shared<Center>(funnelWidget)},
        {"Pyramid", cui::make_shared<Center>(pyramidWidget)},
    };
}

inline SharedPtr<Widget> pageFlChart() {
    auto entries = flChartCatalog();
    std::vector<std::string> labels;
    std::vector<SharedPtr<Widget>> tabPages;
    labels.reserve(entries.size());
    tabPages.reserve(entries.size());
    for (auto& e : entries) {
        labels.push_back(e.first);
        // 路由内容经 Overlay(Stack) 拿到的是无界约束：高度无限，宽度需强制撑满
        // 才能占满内容区（图表的 CustomPaint 在紧宽度下自动按实际尺寸绘制）。
        // 只强制宽度（Container.width 钳进父约束），高度保持图表自然高度——
        // 否则套 1e18 高会因无界变成 ∞ 高，Skia 巨幅绘制直接 OOM/卡死。
        // Center 包裹的（Donut/Radar/Polar）在满宽容器内居中，效果不变。
        auto box = cui::make_shared<Container>(e.second);
        box->width(1e18);
        tabPages.push_back(box);
    }
    auto tabs = cui::make_shared<Tabs>(labels, tabPages);

    // 图表占满页面剩余空间：Column 撑满父高，Tabs 用 Expanded 占满（bar 下方）。
    auto cap = caption("图表控件集（纯控件自绘）；"
                       "每种图表一个标签页，与设计器里的图表页组织一致；"
                       "标签过多时 TabBar 右侧自动折叠为下拉列表。");
    auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cap, vgap(10.0), cui::make_shared<Expanded>(1, tabs)});
    col->setMainAxisSize(MainAxisSize::Max);
    // Stretch：TabBarView(IndexedStack) 给页面的是松宽度约束，Start 会让图表缩回
    // 首选 360 宽；Stretch 把横轴抬到内容区满宽（图表的 CustomPaint 在紧约束下
    // 自动撑满，绘制本就按实际尺寸走）。
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto c = cui::make_shared<Container>(col);
    c->padding(EdgeInsets::symmetric(12.0, 20.0));
    return c;
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面 13：滚动进阶（GridView.builder 5 列）
// ══════════════════════════════════════════════════════════════════════════════

class ScrollAdvPage : public StatefulWidget {
public:
    ScrollAdvPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ScrollAdvPage"; }
};

class ScrollAdvPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        auto grid = GridView::builder(
            &ctrl_,
            cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(5, 4.0, 4.0, 1.0),
            [](BuildContext&, int i) {
                auto t = cui::make_shared<Text>(
                    std::to_string(i),
                    TextStyle().size(13.0).bold().fgColor(Color::fromRGB(1, 1, 1)));
                auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
                c->decoration(BoxDecoration{
                        Color::fromRGBA(0.0, 0.45, 0.95,
                                        (i % 3 == 0) ? 0.85 : (i % 3 == 1 ? 0.65 : 0.45)),
                        6.0});
                return c;
            },
            120);
        auto area = cui::make_shared<SizedBox>(
            1e18, 260.0, cui::make_shared<Scrollbar>(&ctrl_, grid));

        return pageBody({
            sectionTitle("GridView.builder（crossAxisCount=5 · 120 项）"),
            caption("懒构建可见窗口 ±cache；Scrollbar 实时联动。"),
            vgap(10.0),
            area,
        });
    }

private:
    ScrollController ctrl_;
};

std::unique_ptr<State> ScrollAdvPage::createState() {
    return std::make_unique<ScrollAdvPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 内容容器：页面区包一层 Navigator（对话框/弹层需要 Navigator 祖先）
// ══════════════════════════════════════════════════════════════════════════════

class ShellContent : public StatefulWidget {
public:
    int pageIndex = 0;
    std::function<SharedPtr<Widget>(int)> builder;
    ShellContent() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ShellContent"; }
};

class ShellContentState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        if (!nav_) {
            nav_ = cui::make_shared<Navigator>(std::vector<SharedPtr<Page>>{},
                                               navKey_);  // shared_ptr<GlobalKey> → KeyPtr
            auto* self = this;
            nav_->setOnGenerateRoute([self](const RouteSettings&) {
                auto* w = static_cast<ShellContent*>(self->widget());
                self->shown_ = w->pageIndex;
                int idx = self->shown_;
                return cui::make_shared<ContentRoute>(
                    [self, idx](BuildContext&) {
                        auto* w2 = static_cast<ShellContent*>(self->widget());
                        return w2->builder(idx);
                    });
            });
        }
        return nav_;
    }

    void didUpdateWidget(Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = static_cast<ShellContent*>(widget());
        if (!nav_ || w->pageIndex == shown_)
            return;
        auto* el = navKey_->currentElement<Element>();
        auto* se = dynamic_cast<StatefulElement*>(el);
        auto* ns = se ? dynamic_cast<NavigatorState*>(se->state()) : nullptr;
        if (!ns)
            return;
        // 弹回根（关闭遗留对话框），再压入新页面。
        // 注意保留家路由：history 弹到只剩 1 层即可。
        while (ns->history().size() > 1)
            ns->pop();
        int idx = w->pageIndex;
        shown_ = idx;
        auto* self = this;
        ns->push(cui::make_shared<ContentRoute>(
            [self, idx](BuildContext&) {
                auto* w2 = static_cast<ShellContent*>(self->widget());
                return w2->builder(idx);
            }));
    }

private:
    SharedPtr<Navigator> nav_;
    SharedPtr<GlobalKey> navKey_ = cui::make_shared<GlobalKey>("shellNav");
    int shown_ = 0;
};

std::unique_ptr<State> ShellContent::createState() {
    return std::make_unique<ShellContentState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：Components（SegmentedControl / Kbd / Link / SearchBar / DropdownButton
//                    / RatingBar / AvatarGroup）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageComponents() {
    auto seg = cui::make_shared<SegmentedControl>(
        std::vector<std::string>{"Day", "Week", "Month"}, 0);
    auto segRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{seg});
    segRow->setMainAxisSize(MainAxisSize::Min);

    auto kbdRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Kbd>("Ctrl"), cui::make_shared<Kbd>("Shift"),
        cui::make_shared<Kbd>("P")});
    kbdRow->setMainAxisSize(MainAxisSize::Min);

    auto links = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Link>("Documentation"), cui::make_shared<Link>("API"),
        cui::make_shared<Link>("GitHub")});
    links->setMainAxisSize(MainAxisSize::Min);

    auto search = cui::make_shared<SearchBar>("Search components…");
    auto searchBox = cui::make_shared<Container>(search);
    searchBox->width(280.0);

    auto dropdown = cui::make_shared<DropdownButton>(
        std::vector<std::string>{"Apple", "Banana", "Cherry", "Date"}, 0);

    auto rating = cui::make_shared<RatingBar>(5, 3);

    auto avatarGroup = cui::make_shared<AvatarGroup>(
        std::vector<std::string>{"A", "B", "C", "D", "E", "F"},
        std::vector<Color>{Color::fromRGB(0.13, 0.59, 0.95), Color::fromRGB(0.95, 0.36, 0.36),
                           Color::fromRGB(0.26, 0.78, 0.54), Color::fromRGB(0.6, 0.4, 0.9),
                           Color::fromRGB(0.9, 0.6, 0.2), Color::fromRGB(0.2, 0.7, 0.8)},
        4);

    return pageBody({
        sectionTitle("SegmentedControl"),
        caption("分段切换控件（CupertinoSegmentedControl 简化）。"),
        vgap(8.0), segRow,
        vgap(16.0),
        sectionTitle("Kbd"),
        caption("键盘按键样式。"),
        vgap(8.0), kbdRow,
        vgap(16.0),
        sectionTitle("Link"),
        caption("可点击链接文字。"),
        vgap(8.0), links,
        vgap(16.0),
        sectionTitle("SearchBar"),
        caption("点一下即上焦，直接用键盘打字；有内容时右侧出现清除按钮。"),
        vgap(8.0), searchBox,
        vgap(16.0),
        sectionTitle("DropdownButton"),
        caption("点击展开下拉选择。"),
        vgap(8.0), dropdown,
        vgap(16.0),
        sectionTitle("RatingBar"),
        caption("点击星星评分。"),
        vgap(8.0), rating,
        vgap(16.0),
        sectionTitle("AvatarGroup"),
        caption("重叠头像组，超出 maxDisplay 显示 +N。"),
        vgap(8.0), avatarGroup,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：Navigation（Tabs / Stepper / ExpansionTile / NavigationRail / Drawer
//                     / PageView / Breadcrumb / Pagination）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageNav() {
    std::vector<SharedPtr<Widget>> tabPages = {
        cui::make_shared<Container>(cui::make_shared<Center>(cui::make_shared<Text>("首页内容"))),
        cui::make_shared<Container>(cui::make_shared<Center>(cui::make_shared<Text>("设置内容"))),
        cui::make_shared<Container>(cui::make_shared<Center>(cui::make_shared<Text>("关于内容"))),
    };
    auto tabs = cui::make_shared<Tabs>(
        std::vector<std::string>{"首页", "设置", "关于"}, tabPages);

    std::vector<Step> steps = {
        {cui::make_shared<Text>("填写信息"), cui::make_shared<Text>("第一步：输入基本资料。")},
        {cui::make_shared<Text>("确认"), cui::make_shared<Text>("第二步：核对并提交。")},
        {cui::make_shared<Text>("完成"), cui::make_shared<Text>("第三步：完成引导。")},
    };
    auto stepper = cui::make_shared<Stepper>(1, steps);

    auto expansion = cui::make_shared<ExpansionTile>(
        cui::make_shared<Text>("高级选项"),
        std::vector<SharedPtr<Widget>>{
            cui::make_shared<Text>("· 启用调试日志"),
            cui::make_shared<Text>("· 显示性能叠加层"),
            cui::make_shared<Text>("· 关闭动画")},
        false, nullptr, EdgeInsets::only(16.0, 0.0, 16.0, 8.0),
        Alignment::topLeft(), CrossAxisAlignment::Start);

    auto rail = cui::make_shared<NavigationRail>(
        std::vector<RailDestination>{
            {"首页", "", IconCode::Home},
            {"搜索", "", IconCode::Search},
            {"消息", "", IconCode::MoreVert},
            {"我的", "", IconCode::Person}}, 0);

    // ── Drawer：上游的正确用法是 Scaffold.drawer + openDrawer()，
    //    面板不摆在页面上，点按钮才滑出来。抽屉体里的「关闭抽屉」用
    //    ScaffoldState::of(context)->closeDrawer() —— 需要 Scaffold 之下的
    //    context，所以两处按钮都套 Builder 取自身位置的 context。
    auto drawerBody = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<ListTile>(cui::make_shared<Text>("项目一")),
        cui::make_shared<ListTile>(cui::make_shared<Text>("项目二")),
        cui::make_shared<ListTile>(cui::make_shared<Text>("项目三")),
        cui::make_shared<Builder>([](BuildContext& c) {
            return cui::make_shared<TextButton>(cui::make_shared<Text>("关闭抽屉"),
                                                 [&c] {
                                                     ScaffoldState::of(c)->closeDrawer();
                                                 });
        })});
    drawerBody->setMainAxisSize(MainAxisSize::Min);
    drawerBody->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    // 抽屉槽位拿到的是 Scaffold 的**紧约束**（render_scaffold.cpp 的
    // layoutChild(drawer, tight(size))），直接放 Drawer 会被撑成整宽；
    // 外包一层 Align 把约束放松，面板才保住 220 宽。
    auto drawerSlot = cui::make_shared<Align>(
        -1.0, -1.0, cui::make_shared<Drawer>(drawerBody, 220.0));
    auto openDrawerBtn = cui::make_shared<Builder>([](BuildContext& c) {
        return cui::make_shared<ElevatedButton>(cui::make_shared<Text>("打开抽屉"),
                                                [&c] {
                                                    ScaffoldState::of(c)->openDrawer();
                                                });
    });
    auto scaffoldBody = cui::make_shared<Center>(openDrawerBtn);
    auto scaffold = cui::make_shared<Scaffold>(scaffoldBody, nullptr, nullptr, nullptr,
                                               drawerSlot);
    // 关闭态的抽屉是「平移出屏」表达的（render_scaffold.cpp:272 给的是
    // -size.width），那已经在 Scaffold 盒子之外 ⇒ 不裁就会画到页面内容上，
    // 所以外面必须套 ClipRect。上游不需要这层：它的屏就是裁剪边界。
    auto drawerDemo = cui::make_shared<SizedBox>(
        520.0, 220.0, cui::make_shared<ClipRect>(scaffold));

    std::vector<SharedPtr<Widget>> pvPages = {
        cui::make_shared<Center>(cui::make_shared<Text>("Page 1")),
        cui::make_shared<Center>(cui::make_shared<Text>("Page 2")),
        cui::make_shared<Center>(cui::make_shared<Text>("Page 3")),
    };
    auto pageView = cui::make_shared<PageView>(std::move(pvPages), 0);

    auto breadcrumb = cui::make_shared<Breadcrumb>(
        std::vector<std::string>{"Home", "Library", "Data"});

    auto pagination = cui::make_shared<Pagination>(7, 1);

    return pageBody({
        sectionTitle("Tabs"),
        caption("标签页：TabBar + TabBarView（IndexedStack 切换）。"),
        vgap(8.0), tabs,
        vgap(16.0),
        sectionTitle("Stepper"),
        caption("步骤指示器（material/stepper.dart 简化）。"),
        vgap(8.0), stepper,
        vgap(16.0),
        sectionTitle("ExpansionTile"),
        caption("可展开/折叠列表项：点标题栏切换，右侧箭头随展开转半圈，主体淡入并撑开高度。"),
        vgap(8.0), expansion,
        vgap(16.0),
        sectionTitle("NavigationRail"),
        caption("竖向导航栏（点击切换高亮）。"),
        vgap(8.0), lead(rail),
        vgap(16.0),
        sectionTitle("Drawer"),
        caption("抽屉面板挂在 Scaffold.drawer 上，点「打开抽屉」经 "
                "ScaffoldState::openDrawer() 弹出（本移植无 DrawerController 动画，"
                "也没有 scrim ⇒ 只能用抽屉里的按钮关闭）。"),
        vgap(8.0), drawerDemo,
        vgap(16.0),
        sectionTitle("PageView"),
        caption("横向分页视图，箭头 + 指示点切换。"),
        vgap(8.0), pageView,
        vgap(16.0),
        sectionTitle("Breadcrumb"),
        caption("面包屑导航。"),
        vgap(8.0), breadcrumb,
        vgap(16.0),
        sectionTitle("Pagination"),
        caption("分页器（点击翻页）。"),
        vgap(8.0), pagination,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：Feedback（RatingBar / ColorPicker / SnackBar / Skeleton / EmptyState
//                     / NotificationCard / Progress / AlertDialog 展示）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageFeedback() {
    auto colorPicker = cui::make_shared<ColorPicker>(std::vector<Color>{
        Color::fromRGB(0.9, 0.2, 0.2), Color::fromRGB(0.95, 0.6, 0.1),
        Color::fromRGB(0.2, 0.7, 0.3), Color::fromRGB(0.1, 0.5, 0.9),
        Color::fromRGB(0.5, 0.3, 0.8), Color::fromRGB(0.3, 0.3, 0.3)}, 0);

    auto snackbar = cui::make_shared<SnackBar>("文件已保存", "撤销");
    auto snackbarBox = cui::make_shared<Container>(snackbar);
    snackbarBox->width(320.0);

    auto skeleton = cui::make_shared<Skeleton>(std::vector<double>{220.0, 180.0, 200.0}, 14.0);

    // 图标用字体里确定存在的字形（emoji 在多数字体里没有字形 → 豆腐块）。
    auto empty = cui::make_shared<EmptyState>("□", "No messages", "你还没有收到任何消息。");

    auto notif = cui::make_shared<NotificationCard>("新版本发布", "CUI v2.3 已可用，包含控件流水线改进。", "2 分钟前");

    auto progress = cui::make_shared<LinearProgressIndicator>(0.62);
    auto progressBox = cui::make_shared<Container>(progress);
    progressBox->width(280.0);

    auto alert = cui::make_shared<AlertDialog>(
        "确认删除", cui::make_shared<Text>("此操作不可撤销，是否继续？"),
        std::vector<SharedPtr<Widget>>{
            cui::make_shared<Text>("取消"), cui::make_shared<Text>("删除")});
    auto alertBox = cui::make_shared<Container>(alert);
    alertBox->width(320.0);

    return pageBody({
        sectionTitle("ColorPicker"),
        caption("点击色块选择颜色。"),
        vgap(8.0), colorPicker,
        vgap(16.0),
        sectionTitle("SnackBar"),
        caption("轻提示条（material/snack_bar.dart）。"),
        vgap(8.0), snackbarBox,
        vgap(16.0),
        sectionTitle("Skeleton"),
        caption("加载占位骨架屏。"),
        vgap(8.0), skeleton,
        vgap(16.0),
        sectionTitle("EmptyState"),
        caption("空状态占位。"),
        vgap(8.0), empty,
        vgap(16.0),
        sectionTitle("NotificationCard"),
        caption("通知卡片。"),
        vgap(8.0), notif,
        vgap(16.0),
        sectionTitle("LinearProgressIndicator"),
        caption("线性进度条。"),
        vgap(8.0), progressBox,
        vgap(16.0),
        sectionTitle("AlertDialog（展示）"),
        caption("对话框（material/alert_dialog.dart）。"),
        vgap(8.0), alertBox,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// ListView / GridView 演示（ScrollController 由 State 持有，避免悬垂指针）
// ══════════════════════════════════════════════════════════════════════════════

class DataListsPage : public StatefulWidget {
public:
    DataListsPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DataListsPage"; }
};

class DataListsPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        // 列表与网格都已迁到 sliver 流水线（delegate 给子节点紧交叉轴约束，
        // 不再需要 Column + Stretch 的盒子近似）。
        auto list = ListView::builder(
            &listCtrl_,
            [](BuildContext&, int i) {
                auto t = cui::make_shared<Text>("Item " + std::to_string(i + 1),
                                                TextStyle().size(13.0));
                auto c = cui::make_shared<Container>(t);
                c->padding(EdgeInsets::symmetric(8.0, 10.0))
                    .decoration(BoxDecoration{theme().surfaceVariant, 4.0});
                auto boxed = cui::make_shared<Container>(c);
                boxed->margin(EdgeInsets(0.0, 0.0, 0.0, 4.0));
                return boxed;
            },
            10, 32.0);
        auto listArea = cui::make_shared<SizedBox>(300.0, 180.0,
                                                   cui::make_shared<Scrollbar>(&listCtrl_, list));

        auto grid = GridView::builder(
            &gridCtrl_,
            cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(4, 4.0, 0.0, 1.0),
            [](BuildContext&, int i) {
                auto t = cui::make_shared<Text>(
                    std::to_string(i + 1),
                    TextStyle().size(12.0).fgColor(Color::fromRGB(1, 1, 1)));
                auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
                c->decoration(BoxDecoration{primaryColor(), 4.0});
                return c;
            },
            12);
        auto gridArea = cui::make_shared<SizedBox>(300.0, 180.0,
                                                   cui::make_shared<Scrollbar>(&gridCtrl_, grid));

        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            listArea, cui::make_shared<SizedBox>(16.0, 0.0), gridArea});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return row;
    }

private:
    ScrollController listCtrl_;
    ScrollController gridCtrl_;
};

inline std::unique_ptr<State> DataListsPage::createState() {
    return std::make_unique<DataListsPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：数据+（ListView / GridView / Color / Bubble / BarChart / DescriptionList
//                / DiffView / Markdown / SearchableList / GridSheet）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageData2() {
    // ── Color ──
    std::vector<std::pair<std::string, Color>> palette = {
        {"primary", primaryColor()},
        {"red", Color::fromRGB(0.9, 0.2, 0.2)},
        {"green", Color::fromRGB(0.2, 0.7, 0.3)},
        {"amber", Color::fromRGB(0.95, 0.6, 0.1)},
        {"purple", Color::fromRGB(0.5, 0.3, 0.8)},
        {"grey", Color::fromRGB(0.45, 0.47, 0.52)}};
    std::vector<SharedPtr<Widget>> swatches;
    for (const auto& kv : palette) {
        auto box = cui::make_shared<Container>(cui::make_shared<SizedBox>(56.0, 36.0));
        box->decoration(BoxDecoration{kv.second, 6.0});
        auto name = cui::make_shared<Text>(kv.first, TextStyle().size(11.0).fgColor(captionColor()));
        auto col = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{box, cui::make_shared<Center>(name)});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto padded = cui::make_shared<Container>(col);
        padded->padding(EdgeInsets::symmetric(4.0, 0.0));
        swatches.push_back(padded);
    }
    auto paletteRow = cui::make_shared<Row>(std::move(swatches));
    paletteRow->setMainAxisSize(MainAxisSize::Min);

    // ── Bubble ──
    auto bubbleCol = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Bubble>("Hi there!", false),
        cui::make_shared<Bubble>("Hello! How can I help?", false),
        cui::make_shared<Bubble>("I'd like to know more.", true),
        cui::make_shared<Bubble>("Sure — check the docs.", false)});
    bubbleCol->setMainAxisSize(MainAxisSize::Min);
    bubbleCol->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto bubbleBox = cui::make_shared<Container>(bubbleCol);
    bubbleBox->width(320.0).padding(EdgeInsets::all(10.0))
        .decoration(BoxDecoration{theme().surface, 8.0, 1.0,
                                  theme().border});

    // ── BarChart ──
    auto chart = cui::make_shared<BarChart>(
        std::vector<std::string>{"Mon", "Tue", "Wed", "Thu", "Fri"},
        std::vector<double>{30.0, 55.0, 40.0, 70.0, 50.0});

    // ── DescriptionList ──
    auto descList = cui::make_shared<DescriptionList>(
        std::vector<DescriptionList::Item>{
            {"Project", "CUI"}, {"Version", "2.3.0"}, {"License", "MIT"},
            {"Language", "C++20"}, {"Backend", "Skia"}});

    // ── DiffView ──
    auto diff = cui::make_shared<DiffView>(std::vector<DiffLine>{
        {' ', "int main() {"},
        {'-', "    return 0;"},
        {'+', "    return 1;"},
        {' ', "}"}});

    // ── Markdown ──
    auto md = cui::make_shared<Markdown>(
        "# CUI Gallery\n\n"
        "This is **bold** text and `inline code`.\n\n"
        "## Features\n"
        "- Widget / Element / RenderObject\n"
        "- 三层控件流水线\n"
        "- Skia rendering backend\n\n"
        "```\nauto w = cui_core::make_shared<Text>(\"hi\");\n```\n");
    auto mdBox = cui::make_shared<Container>(md);
    mdBox->width(420.0).padding(EdgeInsets::all(10.0))
        .decoration(BoxDecoration{theme().surface, 8.0, 1.0,
                                  theme().border});

    // ── SearchableList ──
    auto searchable = cui::make_shared<SearchableList>(
        std::vector<std::string>{"Alice", "Bob", "Carol", "David", "Eve", "Frank"}, "");

    // ── GridSheet ──
    auto sheet = cui::make_shared<GridSheet>(
        std::vector<std::string>{"A", "B", "C"},
        std::vector<std::vector<std::string>>{
            {"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"10", "11", "12"}});

    return pageBody({
        sectionTitle("ListView / GridView"),
        caption("懒加载的 ListView.builder 与 GridView.builder（ScrollController 由 State 持有）。"),
        vgap(8.0),
        cui::make_shared<DataListsPage>(),
        vgap(16.0),
        sectionTitle("Color"),
        caption("主题色板。"),
        vgap(8.0), paletteRow,
        vgap(16.0),
        sectionTitle("Bubble"),
        caption("聊天气泡（左侧蓝 / 右侧灰）。"),
        vgap(8.0), bubbleBox,
        vgap(16.0),
        sectionTitle("BarChart"),
        caption("柱状图：按最大值归一化高度。"),
        vgap(8.0), chart,
        vgap(16.0),
        sectionTitle("DescriptionList"),
        caption("term / description 两列描述列表。"),
        vgap(8.0), descList,
        vgap(16.0),
        sectionTitle("DiffView"),
        caption("差异视图：+ 新增（绿） / - 删除（红）。"),
        vgap(8.0), diff,
        vgap(16.0),
        sectionTitle("Markdown"),
        caption("极简 Markdown（# / ## / - 列表 / ``` 代码块）。"),
        vgap(8.0), mdBox,
        vgap(16.0),
        sectionTitle("SearchableList"),
        caption("点搜索框直接用键盘输入，大小写不敏感的子串过滤；非空时可一键清除。"),
        vgap(8.0), searchable,
        vgap(16.0),
        sectionTitle("GridSheet"),
        caption("电子表格网格，点击选中单元格。"),
        vgap(8.0), sheet,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：输入+（ContextMenu / MenuBar / OtpInput / TokenInput / Form）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageInput2() {
    // ── ContextMenu ──
    auto ctxChild = cui::make_shared<Container>(cui::make_shared<Center>(
        cui::make_shared<Text>("在此区域右键打开菜单", TextStyle().size(12.5))));
    ctxChild->width(280.0).height(44.0)
        .decoration(BoxDecoration{theme().surfaceVariant, 6.0, 1.0,
                                  theme().border});
    auto ctxMenu = cui::make_shared<ContextMenu>(
        ctxChild, std::vector<std::string>{"复制", "粘贴", "重命名", "删除"});

    // ── MenuBar ──
    auto menuBar = cui::make_shared<MenuBar>(std::vector<MenuEntry>{
        {"文件", {"新建", "打开", "保存"}},
        {"编辑", {"撤销", "重做", "剪切"}},
        {"视图", {"放大", "缩小", "重置"}}});

    // ── OtpInput ──
    auto otp = cui::make_shared<OtpInput>(6);

    // ── TokenInput ──
    auto tokenInput = cui::make_shared<TokenInput>(
        std::vector<std::string>{"C++", "Skia"},
        std::vector<std::string>{"Cui", "Widget", "Render", "Skia"});

    // ── Form ──
    auto styledInput = [](const std::string& text) {
        auto t = cui::make_shared<Text>(text, TextStyle().size(12.5).fgColor(
                                                  theme().textPrimary));
        auto c = cui::make_shared<Container>(t);
        c->width(220.0).padding(EdgeInsets::symmetric(6.0, 8.0))
            .decoration(BoxDecoration{theme().surface, 4.0, 1.0,
                                      theme().border});
        return c;
    };
    auto form = cui::make_shared<Form>(std::vector<FormFieldSpec>{
        {"用户名", styledInput("alice")},
        {"邮箱", styledInput("alice@example.com")},
        {"语言", styledInput("C++20")}});

    return pageBody({
        sectionTitle("ContextMenu"),
        caption("右键在按下点弹出，菜单项挂在上层 Overlay（点菜单外任意处收起）。"),
        vgap(8.0), ctxMenu,
        vgap(24.0),
        sectionTitle("MenuBar"),
        caption("点击顶层菜单展开子项，再点收起。"),
        vgap(8.0), menuBar,
        vgap(24.0),
        sectionTitle("OtpInput"),
        caption("点格子选位后直接敲数字：写入即跳到下一格，Backspace 退格清除，左右键移动焦点。"),
        vgap(8.0), otp,
        vgap(24.0),
        sectionTitle("TokenInput"),
        caption("点击 +候选 添加标签，点击标签删除。"),
        vgap(8.0), tokenInput,
        vgap(24.0),
        sectionTitle("Form"),
        caption("label + field 两列表单布局。"),
        vgap(8.0), form,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：ComboBox（story 的 Basics/ComboBox：可搜索下拉选择框）
// ══════════════════════════════════════════════════════════════════════════════

class ComboBoxPage : public StatefulWidget {
public:
    ComboBoxPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ComboBoxPage"; }
};

class ComboBoxPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto status = cui::make_shared<Text>(
            langSel_ >= 0 ? "onSelected → " + kLangs[static_cast<size_t>(langSel_)]
                          : std::string("onSelected → （还没选）"),
            TextStyle().size(12.0).fgColor(captionColor()));
        auto fruitStatus = cui::make_shared<Text>(
            fruitSel_ >= 0 && fruitSel_ < static_cast<int>(kFruits.size())
                ? "onSelected → " + kFruits[static_cast<size_t>(fruitSel_)]
                : std::string("onSelected → （已清除）"),
            TextStyle().size(12.0).fgColor(captionColor()));

        return pageBody({
            sectionTitle("ComboBox"),
            caption("点输入框聚焦即展开选项，键入即过滤（大小写不敏感子串）。"),
            vgap(8.0),
            cui::make_shared<ComboBox>(
                kLangs, langSel_, "Type to filter",
                [this](int index) {
                    langSel_ = index;
                    setState();
                }),
            vgap(8.0),
            status,
            vgap(20.0),
            caption("预选第 3 项（selectedIndex = 2）："),
            vgap(8.0),
            cui::make_shared<ComboBox>(
                kFruits, fruitSel_, "Select...",
                [this](int index) {
                    fruitSel_ = index;
                    setState();
                }),
            vgap(8.0),
            fruitStatus,
            vgap(20.0),
            sectionTitle("键盘"),
            caption("↑↓ 导航 · PgUp/PgDn 翻页 · Ctrl+↑/↓ 首末项 · Enter 选中 · "
                    "Esc 收起 · 点右侧箭头开合浮层。"),
            caption("收起状态下这些键一律放行给祖先：页面照样能用方向键滚动。"),
        });
    }

private:
    static inline const std::vector<std::string> kLangs = {
        "Rust", "Python", "JavaScript", "TypeScript", "Go", "C++", "Java", "Kotlin"};
    static inline const std::vector<std::string> kFruits = {
        "Apple", "Banana", "Cherry", "Durian", "Elderberry"};

    int langSel_ = -1;
    int fruitSel_ = 2;
};

inline std::unique_ptr<State> ComboBoxPage::createState() {
    return std::make_unique<ComboBoxPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：布局2（Splitter / Zoomable / Modifier → 上游 映射说明）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageLayout2() {
    auto pane = [](const std::string& s) {
        return cui::make_shared<Center>(cui::make_shared<Text>(s, TextStyle().size(13.0)));
    };
    auto hSplit = cui::make_shared<Splitter>(pane("左栏（拖动中间手柄）"), pane("右栏"),
                                             Axis::Horizontal, 0.5, 8.0);
    auto vSplit = cui::make_shared<Splitter>(pane("上栏"), pane("下栏"),
                                             Axis::Vertical, 0.45, 8.0);

    auto zoomChild = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("Zoomable content",
                               TextStyle().size(15.0).bold().fgColor(primaryColor())),
        cui::make_shared<SizedBox>(0.0, 6.0),
        cui::make_shared<Text>("拖拽平移，底部按钮缩放 / 重置", TextStyle().size(12.0))});
    zoomChild->setMainAxisSize(MainAxisSize::Min);
    zoomChild->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto zoomable = cui::make_shared<Zoomable>(zoomChild, 1.0, 320.0, 180.0);

    // 链式修饰 → 控件等价写法映射
    auto modifierMap = cui::make_shared<DescriptionList>(
        std::vector<DescriptionList::Item>{
            {"| size(w,h)", "SizedBox(w, h) / Container.width().height()"},
            {"| background(c)", "Container.decoration(BoxDecoration{c, radius})"},
            {"| padding(e)", "Padding(e, child) / Container.padding(e)"},
            {"| margin(e)", "Container.margin(e)"},
            {"| clickable(fn)", "GestureDetector(child, onTap)"},
            {"| fontSize(n)", "TextStyle().size(n)"},
            {"| textColor(c)", "TextStyle().fgColor(c)"},
            {"| themed(role)", "Theme 系统（待接入控件层）"},
            {"| fillMaxSize()", "SizedBox.expand / Container 撑满约束"}},
        150.0);

    return pageBody({
        sectionTitle("Splitter（横向）"),
        caption("拖动中间手柄改变左右比例（LayoutBuilder 取可用宽度）。"),
        vgap(8.0), hSplit,
        vgap(24.0),
        sectionTitle("Splitter（纵向）"),
        caption("纵向分栏，拖动改变上下比例。"),
        vgap(8.0), vSplit,
        vgap(24.0),
        sectionTitle("Zoomable"),
        caption("InteractiveViewer 简化版：拖拽平移，按钮缩放/重置。"),
        vgap(8.0), zoomable,
        vgap(24.0),
        sectionTitle("链式修饰 → 控件映射"),
        caption("链式修饰写法在控件层的等价组合方式。"),
        vgap(8.0), modifierMap,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：导航2（Accordion / Carousel / CommandPalette / HoverCard）
// ══════════════════════════════════════════════════════════════════════════════

// CommandPalette 的 onRun 只在回调里给一个下标，页面上看不见「执行了什么」，
// 所以用一个小状态控件把结果镜像成文字 —— 键盘流程（↑↓ / 回车 / Esc）才有视觉证据。
class Nav2Palette : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Nav2Palette"; }
};

class Nav2PaletteState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto palette = cui::make_shared<CommandPalette>(
            kCommands, [this](int index) {
                lastRun_ = index;
                setState();
            });
        auto status = cui::make_shared<Text>(
            lastRun_ >= 0 && lastRun_ < static_cast<int>(kCommands.size())
                ? "onRun → " + kCommands[static_cast<size_t>(lastRun_)].title
                : std::string("onRun → （还没执行）"),
            TextStyle().size(12.0).fgColor(captionColor()));
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            palette, vgap(8.0), status});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    static inline const std::vector<CommandItem> kCommands = {
        {"Open File", "快速跳转到文件", "Ctrl+P"},
        {"Go to Line", "跳转到指定行", "Ctrl+G"},
        {"Toggle Terminal", "显示/隐藏终端", "Ctrl+`"},
        {"Format Document", "格式化当前文档", "Shift+Alt+F"},
        {"Run Build Task", "执行构建任务", "Ctrl+Shift+B"}};

    int lastRun_ = -1;
};

inline std::unique_ptr<State> Nav2Palette::createState() {
    return std::make_unique<Nav2PaletteState>();
}

inline SharedPtr<Widget> pageNav2() {
    // ── Accordion ──
    auto accordion = cui::make_shared<Accordion>(std::vector<AccordionItem>{
        {"基本信息", cui::make_shared<Text>("姓名：Alice　　部门：Engineering",
                                            TextStyle().size(12.5))},
        {"联系方式", cui::make_shared<Text>("邮箱：alice@example.com　　分机：8021",
                                            TextStyle().size(12.5))},
        {"权限设置", cui::make_shared<Text>("角色：Maintainer　　可发布：是",
                                            TextStyle().size(12.5))}}, 0);

    // ── Carousel ──
    std::vector<SharedPtr<Widget>> slides;
    const char* captions[] = {"Spring", "Summer", "Autumn", "Winter"};
    Color seasonColors[] = {Color::fromRGB(0.55, 0.8, 0.35), Color::fromRGB(0.95, 0.75, 0.2),
                            Color::fromRGB(0.85, 0.45, 0.15), Color::fromRGB(0.6, 0.75, 0.9)};
    for (int i = 0; i < 4; ++i) {
        auto t = cui::make_shared<Text>(captions[i], TextStyle().size(13.0).bold().fgColor(
                                                          Color::fromRGB(1, 1, 1)));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->decoration(BoxDecoration{seasonColors[i], 8.0});
        slides.push_back(c);
    }
    auto carousel = cui::make_shared<Carousel>(std::move(slides), 120.0, 84.0);

    // ── CommandPalette ──
    auto palette = cui::make_shared<Nav2Palette>();

    // ── HoverCard ──
    auto trigger = cui::make_shared<Container>(cui::make_shared<Center>(
        cui::make_shared<Text>("悬停查看（HoverCard）")));
    trigger->width(200.0).height(28.0)
        .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.10), 6.0, 1.0,
                                  Color::fromRGBA(0.0, 0.45, 0.95, 0.4)});
    auto cardBody = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("详情卡片", TextStyle().size(13.0).bold()),
        cui::make_shared<SizedBox>(0.0, 6.0),
        cui::make_shared<Text>("鼠标悬停时显示，移开 1.5 秒后自动关闭。",
                               TextStyle().size(12.0).fgColor(theme().textSecondary))});
    cardBody->setMainAxisSize(MainAxisSize::Min);
    cardBody->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto hoverCard = cui::make_shared<HoverCard>(trigger, cardBody, 32.0, /*closeDelaySec=*/1.5);

    return pageBody({
        sectionTitle("Accordion"),
        caption("手风琴：同一时刻只允许一项展开（点击标题切换）。"),
        vgap(8.0), accordion,
        vgap(24.0),
        sectionTitle("Carousel"),
        caption("轮播：严格 1:1，一次只显示当前页，箭头与指示点切换。"),
        vgap(8.0), carousel,
        vgap(24.0),
        sectionTitle("CommandPalette"),
        caption("命令面板：点查询框直接键入过滤，↑↓ 移动高亮、回车执行（结果见下方 onRun）、"
                "Esc 清空查询（首字母条仍可点）。"),
        vgap(8.0), palette,
        vgap(24.0),
        sectionTitle("HoverCard"),
        caption("鼠标悬停显示卡片，移开后延时 1.5 秒自动关闭（移回可取消，Listener onHover/onExit）。"),
        vgap(8.0), hoverCard,
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：反馈2（TimePicker / DatePicker / Gif / Spinner）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageFeedback2() {
    auto timePicker = cui::make_shared<TimePicker>(9, 30);
    auto datePicker = cui::make_shared<DatePicker>();

    // Gif：用「加载点」帧序列演示逐帧播放（Ticker 真实时间驱动）
    std::vector<SharedPtr<Widget>> frames;
    for (int step = 0; step < 6; ++step) {
        std::vector<SharedPtr<Widget>> dots;
        for (int i = 0; i < 5; ++i) {
            bool on = (i == step % 5);
            auto d = cui::make_shared<Container>(cui::make_shared<SizedBox>(on ? 14.0 : 10.0,
                                                                           on ? 14.0 : 10.0));
            d->decoration(BoxDecoration{on ? primaryColor() : theme().textDisabled,
                                        5.0});
            dots.push_back(d);
            dots.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
        }
        auto row = cui::make_shared<Row>(std::move(dots));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        frames.push_back(row);
    }
    auto gif = cui::make_shared<Gif>(std::move(frames), 6.0, 150.0, 60.0);

    auto spinnerBare = cui::make_shared<Spinner>();
    auto spinnerLabeled = cui::make_shared<Spinner>("Loading…");

    return pageBody({
        sectionTitle("TimePicker"),
        caption("时间选择：▲/▼ 调整小时与分钟（无键盘输入）。"),
        vgap(8.0), timePicker,
        vgap(24.0),
        sectionTitle("DatePicker"),
        caption("日期选择：点击月历中的日期即选中（主色实心高亮，描边圈标出今天）。"),
        vgap(8.0), datePicker,
        vgap(24.0),
        sectionTitle("Gif（逐帧动画）"),
        caption("帧序列播放器：foundation Ticker 真实时间驱动，按 fps 推进；可暂停/继续。"),
        vgap(8.0), gif,
        vgap(24.0),
        sectionTitle("Spinner"),
        caption("加载指示器（基于 CircularProgressIndicator 的不确定进度）。"),
        vgap(8.0), spinnerBare,
        vgap(10.0), spinnerLabeled,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：高级（CodeEditor / RichTextEditor / Settings / Animated / Popup）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageAdvanced() {
    // 可编辑演示：壳每次重建都会重新调用本函数，控制器必须跨调用保留，
    // 否则编辑内容 / 撤销栈 / 折叠状态会被重置。
    struct CodeDemo {
        SharedPtr<cui::CodeEditorController> code;
        SharedPtr<cui::CodeEditorFindController> find;
        SharedPtr<cui::TextEditingController> query;
        SharedPtr<CodeEditor> editor;

        CodeDemo() {
            code = cui::make_shared<cui::CodeEditorController>(std::string(
                "#include <cui_core/framework/framework.h>\n"
                "#include <cui_core/widgets/container.h>\n"
                "\n"
                "// Build the three-tree pipeline: Widget -> Element -> RenderObject\n"
                "class Panel : public StatefulWidget {\n"
                "public:\n"
                "    explicit Panel(int items = 8, std::string title = \"Panel\")\n"
                "        : StatefulWidget(nullptr), items_(items), title_(std::move(title)) {}\n"
                "\n"
                "    std::unique_ptr<State> createState() override;\n"
                "    std::string toStringShort() const override { return \"Panel\"; }\n"
                "\n"
                "private:\n"
                "    int items_ = 8;\n"
                "    std::string title_;\n"
                "};\n"
                "\n"
                "/* Multi-line block comment:\n"
                "   the State repaints via Ticker at 60 Hz */\n"
                "SharedPtr<Widget> buildRow(int index, double width) {\n"
                "    const double padding = 12.5;\n"
                "    auto color = theme().surfaceVariant;\n"
                "    return makeRow(index, width - padding);\n"
                "}\n"));
            find = cui::make_shared<cui::CodeEditorFindController>(code);
            query = cui::make_shared<cui::TextEditingController>(std::string());
            // 静态生命周期 ⇒ 无需注销；查询变化即刷新编辑器内高亮。
            query->addListener([f = find, q = query]() {
                f->setIsActive(!q->text().empty());
                f->find(q->text(), false);
            });
            editor = cui::make_shared<CodeEditor>(
                code, "framework.cpp", nullptr, 12.5, std::optional<double>(300.0),
                0.0, true, 4, CodeLanguage::Auto, true, /*enableFolding=*/true,
                /*readOnly=*/false, /*showTitleBar=*/true, std::nullopt, find);
        }
    };
    static CodeDemo demo;
    auto codeEditor = demo.editor;

    auto toolBtn = [](const char* label, std::function<void()> cb) {
        return cui::make_shared<TextButton>(cui::make_shared<Text>(label),
                                            std::move(cb));
    };
    auto findField = cui::make_shared<TextField>(
        demo.query, InputDecoration{.hintText = "查找（大小写不敏感）",
                                    .borderColor = theme().border,
                                    .borderRadius = 6.0,
                                    .borderWidth = 1.0});
    auto findBox = cui::make_shared<Container>(findField);
    findBox->width(190.0);

    std::vector<SharedPtr<Widget>> tools{
        toolBtn("撤销", [c = demo.code] { c->undo(); }),
        toolBtn("重做", [c = demo.code] { c->redo(); }),
        toolBtn("全部折叠", [c = demo.code] { c->foldAll(); }),
        toolBtn("全部展开", [c = demo.code] { c->unfoldAll(); }),
        cui::make_shared<SizedBox>(10.0, 0.0),
        findBox,
        toolBtn("上一个", [f = demo.find] { f->previous(); }),
        toolBtn("下一个", [f = demo.find] { f->next(); }),
    };
    auto codeToolbar = cui::make_shared<Row>(std::move(tools));
    codeToolbar->setMainAxisSize(MainAxisSize::Min);
    codeToolbar->setCrossAxisAlignment(CrossAxisAlignment::Center);

    // Quill 风格示例文档：一次摆全块格式（标题 / 引用 / 代码块 / 有序·无序列表 /
    // 缩进 / 对齐）与行内格式（粗斜下删 / 行内代码 / 链接 / 前景 / 底色 / 字体 /
    // 字号 / 上下标）。文档不变量「以一个空行收尾」和 Quill 同口径。
    auto quillDoc = [] {
        auto run = [](std::string t, RichInline f = {}) {
            return RichRun{std::move(t), std::move(f)};
        };
        auto runs = [](std::vector<RichRun> v) {
            RichLine l;
            l.runs = std::move(v);
            return l;
        };
        RichInline bold, ital, undl, stri, code, link, fg, bg, serif, big, sup;
        bold.bold = ital.italic = undl.underline = stri.strike = code.code = true;
        link.link = "https://quilljs.com";
        fg.color = colorFromHex("#e5484d");
        bg.background = colorFromHex("#ffe08a");
        serif.font = "serif";
        big.size = RichSize::Large;
        sup.script = RichScript::Super;

        RichDocument doc;
        doc.lines.clear();

        RichLine h1 = runs({run("Quill 风格富文本")});
        h1.block = RichBlock::Header1;
        doc.lines.push_back(std::move(h1));

        doc.lines.push_back(runs({run("正文里混排 "), run("加粗", bold), run(" "), run("斜体", ital),
                                 run(" "), run("下划线", undl), run(" "), run("删除线", stri),
                                 run(" "), run("inline code", code), run(" "), run("链接", link),
                                 run(" "), run("前景色", fg), run(" "), run("底色", bg), run(" "),
                                 run("serif", serif), run(" "), run("大字", big), run("· E"),
                                 run("2", sup)}));

        RichLine h2 = runs({run("块与列表")});
        h2.block = RichBlock::Header2;
        doc.lines.push_back(std::move(h2));

        RichLine quote = runs({run("引用块：左侧一条竖 bar，文字转灰。")});
        quote.block = RichBlock::Blockquote;
        doc.lines.push_back(std::move(quote));

        RichLine ul1 = runs({run("无序列表第一项")});
        ul1.list = RichListKind::Bullet;
        doc.lines.push_back(std::move(ul1));
        RichLine ul2 = runs({run("Tab / ⇥ 缩进到第二级")});
        ul2.list = RichListKind::Bullet;
        ul2.indent = 1;
        doc.lines.push_back(std::move(ul2));

        RichLine ol1 = runs({run("有序列表第一项")});
        ol1.list = RichListKind::Ordered;
        doc.lines.push_back(std::move(ol1));
        RichLine ol2 = runs({run("回车继续编号，空行回车退出列表")});
        ol2.list = RichListKind::Ordered;
        doc.lines.push_back(std::move(ol2));

        RichLine centered = runs({run("这一行居中对齐")});
        centered.align = RichAlign::Center;
        doc.lines.push_back(std::move(centered));

        for (const char* src : {"RichDocument doc = RichDocument::fromPlainText(\"hi\");",
                                "doc.lines[0].block = RichBlock::Header1;"}) {
            RichLine cb;
            cb.block = RichBlock::CodeBlock;
            cb.runs = {run(src)};
            doc.lines.push_back(std::move(cb));
        }
        doc.lines.push_back(RichLine{});
        return doc;
    }();

    auto richEditor = cui::make_shared<RichTextEditor>(
        quillDoc, "点正文就能改：选中文字再点工具栏按钮试试…", 640.0, 380.0, false,
        RichEditorTheme::fromApp());

    auto settings = cui::make_shared<Settings>(std::vector<SettingItem>{
        {"自动保存", "文件变更时自动写入磁盘", 0, ""},
        {"显示行号", "编辑器左侧显示行号", 1, ""},
        {"主题", "当前使用浅色主题", 2, "Light"},
        {"拼写检查", "编辑时校验拼写", 0, ""}});

    auto animated = cui::make_shared<Animated>(80.0, 220.0, 60.0, 400.0);

    auto popupTrigger = cui::make_shared<Container>(cui::make_shared<Center>(
        cui::make_shared<Text>("点击打开浮层 ▼", TextStyle().size(12.5))));
    popupTrigger->width(160.0).height(30.0)
        .decoration(BoxDecoration{theme().surfaceVariant, 6.0, 1.0,
                                  theme().border});
    auto popupBody = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("浮层内容区。", TextStyle().size(12.5)),
        cui::make_shared<SizedBox>(0.0, 6.0),
        cui::make_shared<Text>("再点触发器或点标题栏 × 关闭。",
                               TextStyle().size(11.5).fgColor(theme().textSecondary))});
    popupBody->setMainAxisSize(MainAxisSize::Min);
    popupBody->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto popup = cui::make_shared<Popup>(popupTrigger, popupBody, "浮层标题");

    return pageBody({
        sectionTitle("CodeEditor"),
        caption("代码编辑器（对齐 code_forge）：点正文即可编辑——方向键 / Shift 选区 / Home·End / Ctrl+A·C·X·V / Ctrl+Z·Y（撤销重做，≤500ms 快速输入合并）/ Tab·Shift+Tab 缩进 / Ctrl+D 复制行 / Ctrl+Shift+↑↓ 移动行；回车自动缩进、括号自动配对。行号槽可整行选中，左侧 ▸ 折叠（全部折叠/展开见工具栏）；查找框输入即高亮并可用上一个/下一个循环跳转。等宽字体 + 制表符展开 + 逐字符词法着色 + 当前行高亮 + 缩进参考线 + 双向滚动（光标自动带回可视区）；语言按扩展名推断，CodeEditorTheme 可覆盖全部配色。LSP/minimap 未做。"),
        vgap(8.0), codeToolbar,
        vgap(6.0), codeEditor,
        vgap(24.0),
        sectionTitle("RichTextEditor"),
        caption("对齐 Quill：文档是 Delta（块 = 一行，run = 一段行内格式，`deltaJson()` 可直接导出），工具栏是 snow 皮肤。块格式 H1/H2/H3、引用、代码块、有序·无序列表、缩进、四种对齐；行内格式粗/斜/下划线/删除线/行内代码/链接/前景/底色/字体/字号/上下标 + clean。可直接编辑：点选定位光标、拖选出选区、回车换行（列表续号、标题降级）、Backspace 在行首先降块格式再并线、Ctrl+B/I/U/Z（Shift 重做）/A、Alt+Enter 进引用、Ctrl+Enter 进出代码块、Esc 收浮层，长行走横向滚动并把光标带回可视区，撤销栈上限 200。上标只缩字号（本仓无基线偏移），不做软换行。"),
        vgap(8.0), richEditor,
        vgap(24.0),
        sectionTitle("Settings"),
        caption("设置面板：右侧为 Switch / Checkbox / 文本值。"),
        vgap(8.0), settings,
        vgap(24.0),
        sectionTitle("Animated"),
        caption("AnimatedContainer 简化版：Ticker 真实时间驱动尺寸插值（400ms）。"),
        vgap(8.0), animated,
        vgap(24.0),
        sectionTitle("Popup"),
        caption("点击触发器展开浮层（Stack + Positioned 叠加）。"),
        vgap(8.0), popup,
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：Chips & List（Chip / ListTile）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageChipsList() {
    std::vector<SharedPtr<Widget>> chips;
    const char* tags[] = {"C++", "Skia", "Cui", "Widget", "RenderObject"};
    for (const char* t : tags)
        chips.push_back(cui::make_shared<Chip>(cui::make_shared<Text>(t, TextStyle().size(12.0))));
    auto chipRow = cui::make_shared<Row>(std::move(chips));
    chipRow->setMainAxisSize(MainAxisSize::Min);
    chipRow->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto selectedChip = cui::make_shared<Chip>(
        cui::make_shared<Text>("selected", TextStyle().size(12.0)),
        theme().surfaceVariant, 16.0, EdgeInsets::symmetric(4.0, 8.0), nullptr,
        IconCode::None, std::function<void()>{}, true);

    auto tiles = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<ListTile>(cui::make_shared<Text>("项目一"),
                                   cui::make_shared<Text>("副标题说明")),
        cui::make_shared<ListTile>(cui::make_shared<Text>("项目二"),
                                   cui::make_shared<Text>("副标题说明")),
        cui::make_shared<ListTile>(cui::make_shared<Text>("项目三"),
                                   cui::make_shared<Text>("副标题说明"))});
    tiles->setMainAxisSize(MainAxisSize::Min);
    tiles->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto tileBox = cui::make_shared<Container>(tiles);
    tileBox->width(320.0).decoration(BoxDecoration{theme().surface, 8.0, 1.0,
                                                   theme().border});

    return pageBody({
        sectionTitle("Chips & Tags"),
        caption("Chip（material/chip.dart）：标签 chip 与 selected 态。"),
        vgap(8.0), chipRow,
        vgap(10.0), lead(selectedChip),
        vgap(16.0),
        sectionTitle("List（ListTile）"),
        caption("ListTile（material/list_tile.dart）：title + subtitle。"),
        vgap(8.0), tileBox,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：窗口栏（TitleBar / Toolbar / StatusBar）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageChrome() {
    auto titleBar = cui::make_shared<TitleBar>(
        "story — 控件流水线", std::vector<std::string>{"—", "□", "×"}, 420.0);
    auto toolbar = cui::make_shared<Toolbar>(std::vector<ToolbarItem>{
        {IconCode::Add, "新建"},
        {IconCode::Search, "打开"},
        {IconCode::Check, "保存"},
        {IconCode::None, "", true},  // 分隔线
        {IconCode::ArrowBack, "撤销"},
        {IconCode::ArrowForward, "重做"}});
    auto statusBar = cui::make_shared<StatusBar>(
        "就绪", std::vector<std::string>{"行 1, 列 1", "UTF-8", "100%"},
        Color::fromRGB(0.22, 0.75, 0.42), 420.0);

    return pageBody({
        sectionTitle("TitleBar"),
        caption("标题栏：标题 + 右侧窗口按钮。"),
        vgap(8.0), titleBar,
        vgap(20.0),
        sectionTitle("Toolbar"),
        caption("工具栏：一行图标/文字按钮。"),
        vgap(8.0), toolbar,
        vgap(20.0),
        sectionTitle("StatusBar"),
        caption("状态栏：底部状态分段。"),
        vgap(8.0), statusBar,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：浮层+（showModalBottomSheet / GroupBox）
// ══════════════════════════════════════════════════════════════════════════════

/// BottomSheet 走 `showModalBottomSheet` 弹出 ⇒ 需要 Navigator 祖先的 context，
/// 所以本页做成 StatefulWidget（和「对话框」页同一个套路）。
class OverlaysPage : public StatefulWidget {
public:
    OverlaysPage() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "OverlaysPage"; }
};

class OverlaysPageState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        BuildContext* ctx = &context;

        auto sheetBody = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<Text>("分享到", TextStyle().size(14.0).bold()),
            cui::make_shared<SizedBox>(0.0, 8.0),
            cui::make_shared<Text>("· 邮件　· 复制链接　· 更多",
                                   TextStyle().size(12.5).fgColor(theme().textSecondary))});
        sheetBody->setMainAxisSize(MainAxisSize::Min);
        sheetBody->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto sheet = cui::make_shared<BottomSheet>(sheetBody, 320.0, 140.0);

        auto sheetBtn = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("showModalBottomSheet"), [ctx, sheet] {
                showModalBottomSheet(*ctx, sheet);
            });
        // 第二枚：barrier 点不掉的模态（上游的 isDismissible: false）。
        auto stickyBtn = cui::make_shared<OutlinedButton>(
            cui::make_shared<Text>("不可点遮罩关闭"), [ctx, sheet] {
                showModalBottomSheet(*ctx, sheet, /*barrierDismissible=*/false);
            });

        auto groupInner = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<Text>("选项 A", TextStyle().size(12.5)),
            cui::make_shared<Text>("选项 B", TextStyle().size(12.5))});
        groupInner->setMainAxisSize(MainAxisSize::Min);
        groupInner->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto groupBox = cui::make_shared<GroupBox>("分组标题", groupInner, 300.0);

        return pageBody({
            sectionTitle("showModalBottomSheet"),
            caption("面板由按钮推入模态路由：barrier + 底部对齐 + 滑入（进 250ms / 出 200ms）。"
                    "点面板以外的遮罩即 pop；右侧那枚是不可点遮罩关闭的版本，"
                    "只能靠面板内的按钮自己 pop。"),
            vgap(8.0),
            cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                sheetBtn, cui::make_shared<SizedBox>(12.0, 0.0), stickyBtn}),
            vgap(24.0),
            sectionTitle("GroupBox"),
            caption("带标题的分组框（fieldset 风格）。"),
            vgap(8.0), groupBox,
            vgap(40.0),
        });
    }
};

inline std::unique_ptr<State> OverlaysPage::createState() {
    return std::make_unique<OverlaysPageState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：FAB（悬浮动作按钮，对齐 rui pages_basics::page_fab）
// ══════════════════════════════════════════════════════════════════════════════

/// 一块演示区：标题 + 说明 + 220 高底板，FAB 按 position 定位在底板内。
inline SharedPtr<Widget> fabDemoBlock(const std::string& title, const std::string& subtitle,
                                      SharedPtr<Widget> fab, FabPosition position) {
    auto hintText = cui::make_shared<Text>("Click FAB to expand",
                                           TextStyle().size(12.0).fgColor(captionColor()));
    auto card = cui::make_shared<Container>(hintText);
    card->padding(EdgeInsets::all(12.0));
    card->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.06), 8.0});
    auto fill = cui::make_shared<Positioned>(card);
    fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;

    auto posFab = [&] {
        SharedPtr<Widget> toPlace = fab;
        double leftV = 0.0, rightV = 0.0;
        bool hasLeft = false, hasRight = false;
        if (position == FabPosition::Left) {
            hasLeft = true;
            leftV = 16.0;
        } else if (position == FabPosition::Center) {
            hasLeft = hasRight = true;
            auto inner = cui::make_shared<Container>(fab);
            inner->alignment(0.0, 0.0);
            toPlace = inner;
        } else {
            hasRight = true;
            rightV = 16.0;
        }
        auto p = cui::make_shared<Positioned>(toPlace);
        p->hasBottom = true;
        p->bottom = 16.0;
        p->hasLeft = hasLeft;
        p->left = leftV;
        p->hasRight = hasRight;
        p->right = rightV;
        return p;
    }();

    std::vector<SharedPtr<Widget>> layers;
    layers.push_back(fill);
    layers.push_back(posFab);
    auto stack = cui::make_shared<Stack>(std::move(layers));
    auto holder = cui::make_shared<SizedBox>(1e18, 220.0, stack);

    return vcol({sectionTitle(title), caption(subtitle), vgap(6.0), holder});
}

inline SharedPtr<Widget> pageFab() {
    // 通用菜单项：Edit / Search / Save / Share。
    auto menuActions = [] {
        return std::vector<FabAction>{
            {IconCode::Edit, {}},   {IconCode::Search, {}},
            {IconCode::Save, {}},   {IconCode::Share, {}}};
    };

    // ── 本体变体（标准 / 扩展 / 禁用）──
    auto fabMini = cui::make_shared<ExpandableFab>(
        IconCode::Edit, "", FabSize::Small, std::vector<FabAction>{}, std::nullopt, std::nullopt,
        ExpandType::Fan, ChildrenAnimation::Rotate, FabPosition::Right, 100.0, 90.0, 250.0, 4.0,
        false, [] {});
    auto fabRegular = cui::make_shared<ExpandableFab>(
        IconCode::Add, "", FabSize::Regular, std::vector<FabAction>{}, std::nullopt, std::nullopt,
        ExpandType::Fan, ChildrenAnimation::Rotate, FabPosition::Right, 100.0, 90.0, 250.0, 4.0,
        false, [] {});
    auto fabLarge = cui::make_shared<ExpandableFab>(
        IconCode::Favorite, "", FabSize::Large, std::vector<FabAction>{},
        Color{0.90, 0.25, 0.35, 1.0}, std::nullopt, ExpandType::Fan, ChildrenAnimation::Rotate,
        FabPosition::Right, 100.0, 90.0, 250.0, 4.0, false, [] {});
    auto fabExtended = cui::make_shared<ExpandableFab>(
        IconCode::Add, "Create New", FabSize::Extended, std::vector<FabAction>{}, std::nullopt,
        std::nullopt, ExpandType::Fan, ChildrenAnimation::Rotate, FabPosition::Right, 100.0, 90.0,
        250.0, 4.0, false, [] {});
    auto fabExtended2 = cui::make_shared<ExpandableFab>(
        IconCode::Star, "Favorite", FabSize::Extended, std::vector<FabAction>{},
        Color{0.95, 0.60, 0.15, 1.0}, std::nullopt, ExpandType::Fan, ChildrenAnimation::Rotate,
        FabPosition::Right, 100.0, 90.0, 250.0, 4.0, false, [] {});
    auto fabDisabled = cui::make_shared<ExpandableFab>(
        IconCode::Close, "", FabSize::Regular, std::vector<FabAction>{}, std::nullopt, std::nullopt,
        ExpandType::Fan, ChildrenAnimation::Rotate, FabPosition::Right, 100.0, 90.0, 250.0, 4.0,
        true);

    // ── 展开样式 ──
    auto styleBlock = [&](const std::string& title, const std::string& sub, ExpandType et,
                          ChildrenAnimation ca, double dist, double fan, FabPosition pos) {
        auto fab = cui::make_shared<ExpandableFab>(
            IconCode::Add, "", FabSize::Regular, menuActions(), std::nullopt, std::nullopt, et, ca,
            pos, dist, fan, 250.0, 4.0, false);
        return fabDemoBlock(title, sub, fab, pos);
    };
    auto fanRotateScale = styleBlock("Fan + RotateScale",
                                     "ExpandType::Fan, RotateScale, distance=100, fanAngle=90°",
                                     ExpandType::Fan, ChildrenAnimation::RotateScale, 100.0, 90.0,
                                     FabPosition::Right);
    auto fanNone = styleBlock("Fan + None", "ExpandType::Fan, None, distance=100, fanAngle=90°",
                              ExpandType::Fan, ChildrenAnimation::None, 100.0, 90.0,
                              FabPosition::Right);
    auto upBlock = styleBlock("Up (vertical)", "ExpandType::Up, Rotate, distance=70",
                              ExpandType::Up, ChildrenAnimation::Rotate, 70.0, 90.0,
                              FabPosition::Right);
    auto sideBlock = styleBlock("Side (horizontal)", "ExpandType::Side, Rotate, distance=70",
                                ExpandType::Side, ChildrenAnimation::Rotate, 70.0, 90.0,
                                FabPosition::Right);
    auto fan180 = styleBlock("Fan 180°", "ExpandType::Fan, Rotate, distance=100, fanAngle=180°",
                             ExpandType::Fan, ChildrenAnimation::Rotate, 100.0, 180.0,
                             FabPosition::Right);

    // ── 位置变体 ──
    auto posBlock = [&](const std::string& title, const std::string& sub, FabPosition pos) {
        auto fab = cui::make_shared<ExpandableFab>(
            IconCode::Add, "", FabSize::Regular, menuActions(), std::nullopt, std::nullopt,
            ExpandType::Fan, ChildrenAnimation::Rotate, pos, 100.0, 90.0, 250.0, 4.0, false);
        return fabDemoBlock(title, sub, fab, pos);
    };
    auto posRight = posBlock("Position: Right", "FabPosition::Right, 扇形向左上展开",
                             FabPosition::Right);
    auto posLeft = posBlock("Position: Left", "FabPosition::Left, 扇形向右上展开",
                            FabPosition::Left);
    auto posCenter = posBlock("Position: Center", "FabPosition::Center, 扇形覆盖上半圆",
                              FabPosition::Center);

    return pageBody({
        sectionTitle("Standard variants"),
        hrow({fabMini, cui::make_shared<SizedBox>(12.0, 0.0), fabRegular,
              cui::make_shared<SizedBox>(12.0, 0.0), fabLarge}),
        vgap(20.0),
        sectionTitle("Extended variants"),
        hrow({fabExtended, cui::make_shared<SizedBox>(12.0, 0.0), fabExtended2}),
        vgap(20.0),
        sectionTitle("Disabled state"),
        hrow({fabDisabled}),
        vgap(24.0),
        sectionTitle("Expand styles"),
        fanRotateScale, vgap(12.0), fanNone, vgap(12.0), upBlock, vgap(12.0), sideBlock,
        vgap(12.0), fan180,
        vgap(24.0),
        sectionTitle("Position variants"),
        posRight, vgap(12.0), posLeft, vgap(12.0), posCenter,
        vgap(20.0),
        caption("点击任意 FAB 看涟漪效果；Tab 聚焦看焦点环。"),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：交互+（Tooltip / InkWell / CircularProgressIndicator）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageInteract() {
    auto tipTarget = cui::make_shared<Container>(cui::make_shared<Center>(
        cui::make_shared<Text>("悬停查看 Tooltip", TextStyle().size(12.5))));
    tipTarget->width(180.0).height(30.0)
        .decoration(BoxDecoration{theme().surfaceVariant, 6.0, 1.0,
                                  theme().border});
    auto tip = cui::make_shared<Tooltip>("这是一个 Tooltip 提示", tipTarget);

    auto inkBox = cui::make_shared<Container>(cui::make_shared<Center>(
        cui::make_shared<Text>("InkWell 水波纹", TextStyle().size(12.5))));
    inkBox->width(180.0).height(36.0)
        .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.10), 6.0});
    auto ink = cui::make_shared<InkWell>(inkBox, [] {});

    auto circular = cui::make_shared<CircularProgressIndicator>(std::nullopt,
                                                               theme().divider,
                                                               primaryColor());
    auto circularBox = cui::make_shared<Container>(circular);
    circularBox->padding(EdgeInsets::all(4.0));

    return pageBody({
        sectionTitle("Tooltip"),
        caption("悬停/长按显示提示气泡（material/tooltip.dart）。"),
        vgap(8.0), tip,
        vgap(24.0),
        sectionTitle("InkWell"),
        caption("点击水波纹反馈（material/ink_well.dart）。"),
        vgap(8.0), ink,
        vgap(24.0),
        sectionTitle("CircularProgressIndicator"),
        caption("圆形进度（不确定态）。"),
        vgap(8.0), circularBox,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：外壳（AppBar / Ribbon / MaterialBanner / WindowFrame / DockPanel）
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageShell() {
    // ── AppBar ──
    auto appBar = cui::make_shared<AppBar>(
        "Gallery",
        std::vector<IconCode>{IconCode::Search, IconCode::MoreVert}, IconCode::Menu);

    // ── Ribbon ──
    auto ribbon = cui::make_shared<Ribbon>(
        std::vector<RibbonTabData>{
            {"开始",
             {{"剪贴板",
               {{"粘贴", true, [] {}}, {"剪切", false, [] {}}, {"复制", false, [] {}}}},
              {"字体",
               {{"加粗", false, [] {}}, {"斜体", false, [] {}}, {"下划线", false, [] {}}}}}},
            {"插入",
             {{"页面",
               {{"封面", true, [] {}}, {"空白页", false, [] {}}, {"分页", false, [] {}}}},
              {"插图",
               {{"图片", true, [] {}}, {"表格", false, [] {}}, {"图表", false, [] {}}}}}},
        },
        [](int) {}, 560.0);

    // ── MaterialBanner ──
    auto banner = cui::make_shared<MaterialBanner>(
        "有 3 项更改尚未保存，关闭前请先同步。", "!",
        std::vector<MaterialBanner::BannerAction>{
            {"立即同步", [] {}},
            {"稍后", [] {}},
        },
        [] {});
    auto dangerBanner = cui::make_shared<MaterialBanner>(
        "许可证将在 3 天后过期。", "!",
        std::vector<MaterialBanner::BannerAction>{{"续订", [] {}}}, [] {},
        Color::fromRGB(1.0, 0.90, 0.90), Color::fromRGB(0.55, 0.10, 0.10));

    // ── WindowFrame ──
    auto frameBody = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("窗口内容区", TextStyle().size(13.0).bold()),
        cui::make_shared<SizedBox>(0.0, 6.0),
        cui::make_shared<Text>("标题栏 + 内容 + 状态栏 组合成的一个「窗口」。",
                               TextStyle().size(12.0).fgColor(captionColor()))});
    frameBody->setMainAxisSize(MainAxisSize::Min);
    frameBody->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto frame = cui::make_shared<WindowFrame>(
        "子窗口", frameBody, std::vector<std::string>{"就绪", "100%", "UTF-8"}, 470.0, 200.0);

    // ── DockPanel ──
    auto dockBody = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cui::make_shared<Text>("· 解决方案资源管理器", TextStyle().size(12.5)),
        cui::make_shared<Text>("· 属性", TextStyle().size(12.5)),
        cui::make_shared<Text>("· 输出", TextStyle().size(12.5))});
    dockBody->setMainAxisSize(MainAxisSize::Min);
    dockBody->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto dock = cui::make_shared<DockPanel>("资源管理器", dockBody, 330.0);

    return pageBody({
        sectionTitle("AppBar"),
        caption("顶部应用栏：leading + 标题 + actions（material/app_bar.dart）。"),
        vgap(8.0), appBar,
        vgap(24.0),
        sectionTitle("Ribbon"),
        caption("Office 风格命令区：选项卡 + 分组大/小按钮；点当前标签或 ▲ 折叠。"),
        vgap(8.0), ribbon,
        vgap(24.0),
        sectionTitle("MaterialBanner"),
        caption("整宽提示条，可带 leading / 操作 / 关闭（material/banner.dart）。"),
        vgap(8.0), banner,
        vgap(10.0), dangerBanner,
        vgap(24.0),
        sectionTitle("WindowFrame"),
        caption("窗口外壳：标题栏 + 内容区 + 状态栏。"),
        vgap(8.0), frame,
        vgap(24.0),
        sectionTitle("DockPanel"),
        caption("可停靠面板：图钉切换 / 折叠展开。"),
        vgap(8.0), dock,
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：值输入（SpinBox / ButtonGroup / Store）
// ══════════════════════════════════════════════════════════════════════════════

/// Store 的状态快照（值语义）。Property<State> 用 `!=` 判重，C++20 由默认 `==` 合成。
struct StoreValueState {
    int count = 0;
    double step = 1.0;
    int align = 1;
    int dispatches = 0;
    std::string lastAction = "Init";
    bool operator==(const StoreValueState&) const = default;
};

/// 所有状态变更都用 Action 表达；View 不直接改 State。
struct StoreInc {};
struct StoreDec {};
struct StoreReset {};
struct StoreSetStep { double value = 1.0; };
struct StoreSetAlign { int index = 0; };
using StoreDemoAction = std::variant<StoreInc, StoreDec, StoreReset,
                                     StoreSetStep, StoreSetAlign>;

/// Reducer：纯函数，(旧 State, Action) → 新 State。
inline StoreValueState reduceStoreValue(StoreValueState s, const StoreDemoAction& a) {
    ++s.dispatches;
    std::visit([&](auto&& act) {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, StoreInc>) {
            s.count += static_cast<int>(s.step);
            s.lastAction = "Increment";
        } else if constexpr (std::is_same_v<T, StoreDec>) {
            s.count -= static_cast<int>(s.step);
            s.lastAction = "Decrement";
        } else if constexpr (std::is_same_v<T, StoreReset>) {
            // 只重置 count：step / align 由输入控件内部持有，重置它们会让控件
            // 显示与 Store 不一致（受控值需要控件支持外部回写）。
            s.count = 0;
            s.lastAction = "Reset";
        } else if constexpr (std::is_same_v<T, StoreSetStep>) {
            s.step = act.value;
            s.lastAction = "SetStep(" + std::to_string(static_cast<int>(act.value)) + ")";
        } else {
            s.align = act.index;
            s.lastAction = "SetAlign(" + std::to_string(act.index) + ")";
        }
    }, a);
    return s;
}

class StoreDemo : public StatefulWidget {
public:
    StoreDemo() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "StoreDemo"; }
};

/// View：单向数据流 Store → Property.changed() → setState() → 重建。
class StoreDemoState : public State {
public:
    void initState() override {
        // subscribe 是 state().changed().connect 的便捷包装。Store 与本 State
        // 同生命周期，无需保存 ConnectionId 去 disconnect。
        store_.subscribe([this](const StoreValueState&) {
            if (mounted()) setState();
        });
    }

    SharedPtr<Widget> build(BuildContext&) override {
        const auto& s = store_.currentState();
        static const char* kAlignNames[] = {"左对齐", "居中", "右对齐"};
        const int alignIdx = std::clamp(s.align, 0, 2);

        auto value = cui::make_shared<Text>(
            "count = " + std::to_string(s.count),
            TextStyle().size(24.0).bold().fgColor(primaryColor()));
        auto meta = caption("dispatch " + std::to_string(s.dispatches) +
                            " 次 · 最近 Action: " + s.lastAction +
                            " · step = " + std::to_string(static_cast<int>(s.step)) +
                            " · " + kAlignNames[alignIdx]);

        auto minus = cui::make_shared<OutlinedButton>(
            cui::make_shared<Text>("− step"), [this] { store_.dispatch(StoreDec{}); });
        auto plus = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("+ step"), [this] { store_.dispatch(StoreInc{}); });
        auto reset = cui::make_shared<TextButton>(
            cui::make_shared<Text>("重置 count"), [this] { store_.dispatch(StoreReset{}); });
        auto buttons = hrow(std::vector<SharedPtr<Widget>>{
            minus, cui::make_shared<SizedBox>(10.0, 0.0), plus,
            cui::make_shared<SizedBox>(10.0, 0.0), reset});

        // 值输入控件只发 Action，不自己持有真值（Store 是唯一真源）。
        auto stepSpin = cui::make_shared<SpinBox>(
            s.step, 1.0, 10.0, 1.0, 0,
            [this](double v) { store_.dispatch(StoreSetStep{v}); });
        auto alignGroup = cui::make_shared<ButtonGroup>(
            std::vector<std::string>{"左对齐", "居中", "右对齐"}, s.align,
            [this](int i) { store_.dispatch(StoreSetAlign{i}); });
        auto inputs = hrow(std::vector<SharedPtr<Widget>>{
            caption("步长"), cui::make_shared<SizedBox>(8.0, 0.0), stepSpin,
            cui::make_shared<SizedBox>(20.0, 0.0), caption("对齐"),
            cui::make_shared<SizedBox>(8.0, 0.0), alignGroup});

        auto col = vcol(std::vector<SharedPtr<Widget>>{
            value, vgap(6.0), meta, vgap(12.0), buttons, vgap(12.0), inputs,
            vgap(8.0),
            caption("改状态只能 dispatch：View → reducer → Property.changed() → setState()。")});
        auto card = cui::make_shared<Container>(col);
        card->padding(EdgeInsets::all(12.0))
            .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.08), 8.0});
        return card;
    }

private:
    cui::Store<StoreValueState, StoreDemoAction> store_{StoreValueState{}, reduceStoreValue};
};

inline std::unique_ptr<State> StoreDemo::createState() {
    return std::make_unique<StoreDemoState>();
}

inline SharedPtr<Widget> pageValueInput() {
    auto intSpin = cui::make_shared<SpinBox>(3.0, 0.0, 10.0, 1.0, 0);
    auto decSpin = cui::make_shared<SpinBox>(1.5, 0.0, 5.0, 0.5, 1);
    auto group = cui::make_shared<ButtonGroup>(
        std::vector<std::string>{"左对齐", "居中", "右对齐"}, 1);

    return pageBody({
        sectionTitle("SpinBox"),
        caption("数值输入：点数值区直接键入（回车/失焦规范化并夹到 min~max），或点 ▲▼ 按步长增减。"),
        vgap(8.0), intSpin,
        vgap(16.0), decSpin,
        vgap(24.0),
        sectionTitle("ButtonGroup"),
        caption("互斥按钮组：点击切换选中项。"),
        vgap(8.0), group,
        vgap(24.0),
        sectionTitle("Store（MVI · 单向数据流）"),
        caption("Store<State, Action> 是唯一真源：输入控件只 dispatch(Action)，reducer 产出新 State。"),
        vgap(8.0), cui::make_shared<StoreDemo>(),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：下拉刷新（PullToRefresh）
// ══════════════════════════════════════════════════════════════════════════════

class RefreshDemo : public StatefulWidget {
public:
    RefreshDemo() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "RefreshDemo"; }
};

class RefreshDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> rows;
        for (int i = 0; i < 4; ++i) {
            auto t = cui::make_shared<Text>("列表项 " + std::to_string(i + 1),
                                            TextStyle().size(12.5));
            auto c = cui::make_shared<Container>(t);
            c->padding(EdgeInsets::symmetric(8.0, 10.0))
                .decoration(BoxDecoration{theme().surfaceVariant, 4.0});
            rows.push_back(c);
            rows.push_back(cui::make_shared<SizedBox>(0.0, 6.0));
        }
        if (!rows.empty())
            rows.pop_back();
        auto list = cui::make_shared<Column>(std::move(rows));
        list->setMainAxisSize(MainAxisSize::Min);
        list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto listBox = cui::make_shared<Container>(list);
        listBox->width(300.0);

        auto refresh = cui::make_shared<PullToRefresh>(listBox, [this] {
            ++count_;
            setState();
        });

        auto hint = caption("已刷新 " + std::to_string(count_) + " 次（在列表上向下拖拽）");
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            refresh, cui::make_shared<SizedBox>(0.0, 8.0), hint});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    int count_ = 0;
};

inline std::unique_ptr<State> RefreshDemo::createState() {
    return std::make_unique<RefreshDemoState>();
}

inline SharedPtr<Widget> pageRefresh() {
    return pageBody({
        sectionTitle("PullToRefresh"),
        caption("下拉超过阈值松手触发刷新；指示条约 700ms 后自动收起"
                "（material/refresh_indicator.dart 简化）。"),
        vgap(10.0), cui::make_shared<RefreshDemo>(),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：尺寸动画（AnimatedSize / AnimatedCrossFade / AnimatedSwitcher）
// ══════════════════════════════════════════════════════════════════════════════

class AnimatedSizeDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "AnimatedSizeDemo"; }
};

class AnimatedSizeDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        // ① 展开/收起：子节点自然尺寸变了，外壳补间过去（最典型的用法）。
        std::vector<SharedPtr<Widget>> rows;
        const int n = expanded_ ? 4 : 1;
        for (int i = 0; i < n; ++i) {
            auto row = cui::make_shared<Container>(cui::make_shared<Text>(
                i == 0 ? "自身尺寸用动画补到子节点的新尺寸"
                       : "第 " + std::to_string(i + 1) + " 行：展开时新增的内容",
                TextStyle().size(12.5)));
            row->padding(EdgeInsets::symmetric(10.0, 6.0));
            rows.push_back(std::move(row));
        }
        auto list = cui::make_shared<Column>(std::move(rows));
        list->setMainAxisSize(MainAxisSize::Min);
        list->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto panel = cui::make_shared<DecoratedBox>(
            BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.10), 6.0}, std::move(list));
        auto expandAnim = cui::make_shared<AnimatedSize>(std::move(panel), Alignment::topLeft(),
                                                        250.0, Easing::EaseInOutCubic);
        auto toggle = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>(expanded_ ? "收起" : "展开"),
            [this] {
                expanded_ = !expanded_;
                setState();
            });

        // ② 补间过程中自身比子节点小 ⇒ 溢出：hardEdge 裁掉、none 放行；
        //    alignment 决定溢出朝哪个方向（topLeft 右下溢、bottomRight 左上溢）。
        auto block = [this](Alignment a, Clip clip, const std::string& label) {
            auto box = cui::make_shared<SizedBox>(
                boxSide_, boxSide_,
                cui::make_shared<DecoratedBox>(
                    BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.90), 4.0}));
            std::vector<SharedPtr<Widget>> kids;
            kids.push_back(cui::make_shared<AnimatedSize>(std::move(box), a, 250.0,
                                                         Easing::EaseInOutCubic, clip));
            kids.push_back(cui::make_shared<SizedBox>(0.0, 6.0));
            kids.push_back(caption(label));
            auto col = cui::make_shared<Column>(std::move(kids));
            col->setMainAxisSize(MainAxisSize::Min);
            col->setCrossAxisAlignment(CrossAxisAlignment::Start);
            return col;
        };
        std::vector<SharedPtr<Widget>> blocks;
        blocks.push_back(block(Alignment::center(), Clip::HardEdge, "hardEdge · center"));
        blocks.push_back(cui::make_shared<SizedBox>(24.0, 0.0));
        blocks.push_back(block(Alignment::center(), Clip::None, "none · center"));
        blocks.push_back(cui::make_shared<SizedBox>(24.0, 0.0));
        blocks.push_back(block(Alignment::bottomRight(), Clip::HardEdge, "hardEdge · bottomRight"));
        auto compareRow = cui::make_shared<Row>(std::move(blocks));
        compareRow->setMainAxisSize(MainAxisSize::Min);
        compareRow->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto grow = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>(boxSide_ > 40.0 ? "缩回 24" : "放大到 84"),
            [this] {
                boxSide_ = boxSide_ > 40.0 ? 24.0 : 84.0;
                setState();
            });

        // ③ AnimatedCrossFade：两个尺寸不同的子节点交叉淡化，自身尺寸由内层
        //    AnimatedSize 补过去（上游 animated_cross_fade.dart 的骨架：
        //    单 controller 驱动 firstCurve（取反）/secondCurve/sizeCurve）。
        auto fadeCard = [](const std::string& title, const std::vector<std::string>& lines,
                           const Color& tint) {
            std::vector<SharedPtr<Widget>> rows;
            rows.push_back(cui::make_shared<Text>(title, TextStyle().size(13.0).bold()));
            for (const auto& l : lines)
                rows.push_back(cui::make_shared<Text>(l, TextStyle().size(12.0)));
            auto col = cui::make_shared<Column>(std::move(rows));
            col->setMainAxisSize(MainAxisSize::Min);
            col->setCrossAxisAlignment(CrossAxisAlignment::Start);
            auto box = cui::make_shared<Container>(std::move(col));
            box->padding(EdgeInsets::symmetric(12.0, 10.0));
            box->decoration(BoxDecoration{tint, 6.0});
            return box;
        };
        auto crossFade = cui::make_shared<AnimatedCrossFade>(
            fadeCard("firstChild（一行）", {"淡出者会被 IgnorePointer 包住，不吃命中。"},
                     Color::fromRGBA(0.0, 0.45, 0.95, 0.12)),
            fadeCard("secondChild（三行）",
                     {"两个子节点尺寸不同 ⇒ 内层 AnimatedSize 用 sizeCurve 补自身尺寸。",
                      "top/bottom 由 crossFadeState 决定，Positioned 上的 key 让子树复用。"},
                     Color::fromRGBA(0.55, 0.20, 0.85, 0.14)),
            showSecond_ ? CrossFadeState::ShowSecond : CrossFadeState::ShowFirst, 300.0,
            Easing::Linear, Easing::Linear, Easing::EaseInOutCubic);
        auto fadeToggle = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>(showSecond_ ? "切回 first" : "切到 second"),
            [this] {
                showSecond_ = !showSecond_;
                setState();
            });

        // ④ AnimatedSwitcher：换子时新旧子并行淡入淡出。上游的坑写在这儿 ——
        //    「同类型 + 同 key」的两个子被 Widget.canUpdate 判成同一个，不会过渡，
        //    所以每个子都要挂一份自己的 key（这里用 KeyedSubtree 补）。
        static constexpr const char* kSwitchTitles[3] = {
            "第一个子节点", "第二个子节点（两行）", "第三个子节点（三行，最宽的一档）"};
        static constexpr int kSwitchLines[3] = {1, 2, 3};
        const Color kSwitchTints[3] = {
            Color::fromRGBA(0.0, 0.45, 0.95, 0.12), Color::fromRGBA(0.55, 0.20, 0.85, 0.14),
            Color::fromRGBA(0.0, 0.60, 0.35, 0.16)};
        const int which = switchIndex_;
        std::vector<std::string> lines;
        for (int i = 0; i < kSwitchLines[which]; ++i)
            lines.push_back("第 " + std::to_string(i + 1) + " 行：换子时旧的在淡出、新的在淡入。");
        auto switcher = cui::make_shared<AnimatedSwitcher>(
            cui::make_shared<KeyedSubtree>(cui::make_shared<const ValueKey<int>>(which),
                                           fadeCard(kSwitchTitles[which], lines, kSwitchTints[which])),
            300.0, Easing::Linear, Easing::Linear);
        auto switchNext = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("换下一个子节点"),
            [this] {
                switchIndex_ = (switchIndex_ + 1) % 3;
                setState();
            });

        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(caption("点下面的按钮改子节点尺寸；补间期间动画自己走。"));
        kids.push_back(vgap(8.0));        kids.push_back(std::move(expandAnim));
        kids.push_back(vgap(8.0));
        kids.push_back(std::move(toggle));
        kids.push_back(vgap(24.0));
        kids.push_back(caption("溢出与对齐：子节点从 24 换到 84 时，自身尺寸补在后面。"));
        kids.push_back(vgap(8.0));
        kids.push_back(std::move(grow));
        kids.push_back(vgap(12.0));
        kids.push_back(std::move(compareRow));
        kids.push_back(vgap(24.0));
        kids.push_back(caption("AnimatedCrossFade：交叉淡化 + 尺寸过渡（内层就是上面的 "
                               "AnimatedSize）。"));
        kids.push_back(vgap(8.0));
        kids.push_back(std::move(fadeToggle));
        kids.push_back(vgap(12.0));
        kids.push_back(std::move(crossFade));
        kids.push_back(vgap(24.0));
        kids.push_back(caption("AnimatedSwitcher：新旧子并行淡入淡出（每个子一份 driver + "
                               "KeyedSubtree 身份；连点可叠出多个正在淡出的旧子）。"));
        kids.push_back(vgap(8.0));
        kids.push_back(std::move(switchNext));
        kids.push_back(vgap(12.0));
        kids.push_back(std::move(switcher));
        auto wrap = cui::make_shared<Column>(std::move(kids));
        wrap->setMainAxisSize(MainAxisSize::Min);
        wrap->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return wrap;
    }

private:
    bool expanded_ = false;
    bool showSecond_ = false;
    int switchIndex_ = 0;
    double boxSide_ = 24.0;
};

inline std::unique_ptr<State> AnimatedSizeDemo::createState() {
    return std::make_unique<AnimatedSizeDemoState>();
}

inline SharedPtr<Widget> pageAnimatedSize() {
    return pageBody({
        sectionTitle("AnimatedSize"),
        caption("子节点尺寸变化时用动画过渡自身尺寸（widgets/animated_size.dart + "
                "rendering/animated_size.dart 的四态状态机）；AnimatedCrossFade 建在它上面，"
                "AnimatedSwitcher 是同一族的换子过渡（widgets/animated_switcher.dart）。"),
        vgap(12.0),
        cui::make_shared<AnimatedSizeDemo>(),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：intrinsic 尺寸（IntrinsicHeight / IntrinsicWidth）
// ══════════════════════════════════════════════════════════════════════════════

class IntrinsicDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "IntrinsicDemo"; }
};

class IntrinsicDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        // 一块纯色矩形：intrinsic 尺寸 = 自身给定尺寸（等价于 SizedBox）。
        auto block = [](double w, double h, const Color& c) {
            return cui::make_shared<SizedBox>(
                    w, h, cui::make_shared<DecoratedBox>(BoxDecoration{c, 4.0}));
        };
        const Color frame(0.0, 0.0, 0.0, 0.05);

        // 带底色的画框：松约束由 Align 给出，框本身固定尺寸，
        // 于是「撑满」与「收成 intrinsic」的差别一眼可见。
        auto framed = [&frame](double w, double h, SharedPtr<Widget> content) {
            return cui::make_shared<SizedBox>(
                    w, h, cui::make_shared<DecoratedBox>(
                                  BoxDecoration{frame, 6.0},
                                  cui::make_shared<Align>(0.0, 0.0, std::move(content))));
        };
        auto labelled = [](SharedPtr<Widget> panel, const std::string& label) {
            std::vector<SharedPtr<Widget>> kids;
            kids.push_back(std::move(panel));
            kids.push_back(vgap(6.0));
            kids.push_back(caption(label));
            auto col = cui::make_shared<Column>(std::move(kids));
            col->setMainAxisSize(MainAxisSize::Min);
            col->setCrossAxisAlignment(CrossAxisAlignment::Start);
            return col;
        };

        // ── ① IntrinsicHeight：高度收成子节点的最大 intrinsic 高 ──────
        auto makeStack = [this, block](bool hug) {
            std::vector<SharedPtr<Widget>> kids;
            kids.push_back(block(120.0, 30.0, Color(0.0, 0.45, 0.95, 0.85)));
            kids.push_back(block(90.0, 40.0, Color(0.0, 0.62, 0.44, 0.85)));
            kids.push_back(block(60.0, growTail_ ? 60.0 : 20.0, Color(0.85, 0.55, 0.05, 0.85)));
            auto col = cui::make_shared<Column>(std::move(kids));
            col->setCrossAxisAlignment(CrossAxisAlignment::Start);
            SharedPtr<Widget> inner = cui::make_shared<DecoratedBox>(
                    BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.10), 6.0}, std::move(col));
            return hug ? cui::static_pointer_cast<Widget>(
                                 cui::make_shared<IntrinsicHeight>(std::move(inner)))
                       : inner;
        };
        std::vector<SharedPtr<Widget>> heightRow;
        heightRow.push_back(labelled(framed(260.0, 170.0, makeStack(false)),
                                     "Column：松约束下主轴撑满（MainAxisSize.max）"));
        heightRow.push_back(cui::make_shared<SizedBox>(20.0, 0.0));
        heightRow.push_back(labelled(framed(260.0, 170.0, makeStack(true)),
                                     "IntrinsicHeight：高 = Σ 子的最大 intrinsic 高"));
        auto heightCompare = cui::make_shared<Row>(std::move(heightRow));
        heightCompare->setMainAxisSize(MainAxisSize::Min);
        heightCompare->setCrossAxisAlignment(CrossAxisAlignment::Start);

        std::vector<SharedPtr<Widget>> tuneRow;
        tuneRow.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>(growTail_ ? "尾块缩回 20" : "尾块长到 60"),
                [this] {
                    growTail_ = !growTail_;
                    setState();
                }));
        tuneRow.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
        tuneRow.push_back(caption("改内容 ⇒ 只有右边那块的尺寸跟着变（左块撑满，本来就无关）"));
        auto tune = cui::make_shared<Row>(std::move(tuneRow));
        tune->setMainAxisSize(MainAxisSize::Min);
        tune->setCrossAxisAlignment(CrossAxisAlignment::Start);

        // ── ② IntrinsicWidth：宽度收成子节点的最大 intrinsic 宽（可带步长）──
        auto makeLine = [](std::optional<double> stepWidth, std::optional<double> stepHeight) {
            std::vector<SharedPtr<Widget>> kids;
            kids.push_back(cui::make_shared<SizedBox>(60.0, 30.0));
            kids.push_back(cui::make_shared<SizedBox>(40.0, 50.0));
            kids.push_back(cui::make_shared<SizedBox>(80.0, 20.0));
            auto row = cui::make_shared<Row>(std::move(kids));
            row->setCrossAxisAlignment(CrossAxisAlignment::Start);
            SharedPtr<Widget> inner = cui::make_shared<DecoratedBox>(
                    BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.10), 6.0}, std::move(row));
            if (stepWidth.has_value() || stepHeight.has_value())
                inner = cui::make_shared<IntrinsicWidth>(stepWidth, stepHeight, std::move(inner));
            return inner;
        };
        std::vector<SharedPtr<Widget>> widthRow;
        widthRow.push_back(labelled(framed(300.0, 90.0, makeLine(std::nullopt, std::nullopt)),
                                    "Row：主轴撑满 300，高 = 子的最大高 50"));
        widthRow.push_back(cui::make_shared<SizedBox>(20.0, 0.0));
        widthRow.push_back(labelled(framed(300.0, 90.0, makeLine(100.0, std::nullopt)),
                                    "IntrinsicWidth(stepWidth:100)：60+40+80=180 → 200"));
        auto widthCompare = cui::make_shared<Row>(std::move(widthRow));
        widthCompare->setMainAxisSize(MainAxisSize::Min);
        widthCompare->setCrossAxisAlignment(CrossAxisAlignment::Start);

        std::vector<SharedPtr<Widget>> stepRow;
        stepRow.push_back(labelled(framed(300.0, 110.0, makeLine(std::nullopt, 40.0)),
                                   "stepHeight:40 → 高 50 抬到 80（宽不受影响）"));
        stepRow.push_back(cui::make_shared<SizedBox>(20.0, 0.0));
        stepRow.push_back(labelled(framed(300.0, 110.0, makeLine(25.0, std::nullopt)),
                                   "stepWidth:25 → 180 已是 25 的倍数，保持 180"));
        auto stepCompare = cui::make_shared<Row>(std::move(stepRow));
        stepCompare->setMainAxisSize(MainAxisSize::Min);
        stepCompare->setCrossAxisAlignment(CrossAxisAlignment::Start);

        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(sectionTitle("IntrinsicHeight"));
        kids.push_back(vgap(8.0));
        kids.push_back(std::move(heightCompare));
        kids.push_back(vgap(12.0));
        kids.push_back(std::move(tune));
        kids.push_back(vgap(24.0));
        kids.push_back(sectionTitle("IntrinsicWidth"));
        kids.push_back(vgap(8.0));
        kids.push_back(std::move(widthCompare));
        kids.push_back(vgap(16.0));
        kids.push_back(std::move(stepCompare));
        auto col = cui::make_shared<Column>(std::move(kids));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    bool growTail_ = false;
};

inline std::unique_ptr<State> IntrinsicDemo::createState() {
    return std::make_unique<IntrinsicDemoState>();
}

inline SharedPtr<Widget> pageIntrinsic() {
    return pageBody({
        sectionTitle("intrinsic 尺寸"),
        caption("上游的 IntrinsicWidth / IntrinsicHeight（widgets/basic.dart + "
                "rendering/proxy_box.dart）：先问子节点「这一轴最多要多长」，再把该轴收紧到"
                "这个值下发。前提是被问的盒实现了 compute*Intrinsic*（本仓 Padding / "
                "ConstrainedBox / Align 因子 / Stack 非 positioned 子 / Flex / Paragraph 已对齐）。"),
        vgap(12.0),
        cui::make_shared<IntrinsicDemo>(),
        vgap(20.0),
        caption("代价：intrinsic 要为「尚未布局的子节点」推算尺寸，最坏把整棵树布局变成 "
                "O(N²)；上游文档同样写着能不套就不套。被查过 intrinsic 的盒标脏时会越过 "
                "relayout boundary 把脏标交给父节点（RenderBox::markNeedsLayout 的缓存耦合），"
                "否则父节点会拿着过期答案排版。"),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：异步加载（Application::runAsync：线程池干活 + GUI 线程收结果）
// ══════════════════════════════════════════════════════════════════════════════

/// 一项后台工作：既是投给线程池的参数，也是它带回来的结果。
///
/// 跨线程流动的只有这个值类型。foundation/shared_ptr.h 的边界铁律：投递给线程池
/// / 事件循环的闭包**不得捕获 `SharedPtr`**（非原子计数会在别的线程上加减），要带
/// 东西就用值类型、裸指针，或 `std::shared_ptr`（原子版）。
struct AsyncJob {
    int index = 0;
    int delayMs = 0;
};

class AsyncDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "AsyncDemo"; }
};

class AsyncDemoState : public State {
public:
    void dispose() override {
        // 池子里可能还有没回来的任务。页面切走 ⇒ State 先死，那些 onDone 再跑到
        // 就是踩空，所以先把存活标志落下。闭包按值握着这个 std::shared_ptr，
        // 它比 State 活得久；正因为跨线程，这里用原子版而不是 SharedPtr。
        *alive_ = false;
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        TextStyle head;
        head.size(17.0).bold();
        head.fgColor(running_ ? primaryColor()
                              : (done_ >= kNumItems ? Color::fromRGB(0.0, 0.55, 0.30)
                                                    : Color::fromRGB(0.15, 0.15, 0.20)));

        const double frac = total_ > 0 ? static_cast<double>(done_) /
                                                  static_cast<double>(total_)
                                       : 0.0;
        auto bar = cui::make_shared<SizedBox>(
                520.0, 8.0, cui::make_shared<LinearProgressIndicator>(
                                    std::optional<double>(frac),
                                    Color::fromRGBA(0.0, 0.0, 0.0, 0.10), primaryColor(), 8.0));

        // Load 在跑的时候 onPressed 留空 —— 上游的禁用态就是「没有回调」。
        ElevatedButton::VoidCallback loadCb;
        if (!running_) loadCb = [this] { start(); };
        std::vector<SharedPtr<Widget>> btnRow;
        btnRow.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>(running_ ? "Loading..." : "Load"), std::move(loadCb)));
        btnRow.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
        btnRow.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("UI taps: " + std::to_string(taps_)),
                [this] { ++taps_; setState(); }));
        btnRow.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
        btnRow.push_back(caption("后台在睡，主线程没睡：加载期间连点右边那个"));
        auto buttons = cui::make_shared<Row>(std::move(btnRow));
        buttons->setMainAxisSize(MainAxisSize::Min);
        buttons->setCrossAxisAlignment(CrossAxisAlignment::Center);

        // 完成日志：按「到达顺序」append，行里带各自的睡眠时长 ⇒ 乱序回来看得见。
        std::vector<SharedPtr<Widget>> lines;
        for (const auto& line : log_) lines.push_back(caption(line));
        if (lines.empty()) lines.push_back(caption("(还没有任务回来)"));
        auto logCol = cui::make_shared<Column>(std::move(lines));
        logCol->setMainAxisSize(MainAxisSize::Min);
        logCol->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto logPanel = cui::make_shared<Container>(std::move(logCol));
        logPanel->padding(EdgeInsets::all(10.0))
                .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.04), 6.0});

        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Text>(status_, head));
        kids.push_back(vgap(10.0));
        kids.push_back(std::move(bar));
        kids.push_back(vgap(14.0));
        kids.push_back(std::move(buttons));
        kids.push_back(vgap(16.0));
        kids.push_back(std::move(logPanel));
        auto col = cui::make_shared<Column>(std::move(kids));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    static constexpr int kNumItems = 8;

    void start() {
        Application* app = Application::instance();
        if (running_ || app == nullptr)
            return;
        running_ = true;
        done_ = 0;
        total_ = kNumItems;
        log_.clear();
        status_ = "Loading... 0/" + std::to_string(kNumItems);
        setState();

        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> delayDist(300, 2000);
        for (int i = 0; i < kNumItems; ++i) {
            const AsyncJob job{i, delayDist(rng)};
            app->runAsync(
                    // 后台线程：只读值类型。GUI 对象、SharedPtr 一个都不许碰。
                    [job] {
                        std::this_thread::sleep_for(
                                std::chrono::milliseconds(job.delayMs));
                        std::fprintf(stderr, "[Async] item %d slept %dms off-thread\n",
                                     job.index, job.delayMs);
                        return job;
                    },
                    // GUI 线程：runAsync 把结果经 postTask 投递回来，这里只做
                    // 「改状态 + setState」，重绘交给正常的一帧。
                    [this, app, alive = alive_](AsyncJob result) {
                        if (!*alive)
                            return;  // 页面已切走，State 不在了
                        ++done_;
                        status_ = done_ >= kNumItems
                                          ? "All items loaded!"
                                          : "Loading... " + std::to_string(done_) + "/" +
                                                    std::to_string(kNumItems);
                        log_.push_back("item " + std::to_string(result.index) + " (slept " +
                                       std::to_string(result.delayMs) + "ms) -> arrived " +
                                       std::to_string(done_) + ", onGuiThread=" +
                                       (app->isGuiThread() ? "true" : "false"));
                        if (done_ >= kNumItems) running_ = false;
                        setState();
                    });
        }
    }

    std::string status_ = "Idle";
    std::vector<std::string> log_;
    int done_ = 0;
    int total_ = 0;
    int taps_ = 0;
    bool running_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

inline std::unique_ptr<State> AsyncDemo::createState() {
    return std::make_unique<AsyncDemoState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：Material Motion（story 的 11–15 节）
// ══════════════════════════════════════════════════════════════════════════════

class MaterialMotionDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "MaterialMotionDemo"; }
};

class MaterialMotionDemoState : public State {
public:
    void initState() override {
        // 上游 animations 包里循环演示一贯的写法：controller 自己来回摆，
        // Ticker 只负责把真实时间喂给它。
        fadeScale_ = cui::make_shared<AnimationController>(1200.0);
        fadeScale_->setEasing(Easing::EaseInOutCubic);
        fadeScale_->setLoop(true);
        fadeScale_->setAutoReverse(true);
        fadeScale_->start();
        ticker_ = std::make_unique<Ticker>([this](Duration elapsed) {
            if (!fadeScale_)
                return;
            fadeScale_->tick(elapsed);
            if (!fadeScale_->isRunning() && ticker_ && ticker_->isActive())
                ticker_->stop();
        });
        ticker_->start();

        open_ = cui::make_shared<AnimationController>(400.0);
        open_->setEasing(Easing::EaseInOutCubic);
    }

    void dispose() override {
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        // ── 11 · FadeThroughTransition（PageTransitionSwitcher 驱动）──────
        auto fadeSwitcher = cui::make_shared<PageTransitionSwitcher>(
                cui::make_shared<KeyedSubtree>(
                        cui::make_shared<const ValueKey<int>>(fadePage_),
                        fadePage_ == 0
                                ? card("Page A", "Outgoing", Color::fromRGB(0.30, 0.55, 0.95))
                                : card("Page B", "Incoming", Color::fromRGB(0.15, 0.65, 0.50))),
                [](SharedPtr<Widget> child, SharedPtr<Animation<double>> primary,
                   SharedPtr<Animation<double>> secondary) {
                    return cui::make_shared<FadeThroughTransition>(primary, secondary, child);
                },
                500.0);
        auto fadeSection = hrow({fadeSwitcher, cui::make_shared<SizedBox>(24.0, 0.0),
                                 cui::make_shared<ElevatedButton>(
                                         cui::make_shared<Text>("A ⇄ B"),
                                         [this] {
                                             fadePage_ ^= 1;
                                             setState();
                                         })});

        // ── 12 · SharedAxisTransition (X) ────────────────────────────────
        auto axisSwitcher = cui::make_shared<PageTransitionSwitcher>(
                cui::make_shared<KeyedSubtree>(
                        cui::make_shared<const ValueKey<int>>(axisPage_),
                        axisPage_ == 0 ? card("Step 1", "横向共享轴", Color::fromRGB(0.55, 0.35, 0.85))
                                       : card("Step 2", "横向共享轴", Color::fromRGB(0.35, 0.45, 0.90))),
                [](SharedPtr<Widget> child, SharedPtr<Animation<double>> primary,
                   SharedPtr<Animation<double>> secondary) {
                    return cui::make_shared<SharedAxisTransition>(
                            primary, secondary, SharedAxisTransitionType::Horizontal, child);
                },
                500.0);
        auto axisSection = hrow({axisSwitcher, cui::make_shared<SizedBox>(24.0, 0.0),
                                 cui::make_shared<ElevatedButton>(
                                         cui::make_shared<Text>("下一步 ⇄ 上一步"),
                                         [this] {
                                             axisPage_ ^= 1;
                                             setState();
                                         })});

        // ── 13 · FadeScaleTransition（循环播放，无按钮）────────────────
        auto scaleCard = cui::make_shared<FadeScaleTransition>(
                fadeScale_, card("FAB", "Fade + Scale", Color::fromRGB(0.90, 0.42, 0.20)));

        // ── 14 · OpenContainer ───────────────────────────────────────────
        std::vector<SharedPtr<Widget>> expandedKids;
        expandedKids.push_back(cui::make_shared<Text>(
                "Expanded!", TextStyle().size(15.0).bold().fgColor(Color::fromRGB(1, 1, 1))));
        expandedKids.push_back(vgap(4.0));
        expandedKids.push_back(cui::make_shared<Text>(
                "Tap to collapse", TextStyle().size(12.0).fgColor(Color::fromRGBA(1, 1, 1, 0.75))));
        auto expandedCol = cui::make_shared<Column>(std::move(expandedKids));
        expandedCol->setMainAxisSize(MainAxisSize::Min);
        expandedCol->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto openContainer = cui::make_shared<OpenContainer>(
                open_, card("Tap to expand", "", Color::fromRGB(0.72, 0.78, 0.98)),
                cui::make_shared<SizedBox>(240.0, 96.0, expandedCol),
                Color::fromRGB(0.95, 0.95, 1.0), Color::fromRGB(0.20, 0.40, 0.85), 8.0);
        auto openSection = hrow({openContainer, cui::make_shared<SizedBox>(24.0, 0.0),
                                 cui::make_shared<ElevatedButton>(
                                         cui::make_shared<Text>("Toggle Container"),
                                         [this] {
                                             if (open_->value() < 0.5)
                                                 open_->forward();
                                             else
                                                 open_->reverse();
                                         })});

        // ── 15 · PageTransitionSwitcher（三页轮换）──────────────────────
        static const char* kTitles[3] = {"Page 1", "Page 2", "Page 3"};
        static const char* kBodies[3] = {"First page content", "Second page content",
                                         "Third page content"};
        static const Color kColors[3] = {Color::fromRGB(0.25, 0.62, 0.85),
                                         Color::fromRGB(0.45, 0.55, 0.90),
                                         Color::fromRGB(0.20, 0.70, 0.60)};
        const int page = cyclePage_ % 3;
        auto cycleSwitcher = cui::make_shared<PageTransitionSwitcher>(
                cui::make_shared<KeyedSubtree>(
                        cui::make_shared<const ValueKey<int>>(page),
                        card(kTitles[page], kBodies[page], kColors[page])),
                [](SharedPtr<Widget> child, SharedPtr<Animation<double>> primary,
                   SharedPtr<Animation<double>>) {
                    return cui::make_shared<FadeScaleTransition>(primary, child);
                },
                350.0);
        auto cycleSection = hrow({cycleSwitcher, cui::make_shared<SizedBox>(24.0, 0.0),
                                  cui::make_shared<ElevatedButton>(
                                          cui::make_shared<Text>("Next Page"),
                                          [this] {
                                              cyclePage_ = (cyclePage_ + 1) % 3;
                                              setState();
                                          })});

        return pageBody({
            sectionTitle("Material Motion 转场"),
            caption("对齐 package:animations 与 Flutter core 的 DualTransitionBuilder："
                    "同一个动画按播放方向切成「入场 / 退场」两套 builder，"
                    "被打断时保持上一次的有效方向（Forward/Reverse 只在端点互换）。"),
            vgap(16.0),
            sectionTitle("11 · FadeThroughTransition"),
            caption("旧内容先淡出、再淡入新内容；两段在中点交叉，新内容从 92% 放大到 100%。"),
            vgap(8.0),
            fadeSection,
            vgap(24.0),
            sectionTitle("12 · SharedAxisTransition (X)"),
            caption("沿共享轴平移 30px + 延迟淡入（Interval 0.3→1.0），"
                    "退场者只在前 30% 淡出并带背景色。"),
            vgap(8.0),
            axisSection,
            vgap(24.0),
            sectionTitle("13 · FadeScaleTransition"),
            caption("入场：前 30% 淡入 + 80%→100% 缩放（decelerate）；退场只淡出。"
                    "这里用 autoReverse 循环播放。"),
            vgap(8.0),
            scaleCard,
            vgap(24.0),
            sectionTitle("14 · OpenContainer"),
            caption("容器变换的行内形态：closed/open 交叉淡入，背景色与圆角随进度补间"
                    "（上游是 ModalRoute 版；本仓按 story 的行内语义实现）。"),
            vgap(8.0),
            openSection,
            vgap(24.0),
            sectionTitle("15 · PageTransitionSwitcher"),
            caption("每个条目一份 primary / secondary controller：新子 primary 正放、"
                    "旧子 secondary 正放，播完自动摘除条目。"),
            vgap(8.0),
            cycleSection,
            vgap(40.0),
        });
    }

private:
    static SharedPtr<Widget> card(const std::string& title, const std::string& sub,
                                  Color color) {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Text>(
                title, TextStyle().size(15.0).bold().fgColor(Color::fromRGB(1, 1, 1))));
        if (!sub.empty()) {
            kids.push_back(vgap(4.0));
            kids.push_back(cui::make_shared<Text>(
                    sub, TextStyle().size(12.0).fgColor(Color::fromRGBA(1, 1, 1, 0.75))));
        }
        auto col = cui::make_shared<Column>(std::move(kids));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(col);
        box->padding(EdgeInsets::all(16.0));
        box->decoration(BoxDecoration{color, 10.0});
        return cui::make_shared<SizedBox>(240.0, 96.0, box);
    }

    SharedPtr<AnimationController> fadeScale_;
    SharedPtr<AnimationController> open_;
    std::unique_ptr<Ticker> ticker_;
    int fadePage_ = 0;
    int axisPage_ = 0;
    int cyclePage_ = 0;
};

inline std::unique_ptr<State> MaterialMotionDemo::createState() {
    return std::make_unique<MaterialMotionDemoState>();
}

inline SharedPtr<Widget> pageMaterialMotion() {
    // 页面自带 pageBody（五节内容 + 说明），这里直接返回演示 StatefulWidget。
    return cui::make_shared<MaterialMotionDemo>();
}

inline SharedPtr<Widget> pageAsync() {
    return pageBody({
        sectionTitle("异步加载（runAsync）"),
        caption(
                "Application::runAsync(work, onDone)：work 丢进线程池，结果由 "
                "postTask 送回 GUI 线程再跑 onDone。于是「耗时」和「出图」各走各的"
                "——状态机只在 GUI 线程被改，改完一次 setState 交给正常的一帧。"),
        vgap(12.0),
        cui::make_shared<AsyncDemo>(),
        vgap(16.0),
        caption("八项的睡眠时长各自随机（300–2000ms），所以下面这张表回来的顺序"
                "基本不等于 0..7 的提交顺序；进度条按「已回来几条」走，不是按序号。"),
        vgap(8.0),
        caption("线程边界（foundation/shared_ptr.h）：投给线程池的闭包只带值类型，"
                "不捕获 SharedPtr（非原子计数不能跨线程加减）；State 用一份 "
                "std::shared_ptr<bool> 存活标志兜住「页面切走、任务才回来」。"),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 故事页 —— 对齐 rui story（crates/rui/examples/story）的条目与演示内容
// ══════════════════════════════════════════════════════════════════════════════

// ── Demos / Spreadsheet ─────────────────────────────────────────────────────
// 对齐 story 的 pages_demos::page_spreadsheet：公式求值 + 可编辑数量/单价。

namespace sheet_detail {

/// 电子表格公式求值器 —— story `pages_demos::Parser` 的直译。
///
/// 递归下降：expr = term (± term)*、term = factor (*|/ factor)*、
/// factor = 括号 / 一元符号 / 数字 / 单元格引用（B2）/ SUM(D2:D4)。
/// 引用到以 '=' 开头的单元格会递归求值（深度上限 16，防循环引用）。
class FormulaParser {
public:
    using Sheet = std::vector<std::vector<std::string>>;

    FormulaParser(const std::string& expr, const Sheet& sheet, int depth = 0)
        : text_(expr), sheet_(sheet), depth_(depth) {}

    double parse() { return expr(); }

private:
    void skipWs() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t'))
            ++pos_;
    }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    bool eat(char c) {
        if (peek() != c)
            return false;
        ++pos_;
        return true;
    }

    double expr() {
        double v = term();
        for (;;) {
            skipWs();
            const char c = peek();
            if (c == '+') {
                ++pos_;
                v += term();
            } else if (c == '-') {
                ++pos_;
                v -= term();
            } else {
                return v;
            }
        }
    }

    double term() {
        double v = factor();
        for (;;) {
            skipWs();
            const char c = peek();
            if (c == '*') {
                ++pos_;
                v *= factor();
            } else if (c == '/') {
                ++pos_;
                const double d = factor();
                v = d != 0.0 ? v / d : 0.0;
            } else {
                return v;
            }
        }
    }

    double factor() {
        skipWs();
        const char c = peek();
        if (c == '(') {
            ++pos_;
            const double v = expr();
            skipWs();
            eat(')');
            return v;
        }
        if (c == '-') {
            ++pos_;
            return -factor();
        }
        if (c == '+') {
            ++pos_;
            return factor();
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.')
            return number();
        if (std::isalpha(static_cast<unsigned char>(c)))
            return refOrCall();
        if (pos_ < text_.size())
            ++pos_;
        return 0.0;
    }

    double number() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '.'))
            ++pos_;
        return std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
    }

    double refOrCall() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
        if (peek() == '(') {
            std::string name = text_.substr(start, pos_ - start);
            for (char& ch : name)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            ++pos_;  // '('
            if (name == "SUM") {
                skipWs();
                const auto a = parseRef();
                if (a) {
                    skipWs();
                    eat(':');
                    skipWs();
                    if (const auto b = parseRef()) {
                        skipWs();
                        eat(')');
                        double sum = 0.0;
                        for (std::size_t r = std::min(a->first, b->first);
                             r <= std::max(a->first, b->first); ++r)
                            for (std::size_t c = std::min(a->second, b->second);
                                 c <= std::max(a->second, b->second); ++c)
                                sum += cellValue(r, c);
                        return sum;
                    }
                }
            }
            return 0.0;
        }
        pos_ = start;
        if (const auto r = parseRef())
            return cellValue(r->first, r->second);
        return 0.0;
    }

    /// A1 → (row, col)，均为 0 基；非法引用返回 nullopt。
    std::optional<std::pair<std::size_t, std::size_t>> parseRef() {
        std::size_t col = 0, letters = 0;
        while (pos_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[pos_]))) {
            col = col * 26 + static_cast<std::size_t>(
                                std::toupper(static_cast<unsigned char>(text_[pos_])) - 'A' + 1);
            ++pos_;
            ++letters;
        }
        std::size_t row = 0, digits = 0;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            row = row * 10 + static_cast<std::size_t>(text_[pos_] - '0');
            ++pos_;
            ++digits;
        }
        if (letters == 0 || digits == 0 || row == 0)
            return std::nullopt;
        return std::make_pair(row - 1, col - 1);
    }

    double cellValue(std::size_t row, std::size_t col) {
        if (depth_ >= 16)
            return 0.0;
        if (row >= sheet_.size() || col >= sheet_[row].size())
            return 0.0;
        const std::string& raw = sheet_[row][col];
        if (raw.empty())
            return 0.0;
        if (raw.front() == '=') {
            FormulaParser sub(raw.substr(1), sheet_, depth_ + 1);
            return sub.parse();
        }
        return std::strtod(raw.c_str(), nullptr);
    }

    std::string text_;
    const Sheet& sheet_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

/// 求值并格式化：整数不带小数点，其余保留 4 位；非公式原样返回。
inline std::string evaluateCell(const std::string& raw, const FormulaParser::Sheet& sheet) {
    if (raw.empty() || raw.front() != '=')
        return raw;
    FormulaParser p(raw.substr(1), sheet);
    const double v = p.parse();
    if (std::isnan(v) || std::isinf(v))
        return "#ERR";
    char buf[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    else
        std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

}  // namespace sheet_detail

class SpreadsheetDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SpreadsheetDemo"; }
};

class SpreadsheetDemoState : public State {
public:
    void initState() override {
        addRow("苹果", "4", "2.5");
        addRow("梨", "2", "3");
        addRow("猕猴桃", "5", "1.2");
        formula_ = cui::make_shared<TextEditingController>("=B2*C2+1");
        formulaHandle_ = formula_->addListener([this] { setState(); });
    }

    void dispose() override {
        // 捕获 this 的监听必须走句柄摘除（ChangeNotifier::removeListener 只比裸函数指针）
        for (auto& row : rows_) {
            row.qty.ctl->removeListener(row.qty.handle);
            row.price.ctl->removeListener(row.price.handle);
        }
        if (formulaHandle_ != 0)
            formula_->removeListener(formulaHandle_);
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        using sheet_detail::evaluateCell;
        using Sheet = sheet_detail::FormulaParser::Sheet;

        Sheet sheet;
        sheet.push_back({"Item", "Qty", "Price", "Total"});
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const std::string n = std::to_string(i + 2);
            sheet.push_back({rows_[i].name, rows_[i].qty.ctl->text(),
                             rows_[i].price.ctl->text(), "=B" + n + "*C" + n});
        }
        sheet.push_back({"", "", "", ""});
        sheet.push_back({"合计", "", "",
                         "=SUM(D2:D" + std::to_string(rows_.size() + 1) + ")"});

        auto cellBox = [](double w, SharedPtr<Widget> child,
                          bool header = false) -> SharedPtr<Widget> {
            auto c = cui::make_shared<Container>(std::move(child));
            c->width(w).padding(EdgeInsets::symmetric(4.0, 6.0));
            if (header)
                c->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.05), 4.0});
            return c;
        };
        auto field = [](SharedPtr<TextEditingController> ctl) {
            return cui::make_shared<TextField>(
                    std::move(ctl),
                    InputDecoration{"", "", "", Color::fromRGBA(0.0, 0.0, 0.0, 0.18), 4.0, 1.0},
                    TextStyle().size(12.5));
        };

        const double widths[4] = {110.0, 70.0, 70.0, 100.0};
        std::vector<SharedPtr<Widget>> grid;
        std::vector<SharedPtr<Widget>> head;
        for (int i = 0; i < 4; ++i)
            head.push_back(cellBox(widths[i],
                                   cui::make_shared<Text>(
                                           std::string(1, static_cast<char>('A' + i)),
                                           TextStyle().size(11.0).bold().fgColor(
                                                   theme().textSecondary)),
                                   true));
        grid.push_back(hrow(std::move(head)));

        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const std::string total = evaluateCell(sheet[i + 1][3], sheet);
            auto row = hrow({
                cellBox(widths[0], cui::make_shared<Text>(rows_[i].name, TextStyle().size(12.5))),
                cellBox(widths[1], field(rows_[i].qty.ctl)),
                cellBox(widths[2], field(rows_[i].price.ctl)),
                cellBox(widths[3], cui::make_shared<Text>(
                        total, TextStyle().size(12.5).bold().fgColor(primaryColor()))),
            });
            auto zebra = cui::make_shared<Container>(row);
            if (i % 2 == 1)
                zebra->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.025), 4.0});
            grid.push_back(zebra);
        }

        const std::string grand = evaluateCell(sheet.back()[3], sheet);
        auto totalRow = hrow({
            cellBox(widths[0], cui::make_shared<Text>("合计", TextStyle().size(12.5).bold())),
            cellBox(widths[1], cui::make_shared<SizedBox>(0.0, 0.0)),
            cellBox(widths[2], cui::make_shared<SizedBox>(0.0, 0.0)),
            cellBox(widths[3], cui::make_shared<Text>(
                    grand, TextStyle().size(13.0).bold().fgColor(primaryColor()))),
        });
        auto totalBox = cui::make_shared<Container>(totalRow);
        totalBox->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.08), 4.0});
        grid.push_back(totalBox);

        auto table = cui::make_shared<Column>(std::move(grid));
        table->setMainAxisSize(MainAxisSize::Min);
        table->setCrossAxisAlignment(CrossAxisAlignment::Start);

        const std::string fx = evaluateCell(formula_->text(), sheet);
        auto fxBar = hrow({
            cui::make_shared<Text>("fx", TextStyle().size(12.0).bold().fgColor(captionColor())),
            cellBox(240.0, field(formula_), false),
            cui::make_shared<SizedBox>(10.0, 0.0),
            cui::make_shared<Text>("= " + fx, TextStyle().size(13.0).bold()),
        });

        return pageBody({table, vgap(14.0), fxBar});
    }

private:
    struct Cell {
        SharedPtr<TextEditingController> ctl;
        int handle = 0;
    };
    struct Row {
        std::string name;
        Cell qty;
        Cell price;
    };

    void addRow(std::string name, std::string qty, std::string price) {
        Row r;
        r.name = std::move(name);
        r.qty.ctl = cui::make_shared<TextEditingController>(std::move(qty));
        r.qty.handle = r.qty.ctl->addListener([this] { setState(); });
        r.price.ctl = cui::make_shared<TextEditingController>(std::move(price));
        r.price.handle = r.price.ctl->addListener([this] { setState(); });
        rows_.push_back(std::move(r));
    }

    std::vector<Row> rows_;
    SharedPtr<TextEditingController> formula_;
    int formulaHandle_ = 0;
};

inline std::unique_ptr<State> SpreadsheetDemo::createState() {
    return std::make_unique<SpreadsheetDemoState>();
}

inline SharedPtr<Widget> pageSpreadsheet() {
    return pageBody({
        sectionTitle("Spreadsheet（公式求值 + 可编辑单元格）"),
        caption("对齐 rui story 的 Demos/Spreadsheet：Qty / Price 是可编辑单元格，"
                "Total 由 =B2*C2 算出，合计用 =SUM(D2:D4)。"),
        caption("求值器是递归下降：支持 + - * /、括号、单元格引用与 SUM 区间；"
                "引用到公式格会递归求值（深度上限 16）。"),
        vgap(12.0),
        cui::make_shared<SpreadsheetDemo>(),
        vgap(16.0),
        caption("底部公式栏是活的：输入 =B2*C2+1 或 =SUM(B2:B4)，右边立刻给出结果；"
                "改上面任意一格 Qty / Price，Total 与本行合计都会跟着重算。"),
        vgap(40.0),
    });
}

// ── Demos / Chat ────────────────────────────────────────────────────────────
// 对齐 story 的 pages_demo_widgets::page_chat：会话列表 + 气泡消息区 + 输入栏。

struct ChatMessage {
    std::string text;
    bool mine = false;
    std::string time;
};

struct ChatThread {
    std::string name;
    std::string initials;
    std::string preview;
    int unread = 0;
    std::vector<ChatMessage> messages;
};

class ChatDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ChatDemo"; }
};

class ChatDemoState : public State {
public:
    void initState() override {
        threads_.push_back({"张三", "张", "好的，明天见！", 2,
                            {{"你好！", false, "14:20"},
                             {"明天开会吗？", false, "14:21"},
                             {"好的，明天见！", true, "14:32"}}});
        threads_.push_back({"李四", "李", "代码已提交", 0,
                            {{"PR 审核了吗？", true, "10:05"},
                             {"代码已提交", false, "10:15"}}});
        threads_.push_back({"工作群", "群", "周报提醒", 5,
                            {{"请大家提交周报", false, "09:00"},
                             {"周报提醒", false, "17:00"}}});
    }

    SharedPtr<Widget> build(BuildContext&) override {
        // ── 左：会话列表（选中态 / 未读角标）──
        std::vector<SharedPtr<Widget>> rows;
        for (int i = 0; i < static_cast<int>(threads_.size()); ++i) {
            const ChatThread& th = threads_[i];
            auto nameCol = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
                cui::make_shared<Text>(th.name, TextStyle().size(13.5).bold()),
                vgap(2.0),
                cui::make_shared<Text>(th.preview,
                                       TextStyle().size(11.5).fgColor(captionColor()))});
            nameCol->setMainAxisSize(MainAxisSize::Min);
            nameCol->setCrossAxisAlignment(CrossAxisAlignment::Start);

            std::vector<SharedPtr<Widget>> kids{
                cui::make_shared<CircleAvatar>(th.initials, 16.0, avatarColor(i)),
                cui::make_shared<SizedBox>(10.0, 0.0),
                cui::make_shared<Expanded>(1, nameCol)};
            if (th.unread > 0) {
                kids.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
                kids.push_back(cui::make_shared<Badge>(
                        cui::make_shared<SizedBox>(0.0, 0.0), th.unread));
            }
            auto row = cui::make_shared<Row>(std::move(kids));
            row->setCrossAxisAlignment(CrossAxisAlignment::Center);

            auto pad = cui::make_shared<Container>(row);
            pad->padding(EdgeInsets::symmetric(8.0, 6.0));
            if (i == selected_)
                pad->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.45, 0.95, 0.12), 6.0});
            rows.push_back(cui::make_shared<InkWell>(pad, [this, i] {
                selected_ = i;
                threads_[i].unread = 0;
                setState();
            }));
        }
        auto listCol = cui::make_shared<Column>(std::move(rows));
        listCol->setMainAxisSize(MainAxisSize::Min);
        listCol->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto left = cui::make_shared<Container>(listCol);
        left->width(228.0).padding(EdgeInsets::all(6.0))
                .decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.035), 8.0});

        // ── 右：消息区 + 输入栏 ──
        ChatThread& th = threads_[selected_];
        std::vector<SharedPtr<Widget>> lines;
        for (const ChatMessage& m : th.messages) {
            auto bubble = cui::make_shared<Bubble>(m.text, m.mine);
            auto when = cui::make_shared<Text>(m.time,
                                               TextStyle().size(10.0).fgColor(captionColor()));
            std::vector<SharedPtr<Widget>> line;
            if (m.mine) {
                line = {when, cui::make_shared<SizedBox>(6.0, 0.0),
                        cui::make_shared<Expanded>(1, bubble)};
            } else {
                line = {cui::make_shared<Expanded>(1, bubble),
                        cui::make_shared<SizedBox>(6.0, 0.0), when};
            }
            auto r = cui::make_shared<Row>(std::move(line));
            r->setCrossAxisAlignment(CrossAxisAlignment::End);
            lines.push_back(r);
        }
        auto msgCol = cui::make_shared<Column>(std::move(lines));
        msgCol->setMainAxisSize(MainAxisSize::Min);
        msgCol->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto msgScroll = cui::make_shared<SingleChildScrollView>(&threadScroll_, msgCol);

        auto header = hrow({
            cui::make_shared<CircleAvatar>(th.initials, 14.0, avatarColor(selected_)),
            cui::make_shared<SizedBox>(10.0, 0.0),
            cui::make_shared<Text>(th.name, TextStyle().size(15.0).bold()),
            cui::make_shared<SizedBox>(12.0, 0.0),
            caption("在线"),
        });

        auto input = cui::make_shared<TextField>(
                draft_,
                InputDecoration{"", "输入消息，点发送追加到当前会话", "",
                                Color::fromRGBA(0.0, 0.0, 0.0, 0.18), 6.0, 1.0},
                TextStyle().size(13.0));
        auto sendBtn = cui::make_shared<ElevatedButton>(cui::make_shared<Text>("发送"),
                                                        [this] { sendDraft(); });
        auto inputRow = hrow({cui::make_shared<Expanded>(1, input),
                              cui::make_shared<SizedBox>(8.0, 0.0), sendBtn});

        auto rightCol = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            header, vgap(6.0), cui::make_shared<Divider>(1.0, 1.0),
            cui::make_shared<Expanded>(1, msgScroll),
            cui::make_shared<Divider>(1.0, 1.0), inputRow});
        rightCol->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto right = cui::make_shared<Container>(rightCol);
        right->padding(EdgeInsets::all(10.0));

        auto body = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            left, cui::make_shared<SizedBox>(8.0, 0.0), cui::make_shared<Expanded>(1, right)});
        body->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto box = cui::make_shared<Container>(body);
        box->height(430.0);
        return box;
    }

private:
    static Color avatarColor(int i) {
        static const Color kPalette[] = {
            Color::fromRGB(0.25, 0.55, 0.95), Color::fromRGB(0.20, 0.70, 0.45),
            Color::fromRGB(0.85, 0.55, 0.15)};
        return kPalette[i % 3];
    }

    std::string nextTime() {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "14:%02d", 33 + (tick_++ % 25));
        return buf;
    }

    void sendDraft() {
        const std::string text = draft_->text();
        if (text.empty())
            return;
        ChatThread& th = threads_[selected_];
        th.messages.push_back({text, true, nextTime()});
        th.preview = text;
        th.unread = 0;
        draft_->setText("");
        setState();
    }

    std::vector<ChatThread> threads_;
    int selected_ = 0;
    int tick_ = 0;
    SharedPtr<TextEditingController> draft_ =
            cui::make_shared<TextEditingController>("");
    ScrollController threadScroll_;
};

inline std::unique_ptr<State> ChatDemo::createState() {
    return std::make_unique<ChatDemoState>();
}

inline SharedPtr<Widget> pageChat() {
    return pageBody({
        sectionTitle("Chat（会话列表 + 气泡消息 + 输入栏）"),
        caption("对齐 rui story 的 Demos/Chat：左侧会话列表（头像 / 未读角标 / 选中态）"
                "+ 右侧消息区 + 输入栏。"),
        caption("全部由现成控件组合（CircleAvatar / Badge / Bubble / InkWell / TextField），"
                "没有自绘。"),
        vgap(12.0),
        cui::make_shared<ChatDemo>(),
        vgap(16.0),
        caption("点左侧会话切换（未读清零），在输入框写字点「发送」会把消息追加成自己的气泡；"
                "消息区是独立的 SingleChildScrollView，条目多了自己滚。"),
        vgap(40.0),
    });
}

// ── Demos / Custom Widget ───────────────────────────────────────────────────
// 对齐 story 的 pages_demo_widgets::page_custom：仪表盘（拖动）、波形条（涂改）、
// 胶囊（点击切换）。cui_core 侧用 CustomPaint 自绘 + GestureDetector 取本地坐标。

class CustomWidgetDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "CustomWidgetDemo"; }
};

class CustomWidgetDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        constexpr double kGaugeW = 260.0, kGaugeH = 180.0;
        auto gaugePaint = [this](Painter& p, const Size& size) { paintGauge(p, size); };
        auto gauge = cui::make_shared<CustomPaint>(gaugePaint, nullptr,
                                                   Size{kGaugeW, kGaugeH});
        GestureDetector::PointerCallback onGauge = [this](const PointerEvent& e) {
            setGaugeFromPointer(e.position);
        };
        auto gaugeTouch = cui::make_shared<GestureDetector>(
                gauge, nullptr, onGauge, nullptr, nullptr, onGauge, nullptr);

        constexpr double kWaveW = 340.0, kWaveH = 120.0;
        auto wavePaint = [this](Painter& p, const Size& size) { paintWave(p, size); };
        auto wave = cui::make_shared<CustomPaint>(wavePaint, nullptr, Size{kWaveW, kWaveH});
        GestureDetector::PointerCallback onWave = [this](const PointerEvent& e) {
            setWaveFromPointer(e.position);
        };
        auto waveTouch = cui::make_shared<GestureDetector>(
                wave, nullptr, onWave, nullptr, nullptr, onWave, nullptr);

        constexpr double kCapW = 220.0, kCapH = 56.0;
        auto capPaint = [this](Painter& p, const Size& size) { paintCapsule(p, size); };
        auto cap = cui::make_shared<CustomPaint>(capPaint, nullptr, Size{kCapW, kCapH});
        auto capTouch = cui::make_shared<GestureDetector>(cap, [this] {
            capsuleOn_ = !capsuleOn_;
            setState();
        });

        return vcol({
            gaugeTouch,
            vgap(6.0),
            caption("仪表盘：点击/拖动指针（270° 量程，右上角死区按 0%/100% 吸附）。"),
            vgap(18.0),
            waveTouch,
            vgap(6.0),
            caption("波形条：在条上拖动或点击，按落点高度改写该槽的幅度。"),
            vgap(18.0),
            capTouch,
            vgap(6.0),
            caption("胶囊：点击在「已启用 / 已停用」之间切换（轨道 + 圆点 + 文字都是自绘）。"),
        });
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    void setGaugeFromPointer(const Offset& pos) {
        const double cx = 260.0 / 2.0;
        const double cy = 180.0 * 0.62;
        double deg = std::atan2(pos.dy - cy, pos.dx - cx) * 180.0 / kPi;
        if (deg < 0.0)
            deg += 360.0;
        double v;
        if (deg >= 45.0)
            v = std::clamp((deg - 135.0) / 270.0, 0.0, 1.0);
        else if (deg > 22.5)
            v = 1.0;
        else
            v = 0.0;
        if (v == gauge_)
            return;
        gauge_ = v;
        setState();
    }

    void setWaveFromPointer(const Offset& pos) {
        if (wave_.empty())
            return;
        const double slot = 340.0 / static_cast<double>(wave_.size());
        int idx = std::clamp(static_cast<int>(pos.dx / slot), 0,
                             static_cast<int>(wave_.size()) - 1);
        const double v = std::clamp(1.0 - (pos.dy - 6.0) / (120.0 - 18.0), 0.02, 1.0);
        if (wave_[idx] == v)
            return;
        wave_[idx] = v;
        setState();
    }

    void paintGauge(Painter& p, const Size& size) {
        const double cx = size.width / 2.0;
        const double cy = size.height * 0.62;
        const double r = 62.0;

        p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.10));
        p.drawArc(cx, cy, r, 135.0, 270.0, 10.0);
        if (gauge_ > 0.001) {
            p.setSourceColor(primaryColor());
            p.drawArc(cx, cy, r, 135.0, 270.0 * gauge_, 10.0);
        }

        p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.28));
        p.setLineWidth(1.0);
        for (int i = 0; i <= 10; ++i) {
            const double a = (135.0 + 27.0 * i) * kPi / 180.0;
            const double c = std::cos(a), s = std::sin(a);
            p.moveTo(cx + c * (r + 9.0), cy + s * (r + 9.0));
            p.lineTo(cx + c * (r + 14.0), cy + s * (r + 14.0));
        }
        p.stroke();

        const double a = (135.0 + 270.0 * gauge_) * kPi / 180.0;
        p.setSourceColor(Color::fromRGB(0.12, 0.13, 0.16));
        p.setLineWidth(3.0);
        p.moveTo(cx, cy);
        p.lineTo(cx + std::cos(a) * (r - 16.0), cy + std::sin(a) * (r - 16.0));
        p.stroke();
        p.circle(cx, cy, 5.0);
        p.fill();

        const std::string label =
                std::to_string(static_cast<int>(std::lround(gauge_ * 100.0))) + "%";
        p.setSourceColor(Color::fromRGB(0.18, 0.20, 0.24));
        p.setFontSize(18.0);
        p.translate(cx - 22.0, cy + 34.0);
        p.showText(label.c_str());
    }

    void paintWave(Painter& p, const Size& size) {
        const double slot = size.width / static_cast<double>(wave_.size());
        for (int i = 0; i < static_cast<int>(wave_.size()); ++i) {
            const double h = std::max(2.0, wave_[i] * (size.height - 18.0));
            p.setSourceColor(Color::fromRGBA(0.0, 0.45, 0.95, 0.25 + 0.75 * wave_[i]));
            p.fillRoundedRect(Rect(slot * i + slot * 0.18, size.height - 8.0 - h,
                                   slot * 0.64, h),
                              3.0);
        }
        p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, 0.12));
        p.fillRoundedRect(Rect(0.0, size.height - 6.0, size.width, 1.5), 1.0);
    }

    void paintCapsule(Painter& p, const Size& size) {
        p.setSourceColor(capsuleOn_ ? primaryColor()
                                    : Color::fromRGBA(0.0, 0.0, 0.0, 0.16));
        p.fillRoundedRect(Rect(0.0, 0.0, size.width, size.height), size.height / 2.0);

        const double d = size.height - 8.0;
        const double x = capsuleOn_ ? size.width - d - 4.0 : 4.0;
        p.setSourceColor(Color::fromRGB(1.0, 1.0, 1.0));
        p.circle(x + d / 2.0, size.height / 2.0, d / 2.0);
        p.fill();

        p.setSourceColor(capsuleOn_ ? Color::fromRGB(1.0, 1.0, 1.0)
                                    : Color::fromRGB(0.25, 0.27, 0.30));
        p.setFontSize(14.0);
        p.translate(capsuleOn_ ? 20.0 : 76.0, size.height / 2.0 + 5.0);
        p.showText(capsuleOn_ ? "已启用" : "已停用");
    }

    double gauge_ = 0.35;
    bool capsuleOn_ = false;
    std::vector<double> wave_ = {0.20, 0.55, 0.35, 0.80, 0.60, 0.25, 0.70, 0.45};
};

inline std::unique_ptr<State> CustomWidgetDemo::createState() {
    return std::make_unique<CustomWidgetDemoState>();
}

inline SharedPtr<Widget> pageCustomWidget() {
    return pageBody({
        sectionTitle("Custom Widget（三个自绘控件）"),
        caption("对齐 rui story 的 Demos/Custom Widget：仪表盘、波形条、胶囊三个自绘控件。"),
        caption("rui 里是实现 Widget trait 自绘；cui_core 侧对应 CustomPaint + GestureDetector"
                "（PointerEvent::position 已是本地坐标）。"),
        vgap(14.0),
        cui::make_shared<CustomWidgetDemo>(),
        vgap(40.0),
    });
}

// ── Navigation / TabBar (Flutter) ───────────────────────────────────────────
// 对齐 story 的 pages_nav::page_tab_bar：TabController + TabBar + TabBarView。

class TabBarDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "TabBarDemo"; }
};

class TabBarDemoState : public State {
public:
    void initState() override {
        unsub_ = controller_.addListener([this] { setState(); });
    }
    void dispose() override {
        if (unsub_) {
            unsub_();
            unsub_ = {};
        }
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        const int idx = controller_.index();
        // TabBar 只认 Text 标签（结构见 tabs.cpp 的 tabLabelOf）；选中态由它自己着色。
        const char* names[3] = {"简介", "参数", "示例"};
        std::vector<SharedPtr<Widget>> labels;
        for (const char* n : names)
            labels.push_back(cui::make_shared<Text>(n));

        auto page = [](const std::string& title, const std::string& body, Color tint) {
            auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
                cui::make_shared<Text>(title, TextStyle().size(16.0).bold()),
                vgap(8.0),
                cui::make_shared<Text>(body, TextStyle().size(12.5).fgColor(
                                                           theme().textSecondary))});
            col->setMainAxisSize(MainAxisSize::Min);
            col->setCrossAxisAlignment(CrossAxisAlignment::Start);
            auto c = cui::make_shared<Container>(col);
            c->padding(EdgeInsets::all(16.0)).height(170.0)
                    .decoration(BoxDecoration{tint, 8.0});
            return c;
        };
        std::vector<SharedPtr<Widget>> pages{
            page("简介", "TabBar 与 TabBarView 共享同一个 TabController：选中状态只有一个真源，"
                         "两边各自订阅重建。",
                 Color::fromRGBA(0.0, 0.45, 0.95, 0.08)),
            page("参数", "TabBar(controller, tabs, indicatorColor) / "
                         "TabBarView(controller, children)。TabBarView 用 IndexedStack 只显示当前页。",
                 Color::fromRGBA(0.20, 0.70, 0.45, 0.08)),
            page("示例", "点标签或下面的按钮切换；index 由 controller 广播，"
                         "标签高亮与内容区始终同步。",
                 Color::fromRGBA(0.85, 0.55, 0.15, 0.10)),
        };

        auto bar = cui::make_shared<TabBar>(&controller_, labels);
        auto view = cui::make_shared<TabBarView>(&controller_, pages);
        auto prev = cui::make_shared<OutlinedButton>(cui::make_shared<Text>("上一页"),
                                                     [this] { controller_.previous(); });
        auto next = cui::make_shared<ElevatedButton>(cui::make_shared<Text>("下一页"),
                                                     [this] { controller_.next(); });
        auto nav = hrow({prev, cui::make_shared<SizedBox>(8.0, 0.0), next,
                         cui::make_shared<SizedBox>(12.0, 0.0),
                         cui::make_shared<Text>(
                                 "index = " + std::to_string(idx),
                                 TextStyle().size(12.0).fgColor(captionColor()))});

        return vcol({bar, vgap(2.0), view, vgap(12.0), nav});
    }

private:
    TabController controller_{3};
    std::function<void()> unsub_;
};

inline std::unique_ptr<State> TabBarDemo::createState() {
    return std::make_unique<TabBarDemoState>();
}

inline SharedPtr<Widget> pageTabBar() {
    return pageBody({
        sectionTitle("TabBar / TabBarView（Flutter）"),
        caption("对齐上游 material/tabs.dart：TabController 持有 index 并向 TabBar / "
                "TabBarView 广播，两个控件各自订阅重建。"),
        caption("这是 story 里此前唯一没有单独演示的 Flutter 控件族；"
                "TabBarView 用 IndexedStack 只显示当前页。"),
        vgap(12.0),
        cui::make_shared<TabBarDemo>(),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：拖放（Draggable / LongPressDraggable / DragTarget）
// ══════════════════════════════════════════════════════════════════════════════

/// 上游 widgets/drag_target.dart 的演示。四段：
///   ① 按下即拖（= 上游 ImmediateMultiDrag）+ `childWhenDragging` 留下的空位；
///   ② 类型匹配但 `onWillAcceptWithDetails` 返回 false ⇒ avatar 进 builder 的第二列
///      rejectedData（投放区变红），抬手算取消；
///   ③ 类型不匹配（载荷 int vs 目标 string）：上游 `_getDragTargets` 里的
///      `isExpectedDataType` 在命中测试阶段就把目标筛掉，所以两列都是空的；
///   ④ LongPressDraggable（延时 300ms = 上游 DelayedMultiDrag）+ axis 锁水平。
class DragDropDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DragDropDemo"; }
};

class DragDropDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        const Color blue = tintBlue(), green = tintGreen(), amber = tintAmber(), red = tintRed();

        // 纯色卡片 + 居中文字：源、反馈、占位共用，只换底色/文案/尺寸。
        auto card = [](const std::string& label, const Color& bg, double w, double h) {
            auto t = cui::make_shared<Text>(tr(label),
                                            TextStyle().size(12.5).fgColor(Color::fromRGB(1.0, 1.0, 1.0)));
            return cui::make_shared<SizedBox>(
                    w, h, cui::make_shared<DecoratedBox>(
                                BoxDecoration{bg, 6.0}, cui::make_shared<Align>(0.0, 0.0, t)));
        };

        // ── 投放区：底色由 candidateData / rejectedData 两列决定；
        //    已接受的载荷留在区内（docked），空区才只显示提示文字 ──────────
        auto dropZone = [this, green, red](const std::string& title, bool hot, bool wrong,
                                           const std::vector<std::string>& docked) {
            const Color frame(0.0, 0.0, 0.0, 0.22);
            Color fill = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
            Color line = frame;
            if (hot) {
                fill = green.withOpacity(0.22);
                line = green;
            } else if (wrong) {
                fill = red.withOpacity(0.18);
                line = red;
            }
            std::string hint = hot ? "松手投放" : wrong ? "willAccept 拒收" : std::string();
            if (hint.empty() && docked.empty())
                hint = title;

            // 提示文字和胶囊同处一条 Wrap：有在飞的载荷时两样都露出来。
            std::vector<SharedPtr<Widget>> kids;
            if (!hint.empty())
                kids.push_back(cui::make_shared<Text>(tr(hint), TextStyle().size(12.5).fgColor(line)));
            for (const std::string& label : docked) {
                auto t = cui::make_shared<Text>(tr(label),
                                                TextStyle().size(11.0).fgColor(Color::fromRGB(1.0, 1.0, 1.0)));
                kids.push_back(cui::make_shared<DecoratedBox>(
                        BoxDecoration{tintFor(label), 4.0},
                        cui::make_shared<Padding>(EdgeInsets::symmetric(6.0, 3.0), std::move(t))));
            }
            SharedPtr<Widget> body;
            if (kids.size() == 1)
                body = std::move(kids.front());
            else
                body = cui::make_shared<Wrap>(Axis::Horizontal, std::move(kids), WrapAlignment::Start,
                                              6.0, WrapAlignment::Start, 5.0);
            return cui::make_shared<SizedBox>(
                    210.0, 88.0, cui::make_shared<DecoratedBox>(
                                        BoxDecoration{fill, 8.0, 1.5, line},
                                        cui::make_shared<Align>(0.0, 0.0, body)));
        };

        // ── ① 字符串载荷 + 接受它的目标 ────────────────────────────────────
        auto target = cui::make_shared<DragTarget<std::string>>(
                [this, dropZone](BuildContext&,
                                 const std::vector<std::optional<std::string>>& cand,
                                 const std::vector<std::any>& rejected) {
                    return dropZone("把卡片投进来", !cand.empty(), !rejected.empty(), docked_);
                },
                [](const DragTargetDetails<std::string>& d) { return d.data != "卡片 B"; },
                [this](const DragTargetDetails<std::string>& d) {
                    ++accepted_;
                    dock(docked_, d.data);
                    status_ = "已接受「" + d.data + "」，落点偏移 (" + num(d.offset.dx) + ", " +
                              num(d.offset.dy) + ")";
                    setState();
                },
                [this](const std::optional<std::string>&) {
                    status_ = "指针离开投放区";
                    setState();
                });

        auto source = cui::make_shared<Draggable>(
                card("卡片 A", blue, 120.0, 48.0), card("反馈：卡片 A", blue.withOpacity(0.8), 150.0, 60.0),
                std::any(std::string("卡片 A")), card("已拖走", blue.withOpacity(0.22), 120.0, 48.0),
                std::optional<Axis>{}, Offset(0.0, 0.0), childDragAnchorStrategy, std::optional<int>{},
                [this] {
                    status_ = "拖拽中…";
                    setState();
                },
                nullptr,
                [this](const Velocity&, const Offset&) {
                    status_ = "抬手在空白处 ⇒ onDraggableCanceled";
                    setState();
                },
                nullptr,
                [this] {
                    status_ = "投放成功 ⇒ onDragCompleted";
                    setState();
                });

        // ── ② 类型匹配但 willAccept 返回 false ⇒ 进 builder 的 rejectedData ──
        auto rejectSource = cui::make_shared<Draggable>(
                card("卡片 B", red, 120.0, 48.0), card("卡片 B", red.withOpacity(0.8), 120.0, 48.0),
                std::any(std::string("卡片 B")), nullptr, std::optional<Axis>{}, Offset(0.0, 0.0),
                childDragAnchorStrategy, std::optional<int>{},
                [this] {
                    status_ = "拖的是「卡片 B」，目标会拒收";
                    setState();
                });

        // ── ③ 类型不匹配：目标在命中测试阶段就被筛掉，连 rejectedData 都不进 ──
        auto intSource = cui::make_shared<Draggable>(
                card("载荷 int", amber, 120.0, 48.0), card("载荷 int", amber.withOpacity(0.8), 120.0, 48.0),
                std::any(42), nullptr, std::optional<Axis>{}, Offset(0.0, 0.0),
                childDragAnchorStrategy, std::optional<int>{},
                [this] {
                    status_ = "拖的是 int，目标是 string ⇒ 投放区毫无反应";
                    setState();
                });

        // ── ④ 长按 + 锁横轴 ───────────────────────────────────────────────
        auto longSource = cui::make_shared<LongPressDraggable>(
                card("按住 300ms", green, 120.0, 48.0), card("按住 300ms", green.withOpacity(0.8), 150.0, 60.0),
                std::any(std::string("长按卡片")), nullptr, Axis::Horizontal, Offset(0.0, 0.0),
                childDragAnchorStrategy, std::optional<int>{},
                [this] {
                    status_ = "长按起拖：竖轴被锁死，只能横移";
                    setState();
                });
        longSource->setDelay(Duration(300));

        auto secondTarget = cui::make_shared<DragTarget<std::string>>(
                [this, dropZone](BuildContext&, const std::vector<std::optional<std::string>>& cand,
                                 const std::vector<std::any>& rejected) {
                    return dropZone("长按卡片的投放区", !cand.empty(), !rejected.empty(), docked2_);
                },
                [](const DragTargetDetails<std::string>&) { return true; },
                [this](const DragTargetDetails<std::string>& d) {
                    ++accepted_;
                    dock(docked2_, d.data);
                    status_ = "已接受「" + d.data + "」";
                    setState();
                });

        std::vector<SharedPtr<Widget>> rowA;
        rowA.push_back(std::move(source));
        rowA.push_back(cui::make_shared<SizedBox>(20.0, 0.0));
        rowA.push_back(std::move(rejectSource));
        rowA.push_back(cui::make_shared<SizedBox>(20.0, 0.0));
        rowA.push_back(std::move(intSource));
        rowA.push_back(cui::make_shared<SizedBox>(20.0, 0.0));
        rowA.push_back(std::move(target));
        std::vector<SharedPtr<Widget>> rowB;
        rowB.push_back(std::move(longSource));
        rowB.push_back(cui::make_shared<SizedBox>(26.0, 0.0));
        rowB.push_back(std::move(secondTarget));

        auto status = cui::make_shared<Text>(
                tr(status_ + "   累计接受 " + std::to_string(accepted_) + " 次"),
                TextStyle().size(12.0).fgColor(captionColor()));
        auto clear = cui::make_shared<TextButton>(
                cui::make_shared<Text>("清空容器", TextStyle().size(12.0)),
                [this] {
                    docked_.clear();
                    docked2_.clear();
                    status_ = "容器已清空";
                    setState();
                });

        return vcol({
            sectionTitle("三种落点状态"),
            caption("卡片 A：类型匹配且 willAccept 放行 ⇒ 进 builder 的 candidateData（投放区变绿），"
                    "松手即 onAcceptWithDetails + onDragCompleted，载荷留在容器里（最多 4 枚，"
                    "超出挤掉最早的一枚；拖拽途中提示文字和已有胶囊并存）。"),
            caption("卡片 B：类型匹配但 onWillAcceptWithDetails 返回 false ⇒ 进 rejectedData"
                    "（变红），松手算取消。"),
            caption("载荷 int：上游 `_getDragTargets` 的 `isExpectedDataType` 在命中测试阶段就把"
                    "目标筛掉 ⇒ 两列都是空的，投放区毫无反应（这就是 `data is T?` 的语义）。"),
            vgap(10.0),
            hrow(std::move(rowA)),
            vgap(20.0),
            sectionTitle("LongPressDraggable + axis"),
            caption("延时 300ms 才起拖（上游 DelayedMultiDrag）；axis=Horizontal 时"
                    "avatar 的 y 恒等于按下时的 y。延时未到之前移动照常冒泡，所以页面还能滚。"),
            vgap(10.0),
            hrow(std::move(rowB)),
            vgap(14.0),
            hrow({status, cui::make_shared<SizedBox>(12.0, 0.0), clear}),
        });
    }

private:
    static constexpr size_t kMaxDocked = 4;

    static std::string num(double v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }

    /// 落进投放区的载荷会留下来；超出上限就挤掉最早的一枚。
    static void dock(std::vector<std::string>& into, const std::string& label) {
        if (into.size() >= kMaxDocked)
            into.erase(into.begin());
        into.push_back(label);
    }

    // 四张源卡片的底色。胶囊按标签取同一份色，肉眼才对得上来源。
    static Color tintBlue() { return Color::fromRGB(0.20, 0.45, 0.90); }
    static Color tintGreen() { return Color::fromRGB(0.00, 0.62, 0.44); }
    static Color tintAmber() { return Color::fromRGB(0.85, 0.55, 0.05); }
    static Color tintRed() { return Color::fromRGB(0.85, 0.25, 0.22); }
    static Color tintFor(const std::string& label) {
        if (label == "卡片 A")
            return tintBlue();
        if (label == "长按卡片")
            return tintGreen();
        return tintAmber();
    }

    std::string status_ = "把左边的卡片拖到右边的投放区";
    int accepted_ = 0;
    std::vector<std::string> docked_;
    std::vector<std::string> docked2_;
};

inline std::unique_ptr<State> DragDropDemo::createState() {
    return std::make_unique<DragDropDemoState>();
}

inline SharedPtr<Widget> pageDragDrop() {
    return pageBody({
        sectionTitle("拖放 DragTarget / Draggable"),
        caption("上游 widgets/drag_target.dart：Draggable 起拖时把 feedback 挂进最近的 "
                "Overlay，_DragAvatar 每帧从根渲染对象做一次命中测试，按 enter/leave 差分"
                "通知沿途的 DragTarget。"),
        caption("本仓没有 GestureArena：按下即拖（ImmediateMultiDrag），长按版由 Ticker "
                "延时（DelayedMultiDrag）；已起拖的那个 pointer 不再向上冒泡，等价于内层"
                "识别器赢掉外层滚动。"),
        vgap(12.0),
        cui::make_shared<DragDropDemo>(),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：Hero 共享元素动画（story 的 Demos / Hero Animation）
// ══════════════════════════════════════════════════════════════════════════════

/// 蓝色圆角方块：Hero 的 child。两端尺寸不同，飞行中由 shuttle 在两端矩形间补间。
inline SharedPtr<Widget> heroAvatar(double size, double radius) {
    auto dot = cui::make_shared<DecoratedBox>(
            BoxDecoration{Color(0.25, 0.55, 1.0, 1.0), radius},
            cui::make_shared<Center>(cui::make_shared<Text>(
                    "A", TextStyle().size(size * 0.34).bold().fgColor(Color(1.0, 1.0, 1.0, 1.0)))));
    return cui::make_shared<SizedBox>(size, size, dot);
}

/// Hero 演示：页内自带一个 Navigator（壳的页面宿主是 ContentRoute，不是
/// PageRoute，HeroController 只认 PageRoute 之间切换），HeroController 作为
/// 它的 observer —— 页内两条整屏路由之间就能看到 shuttle 飞行。
class HeroDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "HeroDemo"; }
};

class HeroDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        if (!nav_) {
            auto* self = this;
            nav_ = cui::make_shared<Navigator>(std::vector<SharedPtr<Page>>{}, navKey_);
            nav_->setObservers({&heroController_});
            nav_->setOnGenerateRoute([self](const RouteSettings& settings) -> SharedPtr<Route> {
                return settings.name() == "hero-detail" ? self->detailRoute()
                                                        : self->sourceRoute();
            });
        }
        return nav_;
    }

private:
    NavigatorState* navigatorState() {
        Element* el = navKey_->currentElement<Element>();
        auto* se = dynamic_cast<StatefulElement*>(el);
        return se ? dynamic_cast<NavigatorState*>(se->state()) : nullptr;
    }

    static SharedPtr<Widget> fadeTransitions(BuildContext&, SharedPtr<Animation<double>> animation,
                                             SharedPtr<Animation<double>>,
                                             SharedPtr<Widget> child) {
        return cui::make_shared<FadeTransition>(animation, child);
    }

    SharedPtr<Route> sourceRoute() {
        auto* self = this;
        return cui::make_shared<PageRouteBuilder>(
                [self](BuildContext&, SharedPtr<Animation<double>>,
                       SharedPtr<Animation<double>>) { return self->sourcePage(); },
                fadeTransitions, 350.0);
    }

    SharedPtr<Route> detailRoute() {
        auto* self = this;
        return cui::make_shared<PageRouteBuilder>(
                [self](BuildContext&, SharedPtr<Animation<double>>,
                       SharedPtr<Animation<double>>) { return self->detailPage(); },
                fadeTransitions, 350.0);
    }

    /// 页内页面外框：白底 + 留白（内层 Navigator 的页面被 Overlay 拉满，
    /// 靠这里的 padding 与内容列控制排版）。
    SharedPtr<Widget> framed(std::vector<SharedPtr<Widget>> kids) {
        auto col = vcol(std::move(kids));
        auto c = cui::make_shared<Container>(col);
        c->padding(EdgeInsets::all(24.0));
        return c;
    }

    SharedPtr<Widget> sourcePage() {
        auto* self = this;
        auto avatar = cui::make_shared<Hero>("story-avatar", heroAvatar(64.0, 14.0));
        std::vector<SharedPtr<Widget>> textKids;
        textKids.push_back(cui::make_shared<Text>("Alice", TextStyle().size(15.0).bold()));
        textKids.push_back(caption("点「打开详情页」，同一 tag 的 Hero 会飞出去"));
        std::vector<SharedPtr<Widget>> rowKids;
        rowKids.push_back(avatar);
        rowKids.push_back(cui::make_shared<SizedBox>(14.0, 0.0));
        rowKids.push_back(vcol(std::move(textKids)));
        auto card = cui::make_shared<Container>(hrow(std::move(rowKids)));
        card->padding(EdgeInsets::all(12.0));
        card->decoration(BoxDecoration{Color::fromRGBA(0.25, 0.55, 1.0, 0.08), 10.0});

        return framed({
            cui::make_shared<Text>("Source page", TextStyle().size(16.0).bold()),
            vgap(6.0),
            caption("列表页里的小头像。Hero 用同一个 tag 在两个路由里各注册一次，"
                    "HeroController 在切页帧末把两端矩形交给 Overlay 顶层的 shuttle 条目补间。"),
            vgap(18.0),
            card,
            vgap(18.0),
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("打开详情页"),
                                             [self] { self->openDetail(); }),
        });
    }

    SharedPtr<Widget> detailPage() {
        auto* self = this;
        return framed({
            cui::make_shared<Text>("Detail page", TextStyle().size(16.0).bold()),
            vgap(6.0),
            caption("同一个 tag 的 Hero 在这里放大到 160×160；返回时反向飞回列表。"
                    "飞行途中再点返回会 divert（同一条飞行换车），不会重开一条。"),
            vgap(18.0),
            cui::make_shared<Hero>("story-avatar", heroAvatar(160.0, 28.0)),
            vgap(18.0),
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("返回列表"),
                                             [self] { self->popDetail(); }),
        });
    }

    void openDetail() {
        if (auto* ns = navigatorState())
            ns->push(detailRoute());
    }

    void popDetail() {
        if (auto* ns = navigatorState())
            ns->pop();
    }

    // 控制器必须先于 Navigator 构造、后于它销毁（hero.h 偏离 6）：
    // 成员销毁是声明序的逆序，所以把 controller 写在前面。
    HeroController heroController_;
    SharedPtr<GlobalKey> navKey_ = cui::make_shared<GlobalKey>("heroDemoNav");
    SharedPtr<Navigator> nav_;
};

inline std::unique_ptr<State> HeroDemo::createState() {
    return std::make_unique<HeroDemoState>();
}

inline SharedPtr<Widget> pageHero() {
    return pageBody({
        sectionTitle("Hero 共享元素动画"),
        caption("上游 widgets/heroes.dart 的 Hero / HeroController / HeroMode：两个整屏路由"
                "（PageRoute）里有同 tag 的 Hero 时，HeroController 在切页帧末起飞 —— 取出两端"
                "hero 的全局矩形，在 Overlay 顶层插一条 shuttle 条目做矩形补间（曲线取 toHero 的 "
                "curve），两端的 Hero 就地换成占位。"),
        caption("本页内嵌一个 Navigator（页面宿主不是 PageRoute），HeroController 挂成它的 "
                "observer。点按钮打开/返回看蓝色方块跨页飞行；退场路由 dispose 后飞行由完成"
                "回调收尾（hero.h 偏离 3：本仓没有 weak_ptr，靠共享 token 兜底）。"),
        vgap(12.0),
        cui::make_shared<SizedBox>(1e18, 440.0, cui::make_shared<HeroDemo>()),
        vgap(40.0),
    });
}

// ── Advanced / AutoHideToolbar ─────────────────────────────────────────────
// 对齐 story 的 pages_auto_hide_toolbar（story 里也归在 Advanced 末尾）：贴顶悬浮的
// 「远程桌面」连接栏。
// 收缩态只剩 2 像素的边线（外加 16 像素的隐形悬停容差），鼠标移到顶边自动展开，
// 离开 2 秒后收回；右上角的 pin 可钉住不放。工具栏是纯复合控件，展开用的是上游
// SizeTransition 那棵树（ClipRect → Align(因子) → 内容），所以「被裁掉的那一段」
// 同时也点不到 —— 靠每条盒子的尺寸闸门（RenderBox::hitTest）。

class AutoHideToolbarDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "AutoHideToolbarDemo"; }
};

class AutoHideToolbarDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto bar = cui::make_shared<AutoHideToolbar>(
            buttonRow(), AutoHideDirection::Top, /*expandedSize=*/56.0,
            /*peekSize=*/2.0, /*hideDelayMs=*/2000.0, /*pinnable=*/true,
            theme().surface,
            [this](bool pinned) {
                status_ = pinned ? "工具栏已钉住" : "工具栏已取消钉住，移开 2 秒后收回";
                setState();
            });
        // 居中自然宽：Row(MainAxisSize::Max) 给非 flex 子节点**无界主轴约束**
        // （render_flex.cpp 的 kInfinity），条带内部的逐帧 Align 于是收缩到内容宽，
        // 再由 MainAxisAlignment::Center 摆到页面中间；Positioned 只负责贴顶。
        std::vector<SharedPtr<Widget>> barRow{std::move(bar)};
        auto centered = cui::make_shared<Row>(std::move(barRow));
        centered->setMainAxisAlignment(MainAxisAlignment::Center);
        auto pos = cui::make_shared<Positioned>(std::move(centered));
        pos->hasTop = pos->hasLeft = pos->hasRight = true;

        std::vector<SharedPtr<Widget>> layers{desktop(), std::move(pos)};
        auto stack = cui::make_shared<Stack>(std::move(layers));
        // 页壳把页面放进竖向 SingleChildScrollView（主轴无限），所以这里给 Stack
        // 一个确定的高度：工具栏的条带交叉轴 = Stack 给的宽度，主轴由补间逐帧决定。
        return cui::make_shared<SizedBox>(1e18, 460.0, std::move(stack));
    }

private:
    /// 一行 Ghost 按钮 + 竖分隔条（story 的 `Row(4.0, 4.0)`，本仓 Row 无 spacing，
    /// 间距用交叉轴 0 的 SizedBox 占位）。
    SharedPtr<Widget> buttonRow() {
        static const char* kLabels[] = {"连接", "传输", "关闭", "全屏", "截图", "剪贴"};
        std::vector<SharedPtr<Widget>> kids;
        for (size_t i = 0; i < sizeof(kLabels) / sizeof(kLabels[0]); ++i) {
            if (i > 0 && (i == 1 || i == 3 || i == 5))
                kids.push_back(separator());
            else if (i > 0)
                kids.push_back(gap());
            const char* label = kLabels[i];
            kids.push_back(cui::make_shared<TextButton>(
                cui::make_shared<Text>(tr(label), TextStyle().size(13.0)),
                [this, label] {
                    status_ = std::string("点了「") + label + "」";
                    setState();
                }));
        }
        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        return cui::make_shared<Padding>(EdgeInsets::symmetric(8.0, 6.0), std::move(row));
    }

    SharedPtr<Widget> gap() { return cui::make_shared<SizedBox>(4.0, 0.0); }

    SharedPtr<Widget> separator() {
        auto line = cui::make_shared<Container>();
        line->width(1.0).height(18.0).decoration(BoxDecoration{
            theme().divider});
        // Align 在紧高度里居中那条 18 高的线；外层 SizedBox 只给 12 的水平槽位。
        auto center = cui::make_shared<Align>(0.0, 0.0, std::move(line));
        return cui::make_shared<SizedBox>(12.0, 0.0, std::move(center));
    }

    /// 底层「桌面」内容：跟随页面背景，只给一块比页面略深的圆角底板。
    SharedPtr<Widget> desktop() {
        std::vector<SharedPtr<Widget>> lines = {
            cui::make_shared<Text>(tr("远程桌面连接"), TextStyle().size(24.0).bold()),
            cui::make_shared<Text>(tr("鼠标移到页面顶部查看工具栏"),
                                   TextStyle().size(14.0)),
            cui::make_shared<Text>(tr("工具栏会在 2 秒后自动隐藏到顶部"),
                                   TextStyle().size(14.0)),
            cui::make_shared<Text>(tr("收缩时只显示 2 像素的边缘线（吸附效果）"),
                                   TextStyle().size(14.0)),
            cui::make_shared<Text>(tr("悬停容差另有 16 像素，贴边即可展开"),
                                   TextStyle().size(14.0)),
            cui::make_shared<SizedBox>(0.0, 16.0),
            cui::make_shared<Text>(status_, TextStyle().size(13.0).fgColor(primaryColor())),
            cui::make_shared<SizedBox>(0.0, 16.0),
            caption("点右上角的 pin 把工具栏钉住：钉住期间离开顶边不收回，取消钉住后"
                    "重新计时。点按钮只改这一行文字，用来确认「被裁掉的按钮点不到、"
                    "露出来的才点得到」。"),
        };
        auto col = cui::make_shared<Column>(std::move(lines));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        // Align 有界松约束下撑满父级（本仓 Center 就是 RenderAlign）⇒ 底板铺满整块
        // 桌面，内边距随内容一起被它推着走。
        auto fill = cui::make_shared<Align>(-1.0, -1.0,
                                           cui::make_shared<Padding>(
                                               EdgeInsets::all(24.0), std::move(col)));
        auto bg = cui::make_shared<Container>(std::move(fill));
        bg->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.03), 10.0});
        return bg;
    }

    std::string status_ = "（还没点）";
};

inline std::unique_ptr<State> AutoHideToolbarDemo::createState() {
    return std::make_unique<AutoHideToolbarDemoState>();
}

inline SharedPtr<Widget> pageAutoHideToolbar() {
    return pageBody({
        sectionTitle("AutoHideToolbar 自动隐藏工具栏"),
        caption("来自 rui 的 story 示例页，重建成 Widget/Element/RenderObject 三层的"
                "纯复合控件：MouseRegion 收 enter/exit、Ticker 走 2 秒延迟、逐帧显露"
                "用上游 SizeTransition 的 ClipRect → Align(因子) → 内容。"),
        vgap(12.0),
        cui::make_shared<AutoHideToolbarDemo>(),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：系统服务（平台通道 / 权限 / 相机 / 蓝牙）
// ══════════════════════════════════════════════════════════════════════════════

inline const char* lensFacingName(services::CameraLensFacing facing) {
    switch (facing) {
        case services::CameraLensFacing::Back: return "back";
        case services::CameraLensFacing::Front: return "front";
        case services::CameraLensFacing::External: return "external";
        case services::CameraLensFacing::Unknown: break;
    }
    return "unknown";
}

class SystemServicesDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SystemServicesDemo"; }
};

class SystemServicesDemoState : public State {
public:
    void initState() override {
        State::initState();
        alive_ = std::make_shared<bool>(true);
        // 相机事件由 pumpPlatformMessages()（UI 线程、帧首）送进来。State 卸载
        // 后服务成员还在（成员在 ~State 之后析构），回调里的 this 必须自保。
        auto alive = alive_;
        lifecycleHandle_ = services::addAppLifecycleObserver(
            [this, alive](services::AppLifecycleState state) {
                if (!*alive)
                    return;
                lifecycleStatus_ = std::string("生命周期：") +
                                   services::appLifecycleStateName(state);
                setState();
            });
        camera_.setEventListener([this, alive](const services::CameraEvent& event) {
            if (!*alive)
                return;
            switch (event.type) {
                case services::CameraEventType::StateChanged:
                    cameraStatus_ = std::string("相机：") +
                                    services::cameraStateName(event.state);
                    break;
                case services::CameraEventType::Error:
                    cameraStatus_ = "相机错误：" + event.message;
                    break;
                case services::CameraEventType::PictureSaved:
                    cameraStatus_ = "已保存：" + event.message;
                    break;
                case services::CameraEventType::PreviewFrame:
                    return;  // 预览帧不进文字状态
            }
            setState();
        });
        refreshAll();
    }

    void dispose() override {
        if (alive_)
            *alive_ = false;
        services::removeAppLifecycleObserver(lifecycleHandle_);
        if (backHandle_ != 0)
            services::SystemNavigator::removeBackHandler(backHandle_);
        camera_.setEventListener(nullptr);
        bluetooth_.stopScan();
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(sectionTitle("系统服务（平台通道）"));
        kids.push_back(caption(
            "MethodChannel / EventChannel 对齐 Flutter；Android 宿主用 JNI 接相机"
            "（Camera2）、蓝牙（BLE + GATT）、权限和剪贴板。桌面没有宿主 ⇒ 如实显示未连接。"));
        kids.push_back(vgap(16.0));
        kids.push_back(channelCard());
        kids.push_back(permissionCard());
        kids.push_back(cameraCard());
        kids.push_back(bluetoothCard());
        kids.push_back(mobilePlatformCard());
        kids.push_back(vgap(24.0));
        return pageBody(std::move(kids));
    }

private:
    SharedPtr<Widget> card(const std::string& title,
                           std::vector<SharedPtr<Widget>> rows) {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Text>(title, TextStyle().size(14.0).bold()));
        kids.push_back(vgap(8.0));
        for (auto& row : rows)
            kids.push_back(std::move(row));
        auto col = vcol(std::move(kids));
        auto box = cui::make_shared<Container>(std::move(col));
        box->padding(EdgeInsets::all(14.0));
        box->margin(EdgeInsets(0.0, 0.0, 0.0, 12.0));
        box->decoration(BoxDecoration{Color::fromRGBA(0.0, 0.0, 0.0, 0.035), 10.0});
        return box;
    }

    SharedPtr<Widget> statusLine(const std::string& text) {
        TextStyle style;
        style.size(13.0).fgColor(primaryColor());
        return cui::make_shared<Text>(text, style);
    }

    SharedPtr<Widget> buttonRow(std::vector<SharedPtr<Widget>> kids) {
        std::vector<SharedPtr<Widget>> spaced;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (i > 0)
                spaced.push_back(cui::make_shared<SizedBox>(10.0, 0.0));
            spaced.push_back(std::move(kids[i]));
        }
        return hrow(std::move(spaced));
    }

    SharedPtr<Widget> channelCard() {
        return card("① 通道层 MethodChannel", {
            statusLine(channelStatus_),
            vgap(8.0),
            buttonRow({cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("Ping 宿主"),
                [this] {
                    auto reply = services::invokeMethod("cui_core/platform", "ping");
                    channelStatus_ = reply ? "通道已连接（cui_core/platform ping → " + *reply + "）"
                                           : "通道未连接：没有原生宿主（桌面 / 纯 C++ 构建）";
                    setState();
                })}),
        });
    }

    SharedPtr<Widget> permissionCard() {
        return card("② 权限（异步 requestPermission）", {
            statusLine(cameraPerm_),
            statusLine(btPerm_),
            vgap(8.0),
            buttonRow({
                cui::make_shared<ElevatedButton>(
                    cui::make_shared<Text>("请求相机权限"),
                    [this] { request(services::Permission::Camera, true); }),
                cui::make_shared<ElevatedButton>(
                    cui::make_shared<Text>("请求蓝牙扫描权限"),
                    [this] { request(services::Permission::BluetoothScan, false); }),
            }),
            vgap(6.0),
            caption("桌面无运行时权限框架 ⇒ 立即 granted；Android 先回 pending，"
                    "onRequestPermissionsResult 之后再交给回调。"),
        });
    }

    void request(services::Permission permission, bool cameraPermission) {
        auto& target = cameraPermission ? cameraPerm_ : btPerm_;
        const std::string name = services::permissionCode(permission);
        target = name + "：等待系统对话框…";
        setState();
        auto alive = alive_;
        services::requestPermission(
            permission, [this, alive, name, cameraPermission](services::PermissionStatus s) {
                if (!*alive)
                    return;
                (cameraPermission ? cameraPerm_ : btPerm_) =
                    name + "：" + services::permissionStatusName(s);
                setState();
            });
    }

    SharedPtr<Widget> cameraCard() {
        std::vector<SharedPtr<Widget>> rows;
        rows.push_back(statusLine(cameraStatus_));
        if (cameras_.empty()) {
            rows.push_back(caption("availableCameras() 返回空：桌面无原生宿主，或设备没有相机。"));
        } else {
            for (const auto& cam : cameras_) {
                rows.push_back(caption("相机 " + std::to_string(cam.id) + " · " +
                                       (cam.name.empty() ? "未命名" : cam.name) + " · " +
                                       lensFacingName(cam.facing) + " · sensor " +
                                       std::to_string(cam.sensorOrientation) + "°"));
            }
        }
        rows.push_back(vgap(8.0));
        rows.push_back(buttonRow({
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("刷新列表"),
                                             [this] { refreshAll(); }),
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("打开 0 号"),
                                             [this] { camera_.open(0); }),
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("关闭"),
                                             [this] { camera_.close(); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("拍照"),
                [this] {
                    if (!camera_.takePicture("cui_shot.jpg"))
                        setState();  // takePicture 失败时事件已同步发出；兜底刷一帧
                }),
        }));
        rows.push_back(vgap(6.0));
        rows.push_back(caption("拍照路径由宿主决定（建议应用私有目录的绝对路径）；结果经 "
                               "saved 事件回来。"));
        return card("③ 相机 CameraController", std::move(rows));
    }

    SharedPtr<Widget> bluetoothCard() {
        std::vector<SharedPtr<Widget>> rows;
        rows.push_back(statusLine(
            std::string("适配器：") + services::bluetoothAdapterStateName(btState_) +
            (btAvailable_ ? "（可用）" : "（不可用）")));
        rows.push_back(caption(std::string("扫描：") +
                               (bluetooth_.isScanning() ? "进行中" : "未开始") +
                               (scanMessage_.empty() ? "" : " · " + scanMessage_)));
        if (!bonded_.empty()) {
            rows.push_back(vgap(4.0));
            rows.push_back(caption("已配对设备："));
            for (const auto& d : bonded_)
                rows.push_back(caption("  " + (d.name.empty() ? d.id : d.name) + "  " + d.id));
        }
        if (!devices_.empty()) {
            rows.push_back(vgap(4.0));
            rows.push_back(caption("扫描结果："));
            for (const auto& d : devices_) {
                rows.push_back(hrow({
                    caption("  " + (d.name.empty() ? "(未命名)" : d.name) + "  " + d.id +
                            "  " + std::to_string(d.rssi) + " dBm"),
                    cui::make_shared<SizedBox>(8.0, 0.0),
                    cui::make_shared<TextButton>(
                        cui::make_shared<Text>("连接"),
                        [this, id = d.id] {
                            auto alive = alive_;
                            bluetooth_.connect(
                                id, [this, alive](const std::string& deviceId,
                                                  services::BluetoothConnectionState s) {
                                    if (!*alive)
                                        return;
                                    connection_[deviceId] =
                                        (s == services::BluetoothConnectionState::Connected)
                                            ? "已连接"
                                            : (s == services::BluetoothConnectionState::Connecting
                                                   ? "连接中"
                                                   : "未连接");
                                    setState();
                                });
                            setState();
                        }),
                }));
            }
        }
        if (!connection_.empty()) {
            std::string line = "连接状态：";
            for (const auto& entry : connection_)
                line += entry.first + "=" + entry.second + "  ";
            rows.push_back(caption(line));
        }
        rows.push_back(vgap(8.0));
        rows.push_back(buttonRow({
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("刷新"),
                                             [this] { refreshAll(); }),
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("开始扫描"),
                                             [this] { startScan(); }),
            cui::make_shared<ElevatedButton>(cui::make_shared<Text>("停止扫描"),
                                             [this] {
                                                 bluetooth_.stopScan();
                                                 setState();
                                             }),
        }));
        return card("④ 蓝牙 BluetoothService（BLE）", std::move(rows));
    }

    SharedPtr<Widget> mobilePlatformCard() {
        std::vector<SharedPtr<Widget>> rows;
        rows.push_back(statusLine(lifecycleStatus_));
        rows.push_back(statusLine(chromeStatus_));
        rows.push_back(statusLine(backStatus_));
        rows.push_back(vgap(8.0));
        rows.push_back(buttonRow({
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("锁定竖屏"),
                [this] { reportChrome("setPreferredOrientations", services::SystemChrome::
                    setPreferredOrientations({services::DeviceOrientation::PortraitUp})); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("恢复旋转"),
                [this] { reportChrome("setPreferredOrientations", services::SystemChrome::
                    setPreferredOrientations({})); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("沉浸式"),
                [this] { reportChrome("setEnabledSystemUIMode", services::SystemChrome::
                    setEnabledSystemUIMode(services::SystemUiMode::ImmersiveSticky)); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("边到边"),
                [this] { reportChrome("setEnabledSystemUIMode", services::SystemChrome::
                    setEnabledSystemUIMode(services::SystemUiMode::EdgeToEdge)); }),
        }));
        rows.push_back(vgap(8.0));
        rows.push_back(buttonRow({
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("浅色状态栏"),
                [this] { reportChrome("setSystemUIOverlayStyle", services::SystemChrome::
                    setSystemUIOverlayStyle(services::SystemUiOverlayStyle::dark)); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("触感：轻"),
                [this] { reportChrome("lightImpact", services::HapticFeedback::lightImpact()); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("触感：重"),
                [this] { reportChrome("heavyImpact", services::HapticFeedback::heavyImpact()); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("提示音"),
                [this] { reportChrome("click", services::SystemSound::playClick()); }),
        }));
        rows.push_back(vgap(8.0));
        rows.push_back(buttonRow({
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("注册返回键处理器"),
                [this] { toggleBackHandler(); }),
            cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("模拟返回键"),
                [this] {
                    const bool consumed = services::SystemNavigator::handleBack();
                    backStatus_ = consumed ? "返回键：被页面处理器消费"
                                           : "返回键：无人消费（引擎会退出根路由）";
                    setState();
                }),
        }));
        rows.push_back(vgap(6.0));
        rows.push_back(caption(
            "生命周期由宿主在 onResume/onPause/onStop 推 cui_core/lifecycle；桌面恒为 "
            "detached。返回键：宿主把 back 交给引擎，处理器按后注册优先的顺序尝试，"
            "都返回 false 才由引擎 pop。"));
        return card("⑤ 移动平台（生命周期 / SystemChrome / 触感 / 返回键）", std::move(rows));
    }

    void reportChrome(const std::string& what, bool ok) {
        chromeStatus_ = what + (ok ? "：已下发宿主" : "：宿主不可用（桌面）");
        setState();
    }

    void toggleBackHandler() {
        if (backHandle_ != 0) {
            services::SystemNavigator::removeBackHandler(backHandle_);
            backHandle_ = 0;
            backStatus_ = "返回键：已注销处理器";
            setState();
            return;
        }
        auto alive = alive_;
        backHandle_ = services::SystemNavigator::addBackHandler([this, alive] {
            if (!*alive)
                return false;
            ++backCount_;
            backStatus_ = "返回键：处理器第 " + std::to_string(backCount_) + " 次消费";
            setState();
            return true;
        });
        backStatus_ = "返回键：已注册处理器（模拟返回键可验证）";
        setState();
    }

    void refreshAll() {
        auto ping = services::invokeMethod("cui_core/platform", "ping");
        channelStatus_ = ping ? "通道已连接（cui_core/platform ping → " + *ping + "）"
                              : "通道未连接：没有原生宿主（桌面 / 纯 C++ 构建）";
        cameraPerm_ = std::string("camera：") +
                      services::permissionStatusName(
                          services::checkPermission(services::Permission::Camera));
        btPerm_ = std::string("bluetoothScan：") +
                  services::permissionStatusName(
                      services::checkPermission(services::Permission::BluetoothScan));
        cameras_ = services::CameraController::availableCameras();
        btAvailable_ = bluetooth_.isAvailable();
        btState_ = bluetooth_.adapterState();
        bonded_ = bluetooth_.bondedDevices();
        setState();
    }

    void startScan() {
        devices_.clear();
        auto alive = alive_;
        const bool started = bluetooth_.startScan(
            [this, alive](const services::BluetoothDevice& device) {
                if (!*alive)
                    return;
                for (auto& d : devices_) {
                    if (d.id == device.id) {
                        d = device;
                        setState();
                        return;
                    }
                }
                if (devices_.size() >= 12)
                    devices_.erase(devices_.begin());
                devices_.push_back(device);
                setState();
            },
            [this, alive](services::BluetoothAdapterState s) {
                if (!*alive)
                    return;
                btState_ = s;
                setState();
            });
        scanMessage_ = started ? "扫描中…" : "无法开始扫描：宿主不可用或缺少蓝牙权限";
        setState();
    }

    services::CameraController camera_;
    services::BluetoothService bluetooth_;
    std::shared_ptr<bool> alive_;

    std::string channelStatus_ = "通道：未知";
    std::string cameraPerm_ = "camera：未知";
    std::string btPerm_ = "bluetoothScan：未知";
    std::string cameraStatus_ = "相机：未打开";
    std::string scanMessage_;

    std::string lifecycleStatus_ =
        std::string("生命周期：") + services::appLifecycleStateName(services::appLifecycleState());
    std::string chromeStatus_ = "系统 UI：未下发";
    std::string backStatus_ = "返回键：无处理器";
    int lifecycleHandle_ = 0;
    int backHandle_ = 0;
    int backCount_ = 0;

    std::vector<services::CameraInfo> cameras_;
    bool btAvailable_ = false;
    services::BluetoothAdapterState btState_ = services::BluetoothAdapterState::Unknown;
    std::vector<services::BluetoothDevice> bonded_;
    std::vector<services::BluetoothDevice> devices_;
    std::map<std::string, std::string> connection_;
};

inline std::unique_ptr<State> SystemServicesDemo::createState() {
    return std::make_unique<SystemServicesDemoState>();
}

inline SharedPtr<Widget> pageSystemServices() {
    return cui::make_shared<SystemServicesDemo>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：图像（extended_image / photo_view / interactive_viewer 那一族）
// ══════════════════════════════════════════════════════════════════════════════

namespace image_studio_detail {

/// 演示素材：一张 400×260 的离线位图，先编码成 PNG 字节，之后所有控件都走
/// 「取字节 → 解码 → 缓存」这条真链路（内存 Provider 只是把网络换成内存）。
struct Stock {
    std::vector<uint8_t> bytes;  ///< PNG 编码字节
    DecodedImagePtr image;       ///< 已解码结果，给编辑器/看图直用
};

inline sk_sp<SkImage> paintStockBitmap() {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 260));
    if (!surface)
        return nullptr;
    Painter p(surface->getCanvas());
    // 四象限不同色 ⇒ 旋转/翻转后一眼能看出姿态变了。
    const Color quad[4] = {Color(0.90, 0.94, 1.00, 1.0), Color(0.98, 0.86, 0.60, 1.0),
                           Color(0.72, 0.92, 0.80, 1.0), Color(0.96, 0.74, 0.78, 1.0)};
    const Rect part[4] = {Rect(0.0, 0.0, 200.0, 130.0), Rect(200.0, 0.0, 200.0, 130.0),
                          Rect(0.0, 130.0, 200.0, 130.0), Rect(200.0, 130.0, 200.0, 130.0)};
    for (int i = 0; i < 4; ++i) {
        p.setSourceColor(quad[i]);
        p.rectangle(part[i]);
        p.fill();
    }
    // 左上角的白块 = 「原点记号」，配合对角线判方向。
    p.setSourceColor(Color(1.0, 1.0, 1.0, 1.0));
    p.rectangle(Rect(12.0, 12.0, 40.0, 24.0));
    p.fill();
    p.setSourceColor(Color(0.13, 0.13, 0.15, 1.0));
    p.setLineWidth(3.0);
    p.moveTo(14.0, 18.0);
    p.lineTo(386.0, 244.0);
    p.stroke();
    // 圆心偏离四象限交界 ⇒ 翻转后圆心会跑到另一侧。
    p.circle(200.0, 130.0, 56.0);
    p.stroke();
    return surface->makeImageSnapshot();
}

inline const Stock& stock() {
    static const Stock kStock = [] {
        Stock s;
        const sk_sp<SkImage> bitmap = paintStockBitmap();
        if (!bitmap)
            return s;
        const sk_sp<SkData> data = encodePng(bitmap);
        if (data) {
            const auto* raw = static_cast<const uint8_t*>(data->data());
            s.bytes.assign(raw, raw + data->size());
            s.image = decodeImage(s.bytes);
        }
        return s;
    }();
    return kStock;
}

/// 把渲染对象直接产出的位图包成 DecodedImage，省掉「再编码再解码」一趟往返。
inline DecodedImagePtr wrapDecoded(const sk_sp<SkImage>& bitmap) {
    if (!bitmap)
        return nullptr;
    auto decoded = std::make_shared<DecodedImage>();
    decoded->frames.push_back(ImageFrame{bitmap, 0});
    decoded->format = "decoded";
    return decoded;
}

/// 导出目录：Windows 用 %TEMP%，POSIX 用 %TMPDIR%/$TMPDIR，都没有就 /tmp。
inline std::string exportPath(const char* tag) {
    const char* dir = nullptr;
#if defined(_WIN32)
    dir = std::getenv("TEMP");
#else
    dir = std::getenv("TMPDIR");
    if (!dir || !*dir)
        dir = std::getenv("TMP");
#endif
    std::string out = (dir && *dir) ? dir : "/tmp";
    if (out.back() != '/' && out.back() != '\\')
        out += '/';
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return out + "cui_core-" + tag + "-" + std::to_string(static_cast<long long>(ticks % 1000000)) +
           ".png";
}

}  // namespace image_studio_detail

using namespace image_studio_detail;  // 只在 story 内部这一个小节用

class ImageStudioDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ImageStudioDemo"; }
};

class ImageStudioDemoState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        return pageBody({
            cui::make_shared<Text>("图像", TextStyle().size(18.0).bold()),
            vgap(6.0),
            caption("对齐 extended_image / photo_view / interactive_viewer：占位与失败兜底、"
                    "内存与磁盘缓存、缩放平移、全屏看图（下滑退出）、裁剪旋转翻转、画笔涂抹。"),
            vgap(16.0),
            sectionTitle("① ExtendedImage：占位 / 失败 / 缓存"),
            extendedSection(),
            vgap(20.0),
            sectionTitle("② InteractiveViewer：滚轮缩放 + 拖拽平移"),
            viewerSection(),
            vgap(20.0),
            sectionTitle("③ PhotoView：双击换挡 / 下滑退出"),
            photoSection(),
            vgap(20.0),
            sectionTitle("④ ImageEditor：拖框裁剪 + 旋转翻转"),
            editorSection(),
            vgap(20.0),
            sectionTitle("⑤ ImagePainterLayer：在图上有层笔迹"),
            painterSection(),
        });
    }

private:
    // ── 小工具 ──────────────────────────────────────────────────────────────

    /// 一行按钮，按钮之间留 8 像素。
    static SharedPtr<Widget> buttonRow(std::vector<SharedPtr<Widget>> kids) {
        std::vector<SharedPtr<Widget>> out;
        for (auto& kid : kids) {
            if (!out.empty())
                out.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
            out.push_back(std::move(kid));
        }
        return hrow(std::move(out));
    }

    static SharedPtr<Widget> loadingPlaceholder(BuildContext&, double progress) {
        const std::string label = progress < 0.0
                                      ? "加载中…"
                                      : "加载中 " + std::to_string(int(progress * 100.0 + 0.5)) + "%";
        return cui::make_shared<Center>(
            cui::make_shared<Text>(label, TextStyle().size(12.0).fgColor(captionColor())));
    }

    static SharedPtr<Widget> errorPlaceholder(BuildContext&, const std::string& error) {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Text>("加载失败",
                                              TextStyle().size(12.0).fgColor(Color(0.85, 0.2, 0.2, 1.0))));
        kids.push_back(vgap(4.0));
        kids.push_back(caption(error));
        auto col = cui::make_shared<Column>(std::move(kids));
        col->setMainAxisSize(MainAxisSize::Min);
        auto box = cui::make_shared<Container>(col);
        box->padding(EdgeInsets::symmetric(8.0, 8.0))
            .decoration(BoxDecoration{Color(0.85, 0.2, 0.2, 0.10), 6.0});
        return cui::make_shared<Center>(std::move(box));
    }

    /// 每帧现造的坏源：字节不是任何可识别的编码格式 ⇒ 解码器给回 error。
    std::shared_ptr<ImageProvider> makeProvider() {
        if (badSource_) {
            const std::vector<uint8_t> junk{'n', 'o', 't', ' ', 'a', 'n', ' ', 'p', 'n', 'g'};
            return std::make_shared<MemoryImageProvider>(junk, "story-broken");
        }
        std::string name = "story-stock";
        if (reloadNonce_ > 0)
            name += "#" + std::to_string(reloadNonce_);
        return std::make_shared<MemoryImageProvider>(stock().bytes, std::move(name));
    }

    // ── ① ExtendedImage ─────────────────────────────────────────────────────

    SharedPtr<Widget> extendedSection() {
        auto* self = this;
        auto image = cui::make_shared<ExtendedImage>(
            makeProvider(), BoxFit::Contain, Alignment::center(), 320.0, 208.0,
            std::nullopt, std::nullopt, true, true, true, true, true, -1,
            loadingPlaceholder, errorPlaceholder, ExtendedImage::FrameBuilder{},
            [self] {
                ++self->taps_;
                self->setState();
            });
        image->setOnReady([self](const ImageLoadResult& r) {
            if (!r.ok()) {
                self->lastLoad_ = "失败：" + r.error;
            } else {
                self->lastLoad_ = std::string(r.fromCache ? "命中解码缓存" : "现取现解") +
                                  "，来源 " + r.source + "，" + std::to_string(r.bytes) +
                                  " 字节 · " + std::to_string(r.image->width()) + "×" +
                                  std::to_string(r.image->height()) +
                                  (r.image->animated ? " · 动图" : "");
            }
            self->setState();
        });
        // onReady 只在成功时发，失败走 onError ⇒ 两条都接才看得到坏源的报错
        image->setOnError([self](const std::string& message) {
            self->lastLoad_ = "失败：" + message;
            self->setState();
        });

        std::vector<SharedPtr<Widget>> buttons;
        buttons.push_back(cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>(badSource_ ? "换回好源" : "换成坏源"), [self] {
                self->badSource_ = !self->badSource_;
                self->setState();
            }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("重新加载"),
            [self] {
                ++self->reloadNonce_;  // 换 key ⇒ 缓存必然 miss，走完整链路
                self->setState();
            }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("清空缓存"), [self] {
                DecodedImageCache::instance().clear();
                EncodedDataCache::instance().clear();
                ++self->reloadNonce_;
                self->setState();
            }));

        const std::string cacheLine =
            "解码缓存 " + std::to_string(DecodedImageCache::instance().count()) + " 条 · 编码缓存 " +
            std::to_string(EncodedDataCache::instance().bytes()) + " 字节 · 点击 " +
            std::to_string(taps_) + " 次";

        return vcol({
            caption("来源是内存里的 PNG 字节，仍然按「取字节 → 解码 → 入缓存」跑一遍；"
                    "loadingBuilder 出进度，errorBuilder 出兜底，gaplessPlayback 换源不闪白。"),
            vgap(8.0),
            cui::make_shared<SizedBox>(320.0, 208.0, image),
            vgap(8.0),
            buttonRow(std::move(buttons)),
            vgap(6.0),
            caption(loadInfoLine(lastLoad_)),
            caption(cacheLine),
        });
    }

    static std::string loadInfoLine(const std::string& s) {
        return s.empty() ? "尚未加载" : "上次加载：" + s;
    }

    // ── ② InteractiveViewer ─────────────────────────────────────────────────

    void zoomViewer(double factor) {
        const double current = viewerCtrl_->scale();
        if (current <= 0.0)
            return;
        const double next = clampDoubleValue(current * factor, 0.8, 4.0);
        if (next == current)
            return;
        viewerCtrl_->setValue(matrix4Scaled(viewerCtrl_->value(), next / current));
        setState();
    }

    SharedPtr<Widget> viewerSection() {
        auto* self = this;
        auto content = cui::make_shared<ExtendedImage>(
            std::make_shared<DecodedImageProvider>(stock().image, "story-viewer"),
            BoxFit::Cover);
        auto viewer = cui::make_shared<InteractiveViewer>(
            std::move(content), Clip::AntiAlias, PanAxis::Free, EdgeInsets::all(20.0), true,
            4.0, 0.8, true, true, 200.0, false, viewerCtrl_, std::nullopt,
            InteractionStartCallback{}, InteractionUpdateCallback{},
            [self](const InteractiveViewerEndDetails&) { self->setState(); });

        std::vector<SharedPtr<Widget>> buttons;
        buttons.push_back(cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("放大"), [self] { self->zoomViewer(1.4); }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("缩小"), [self] { self->zoomViewer(1.0 / 1.4); }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("复位"), [self] {
                self->viewerCtrl_->reset();
                self->setState();
            }));

        return vcol({
            caption("带 boundaryMargin 的 constrained 视口：内容比视口小时不许拖，"
                    "放大后才能平移，且平移会被夹回边界。"),
            vgap(8.0),
            cui::make_shared<SizedBox>(320.0, 208.0, std::move(viewer)),
            vgap(8.0),
            buttonRow(std::move(buttons)),
            vgap(6.0),
            caption("当前缩放：" + std::to_string(int(viewerCtrl_->scale() * 100.0 + 0.5)) +
                    "%　平移：" + std::to_string(int(viewerCtrl_->translation().dx)) + ", " +
                    std::to_string(int(viewerCtrl_->translation().dy))),
        });
    }

    // ── ③ PhotoView + 下滑退出 ──────────────────────────────────────────────

    SharedPtr<Widget> photoSection() {
        auto* self = this;
        auto box = PhotoView::decoded(stock().image, "story-photo-box");
        box->setOnTapUp(
            [self](const PhotoViewTapDetails&) {
                ++self->photoTaps_;
                self->setState();
            });

        auto fullScreenButton = cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("全屏看图（下滑退出）"), [self] {
                BuildContext* ctx = self->context();
                auto* ns = ctx ? NavigatorState::of(*ctx) : nullptr;
                if (!ns)
                    return;
                auto page = cui::make_shared<PhotoViewSlidePage>(
                    PhotoView::decoded(stock().image, "story-photo-full"));
                ns->push(makePhotoViewSlideRoute(std::move(page)));
            });

        return vcol({
            caption("PhotoView 自带 initial/covered/zoomedIn/zoomedOut 的换挡循环，"
                    "下滑页（PhotoViewSlidePage）拖过头就把整页淡出并 pop 掉所在 Navigator 的这条路由。"),
            vgap(8.0),
            cui::make_shared<SizedBox>(320.0, 208.0, std::move(box)),
            vgap(8.0),
            buttonRow({fullScreenButton}),
            vgap(6.0),
            caption("框内点击 " + std::to_string(photoTaps_) +
                    " 次；全屏页里双指捏合或双击可切档，向下拖过 25% 就退出。"),
        });
    }

    // ── ④ ImageEditor ───────────────────────────────────────────────────────

    ImageEditorState* editorState() {
        Element* el = editorKey_->currentElement<Element>();
        auto* se = dynamic_cast<StatefulElement*>(el);
        return se ? dynamic_cast<ImageEditorState*>(se->state()) : nullptr;
    }

    void exportEditor() {
        auto* st = editorState();
        if (!st)
            return;
        editorPreview_ = wrapDecoded(st->produceImage());
        ++editorPreviewNonce_;
        const std::string path = exportPath("editor");
        editorInfo_ = st->savePng(path) ? ("已存盘：" + path) : "存盘失败";
        setState();
    }

    SharedPtr<Widget> editorSection() {
        auto* self = this;
        ImageEditorConfig config;
        config.cropAspectRatio = editorRatio_;
        auto editor = cui::make_shared<ImageEditor>(
            std::make_shared<DecodedImageProvider>(stock().image, "story-editor"), config,
            nullptr, loadingPlaceholder, errorPlaceholder,
            ImageEditor::EditChangedCallback{}, editorKey_);

        auto ratioButton = [self](const char* label, std::optional<double> ratio) {
            return cui::make_shared<TextButton>(
                cui::make_shared<Text>(label), [self, ratio] {
                    self->editorRatio_ = ratio;
                    self->setState();
                });
        };

        std::vector<SharedPtr<Widget>> buttons;
        buttons.push_back(cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("右转 90°"), [self] {
                if (auto* st = self->editorState())
                    st->rotate(true);
            }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("左转"), [self] {
                if (auto* st = self->editorState())
                    st->rotate(false);
            }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("水平翻转"), [self] {
                if (auto* st = self->editorState())
                    st->flipHorizontal();
            }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("垂直翻转"), [self] {
                if (auto* st = self->editorState())
                    st->flipVertical();
            }));
        buttons.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("还原"), [self] {
                if (auto* st = self->editorState())
                    st->reset();
            }));

        std::vector<SharedPtr<Widget>> ratioRow;
        ratioRow.push_back(ratioButton("自由", std::nullopt));
        ratioRow.push_back(ratioButton("1:1", CropAspectRatios::kRatio1_1));
        ratioRow.push_back(ratioButton("4:3", CropAspectRatios::kRatio4_3));
        ratioRow.push_back(ratioButton("原图", CropAspectRatios::kOriginal));

        std::vector<SharedPtr<Widget>> exportRow;
        exportRow.push_back(cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("出图并存盘"), [self] { self->exportEditor(); }));

        SharedPtr<Widget> preview = caption("（还没出过图）");
        if (editorPreview_) {
            preview = cui::make_shared<SizedBox>(
                200.0, 130.0,
                cui::make_shared<ExtendedImage>(
                    std::make_shared<DecodedImageProvider>(
                        editorPreview_, "story-editor-out#" + std::to_string(editorPreviewNonce_)),
                    BoxFit::Contain, Alignment::center(), 200.0, 130.0));
        }

        return vcol({
            caption("拖框选裁剪区，出图顺序与上游一致：翻转 → 旋转 → 裁剪。换比例锁定会把已做的"
                    "编辑作废（上游同款），因为裁剪框要按新比例重铺。"),
            vgap(8.0),
            cui::make_shared<SizedBox>(320.0, 220.0, std::move(editor)),
            vgap(8.0),
            buttonRow(std::move(buttons)),
            vgap(6.0),
            buttonRow(std::move(ratioRow)),
            vgap(6.0),
            buttonRow(std::move(exportRow)),
            vgap(8.0),
            hrow({preview, cui::make_shared<SizedBox>(12.0, 0.0),
                  vcol({caption(editorInfo_.empty() ? "出图结果预览" : editorInfo_),
                        caption("当前比例锁定：" + ratioLabel(editorRatio_))})}),
        });
    }

    static std::string ratioLabel(const std::optional<double>& ratio) {
        if (!ratio)
            return "自由";
        if (*ratio == CropAspectRatios::kRatio1_1)
            return "1:1";
        if (*ratio == CropAspectRatios::kRatio4_3)
            return "4:3";
        if (*ratio <= 0.0)
            return "原图比例";
        return std::to_string(*ratio);
    }

    // ── ⑤ ImagePainterLayer ─────────────────────────────────────────────────

    ImagePainterLayerState* painterState() {
        Element* el = painterKey_->currentElement<Element>();
        auto* se = dynamic_cast<StatefulElement*>(el);
        return se ? dynamic_cast<ImagePainterLayerState*>(se->state()) : nullptr;
    }

    void exportPainter() {
        auto* st = painterState();
        const sk_sp<SkImage> base = stock().image ? stock().image->image() : nullptr;
        if (!st || !base)
            return;
        painterPreview_ = wrapDecoded(painterCtrl_->compose(base, st->layerSize()));
        ++painterPreviewNonce_;
        const std::string path = exportPath("painter");
        painterInfo_ =
            painterCtrl_->savePng(path, base, st->layerSize()) ? ("已存盘：" + path) : "存盘失败";
        setState();
    }

    SharedPtr<Widget> painterSection() {
        auto* self = this;
        auto content = cui::make_shared<ExtendedImage>(
            std::make_shared<DecodedImageProvider>(stock().image, "story-painter"), BoxFit::Cover);
        auto layer = cui::make_shared<ImagePainterLayer>(
            std::move(content), painterCtrl_, painterMode_, painterColor_, painterWidth_, 2.0,
            [self](const ImagePainterStroke&) { self->setState(); }, painterKey_);

        auto colorButton = [self](const char* label, const Color& color) {
            return cui::make_shared<TextButton>(
                cui::make_shared<Text>(label), [self, color] {
                    self->painterColor_ = color;
                    self->setState();
                });
        };
        auto widthButton = [self](const char* label, double width) {
            return cui::make_shared<TextButton>(
                cui::make_shared<Text>(label), [self, width] {
                    self->painterWidth_ = width;
                    self->setState();
                });
        };

        std::vector<SharedPtr<Widget>> modeRow;
        modeRow.push_back(cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>(painterMode_ == ImagePainterMode::Draw ? "画笔" : "橡皮"),
            [self] {
                self->painterMode_ = self->painterMode_ == ImagePainterMode::Draw
                                         ? ImagePainterMode::Eraser
                                         : ImagePainterMode::Draw;
                self->setState();
            }));
        modeRow.push_back(colorButton("红", Color(0.90, 0.15, 0.15, 1.0)));
        modeRow.push_back(colorButton("蓝", Color(0.10, 0.35, 0.85, 1.0)));
        modeRow.push_back(colorButton("墨", Color(0.10, 0.10, 0.12, 1.0)));
        modeRow.push_back(widthButton("细", 2.0));
        modeRow.push_back(widthButton("中", 6.0));
        modeRow.push_back(widthButton("粗", 14.0));

        std::vector<SharedPtr<Widget>> historyRow;
        historyRow.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("撤销"), [self] {
                if (self->painterCtrl_->undo())
                    self->setState();
            }));
        historyRow.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("重做"), [self] {
                if (self->painterCtrl_->redo())
                    self->setState();
            }));
        historyRow.push_back(cui::make_shared<TextButton>(
            cui::make_shared<Text>("清空"), [self] {
                if (self->painterCtrl_->clear())
                    self->setState();
            }));
        historyRow.push_back(cui::make_shared<ElevatedButton>(
            cui::make_shared<Text>("合成并存盘"), [self] { self->exportPainter(); }));

        SharedPtr<Widget> preview = caption("（还没合成过）");
        if (painterPreview_) {
            preview = cui::make_shared<SizedBox>(
                200.0, 130.0,
                cui::make_shared<ExtendedImage>(
                    std::make_shared<DecodedImageProvider>(
                        painterPreview_, "story-painter-out#" + std::to_string(painterPreviewNonce_)),
                    BoxFit::Contain, Alignment::center(), 200.0, 130.0));
        }

        return vcol({
            caption("笔迹是一条独立的覆盖层，只在图层坐标里记录点序列 + 落笔时的颜色/宽度/模式快照；"
                    "compose() 才把它烘进位图，橡皮擦掉的是笔迹层。"),
            vgap(8.0),
            cui::make_shared<SizedBox>(320.0, 208.0, std::move(layer)),
            vgap(8.0),
            buttonRow(std::move(modeRow)),
            vgap(6.0),
            buttonRow(std::move(historyRow)),
            vgap(8.0),
            hrow({preview, cui::make_shared<SizedBox>(12.0, 0.0),
                  vcol({caption(painterInfo_.empty() ? "合成结果预览" : painterInfo_),
                        caption("在层笔迹 " + std::to_string(painterCtrl_->strokeCount()) +
                                " 笔 · 可撤销 " +
                                std::to_string(painterCtrl_->historyCount()) +
                                " · 可重做 " + std::to_string(painterCtrl_->redoCount()))})}),
        });
    }

    // ── 状态 ────────────────────────────────────────────────────────────────

    bool badSource_ = false;
    int reloadNonce_ = 0;
    int taps_ = 0;
    std::string lastLoad_;

    TransformationControllerPtr viewerCtrl_ = std::make_shared<TransformationController>();
    int photoTaps_ = 0;

    SharedPtr<GlobalKey> editorKey_ = cui::make_shared<GlobalKey>("imageStudioEditor");
    std::optional<double> editorRatio_;
    DecodedImagePtr editorPreview_;
    int editorPreviewNonce_ = 0;
    std::string editorInfo_;

    std::shared_ptr<ImagePainterController> painterCtrl_ =
        std::make_shared<ImagePainterController>();
    SharedPtr<GlobalKey> painterKey_ = cui::make_shared<GlobalKey>("imageStudioPainter");
    ImagePainterMode painterMode_ = ImagePainterMode::Draw;
    Color painterColor_{0.90, 0.15, 0.15, 1.0};
    double painterWidth_ = 6.0;
    DecodedImagePtr painterPreview_;
    int painterPreviewNonce_ = 0;
    std::string painterInfo_;
};

inline std::unique_ptr<State> ImageStudioDemo::createState() {
    return std::make_unique<ImageStudioDemoState>();
}

inline SharedPtr<Widget> pageImageStudio() {
    return cui::make_shared<ImageStudioDemo>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：视频播放（widgets/video_player.h）
// ══════════════════════════════════════════════════════════════════════════════

class VideoPlayerDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "VideoPlayerDemo"; }
};

class VideoPlayerDemoState : public State {
public:
    void initState() override {
        // CUI_VIDEO_BACKEND=auto|ffmpeg-first|system|ffmpeg：在同时编进两条路线的
        // 机器上（Windows = Media Foundation + FFmpeg）各跑一遍，页面下方
        // 「解码后端：」那行与这句 stdout 都会报出实际生效的那一条。
        if (const char* pref = std::getenv("CUI_VIDEO_BACKEND"); pref && *pref) {
            const std::string name = pref;
            setVideoBackendPreference(name == "system"         ? VideoBackendPreference::SystemOnly
                                      : name == "ffmpeg"       ? VideoBackendPreference::FfmpegOnly
                                      : name == "ffmpeg-first" ? VideoBackendPreference::FfmpegFirst
                                                               : VideoBackendPreference::Auto);
            std::printf("[video] CUI_VIDEO_BACKEND=%s ⇒ %s\n", pref,
                        videoCapabilities().summary().c_str());
        }
        // 默认仍播运行时合成的 MJPEG AVI（开箱即有画面）；CUI_VIDEO 指向本地
        // 文件时优先用它。之后都可点「打开文件…」经原生对话框换成任意视频。
        if (const char* path = std::getenv("CUI_VIDEO"); path && *path)
            loadSource(VideoPlayerController::file(path), "文件 " + std::string(path));
        else
            loadSource(VideoPlayerController::memory(video_demo::demoVideoBytes(), "avi"),
                       "运行时生成的内存 MJPEG AVI（40 帧 / 10 fps / 4 秒）");
    }

    void dispose() override {
        stopHideTimer();
        unloadCurrent();
        State::dispose();
    }

    /// 起 / 换一个新的 controller：先摘掉旧的监听并 dispose，再订阅 + 初始化。
    /// 桌面原生对话框是阻塞调用，返回后在 UI 线程直接切源 + setState 即可。
    void loadSource(std::shared_ptr<VideoPlayerController> next, std::string label) {
        unloadCurrent();
        controller_ = std::move(next);
        sourceLabel_ = std::move(label);
        handle_ = controller_->addListenerWithHandle([this] {
            if (mounted())
                setState();
        });
        controller_->initialize();
        if (mounted())
            setState();
    }

    /// 摘监听 + dispose + 复位句柄，并把 controller_ 置空。幂等。
    void unloadCurrent() {
        if (handle_ != 0 && controller_) {
            controller_->removeListenerByHandle(handle_);
            handle_ = 0;
        }
        if (controller_) {
            controller_->dispose();
            controller_.reset();
        }
    }

    /// 弹系统原生文件对话框选一个视频并切换播放源；取消则什么都不做。
    void openVideoFile() {
        services::FileDialogOptions options;
        options.title = "选择视频文件";
        options.filters = {{"Videos",
                           {"mp4", "mkv", "mov", "avi", "webm", "m4v",
                            "flv", "wmv", "mpg", "mpeg", "3gp"}}};
        const std::optional<std::string> path = services::openFileDialog(options);
        if (!path || path->empty())
            return;  // 取消 / 平台不支持
        loadSource(VideoPlayerController::file(*path), "文件 " + *path);
    }

    SharedPtr<Widget> build(BuildContext&) override {
        const VideoPlayerValue v = controller_ ? controller_->value() : VideoPlayerValue();
        // 同步自动隐藏计时：就绪且未收起时保表在跑（暂停也照收，见 controlsShown）。
        syncHideTimer(v.isInitialized && !v.hasError());

        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Text>("视频播放", TextStyle().size(18.0).bold()));
        kids.push_back(vgap(6.0));
        kids.push_back(caption(
                "对齐 video_player：controller 状态机（initialize / play / pause / seekTo / "
                "setVolume / setPlaybackSpeed / setLooping）+ VideoPlayer 画面 + "
                "VideoControls 控制条（VCL 风格：seek 轨道 / 传输键 / 音量 / 快照）。"));
        kids.push_back(vgap(16.0));

        kids.push_back(sectionTitle("① 播放器"));
        // 控制条不再摆在画面外，而是叠在画面最底部一条 + 自动隐藏（见 stage()）。
        kids.push_back(stage(v));
        kids.push_back(vgap(10.0));
        kids.push_back(caption(
                "控制条贴在画面最底部：鼠标在画面上移动即淡入，静止约 2.6 秒自动淡出；"
                "点画面任意处 = 播放 / 暂停，同时把控制条叫出来（稍后同样隐去）；"
                "未就绪 / 出错时常显。收起时整条不吃点击，点画面不会误触传输键。"));
        kids.push_back(vgap(4.0));
        kids.push_back(caption(statusLine(v)));
        kids.push_back(vgap(18.0));

        kids.push_back(sectionTitle("② 来源与后端"));
        kids.push_back(hrow({
                cui::make_shared<ElevatedButton>(cui::make_shared<Text>("打开文件…"),
                                                 [this] { openVideoFile(); }),
                cui::make_shared<SizedBox>(10.0, 0.0),
                caption("弹系统原生文件对话框选一个本地视频（MP4/MKV/MOV/AVI/WebM…）"),
        }));
        kids.push_back(vgap(8.0));
        kids.push_back(caption("当前来源：" + sourceLabel_));
        kids.push_back(vgap(4.0));
        kids.push_back(caption("解码后端：" + videoCapabilities().summary()));
        kids.push_back(vgap(4.0));
        kids.push_back(caption(
                "环境变量 CUI_VIDEO=/path/to/video.mp4 可改默认源；进度条拖动时自动暂停、"
                "松手恢复（VideoScrubber），暂停中 seek 会补出一帧。"));
        return pageBody(std::move(kids));
    }

private:
    // ── 播放区：画面 + 贴在画面最底部的控制条（自动隐藏）─────────────

    /// 播放区：画面（未就绪时是占位底板）打底，控制条叠在**最底部**一条。
    ///
    /// 几何口径：
    ///   - ready 分支把 `Stack` 放进 `AspectRatio` 里 —— `RenderAspectRatio` 给子
    ///     节点**紧**约束（= 算出的画面尺寸），于是 Stack 尺寸恒等于画面，
    ///     `Positioned(left/right=0)` 拉出来的控制条不会比画面宽；
    ///   - 未 ready 没有画面可量，用占位底板撑出同样的播放区尺寸（Stack 自身
    ///     取非定位子的尺寸，定位子只贴一条），切到 ready 时控制条位置不跳；
    ///   - `ClipRRect` 包在 Stack 外面 ⇒ 控制条下缘随画面一起圆角。
    SharedPtr<Widget> stage(const VideoPlayerValue& v) {
        const bool ready = v.isInitialized && !v.hasError();

        // 画面层可点：点任意处 = toggle 播放 + 叫出控制条。控制条在 Stack 里是
        // 后画的（上层），命中测试倒序且「第一个命中就停」⇒ 落在条上的点击先被
        // 条吃掉，不会同时触发这里的 onTap（本仓无手势竞技场，靠的就是这条层序）。
        std::vector<SharedPtr<Widget>> layers;
        layers.push_back(cui::make_shared<GestureDetector>(
                ready ? framed(controller_) : placeholder(v), [this] { onStageTap(); }));
        layers.push_back(bottomControls(ready));
        // Fit::Expand：非定位子（画面）拿紧约束 = Stack 尺寸。默认的 Loose 会
        // 让 Container 缩回图片自然尺寸，画面就变成左上角一小块。
        SharedPtr<Widget> base = cui::make_shared<Stack>(
                std::move(layers), nullptr, RenderStack::Alignment::TopStart,
                RenderStack::Fit::Expand);
        if (ready)
            base = cui::make_shared<AspectRatio>(v.aspectRatio(), std::move(base));
        else
            // 未就绪时也给个 16:9 的空框：Fit::Expand 下没有 AspectRatio 会撑满
            // 约束上限（720×480），加载完成前后尺寸会跳一下。
            base = cui::make_shared<AspectRatio>(16.0 / 9.0, std::move(base));

        auto boxed = cui::make_shared<ConstrainedBox>(
                BoxConstraints(0.0, 720.0, 0.0, 480.0),
                cui::make_shared<ClipRRect>(8.0, std::move(base)));
        // MouseRegion 覆盖整块播放区：进入 / 移动即显形并重新计时（= 经典播放器）。
        return cui::make_shared<MouseRegion>(
                std::move(boxed),
                [this](const PointerEvent&) { revealControls(); },
                [this](const PointerEvent&) { onStageExit(); },
                [this](const PointerEvent&) { revealControls(); });
    }

    SharedPtr<Widget> framed(const std::shared_ptr<VideoPlayerController>& controller) {
        auto box = cui::make_shared<Container>(cui::make_shared<VideoPlayer>(controller));
        box->decoration(BoxDecoration{Color::fromRGB(0.04, 0.05, 0.08), 8.0});
        return box;
    }

    /// 未就绪 / 出错时的占位底板：保持深色播放区形状，提示文案居中（浅色字形）。
    SharedPtr<Widget> placeholder(const VideoPlayerValue& v) {
        const bool error = v.hasError();
        auto title = cui::make_shared<Text>(error ? "视频打开失败" : "正在打开视频…",
                                            TextStyle().size(14.0).bold().fgColor(
                                                    Color::fromRGBA(0.95, 0.96, 0.98, 1.0)));
        auto detail = cui::make_shared<Text>(
                error ? v.errorDescription.value_or("")
                      : "后台线程打开解码器；完成前画面是空的。",
                TextStyle().size(12.0).fgColor(Color::fromRGBA(0.85, 0.88, 0.92, 0.72)));
        auto box = cui::make_shared<Container>(
                cui::make_shared<Center>(vcol({title, vgap(6.0), detail})));
        box->decoration(BoxDecoration{Color::fromRGB(0.04, 0.05, 0.08), 8.0});
        return box;
    }

    /// 贴在底部的那一条：淡入淡出 + 收起时 `IgnorePointer` 不吃点击。
    SharedPtr<Widget> bottomControls(bool ready) {
        const bool show = controlsShown(ready);
        auto bar = cui::make_shared<VideoControls>(controller_, nullptr, /*overlay=*/true);
        auto fade = cui::make_shared<AnimatedOpacity>(show ? 1.0 : 0.0, std::move(bar), 220.0);
        auto pos = cui::make_shared<Positioned>(
                cui::make_shared<IgnorePointer>(!show, std::move(fade)));
        // left + right + bottom 全 0 ⇒ 横向拉满画面宽度（紧宽度）、竖向取自然高。
        // 切勿用 SizedBox(w, 0)：那是紧高 0，会穿透到控制条把命中区压成 0 高。
        pos->hasLeft = pos->hasRight = pos->hasBottom = true;
        return pos;
    }

    /// 收起只看「静止多久 / 鼠标是否离开」，与播放状态无关（暂停后放手不管
    /// 也会隐去，要再看只需动一下鼠标或点画面）。只有未就绪 / 出错时常显：
    /// 那时点画面也切不出播放，藏起来反而找不到键。
    bool controlsShown(bool ready) const { return !ready || !collapsed_; }

    /// 点画面：播放 / 暂停切换，同时把控制条叫出来（之后照样自动隐去）。
    /// 收起状态下控制条整条 `IgnorePointer`，所以这点会落回画面层，不会被条抢走。
    void onStageTap() {
        if (!controller_)
            return;
        const VideoPlayerValue v = controller_->value();
        if (!v.isInitialized || v.hasError())
            return;
        if (v.isPlaying)
            controller_->pause();
        else
            controller_->play();
        revealControls();
    }

    void revealControls() {
        const bool wasCollapsed = collapsed_;
        collapsed_ = false;
        armHideTimer();
        // 只在「收起 → 显形」那一次重建；鼠标持续移动只重新计时，不重绘整页。
        if (wasCollapsed && mounted())
            setState();
    }

    void onStageExit() {
        if (collapsed_) {
            stopHideTimer();
            return;
        }
        collapsed_ = true;
        stopHideTimer();
        if (mounted())
            setState();
    }

    /// 就绪且未收起时保表在跑（同 AutoHideToolbar：自驱动比 elapsed，不等 hover）。
    void armHideTimer() {
        if (!hideTicker_) {
            hideTicker_ = std::make_unique<Ticker>([this](Duration elapsed) {
                if (elapsed.count() < kHideDelayMs)
                    return;
                stopHideTimer();
                if (collapsed_)
                    return;
                collapsed_ = true;
                if (mounted())
                    setState();
            });
        }
        hideTicker_->start();
    }

    void stopHideTimer() {
        if (hideTicker_ && hideTicker_->isActive())
            hideTicker_->stop();
    }

    /// 只要播放区就绪且控制条还显着，就需要计时收起；build 里顺便补上
    /// 「非鼠标途径（如点「打开文件…」加载完成）」那一次计时。
    void syncHideTimer(bool ready) {
        if (!ready || collapsed_) {
            stopHideTimer();
            return;
        }
        if (!hideTicker_ || !hideTicker_->isActive())
            armHideTimer();
    }

    std::string statusLine(const VideoPlayerValue& v) {
        const double position = double(v.position.count()) / 1e6;
        const double duration = double(v.duration.count()) / 1e6;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "位置 %.2f / %.2f s · 画面 %d×%d · 倍速 %.1f× · 音量 %d%% · "
                      "循环 %s · %s",
                      position, duration, int(v.size.width), int(v.size.height),
                      v.playbackSpeed, int(v.volume * 100.0 + 0.5),
                      v.isLooping ? "开" : "关",
                      v.isPlaying ? "播放中" : (v.isCompleted ? "已播完" : "暂停"));
        return buf;
    }

    std::shared_ptr<VideoPlayerController> controller_;
    std::string sourceLabel_;
    int handle_ = 0;

    // ── 控制条自动隐藏 ────────────────────────────────────────
    /// 鼠标静止 / 点画面之后多久收起控制条（毫秒）。
    static constexpr double kHideDelayMs = 2600.0;
    /// 计时到期 / 鼠标已离开播放区 ⇒ 收起。
    bool collapsed_ = false;
    std::unique_ptr<Ticker> hideTicker_;
};

inline std::unique_ptr<State> VideoPlayerDemo::createState() {
    return std::make_unique<VideoPlayerDemoState>();
}

inline SharedPtr<Widget> pageVideo() {
    return cui::make_shared<VideoPlayerDemo>();
}

// ══════════════════════════════════════════════════════════════════════════════
// 页面：多窗口（Application::createWindow / AppWindow 每窗一条独立流水线）
// ══════════════════════════════════════════════════════════════════════════════

/// 子窗口里的内容。这棵树的 BuildOwner / PipelineOwner / RenderView 都属于
/// 那个 AppWindow，与主窗口完全不共享 —— 两边的 setState 只重建各自那棵。
class DemoWindowBody : public StatefulWidget {
public:
    DemoWindowBody(int index, AppWindow* window)
        : index_(index), window_(window) {}

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DemoWindowBody"; }

    int index() const { return index_; }
    AppWindow* window() const { return window_; }

private:
    int index_;
    AppWindow* window_;
};

class DemoWindowBodyState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        auto* body = static_cast<DemoWindowBody*>(widget());
        const int index = body ? body->index() : 0;
        AppWindow* self = body ? body->window() : nullptr;

        std::vector<SharedPtr<Widget>> buttons;
        buttons.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("点我 +1"), [this] {
                    ++count_;
                    setState();
                }));
        buttons.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
        buttons.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("关闭本窗口"), [self] {
                    if (auto* app = Application::instance())
                        app->closeWindow(self);
                }));

        std::vector<SharedPtr<Widget>> colKids;
        colKids.push_back(cui::make_shared<Text>(
                "子窗口 #" + std::to_string(index),
                TextStyle().size(18.0).bold()));
        colKids.push_back(vgap(8.0));
        colKids.push_back(caption("独立 AppWindow：自己的 PlatformWindow、Skia surface、"
                                  "BuildOwner / PipelineOwner / RenderView 与脏区。"));
        colKids.push_back(vgap(24.0));
        colKids.push_back(cui::make_shared<Text>(
                "计数：" + std::to_string(count_),
                TextStyle().size(30.0).bold()));
        colKids.push_back(vgap(24.0));
        colKids.push_back(hrow(std::move(buttons)));
        colKids.push_back(vgap(16.0));
        colKids.push_back(caption("在主窗口点什么都不会动这里 —— 两棵树各自重建、各自出帧。"));

        auto col = cui::make_shared<Column>(std::move(colKids));
        col->setMainAxisSize(MainAxisSize::Max);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(col);
        box->padding(EdgeInsets::all(20.0));
        return box;
    }

private:
    int count_ = 0;
};

inline std::unique_ptr<State> DemoWindowBody::createState() {
    return std::make_unique<DemoWindowBodyState>();
}

class MultiWindowDemo : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "MultiWindowDemo"; }
};

class MultiWindowDemoState : public State {
public:
    void initState() override {
        State::initState();
        app_ = Application::instance();
    }

    void dispose() override {
        // 页面销毁时收掉本页开的子窗口，避免留下无法再操作的原生窗口。
        closeChildren();
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        int open = 0;
        if (app_) {
            for (size_t i = 0; i < app_->windowCount(); ++i) {
                AppWindow* w = app_->windowAt(i);
                if (w && w->isOpen())
                    ++open;
            }
        }

        std::vector<SharedPtr<Widget>> buttons;
        buttons.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("打开新窗口"), [this] { openChild(); }));
        buttons.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
        buttons.push_back(cui::make_shared<ElevatedButton>(
                cui::make_shared<Text>("关闭全部子窗口"), [this] {
                    closeChildren();
                    setState();
                }));

        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(caption(
                "Application 现在运行一个事件循环、一个任务队列，但可以同时持有多个顶层窗口。"
                "每个窗口是一个 AppWindow：平台窗口 + Skia surface + 自己的 "
                "BuildOwner / PipelineOwner / RenderView / 脏区 / 命中链。"));
        kids.push_back(vgap(12.0));
        kids.push_back(caption(
                "输入按「来源窗口」派发（谁收到事件就进谁的管线），Ticker 全进程只推进一次，"
                "随后每个窗口各自出一帧。旧单窗口 API（setRoot / renderFrame / dispatchInput / "
                "saveScreenshot）继续操作第一个窗口。"));
        kids.push_back(vgap(20.0));
        kids.push_back(hrow(std::move(buttons)));
        kids.push_back(vgap(16.0));
        kids.push_back(cui::make_shared<Text>(
                "当前窗口数：" + std::to_string(open) +
                        "（含主窗口）",
                TextStyle().size(14.0).bold()));
        kids.push_back(vgap(12.0));
        kids.push_back(caption(
                "子窗口里的「关闭本窗口」走 Application::closeWindow；关掉最后一个原生窗口会退出事件循环。"));
        kids.push_back(vgap(40.0));
        return vcol(std::move(kids));
    }

private:
    void openChild() {
        if (!app_)
            app_ = Application::instance();
        if (!app_)
            return;
        const int index = ++spawned_;
        AppWindow* w = app_->createWindow(
                "cui_core — 子窗口 #" + std::to_string(index), 480, 360);
        if (!w)
            return;
        children_.push_back(w);
        w->setRoot(cui::make_shared<DemoWindowBody>(index, w));
        w->renderFrame(/*forceFull=*/true);
        setState();
    }

    void closeChildren() {
        if (!app_)
            return;
        for (AppWindow* w : children_) {
            if (w && w->isOpen())
                app_->closeWindow(w);
        }
        children_.clear();
    }

    Application* app_ = nullptr;
    std::vector<AppWindow*> children_;
    int spawned_ = 0;
};

inline std::unique_ptr<State> MultiWindowDemo::createState() {
    return std::make_unique<MultiWindowDemoState>();
}

inline SharedPtr<Widget> pageMultiWindow() {
    return pageBody({
        sectionTitle("多窗口"),
        caption("Application::createWindow(title, w, h) 返回 AppWindow*；"
                "createOffscreenWindow(w, h) 建无原生窗口的离屏窗口（测试 / 截图用）。"
                "windowCount() / windowAt(i) 枚举，closeWindow(w) 关闭。"),
        vgap(12.0),
        cui::make_shared<MultiWindowDemo>(),
        vgap(20.0),
        caption("实现要点：窗口列表由 Application 持有，事件按来源窗口派发；"
                "每个 AppWindow 自带 SchedulerBinding → PipelineOwner → RenderView，"
                "帧末只 flush 自己那棵树的 build/layout/paint，脏区也只落在自己的 surface 上。"),
        vgap(40.0),
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 真正的虚拟列表：100 万行，零逐行节点
// ══════════════════════════════════════════════════════════════════════════════

inline SharedPtr<Widget> pageVirtualList() {
    auto model = cui::make_shared<cui::FixedListModel>(
        1'000'000u, 26.0, [](std::size_t i) {
            return "  Row " + std::to_string(i + 1) + "  ·  虚拟列表第 " +
                   std::to_string(i + 1) + " 行";
        });
    auto list = cui::make_shared<cui::VirtualList>(model);
    auto sized = cui::make_shared<cui::Container>(list);
    sized->width(460.0).height(300.0);
    return pageBody({
        sectionTitle("Virtual List"),
        caption("1,000,000 行 · 零逐行节点 · 滚轮 / 拖动滚动条 / 点击选中"),
        vgap(8.0),
        sized,
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// 壳：标题栏 + 侧边栏 + 内容区 + 状态条
// ══════════════════════════════════════════════════════════════════════════════

class GalleryShell : public StatefulWidget {
public:
    struct Entry {
        const char* group;  // 侧边栏分组（folder），可折叠
        const char* name;
        IconCode icon;      // 侧栏图标（折叠态只显示它）
        std::function<SharedPtr<Widget>()> build;
    };

    GalleryShell() = default;
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "GalleryShell"; }

    /// 页面注册表（静态，壳的 State 读取）。分组沿用原 story 的
    /// Demos / Basics / Data Display / Layout / Navigation / Feedback / Advanced。
    static const std::vector<Entry>& pages() {
        static const std::vector<Entry> kPages = {
            {"入门", "概览", IconCode::Home, pageOverview},
            {"Demos", "Animation", IconCode::Play, pageFlAnimation},
            {"Demos", "Spreadsheet", IconCode::Table, pageSpreadsheet},
            {"Demos", "Chat", IconCode::Person, pageChat},
            {"Demos", "Custom Widget", IconCode::Gesture, pageCustomWidget},
            {"Demos", "Drag & Drop", IconCode::Layers, pageDragDrop},
            {"Demos", "Hero Animation", IconCode::ArrowForward, pageHero},
            {"Basics", "按钮", IconCode::Button, pageButtons},
            {"Basics", "选择", IconCode::Dropdown,
             [] { return cui::make_shared<SelectionPage>(); }},
            {"Basics", "滑块", IconCode::Slider,
             [] { return cui::make_shared<SliderPage>(); }},
            {"Basics", "图标", IconCode::Star,
             [] { return cui::make_shared<IconPage>(); }},
            {"Basics", "文本", IconCode::Text, pageTextAdv},
            {"Basics", "文本输入", IconCode::Input, pageTextInput},
            {"Basics", "输入+", IconCode::Checkbox, pageInput2},
            {"Basics", "ComboBox", IconCode::Dropdown,
             [] { return cui::make_shared<ComboBoxPage>(); }},
            {"Basics", "值输入", IconCode::Toggle, pageValueInput},
            {"Data Display", "数据", IconCode::Table, pageData},
            {"Data Display", "数据+", IconCode::List, pageData2},
            {"Data Display", "组件", IconCode::Grid, pageComponents},
            {"Data Display", "图表", IconCode::Chart, pageFlChart},
            {"Data Display", "Chips", IconCode::Tag, pageChipsList},
            {"Layout", "布局", IconCode::Folder, pageLayout},
            {"Layout", "布局+", IconCode::Layers, pageLayoutAdv},
            {"Layout", "布局2", IconCode::Search, pageLayout2},
            {"Layout", "滚动", IconCode::ArrowDownward,
             [] { return cui::make_shared<ScrollPage>(); }},
            {"Layout", "滚动+", IconCode::ArrowUpward,
             [] { return cui::make_shared<ScrollAdvPage>(); }},
            {"Layout", "下拉刷新", IconCode::Refresh, pageRefresh},
            {"Layout", "Virtual List", IconCode::List, pageVirtualList},
            {"Navigation", "导航", IconCode::Menu, pageNav},
            {"Navigation", "导航2", IconCode::Radio, pageNav2},
            {"Navigation", "TabBar", IconCode::List, pageTabBar},
            {"Navigation", "窗口栏", IconCode::MoreHoriz, pageChrome},
            {"Feedback", "反馈", IconCode::Bell, pageFeedback},
            {"Feedback", "反馈2", IconCode::Favorite, pageFeedback2},
            {"Feedback", "对话框", IconCode::Dialog,
             [] { return cui::make_shared<DialogsPage>(); }},
            {"Feedback", "手势", IconCode::Gesture,
             [] { return cui::make_shared<GesturesPage>(); }},
            {"Feedback", "交互+", IconCode::Person, pageInteract},
            {"Advanced", "高级", IconCode::Settings, pageAdvanced},
            {"Advanced", "Material", IconCode::Palette, pageMaterial},
            {"Advanced", "浮层+", IconCode::MoreVert,
             [] { return cui::make_shared<OverlaysPage>(); }},
            {"Advanced", "FAB", IconCode::Add, pageFab},
            {"Advanced", "外壳", IconCode::Clock, pageShell},
            {"Advanced", "尺寸动画", IconCode::Layers, pageAnimatedSize},
            {"Advanced", "intrinsic", IconCode::Grid, pageIntrinsic},
            {"Advanced", "Async", IconCode::Refresh, pageAsync},
            {"Advanced", "Material Motion", IconCode::Play, pageMaterialMotion},
            {"Advanced", "AutoHideToolbar", IconCode::MoreHoriz,
             pageAutoHideToolbar},
            {"Advanced", "系统服务", IconCode::Settings, pageSystemServices},
            {"Advanced", "图像", IconCode::Image, pageImageStudio},
            {"Advanced", "视频", IconCode::Play, pageVideo},
            {"Advanced", "多窗口", IconCode::Dialog, pageMultiWindow},
        };
        return kPages;
    }

    /// 分组名（按注册顺序去重）——侧边栏 folder 列表。
    static const std::vector<std::string>& groups() {
        static const std::vector<std::string> kGroups = [] {
            std::vector<std::string> g;
            for (const auto& e : pages())
                if (std::find(g.begin(), g.end(), std::string(e.group)) == g.end())
                    g.push_back(e.group);
            return g;
        }();
        return kGroups;
    }

    /// 按页面名查下标（找不到返回 0）——避免页面表变动后硬编码下标失效。
    static int indexOf(const char* name) {
        const auto& p = pages();
        for (int i = 0; i < static_cast<int>(p.size()); ++i)
            if (std::string(p[i].name) == name)
                return i;
        return 0;
    }

    /// 分组图标（对齐侧栏每项的 IconName）。
    static IconCode iconForGroup(const std::string& g) {
        if (g == "入门")
            return IconCode::Home;
        if (g == "Basics")
            return IconCode::Check;
        if (g == "Data Display")
            return IconCode::Star;
        if (g == "Layout")
            return IconCode::Menu;
        if (g == "Navigation")
            return IconCode::ArrowForward;
        if (g == "Feedback")
            return IconCode::Favorite;
        return IconCode::Settings;  // Advanced 等
    }

    /// 页面 i 所属分组的下标（-1 表示无）。
    static int groupIndexOf(int i) {
        const auto& p = pages();
        if (i < 0 || i >= static_cast<int>(p.size()))
            return -1;
        const auto& g = groups();
        for (int k = 0; k < static_cast<int>(g.size()); ++k)
            if (g[k] == p[i].group)
                return k;
        return -1;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// GallerySelection — 页面列表与内容面板共享的「当前页」模型
// ══════════════════════════════════════════════════════════════════════════════

/// 切换页面不是停靠布局变化，不会触发 DockManager 事件；所以用 ChangeNotifier
/// 让「页面列表面板」与「内容面板」各自订阅、各自 setState。
class GallerySelection : public ChangeNotifier {
public:
    int selected() const { return selected_; }
    void select(int i) {
        const int n = static_cast<int>(GalleryShell::pages().size());
        if (i < 0)
            i = 0;
        if (n > 0 && i >= n)
            i = n - 1;
        if (i == selected_)
            return;
        selected_ = i;
        notifyListeners();
    }

private:
    int selected_ = 0;
};

// ══════════════════════════════════════════════════════════════════════════════
// GalleryPagesPanel — 页面列表（原固定侧栏）改造成可停靠左侧面板
// ══════════════════════════════════════════════════════════════════════════════

class GalleryPagesPanel : public StatefulWidget {
public:
    GalleryPagesPanel(SharedPtr<GallerySelection> sel, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), sel_(std::move(sel)) {}

    const SharedPtr<GallerySelection>& selection() const { return sel_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "GalleryPagesPanel"; }

private:
    SharedPtr<GallerySelection> sel_;
};

class GalleryPagesPanelState : public State {
public:
    // 行高估算（用于「选中项必须可见」的滚动定位）。
    static constexpr double kHeadH = 34.0;   // 页眉
    static constexpr double kGroupH = 32.0;  // 分组 folder 标题
    static constexpr double kItemH = 30.0;   // 页面条目

    void initState() override {
        State::initState();
        auto* self = this;
        handle_ = panel()->selection()->addListenerWithHandle([self] {
            self->needReveal_ = true;  // 任何来源的选中变化都保证滚入可见区
            self->setState();
        });
    }
    void dispose() override {
        panel()->selection()->removeListenerByHandle(handle_);
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        const auto& pages = GalleryShell::pages();
        const int n = static_cast<int>(pages.size());
        int selected = panel()->selection()->selected();
        if (selected >= n)
            selected = 0;

        std::vector<SharedPtr<Widget>> items;
        auto headTitle =
            cui::make_shared<Text>("CUI Gallery", TextStyle().size(12.5).bold());
        auto headRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
            headTitle, cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0))});
        headRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto headBox = cui::make_shared<Container>(headRow);
        headBox->height(kHeadH).padding(EdgeInsets::symmetric(8.0, 12.0))
            .decoration(BoxDecoration{theme().hoverOverlay, 0.0, 1.0, theme().divider});
        items.push_back(headBox);

        double y = kHeadH;
        const auto& groups = GalleryShell::groups();
        if (collapsedGroups_.size() != groups.size())
            collapsedGroups_.assign(groups.size(), false);

        for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
            std::vector<int> members;
            for (int i = 0; i < n; ++i)
                if (GalleryShell::groupIndexOf(i) == gi)
                    members.push_back(i);

            bool folded = collapsedGroups_[gi];
            auto arrow = cui::make_shared<Text>(
                folded ? "▶" : "▼",
                TextStyle().size(11.0).fgColor(theme().textSecondary));
            auto gname = cui::make_shared<Text>(
                tr(groups[gi]),
                TextStyle().size(12.0).bold().fgColor(theme().textPrimary));
            auto cnt = cui::make_shared<Text>(
                std::to_string(members.size()),
                TextStyle().size(10.5).fgColor(theme().textDisabled));
            auto gIcon = cui::make_shared<Icon>(
                GalleryShell::iconForGroup(groups[gi]), 13.0, theme().textSecondary);
            auto gRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                arrow, cui::make_shared<SizedBox>(6.0, 0.0), gIcon,
                cui::make_shared<SizedBox>(6.0, 0.0),
                cui::make_shared<Expanded>(1, gname), cnt});
            gRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
            auto gBox = cui::make_shared<Container>(gRow);
            gBox->height(kGroupH).padding(EdgeInsets::symmetric(7.0, 10.0))
                .decoration(BoxDecoration{theme().hoverOverlay, 0.0, 1.0, theme().divider});
            int gidx = gi;
            items.push_back(cui::make_shared<GestureDetector>(gBox, [this, gidx] {
                collapsedGroups_[gidx] = !collapsedGroups_[gidx];
                setState();
            }));
            y += kGroupH;
            if (folded)
                continue;

            for (int idx : members) {
                bool sel = (idx == selected);
                auto bullet = cui::make_shared<Icon>(
                    pages[idx].icon, 14.0,
                    sel ? primaryColor() : theme().textDisabled);
                auto t = cui::make_shared<Text>(
                    tr(pages[idx].name),
                    sel ? TextStyle().size(13.0).bold().fgColor(primaryColor())
                        : TextStyle().size(13.0));
                auto line = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                    bullet, cui::make_shared<SizedBox>(8.0, 0.0),
                    cui::make_shared<Expanded>(1, t)});
                line->setCrossAxisAlignment(CrossAxisAlignment::Center);
                auto c = cui::make_shared<Container>(line);
                c->height(kItemH).padding(EdgeInsets(22.0, 7.0, 10.0, 7.0));
                c->decoration(BoxDecoration{
                    sel ? itemHoverBg() : Color::fromRGBA(0, 0, 0, 0), 0.0});
                if (sel) {
                    selectedTop_ = y;
                    selectedHeight_ = kItemH;
                }
                int ii = idx;
                items.push_back(cui::make_shared<GestureDetector>(c, [this, ii] {
                    selectPage(ii);
                }));
                y += kItemH;
            }
        }

        auto itemCol = cui::make_shared<Column>(std::move(items));
        itemCol->setMainAxisSize(MainAxisSize::Min);
        itemCol->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        // 右侧留出滚动条轨道的宽度（thickness+margin=8，再加 2px 透气），避免
        // 内容画到覆盖层 thumb 之下；Scrollbar 是覆盖层语义，不收窄内容视口。
        auto sidebar = cui::make_shared<SingleChildScrollView>(
            &sidebarScroll_, itemCol, Axis::Vertical, /*keyboardScroll=*/false,
            /*reverse=*/false, EdgeInsets(0, 0, 10, 0));
        // 注：SingleChildScrollView 桌面端自带 Scrollbar(controller)，勿再显式包裹。
        // 侧栏获得焦点时，方向键/PgUp/PgDn 切换选中项（而非滚动）。
        auto sidebarFocus = cui::make_shared<Focus>(
            sidebarFocusNode_, sidebar, [](bool) {}, /*autofocus=*/true,
            nullptr, [this](const KeyEvent& ev) { return handleSidebarKey(ev); });
        // 视口高度要到布局阶段才知道：用 LayoutBuilder 拿到约束后再定位。
        auto sidebarWrap = cui::make_shared<LayoutBuilder>(
            [this, sidebarFocus](BuildContext&, const BoxConstraints& c) {
                maybeRevealSelected(c.maxHeight);
                return sidebarFocus;
            });
        return sidebarWrap;
    }

private:
    GalleryPagesPanel* panel() { return static_cast<GalleryPagesPanel*>(widget()); }

    /// 点击/键盘选中某页：自动展开其分组，再写回共享选中态。
    void selectPage(int i) {
        int gi = GalleryShell::groupIndexOf(i);
        const auto& groups = GalleryShell::groups();
        if (collapsedGroups_.size() != groups.size())
            collapsedGroups_.assign(groups.size(), false);
        if (gi >= 0 && gi < static_cast<int>(collapsedGroups_.size()))
            collapsedGroups_[gi] = false;
        panel()->selection()->select(i);
    }

    /// 选中项滚动定位：偏移落在视口外时把侧栏滚到刚好露出该项。
    void maybeRevealSelected(double viewportH = -1.0) {
        if (!needReveal_)
            return;
        double vp = (viewportH > 0.0) ? viewportH : sidebarScroll_.viewportExtent();
        if (vp <= 0.0)
            return;
        double off = sidebarScroll_.offset();
        double want = off;
        const double margin = 8.0;
        if (selectedTop_ < off)
            want = std::max(0.0, selectedTop_ - margin);
        else if (selectedTop_ + selectedHeight_ > off + vp)
            want = selectedTop_ + selectedHeight_ - vp + margin;
        want = std::max(0.0, want);
        needReveal_ = false;
        if (std::fabs(want - off) > 0.5) {
            if (sidebarScroll_.hasClients())
                sidebarScroll_.jumpTo(want);
            else
                sidebarScroll_.forceOffset(want);
        }
    }

    bool groupCollapsed(int pageIdx) const {
        int gi = GalleryShell::groupIndexOf(pageIdx);
        return (gi >= 0 && gi < static_cast<int>(collapsedGroups_.size()))
                   ? collapsedGroups_[gi]
                   : false;
    }
    int nextVisiblePage(int i) const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        i = std::max(0, i);
        for (; i < n; ++i)
            if (!groupCollapsed(i))
                return i;
        return lastVisiblePage();
    }
    int prevVisiblePage(int i) const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        i = std::min(n - 1, i);
        for (; i >= 0; --i)
            if (!groupCollapsed(i))
                return i;
        return firstVisiblePage();
    }
    int firstVisiblePage() const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        for (int i = 0; i < n; ++i)
            if (!groupCollapsed(i))
                return i;
        return 0;
    }
    int lastVisiblePage() const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        for (int i = n - 1; i >= 0; --i)
            if (!groupCollapsed(i))
                return i;
        return n - 1;
    }
    KeyEventResult handleSidebarKey(const KeyEvent& ev) {
        const double vp = sidebarScroll_.viewportExtent();
        const int pageStep = std::max(1, static_cast<int>(vp / kItemH) - 1);
        const int cur = panel()->selection()->selected();
        int next = cur;
        switch (static_cast<KeyCode>(ev.key)) {
            case KeyCode::Down:     next = nextVisiblePage(cur + 1); break;
            case KeyCode::Up:       next = prevVisiblePage(cur - 1); break;
            case KeyCode::PageDown: next = nextVisiblePage(cur + pageStep); break;
            case KeyCode::PageUp:   next = prevVisiblePage(cur - pageStep); break;
            default:                return KeyEventResult::Ignored;
        }
        if (next != cur)
            selectPage(next);
        return KeyEventResult::Handled;
    }

    int handle_ = 0;
    bool needReveal_ = true;
    std::vector<bool> collapsedGroups_;  // 侧边栏 folder 折叠状态
    double selectedTop_ = 0.0;           // 选中条目在侧栏内容中的估算 y
    double selectedHeight_ = 30.0;
    ScrollController sidebarScroll_;
    SharedPtr<FocusNode> sidebarFocusNode_ = cui::make_shared<FocusNode>();
};

inline std::unique_ptr<State> GalleryPagesPanel::createState() {
    return std::make_unique<GalleryPagesPanelState>();
}

// ══════════════════════════════════════════════════════════════════════════════
// GalleryContentPanel — 当前页内容（ShellContent/Navigator）作为停靠中心面板
// ══════════════════════════════════════════════════════════════════════════════

class GalleryContentPanel : public StatefulWidget {
public:
    GalleryContentPanel(SharedPtr<GallerySelection> sel, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), sel_(std::move(sel)) {}

    const SharedPtr<GallerySelection>& selection() const { return sel_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "GalleryContentPanel"; }

private:
    SharedPtr<GallerySelection> sel_;
};

class GalleryContentPanelState : public State {
public:
    void initState() override {
        State::initState();
        auto* self = this;
        handle_ = panel()->selection()->addListenerWithHandle([self] {
            if (self->pageScroll_.hasClients())
                self->pageScroll_.jumpTo(0.0);
            self->setState();
        });
    }
    void dispose() override {
        panel()->selection()->removeListenerByHandle(handle_);
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        // ShellContent 自带 Navigator（对话框/弹层需要），页变化时其
        // didUpdateWidget 会弹回根再压入新页。
        auto shell = cui::make_shared<ShellContent>();
        shell->pageIndex = panel()->selection()->selected();
        auto* self = this;
        shell->builder = [self](int i) {
            return cui::make_shared<SingleChildScrollView>(
                &self->pageScroll_, GalleryShell::pages()[i].build());
        };
        return shell;
    }

private:
    GalleryContentPanel* panel() { return static_cast<GalleryContentPanel*>(widget()); }

    int handle_ = 0;
    ScrollController pageScroll_;
};

inline std::unique_ptr<State> GalleryContentPanel::createState() {
    return std::make_unique<GalleryContentPanelState>();
}

class GalleryShellState : public State {
public:
    void initState() override {
        State::initState();

        // ── 停靠外壳：页面列表（pages）停靠左侧，页面内容（content）居中 ──
        Docking::initialize();

        sel_ = cui::make_shared<GallerySelection>();
        outputText_ = buildOutput();

        // 两个 story 面板用稳定的 ValueKey，dock 重建时可复用 State（而非重建）。
        auto contentPanel = cui::make_shared<GalleryContentPanel>(
            sel_, cui::make_shared<ValueKey<std::string>>("story-content"));
        auto pagesPanel = cui::make_shared<GalleryPagesPanel>(
            sel_, cui::make_shared<ValueKey<std::string>>("story-pages"));

        docks_ = {
            makePanel({"content", tr("页面"), tr("页面"), "",
                       DockableStyle::CenterOnly, false, false, false,
                       GalleryDockPanel::PanelType::Generic},
                      contentPanel),
            makePanel({"pages", tr("页面列表"), tr("页面列表"), "",
                       DockableStyle::Horizontal, true, true, true,
                       GalleryDockPanel::PanelType::PageList},
                      pagesPanel),
            makePanel({"solution", "Solution Explorer", "Solution Explorer",
                       "Browse the solution tree", DockableStyle::Horizontal,
                       true, true, true,
                       GalleryDockPanel::PanelType::Explorer},
                      buildSolutionExplorer()),
            makePanel({"toolbox", "Toolbox", "Toolbox",
                       "Drag controls onto the designer", DockableStyle::Horizontal,
                       true, true, true,
                       GalleryDockPanel::PanelType::Explorer},
                      buildToolbox()),
            makePanel({"properties", "Properties", "Properties",
                       "Selected item properties", DockableStyle::Horizontal,
                       true, true, true,
                       GalleryDockPanel::PanelType::Inspector},
                      buildProperties()),
            makePanel({"class_view", "Class View", "Class View",
                       "Symbols in the solution", DockableStyle::Horizontal,
                       true, true, true,
                       GalleryDockPanel::PanelType::Inspector},
                      buildClassView()),
            makePanel({"watch", "Watch", "Watch",
                       "Debugger watch expressions", DockableStyle::Horizontal,
                       true, true, true,
                       GalleryDockPanel::PanelType::Inspector},
                      buildWatch()),
            makePanel({"output", "Output", "Output",
                       "Build output stream", DockableStyle::Vertical,
                       true, true, true,
                       GalleryDockPanel::PanelType::Output},
                      cui::make_shared<IdeLogView>(outputText_)),
            makePanel({"error_list", "Error List", "Error List",
                       "Build errors and warnings", DockableStyle::Vertical,
                       true, true, true,
                       GalleryDockPanel::PanelType::Output},
                      buildErrorList()),
            makePanel({"terminal", "Terminal", "Terminal",
                       "Integrated developer terminal", DockableStyle::Vertical,
                       true, true, true,
                       GalleryDockPanel::PanelType::Output},
                      buildTerminal()),
            makePanel({"find_results", "Find Results", "Find Results",
                       "Find in Files results", DockableStyle::Vertical,
                       true, true, true,
                       GalleryDockPanel::PanelType::Output},
                      buildFindResults()),
            makePanel({"build_output", "Build Output", "Build Output",
                       "Full build log", DockableStyle::Vertical,
                       true, true, true,
                       GalleryDockPanel::PanelType::Output},
                      buildBuildOutput()),
        };

        Docking::setDefaultApplicationLayout(dockGalleryLayoutBuilder());
        applyInitialDockGalleryLayout();

        // 离屏冒烟：CUI_PAGE="<idx>" 直接跳到指定页（默认 0）。
        if (const char* p = std::getenv("CUI_PAGE")) {
            long v = std::strtol(p, nullptr, 10);
            sel_->select(static_cast<int>(v));
        }
        // CUI_GLASS=1 以「液态玻璃」外观启动（截图核验 / 演示用）。
        if (const char* g = std::getenv("CUI_GLASS"))
            glass_ = g[0] != '\0' && g[0] != '0';
    }

    void dispose() override {
        docks_.clear();  // 面板析构逐个 unregister，避免退出时残留悬空面板
        State::dispose();
    }

    // ── 停靠版外壳：IDE 菜单栏 / 工具条 / 停靠区 / 状态条 ────────────────
    // 页面列表（pages）与页面内容（content）都是 RootDockingPanel 里的
    // 可停靠面板；主题/语言切换靠本 State setState 重建整棵 dock 树。
    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        // 液态玻璃：glass 模式下把背景/表面/表面变体置透明，让 dock（及其内部
        // CUI 控件）连同菜单栏/工具栏一起变透，露出后面的磨砂壁纸——这才是真正
        // 的「液态玻璃」。关闭时恢复惰性捕获的基准主题（主题切换时同步更新）。
        // 首次 build 即捕获基准主题（此时仍为不透明），避免从玻璃态切回时把
        // 「透明主题」误存为基准。
        if (!glassBaseCaptured_) {
            baseTheme_ = theme();
            glassBaseCaptured_ = true;
        }
        if (glass_) {
            theme().background = Color::fromRGBA(0.0, 0.0, 0.0, 0.0);
            theme().surface = Color::fromRGBA(0.0, 0.0, 0.0, 0.0);
            theme().surfaceVariant = Color::fromRGBA(0.0, 0.0, 0.0, 0.0);
        } else {
            theme() = baseTheme_;
        }
        auto logToOutput = [this](const std::string& line) {
            outputText_->append(line);
            // 仅 append 不会把 Output 标签顶到前台，用户看不到变化；这里顺带前置。
            Docking::bringToFront("output");
        };

        // ── 菜单（合并 story 主题/语言入口 + IDE 外壳）──────────────────
        static const char* kThemeKeys[] = {"light",   "dark",     "ocean",
                                           "forest",  "skyblue",  "orange",
                                           "lavender", "ide"};
        static const char* kThemeLabels[] = {
            "浅色 Light", "深色 Dark",     "海洋 Ocean", "森林 Forest",
            "天蓝 SkyBlue", "橙色 Orange", "熏衣草 Lavender", "IDE"};
        static const char* kLangCodes[] = {"en", "zh", "zh-TW", "fr", "ko"};
        const char* kLangLabels[] = {"English", "中文", "繁體中文", "Français",
                                     "한국어"};

        std::vector<MenuDef> menus;
        menus.push_back({tr("文件"),
                         {MenuAction{tr("新建"), [logToOutput] { logToOutput("1> New document"); }},
                          MenuAction{tr("打开"), [logToOutput] { logToOutput("1> Open file"); }},
                          MenuAction{tr("保存"), [logToOutput] { logToOutput("1> Saved"); }},
                          MenuAction{tr("退出"),
                                     [] { if (auto* a = Application::instance()) a->quit(); }}}});
        menus.push_back({tr("视图"),
                         {MenuAction{tr("上一页"), [this] { sel_->select(sel_->selected() - 1); }},
                          MenuAction{tr("下一页"), [this] { sel_->select(sel_->selected() + 1); }},
                          MenuAction{tr("液态玻璃"), [this] { toggleGlass(); }},
                          MenuAction{tr("重置布局"), [] { applyInitialDockGalleryLayout(); }}}});
        MenuDef themeMenu{tr("主题"), {}};
        for (int i = 0; i < 8; ++i) {
            std::string key = kThemeKeys[i];
            themeMenu.actions.push_back(MenuAction{kThemeLabels[i], [this, key] {
                                            theme() = Theme::fromName(key);
                                            baseTheme_ = theme();
                                            glassBaseCaptured_ = true;
                                            setState();
                                        }});
        }
        menus.push_back(themeMenu);
        MenuDef langMenu{tr("语言"), {}};
        for (int i = 0; i < 5; ++i) {
            std::string code = kLangCodes[i];
            langMenu.actions.push_back(MenuAction{kLangLabels[i], [this, code] {
                                            Translator::instance().setLanguage(code);
                                            setState();
                                        }});
        }
        menus.push_back(langMenu);
        menus.push_back({tr("帮助"),
                         {MenuAction{tr("关于 CUI Gallery"),
                                     [logToOutput] { logToOutput("1> About CUI Gallery"); }}}});

        // 顶栏下拉必须由覆盖整窗的顶层 Stack 承载（见 buildIdeMenuBar 注释），
        // 故把菜单定义存到成员，供浮层构建器复用同一份 MenuDef。
        dockMenus_ = std::move(menus);
        auto menuBar = buildIdeMenuBar(
            dockMenus_, dockMenuOpen_, [this](int i) { toggleDockMenu(i); });
        // 注：独立工具栏已合并进下方 Ribbon（「文件 / 编辑 / 生成」分组 + 尾部配置下拉）。

        // ── 状态条（分组 · 页名 + 条目数 + 宿主）─────────────────────
        const auto& pages = GalleryShell::pages();
        const int n = static_cast<int>(pages.size());
        int idx = sel_->selected();
        if (idx < 0 || idx >= n)
            idx = 0;
#if defined(__EMSCRIPTEN__)
        const char* hostName = "host: web";
#else
        const char* hostName = "host: native";
#endif
        auto statusBar = buildIdeStatusBar(
            std::vector<std::string>{tr(pages[idx].group) + " · " + tr(pages[idx].name),
                                     std::to_string(n) + " pages", hostName});

        // ── Ribbon（Office 风格命令区：选项卡 + 分组按钮）──────────────
        // 加回曾被退役外壳移除的 Ribbon：布局在菜单栏/工具栏之下、停靠区之上，
        // 与停靠面板同处一壳。宽度 0.0 ⇒ 由外壳拉伸；液态玻璃模式下面改半透明白。
        auto ribbon = cui::make_shared<Ribbon>(
            std::vector<RibbonTabData>{
                {tr("开始"),
                 {{tr("文件"),
                   {{tr("新建"), true, [logToOutput] { logToOutput("1> New document"); }},
                    {tr("打开"), false, [logToOutput] { logToOutput("1> Open file"); }},
                    {tr("保存"), false, [logToOutput] { logToOutput("1> Saved"); }}}},
                  {tr("编辑"),
                   {{tr("撤销"), true, [logToOutput] { logToOutput("1> Undo"); }},
                    {tr("重做"), false, [logToOutput] { logToOutput("1> Redo"); }}}},
                  {tr("生成"),
                   {{tr("生成"), true, [this] { ideBuildSolution(outputText_); }},
                    {tr("运行"), false,
                     [logToOutput] { logToOutput("1> Run started: Project: cui_core"); }}}}}},
                {tr("视图"),
                 {{tr("导航"),
                   {{tr("上一页"), true, [this] { stepPage(-1); }},
                    {tr("下一页"), true, [this] { stepPage(1); }}}},
                  {tr("跳转"),
                   {{tr("概览"), false, [this] { gotoPage(0); }},
                    {tr("数据"), false, [this] { gotoPage(GalleryShell::indexOf("数据")); }},
                    {tr("导航"), false, [this] { gotoPage(GalleryShell::indexOf("导航")); }},
                    {tr("外壳"), false, [this, n] { gotoPage(n - 1); }}}},
                  {tr("窗口"),
                   {{tr("折叠侧栏"), true, [this] { toggleSidebar(); }},
                    {tr("液态玻璃"), false, [this] { toggleGlass(); }}}},
                  {tr("页面"),
                   {{tr("首页"), false, [this] { gotoPage(0); }},
                    {tr("末页"), false, [this, n] { gotoPage(n - 1); }}}}}},
                {tr("帮助"), {{tr("关于"),
                              {{tr("关于"), true, [] {}}, {tr("文档"), false, [] {}}}}}},
            },
            [](int) {}, 0.0);  // 宽度由壳拉伸决定
        ribbon->setGlass(glass_);
        // 工具栏右端的「解决方案配置」下拉合并到 Ribbon 命令区右侧（尾部挂件）。
        ribbon->setTrailing(buildIdeConfigCombo(logToOutput));

        // ── 停靠中心 + 外壳列 ──────────────────────────────────
        // glassPanel：glass 模式下在 dock 下垫一层磨砂玻璃片并留圆角边距，
        // 普通模式原样返回。配合「透明主题」让停靠区真正呈现液态玻璃。
        auto dock = glassPanel(cui::make_shared<RootDockingPanel>());
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            menuBar, ribbon, cui::make_shared<Expanded>(1, dock), statusBar});
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

        // ── 顶层 Stack：col 作为非定位子驱动 Stack 尺寸（= 整窗，因 RenderView
        //    给的是窗口大小的紧约束）；液态玻璃在其下再垫壁纸 + 磨砂两片；菜单
        //    展开时叠加「屏障 + 下拉浮层」为最上层——它们都是 Positioned，不挤占
        //    布局，且在命中测试时位于停靠区之上（Stack 逆序命中，后加的浮层先命中）。
        constexpr double kDockBarH = 28.0;  // 与 buildIdeMenuBar 的 height 一致
        auto fillLayer = [](SharedPtr<Widget> w) {
            auto pos = cui::make_shared<Positioned>(std::move(w));
            pos->hasLeft = pos->hasRight = pos->hasTop = pos->hasBottom = true;
            return pos;
        };
        std::vector<SharedPtr<Widget>> layers;
        if (glass_) {
            layers.push_back(fillLayer(cui::make_shared<CustomPaint>(
                [](Painter& p, const Size& s) {
                    auto img = frostedWallpaper(s.width, s.height, glassSheetBlur());
                    if (!img) {
                        paintGlassWallpaper(p, s);
                        return;
                    }
                    p.canvas()->drawImageRect(
                        img.get(), SkRect::MakeWH((SkScalar)s.width, (SkScalar)s.height),
                        SkSamplingOptions(SkFilterMode::kNearest), nullptr);
                })));
            layers.push_back(fillLayer(cui::make_shared<GlassOverlay>(1.0, glassSheetConfig())));
        }
        layers.push_back(col);
        if (dockMenuOpen_ >= 0 && dockMenuOpen_ < static_cast<int>(dockMenus_.size())) {
            // 屏障：覆盖「菜单条以下」全屏（菜单条不覆盖，便于再次点击切换）。
            auto barrierBox = cui::make_shared<Container>();
            barrierBox->decoration(BoxDecoration{Color{0.0, 0.0, 0.0, 0.0}});
            auto barrier = cui::make_shared<GestureDetector>(barrierBox,
                                                             [this] { closeDockMenu(); });
            auto fill = cui::make_shared<Positioned>(barrier);
            fill->hasLeft = fill->hasRight = fill->hasTop = fill->hasBottom = true;
            fill->top = kDockBarH;
            layers.push_back(fill);
            // 下拉浮层（最上层）。
            if (auto dd = buildIdeMenuDropdown(dockMenus_, dockMenuOpen_, kDockBarH,
                                               [this] { closeDockMenu(); }))
                layers.push_back(dd);
        }
        auto stack = cui::make_shared<Stack>(std::move(layers));
        stack->setFit(RenderStack::Fit::Loose);
        return stack;
    }

private:
    // 停靠版新成员：共享选中态、面板集合（保活整个进程）、Output 日志缓冲。
    SharedPtr<GallerySelection> sel_;
    std::vector<SharedPtr<GalleryDockPanel>> docks_;
    SharedPtr<IdeTextBuffer> outputText_;

    // 顶栏菜单：定义（build 里存一份供浮层复用）+ 当前展开索引（-1 未展开）。
    std::vector<MenuDef> dockMenus_;
    int dockMenuOpen_ = -1;

    void toggleDockMenu(int i) {
        dockMenuOpen_ = (dockMenuOpen_ == i) ? -1 : i;
        setState();
    }
    void closeDockMenu() {
        if (dockMenuOpen_ != -1) {
            dockMenuOpen_ = -1;
            setState();
        }
    }

    // 侧栏行高（显式设定 → 「选中项可见」的滚动定位与实际布局完全一致）。
    static constexpr double kHeadH = 34.0;   // 「页面」页眉
    static constexpr double kGroupH = 32.0;  // 分组 folder 标题
    static constexpr double kItemH = 30.0;   // 页面条目
    static constexpr double kItemIconH = 28.0;   // 折叠态条目图标行
    static constexpr double kGroupSepH = 9.0;    // 折叠态组间分隔线
    static constexpr double kBarH = 30.0;    // 顶部菜单条高度（浮层紧贴其下）
    static constexpr double kPadX = 12.0;    // 菜单标签左右内边距（须与 MenuBar 一致）
    static constexpr double kGap = 4.0;      // 菜单标签间距（须与 MenuBar 一致）

    /// 当前展开的菜单索引（-1 表示未展开）。
    int menuOpen_ = -1;
    /// 菜单定义（缓存，供浮层面板复用）。
    std::vector<MenuEntry> menus_;

    void toggleMenu(int i) {
        if (menuOpen_ == i)
            closeMenu();
        else {
            menuOpen_ = i;
            setState();
        }
    }
    /// hover 切换：MenuBar 已把住「只在展开态转发 hover」，这里只负责把展开项挪
    /// 过去（同项直接返回，避免鼠标在同一个标签上反复进出时白重建）。
    void openMenu(int i) {
        if (menuOpen_ == i)
            return;
        menuOpen_ = i;
        setState();
    }
    void closeMenu() {
        if (menuOpen_ != -1) {
            menuOpen_ = -1;
            setState();
        }
    }

    /// 展开项对应标签相对菜单条左缘的偏移（与 MenuBar 的标签布局算法保持一致，
    /// 使浮层左缘与标签左缘对齐）。
    double menuOpenLeft() const {
        double x = 0.0;
        for (int i = 0; i < menuOpen_ && i < static_cast<int>(menus_.size()); ++i)
            x += Painter::measureTextWidth(nullptr, menus_[i].label, 13.0) + 2.0 * kPadX + kGap;
        return x;
    }

    /// 构建下拉面板（已用 Positioned 定位，作为顶层 Stack 的最上层浮层）。
    SharedPtr<Widget> buildMenuPanel() {
        std::vector<SharedPtr<Widget>> rows;
        const auto& items = menus_[menuOpen_].items;
        const int openIdx = menuOpen_;
        for (int k = 0; k < static_cast<int>(items.size()); ++k) {
            auto it = cui::make_shared<Text>(items[k], TextStyle().size(12.5));
            auto ic = cui::make_shared<Container>(it);
            ic->padding(EdgeInsets::symmetric(6.0, 12.0));
            int kk = k;
            rows.push_back(cui::make_shared<GestureDetector>(ic, [this, openIdx, kk] {
                onMenuSelected(openIdx, kk);
                closeMenu();
            }));
        }
        auto list = cui::make_shared<Column>(std::move(rows));
        list->setMainAxisSize(MainAxisSize::Min);
        list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto panel = cui::make_shared<Container>(list);
        panel->width(160.0)
            .decoration(BoxDecoration{glass_ ? Color::fromRGBA(1.0, 1.0, 1.0, 0.86)
                                             : theme().surface,
                                      6.0, 1.0, theme().border, 6.0,
                                      Color::fromRGBA(0, 0, 0, 0.2)});
        auto pos = cui::make_shared<Positioned>(panel);
        pos->hasLeft = true;
        pos->left = menuOpenLeft();
        pos->hasTop = true;
        pos->top = kBarH;
        return pos;
    }

    /// 侧栏点击 / Ribbon 跳转 / 菜单「上一页/下一页」的统一入口。
    void gotoPage(int i) {
        const int n = static_cast<int>(GalleryShell::pages().size());
        if (i < 0)
            i = 0;
        if (i >= n)
            i = n - 1;
        // 目标页所在分组自动展开（否则跳转后选中项被折叠隐藏）。
        int gi = GalleryShell::groupIndexOf(i);
        const auto& groups = GalleryShell::groups();
        if (collapsedGroups_.size() != groups.size())
            collapsedGroups_.assign(groups.size(), false);
        if (gi >= 0 && gi < static_cast<int>(collapsedGroups_.size()))
            collapsedGroups_[gi] = false;
        if (i == selected_)
            return;
        selected_ = i;
        needReveal_ = true;  // 下一次 build 保证选中项滚入可见区
        pageScroll_.jumpTo(0.0);
        setState();
    }

    /// 选中项滚动定位：偏移落在视口外时把侧栏滚到刚好露出该项
    /// （对应侧栏的 setRevealBridge）。
    /// @param viewportH 布局阶段拿到的高度；<=0 时退回 ScrollController 的测量值。
    void maybeRevealSelected(double viewportH = -1.0) {
        if (!needReveal_)
            return;
        double vp = (viewportH > 0.0) ? viewportH : sidebarScroll_.viewportExtent();
        if (vp <= 0.0)
            return;  // 视口高度未知：等下一帧布局后再定位
        double off = sidebarScroll_.offset();
        double want = off;
        const double margin = 8.0;
        if (selectedTop_ < off)
            want = std::max(0.0, selectedTop_ - margin);
        else if (selectedTop_ + selectedHeight_ > off + vp)
            want = selectedTop_ + selectedHeight_ - vp + margin;
        want = std::max(0.0, want);
        needReveal_ = false;  // 先清标记：下面的 jumpTo 会同步通知流式监听者重建
        if (std::fabs(want - off) > 0.5) {
            if (sidebarScroll_.hasClients())
                sidebarScroll_.jumpTo(want);  // 已测量：按真实 maxScrollExtent clamp
            else
                sidebarScroll_.forceOffset(want);  // 首帧：maxExtent 还是 0，布局后再 clamp
        }
    }
    void stepPage(int delta) { gotoPage(selected_ + delta); }
    void toggleSidebar() {
        sidebarCollapsed_ = !sidebarCollapsed_;
        setState();
    }
    /// 普通外观 ↔ 液态玻璃外观。
    void toggleGlass() {
        glass_ = !glass_;
        setState();
    }

    /// 液态玻璃模式：把一块区域包成圆角玻璃卡片。玻璃片在内容**之下**
    /// （Stack 先玻璃后内容），文字与控件不会被霜色糊住；普通模式原样返回。
    SharedPtr<Widget> glassPanel(SharedPtr<Widget> child) {
        if (!glass_)
            return child;
        auto pane = cui::make_shared<GlassOverlay>(1.0, glassPanelConfig());
        auto stack = cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{pane, child});
        stack->setFit(RenderStack::Fit::Loose);
        auto pad = cui::make_shared<Container>(stack);
        pad->padding(EdgeInsets::all(8.0));
        return pad;
    }
    /// 菜单栏选择回调（File/Edit/View/Help 菜单项）。
    void onMenuSelected(int menu, int item) {
        static const char* kThemeKeys[] = {"light",  "dark",   "ocean",
                                           "forest", "skyblue", "orange",
                                           "lavender", "ide"};
        static const char* kLangCodes[] = {"en", "zh", "zh-TW", "fr", "ko"};
        if (menu == 4 && item >= 0 && item < 8) {  // 主题
            theme() = Theme::fromName(kThemeKeys[item]);
            setState();
            return;
        }
        if (menu == 5 && item >= 0 && item < 5) {  // 语言
            Translator::instance().setLanguage(kLangCodes[item]);
            setState();
            return;
        }
        if (menu == 2) {  // 视图
            if (item == 0)
                toggleSidebar();
            else if (item == 1)
                stepPage(-1);
            else if (item == 2)
                stepPage(1);
            else if (item == 3)
                toggleGlass();
        }
        std::fprintf(stderr, "[story] menu %d/%d\n", menu, item);
    }

    int selected_ = 0;
    bool sidebarCollapsed_ = false;
    /// 液态玻璃外观（视图菜单 / Ribbon「窗口」组 / CUI_GLASS=1 切换）。
    bool glass_ = false;
    /// 玻璃模式关闭时恢复的基准主题（惰性捕获；主题切换时同步更新），
    /// 避免把透明主题永久写死。
    Theme baseTheme_;
    bool glassBaseCaptured_ = false;
    std::vector<bool> collapsedGroups_;  // 侧边栏 folder 折叠状态
    double selectedTop_ = 0.0;           // 选中条目在侧栏内容中的估算 y
    double selectedHeight_ = 30.0;
    bool needReveal_ = true;             // 待把选中项滚入可见区
    ScrollController sidebarScroll_;
    ScrollController pageScroll_;
    /// 侧栏选中导航的持久焦点节点（跨 rebuild 复用，避免每次 build 重建丢失焦点）。
    SharedPtr<FocusNode> sidebarFocusNode_ = cui::make_shared<FocusNode>();

    // ── 侧栏键盘选中导航（方向键/PgUp/PgDn）──────────────────────────
    bool groupCollapsed(int pageIdx) const {
        int gi = GalleryShell::groupIndexOf(pageIdx);
        return (gi >= 0 && gi < static_cast<int>(collapsedGroups_.size()))
                   ? collapsedGroups_[gi]
                   : false;
    }
    /// 未折叠分组里、>= i 的最小可见页索引（越界回退到最后一个可见项）。
    int nextVisiblePage(int i) const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        i = std::max(0, i);
        for (; i < n; ++i)
            if (!groupCollapsed(i))
                return i;
        return lastVisiblePage();
    }
    int prevVisiblePage(int i) const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        i = std::min(n - 1, i);
        for (; i >= 0; --i)
            if (!groupCollapsed(i))
                return i;
        return firstVisiblePage();
    }
    int firstVisiblePage() const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        for (int i = 0; i < n; ++i)
            if (!groupCollapsed(i))
                return i;
        return 0;
    }
    int lastVisiblePage() const {
        const int n = static_cast<int>(GalleryShell::pages().size());
        for (int i = n - 1; i >= 0; --i)
            if (!groupCollapsed(i))
                return i;
        return n - 1;
    }
    /// 方向键/PgUp/PgDn 切换侧栏选中项（折叠分组内的页不可选）。返回 Handled。
    KeyEventResult handleSidebarKey(const KeyEvent& ev) {
        const double vp = sidebarScroll_.viewportExtent();
        const int pageStep = std::max(1, static_cast<int>(vp / kItemH) - 1);
        int next = selected_;
        switch (static_cast<KeyCode>(ev.key)) {
            case KeyCode::Down:    next = nextVisiblePage(selected_ + 1); break;
            case KeyCode::Up:      next = prevVisiblePage(selected_ - 1); break;
            case KeyCode::PageDown: next = nextVisiblePage(selected_ + pageStep); break;
            case KeyCode::PageUp:  next = prevVisiblePage(selected_ - pageStep); break;
            default:               return KeyEventResult::Ignored;
        }
        if (next != selected_)
            gotoPage(next);  // 内部已 setState + 把选中项滚入可见区
        return KeyEventResult::Handled;
    }
};

std::unique_ptr<State> GalleryShell::createState() {
    return std::make_unique<GalleryShellState>();
}

}  // namespace story
}  // namespace cui_core

// ── makeGalleryShell：从 gallery_host.cpp 迁移过来。
//    实现需要 story::GalleryShell（以及 Overlay / Column / Text 等）的
//    完整定义，而这些现在只在本 .cpp 内可见（薄头文件不再暴露它们），
//    故把该工厂函数的定义放在这里最合适。
namespace cui {
SharedPtr<Widget> makeGalleryShell() {
    // 冒烟开关：CUI_MINIMAL=1 用最小树验证挂载本身。
    if (std::getenv("CUI_MINIMAL")) {
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<Text>("minimal bridge ok")});
        col->setMainAxisSize(MainAxisSize::Min);
        return col;
    }
    // 给整棵应用套一层 app-root Overlay：工具条里的 PopupMenuButton（配置下拉）
    // 位于 IDE 外壳 chrome，不在 dock 内容区那个 Navigator/Overlay 之下，之前
    // 找不到 Overlay 会抛 "No Overlay widget found"。这里把 shell 作为唯一条目
    // 挂进去。shell 实例被捕获进 entry builder ⇒ Overlay 因弹层 insert/remove
    // 重建时返回同一个 widget，Element/State 复用，不会重置外壳状态。
    auto shell = cui::make_shared<story::GalleryShell>();
    auto entry = cui::make_shared<OverlayEntry>(
        [shell](BuildContext&) { return shell; }, /*opaque=*/true, /*maintainState=*/true);
    return cui::make_shared<Overlay>(std::vector{std::move(entry)});
}
}  // namespace cui_core
