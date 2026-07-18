#pragma once
/// @file widgets/data.h
/// @brief 数据展示控件（上游移植第二批）。
///
/// 把 legacy 的 Badge / CircleAvatar / Calendar / TableView / TreeView 以
/// `cui_core` 三层（Widget/Element/RenderObject）重新实现，供
/// gallery 的流水线页面使用。全部由已有控件原语
/// （Container / Row / Column / Stack / Text / GestureDetector）组合而成，
/// 不引入任何 legacy 控件头，避免命名冲突。
///
/// 对应 上游源码：
///   - Badge（material/badge.dart）
///   - CircleAvatar（material/circle_avatar.dart）
///   - CalendarDatePicker（material/calendar_date_picker.dart）简化月视图
///   - DataTable（material/data_table.dart）简化（排序 / 斑马纹 / 列对齐）
///   - TreeView（第三方，上游 无官方 Tree，这里给出通用树）

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/theme.h"
#include "cui/framework/framework.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"

/// 本文件的控件放在 `cui_core` 子命名空间：legacy 层已有同名类
/// （cui_core::Badge / cui_core::CircleAvatar / cui_core::Calendar / cui_core::DataTable /
/// cui_core::TreeView / cui_core::Bubble / cui_core::Markdown …），若也放进 `cui_core` 会与
/// legacy 定义在同一二进制中重名（ODR），链接器按符号名任选其一导致对象
/// 布局错乱。放进子命名空间即可彻底隔离。
namespace cui {

// ── 主题色（与 gallery 壳一致，避免依赖其匿名命名空间）─────────────────
inline Color dataPrimary() { return theme().primary; }
inline Color dataCaption() { return theme().textSecondary; }

// ── Badge ────────────────────────────────────────────────────────────────────

/// 角标。上游的 `Badge`（badge.dart）简化版：在子控件右上角叠加计数圆点。
///
/// count <= 0 且 showZero == false 时直接返回 child（不绘制角标）。
class Badge : public StatelessWidget {
public:
explicit Badge(SharedPtr<Widget> child = nullptr, int count = 0,
               bool showZero = false,
               Color badgeColor = Color::fromRGB(0.9, 0.2, 0.2),
               KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), child_(std::move(child)), count_(count),
      showZero_(showZero), badgeColor_(badgeColor) {}

SharedPtr<Widget> build(BuildContext& context) override {
    (void)context;
    if (count_ <= 0 && !showZero_)
        return child_;

    auto label = cui::make_shared<Text>(std::to_string(count_),
                                       TextStyle().size(11.0).bold().fgColor(Color::fromRGB(1, 1, 1)));
    auto dot = cui::make_shared<Container>(cui::make_shared<Center>(label));
    dot->padding(EdgeInsets::symmetric(5.0, 5.0))
        .decoration(BoxDecoration{badgeColor_, 9.0});

    auto pos = cui::make_shared<Positioned>(dot);
    pos->top = -3.0;
    pos->right = -3.0;
    pos->hasTop = true;
    pos->hasRight = true;

    std::vector<SharedPtr<Widget>> kids;
    if (child_)
        kids.push_back(child_);
    kids.push_back(pos);
    auto stack = cui::make_shared<Stack>(std::move(kids));
    stack->setFit(RenderStack::Fit::Loose);
    return stack;
}

std::string toStringShort() const override { return "Badge"; }

private:
SharedPtr<Widget> child_;
int count_;
bool showZero_;
Color badgeColor_;
};

// ── CircleAvatar ───────────────────────────────────────────────────────────────

/// 圆形头像。上游的 `CircleAvatar`（circle_avatar.dart）。
///
/// 以 initials（首字母）居中显示，背景色可定制；radius 即圆形半径。
class CircleAvatar : public StatelessWidget {
public:
explicit CircleAvatar(std::string initials = "", double radius = 20.0,
                      Color backgroundColor = theme().primary,
                      KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), initials_(std::move(initials)),
      radius_(radius), backgroundColor_(backgroundColor) {}

SharedPtr<Widget> build(BuildContext& context) override {
    (void)context;
    auto t = cui::make_shared<Text>(initials_,
                                   TextStyle().size(radius_ * 0.8).bold().fgColor(theme().textOnPrimary));
    auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
    c->width(radius_ * 2.0).height(radius_ * 2.0)
        .decoration(BoxDecoration{backgroundColor_, radius_});
    return c;
}

std::string toStringShort() const override { return "CircleAvatar"; }

private:
std::string initials_;
double radius_;
Color backgroundColor_;
};

// ── Calendar ───────────────────────────────────────────────────────────────────

/// 月历。上游的 `CalendarDatePicker` 简化版：单月网格 + 上/下月导航。
///
/// 点击日期即选中——选中态存在 State 里（对齐 上游 `_selectedDate`），
/// `onDaySelected` 只是「选中变化」的通知，不传回调也照样可选中。
/// 高亮对齐 上游 M3：选中 = 主色实心 + 反色文字，今天 = 主色描边。
class Calendar : public StatefulWidget {
public:
/// selectedYear / selectedMonth(1-12) / selectedDay：初始选中日；任一为 0 = 无选中。
explicit Calendar(int year = 0, int month = 0,
                  std::function<void(int, int, int)> onDaySelected = {},
                  KeyPtr key = nullptr, int selectedYear = 0, int selectedMonth = 0,
                  int selectedDay = 0)
    : StatefulWidget(std::move(key)), onDaySelected_(std::move(onDaySelected)) {
    if (year == 0 || month == 0) {
        std::time_t now = std::time(nullptr);
        std::tm* t = std::localtime(&now);
        year_ = t->tm_year + 1900;
        month_ = t->tm_mon;  // 0-11
    } else {
        year_ = year;
        month_ = month - 1;  // 入参 1-12 → 内部 0-11
    }
    if (selectedYear != 0 && selectedMonth != 0 && selectedDay != 0) {
        selYear_ = selectedYear;
        selMonth_ = selectedMonth - 1;
        selDay_ = selectedDay;
    }
}

int year() const { return year_; }
int month() const { return month_; }  ///< 0-11
int selectedYear() const { return selYear_; }
int selectedMonth() const { return selMonth_; }  ///< 0-11
int selectedDay() const { return selDay_; }
const std::function<void(int, int, int)>& onDaySelected() const { return onDaySelected_; }

std::unique_ptr<State> createState() override;
std::string toStringShort() const override { return "Calendar"; }

private:
int year_;
int month_;
int selYear_ = 0;
int selMonth_ = 0;
int selDay_ = 0;
std::function<void(int, int, int)> onDaySelected_;
};

class CalendarState : public State {
public:
void initState() override {
    auto* c = static_cast<Calendar*>(widget());
    year_ = c->year();
    month_ = c->month();
    selY_ = c->selectedYear();
    selM_ = c->selectedMonth();
    selD_ = c->selectedDay();
}

int selectedYear() const { return selY_; }
int selectedMonth() const { return selM_; }  ///< 0-11
int selectedDay() const { return selD_; }

SharedPtr<Widget> build(BuildContext& context) override;

private:
static int daysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[m];
}
/// 返回某月 1 号是星期几（0=周日 … 6=周六）。
static int firstWeekday(int y, int m) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m;
    t.tm_mday = 1;
    t.tm_hour = 12;
    std::mktime(&t);
    return t.tm_wday;
}
void step(int delta) {
    int m = month_ + delta;
    if (m < 0) { m = 11; --year_; }
    else if (m > 11) { m = 0; ++year_; }
    month_ = m;
    setState();
}
/// 上游的 `_handleDayChanged`：先改选中态再通知，切月不清选中。
void selectDay(int y, int m, int d) {
    selY_ = y;
    selM_ = m;
    selD_ = d;
    setState();
    const auto& cb = static_cast<Calendar*>(widget())->onDaySelected();
    if (cb)
        cb(y, m, d);
}
bool hasSelection() const { return selD_ != 0; }
bool isDaySelected(int y, int m, int d) const {
    return hasSelection() && selY_ == y && selM_ == m && selD_ == d;
}

int year_ = 2026;
int month_ = 0;
int selY_ = 0;
int selM_ = 0;
int selD_ = 0;
};

inline std::unique_ptr<State> Calendar::createState() {
return std::make_unique<CalendarState>();
}

inline SharedPtr<Widget> CalendarState::build(BuildContext& context) {
(void)context;
int y = year_, m = month_;
const char* months[] = {"January", "February", "March", "April", "May", "June",
                        "July", "August", "September", "October", "November", "December"};
const char* wd[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

// 今天
std::time_t now = std::time(nullptr);
std::tm* tn = std::localtime(&now);
int todayY = tn->tm_year + 1900, todayM = tn->tm_mon, todayD = tn->tm_mday;

auto navBtn = [&](const std::string& sym, std::function<void()> fn) {
    auto t = cui::make_shared<Text>(sym, TextStyle().size(18.0).bold().fgColor(dataPrimary()));
    auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
    c->padding(EdgeInsets::symmetric(6.0, 10.0));
    return cui::make_shared<GestureDetector>(c, std::move(fn));
};

auto title = cui::make_shared<Text>(std::string(months[m]) + " " + std::to_string(y),
                                    TextStyle().size(14.0).bold());
auto titleBox = cui::make_shared<Container>(cui::make_shared<Center>(title));
titleBox->padding(EdgeInsets::symmetric(8.0, 0.0));

auto header = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
    navBtn("‹", [this] { step(-1); }), titleBox, navBtn("›", [this] { step(1); })});
header->setMainAxisAlignment(MainAxisAlignment::SpaceBetween);
header->setMainAxisSize(MainAxisSize::Min);

// 星期表头
std::vector<SharedPtr<Widget>> wdRow;
for (int i = 0; i < 7; ++i) {
    auto t = cui::make_shared<Text>(wd[i], TextStyle().size(11.0).bold().fgColor(dataCaption()));
    auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
    c->width(34.0).height(22.0);
    wdRow.push_back(c);
}
auto weekHeader = cui::make_shared<Row>(std::move(wdRow));
weekHeader->setMainAxisSize(MainAxisSize::Min);

// 日期网格（6 行 × 7 列）
int lead = firstWeekday(y, m);
int total = daysInMonth(y, m);
int cur = 1 - lead;
std::vector<SharedPtr<Widget>> weekRows;
for (int r = 0; r < 6; ++r) {
    std::vector<SharedPtr<Widget>> cells;
    for (int c = 0; c < 7; ++c) {
        if (cur >= 1 && cur <= total) {
            bool isToday = (y == todayY && m == todayM && cur == todayD);
            bool isSelected = isDaySelected(y, m, cur);
            // 上游 M3 的 dayBackgroundColor / todayBorder 两条：选中 = 主色实心 +
            // 反色文字；今天 = 主色描边 + 主色文字；两者叠加时描边取深色。
            Color fg = isSelected ? theme().textOnPrimary
                                  : (isToday ? dataPrimary() : theme().textPrimary);
            BoxDecoration dec{};
            if (isSelected) {
                dec = BoxDecoration{dataPrimary(), 15.0};
                if (isToday) {
                    dec.borderWidth = 1.0;
                    dec.borderColor = Color::fromRGB(0.1, 0.1, 0.1);
                }
            } else if (isToday) {
                dec = BoxDecoration{Color{0, 0, 0, 0}, 15.0, 1.0, dataPrimary()};
            }
            auto t = cui::make_shared<Text>(std::to_string(cur),
                                            TextStyle().size(12.0).bold().fgColor(fg));
            auto cellInner = cui::make_shared<Center>(t);
            auto cell = cui::make_shared<Container>(cellInner);
            cell->width(34.0).height(30.0);
            cell->decoration(dec);
            int yy = y, mm = m, dd = cur;
            cells.push_back(cui::make_shared<GestureDetector>(cell, [this, yy, mm, dd] {
                selectDay(yy, mm, dd);
            }));
        } else {
            cells.push_back(cui::make_shared<SizedBox>(34.0, 30.0));
        }
        ++cur;
    }
    auto row = cui::make_shared<Row>(std::move(cells));
    row->setMainAxisSize(MainAxisSize::Min);
    weekRows.push_back(row);
}
auto grid = cui::make_shared<Column>(std::move(weekRows));
grid->setCrossAxisAlignment(CrossAxisAlignment::Start);

auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{header, weekHeader, grid});
col->setMainAxisSize(MainAxisSize::Min);
col->setCrossAxisAlignment(CrossAxisAlignment::Start);

auto card = cui::make_shared<Container>(col);
card->padding(EdgeInsets::all(10.0)).decoration(BoxDecoration{theme().surface, 8.0, 1.0,
                                                              theme().border, 4.0,
                                                              Color::fromRGBA(0, 0, 0, 0.2)});
return card;
}

// ── DataTable ──────────────────────────────────────────────────────────────────

/// 数据列描述。
struct DataColumn {
std::string label;
double width = 100.0;  ///< 列宽（px）
bool numeric = false;  ///< true → 右对齐
};

/// 数据表。上游的 `DataTable`（data_table.dart）简化版：
/// 表头可点击排序（再次点击反向）、列对齐、斑马纹。
///
/// 列宽由 DataColumn::width 固定；整表宽度 = 各列宽之和，左对齐于页面。
class DataTable : public StatefulWidget {
public:
explicit DataTable(std::vector<DataColumn> columns,
                   std::vector<std::vector<std::string>> rows, bool zebra = true,
                   KeyPtr key = nullptr)
    : StatefulWidget(std::move(key)), columns_(std::move(columns)),
      rows_(std::move(rows)), zebra_(zebra) {}

const std::vector<DataColumn>& columns() const { return columns_; }
const std::vector<std::vector<std::string>>& rows() const { return rows_; }
bool zebra() const { return zebra_; }

std::unique_ptr<State> createState() override;
std::string toStringShort() const override { return "DataTable"; }

private:
std::vector<DataColumn> columns_;
std::vector<std::vector<std::string>> rows_;
bool zebra_;
};

class DataTableState : public State {
public:
SharedPtr<Widget> build(BuildContext& context) override;

private:
DataTable* table() { return static_cast<DataTable*>(widget()); }

int sortCol_ = -1;
bool asc_ = true;

static bool isNumeric(const std::string& s) {
    if (s.empty())
        return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+')
        ++i;
    bool dot = false;
    for (; i < s.size(); ++i) {
        if (s[i] == '.') {
            if (dot)
                return false;
            dot = true;
        } else if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

void sortRows(std::vector<std::vector<std::string>>& r) {
    if (sortCol_ < 0)
        return;
    bool numeric = isNumeric(r.empty() ? std::string() : r[0][sortCol_]);
    std::sort(r.begin(), r.end(), [&](const std::vector<std::string>& a,
                                     const std::vector<std::string>& b) {
        const std::string& x = a[sortCol_];
        const std::string& y = b[sortCol_];
        int cmp;
        if (numeric) {
            double dx = std::atof(x.c_str()), dy = std::atof(y.c_str());
            cmp = dx < dy ? -1 : (dx > dy ? 1 : 0);
        } else {
            cmp = x.compare(y);
        }
        return asc_ ? cmp < 0 : cmp > 0;
    });
}
};

inline std::unique_ptr<State> DataTable::createState() {
return std::make_unique<DataTableState>();
}

inline SharedPtr<Widget> DataTableState::build(BuildContext& context) {
(void)context;
const auto& cols = table()->columns();
auto rows = table()->rows();
sortRows(rows);

double totalW = 0.0;
for (const auto& c : cols)
    totalW += c.width;

// ── 表头 ──
std::vector<SharedPtr<Widget>> headCells;
for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
    const auto& col = cols[i];
    std::string label = col.label;
    if (i == sortCol_)
        label += asc_ ? " ▲" : " ▼";
    auto t = cui::make_shared<Text>(label, TextStyle().size(12.0).bold());
    auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
    c->width(col.width).height(28.0)
        .padding(EdgeInsets::symmetric(4.0, 6.0))
        .alignment(col.numeric ? 1.0 : -1.0, 0.0)
        .decoration(BoxDecoration{Color(theme().primary.r, theme().primary.g,
                                        theme().primary.b, 0.10)});
    int idx = i;
    headCells.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
        if (sortCol_ == idx)
            asc_ = !asc_;
        else {
            sortCol_ = idx;
            asc_ = true;
        }
        setState();
    }));
}
auto headRow = cui::make_shared<Row>(std::move(headCells));
headRow->setMainAxisSize(MainAxisSize::Min);

// ── 数据行 ──
std::vector<SharedPtr<Widget>> bodyRows;
for (size_t r = 0; r < rows.size(); ++r) {
    std::vector<SharedPtr<Widget>> cells;
    for (size_t i = 0; i < cols.size(); ++i) {
        auto t = cui::make_shared<Text>(rows[r][i], TextStyle().size(12.0));
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        Color bg = (table()->zebra() && (r % 2 == 1))
                       ? theme().hoverOverlay
                       : Color::fromRGBA(0, 0, 0, 0);
        c->width(cols[i].width).height(26.0)
            .padding(EdgeInsets::symmetric(3.0, 6.0))
            .alignment(cols[i].numeric ? 1.0 : -1.0, 0.0)
            .decoration(BoxDecoration{bg});
        cells.push_back(c);
    }
    auto row = cui::make_shared<Row>(std::move(cells));
    row->setMainAxisSize(MainAxisSize::Min);
    bodyRows.push_back(row);
}

auto col = cui::make_shared<Column>(std::move(bodyRows));
col->setCrossAxisAlignment(CrossAxisAlignment::Start);

auto box = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{headRow, col});
box->setMainAxisSize(MainAxisSize::Min);
box->setCrossAxisAlignment(CrossAxisAlignment::Start);

auto outer = cui::make_shared<Container>(box);
outer->width(totalW)
    .decoration(BoxDecoration{theme().surface, 8.0, 1.0,
                              theme().border, 4.0,
                              Color::fromRGBA(0, 0, 0, 0.2)});
return outer;
}

// ── TreeView ───────────────────────────────────────────────────────────────────

/// 树节点。
struct TreeItem {
std::string id;
std::string label;
std::vector<TreeItem> children;
};

/// 树形视图。点击行 = 选中（整条高亮）；有子节点时同时切换展开/折叠。
/// 默认全部展开以展示结构。
class TreeView : public StatefulWidget {
public:
explicit TreeView(std::vector<TreeItem> roots, bool defaultExpanded = true,
                  KeyPtr key = nullptr)
    : StatefulWidget(std::move(key)), roots_(std::move(roots)),
      defaultExpanded_(defaultExpanded) {}

const std::vector<TreeItem>& roots() const { return roots_; }
bool defaultExpanded() const { return defaultExpanded_; }

std::unique_ptr<State> createState() override;
std::string toStringShort() const override { return "TreeView"; }

private:
std::vector<TreeItem> roots_;
bool defaultExpanded_;
};

class TreeViewState : public State {
public:
void initState() override {
    if (tree()->defaultExpanded()) {
        std::function<void(const std::vector<TreeItem>&)> collect =
            [&](const std::vector<TreeItem>& items) {
                for (const auto& it : items) {
                    if (!it.children.empty()) {
                        expanded_.insert(it.id);
                        collect(it.children);
                    }
                }
            };
        collect(tree()->roots());
    }
}

SharedPtr<Widget> build(BuildContext& context) override {
    (void)context;
    std::vector<SharedPtr<Widget>> rows;
    buildNodes(tree()->roots(), 0, rows);
    auto col = cui::make_shared<Column>(std::move(rows));
    col->setMainAxisSize(MainAxisSize::Min);
    // Stretch：让每行撑满面板宽度，选中高亮才能覆盖整条（与文件树一致）。
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    return col;
}

private:
TreeView* tree() { return static_cast<TreeView*>(widget()); }

void buildNodes(const std::vector<TreeItem>& items, int depth,
                std::vector<SharedPtr<Widget>>& out) {
    for (const auto& it : items) {
        bool hasKids = !it.children.empty();
        bool open = expanded_.count(it.id) > 0;

        std::vector<SharedPtr<Widget>> rowKids;
        rowKids.push_back(cui::make_shared<SizedBox>(depth * 16.0, 0.0));
        if (hasKids) {
            // 展开/收起箭头用矢量图标（「▾/▸」字形在默认字体里不一定存在，
            // 会渲染成空盒子）：向下箭头转 270° = 指向右。
            auto arrow = cui::make_shared<RotatedBox>(
                open ? 0 : 3,
                cui::make_shared<Icon>(IconCode::ExpandMore, 14.0, theme().textSecondary));
            auto a = cui::make_shared<Container>(cui::make_shared<Center>(arrow));
            a->width(16.0).height(20.0);
            rowKids.push_back(a);
        } else {
            rowKids.push_back(cui::make_shared<SizedBox>(16.0, 0.0));
        }
        auto t = cui::make_shared<Text>(it.label,
                                        TextStyle().size(12.5).fgColor(theme().textPrimary));
        rowKids.push_back(cui::make_shared<Container>(t));

        auto rowBox = cui::make_shared<Row>(std::move(rowKids));
        rowBox->setMainAxisSize(MainAxisSize::Min);
        rowBox->setCrossAxisAlignment(CrossAxisAlignment::Center);

        // 选中行整条高亮；点击既选中，又切换有子节点的展开/折叠。
        auto rowPad = cui::make_shared<Container>(rowBox);
        rowPad->height(20.0).padding(EdgeInsets::symmetric(2.0, 0.0));
        if (it.id == selectedId_)
            rowPad->decoration(BoxDecoration{theme().primaryLight});

        std::string id = it.id;
        bool kids = hasKids;
        auto row = cui::make_shared<GestureDetector>(rowPad, [this, id, kids] {
            selectedId_ = id;
            if (kids) {
                if (expanded_.count(id))
                    expanded_.erase(id);
                else
                    expanded_.insert(id);
            }
            setState();
        });

        out.push_back(row);
        if (hasKids && open)
            buildNodes(it.children, depth + 1, out);
    }
}

std::set<std::string> expanded_;
std::string selectedId_;
};

inline std::unique_ptr<State> TreeView::createState() {
return std::make_unique<TreeViewState>();
}

// ── Bubble ─────────────────────────────────────────────────────────────────────
/// 聊天气泡。上游 社区 `Bubble`（无官方）。
class Bubble : public StatelessWidget {
public:
explicit Bubble(std::string text, bool isMe = false, KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), text_(std::move(text)), isMe_(isMe) {}

SharedPtr<Widget> build(BuildContext&) override {
    auto t = cui::make_shared<Text>(text_, TextStyle().size(13.0).fgColor(
                                                isMe_ ? theme().textOnPrimary
                                                      : theme().textPrimary));
    auto bubble = cui::make_shared<Container>(t);
    bubble->padding(EdgeInsets::symmetric(8.0, 12.0))
        .decoration(BoxDecoration{isMe_ ? theme().primary
                                        : theme().surfaceVariant, 12.0});
    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{bubble});
    row->setMainAxisSize(MainAxisSize::Min);
    row->setMainAxisAlignment(isMe_ ? MainAxisAlignment::End : MainAxisAlignment::Start);
    return row;
}
std::string toStringShort() const override { return "Bubble"; }

private:
std::string text_;
bool isMe_;
};

// ── BarChart ───────────────────────────────────────────────────────────────────
/// 柱状图。上游 社区 `BarChart`（无官方 charts 包）。
class BarChart : public StatelessWidget {
public:
explicit BarChart(std::vector<std::string> labels, std::vector<double> values,
                  double maxHeight = 110.0, KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), labels_(std::move(labels)),
      values_(std::move(values)), maxHeight_(maxHeight) {}

SharedPtr<Widget> build(BuildContext&) override {
    double max = 1.0;
    for (double v : values_)
        max = std::max(max, v);
    static const Color palette[] = {
        Color::fromRGB(0.0, 0.45, 0.95), Color::fromRGB(0.26, 0.78, 0.54),
        Color::fromRGB(0.95, 0.6, 0.1),  Color::fromRGB(0.9, 0.3, 0.35),
        Color::fromRGB(0.6, 0.4, 0.9),  Color::fromRGB(0.2, 0.7, 0.8)};
    std::vector<SharedPtr<Widget>> bars, tags;
    for (size_t i = 0; i < values_.size(); ++i) {
        double h = std::max(2.0, values_[i] / max * maxHeight_);
        auto bar = cui::make_shared<Container>(cui::make_shared<SizedBox>(30.0, h));
        bar->decoration(BoxDecoration{palette[i % 6], 3.0});
        bars.push_back(bar);
        auto tag = cui::make_shared<Text>(
            i < labels_.size() ? labels_[i] : "",
            TextStyle().size(11.0).fgColor(dataCaption()));
        auto tagBox = cui::make_shared<Container>(cui::make_shared<Center>(tag));
        tagBox->width(30.0).padding(EdgeInsets(0.0, 4.0, 0.0, 0.0));
        tags.push_back(tagBox);
    }
    auto barRow = cui::make_shared<Row>(std::move(bars));
    barRow->setMainAxisSize(MainAxisSize::Min);
    barRow->setCrossAxisAlignment(CrossAxisAlignment::End);
    auto tagRow = cui::make_shared<Row>(std::move(tags));
    tagRow->setMainAxisSize(MainAxisSize::Min);
    auto baseline = cui::make_shared<Container>(cui::make_shared<SizedBox>(1e18, 1.0));
    baseline->decoration(BoxDecoration{theme().divider});
    auto col = cui::make_shared<Column>(
        std::vector<SharedPtr<Widget>>{barRow, baseline, tagRow});
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}
std::string toStringShort() const override { return "BarChart"; }

private:
std::vector<std::string> labels_;
std::vector<double> values_;
double maxHeight_;
};

// ── DescriptionList ────────────────────────────────────────────────────────────
/// 描述列表（term / description 两列）。上游 社区常见（无官方）。
class DescriptionList : public StatelessWidget {
public:
using Item = std::pair<std::string, std::string>;

explicit DescriptionList(std::vector<Item> items, double termWidth = 120.0,
                         KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), items_(std::move(items)), termWidth_(termWidth) {}

SharedPtr<Widget> build(BuildContext&) override {
    std::vector<SharedPtr<Widget>> kids;
    for (const auto& it : items_) {
        auto term = cui::make_shared<Text>(it.first, TextStyle().size(12.5).bold());
        auto termBox = cui::make_shared<Container>(term);
        termBox->width(termWidth_);
        auto desc = cui::make_shared<Text>(it.second, TextStyle().size(12.5).fgColor(
                                                          theme().textSecondary));
        auto row = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{termBox, desc});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(row);
        box->padding(EdgeInsets::symmetric(6.0, 0.0));
        kids.push_back(box);
    }
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto outer = cui::make_shared<Container>(col);
    outer->padding(EdgeInsets::all(10.0))
        .decoration(BoxDecoration{theme().surface, 6.0, 1.0,
                                  theme().border});
    return outer;
}
std::string toStringShort() const override { return "DescriptionList"; }

private:
std::vector<Item> items_;
double termWidth_;
};

// ── DiffView ───────────────────────────────────────────────────────────────────
/// diff 行。
struct DiffLine {
char sign = ' ';  ///< '+' 新增 / '-' 删除 / ' ' 上下文
std::string text;
};

/// 差异视图。上游 社区 `DiffView`（无官方）。
class DiffView : public StatelessWidget {
public:
explicit DiffView(std::vector<DiffLine> lines, KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), lines_(std::move(lines)) {}

SharedPtr<Widget> build(BuildContext&) override {
    std::vector<SharedPtr<Widget>> kids;
    for (const auto& ln : lines_) {
        Color bg = Color::fromRGBA(0, 0, 0, 0);
        Color fg = theme().textPrimary;
        if (ln.sign == '+') {
            bg = Color::fromRGBA(0.1, 0.6, 0.2, 0.14);
            fg = theme().success;
        } else if (ln.sign == '-') {
            bg = Color::fromRGBA(0.8, 0.15, 0.15, 0.14);
            fg = theme().error;
        }
        std::string shown = std::string(1, ln.sign) + " " + ln.text;
        auto t = cui::make_shared<Text>(shown, TextStyle().size(12.0).fgColor(fg));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::symmetric(4.0, 8.0)).decoration(BoxDecoration{bg});
        kids.push_back(c);
    }
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto outer = cui::make_shared<Container>(col);
    outer->width(420.0).decoration(BoxDecoration{theme().surface, 6.0, 1.0,
                                                 theme().border});
    return outer;
}
std::string toStringShort() const override { return "DiffView"; }

private:
std::vector<DiffLine> lines_;
};

// ── Markdown ───────────────────────────────────────────────────────────────────
/// 极简 Markdown 渲染器（# / ## / - 列表 / ``` 代码块 / 段落）。
/// 上游 社区 `Markdown`（无官方实现）。
class Markdown : public StatelessWidget {
public:
explicit Markdown(std::string source, KeyPtr key = nullptr)
    : StatelessWidget(std::move(key)), source_(std::move(source)) {}

SharedPtr<Widget> build(BuildContext&) override {
    std::vector<SharedPtr<Widget>> kids;
    std::istringstream in(source_);
    std::string line;
    bool inCode = false;
    std::string codeBuf;
    auto flushCode = [&] {
        if (codeBuf.empty())
            return;
        auto t = cui::make_shared<Text>(codeBuf, TextStyle().size(12.0));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::all(8.0))
            .decoration(BoxDecoration{theme().surfaceVariant, 4.0});
        kids.push_back(c);
        kids.push_back(cui::make_shared<SizedBox>(0.0, 6.0));
        codeBuf.clear();
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("```", 0) == 0) {
            if (inCode) { flushCode(); inCode = false; }
            else { inCode = true; codeBuf.clear(); }
            continue;
        }
        if (inCode) {
            codeBuf += line;
            codeBuf += "\n";
            continue;
        }
        if (line.empty()) {
            kids.push_back(cui::make_shared<SizedBox>(0.0, 6.0));
            continue;
        }
        if (line.rfind("# ", 0) == 0) {
            kids.push_back(cui::make_shared<Text>(stripMarks(line.substr(2)),
                                                  TextStyle().size(18.0).bold()));
            kids.push_back(cui::make_shared<SizedBox>(0.0, 6.0));
        } else if (line.rfind("## ", 0) == 0) {
            kids.push_back(cui::make_shared<Text>(stripMarks(line.substr(3)),
                                                  TextStyle().size(15.0).bold()));
            kids.push_back(cui::make_shared<SizedBox>(0.0, 6.0));
        } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            auto t = cui::make_shared<Text>("• " + stripMarks(line.substr(2)),
                                            TextStyle().size(13.0));
            auto c = cui::make_shared<Container>(t);
            c->padding(EdgeInsets(12.0, 0.0, 0.0, 0.0));
            kids.push_back(c);
        } else {
            kids.push_back(cui::make_shared<Text>(stripMarks(line), TextStyle().size(13.0)));
            kids.push_back(cui::make_shared<SizedBox>(0.0, 4.0));
        }
    }
    flushCode();
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}
std::string toStringShort() const override { return "Markdown"; }

private:
std::string source_;
/// 去掉 ** 与 ` 标记（简化渲染，不做富文本 span）。
static std::string stripMarks(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch != '*' && ch != '`')
            out += ch;
    }
    return out;
}
};

// ── SearchableList ─────────────────────────────────────────────────────────────
/// 可搜索列表。上游 社区 `SearchableList`（无官方）。
///
/// 输入走真实键盘路由：状态持 `TextEditingController` + `FocusNode`，点搜索框即打字，
/// 每次变化写回 widget 的 query_ 并回调 onQueryChanged，过滤是大小写不敏感的子串匹配。
/// 与社区版的偏离：没有 searchPlaceholder / noResults 插槽，只保留「输入 + 计数 + 列表」。
class SearchableList : public StatefulWidget {
public:
explicit SearchableList(std::vector<std::string> items, std::string query = "",
                        KeyPtr key = nullptr,
                        std::function<void(const std::string&)> onQueryChanged = nullptr,
                        std::function<void(const std::string&)> onSubmitted = nullptr)
    : StatefulWidget(std::move(key)), items_(std::move(items)), query_(std::move(query)),
      onQueryChanged_(std::move(onQueryChanged)), onSubmitted_(std::move(onSubmitted)) {}

const std::vector<std::string>& items() const { return items_; }
/// 当前查询串。挂载后状态在每次变化时写回 query_，所以从 widget 读也是真值。
const std::string& query() const { return query_; }
void setQuery(const std::string& q) { query_ = q; }
const std::function<void(const std::string&)>& onQueryChanged() const {
    return onQueryChanged_;
}
const std::function<void(const std::string&)>& onSubmitted() const { return onSubmitted_; }

std::unique_ptr<State> createState() override;
std::string toStringShort() const override { return "SearchableList"; }

private:
std::vector<std::string> items_;
std::string query_;
std::function<void(const std::string&)> onQueryChanged_;
std::function<void(const std::string&)> onSubmitted_;
};

class SearchableListState : public State {
public:
void initState() override;
SharedPtr<Widget> build(BuildContext&) override;
///@{ 状态实际使用的控制器/焦点节点（测试与 gallery 用）。
const SharedPtr<TextEditingController>& controller() const { return controller_; }
const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
///@}

private:
SearchableList* w() { return static_cast<SearchableList*>(widget()); }
void onTextChanged(const std::string& text);

SharedPtr<TextEditingController> controller_;
SharedPtr<FocusNode> focusNode_;
};

inline std::unique_ptr<State> SearchableList::createState() {
return std::make_unique<SearchableListState>();
}

inline void SearchableListState::initState() {
    controller_ = cui::make_shared<TextEditingController>();
    focusNode_ = cui::make_shared<FocusNode>();
    // widget 上预置的 query_ 是初值（挂载前由宿主写的那一种）。
    controller_->setText(w()->query());
}

inline void SearchableListState::onTextChanged(const std::string& text) {
    auto* self = w();
    self->setQuery(text);
    if (self->onQueryChanged())
        self->onQueryChanged()(text);
    // 结果列表与「清除按钮显隐」都跟着查询串变。
    setState();
}

inline SharedPtr<Widget> SearchableListState::build(BuildContext&) {
auto* self = w();
const auto& items = self->items();
const Color kText = theme().textPrimary;
const Color kMuted = theme().textSecondary;

auto lower = [](std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
};
const std::string q = controller_ ? controller_->text() : self->query();
std::string lq = lower(q);

// 搜索框（真实键盘输入）
auto searchIcon = cui::make_shared<Icon>(IconCode::Search, 14.0, kMuted);
auto field = cui::make_shared<EditableText>(
        controller_, TextStyle().size(13.0).fgColor(kText), TextAlign::Left,
        "Search…", /*obscureText=*/false, /*autofocus=*/false, nullptr, focusNode_,
        [this](const std::string& text) { onTextChanged(text); }, self->onSubmitted());
std::vector<SharedPtr<Widget>> barKids;
barKids.push_back(std::move(searchIcon));
barKids.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
barKids.push_back(cui::make_shared<Expanded>(1, std::move(field)));
if (!q.empty()) {
    // 图标本身不参与命中（RenderIcon 没有 hitTestSelf），外包装饰盒子撑出命中区。
    auto clear = cui::make_shared<Icon>(IconCode::Close, 13.0, kMuted);
    auto clearBox = cui::make_shared<Container>(cui::make_shared<Center>(clear));
    clearBox->width(18.0).height(18.0)
        .decoration(BoxDecoration{theme().surfaceVariant, 9.0});
    barKids.push_back(cui::make_shared<GestureDetector>(clearBox, [this] {
        controller_->setText("");
        focusNode_->requestFocus();
    }));
}
auto barRow = cui::make_shared<Row>(std::move(barKids));
auto searchBar = cui::make_shared<Container>(barRow);
searchBar->padding(EdgeInsets::symmetric(10.0, 6.0))
    .decoration(BoxDecoration{theme().surfaceVariant, 8.0});

// 过滤结果
std::vector<SharedPtr<Widget>> rows;
int shown = 0;
for (const auto& it : items) {
    if (!lq.empty() && lower(it).find(lq) == std::string::npos)
        continue;
    ++shown;
    auto t = cui::make_shared<Text>(it, TextStyle().size(13.0).fgColor(kText));
    auto c = cui::make_shared<Container>(t);
    c->padding(EdgeInsets::symmetric(6.0, 10.0))
        .decoration(BoxDecoration{theme().surfaceVariant, 6.0});
    auto rowBox = cui::make_shared<Container>(c);
    rowBox->margin(EdgeInsets(0.0, 0.0, 0.0, 4.0));
    rows.push_back(rowBox);
}
if (shown == 0)
    rows.push_back(cui::make_shared<Text>("（无匹配）", TextStyle().size(12.0).fgColor(
                                                           kMuted)));
auto list = cui::make_shared<Column>(std::move(rows));
list->setMainAxisSize(MainAxisSize::Min);
list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

auto count = cui::make_shared<Text>(
    "query=" + (q.empty() ? std::string("(all)") : q) + "  ·  " +
        std::to_string(shown) + " / " + std::to_string(items.size()),
    TextStyle().size(11.5).fgColor(kMuted));

auto col = cui::make_shared<Column>(
    std::vector<SharedPtr<Widget>>{searchBar, cui::make_shared<SizedBox>(0.0, 8.0),
                                         count, cui::make_shared<SizedBox>(0.0, 6.0), list});
col->setMainAxisSize(MainAxisSize::Min);
col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
auto outer = cui::make_shared<Container>(col);
outer->width(320.0).padding(EdgeInsets::all(10.0))
    .decoration(BoxDecoration{theme().surface, 8.0, 1.0,
                              theme().border});
return outer;
}

// ── GridSheet ──────────────────────────────────────────────────────────────────
/// 电子表格网格（可点选单元格）。上游 社区 `GridSheet`（无官方）。
class GridSheet : public StatefulWidget {
public:
explicit GridSheet(std::vector<std::string> columns,
                   std::vector<std::vector<std::string>> rows, KeyPtr key = nullptr)
    : StatefulWidget(std::move(key)), columns_(std::move(columns)), rows_(std::move(rows)) {}

const std::vector<std::string>& columns() const { return columns_; }
const std::vector<std::vector<std::string>>& rows() const { return rows_; }

std::unique_ptr<State> createState() override;
std::string toStringShort() const override { return "GridSheet"; }

private:
std::vector<std::string> columns_;
std::vector<std::vector<std::string>> rows_;
};

class GridSheetState : public State {
public:
SharedPtr<Widget> build(BuildContext&) override;
private:
GridSheet* w() { return static_cast<GridSheet*>(widget()); }
int selRow_ = -1;
int selCol_ = -1;
};

inline std::unique_ptr<State> GridSheet::createState() {
return std::make_unique<GridSheetState>();
}

inline SharedPtr<Widget> GridSheetState::build(BuildContext&) {
auto* self = w();
const Color border = theme().border;

auto cell = [&](const std::string& text, bool header, bool selected) {
    auto t = cui::make_shared<Text>(text, TextStyle().size(12.0).bold().fgColor(
                                              header ? theme().textSecondary
                                                     : theme().textPrimary));
    auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
    c->width(90.0).height(26.0)
        .decoration(BoxDecoration{selected ? Color(theme().primary.r, theme().primary.g,
                                                  theme().primary.b, 0.18)
                                           : (header ? theme().surfaceVariant
                                                     : theme().surface),
                                  0.0, 1.0, border});
    return c;
};
auto corner = [&] {
    auto c = cui::make_shared<Container>(cui::make_shared<SizedBox>(40.0, 26.0));
    c->decoration(BoxDecoration{theme().surfaceVariant, 0.0, 1.0, border});
    return c;
};
auto rowHeader = [&](int r) {
    auto t = cui::make_shared<Text>(std::to_string(r + 1),
                                    TextStyle().size(11.5).bold().fgColor(theme().textSecondary));
    auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
    c->width(40.0).height(26.0)
        .decoration(BoxDecoration{theme().surfaceVariant, 0.0, 1.0, border});
    return c;
};

// 表头行
std::vector<SharedPtr<Widget>> headRow;
headRow.push_back(corner());
for (size_t i = 0; i < self->columns().size(); ++i)
    headRow.push_back(cell(self->columns()[i], true, false));
auto head = cui::make_shared<Row>(std::move(headRow));
head->setMainAxisSize(MainAxisSize::Min);

// 数据行
std::vector<SharedPtr<Widget>> bodyRows;
const auto& rows = self->rows();
for (size_t r = 0; r < rows.size(); ++r) {
    std::vector<SharedPtr<Widget>> cells;
    cells.push_back(rowHeader(static_cast<int>(r)));
    for (size_t c = 0; c < self->columns().size(); ++c) {
        bool selected = (static_cast<int>(r) == selRow_ && static_cast<int>(c) == selCol_);
        std::string text = c < rows[r].size() ? rows[r][c] : "";
        int rr = static_cast<int>(r), cc = static_cast<int>(c);
        cells.push_back(cui::make_shared<GestureDetector>(
            cell(text, false, selected), [this, rr, cc] {
                selRow_ = rr;
                selCol_ = cc;
                setState();
            }));
    }
    auto row = cui::make_shared<Row>(std::move(cells));
    row->setMainAxisSize(MainAxisSize::Min);
    bodyRows.push_back(row);
}

std::vector<SharedPtr<Widget>> all;
all.push_back(head);
for (auto& r : bodyRows)
    all.push_back(r);
auto col = cui::make_shared<Column>(std::move(all));
col->setMainAxisSize(MainAxisSize::Min);
col->setCrossAxisAlignment(CrossAxisAlignment::Start);
return col;
}


}  // namespace cui_core
