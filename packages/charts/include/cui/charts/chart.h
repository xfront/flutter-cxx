#pragma once
/// @file chart.h
/// @brief 图表控件集（折线 / 柱状 / 饼图 / 散点 / 气泡 / 雷达 / 瀑布 / 直方 …）。
///
/// 数据类命名沿用上游图表库的习惯（ChartGridData / ChartTitlesData /
/// ChartBorderData / LineChartData / BarChartData / PieChartData /
/// ScatterChartData / RadarChartData …），全部由控件体系自绘实现，
/// 不依赖任何外部绘图后端。
///
/// 支持的图表：Line / Bar / Pie / Scatter / Radar。
///
/// 仅保留 ≤6 行的小工具函数为 inline；绘制与构建实现见 src/widgets/chart.cpp。

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"

namespace cui {
namespace chart {

// ═══════════════════════════════════════════════════════════════════════════
// 共享类型（对齐 chart 的 base / chart 基类）
// ═══════════════════════════════════════════════════════════════════════════

struct ChartSpot {
    double x = 0.0;
    double y = 0.0;
    ChartSpot() = default;
    ChartSpot(double x_, double y_) : x(x_), y(y_) {}
};

/// 一条线（网格线 / 边框线 / 分隔线）。
struct ChartLine {
    Color color = theme().divider;
    double strokeWidth = 1.0;
    std::vector<double> dashArray;  ///< 空 = 实线；否则 [dash, gap, ...]
};

struct BorderSide {
    Color color = theme().border;
    double width = 1.0;
};

struct ChartBorderData {
    bool show = true;
    BorderSide border;
};

struct ChartGridData {
    bool show = true;
    ChartLine horizontalLine;
    ChartLine verticalLine;
    int horizontalInterval = 0;  ///< 0 = 自动（= 5 等分）
    int verticalInterval = 0;
};

/// 坐标轴一侧的标题。getTitles(value) 把数据坐标映射成文字标签。
struct SideTitles {
    bool showTitles = false;
    double reservedSize = 24.0;
    double textSize = 11.0;
    Color textColor = theme().textSecondary;
    std::function<std::string(double)> getTitles;
};

struct ChartTitlesData {
    bool show = true;
    SideTitles left;
    SideTitles bottom;
    SideTitles top;
    SideTitles right;
    ChartTitlesData() {
        left.showTitles = true;
        bottom.showTitles = true;
        top.showTitles = false;
        right.showTitles = false;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// 绘制辅助（>6 行的实现在 chart.cpp）
// ═══════════════════════════════════════════════════════════════════════════

enum class ChartTextAlign { Left, Center, Right };

void chartDrawText(Painter& p, const std::string& s, double x, double baselineY,
                double size, const Color& c, ChartTextAlign align = ChartTextAlign::Left);

void chartStrokeDashed(Painter& p, double x1, double y1, double x2, double y2,
                    const std::vector<double>& dash);

inline double chartMapX(double x, double minX, double maxX, const Rect& a) {
    if (maxX - minX < 1e-9) return a.x + a.width / 2.0;
    return a.x + (x - minX) / (maxX - minX) * a.width;
}
inline double chartMapY(double y, double minY, double maxY, const Rect& a) {
    if (maxY - minY < 1e-9) return a.y + a.height / 2.0;
    return a.bottom() - (y - minY) / (maxY - minY) * a.height;
}

/// 计算绘图区（扣除四向标题预留空间）。
Rect chartArea(const Size& size, const ChartTitlesData& t);

void chartDrawGrid(Painter& p, const Rect& a, const ChartGridData& g,
                double minX, double maxX, double minY, double maxY,
                int nX, int nY);

void chartDrawBorder(Painter& p, const Rect& a, const ChartBorderData& b);

inline std::string chartDefaultLabel(double v) {
    if (std::abs(v - std::round(v)) < 1e-6) return std::to_string((int)std::round(v));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf);
}

void chartDrawTitles(Painter& p, const Rect& a, const ChartTitlesData& t,
                  double minX, double maxX, double minY, double maxY,
                  int nX, int nY, bool bottomIsIndex = false);

// ═══════════════════════════════════════════════════════════════
// 交互层（对齐 rui：hover 命中 → tooltip；Donut 悬停动画）
// ═══════════════════════════════════════════════════════════════

/// 图表触摸命中结果：hover 到某个元素时由 hit-test 填充，供绘制高亮/tooltip。
struct ChartTouch {
    bool active = false;     ///< 是否命中元素
    int series = -1;         ///< 系列下标（line/scatter/bar）
    int index = -1;          ///< 元素下标（点 / 扇区 / 柱）
    double px = 0, py = 0;   ///< 命中元素在图表本地坐标的像素位置
    std::string title;       ///< tooltip 主标题（系列名 / 扇区名）
    std::string value;       ///< tooltip 数值文本
    Color color = Color::fromRGB(0.25, 0.55, 0.95);  ///< 高亮 / 强调色
};

/// 泛型交互包装：在纯绘制图表之上叠加 hover 命中 + tooltip + 悬停动画。
/// 由各个 chart 图表的 build() 构造（paint 负责绘制，hit 负责命中测试）。
class ChartHost : public StatefulWidget {
public:
    using PaintFn = std::function<void(Painter&, const Size&, const ChartTouch&, double progress)>;
    using HitFn = std::function<ChartTouch(double x, double y)>;

    ChartHost(PaintFn paint, HitFn hit, Size size,
           Color accent = Color::fromRGB(0.25, 0.55, 0.95), KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), paint_(std::move(paint)),
          hit_(std::move(hit)), size_(size), accent_(accent) {}

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ChartHost"; }

    const PaintFn& paint() const { return paint_; }
    const HitFn& hit() const { return hit_; }
    const Size& size() const { return size_; }
    const Color& accent() const { return accent_; }

private:
    PaintFn paint_;
    HitFn hit_;
    Size size_;
    Color accent_;
};

// Catmull-Rom 平滑采样成折线（Painter 无贝塞尔，用细分线段近似）。
void chartCatmullRom(const std::vector<Point>& pts, int steps, std::vector<Point>& out);

// 阶梯线：相邻点之间先水平延伸到下一 x（保持上一 y），再垂直落到下一 y。
void chartStepLine(const std::vector<Point>& pts, std::vector<Point>& out);

// ═══════════════════════════════════════════════════════════════════════════
// LineChart
// ═══════════════════════════════════════════════════════════════════════════

struct LineChartBarData {
    std::vector<ChartSpot> spots;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
    double width = 2.5;
    bool isCurved = false;
    bool isStep = false;     ///< 阶梯线：每点先水平到下一 x、再垂直到 y
    bool dotData = true;
    double dotRadius = 3.0;
    bool showBelow = false;
    Color belowColor = Color::fromRGBA(0.25, 0.55, 0.95, 0.15);
};

struct LineChartData {
    std::vector<LineChartBarData> lineBarsData;
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;  ///< 0 区间 = 自动
};

void chartPaintLine(Painter& p, const Size& size, const LineChartData& d,
                      const ChartTouch* touch = nullptr, double progress = 0.0);

class LineChart : public StatelessWidget {
public:
    explicit LineChart(LineChartData data, Size size = Size(360.0, 220.0),
                       KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}

    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "LineChart"; }

private:
    LineChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// BarChart
// ═══════════════════════════════════════════════════════════════════════════

struct BarChartRodData {
    double toY = 0.0;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
    double width = 16.0;
    double borderRadius = 0.0;
};

struct BarChartGroupData {
    double x = 0.0;
    std::vector<BarChartRodData> barRods;
    double barsSpace = 2.0;
};

struct BarChartData {
    std::vector<BarChartGroupData> groups;
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    double maxY = 0.0;      ///< 0 = 自动
    bool stacked = false;   ///< true = 组内 rods 堆叠
};

void chartPaintBar(Painter& p, const Size& size, const BarChartData& d,
                     const ChartTouch* touch = nullptr, double progress = 0.0);

class BarChart : public StatelessWidget {
public:
    explicit BarChart(BarChartData data, Size size = Size(360.0, 220.0),
                      KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}

    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "BarChart"; }

private:
    BarChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// PieChart
// ═══════════════════════════════════════════════════════════════════════════

struct PieChartSectionData {
    double value = 0.0;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
    double radius = 50.0;
    std::string title;
    double titleSize = 12.0;
    Color titleColor = Color::fromRGB(1.0, 1.0, 1.0);
};

struct PieChartData {
    std::vector<PieChartSectionData> sections;
    double centerSpaceRadius = 0.0;  ///< >0 = 甜甜圈
    double sectionsSpace = 2.0;      ///< 扇区间空隙（角度，度）
};

void chartPaintPie(Painter& p, const Size& size, const PieChartData& d,
                     const ChartTouch* touch = nullptr, double progress = 0.0);

class PieChart : public StatelessWidget {
public:
    explicit PieChart(PieChartData data, Size size = Size(220.0, 220.0),
                      KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}

    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "PieChart"; }

private:
    PieChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// ScatterChart
// ═══════════════════════════════════════════════════════════════════════════

struct ScatterChartSpot {
    double x = 0.0, y = 0.0;
    double radius = 4.0;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
};

struct ScatterChartData {
    std::vector<ScatterChartSpot> spots;
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;  ///< 0 区间 = 自动
};

void chartPaintScatter(Painter& p, const Size& size, const ScatterChartData& d,
                         const ChartTouch* touch = nullptr, double progress = 0.0);

class ScatterChart : public StatelessWidget {
public:
    explicit ScatterChart(ScatterChartData data, Size size = Size(360.0, 220.0),
                          KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}

    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "ScatterChart"; }

private:
    ScatterChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// RadarChart
// ═══════════════════════════════════════════════════════════════════════════

struct RadarDataSet {
    std::string name;
    std::vector<double> values;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
    double fillAlpha = 0.15;
};

struct RadarChartFeature {
    std::string name;
    double max = 100.0;
};

struct RadarChartData {
    std::vector<RadarDataSet> dataSets;
    std::vector<RadarChartFeature> features;
};

void chartPaintRadar(Painter& p, const Size& size, const RadarChartData& d,
                       const ChartTouch* touch = nullptr, double progress = 0.0);

class RadarChart : public StatelessWidget {
public:
    explicit RadarChart(RadarChartData data, Size size = Size(320.0, 320.0),
                        KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}

    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "RadarChart"; }

private:
    RadarChartData data_;
    Size size_;
};

// 共享调色板（与 legacy Chart 一致）。
inline const Color& chartPalette(int i) {
    static const Color k[] = {
        Color::fromRGB(0.25, 0.55, 0.95), Color::fromRGB(0.20, 0.75, 0.45),
        Color::fromRGB(0.95, 0.65, 0.15), Color::fromRGB(0.90, 0.35, 0.30),
        Color::fromRGB(0.60, 0.35, 0.85), Color::fromRGB(0.35, 0.70, 0.80)};
    return k[((i % 6) + 6) % 6];
}

// ═══════════════════════════════════════════════════════════════════════════
// BubbleChart（气泡图：尺寸映射半径）
// ═══════════════════════════════════════════════════════════════════════════

struct BubbleSpot {
    double x = 0.0, y = 0.0;
    double size = 1.0;  ///< 相对量级；半径 = sqrt(size/maxSize) * maxBubbleRadius
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
};

struct BubbleChartData {
    std::vector<BubbleSpot> spots;
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;  ///< 0 区间 = 自动
    double maxBubbleRadius = 30.0;
};

void chartPaintBubble(Painter& p, const Size& size, const BubbleChartData& d,
                        const ChartTouch* touch = nullptr, double progress = 0.0);

class BubbleChart : public StatelessWidget {
public:
    explicit BubbleChart(BubbleChartData data, Size size = Size(360.0, 220.0),
                         KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "BubbleChart"; }

private:
    BubbleChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// WaterfallChart（瀑布图：累计条形）
// ═══════════════════════════════════════════════════════════════════════════

struct WaterfallItem {
    std::string label;
    double value = 0.0;
    bool isTotal = false;          ///< 总计条（从 0 起），否则为增减量
    Color color{0, 0, 0, 0};        ///< 透明 = 按增减自动配色
};

struct WaterfallChartData {
    std::vector<WaterfallItem> items;
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    Color increaseColor = Color::fromRGB(0.20, 0.75, 0.45);
    Color decreaseColor = Color::fromRGB(0.90, 0.35, 0.30);
    Color totalColor = Color::fromRGB(0.25, 0.55, 0.95);
};

void chartPaintWaterfall(Painter& p, const Size& size, const WaterfallChartData& d,
                           const ChartTouch* touch = nullptr, double progress = 0.0);

class WaterfallChart : public StatelessWidget {
public:
    explicit WaterfallChart(WaterfallChartData data, Size size = Size(360.0, 240.0),
                            KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "WaterfallChart"; }

private:
    WaterfallChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// HistogramChart（直方图：自动分箱）
// ═══════════════════════════════════════════════════════════════════════════

struct HistogramChartData {
    std::vector<double> samples;
    int binCount = 0;            ///< 0 = 自动（round(sqrt(n))）
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
};

void chartPaintHistogram(Painter& p, const Size& size, const HistogramChartData& d,
                           const ChartTouch* touch = nullptr, double progress = 0.0);

class HistogramChart : public StatelessWidget {
public:
    explicit HistogramChart(HistogramChartData data, Size size = Size(360.0, 220.0),
                            KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "HistogramChart"; }

private:
    HistogramChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// CandlestickChart（K 线 / OHLC）
// ═══════════════════════════════════════════════════════════════════════════

struct CandleData {
    double x = 0.0;     ///< 位置（按索引或日期值映射）
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
};

struct CandlestickChartData {
    std::vector<CandleData> candles;
    ChartGridData gridData;
    ChartTitlesData titlesData;
    ChartBorderData borderData;
    double candleWidth = 10.0;
    Color bullishColor = Color::fromRGB(0.20, 0.75, 0.45);  ///< 收 >= 开
    Color bearishColor = Color::fromRGB(0.90, 0.35, 0.30);  ///< 收 < 开
};

void chartPaintCandlestick(Painter& p, const Size& size, const CandlestickChartData& d);

class CandlestickChart : public StatelessWidget {
public:
    explicit CandlestickChart(CandlestickChartData data, Size size = Size(360.0, 220.0),
                              KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "CandlestickChart"; }

private:
    CandlestickChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// SankeyChart（桑基图：能量流）
// ═══════════════════════════════════════════════════════════════════════════

struct SankeyNodeData {
    std::string label;
    double value = 0.0;
    Color color{0, 0, 0, 0};  ///< 透明 = 自动调色板
};

struct SankeyLinkData {
    int from = 0;
    int to = 0;
    double value = 0.0;
};

struct SankeyChartData {
    std::vector<SankeyNodeData> nodes;
    std::vector<SankeyLinkData> links;
    double nodeWidth = 14.0;
    double nodeGap = 12.0;
};

void chartPaintSankey(Painter& p, const Size& size, const SankeyChartData& d);

class SankeyChart : public StatelessWidget {
public:
    explicit SankeyChart(SankeyChartData data, Size size = Size(420.0, 260.0),
                         KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "SankeyChart"; }

private:
    SankeyChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// PolarChart（极坐标折线 / 面积）
// ═══════════════════════════════════════════════════════════════════════════

struct PolarDataSet {
    std::string name;
    std::vector<double> values;  ///< 每个类别一个半径值
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
    double fillAlpha = 0.18;
    bool showArea = true;
};

struct PolarChartData {
    std::vector<PolarDataSet> dataSets;
    std::vector<std::string> categories;  ///< 角度轴类别标签
    double maxValue = 0.0;                 ///< 0 = 自动
};

void chartPaintPolar(Painter& p, const Size& size, const PolarChartData& d);

class PolarChart : public StatelessWidget {
public:
    explicit PolarChart(PolarChartData data, Size size = Size(360.0, 320.0),
                        KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "PolarChart"; }

private:
    PolarChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// RadialBarChart（环形进度条：每数据集一个环）
// ═══════════════════════════════════════════════════════════════════════════

struct RadialBarDataSet {
    std::string name;
    double value = 0.0;
    double max = 100.0;
    Color color = Color::fromRGB(0.25, 0.55, 0.95);
};

struct RadialBarChartData {
    std::vector<RadialBarDataSet> dataSets;
    double startAngle = -90.0;   ///< 起始角（度，0=3 点方向）
    double ringWidth = 12.0;
    double ringGap = 6.0;
};

void chartPaintRadialBar(Painter& p, const Size& size, const RadialBarChartData& d);

class RadialBarChart : public StatelessWidget {
public:
    explicit RadialBarChart(RadialBarChartData data, Size size = Size(300.0, 300.0),
                            KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "RadialBarChart"; }

private:
    RadialBarChartData data_;
    Size size_;
};

// ═══════════════════════════════════════════════════════════════════════════
// FunnelChart / PyramidChart（漏斗 / 金字塔）
// ═══════════════════════════════════════════════════════════════════════════

struct FunnelSlice {
    std::string label;
    double value = 0.0;
    Color color{0, 0, 0, 0};  ///< 透明 = 自动调色板
};

struct FunnelChartData {
    std::vector<FunnelSlice> slices;
};

/// @param pyramid true = 居中对称金字塔条；false = 梯形收窄漏斗。
void chartPaintFunnel(Painter& p, const Size& size, const FunnelChartData& d,
                        bool pyramid);

class FunnelChart : public StatelessWidget {
public:
    explicit FunnelChart(FunnelChartData data, Size size = Size(320.0, 260.0),
                         KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "FunnelChart"; }

private:
    FunnelChartData data_;
    Size size_;
};

class PyramidChart : public StatelessWidget {
public:
    explicit PyramidChart(FunnelChartData data, Size size = Size(320.0, 260.0),
                          KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), data_(std::move(data)), size_(size) {}
    SharedPtr<Widget> build(BuildContext&) override;
    std::string toStringShort() const override { return "PyramidChart"; }

private:
    FunnelChartData data_;
    Size size_;
};

}  // namespace chart
}  // namespace cui
