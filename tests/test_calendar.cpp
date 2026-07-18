/// @file test_calendar.cpp
/// @brief Calendar（月历）交互回归：日期点击、选中态、月份导航。
///
/// 对应 上游 `CalendarDatePicker`（material/calendar_date_picker.dart）的简化版。
/// 全部 headless，通过 PipelineOwner::dispatchPointerEvent 驱动指针事件。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/framework/listener.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/data.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/scroll.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }
    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
    }
    RenderObject* ro() { return root->findRenderObject(); }
    void tap(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        PointerUpEvent u;
        u.position = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }
};

/// 日期格的「命中层」尺寸是 34×30（GestureDetector 的 RenderPointerListener）；
/// 表头导航按钮是 22×42，星期表头没有 Listener 层，所以按尺寸筛。
/// 注意非今天的格子 decorations 是空的，Container 不会插 DecoratedBox，
/// 所以不能拿 RenderDecoratedBox 当锚点。
std::vector<Offset> dayCenters(RenderObject* ro) {
    std::vector<Offset> out;
    struct V {
        std::vector<Offset>* out;
        void visit(RenderObject* n) {
            if (!n)
                return;
            auto* box = dynamic_cast<RenderBox*>(n);
            if (box && dynamic_cast<RenderPointerListener*>(n) &&
                std::fabs(box->size().width - 34.0) < 0.5 &&
                std::fabs(box->size().height - 30.0) < 0.5)
                out->push_back(box->localToGlobal({box->size().width * 0.5,
                                                   box->size().height * 0.5}));
            n->visitChildren([this](AbstractNode* c) {
                visit(static_cast<RenderObject*>(c));
            });
        }
    } v{&out};
    v.visit(ro);
    std::sort(out.begin(), out.end(), [](const Offset& a, const Offset& b) {
        if (std::fabs(a.dy - b.dy) > 1.0)
            return a.dy < b.dy;
        return a.dx < b.dx;
    });
    return out;
}

/// 一格一次回调的记录。
struct Sel {
    int calls = 0;
    int y = 0, m = -1, d = 0;
    void operator()(int yy, int mm, int dd) {
        ++calls;
        y = yy;
        m = mm;
        d = dd;
    }
};

/// 高亮格：Container 只在 decoration 非空时才插 DecoratedBox，所以「这一格有
/// RenderDecoratedBox」就等于「被画了底色或描边」——选中格与今天格都靠它。
struct Deco {
    Offset c;
    Color color;
    Color borderColor;
    double radius = 0.0;
    double borderWidth = 0.0;
};

std::vector<Deco> decoratedCells(RenderObject* ro) {
    std::vector<Deco> out;
    struct V {
        std::vector<Deco>* out;
        void visit(RenderObject* n) {
            if (!n)
                return;
            auto* box = dynamic_cast<RenderDecoratedBox*>(n);
            if (box && std::fabs(box->size().width - 34.0) < 0.5 &&
                std::fabs(box->size().height - 30.0) < 0.5)
                out->push_back(Deco{box->localToGlobal({17.0, 15.0}), box->color(),
                                    box->borderColor(), box->borderRadius(),
                                    box->borderWidth()});
            n->visitChildren([this](AbstractNode* c) {
                visit(static_cast<RenderObject*>(c));
            });
        }
    } v{&out};
    v.visit(ro);
    return out;
}

bool samePoint(const Offset& a, const Offset& b) {
    return std::fabs(a.dx - b.dx) < 1.0 && std::fabs(a.dy - b.dy) < 1.0;
}
bool closeColor(const Color& a, const Color& b) {
    return std::fabs(a.r - b.r) < 0.01 && std::fabs(a.g - b.g) < 0.01 &&
           std::fabs(a.b - b.b) < 0.01 && std::fabs(a.a - b.a) < 0.01;
}
const Deco* decoAt(const std::vector<Deco>& ds, const Offset& c) {
    for (const auto& d : ds)
        if (samePoint(d.c, c))
            return &d;
    return nullptr;
}

// ── 1. 裸挂：点击日期触发 onDaySelected ──────────────────────────────────────
void testDayTapPlain() {
    std::printf("== day tap (plain) ==\n");
    Harness h;
    Sel sel;
    auto cal = cui::make_shared<Calendar>(2026, 11, [&](int y, int m, int d) {
        sel(y, m, d);
    });
    h.attach(cui::make_shared<Align>(-1.0, -1.0, cal), 600.0, 500.0);
    auto cells = dayCenters(h.ro());
    check(cells.size() == 30,
          "十一月有 30 个可点日期格（实得 " + std::to_string(cells.size()) + "）");
    if (cells.size() < 30)
        return;

    // 2026-11-01 是周日，无前导空格 ⇒ cells[i] 就是 11/(i+1)
    h.tap(cells[3].dx, cells[3].dy);
    check(sel.calls == 1, "点击第 4 格触发一次回调（实得 " + std::to_string(sel.calls) + "）");
    check(sel.y == 2026 && sel.m == 10 && sel.d == 4,
          "回调参数 2026-11-04（month0=" + std::to_string(sel.m) + " day=" +
              std::to_string(sel.d) + "）");

    h.tap(cells[10].dx, cells[10].dy);
    check(sel.calls == 2 && sel.d == 11, "点击第 11 格回调 day=11");
}

// ── 2. 套在 Scrollable 里（story 的实际嵌套）───────────────────────────────
void testDayTapInScroll() {
    std::printf("== day tap (inside Scrollable) ==\n");
    // 注意声明顺序：ScrollController 必须早于 Harness，保证函数退出时 Harness
    // 先析构（卸载树 → ScrollableState::dispose 调用 removeListener），控制器
    // 仍然存活；否则控制器先销毁、卸载时访问已释放的 listeners_（use-after-free）。
    ScrollController ctrl;
    Harness h;
    Sel sel;
    auto cal = cui::make_shared<Calendar>(2026, 11, [&](int y, int m, int d) {
        sel(y, m, d);
    });
    auto body = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        cal, cui::make_shared<SizedBox>(600.0, 900.0, cui::make_shared<Text>("下方内容"))});
    auto scroll = cui::make_shared<Scrollable>(&ctrl, Axis::Vertical, body);
    h.attach(scroll, 600.0, 500.0);
    auto cells = dayCenters(h.ro());
    check(cells.size() == 30,
          "滚动视图内仍有 30 个可点日期格（实得 " + std::to_string(cells.size()) + "）");
    if (cells.size() < 30)
        return;

    h.tap(cells[7].dx, cells[7].dy);
    check(sel.calls == 1 && sel.d == 8,
          "滚动视图内点击第 8 格生效（calls=" + std::to_string(sel.calls) +
              " day=" + std::to_string(sel.d) + "）");
}

// ── 3. 选中态：点击即高亮，换格即迁移，切月不清 ──────────────────────────────
void testSelectionHighlight() {
    std::printf("== selection highlight ==\n");
    Harness h;
    Sel sel;
    // 用 2027-03（不是今天所在的月）⇒ 除了选中格外不会有「今天」描边格。
    auto cal = cui::make_shared<Calendar>(2027, 3, [&](int y, int m, int d) {
        sel(y, m, d);
    });
    h.attach(cui::make_shared<Align>(-1.0, -1.0, cal), 600.0, 500.0);
    auto cells = dayCenters(h.ro());
    check(cells.size() == 31, "三月有 31 个日期格（实得 " + std::to_string(cells.size()) + "）");
    if (cells.size() != 31)
        return;
    check(decoratedCells(h.ro()).empty(), "初始无选中格");

    h.tap(cells[11].dx, cells[11].dy);  // 3/12
    auto ds = decoratedCells(h.ro());
    check(ds.size() == 1, "点 3/12 后恰有一格被高亮（实得 " + std::to_string(ds.size()) + "）");
    const Deco* on = decoAt(ds, cells[11]);
    check(on != nullptr, "高亮的正是被点中的那一格");
    if (on) {
        check(closeColor(on->color, dataPrimary()), "选中格底色取主色（对齐 上游 primary 填充）");
        check(on->radius > 0.0, "选中格是圆角/圆形");
        check(on->borderWidth == 0.0, "非今天的选中格不带描边");
    }

    h.tap(cells[19].dx, cells[19].dy);  // 3/20
    ds = decoratedCells(h.ro());
    check(ds.size() == 1 && decoAt(ds, cells[19]) != nullptr,
          "选中态从 3/12 迁移到 3/20（单格高亮）");

    // 切月：走 4 月再回来，选中仍是 3/20（上游 _handleMonthChanged 不动 _selectedDate）。
    // 导航按钮宽 22，按尺寸从剩下的 Listener 里挑出来。
    auto navCenters = [&](void) {
        std::vector<Offset> out;
        struct V {
            std::vector<Offset>* out;
            void visit(RenderObject* n) {
                if (!n)
                    return;
                auto* box = dynamic_cast<RenderBox*>(n);
                // 导航按钮（‹ / ›）是 GestureDetector，没有写死宽度，尺寸随字体变化；
                // 日期格写死 34×30。所以这里用「排除 34 宽日期格」来挑出导航按钮，
                // 避免依赖具体字形宽度（不同环境下 ‹/› 渲染宽度不同）。
                if (box && dynamic_cast<RenderPointerListener*>(n) &&
                    box->size().width < 30.0)
                    out->push_back(box->localToGlobal({box->size().width * 0.5,
                                                        box->size().height * 0.5}));
                n->visitChildren([this](AbstractNode* c) {
                    visit(static_cast<RenderObject*>(c));
                });
            }
        } v{&out};
        v.visit(h.ro());
        std::sort(out.begin(), out.end(), [](const Offset& a, const Offset& b) {
            return a.dx < b.dx;
        });
        return out;
    };
    auto nav = navCenters();
    check(nav.size() == 2, "表头有 ‹ / › 两个导航按钮");
    if (nav.size() == 2) {
        h.tap(nav[1].dx, nav[1].dy);  // › 到四月
        check(decoratedCells(h.ro()).empty(), "切到四月：三月那格不再出现（本月无选中）");
        h.tap(nav[0].dx, nav[0].dy);  // ‹ 回三月
        ds = decoratedCells(h.ro());
        check(ds.size() == 1 && decoAt(ds, cells[19]) != nullptr, "切回三月：选中仍停在 3/20");
        check(sel.d == 20, "切月不重复触发回调（仍只记最后一次 3/20）");
    }
}

// ── 4. 今天格：描边圈，点中后「实心 + 深色描边」──────────────────────────────
void testTodayRing() {
    std::printf("== today ring ==\n");
    Harness h;
    Sel sel;
    auto cal = cui::make_shared<Calendar>(0, 0, [&](int y, int m, int d) {
        sel(y, m, d);
    });
    h.attach(cui::make_shared<Align>(-1.0, -1.0, cal), 600.0, 500.0);
    auto cells = dayCenters(h.ro());
    auto ds = decoratedCells(h.ro());
    check(ds.size() == 1, "今天格有且仅有一层描边（实得 " + std::to_string(ds.size()) + "）");
    if (ds.size() != 1)
        return;
    check(ds[0].borderWidth > 0.0 && closeColor(ds[0].borderColor, dataPrimary()),
          "今天 = 主色描边圈，不填底色");
    check(ds[0].color.a == 0.0, "今天（未选中）底色透明");

    h.tap(ds[0].c.dx, ds[0].c.dy);
    ds = decoratedCells(h.ro());
    check(ds.size() == 1 && closeColor(ds[0].color, dataPrimary()),
          "点中今天：主色实心");
    check(ds.size() == 1 && ds[0].borderWidth > 0.0 &&
              !closeColor(ds[0].borderColor, dataPrimary()),
          "今天且选中：描边换成深色（对齐 上游 todayBorder 在 selected 态取 onSurface）");
    check(sel.calls == 1, "点今天同样触发回调");
}

// ── 5. 不传回调也可选中（story「数据」页就是裸 Calendar）───────────────────
void testSelectableWithoutCallback() {
    std::printf("== selectable without callback ==\n");
    Harness h;
    auto cal = cui::make_shared<Calendar>(2027, 3);
    h.attach(cui::make_shared<Align>(-1.0, -1.0, cal), 600.0, 500.0);
    auto cells = dayCenters(h.ro());
    check(cells.size() == 31, "无回调也渲染出 31 个可点格");
    if (cells.size() != 31)
        return;
    h.tap(cells[4].dx, cells[4].dy);
    auto ds = decoratedCells(h.ro());
    check(ds.size() == 1 && decoAt(ds, cells[4]) != nullptr,
          "无回调时点击照样选中（用户报的就是这一格不动）");
}

}  // namespace

int main() {
    std::setbuf(stdout, nullptr);
    testDayTapPlain();
    testDayTapInScroll();
    testSelectionHighlight();
    testTodayRing();
    testSelectableWithoutCallback();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
