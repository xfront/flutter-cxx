#pragma once
/// @file rendering/render_icon.h
/// @brief 矢量图标渲染对象（上游的 Icon + MaterialIcons 方案）。
///
/// 图标字体需要字体文件与本.setup；这里用**程序化矢量路径**替代：
/// 每个图标在 24×24 视口内以 SkPath 描述（对齐 Material Icons 的
/// 设计网格），绘制时按 size/24 缩放。填充与描边路径分开存储，
/// 一次绘制可同时含两者（如 Settings）。
///
/// 对应 上游源码：widgets/icon.dart 的 `RenderIcon`（本实现为
/// `RenderBox` 版本，上游 用 RichText 方案绘制字体图标）。

#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/rendering/render_box.h"

namespace cui {

/// 图标编号（Material Icons 常用子集）。24×24 网格。
enum class IconCode {
    None,
    Check,           ///< ✓
    Close,           ///< ✕
    Add,             ///< ＋
    Remove,          ///< －
    ArrowBack,       ///< ←
    ArrowForward,    ///< →
    ArrowUpward,     ///< ↑
    ArrowDownward,   ///< ↓
    ExpandMore,      ///< ⌄（上游 Icons.expand_more）
    Menu,            ///< ☰
    Search,          ///< 🔍
    Home,            ///< 房子
    Favorite,        ///< ♥（填充）
    Star,            ///< ★（填充）
    Play,            ///< ▶（填充）
    Pause,           ///< ⏸（填充）
    MoreVert,        ///< ⋮
    MoreHoriz,       ///< ⋯
    Delete,          ///< 垃圾桶
    Person,          ///< 人像
    Settings,        ///< 齿轮简化（圆环+圆点）

    // ── 语义图标（控件族，24×24 网格）────────────────────────────────────
    Button,          ///< 圆角按钮
    Toggle,          ///< 开关（轨道 + 滑点）
    Radio,           ///< 单选圆
    Checkbox,        ///< 复选框
    Dropdown,        ///< 下拉框 + 箭头
    Slider,          ///< 滑轨 + 滑块
    Text,            ///< 文本 T
    Input,           ///< 输入框
    Table,           ///< 表格
    List,            ///< 列表
    Grid,            ///< 网格
    Chart,           ///< 柱状图
    Image,           ///< 图片
    Dialog,          ///< 对话框窗口
    Bell,            ///< 铃铛
    Palette,         ///< 调色板
    Layers,          ///< 层叠
    Refresh,         ///< 刷新
    Tag,             ///< 标签
    Gesture,         ///< 手势指针
    Folder,          ///< 文件夹
    Clock,           ///< 时钟

    // ── 动作图标（FAB 菜单/工具栏常用，24×24 网格描边）──────────────────
    Edit,            ///< 铅笔（编辑）
    Save,            ///< 软盘（保存）
    Share,           ///< 上箭头 + 托盘（分享/上传）
    PushPin,         ///< 图钉（上游 Icons.push_pin，钉住/固定）

    // ── 媒体播放图标（VCL 风格控制条，24×24 网格）───────────────────────
    Stop,            ///< ⏹（填充方块）
    SkipPrevious,    ///< ⏮（左竖条 + 左三角）
    SkipNext,        ///< ⏭（右三角 + 右竖条）
    Rewind,          ///< ⏪（两个左三角）
    FastForward,     ///< ⏩（两个右三角）
    VolumeUp,        ///< 🔊（喇叭 + 两条弧线）
    VolumeMute,      ///< 🔇（喇叭 + 叉）
    PhotoCamera,     ///< 📷（相机，快照）
};

/// 矢量图标渲染对象。
class RenderIcon : public RenderBox {
public:
    RenderIcon(IconCode code, double size, Color color)
        : code_(code), iconSize_(size), color_(color) {}

    IconCode code() const { return code_; }
    void setCode(IconCode c) {
        if (c != code_) {
            code_ = c;
            markNeedsPaint();
        }
    }
    double iconSize() const { return iconSize_; }
    void setIconSize(double s) {
        if (s != iconSize_) {
            iconSize_ = s;
            markNeedsLayout();
        }
    }
    Color color() const { return color_; }
    void setColor(Color c) {
        if (c != color_) {
            color_ = c;
            markNeedsPaint();
        }
    }

    double getDistanceToBaseline(BaselineType) const override { return iconSize_; }

protected:
    void performLayout() override {
        double s = iconSize_;
        if (constraints_.hasBoundedWidth())
            s = std::min(s, constraints_.maxWidth);
        if (constraints_.hasBoundedHeight())
            s = std::min(s, constraints_.maxHeight);
        setSize(Size(s, s));
    }
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    IconCode code_;
    double iconSize_;
    Color color_;
};

}  // namespace cui_core
