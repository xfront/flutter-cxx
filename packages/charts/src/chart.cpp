/// @file chart.cpp
/// @brief chart.h 中 >6 行函数体的实现（绘制 + 控件 build）。

#include "cui/charts/chart.h"

#include <cstdio>
#include <queue>

#include "cui/widgets/animation/shared_animation.h"
#include "cui/widgets/paint.h"  // CustomPaint
#include "cui/framework/listener.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {
namespace chart {

// ═══════════════════════════════════════════════════════════════════════════
// 绘制辅助
// ═══════════════════════════════════════════════════════════════════════════

void chartDrawText(Painter& p, const std::string& s, double x, double baselineY,
                double size, const Color& c, ChartTextAlign align) {
    if (s.empty()) return;
    p.setFontSize(size);
    p.setFontBold(false);
    p.setSourceColor(c);
    double w = Painter::measureTextWidth(nullptr, s, size);
    double tx = x;
    if (align == ChartTextAlign::Center) tx = x - w / 2.0;
    else if (align == ChartTextAlign::Right) tx = x - w;
    p.save();
    p.translate(tx, baselineY - size);  // showText 基线落在 fontSize 处
    p.showText(s.c_str());
    p.restore();
}

void chartStrokeDashed(Painter& p, double x1, double y1, double x2, double y2,
                    const std::vector<double>& dash) {
    double len = std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    if (len < 1e-6) return;
    double ux = (x2 - x1) / len, uy = (y2 - y1) / len;
    if (dash.empty()) {
        p.moveTo(x1, y1);
        p.lineTo(x2, y2);
        p.stroke();
        return;
    }
    double period = 0.0;
    for (double d : dash) period += d;
    if (period <= 0) period = 1.0;
    double dist = 0.0; int idx = 0;
    while (dist < len) {
        double seg = dash[idx % dash.size()];
        double d = std::min(seg, len - dist);
        bool draw = (idx % 2 == 0);  // 偶数段 = 实线
        if (draw) {
            p.moveTo(x1 + ux * dist, y1 + uy * dist);
            p.lineTo(x1 + ux * (dist + d), y1 + uy * (dist + d));
            p.stroke();
        }
        dist += d;
        ++idx;
    }
}

Rect chartArea(const Size& size, const ChartTitlesData& t) {
    double padL = t.left.showTitles ? t.left.reservedSize : 10.0;
    double padB = t.bottom.showTitles ? t.bottom.reservedSize : 10.0;
    double padT = t.top.showTitles ? t.top.reservedSize : 10.0;
    double padR = t.right.showTitles ? t.right.reservedSize : 10.0;
    return Rect(padL, padT, std::max(1.0, size.width - padL - padR),
                std::max(1.0, size.height - padT - padB));
}

void chartDrawGrid(Painter& p, const Rect& a, const ChartGridData& g,
                double minX, double maxX, double minY, double maxY,
                int nX, int nY) {
    if (!g.show) return;
    auto lineAt = [&](double x1, double y1, double x2, double y2, const ChartLine& ln) {
        p.setSourceColor(ln.color);
        p.setLineWidth(ln.strokeWidth);
        chartStrokeDashed(p, x1, y1, x2, y2, ln.dashArray);
    };
    for (int i = 0; i <= nY; ++i) {
        double y = a.y + (double)i / (double)nY * a.height;
        lineAt(a.x, y, a.right(), y, g.horizontalLine);
    }
    for (int i = 0; i <= nX; ++i) {
        double x = a.x + (double)i / (double)nX * a.width;
        lineAt(x, a.y, x, a.bottom(), g.verticalLine);
    }
}

void chartDrawBorder(Painter& p, const Rect& a, const ChartBorderData& b) {
    if (!b.show) return;
    p.setSourceColor(b.border.color);
    p.setLineWidth(b.border.width);
    p.rectangle(a);
    p.stroke();
}

void chartDrawTitles(Painter& p, const Rect& a, const ChartTitlesData& t,
                  double minX, double maxX, double minY, double maxY,
                  int nX, int nY, bool bottomIsIndex) {
    if (!t.show) return;
    if (t.bottom.showTitles) {
        for (int i = 0; i <= nX; ++i) {
            double val = minX + (maxX - minX) * (double)i / (double)nX;
            std::string s = t.bottom.getTitles ? t.bottom.getTitles(val) : chartDefaultLabel(val);
            double px = chartMapX(val, minX, maxX, a);
            chartDrawText(p, s, px, a.bottom() + 4.0, t.bottom.textSize, t.bottom.textColor,
                       ChartTextAlign::Center);
        }
    }
    if (t.left.showTitles) {
        for (int i = 0; i <= nY; ++i) {
            double val = minY + (maxY - minY) * (double)i / (double)nY;
            std::string s = t.left.getTitles ? t.left.getTitles(val) : chartDefaultLabel(val);
            double py = chartMapY(val, minY, maxY, a);
            chartDrawText(p, s, a.x - 4.0, py, t.left.textSize, t.left.textColor,
                       ChartTextAlign::Right);
        }
    }
}

void chartCatmullRom(const std::vector<Point>& pts, int steps, std::vector<Point>& out) {
    out.clear();
    if (pts.size() < 2) { out = pts; return; }
    auto cr = [](const Point& p0, const Point& p1, const Point& p2, const Point& p3, double t) {
        double t2 = t * t, t3 = t2 * t;
        double x = 0.5 * (2 * p1.x + (-p0.x + p2.x) * t +
                          (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                          (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
        double y = 0.5 * (2 * p1.y + (-p0.y + p2.y) * t +
                          (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                          (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
        return Point{x, y};
    };
    out.push_back(pts.front());
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Point p0 = (i == 0) ? pts[i] : pts[i - 1];
        Point p1 = pts[i];
        Point p2 = pts[i + 1];
        Point p3 = (i + 2 < pts.size()) ? pts[i + 2] : pts.back();
        for (int s = 1; s <= steps; ++s) {
            double t = (double)s / (double)steps;
            out.push_back(cr(p0, p1, p2, p3, t));
        }
    }
}

void chartStepLine(const std::vector<Point>& pts, std::vector<Point>& out) {
    out.clear();
    if (pts.size() < 2) { out = pts; return; }
    out.push_back(pts.front());
    for (size_t i = 1; i < pts.size(); ++i) {
        out.push_back(Point{pts[i].x, pts[i - 1].y});  // 水平到下一 x
        out.push_back(pts[i]);                          // 垂直到下一 y
    }
}

// ═══════════════════════════════════════════════════════════════
// 交互层：命中测试 + tooltip 绘制（对齐 rui：hover 命中 → tooltip；
// Donut 悬停时扇区 explode 动画由 ChartHostState 的 SharedAnimation 驱动）
// ═══════════════════════════════════════════════════════════════

static void chartLineRanges(const LineChartData& d, const Size& size, Rect& a,
                         double& minX, double& maxX, double& minY, double& maxY) {
    minX = d.minX; maxX = d.maxX; minY = d.minY; maxY = d.maxY;
    if (maxX - minX < 1e-9) {
        minX = minY = std::numeric_limits<double>::infinity();
        maxX = maxY = -std::numeric_limits<double>::infinity();
        for (const auto& bar : d.lineBarsData)
            for (const auto& s : bar.spots) {
                minX = std::min(minX, s.x); maxX = std::max(maxX, s.x);
                minY = std::min(minY, s.y); maxY = std::max(maxY, s.y);
            }
        if (!std::isfinite(minX)) { minX = 0; maxX = 1; minY = 0; maxY = 1; }
    }
    if (minY > 0) minY = 0;
    a = chartArea(size, d.titlesData);
}

ChartTouch chartHitLine(double x, double y, const LineChartData& d, const Size& size) {
    ChartTouch res;
    if (d.lineBarsData.empty()) return res;
    Rect a; double minX, maxX, minY, maxY;
    chartLineRanges(d, size, a, minX, maxX, minY, maxY);
    double best = 16.0 * 16.0;
    for (size_t si = 0; si < d.lineBarsData.size(); ++si) {
        const auto& bar = d.lineBarsData[si];
        for (size_t pi = 0; pi < bar.spots.size(); ++pi) {
            const auto& s = bar.spots[pi];
            double px = chartMapX(s.x, minX, maxX, a);
            double py = chartMapY(s.y, minY, maxY, a);
            double dd = (px - x) * (px - x) + (py - y) * (py - y);
            if (dd < best) {
                best = dd;
                res.active = true; res.series = (int)si; res.index = (int)pi;
                res.px = px; res.py = py; res.color = bar.color;
                res.value = "x: " + chartDefaultLabel(s.x) + ", y: " + chartDefaultLabel(s.y);
            }
        }
    }
    return res;
}

ChartTouch chartHitBar(double x, double y, const BarChartData& d, const Size& size) {
    ChartTouch res;
    if (d.groups.empty()) return res;
    double minY = 0.0, maxY = d.maxY;
    if (maxY <= 0) {
        for (const auto& g : d.groups) {
            double tot = 0.0;
            for (const auto& r : g.barRods) {
                if (d.stacked) tot += r.toY; else tot = std::max(tot, r.toY);
            }
            maxY = std::max(maxY, tot);
        }
        if (maxY <= 0) maxY = 1;
    }
    Rect a = chartArea(size, d.titlesData);
    if (x < a.x || x > a.right()) return res;
    double slot = a.width / (double)d.groups.size();
    size_t gi = (size_t)((x - a.x) / slot);
    if (gi >= d.groups.size()) return res;
    const auto& g = d.groups[gi];
    size_t n = g.barRods.size();
    double totalW = 0.0; for (const auto& r : g.barRods) totalW += r.width;
    totalW += g.barsSpace * (n > 0 ? n - 1 : 0);
    double cx = a.x + (slot - totalW) / 2.0;
    double acc = 0.0;
    for (size_t ri = 0; ri < g.barRods.size(); ++ri) {
        const auto& r = g.barRods[ri];
        double y0, y1, val;
        if (d.stacked) { y0 = chartMapY(acc, minY, maxY, a); y1 = chartMapY(acc + r.toY, minY, maxY, a); val = acc + r.toY; acc += r.toY; }
        else { y0 = chartMapY(0.0, minY, maxY, a); y1 = chartMapY(r.toY, minY, maxY, a); val = r.toY; }
        Rect bar(cx, std::min(y0, y1), r.width, std::fabs(y1 - y0));
        if (x >= bar.x && x <= bar.right() && y >= bar.y && y <= bar.bottom()) {
            res.active = true; res.series = (int)gi; res.index = (int)ri;
            res.px = bar.x + bar.width / 2.0; res.py = bar.y + bar.height / 2.0;
            res.color = r.color;
            res.title = d.titlesData.bottom.getTitles ? d.titlesData.bottom.getTitles(g.x) : chartDefaultLabel(g.x);
            res.value = "value: " + chartDefaultLabel(val);
            return res;
        }
        cx += r.width + g.barsSpace;
    }
    return res;
}

ChartTouch chartHitPie(double x, double y, const PieChartData& d, const Size& size) {
    ChartTouch res;
    if (d.sections.empty()) return res;
    Point c{size.width / 2.0, size.height / 2.0};
    double maxR = std::min(size.width, size.height) / 2.0 - 6.0;
    double inner = d.centerSpaceRadius;
    double total = 0.0; for (const auto& s : d.sections) total += s.value;
    if (total <= 0) return res;
    double dx = x - c.x, dy = y - c.y;
    double r = std::sqrt(dx * dx + dy * dy);
    if (r > maxR + 2.0) return res;
    if (inner > 0 && r < inner - 2.0) return res;
    double ang = std::atan2(dy, dx) * 180.0 / M_PI;
    while (ang < -90.0) ang += 360.0;
    while (ang >= 270.0) ang -= 360.0;
    double cur = -90.0;
    for (size_t i = 0; i < d.sections.size(); ++i) {
        const auto& s = d.sections[i];
        double sweep = s.value / total * 360.0;
        double a0 = cur + d.sectionsSpace / 2.0;
        double a1 = cur + sweep - d.sectionsSpace / 2.0;
        if (ang >= a0 && ang <= a1) {
            double mid = (a0 + a1) / 2.0 * M_PI / 180.0;
            double tr = (maxR + inner) / 2.0;
            res.active = true; res.index = (int)i;
            res.px = c.x + std::cos(mid) * tr;
            res.py = c.y + std::sin(mid) * tr;
            res.color = s.color; res.title = s.title;
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.1f%%", s.value / total * 100.0);
            res.value = buf;
            return res;
        }
        cur += sweep;
    }
    return res;
}

template <typename D>
static void chartPointRanges(const D& d, const Size& size, Rect& a,
                          double& minX, double& maxX, double& minY, double& maxY) {
    minX = d.minX; maxX = d.maxX; minY = d.minY; maxY = d.maxY;
    if (maxX - minX < 1e-9) {
        minX = minY = std::numeric_limits<double>::infinity();
        maxX = maxY = -std::numeric_limits<double>::infinity();
        for (const auto& s : d.spots) { minX = std::min(minX, s.x); maxX = std::max(maxX, s.x); minY = std::min(minY, s.y); maxY = std::max(maxY, s.y); }
        if (!std::isfinite(minX)) { minX = 0; maxX = 1; minY = 0; maxY = 1; }
    }
    a = chartArea(size, d.titlesData);
}

ChartTouch chartHitScatter(double x, double y, const ScatterChartData& d, const Size& size) {
    ChartTouch res;
    if (d.spots.empty()) return res;
    Rect a; double minX, maxX, minY, maxY;
    chartPointRanges(d, size, a, minX, maxX, minY, maxY);
    double best = 1e18;
    for (size_t i = 0; i < d.spots.size(); ++i) {
        const auto& s = d.spots[i];
        double px = chartMapX(s.x, minX, maxX, a), py = chartMapY(s.y, minY, maxY, a);
        double thr = std::max(s.radius + 8.0, 12.0);
        double dd = (px - x) * (px - x) + (py - y) * (py - y);
        if (dd < thr * thr && dd < best) {
            best = dd; res.active = true; res.index = (int)i;
            res.px = px; res.py = py; res.color = s.color;
            res.value = "x: " + chartDefaultLabel(s.x) + ", y: " + chartDefaultLabel(s.y);
        }
    }
    return res;
}

ChartTouch chartHitBubble(double x, double y, const BubbleChartData& d, const Size& size) {
    ChartTouch res;
    if (d.spots.empty()) return res;
    Rect a; double minX, maxX, minY, maxY;
    chartPointRanges(d, size, a, minX, maxX, minY, maxY);
    double best = 1e18;
    for (size_t i = 0; i < d.spots.size(); ++i) {
        const auto& s = d.spots[i];
        double px = chartMapX(s.x, minX, maxX, a), py = chartMapY(s.y, minY, maxY, a);
        double thr = std::max(8.0, 14.0);
        double dd = (px - x) * (px - x) + (py - y) * (py - y);
        if (dd < thr * thr && dd < best) {
            best = dd; res.active = true; res.index = (int)i;
            res.px = px; res.py = py; res.color = s.color;
            res.value = "value: " + chartDefaultLabel(s.size);
        }
    }
    return res;
}

static void chartDrawTooltip(Painter& p, double px, double py, const ChartTouch& t, const Size& size) {
    double fs = 11.0;
    auto wOf = [&](const std::string& s) { return s.empty() ? 0.0 : s.size() * fs * 0.6; };
    double tw = std::max(wOf(t.title), wOf(t.value)) + 14.0;
    if (tw < 36.0) tw = 36.0;
    double th = (t.title.empty() ? 0.0 : fs + 5.0) + (t.value.empty() ? 0.0 : fs + 3.0) + 8.0;
    double tx = px + 14.0;
    double ty = py - th - 12.0;
    if (tx + tw > size.width) tx = px - tw - 14.0;
    if (tx < 2.0) tx = 2.0;
    if (ty < 2.0) ty = py + 14.0;
    if (ty + th > size.height) ty = size.height - th - 2.0;
    p.setSourceColor(Color::fromRGBA(0.12, 0.13, 0.16, 0.95));
    p.fillRoundedRect(Rect{tx, ty, tw, th}, 5.0);
    double yy = ty + 7.0;
    if (!t.title.empty()) { chartDrawText(p, t.title, tx + 7.0, yy, fs, t.color, ChartTextAlign::Left); yy += fs + 3.0; }
    if (!t.value.empty()) { chartDrawText(p, t.value, tx + 7.0, yy, fs, Color::fromRGB(0.92, 0.92, 0.92), ChartTextAlign::Left); }
}

static void chartDrawHighlight(Painter& p, const Rect& a, double px, double py,
                                 const ChartTouch& t, const Size& size) {
    if (!t.active) return;
    p.setSourceColor(Color::fromRGBA(t.color.r, t.color.g, t.color.b, 0.4));
    p.setLineWidth(1.0);
    chartStrokeDashed(p, px, a.y, px, a.bottom(), {4.0, 3.0});
    chartStrokeDashed(p, a.x, py, a.right(), py, {4.0, 3.0});
    p.setSourceColor(Color::fromRGBA(t.color.r, t.color.g, t.color.b, 0.25));
    p.circle(px, py, 9.0); p.fill();
    p.setSourceColor(Color::fromRGBA(t.color.r, t.color.g, t.color.b, 0.95));
    p.setLineWidth(2.0);
    p.circle(px, py, 5.0); p.stroke();
    chartDrawTooltip(p, px, py, t, size);
}

namespace {
/// chart 交互状态：持有 hover 命中结果与 Donut 悬停动画。
class ChartHostState : public State {
public:
    void initState() override {
        anim_ = cui::make_shared<SharedAnimation>(220.0, Easing::EaseOutCubic);
        listenerId_ = anim_->addListener([this](double) {
            if (mounted()) setState();
        });
    }
    void dispose() override {
        if (anim_) { anim_->removeListener(listenerId_); anim_->stop(); anim_.reset(); }
    }
    void updateTouch(const ChartTouch& h) {
        bool wasActive = touch_.active;
        bool nowActive = h.active;
        if (nowActive) {
            touch_ = h;
        } else {
            touch_.active = false;  // 保留 index，供 Donut 反向动画收起
        }
        if (nowActive && !wasActive && anim_) anim_->forward();
        else if (!nowActive && wasActive && anim_) anim_->reverse();
        if (mounted()) setState();
    }
    SharedPtr<Widget> build(BuildContext&) override {
        auto* w = static_cast<ChartHost*>(widget());
        auto paint = w->paint();
        auto hit = w->hit();
        auto size = w->size();
        auto self = this;
        auto painter = [self, paint](Painter& p, const Size& s) {
            double prog = self->anim_ ? self->anim_->value() : 0.0;
            paint(p, s, self->touch_, prog);
        };
        auto onMove = [self, hit](const PointerEvent& e) {
            self->updateTouch(hit(e.position.dx, e.position.dy));
        };
        auto onLeave = [self](const PointerEvent&) { self->updateTouch(ChartTouch{}); };
        return cui::make_shared<Listener>(
            onMove, onMove, nullptr, onMove, nullptr,
            cui::make_shared<CustomPaint>(painter, nullptr, size),
            nullptr, onLeave);
    }
    ChartTouch touch_;
    SharedPtr<SharedAnimation> anim_;
    int32_t listenerId_ = 0;
};
}  // namespace

std::unique_ptr<State> ChartHost::createState() {
    return std::make_unique<ChartHostState>();
}

// ═══════════════════════════════════════════════════════════════════════════
// LineChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintLine(Painter& p, const Size& size, const LineChartData& d,
                      const ChartTouch* touch, double /*progress*/) {
    if (d.lineBarsData.empty()) return;
    double minX = d.minX, maxX = d.maxX, minY = d.minY, maxY = d.maxY;
    if (maxX - minX < 1e-9) {
        minX = minY = std::numeric_limits<double>::infinity();
        maxX = maxY = -std::numeric_limits<double>::infinity();
        for (const auto& bar : d.lineBarsData)
            for (const auto& s : bar.spots) {
                minX = std::min(minX, s.x); maxX = std::max(maxX, s.x);
                minY = std::min(minY, s.y); maxY = std::max(maxY, s.y);
            }
        if (!std::isfinite(minX)) { minX = 0; maxX = 1; minY = 0; maxY = 1; }
    }
    if (minY > 0) minY = 0;  // 折线图通常从 0 起
    Rect a = chartArea(size, d.titlesData);
    int nX = d.gridData.verticalInterval > 0 ? d.gridData.verticalInterval : 5;
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, minX, maxX, minY, maxY, nX, nY);

    int steps = 16;
    std::vector<Point> line;
    for (const auto& bar : d.lineBarsData) {
        if (bar.spots.empty()) continue;
        std::vector<Point> raw;
        raw.reserve(bar.spots.size());
        for (const auto& s : bar.spots)
            raw.push_back(Point{chartMapX(s.x, minX, maxX, a), chartMapY(s.y, minY, maxY, a)});
        if (bar.isStep) chartStepLine(raw, line);
        else if (bar.isCurved) chartCatmullRom(raw, steps, line);
        else line = raw;

        // 下方填充
        if (bar.showBelow) {
            p.setSourceColor(bar.belowColor);
            p.moveTo(line.front().x, a.bottom());
            for (const auto& pt : line) p.lineTo(pt.x, pt.y);
            p.lineTo(line.back().x, a.bottom());
            p.lineTo(line.front().x, a.bottom());
            p.fill();
        }
        // 折线
        p.setSourceColor(bar.color);
        p.setLineWidth(bar.width);
        p.moveTo(line.front().x, line.front().y);
        for (size_t i = 1; i < line.size(); ++i) p.lineTo(line[i].x, line[i].y);
        p.stroke();
        // 数据点
        if (bar.dotData) {
            p.setSourceColor(bar.color);
            for (const auto& pt : raw) { p.circle(pt.x, pt.y, bar.dotRadius); p.fill(); }
        }
    }
    chartDrawBorder(p, a, d.borderData);
    chartDrawTitles(p, a, d.titlesData, minX, maxX, minY, maxY, nX, nY);
    if (touch) chartDrawHighlight(p, a, touch->px, touch->py, *touch, size);
}

SharedPtr<Widget> LineChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintLine(p, s, d, &t, prog);
    };
    auto hit = [d, w, h](double x, double y) { return chartHitLine(x, y, d, Size(w, h)); };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// BarChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintBar(Painter& p, const Size& size, const BarChartData& d,
                     const ChartTouch* touch, double /*progress*/) {
    if (d.groups.empty()) return;
    double minY = 0.0;
    double maxY = d.maxY;
    if (maxY <= 0) {
        for (const auto& g : d.groups) {
            double tot = 0.0;
            for (const auto& r : g.barRods) {
                if (d.stacked) tot += r.toY;
                else tot = std::max(tot, r.toY);
            }
            maxY = std::max(maxY, tot);
        }
        if (maxY <= 0) maxY = 1;
    }
    Rect a = chartArea(size, d.titlesData);
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, 0, 1, minY, maxY, (int)d.groups.size(), nY);

    double slot = a.width / (double)d.groups.size();
    for (size_t gi = 0; gi < d.groups.size(); ++gi) {
        const auto& g = d.groups[gi];
        double gx = a.x + slot * (double)gi;
        // 组内 rods 横向分布
        size_t n = g.barRods.size();
        double totalW = 0.0;
        for (const auto& r : g.barRods) totalW += r.width;
        totalW += g.barsSpace * (n > 0 ? n - 1 : 0);
        double startX = gx + (slot - totalW) / 2.0;
        double cx = startX;
        double acc = 0.0;  // 堆叠时累计高度
        for (const auto& r : g.barRods) {
            double y0, y1;
            if (d.stacked) {
                y0 = chartMapY(acc, minY, maxY, a);
                y1 = chartMapY(acc + r.toY, minY, maxY, a);
                acc += r.toY;
            } else {
                y0 = chartMapY(0.0, minY, maxY, a);
                y1 = chartMapY(r.toY, minY, maxY, a);
            }
            Rect bar(cx, std::min(y0, y1), r.width, std::fabs(y1 - y0));
            p.setSourceColor(r.color);
            if (r.borderRadius > 0)
                p.fillRoundedRect(bar, r.borderRadius);
            else {
                p.rectangle(bar);
                p.fill();
            }
            cx += r.width + g.barsSpace;
        }
    }
    chartDrawBorder(p, a, d.borderData);
    // 底部标题：每个 group 一个标签
    if (d.titlesData.show && d.titlesData.bottom.showTitles) {
        for (size_t gi = 0; gi < d.groups.size(); ++gi) {
            double val = d.groups[gi].x;
            std::string s = d.titlesData.bottom.getTitles
                                ? d.titlesData.bottom.getTitles(val)
                                : chartDefaultLabel(val);
            double px = a.x + slot * ((double)gi + 0.5);
            chartDrawText(p, s, px, a.bottom() + 4.0, d.titlesData.bottom.textSize,
                       d.titlesData.bottom.textColor, ChartTextAlign::Center);
        }
    }
    if (d.titlesData.show && d.titlesData.left.showTitles) {
        for (int i = 0; i <= nY; ++i) {
            double val = minY + (maxY - minY) * (double)i / (double)nY;
            std::string s = d.titlesData.left.getTitles ? d.titlesData.left.getTitles(val)
                                                        : chartDefaultLabel(val);
            double py = chartMapY(val, minY, maxY, a);
            chartDrawText(p, s, a.x - 4.0, py, d.titlesData.left.textSize,
                       d.titlesData.left.textColor, ChartTextAlign::Right);
        }
    }
    if (touch) chartDrawHighlight(p, a, touch->px, touch->py, *touch, size);
}

SharedPtr<Widget> BarChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintBar(p, s, d, &t, prog);
    };
    auto hit = [d, w, h](double x, double y) { return chartHitBar(x, y, d, Size(w, h)); };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// PieChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintPie(Painter& p, const Size& size, const PieChartData& d,
                     const ChartTouch* touch, double progress) {
    if (d.sections.empty()) return;
    Point c{size.width / 2.0, size.height / 2.0};
    double maxR = std::min(size.width, size.height) / 2.0 - 6.0;
    double inner = d.centerSpaceRadius;
    double total = 0.0;
    for (const auto& s : d.sections) total += s.value;
    if (total <= 0) return;
    int hoverIdx = touch ? touch->index : -1;
    double ang = -90.0;  // 从正上方开始
    for (size_t i = 0; i < d.sections.size(); ++i) {
        const auto& s = d.sections[i];
        double sweep = s.value / total * 360.0;
        double a0 = ang + d.sectionsSpace / 2.0;
        double a1 = ang + sweep - d.sectionsSpace / 2.0;
        if (a1 < a0) a1 = a0;
        // Donut 悬停动画：被悬停扇区沿角平分线 explode 弹出并轻微放大
        bool hovered = (i == (size_t)hoverIdx) && progress > 0.001;
        double explode = hovered ? 10.0 * progress : 0.0;
        double grow = hovered ? 4.0 * progress : 0.0;
        double mid = (a0 + a1) / 2.0 * M_PI / 180.0;
        Point cc{c.x + std::cos(mid) * explode, c.y + std::sin(mid) * explode};
        double maxR2 = maxR + grow, inner2 = inner + grow;
        // 扇区路径（外弧 + 内弧）
        p.setSourceColor(s.color);
        p.moveTo(cc.x + std::cos(a0 * M_PI / 180.0) * inner2,
                 cc.y + std::sin(a0 * M_PI / 180.0) * inner2);
        p.arcTo(cc.x, cc.y, maxR2, a0, a1 - a0);
        p.arcTo(cc.x, cc.y, inner2, a1, a0 - a1);
        p.lineTo(cc.x + std::cos(a0 * M_PI / 180.0) * inner2,
                 cc.y + std::sin(a0 * M_PI / 180.0) * inner2);
        p.fill();
        // 标题
        if (!s.title.empty()) {
            double tr = (maxR2 + inner2) / 2.0;
            double tx = cc.x + std::cos(mid) * tr;
            double ty = cc.y + std::sin(mid) * tr;
            chartDrawText(p, s.title, tx, ty, s.titleSize, s.titleColor, ChartTextAlign::Center);
        }
        ang += sweep;
    }
    if (touch && touch->active) chartDrawTooltip(p, touch->px, touch->py, *touch, size);
}

SharedPtr<Widget> PieChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintPie(p, s, d, &t, prog);
    };
    auto hit = [d, w, h](double x, double y) { return chartHitPie(x, y, d, Size(w, h)); };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// ScatterChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintScatter(Painter& p, const Size& size, const ScatterChartData& d,
                         const ChartTouch* touch, double /*progress*/) {
    if (d.spots.empty()) return;
    double minX = d.minX, maxX = d.maxX, minY = d.minY, maxY = d.maxY;
    if (maxX - minX < 1e-9) {
        minX = minY = std::numeric_limits<double>::infinity();
        maxX = maxY = -std::numeric_limits<double>::infinity();
        for (const auto& s : d.spots) {
            minX = std::min(minX, s.x); maxX = std::max(maxX, s.x);
            minY = std::min(minY, s.y); maxY = std::max(maxY, s.y);
        }
        if (!std::isfinite(minX)) { minX = 0; maxX = 1; minY = 0; maxY = 1; }
    }
    Rect a = chartArea(size, d.titlesData);
    int nX = d.gridData.verticalInterval > 0 ? d.gridData.verticalInterval : 5;
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, minX, maxX, minY, maxY, nX, nY);
    for (const auto& s : d.spots) {
        p.setSourceColor(s.color);
        p.circle(chartMapX(s.x, minX, maxX, a), chartMapY(s.y, minY, maxY, a), s.radius);
        p.fill();
    }
    chartDrawBorder(p, a, d.borderData);
    chartDrawTitles(p, a, d.titlesData, minX, maxX, minY, maxY, nX, nY);
    if (touch) chartDrawHighlight(p, a, touch->px, touch->py, *touch, size);
}

SharedPtr<Widget> ScatterChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintScatter(p, s, d, &t, prog);
    };
    auto hit = [d, w, h](double x, double y) { return chartHitScatter(x, y, d, Size(w, h)); };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// RadarChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintRadar(Painter& p, const Size& size, const RadarChartData& d,
                       const ChartTouch* /*touch*/, double /*progress*/) {
    if (d.features.empty() || d.dataSets.empty()) return;
    Point c{size.width / 2.0, size.height / 2.0};
    double radius = std::min(size.width, size.height) / 2.0 - 28.0;
    size_t n = d.features.size();
    double step = 360.0 / (double)n;
    auto ptAt = [&](double idx, double ratio) -> Point {
        double ang = (-90.0 + idx * step) * M_PI / 180.0;
        return Point{c.x + std::cos(ang) * radius * ratio,
                     c.y + std::sin(ang) * radius * ratio};
    };
    // 网格环
    p.setSourceColor(Color::fromRGBA(0, 0, 0, 0.15));
    p.setLineWidth(1.0);
    int levels = 4;
    for (int lv = 1; lv <= levels; ++lv) {
        double ratio = (double)lv / (double)levels;
        p.moveTo(ptAt(0, ratio).x, ptAt(0, ratio).y);
        for (size_t i = 1; i <= n; ++i) {
            Point q = ptAt((double)i, ratio);
            p.lineTo(q.x, q.y);
        }
        p.lineTo(ptAt(0, ratio).x, ptAt(0, ratio).y);
        p.stroke();
    }
    // 轴线 + 标签
    for (size_t i = 0; i < n; ++i) {
        Point q = ptAt((double)i, 1.0);
        p.moveTo(c.x, c.y);
        p.lineTo(q.x, q.y);
        p.stroke();
        Point lab = ptAt((double)i, 1.18);
        chartDrawText(p, d.features[i].name, lab.x, lab.y, 11.0,
                   Color::fromRGB(0.4, 0.4, 0.45), ChartTextAlign::Center);
    }
    // 数据多边形
    for (const auto& ds : d.dataSets) {
        std::vector<Point> verts;
        for (size_t i = 0; i < n; ++i) {
            double v = (i < ds.values.size()) ? ds.values[i] : 0.0;
            double m = d.features[i].max;
            double r = m > 0 ? std::clamp(v / m, 0.0, 1.0) : 0.0;
            verts.push_back(ptAt((double)i, r));
        }
        // 填充
        p.setSourceColor(Color::fromRGBA(ds.color.r, ds.color.g, ds.color.b, ds.fillAlpha));
        p.moveTo(verts.front().x, verts.front().y);
        for (size_t i = 1; i < verts.size(); ++i) p.lineTo(verts[i].x, verts[i].y);
        p.lineTo(verts.front().x, verts.front().y);
        p.fill();
        // 描边（fill 已消耗路径，重建）
        p.setSourceColor(ds.color);
        p.setLineWidth(2.0);
        p.moveTo(verts.front().x, verts.front().y);
        for (size_t i = 1; i < verts.size(); ++i) p.lineTo(verts[i].x, verts[i].y);
        p.lineTo(verts.front().x, verts.front().y);
        p.stroke();
    }
}

SharedPtr<Widget> RadarChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintRadar(p, s, d, &t, prog);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// BubbleChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintBubble(Painter& p, const Size& size, const BubbleChartData& d,
                        const ChartTouch* touch, double /*progress*/) {
    if (d.spots.empty()) return;
    double minX = d.minX, maxX = d.maxX, minY = d.minY, maxY = d.maxY;
    double maxSize = 0.0;
    if (maxX - minX < 1e-9) {
        minX = minY = std::numeric_limits<double>::infinity();
        maxX = maxY = -std::numeric_limits<double>::infinity();
        for (const auto& s : d.spots) {
            minX = std::min(minX, s.x); maxX = std::max(maxX, s.x);
            minY = std::min(minY, s.y); maxY = std::max(maxY, s.y);
            maxSize = std::max(maxSize, s.size);
        }
        if (!std::isfinite(minX)) { minX = 0; maxX = 1; minY = 0; maxY = 1; }
    } else {
        for (const auto& s : d.spots) maxSize = std::max(maxSize, s.size);
    }
    if (maxSize <= 0) maxSize = 1.0;

    Rect a = chartArea(size, d.titlesData);
    int nX = d.gridData.verticalInterval > 0 ? d.gridData.verticalInterval : 5;
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, minX, maxX, minY, maxY, nX, nY);

    for (const auto& s : d.spots) {
        double r = d.maxBubbleRadius * std::sqrt(s.size / maxSize);
        p.setSourceColor(s.color);
        p.circle(chartMapX(s.x, minX, maxX, a), chartMapY(s.y, minY, maxY, a), r);
        p.fill();
    }
    chartDrawBorder(p, a, d.borderData);
    chartDrawTitles(p, a, d.titlesData, minX, maxX, minY, maxY, nX, nY);
    if (touch) chartDrawHighlight(p, a, touch->px, touch->py, *touch, size);
}

SharedPtr<Widget> BubbleChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintBubble(p, s, d, &t, prog);
    };
    auto hit = [d, w, h](double x, double y) { return chartHitBubble(x, y, d, Size(w, h)); };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// WaterfallChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintWaterfall(Painter& p, const Size& size, const WaterfallChartData& d,
                           const ChartTouch* /*touch*/, double /*progress*/) {
    if (d.items.empty()) return;
    // 计算累计范围
    double base = 0.0, hi = 0.0, lo = 0.0;
    for (const auto& it : d.items) {
        double y0, y1;
        if (it.isTotal) { y0 = 0.0; y1 = it.value; base = it.value; }
        else { y0 = base; y1 = base + it.value; base += it.value; }
        hi = std::max(hi, std::max(y0, y1));
        lo = std::min(lo, std::min(y0, y1));
    }
    if (hi <= lo) hi = lo + 1.0;

    ChartTitlesData td = d.titlesData;
    td.bottom.showTitles = false;  // x 轴标签手动绘制
    Rect a = chartArea(size, td);
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, 0, 1, lo, hi, (int)d.items.size(), nY);

    double slot = a.width / (double)d.items.size();
    double bw = slot * 0.6;
    base = 0.0;
    for (size_t i = 0; i < d.items.size(); ++i) {
        const auto& it = d.items[i];
        double y0, y1;
        Color c;
        if (it.isTotal) {
            y0 = 0.0; y1 = it.value; base = it.value;
            c = it.color.a > 0 ? it.color : d.totalColor;
        } else {
            y0 = base; y1 = base + it.value; base += it.value;
            c = it.color.a > 0 ? it.color
                               : (it.value >= 0 ? d.increaseColor : d.decreaseColor);
        }
        double px = a.x + slot * ((double)i + 0.5);
        double py0 = chartMapY(y0, lo, hi, a);
        double py1 = chartMapY(y1, lo, hi, a);
        Rect bar(px - bw / 2.0, std::min(py0, py1), bw, std::fabs(py1 - py0));
        p.setSourceColor(c);
        p.rectangle(bar);
        p.fill();
        // x 标签
        chartDrawText(p, it.label, px, a.bottom() + 4.0, td.bottom.textSize,
                   td.bottom.textColor, ChartTextAlign::Center);
    }
    chartDrawBorder(p, a, d.borderData);
    if (td.show && td.left.showTitles) {
        for (int i = 0; i <= nY; ++i) {
            double val = lo + (hi - lo) * (double)i / (double)nY;
            std::string s = td.left.getTitles ? td.left.getTitles(val)
                                              : chartDefaultLabel(val);
            double py = chartMapY(val, lo, hi, a);
            chartDrawText(p, s, a.x - 4.0, py, td.left.textSize,
                       td.left.textColor, ChartTextAlign::Right);
        }
    }
}

SharedPtr<Widget> WaterfallChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintWaterfall(p, s, d, &t, prog);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// HistogramChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintHistogram(Painter& p, const Size& size, const HistogramChartData& d,
                           const ChartTouch* /*touch*/, double /*progress*/) {
    if (d.samples.empty()) return;
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (double s : d.samples) { mn = std::min(mn, s); mx = std::max(mx, s); }
    if (!std::isfinite(mn)) { mn = 0; mx = 1; }
    int n = d.binCount > 0 ? d.binCount : (int)std::round(std::sqrt((double)d.samples.size()));
    if (n < 1) n = 1;
    double bw = (mx - mn) / (double)n;
    if (bw <= 0) { bw = 1.0; mx = mn + 1.0; }
    std::vector<double> counts(n, 0.0);
    for (double s : d.samples) {
        int idx = (int)((s - mn) / bw);
        if (idx >= n) idx = n - 1;
        if (idx < 0) idx = 0;
        counts[idx] += 1.0;
    }
    double maxC = 1.0;
    for (double c : counts) maxC = std::max(maxC, c);

    ChartTitlesData td = d.titlesData;
    td.bottom.showTitles = false;
    Rect a = chartArea(size, td);
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, mn, mx, 0.0, maxC, n, nY);

    double slot = a.width / (double)n;
    double bwpx = slot * 0.85;
    for (int i = 0; i < n; ++i) {
        double px = a.x + slot * ((double)i + 0.5);
        double y0 = chartMapY(0.0, 0.0, maxC, a);
        double y1 = chartMapY(counts[i], 0.0, maxC, a);
        Rect bar(px - bwpx / 2.0, y1, bwpx, y0 - y1);
        p.setSourceColor(d.color);
        p.rectangle(bar);
        p.fill();
        // 箱中心值标签
        double center = mn + ((double)i + 0.5) * bw;
        chartDrawText(p, chartDefaultLabel(center), px, a.bottom() + 4.0,
                   td.bottom.textSize, td.bottom.textColor, ChartTextAlign::Center);
    }
    chartDrawBorder(p, a, d.borderData);
    if (td.show && td.left.showTitles) {
        for (int i = 0; i <= nY; ++i) {
            double val = maxC * (double)i / (double)nY;
            std::string s = td.left.getTitles ? td.left.getTitles(val)
                                              : chartDefaultLabel(val);
            double py = chartMapY(val, 0.0, maxC, a);
            chartDrawText(p, s, a.x - 4.0, py, td.left.textSize,
                       td.left.textColor, ChartTextAlign::Right);
        }
    }
}

SharedPtr<Widget> HistogramChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch& t, double prog) {
        chartPaintHistogram(p, s, d, &t, prog);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// CandlestickChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintCandlestick(Painter& p, const Size& size, const CandlestickChartData& d) {
    if (d.candles.empty()) return;
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    for (const auto& c : d.candles) {
        minY = std::min(minY, std::min(c.low, c.open));
        maxY = std::max(maxY, std::max(c.high, c.close));
        minX = std::min(minX, c.x); maxX = std::max(maxX, c.x);
    }
    if (!std::isfinite(minY)) { minY = 0; maxY = 1; minX = 0; maxX = 1; }
    if (maxY - minY < 1e-9) { minY -= 1; maxY += 1; }

    ChartTitlesData td = d.titlesData;
    td.bottom.showTitles = false;
    Rect a = chartArea(size, td);
    int nY = d.gridData.horizontalInterval > 0 ? d.gridData.horizontalInterval : 5;
    chartDrawGrid(p, a, d.gridData, minX, maxX, minY, maxY, (int)d.candles.size(), nY);

    double slot = a.width / (double)d.candles.size();
    for (size_t i = 0; i < d.candles.size(); ++i) {
        const auto& c = d.candles[i];
        double cx = chartMapX(c.x, minX, maxX, a);
        double yOpen = chartMapY(c.open, minY, maxY, a);
        double yClose = chartMapY(c.close, minY, maxY, a);
        double yHigh = chartMapY(c.high, minY, maxY, a);
        double yLow = chartMapY(c.low, minY, maxY, a);
        bool up = c.close >= c.open;
        Color col = up ? d.bullishColor : d.bearishColor;
        double halfW = d.candleWidth / 2.0;
        // 影线
        p.setSourceColor(col);
        p.setLineWidth(1.5);
        p.moveTo(cx, yHigh); p.lineTo(cx, yLow); p.stroke();
        // 实体
        double top = std::min(yOpen, yClose);
        double h = std::fabs(yClose - yOpen);
        if (h < 1.0) h = 1.0;
        Rect body(cx - halfW, top, d.candleWidth, h);
        p.rectangle(body);
        p.fill();
    }
    chartDrawBorder(p, a, d.borderData);
    if (td.show && td.left.showTitles) {
        for (int i = 0; i <= nY; ++i) {
            double val = minY + (maxY - minY) * (double)i / (double)nY;
            std::string s = td.left.getTitles ? td.left.getTitles(val)
                                              : chartDefaultLabel(val);
            double py = chartMapY(val, minY, maxY, a);
            chartDrawText(p, s, a.x - 4.0, py, td.left.textSize,
                       td.left.textColor, ChartTextAlign::Right);
        }
    }
}

SharedPtr<Widget> CandlestickChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch&, double) {
        chartPaintCandlestick(p, s, d);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// SankeyChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintSankey(Painter& p, const Size& size, const SankeyChartData& d) {
    int n = (int)d.nodes.size();
    if (n == 0 || d.links.empty()) return;
    // 拓扑深度（最长路径）
    std::vector<int> indeg(n, 0), depth(n, 0);
    for (const auto& l : d.links) {
        if (l.from >= 0 && l.from < n && l.to >= 0 && l.to < n) indeg[l.to]++;
    }
    std::queue<int> q;
    for (int i = 0; i < n; ++i) if (indeg[i] == 0) q.push(i);
    std::vector<int> rem = indeg;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        for (const auto& l : d.links) {
            if (l.from != u) continue;
            int v = l.to;
            depth[v] = std::max(depth[v], depth[u] + 1);
            if (--rem[v] == 0) q.push(v);
        }
    }
    int maxDepth = 0;
    for (int dv : depth) maxDepth = std::max(maxDepth, dv);
    int cols = maxDepth + 1;
    std::vector<std::vector<int>> byCol(cols);
    for (int i = 0; i < n; ++i) byCol[depth[i]].push_back(i);
    // 列总流量 → 缩放
    double maxColTotal = 1.0;
    for (int c = 0; c < cols; ++c) {
        double tot = 0.0;
        for (int i : byCol[c]) tot += d.nodes[i].value;
        maxColTotal = std::max(maxColTotal, tot);
    }
    double padL = 12.0, padR = 12.0, padT = 12.0, padB = 12.0;
    double plotW = size.width - padL - padR;
    double plotH = size.height - padT - padB;
    double colGap = cols > 1 ? plotW / (double)(cols - 1) : 0.0;
    double scale = (plotH - (double)(n) * d.nodeGap) / maxColTotal;
    if (scale <= 0) scale = plotH / maxColTotal;
    // 节点布局
    struct NLayout { double y0 = 0, y1 = 0, x = 0; };
    std::vector<NLayout> lay(n);
    for (int c = 0; c < cols; ++c) {
        double x = padL + (cols > 1 ? colGap * (double)c : plotW / 2.0);
        double cur = padT;
        for (int i : byCol[c]) {
            double h = d.nodes[i].value * scale;
            lay[i].x = x; lay[i].y0 = cur; lay[i].y1 = cur + h; cur += h + d.nodeGap;
        }
    }
    // 链接带（按 source/target 累计偏移）
    std::vector<double> outOff(n, 0), inOff(n, 0);
    for (const auto& l : d.links) {
        if (l.from < 0 || l.from >= n || l.to < 0 || l.to >= n) continue;
        double lw = l.value * scale;
        double sTop = lay[l.from].y0 + outOff[l.from]; outOff[l.from] += lw;
        double tTop = lay[l.to].y0 + inOff[l.to]; inOff[l.to] += lw;
        double x0 = lay[l.from].x + d.nodeWidth / 2.0;
        double x1 = lay[l.to].x - d.nodeWidth / 2.0;
        Color c = d.nodes[l.from].color.a <= 0 ? chartPalette(l.from) : d.nodes[l.from].color;
        p.setSourceColor(Color::fromRGBA(c.r, c.g, c.b, 0.35));
        p.moveTo(x0, sTop);
        p.lineTo(x1, tTop);
        p.lineTo(x1, tTop + lw);
        p.lineTo(x0, sTop + lw);
        p.lineTo(x0, sTop);
        p.fill();
    }
    // 节点矩形 + 标签
    for (int i = 0; i < n; ++i) {
        Color c = d.nodes[i].color.a <= 0 ? chartPalette(i) : d.nodes[i].color;
        p.setSourceColor(c);
        Rect r(lay[i].x - d.nodeWidth / 2.0, lay[i].y0, d.nodeWidth, lay[i].y1 - lay[i].y0);
        p.rectangle(r);
        p.fill();
        double cy = (lay[i].y0 + lay[i].y1) / 2.0;
        std::string s = d.nodes[i].label;
        if (depth[i] == 0)
            chartDrawText(p, s, lay[i].x - d.nodeWidth / 2.0 - 4.0, cy, 11.0,
                       Color::fromRGB(0.4, 0.4, 0.45), ChartTextAlign::Right);
        else
            chartDrawText(p, s, lay[i].x + d.nodeWidth / 2.0 + 4.0, cy, 11.0,
                       Color::fromRGB(0.4, 0.4, 0.45), ChartTextAlign::Left);
    }
}

SharedPtr<Widget> SankeyChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch&, double) {
        chartPaintSankey(p, s, d);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// PolarChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintPolar(Painter& p, const Size& size, const PolarChartData& d) {
    if (d.categories.empty() || d.dataSets.empty()) return;
    size_t n = d.categories.size();
    double maxV = d.maxValue;
    if (maxV <= 0) {
        for (const auto& ds : d.dataSets)
            for (double v : ds.values) maxV = std::max(maxV, v);
    }
    if (maxV <= 0) maxV = 1.0;

    Point c{size.width / 2.0, size.height / 2.0};
    double radius = std::min(size.width, size.height) / 2.0 - 30.0;
    double step = 360.0 / (double)n;
    auto ptAt = [&](size_t idx, double ratio) -> Point {
        double ang = (-90.0 + (double)idx * step) * M_PI / 180.0;
        return Point{c.x + std::cos(ang) * radius * ratio,
                     c.y + std::sin(ang) * radius * ratio};
    };
    // 同心圆网格
    p.setSourceColor(Color::fromRGBA(0, 0, 0, 0.15));
    p.setLineWidth(1.0);
    int levels = 4;
    for (int lv = 1; lv <= levels; ++lv) {
        double ratio = (double)lv / (double)levels;
        p.circle(c.x, c.y, radius * ratio);
        p.stroke();
    }
    // 辐射轴 + 类别标签
    for (size_t i = 0; i < n; ++i) {
        Point q = ptAt(i, 1.0);
        p.setSourceColor(Color::fromRGBA(0, 0, 0, 0.15));
        p.moveTo(c.x, c.y); p.lineTo(q.x, q.y); p.stroke();
        Point lab = ptAt(i, 1.18);
        chartDrawText(p, d.categories[i], lab.x, lab.y, 11.0,
                   Color::fromRGB(0.4, 0.4, 0.45), ChartTextAlign::Center);
    }
    // 数据多边形
    for (const auto& ds : d.dataSets) {
        std::vector<Point> verts;
        for (size_t i = 0; i < n; ++i) {
            double v = (i < ds.values.size()) ? ds.values[i] : 0.0;
            double r = maxV > 0 ? std::clamp(v / maxV, 0.0, 1.0) : 0.0;
            verts.push_back(ptAt(i, r));
        }
        if (ds.showArea) {
            p.setSourceColor(Color::fromRGBA(ds.color.r, ds.color.g, ds.color.b, ds.fillAlpha));
            p.moveTo(verts.front().x, verts.front().y);
            for (size_t i = 1; i < verts.size(); ++i) p.lineTo(verts[i].x, verts[i].y);
            p.lineTo(verts.front().x, verts.front().y);
            p.fill();
        }
        p.setSourceColor(ds.color);
        p.setLineWidth(2.0);
        p.moveTo(verts.front().x, verts.front().y);
        for (size_t i = 1; i < verts.size(); ++i) p.lineTo(verts[i].x, verts[i].y);
        p.lineTo(verts.front().x, verts.front().y);
        p.stroke();
    }
}

SharedPtr<Widget> PolarChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch&, double) {
        chartPaintPolar(p, s, d);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// RadialBarChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintRadialBar(Painter& p, const Size& size, const RadialBarChartData& d) {
    if (d.dataSets.empty()) return;
    Point c{size.width / 2.0, size.height / 2.0};
    double outer = std::min(size.width, size.height) / 2.0 - 8.0;
    int k = (int)d.dataSets.size();
    double inner0 = outer - (double)k * d.ringWidth - (double)(k - 1) * d.ringGap;
    double baseR = inner0 > 8.0 ? inner0 : 8.0;
    for (int i = 0; i < k; ++i) {
        double r = baseR + (double)i * (d.ringWidth + d.ringGap) + d.ringWidth / 2.0;
        const auto& ds = d.dataSets[i];
        double frac = ds.max > 0 ? std::clamp(ds.value / ds.max, 0.0, 1.0) : 0.0;
        // 背景环
        p.setSourceColor(Color::fromRGBA(0, 0, 0, 0.10));
        p.drawArc(c.x, c.y, r, d.startAngle, 360.0, d.ringWidth);
        // 进度环
        p.setSourceColor(ds.color);
        p.drawArc(c.x, c.y, r, d.startAngle, 360.0 * frac, d.ringWidth);
        // 标签（统一堆叠在图表右侧，避免重叠）
        double ly = c.y - (double)(k - 1) * 7.0 + (double)i * 14.0;
        chartDrawText(p, ds.name, c.x + outer + 8.0, ly, 11.0,
                   Color::fromRGB(0.4, 0.4, 0.45), ChartTextAlign::Left);
    }
}

SharedPtr<Widget> RadialBarChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch&, double) {
        chartPaintRadialBar(p, s, d);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

// ═══════════════════════════════════════════════════════════════════════════
// FunnelChart / PyramidChart
// ═══════════════════════════════════════════════════════════════════════════

void chartPaintFunnel(Painter& p, const Size& size, const FunnelChartData& d,
                        bool pyramid) {
    if (d.slices.empty()) return;
    double maxV = d.slices.front().value;
    for (const auto& s : d.slices) maxV = std::max(maxV, s.value);
    if (maxV <= 0) maxV = 1.0;

    double padL = 12.0, padR = 12.0, padT = 12.0, padB = 12.0;
    double fullW = size.width - padL - padR;
    double slotH = (size.height - padT - padB) / (double)d.slices.size();
    double cx = size.width / 2.0;

    for (size_t i = 0; i < d.slices.size(); ++i) {
        const auto& s = d.slices[i];
        double yTop = padT + slotH * (double)i;
        double yBot = yTop + slotH * 0.86;
        if (pyramid) {
            // 居中对称水平条（人口金字塔风格）
            double w = s.value / maxV * fullW;
            Rect r(cx - w / 2.0, yTop, w, yBot - yTop);
            p.setSourceColor(s.color.a <= 0 ? chartPalette((int)i) : s.color);
            p.rectangle(r);
            p.fill();
        } else {
            // 梯形漏斗：上宽 topW、下宽 botW，居中
            double topW = s.value / maxV * fullW;
            double botW = (i + 1 < d.slices.size())
                              ? d.slices[i + 1].value / maxV * fullW
                              : topW;  // 末段不收口
            p.setSourceColor(s.color.a <= 0 ? chartPalette((int)i) : s.color);
            p.moveTo(cx - topW / 2.0, yTop);
            p.lineTo(cx + topW / 2.0, yTop);
            p.lineTo(cx + botW / 2.0, yBot);
            p.lineTo(cx - botW / 2.0, yBot);
            p.lineTo(cx - topW / 2.0, yTop);
            p.fill();
        }
        chartDrawText(p, s.label, cx, (yTop + yBot) / 2.0, 12.0,
                   Color::fromRGB(1.0, 1.0, 1.0), ChartTextAlign::Center);
    }
}

SharedPtr<Widget> FunnelChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch&, double) {
        chartPaintFunnel(p, s, d, false);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

SharedPtr<Widget> PyramidChart::build(BuildContext&) {
    auto d = data_;
    auto w = size_.width, h = size_.height;
    auto paint = [d](Painter& p, const Size& s, const ChartTouch&, double) {
        chartPaintFunnel(p, s, d, true);
    };
    auto hit = [](double, double) { return ChartTouch{}; };
    return cui::make_shared<ChartHost>(paint, hit, Size(w, h));
}

}  // namespace chart
}  // namespace cui_core
